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
#include <string.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>

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

#include "version.h"
#include "droid-util.h"
#include "droid-config.h"
#include "conversion.h"

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
};


#define DEFAULT_PRIORITY (100)


static const char * const droid_combined_auto_outputs[3]    = { "primary", "low_latency", NULL };
static const char * const droid_combined_auto_inputs[2]     = { "primary", NULL };

static void droid_port_free(pa_droid_port *p);

static pa_droid_stream *get_primary_output(pa_droid_hw_module *hw);
static int input_stream_set_route(pa_droid_stream *s, audio_devices_t device);

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
    p->input_mappings = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    pa_hashmap_put(ps->profiles, p->name, p);

    return p;
}

static pa_droid_profile *droid_profile_new(pa_droid_profile_set *ps,
                                           const pa_droid_config_output *primary_output,
                                           const pa_droid_config_output *output,
                                           const pa_droid_config_input *input) {
    pa_droid_profile *p;
    char *name;
    char *description;

    pa_assert(ps);
    pa_assert(output);

    name = pa_sprintf_malloc("%s%s%s", output->name, input ? "-" : "", input ? input->name : "");
    description = pa_sprintf_malloc("%s output%s%s%s", output->name,
                                                       input ? " and " : "",
                                                       input ? input->name : "",
                                                       input ? " input." : "");

    p = profile_new(ps, output->module, name, description);
    pa_xfree(name);
    pa_xfree(description);

    if (pa_streq(output->name, "primary")) {
        p->priority += DEFAULT_PRIORITY;

        if (input && pa_streq(input->name, "primary"))
            p->priority += DEFAULT_PRIORITY;
    }

    if (primary_output && primary_output != output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, PA_DIRECTION_OUTPUT, primary_output), NULL);
    if (output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, PA_DIRECTION_OUTPUT, output), NULL);
    if (input)
        pa_idxset_put(p->input_mappings, pa_droid_mapping_get(ps, PA_DIRECTION_INPUT, input), NULL);

    return p;
}

void pa_droid_profile_add_mapping(pa_droid_profile *p, pa_droid_mapping *am) {
    pa_assert(p);
    pa_assert(am);

    if (am->direction == PA_DIRECTION_OUTPUT)
        pa_idxset_put(p->output_mappings, am, NULL);
    else
        pa_idxset_put(p->input_mappings, am, NULL);
}

static pa_droid_profile *add_profile(pa_droid_profile_set *ps,
                                     const pa_droid_config_output *primary_output,
                                     const pa_droid_config_output *output,
                                     const pa_droid_config_input *input) {
    pa_droid_profile *ap;

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
                             const pa_droid_config_output *primary_output) {
    pa_droid_config_output *output;
    pa_droid_config_input *input;

    pa_assert(ps);
    pa_assert(module);

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
                                const pa_droid_config_output *primary_output,
                                const pa_droid_config_output *low_latency_output,
                                const pa_droid_config_output *media_latency_output,
                                const pa_droid_config_input *builtin_input,
                                const pa_droid_config_input *external_input,
                                bool merge_inputs) {

    pa_droid_profile *p;

    pa_assert(ps);
    pa_assert(module);

    pa_log_debug("New default profile");

    p = profile_new(ps, module, "default", "Default profile");

    if (primary_output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, PA_DIRECTION_OUTPUT, primary_output), NULL);
    if (low_latency_output && primary_output != low_latency_output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, PA_DIRECTION_OUTPUT, low_latency_output), NULL);
    if (media_latency_output && primary_output != media_latency_output && low_latency_output != media_latency_output)
        pa_idxset_put(p->output_mappings, pa_droid_mapping_get(ps, PA_DIRECTION_OUTPUT, media_latency_output), NULL);

    if (builtin_input && external_input && builtin_input != external_input && merge_inputs) {
        pa_idxset_put(p->input_mappings, pa_droid_mapping_merged_get(ps, builtin_input, external_input), NULL);
    } else {
        if (builtin_input)
            pa_idxset_put(p->input_mappings, pa_droid_mapping_get(ps, PA_DIRECTION_INPUT, builtin_input), NULL);
        if (external_input && builtin_input != external_input)
            pa_idxset_put(p->input_mappings, pa_droid_mapping_get(ps, PA_DIRECTION_INPUT, external_input), NULL);
    }

    p->priority += DEFAULT_PRIORITY * (pa_idxset_size(p->output_mappings) + pa_idxset_size(p->input_mappings));
    p->priority += primary_output ? DEFAULT_PRIORITY : 0;
    pa_hashmap_put(ps->profiles, p->name, p);
}

