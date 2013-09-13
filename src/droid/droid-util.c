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

#include <hardware/audio.h>
#include <hardware_legacy/audio_policy_conf.h>

#include "droid-util.h"

#include <android-version.h>

#ifndef ANDROID_VERSION_MAJOR
#error "ANDROID_VERSION_* not defined."
#endif

#if ANDROID_VERSION_MAJOR == 4 && ANDROID_VERSION_MINOR == 1
#include "droid-util-41qc.h"
#elif ANDROID_VERSION_MAJOR == 4 && ANDROID_VERSION_MINOR == 2
#include "droid-util-42.h"
#else
#error "No valid ANDROID_VERSION found."
#endif

#define CONVERT_FUNC(TABL) \
pa_bool_t pa_convert_ ## TABL (uint32_t value, pa_conversion_field_t field, uint32_t *to_value) {               \
    for (unsigned int i = 0; i < sizeof( conversion_table_ ## TABL )/(sizeof(uint32_t)*2); i++) {               \
        if ( conversion_table_ ## TABL [i][field] == value) {                                                   \
            *to_value = conversion_table_ ## TABL [i][!field];                                                  \
            return TRUE;                                                                                        \
        }                                                                                                       \
    }                                                                                                           \
    return FALSE;                                                                                               \
} struct __funny_extra_to_allow_semicolon

/* Creates convert_format convert_channel etc.
 * pa_bool_t pa_convert_func(uint32_t value, pa_conversion_field_t field, uint32_t *to_value);
 * return TRUE if conversion succesful */
CONVERT_FUNC(format);
CONVERT_FUNC(output_channel);
CONVERT_FUNC(input_channel);


static pa_bool_t string_convert_num_to_str(const struct string_conversion *list, const uint32_t value, const char **to_str) {
    pa_assert(list);
    pa_assert(to_str);

    for (unsigned int i = 0; list[i].str; i++) {
        if (list[i].value == value) {
            *to_str = list[i].str;
            return TRUE;
        }
    }
    return FALSE;
}

static pa_bool_t string_convert_str_to_num(const struct string_conversion *list, const char *str, uint32_t *to_value) {
    pa_assert(list);
    pa_assert(str);
    pa_assert(to_value);

    for (unsigned int i = 0; list[i].str; i++) {
        if (pa_streq(list[i].str, str)) {
            *to_value = list[i].value;
            return TRUE;
        }
    }
    return FALSE;
}

static char *list_string(struct string_conversion *list, uint32_t flags) {
    char *str = NULL;
    char *tmp;

#ifdef HAL_V2
    if (flags & AUDIO_DEVICE_BIT_IN)
        flags &= ~AUDIO_DEVICE_BIT_IN;
#endif

    for (unsigned int i = 0; list[i].str; i++) {
#ifdef HAL_V2
        if (list[i].value & AUDIO_DEVICE_BIT_IN) {
            if (popcount(list[i].value & ~AUDIO_DEVICE_BIT_IN) != 1)
                continue;
        } else
#endif
        if (popcount(list[i].value) != 1)
            continue;

        if (flags & list[i].value) {
            if (str) {
                tmp = pa_sprintf_malloc("%s|%s", str, list[i].str);
                pa_xfree(str);
                str = tmp;
            } else {
                str = pa_sprintf_malloc("%s", list[i].str);
            }
        }
    }

    return str;
}


/* Output device */
pa_bool_t pa_string_convert_output_device_num_to_str(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_output_device, (uint32_t) value, to_str);
}

pa_bool_t pa_string_convert_output_device_str_to_num(const char *str, audio_devices_t *to_value) {
    return string_convert_str_to_num(string_conversion_table_output_device, str, (uint32_t*) to_value);
}

char *pa_list_string_output_device(audio_devices_t devices) {
    return list_string(string_conversion_table_output_device, devices);
}

/* Input device */
pa_bool_t pa_string_convert_input_device_num_to_str(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_input_device, (uint32_t) value, to_str);
}

