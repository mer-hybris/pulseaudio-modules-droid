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

#include "droid-util.h"

#define SLLIST_APPEND(t, head, item)                                \
    do {                                                            \
        item->next = NULL;                                          \
        if (!head) {                                                \
            head = item;                                            \
        } else {                                                    \
            t *_list;                                               \
            for (_list = head; _list->next; _list = _list->next);   \
            _list->next = item;                                     \
        }                                                           \
    } while (0)

#define SLLIST_FOREACH(i, head)                                     \
    for (i = (head); i; i = i->next)

#define SLLIST_STEAL_FIRST(i, head)                                 \
    do {                                                            \
        if (head) {                                                 \
            i = head;                                               \
            head = head->next;                                      \
        } else                                                      \
            i = NULL;                                               \
    } while (0)


#define CONVERT_FUNC(TABL) \
bool pa_convert_ ## TABL (uint32_t value, pa_conversion_field_t field, uint32_t *to_value) {                    \
    for (unsigned int i = 0; i < sizeof( conversion_table_ ## TABL )/(sizeof(uint32_t)*2); i++) {               \
        if ( conversion_table_ ## TABL [i][field] == value) {                                                   \
            *to_value = conversion_table_ ## TABL [i][!field];                                                  \
            return true;                                                                                        \
        }                                                                                                       \
    }                                                                                                           \
    return false;                                                                                               \
} struct __funny_extra_to_allow_semicolon

/* Creates convert_format convert_channel etc.
 * bool pa_convert_func(uint32_t value, pa_conversion_field_t field, uint32_t *to_value);
 * return true if conversion succesful */
CONVERT_FUNC(format);
CONVERT_FUNC(output_channel);
CONVERT_FUNC(input_channel);

#define DEFAULT_PRIORITY (100)

/* Section defining custom global configuration variables. */
#define GLOBAL_CONFIG_EXT_TAG "custom_properties"

static const char * const droid_combined_auto_outputs[3]    = { "primary", "low_latency", NULL };
static const char * const droid_combined_auto_inputs[2]     = { "primary", NULL };

static void droid_config_free(pa_droid_config_audio *config);
static void droid_port_free(pa_droid_port *p);

static bool string_convert_num_to_str(const struct string_conversion *list, const uint32_t value, const char **to_str) {
    pa_assert(list);
    pa_assert(to_str);

    for (unsigned int i = 0; list[i].str; i++) {
        if (list[i].value == value) {
            *to_str = list[i].str;
            return true;
        }
    }
    return false;
}

static bool string_convert_str_to_num(const struct string_conversion *list, const char *str, uint32_t *to_value) {
    pa_assert(list);
    pa_assert(str);
    pa_assert(to_value);

    for (unsigned int i = 0; list[i].str; i++) {
        if (pa_streq(list[i].str, str)) {
            *to_value = list[i].value;
            return true;
        }
    }
    return false;
}

static char *list_string(struct string_conversion *list, uint32_t flags) {
    char *str = NULL;
    char *tmp;

#if AUDIO_API_VERSION_MAJ >= 2
    if (flags & AUDIO_DEVICE_BIT_IN)
        flags &= ~AUDIO_DEVICE_BIT_IN;
#endif

    for (unsigned int i = 0; list[i].str; i++) {
#if AUDIO_API_VERSION_MAJ >= 2
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
bool pa_string_convert_output_device_num_to_str(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_output_device, (uint32_t) value, to_str);
}

bool pa_string_convert_output_device_str_to_num(const char *str, audio_devices_t *to_value) {
    return string_convert_str_to_num(string_conversion_table_output_device, str, (uint32_t*) to_value);
}

char *pa_list_string_output_device(audio_devices_t devices) {
    return list_string(string_conversion_table_output_device, devices);
}

/* Input device */
bool pa_string_convert_input_device_num_to_str(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_input_device, (uint32_t) value, to_str);
}

bool pa_string_convert_input_device_str_to_num(const char *str, audio_devices_t *to_value) {
    return string_convert_str_to_num(string_conversion_table_input_device, str, (uint32_t*) to_value);
}

char *pa_list_string_input_device(audio_devices_t devices) {
    return list_string(string_conversion_table_input_device, devices);
}

/* Flags */
bool pa_string_convert_flag_num_to_str(audio_output_flags_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_output_flag, (uint32_t) value, to_str);
}

bool pa_string_convert_flag_str_to_num(const char *str, audio_output_flags_t *to_value) {
    return string_convert_str_to_num(string_conversion_table_output_flag, str, (uint32_t*) to_value);
}

char *pa_list_string_flags(audio_output_flags_t flags) {
    return list_string(string_conversion_table_output_flag, flags);
}

bool pa_input_device_default_audio_source(audio_devices_t input_device, audio_source_t *default_source)
{
#if AUDIO_API_VERSION_MAJ >= 2
    input_device &= ~AUDIO_DEVICE_BIT_IN;
#endif

    /* Note converting HAL values to different HAL values! */
    for (unsigned int i = 0; i < sizeof(conversion_table_default_audio_source) / (sizeof(uint32_t) * 2); i++) {
        if (conversion_table_default_audio_source[i][0] & input_device) {
            *default_source = conversion_table_default_audio_source[i][1];
            return true;
        }
    }
    return false;
}

/* Config parser */

#define WHITESPACE "\n\r \t"

