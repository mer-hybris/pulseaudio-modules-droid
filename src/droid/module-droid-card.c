/*
 * Copyright (C) 2013-2018 Jolla Ltd.
 *
 * Contact: Juho Hämäläinen <juho.hamalainen@jolla.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>
#include <stdio.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/time-smoother.h>
#include <pulsecore/card.h>
#include <pulsecore/device-port.h>
#include <pulsecore/idxset.h>
#include <pulsecore/strlist.h>

//#include <droid/hardware/audio_policy.h>
//#include <droid/system/audio_policy.h>

#include <droid/droid-util.h>
#include "droid-sink.h"
#include "droid-source.h"

#include "module-droid-card-symdef.h"

PA_MODULE_AUTHOR("Juho Hämäläinen");
PA_MODULE_DESCRIPTION("Droid card");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE(
        "card_name=<name for the card> "
        "sink_name=<name for the sink> "
        "source_name=<name for the source> "
        "namereg_fail=<when false attempt to synthesise new names if they are already taken> "
        "rate=<sample rate> "
        "output_flags=<flags for sink> "
        "module_id=<which droid hw module to load, default primary> "
        "voice_source_routing=<always true, parameter left for compatibility> "
        "deferred_volume=<synchronize software and hardware volume changes to avoid momentary jumps?> "
        "config=<location for droid audio configuration> "
        "voice_property_key=<proplist key searched for sink-input that should control voice call volume> "
        "voice_property_value=<proplist value for the key for voice control sink-input> "
        "default_profile=<boolean. create default profile for primary module or not. defaults to true> "
        "merge_inputs=<boolean. merge input streams to single source with default profile. defaults to true> "
        "quirks=<comma separated list of quirks to enable/disable>"
);

static const char* const valid_modargs[] = {
    "card_name",
    "sink_name",
    "source_name",
    "namereg_fail",
    "format",
    "rate",
    "channels",
    "channel_map",
    "sink_rate",
    "sink_format",
    "sink_channel_map",
    "sink_mix_route",
    "source_rate",
    "source_format",
    "source_channel_map",
    "output_flags",
    "module_id",
    "voice_source_routing",
    "sink_buffer",
    "source_buffer",
    "deferred_volume",
    "mute_routing_before",
    "mute_routing_after",
    "prewrite_on_resume",
    "config",
    "voice_property_key",
    "voice_property_value",
    "default_profile",
    "combine",
    "quirks",
    NULL,
};

#define DEFAULT_MODULE_ID "primary"
#define VOICE_CALL_PROFILE_NAME     "voicecall"
#define VOICE_CALL_PROFILE_DESC     "Call mode"
#define VOICE_RECORD_PROFILE_NAME   "voicecall-record"
#define VOICE_RECORD_PROFILE_DESC   "Call mode record"
#define RINGTONE_PROFILE_NAME       "ringtone"
#define RINGTONE_PROFILE_DESC       "Ringtone mode"
#define COMMUNICATION_PROFILE_NAME  "communication"
#define COMMUNICATION_PROFILE_DESC  "Communication mode"

#define VENDOR_EXT_REALCALL_ON      "realcall=on"
#define VENDOR_EXT_REALCALL_OFF     "realcall=off"

struct userdata;

typedef bool (*virtual_profile_event_cb)(struct userdata *u, pa_droid_profile *p, bool enabling);

struct virtual_profile {
    pa_card_profile *parent;
    pa_card_profile *extension;
    virtual_profile_event_cb event_cb;
};

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_droid_profile_set *profile_set;

    pa_droid_hw_module *hw_module;
    pa_droid_card_data card_data;

    pa_droid_profile *old_profile;

    pa_modargs *modargs;
    pa_card *card;
};

struct profile_data {
    pa_droid_profile *profile;
    audio_mode_t mode;
    bool virtual_profile;
    /* Variables for virtual profiles: */
    struct virtual_profile virtual;
};

#ifdef DROID_AUDIO_HAL_USE_VSID

/* From hal/voice_extn/voice_extn.c */
#define AUDIO_PARAMETER_KEY_VSID            "vsid"
#define AUDIO_PARAMETER_KEY_CALL_STATE      "call_state"