pa_bool_t pa_string_convert_input_device_str_to_num(const char *str, audio_devices_t *to_value) {
    return string_convert_str_to_num(string_conversion_table_input_device, str, (uint32_t*) to_value);
}

char *pa_list_string_input_device(audio_devices_t devices) {
    return list_string(string_conversion_table_input_device, devices);
}

/* Flags */
pa_bool_t pa_string_convert_flag_num_to_str(audio_output_flags_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_flag, (uint32_t) value, to_str);
}

pa_bool_t pa_string_convert_flag_str_to_num(const char *str, audio_output_flags_t *to_value) {
    return string_convert_str_to_num(string_conversion_table_flag, str, (uint32_t*) to_value);
}

char *pa_list_string_flags(audio_output_flags_t flags) {
    return list_string(string_conversion_table_flag, flags);
}

/* Config parser */

#define WHITESPACE "\n\r \t"

static int parse_list(const struct string_conversion *table, const char *str, uint32_t *dst) {
    int count = 0;
    char *entry;
    const char *state = NULL;

    pa_assert(table);
    pa_assert(str);
    pa_assert(dst);

    *dst = 0;

    while ((entry = pa_split(str, "|", &state))) {
        uint32_t d = 0;

        if (!string_convert_str_to_num(table, entry, &d)) {
            pa_log("Unknown entry %s", entry);
            pa_xfree(entry);
            return -1;
        }

        *dst |= d;
        count++;

        pa_xfree(entry);
    }

    return count;
}

static pa_bool_t parse_sampling_rates(const char *str, uint32_t sampling_rates[32]) {
    pa_assert(str);

    char *entry;
    const char *state = NULL;

    uint32_t pos = 0;
    while ((entry = pa_split(str, "|", &state))) {
        int32_t val;

        if (pos == AUDIO_MAX_SAMPLING_RATES) {
            pa_log("Too many sample rate entries (> %d)", AUDIO_MAX_SAMPLING_RATES);
            pa_xfree(entry);
            return FALSE;
        }

        if (pa_atoi(entry, &val) < 0) {
            pa_log("Bad sample rate value %s", entry);
            pa_xfree(entry);
            return FALSE;
        }

        sampling_rates[pos++] = val;

        pa_xfree(entry);

    }

    sampling_rates[pos] = 0;

    return TRUE;
}

static pa_bool_t parse_formats(const char *str, audio_format_t *formats) {
    pa_assert(str);
    pa_assert(formats);

    return parse_list(string_conversion_table_format, str, formats) > 0;
}

static int parse_channels(const char *str, pa_bool_t in_output, audio_channel_mask_t *channels) {
    pa_assert(str);
    pa_assert(channels);

    /* Needs to be probed later */
    if (pa_streq(str, "dynamic")) {
        *channels = 0;
        return TRUE;
    }

    if (in_output)
        return parse_list(string_conversion_table_output_channels, str, channels);
    else
        return parse_list(string_conversion_table_input_channels, str, channels);
}

static pa_bool_t parse_devices(const char *str, pa_bool_t in_output, audio_devices_t *devices) {
    pa_assert(str);
    pa_assert(devices);

    if (in_output)
        return parse_list(string_conversion_table_output_device, str, devices) > 0;
    else
        return parse_list(string_conversion_table_input_device, str, devices) > 0;
}

static pa_bool_t parse_flags(const char *str, audio_output_flags_t *flags) {
    pa_assert(str);
    pa_assert(flags);

    return parse_list(string_conversion_table_flag, str, flags) > 0;
}

