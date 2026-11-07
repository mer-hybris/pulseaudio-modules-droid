/*
 * aidl-hal-wrapper.c — synthesize audio_hw_device_t/audio_stream_out/in
 * whose function pointers forward to the AIDL HAL client in aidl-binder.c.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include <hardware/audio.h>

#include "droid/aidl-config-builder.h"
#include "droid/aidl-binder.h"
#include "droid/aidl-hal-wrapper.h"
#include "droid/droid-config.h"
#include "droid/sllist.h"

/* ========================================================================== *
 *  Wrapper structs (legacy struct must be first member for casting).
 * ========================================================================== */

struct aidl_hal_wrapper {
    struct audio_hw_device base;
    aidl_audio_module *aidl_module;
    dm_config_device *config;
    pa_hashmap *aidl_port_ids;     /* dm_config_port name -> port_id_entry */
    pa_hashmap *io_streams_out;    /* audio_io_handle_t -> aidl_stream_out_wrapper* */
    pa_hashmap *io_streams_in;     /* audio_io_handle_t -> aidl_stream_in_wrapper* */
    pa_hashmap *device_patches;    /* patch id -> aidl_device_patch* */
    pthread_mutex_t mtx;
};

struct aidl_stream_out_wrapper {
    struct audio_stream_out base;
    struct aidl_hal_wrapper *parent;
    aidl_audio_stream *aidl;
    int mix_port_config_id;
    int device_port_config_id;
    int patch_id;
    audio_io_handle_t io_handle;
    /* Cached metadata reported back through common.* getters */
    uint32_t sample_rate;
    audio_channel_mask_t channel_mask;
    audio_format_t format;
    size_t buffer_size;
};

struct aidl_stream_in_wrapper {
    struct audio_stream_in base;
    struct aidl_hal_wrapper *parent;
    aidl_audio_stream *aidl;
    int mix_port_config_id;
    int device_port_config_id;
    int patch_id;
    audio_io_handle_t io_handle;
    uint32_t sample_rate;
    audio_channel_mask_t channel_mask;
    audio_format_t format;
    size_t buffer_size;
};

struct aidl_device_patch {
    int patch_id;
    int source_port_config_id;
    int sink_port_config_id;
    bool reset_source_port_config;
    bool reset_sink_port_config;
};

struct port_id_entry { int aidl_id; };

/* ========================================================================== *
 *  Helpers
 * ========================================================================== */

static int aidl_format_id_for(audio_format_t legacy) {
    switch (legacy) {
        case AUDIO_FORMAT_PCM_16_BIT:        return AIDL_AUDIO_FORMAT_S16LE;
        case AUDIO_FORMAT_PCM_32_BIT:        return AIDL_AUDIO_FORMAT_S32LE;
        case AUDIO_FORMAT_PCM_FLOAT:         return AIDL_AUDIO_FORMAT_FLOAT32LE;
        case AUDIO_FORMAT_PCM_24_BIT_PACKED: return AIDL_AUDIO_FORMAT_S24LE;
        case AUDIO_FORMAT_PCM_8_BIT:         return AIDL_AUDIO_FORMAT_U8;
        default:                             return AIDL_AUDIO_FORMAT_S16LE;
    }
}

static audio_format_t legacy_format_for_aidl(int aidl) {
    switch (aidl) {
        case AIDL_AUDIO_FORMAT_S16LE:     return AUDIO_FORMAT_PCM_16_BIT;
        case AIDL_AUDIO_FORMAT_S32LE:     return AUDIO_FORMAT_PCM_32_BIT;
        case AIDL_AUDIO_FORMAT_FLOAT32LE: return AUDIO_FORMAT_PCM_FLOAT;
        case AIDL_AUDIO_FORMAT_S24LE:     return AUDIO_FORMAT_PCM_24_BIT_PACKED;
        case AIDL_AUDIO_FORMAT_U8:        return AUDIO_FORMAT_PCM_8_BIT;
        default:                          return AUDIO_FORMAT_DEFAULT;
    }
}

static int aidl_channel_mask_for(audio_channel_mask_t legacy, bool is_input) {
    if (!is_input)
        return (int)legacy;

    switch (audio_channel_count_from_in_mask(legacy)) {
        case 1:  return AUDIO_CHANNEL_OUT_MONO;
        case 2:  return AUDIO_CHANNEL_OUT_STEREO;
        default: return (int)legacy;
    }
}

static audio_channel_mask_t legacy_channel_mask_for_aidl(int aidl, bool is_input) {
    if (!is_input)
        return (audio_channel_mask_t)aidl;

    return droid_aidl_channel_mask_legacy(aidl, true);
}

static void apply_suggested_config(struct audio_config *config,
                                   const struct aidl_port_config *suggested,
                                   bool is_input) {
    audio_format_t legacy_format;

    pa_assert(config);
    pa_assert(suggested);

    if (suggested->sample_rate > 0)
        config->sample_rate = (uint32_t)suggested->sample_rate;

    if (suggested->channel_mask != 0)
        config->channel_mask = legacy_channel_mask_for_aidl(suggested->channel_mask, is_input);

    if (suggested->format >= 0 &&
        (legacy_format = legacy_format_for_aidl(suggested->format)) != AUDIO_FORMAT_DEFAULT)
        config->format = legacy_format;
}

static int aidl_port_id_for(struct aidl_hal_wrapper *w, const dm_config_port *p) {
    if (!p) return 0;
    struct port_id_entry *e = pa_hashmap_get(w->aidl_port_ids, p->name);
    return e ? e->aidl_id : 0;
}

