#ifndef foodroidutilfoo
#define foodroidutilfoo

/*
 * Copyright (C) 2013-2022 Jolla Ltd.
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
#include <pulsecore/modargs.h>

#include <droid/version.h>
#include <droid/droid-config.h>

#define PROP_DROID_DEVICES    "droid.devices"
#define PROP_DROID_FLAGS      "droid.flags"
#define PROP_DROID_HW_MODULE  "droid.hw_module"
#define PROP_DROID_API_STRING "droid-hal"

#define PROP_DROID_OUTPUT_PRIMARY       "droid.output.primary"
#define PROP_DROID_OUTPUT_LOW_LATENCY   "droid.output.low_latency"
#define PROP_DROID_OUTPUT_MEDIA_LATENCY "droid.output.media_latency"
#define PROP_DROID_OUTPUT_OFFLOAD       "droid.output.offload"
#define PROP_DROID_OUTPUT_VOIP          "droid.output.voip"
#define PROP_DROID_INPUT_BUILTIN        "droid.input.builtin"
#define PROP_DROID_INPUT_EXTERNAL       "droid.input.external"
#define PROP_DROID_INPUT_VOIP           "droid.input.voip"

#define EXT_PROP_AUDIO_SOURCE           "audio.source"

#define PA_DROID_PRIMARY_DEVICE     "primary"

typedef struct pa_droid_hw_module pa_droid_hw_module;
typedef struct pa_droid_stream pa_droid_stream;
typedef struct pa_droid_output_stream pa_droid_output_stream;
typedef struct pa_droid_input_stream pa_droid_input_stream;
typedef struct pa_droid_card_data pa_droid_card_data;

typedef struct pa_droid_options pa_droid_options;

enum pa_droid_option_type {
    DM_OPTION_INPUT_ATOI,
    DM_OPTION_CLOSE_INPUT,
    DM_OPTION_UNLOAD_NO_CLOSE,
    DM_OPTION_HW_VOLUME,
    DM_OPTION_REALCALL,
    DM_OPTION_UNLOAD_CALL_EXIT,
    DM_OPTION_OUTPUT_FAST,
    DM_OPTION_OUTPUT_DEEP_BUFFER,
    DM_OPTION_AUDIO_CAL_WAIT,
    DM_OPTION_SPEAKER_BEFORE_VOICE,
    DM_OPTION_OUTPUT_VOIP_RX,
    DM_OPTION_RECORD_VOICE_16K,
    DM_OPTION_USE_LEGACY_STREAM_SET_PARAMETERS,
    DM_OPTION_COUNT
};

struct pa_droid_options {
    bool enabled[DM_OPTION_COUNT];
};

struct pa_droid_hw_module {
    PA_REFCNT_DECLARE;

    pa_core *core;
    char *shared_name;

    dm_config_device *config;
    dm_config_module *enabled_module;
    pa_mutex *hw_mutex;
    pa_mutex *output_mutex;
    pa_mutex *input_mutex;

    struct hw_module_t *hwmod;
    audio_hw_device_t *device;

    const char *module_id;

    uint32_t stream_id;
    bool bt_sco_enabled;

    pa_idxset *outputs;
    pa_idxset *inputs;
    pa_hook_slot *sink_put_hook_slot;
    pa_hook_slot *sink_unlink_hook_slot;

    pa_atomic_t active_outputs;

    pa_droid_options options;

    /* Mode and input control */
    struct _state {
        audio_mode_t mode;
    } state;
};

struct pa_droid_output_stream {
    struct audio_stream_out *stream;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
};

struct pa_droid_input_stream {
    struct audio_stream_in *stream;
    pa_sample_spec default_sample_spec;
    pa_channel_map default_channel_map;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    pa_sample_spec req_sample_spec;
    pa_channel_map req_channel_map;

    audio_source_t audio_source;
    dm_config_port *default_mix_port;
    dm_config_port *input_port;
    pa_droid_stream *active_input;

    uint32_t flags;
    uint32_t device;
    bool first;
};

struct pa_droid_stream {
    PA_REFCNT_DECLARE;

    pa_droid_hw_module *module;
    dm_config_port *mix_port;
    size_t buffer_size;
    void *data;

    audio_io_handle_t io_handle;
    audio_patch_handle_t audio_patch;
    const dm_config_port *active_device_port;

    pa_droid_output_stream *output;
    pa_droid_input_stream *input;
};

struct pa_droid_card_data {
    void *userdata;
    char *module_id;
};


/* Profiles */

typedef struct pa_droid_profile_set pa_droid_profile_set;
typedef struct pa_droid_mapping pa_droid_mapping;

typedef struct pa_droid_port_data {
    dm_config_port *device_port;
} pa_droid_port_data;

typedef struct pa_droid_port {
    pa_droid_mapping *mapping;

    dm_config_port *device_port;
    char *name;
    char *description;
    unsigned priority;
} pa_droid_port;

struct pa_droid_mapping {
    pa_droid_profile_set *profile_set;

    dm_config_module *module;
    dm_config_port *mix_port;
    dm_list *device_ports;

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

    dm_config_module *module;

    char *name;
    char *description;
    unsigned priority;

    /* Idxsets contain pa_droid_mapping objects.
     * Profile doesn't own the mappings, these
     * are references to structs in profile set
     * hashmaps. */
    pa_idxset *output_mappings;
    /* Only one input */
    pa_idxset *input_mappings;
    pa_droid_mapping *input_mapping;

} pa_droid_profile;