pa_bool_t pa_parse_droid_audio_config(const char *filename, pa_droid_config_audio *config) {
    FILE *f;
    int n = 0;
    pa_bool_t ret = TRUE;

    enum config_loc {
        IN_ROOT = 0,
        IN_GLOBAL = 1,
        IN_HW_MODULES = 1,
        IN_MODULE = 2,
        IN_OUTPUT_INPUT = 3,
        IN_CONFIG = 4
    } loc = IN_ROOT;


    pa_bool_t in_global = FALSE;
    pa_bool_t in_output = TRUE;

    pa_droid_config_hw_module *module = NULL;
    pa_droid_config_output *output = NULL;
    pa_droid_config_input *input = NULL;

    pa_assert(filename);
    pa_assert(config);

    memset(config, 0, sizeof(pa_droid_config_audio));

    f = fopen(filename, "r");

    if (!f) {
        pa_log_info("Failed to open config file (%s): %s", filename, pa_cstrerror(errno));
        ret = FALSE;
        goto finish;
    }

    pa_lock_fd(fileno(f), 1);

    while (!feof(f)) {
        char ln[512];
        char *d, *v, *val;

        if (!fgets(ln, sizeof(ln), f))
            break;

        n++;

        pa_strip_nl(ln);

        if (ln[0] == '#' || !*ln )
            continue;

        /* Enter section */
        if (ln[strlen(ln)-1] == '{') {
            d = ln+strspn(ln, WHITESPACE);
            v = d;
            d = v+strcspn(v, WHITESPACE);
            d[0] = '\0';

            if (!*v) {
                pa_log(__FILE__ ": [%s:%u] failed to parse line - too few words", filename, n);
                goto finish;
            }

            switch (loc) {
                case IN_ROOT:
                    if (pa_streq(v, GLOBAL_CONFIG_TAG)) {
                        in_global = TRUE;
                        loc = IN_GLOBAL;
                    }
                    else if (pa_streq(v, AUDIO_HW_MODULE_TAG))
                        loc = IN_HW_MODULES;
                    else {
                        pa_log(__FILE__ ": [%s:%u] failed to parse line - unknown field (%s)", filename, n, v);
                        ret = FALSE;
                        goto finish;
                    }
                    break;

                case IN_HW_MODULES:
                    module = &config->hw_modules[config->hw_modules_size];
                    config->hw_modules_size++;
                    strncpy(module->name, v, AUDIO_HARDWARE_MODULE_ID_MAX_LEN);
                    module->config = config;
                    loc = IN_MODULE;
                    pa_log_debug("config: New module: %s", module->name);
                    break;

                case IN_MODULE:
                    if (pa_streq(v, OUTPUTS_TAG)) {
                        loc = IN_OUTPUT_INPUT;
                        in_output = TRUE;
                    } else if (pa_streq(v, INPUTS_TAG)) {
                        loc = IN_OUTPUT_INPUT;
                        in_output = FALSE;
                    } else {
                        pa_log(__FILE__ ": [%s:%u] failed to parse line - unknown field (%s)", filename, n, v);
                        ret = FALSE;
                        goto finish;
                    }
                    break;

                case IN_OUTPUT_INPUT:
                    pa_assert(module);

                    if (in_output) {
                        output = &module->outputs[module->outputs_size];
                        module->outputs_size++;
                        strncpy(output->name, v, AUDIO_HARDWARE_MODULE_ID_MAX_LEN);
                        output->module = module;
                        loc = IN_CONFIG;
                        pa_log_debug("config: %s: New output: %s", module->name, output->name);
                    } else {
                        input = &module->inputs[module->inputs_size];
                        module->inputs_size++;
                        strncpy(input->name, v, AUDIO_HARDWARE_MODULE_ID_MAX_LEN);
                        input->module = module;
                        loc = IN_CONFIG;
                        pa_log_debug("config: %s: New input: %s", module->name, input->name);
                    }
                    break;

                case IN_CONFIG:
                    pa_log(__FILE__ ": [%s:%u] failed to parse line - unknown field in config (%s)", filename, n, v);
                    ret = FALSE;
                    goto finish;
            }

            continue;
        }

        /* Exit section */
        if (ln[strlen(ln)-1] == '}') {
            if (loc == IN_ROOT) {
                pa_log(__FILE__ ": [%s:%u] failed to parse line - extra closing bracket", filename, n);
                ret = FALSE;
                goto finish;
            }

            loc--;
            if (loc == IN_MODULE) {
                if (in_output)
                    output = NULL;
                else
                    input = NULL;
            }
            if (loc == IN_ROOT)
                module = NULL;

            in_global = FALSE;

            continue;
        }

        /* Parse global configuration */
        if (in_global) {
            pa_bool_t success = FALSE;

            d = ln+strspn(ln, WHITESPACE);
            v = d;
            d = v+strcspn(v, WHITESPACE);

            val = d+strspn(d, WHITESPACE);
            d[0] = '\0';
            d = val+strcspn(val, WHITESPACE);
            d[0] = '\0';

            if (pa_streq(v, ATTACHED_OUTPUT_DEVICES_TAG))
                success = parse_devices(val, TRUE, &config->global_config.attached_output_devices);
            else if (pa_streq(v, DEFAULT_OUTPUT_DEVICE_TAG))
                success = parse_devices(val, TRUE, &config->global_config.default_output_device);
            else if (pa_streq(v, ATTACHED_INPUT_DEVICES_TAG))
                success = parse_devices(val, FALSE, &config->global_config.attached_input_devices);
            else {
                pa_log(__FILE__ ": [%s:%u] failed to parse line - unknown config entry %s", filename, n, v);
                success = FALSE;
            }

            if (!success) {
                ret = FALSE;
                goto finish;
            }
        }

        /* Parse per-output or per-input configuration */
        if (loc == IN_CONFIG) {
            pa_bool_t success = FALSE;

            pa_assert(module);

            d = ln+strspn(ln, WHITESPACE);
            v = d;
            d = v+strcspn(v, WHITESPACE);

            val = d+strspn(d, WHITESPACE);
            d[0] = '\0';
            d = val+strcspn(val, WHITESPACE);
            d[0] = '\0';


            if ((in_output && !output) || (!in_output && !input)) {
                pa_log(__FILE__ ": [%s:%u] failed to parse line", filename, n);
                ret = FALSE;
                goto finish;
            }

            if (pa_streq(v, SAMPLING_RATES_TAG))
                success = parse_sampling_rates(val, in_output ? output->sampling_rates : input->sampling_rates);
            else if (pa_streq(v, FORMATS_TAG))
                success = parse_formats(val, in_output ? &output->formats : &input->formats);
            else if (pa_streq(v, CHANNELS_TAG)) {
                if (in_output)
                    success = (parse_channels(val, TRUE, &output->channel_masks) > 0);
                else
                    success = (parse_channels(val, FALSE, &input->channel_masks) > 0);
            } else if (pa_streq(v, DEVICES_TAG)) {
                if (in_output)
                    success = parse_devices(val, TRUE, &output->devices);
                else
                    success = parse_devices(val, FALSE, &input->devices);
            } else if (pa_streq(v, FLAGS_TAG)) {
                if (in_output)
                    success = parse_flags(val, &output->flags);
                else {
                    pa_log(__FILE__ ": [%s:%u] failed to parse line - output flags inside input definition", filename, n);
                    success = FALSE;
                }
            } else {
                pa_log(__FILE__ ": [%s:%u] failed to parse line - unknown config entry %s", filename, n, v);
                success = FALSE;
            }

            if (!success) {
                ret = FALSE;
                goto finish;
            }
        }
    }

    pa_log_info("Parsed config file (%s): %u modules.", filename, config->hw_modules_size);

finish:
    if (f) {
        pa_lock_fd(fileno(f), 0);
        fclose(f);
    }

    return ret;
}