/* Look up mix port in our config by direction + flags match. */
static dm_config_port *find_mix_port(struct aidl_hal_wrapper *w,
                                     bool is_output, uint32_t flags) {
    dm_config_module *m;
    void *mstate;
    DM_LIST_FOREACH_DATA(m, w->config->modules, mstate) {
        dm_config_port *p;
        void *pstate;
        DM_LIST_FOREACH_DATA(p, m->mix_ports, pstate) {
            bool match_dir = is_output ? (p->role == DM_CONFIG_ROLE_SOURCE)
                                       : (p->role == DM_CONFIG_ROLE_SINK);
            if (!match_dir) continue;
            if (p->flags == flags) return p;
        }
    }
    /* Fallback: any matching direction. */
    DM_LIST_FOREACH_DATA(m, w->config->modules, mstate) {
        dm_config_port *p;
        void *pstate;
        DM_LIST_FOREACH_DATA(p, m->mix_ports, pstate) {
            bool match_dir = is_output ? (p->role == DM_CONFIG_ROLE_SOURCE)
                                       : (p->role == DM_CONFIG_ROLE_SINK);
            if (match_dir) return p;
        }
    }
    return NULL;
}

static dm_config_port *find_device_port_by_type(struct aidl_hal_wrapper *w,
                                                audio_devices_t device_bits) {
    dm_config_module *m;
    void *mstate;
    DM_LIST_FOREACH_DATA(m, w->config->modules, mstate) {
        dm_config_port *p;
        void *pstate;
        DM_LIST_FOREACH_DATA(p, m->device_ports, pstate) {
            if (p->type == device_bits) return p;
            /* Match if device_bits is a subset (caller might pass a single bit). */
            if ((p->type & device_bits) == device_bits && device_bits != 0)
                return p;
        }
    }
    return NULL;
}

static int set_port_config_by_aidl_id(struct aidl_hal_wrapper *w, int port_id) {
    struct aidl_port_config cfg = { .config_id = 0, .port_id = port_id,
                                    .sample_rate = 0, .channel_mask = 0,
                                    .format = -1, .io_flags = -1 };

    if (port_id <= 0)
        return -ENOENT;

    struct aidl_port_config suggested;
    int applied;
    if (aidl_audio_module_set_port_config(w->aidl_module, &cfg, &suggested, &applied) < 0)
        return -EIO;

    if (!applied || suggested.config_id <= 0)
        return -EIO;

    return suggested.config_id;
}

static int create_tracked_patch(struct aidl_hal_wrapper *w,
                                int source_config_id,
                                bool reset_source_config,
                                int sink_config_id,
                                bool reset_sink_config,
                                audio_patch_handle_t *handle) {
    int src_ids[1] = { source_config_id };
    int snk_ids[1] = { sink_config_id };
    int new_patch = 0;
    struct aidl_device_patch *patch;

    pa_assert(w);

    if (source_config_id <= 0 || sink_config_id <= 0)
        return -EINVAL;

    if (aidl_audio_module_create_patch(w->aidl_module, src_ids, 1, snk_ids, 1, &new_patch) < 0)
        return -EIO;

    patch = pa_xnew0(struct aidl_device_patch, 1);
    patch->patch_id = new_patch;
    patch->source_port_config_id = source_config_id;
    patch->sink_port_config_id = sink_config_id;
    patch->reset_source_port_config = reset_source_config;
    patch->reset_sink_port_config = reset_sink_config;

    pthread_mutex_lock(&w->mtx);
    pa_hashmap_put(w->device_patches, (void *)(intptr_t)new_patch, patch);
    pthread_mutex_unlock(&w->mtx);

    if (handle)
        *handle = new_patch;

    return 0;
}

static int create_hostless_patch(struct aidl_hal_wrapper *w,
                                 const struct audio_port_config *source,
                                 const struct audio_port_config *sink,
                                 audio_patch_handle_t *handle) {
    int source_config_id, sink_config_id, ret;

    pa_assert(w);
    pa_assert(source);
    pa_assert(sink);

    source_config_id = set_port_config_by_aidl_id(w, source->id);
    if (source_config_id <= 0) {
        pa_log_warn("aidl-hal-wrapper: hostless source port config failed for port id %d (%d)",
                    source->id, source_config_id);
        return source_config_id < 0 ? source_config_id : -EIO;
    }

    sink_config_id = set_port_config_by_aidl_id(w, sink->id);
    if (sink_config_id <= 0) {
        pa_log_warn("aidl-hal-wrapper: hostless sink port config failed for port id %d (%d)",
                    sink->id, sink_config_id);
        aidl_audio_module_reset_port_config(w->aidl_module, source_config_id);
        return sink_config_id < 0 ? sink_config_id : -EIO;
    }

    pa_log_warn("aidl-hal-wrapper: creating hostless patch config %d -> config %d",
                source_config_id, sink_config_id);

    ret = create_tracked_patch(w,
                               source_config_id, true,
                               sink_config_id, true,
                               handle);
    if (ret < 0) {
        aidl_audio_module_reset_port_config(w->aidl_module, sink_config_id);
        aidl_audio_module_reset_port_config(w->aidl_module, source_config_id);
    }

    return ret;
}

/* ========================================================================== *
 *  audio_stream_out function pointers
 * ========================================================================== */

#define OUT_FROM_BASE(s) ((struct aidl_stream_out_wrapper *)(s))
#define OUT_FROM_COMMON(s) \
    ((struct aidl_stream_out_wrapper *)((char *)(s) - offsetof(struct audio_stream_out, common)))

