/*
 * Copyright (C) 2013 Jolla Ltd.
 *
 * Contact: Juho Hämäläinen <juho.hamalainen@tieto.com>
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

#include <hardware/audio.h>
#include <system/audio.h>

//#include <droid/hardware/audio_policy.h>
//#include <droid/system/audio_policy.h>

#include "droid-util.h"
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
        "voice_source_routing=<route source ports during voice call, default FALSE> "
        "deferred_volume=<synchronize software and hardware volume changes to avoid momentary jumps?> "
        "config=<location for droid audio configuration>"
);

static const char* const valid_modargs[] = {
    "card_name",
    "sink_name",
    "source_name",
    "namereg_fail",
    "format",
    "rate",
    "output_flags",
    "module_id",
    "voice_source_routing",
    "deferred_volume",
    "config",
    NULL,
};

#define DEFAULT_MODULE_ID "primary"
#define DEFAULT_AUDIO_POLICY_CONF "/system/etc/audio_policy.conf"
#define VOICE_CALL_PROFILE_NAME "voicecall"

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_droid_profile_set *profile_set;

    pa_droid_config_audio *config;
    pa_droid_hw_module *hw_module;

    struct call_profile_data {
        pa_droid_profile *profile;
        pa_droid_profile *old_profile;
    } call_profile;

    pa_bool_t voice_source_routing;

    pa_modargs *modargs;
    pa_card *card;
};

struct profile_data {
    pa_droid_profile *profile;
};

static void add_disabled_profile(pa_hashmap *profiles) {
    pa_card_profile *cp;
    struct profile_data *d;

    cp = pa_card_profile_new("off", _("Off"), sizeof(struct profile_data));

    d = PA_CARD_PROFILE_DATA(cp);
    d->profile = NULL;

    pa_hashmap_put(profiles, cp->name, cp);
}

/* Special profile for calls */
static void add_call_profile(struct userdata *u, pa_hashmap *profiles) {
    pa_droid_profile *ap;
    pa_card_profile *cp;
    struct profile_data *d;

    pa_assert(u);
    pa_assert(u->profile_set);

    pa_log_debug("New profile: %s", VOICE_CALL_PROFILE_NAME);

    ap = pa_xnew0(pa_droid_profile, 1);
    ap->profile_set = u->profile_set;
    ap->name = pa_sprintf_malloc(VOICE_CALL_PROFILE_NAME);
    ap->description = pa_sprintf_malloc("Call mode");
    ap->priority = 50;

    u->call_profile.profile = ap;

    pa_hashmap_put(u->profile_set->profiles, ap->name, ap);

    cp = pa_card_profile_new(ap->name, ap->description, sizeof(struct profile_data));
    d = PA_CARD_PROFILE_DATA(cp);
    d->profile = ap;

    pa_hashmap_put(profiles, cp->name, cp);
}

static void set_parameters_cb(pa_droid_hw_module *module, const char *str) {
    pa_assert(module);
    pa_assert(str);

    if (module->device)
        module->device->set_parameters(module->device, str);
}

static void set_card_name(pa_modargs *ma, pa_card_new_data *data, const char *module_id) {
    const char *tmp;
    char *name;

    pa_assert(ma);
    pa_assert(data);
    pa_assert(module_id);

    if ((tmp = pa_modargs_get_value(ma, "card_name", NULL))) {
        pa_card_new_data_set_name(data, tmp);
        data->namereg_fail = TRUE;
        return;
    }

    name = pa_sprintf_malloc("droid_card.%s", module_id);
    pa_card_new_data_set_name(data, name);
    pa_xfree(name);
    data->namereg_fail = FALSE;
}

static void add_profile(struct userdata *u, pa_hashmap *h, pa_hashmap *ports, pa_droid_profile *ap) {
    pa_card_profile *cp;
    struct profile_data *d;

    pa_assert(u);
    pa_assert(h);
    pa_assert(ports);
    pa_assert(ap);

    pa_log_debug("Card profile %s", ap->name);

    cp = pa_card_profile_new(ap->name, ap->description, sizeof(struct profile_data));
    cp->priority = ap->priority;

    cp->n_sinks = 1;
    pa_droid_add_card_ports(cp, ports, ap->output, u->core);
    cp->max_sink_channels = popcount(ap->output->output->channel_masks);
    if (ap->input) {
        pa_droid_add_card_ports(cp, ports, ap->input, u->core);
        cp->n_sources = 1;
        cp->max_source_channels = popcount(ap->input->input->channel_masks);
    }

    d = PA_CARD_PROFILE_DATA(cp);
    d->profile = ap;

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

    pa_assert(u);

    pa_log_debug("Init profile.");

    d = PA_CARD_PROFILE_DATA(u->card->active_profile);

    if (d->profile && d->profile->output) {
        am = d->profile->output;
        am->sink = pa_droid_sink_new(u->module, u->modargs, __FILE__, u->hw_module, 0, am, u->card);
    }

    if (d->profile && d->profile->input) {
        am = d->profile->input;
        am->source = pa_droid_source_new(u->module, u->modargs, __FILE__, u->hw_module, am, u->card);
    }
}

