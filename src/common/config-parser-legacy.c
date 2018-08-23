/*
 * Copyright (C) 2018 Jolla Ltd.
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
#include <stdbool.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulse/xmalloc.h>

#include <hardware_legacy/audio_policy_conf.h>

#include "droid/version.h"
#include "droid/droid-config.h"
#include "droid/conversion.h"
#include "droid/sllist.h"

/* Section defining custom global configuration variables. */
#define GLOBAL_CONFIG_EXT_TAG   "custom_properties"

#define GAIN_TAG_PREFIX         "gain_"

#define MAX_LINE_LENGTH         (1024)
#define WHITESPACE              "\n\r \t"

static void log_parse_error(const char *fn, const unsigned ln, const char *section, const char *v) {
    pa_log("[%s:%u] failed to parse line in section %s: unknown section (%s)", fn, ln, section, v);
}

pa_droid_config_audio *pa_parse_droid_audio_config_legacy(const char *filename) {
    pa_droid_config_audio *config = NULL;
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
        IN_CONFIG           = 6,
        IN_MODULE_GLOBAL    = 10,
        IN_DEVICES          = 20,
        IN_DEVICES_DEVICE   = 21,
        IN_GAINS            = 22,
        IN_GAIN_N           = 23
    } loc = IN_ROOT;

    bool in_output = true;

    pa_droid_config_hw_module *module = NULL;
    pa_droid_config_device *output = NULL;
    pa_droid_config_device *input = NULL;

    pa_assert(filename);

    f = fopen(filename, "r");

    if (!f) {
        pa_log_info("Failed to open config file (%s): %s", filename, pa_cstrerror(errno));
        ret = false;
        goto finish;
    }

    config = pa_xnew0(pa_droid_config_audio, 1);
    config->global_config = pa_xnew0(pa_droid_config_global, 1);

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
                        log_parse_error(filename, n, "<root>", v);
                        ret = false;
                        goto finish;
                    }
                    break;

                case IN_GLOBAL:
                    if (pa_streq(v, GLOBAL_CONFIG_EXT_TAG))
                        loc = IN_GLOBAL_EXT;
                    else {
                        log_parse_error(filename, n, GLOBAL_CONFIG_TAG, v);
                        ret = false;
                        goto finish;
                    }
                    break;

                case IN_HW_MODULES:
                    pa_assert(!module);

                    module = pa_droid_config_hw_module_new(config, v);
                    SLLIST_APPEND(pa_droid_config_hw_module, config->hw_modules, module);
                    hw_module_count++;
                    loc = IN_MODULE;
                    pa_log_debug("config: New module: %s", module->name);
                    break;

                case IN_MODULE:
                    pa_assert(module);

                    if (pa_streq(v, OUTPUTS_TAG)) {
                        loc = IN_OUTPUT_INPUT;
                        in_output = true;
                    } else if (pa_streq(v, INPUTS_TAG)) {
                        loc = IN_OUTPUT_INPUT;
                        in_output = false;
                    } else if (pa_streq(v, GLOBAL_CONFIG_TAG)) {
                        loc = IN_MODULE_GLOBAL;
                    } else if (pa_streq(v, DEVICES_TAG)) {
                        loc = IN_DEVICES;
                    } else {
                        log_parse_error(filename, n, module->name, v);
                        ret = false;
                        goto finish;
                    }
                    break;

                case IN_OUTPUT_INPUT:
                    pa_assert(module);

                    if (in_output) {
                        output = pa_droid_config_device_new(module, PA_DIRECTION_OUTPUT, v);
                        SLLIST_APPEND(pa_droid_config_device, module->outputs, output);
                        loc = IN_CONFIG;
                        pa_log_debug("config: %s: New output: %s", module->name, output->name);
                    } else {
                        input = pa_droid_config_device_new(module, PA_DIRECTION_INPUT, v);
                        SLLIST_APPEND(pa_droid_config_device, module->inputs, input);
                        loc = IN_CONFIG;
                        pa_log_debug("config: %s: New input: %s", module->name, input->name);
                    }
                    break;

                case IN_DEVICES:
                    /* TODO Missing implementation of parsing the module/devices section.
                     * As of now there is no need for the information, fix this when that
                     * changes. */
                    loc = IN_DEVICES_DEVICE;
                    break;

                case IN_DEVICES_DEVICE:
                    if (pa_streq(v, GAINS_TAG))
                        loc = IN_GAINS;
                    else {
                        log_parse_error(filename, n, DEVICES_TAG, v);
                        ret = false;
                        goto finish;
                    }
                    break;

                case IN_GAINS:
                    /* TODO Missing implementation of parsing the gain_n section.
                     * As of now there is no need for the information, fix this when that
                     * changes. */
                    if (pa_startswith(v, GAIN_TAG_PREFIX))
                        loc = IN_GAIN_N;
                    else {
                        log_parse_error(filename, n, GAINS_TAG, v);
                        ret = false;
                        goto finish;
                    }
                    break;

                case IN_CONFIG:
                    if (pa_streq(v, GAINS_TAG)) {
                        loc = IN_GAINS;
                    } else {
                        log_parse_error(filename, n, in_output ? output->name : input->name, v);
                        ret = false;
                        goto finish;
                    }
                    break;

                default:
                    pa_log("[%s:%u] failed to parse line: unknown section (%s)", filename, n, v);
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
                    /* fall through */
                case IN_GLOBAL:
                    loc = IN_ROOT;
                    break;

                case IN_MODULE:
                    module = NULL;
                    loc = IN_HW_MODULES;
                    break;

                case IN_DEVICES:
                    /* fall through */
                case IN_MODULE_GLOBAL:
                    loc = IN_MODULE;
                    break;

                case IN_GAINS:
                    if (output || input)
                        loc = IN_CONFIG;
                    else
                        loc = IN_DEVICES_DEVICE;
                    break;

                case IN_OUTPUT_INPUT:
                    if (in_output)
                        output = NULL;
                    else
                        input = NULL;
                    /* fall through */
                case IN_GAIN_N:
                    /* fall through */
                case IN_DEVICES_DEVICE:
                    /* fall through */
                case IN_CONFIG:
                    /* fall through */
                case IN_GLOBAL_EXT:
                    loc--;
                    break;
            }

            continue;
        }

        /* Parsing of values */
        if (loc == IN_GLOBAL ||
            loc == IN_GLOBAL_EXT ||
            loc == IN_MODULE_GLOBAL ||
            loc == IN_CONFIG ||
            loc == IN_DEVICES_DEVICE ||
            loc == IN_GAIN_N) {

            bool success = false;

            if (loc == IN_GLOBAL || loc == IN_MODULE_GLOBAL) {
                pa_droid_config_global *global_config = NULL;

                if (loc == IN_MODULE_GLOBAL) {
                    pa_assert(module);
                    if (!module->global_config)
                        module->global_config = pa_xnew0(pa_droid_config_global, 1);
                    global_config = module->global_config;
                } else
                    global_config = config->global_config;

                pa_assert(global_config);

                /* Parse global configuration */

                if (pa_streq(v, ATTACHED_OUTPUT_DEVICES_TAG))
                    success = pa_conversion_parse_output_devices(filename, n, value, true, true,
                                                                 &global_config->attached_output_devices);
                else if (pa_streq(v, DEFAULT_OUTPUT_DEVICE_TAG))
                    success = pa_conversion_parse_output_devices(filename, n, value, true, true,
                                                                 &global_config->default_output_device);
                else if (pa_streq(v, ATTACHED_INPUT_DEVICES_TAG))
                    success = pa_conversion_parse_input_devices(filename, n, value, true, false,
                                                                &global_config->attached_input_devices);
                else if (pa_streq(v, AUDIO_HAL_VERSION_TAG))
                    success = pa_conversion_parse_version(filename, n, value,
                                                          &global_config->audio_hal_version);
#ifdef SPEAKER_DRC_ENABLED_TAG
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
                    success = pa_conversion_parse_sampling_rates(filename, n, value, true,
                                                                 in_output ? output->sampling_rates : input->sampling_rates);
                else if (pa_streq(v, FORMATS_TAG))
                    success = pa_conversion_parse_formats(filename, n, value, true,
                                                          in_output ? &output->formats : &input->formats);
                else if (pa_streq(v, CHANNELS_TAG)) {
                    if (in_output)
                        success = pa_conversion_parse_output_channels(filename, n, value, true, &output->channel_masks);
                    else
                        success = pa_conversion_parse_input_channels(filename, n, value, true, &input->channel_masks);
                } else if (pa_streq(v, DEVICES_TAG)) {
                    if (in_output)
                        success = pa_conversion_parse_output_devices(filename, n, value, true, false, &output->devices);
                    else
                        success = pa_conversion_parse_input_devices(filename, n, value, true, false, &input->devices);
                } else if (pa_streq(v, FLAGS_TAG)) {
                    if (in_output)
                        success = pa_conversion_parse_output_flags(filename, n, value, &output->flags);
                    else {
#if AUDIO_API_VERSION_MAJ >= 3
                        success = pa_conversion_parse_input_flags(filename, n, value, &input->flags);
#else
                        pa_log("[%s:%u] failed to parse line - output flags inside input definition", filename, n);
                        success = false;
#endif
                    }
                } else {
                    pa_log("[%s:%u] failed to parse line - unknown config entry %s", filename, n, v);
                    success = false;
                }

            } else if (loc == IN_DEVICES_DEVICE) {
                /* TODO Missing implementation of parsing the module/devices section.
                 * As of now there is no need for the information, fix this when that
                 * changes. */
                success = true;
            } else if (loc == IN_GAIN_N) {
                /* TODO Missing implementation of parsing the gain_n section.
                 * As of now there is no need for the information, fix this when that
                 * changes. */
                success = true;
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

    if (!ret)
        pa_droid_config_free(config), config = NULL;

    return config;
}