static uint32_t out_get_sample_rate(const struct audio_stream *s) {
    struct aidl_stream_out_wrapper *w = OUT_FROM_COMMON((struct audio_stream *)s);
    return w->sample_rate;
}
static int out_set_sample_rate(struct audio_stream *s, uint32_t r) {
    (void)s; (void)r; return -ENOSYS;
}
static size_t out_get_buffer_size(const struct audio_stream *s) {
    struct aidl_stream_out_wrapper *w = OUT_FROM_COMMON((struct audio_stream *)s);
    return w->buffer_size;
}
static audio_channel_mask_t out_get_channels(const struct audio_stream *s) {
    struct aidl_stream_out_wrapper *w = OUT_FROM_COMMON((struct audio_stream *)s);
    return w->channel_mask;
}
static audio_format_t out_get_format(const struct audio_stream *s) {
    struct aidl_stream_out_wrapper *w = OUT_FROM_COMMON((struct audio_stream *)s);
    return w->format;
}
static int out_set_format(struct audio_stream *s, audio_format_t f) {
    (void)s; (void)f; return -ENOSYS;
}
static int out_standby(struct audio_stream *s) {
    struct aidl_stream_out_wrapper *w = OUT_FROM_COMMON(s);
    return aidl_audio_stream_standby(w->aidl);
}
static int out_dump(const struct audio_stream *s, int fd) { (void)s; (void)fd; return 0; }
static int out_set_parameters(struct audio_stream *s, const char *kv) {
    /* Routing uses create_audio_patch on the device. */
    (void)s; (void)kv;
    return 0;
}
static char *out_get_parameters(const struct audio_stream *s, const char *keys) {
    (void)s; (void)keys;
    return strdup("");
}
static int out_add_audio_effect(const struct audio_stream *s, effect_handle_t e) {
    (void)s; (void)e; return 0;
}
static int out_remove_audio_effect(const struct audio_stream *s, effect_handle_t e) {
    (void)s; (void)e; return 0;
}

static uint32_t out_get_latency(const struct audio_stream_out *s) {
    struct aidl_stream_out_wrapper *w = OUT_FROM_BASE((struct audio_stream_out *)s);
    return (uint32_t)aidl_audio_stream_get_latency_ms(w->aidl);
}
static int out_set_volume(struct audio_stream_out *s, float left, float right) {
    struct aidl_stream_out_wrapper *w = OUT_FROM_BASE(s);
    return aidl_audio_stream_set_hw_volume(w->aidl, left, right);
}
static ssize_t out_write(struct audio_stream_out *s, const void *buffer, size_t bytes) {
    struct aidl_stream_out_wrapper *w = OUT_FROM_BASE(s);
    int rc = aidl_audio_stream_write(w->aidl, buffer, (int)bytes);
    return rc;
}
static int out_get_render_position(const struct audio_stream_out *s, uint32_t *frames) {
    struct aidl_stream_out_wrapper *w = OUT_FROM_BASE((struct audio_stream_out *)s);

    if (!frames)
        return -EINVAL;

    *frames = 0;

    if (!w->aidl)
        return -ENOTCONN;

    return aidl_audio_stream_get_render_position(w->aidl, frames);
}
static int out_get_next_write_timestamp(const struct audio_stream_out *s, int64_t *ts) {
    (void)s; if (ts) *ts = 0; return -ENOSYS;
}

/* ========================================================================== *
 *  audio_stream_in function pointers
 * ========================================================================== */

#define IN_FROM_BASE(s) ((struct aidl_stream_in_wrapper *)(s))
#define IN_FROM_COMMON(s) \
    ((struct aidl_stream_in_wrapper *)((char *)(s) - offsetof(struct audio_stream_in, common)))

static uint32_t in_get_sample_rate(const struct audio_stream *s) {
    return IN_FROM_COMMON((struct audio_stream *)s)->sample_rate;
}
static int in_set_sample_rate(struct audio_stream *s, uint32_t r) {
    (void)s; (void)r; return -ENOSYS;
}
static size_t in_get_buffer_size(const struct audio_stream *s) {
    return IN_FROM_COMMON((struct audio_stream *)s)->buffer_size;
}
static audio_channel_mask_t in_get_channels(const struct audio_stream *s) {
    return IN_FROM_COMMON((struct audio_stream *)s)->channel_mask;
}
static audio_format_t in_get_format(const struct audio_stream *s) {
    return IN_FROM_COMMON((struct audio_stream *)s)->format;
}
static int in_set_format(struct audio_stream *s, audio_format_t f) {
    (void)s; (void)f; return -ENOSYS;
}
static int in_standby(struct audio_stream *s) {
    struct aidl_stream_in_wrapper *w = IN_FROM_COMMON(s);
    return aidl_audio_stream_standby(w->aidl);
}
static int in_dump(const struct audio_stream *s, int fd) { (void)s; (void)fd; return 0; }
static int in_set_parameters(struct audio_stream *s, const char *kv) { (void)s; (void)kv; return 0; }
static char *in_get_parameters(const struct audio_stream *s, const char *keys) {
    (void)s; (void)keys; return strdup("");
}
static int in_add_audio_effect(const struct audio_stream *s, effect_handle_t e) { (void)s; (void)e; return 0; }
static int in_remove_audio_effect(const struct audio_stream *s, effect_handle_t e) { (void)s; (void)e; return 0; }

static int in_set_gain(struct audio_stream_in *s, float g) { (void)s; (void)g; return -ENOSYS; }
static ssize_t in_read(struct audio_stream_in *s, void *buffer, size_t bytes) {
    struct aidl_stream_in_wrapper *w = IN_FROM_BASE(s);
    int rc = aidl_audio_stream_read(w->aidl, buffer, (int)bytes);
    return rc;
}
static uint32_t in_get_input_frames_lost(struct audio_stream_in *s) { (void)s; return 0; }

static void init_stream_out_ops(struct audio_stream_out *s) {
    s->common.get_sample_rate     = out_get_sample_rate;
    s->common.set_sample_rate     = out_set_sample_rate;
    s->common.get_buffer_size     = out_get_buffer_size;
    s->common.get_channels        = out_get_channels;
    s->common.get_format          = out_get_format;
    s->common.set_format          = out_set_format;
    s->common.standby             = out_standby;
    s->common.dump                = out_dump;
    s->common.set_parameters      = out_set_parameters;
    s->common.get_parameters      = out_get_parameters;
    s->common.add_audio_effect    = out_add_audio_effect;
    s->common.remove_audio_effect = out_remove_audio_effect;
    s->get_latency                = out_get_latency;
    s->set_volume                 = out_set_volume;
    s->write                      = out_write;
    s->get_render_position        = out_get_render_position;
    s->get_next_write_timestamp   = out_get_next_write_timestamp;
}

