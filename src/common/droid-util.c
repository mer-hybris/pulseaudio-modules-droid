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

#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>
#include <pulse/direction.h>

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

struct droid_quirk {
    const char *name;
    uint32_t value;
};

struct droid_quirk valid_quirks[] = {
    { "input_atoi",             QUIRK_INPUT_ATOI            },
    { "set_parameters",         QUIRK_SET_PARAMETERS        },
    { "close_input",            QUIRK_CLOSE_INPUT           },
    { "unload_no_close",        QUIRK_UNLOAD_NO_CLOSE       },
    { "no_hw_volume",           QUIRK_NO_HW_VOLUME          },
    { "output_make_writable",   QUIRK_OUTPUT_MAKE_WRITABLE  },
    { "realcall",               QUIRK_REALCALL              },
    { "unload_call_exit",       QUIRK_UNLOAD_CALL_EXIT      },
};


#define DEFAULT_PRIORITY (100)


static const char * const droid_combined_auto_outputs[3]    = { "primary", "low_latency", NULL };
static const char * const droid_combined_auto_inputs[2]     = { "primary", NULL };

static void droid_port_free(pa_droid_port *p);

static pa_droid_stream *get_primary_output(pa_droid_hw_module *hw);
static int input_stream_set_route(pa_droid_hw_module *hw_module);

static pa_droid_profile *profile_new(pa_droid_profile_set *ps,
                                     const pa_droid_config_hw_module *module,
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

    pa_hashmap_put(ps->profiles, p->name, p);

    return p;
}

static pa_droid_profile *droid_profile_new(pa_droid_profile_set *ps,
                                           const pa_droid_config_device *primary_output,
                                           const pa_droid_config_device *output,
                                           const pa_droid_config_device *inputs) {
    pa_droid_profile *p;
    char *name;
    char *description;

    pa_assert(ps);
    pa_assert(output);
    pa_assert(!primary_output || primary_output->direction == PA_DIRECTION_OUTPUT);
    pa_assert(!inputs || inputs->direction == PA_DIRECTION_INPUT);

    name = pa_sprintf_malloc("%s%s%s", output->name, inputs ? "-" : "", inputs ? inputs->name : "");
    description = pa_sprintf_malloc("%s output%s%s%s", output->name,
                                                       inputs ? " and " : "",
                                                       inputs ? inputs->name : "",
                                                       inputs ? " inputs." : "");

    p = profile_new(ps, output->module, name, description);
    pa_xfree(name);
    pa_xfree(description);

    if (pa_streq(output->name, "primary")) {
        p->priority += DEFAULT_PRIORITY;

        if (inputs && pa_streq(inputs->name, "primary"))
            p->priority += DEFAULT_PRIORITY;
    }

    if (primary_output && primary_output != output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, primary_output), NULL);
    if (output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, output), NULL);
    if (inputs)
        p->input_mapping = pa_droid_mapping_get(ps, inputs);

    return p;
}

void pa_droid_profile_add_mapping(pa_droid_profile *p, pa_droid_mapping *am) {
    pa_assert(p);
    pa_assert(am);

    if (am->direction == PA_DIRECTION_OUTPUT)
        pa_idxset_put(p->output_mappings, am, NULL);
    else
        p->input_mapping = am;
}

static pa_droid_profile *add_profile(pa_droid_profile_set *ps,
                                     const pa_droid_config_device *primary_output,
                                     const pa_droid_config_device *output,
                                     const pa_droid_config_device *input) {
    pa_droid_profile *ap;

    pa_assert(primary_output && primary_output->direction == PA_DIRECTION_OUTPUT);
    pa_assert(output && output->direction == PA_DIRECTION_OUTPUT);
    pa_assert(!input || input->direction == PA_DIRECTION_INPUT);

    pa_log_debug("New profile: %s-%s", output->name, input ? input->name : "no input");

    ap = droid_profile_new(ps, primary_output, output, input);

    pa_hashmap_put(ps->profiles, ap->name, ap);

    return ap;
}

static pa_droid_profile_set *profile_set_new(const pa_droid_config_hw_module *module) {
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

static void add_all_profiles(pa_droid_profile_set *ps,
                             const pa_droid_config_hw_module *module,
                             const pa_droid_config_device *primary_output) {
    pa_droid_config_device *output;
    pa_droid_config_device *input;

    pa_assert(ps);
    pa_assert(module);
    pa_assert(primary_output && primary_output->direction == PA_DIRECTION_OUTPUT);

    /* Each distinct hw module output matches one profile. If there are multiple inputs
     * combinations are made so that all possible outputs and inputs can be selected.
     * So for outputs "primary" and "hdmi" and input "primary" profiles
     * "primary-primary" and "hdmi-primary" are created. */

    SLLIST_FOREACH(output, module->outputs) {

        if (module->inputs) {
            SLLIST_FOREACH(input, module->inputs)
                add_profile(ps, primary_output, output, input);
        } else
            add_profile(ps, primary_output, output, NULL);
    }
}

pa_droid_profile_set *pa_droid_profile_set_new(const pa_droid_config_hw_module *module) {
    pa_droid_profile_set *ps;

    ps = profile_set_new(module);
    add_all_profiles(ps, module, NULL);

    return ps;
}

static void add_default_profile(pa_droid_profile_set *ps,
                                const pa_droid_config_hw_module *module,
                                const pa_droid_config_device *primary_output,
                                const pa_droid_config_device *low_latency_output,
                                const pa_droid_config_device *media_latency_output,
                                const pa_droid_config_device *inputs) {

    pa_droid_profile *p;

    pa_assert(ps);
    pa_assert(module);
    pa_assert(!primary_output || primary_output->direction == PA_DIRECTION_OUTPUT);
    pa_assert(!low_latency_output || primary_output->direction == PA_DIRECTION_OUTPUT);
    pa_assert(!media_latency_output || primary_output->direction == PA_DIRECTION_OUTPUT);

    pa_log_debug("New default profile");

    p = profile_new(ps, module, "default", "Default profile");

    if (primary_output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, primary_output), NULL);
    if (low_latency_output && primary_output != low_latency_output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, low_latency_output), NULL);
    if (media_latency_output && primary_output != media_latency_output && low_latency_output != media_latency_output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, media_latency_output), NULL);

    if (inputs)
        p->input_mapping = pa_droid_mapping_get(ps, inputs);

    p->priority += DEFAULT_PRIORITY * (pa_idxset_size(p->output_mappings) + p->input_mapping ? 1 : 0);
    p->priority += primary_output ? DEFAULT_PRIORITY : 0;
    pa_hashmap_put(ps->profiles, p->name, p);
}

