#ifndef foodroidutilfoo
#define foodroidutilfoo

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
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/mutex.h>

#include <android-version.h>

#if !defined(ANDROID_VERSION_MAJOR) || !defined(ANDROID_VERSION_MINOR) || !defined(ANDROID_VERSION_PATCH)
#error "ANDROID_VERSION_* not defined. Did you get your headers via extract-headers.sh?"
#endif

#if ANDROID_VERSION_MAJOR == 4 && ANDROID_VERSION_MINOR == 1
#include "droid-util-41qc.h"
#elif ANDROID_VERSION_MAJOR == 4 && ANDROID_VERSION_MINOR == 2
#include "droid-util-42.h"
#elif ANDROID_VERSION_MAJOR == 4 && ANDROID_VERSION_MINOR == 4
#include "droid-util-44.h"
#elif ANDROID_VERSION_MAJOR == 5 && ANDROID_VERSION_MINOR == 1
#include "droid-util-51.h"
#else
#error "No valid ANDROID_VERSION found."
#endif

#define PROP_DROID_DEVICES    "droid.devices"
#define PROP_DROID_FLAGS      "droid.flags"
#define PROP_DROID_HW_MODULE  "droid.hw_module"

typedef struct pa_droid_hw_module pa_droid_hw_module;
typedef struct pa_droid_card_data pa_droid_card_data;
typedef int (*common_set_parameters_cb_t)(pa_droid_card_data *card_data, const char *str);

typedef struct pa_droid_config_audio pa_droid_config_audio;
typedef struct pa_droid_config_hw_module pa_droid_config_hw_module;

struct pa_droid_hw_module {
    PA_REFCNT_DECLARE;

    pa_core *core;
    char *shared_name;

    pa_droid_config_audio *config;
    const pa_droid_config_hw_module *enabled_module;
    pa_mutex *hw_mutex;

    struct hw_module_t *hwmod;
    audio_hw_device_t *device;

    const char *module_id;

    uint32_t stream_out_id;
    uint32_t stream_in_id;

};

struct pa_droid_card_data {
    void *userdata;
    /* General functions */
    char *module_id;
    common_set_parameters_cb_t set_parameters;
};

#define AUDIO_MAX_SAMPLING_RATES (32)
#define AUDIO_MAX_HW_MODULES (8)
#define AUDIO_MAX_INPUTS (8)
#define AUDIO_MAX_OUTPUTS (8)

typedef struct pa_droid_config_global {
    audio_devices_t attached_output_devices;
    audio_devices_t default_output_device;
    audio_devices_t attached_input_devices;
} pa_droid_config_global;

typedef struct pa_droid_config_output {
    const pa_droid_config_hw_module *module;

    char name[AUDIO_HARDWARE_MODULE_ID_MAX_LEN];
    uint32_t sampling_rates[AUDIO_MAX_SAMPLING_RATES]; /* (uint32_t) -1 -> dynamic */
    audio_channel_mask_t channel_masks; /* 0 -> dynamic */
    audio_format_t formats;
    audio_devices_t devices;
    audio_output_flags_t flags;
} pa_droid_config_output;

typedef struct pa_droid_config_input {
    const pa_droid_config_hw_module *module;

    char name[AUDIO_HARDWARE_MODULE_ID_MAX_LEN];
    uint32_t sampling_rates[AUDIO_MAX_SAMPLING_RATES]; /* (uint32_t) -1 -> dynamic */
    audio_channel_mask_t channel_masks; /* 0 -> dynamic */
    audio_format_t formats;
    audio_devices_t devices;
#if DROID_HAL >= 3
    audio_input_flags_t flags;
#endif
} pa_droid_config_input;

struct pa_droid_config_hw_module {
    const pa_droid_config_audio *config;

    char name[AUDIO_HARDWARE_MODULE_ID_MAX_LEN];
    pa_droid_config_output outputs[AUDIO_MAX_OUTPUTS];
    uint32_t outputs_size;
    pa_droid_config_input inputs[AUDIO_MAX_INPUTS];
    uint32_t inputs_size;
};

struct pa_droid_config_audio {
    pa_droid_config_global global_config;
    pa_droid_config_hw_module hw_modules[AUDIO_MAX_HW_MODULES];
    uint32_t hw_modules_size;
};


/* Profiles */