static void init_stream_in_ops(struct audio_stream_in *s) {
    s->common.get_sample_rate     = in_get_sample_rate;
    s->common.set_sample_rate     = in_set_sample_rate;
    s->common.get_buffer_size     = in_get_buffer_size;
    s->common.get_channels        = in_get_channels;
    s->common.get_format          = in_get_format;
    s->common.set_format          = in_set_format;
    s->common.standby             = in_standby;
    s->common.dump                = in_dump;
    s->common.set_parameters      = in_set_parameters;
    s->common.get_parameters      = in_get_parameters;
    s->common.add_audio_effect    = in_add_audio_effect;
    s->common.remove_audio_effect = in_remove_audio_effect;
    s->set_gain                   = in_set_gain;
    s->read                       = in_read;
    s->get_input_frames_lost      = in_get_input_frames_lost;
}

static void close_output_stream_resources(struct aidl_stream_out_wrapper *sw) {
    struct aidl_hal_wrapper *w;

    pa_assert(sw);
    pa_assert(sw->parent);

    w = sw->parent;

    if (sw->aidl) {
        aidl_audio_stream_close(sw->aidl);
        sw->aidl = NULL;
    }
    if (sw->patch_id) {
        aidl_audio_module_release_patch(w->aidl_module, sw->patch_id);
        sw->patch_id = 0;
    }
    if (sw->mix_port_config_id) {
        aidl_audio_module_reset_port_config(w->aidl_module, sw->mix_port_config_id);
        sw->mix_port_config_id = 0;
    }
    if (sw->device_port_config_id) {
        aidl_audio_module_reset_port_config(w->aidl_module, sw->device_port_config_id);
        sw->device_port_config_id = 0;
    }
}

static void close_input_stream_resources(struct aidl_stream_in_wrapper *sw) {
    struct aidl_hal_wrapper *w;

    pa_assert(sw);
    pa_assert(sw->parent);

    w = sw->parent;

    if (sw->aidl) {
        aidl_audio_stream_close(sw->aidl);
        sw->aidl = NULL;
    }
    if (sw->patch_id) {
        aidl_audio_module_release_patch(w->aidl_module, sw->patch_id);
        sw->patch_id = 0;
    }
    if (sw->mix_port_config_id) {
        aidl_audio_module_reset_port_config(w->aidl_module, sw->mix_port_config_id);
        sw->mix_port_config_id = 0;
    }
    if (sw->device_port_config_id) {
        aidl_audio_module_reset_port_config(w->aidl_module, sw->device_port_config_id);
        sw->device_port_config_id = 0;
    }
}

/* ========================================================================== *
 *  audio_hw_device_t function pointers
 * ========================================================================== */

#define DEV_FROM_BASE(d) ((struct aidl_hal_wrapper *)(d))

static uint32_t dev_get_supported_devices(const struct audio_hw_device *d) {
    (void)d; return 0;  /* Legacy "every device supported" sentinel. */
}
static int dev_init_check(const struct audio_hw_device *d) { (void)d; return 0; }

static int dev_set_voice_volume(struct audio_hw_device *d, float v) {
    return aidl_audio_module_set_voice_volume(DEV_FROM_BASE(d)->aidl_module, v);
}
static int dev_set_master_volume(struct audio_hw_device *d, float v) {
    return aidl_audio_module_set_master_volume(DEV_FROM_BASE(d)->aidl_module, v);
}
static int dev_get_master_volume(struct audio_hw_device *d, float *v) {
    return aidl_audio_module_get_master_volume(DEV_FROM_BASE(d)->aidl_module, v);
}
static int dev_set_master_mute(struct audio_hw_device *d, bool m) {
    return aidl_audio_module_set_master_mute(DEV_FROM_BASE(d)->aidl_module, m);
}
static int dev_get_master_mute(struct audio_hw_device *d, bool *m) {
    return aidl_audio_module_get_master_mute(DEV_FROM_BASE(d)->aidl_module, m);
}
static int dev_set_mode(struct audio_hw_device *d, audio_mode_t m) {
    return aidl_audio_module_set_mode(DEV_FROM_BASE(d)->aidl_module, (int)m);
}
static int dev_set_mic_mute(struct audio_hw_device *d, bool m) {
    return aidl_audio_module_set_mic_mute(DEV_FROM_BASE(d)->aidl_module, m);
}
static int dev_get_mic_mute(const struct audio_hw_device *d, bool *m) {
    return aidl_audio_module_get_mic_mute(
            DEV_FROM_BASE((struct audio_hw_device *)d)->aidl_module, m);
}
static int dev_set_parameters(struct audio_hw_device *d, const char *kv) {
    return aidl_audio_module_set_vendor_parameters(DEV_FROM_BASE(d)->aidl_module, kv);
}
static char *dev_get_parameters(const struct audio_hw_device *d, const char *keys) {
    return aidl_audio_module_get_vendor_parameters(
            DEV_FROM_BASE((struct audio_hw_device *)d)->aidl_module, keys);
}
static size_t dev_get_input_buffer_size(const struct audio_hw_device *d,
                                        const struct audio_config *cfg) {
    (void)d; (void)cfg;
    return 0;  /* Caller falls back to stream's reported buffer size. */
}

