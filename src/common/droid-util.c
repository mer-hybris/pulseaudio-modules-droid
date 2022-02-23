/*
 * Copyright (C) 2013-2022 Jolla Ltd.
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

#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
#include <stdarg.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>
#include <pulse/direction.h>
#include <pulse/util.h>

#include <pulsecore/core.h>
#include <pulsecore/core-error.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/sink.h>
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
#include <pulsecore/refcnt.h>
#include <pulsecore/shared.h>
#include <pulsecore/mutex.h>
#include <pulsecore/strlist.h>
#include <pulsecore/atomic.h>

#include "droid/version.h"
#include "droid/droid-util.h"
#include "droid/droid-config.h"
#include "droid/conversion.h"
#include "droid/sllist.h"
#include "droid/utils.h"

struct droid_option {
    const char *name;
    uint32_t value;
};

struct droid_option valid_options[] = {
    { "input_atoi",             DM_OPTION_INPUT_ATOI            },
    { "close_input",            DM_OPTION_CLOSE_INPUT           },
    { "unload_no_close",        DM_OPTION_UNLOAD_NO_CLOSE       },
    { "hw_volume",              DM_OPTION_HW_VOLUME             },
    { "realcall",               DM_OPTION_REALCALL              },
    { "unload_call_exit",       DM_OPTION_UNLOAD_CALL_EXIT      },
    { "output_fast",            DM_OPTION_OUTPUT_FAST           },
    { "output_deep_buffer",     DM_OPTION_OUTPUT_DEEP_BUFFER    },
    { "audio_cal_wait",         DM_OPTION_AUDIO_CAL_WAIT        },
    { "speaker_before_voice",   DM_OPTION_SPEAKER_BEFORE_VOICE  },
    { "output_voip_rx",         DM_OPTION_OUTPUT_VOIP_RX        },
    { "record_voice_16k",       DM_OPTION_RECORD_VOICE_16K      },
};

struct user_options {
    struct user_option {
        bool enable;
        bool set;
    } options[DM_OPTION_COUNT];
};

#define DM_OPTION_AUDIO_CAL_WAIT_S  (10)
#define DM_OPTION_AUDIO_CAL_FILE    "/data/vendor/audio/cirrus_sony.cal"
#define DM_OPTION_AUDIO_CAL_GROUP   "audio"
#define DM_OPTION_AUDIO_CAL_MODE    (0664)

#define DEFAULT_PRIORITY            (100)
#define DEFAULT_AUDIO_FORMAT        (AUDIO_FORMAT_PCM_16_BIT)


#ifndef AUDIO_PARAMETER_VALUE_ON
#define AUDIO_PARAMETER_VALUE_ON    "on"
#endif

#ifndef AUDIO_PARAMETER_VALUE_OFF
#define AUDIO_PARAMETER_VALUE_OFF   "off"
#endif

#define AUDIO_PARAMETER_BT_SCO_ON   "BT_SCO=" AUDIO_PARAMETER_VALUE_ON
#define AUDIO_PARAMETER_BT_SCO_OFF  "BT_SCO=" AUDIO_PARAMETER_VALUE_OFF

#define DROID_HW_HANDLE_V1          "droid.handle.v1"
#define DROID_SET_PARAMETERS_V1     "droid.set_parameters.v1"
#define DROID_GET_PARAMETERS_V1     "droid.get_parameters.v1"

static void droid_port_free(pa_droid_port *p);

static int input_stream_set_route(pa_droid_stream *stream, const dm_config_port *device_port);
static int droid_set_parameters(pa_droid_hw_module *hw, const char *parameters);
static bool droid_set_audio_source(pa_droid_stream *stream, audio_source_t audio_source);
static void add_output_ports(pa_droid_mapping *droid_mapping, dm_config_port *device_port);
static void add_input_ports(pa_droid_mapping *droid_mapping, dm_config_port *device_port);
static void audio_patch_release(pa_droid_stream *stream);

static pa_droid_profile *profile_new(pa_droid_profile_set *ps,
                                     dm_config_module *module,
                                     const char *name,
                                     const char *description) {
    pa_droid_profile *p;

    pa_assert(ps);
    pa_assert(module);
    pa_assert(name);
    pa_assert(description);

    p = pa_xnew0(pa_droid_profile, 1);
    p->profile_set = ps;
    p->module = module;
    p->name = pa_xstrdup(name);
    p->description = pa_xstrdup(description);
    p->priority = DEFAULT_PRIORITY;

    p->output_mappings = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    p->input_mappings = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    pa_hashmap_put(ps->profiles, p->name, p);

    return p;
}

static pa_droid_profile_set *profile_set_new(dm_config_module *module) {
    pa_droid_profile_set *ps;

    pa_assert(module);

    ps = pa_xnew0(pa_droid_profile_set, 1);
    ps->config = module->config;
    ps->profiles        = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                              NULL, (pa_free_cb_t) pa_droid_profile_free);
    ps->output_mappings = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                              NULL, (pa_free_cb_t) pa_droid_mapping_free);
    ps->input_mappings  = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                              NULL, (pa_free_cb_t) pa_droid_mapping_free);
    ps->all_ports       = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                              NULL, (pa_free_cb_t) droid_port_free);

    return ps;
}

static pa_droid_mapping *mapping_by_name(pa_idxset *idxset, const char *name) {
    pa_droid_mapping *droid_mapping;
    uint32_t idx;

    PA_IDXSET_FOREACH(droid_mapping, idxset, idx) {
        if (pa_streq(droid_mapping->mix_port->name, name))
            return droid_mapping;
    }

    return NULL;
}

static pa_droid_mapping *droid_mapping_update(pa_droid_mapping *droid_mapping,
                                              pa_droid_profile_set *profile_set,
                                              dm_config_module *module,
                                              dm_config_port *mix_port,
                                              dm_config_port *device_port) {
    pa_hashmap *map;
    bool output_mapping = true;

    pa_assert(profile_set);
    pa_assert(module);
    pa_assert(mix_port);
    pa_assert(device_port);

    if (mix_port->role == DM_CONFIG_ROLE_SINK)
        output_mapping = false;

    map = output_mapping ? profile_set->output_mappings : profile_set->input_mappings;

    if (!(droid_mapping = pa_hashmap_get(map, mix_port->name))) {
        pa_log_debug("New %s mapping \"%s\"", output_mapping ? "output" : "input", mix_port->name);

        droid_mapping = pa_xnew0(pa_droid_mapping, 1);
        droid_mapping->module = module;
        droid_mapping->profile_set = profile_set;
        droid_mapping->proplist = pa_proplist_new();
        droid_mapping->direction = output_mapping ? PA_DIRECTION_OUTPUT : PA_DIRECTION_INPUT;
        droid_mapping->ports = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);;
        droid_mapping->mix_port = mix_port;
        droid_mapping->device_ports = dm_list_new();
        droid_mapping->name = pa_xstrdup(mix_port->name);

        pa_hashmap_put(map, droid_mapping->name, droid_mapping);
    }

    /* Device ports associated with the mapping */
    dm_list_push_back(droid_mapping->device_ports, device_port);

    if (droid_mapping->direction == PA_DIRECTION_OUTPUT)
        add_output_ports(droid_mapping, device_port);
    else
        add_input_ports(droid_mapping, device_port);

    return droid_mapping;

}

#define ALLOWED_OUTPUT_TYPES (AUDIO_OUTPUT_FLAG_FAST | AUDIO_OUTPUT_FLAG_PRIMARY | AUDIO_OUTPUT_FLAG_DEEP_BUFFER)

static void update_mapping(pa_droid_profile_set *profile_set,
                           pa_droid_profile *profile,
                           dm_config_module *module,
                           dm_config_port *source,
                           dm_config_port *sink) {


    pa_droid_mapping *droid_mapping = NULL;
    pa_idxset *mappings;
    bool put = true;

    /*                                    source        sink
     * For output routes    PulseAudio -> mixPort    -> devicePort
     *     input routes                   devicePort -> mixPort -> PulseAudio
     */
    if (source->port_type == DM_CONFIG_TYPE_MIX_PORT &&
        sink->port_type == DM_CONFIG_TYPE_DEVICE_PORT) {

        mappings = profile->output_mappings;

        if ((droid_mapping = mapping_by_name(mappings, source->name)))
            put = false;

        droid_mapping = droid_mapping_update(droid_mapping, profile_set, module, source, sink);

    } else if (source->port_type == DM_CONFIG_TYPE_DEVICE_PORT &&
               sink->port_type == DM_CONFIG_TYPE_MIX_PORT) {

        mappings = profile->input_mappings;

        if ((droid_mapping = mapping_by_name(mappings, sink->name)))
            put = false;

        droid_mapping = droid_mapping_update(droid_mapping, profile_set, module, sink, source);

    } else {
        pa_log("Internal data structures are confused.");
        pa_assert_not_reached();
    }


    if (put)
        pa_idxset_put(mappings, droid_mapping, NULL);
}

static void auto_add_profiles(pa_droid_profile_set *profile_set,
                              dm_config_module *module) {
    pa_droid_profile *profile;
    dm_config_route *route;
    void *state, *state2;

    pa_assert(profile_set);
    pa_assert(module);

    profile = profile_new(profile_set, module, "default", "Default profile");

    /* Droid profiles, mappings and ports are genrated like this:
     *
     * 1. route definitions in audio policy configuration are iterated through
     * 2. for every route, update_mapping is called for every combination of
     *    sink and source, so practically the function is called many times
     *    with identical sink and different source
     *
     * PulseAudio internals and audio policy configuration are mapped like this:
     *
     * For outputs:
     *
     * audio policy xml     | port type     | PulseAudio
     * ---------------------|---------------|------------------------
     * mixPort              | source        | pa_droid_mapping
     * devicePort           | sink          | pa_droid_port
     *
     * For inputs:
     *
     * audio policy xml     | port type     | PulseAudio
     * ---------------------|---------------|------------------------
     * mixPort              | sink          | pa_droid_mapping
     * devicePort           | source        | pa_droid_port
     *
     * In other words, for every mixPort there will be one sink or source,
     * and for every devicePort there will be one output or input port used
     * by sinks and sources.
     */

    DM_LIST_FOREACH_DATA(route, module->routes, state) {
        dm_config_port *source;

        DM_LIST_FOREACH_DATA(source, route->sources, state2) {
            update_mapping(profile_set, profile, module, source, route->sink);
        }
    }
}

pa_droid_profile_set *pa_droid_profile_set_default_new(dm_config_module *module) {
    pa_droid_profile_set *ps;

    ps = profile_set_new(module);
    auto_add_profiles(ps, module);

    return ps;
}

void pa_droid_mapping_free(pa_droid_mapping *am) {
    pa_assert(am);

    pa_xfree(am->name);
    pa_proplist_free(am->proplist);
    pa_idxset_free(am->ports, NULL);
    dm_list_free(am->device_ports, NULL);
    pa_xfree(am);
}