typedef struct pa_droid_profile_set pa_droid_profile_set;
typedef struct pa_droid_mapping pa_droid_mapping;

typedef struct pa_droid_port_data {
    audio_devices_t device;
} pa_droid_port_data;

typedef struct pa_droid_port {
    pa_droid_mapping *mapping;

    audio_devices_t device;
    char *name;
    char *description;
    unsigned priority;
} pa_droid_port;

struct pa_droid_mapping {
    pa_droid_profile_set *profile_set;

    const pa_droid_config_output *output;
    const pa_droid_config_input *input;

    char *name;
    char *description;
    unsigned priority;
    pa_proplist *proplist;

    /* Mapping doesn't own the ports */
    pa_idxset *ports;

    pa_direction_t direction;

    pa_sink *sink;
    pa_source *source;
};

typedef struct pa_droid_profile {
    pa_droid_profile_set *profile_set;

    const pa_droid_config_hw_module *module;

    char *name;
    char *description;
    unsigned priority;

    /* Profile doesn't own the mappings */
    pa_droid_mapping *output;
    pa_droid_mapping *input;

} pa_droid_profile;

struct pa_droid_profile_set {
    const pa_droid_config_audio *config;

    pa_hashmap *all_ports;
    pa_hashmap *output_mappings;
    pa_hashmap *input_mappings;
    pa_hashmap *profiles;
};

#define PA_DROID_OUTPUT_PARKING "output-parking"
#define PA_DROID_INPUT_PARKING "input-parking"

/* Open hardware module */
/* 'config' can be NULL if it is assumed that hw module with module_id already is open. */
/* if opening of hw_module succeeds, config ownership is transferred to hw_module and config
 * shouldn't be freed. */
pa_droid_hw_module *pa_droid_hw_module_get(pa_core *core, pa_droid_config_audio *config, const char *module_id);
pa_droid_hw_module *pa_droid_hw_module_ref(pa_droid_hw_module *hw);
void pa_droid_hw_module_unref(pa_droid_hw_module *hw);

void pa_droid_hw_module_lock(pa_droid_hw_module *hw);
bool pa_droid_hw_module_try_lock(pa_droid_hw_module *hw);
void pa_droid_hw_module_unlock(pa_droid_hw_module *hw);

/* Conversion helpers */
typedef enum {
    CONV_FROM_PA,
    CONV_FROM_HAL
} pa_conversion_field_t;

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
 * Return true if default source was found, false if not. */
bool pa_input_device_default_audio_source(audio_devices_t input_device, audio_source_t *default_source);

/* Config parser */
bool pa_parse_droid_audio_config(const char *filename, pa_droid_config_audio *config);
pa_droid_config_audio *pa_droid_config_load(pa_modargs *ma);

const pa_droid_config_output *pa_droid_config_find_output(const pa_droid_config_hw_module *module, const char *name);
const pa_droid_config_input *pa_droid_config_find_input(const pa_droid_config_hw_module *module, const char *name);
const pa_droid_config_hw_module *pa_droid_config_find_module(const pa_droid_config_audio *config, const char* module_id);


/* Profiles */
pa_droid_profile_set *pa_droid_profile_set_new(const pa_droid_config_hw_module *module);
void pa_droid_profile_set_free(pa_droid_profile_set *ps);

pa_droid_profile *pa_droid_profile_new(pa_droid_profile_set *ps, const pa_droid_config_output *output, const pa_droid_config_input *input);
void pa_droid_profile_free(pa_droid_profile *p);

pa_droid_mapping *pa_droid_mapping_get(pa_droid_profile_set *ps, pa_direction_t direction, const void *data);
void pa_droid_mapping_free(pa_droid_mapping *am);

/* Add ports from sinks/sources */
void pa_droid_add_ports(pa_hashmap *ports, pa_droid_mapping *am, pa_card *card);
/* Add ports from card */
void pa_droid_add_card_ports(pa_card_profile *cp, pa_hashmap *ports, pa_droid_mapping *am, pa_core *core);

/* Pretty port names */
bool pa_droid_output_port_name(audio_devices_t value, const char **to_str);
bool pa_droid_input_port_name(audio_devices_t value, const char **to_str);

/* Pretty audio source names */
bool pa_droid_audio_source_name(audio_source_t value, const char **to_str);

#endif