static int dev_open_output_stream(struct audio_hw_device *d,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  audio_output_flags_t flags,
                                  struct audio_config *config,
                                  struct audio_stream_out **stream_out,
                                  const char *address) {
    (void)address;
    struct aidl_hal_wrapper *w = DEV_FROM_BASE(d);

    dm_config_port *mix = find_mix_port(w, /*is_output=*/true, (uint32_t)flags);
    if (!mix) { pa_log("aidl-hal-wrapper: no mix port for output flags 0x%x", flags); return -ENOENT; }

    dm_config_port *dev = find_device_port_by_type(w, devices);
    if (!dev) { pa_log("aidl-hal-wrapper: no device port for type 0x%x", devices); return -ENOENT; }

    int mix_aidl  = aidl_port_id_for(w, mix);
    int dev_aidl  = aidl_port_id_for(w, dev);
    if (!mix_aidl || !dev_aidl) {
        pa_log("aidl-hal-wrapper: missing AIDL port ids for mix=%s dev=%s",
               mix->name, dev->name);
        return -ENOENT;
    }

    /* Configure device port (let HAL pick most fields). */
    struct aidl_port_config dev_cfg = { .config_id = 0, .port_id = dev_aidl,
                                        .sample_rate = 0, .channel_mask = 0,
                                        .format = -1, .io_flags = -1 };
    struct aidl_port_config dev_suggested; int dev_applied;
    if (aidl_audio_module_set_port_config(w->aidl_module, &dev_cfg, &dev_suggested, &dev_applied) < 0)
        return -EIO;
    if (!dev_applied || dev_suggested.config_id <= 0)
        return -EIO;

    /* Configure mix port with requested params. */
    struct aidl_port_config mix_cfg = { .config_id = 0, .port_id = mix_aidl,
                                        .sample_rate = (int)config->sample_rate,
                                        .channel_mask = (int)config->channel_mask,
                                        .format = aidl_format_id_for(config->format),
                                        .io_flags = (int)flags };
    struct aidl_port_config mix_suggested; int mix_applied;
    if (aidl_audio_module_set_port_config(w->aidl_module, &mix_cfg, &mix_suggested, &mix_applied) < 0) {
        aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
        return -EIO;
    }
    if (!mix_applied || mix_suggested.config_id <= 0) {
        aidl_audio_module_reset_port_config(w->aidl_module, mix_suggested.config_id);
        aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
        return -EIO;
    }
    apply_suggested_config(config, &mix_suggested, false);

    /* Patch: mix -> device. */
    int patch_id = 0;
    int src_ids[1] = { mix_suggested.config_id };
    int snk_ids[1] = { dev_suggested.config_id };
    if (aidl_audio_module_create_patch(w->aidl_module, src_ids, 1, snk_ids, 1, &patch_id) < 0) {
        aidl_audio_module_reset_port_config(w->aidl_module, mix_suggested.config_id);
        aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
        return -EIO;
    }

    /* Open the stream. */
    struct aidl_stream_params sp = { .mix_port_config_id = mix_suggested.config_id,
                                     .audio_usage = 0, .audio_content_type = 0,
                                     .audio_source = 0, .requested_buffer_frames = 0 };
    struct aidl_stream_info info = {0};
    aidl_audio_stream *aidl_stream = aidl_audio_open_output_stream(w->aidl_module, &sp, &info);
    if (!aidl_stream) {
        aidl_audio_module_release_patch(w->aidl_module, patch_id);
        aidl_audio_module_reset_port_config(w->aidl_module, mix_suggested.config_id);
        aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
        return -EIO;
    }

    struct aidl_stream_out_wrapper *sw = pa_xnew0(struct aidl_stream_out_wrapper, 1);
    sw->parent = w;
    sw->aidl = aidl_stream;
    sw->mix_port_config_id = mix_suggested.config_id;
    sw->device_port_config_id = dev_suggested.config_id;
    sw->patch_id = patch_id;
    sw->io_handle = handle;
    sw->sample_rate = config->sample_rate;
    sw->channel_mask = config->channel_mask;
    sw->format = config->format;
    sw->buffer_size = (size_t)info.buffer_frames * (size_t)info.frame_bytes;
    init_stream_out_ops(&sw->base);

    pthread_mutex_lock(&w->mtx);
    pa_hashmap_put(w->io_streams_out, (void *)(intptr_t)handle, sw);
    pthread_mutex_unlock(&w->mtx);

    *stream_out = &sw->base;
    pa_log_info("aidl-hal-wrapper: opened output stream handle=%d mix=%s dev=%s "
                "(rate=%u ch=0x%x fmt=0x%x buf=%zu)",
                handle, mix->name, dev->name,
                sw->sample_rate, sw->channel_mask, sw->format, sw->buffer_size);
    return 0;
}

static void dev_close_output_stream(struct audio_hw_device *d, struct audio_stream_out *s) {
    struct aidl_hal_wrapper *w = DEV_FROM_BASE(d);
    struct aidl_stream_out_wrapper *sw = OUT_FROM_BASE(s);

    pthread_mutex_lock(&w->mtx);
    pa_hashmap_remove(w->io_streams_out, (void *)(intptr_t)sw->io_handle);
    pthread_mutex_unlock(&w->mtx);

    close_output_stream_resources(sw);
    pa_xfree(sw);
}

