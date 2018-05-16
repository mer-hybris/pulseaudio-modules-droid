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
#include <pulsecore/strlist.h>
#include <pulsecore/atomic.h>

#include <android-config.h>

#if !defined(ANDROID_VERSION_MAJOR) || !defined(ANDROID_VERSION_MINOR) || !defined(ANDROID_VERSION_PATCH)
#error "ANDROID_VERSION_* not defined. Did you get your headers via extract-headers.sh?"
#endif

#if ANDROID_VERSION_MAJOR == 4 && ANDROID_VERSION_MINOR == 1
#include "droid-util-41qc.h"
#else
#include "droid-util-audio.h"
#endif

/* We currently support API version up-to 3.0 */
#define DROID_API_VERSION_SUPPORT       HARDWARE_DEVICE_API_VERSION(3, 0)

#if AUDIO_DEVICE_API_VERSION_CURRENT > DROID_API_VERSION_SUPPORT
#warning Compiling against higher audio device API version than currently supported!
#warning Compile likely fails or module may malfunction.
#endif

#define AUDIO_API_VERSION_MAJ           ((AUDIO_DEVICE_API_VERSION_CURRENT >> 8) & 0xff)
#define AUDIO_API_VERSION_MIN           (AUDIO_DEVICE_API_VERSION_CURRENT & 0xff)

#define AUDIO_API_VERSION_GET_MAJ(x)    ((x >> 8) & 0xff)
#define AUDIO_API_VERSION_GET_MIN(x)    (x & 0xff)

#if defined(QCOM_BSP) && (AUDIO_API_VERSION_MAJ >= 3)
#define DROID_AUDIO_HAL_USE_VSID
#endif

#define PROP_DROID_DEVICES    "droid.devices"
#define PROP_DROID_FLAGS      "droid.flags"
#define PROP_DROID_HW_MODULE  "droid.hw_module"
#define PROP_DROID_API_STRING "droid-hal"

#define PROP_DROID_OUTPUT_PRIMARY       "droid.output.primary"
#define PROP_DROID_OUTPUT_LOW_LATENCY   "droid.output.low_latency"
#define PROP_DROID_OUTPUT_MEDIA_LATENCY "droid.output.media_latency"
#define PROP_DROID_OUTPUT_OFFLOAD       "droid.output.offload"
#define PROP_DROID_INPUT_BUILTIN        "droid.input.builtin"
#define PROP_DROID_INPUT_EXTERNAL       "droid.input.external"

#define PA_DROID_PRIMARY_DEVICE     "primary"

typedef struct pa_droid_hw_module pa_droid_hw_module;
typedef struct pa_droid_stream pa_droid_stream;
typedef struct pa_droid_output_stream pa_droid_output_stream;
typedef struct pa_droid_input_stream pa_droid_input_stream;
typedef struct pa_droid_card_data pa_droid_card_data;
typedef int (*common_set_parameters_cb_t)(pa_droid_card_data *card_data, const char *str);

typedef struct pa_droid_config_audio pa_droid_config_audio;
typedef struct pa_droid_config_hw_module pa_droid_config_hw_module;

typedef struct pa_droid_quirks pa_droid_quirks;

typedef enum pa_droid_hook {
    PA_DROID_HOOK_INPUT_CHANNEL_MAP_CHANGED,    /* Call data: pa_droid_stream */
    PA_DROID_HOOK_INPUT_BUFFER_SIZE_CHANGED,    /* Call data: pa_droid_stream */
    PA_DROID_HOOK_MAX
} pa_droid_hook_t;


struct pa_droid_hw_module {
    PA_REFCNT_DECLARE;

    pa_core *core;
    char *shared_name;

    pa_droid_config_audio *config;
    const pa_droid_config_hw_module *enabled_module;
    pa_mutex *hw_mutex;
    pa_mutex *output_mutex;
    pa_mutex *input_mutex;

    struct hw_module_t *hwmod;
    audio_hw_device_t *device;

    const char *module_id;

    uint32_t stream_out_id;
    uint32_t stream_in_id;