struct pa_droid_profile_set {
    dm_config_device *config;

    pa_hashmap *all_ports;
    pa_hashmap *output_mappings;
    pa_hashmap *input_mappings;
    pa_hashmap *profiles;
};

#define PA_DROID_OUTPUT_PARKING "output-parking"
#define PA_DROID_INPUT_PARKING "input-parking"

/* Open hardware module */
/* 'config' can be NULL if it is assumed that hw module with module_id already is open. */
pa_droid_hw_module *pa_droid_hw_module_get(pa_core *core, dm_config_device *config, const char *module_id);
/* First try to get already open hw module and if none found parse config and options from modargs
 * and do initial open. */
pa_droid_hw_module *pa_droid_hw_module_get2(pa_core *core, pa_modargs *ma, const char *module_id);
pa_droid_hw_module *pa_droid_hw_module_ref(pa_droid_hw_module *hw);
void pa_droid_hw_module_unref(pa_droid_hw_module *hw);

void pa_droid_hw_module_lock(pa_droid_hw_module *hw);
bool pa_droid_hw_module_try_lock(pa_droid_hw_module *hw);
void pa_droid_hw_module_unlock(pa_droid_hw_module *hw);

void pa_droid_options_log(pa_droid_hw_module *hw);

static inline bool pa_droid_option(pa_droid_hw_module *hw, enum pa_droid_option_type option) {
    return hw && hw->options.enabled[option];
}

bool pa_droid_hw_set_mode(pa_droid_hw_module *hw_module, audio_mode_t mode);
bool pa_droid_hw_has_mic_control(pa_droid_hw_module *hw);
int pa_droid_hw_mic_get_mute(pa_droid_hw_module *hw_module, bool *muted);
void pa_droid_hw_mic_set_mute(pa_droid_hw_module *hw_module, bool muted);

/* Profiles */
pa_droid_profile_set *pa_droid_profile_set_default_new(dm_config_module *module);
void pa_droid_profile_set_free(pa_droid_profile_set *ps);

void pa_droid_profile_free(pa_droid_profile *p);

bool pa_droid_mapping_is_primary(pa_droid_mapping *am);
/* Go through idxset containing pa_droid_mapping objects and if primary output or input
 * mapping is found, return pointer to that mapping. */
pa_droid_mapping *pa_droid_idxset_get_primary(pa_idxset *i);
void pa_droid_mapping_free(pa_droid_mapping *am);

/* Add ports from sinks/sources.
 * May be called multiple times for one sink/source. */
void pa_droid_add_ports(pa_hashmap *ports, pa_droid_mapping *am, pa_card *card);
/* Add ports from card.
 * May be called multiple times for one card profile. */
void pa_droid_add_card_ports(pa_card_profile *cp, pa_hashmap *ports, pa_droid_mapping *am, pa_core *core);

/* Module operations */
int pa_droid_set_parameters(pa_droid_hw_module *hw, const char *parameters);
pa_droid_stream *pa_droid_hw_primary_output_stream(pa_droid_hw_module *hw);

/* Stream operations */
pa_droid_stream *pa_droid_stream_ref(pa_droid_stream *s);
void pa_droid_stream_unref(pa_droid_stream *s);

int pa_droid_stream_set_parameters(pa_droid_stream *s, const char *parameters);

/* Output stream operations */
pa_droid_stream *pa_droid_open_output_stream(pa_droid_hw_module *module,
                                             const pa_sample_spec *spec,
                                             const pa_channel_map *map,
                                             dm_config_port *mix_port,
                                             dm_config_port *device_port);

/* Set routing to the input or output stream, with following side-effects:
 * Output:
 * - if routing is set to primary output stream, set routing to all other
 *   open streams as well
 * - if routing is set to non-primary stream and primary stream exists, do nothing
 * - if routing is set to non-primary stream and primary stream doesn't exist, set routing
 * Input:
 * - buffer size or channel count may change
 */
int pa_droid_stream_set_route(pa_droid_stream *s, dm_config_port *device_port);

/* Open input stream with currently active routing, sample_spec and channel_map
 * are requests and may change when opening the stream. */
pa_droid_stream *pa_droid_open_input_stream(pa_droid_hw_module *hw_module,
                                            const pa_sample_spec *default_sample_spec,
                                            const pa_channel_map *default_channel_map,
                                            const char *mix_port_name);
/* Test if reconfiguring of input stream is needed */
bool pa_droid_stream_reconfigure_input_needed(pa_droid_stream *s,
                                              const pa_sample_spec *requested_sample_spec,
                                              const pa_channel_map *requested_channel_map,
                                              const pa_proplist *proplist);
bool pa_droid_stream_reconfigure_input(pa_droid_stream *s,
                                       const pa_sample_spec *requested_sample_spec,
                                       const pa_channel_map *requested_channel_map,
                                       const pa_proplist *proplist);
bool pa_droid_hw_set_input_device(pa_droid_stream *s,
                                  dm_config_port *device_port);

const pa_sample_spec *pa_droid_stream_sample_spec(pa_droid_stream *stream);
const pa_channel_map *pa_droid_stream_channel_map(pa_droid_stream *stream);

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

pa_modargs *pa_droid_modargs_new(const char *args, const char* const keys[]);

/* Misc */
size_t pa_droid_buffer_size_round_up(size_t buffer_size, size_t block_size);

#endif