static int dev_open_input_stream(struct audio_hw_device *d,
                                 audio_io_handle_t handle,
                                 audio_devices_t devices,
                                 struct audio_config *config,
                                 struct audio_stream_in **stream_in,
                                 audio_input_flags_t flags,
                                 const char *address,
                                 audio_source_t source) {
    (void)address;
    struct aidl_hal_wrapper *w = DEV_FROM_BASE(d);

    dm_config_port *mix = find_mix_port(w, /*is_output=*/false, (uint32_t)flags);
    if (!mix) { pa_log("aidl-hal-wrapper: no mix port for input flags 0x%x", flags); return -ENOENT; }

    dm_config_port *dev = find_device_port_by_type(w, devices);
    if (!dev) { pa_log("aidl-hal-wrapper: no device port for type 0x%x", devices); return -ENOENT; }

    int mix_aidl = aidl_port_id_for(w, mix);
    int dev_aidl = aidl_port_id_for(w, dev);
    if (!mix_aidl || !dev_aidl) return -ENOENT;

    struct aidl_port_config dev_cfg = { .config_id = 0, .port_id = dev_aidl,
                                        .sample_rate = 0, .channel_mask = 0,
                                        .format = -1, .io_flags = -1 };
    struct aidl_port_config dev_suggested; int dev_applied;
    if (aidl_audio_module_set_port_config(w->aidl_module, &dev_cfg, &dev_suggested, &dev_applied) < 0)
        return -EIO;
    if (!dev_applied || dev_suggested.config_id <= 0)
        return -EIO;

    struct aidl_port_config mix_cfg = { .config_id = 0, .port_id = mix_aidl,
                                        .sample_rate = (int)config->sample_rate,
                                        .channel_mask = aidl_channel_mask_for(config->channel_mask, true),
                                        .format = aidl_format_id_for(config->format),
                                        .io_flags = (int)flags };
    struct aidl_port_config mix_suggested; int mix_applied;
    if (aidl_audio_module_set_port_config(w->aidl_module, &mix_cfg, &mix_suggested, &mix_applied) < 0) {
        aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
        return -EIO;
    }
    if (!mix_applied || mix_suggested.config_id <= 0) {
        aidl_audio_module_reset_port_config(w->aidl_module, mix_suggested.config_id);
        aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
        return -EIO;
    }
    apply_suggested_config(config, &mix_suggested, true);

    /* Patch: device -> mix (input direction). */
    int patch_id = 0;
    int src_ids[1] = { dev_suggested.config_id };
    int snk_ids[1] = { mix_suggested.config_id };
    if (aidl_audio_module_create_patch(w->aidl_module, src_ids, 1, snk_ids, 1, &patch_id) < 0) {
        aidl_audio_module_reset_port_config(w->aidl_module, mix_suggested.config_id);
        aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
        return -EIO;
    }

    struct aidl_stream_params sp = { .mix_port_config_id = mix_suggested.config_id,
                                     .audio_usage = 0, .audio_content_type = 0,
                                     .audio_source = (int)source,
                                     .requested_buffer_frames = 0 };
    struct aidl_stream_info info = {0};
    aidl_audio_stream *aidl_stream = aidl_audio_open_input_stream(w->aidl_module, &sp, &info);
    if (!aidl_stream) {
        aidl_audio_module_release_patch(w->aidl_module, patch_id);
        aidl_audio_module_reset_port_config(w->aidl_module, mix_suggested.config_id);
        aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
        return -EIO;
    }

    struct aidl_stream_in_wrapper *sw = pa_xnew0(struct aidl_stream_in_wrapper, 1);
    sw->parent = w;
    sw->aidl = aidl_stream;
    sw->mix_port_config_id = mix_suggested.config_id;
    sw->device_port_config_id = dev_suggested.config_id;
    sw->patch_id = patch_id;
    sw->io_handle = handle;
    sw->sample_rate = config->sample_rate;
    sw->channel_mask = config->channel_mask;
    sw->format = config->format;
    sw->buffer_size = (size_t)info.buffer_frames * (size_t)info.frame_bytes;
    init_stream_in_ops(&sw->base);

    pthread_mutex_lock(&w->mtx);
    pa_hashmap_put(w->io_streams_in, (void *)(intptr_t)handle, sw);
    pthread_mutex_unlock(&w->mtx);

    *stream_in = &sw->base;
    pa_log_info("aidl-hal-wrapper: opened input stream handle=%d mix=%s dev=%s "
                "source=%d (rate=%u ch=0x%x fmt=0x%x buf=%zu)",
                handle, mix->name, dev->name, source,
                sw->sample_rate, sw->channel_mask, sw->format, sw->buffer_size);
    return 0;
}

static void dev_close_input_stream(struct audio_hw_device *d, struct audio_stream_in *s) {
    struct aidl_hal_wrapper *w = DEV_FROM_BASE(d);
    struct aidl_stream_in_wrapper *sw = IN_FROM_BASE(s);

    pthread_mutex_lock(&w->mtx);
    pa_hashmap_remove(w->io_streams_in, (void *)(intptr_t)sw->io_handle);
    pthread_mutex_unlock(&w->mtx);

    close_input_stream_resources(sw);
    pa_xfree(sw);
}