const pa_droid_config_output *pa_droid_config_find_output(const pa_droid_config_hw_module *module, const char *name) {
    pa_assert(module);
    pa_assert(name);

    for (unsigned i = 0; i < module->outputs_size; i++) {
        if (pa_streq(name, module->outputs[i].name))
            return &module->outputs[i];
    }

    return NULL;
}

const pa_droid_config_input *pa_droid_config_find_input(const pa_droid_config_hw_module *module, const char *name) {
    pa_assert(module);
    pa_assert(name);

    for (unsigned i = 0; i < module->inputs_size; i++) {
        if (pa_streq(name, module->inputs[i].name))
            return &module->inputs[i];
    }

    return NULL;
}

const pa_droid_config_hw_module *pa_droid_config_find_module(const pa_droid_config_audio *config, const char* module_id) {
    pa_assert(config);
    pa_assert(module_id);

    for (unsigned i = 0; i < config->hw_modules_size; i++) {
        if (pa_streq(module_id, config->hw_modules[i].name))
            return &config->hw_modules[i];
    }

    return NULL;
}

pa_droid_profile *pa_droid_profile_new(pa_droid_profile_set *ps, const pa_droid_config_output *output, const pa_droid_config_input *input) {
    pa_droid_profile *p;

    pa_assert(ps);
    pa_assert(output);

    p = pa_xnew0(pa_droid_profile, 1);
    p->profile_set = ps;
    p->module = output->module;
    p->name = pa_sprintf_malloc("%s%s%s", output->name, input ? "-" : "", input ? input->name : "");
    p->description = pa_sprintf_malloc("%s output%s%s%s", output->name,
                                                          input ? " and " : "",
                                                          input ? input->name : "",
                                                          input ? " input." : "");
    p->priority = 100;
    if (pa_streq(output->name, "primary")) {
        p->priority += 100;

        if (input && pa_streq(input->name, "primary"))
            p->priority += 100;
    }

    if (output)
        p->output = pa_droid_mapping_get(ps, PA_DIRECTION_OUTPUT, output);
    if (input)
        p->input = pa_droid_mapping_get(ps, PA_DIRECTION_INPUT, input);

    pa_hashmap_put(ps->profiles, p->name, p);

    return p;
}