static int parse_list(const struct string_conversion *table,
                      const char *str,
                      uint32_t *dst,
                      char **unknown_entries) {
    int count = 0;
    char *entry;
    char *unknown = NULL;
    const char *state = NULL;

    pa_assert(table);
    pa_assert(str);
    pa_assert(dst);
    pa_assert(unknown_entries);

    *dst = 0;
    *unknown_entries = NULL;

    while ((entry = pa_split(str, "|", &state))) {
        uint32_t d = 0;

        if (!string_convert_str_to_num(table, entry, &d)) {
            if (*unknown_entries) {
                unknown = pa_sprintf_malloc("%s|%s", *unknown_entries, entry);
                pa_xfree(*unknown_entries);
                pa_xfree(entry);
            } else
                unknown = entry;

            *unknown_entries = unknown;
            continue;
        }

        *dst |= d;
        count++;

        pa_xfree(entry);
    }

    return count;
}

static bool parse_sampling_rates(const char *fn, const unsigned ln,
                                 const char *str, uint32_t sampling_rates[32]) {
    pa_assert(fn);
    pa_assert(str);

    char *entry;
    const char *state = NULL;

    uint32_t pos = 0;
    while ((entry = pa_split(str, "|", &state))) {
        int32_t val;

#if AUDIO_API_VERSION_MAJ >= 3
        if (pos == 0 && pa_streq(entry, "dynamic")) {
            sampling_rates[pos++] = (uint32_t) -1;
            pa_xfree(entry);
            break;
        }
#endif

        if (pos == AUDIO_MAX_SAMPLING_RATES) {
            pa_log("[%s:%u] Too many sample rate entries (> %d)", fn, ln, AUDIO_MAX_SAMPLING_RATES);
            pa_xfree(entry);
            return false;
        }

        if (pa_atoi(entry, &val) < 0) {
            pa_log("[%s:%u] Bad sample rate value %s", fn, ln, entry);
            pa_xfree(entry);
            return false;
        }

        sampling_rates[pos++] = val;

        pa_xfree(entry);

    }

    sampling_rates[pos] = 0;

    return true;
}

static bool check_and_log(const char *fn, const unsigned ln, const char *field,
                          const int count, const char *str, char *unknown,
                          const bool must_have_all) {
    bool fail;

    pa_assert(fn);
    pa_assert(field);
    pa_assert(str);

    fail = must_have_all && unknown;

    if (unknown) {
        pa_log_info("[%s:%u] Unknown %s entries: %s", fn, ln, field, unknown);
        pa_xfree(unknown);
    }

    if (count == 0 || fail) {
        pa_log("[%s:%u] Failed to parse %s (%s).", fn, ln, field, str);
        return false;
    }

    return true;
}

static bool parse_formats(const char *fn, const unsigned ln,
                          const char *str, audio_format_t *formats) {
    int count;
    char *unknown = NULL;

    pa_assert(fn);
    pa_assert(str);
    pa_assert(formats);

#if AUDIO_API_VERSION_MAJ >= 3
    /* Needs to be probed later */
    if (pa_streq(str, "dynamic")) {
        *formats = 0;
        return true;
    }
#endif

    count = parse_list(string_conversion_table_format, str, formats, &unknown);

    return check_and_log(fn, ln, "formats", count, str, unknown, false);
}

static int parse_channels(const char *fn, const unsigned ln,
                          const char *str, bool in_output, audio_channel_mask_t *channels) {
    int count;
    char *unknown = NULL;

    pa_assert(fn);
    pa_assert(str);
    pa_assert(channels);

    /* Needs to be probed later */
    if (pa_streq(str, "dynamic")) {
        *channels = 0;
        return true;
    }

    count = parse_list(in_output ? string_conversion_table_output_channels
                                 : string_conversion_table_input_channels,
                                 str, channels, &unknown);

    return check_and_log(fn, ln, in_output ? "output channel_masks" : "input channel_masks",
                         count, str, unknown, false);
}

static bool parse_devices(const char *fn, const unsigned ln,
                          const char *str, bool in_output, audio_devices_t *devices, bool must_have_all) {
    int count;
    char *unknown = NULL;

    pa_assert(fn);
    pa_assert(str);
    pa_assert(devices);

    count = parse_list(in_output ? string_conversion_table_output_device
                                 : string_conversion_table_input_device,
                                 str, devices, &unknown);

    return check_and_log(fn, ln, in_output ? "output devices" : "input devices",
                         count, str, unknown, must_have_all);
}

static bool parse_output_flags(const char *fn, const unsigned ln,
                        const char *str, audio_output_flags_t *flags) {
    int count;
    char *unknown = NULL;

    pa_assert(fn);
    pa_assert(str);
    pa_assert(flags);

    count = parse_list(string_conversion_table_output_flag, str, flags, &unknown);

    return check_and_log(fn, ln, "flags", count, str, unknown, false);
}

#if AUDIO_API_VERSION_MAJ >= 3
static bool parse_input_flags(const char *fn, const unsigned ln,
                        const char *str, audio_input_flags_t *flags) {
    int count;
    char *unknown = NULL;

    pa_assert(fn);
    pa_assert(str);
    pa_assert(flags);

    count = parse_list(string_conversion_table_input_flag, str, flags, &unknown);

    return check_and_log(fn, ln, "flags", count, str, unknown, false);
}
#endif

#define MAX_LINE_LENGTH (1024)