static void auto_add_profiles(pa_droid_profile_set *ps,
                              const pa_droid_config_hw_module *module) {
    const pa_droid_config_device *output;
    const pa_droid_config_device *primary_output        = NULL;
    const pa_droid_config_device *low_latency_output    = NULL;
    const pa_droid_config_device *media_latency_output  = NULL;

    pa_assert(ps);
    pa_assert(module);

    /* first find different output types */
    SLLIST_FOREACH(output, module->outputs) {
        if (output->flags & AUDIO_OUTPUT_FLAG_PRIMARY)
            primary_output = output;

#if defined(HAVE_ENUM_AUDIO_OUTPUT_FLAG_RAW)
        else if (output->flags & AUDIO_OUTPUT_FLAG_RAW)
            pa_log_debug("Ignore output %s with flag AUDIO_OUTPUT_FLAG_RAW", output->name);
#endif

#if defined(HAVE_ENUM_AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
        else if (output->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
            pa_log_debug("Ignore output %s with flag AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD", output->name);
#endif

        else if (output->flags & AUDIO_OUTPUT_FLAG_FAST)
            low_latency_output = output;

        else if (output->flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER)
            media_latency_output = output;
    }

    add_default_profile(ps, module,
                        primary_output, low_latency_output, media_latency_output,
                        module->inputs);
}

pa_droid_profile_set *pa_droid_profile_set_default_new(const pa_droid_config_hw_module *module) {
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
    pa_xfree(am);
}

void pa_droid_profile_free(pa_droid_profile *ap) {
    pa_assert(ap);

    pa_xfree(ap->name);
    pa_xfree(ap->description);
    if (ap->output_mappings)
        pa_idxset_free(ap->output_mappings, NULL);
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

static pa_droid_port *create_o_port(pa_droid_mapping *am, uint32_t device, const char *name, const char *description) {
    pa_droid_port *p;
    char *desc;

    pa_assert(am);
    pa_assert(name);

    pa_log_debug("  New output port %s", name);
    p = pa_xnew0(pa_droid_port, 1);

    p->mapping = am;
    p->name = pa_xstrdup(name);
    if (description) {
        p->description = pa_xstrdup(description);
    } else {
        desc = pa_replace(name, "output-", "Output to ");
        p->description = pa_replace(desc, "_", " ");
        pa_xfree(desc);
    }
    p->priority = DEFAULT_PRIORITY;
    p->device = device;

    if (am->output->module->global_config ? am->output->module->global_config->attached_output_devices & device
                                          : am->profile_set->config->global_config->attached_output_devices & device)
        p->priority += DEFAULT_PRIORITY;

    if (am->output->module->global_config ? am->output->module->global_config->default_output_device & device
                                          : am->profile_set->config->global_config->default_output_device & device)
        p->priority += DEFAULT_PRIORITY;

    return p;
}

static void add_o_ports(pa_droid_mapping *am) {
    pa_droid_port *p;
    const char *name;
    uint32_t devices;
    uint32_t combo_devices;
    uint32_t i = 0;

    pa_assert(am);

    devices = am->output->devices;

    /* IHF combo devices, these devices are combined with IHF */
    combo_devices = AUDIO_DEVICE_OUT_SPEAKER | AUDIO_DEVICE_OUT_WIRED_HEADPHONE;

    while (devices) {
        uint32_t cur_device = (1 << i++);

        if (devices & cur_device) {

            pa_assert_se(pa_droid_output_port_name(cur_device, &name));

            if (!(p = pa_hashmap_get(am->profile_set->all_ports, name))) {

                p = create_o_port(am, cur_device, name, NULL);
                pa_hashmap_put(am->profile_set->all_ports, p->name, p);
            } else
                pa_log_debug("  Output port %s from cache", name);

            pa_idxset_put(am->ports, p, NULL);

            devices &= ~cur_device;
        }
    }

    /* Combo devices, route to multiple routing targets simultaneously. */
    if (am->output->devices & combo_devices) {
        pa_assert_se(pa_droid_output_port_name(combo_devices, &name));
        if (!(p = pa_hashmap_get(am->profile_set->all_ports, name))) {
            p = create_o_port(am, combo_devices, name, NULL);
            /* Reset priority to default. */
            p->priority = DEFAULT_PRIORITY;

            pa_hashmap_put(am->profile_set->all_ports, p->name, p);
        } else
            pa_log_debug("  Output port %s from cache", name);

        pa_idxset_put(am->ports, p, NULL);
    }

    if (!(p = pa_hashmap_get(am->profile_set->all_ports, PA_DROID_OUTPUT_PARKING))) {
        /* Create parking port for output mapping to be used when audio_mode_t changes. */
        p = create_o_port(am, 0, PA_DROID_OUTPUT_PARKING, "Parking port");
        /* Reset priority to half of default */
        p->priority = DEFAULT_PRIORITY / 2;

        pa_hashmap_put(am->profile_set->all_ports, p->name, p);
    } else
        pa_log_debug("  Output port %s from cache", PA_DROID_OUTPUT_PARKING);

    pa_idxset_put(am->ports, p, NULL);
}

static void add_i_port(pa_droid_mapping *am, uint32_t device, const char *name) {
    pa_droid_port *p;
    char *desc;

    pa_assert(am);
    pa_assert(name);

    if (!(p = pa_hashmap_get(am->profile_set->all_ports, name))) {
        pa_log_debug("  New input port %s", name);
        p = pa_xnew0(pa_droid_port, 1);

        p->mapping = am;
        p->name = pa_xstrdup(name);
        desc = pa_replace(name, "input-", "Input from ");
        p->description = pa_replace(desc, "_", " ");
        pa_xfree(desc);
        p->priority = DEFAULT_PRIORITY;
        p->device = device;

        if (am->inputs->module->global_config ? am->inputs->module->global_config->attached_input_devices & device
                                              : am->profile_set->config->global_config->attached_input_devices & device)
            p->priority += DEFAULT_PRIORITY;

        pa_hashmap_put(am->profile_set->all_ports, p->name, p);
    } else
        pa_log_debug("  Input port %s from cache", name);

    pa_idxset_put(am->ports, p, NULL);
}

static void add_i_ports(pa_droid_mapping *am) {
    pa_droid_port *p;
    const char *name;
    const pa_droid_config_device *input;
    uint32_t devices = AUDIO_DEVICE_IN_DEFAULT;
    uint32_t i = 0;

    pa_assert(am);

    SLLIST_FOREACH(input, am->inputs)
        devices |= input->devices;

#if AUDIO_API_VERSION_MAJ >= 2
    devices &= ~AUDIO_DEVICE_BIT_IN;
#endif

    while (devices) {
        uint32_t cur_device = (1 << i++);

        if (devices & cur_device) {

#if AUDIO_API_VERSION_MAJ >= 2
            cur_device |= AUDIO_DEVICE_BIT_IN;
#endif

            pa_assert_se(pa_droid_input_port_name(cur_device, &name));
            add_i_port(am, cur_device, name);

            devices &= ~cur_device;
        }
    }

#if AUDIO_API_VERSION_MAJ == 1
    /* HAL v1 has default input device defined as another input device,
     * so we need to add it by hand here. */
    add_i_port(am, AUDIO_DEVICE_IN_DEFAULT, "input-default");
#endif

    if (!(p = pa_hashmap_get(am->profile_set->all_ports, PA_DROID_INPUT_PARKING))) {
        pa_log_debug("  New input port %s", PA_DROID_INPUT_PARKING);
        /* Create parking port for input mapping to be used when audio_mode_t changes. */
        p = pa_xnew0(pa_droid_port, 1);
        p->mapping = am;
        p->name = pa_sprintf_malloc(PA_DROID_INPUT_PARKING);
        p->description = pa_sprintf_malloc("Parking port");
        p->priority = 50;
        p->device = 0; /* No routing */

        pa_hashmap_put(am->profile_set->all_ports, p->name, p);
    } else
        pa_log_debug("  Input port %s from cache", PA_DROID_INPUT_PARKING);

    pa_idxset_put(am->ports, p, NULL);
}

pa_droid_mapping *pa_droid_mapping_get(pa_droid_profile_set *ps, const pa_droid_config_device *device) {
    pa_droid_mapping *am;
    pa_hashmap *map;

    pa_assert(ps);
    pa_assert(device);

    map = device->direction == PA_DIRECTION_OUTPUT ? ps->output_mappings : ps->input_mappings;

    if ((am = pa_hashmap_get(map, device->name))) {
        pa_log_debug("  %s mapping %s from cache", pa_direction_to_string(device->direction), device->name);
        return am;
    }
    pa_log_debug("  New %s mapping %s", pa_direction_to_string(device->direction), device->name);

    am = pa_xnew0(pa_droid_mapping, 1);
    am->profile_set = ps;
    am->proplist = pa_proplist_new();
    am->direction = device->direction;
    am->ports = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);;

    if (am->direction == PA_DIRECTION_OUTPUT) {
        am->name = pa_xstrdup(device->name);
        am->output = device;
        add_o_ports(am);
    } else {
        /* Use common name */
        am->name = pa_xstrdup("droid");
        /* Use all inputs as a list */
        am->inputs = device;
        add_i_ports(am);
    }

    pa_hashmap_put(map, am->name, am);

    return am;
}

bool pa_droid_mapping_is_primary(pa_droid_mapping *am) {
    pa_assert(am);

    if (am->direction == PA_DIRECTION_OUTPUT) {
        pa_assert(am->output);
        return am->output->flags & AUDIO_OUTPUT_FLAG_PRIMARY;
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
            data->device = p->device;
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

void pa_droid_quirk_log(pa_droid_hw_module *hw) {
    uint32_t i;

    pa_assert(hw);

    if (hw->quirks) {
        for (i = 0; i < sizeof(valid_quirks) / sizeof(struct droid_quirk); i++) {
            if (hw->quirks->enabled[i]) {
                pa_log_debug("Enabled quirks:");
                for (i = 0; i < sizeof(valid_quirks) / sizeof(struct droid_quirk); i++)
                    if (hw->quirks->enabled[i])
                        pa_log_debug("  %s", valid_quirks[i].name);
                return;
            }
        }
    }
}

static pa_droid_quirks *get_quirks(pa_droid_quirks *q) {
    if (!q)
        q = pa_xnew0(pa_droid_quirks, 1);

    return q;
}

static pa_droid_quirks *set_default_quirks(pa_droid_quirks *q) {
    q = NULL;

#if (ANDROID_VERSION_MAJOR >= 5) || defined(DROID_AUDIO_HAL_ATOI_FIX)
    q = get_quirks(q);
    q->enabled[QUIRK_INPUT_ATOI] = true;
#endif

#if defined(DROID_DEVICE_ANZU) ||\
    defined(DROID_DEVICE_COCONUT) || defined(DROID_DEVICE_HAIDA) ||\
    defined(DROID_DEVICE_HALLON) || defined(DROID_DEVICE_IYOKAN) ||\
    defined(DROID_DEVICE_MANGO) || defined(DROID_DEVICE_SATSUMA) ||\
    defined(DROID_DEVICE_SMULTRON) || defined(DROID_DEVICE_URUSHI)
#warning Using set_parameters hack, originating from previous cm10 mako.
    q = get_quirks(q);
    q->enabled[QUIRK_SET_PARAMETERS] = true;
#endif

    q = get_quirks(q);
    q->enabled[QUIRK_CLOSE_INPUT] = true;

    return q;
}

bool pa_droid_quirk_parse(pa_droid_hw_module *hw, const char *quirks) {
    char *quirk = NULL;
    char *d;
    const char *state = NULL;

    pa_assert(hw);
    pa_assert(quirks);

    hw->quirks = get_quirks(hw->quirks);

    while ((quirk = pa_split(quirks, ",", &state))) {
        uint32_t i;
        bool enable = false;

        if (strlen(quirk) < 2)
            goto error;

        d = quirk + 1;

        if (quirk[0] == '+')
            enable = true;
        else if (quirk[0] == '-')
            enable = false;
        else
            goto error;

        for (i = 0; i < sizeof(valid_quirks) / sizeof(struct droid_quirk); i++) {
            if (pa_streq(valid_quirks[i].name, d))
                hw->quirks->enabled[valid_quirks[i].value] = enable;
        }

        pa_xfree(quirk);
    }

    return true;

error:
    pa_log("Incorrect quirk definition \"%s\" (\"%s\")", quirk ? quirk : "<null>", quirks);
    pa_xfree(quirk);

    return false;
}

static void update_sink_types(pa_droid_hw_module *hw, pa_sink *ignore_sink) {
    pa_sink *sink;
    pa_sink *primary_sink       = NULL;
    pa_sink *low_latency_sink   = NULL;
    pa_sink *media_latency_sink = NULL;
    pa_sink *offload_sink       = NULL;

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

        if (s->output->flags & AUDIO_OUTPUT_FLAG_PRIMARY)
            primary_sink = sink;

        if (s->output->flags & AUDIO_OUTPUT_FLAG_FAST)
            low_latency_sink = sink;

        if (s->output->flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER)
            media_latency_sink = sink;

#if defined(HAVE_ENUM_AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
        if (s->output->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
            offload_sink = sink;
#endif
    }

    if (primary_sink == low_latency_sink)
        low_latency_sink = NULL;

    if (primary_sink == media_latency_sink)
        media_latency_sink = NULL;

    if (low_latency_sink)
        pa_proplist_sets(low_latency_sink->proplist, PROP_DROID_OUTPUT_LOW_LATENCY, "true");

    if (media_latency_sink)
        pa_proplist_sets(media_latency_sink->proplist, PROP_DROID_OUTPUT_MEDIA_LATENCY, "true");

    if (offload_sink)
        pa_proplist_sets(offload_sink->proplist, PROP_DROID_OUTPUT_OFFLOAD, "true");

    if (primary_sink) {
        pa_proplist_sets(primary_sink->proplist, PROP_DROID_OUTPUT_PRIMARY, "true");
        pa_proplist_sets(primary_sink->proplist, PROP_DROID_OUTPUT_LOW_LATENCY, low_latency_sink ? "false" : "true");
        pa_proplist_sets(primary_sink->proplist, PROP_DROID_OUTPUT_MEDIA_LATENCY, media_latency_sink ? "false" : "true");
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

static pa_droid_hw_module *droid_hw_module_open(pa_core *core, const pa_droid_config_audio *config, const char *module_id) {
    const pa_droid_config_hw_module *module;
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

    if (!(module = pa_droid_config_find_module(config, module_id))) {
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
    PA_REFCNT_INIT(hw);
    hw->core = core;
    hw->hwmod = hwmod;
    hw->hw_mutex = pa_mutex_new(true, false);
    hw->output_mutex = pa_mutex_new(true, false);
    hw->input_mutex = pa_mutex_new(true, false);
    hw->device = device;
    hw->config = pa_droid_config_dup(config);
    hw->enabled_module = pa_droid_config_find_module(hw->config, module_id);
    hw->module_id = hw->enabled_module->name;
    hw->shared_name = shared_name_get(hw->module_id);
    hw->outputs = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    hw->inputs = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    hw->quirks = set_default_quirks(hw->quirks);

    hw->sink_put_hook_slot      = pa_hook_connect(&core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_EARLY-10,
                                                  sink_put_hook_cb, hw);
    hw->sink_unlink_hook_slot   = pa_hook_connect(&core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_EARLY-10,
                                                  sink_unlink_hook_cb, hw);

    pa_assert_se(pa_shared_set(core, hw->shared_name, hw) >= 0);

    return hw;

fail:
    if (device)
        audio_hw_device_close(device);

    if (hw)
        pa_xfree(hw);

    return NULL;
}

pa_droid_hw_module *pa_droid_hw_module_get(pa_core *core, const pa_droid_config_audio *config, const char *module_id) {
    pa_droid_hw_module *hw;
    char *shared_name;

    pa_assert(core);
    pa_assert(module_id);

    shared_name = shared_name_get(module_id);
    if ((hw = pa_shared_get(core, shared_name)))
        hw = pa_droid_hw_module_ref(hw);
    else
        hw = droid_hw_module_open(core, config, module_id);

    pa_xfree(shared_name);
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

    if (hw->sink_put_hook_slot)
        pa_hook_slot_free(hw->sink_put_hook_slot);
    if (hw->sink_unlink_hook_slot)
        pa_hook_slot_free(hw->sink_unlink_hook_slot);

    if (hw->config)
        pa_droid_config_free(hw->config);

    if (hw->device) {
        if (pa_droid_quirk(hw, QUIRK_UNLOAD_CALL_EXIT))
            exit(EXIT_SUCCESS);
        else if (!pa_droid_quirk(hw, QUIRK_UNLOAD_NO_CLOSE))
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

    pa_xfree(hw->quirks);

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

static pa_droid_stream *droid_stream_new(pa_droid_hw_module *module) {
    pa_droid_stream *s;

    s = pa_xnew0(pa_droid_stream, 1);
    PA_REFCNT_INIT(s);

    s->module = pa_droid_hw_module_ref(module);

    return s;
}

static pa_droid_output_stream *droid_output_stream_new(void) {
    return pa_xnew0(pa_droid_output_stream, 1);
}

static pa_droid_input_stream *droid_input_stream_new(void) {
    return pa_xnew0(pa_droid_input_stream, 1);
}

static bool stream_config_fill(audio_devices_t devices,
                               pa_sample_spec *sample_spec,
                               pa_channel_map *channel_map,
                               struct audio_config *config) {
    audio_format_t hal_audio_format = 0;
    audio_channel_mask_t hal_channel_mask = 0;
    bool voicecall_record = false;
    bool output = true;

    pa_assert(sample_spec);
    pa_assert(channel_map);
    pa_assert(config);

#if AUDIO_API_VERSION_MAJ >= 2
    if (devices & AUDIO_DEVICE_BIT_IN) {
        output = false;
        devices &= ~AUDIO_DEVICE_BIT_IN;
    }
#else
    output = !(devices & AUDIO_DEVICE_IN_ALL);
#endif

    if (devices & AUDIO_DEVICE_IN_VOICE_CALL)
        voicecall_record = true;

    if (!pa_convert_format(sample_spec->format, CONV_FROM_PA, &hal_audio_format)) {
        pa_log("Sample spec format %u not supported.", sample_spec->format);
        goto fail;
    }

    for (int i = 0; i < channel_map->channels; i++) {
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

    if (voicecall_record) {
        /* Only allow recording both downlink and uplink. */
#if defined(QCOM_HARDWARE)
        pa_channel_map_init_mono(channel_map);
        sample_spec->channels = 1;
  #if (ANDROID_VERSION_MAJOR <= 4) && defined(HAVE_ENUM_AUDIO_CHANNEL_IN_VOICE_CALL_MONO)
        hal_channel_mask = AUDIO_CHANNEL_IN_VOICE_CALL_MONO;
  #else
        hal_channel_mask = AUDIO_CHANNEL_IN_MONO;
  #endif
#elif defined(HAVE_ENUM_AUDIO_CHANNEL_IN_VOICE_UPLINK) && defined(HAVE_ENUM_AUDIO_CHANNEL_IN_VOICE_DNLINK)
        pa_channel_map_init_stereo(channel_map);
        sample_spec->channels = 2;
        hal_channel_mask = AUDIO_CHANNEL_IN_VOICE_UPLINK | AUDIO_CHANNEL_IN_VOICE_DNLINK;
#else
        pa_channel_map_init_mono(channel_map);
        sample_spec->channels = 1;
        hal_channel_mask = AUDIO_CHANNEL_IN_MONO;
#endif
    }

    memset(config, 0, sizeof(*config));
    config->sample_rate = sample_spec->rate;
    config->channel_mask = hal_channel_mask;
    config->format = hal_audio_format;

    return true;

fail:
    return false;
}

pa_droid_stream *pa_droid_open_output_stream(pa_droid_hw_module *module,
                                             const pa_sample_spec *spec,
                                             const pa_channel_map *map,
                                             audio_output_flags_t flags,
                                             audio_devices_t devices) {
    pa_droid_stream *s = NULL;
    pa_droid_output_stream *output = NULL;
    pa_droid_stream *primary_stream = NULL;
    int ret;
    struct audio_stream_out *stream;
    pa_channel_map channel_map;
    pa_sample_spec sample_spec;
    struct audio_config config_out;

    pa_assert(module);
    pa_assert(spec);
    pa_assert(map);

    sample_spec = *spec;
    channel_map = *map;

    if (!stream_config_fill(devices, &sample_spec, &channel_map, &config_out))
        goto fail;

    if (pa_idxset_size(module->outputs) == 0)
        pa_log_debug("Set initial output device to %#010x", devices);
    else if ((primary_stream = get_primary_output(module))) {
        pa_log_debug("Primary output with device %#010x already open, using as initial device.",
                     primary_stream->output->device);
        devices = primary_stream->output->device;
    }

    pa_droid_hw_module_lock(module);
    ret = module->device->open_output_stream(module->device,
                                             ++module->stream_out_id,
                                             devices,
                                             flags,
                                             &config_out,
                                             &stream
#if AUDIO_API_VERSION_MAJ >= 3
                                             /* Go with empty address, should work
                                              * with most devices for now. */
                                             , NULL
#endif
                                             );
    pa_droid_hw_module_unlock(module);

    if (ret < 0 || !stream) {
        pa_log("Failed to open output stream: %d", ret);
        goto fail;
    }

    s = droid_stream_new(module);
    s->output = output = droid_output_stream_new();
    output->stream = stream;
    output->sample_spec = *spec;
    output->channel_map = *map;
    output->flags = flags;
    output->device = devices;

    if ((output->sample_spec.rate = output->stream->common.get_sample_rate(&output->stream->common)) != spec->rate)
        pa_log_warn("Requested sample rate %u but got %u instead.", spec->rate, output->sample_spec.rate);

    pa_idxset_put(module->outputs, s, NULL);

    s->buffer_size = output->stream->common.get_buffer_size(&output->stream->common);

    pa_log_info("Opened droid output stream %p with device: %u flags: %u sample rate: %u channels: %u (%u) format: %u (%u) buffer size: %u (%llu usec)",
            (void *) s,
            devices,
            output->flags,
            output->sample_spec.rate,
            output->sample_spec.channels, config_out.channel_mask,
            output->sample_spec.format, config_out.format,
            s->buffer_size,
            pa_bytes_to_usec(s->buffer_size, &output->sample_spec));

    return s;

fail:
    pa_xfree(s);
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

static void set_active_input(pa_droid_hw_module *hw_module, pa_droid_stream *stream) {
    pa_assert(hw_module);

    if (hw_module->state.active_input != stream) {
        pa_log_info("Set active input to %p", (void *) stream);
        hw_module->state.active_input = stream;
    }
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
                           audio_devices_t device,
                           audio_source_t source,
                           uint32_t flags,  /* audio_input_flags_t */
                           const pa_sample_spec *sample_spec,
                           const struct audio_config *config,
                           int return_code) {
    pa_logl(log_level,
            "%s input stream with device: %#010x source: %#010x flags: %#010x sample rate: %u (%u) channels: %u (%#010x) format: %u (%#010x) (return code %d)",
            prefix,
            device,
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

static int input_stream_open(pa_droid_stream *s, bool resume_from_suspend) {
    pa_droid_hw_module *hw_module;
    pa_droid_input_stream *input = NULL;
    pa_channel_map channel_map;
    pa_sample_spec sample_spec;
    size_t buffer_size;
    int ret = -1;

    struct audio_config config_try;
    struct audio_config config_in;

    bool diff_sample_rate = false;
    bool diff_channel_mask = false;
    bool diff_format = false;

    pa_assert(s);
    pa_assert(s->input);
    pa_assert_se((hw_module = s->module));

    if (s->input->stream) /* already open */
        return 0;

    pa_assert(!s->module->state.active_input);

    input = s->input;
    input->stream = NULL;

    /* Copy our requested specs */
    sample_spec = input->req_sample_spec;
    channel_map = input->req_channel_map;

    if (!stream_config_fill(hw_module->state.input_device, &sample_spec, &channel_map, &config_try))
        goto done;

    pa_droid_hw_module_lock(s->module);
    while (true) {
        config_in = config_try;

        log_input_open(PA_LOG_INFO, "Trying to open",
                       hw_module->state.input_device,
                       hw_module->state.audio_source,
                       0, /* AUDIO_INPUT_FLAG_NONE on v3. v1 and v2 don't have input flags. */
                       &sample_spec,
                       &config_in,
                       0);

        ret = hw_module->device->open_input_stream(hw_module->device,
                                                   ++hw_module->stream_in_id,
                                                   hw_module->state.input_device,
                                                   &config_in,
                                                   &input->stream
#if AUDIO_API_VERSION_MAJ >= 3
                                                   , 0
                                                   , NULL                   /* Don't define address */
                                                   , hw_module->state.audio_source
#endif
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
                    break;
                }

                config_try = config_in;
                continue;
            } else {
                pa_log_warn("Could not open input stream and no suggested changes received, bailing out.");
                break;
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

        break;
    }
    pa_droid_hw_module_unlock(s->module);

    if (ret < 0 || !input->stream) {
        log_input_open(resume_from_suspend ? PA_LOG_INFO : PA_LOG_ERROR, "Failed to open",
                       hw_module->state.input_device,
                       hw_module->state.audio_source,
                       0, /* AUDIO_INPUT_FLAG_NONE on v3. v1 and v2 don't have input flags. */
                       &sample_spec,
                       &config_in,
                       ret);

        goto done;
    }

    log_input_open(PA_LOG_INFO, "Opened",
                   hw_module->state.input_device,
                   hw_module->state.audio_source,
                   0, /* AUDIO_INPUT_FLAG_NONE on v3. v1 and v2 don't have input flags. */
                   &sample_spec,
                   &config_in,
                   ret);

    input->sample_spec = sample_spec;
    input->channel_map = channel_map;
    buffer_size = input->stream->common.get_buffer_size(&input->stream->common);
    s->buffer_size = buffer_size;

    /* Set input stream to standby */
    s->input->stream->common.standby(&s->input->stream->common);

    pa_log_debug("Opened input stream %p", (void *) s);
    set_active_input(s->module, s);

done:
    return ret;
}

static void input_stream_close(pa_droid_stream *s) {
    pa_assert(s);
    pa_assert(s->input);
    pa_assert(s->input->stream);

    if (!s->input->stream)
        return;

    pa_mutex_lock(s->module->input_mutex);
    set_active_input(s->module, NULL);
    s->module->device->close_input_stream(s->module->device, s->input->stream);
    s->input->stream = NULL;
    pa_log_debug("Closed input stream %p", (void *) s);
    pa_mutex_unlock(s->module->input_mutex);
}

pa_droid_stream *pa_droid_open_input_stream(pa_droid_hw_module *hw_module,
                                            const pa_sample_spec *requested_sample_spec,
                                            const pa_channel_map *requested_channel_map) {

    pa_droid_stream *s = NULL;
    pa_droid_input_stream *input = NULL;

    pa_assert(hw_module);
    pa_assert(requested_sample_spec);
    pa_assert(requested_channel_map);

    if (hw_module->state.active_input) {
        pa_log_warn("Opening input stream while there is already active input stream.");
        return pa_droid_stream_ref(hw_module->state.active_input);
    }

    s = droid_stream_new(hw_module);
    s->input = input = droid_input_stream_new();

    /* Copy our requested specs, so we know them when resuming from suspend
     * as well. */
    input->req_sample_spec = *requested_sample_spec;
    input->req_channel_map = *requested_channel_map;

    if (input_stream_open(s, false) < 0) {
        pa_droid_stream_unref(s);
        s = NULL;
    }

    return s;
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
        set_active_input(s->module, NULL);
        pa_mutex_lock(s->module->input_mutex);
        pa_idxset_remove_by_data(s->module->inputs, s, NULL);
        if (s->input->stream) {
            s->input->stream->common.standby(&s->input->stream->common);
            s->module->device->close_input_stream(s->module->device, s->input->stream);
        }
        pa_mutex_unlock(s->module->input_mutex);
        pa_xfree(s->input);
    }

    pa_droid_hw_module_unref(s->module);

    pa_xfree(s);
}

static pa_droid_stream *get_primary_output(pa_droid_hw_module *hw) {
    pa_droid_stream *s;
    uint32_t idx;

    pa_assert(hw);
    pa_assert(hw->outputs);

    PA_IDXSET_FOREACH(s, hw->outputs, idx) {
        if (s->output->flags & AUDIO_OUTPUT_FLAG_PRIMARY)
            return s;
    }

    return NULL;
}

static int droid_output_stream_set_route(pa_droid_stream *s, audio_devices_t device) {
    pa_droid_output_stream *output;
    pa_droid_stream *slave;
    uint32_t idx;
    char *parameters = NULL;
    int ret = 0;

    pa_assert(s);
    pa_assert(s->output);
    pa_assert(s->module);
    pa_assert(s->module->output_mutex);

    output = s->output;

    pa_mutex_lock(s->module->output_mutex);

    if (output->flags & AUDIO_OUTPUT_FLAG_PRIMARY || get_primary_output(s->module) == NULL) {
        parameters = pa_sprintf_malloc("%s=%u;", AUDIO_PARAMETER_STREAM_ROUTING, device);

        pa_log_debug("output stream %p set_parameters(%s) %#010x", (void *) s, parameters, device);
        ret = output->stream->common.set_parameters(&output->stream->common, parameters);

        if (ret < 0) {
            if (ret == -ENOSYS)
                pa_log_warn("output set_parameters(%s) not allowed while stream is active", parameters);
            else
                pa_log_warn("output set_parameters(%s) failed", parameters);
        } else {
            /* Store last set output device. */
            output->device = device;
        }
    }

    if (output->flags & AUDIO_OUTPUT_FLAG_PRIMARY && pa_idxset_size(s->module->outputs) > 1) {
        pa_assert(parameters);

        PA_IDXSET_FOREACH(slave, s->module->outputs, idx) {
            if (slave == s)
                continue;

            pa_log_debug("slave output stream %p set_parameters(%s)", (void *) slave, parameters);
            ret = slave->output->stream->common.set_parameters(&slave->output->stream->common, parameters);

            if (ret < 0) {
                if (ret == -ENOSYS)
                    pa_log_warn("output set_parameters(%s) not allowed while stream is active", parameters);
                else
                    pa_log_warn("output set_parameters(%s) failed", parameters);
            } else
                slave->output->device = output->device;
        }
    }

    pa_xfree(parameters);

    pa_mutex_unlock(s->module->output_mutex);

    return ret;
}

static int input_stream_set_route(pa_droid_hw_module *hw_module) {
    pa_droid_stream *s;
    pa_droid_input_stream *input;
    audio_devices_t device;
    audio_source_t source;
    char *parameters = NULL;
    int ret = 0;

    pa_assert(hw_module);

    /* No active input, no need for set parameters */
    if (!(s = hw_module->state.active_input))
        goto done;

    input = s->input;

     /* Input stream closed, no need for set parameters */
    if (!input->stream)
        goto done;

    device = hw_module->state.input_device;
    source = hw_module->state.audio_source;
#ifdef DROID_DEVICE_I9305
    device &= ~AUDIO_DEVICE_BIT_IN;
#endif

    if (pa_droid_quirk(hw_module, QUIRK_INPUT_ATOI))
        parameters = pa_sprintf_malloc("%s=%d;%s=%u", AUDIO_PARAMETER_STREAM_ROUTING, (int32_t) device,
                                                      AUDIO_PARAMETER_STREAM_INPUT_SOURCE, source);
    else
        parameters = pa_sprintf_malloc("%s=%u;%s=%u", AUDIO_PARAMETER_STREAM_ROUTING, device,
                                                      AUDIO_PARAMETER_STREAM_INPUT_SOURCE, source);

    pa_log_debug("input stream %p set_parameters(%s) %#010x ; %#010x",
                 (void *) input, parameters, device, source);

    if (pa_droid_quirk(hw_module, QUIRK_SET_PARAMETERS)) {
        pa_mutex_lock(hw_module->hw_mutex);
        ret = hw_module->device->set_parameters(hw_module->device, parameters);
        pa_mutex_unlock(hw_module->hw_mutex);
    } else {
        pa_mutex_lock(hw_module->input_mutex);
        ret = input->stream->common.set_parameters(&input->stream->common, parameters);
        pa_mutex_unlock(hw_module->input_mutex);
    }

    if (ret < 0) {
        if (ret == -ENOSYS)
            pa_log_warn("input set_parameters(%s) not allowed while stream is active", parameters);
        else
            pa_log_warn("input set_parameters(%s) failed", parameters);
    }

    pa_xfree(parameters);

done:
    return ret;
}

int pa_droid_stream_set_route(pa_droid_stream *s, audio_devices_t device) {
    pa_assert(s);

    if (s->output)
        return droid_output_stream_set_route(s, device);
    else {
        pa_droid_hw_set_input_device(s->module, device);
        return input_stream_set_route(s->module);
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

int pa_droid_set_parameters(pa_droid_hw_module *hw, const char *parameters) {
    int ret;

    pa_assert(hw);
    pa_assert(parameters);

    pa_log_debug("hw %p set_parameters(%s)", (void *) hw, parameters);
    pa_mutex_lock(hw->hw_mutex);
    ret = hw->device->set_parameters(hw->device, parameters);
    pa_mutex_unlock(hw->hw_mutex);

    if (ret < 0)
        pa_log("hw module %p set_parameters(%s) failed: %d", (void *) hw, parameters, ret);

    return ret;
}

bool pa_droid_stream_is_primary(pa_droid_stream *s) {
    pa_assert(s);
    pa_assert(s->output || s->input);

    if (s->output)
        return s->output->flags & AUDIO_OUTPUT_FLAG_PRIMARY;

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
            return s->output->stream->common.standby(&s->output->stream->common);
        } else {
            pa_atomic_inc(&s->module->active_outputs);
        }
    } else {
        if (suspend) {
            if (s->input->stream) {
                if (pa_droid_quirk(s->module, QUIRK_CLOSE_INPUT)) {
                    s->input->stream->common.standby(&s->input->stream->common);
                    input_stream_close(s);
                } else
                    return s->input->stream->common.standby(&s->input->stream->common);
            }
        } else if (pa_droid_quirk(s->module, QUIRK_CLOSE_INPUT))
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

    pa_droid_hw_module_lock(hw_module);
    if (hw_module->device->set_mode(hw_module->device, mode) < 0) {
        ret = false;
        pa_log_warn("Failed to set mode.");
    } else {
        hw_module->state.mode = mode;
    }
    pa_droid_hw_module_unlock(hw_module);

    /* Update possible audio source. */
    pa_droid_hw_set_input_device(hw_module, hw_module->state.input_device);

    return ret;
}

bool pa_droid_hw_set_input_device(pa_droid_hw_module *hw_module,
                                  audio_devices_t device) {
    audio_source_t audio_source = AUDIO_SOURCE_DEFAULT;
    audio_source_t audio_source_override = AUDIO_SOURCE_DEFAULT;
    bool device_changed = false;
    bool source_changed = false;
    const char *audio_source_name;

    pa_assert(hw_module);

    if (hw_module->state.input_device != device) {
        pa_log_debug("Set global input to %#010x", device);
        hw_module->state.input_device = device;
        device_changed = true;
    }

    pa_input_device_default_audio_source(hw_module->state.input_device, &audio_source);

    /* Override audio source based on mode. */
    switch (hw_module->state.mode) {
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
        const char *from, *to;
        pa_droid_audio_source_name(audio_source, &from);
        pa_droid_audio_source_name(audio_source_override, &to);
        pa_log_info("Audio mode %s, overriding audio source %s with %s",
                    audio_mode_to_string(hw_module->state.mode),
                    from ? from : "<unknown>",
                    to ? to : "<unknown>");
        audio_source = audio_source_override;
    }

    if (audio_source != hw_module->state.audio_source) {
        pa_log_debug("set global audio source to %s (%#010x)",
                     pa_droid_audio_source_name(audio_source, &audio_source_name)
                       ? audio_source_name : "<unknown>",
                     audio_source);
        hw_module->state.audio_source = audio_source;
        source_changed = true;
    }

    if (hw_module->state.active_input && (device_changed || source_changed))
        input_stream_set_route(hw_module);

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