static int set_call_mode(struct userdata *u, audio_mode_t mode) {
    pa_assert(u);
    pa_assert(u->hw_module);
    pa_assert(u->hw_module->device);

    pa_log_debug("Set mode to %s.", mode == AUDIO_MODE_IN_CALL ? "AUDIO_MODE_IN_CALL" : "AUDIO_MODE_NORMAL");

    if (u->hw_module->device->set_mode(u->hw_module->device, mode) < 0) {
        pa_log("Failed to set mode.");
        return -1;
    }

    return 0;
}

static int card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    struct userdata *u;
    pa_droid_mapping *am;
    struct profile_data *nd, *od;
    pa_queue *sink_inputs = NULL, *source_outputs = NULL;

    pa_assert(c);
    pa_assert(new_profile);
    pa_assert_se(u = c->userdata);

    nd = PA_CARD_PROFILE_DATA(new_profile);
    od = PA_CARD_PROFILE_DATA(c->active_profile);

    if (nd->profile == u->call_profile.profile) {
        pa_assert(u->call_profile.old_profile == NULL);

        set_call_mode(u, AUDIO_MODE_IN_CALL);

        /* Transfer ownership of sinks and sources to
         * call profile */
        u->call_profile.old_profile = od->profile;
        pa_droid_sink_set_voice_control(u->call_profile.old_profile->output->sink, TRUE);
        if (!u->voice_source_routing)
            pa_droid_source_set_routing(u->call_profile.old_profile->input->source, FALSE);
        return 0;
    }

    if (od->profile == u->call_profile.profile) {
        pa_assert(u->call_profile.old_profile);

        set_call_mode(u, AUDIO_MODE_NORMAL);
        pa_droid_sink_set_voice_control(u->call_profile.old_profile->output->sink, FALSE);
        if (!u->voice_source_routing)
            pa_droid_source_set_routing(u->call_profile.old_profile->input->source, TRUE);

        /* If new profile is the same as from which we switched to
         * call profile, transfer ownership back to that profile.
         * Otherwise destroy sinks & sources and switch to new profile. */
        if (nd->profile == u->call_profile.old_profile) {
            u->call_profile.old_profile = NULL;
            return 0;
        } else {
            od->profile = u->call_profile.old_profile;
            u->call_profile.old_profile = NULL;

            /* Continue to sink-input transfer below */
        }
    }

    /* If there are connected sink inputs/source outputs in old profile's sinks/sources move
     * them all to new sinks/sources. */

    if (od->profile && od->profile->output) {
        do {
            am = od->profile->output;

            if (!am->sink)
                continue;

            if (nd->profile && nd->profile->output && am == nd->profile->output)
                continue;

            sink_inputs = pa_sink_move_all_start(am->sink, sink_inputs);
            pa_droid_sink_free(am->sink);
            am->sink = NULL;
        } while(0);
    }

    if (od->profile && od->profile->input) {
        do {
            am = od->profile->input;

            if (!am->source)
                continue;

            if (nd->profile && nd->profile->input && am == nd->profile->input)
                continue;

            source_outputs = pa_source_move_all_start(am->source, source_outputs);
            pa_droid_source_free(am->source);
            am->source = NULL;
        } while(0);
    }

    if (nd->profile && nd->profile->output) {
        am = nd->profile->output;

        if (!am->sink)
            am->sink = pa_droid_sink_new(u->module, u->modargs, __FILE__, u->hw_module, 0, am, u->card);

        if (sink_inputs && am->sink) {
            pa_sink_move_all_finish(am->sink, sink_inputs, FALSE);
            sink_inputs = NULL;
        }
    }

    if (nd->profile && nd->profile->input) {
        am = nd->profile->input;

        if (!am->source)
            am->source = pa_droid_source_new(u->module, u->modargs, __FILE__, u->hw_module, am, u->card);

        if (source_outputs && am->source) {
            pa_source_move_all_finish(am->source, source_outputs, FALSE);
            source_outputs = NULL;
        }
    }

    if (sink_inputs)
        pa_sink_move_all_fail(sink_inputs);

    if (source_outputs)
        pa_source_move_all_fail(source_outputs);

    return 0;
}


int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    pa_card_new_data data;
    const char *module_id;
    pa_bool_t namereg_fail = FALSE;
    pa_bool_t voice_source_routing = FALSE;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module argumets.");
        goto fail;
    }

    struct userdata *u = pa_xnew0(struct userdata, 1);
    u->core = m->core;

    if (!(u->config = pa_droid_config_load(ma)))
        goto fail;

    if (pa_modargs_get_value_boolean(ma, "voice_source_routing", &voice_source_routing) < 0) {
        pa_log("Failed to parse voice_source_routing argument.");
        goto fail;
    }
    u->voice_source_routing = voice_source_routing;

    module_id = pa_modargs_get_value(ma, "module_id", DEFAULT_MODULE_ID);

    if (!(u->hw_module = pa_droid_hw_module_open(u->config, module_id, u)))
        goto fail;

    u->hw_module->set_parameters = set_parameters_cb;

    u->profile_set = pa_droid_profile_set_new(u->hw_module->enabled_module);

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

    add_call_profile(u, data.profiles);

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

    m->userdata = u;

    init_profile(u);


    return 0;

fail:
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

        if (u->hw_module)
            pa_droid_hw_module_close(u->hw_module);

        if (u->config)
            pa_xfree(u->config);

        pa_xfree(u);
    }
}