bool pa_parse_droid_audio_config(const char *filename, pa_droid_config_audio *config) {
    FILE *f;
    unsigned n = 0;
    bool ret = true;
    char *full_line = NULL;
    uint32_t hw_module_count = 0;

    enum config_loc {
        IN_ROOT             = 0,
        IN_GLOBAL           = 1,
        IN_GLOBAL_EXT       = 2,
        IN_HW_MODULES       = 3,
        IN_MODULE           = 4,
        IN_OUTPUT_INPUT     = 5,
        IN_CONFIG           = 6
    } loc = IN_ROOT;

    bool in_output = true;

    pa_droid_config_hw_module *module = NULL;
    pa_droid_config_output *output = NULL;
    pa_droid_config_input *input = NULL;

    pa_assert(filename);
    pa_assert(config);

    memset(config, 0, sizeof(pa_droid_config_audio));

    f = fopen(filename, "r");

    if (!f) {
        pa_log_info("Failed to open config file (%s): %s", filename, pa_cstrerror(errno));
        ret = false;
        goto finish;
    }

    pa_lock_fd(fileno(f), 1);

    full_line = pa_xmalloc0(sizeof(char) * MAX_LINE_LENGTH);

    while (!feof(f)) {
        char *ln, *d, *v, *value;

        if (!fgets(full_line, MAX_LINE_LENGTH, f))
            break;

        n++;

        pa_strip_nl(full_line);

        if (!*full_line)
            continue;

        ln = full_line + strspn(full_line, WHITESPACE);

        if (ln[0] == '#')
            continue;

        v = ln;
        d = v + strcspn(v, WHITESPACE);

        value = d + strspn(d, WHITESPACE);
        d[0] = '\0';
        d = value + strcspn(value, WHITESPACE);
        d[0] = '\0';

        /* Enter section */
        if (pa_streq(value, "{")) {

            if (!*v) {
                pa_log("[%s:%u] failed to parse line - too few words", filename, n);
                goto finish;
            }

            switch (loc) {
                case IN_ROOT:
                    if (pa_streq(v, GLOBAL_CONFIG_TAG)) {
                        loc = IN_GLOBAL;
                    }
                    else if (pa_streq(v, AUDIO_HW_MODULE_TAG))
                        loc = IN_HW_MODULES;
                    else {
                        pa_log("[%s:%u] failed to parse line - unknown field (%s)", filename, n, v);
                        ret = false;
                        goto finish;
                    }
                    break;

                case IN_GLOBAL:
                    if (pa_streq(v, GLOBAL_CONFIG_EXT_TAG))
                        loc = IN_GLOBAL_EXT;
                    else {
                        pa_log("[%s:%u] failed to parse line - unknown section (%s)", filename, n, v);
                        ret = false;
                        goto finish;
                    }
                    break;

                case IN_HW_MODULES:
                    module = pa_xnew0(pa_droid_config_hw_module, 1);
                    SLLIST_APPEND(pa_droid_config_hw_module, config->hw_modules, module);
                    hw_module_count++;
                    module->name = pa_xstrndup(v, AUDIO_HARDWARE_MODULE_ID_MAX_LEN);
                    module->config = config;
                    loc = IN_MODULE;
                    pa_log_debug("config: New module: %s", module->name);
                    break;

                case IN_MODULE:
                    if (pa_streq(v, OUTPUTS_TAG)) {
                        loc = IN_OUTPUT_INPUT;
                        in_output = true;
                    } else if (pa_streq(v, INPUTS_TAG)) {
                        loc = IN_OUTPUT_INPUT;
                        in_output = false;
                    } else {
                        pa_log("[%s:%u] failed to parse line - unknown field (%s)", filename, n, v);
                        ret = false;
                        goto finish;
                    }
                    break;

                case IN_OUTPUT_INPUT:
                    pa_assert(module);

                    if (in_output) {
                        output = pa_xnew0(pa_droid_config_output, 1);
                        SLLIST_APPEND(pa_droid_config_output, module->outputs, output);
                        output->name = pa_xstrndup(v, AUDIO_HARDWARE_MODULE_ID_MAX_LEN);
                        output->module = module;
                        loc = IN_CONFIG;
                        pa_log_debug("config: %s: New output: %s", module->name, output->name);
                    } else {
                        input = pa_xnew0(pa_droid_config_input, 1);
                        SLLIST_APPEND(pa_droid_config_input, module->inputs, input);
                        input->name = pa_xstrndup(v, AUDIO_HARDWARE_MODULE_ID_MAX_LEN);
                        input->module = module;
                        loc = IN_CONFIG;
                        pa_log_debug("config: %s: New input: %s", module->name, input->name);
                    }
                    break;

                case IN_CONFIG:
                    pa_log("[%s:%u] failed to parse line - unknown field in config (%s)", filename, n, v);
                    ret = false;
                    goto finish;

                default:
                    pa_log("[%s:%u] failed to parse line - unknown section (%s)", filename, n, v);
                    ret = false;
                    goto finish;
            }

            continue;
        }

        /* Exit section */
        if (pa_streq(v, "}")) {
            switch (loc) {
                case IN_ROOT:
                    pa_log("[%s:%u] failed to parse line - extra closing bracket", filename, n);
                    ret = false;
                    goto finish;

                case IN_HW_MODULES:
                    module = NULL;
                    /* fall through */
                case IN_GLOBAL:
                    loc = IN_ROOT;
                    break;

                case IN_OUTPUT_INPUT:
                    if (in_output)
                        output = NULL;
                    else
                        input = NULL;
                    /* fall through */
                case IN_MODULE:
                    /* fall through */
                case IN_CONFIG:
                    /* fall through */
                case IN_GLOBAL_EXT:
                    loc--;
                    break;
            }

            continue;
        }

        if (loc == IN_GLOBAL ||
            loc == IN_GLOBAL_EXT ||
            loc == IN_CONFIG) {

            bool success = false;

            if (loc == IN_GLOBAL) {

                /* Parse global configuration */

                if (pa_streq(v, ATTACHED_OUTPUT_DEVICES_TAG))
                    success = parse_devices(filename, n, value, true,
                                            &config->global_config.attached_output_devices, false);
                else if (pa_streq(v, DEFAULT_OUTPUT_DEVICE_TAG))
                    success = parse_devices(filename, n, value, true,
                                            &config->global_config.default_output_device, true);
                else if (pa_streq(v, ATTACHED_INPUT_DEVICES_TAG))
                    success = parse_devices(filename, n, value, false,
                                            &config->global_config.attached_input_devices, false);
#ifdef DROID_HAVE_DRC
                // SPEAKER_DRC_ENABLED_TAG is only from Android v4.4
                else if (pa_streq(v, SPEAKER_DRC_ENABLED_TAG))
                    /* TODO - Add support for dynamic range control */
                    success = true; /* Do not fail while parsing speaker_drc_enabled entry */
#endif
                else {
                    pa_log("[%s:%u] failed to parse line - unknown config entry %s", filename, n, v);
                    success = false;
                }

            } else if (loc == IN_GLOBAL_EXT) {

                /* Parse custom global configuration
                 * For now just log all custom variables, don't do
                 * anything with the values.
                 * TODO: Store custom values somehow */

                pa_log_debug("[%s:%u] TODO custom variable: %s = %s", filename, n, v, value);
                success = true;

            } else if (loc == IN_CONFIG) {

                /* Parse per-output or per-input configuration */

                if ((in_output && !output) || (!in_output && !input)) {
                    pa_log("[%s:%u] failed to parse line", filename, n);
                    ret = false;
                    goto finish;
                }

                if (pa_streq(v, SAMPLING_RATES_TAG))
                    success = parse_sampling_rates(filename, n, value,
                                                   in_output ? output->sampling_rates : input->sampling_rates);
                else if (pa_streq(v, FORMATS_TAG))
                    success = parse_formats(filename, n, value, in_output ? &output->formats : &input->formats);
                else if (pa_streq(v, CHANNELS_TAG)) {
                    if (in_output)
                        success = parse_channels(filename, n, value, true, &output->channel_masks);
                    else
                        success = parse_channels(filename, n, value, false, &input->channel_masks);
                } else if (pa_streq(v, DEVICES_TAG)) {
                    if (in_output)
                        success = parse_devices(filename, n, value, true, &output->devices, false);
                    else
                        success = parse_devices(filename, n, value, false, &input->devices, false);
                } else if (pa_streq(v, FLAGS_TAG)) {
                    if (in_output)
                        success = parse_output_flags(filename, n, value, &output->flags);
                    else {
#if AUDIO_API_VERSION_MAJ >= 3
                        success = parse_input_flags(filename, n, value, &input->flags);
#else
                        pa_log("[%s:%u] failed to parse line - output flags inside input definition", filename, n);
                        success = false;
#endif
                    }
                } else {
                    pa_log("[%s:%u] failed to parse line - unknown config entry %s", filename, n, v);
                    success = false;
                }

            } else
                pa_assert_not_reached();

            if (!success) {
                ret = false;
                goto finish;
            }
        }
    }

    pa_log_info("Parsed config file (%s): %u modules.", filename, hw_module_count);

finish:
    if (f) {
        pa_lock_fd(fileno(f), 0);
        fclose(f);
    }

    pa_xfree(full_line);

    return ret;
}