    pa_idxset *outputs;
    pa_idxset *inputs;
    pa_hook_slot *sink_put_hook_slot;
    pa_hook_slot *sink_unlink_hook_slot;
    pa_hook_slot *source_put_hook_slot;
    pa_hook_slot *source_unlink_hook_slot;

    pa_atomic_t active_outputs;

    pa_droid_quirks *quirks;
    pa_hook hooks[PA_DROID_HOOK_MAX];
};

struct pa_droid_output_stream {
    struct audio_stream_out *stream;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    uint32_t flags;
    uint32_t device;
};

struct pa_droid_input_stream {
    struct audio_stream_in *stream;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    pa_sample_spec input_sample_spec;
    pa_channel_map input_channel_map;
    uint32_t flags;
    uint32_t device;
    audio_devices_t all_devices;
    bool merged;
};

struct pa_droid_stream {
    PA_REFCNT_DECLARE;

    pa_droid_hw_module *module;
    size_t buffer_size;
    void *data;

    pa_droid_output_stream *output;
    pa_droid_input_stream *input;
};

struct pa_droid_card_data {
    void *userdata;
    /* General functions */
    char *module_id;
    common_set_parameters_cb_t set_parameters;
};

#define AUDIO_MAX_SAMPLING_RATES (32)

typedef struct pa_droid_config_global {
    uint32_t audio_hal_version;
    audio_devices_t attached_output_devices;
    audio_devices_t default_output_device;
    audio_devices_t attached_input_devices;
} pa_droid_config_global;

typedef struct pa_droid_config_output {
    const pa_droid_config_hw_module *module;

    char *name;
    uint32_t sampling_rates[AUDIO_MAX_SAMPLING_RATES]; /* (uint32_t) -1 -> dynamic */
    audio_channel_mask_t channel_masks; /* 0 -> dynamic */
    audio_format_t formats; /* 0 -> dynamic */
    audio_devices_t devices;
    audio_output_flags_t flags;

    struct pa_droid_config_output *next;
} pa_droid_config_output;

typedef struct pa_droid_config_input {
    const pa_droid_config_hw_module *module;

    char *name;
    uint32_t sampling_rates[AUDIO_MAX_SAMPLING_RATES]; /* (uint32_t) -1 -> dynamic */
    audio_channel_mask_t channel_masks; /* 0 -> dynamic */
    audio_format_t formats; /* 0 -> dynamic */
    audio_devices_t devices;
#if AUDIO_API_VERSION_MAJ >= 3
    audio_input_flags_t flags;
#endif

    struct pa_droid_config_input *next;
} pa_droid_config_input;

struct pa_droid_config_hw_module {
    const pa_droid_config_audio *config;

    char *name;
    /* If global config is not defined for module, use root global config. */
    pa_droid_config_global *global_config;
    pa_droid_config_output *outputs;
    pa_droid_config_input *inputs;

    struct pa_droid_config_hw_module *next;
};

struct pa_droid_config_audio {
    pa_droid_config_global *global_config;
    pa_droid_config_hw_module *hw_modules;
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
    const pa_droid_config_input *input2;

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