/* From hal/voice_extn/voice_extn.c */
#define VOICE2_VSID         (0x10DC1000)
#define VOLTE_VSID          (0x10C02000)
#define QCHAT_VSID          (0x10803000)
#define VOWLAN_VSID         (0x10002000)
#define VOICEMMODE1_VSID    (0x11C05000)
#define VOICEMMODE2_VSID    (0x11DC5000)

/* From hal/voice.h */
#define BASE_CALL_STATE     1
#define CALL_INACTIVE       (BASE_CALL_STATE)
#define CALL_ACTIVE         (BASE_CALL_STATE + 1)
#define VOICE_VSID  0x10C01000

/* For virtual profiles */
#define VOICE_SESSION_VOICE1_PROFILE_NAME       "voicecall-voice1"
#define VOICE_SESSION_VOICE1_PROFILE_DESC       "Call mode, default to voice 1 vsid"
#define VOICE_SESSION_VOICE2_PROFILE_NAME       "voicecall-voice2"
#define VOICE_SESSION_VOICE2_PROFILE_DESC       "Call mode, default to voice 2 vsid"
#define VOICE_SESSION_VOLTE_PROFILE_NAME        "voicecall-volte"
#define VOICE_SESSION_VOLTE_PROFILE_DESC        "Call mode, default to volte vsid"
#define VOICE_SESSION_QCHAT_PROFILE_NAME        "voicecall-qchat"
#define VOICE_SESSION_QCHAT_PROFILE_DESC        "Call mode, default to qchat vsid"
#define VOICE_SESSION_VOWLAN_PROFILE_NAME       "voicecall-vowlan"
#define VOICE_SESSION_VOWLAN_PROFILE_DESC       "Call mode, default to vowlan vsid"
#define VOICE_SESSION_VOICEMMODE1_PROFILE_NAME  "voicecall-voicemmode1"
#define VOICE_SESSION_VOICEMMODE1_PROFILE_DESC  "Call mode, default to voicemmode1 vsid"
#define VOICE_SESSION_VOICEMMODE2_PROFILE_NAME  "voicecall-voicemmode2"
#define VOICE_SESSION_VOICEMMODE2_PROFILE_DESC  "Call mode, default to voicemmode2 vsid"

static bool voicecall_voice1_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling);
static bool voicecall_voice2_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling);
static bool voicecall_volte_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling);
static bool voicecall_qchat_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling);
static bool voicecall_vowlan_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling);
static bool voicecall_voicemmode1_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling);
static bool voicecall_voicemmode2_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling);

#endif /* DROID_AUDIO_HAL_USE_VSID */

static void add_disabled_profile(pa_hashmap *profiles) {
    pa_card_profile *cp;
    struct profile_data *d;

    cp = pa_card_profile_new("off", _("Off"), sizeof(struct profile_data));
    cp->available = PA_AVAILABLE_YES;

    d = PA_CARD_PROFILE_DATA(cp);
    d->profile = NULL;

    pa_hashmap_put(profiles, cp->name, cp);
}

/* Special profile for calls */
static pa_card_profile* add_virtual_profile(struct userdata *u, const char *name, const char *description,
                                            audio_mode_t audio_mode, virtual_profile_event_cb event_cb,
                                            pa_available_t available, pa_card_profile *extension_to,
                                            pa_hashmap *profiles) {
    pa_droid_profile *ap;
    pa_card_profile *cp;
    struct profile_data *d, *ext;

    pa_assert(u);
    pa_assert(u->profile_set);

    pa_log_debug("New virtual profile: %s", name);

    ap = pa_xnew0(pa_droid_profile, 1);
    ap->profile_set = u->profile_set;
    ap->name = pa_xstrdup(name);
    ap->description = pa_xstrdup(description);
    ap->priority = 50;

    pa_hashmap_put(u->profile_set->profiles, ap->name, ap);

    cp = pa_card_profile_new(ap->name, ap->description, sizeof(struct profile_data));
    cp->available = available;
    d = PA_CARD_PROFILE_DATA(cp);
    d->profile = ap;
    d->virtual_profile = true;
    d->mode = audio_mode;
    d->virtual.event_cb = event_cb;
    d->virtual.extension = NULL;
    if (extension_to) {
        ext = PA_CARD_PROFILE_DATA(extension_to);
        ext->virtual.extension = cp;
        d->virtual.parent = extension_to;
    } else
        d->virtual.parent = NULL;

    pa_hashmap_put(profiles, cp->name, cp);

    return cp;
}