const pa_droid_config_output *pa_droid_config_find_output(const pa_droid_config_hw_module *module, const char *name) {
    pa_droid_config_output *output;

    pa_assert(module);
    pa_assert(name);

    SLLIST_FOREACH(output, module->outputs) {
        if (pa_streq(name, output->name))
            return output;
    }

    return NULL;
}

const pa_droid_config_input *pa_droid_config_find_input(const pa_droid_config_hw_module *module, const char *name) {
    pa_droid_config_input *input;

    pa_assert(module);
    pa_assert(name);

    SLLIST_FOREACH(input, module->inputs) {
        if (pa_streq(name, input->name))
            return input;
    }

    return NULL;
}

const pa_droid_config_hw_module *pa_droid_config_find_module(const pa_droid_config_audio *config, const char* module_id) {
    pa_droid_config_hw_module *module;

    pa_assert(config);
    pa_assert(module_id);

    SLLIST_FOREACH(module, config->hw_modules) {
        if (pa_streq(module_id, module->name))
            return module;
    }

    return NULL;
}

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

pa_droid_profile *pa_droid_profile_new(pa_droid_profile_set *ps, const pa_droid_config_output *output, const pa_droid_config_input *input) {
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

static pa_droid_profile *add_profile(pa_droid_profile_set *ps, const pa_droid_config_output *output, const pa_droid_config_input *input) {
    pa_droid_profile *ap;

    pa_log_debug("New profile: %s-%s", output->name, input ? input->name : "no input");

    ap = pa_droid_profile_new(ps, output, input);

    pa_hashmap_put(ps->profiles, ap->name, ap);

    return ap;
}

static bool str_in_strlist(const char *str, pa_strlist *list) {
    pa_strlist *iter;

    pa_assert(str);
    pa_assert(list);

    for (iter = list; iter; iter = pa_strlist_next(iter)) {
        if (pa_streq(str, pa_strlist_data(iter)))
            return true;
    }

    return false;
}

/* If outputs or inputs string list contain string *all* it means all
 * outputs or inputs are added to the combined profile.
 * If outputs or inputs string list contain string *auto* it means
 * all devices that are in module and listed in droid_combined_auto_*
 * are added to the combined profile. */
static pa_droid_profile *add_combined_profile(pa_droid_profile_set *ps,
                                              const pa_droid_config_hw_module *module,
                                              pa_strlist *outputs,
                                              pa_strlist *inputs) {
    pa_droid_profile *p;
    char *description;
    char *o_str;
    char *i_str;
    pa_strlist *to_outputs = NULL;
    pa_strlist *to_inputs = NULL;
    pa_droid_mapping *am;
    pa_droid_config_output *output;
    pa_droid_config_input *input;

    pa_assert(ps);
    pa_assert(module);

    if (outputs) {
        if (str_in_strlist(PA_DROID_COMBINED_AUTO, outputs)) {
            for (unsigned i = 0; droid_combined_auto_outputs[i]; i++) {
                SLLIST_FOREACH(output, module->outputs) {
                    if (pa_streq(droid_combined_auto_outputs[i], output->name)) {
                        pa_log_debug("Auto add to combined profile output %s", output->name);
                        to_outputs = pa_strlist_prepend(to_outputs, output->name);
                    }
                }
            }
        } else {
            SLLIST_FOREACH(output, module->outputs) {
                if (!str_in_strlist(PA_DROID_COMBINED_ALL, outputs) &&
                    !str_in_strlist(output->name, outputs))
                    continue;

                to_outputs = pa_strlist_prepend(to_outputs, output->name);
            }
        }

        to_outputs = pa_strlist_reverse(to_outputs);
    }

    if (inputs) {
        if (str_in_strlist(PA_DROID_COMBINED_AUTO, inputs)) {
            for (unsigned i = 0; droid_combined_auto_inputs[i]; i++) {
                SLLIST_FOREACH(input, module->inputs) {
                    if (pa_streq(droid_combined_auto_inputs[i], input->name)) {
                        pa_log_debug("Auto add to combined profile input %s", input->name);
                        to_inputs = pa_strlist_prepend(to_inputs, input->name);
                    }
                }
            }
        } else {
            SLLIST_FOREACH(input, module->inputs) {
                if (!str_in_strlist(PA_DROID_COMBINED_ALL, inputs) &&
                    !str_in_strlist(input->name, inputs))
                    continue;

                to_inputs = pa_strlist_prepend(to_inputs, input->name);
            }
        }

        to_inputs = pa_strlist_reverse(to_inputs);
    }

#if (PULSEAUDIO_VERSION >= 8)
    o_str = pa_strlist_to_string(to_outputs);
    i_str = pa_strlist_to_string(to_inputs);
#else
    o_str = pa_strlist_tostring(to_outputs);
    i_str = pa_strlist_tostring(to_inputs);
#endif

    pa_log_debug("New combined profile: %s (outputs: %s, inputs: %s)", module->name, o_str, i_str);

    if (!to_outputs && !to_inputs)
        pa_log("Combined profile doesn't have any outputs or inputs!");

    description = pa_sprintf_malloc("Combined outputs (%s) and inputs (%s) of %s.", o_str,
                                                                                    i_str,
                                                                                    module->name);
    p = profile_new(ps, module, module->name, description);
    pa_xfree(description);
    pa_xfree(o_str);
    pa_xfree(i_str);

    if (to_outputs) {
        SLLIST_FOREACH(output, module->outputs) {
            if (!str_in_strlist(output->name, to_outputs))
                continue;

            am = pa_droid_mapping_get(ps, PA_DIRECTION_OUTPUT, output);
            pa_droid_profile_add_mapping(p, am);

            if (pa_streq(output->name, "primary"))
                p->priority += DEFAULT_PRIORITY;
        }
    }

    if (to_inputs) {
        SLLIST_FOREACH(input, module->inputs) {
            if (!str_in_strlist(input->name, to_inputs))
                continue;

            am = pa_droid_mapping_get(ps, PA_DIRECTION_INPUT, input);
            pa_droid_profile_add_mapping(p, am);

            if (pa_streq(input->name, "primary"))
                p->priority += DEFAULT_PRIORITY;
        }
    }

    pa_strlist_free(to_outputs);
    pa_strlist_free(to_inputs);

    return p;
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

static void add_all_profiles(pa_droid_profile_set *ps, const pa_droid_config_hw_module *module) {
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
                add_profile(ps, output, input);
        } else
            add_profile(ps, output, NULL);
    }
}