static void add_profile(pa_droid_profile_set *ps, const pa_droid_config_output *output, const pa_droid_config_input *input) {
    pa_droid_profile *ap;

    pa_log_debug("New profile: %s-%s", output->name, input ? input->name : "no input");

    ap = pa_droid_profile_new(ps, output, input);

    pa_hashmap_put(ps->profiles, ap->name, ap);
}

pa_droid_profile_set *pa_droid_profile_set_new(const pa_droid_config_hw_module *module) {
    pa_droid_profile_set *ps;

    pa_assert(module);

    ps = pa_xnew0(pa_droid_profile_set, 1);
    ps->config = module->config;
    ps->profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    ps->output_mappings = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    ps->input_mappings = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    ps->all_ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    /* Each distinct hw module output matches one profile. If there are multiple inputs
     * combinations are made so that all possible outputs and inputs can be selected.
     * So for outputs "primary" and "hdmi" and input "primary" profiles
     * "primary-primary" and "hdmi-primary" are created. */

    for (unsigned o = 0; o < module->outputs_size; o++) {

        if (module->inputs_size > 0) {
            for (unsigned i = 0; i < module->inputs_size; i++) {
                add_profile(ps, &module->outputs[o], &module->inputs[i]);
            }
        } else
            add_profile(ps, &module->outputs[o], NULL);
    }

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

    if (ps->output_mappings) {
        pa_droid_mapping *am;

        pa_hashmap_free(ps->output_mappings, (pa_free_cb_t) pa_droid_mapping_free);
    }

    if (ps->input_mappings) {
        pa_droid_mapping *am;

        pa_hashmap_free(ps->input_mappings, (pa_free_cb_t) pa_droid_mapping_free);
    }

    if (ps->all_ports) {
        pa_droid_port *p;

        pa_hashmap_free(ps->all_ports, (pa_free_cb_t) droid_port_free);
    }

    if (ps->profiles) {
        pa_droid_profile *p;

        pa_hashmap_free(ps->profiles, (pa_free_cb_t) pa_droid_profile_free);
    }

    pa_xfree(ps);
}