static int set_parameters_cb(pa_droid_card_data *card_data, const char *str) {
    struct userdata *u;

    pa_assert(card_data);
    pa_assert_se((u = card_data->userdata));
    pa_assert(str);

    return pa_droid_set_parameters(u->hw_module, str);
}

static void set_card_name(pa_modargs *ma, pa_card_new_data *data, const char *module_id) {
    const char *tmp;
    char *name;

    pa_assert(ma);
    pa_assert(data);
    pa_assert(module_id);

    if ((tmp = pa_modargs_get_value(ma, "card_name", NULL))) {
        pa_card_new_data_set_name(data, tmp);
        data->namereg_fail = true;
        return;
    }

    name = pa_sprintf_malloc("droid_card.%s", module_id);
    pa_card_new_data_set_name(data, name);
    pa_xfree(name);
    data->namereg_fail = false;
}

static void add_profile(struct userdata *u, pa_hashmap *h, pa_hashmap *ports, pa_droid_profile *ap) {
    pa_card_profile *cp;
    struct profile_data *d;
    pa_droid_mapping *am;
    int max_channels;
    uint32_t idx;

    pa_assert(u);
    pa_assert(h);
    pa_assert(ports);
    pa_assert(ap);

    pa_log_debug("Card profile %s", ap->name);

    cp = pa_card_profile_new(ap->name, ap->description, sizeof(struct profile_data));
    cp->available = PA_AVAILABLE_YES;
    cp->priority = ap->priority;

    max_channels = 0;
    PA_IDXSET_FOREACH(am, ap->output_mappings, idx) {
        cp->n_sinks++;
        pa_droid_add_card_ports(cp, ports, am, u->core);
        max_channels = popcount(am->output->channel_masks) > max_channels
                        ? popcount(am->output->channel_masks) : max_channels;
    }
    cp->max_sink_channels = max_channels;

    max_channels = 0;
    PA_IDXSET_FOREACH(am, ap->input_mappings, idx) {
        cp->n_sources++;
        pa_droid_add_card_ports(cp, ports, am, u->core);
        max_channels = popcount(am->input->channel_masks) > max_channels
                        ? popcount(am->input->channel_masks) : max_channels;
    }
    cp->max_source_channels = max_channels;

    d = PA_CARD_PROFILE_DATA(cp);
    d->profile = ap;
    d->virtual_profile = false;
    d->mode = AUDIO_MODE_NORMAL;

    pa_hashmap_put(h, cp->name, cp);
}

static void add_profiles(struct userdata *u, pa_hashmap *h, pa_hashmap *ports) {
    void *state;
    pa_droid_profile *ap;

    pa_assert(u);
    pa_assert(h);
    pa_assert(ports);

    PA_HASHMAP_FOREACH(ap, u->profile_set->profiles, state) {
        add_profile(u, h, ports, ap);
    }
}

static void init_profile(struct userdata *u) {
    pa_droid_mapping *am;
    struct profile_data *d;
    uint32_t idx;

    pa_assert(u);

    pa_log_debug("Init profile.");

    d = PA_CARD_PROFILE_DATA(u->card->active_profile);

    if (d->profile && pa_idxset_size(d->profile->output_mappings) > 0) {
        PA_IDXSET_FOREACH(am, d->profile->output_mappings, idx) {
            am->sink = pa_droid_sink_new(u->module, u->modargs, __FILE__, &u->card_data, 0, am, u->card);
        }
    }

    if (d->profile && pa_idxset_size(d->profile->input_mappings) > 0) {
        PA_IDXSET_FOREACH(am, d->profile->input_mappings, idx) {
            am->source = pa_droid_source_new(u->module, u->modargs, __FILE__, (audio_devices_t) 0, &u->card_data, am, u->card);
        }
    }
}

static int set_mode(struct userdata *u, audio_mode_t mode) {
    int ret;
    const char *mode_str;

    pa_assert(u);
    pa_assert(u->hw_module);
    pa_assert(u->hw_module->device);

    switch (mode) {
        case AUDIO_MODE_RINGTONE:
            mode_str = "AUDIO_MODE_RINGTONE";
            break;
        case AUDIO_MODE_IN_CALL:
            mode_str = "AUDIO_MODE_IN_CALL";
            break;
        case AUDIO_MODE_IN_COMMUNICATION:
            mode_str = "AUDIO_MODE_IN_COMMUNICATION";
            break;
        default:
            mode_str = "AUDIO_MODE_NORMAL";
            break;
    }

    pa_log_debug("Set mode to %s.", mode_str);

    pa_droid_hw_module_lock(u->hw_module);
    if ((ret = u->hw_module->device->set_mode(u->hw_module->device, mode)) < 0)
        pa_log("Failed to set mode.");
    pa_droid_hw_module_unlock(u->hw_module);

    return ret;
}

