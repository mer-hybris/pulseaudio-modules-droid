#ifndef foodroidconversionfoo
#define foodroidconversionfoo

/*
 * Copyright (C) 2018 Jolla Ltd.
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
#include <pulsecore/modargs.h>

#include <hardware/audio.h>

/* From recent audio_policy_conf.h */
#ifndef AUDIO_HAL_VERSION_TAG
#define AUDIO_HAL_VERSION_TAG "audio_hal_version"
#endif
#ifndef GAINS_TAG
#define GAINS_TAG "gains"
#endif

#include "droid-config.h"

typedef enum {
    CONV_FROM_PA,
    CONV_FROM_HAL
} pa_conversion_field_t;

typedef enum {
    CONV_STRING_FORMAT,
    CONV_STRING_OUTPUT_CHANNELS,
    CONV_STRING_INPUT_CHANNELS,
    CONV_STRING_OUTPUT_DEVICE,
    CONV_STRING_INPUT_DEVICE,
    CONV_STRING_OUTPUT_FLAG,
    CONV_STRING_INPUT_FLAG
} pa_conversion_string_t;

bool pa_convert_output_channel(uint32_t value, pa_conversion_field_t from, uint32_t *to_value);
bool pa_convert_input_channel(uint32_t value, pa_conversion_field_t from, uint32_t *to_value);
bool pa_convert_format(uint32_t value, pa_conversion_field_t from, uint32_t *to_value);

bool pa_string_convert_output_device_num_to_str(audio_devices_t value, const char **to_str);
bool pa_string_convert_output_device_str_to_num(const char *str, audio_devices_t *to_value);
bool pa_string_convert_input_device_num_to_str(audio_devices_t value, const char **to_str);
bool pa_string_convert_input_device_str_to_num(const char *str, audio_devices_t *to_value);

bool pa_string_convert_flag_num_to_str(audio_output_flags_t value, const char **to_str);
bool pa_string_convert_flag_str_to_num(const char *str, audio_output_flags_t *to_value);

char *pa_list_string_output_device(audio_devices_t devices);
char *pa_list_string_input_device(audio_devices_t devices);
char *pa_list_string_flags(audio_output_flags_t flags);

/* Get default audio source associated with input device.
 * Return true if default source was found. */
bool pa_input_device_default_audio_source(audio_devices_t input_device, audio_source_t *default_source);

/* Pretty port names */
bool pa_droid_output_port_name(audio_devices_t value, const char **to_str);
bool pa_droid_input_port_name(audio_devices_t value, const char **to_str);

/* Pretty audio source names */
bool pa_droid_audio_source_name(audio_source_t value, const char **to_str);

int pa_conversion_parse_list(pa_conversion_string_t type, const char *separator,
                             const char *str, uint32_t *dst, char **unknown_entries);

bool pa_conversion_parse_sampling_rates(const char *fn, const unsigned ln,
                                        const char *str, bool legacy,
                                        uint32_t sampling_rates[AUDIO_MAX_SAMPLING_RATES]);
bool pa_conversion_parse_formats(const char *fn, const unsigned ln,
                                 const char *str, bool legacy,
                                 audio_format_t *formats);
bool pa_conversion_parse_output_channels(const char *fn, const unsigned ln,
                                         const char *str, bool legacy,
                                         audio_channel_mask_t *channels);
bool pa_conversion_parse_input_channels(const char *fn, const unsigned ln,
                                        const char *str, bool legacy,
                                        audio_channel_mask_t *channels);
bool pa_conversion_parse_output_devices(const char *fn, const unsigned ln,
                                        char *str, bool legacy, bool must_recognize_all,
                                        audio_devices_t *devices);
bool pa_conversion_parse_input_devices(const char *fn, const unsigned ln,
                                       char *str, bool legacy, bool must_recognize_all,
                                       audio_devices_t *devices);
bool pa_conversion_parse_output_flags(const char *fn, const unsigned ln,
                                      const char *str, audio_output_flags_t *flags);
bool pa_conversion_parse_input_flags(const char *fn, const unsigned ln,
                                     const char *str, audio_input_flags_t *flags);
bool pa_conversion_parse_version(const char *fn, const unsigned ln, const char *str, uint32_t *version);

#endif