static void add_o_ports(pa_droid_mapping *am) {
    pa_droid_port *p;
    const char *name;
    char *desc;
    uint32_t devices;
    uint32_t i = 0;

    pa_assert(am);

    devices = am->output->devices;

    while (devices) {
        uint32_t cur_device = (1 << i++);

        if (devices & cur_device) {

            pa_droid_output_port_name(cur_device, &name);

            if (!(p = pa_hashmap_get(am->profile_set->all_ports, name))) {
                pa_log_debug("  New output port %s", name);
                p = pa_xnew0(pa_droid_port, 1);

                p->mapping = am;
                p->name = pa_xstrdup(name);
                desc = pa_replace(name, "output-", "Output to ");
                p->description = pa_replace(desc, "_", " ");
                pa_xfree(desc);
                p->priority = 100;
                p->device = cur_device;

                if (am->profile_set->config->global_config.attached_output_devices & cur_device)
                    p->priority += 100;

                if (am->profile_set->config->global_config.default_output_device & cur_device)
                    p->priority += 100;

                pa_hashmap_put(am->profile_set->all_ports, p->name, p);
            } else
                pa_log_debug("  Output port %s from cache", name);

            pa_idxset_put(am->ports, p, NULL);

            devices &= ~cur_device;
        }
    }
}

static void add_i_ports(pa_droid_mapping *am) {
    pa_droid_port *p;
    const char *name;
    char *desc;
    uint32_t devices;
    uint32_t i = 0;

    pa_assert(am);

    devices = am->input->devices;
#ifdef HAL_V2
    devices &= ~AUDIO_DEVICE_BIT_IN;
#endif

    while (devices) {
        uint32_t cur_device = (1 << i++);

        if (devices & cur_device) {

            pa_droid_input_port_name(cur_device, &name);

            if (!(p = pa_hashmap_get(am->profile_set->all_ports, name))) {
                pa_log_debug("  New input port %s", name);
                p = pa_xnew0(pa_droid_port, 1);

                p->mapping = am;
                p->name = pa_xstrdup(name);
                desc = pa_replace(name, "input-", "Input from ");
                p->description = pa_replace(desc, "_", " ");
                pa_xfree(desc);
                p->priority = 100;
                p->device = cur_device;

                if (am->profile_set->config->global_config.attached_input_devices & cur_device)
                    p->priority += 100;

                pa_hashmap_put(am->profile_set->all_ports, p->name, p);
            } else
                pa_log_debug("  Input port %s from cache", name);

            pa_idxset_put(am->ports, p, NULL);

            devices &= ~cur_device;
        }
    }
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

pa_bool_t pa_droid_output_port_name(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_output_device_fancy, (uint32_t) value, to_str);
}

pa_bool_t pa_droid_input_port_name(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_input_device_fancy, (uint32_t) value, to_str);
}