pa_droid_profile_set *pa_droid_profile_set_new(const pa_droid_config_hw_module *module) {
    pa_droid_profile_set *ps;

    ps = profile_set_new(module);
    add_all_profiles(ps, module);

    return ps;
}

pa_droid_profile_set *pa_droid_profile_set_combined_new(const pa_droid_config_hw_module *module,
                                                        pa_strlist *outputs,
                                                        pa_strlist *inputs) {
    pa_droid_profile_set *ps;

    ps = profile_set_new(module);
    add_combined_profile(ps, module, outputs, inputs);
    add_all_profiles(ps, module);

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

    if (am->profile_set->config->global_config.attached_output_devices & device)
        p->priority += DEFAULT_PRIORITY;

    if (am->profile_set->config->global_config.default_output_device & device)
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

        if (am->profile_set->config->global_config.attached_input_devices & device)
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

bool pa_droid_mapping_is_primary(pa_droid_mapping *am) {
    pa_assert(am);

    if (am->direction == PA_DIRECTION_OUTPUT) {
        pa_assert(am->output);
        return pa_streq(am->output->name, PA_DROID_PRIMARY_DEVICE);
    } else {
        pa_assert(am->input);
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

bool pa_droid_output_port_name(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_output_device_fancy, (uint32_t) value, to_str);
}

bool pa_droid_input_port_name(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_input_device_fancy, (uint32_t) value, to_str);
}

bool pa_droid_audio_source_name(audio_source_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_audio_source_fancy, (uint32_t) value, to_str);
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

static char *shared_name_get(const char *module_id) {
    pa_assert(module_id);
    return pa_sprintf_malloc("droid-hardware-module-%s", module_id);
}

static pa_droid_hw_module *droid_hw_module_open(pa_core *core, pa_droid_config_audio *config, const char *module_id) {
    const pa_droid_config_hw_module *module;
    pa_droid_hw_module *hw = NULL;
    struct hw_module_t *hwmod = NULL;
    audio_hw_device_t *device = NULL;
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
    pa_assert(hw);

    pa_log_info("Closing hw module %s", hw->enabled_module->name);

    if (hw->config)
        droid_config_free(hw->config);

    if (hw->device)
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

static void droid_config_free(pa_droid_config_audio *config) {
    pa_droid_config_hw_module *module;
    pa_droid_config_output *output;
    pa_droid_config_input *input;

    pa_assert(config);

    while (config->hw_modules) {
        SLLIST_STEAL_FIRST(module, config->hw_modules);

        while (module->outputs) {
            SLLIST_STEAL_FIRST(output, module->outputs);
            pa_xfree(output->name);
            pa_xfree(output);
        }

        while (module->inputs) {
            SLLIST_STEAL_FIRST(input, module->inputs);
            pa_xfree(input->name);
            pa_xfree(input);
        }

        pa_xfree(module->name);
        pa_xfree(module);
    }

    pa_xfree(config);
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
    droid_config_free(config);
    return NULL;
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

pa_droid_stream *pa_droid_open_output_stream(pa_droid_hw_module *module,
                                             const pa_sample_spec *spec,
                                             const pa_channel_map *map,
                                             audio_output_flags_t flags,
                                             audio_devices_t devices) {
    pa_droid_stream *s = NULL;
    int ret;
    struct audio_stream_out *stream;
    audio_format_t hal_audio_format = 0;
    audio_channel_mask_t hal_channel_mask = 0;
    struct audio_config config_out;
    size_t buffer_size;

    pa_assert(module);
    pa_assert(spec);
    pa_assert(map);

    if (!pa_convert_format(spec->format, CONV_FROM_PA, &hal_audio_format)) {
        pa_log("Sample spec format %u not supported.", spec->format);
        goto fail;
    }

    for (int i = 0; i < map->channels; i++) {
        audio_channel_mask_t c;
        if (!pa_convert_output_channel(map->map[i], CONV_FROM_PA, &c)) {
            pa_log("Failed to convert channel map.");
            goto fail;
        }
        hal_channel_mask |= c;
    }

    memset(&config_out, 0, sizeof(struct audio_config));
    config_out.sample_rate = spec->rate;
    config_out.channel_mask = hal_channel_mask;
    config_out.format = hal_audio_format;

    if (pa_idxset_size(module->outputs) == 0) {
        pa_log_debug("Set initial output device to %#010x", devices);
        module->output_device = devices;
    } else {
        pa_log_debug("Output with device %#010x already open, using as initial device.", module->output_device);
        devices = module->output_device;
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
    s->out = stream;
    s->sample_spec = *spec;
    s->channel_map = *map;
    s->flags = flags;

    if ((s->sample_spec.rate = s->out->common.get_sample_rate(&s->out->common)) != spec->rate)
        pa_log_warn("Requested sample rate %u but got %u instead.", spec->rate, s->sample_spec.rate);

    pa_idxset_put(module->outputs, s, NULL);

    buffer_size = s->out->common.get_buffer_size(&s->out->common);

    pa_log_info("Opened droid output stream %p with device: %u flags: %u sample rate: %u channels: %u (%u) format: %u (%u) buffer size: %u (%llu usec)",
            (void *) s,
            devices,
            s->flags,
            s->sample_spec.rate,
            s->sample_spec.channels, hal_channel_mask,
            s->sample_spec.format, hal_audio_format,
            buffer_size,
            pa_bytes_to_usec(buffer_size, &s->sample_spec));

    return s;

fail:
    pa_xfree(s);

    return NULL;
}

pa_droid_stream *pa_droid_open_input_stream(pa_droid_hw_module *module,
                                            const pa_sample_spec *spec,
                                            const pa_channel_map *map,
                                            audio_devices_t devices) {

    pa_droid_stream *s = NULL;
    int ret;
    audio_stream_in_t *stream;
    audio_format_t hal_audio_format = 0;
    audio_channel_mask_t hal_channel_mask = 0;
    pa_channel_map channel_map;
    pa_sample_spec sample_spec;
    bool voicecall_record = false;
    struct audio_config config_in;
    size_t buffer_size;

#if AUDIO_API_VERSION_MAJ >= 2
    if ((devices & ~AUDIO_DEVICE_BIT_IN) & AUDIO_DEVICE_IN_VOICE_CALL)
#else
    if (devices & AUDIO_DEVICE_IN_VOICE_CALL)
#endif
        voicecall_record = true;

    channel_map = *map;
    sample_spec = *spec;

    if (!pa_convert_format(spec->format, CONV_FROM_PA, &hal_audio_format)) {
        pa_log("Sample spec format %u not supported.", spec->format);
        goto fail;
    }

    for (int i = 0; i < map->channels; i++) {
        audio_channel_mask_t c;
        if (!pa_convert_input_channel(map->map[i], CONV_FROM_PA, &c)) {
            pa_log("Failed to convert channel map.");
            goto fail;
        }
        hal_channel_mask |= c;
    }

    if (voicecall_record) {
        pa_channel_map_init_mono(&channel_map);
        sample_spec.channels = 1;
        /* Only allow recording both downlink and uplink. */
#if defined(QCOM_HARDWARE)
  #if (ANDROID_VERSION_MAJOR <= 4) && defined(HAVE_ENUM_AUDIO_CHANNEL_IN_VOICE_CALL_MONO)
        hal_channel_mask = AUDIO_CHANNEL_IN_VOICE_CALL_MONO;
  #else
        hal_channel_mask = AUDIO_CHANNEL_IN_MONO;
  #endif
#else
        hal_channel_mask = AUDIO_CHANNEL_IN_VOICE_UPLINK | AUDIO_CHANNEL_IN_VOICE_DNLINK;
#endif
    }

    memset(&config_in, 0, sizeof(struct audio_config));
    config_in.sample_rate = sample_spec.rate;
    config_in.channel_mask = hal_channel_mask;
    config_in.format = hal_audio_format;

    pa_droid_hw_module_lock(module);
    ret = module->device->open_input_stream(module->device,
                                            module->stream_in_id++,
                                            devices,
                                            &config_in,
                                            &stream
#if AUDIO_API_VERSION_MAJ >= 3
                                                  , AUDIO_INPUT_FLAG_NONE   /* Default to no input flags */
                                                  , NULL                    /* Don't define address */
                                                  , AUDIO_SOURCE_DEFAULT    /* Default audio source */
#endif
                                                  );
    pa_droid_hw_module_unlock(module);

    if (ret < 0 || !stream) {
        pa_log("Failed to open input stream: %d with device: %u flags: %u sample rate: %u channels: %u (%u) format: %u (%u)",
               ret,
               devices,
               0, /* AUDIO_INPUT_FLAG_NONE on v3. v1 and v2 don't have input flags. */
               config_in.sample_rate,
               sample_spec.channels,
               config_in.channel_mask,
               sample_spec.format,
               config_in.format);
        goto fail;
    }

    s = droid_stream_new(module);
    s->in = stream;
    s->sample_spec = sample_spec;
    s->channel_map = channel_map;
    s->flags = 0;

    if ((s->sample_spec.rate = s->in->common.get_sample_rate(&s->in->common)) != spec->rate)
        pa_log_warn("Requested sample rate %u but got %u instead.", spec->rate, s->sample_spec.rate);

    pa_idxset_put(module->inputs, s, NULL);

    buffer_size = s->in->common.get_buffer_size(&s->in->common);

    /* As audio_source_t may not have any effect when opening the input stream
     * set input parameters immediately after opening the stream. */
    pa_droid_stream_set_input_route(s, devices, NULL);

    pa_log_info("Opened droid input stream %p with device: %u flags: %u sample rate: %u channels: %u (%u) format: %u (%u) buffer size: %u (%llu usec)",
            (void *) s,
            devices,
            s->flags,
            s->sample_spec.rate,
            s->sample_spec.channels, hal_channel_mask,
            s->sample_spec.format, hal_audio_format,
            buffer_size,
            pa_bytes_to_usec(buffer_size, &s->sample_spec));

    return s;

fail:
    pa_xfree(s);

    return NULL;
}

pa_droid_stream *pa_droid_stream_ref(pa_droid_stream *s) {
    pa_assert(s);
    pa_assert(s->out || s->in);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    PA_REFCNT_INC(s);
    return s;
}

void pa_droid_stream_unref(pa_droid_stream *s) {
    pa_assert(s);
    pa_assert(s->out || s->in);
    pa_assert(PA_REFCNT_VALUE(s) >= 1);

    if (PA_REFCNT_DEC(s) > 0)
        return;

    if (s->out) {
        pa_mutex_lock(s->module->output_mutex);
        pa_idxset_remove_by_data(s->module->outputs, s, NULL);
        s->module->device->close_output_stream(s->module->device, s->out);
        pa_mutex_unlock(s->module->output_mutex);
    } else {
        pa_mutex_lock(s->module->input_mutex);
        pa_idxset_remove_by_data(s->module->inputs, s, NULL);
        s->module->device->close_input_stream(s->module->device, s->in);
        pa_mutex_unlock(s->module->input_mutex);
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
        if (s->flags & AUDIO_OUTPUT_FLAG_PRIMARY)
            return s;
    }

    return NULL;
}

int pa_droid_stream_set_output_route(pa_droid_stream *s, audio_devices_t device) {
    pa_droid_stream *slave;
    uint32_t idx;
    char *parameters;
    int ret = 0;

    pa_assert(s);
    pa_assert(s->out);
    pa_assert(s->module);
    pa_assert(s->module->output_mutex);

    pa_mutex_lock(s->module->output_mutex);

    parameters = pa_sprintf_malloc("%s=%u;", AUDIO_PARAMETER_STREAM_ROUTING, device);

    if (s->flags & AUDIO_OUTPUT_FLAG_PRIMARY || get_primary_output(s->module) == NULL) {
        pa_log_debug("output stream %p set_parameters(%s) %#010x", (void *) s, parameters, device);
        ret = s->out->common.set_parameters(&s->out->common, parameters);

        if (ret < 0) {
            if (ret == -ENOSYS)
                pa_log_warn("output set_parameters(%s) not allowed while stream is active", parameters);
            else
                pa_log_warn("output set_parameters(%s) failed", parameters);
        } else {
            /* Store last set output device. */
            s->module->output_device = device;
        }
    }

    if (s->flags & AUDIO_OUTPUT_FLAG_PRIMARY && pa_idxset_size(s->module->outputs) > 1) {

        PA_IDXSET_FOREACH(slave, s->module->outputs, idx) {
            if (slave == s)
                continue;

            pa_log_debug("slave output stream %p set_parameters(%s)", (void *) slave, parameters);
            ret = slave->out->common.set_parameters(&slave->out->common, parameters);

            if (ret < 0) {
                if (ret == -ENOSYS)
                    pa_log_warn("output set_parameters(%s) not allowed while stream is active", parameters);
                else
                    pa_log_warn("output set_parameters(%s) failed", parameters);
            }
        }
    }

    pa_xfree(parameters);

    pa_mutex_unlock(s->module->output_mutex);

    return ret;
}

int pa_droid_stream_set_input_route(pa_droid_stream *s, audio_devices_t device, audio_source_t *new_source) {
    audio_source_t source = (uint32_t) -1;
    char *parameters;
    int ret;

    pa_assert(s);
    pa_assert(s->in);

#ifdef DROID_DEVICE_I9305
    device &= ~AUDIO_DEVICE_BIT_IN;
#endif

    if (pa_input_device_default_audio_source(device, &source))
#ifdef DROID_AUDIO_HAL_ATOI_FIX
        parameters = pa_sprintf_malloc("%s=%d;%s=%u", AUDIO_PARAMETER_STREAM_ROUTING, (int32_t) device,
                                                      AUDIO_PARAMETER_STREAM_INPUT_SOURCE, source);
#else
        parameters = pa_sprintf_malloc("%s=%u;%s=%u", AUDIO_PARAMETER_STREAM_ROUTING, device,
                                                      AUDIO_PARAMETER_STREAM_INPUT_SOURCE, source);
#endif
    else
        parameters = pa_sprintf_malloc("%s=%u", AUDIO_PARAMETER_STREAM_ROUTING, device);

    pa_log_debug("input stream %p set_parameters(%s) %#010x ; %#010x",
                 (void *) s, parameters, device, source);


#if defined(DROID_DEVICE_ANZU) ||\
    defined(DROID_DEVICE_COCONUT) || defined(DROID_DEVICE_HAIDA) ||\
    defined(DROID_DEVICE_HALLON) || defined(DROID_DEVICE_IYOKAN) ||\
    defined(DROID_DEVICE_MANGO) || defined(DROID_DEVICE_SATSUMA) ||\
    defined(DROID_DEVICE_SMULTRON) || defined(DROID_DEVICE_URUSHI) ||\
    defined(DROID_DEVICE_MOTO_MSM8960_JBBL)
#warning Using set_parameters hack, originating from previous cm10 mako.
    pa_mutex_lock(s->module->hw_mutex);
    ret = s->module->device->set_parameters(s->module->device, parameters);
    pa_mutex_unlock(s->module->hw_mutex);
#else
    pa_mutex_lock(s->module->input_mutex);
    ret = s->in->common.set_parameters(&s->in->common, parameters);
    pa_mutex_unlock(s->module->input_mutex);
#endif

    if (ret < 0) {
        if (ret == -ENOSYS)
            pa_log_warn("input set_parameters(%s) not allowed while stream is active", parameters);
        else
            pa_log_warn("input set_parameters(%s) failed", parameters);
    }

    if (new_source)
        *new_source = source;

    pa_xfree(parameters);

    return ret;
}

int pa_droid_stream_set_parameters(pa_droid_stream *s, const char *parameters) {
    int ret;

    pa_assert(s);
    pa_assert(s->out || s->in);
    pa_assert(parameters);

    if (s->out) {
        pa_log_debug("output stream %p set_parameters(%s)", (void *) s, parameters);
        pa_mutex_lock(s->module->output_mutex);
        ret = s->out->common.set_parameters(&s->out->common, parameters);
        pa_mutex_unlock(s->module->output_mutex);
    } else {
        pa_log_debug("input stream %p set_parameters(%s)", (void *) s, parameters);
        pa_mutex_lock(s->module->input_mutex);
        ret = s->in->common.set_parameters(&s->in->common, parameters);
        pa_mutex_unlock(s->module->input_mutex);
    }

    if (ret < 0)
        pa_log("%s stream %p set_parameters(%s) failed: %d",
               s->out ? "output" : "input", (void *) s, parameters, ret);

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
    pa_assert(s->out || s->in);

    /* Even though earlier (< 3) HALs don't have input flags,
     * input flags don't have anything similar as output stream's
     * primary flag and we can just always reply false for
     * input streams. */
    if (s->out)
        return s->flags & AUDIO_OUTPUT_FLAG_PRIMARY;
    else
        return false;
}

int pa_droid_stream_suspend(pa_droid_stream *s, bool suspend) {
    pa_assert(s);
    pa_assert(s->out || s->in);

    if (s->out) {
        if (suspend) {
            pa_atomic_dec(&s->module->active_outputs);
            return s->out->common.standby(&s->out->common);
        } else {
            pa_atomic_inc(&s->module->active_outputs);
            return 0;
        }
    } else {
        if (suspend)
            return s->in->common.standby(&s->in->common);
        else
            return 0;
    }
}

bool pa_sink_is_droid_sink(pa_sink *s) {
    const char *api;

    pa_assert(s);

    if ((api = pa_proplist_gets(s->proplist, PA_PROP_DEVICE_API)))
        return pa_streq(api, PROP_DROID_API_STRING);
    else
        return false;
}