void pa_droid_profile_free(pa_droid_profile *ap) {
    pa_assert(ap);

    pa_xfree(ap->name);
    pa_xfree(ap->description);
    if (ap->output_mappings)
        pa_idxset_free(ap->output_mappings, NULL);
    if (ap->input_mappings)
        pa_idxset_free(ap->input_mappings, NULL);
    ap->input_mapping = NULL;
    pa_xfree(ap);
}

static void droid_port_free(pa_droid_port *p) {
    pa_assert(p);

    pa_xfree(p->name);
    pa_xfree(p->description);
    pa_xfree(p);
}

void pa_droid_profile_set_free(pa_droid_profile_set *ps) {
    pa_assert(ps);

    if (ps->output_mappings)
        pa_hashmap_free(ps->output_mappings);

    if (ps->input_mappings)
        pa_hashmap_free(ps->input_mappings);

    if (ps->all_ports)
        pa_hashmap_free(ps->all_ports);

    if (ps->profiles)
        pa_hashmap_free(ps->profiles);

    pa_xfree(ps);
}

static pa_droid_port *create_output_port(pa_droid_mapping *am,
                                         dm_config_port *device_port,
                                         const char *name,
                                         const char *description) {
    pa_droid_port *p;

    pa_assert(am);
    pa_assert(name);

    p = pa_xnew0(pa_droid_port, 1);

    p->mapping = am;
    p->name = pa_xstrdup(name);
    if (description) {
        p->description = pa_xstrdup(description);
    } else {
        p->description = pa_replace(name, "output-", "Output to ");
        dm_replace_in_place(&p->description, "_", " ");
    }
    p->priority = DEFAULT_PRIORITY;
    p->device_port = device_port;

    if (device_port) {
        if (am->module->attached_devices) {
            const dm_config_port *attached_port;
            void *state;

            DM_LIST_FOREACH_DATA(attached_port, am->module->attached_devices, state) {
                if (attached_port->type == device_port->type) {
                    p->priority += DEFAULT_PRIORITY;
                    break;
                }
            }
        }

        if (am->module->default_output_device &&
            am->module->default_output_device->type == device_port->type)
            p->priority += DEFAULT_PRIORITY;
    }

    return p;
}

static void add_output_ports(pa_droid_mapping *droid_mapping, dm_config_port *device_port) {
    pa_droid_port *port;
    const char *name;
    bool new = false;

    pa_assert(droid_mapping);

    pa_assert_se(pa_droid_output_port_name(device_port->type, &name));

    /* First add parking port */

    if (!(port = pa_hashmap_get(droid_mapping->profile_set->all_ports, PA_DROID_OUTPUT_PARKING))) {
        /* Create parking port for output mapping to be used when audio_mode_t changes. */
        port = create_output_port(droid_mapping, NULL, PA_DROID_OUTPUT_PARKING, "Parking port");
        /* Reset priority to half of default */
        port->priority = DEFAULT_PRIORITY / 2;

        pa_hashmap_put(droid_mapping->profile_set->all_ports, port->name, port);
    }

    pa_idxset_put(droid_mapping->ports, port, NULL);

    /* Then add normal port */

    if (!(port = pa_hashmap_get(droid_mapping->profile_set->all_ports, name))) {
        new = true;
        port = create_output_port(droid_mapping, device_port, name, NULL);
        pa_hashmap_put(droid_mapping->profile_set->all_ports, port->name, port);
    }

    if (new)
        pa_log_debug("  Mapping %s add new output port %s", droid_mapping->name, name);
    else
        pa_log_debug("  Mapping %s add output port %s from cache", droid_mapping->name, name);

    pa_idxset_put(droid_mapping->ports, port, NULL);
}

static pa_droid_port *create_input_port(pa_droid_mapping *am,
                                        dm_config_port *device_port,
                                        const char *name,
                                        const char *description) {
    pa_droid_port *p;

    pa_assert(am);
    pa_assert(name);

    p = pa_xnew0(pa_droid_port, 1);

    p->mapping = am;
    p->name = pa_xstrdup(name);
    if (description) {
        p->description = pa_xstrdup(description);
    } else {
        p->description = pa_replace(name, "input-", "Input from ");
        dm_replace_in_place(&p->description, "_", " ");
    }
    p->priority = DEFAULT_PRIORITY;
    p->device_port = device_port;

    if (device_port) {
        if (am->module->attached_devices) {
            const dm_config_port *attached_port;
            void *state;

            DM_LIST_FOREACH_DATA(attached_port, am->module->attached_devices, state) {
                if (attached_port->type == device_port->type) {
                    p->priority += DEFAULT_PRIORITY;
                    break;
                }
            }
        }
    }

    return p;
}

static void add_input_ports(pa_droid_mapping *droid_mapping, dm_config_port *device_port) {
    pa_droid_port *port;
    const char *name;
    bool new = false;

    pa_assert(droid_mapping);
    pa_assert(device_port);

    pa_assert_se(pa_droid_input_port_name(device_port->type, &name));

    /* First add parking port */

    if (!(port = pa_hashmap_get(droid_mapping->profile_set->all_ports, PA_DROID_INPUT_PARKING))) {
        /* Create parking port for input mapping to be used when audio_mode_t changes. */
        port = create_input_port(droid_mapping, NULL, PA_DROID_INPUT_PARKING, "Parking port");
        /* Reset priority to half of default */
        port->priority = DEFAULT_PRIORITY / 2;

        pa_hashmap_put(droid_mapping->profile_set->all_ports, port->name, port);
    }

    pa_idxset_put(droid_mapping->ports, port, NULL);

    /* Then add normal port */

    if (!(port = pa_hashmap_get(droid_mapping->profile_set->all_ports, name))) {
        new = true;
        port = create_input_port(droid_mapping, device_port, name, NULL);
        pa_hashmap_put(droid_mapping->profile_set->all_ports, port->name, port);
    }

    if (new)
        pa_log_debug("  Mapping %s add new input port %s", droid_mapping->name, name);
    else
        pa_log_debug("  Mapping %s add input port %s from cache", droid_mapping->name, name);

    pa_idxset_put(droid_mapping->ports, port, NULL);
}

bool pa_droid_mapping_is_primary(pa_droid_mapping *am) {
    pa_assert(am);

    if (am->direction == PA_DIRECTION_OUTPUT) {
        pa_assert(am->mix_port);
        return am->mix_port->flags & AUDIO_OUTPUT_FLAG_PRIMARY;
    }

    return true;
}

pa_droid_mapping *pa_droid_idxset_get_primary(pa_idxset *i) {
    pa_droid_mapping *am;
    uint32_t idx;

    pa_assert(i);

    PA_IDXSET_FOREACH(am, i, idx) {
        if (pa_droid_mapping_is_primary(am))
            return am;
    }

    return NULL;
}

static int add_ports(pa_core *core, pa_card_profile *cp, pa_hashmap *ports, pa_droid_mapping *am, pa_hashmap *extra) {
    pa_droid_port *p;
    pa_device_port_new_data dp_data;
    pa_device_port *dp;
    pa_droid_port_data *data;
    uint32_t idx;
    int count = 0;

    if (am->direction == PA_DIRECTION_OUTPUT && !(am->mix_port->flags & AUDIO_OUTPUT_FLAG_PRIMARY)) {
        /* Don't create ports for other than primary sink. */
        return 0;
    }

    pa_log_debug("Ports for %s%s: %s", cp ? "card " : "", am->direction == PA_DIRECTION_OUTPUT ? "output" : "input", am->name);

    PA_IDXSET_FOREACH(p, am->ports, idx) {
        if (!(dp = pa_hashmap_get(ports, p->name))) {
            pa_log_debug("  New port %s", p->name);
            pa_device_port_new_data_init(&dp_data);
            pa_device_port_new_data_set_name(&dp_data, p->name);
            pa_device_port_new_data_set_description(&dp_data, p->description);
            pa_device_port_new_data_set_direction(&dp_data, p->mapping->direction);
            pa_device_port_new_data_set_available(&dp_data, PA_AVAILABLE_YES);

            dp = pa_device_port_new(core, &dp_data, sizeof(pa_droid_port_data));
            dp->priority = p->priority;

            pa_device_port_new_data_done(&dp_data);

            pa_hashmap_put(ports, dp->name, dp);
            dp->profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

            data = PA_DEVICE_PORT_DATA(dp);
            data->device_port = p->device_port;
        } else
            pa_log_debug("  Port %s from cache", p->name);

        if (cp) {
            if (!pa_hashmap_get(dp->profiles, cp->name))
                pa_hashmap_put(dp->profiles, cp->name, cp);
        }

        count++;

        if (extra) {
            if (!pa_hashmap_get(extra, dp->name)) {
                pa_hashmap_put(extra, dp->name, dp);
                pa_device_port_ref(dp);
            }
        }
    }

    return count;
}


void pa_droid_add_ports(pa_hashmap *p, pa_droid_mapping *am, pa_card *card) {
    pa_assert(p);

    add_ports(card->core, NULL, card->ports, am, p);
}

void pa_droid_add_card_ports(pa_card_profile *cp, pa_hashmap *ports, pa_droid_mapping *am, pa_core *core) {
    pa_assert(cp);
    pa_assert(am);
    pa_assert(core);

    add_ports(core, cp, ports, am, NULL);
}

void pa_droid_options_log(pa_droid_hw_module *hw) {
    uint32_t i;

    pa_assert(hw);

    pa_log_debug("Module options:");
    for (i = 0; i < sizeof(valid_options) / sizeof(struct droid_option); i++) {
        pa_log_debug("  [%s] %s", hw->options.enabled[i] ? "x" : " ", valid_options[i].name);
    }
}

static void set_options(pa_droid_options *options,
                        const audio_hw_device_t *hw_device,
                        const struct user_options *user_options) {
    pa_assert(options);

    memset(options, 0, sizeof(*options));

    /* First set defaults */

    options->enabled[DM_OPTION_CLOSE_INPUT] = true;
    options->enabled[DM_OPTION_OUTPUT_FAST] = true;
    options->enabled[DM_OPTION_OUTPUT_DEEP_BUFFER] = true;
    options->enabled[DM_OPTION_HW_VOLUME] = true;
    options->enabled[DM_OPTION_OUTPUT_VOIP_RX] = true;

#if (ANDROID_VERSION_MAJOR >= 5) || defined(DROID_AUDIO_HAL_ATOI_FIX)
    options->enabled[DM_OPTION_INPUT_ATOI] = true;
#endif

    /* Then override by user defined options */

    if (user_options) {
        int i;

        for (i = 0; i < DM_OPTION_COUNT; i++) {
            if (user_options->options[i].set)
                options->enabled[i] = user_options->options[i].enable;
        }
    }
}