static int dev_create_audio_patch(struct audio_hw_device *d,
                                  unsigned int num_sources,
                                  const struct audio_port_config *sources,
                                  unsigned int num_sinks,
                                  const struct audio_port_config *sinks,
                                  audio_patch_handle_t *handle) {
    struct aidl_hal_wrapper *w = DEV_FROM_BASE(d);

    /* Upstream droid-util's audio_patch_update_output passes:
     *   source.type=MIX, source.ext.mix.handle = io_handle
     *   sink.type=DEVICE, sink.ext.device.type = device_port->type
     * For input it's flipped. We map either direction to one
     * stream-wrapper + one device port. */
    if (num_sources != 1 || num_sinks != 1) {
        pa_log_warn("aidl-hal-wrapper: only 1->1 patches supported (got %u->%u)",
                    num_sources, num_sinks);
        return -ENOSYS;
    }

    bool is_output;
    audio_io_handle_t io_handle;
    audio_devices_t dev_type;
    if (sources[0].type == AUDIO_PORT_TYPE_MIX && sinks[0].type == AUDIO_PORT_TYPE_DEVICE) {
        is_output = true;
        io_handle = sources[0].ext.mix.handle;
        dev_type = sinks[0].ext.device.type;

        if (!io_handle && sources[0].id > 0 && sinks[0].id > 0) {
            pa_log_warn("aidl-hal-wrapper: creating hostless patch mix port %d -> device port %d",
                        sources[0].id, sinks[0].id);
            return create_hostless_patch(w, &sources[0], &sinks[0], handle);
        }
    } else if (sources[0].type == AUDIO_PORT_TYPE_DEVICE && sinks[0].type == AUDIO_PORT_TYPE_MIX) {
        is_output = false;
        io_handle = sinks[0].ext.mix.handle;
        dev_type = sources[0].ext.device.type;

        if (!io_handle && sources[0].id > 0 && sinks[0].id > 0) {
            pa_log_warn("aidl-hal-wrapper: creating hostless patch device port %d -> mix port %d",
                        sources[0].id, sinks[0].id);
            return create_hostless_patch(w, &sources[0], &sinks[0], handle);
        }
    } else if (sources[0].type == AUDIO_PORT_TYPE_DEVICE &&
               sinks[0].type == AUDIO_PORT_TYPE_DEVICE) {
        dm_config_port *src_dev = find_device_port_by_type(w, sources[0].ext.device.type);
        dm_config_port *sink_dev = find_device_port_by_type(w, sinks[0].ext.device.type);
        int src_cfg = 0, sink_cfg = 0, ret;

        if (!src_dev || !sink_dev) {
            pa_log_warn("aidl-hal-wrapper: create_audio_patch: device port not found (%#010x -> %#010x)",
                        sources[0].ext.device.type, sinks[0].ext.device.type);
            return -ENOENT;
        }

        src_cfg = set_port_config_by_aidl_id(w, aidl_port_id_for(w, src_dev));
        if (src_cfg <= 0)
            return src_cfg < 0 ? src_cfg : -EIO;

        sink_cfg = set_port_config_by_aidl_id(w, aidl_port_id_for(w, sink_dev));
        if (sink_cfg <= 0) {
            aidl_audio_module_reset_port_config(w->aidl_module, src_cfg);
            return sink_cfg < 0 ? sink_cfg : -EIO;
        }

        ret = create_tracked_patch(w, src_cfg, true, sink_cfg, true, handle);
        if (ret < 0) {
            aidl_audio_module_reset_port_config(w->aidl_module, sink_cfg);
            aidl_audio_module_reset_port_config(w->aidl_module, src_cfg);
            return ret;
        }

        return 0;
    } else {
        pa_log_warn("aidl-hal-wrapper: unsupported patch shape (src type=%d sink type=%d)",
                    sources[0].type, sinks[0].type);
        return -ENOSYS;
    }

    pthread_mutex_lock(&w->mtx);
    void *sw_void = is_output
                  ? pa_hashmap_get(w->io_streams_out, (void *)(intptr_t)io_handle)
                  : pa_hashmap_get(w->io_streams_in,  (void *)(intptr_t)io_handle);
    pthread_mutex_unlock(&w->mtx);
    if (!sw_void) {
        pa_log_warn("aidl-hal-wrapper: create_audio_patch: no stream for io_handle=%d", io_handle);
        return -ENOENT;
    }

    /* Common fields: drop the old patch + dev config, set up new dev config,
     * create new patch with the same mix config id. */
    int old_patch, old_dev_cfg, mix_cfg;
    if (is_output) {
        struct aidl_stream_out_wrapper *sw = (struct aidl_stream_out_wrapper *)sw_void;
        old_patch = sw->patch_id;
        old_dev_cfg = sw->device_port_config_id;
        mix_cfg = sw->mix_port_config_id;

        dm_config_port *dev = find_device_port_by_type(w, dev_type);
        int dev_aidl = aidl_port_id_for(w, dev);
        if (!dev_aidl) return -ENOENT;

        if (old_patch) {
            aidl_audio_module_release_patch(w->aidl_module, old_patch);
            sw->patch_id = 0;
        }
        if (old_dev_cfg) {
            aidl_audio_module_reset_port_config(w->aidl_module, old_dev_cfg);
            sw->device_port_config_id = 0;
        }

        struct aidl_port_config dev_cfg = { .config_id = 0, .port_id = dev_aidl,
                                            .sample_rate = 0, .channel_mask = 0,
                                            .format = -1, .io_flags = -1 };
        struct aidl_port_config dev_suggested; int dev_applied;
        if (aidl_audio_module_set_port_config(w->aidl_module, &dev_cfg, &dev_suggested, &dev_applied) < 0)
            return -EIO;
        if (!dev_applied || dev_suggested.config_id <= 0)
            return -EIO;

        int src_ids[1] = { mix_cfg };
        int snk_ids[1] = { dev_suggested.config_id };
        int new_patch = 0;
        if (aidl_audio_module_create_patch(w->aidl_module, src_ids, 1, snk_ids, 1, &new_patch) < 0) {
            aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
            return -EIO;
        }
        sw->device_port_config_id = dev_suggested.config_id;
        sw->patch_id = new_patch;
        if (handle) *handle = new_patch;
    } else {
        struct aidl_stream_in_wrapper *sw = (struct aidl_stream_in_wrapper *)sw_void;
        old_patch = sw->patch_id;
        old_dev_cfg = sw->device_port_config_id;
        mix_cfg = sw->mix_port_config_id;

        dm_config_port *dev = find_device_port_by_type(w, dev_type);
        int dev_aidl = aidl_port_id_for(w, dev);
        if (!dev_aidl) return -ENOENT;

        if (old_patch) {
            aidl_audio_module_release_patch(w->aidl_module, old_patch);
            sw->patch_id = 0;
        }
        if (old_dev_cfg) {
            aidl_audio_module_reset_port_config(w->aidl_module, old_dev_cfg);
            sw->device_port_config_id = 0;
        }

        struct aidl_port_config dev_cfg = { .config_id = 0, .port_id = dev_aidl,
                                            .sample_rate = 0, .channel_mask = 0,
                                            .format = -1, .io_flags = -1 };
        struct aidl_port_config dev_suggested; int dev_applied;
        if (aidl_audio_module_set_port_config(w->aidl_module, &dev_cfg, &dev_suggested, &dev_applied) < 0)
            return -EIO;
        if (!dev_applied || dev_suggested.config_id <= 0)
            return -EIO;

        int src_ids[1] = { dev_suggested.config_id };
        int snk_ids[1] = { mix_cfg };
        int new_patch = 0;
        if (aidl_audio_module_create_patch(w->aidl_module, src_ids, 1, snk_ids, 1, &new_patch) < 0) {
            aidl_audio_module_reset_port_config(w->aidl_module, dev_suggested.config_id);
            return -EIO;
        }
        sw->device_port_config_id = dev_suggested.config_id;
        sw->patch_id = new_patch;
        if (handle) *handle = new_patch;
    }

    return 0;
}