static void park_profile(pa_droid_profile *dp) {
    pa_droid_mapping *am;
    uint32_t idx;

    pa_assert(dp);

    /* Virtual profiles don't have output mappings. */
    if (dp->output_mappings) {
        PA_IDXSET_FOREACH(am, dp->output_mappings, idx) {
            if (pa_droid_mapping_is_primary(am))
                pa_sink_set_port(am->sink, PA_DROID_OUTPUT_PARKING, false);
        }
    };

    /* Virtual profiles don't have input mappings. */
    if (dp->input_mappings) {
        PA_IDXSET_FOREACH(am, dp->input_mappings, idx) {
            if (pa_droid_mapping_is_primary(am))
                pa_source_set_port(am->source, PA_DROID_INPUT_PARKING, false);
        }
    };
}

static bool voicecall_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling) {
    pa_card_profile *cp = NULL;
    pa_droid_mapping *am_output;

    pa_assert(u);
    pa_assert(p);
    pa_assert(u->old_profile);

    if (!(am_output = pa_droid_idxset_get_primary(u->old_profile->output_mappings))) {
        pa_log("Active profile doesn't have primary output device.");
        return false;
    }

    if (pa_droid_idxset_mapping_with_device(u->old_profile->input_mappings,
                                            AUDIO_DEVICE_IN_VOICE_CALL))
        cp = pa_hashmap_get(u->card->profiles, VOICE_RECORD_PROFILE_NAME);

    /* call mode specialities */
    if (enabling) {
        pa_droid_sink_set_voice_control(am_output->sink, true);
        if (cp && cp->available == PA_AVAILABLE_NO) {
            pa_log_debug("Enable " VOICE_RECORD_PROFILE_NAME " profile.");
            pa_card_profile_set_available(cp, PA_AVAILABLE_YES);
        }
        if (pa_droid_quirk(u->hw_module, QUIRK_REALCALL))
            pa_droid_set_parameters(u->hw_module, VENDOR_EXT_REALCALL_ON);
    } else {
        pa_droid_sink_set_voice_control(am_output->sink, false);
        if (cp && cp->available == PA_AVAILABLE_YES) {
            pa_log_debug("Disable " VOICE_RECORD_PROFILE_NAME " profile.");
            pa_card_profile_set_available(cp, PA_AVAILABLE_NO);
        }
        if (pa_droid_quirk(u->hw_module, QUIRK_REALCALL))
            pa_droid_set_parameters(u->hw_module, VENDOR_EXT_REALCALL_OFF);
    }

    return true;
}

#ifdef DROID_AUDIO_HAL_USE_VSID
static bool voicecall_vsid(struct userdata *u, pa_droid_profile *p, uint32_t vsid, bool enabling)
{
    char *setparam;

    voicecall_profile_event_cb(u, p, enabling);

    setparam = pa_sprintf_malloc("%s=%u;%s=%d", AUDIO_PARAMETER_KEY_VSID, vsid,
                                                AUDIO_PARAMETER_KEY_CALL_STATE,
                                                enabling ? CALL_ACTIVE : CALL_INACTIVE);

    pa_droid_set_parameters(u->hw_module, setparam);
    pa_xfree(setparam);

    return true;
}

static bool voicecall_voice1_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling)
{
    return voicecall_vsid(u, p, VOICE_VSID, enabling);
}

static bool voicecall_voice2_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling)
{
    return voicecall_vsid(u, p, VOICE2_VSID, enabling);
}

static bool voicecall_volte_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling)
{
    return voicecall_vsid(u, p, VOLTE_VSID, enabling);
}

static bool voicecall_qchat_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling)
{
    return voicecall_vsid(u, p, QCHAT_VSID, enabling);
}

static bool voicecall_vowlan_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling)
{
    return voicecall_vsid(u, p, VOWLAN_VSID, enabling);
}