    /* Idxsets contain pa_droid_mapping objects.
     * Profile doesn't own the mappings. */
    pa_idxset *output_mappings;
    pa_idxset *input_mappings;

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

enum pa_droid_quirk_type {
    QUIRK_INPUT_ATOI,
    QUIRK_SET_PARAMETERS,
    QUIRK_CLOSE_INPUT,
    QUIRK_UNLOAD_NO_CLOSE,
    QUIRK_NO_HW_VOLUME,
    QUIRK_OUTPUT_MAKE_WRITABLE,
    QUIRK_COUNT
};

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

bool pa_droid_quirk_parse(pa_droid_hw_module *hw, const char *quirks);
bool pa_droid_quirk(pa_droid_hw_module *hw, enum pa_droid_quirk_type quirk);
void pa_droid_quirk_log(pa_droid_hw_module *hw);

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
pa_droid_profile_set *pa_droid_profile_set_default_new(const pa_droid_config_hw_module *module,
                                                       bool merge_inputs);
void pa_droid_profile_set_free(pa_droid_profile_set *ps);

void pa_droid_profile_add_mapping(pa_droid_profile *p, pa_droid_mapping *am);
void pa_droid_profile_free(pa_droid_profile *p);

pa_droid_mapping *pa_droid_mapping_get(pa_droid_profile_set *ps, pa_direction_t direction, const void *data);
pa_droid_mapping *pa_droid_mapping_merged_get(pa_droid_profile_set *ps,
                                              const pa_droid_config_input *input1,
                                              const pa_droid_config_input *input2);
bool pa_droid_mapping_is_primary(pa_droid_mapping *am);
/* Go through idxset containing pa_droid_mapping objects and if primary output or input
 * mapping is found, return pointer to that mapping. */
pa_droid_mapping *pa_droid_idxset_get_primary(pa_idxset *i);
pa_droid_mapping *pa_droid_idxset_mapping_with_device(pa_idxset *i, uint32_t flag);
void pa_droid_mapping_free(pa_droid_mapping *am);

/* Add ports from sinks/sources.
 * May be called multiple times for one sink/source. */
void pa_droid_add_ports(pa_hashmap *ports, pa_droid_mapping *am, pa_card *card);
/* Add ports from card.
 * May be called multiple times for one card profile. */
void pa_droid_add_card_ports(pa_card_profile *cp, pa_hashmap *ports, pa_droid_mapping *am, pa_core *core);

/* Pretty port names */
bool pa_droid_output_port_name(audio_devices_t value, const char **to_str);
bool pa_droid_input_port_name(audio_devices_t value, const char **to_str);

/* Pretty audio source names */
bool pa_droid_audio_source_name(audio_source_t value, const char **to_str);

pa_hook *pa_droid_hooks(pa_droid_hw_module *hw);

/* Module operations */
int pa_droid_set_parameters(pa_droid_hw_module *hw, const char *parameters);

/* Stream operations */
pa_droid_stream *pa_droid_stream_ref(pa_droid_stream *s);
void pa_droid_stream_unref(pa_droid_stream *s);

int pa_droid_stream_set_parameters(pa_droid_stream *s, const char *parameters);

/* Output stream operations */
pa_droid_stream *pa_droid_open_output_stream(pa_droid_hw_module *module,
                                             const pa_sample_spec *spec,
                                             const pa_channel_map *map,
                                             audio_output_flags_t flags,
                                             audio_devices_t devices);

/* Set routing to the input or output stream, with following side-effects:
 * Output:
 * - if routing is set to primary output stream, set routing to all other
 *   open streams as well
 * - if routing is set to non-primary stream and primary stream exists, do nothing
 * - if routing is set to non-primary stream and primary stream doesn't exist, set routing
 * Input:
 * - buffer size or channel count may change
 */
int pa_droid_stream_set_route(pa_droid_stream *s, audio_devices_t device);

/* Input stream operations */
pa_droid_stream *pa_droid_open_input_stream(pa_droid_hw_module *module,
                                            const pa_sample_spec *spec,
                                            const pa_channel_map *map,
                                            audio_devices_t devices,
                                            pa_droid_mapping *am);

bool pa_droid_stream_is_primary(pa_droid_stream *s);

int pa_droid_stream_suspend(pa_droid_stream *s, bool suspend);

size_t pa_droid_stream_buffer_size(pa_droid_stream *s);
pa_usec_t pa_droid_stream_get_latency(pa_droid_stream *s);

static inline int pa_droid_output_stream_any_active(pa_droid_stream *s) {
    return pa_atomic_load(&s->module->active_outputs);
}

static inline ssize_t pa_droid_stream_write(pa_droid_stream *stream, const void *buffer, size_t bytes) {
    return stream->output->stream->write(stream->output->stream, buffer, bytes);
}

static inline ssize_t pa_droid_stream_read(pa_droid_stream *stream, void *buffer, size_t bytes) {
    return stream->input->stream->read(stream->input->stream, buffer, bytes);
}

void pa_droid_stream_set_data(pa_droid_stream *s, void *data);
void *pa_droid_stream_get_data(pa_droid_stream *s);
bool pa_sink_is_droid_sink(pa_sink *sink);
bool pa_source_is_droid_source(pa_source *source);

/* Misc */
size_t pa_droid_buffer_size_round_up(size_t buffer_size, size_t block_size);

#endif