static int dev_release_audio_patch(struct audio_hw_device *d, audio_patch_handle_t patch) {
    struct aidl_hal_wrapper *w = DEV_FROM_BASE(d);
    struct aidl_device_patch *device_patch;

    if (!patch) return 0;

    aidl_audio_module_release_patch(w->aidl_module, patch);

    pthread_mutex_lock(&w->mtx);
    device_patch = pa_hashmap_remove(w->device_patches, (void *)(intptr_t)patch);
    if (w->io_streams_out) {
        void *state = NULL;
        void *v;
        while ((v = pa_hashmap_iterate(w->io_streams_out, &state, NULL))) {
            struct aidl_stream_out_wrapper *sw = v;
            if (sw->patch_id == patch) {
                sw->patch_id = 0;
                break;
            }
        }
    }
    if (w->io_streams_in) {
        void *state = NULL;
        void *v;
        while ((v = pa_hashmap_iterate(w->io_streams_in, &state, NULL))) {
            struct aidl_stream_in_wrapper *sw = v;
            if (sw->patch_id == patch) {
                sw->patch_id = 0;
                break;
            }
        }
    }
    pthread_mutex_unlock(&w->mtx);

    if (device_patch) {
        if (device_patch->reset_sink_port_config)
            aidl_audio_module_reset_port_config(w->aidl_module, device_patch->sink_port_config_id);
        if (device_patch->reset_source_port_config)
            aidl_audio_module_reset_port_config(w->aidl_module, device_patch->source_port_config_id);
        pa_xfree(device_patch);
    }

    return 0;
}

static int dev_dump(const struct audio_hw_device *d, int fd) { (void)d; (void)fd; return 0; }

static int dev_close(hw_device_t *device) {
    struct aidl_hal_wrapper *w = (struct aidl_hal_wrapper *)device;
    pa_log_info("aidl-hal-wrapper: closing");

    /* Close any leftover streams. */
    pthread_mutex_lock(&w->mtx);
    if (w->io_streams_out) {
        void *v;
        while ((v = pa_hashmap_steal_first(w->io_streams_out))) {
            struct aidl_stream_out_wrapper *sw = v;
            close_output_stream_resources(sw);
            pa_xfree(sw);
        }
        pa_hashmap_free(w->io_streams_out);
    }
    if (w->io_streams_in) {
        void *v;
        while ((v = pa_hashmap_steal_first(w->io_streams_in))) {
            struct aidl_stream_in_wrapper *sw = v;
            close_input_stream_resources(sw);
            pa_xfree(sw);
        }
        pa_hashmap_free(w->io_streams_in);
    }
    if (w->device_patches) {
        void *v;
        while ((v = pa_hashmap_steal_first(w->device_patches))) {
            struct aidl_device_patch *patch = v;
            aidl_audio_module_release_patch(w->aidl_module, patch->patch_id);
            if (patch->reset_sink_port_config)
                aidl_audio_module_reset_port_config(w->aidl_module, patch->sink_port_config_id);
            if (patch->reset_source_port_config)
                aidl_audio_module_reset_port_config(w->aidl_module, patch->source_port_config_id);
            pa_xfree(patch);
        }
        pa_hashmap_free(w->device_patches);
    }
    pthread_mutex_unlock(&w->mtx);

    aidl_audio_module_disconnect(w->aidl_module);

    if (w->aidl_port_ids)
        pa_hashmap_free(w->aidl_port_ids);

    pthread_mutex_destroy(&w->mtx);
    pa_xfree(w);
    return 0;
}

/* ========================================================================== *
 *  Public entry point
 * ========================================================================== */

struct audio_hw_device *aidl_hal_wrapper_open(aidl_audio_module *aidl_module,
                                              dm_config_device *config,
                                              pa_hashmap *aidl_port_ids) {
    pa_assert(aidl_module);
    pa_assert(config);
    pa_assert(aidl_port_ids);

    struct aidl_hal_wrapper *w = pa_xnew0(struct aidl_hal_wrapper, 1);
    w->aidl_module = aidl_module;
    w->config = config;
    w->aidl_port_ids = aidl_port_ids;
    w->io_streams_out = pa_hashmap_new(pa_idxset_trivial_hash_func,
                                       pa_idxset_trivial_compare_func);
    w->io_streams_in  = pa_hashmap_new(pa_idxset_trivial_hash_func,
                                       pa_idxset_trivial_compare_func);
    w->device_patches = pa_hashmap_new(pa_idxset_trivial_hash_func,
                                       pa_idxset_trivial_compare_func);
    pthread_mutex_init(&w->mtx, NULL);

    /* Fill in the audio_hw_device_t function table. */
    w->base.common.tag           = HARDWARE_DEVICE_TAG;
    w->base.common.version       = AUDIO_DEVICE_API_VERSION_2_0;
    w->base.common.module        = NULL;  /* no real hw_module_t */
    w->base.common.close         = dev_close;

    w->base.get_supported_devices = dev_get_supported_devices;
    w->base.init_check            = dev_init_check;
    w->base.set_voice_volume      = dev_set_voice_volume;
    w->base.set_master_volume     = dev_set_master_volume;
    w->base.get_master_volume     = dev_get_master_volume;
    w->base.set_master_mute       = dev_set_master_mute;
    w->base.get_master_mute       = dev_get_master_mute;
    w->base.set_mode              = dev_set_mode;
    w->base.set_mic_mute          = dev_set_mic_mute;
    w->base.get_mic_mute          = dev_get_mic_mute;
    w->base.set_parameters        = dev_set_parameters;
    w->base.get_parameters        = dev_get_parameters;
    w->base.get_input_buffer_size = dev_get_input_buffer_size;
    w->base.open_output_stream    = dev_open_output_stream;
    w->base.close_output_stream   = dev_close_output_stream;
    w->base.open_input_stream     = dev_open_input_stream;
    w->base.close_input_stream    = dev_close_input_stream;
    w->base.dump                  = dev_dump;
    w->base.create_audio_patch    = dev_create_audio_patch;
    w->base.release_audio_patch   = dev_release_audio_patch;

    pa_log_info("aidl-hal-wrapper: synthesised audio_hw_device_t over AIDL HAL");
    return &w->base;
}