static int add_ports(pa_core *core, pa_card_profile *cp, pa_hashmap *ports, pa_droid_mapping *am, pa_hashmap *extra) {
    pa_droid_port *p;
    pa_device_port *dp;
    pa_droid_port_data *data;
    uint32_t idx;
    int count = 0;

    pa_log_debug("Ports for %s%s: %s", cp ? "card " : "", am->direction == PA_DIRECTION_OUTPUT ? "output" : "input", am->name);

    PA_IDXSET_FOREACH(p, am->ports, idx) {
        if (!(dp = pa_hashmap_get(ports, p->name))) {
            pa_log_debug("  New port %s", p->name);
            dp = pa_device_port_new(core, p->name, p->description, sizeof(pa_droid_port_data));
            dp->priority = p->priority;

            pa_hashmap_put(ports, dp->name, dp);
            dp->profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

            data = PA_DEVICE_PORT_DATA(dp);
            data->device = p->device;
        } else
            pa_log_debug("  Port %s from cache", p->name);

        dp->is_output = p->mapping->direction == PA_DIRECTION_OUTPUT;
        dp->is_input = p->mapping->direction == PA_DIRECTION_INPUT;

        if (cp)
            pa_hashmap_put(dp->profiles, cp->name, cp);

        count++;

        if (extra) {
            pa_hashmap_put(extra, dp->name, dp);
            pa_device_port_ref(dp);
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

static char *shared_name_get(const char *module_id) {
    pa_assert(module_id);
    return pa_sprintf_malloc("droid-hardware-module-%s", module_id);
}

static pa_droid_hw_module *droid_hw_module_open(pa_core *core, pa_droid_config_audio *config, const char *module_id) {
    const pa_droid_config_hw_module *module;
    pa_droid_hw_module *hw = NULL;
    struct hw_module_t *hwmod = NULL;
    audio_hw_device_t *device = NULL;
    char *shared_name;
    int ret;

    pa_assert(core);
    pa_assert(module_id);

    if (!config) {
        pa_log("No configuration provided for opening module with id %s", module_id);
        goto fail;
    }

    if (!(module = pa_droid_config_find_module(config, module_id))) {
        pa_log("Couldn't find module with id %s", module_id);
        goto fail;
    }

    hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, module->name, (const hw_module_t**) &hwmod);
    if (!hwmod) {
        pa_log("Failed to get hw module %s.", module->name);
        goto fail;
    }

    pa_log_info("Loaded hw module %s", module->name);

    ret = audio_hw_device_open(hwmod, &device);
    if (!device) {
        pa_log("Failed to open device (errno %d).", ret);
        goto fail;
    }

    if ((ret = device->init_check(device)) != 0) {
        pa_log("Failed init_check() (errno %d)", ret);
        goto fail;
    }

    hw = pa_xnew0(pa_droid_hw_module, 1);
    PA_REFCNT_INIT(hw);
    hw->core = core;
    hw->hwmod = hwmod;
    hw->hw_mutex = pa_mutex_new(TRUE, FALSE);
    hw->device = device;
    hw->config = pa_xnew(pa_droid_config_audio, 1);
    memcpy(hw->config, config, sizeof(pa_droid_config_audio));
    hw->enabled_module = pa_droid_config_find_module(hw->config, module_id);
    hw->module_id = hw->enabled_module->name;

    shared_name = shared_name_get(hw->module_id);
    pa_assert_se(pa_shared_set(core, shared_name, hw) >= 0);

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
    pa_assert(hw);

    pa_log_info("Closing hw module %s", hw->enabled_module->name);

    if (hw->config)
        pa_xfree(hw->config);

    if (hw->device)
        audio_hw_device_close(hw->device);

    if (hw->hw_mutex)
        pa_mutex_free(hw->hw_mutex);

    pa_xfree(hw);
}

void pa_droid_hw_module_unref(pa_droid_hw_module *hw) {
    char *shared_name;

    pa_assert(hw);
    pa_assert(PA_REFCNT_VALUE(hw) >= 1);

    if (PA_REFCNT_DEC(hw) > 0)
        return;

    shared_name = shared_name_get(hw->module_id);
    pa_assert_se(pa_shared_remove(hw->core, shared_name) >= 0);
    pa_xfree(shared_name);
    droid_hw_module_close(hw);
}

pa_droid_config_audio *pa_droid_config_load(pa_modargs *ma) {
    pa_droid_config_audio *config;
    const char *config_location;

    pa_assert(ma);

    config = pa_xnew0(pa_droid_config_audio, 1);

    if ((config_location = pa_modargs_get_value(ma, "config", NULL))) {
        if (!pa_parse_droid_audio_config(config_location, config)) {
            pa_log("Failed to parse configuration from %s", config_location);
            goto fail;
        }
    } else {
        config_location = AUDIO_POLICY_VENDOR_CONFIG_FILE;

        if (!pa_parse_droid_audio_config(config_location, config)) {
            pa_log_debug("Failed to parse configuration from vendor %s", config_location);

            config_location = AUDIO_POLICY_CONFIG_FILE;

            if (!pa_parse_droid_audio_config(config_location, config)) {
                pa_log("Failed to parse configuration from %s", config_location);
                goto fail;
            }
        }
    }

    return config;

fail:
    pa_xfree(config);
    return NULL;
}

void pa_droid_hw_module_lock(pa_droid_hw_module *hw) {
    pa_assert(hw);

    pa_mutex_lock(hw->hw_mutex);
}

pa_bool_t pa_droid_hw_module_try_lock(pa_droid_hw_module *hw) {
    pa_assert(hw);

    return pa_mutex_try_lock(hw->hw_mutex);
}

void pa_droid_hw_module_unlock(pa_droid_hw_module *hw) {
    pa_assert(hw);

    pa_mutex_unlock(hw->hw_mutex);
}
