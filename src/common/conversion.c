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

#include "version.h"
#if ANDROID_VERSION_MAJOR == 4 && ANDROID_VERSION_MINOR == 1
#include "droid-util-41qc.h"
#else
#include "droid-util-audio.h"
#endif

#include <pulsecore/core-util.h>

#include <hardware/audio.h>

#include "conversion.h"
#include "droid-config.h"

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

#define value_separator(legacy) (legacy ? "|" : ",")

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


bool pa_droid_output_port_name(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_output_device_fancy, (uint32_t) value, to_str);
}

bool pa_droid_input_port_name(audio_devices_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_input_device_fancy, (uint32_t) value, to_str);
}

bool pa_droid_audio_source_name(audio_source_t value, const char **to_str) {
    return string_convert_num_to_str(string_conversion_table_audio_source_fancy, (uint32_t) value, to_str);
}

static int parse_list(const struct string_conversion *table,
                      const char *separator,
                      const char *str,
                      uint32_t *dst,
                      char **unknown_entries) {
    int count = 0;
    char *entry;
    char *unknown = NULL;
    const char *state = NULL;

    pa_assert(table);
    pa_assert(separator);
    pa_assert(str);
    pa_assert(dst);
    pa_assert(unknown_entries);

    *dst = 0;
    *unknown_entries = NULL;

    while ((entry = pa_split(str, separator, &state))) {
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

int pa_conversion_parse_list(pa_conversion_string_t type, const char *separator,
                             const char *str, uint32_t *dst, char **unknown_entries) {
    switch (type) {
        case CONV_STRING_FORMAT:
            return parse_list(string_conversion_table_format, separator, str, dst, unknown_entries);

        case CONV_STRING_OUTPUT_CHANNELS:
            return parse_list(string_conversion_table_output_channels, separator, str, dst, unknown_entries);

        case CONV_STRING_INPUT_CHANNELS:
            return parse_list(string_conversion_table_input_channels, separator, str, dst, unknown_entries);

        case CONV_STRING_OUTPUT_DEVICE:
            return parse_list(string_conversion_table_output_device, separator, str, dst, unknown_entries);

        case CONV_STRING_INPUT_DEVICE:
            return parse_list(string_conversion_table_input_device, separator, str, dst, unknown_entries);

        case CONV_STRING_OUTPUT_FLAG:
            return parse_list(string_conversion_table_output_flag, separator, str, dst, unknown_entries);

        case CONV_STRING_INPUT_FLAG:
            return parse_list(string_conversion_table_input_flag, separator, str, dst, unknown_entries);
    }

    pa_assert_not_reached();
    return 0;
}

bool pa_conversion_parse_sampling_rates(const char *fn, const unsigned ln,
                                        const char *str, bool legacy,
                                        uint32_t sampling_rates[AUDIO_MAX_SAMPLING_RATES]) {
    pa_assert(fn);
    pa_assert(str);

    char *entry;
    const char *state = NULL;

    uint32_t pos = 0;
    while ((entry = pa_split(str, value_separator(legacy), &state))) {
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
                          const bool must_recognize_all) {
    bool fail;

    pa_assert(fn);
    pa_assert(field);
    pa_assert(str);

    fail = must_recognize_all && unknown;

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

bool pa_conversion_parse_formats(const char *fn, const unsigned ln,
                                 const char *str, bool legacy,
                                 audio_format_t *formats) {
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

    count = pa_conversion_parse_list(CONV_STRING_FORMAT, value_separator(legacy), str, formats, &unknown);

    if (legacy)
        return check_and_log(fn, ln, "formats", count, str, unknown, false);

    /* As the new XML configuration lists formats as one per profile, unknown
     * formats will cause the parser to quit. As a workaround for non-legacy
     * conversions with no recognized formats log only info level and return false. */
    check_and_log(fn, ln, "format", count == 0 ? 1 : count, str, unknown, false);
    return count > 0;
}

static bool parse_channels(const char *fn, const unsigned ln,
                           const char *str, bool in_output,
                           bool legacy, audio_channel_mask_t *channels) {
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

    count = pa_conversion_parse_list(in_output ? CONV_STRING_OUTPUT_CHANNELS : CONV_STRING_INPUT_CHANNELS,
                                     value_separator(legacy), str, channels, &unknown);

    return check_and_log(fn, ln, in_output ? "output channel_masks" : "input channel_masks",
                         count, str, unknown, false);
}

bool pa_conversion_parse_output_channels(const char *fn, const unsigned ln,
                                         const char *str, bool legacy,
                                         audio_channel_mask_t *channels) {
    return parse_channels(fn, ln, str, true, legacy, channels);
}

bool pa_conversion_parse_input_channels(const char *fn, const unsigned ln,
                                        const char *str, bool legacy,
                                        audio_channel_mask_t *channels) {
    return parse_channels(fn, ln, str, false, legacy, channels);
}

static bool parse_devices(const char *fn, const unsigned ln,
                          const char *str, bool in_output,
                          bool legacy, bool must_recognize_all,
                          audio_devices_t *devices) {
    int count;
    char *unknown = NULL;

    pa_assert(fn);
    pa_assert(str);
    pa_assert(devices);

    count = pa_conversion_parse_list(in_output ? CONV_STRING_OUTPUT_DEVICE : CONV_STRING_INPUT_DEVICE,
                                     value_separator(legacy), str, devices, &unknown);

    if (legacy)
        return check_and_log(fn, ln, in_output ? "output devices" : "input devices",
                             count, str, unknown, must_recognize_all);

    /* As the new XML configuration lists devices as one per devicePort, unknown
     * devices will cause the parser to quit. As a workaround for non-legacy
     * conversions with no recognized devices log only info level and return false. */
    check_and_log(fn, ln, in_output ? "output device" : "input device",
                  count == 0 ? 1 : count, str, unknown, must_recognize_all);
    return count > 0;
}

bool pa_conversion_parse_output_devices(const char *fn, const unsigned ln,
                                        char *str, bool legacy, bool must_recognize_all,
                                        audio_devices_t *devices) {
    return parse_devices(fn, ln, str, true, legacy, must_recognize_all, devices);
}

bool pa_conversion_parse_input_devices(const char *fn, const unsigned ln,
                                       char *str, bool legacy, bool must_recognize_all,
                                       audio_devices_t *devices) {
    return parse_devices(fn, ln, str, false, legacy, must_recognize_all, devices);
}

bool pa_conversion_parse_output_flags(const char *fn, const unsigned ln,
                                      const char *str, audio_output_flags_t *flags) {
    int count;
    char *unknown = NULL;

    pa_assert(fn);
    pa_assert(str);
    pa_assert(flags);

    count = pa_conversion_parse_list(CONV_STRING_OUTPUT_FLAG, "|", str, flags, &unknown);

    return check_and_log(fn, ln, "flags", count, str, unknown, false);
}

bool pa_conversion_parse_input_flags(const char *fn, const unsigned ln,
                                     const char *str, uint32_t *flags) {
    int count;
    char *unknown = NULL;

    pa_assert(fn);
    pa_assert(str);
    pa_assert(flags);

    count = pa_conversion_parse_list(CONV_STRING_INPUT_FLAG, "|", str, flags, &unknown);

    return check_and_log(fn, ln, "flags", count, str, unknown, false);
}

bool pa_conversion_parse_version(const char *fn, const unsigned ln, const char *str, uint32_t *version) {
    uint32_t version_maj;
    uint32_t version_min;

    pa_assert(fn);
    pa_assert(str);
    pa_assert(version);

    if ((sscanf(str, "%u.%u", &version_maj, &version_min)) != 2) {
        pa_log("[%s:%u] Failed to parse %s (%s).", fn, ln, AUDIO_HAL_VERSION_TAG, str);
        return false;
    } else {
        *version = HARDWARE_DEVICE_API_VERSION(version_maj, version_min);
        return true;
    }
}
