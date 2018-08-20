#ifndef foodroidutilfoo
#define foodroidutilfoo

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
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/mutex.h>
#include <pulsecore/strlist.h>
#include <pulsecore/atomic.h>

#include "version.h"
#include "droid-config.h"

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

    const pa_droid_config_device *output;
    const pa_droid_config_device *input;
    const pa_droid_config_device *input2;

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
    QUIRK_REALCALL,
    QUIRK_COUNT
};

struct pa_droid_quirks {
    bool enabled[QUIRK_COUNT];
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
void pa_droid_quirk_log(pa_droid_hw_module *hw);

static inline bool pa_droid_quirk(pa_droid_hw_module *hw, enum pa_droid_quirk_type quirk) {
    return hw->quirks && hw->quirks->enabled[quirk];
}

/* Profiles */
pa_droid_profile_set *pa_droid_profile_set_new(const pa_droid_config_hw_module *module);
pa_droid_profile_set *pa_droid_profile_set_default_new(const pa_droid_config_hw_module *module,
                                                       bool merge_inputs);
void pa_droid_profile_set_free(pa_droid_profile_set *ps);

void pa_droid_profile_add_mapping(pa_droid_profile *p, pa_droid_mapping *am);
void pa_droid_profile_free(pa_droid_profile *p);

pa_droid_mapping *pa_droid_mapping_get(pa_droid_profile_set *ps, const pa_droid_config_device *device);
pa_droid_mapping *pa_droid_mapping_merged_get(pa_droid_profile_set *ps,
                                              const pa_droid_config_device *input1,
                                              const pa_droid_config_device *input2);
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