static bool voicecall_voicemmode1_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling)
{
    return voicecall_vsid(u, p, VOICEMMODE1_VSID, enabling);
}

static bool voicecall_voicemmode2_vsid_profile_event_cb(struct userdata *u, pa_droid_profile *p, bool enabling)
{
    return voicecall_vsid(u, p, VOICEMMODE2_VSID, enabling);
}
#endif /* DROID_AUDIO_HAL_USE_VSID */

static void leave_virtual_profile(struct userdata *u, pa_card *c, pa_card_profile *cp, pa_card_profile *new_profile) {
    struct profile_data *pd, *parent;

    pa_assert(u);
    pa_assert(c);
    pa_assert(cp);
    pa_assert_se((pd = PA_CARD_PROFILE_DATA(cp)));

    if (pd->virtual.parent)
        pa_log_debug("Leaving extension %s.", cp->name);

    /* First run event for old virtual profile, unless new profile is extension
     * to the old profile. */
    if (pd->virtual.extension) {
        if (pd->virtual.extension != new_profile && pd->virtual.event_cb)
            pd->virtual.event_cb(u, pd->profile, false);
    } else {
        if (pd->virtual.event_cb)
            pd->virtual.event_cb(u, pd->profile, false);
    }

    /* If old virtual profile was extension, and new profile is not extensions parent, run event
     * for extension's parent as well. */
    if (pd->virtual.parent != new_profile && pd->virtual.parent) {
        parent = PA_CARD_PROFILE_DATA(pd->virtual.parent);

        if (parent->virtual.event_cb)
            parent->virtual.event_cb(u, parent->profile, false);
    }
}

static int card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    struct userdata *u;
    pa_droid_mapping *am;
    struct profile_data *nd, *od;
    pa_queue *sink_inputs = NULL, *source_outputs = NULL;
    pa_sink *primary_sink = NULL;
    uint32_t idx;

    pa_assert(c);
    pa_assert(new_profile);
    pa_assert_se(u = c->userdata);

    if (new_profile->available != PA_AVAILABLE_YES) {
        pa_log("Profile %s is not available.", new_profile->name);
        return -1;
    }

    nd = PA_CARD_PROFILE_DATA(new_profile);
    od = PA_CARD_PROFILE_DATA(c->active_profile);

    if (nd->virtual_profile) {
        /* old_profile should always be real profile. */
        if (u->old_profile == NULL) {
            pa_assert(!od->virtual_profile);
            u->old_profile = od->profile;
        }

        if (od->virtual_profile)
            leave_virtual_profile(u, c, c->active_profile, new_profile);

        park_profile(od->profile);

        if (nd->mode != od->mode)
            set_mode(u, nd->mode);

        if (od->virtual.parent != new_profile && nd->virtual.event_cb)
            nd->virtual.event_cb(u, nd->profile, true);

        return 0;
    }

    if (od->virtual_profile) {
        pa_assert(u->old_profile);

        if (od->virtual_profile)
            leave_virtual_profile(u, c, c->active_profile, NULL);

        park_profile(nd->profile);

        if (nd->mode != od->mode)
            set_mode(u, nd->mode);

        /* If new profile is the same as from which we switched to
         * virtual profile, transfer ownership back to that profile.
         * Otherwise destroy sinks & sources and switch to new profile. */
        if (nd->profile == u->old_profile) {
            u->old_profile = NULL;
            return 0;
        } else {
            od->profile = u->old_profile;
            u->old_profile = NULL;

            /* Continue to sink-input/source-output transfer below. */
        }
    }

    /* If there are connected sink inputs/source outputs in old profile's sinks/sources move
     * them all to new sinks/sources. */

    if (od->profile && pa_idxset_size(od->profile->output_mappings) > 0) {
        PA_IDXSET_FOREACH(am, od->profile->output_mappings, idx) {
            if (!am->sink)
                continue;

            if (nd->profile &&
                pa_idxset_get_by_data(nd->profile->output_mappings, am, NULL)) {

                if (pa_droid_mapping_is_primary(am))
                    primary_sink = am->sink;
                continue;
            }

            sink_inputs = pa_sink_move_all_start(am->sink, sink_inputs);
            pa_droid_sink_free(am->sink);
            am->sink = NULL;
        }
    }

    if (od->profile && pa_idxset_size(od->profile->input_mappings) > 0) {
        PA_IDXSET_FOREACH(am, od->profile->input_mappings, idx) {
            if (!am->source)
                continue;

            if (nd->profile &&
                pa_idxset_get_by_data(nd->profile->input_mappings, am, NULL))
                continue;

            source_outputs = pa_source_move_all_start(am->source, source_outputs);
            pa_droid_source_free(am->source);
            am->source = NULL;
        }
    }

    if (nd->profile && pa_idxset_size(nd->profile->output_mappings) > 0) {
        PA_IDXSET_FOREACH(am, nd->profile->output_mappings, idx) {
            if (!am->sink)
                am->sink = pa_droid_sink_new(u->module, u->modargs, __FILE__, &u->card_data, 0, am, u->card);

            if (sink_inputs && am->sink) {
                pa_sink_move_all_finish(am->sink, sink_inputs, false);
                sink_inputs = NULL;
            }
        }
    }

    if (nd->profile && pa_idxset_size(nd->profile->input_mappings) > 0) {
        PA_IDXSET_FOREACH(am, nd->profile->input_mappings, idx) {
            if (!am->source)
                am->source = pa_droid_source_new(u->module, u->modargs, __FILE__, (audio_devices_t) 0, &u->card_data, am, u->card);

            if (source_outputs && am->source) {
                pa_source_move_all_finish(am->source, source_outputs, false);
                source_outputs = NULL;
            }
        }
    }

    /* if only primary sink is left after profile change and we have detached sink-inputs attach
     * them to primary sink. */
    if (sink_inputs && primary_sink) {
        pa_sink_move_all_finish(primary_sink, sink_inputs, false);
        sink_inputs = NULL;
    }

    if (sink_inputs)
        pa_sink_move_all_fail(sink_inputs);

    if (source_outputs)
        pa_source_move_all_fail(source_outputs);

    return 0;
}