static void auto_add_profiles(pa_droid_profile_set *ps,
                              const pa_droid_config_hw_module *module,
                              bool merge_inputs) {
    const pa_droid_config_output *output;
    const pa_droid_config_input *input;

    const pa_droid_config_output *primary_output        = NULL;
    const pa_droid_config_output *low_latency_output    = NULL;
    const pa_droid_config_output *media_latency_output  = NULL;

    const pa_droid_config_input *builtin_input          = NULL;
    const pa_droid_config_input *external_input         = NULL;

    uint32_t input_devices;

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

        else if (output->flags & AUDIO_OUTPUT_FLAG_FAST)
            low_latency_output = output;

        else if (output->flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER)
            media_latency_output = output;
    }

    SLLIST_FOREACH(input, module->inputs) {
        input_devices = input->devices;
#if AUDIO_API_VERSION_MAJ >= 2
        input_devices &= ~AUDIO_DEVICE_BIT_IN;
#endif
        if (input_devices & (AUDIO_DEVICE_IN_BUILTIN_MIC | AUDIO_DEVICE_IN_BACK_MIC))
            builtin_input = input;

        else if (input_devices & AUDIO_DEVICE_IN_WIRED_HEADSET)
            external_input = input;
    }

    add_default_profile(ps, module,
                        primary_output, low_latency_output, media_latency_output,
                        builtin_input, external_input, merge_inputs);
    add_all_profiles(ps, module, primary_output);
}