static bool droid_options_parse(struct user_options *user_options, pa_modargs *ma) {
    const char *value;
    int i;

    pa_assert(user_options);
    pa_assert(ma);

    memset(user_options, 0, sizeof(*user_options));

    for (i = 0; i < DM_OPTION_COUNT; i++) {
        if ((value = pa_modargs_get_value(ma, valid_options[i].name, NULL))) {
            if (pa_modargs_get_value_boolean(ma, valid_options[i].name, &user_options->options[i].enable) < 0) {
                pa_log("Failed to parse module option %s=%s (needs boolean value).", valid_options[i].name, value);
                return false;
            } else {
                user_options->options[i].set = true;
            }
        }
    }

    return true;
}

static void update_sink_types(pa_droid_hw_module *hw, pa_sink *ignore_sink) {
    pa_sink *sink;
    pa_sink *primary_sink       = NULL;
    pa_sink *low_latency_sink   = NULL;
    pa_sink *media_latency_sink = NULL;
    pa_sink *offload_sink       = NULL;
    pa_sink *voip_sink          = NULL;

    pa_droid_stream *s;
    uint32_t idx;

    /* only update primary hw module types for now. */
    if (!pa_streq(hw->module_id, PA_DROID_PRIMARY_DEVICE))
        return;

    PA_IDXSET_FOREACH(s, hw->outputs, idx) {
        if (!(sink = pa_droid_stream_get_data(s)))
            continue;

        if (sink == ignore_sink)
            continue;

        if (s->mix_port->flags & AUDIO_OUTPUT_FLAG_PRIMARY)
            primary_sink = sink;

        if (pa_droid_option(hw, DM_OPTION_OUTPUT_FAST) && s->mix_port->flags & AUDIO_OUTPUT_FLAG_FAST)
            low_latency_sink = sink;

        if (pa_droid_option(hw, DM_OPTION_OUTPUT_DEEP_BUFFER) && s->mix_port->flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER)
            media_latency_sink = sink;

        if (pa_droid_option(hw, DM_OPTION_OUTPUT_VOIP_RX) && s->mix_port->flags & AUDIO_OUTPUT_FLAG_VOIP_RX)
            voip_sink = sink;

#if defined(HAVE_ENUM_AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
        if (s->mix_port->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
            offload_sink = sink;
#endif
    }

    if (primary_sink == low_latency_sink)
        low_latency_sink = NULL;

    if (primary_sink == media_latency_sink)
        media_latency_sink = NULL;

    if (primary_sink == voip_sink)
        voip_sink = NULL;

    if (low_latency_sink)
        pa_proplist_sets(low_latency_sink->proplist, PROP_DROID_OUTPUT_LOW_LATENCY, "true");

    if (media_latency_sink)
        pa_proplist_sets(media_latency_sink->proplist, PROP_DROID_OUTPUT_MEDIA_LATENCY, "true");

    if (offload_sink)
        pa_proplist_sets(offload_sink->proplist, PROP_DROID_OUTPUT_OFFLOAD, "true");

    if (voip_sink)
        pa_proplist_sets(voip_sink->proplist, PROP_DROID_OUTPUT_VOIP, "true");

    if (primary_sink) {
        pa_proplist_sets(primary_sink->proplist, PROP_DROID_OUTPUT_PRIMARY, "true");
        pa_proplist_sets(primary_sink->proplist, PROP_DROID_OUTPUT_LOW_LATENCY, low_latency_sink ? "false" : "true");
        pa_proplist_sets(primary_sink->proplist, PROP_DROID_OUTPUT_MEDIA_LATENCY, media_latency_sink ? "false" : "true");
        pa_proplist_sets(primary_sink->proplist, PROP_DROID_OUTPUT_VOIP, voip_sink ? "false" : "true");
    }
}

static pa_hook_result_t sink_put_hook_cb(void *hook_data, void *call_data, void *slot_data) {
    pa_sink *sink           = call_data;
    pa_droid_hw_module *hw  = slot_data;

    if (!pa_sink_is_droid_sink(sink))
        return PA_HOOK_OK;

    update_sink_types(hw, NULL);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_unlink_hook_cb(void *hook_data, void *call_data, void *slot_data) {
    pa_sink *sink           = call_data;
    pa_droid_hw_module *hw  = slot_data;

    if (!pa_sink_is_droid_sink(sink))
        return PA_HOOK_OK;

    update_sink_types(hw, sink);

    return PA_HOOK_OK;
}

static char *shared_name_get(const char *module_id) {
    pa_assert(module_id);
    return pa_sprintf_malloc("droid-hardware-module-%s", module_id);
}

static void option_audio_cal(pa_droid_hw_module *hw, uint32_t flags) {
    struct group *grp;

    pa_assert(hw);

    if (!pa_droid_option(hw, DM_OPTION_AUDIO_CAL_WAIT))
        return;

    if (access(DM_OPTION_AUDIO_CAL_FILE, F_OK) == 0) {
        if (flags & AUDIO_OUTPUT_FLAG_PRIMARY) {
            pa_log_info("Waiting for audio calibration to load.");
            /* 1 second is enough, so let's double that. */
            pa_msleep(2 * PA_MSEC_PER_SEC);
        }
        return;
    }

    pa_log_info("Waiting for audio calibration to finish... (%d seconds)", DM_OPTION_AUDIO_CAL_WAIT_S);

    /* First wait until the calibration file appears on file system. */
    for (int i = 0; i < DM_OPTION_AUDIO_CAL_WAIT_S; i++) {
        pa_log_debug("%d...", DM_OPTION_AUDIO_CAL_WAIT_S - i);
        pa_msleep(PA_MSEC_PER_SEC);
        if (access(DM_OPTION_AUDIO_CAL_FILE, F_OK) == 0) {
            pa_log_debug("Calibration file " DM_OPTION_AUDIO_CAL_FILE " appeared, wait one second more.");
            /* Then wait for a bit more. */
            pa_msleep(PA_MSEC_PER_SEC);
            break;
        }
    }

    if (access(DM_OPTION_AUDIO_CAL_FILE, F_OK) != 0)
        goto fail;

    if (!(grp = getgrnam(DM_OPTION_AUDIO_CAL_GROUP))) {
        pa_log("couldn't get gid for " DM_OPTION_AUDIO_CAL_GROUP);
        goto fail;
    }

    if (chown(DM_OPTION_AUDIO_CAL_FILE, getuid(), grp->gr_gid) < 0) {
        pa_log("chown failed for " DM_OPTION_AUDIO_CAL_FILE);
        goto fail;
    }

    if (chmod(DM_OPTION_AUDIO_CAL_FILE, DM_OPTION_AUDIO_CAL_MODE) < 0) {
        pa_log("chmod failed for " DM_OPTION_AUDIO_CAL_FILE);
        goto fail;
    }

    pa_log_info("Done waiting for audio calibration.");

    return;

fail:
    if (access(DM_OPTION_AUDIO_CAL_FILE, F_OK) == 0)
        unlink(DM_OPTION_AUDIO_CAL_FILE);

    pa_log("Audio calibration file generation failed! (" DM_OPTION_AUDIO_CAL_FILE " doesn't exist)");
}

static int droid_set_parameters_v1_cb(void *handle, const char *key_value_pairs) {
    pa_droid_hw_module *hw = handle;
    int ret = 0;

    pa_assert(hw);
    pa_assert(key_value_pairs);

    pa_log_debug(DROID_SET_PARAMETERS_V1 "(\"%s\")", key_value_pairs);

    pa_droid_hw_module_lock(hw);
    ret = hw->device->set_parameters(hw->device, key_value_pairs);
    pa_droid_hw_module_unlock(hw);

    if (ret != 0)
        pa_log_warn(DROID_SET_PARAMETERS_V1 "(\"%s\") failed: %d", key_value_pairs, ret);

    return ret;
}

static char *droid_get_parameters_v1_cb(void *handle, const char *keys) {
    pa_droid_hw_module *hw = handle;
    char *key_value_pairs = NULL;

    pa_assert(hw);
    pa_assert(keys);

    pa_droid_hw_module_lock(hw);
    key_value_pairs = hw->device->get_parameters(hw->device, keys);
    pa_droid_hw_module_unlock(hw);

    pa_log_debug(DROID_GET_PARAMETERS_V1 "(\"%s\"): \"%s\"", keys, key_value_pairs ? key_value_pairs : "<null>");

    return key_value_pairs;
}

static pa_droid_hw_module *droid_hw_module_open(pa_core *core, dm_config_device *config,
                                                const char *module_id, const struct user_options *user_options) {
    const dm_config_module *module;
    pa_droid_hw_module *hw = NULL;
    struct hw_module_t *hwmod = NULL;
    audio_hw_device_t *device = NULL;
    int ret;

    pa_assert(core);
    pa_assert(module_id);

    if (!config) {
        pa_log_debug("No configuration provided for opening module with id %s", module_id);
        goto fail;
    }

    pa_log_info("Droid hw module %s", VERSION);

    if (!(module = dm_config_find_module(config, module_id))) {
        pa_log("Couldn't find module with id %s", module_id);
        goto fail;
    }

    ret = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, module->name, (const hw_module_t**) &hwmod);
    if (ret) {
        pa_log("Failed to load audio hw module %s.%s : %s (%d)", AUDIO_HARDWARE_MODULE_ID, module->name,
                                                                 strerror(-ret), -ret);
        goto fail;
    }

    pa_log_info("Loaded hw module %s.%s (%s)", AUDIO_HARDWARE_MODULE_ID, module->name, DROID_DEVICE_STRING);

    ret = audio_hw_device_open(hwmod, &device);
    if (ret) {
        pa_log("Failed to open audio hw device : %s (%d).", strerror(-ret), -ret);
        goto fail;
    }

    pa_log_info("Opened hw audio device version %d.%d (This module compiled for API %d.%d, Android %d.%d.%d)",
                AUDIO_API_VERSION_GET_MAJ(device->common.version), AUDIO_API_VERSION_GET_MIN(device->common.version),
                AUDIO_API_VERSION_MAJ, AUDIO_API_VERSION_MIN,
                ANDROID_VERSION_MAJOR, ANDROID_VERSION_MINOR, ANDROID_VERSION_PATCH);

    if ((ret = device->init_check(device)) != 0) {
        pa_log("Failed init_check() : %s (%d)", strerror(-ret), -ret);
        goto fail;
    }

    hw = pa_xnew0(pa_droid_hw_module, 1);
    set_options(&hw->options, device, user_options);
    PA_REFCNT_INIT(hw);
    hw->core = core;
    hw->hwmod = hwmod;
    hw->hw_mutex = pa_mutex_new(true, false);
    hw->output_mutex = pa_mutex_new(true, false);
    hw->input_mutex = pa_mutex_new(true, false);
    hw->device = device;
    hw->config = dm_config_dup(config);
    hw->enabled_module = dm_config_find_module(hw->config, module_id);
    hw->module_id = hw->enabled_module->name;
    hw->shared_name = shared_name_get(hw->module_id);
    hw->outputs = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    hw->inputs = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    hw->sink_put_hook_slot      = pa_hook_connect(&core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_EARLY-10,
                                                  sink_put_hook_cb, hw);
    hw->sink_unlink_hook_slot   = pa_hook_connect(&core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_EARLY-10,
                                                  sink_unlink_hook_cb, hw);

    pa_assert_se(pa_shared_set(core, hw->shared_name, hw) >= 0);

    /* API for calling HAL functions from other modules. */

    if (pa_streq(hw->module_id, PA_DROID_PRIMARY_DEVICE)) {
        pa_shared_set(core, DROID_HW_HANDLE_V1, hw);
        pa_shared_set(core, DROID_SET_PARAMETERS_V1, droid_set_parameters_v1_cb);
        pa_shared_set(core, DROID_GET_PARAMETERS_V1, droid_get_parameters_v1_cb);
    }

    return hw;

fail:
    if (device)
        audio_hw_device_close(device);

    if (hw)
        pa_xfree(hw);

    return NULL;
}

static pa_droid_hw_module *droid_hw_module_shared_get(pa_core *core, const char *module_id) {
    pa_droid_hw_module *hw = NULL;
    char *shared_name;

    pa_assert(core);
    pa_assert(module_id);

    shared_name = shared_name_get(module_id);

    if ((hw = pa_shared_get(core, shared_name)))
        hw = pa_droid_hw_module_ref(hw);

    pa_xfree(shared_name);

    return hw;
}

pa_droid_hw_module *pa_droid_hw_module_get2(pa_core *core, pa_modargs *ma, const char *module_id) {
    pa_droid_hw_module *hw = NULL;
    dm_config_device *config = NULL;
    struct user_options user_options;

    pa_assert(core);
    pa_assert(ma);
    pa_assert(module_id);

    /* First let's find out if hw module has already been opened. */

    if ((hw = droid_hw_module_shared_get(core, module_id)))
        return hw;

    /* No hw module object in shared object db, let's parse options and config and
     * open the module now. */

    if (!droid_options_parse(&user_options, ma))
        return NULL;

    if (!(config = dm_config_load(ma)))
        return NULL;

    hw = droid_hw_module_open(core, config, module_id, &user_options);

    dm_config_free(config);

    return hw;
}

pa_droid_hw_module *pa_droid_hw_module_get(pa_core *core, dm_config_device *config, const char *module_id) {
    pa_droid_hw_module *hw;

    if (!(hw = droid_hw_module_shared_get(core, module_id)))
        hw = droid_hw_module_open(core, config, module_id, NULL);

    return hw;
}

pa_droid_hw_module *pa_droid_hw_module_ref(pa_droid_hw_module *hw) {
    pa_assert(hw);
    pa_assert(PA_REFCNT_VALUE(hw) >= 1);

    PA_REFCNT_INC(hw);
    return hw;
}

static void droid_hw_module_close(pa_droid_hw_module *hw) {
    pa_assert(hw);

    pa_log_info("Closing hw module %s.%s (%s)", AUDIO_HARDWARE_MODULE_ID, hw->enabled_module->name, DROID_DEVICE_STRING);

    if (pa_streq(hw->module_id, PA_DROID_PRIMARY_DEVICE)) {
        pa_shared_remove(hw->core, DROID_HW_HANDLE_V1);
        pa_shared_remove(hw->core, DROID_SET_PARAMETERS_V1);
        pa_shared_remove(hw->core, DROID_GET_PARAMETERS_V1);
    }

    if (hw->sink_put_hook_slot)
        pa_hook_slot_free(hw->sink_put_hook_slot);
    if (hw->sink_unlink_hook_slot)
        pa_hook_slot_free(hw->sink_unlink_hook_slot);

    if (hw->config)
        dm_config_free(hw->config);

    if (hw->device) {
        if (pa_droid_option(hw, DM_OPTION_UNLOAD_CALL_EXIT))
            exit(EXIT_SUCCESS);
        else if (!pa_droid_option(hw, DM_OPTION_UNLOAD_NO_CLOSE))
            audio_hw_device_close(hw->device);
    }

    if (hw->hw_mutex)
        pa_mutex_free(hw->hw_mutex);

    if (hw->output_mutex)
        pa_mutex_free(hw->output_mutex);

    if (hw->input_mutex)
        pa_mutex_free(hw->input_mutex);

    if (hw->shared_name)
        pa_xfree(hw->shared_name);

    if (hw->outputs) {
        pa_assert(pa_idxset_size(hw->outputs) == 0);
        pa_idxset_free(hw->outputs, NULL);
    }

    if (hw->inputs) {
        pa_assert(pa_idxset_size(hw->inputs) == 0);
        pa_idxset_free(hw->inputs, NULL);
    }

    pa_xfree(hw);
}

void pa_droid_hw_module_unref(pa_droid_hw_module *hw) {

    pa_assert(hw);
    pa_assert(PA_REFCNT_VALUE(hw) >= 1);

    if (PA_REFCNT_DEC(hw) > 0)
        return;

    pa_assert_se(pa_shared_remove(hw->core, hw->shared_name) >= 0);
    droid_hw_module_close(hw);
}

void pa_droid_hw_module_lock(pa_droid_hw_module *hw) {
    pa_assert(hw);

    pa_mutex_lock(hw->hw_mutex);
}

bool pa_droid_hw_module_try_lock(pa_droid_hw_module *hw) {
    pa_assert(hw);

    return pa_mutex_try_lock(hw->hw_mutex);
}

void pa_droid_hw_module_unlock(pa_droid_hw_module *hw) {
    pa_assert(hw);

    pa_mutex_unlock(hw->hw_mutex);
}

static pa_droid_stream *droid_stream_new(pa_droid_hw_module *module,
                                         dm_config_port *mix_port) {
    pa_droid_stream *s;

    s = pa_xnew0(pa_droid_stream, 1);
    PA_REFCNT_INIT(s);

    s->module = pa_droid_hw_module_ref(module);
    s->mix_port = mix_port;

    return s;
}

static pa_droid_output_stream *droid_output_stream_new(void) {
    return pa_xnew0(pa_droid_output_stream, 1);
}

static pa_droid_input_stream *droid_input_stream_new(dm_config_port *default_mix_port,
                                                     const pa_sample_spec *default_sample_spec,
                                                     const pa_channel_map *default_channel_map) {
    pa_droid_input_stream *input;

    input = pa_xnew0(pa_droid_input_stream, 1);
    input->first = true;
    input->default_mix_port    =  default_mix_port;
    input->default_sample_spec = *default_sample_spec;
    input->default_channel_map = *default_channel_map;

    return input;
}

static int stream_standby(pa_droid_stream *s) {
    int ret = 0;

    pa_assert(s);
    pa_assert(s->output || s->input);

    if ((s->output && !s->output->stream) ||
        (s->input && !s->input->stream))
        return ret;

    if (s->output) {
        pa_mutex_lock(s->module->output_mutex);
        ret = s->output->stream->common.standby(&s->output->stream->common);
        pa_mutex_unlock(s->module->output_mutex);
    } else {
        pa_mutex_lock(s->module->input_mutex);
        ret = s->input->stream->common.standby(&s->input->stream->common);
        pa_mutex_unlock(s->module->input_mutex);
    }

    return ret;
}

static bool compatible_port(const dm_config_port *port,
                            const pa_sample_spec *sample_spec,
                            const pa_channel_map *channel_map,
                            const dm_config_profile **compatible_profile,
                            pa_sample_spec *compatible_sample_spec,
                            pa_channel_map *compatible_channel_map,
                            audio_channel_mask_t *compatible_channel_mask) {
    const dm_config_profile *profile;
    void *state;

    pa_assert(port);
    pa_assert(port->port_type != DM_CONFIG_TYPE_MIX);
    pa_assert(sample_spec);
    pa_assert(compatible_sample_spec);
    pa_assert(compatible_channel_map);
    pa_assert(compatible_channel_mask);

    *compatible_sample_spec = *sample_spec;
    *compatible_channel_map = *channel_map;

    DM_LIST_FOREACH_DATA(profile, port->profiles, state) {
        uint32_t format = 0;
        bool sample_rate_compatible = false;
        bool channel_count_compatible = false;
        int i;

        if (!pa_convert_format(profile->format, CONV_FROM_HAL, &format))
            continue;

        if (sample_spec->format != (pa_sample_format_t) format)
            continue;

        if (profile->sampling_rates[0] == 0) {
            sample_rate_compatible = true;
            pa_log_info("%s port \"%s\" profile has dynamic sample rate.",
                        port->port_type == DM_CONFIG_TYPE_MIX_PORT ? "Mix" : "Device", port->name);
        } else {
            /* Rate count is used for reverse iteration if no direct matching sample rate is found. */
            uint32_t rate_count = 0;

            for (i = 0; profile->sampling_rates[i]; i++) {
                if (profile->sampling_rates[i] == sample_spec->rate) {
                    sample_rate_compatible = true;
                    break;
                }
                rate_count++;
            }

            if (!sample_rate_compatible) {
                /* Search from highest sample rate to lowest. */
                for (i = rate_count - 1; i >= 0; i--) {
                    if (profile->sampling_rates[i] % sample_spec->rate == 0) {
                        sample_rate_compatible = true;
                        compatible_sample_spec->rate = profile->sampling_rates[i];
                        break;
                    }

                }
            }

            if (!sample_rate_compatible) {
                for (i = 0; profile->sampling_rates[i]; i++) {
                    compatible_sample_spec->rate = profile->sampling_rates[i];
                    if (compatible_sample_spec->rate > sample_spec->rate) {
                        break;
                    }
                }
            }

            /* Sample rate is compatible if at least one sample rate is found. */
            sample_rate_compatible = true;
        }

        if (profile->channel_masks[0] == 0) {
            channel_count_compatible = true;
            *compatible_channel_mask = 0;
        } else {
            for (i = 0; profile->channel_masks[i]; i++) {
                if (audio_channel_count_from_out_mask(profile->channel_masks[i]) == channel_map->channels) {
                    channel_count_compatible = true;
                    *compatible_channel_mask = profile->channel_masks[i];
                    break;
                }
            }

            if (!channel_count_compatible) {
                /* We support only mono and stereo anyway at the moment so just choose either.
                 * If we wanted mono and mono wasn't available above then use stereo if found,
                 * and same if we wanted stereo and stereo wasn't available then use mono if found. */
                for (i = 0; profile->channel_masks[i]; i++) {
                    if (audio_channel_count_from_out_mask(profile->channel_masks[i]) == 2 &&
                        channel_map->channels == 1) {
                        channel_count_compatible = true;
                        pa_channel_map_init_stereo(compatible_channel_map);
                        *compatible_channel_mask = profile->channel_masks[i];
                        break;
                    } else if (audio_channel_count_from_out_mask(profile->channel_masks[i]) == 1 &&
                               channel_map->channels == 2) {
                        channel_count_compatible = true;
                        pa_channel_map_init_mono(compatible_channel_map);
                        *compatible_channel_mask = profile->channel_masks[i];
                        break;
                    }
                }
            }
        }

        if (sample_rate_compatible && channel_count_compatible) {
            if (compatible_profile)
                *compatible_profile = profile;

            compatible_sample_spec->channels = compatible_channel_map->channels;

            return true;
        }
    } /* DM_LIST_FOREACH_DATA */

    return false;
}

static bool stream_config_fill(pa_droid_hw_module *hw,
                               const dm_config_port *mix_port,
                               const dm_config_port *device_port,
                               pa_sample_spec *sample_spec,
                               pa_channel_map *channel_map,
                               struct audio_config *config) {
    audio_format_t hal_audio_format = 0;
    audio_channel_mask_t hal_channel_mask = 0;
    bool output = true;
    pa_sample_spec compatible_sample_spec;
    pa_channel_map compatible_channel_map;
    char tmp[64];
    int i;

    pa_assert(mix_port);
    pa_assert(mix_port->port_type == DM_CONFIG_TYPE_MIX_PORT);
    pa_assert(device_port);
    pa_assert(sample_spec);
    pa_assert(channel_map);
    pa_assert(config);

    output = mix_port->role == DM_CONFIG_ROLE_SOURCE ? true : false;

    if (!pa_convert_format(sample_spec->format, CONV_FROM_PA, &hal_audio_format)) {
        pa_log_warn("Sample spec format %u not supported.", sample_spec->format);
        goto fail;
    }

    if (!output && pa_droid_option(hw, DM_OPTION_RECORD_VOICE_16K) && hw->state.mode == AUDIO_MODE_IN_CALL) {
        pa_log_debug("Suggest sample rate of 16kHz for voice call input stream.");
        sample_spec->rate = 16000;
    }

    if (!compatible_port(mix_port, sample_spec, channel_map,
                         NULL, &compatible_sample_spec, &compatible_channel_map, &hal_channel_mask)) {
        pa_log("Couldn't find compatible configuration for mix port \"%s\"", mix_port->name);
        goto fail;
    }

    /* Dynamic channel mask, so let's just convert our pa_channel_map. */
    if (hal_channel_mask == 0) {
        for (i = 0; i < channel_map->channels; i++) {
            bool found;
            audio_channel_mask_t c;

            found = output ? pa_convert_output_channel(channel_map->map[i], CONV_FROM_PA, &c)
                           : pa_convert_input_channel(channel_map->map[i], CONV_FROM_PA, &c);

            if (!found) {
                pa_log("Failed to convert %s channel map.", output ? "output" : "input");
                goto fail;
            }

            hal_channel_mask |= c;
        }
    }

    if (!pa_sample_spec_equal(sample_spec, &compatible_sample_spec)) {
        pa_log_debug("With mix port \"%s\" requested sample spec: %s %uch %uHz",
                     mix_port->name,
                     pa_sample_format_to_string(sample_spec->format),
                     sample_spec->channels,
                     sample_spec->rate);

        *sample_spec = compatible_sample_spec;
    }

    pa_log_info("Using mix port \"%s\" with sample spec: %s %uch, %uHz",
                mix_port->name,
                pa_sample_format_to_string(compatible_sample_spec.format),
                compatible_sample_spec.channels,
                compatible_sample_spec.rate);

    if (!pa_channel_map_equal(channel_map, &compatible_channel_map)) {
        pa_channel_map_snprint(tmp, sizeof(tmp), channel_map);
        pa_log_debug("With mix port \"%s\" requested channel map: %s",
                     mix_port->name,
                     tmp);

        *channel_map = compatible_channel_map;
    }

    pa_channel_map_snprint(tmp, sizeof(tmp), &compatible_channel_map);
    pa_log_info("Using mix port \"%s\" with channel map: %s",
                mix_port->name,
                tmp);

    memset(config, 0, sizeof(*config));
    config->sample_rate = compatible_sample_spec.rate;
    config->channel_mask = hal_channel_mask;
    config->format = hal_audio_format;

    *sample_spec = compatible_sample_spec;
    *channel_map = compatible_channel_map;

    return true;

fail:
    return false;
}

static dm_config_port *stream_select_mix_port(pa_droid_stream *stream) {
    dm_config_port *selected_port;

    pa_assert(stream);
    pa_assert(stream->mix_port);
    pa_assert(stream->input);

    selected_port = stream->input->default_mix_port;

    if (stream->input && stream->module->state.mode == AUDIO_MODE_IN_COMMUNICATION) {
        dm_config_port *port;
        void *state;

        DM_LIST_FOREACH_DATA(port, stream->module->enabled_module->mix_ports, state) {
            if (port->role != DM_CONFIG_ROLE_SINK)
                continue;

            if (port->flags & AUDIO_INPUT_FLAG_VOIP_TX) {
                selected_port = port;
                goto done;
            }
        }
    }

    if (stream->input && stream->module->state.mode == AUDIO_MODE_IN_CALL) {
        dm_config_route *route;
        dm_config_port *port;
        void *state1, *state2;

        DM_LIST_FOREACH_DATA(route, stream->module->enabled_module->routes, state1) {
            DM_LIST_FOREACH_DATA(port, route->sources, state2) {
                if (port->role != DM_CONFIG_ROLE_SOURCE)
                    continue;

                if (port->type == AUDIO_DEVICE_IN_TELEPHONY_RX) {
                    selected_port = route->sink;
                    goto done;
                }
            }
        }
    }

done:
    pa_log_debug("Select input mix port \"%s\"", selected_port->name);

    return selected_port;
}

pa_droid_stream *pa_droid_open_output_stream(pa_droid_hw_module *module,
                                             const pa_sample_spec *spec,
                                             const pa_channel_map *map,
                                             dm_config_port *mix_port,
                                             dm_config_port *device_port) {
    pa_droid_stream *stream = NULL;
    pa_droid_output_stream *output = NULL;
    pa_droid_stream *primary_stream = NULL;
    int ret;
    pa_channel_map channel_map;
    pa_sample_spec sample_spec;
    struct audio_config config_out;

    pa_assert(module);
    pa_assert(spec);
    pa_assert(map);
    pa_assert(mix_port);
    pa_assert(device_port);

    sample_spec = *spec;
    channel_map = *map;

    stream = droid_stream_new(module, mix_port);
    stream->output = output = droid_output_stream_new();

    if (mix_port != dm_config_find_mix_port(module->enabled_module, mix_port->name)) {
        pa_log("Could not find mix port \"%s\" from module %s.", mix_port->name, module->enabled_module->name);
        goto fail;
    }

    if (device_port != dm_config_find_device_port(mix_port, device_port->type)) {
        pa_log("Could not find device port \"%s\" (%#010x) usable with mix port \"%s\".",
               device_port->name, device_port->type, mix_port->name);
        goto fail;
    }

    pa_log_info("Open output stream \"%s\"->\"%s\".", mix_port->name, device_port->name);

    if (!stream_config_fill(module, stream->mix_port, device_port, &sample_spec, &channel_map, &config_out))
        goto fail;

    pa_droid_hw_module_lock(module);
    ret = module->device->open_output_stream(module->device,
                                             ++module->stream_id,
                                             device_port->type,
                                             mix_port->flags,
                                             &config_out,
                                             &output->stream,
                                             device_port->address
                                             );
    pa_droid_hw_module_unlock(module);

    if (ret < 0 || !output->stream) {
        pa_log("Failed to open output stream: %d", ret);
        goto fail;
    }

    output->sample_spec = *spec;
    output->channel_map = *map;
    stream->active_device_port = NULL;
    stream->io_handle = module->stream_id;

    if ((output->sample_spec.rate = output->stream->common.get_sample_rate(&output->stream->common)) != sample_spec.rate)
        pa_log_warn("Requested sample rate %u but got %u instead.", sample_spec.rate, output->sample_spec.rate);

    pa_idxset_put(module->outputs, stream, NULL);

    stream->buffer_size = output->stream->common.get_buffer_size(&output->stream->common);

    if ((primary_stream = pa_droid_hw_primary_output_stream(module))) {
        pa_droid_stream_set_route(primary_stream, device_port);
    }

    pa_log_info("Opened droid output stream %p with device: %u flags: %u sample rate: %u channels: %u (%u) format: %u (%u) buffer size: %zu (%" PRIu64 " usec)",
            (void *) output->stream,
            device_port->type,
            stream->mix_port->flags,
            output->sample_spec.rate,
            output->sample_spec.channels, config_out.channel_mask,
            output->sample_spec.format, config_out.format,
            stream->buffer_size,
            pa_bytes_to_usec(stream->buffer_size, &output->sample_spec));

    return stream;

fail:
    pa_xfree(stream);
    pa_xfree(output);

    return NULL;
}

static const char *audio_mode_to_string(audio_mode_t mode) {
    switch (mode) {
        case AUDIO_MODE_RINGTONE:           return "AUDIO_MODE_RINGTONE";
        case AUDIO_MODE_IN_CALL:            return "AUDIO_MODE_IN_CALL";
        case AUDIO_MODE_IN_COMMUNICATION:   return "AUDIO_MODE_IN_COMMUNICATION";
        default: break;
    }

    return "AUDIO_MODE_NORMAL";
}

static bool config_diff(const struct audio_config *a, const struct audio_config *b,
                        bool *sample_rate, bool *channel_mask, bool *format) {
    bool diff = false;

    pa_assert(a);
    pa_assert(b);
    pa_assert(sample_rate);
    pa_assert(channel_mask);
    pa_assert(format);

    *sample_rate = *channel_mask = *format = false;

    if (a->sample_rate != b->sample_rate)
        diff = *sample_rate = true;

    if (a->channel_mask != b->channel_mask)
        diff = *channel_mask = true;

    if (a->format != b->format)
        diff = *format = true;

    return diff;
}

static bool stream_config_convert(pa_direction_t direction,
                                  const struct audio_config *config,
                                  pa_sample_spec *sample_spec,
                                  pa_channel_map *channel_map) {
    uint32_t format;
    uint32_t channel_mask;
    uint32_t pa_channel = 0;
    uint32_t channel = 0;
    uint32_t i = 0;

    pa_assert(direction == PA_DIRECTION_INPUT || direction == PA_DIRECTION_OUTPUT);
    pa_assert(config);
    pa_assert(sample_spec);
    pa_assert(channel_map);

    if (!pa_convert_format(config->format, CONV_FROM_HAL, (uint32_t *) &format)) {
        pa_log("Config format %#010x not supported.", config->format);
        return false;
    }

    sample_spec->format = format;

    channel_mask = config->channel_mask;

    while (channel_mask) {
        uint32_t current = (1 << i++);

        if (channel_mask & current) {
            if (!pa_convert_input_channel(current, CONV_FROM_HAL, &pa_channel)) {
                pa_log_warn("Could not convert channel mask value %#010x", current);
                return false;
            }

            channel_map->map[channel] = pa_channel;

            channel++;
            channel_mask &= ~current;
        }
    }

    channel_map->channels = channel;

    sample_spec->rate = config->sample_rate;
    sample_spec->channels = channel_map->channels;

    if (!pa_sample_spec_valid(sample_spec)) {
        pa_log_warn("Conversion resulted in invalid sample spec.");
        return false;
    }

    if (!pa_channel_map_valid(channel_map)) {
        pa_log_warn("Conversion resulted in invalid channel map.");
        return false;
    }

    return true;
}

static void log_input_open(pa_log_level_t log_level, const char *prefix,
                           const dm_config_port *device_port,
                           audio_source_t source,
                           uint32_t flags,  /* audio_input_flags_t */
                           const pa_sample_spec *sample_spec,
                           const struct audio_config *config,
                           int return_code) {
    pa_logl(log_level,
            "%s input stream with device: %#010x source: %#010x flags: %#010x sample rate: %u (%u) channels: %u (%#010x) format: %u (%#010x) (return code %d)",
            prefix,
            device_port->type,
            source,
            flags,
            sample_spec->rate,
            config->sample_rate,
            sample_spec->channels,
            config->channel_mask,
            sample_spec->format,
            config->format,
            return_code);
}


static int input_stream_open(pa_droid_stream *stream, bool resume_from_suspend) {
    pa_droid_hw_module *hw_module;
    pa_droid_input_stream *input = NULL;
    pa_channel_map channel_map;
    pa_sample_spec sample_spec;
    dm_config_port *mix_port;
    size_t buffer_size;
    bool try_defaults = true;
    int ret = -1;

    struct audio_config config_try;
    struct audio_config config_in;

    bool diff_sample_rate = false;
    bool diff_channel_mask = false;
    bool diff_format = false;

    pa_assert(stream);
    pa_assert(stream->input);
    pa_assert_se((hw_module = stream->module));

    if (stream->input->stream) /* already open */
        return 0;

    input = stream->input;
    input->stream = NULL;

    /* Copy our requested specs */
    if (input->first) {
        sample_spec = input->default_sample_spec;
        channel_map = input->default_channel_map;
    } else {
        sample_spec = input->req_sample_spec;
        channel_map = input->req_channel_map;
    }

    mix_port = stream_select_mix_port(stream);

    if (!stream_config_fill(hw_module, mix_port, stream->active_device_port, &sample_spec, &channel_map, &config_try))
        goto done;

    pa_droid_hw_module_lock(stream->module);
    while (true) {
        config_in = config_try;

        log_input_open(PA_LOG_INFO, "Trying to open",
                       stream->active_device_port,
                       input->audio_source,
                       mix_port->flags,
                       &sample_spec,
                       &config_in,
                       0);

        ret = hw_module->device->open_input_stream(hw_module->device,
                                                   ++hw_module->stream_id,
                                                   stream->active_device_port->type,
                                                   &config_in,
                                                   &input->stream,
                                                   mix_port->flags,
                                                   stream->active_device_port->address,
                                                   input->audio_source
                                                   );
        if (ret < 0) {
            if (config_diff(&config_in, &config_try, &diff_sample_rate, &diff_channel_mask, &diff_format)) {
                pa_log_info("Could not open input stream, differences in%s%s%s",
                            diff_sample_rate ? " sample_rate" : "",
                            diff_channel_mask ? " channel_mask" : "",
                            diff_format ? " format" : "");
                if (diff_sample_rate)
                    pa_log_info("Wanted sample_rate %d suggested %d", config_try.sample_rate, config_in.sample_rate);
                if (diff_channel_mask)
                    pa_log_info("Wanted channel_mask %#010x suggested %#010x", config_try.channel_mask, config_in.channel_mask);
                if (diff_format)
                    pa_log_info("Wanted format %#010x suggested %#010x", config_try.format, config_in.format);

                if (!stream_config_convert(PA_DIRECTION_INPUT, &config_in, &sample_spec, &channel_map)) {
                    pa_log_warn("Failed to update PulseAudio structures from received config values.");
                    goto open_done;
                }

                config_try = config_in;
                continue;
            } else if (try_defaults) {
                pa_log_info("Could not open input stream, trying with defaults.");
                sample_spec = input->default_sample_spec;
                channel_map = input->default_channel_map;

                if (!stream_config_fill(hw_module, mix_port, stream->active_device_port, &sample_spec, &channel_map, &config_try))
                    goto done;

                try_defaults = false;
                continue;
            } else {
                pa_log_warn("Could not open input stream and no suggested changes received, bailing out.");
                goto open_done;
            }
        } else if (config_diff(&config_in, &config_try, &diff_sample_rate, &diff_channel_mask, &diff_format)) {
            pa_log_info("Opened input stream, but differences in%s%s%s",
                        diff_sample_rate ? " sample_rate" : "",
                        diff_channel_mask ? " channel_mask" : "",
                        diff_format ? " format" : "");
            if (!stream_config_convert(PA_DIRECTION_INPUT, &config_in, &sample_spec, &channel_map)) {
                pa_log_warn("Failed to update PulseAudio structures from received config values.");
                input->stream->common.standby(&input->stream->common);
                hw_module->device->close_input_stream(hw_module->device, input->stream);
                input->stream = NULL;
                ret = -1;
            }
        }

        goto open_done;
    }
open_done:
    pa_droid_hw_module_unlock(stream->module);

    if (ret < 0 || !input->stream) {
        log_input_open(resume_from_suspend ? PA_LOG_INFO : PA_LOG_ERROR, "Failed to open",
                       stream->active_device_port,
                       input->audio_source,
                       mix_port->flags,
                       &sample_spec,
                       &config_in,
                       ret);

        goto done;
    }

    log_input_open(PA_LOG_INFO, "Opened",
                   stream->active_device_port,
                   input->audio_source,
                   mix_port->flags,
                   &sample_spec,
                   &config_in,
                   ret);

    stream->mix_port = mix_port;
    input->req_sample_spec = input->sample_spec = sample_spec;
    input->req_channel_map = input->channel_map = channel_map;
    buffer_size = input->stream->common.get_buffer_size(&input->stream->common);
    stream->buffer_size = buffer_size;
    stream->io_handle = hw_module->stream_id;

    /* Set input stream to standby */
    stream_standby(stream);

    /* As audio_source_t may not have any effect when opening the input stream
     * set input parameters immediately after opening the stream. */
    input_stream_set_route(stream, stream->active_device_port);

    pa_log_debug("Opened input stream %p", (void *) stream);

done:
    return ret;
}

static void input_stream_close(pa_droid_stream *s) {
    pa_assert(s);
    pa_assert(s->input);

    if (!s->input->stream)
        return;

    audio_patch_release(s);

    pa_mutex_lock(s->module->input_mutex);
    s->input->stream->common.standby(&s->input->stream->common);
    s->module->device->close_input_stream(s->module->device, s->input->stream);
    s->input->stream = NULL;
    pa_log_debug("Closed input stream %p", (void *) s);
    pa_mutex_unlock(s->module->input_mutex);
}

bool pa_droid_stream_reconfigure_input(pa_droid_stream *stream,
                                       const pa_sample_spec *requested_sample_spec,
                                       const pa_channel_map *requested_channel_map,
                                       const pa_proplist *proplist) {
    /* Use default audio source by default */
    audio_source_t audio_source = AUDIO_SOURCE_DEFAULT;

    pa_assert(stream);
    pa_assert(stream->input);
    pa_assert(requested_sample_spec);
    pa_assert(requested_channel_map);

    /* Copy our requested specs, so we know them when resuming from suspend
     * as well. */
    stream->input->req_sample_spec = *requested_sample_spec;
    stream->input->req_channel_map = *requested_channel_map;

    if (proplist) {
        const char *source;
        /* If audio source is defined in source-output proplist use that instead. */
        if ((source = pa_proplist_gets(proplist, EXT_PROP_AUDIO_SOURCE)))
            pa_string_convert_str_to_num(CONV_STRING_AUDIO_SOURCE_FANCY, source, &audio_source);
    }

    /* Update audio source */
    droid_set_audio_source(stream, audio_source);

    input_stream_close(stream);

    /* Set some sensible default for device port -> look through attached devices and
     * select first source port. */
    if (stream->input->first) {
        dm_config_port *device_port;
        void *state;

        DM_LIST_FOREACH_DATA(device_port, stream->module->enabled_module->attached_devices, state) {
            if (device_port->role == DM_CONFIG_ROLE_SOURCE) {
                pa_log_debug("Select initial input device port \"%s\".", device_port->name);
                stream->active_device_port = device_port;
                break;
            }
        }

        pa_assert(stream->active_device_port);
    }

    if (input_stream_open(stream, false) < 0) {
        if (!stream->input->first) {
            pa_log_warn("Input stream reconfigure failed, restore default values.");
            stream->input->req_sample_spec = stream->input->default_sample_spec;
            stream->input->req_channel_map = stream->input->default_channel_map;
            input_stream_open(stream, false);
        }
        return false;
    }

    return true;
}

bool pa_droid_stream_reconfigure_input_needed(pa_droid_stream *stream,
                                              const pa_sample_spec *requested_sample_spec,
                                              const pa_channel_map *requested_channel_map,
                                              const pa_proplist *proplist) {
    bool reconfigure_needed = false;

    pa_assert(stream);
    pa_assert(stream->input);

    if (requested_sample_spec && !pa_sample_spec_equal(&stream->input->sample_spec, requested_sample_spec)) {
        reconfigure_needed = true;
        pa_log_debug("input reconfigure needed: sample specs not equal");
    }

    if (requested_channel_map && !pa_channel_map_equal(&stream->input->channel_map, requested_channel_map)) {
        reconfigure_needed = true;
        pa_log_debug("input reconfigure needed: channel maps not equal");
    }

    if (proplist) {
        const char *source;
        audio_source_t audio_source;

        /* If audio source is defined in source-output proplist use that instead. */
        if ((source = pa_proplist_gets(proplist, EXT_PROP_AUDIO_SOURCE))) {
            if (pa_string_convert_str_to_num(CONV_STRING_AUDIO_SOURCE_FANCY, source, &audio_source) &&
                stream->input->audio_source != audio_source) {

                reconfigure_needed = true;
                pa_log_debug("input reconfigure needed: " EXT_PROP_AUDIO_SOURCE " changes");
            }
        } else {
            if (pa_input_device_default_audio_source(stream->active_device_port->type, &audio_source) &&
                stream->input->audio_source != audio_source) {

                reconfigure_needed = true;
                pa_log_debug("input reconfigure needed: audio source changes");
            }
        }
    }

    return reconfigure_needed;
}

pa_droid_stream *pa_droid_open_input_stream(pa_droid_hw_module *hw_module,
                                            const pa_sample_spec *default_sample_spec,
                                            const pa_channel_map *default_channel_map,
                                            const char *mix_port_name) {

    pa_droid_stream *stream = NULL;
    pa_droid_input_stream *input = NULL;
    dm_config_port *mix_port;

    pa_assert(hw_module);
    pa_assert(hw_module->enabled_module);
    pa_assert(default_sample_spec);
    pa_assert(default_channel_map);

    if (!(mix_port = dm_config_find_mix_port(hw_module->enabled_module, mix_port_name))) {
        pa_log("Could not find mix port \"%s\" from module \"%s\".", mix_port_name, hw_module->enabled_module->name);
        return NULL;
    }

    stream = droid_stream_new(hw_module, mix_port);
    stream->input = input = droid_input_stream_new(mix_port, default_sample_spec, default_channel_map);

    if (!pa_droid_stream_reconfigure_input(stream, default_sample_spec, default_channel_map, NULL)) {
        pa_droid_stream_unref(stream);
        stream = NULL;
    } else
        stream->input->first = false;

    return stream;
}

pa_droid_stream *pa_droid_stream_ref(pa_droid_stream *s) {
    pa_assert(s);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_REFCNT_INC(s);
    return s;
}

void pa_droid_stream_unref(pa_droid_stream *s) {
    pa_assert(s);
    pa_assert(s->input || s->output);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (PA_REFCNT_DEC(s) > 0)
        return;

    if (s->output) {
        pa_log_debug("Destroy output stream %p", (void *) s);
        pa_mutex_lock(s->module->output_mutex);
        pa_idxset_remove_by_data(s->module->outputs, s, NULL);
        s->module->device->close_output_stream(s->module->device, s->output->stream);
        pa_mutex_unlock(s->module->output_mutex);
        pa_xfree(s->output);
    } else {
        pa_log_debug("Destroy input stream %p", (void *) s);
        pa_idxset_remove_by_data(s->module->inputs, s, NULL);
        input_stream_close(s);
        pa_xfree(s->input);
    }

    pa_droid_hw_module_unref(s->module);

    pa_xfree(s);
}

pa_droid_stream *pa_droid_hw_primary_output_stream(pa_droid_hw_module *hw) {
    pa_droid_stream *s;
    uint32_t idx;

    pa_assert(hw);
    pa_assert(hw->outputs);

    PA_IDXSET_FOREACH(s, hw->outputs, idx) {
        if (s->mix_port->flags & AUDIO_OUTPUT_FLAG_PRIMARY)
            return s;
    }

    return NULL;
}

static void stream_update_bt_sco(pa_droid_hw_module *hw_module, const dm_config_port *device_port) {
    int set_bt_sco = -1;

    pa_assert(hw_module);
    pa_assert(device_port);

    if (device_port->type == AUDIO_DEVICE_OUT_BLUETOOTH_SCO ||
        device_port->type == AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET ||
        device_port->type == AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT) {

        if (!hw_module->bt_sco_enabled)
            set_bt_sco = 1;

    } else {
        if (hw_module->bt_sco_enabled)
            set_bt_sco = 0;
    }

    if (set_bt_sco != -1) {
        hw_module->bt_sco_enabled = set_bt_sco ? true : false;
        droid_set_parameters(hw_module, set_bt_sco ? AUDIO_PARAMETER_BT_SCO_ON : AUDIO_PARAMETER_BT_SCO_OFF);
    }
}

static void audio_patch_release(pa_droid_stream *stream) {
    int ret;

    pa_assert(stream);

    if (stream->audio_patch != AUDIO_PATCH_HANDLE_NONE) {
        ret = stream->module->device->release_audio_patch(stream->module->device, stream->audio_patch);
        stream->audio_patch = AUDIO_PATCH_HANDLE_NONE;
        if (ret < 0)
            pa_log_info("Release %s audio patch %s:%s (%d)",
                        stream->mix_port->role == DM_CONFIG_ROLE_SINK ? "output" : "input",
                        stream->mix_port->name,
                        stream->active_device_port->name,
                        -ret);
    }
}

static int audio_patch_update_output(pa_droid_stream *stream, const dm_config_port *device_port) {
    pa_droid_output_stream *output;
    struct audio_port_config sink, source;
    int ret;

    output = stream->output;

    memset(&sink, 0, sizeof(sink));
    memset(&source, 0, sizeof(source));

    source.type = AUDIO_PORT_TYPE_MIX;
    source.role = AUDIO_PORT_ROLE_SOURCE;
    source.sample_rate = output->sample_spec.rate;
    source.format = AUDIO_FORMAT_PCM_16_BIT;
    source.ext.mix.handle = stream->io_handle;

    sink.role = AUDIO_PORT_ROLE_SINK;
    sink.type = AUDIO_PORT_TYPE_DEVICE;
    sink.sample_rate = output->sample_spec.rate;
    sink.format = AUDIO_FORMAT_PCM_16_BIT;
    sink.ext.device.address[0] = '\0';
    if (strlen(device_port->address))
        strncpy(sink.ext.device.address, device_port->address, AUDIO_DEVICE_MAX_ADDRESS_LEN);
    sink.ext.device.type = device_port->type;

    ret = stream->module->device->create_audio_patch(stream->module->device, 1, &source, 1, &sink, &stream->audio_patch);
    if (ret < 0)
        pa_log_warn("Failed to create output audio patch \"%s\"->\"%s\" (%d)", stream->mix_port->name, device_port->name, -ret);
    else
        pa_log_info("Created output audio patch \"%s\"->\"%s\"", stream->mix_port->name, device_port->name);

    stream->active_device_port = device_port;

    return ret;
}

static int audio_patch_update_input(pa_droid_stream *stream, const dm_config_port *device_port) {
    pa_droid_input_stream *input;
    struct audio_port_config sink, source;
    int ret;

    input = stream->input;

    memset(&sink, 0, sizeof(sink));
    memset(&source, 0, sizeof(source));

    sink.type = AUDIO_PORT_TYPE_MIX;
    sink.role = AUDIO_PORT_ROLE_SINK;
    sink.sample_rate = input->sample_spec.rate;
    sink.format = AUDIO_FORMAT_PCM_16_BIT;
    sink.ext.mix.handle = stream->io_handle;

    source.role = AUDIO_PORT_ROLE_SOURCE;
    source.type = AUDIO_PORT_TYPE_DEVICE;
    source.sample_rate = input->sample_spec.rate;
    source.format = AUDIO_FORMAT_PCM_16_BIT;
    source.ext.device.address[0] = '\0';
    if (strlen(device_port->address))
        strncpy(source.ext.device.address, device_port->address, AUDIO_DEVICE_MAX_ADDRESS_LEN);
    source.ext.device.type = device_port->type;

    ret = stream->module->device->create_audio_patch(stream->module->device, 1, &source, 1, &sink, &stream->audio_patch);
    if (ret < 0)
        pa_log_warn("Failed to create input audio patch \"%s\"<-\"%s\" (%d)", stream->mix_port->name, device_port->name, -ret);
    else
        pa_log_info("Created input audio patch \"%s\"<-\"%s\"", stream->mix_port->name, device_port->name);

    stream->active_device_port = device_port;

    return ret;
}

static int droid_output_stream_audio_patch_update(pa_droid_stream *primary_stream, dm_config_port *device_port) {
    pa_droid_stream *stream;
    uint32_t idx;
    int ret;

    pa_assert(primary_stream);
    pa_assert(primary_stream->output);
    pa_assert(primary_stream->mix_port);
    pa_assert(primary_stream->mix_port->type == DM_CONFIG_TYPE_MIX);
    pa_assert(primary_stream->mix_port->flags & AUDIO_OUTPUT_FLAG_PRIMARY);
    pa_assert(device_port);
    pa_assert(device_port->role == DM_CONFIG_ROLE_SINK);

    PA_IDXSET_FOREACH(stream, primary_stream->module->outputs, idx) {
        audio_patch_release(stream);
    }

    ret = audio_patch_update_output(primary_stream, device_port);

    if (ret == 0) {
        PA_IDXSET_FOREACH(stream, primary_stream->module->outputs, idx) {
            if (primary_stream == stream)
                continue;

            audio_patch_update_output(stream, device_port);
        }
    }

    if (ret < 0) {
        pa_log_warn("Failed to update output stream audio patch (%d)", -ret);
    }

    return ret;
}

static int input_stream_set_route(pa_droid_stream *stream, const dm_config_port *device_port) {
    pa_droid_input_stream *input;
    char *parameters = NULL;
    int ret = 0;

    pa_assert(stream);

    input = stream->input;

     /* Input stream closed, no need for routing changes */
    if (!input->stream)
        goto done;

    audio_patch_release(stream);
    ret = audio_patch_update_input(stream, device_port);

    if (ret < 0)
        pa_log_warn("input_stream_set_route(%s) failed", device_port->name);

    pa_xfree(parameters);

done:
    return ret;
}

int pa_droid_stream_set_route(pa_droid_stream *s, dm_config_port *device_port) {
    pa_assert(s);

    if (s->output) {
        int ret;

        if (pa_droid_stream_is_primary(s))
            stream_update_bt_sco(s->module, device_port);

        ret = droid_output_stream_audio_patch_update(s, device_port);

        return ret;
    } else {
        pa_droid_hw_set_input_device(s, device_port);
        return 0;
    }
}

int pa_droid_stream_set_parameters(pa_droid_stream *s, const char *parameters) {
    int ret;

    pa_assert(s);
    pa_assert(s->output || s->input);
    pa_assert(parameters);

    if (s->output) {
        pa_log_debug("output stream %p set_parameters(%s)", (void *) s, parameters);
        pa_mutex_lock(s->module->output_mutex);
        ret = s->output->stream->common.set_parameters(&s->output->stream->common, parameters);
        pa_mutex_unlock(s->module->output_mutex);
    } else {
        pa_log_debug("input stream %p set_parameters(%s)", (void *) s, parameters);
        pa_mutex_lock(s->module->input_mutex);
        ret = s->input->stream->common.set_parameters(&s->input->stream->common, parameters);
        pa_mutex_unlock(s->module->input_mutex);
    }

    if (ret < 0)
        pa_log("%s stream %p set_parameters(%s) failed: %d",
               s->output ? "output" : "input", (void *) s, parameters, ret);

    return ret;
}

static int droid_set_parameters(pa_droid_hw_module *hw, const char *parameters) {
    int ret;

    pa_assert(hw);
    pa_assert(parameters);

    pa_log_debug("hw %p set_parameters(%s)", (void *) hw, parameters);
    ret = hw->device->set_parameters(hw->device, parameters);

    if (ret < 0)
        pa_log("hw module %p set_parameters(%s) failed: %d", (void *) hw, parameters, ret);

    return ret;
}

int pa_droid_set_parameters(pa_droid_hw_module *hw, const char *parameters) {
    int ret;

    pa_assert(hw);
    pa_assert(parameters);

    pa_mutex_lock(hw->hw_mutex);
    ret = droid_set_parameters(hw, parameters);
    pa_mutex_unlock(hw->hw_mutex);

    return ret;
}

bool pa_droid_stream_is_primary(pa_droid_stream *s) {
    pa_assert(s);
    pa_assert(s->output || s->input);

    if (s->output)
        return s->mix_port->flags & AUDIO_OUTPUT_FLAG_PRIMARY;

    /* Even though earlier (< 3) HALs don't have input flags,
     * input flags don't have anything similar as output stream's
     * primary flag and we can just always reply true for
     * input streams. */
    return true;
}

int pa_droid_stream_suspend(pa_droid_stream *s, bool suspend) {
    pa_assert(s);
    pa_assert(s->output || s->input);

    if (s->output) {
        if (suspend) {
            pa_atomic_dec(&s->module->active_outputs);
            return stream_standby(s);
        } else {
            pa_atomic_inc(&s->module->active_outputs);
        }
    } else {
        if (suspend) {
            if (s->input->stream) {
                if (pa_droid_option(s->module, DM_OPTION_CLOSE_INPUT))
                    input_stream_close(s);
                else
                    return stream_standby(s);
            }
        } else if (pa_droid_option(s->module, DM_OPTION_CLOSE_INPUT))
            return input_stream_open(s, true);
    }

    return 0;
}

size_t pa_droid_stream_buffer_size(pa_droid_stream *s) {
    pa_assert(s);

    return s->buffer_size;
}

pa_usec_t pa_droid_stream_get_latency(pa_droid_stream *s) {
    pa_assert(s);

    if (s->output && s->output->stream)
        return s->output->stream->get_latency(s->output->stream) * PA_USEC_PER_MSEC;

    return 0;
}

void pa_droid_stream_set_data(pa_droid_stream *s, void *data) {
    pa_assert(s);

    s->data = data;
}

void *pa_droid_stream_get_data(pa_droid_stream *s) {
    pa_assert(s);

    return s->data;
}

static bool proplist_check_api(pa_proplist *proplist) {
    const char *api;

    pa_assert(proplist);

    if ((api = pa_proplist_gets(proplist, PA_PROP_DEVICE_API)))
        return pa_streq(api, PROP_DROID_API_STRING);

    return false;
}

bool pa_source_is_droid_source(pa_source *source) {
    pa_assert(source);
    return proplist_check_api(source->proplist);
}

bool pa_sink_is_droid_sink(pa_sink *sink) {
    pa_assert(sink);
    return proplist_check_api(sink->proplist);
}

size_t pa_droid_buffer_size_round_up(size_t buffer_size, size_t block_size) {
    size_t r;

    pa_assert(buffer_size);
    pa_assert(block_size);

    r = buffer_size % block_size;

    if (r)
        return buffer_size + block_size - r;

    return buffer_size;
}

bool pa_droid_hw_has_mic_control(pa_droid_hw_module *hw) {
    pa_assert(hw);
    pa_assert(hw->device);

    if (hw->device->set_mic_mute && hw->device->get_mic_mute) {
        pa_log_info("Module has HAL mic mute control.");
        return true;
    }

    pa_log_info("Module has soft mic mute control.");
    return false;
}

int pa_droid_hw_mic_get_mute(pa_droid_hw_module *hw_module, bool *muted) {
    int ret = 0;

    pa_assert(hw_module);
    pa_assert(hw_module->device);
    pa_assert(hw_module->device->get_mic_mute);

    pa_droid_hw_module_lock(hw_module);
    if (hw_module->device->get_mic_mute(hw_module->device, muted) < 0) {
        pa_log("Failed to get mute state.");
        ret = -1;
    }
    pa_droid_hw_module_unlock(hw_module);

    return ret;
}

void pa_droid_hw_mic_set_mute(pa_droid_hw_module *hw_module, bool muted) {
    pa_assert(hw_module);
    pa_assert(hw_module->device);
    pa_assert(hw_module->device->set_mic_mute);

    pa_droid_hw_module_lock(hw_module);
    if (hw_module->device->set_mic_mute(hw_module->device, muted) < 0)
        pa_log("Failed to set mute state to %smuted.", muted ? "" : "un");
    pa_droid_hw_module_unlock(hw_module);
}

bool pa_droid_hw_set_mode(pa_droid_hw_module *hw_module, audio_mode_t mode) {
    bool ret = true;

    pa_assert(hw_module);
    pa_assert(hw_module->device);

    pa_log_info("Set mode to %s.", audio_mode_to_string(mode));

    if (pa_droid_option(hw_module, DM_OPTION_SPEAKER_BEFORE_VOICE) &&
        hw_module->state.mode != mode && mode == AUDIO_MODE_IN_CALL) {
        pa_droid_stream *primary_output;
        dm_config_port *device_port;

        /* Set route to speaker before changing audio mode to AUDIO_MODE_IN_CALL.
         * Some devices don't get routing right if the route is something else
         * (like AUDIO_DEVICE_OUT_WIRED_HEADSET) before calling set_mode().*/
        if ((primary_output = pa_droid_hw_primary_output_stream(hw_module)) &&
            (device_port = dm_config_find_device_port(primary_output->mix_port, AUDIO_DEVICE_OUT_SPEAKER)))
            pa_droid_stream_set_route(primary_output, device_port);
    }

    pa_droid_hw_module_lock(hw_module);
    if (hw_module->device->set_mode(hw_module->device, mode) < 0) {
        ret = false;
        pa_log_warn("Failed to set mode.");
    } else {
        if (hw_module->state.mode != mode && mode == AUDIO_MODE_IN_CALL) {
            pa_droid_stream *primary_output;
            dm_config_port *device_port;

            /* Always start call mode with earpiece. This helps some devices which cannot
             * start call directly with headset and doesn't cause any harm with devices
             * which can either. */
            if ((primary_output = pa_droid_hw_primary_output_stream(hw_module)) &&
                (device_port = dm_config_find_device_port(primary_output->mix_port, AUDIO_DEVICE_OUT_EARPIECE)))
                pa_droid_stream_set_route(primary_output, device_port);
        }

        hw_module->state.mode = mode;
    }
    pa_droid_hw_module_unlock(hw_module);

    return ret;
}

/* Return true if audio source changes */
static bool droid_set_audio_source(pa_droid_stream *stream, audio_source_t audio_source) {
    audio_source_t audio_source_override = AUDIO_SOURCE_DEFAULT;

    pa_assert(stream);

    if (audio_source == AUDIO_SOURCE_DEFAULT) {
        if (!stream->active_device_port) {
            /* Default to mic source if there is no active device_port. */
            audio_source = AUDIO_SOURCE_MIC;
        } else {
            pa_input_device_default_audio_source(stream->active_device_port->type, &audio_source);

        }
    }

    /* Override audio source based on mode. */
    switch (stream->module->state.mode) {
        case AUDIO_MODE_IN_CALL:
            audio_source_override = AUDIO_SOURCE_VOICE_CALL;
            break;
        case AUDIO_MODE_IN_COMMUNICATION:
            audio_source_override = AUDIO_SOURCE_VOICE_COMMUNICATION;
            break;
        default:
            audio_source_override = audio_source;
            break;
    }

    if (audio_source != audio_source_override) {
        const char *from = NULL, *to = NULL;
        pa_string_convert_num_to_str(CONV_STRING_AUDIO_SOURCE_FANCY, audio_source, &from);
        pa_string_convert_num_to_str(CONV_STRING_AUDIO_SOURCE_FANCY, audio_source_override, &to);
        pa_log_info("Audio mode %s, overriding audio source %s with %s",
                    audio_mode_to_string(stream->module->state.mode),
                    from ? from : "<unknown>",
                    to ? to : "<unknown>");
        audio_source = audio_source_override;
    }

    if (audio_source != stream->input->audio_source) {
        const char *name = NULL;
        pa_log_debug("Set mix port \"%s\" audio source to %s (%#010x)",
                     stream->mix_port->name,
                     pa_string_convert_num_to_str(CONV_STRING_AUDIO_SOURCE_FANCY, audio_source, &name)
                       ? name : "<unknown>",
                     audio_source);
        stream->input->audio_source = audio_source;

        /* audio source changed */
        return true;
    }

    /* audio source did not change */
    return false;
}


bool pa_droid_hw_set_input_device(pa_droid_stream *stream,
                                  dm_config_port *device_port) {
    bool device_changed = false;
    bool source_changed = false;

    pa_assert(stream);
    pa_assert(stream->input);
    pa_assert(device_port);
    pa_assert(device_port->port_type == DM_CONFIG_TYPE_DEVICE_PORT);

    if (!stream->active_device_port ||
        !dm_config_port_equal(stream->active_device_port, device_port)) {

        const char *name = NULL;
        pa_log_debug("Set mix port \"%s\" input to %s (%#010x, %s)",
                     stream->mix_port->name,
                     pa_string_convert_input_device_num_to_str(device_port->type, &name)
                       ? name : "<unknown>",
                     device_port->type,
                     device_port->name);
        stream->active_device_port = device_port;
        device_changed = true;
    }

    source_changed = droid_set_audio_source(stream, stream->input->audio_source);

    if (stream->active_device_port && (device_changed || source_changed))
        input_stream_set_route(stream, device_port);

    return true;
}

const pa_sample_spec *pa_droid_stream_sample_spec(pa_droid_stream *stream) {
    pa_assert(stream);
    pa_assert(stream->output || stream->input);

    return stream->output ? &stream->output->sample_spec : &stream->input->sample_spec;
}

const pa_channel_map *pa_droid_stream_channel_map(pa_droid_stream *stream) {
    pa_assert(stream);
    pa_assert(stream->output || stream->input);

    return stream->output ? &stream->output->channel_map : &stream->input->channel_map;
}

pa_modargs *pa_droid_modargs_new(const char *args, const char* const keys[]) {
    pa_modargs *ma;
    const char **full_keys;
    int count;
    int i, k;

    for (count = 0; keys[count]; count++) ;

    full_keys = pa_xnew0(const char *, count + DM_OPTION_COUNT + 1);

    for (i = 0; keys[i]; i++)
        full_keys[i] = keys[i];

    for (k = 0; k < DM_OPTION_COUNT; k++)
        full_keys[i++] = valid_options[k].name;

    ma = pa_modargs_new(args, full_keys);

    pa_xfree(full_keys);

    return ma;
}