int pa__init(pa_module *m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    pa_card_new_data data;
    pa_droid_config_audio *config = NULL;
    const char *module_id;
    bool namereg_fail = false;
    pa_card_profile *virtual;
    bool default_profile = true;
    bool merge_inputs = true;
    const char *quirks;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "default_profile", &default_profile) < 0) {
        pa_log("Failed to parse default_profile argument. Expects boolean value");
        goto fail;
    }

    if (pa_modargs_get_value_boolean(ma, "merge_inputs", &merge_inputs) < 0) {
        pa_log("Failed to parse merge_inputs argument. Expects boolean value");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    m->userdata = u;

    module_id = pa_modargs_get_value(ma, "module_id", DEFAULT_MODULE_ID);

    /* First let's find out if hw module has already been opened, or if we need to
     * do it ourself. */
    if (!(u->hw_module = pa_droid_hw_module_get(u->core, NULL, module_id))) {
        /* No hw module object in shared object db, let's open the module now. */
        if (!(config = pa_droid_config_load(ma)))
            goto fail;

        if (!(u->hw_module = pa_droid_hw_module_get(u->core, config, module_id)))
            goto fail;

        pa_droid_config_free(config);
        config = NULL;
    }

    if ((quirks = pa_modargs_get_value(ma, "quirks", NULL))) {
        if (!pa_droid_quirk_parse(u->hw_module, quirks)) {
            pa_log("Failed to parse quirks.");
            goto fail;
        }
    }

    pa_droid_quirk_log(u->hw_module);

    u->card_data.set_parameters = set_parameters_cb;
    u->card_data.module_id = pa_xstrdup(module_id);
    u->card_data.userdata = u;

    if (!default_profile || !pa_streq(module_id, DEFAULT_MODULE_ID))
        u->profile_set = pa_droid_profile_set_new(u->hw_module->enabled_module);
    else
        u->profile_set = pa_droid_profile_set_default_new(u->hw_module->enabled_module, merge_inputs);

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;

    set_card_name(ma, &data, u->hw_module->module_id);

    /* We need to give pa_modargs_get_value_boolean() a pointer to a local
     * variable instead of using &data.namereg_fail directly, because
     * data.namereg_fail is a bitfield and taking the address of a bitfield
     * variable is impossible. */
    namereg_fail = data.namereg_fail;
    if (pa_modargs_get_value_boolean(ma, "namereg_fail", &namereg_fail) < 0) {
        pa_log("Failed to parse namereg_fail argument.");
        pa_card_new_data_done(&data);
        goto fail;
    }
    data.namereg_fail = namereg_fail;

    add_profiles(u, data.profiles, data.ports);

    if (pa_hashmap_isempty(data.profiles)) {
        pa_log("Failed to find a working profile.");
        pa_card_new_data_done(&data);
        goto fail;
    }

    virtual =
    add_virtual_profile(u, VOICE_CALL_PROFILE_NAME, VOICE_CALL_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_profile_event_cb,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    add_virtual_profile(u, VOICE_RECORD_PROFILE_NAME, VOICE_RECORD_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, NULL,
                        PA_AVAILABLE_NO, virtual, data.profiles);
    add_virtual_profile(u, COMMUNICATION_PROFILE_NAME, COMMUNICATION_PROFILE_DESC,
                        AUDIO_MODE_IN_COMMUNICATION, NULL,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    add_virtual_profile(u, RINGTONE_PROFILE_NAME, RINGTONE_PROFILE_DESC,
                        AUDIO_MODE_RINGTONE, NULL,
                        PA_AVAILABLE_YES, NULL, data.profiles);
#ifdef DROID_AUDIO_HAL_USE_VSID
    add_virtual_profile(u, VOICE_SESSION_VOICE1_PROFILE_NAME, VOICE_SESSION_VOICE1_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_voice1_vsid_profile_event_cb,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    add_virtual_profile(u, VOICE_SESSION_VOICE2_PROFILE_NAME, VOICE_SESSION_VOICE2_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_voice2_vsid_profile_event_cb,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    /* TODO: Probably enabled state needs to be determined dynamically for VOLTE and friends. */
    add_virtual_profile(u, VOICE_SESSION_VOLTE_PROFILE_NAME, VOICE_SESSION_VOLTE_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_volte_vsid_profile_event_cb,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    add_virtual_profile(u, VOICE_SESSION_QCHAT_PROFILE_NAME, VOICE_SESSION_QCHAT_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_qchat_vsid_profile_event_cb,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    add_virtual_profile(u, VOICE_SESSION_VOWLAN_PROFILE_NAME, VOICE_SESSION_VOWLAN_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_vowlan_vsid_profile_event_cb,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    add_virtual_profile(u, VOICE_SESSION_VOICEMMODE1_PROFILE_NAME, VOICE_SESSION_VOICEMMODE1_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_voicemmode1_vsid_profile_event_cb,
                        PA_AVAILABLE_YES, NULL, data.profiles);
    add_virtual_profile(u, VOICE_SESSION_VOICEMMODE2_PROFILE_NAME, VOICE_SESSION_VOICEMMODE2_PROFILE_DESC,
                        AUDIO_MODE_IN_CALL, voicecall_voicemmode2_vsid_profile_event_cb,
                        PA_AVAILABLE_YES, NULL, data.profiles);
#endif /* DROID_AUDIO_HAL_USE_VSID */

    add_disabled_profile(data.profiles);

    pa_proplist_sets(data.proplist, PROP_DROID_HW_MODULE, u->hw_module->module_id);

    u->card = pa_card_new(m->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card) {
        pa_log("Couldn't create card.");
        goto fail;
    }

    u->card->userdata = u;
    u->card->set_profile = card_set_profile;

    u->modargs = ma;
    u->module = m;

#if (PULSEAUDIO_VERSION >= 10)
    pa_card_choose_initial_profile(u->card);
#endif
    init_profile(u);

#if (PULSEAUDIO_VERSION >= 10)
    pa_card_put(u->card);
#endif

    return 0;

fail:
    pa_droid_config_free(config);

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if ((u = m->userdata)) {

        if (u->card && u->card->sinks)
            pa_idxset_remove_all(u->card->sinks, (pa_free_cb_t) pa_droid_sink_free);

        if (u->card && u->card->sources)
            pa_idxset_remove_all(u->card->sources, (pa_free_cb_t) pa_droid_source_free);


        if (u->card)
            pa_card_free(u->card);

        if (u->modargs)
            pa_modargs_free(u->modargs);

        if (u->profile_set)
            pa_droid_profile_set_free(u->profile_set);

        if (u->card_data.module_id)
            pa_xfree(u->card_data.module_id);

        if (u->hw_module)
            pa_droid_hw_module_unref(u->hw_module);

        pa_xfree(u);
    }
}