pa_droid_profile_set *pa_droid_profile_set_default_new(const pa_droid_config_hw_module *module,
                                                       bool merge_inputs) {
    pa_droid_profile_set *ps;

    ps = profile_set_new(module);
    auto_add_profiles(ps, module, merge_inputs);

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
    if (ap->input_mappings)
        pa_idxset_free(ap->input_mappings, NULL);
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

        if (am->input->module->global_config ? am->input->module->global_config->attached_input_devices & device
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
    uint32_t devices;
    uint32_t i = 0;

    pa_assert(am);

    devices = am->input->devices | AUDIO_DEVICE_IN_DEFAULT;
    if (am->input2)
        devices |= am->input2->devices;

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

pa_droid_mapping *pa_droid_mapping_get(pa_droid_profile_set *ps, pa_direction_t direction, const void *data) {
    pa_droid_mapping *am;
    pa_hashmap *map;
    const char *name;
    const pa_droid_config_output *output = NULL;
    const pa_droid_config_input *input = NULL;

    if (direction == PA_DIRECTION_OUTPUT) {
        output = (pa_droid_config_output *) data;
        map = ps->output_mappings;
        name = output->name;
    } else {
        input = (pa_droid_config_input *) data;
        map = ps->input_mappings;
        name = input->name;
    }

    if ((am = pa_hashmap_get(map, name))) {
        pa_log_debug("  %s mapping %s from cache", input ? "Input" : "Output", name);
        return am;
    }
    pa_log_debug("  New %s mapping %s", input ? "input" : "output", name);

    am = pa_xnew0(pa_droid_mapping, 1);
    am->profile_set = ps;
    am->name = pa_xstrdup(name);
    am->proplist = pa_proplist_new();
    am->direction = direction;
    am->output = output;
    am->input = input;
    am->ports = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);;

    if (am->direction == PA_DIRECTION_OUTPUT)
        add_o_ports(am);
    else
        add_i_ports(am);

    pa_hashmap_put(map, am->name, am);

    return am;
}

pa_droid_mapping *pa_droid_mapping_merged_get(pa_droid_profile_set *ps,
                                              const pa_droid_config_input *input1,
                                              const pa_droid_config_input *input2) {
    pa_droid_mapping *am;
    pa_hashmap *map = ps->input_mappings;
    char *name;

    name = pa_sprintf_malloc("%s+%s", input1->name, input2->name);

    if ((am = pa_hashmap_get(map, name))) {
        pa_log_debug("  Input mapping %s from cache", name);
        pa_xfree(name);
        return am;
    }
    pa_log_debug("  New input mapping %s", name);

    am = pa_xnew0(pa_droid_mapping, 1);
    am->profile_set = ps;
    am->name = name;
    am->proplist = pa_proplist_new();
    am->direction = PA_DIRECTION_INPUT;
    am->output = NULL;
    am->input = input1;
    am->input2 = input2;
    am->ports = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    add_i_ports(am);

    pa_hashmap_put(map, am->name, am);

    return am;
}

bool pa_droid_mapping_is_primary(pa_droid_mapping *am) {
    pa_assert(am);

    if (am->direction == PA_DIRECTION_OUTPUT) {
        pa_assert(am->output);
        return pa_streq(am->output->name, PA_DROID_PRIMARY_DEVICE);
    } else {
        pa_assert(am->input);
        /* merged input mapping is always primary */
        if (am->input && am->input2)
            return true;
        return pa_streq(am->input->name, PA_DROID_PRIMARY_DEVICE);
    }
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

pa_droid_mapping *pa_droid_idxset_mapping_with_device(pa_idxset *i, uint32_t device) {
    pa_droid_mapping *am;
    uint32_t idx;

    pa_assert(i);

#if AUDIO_API_VERSION_MAJ >= 2
    device &= ~AUDIO_DEVICE_BIT_IN;
#endif

    PA_IDXSET_FOREACH(am, i, idx) {
        if (am->direction == PA_DIRECTION_OUTPUT) {
            pa_assert(am->output);
            if (am->output->devices & device)
                return am;
        } else {
            uint32_t all_devices;
            pa_assert(am->input);
            all_devices = am->input->devices | (am->input2 ? am->input2->devices : 0);
            if (all_devices & device)
                return am;
        }
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

static void update_source_types(pa_droid_hw_module *hw, pa_source *ignore_source) {
    pa_source *source;
    pa_source *builtin_source   = NULL;
    pa_source *external_source  = NULL;

    pa_droid_stream *s;
    uint32_t idx;

    /* only update primary hw module types for now. */
    if (!pa_streq(hw->module_id, PA_DROID_PRIMARY_DEVICE))
        return;

    PA_IDXSET_FOREACH(s, hw->inputs, idx) {
        if (!(source = pa_droid_stream_get_data(s)))
            continue;

        if (source == ignore_source)
            continue;

        if (s->input->all_devices & (AUDIO_DEVICE_IN_BUILTIN_MIC | AUDIO_DEVICE_IN_BACK_MIC))
            builtin_source = source;

        if (s->input->all_devices & AUDIO_DEVICE_IN_WIRED_HEADSET)
            external_source = source;
    }

    if (builtin_source)
        pa_proplist_sets(builtin_source->proplist, PROP_DROID_INPUT_BUILTIN, "true");

    if (external_source)
        pa_proplist_sets(external_source->proplist, PROP_DROID_INPUT_EXTERNAL, "true");

    if (builtin_source && external_source && builtin_source != external_source) {
        pa_proplist_sets(builtin_source->proplist, PROP_DROID_INPUT_EXTERNAL, "false");
        pa_proplist_sets(external_source->proplist, PROP_DROID_INPUT_BUILTIN, "false");
    }
}

static pa_hook_result_t source_put_hook_cb(void *hook_data, void *call_data, void *slot_data) {
    pa_source *source       = call_data;
    pa_droid_hw_module *hw  = slot_data;

    if (!pa_source_is_droid_source(source))
        return PA_HOOK_OK;

    update_source_types(hw, NULL);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_unlink_hook_cb(void *hook_data, void *call_data, void *slot_data) {
    pa_source *source       = call_data;
    pa_droid_hw_module *hw  = slot_data;

    if (!pa_source_is_droid_source(source))
        return PA_HOOK_OK;

    update_source_types(hw, source);

    return PA_HOOK_OK;
}

static char *shared_name_get(const char *module_id) {
    pa_assert(module_id);
    return pa_sprintf_malloc("droid-hardware-module-%s", module_id);
}

static pa_droid_hw_module *droid_hw_module_open(pa_core *core, pa_droid_config_audio *config, const char *module_id) {
    const pa_droid_config_hw_module *module;
    pa_droid_hw_module *hw = NULL;
    struct hw_module_t *hwmod = NULL;
    audio_hw_device_t *device = NULL;
    int h;
    int ret;

    pa_assert(core);
    pa_assert(module_id);

    pa_log_info("Droid hw module %s", VERSION);

    if (!config) {
        pa_log("No configuration provided for opening module with id %s", module_id);
        goto fail;
    }

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
    hw->config = config; /* We take ownership of config struct. */
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
    hw->source_put_hook_slot    = pa_hook_connect(&core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_EARLY-10,
                                                  source_put_hook_cb, hw);
    hw->source_unlink_hook_slot = pa_hook_connect(&core->hooks[PA_CORE_HOOK_SOURCE_UNLINK], PA_HOOK_EARLY-10,
                                                  source_unlink_hook_cb, hw);

    for (h = 0; h < PA_DROID_HOOK_MAX; h++)
        pa_hook_init(&hw->hooks[h], hw);

    pa_assert_se(pa_shared_set(core, hw->shared_name, hw) >= 0);

    return hw;

fail:
    if (device)
        audio_hw_device_close(device);

    if (hw)
        pa_xfree(hw);

    return NULL;
}

pa_droid_hw_module *pa_droid_hw_module_get(pa_core *core, pa_droid_config_audio *config, const char *module_id) {
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
    int h;

    pa_assert(hw);

    pa_log_info("Closing hw module %s.%s (%s)", AUDIO_HARDWARE_MODULE_ID, hw->enabled_module->name, DROID_DEVICE_STRING);

    if (hw->sink_put_hook_slot)
        pa_hook_slot_free(hw->sink_put_hook_slot);
    if (hw->sink_unlink_hook_slot)
        pa_hook_slot_free(hw->sink_unlink_hook_slot);

    if (hw->source_put_hook_slot)
        pa_hook_slot_free(hw->source_put_hook_slot);
    if (hw->source_unlink_hook_slot)
        pa_hook_slot_free(hw->source_unlink_hook_slot);

    for (h = 0; h < PA_DROID_HOOK_MAX; h++)
        pa_hook_done(&hw->hooks[h]);

    if (hw->config)
        pa_droid_config_free(hw->config);

    if (hw->device && !pa_droid_quirk(hw, QUIRK_UNLOAD_NO_CLOSE))
        audio_hw_device_close(hw->device);

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
                                             module->stream_out_id++,
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

static int input_stream_open(pa_droid_stream *s, bool resume_from_suspend) {
    pa_droid_input_stream *input;
    audio_stream_in_t *stream;
    audio_source_t audio_source = AUDIO_SOURCE_DEFAULT;
    pa_channel_map channel_map;
    pa_sample_spec sample_spec;
    struct audio_config config_in;
    size_t buffer_size;
    bool buffer_size_changed = false;
    bool channel_map_changed = false;
    int ret = -1;

    pa_assert(s);
    pa_assert(s->input);
    pa_assert(!s->input->stream);

    input = s->input;

    channel_map = input->channel_map;
    sample_spec = input->sample_spec;

    if (!stream_config_fill(input->device, &sample_spec, &channel_map, &config_in))
        goto done;

    pa_input_device_default_audio_source(input->device, &audio_source);

    if (channel_map.channels != input->input_channel_map.channels)
        channel_map_changed = true;

    pa_droid_hw_module_lock(s->module);
    ret = s->module->device->open_input_stream(s->module->device,
                                               s->module->stream_in_id++,
                                               input->device,
                                               &config_in,
                                               &stream
#if AUDIO_API_VERSION_MAJ >= 3
                                               , input->flags
                                               , NULL                    /* Don't define address */
                                               , audio_source
#endif
                                              );
    pa_droid_hw_module_unlock(s->module);

    if (ret < 0 || !stream) {
        pa_logl(resume_from_suspend ? PA_LOG_DEBUG : PA_LOG_ERROR,
                "Failed to open input stream: %d with device: %u flags: %u sample rate: %u channels: %u (%u) format: %u (%u)",
               ret,
               input->device,
               0, /* AUDIO_INPUT_FLAG_NONE on v3. v1 and v2 don't have input flags. */
               config_in.sample_rate,
               sample_spec.channels,
               config_in.channel_mask,
               sample_spec.format,
               config_in.format);
        goto done;
    }

    input->stream = stream;
    input->input_sample_spec = sample_spec;
    input->input_channel_map = channel_map;
    buffer_size = input->stream->common.get_buffer_size(&input->stream->common);
    if (s->buffer_size != 0 && s->buffer_size != buffer_size)
        buffer_size_changed = true;
    s->buffer_size = buffer_size;

    /* we need to call standby before reading with some devices. */
    input->stream->common.standby(&input->stream->common);

    pa_log_debug("Opened input stream %p", (void *) s);

    input_stream_set_route(s, input->device);

    if (buffer_size_changed) {
        pa_log_debug("Input stream %p buffer size changed to %u.", (void *) s, s->buffer_size);
        pa_hook_fire(&s->module->hooks[PA_DROID_HOOK_INPUT_BUFFER_SIZE_CHANGED], (void *) s);
    }

    if (channel_map_changed) {
        pa_log_debug("Input stream %p channel count changed to %d.", (void *) s, input->input_channel_map.channels);
        pa_hook_fire(&s->module->hooks[PA_DROID_HOOK_INPUT_CHANNEL_MAP_CHANGED], (void *) s);
    }

done:
    return ret;
}

static void input_stream_close(pa_droid_stream *s) {
    pa_droid_input_stream *input;

    pa_assert(s);
    pa_assert(s->input);
    pa_assert(s->input->stream);

    input = s->input;

    pa_mutex_lock(s->module->input_mutex);
    s->module->device->close_input_stream(s->module->device, input->stream);
    input->stream = NULL;
    pa_log_debug("Closed input stream %p", (void *) s);
    pa_mutex_unlock(s->module->input_mutex);
}

pa_droid_stream *pa_droid_open_input_stream(pa_droid_hw_module *module,
                                            const pa_sample_spec *spec,
                                            const pa_channel_map *map,
                                            audio_devices_t devices,
                                            pa_droid_mapping *am) {

    pa_droid_stream *s = NULL;
    pa_droid_input_stream *input = NULL;
    int ret = -1;

    s = droid_stream_new(module);
    s->input = input = droid_input_stream_new();
    input->sample_spec = *spec;
    input->channel_map = *map;
    input->flags = 0;   /* AUDIO_INPUT_FLAG_NONE */
    input->device = devices;
    if (am)
        input->all_devices = am->input->devices | (am->input2 ? am->input2->devices : 0);
    else
        input->all_devices = devices;
#if AUDIO_API_VERSION_MAJ >= 2
    input->all_devices &= ~AUDIO_DEVICE_BIT_IN;
#endif

    if (am && am->input && am->input2)
        input->merged = true;

    /* We need to open the stream for a while so that we can know
     * what sample rate we get. We need the rate for droid source. */

    if ((ret = input_stream_open(s, false)) < 0)
        goto fail;

    if ((input->sample_spec.rate = input->stream->common.get_sample_rate(&input->stream->common)) != spec->rate)
        pa_log_warn("Requested sample rate %u but got %u instead.", spec->rate, input->sample_spec.rate);

    pa_idxset_put(module->inputs, s, NULL);

    pa_log_info("Opened droid input stream %p with device: %u flags: %u sample rate: %u channels: %u format: %u buffer size: %u (%llu usec)",
            (void *) s,
            devices,
            input->flags,
            input->sample_spec.rate,
            input->sample_spec.channels,
            input->sample_spec.format,
            s->buffer_size,
            pa_bytes_to_usec(s->buffer_size, &input->sample_spec));

    /* As audio_source_t may not have any effect when opening the input stream
     * set input parameters immediately after opening the stream. */
    if (!s->input->merged && !pa_droid_quirk(module, QUIRK_CLOSE_INPUT))
        input_stream_set_route(s, devices);

    /* We start the stream in suspended state. */
    pa_droid_stream_suspend(s, true);

    return s;

fail:
    pa_xfree(input);
    pa_xfree(s);

    return NULL;
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
        pa_mutex_lock(s->module->output_mutex);
        pa_idxset_remove_by_data(s->module->outputs, s, NULL);
        s->module->device->close_output_stream(s->module->device, s->output->stream);
        pa_mutex_unlock(s->module->output_mutex);
        pa_xfree(s->output);
    } else {
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

static int input_stream_set_route(pa_droid_stream *s, audio_devices_t device) {
    pa_droid_input_stream *input;
    audio_source_t source = (uint32_t) -1;
    char *parameters;
    int ret = 0;

    pa_assert(s);
    pa_assert(s->input);
    pa_assert(s->input->stream);

    input = s->input;

#ifdef DROID_DEVICE_I9305
    device &= ~AUDIO_DEVICE_BIT_IN;
#endif

    if (pa_input_device_default_audio_source(device, &source)) {
        if (pa_droid_quirk(s->module, QUIRK_INPUT_ATOI))
            parameters = pa_sprintf_malloc("%s=%d;%s=%u", AUDIO_PARAMETER_STREAM_ROUTING, (int32_t) device,
                                                          AUDIO_PARAMETER_STREAM_INPUT_SOURCE, source);
        else
            parameters = pa_sprintf_malloc("%s=%u;%s=%u", AUDIO_PARAMETER_STREAM_ROUTING, device,
                                                          AUDIO_PARAMETER_STREAM_INPUT_SOURCE, source);
    } else
            parameters = pa_sprintf_malloc("%s=%u", AUDIO_PARAMETER_STREAM_ROUTING, device);

    pa_log_debug("input stream %p set_parameters(%s) %#010x ; %#010x",
                 (void *) s, parameters, device, source);


    if (pa_droid_quirk(s->module, QUIRK_SET_PARAMETERS)) {
        pa_mutex_lock(s->module->hw_mutex);
        ret = s->module->device->set_parameters(s->module->device, parameters);
        pa_mutex_unlock(s->module->hw_mutex);
    } else {
        pa_mutex_lock(s->module->input_mutex);
        ret = input->stream->common.set_parameters(&input->stream->common, parameters);
        pa_mutex_unlock(s->module->input_mutex);
    }

    if (ret < 0) {
        if (ret == -ENOSYS)
            pa_log_warn("input set_parameters(%s) not allowed while stream is active", parameters);
        else
            pa_log_warn("input set_parameters(%s) failed", parameters);
    } else
        input->device = device;

    pa_xfree(parameters);

    return ret;
}

static int droid_input_stream_set_route(pa_droid_stream *s, audio_devices_t device) {
    int ret = 0;

    pa_assert(s);
    pa_assert(s->input);

    if (s->input->stream) {
        input_stream_set_route(s, device);
    } else {
        s->input->device = device;
        pa_log_debug("input stream (inactive) %p store route %#010x", (void *) s, device);
    }

    return ret;
}

int pa_droid_stream_set_route(pa_droid_stream *s, audio_devices_t device) {
    pa_assert(s);

    if (s->output)
        return droid_output_stream_set_route(s, device);
    else
        return droid_input_stream_set_route(s, device);
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

    /* Even though earlier (< 3) HALs don't have input flags,
     * input flags don't have anything similar as output stream's
     * primary flag and we can just always reply false for
     * input streams. */
    if (s->output)
        return s->output->flags & AUDIO_OUTPUT_FLAG_PRIMARY;
    else
        return false;
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
                if (s->input->merged || pa_droid_quirk(s->module, QUIRK_CLOSE_INPUT)) {
                    s->input->stream->common.standby(&s->input->stream->common);
                    input_stream_close(s);
                } else
                    return s->input->stream->common.standby(&s->input->stream->common);
            }
        } else if (s->input->merged || pa_droid_quirk(s->module, QUIRK_CLOSE_INPUT))
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

pa_hook *pa_droid_hooks(pa_droid_hw_module *hw) {
    pa_assert(hw);
    pa_assert(PA_REFCNT_VALUE(hw) >= 1);

    return hw->hooks;
}
