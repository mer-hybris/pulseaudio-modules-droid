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
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/source.h>
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
#include <pulsecore/resampler.h>

#include "droid-source.h"
#include "droid-util.h"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_card *card;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_memchunk memchunk;
    audio_devices_t primary_devices;

    size_t source_buffer_size;
    size_t buffer_size;
    pa_usec_t timestamp;

    pa_hook_slot *input_buffer_size_changed_slot;
    pa_hook_slot *input_channel_map_changed_slot;
    pa_resampler *resampler;

    pa_droid_card_data *card_data;
    pa_droid_hw_module *hw_module;
    pa_droid_stream *stream;
    bool stream_valid;
};

enum {
    SOURCE_MESSAGE_DO_ROUTING = PA_SOURCE_MESSAGE_MAX
};

#define DEFAULT_MODULE_ID "primary"

#define DROID_AUDIO_SOURCE "droid.audio_source"
#define DROID_AUDIO_SOURCE_UNDEFINED "undefined"

static void userdata_free(struct userdata *u);
static int suspend(struct userdata *u);
static void unsuspend(struct userdata *u);

static int do_routing(struct userdata *u, audio_devices_t devices) {
    int ret;
    audio_devices_t old_device;

    pa_assert(u);
    pa_assert(u->stream);

    if (u->primary_devices == devices)
        pa_log_debug("Refresh active device routing.");

    old_device = u->primary_devices;
    u->primary_devices = devices;

    ret = pa_droid_stream_set_route(u->stream, devices);

    if (ret < 0)
        u->primary_devices = old_device;

    return ret;
}

static bool parse_device_list(const char *str, audio_devices_t *dst) {
    pa_assert(str);
    pa_assert(dst);

    char *dev;
    const char *state = NULL;

    *dst = 0;

    while ((dev = pa_split(str, "|", &state))) {
        audio_devices_t d;

        if (!pa_string_convert_input_device_str_to_num(dev, &d)) {
            pa_log_warn("Unknown device %s", dev);
            pa_xfree(dev);
            return false;
        }

        *dst |= d;

        pa_xfree(dev);
    }

    return true;
}

static int thread_read(struct userdata *u) {
    void *p;
    ssize_t readd;
    pa_memchunk chunk;

    chunk.index = 0;
    chunk.memblock = pa_memblock_new(u->core->mempool, (size_t) u->buffer_size);

    if (!u->stream_valid) {
        /* try to resume or post silence */
        unsuspend(u);
        if (!u->stream_valid) {
            p = pa_memblock_acquire(chunk.memblock);
            chunk.length = pa_memblock_get_length(chunk.memblock);
            memset(p, 0, chunk.length);
            pa_source_post(u->source, &chunk);
            pa_memblock_release(chunk.memblock);
            goto end;
        }
    }

    p = pa_memblock_acquire(chunk.memblock);
    readd = pa_droid_stream_read(u->stream, p, pa_memblock_get_length(chunk.memblock));
    pa_memblock_release(chunk.memblock);

    if (readd < 0) {
        pa_log("Failed to read from stream. (err %i)", readd);
        goto end;
    }

    u->timestamp += pa_bytes_to_usec(readd, &u->source->sample_spec);

    chunk.length = readd;

    if (u->resampler) {
        pa_memchunk rchunk;

        pa_resampler_run(u->resampler, &chunk, &rchunk);

        if (rchunk.length > 0)
            pa_source_post(u->source, &rchunk);
        if (rchunk.memblock)
            pa_memblock_unref(rchunk.memblock);

        goto end;
    }

    if (chunk.length > 0)
        pa_source_post(u->source, &chunk);

end:
    pa_memblock_unref(chunk.memblock);

    return 0;
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_assert(u->stream);

    pa_log_debug("Thread starting up.");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);

    u->timestamp = pa_rtclock_now();

    for (;;) {
        int ret;

        if (PA_SOURCE_IS_OPENED(u->source->thread_info.state)) {
            thread_read(u);

            pa_rtpoll_set_timer_absolute(u->rtpoll, u->timestamp);
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Sleep */
#if (PULSEAUDIO_VERSION == 5)
        if ((ret = pa_rtpoll_run(u->rtpoll, true)) < 0)
#elif (PULSEAUDIO_VERSION >= 6)
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
#endif
            goto fail;

        if (ret == 0)
            goto finish;

    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down.");
}

/* Called from IO context */
static int suspend(struct userdata *u) {
    int ret;

    pa_assert(u);
    pa_assert(u->stream);

    ret = pa_droid_stream_suspend(u->stream, true);

    if (ret == 0)
        pa_log_info("Device suspended.");

    return ret;
}

/* Called from IO context */
static void unsuspend(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->stream);

    if (pa_droid_stream_suspend(u->stream, false) >= 0) {
        u->stream_valid = true;
        pa_log_info("Resuming...");
    } else
        u->stream_valid = false;
}

/* Called from IO context */
static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {
        case SOURCE_MESSAGE_DO_ROUTING: {
            audio_devices_t device = PA_PTR_TO_UINT(data);

            pa_assert(PA_SOURCE_IS_OPENED(u->source->thread_info.state));

            suspend(u);
            do_routing(u, device);
            unsuspend(u);
            break;
        }

        case PA_SOURCE_MESSAGE_SET_STATE: {
            switch ((pa_source_state_t) PA_PTR_TO_UINT(data)) {
                case PA_SOURCE_SUSPENDED: {
                    int r;

                    if (PA_SOURCE_IS_OPENED(u->source->thread_info.state)) {
                        if ((r = suspend(u)) < 0)
                            return r;
                    }

                    break;
                }

                case PA_SOURCE_IDLE:
                    /* Fall through */
                case PA_SOURCE_RUNNING: {
                    if (u->source->thread_info.state == PA_SOURCE_SUSPENDED) {
                        unsuspend(u);
                        u->timestamp = pa_rtclock_now();
                    }
                    break;
                }

                case PA_SOURCE_UNLINKED: {
                    /* Suspending since some implementations do not want to free running stream. */
                    suspend(u);
                    break;
                }

                /* not needed */
                case PA_SOURCE_INIT:
                case PA_SOURCE_INVALID_STATE:
                    ;
            }
            break;
        }
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static int source_set_port_cb(pa_source *s, pa_device_port *p) {
    struct userdata *u = s->userdata;
    pa_droid_port_data *data;

    pa_assert(u);
    pa_assert(p);

    data = PA_DEVICE_PORT_DATA(p);

    if (!data->device) {
        /* If there is no device defined, just return 0 to say everything is ok.
         * Then next port change can be whatever source port, even the one enabled
         * before parking. */
        pa_log_debug("Source set port to parking");
        return 0;
    }

    pa_log_debug("Source set port %u", data->device);

    if (!PA_SOURCE_IS_OPENED(pa_source_get_state(u->source)))
        do_routing(u, data->device);
    else {
        pa_asyncmsgq_post(u->source->asyncmsgq, PA_MSGOBJECT(u->source), SOURCE_MESSAGE_DO_ROUTING, PA_UINT_TO_PTR(data->device), 0, NULL, NULL);
    }

    return 0;
}

static void source_set_name(pa_modargs *ma, pa_source_new_data *data, const char *module_id) {
    const char *tmp;

    pa_assert(ma);
    pa_assert(data);

    if ((tmp = pa_modargs_get_value(ma, "source_name", NULL))) {
        pa_source_new_data_set_name(data, tmp);
        data->namereg_fail = true;
        pa_proplist_sets(data->proplist, PA_PROP_DEVICE_DESCRIPTION, "Droid source");
    } else {
        char *tt;
        pa_assert(module_id);
        tt = pa_sprintf_malloc("source.%s", module_id);
        pa_source_new_data_set_name(data, tt);
        pa_xfree(tt);
        data->namereg_fail = false;
        pa_proplist_setf(data->proplist, PA_PROP_DEVICE_DESCRIPTION, "Droid source %s", module_id);
    }
}

#if (PULSEAUDIO_VERSION == 5)
static void source_get_mute_cb(pa_source *s) {
#elif (PULSEAUDIO_VERSION >= 6)
static int source_get_mute_cb(pa_source *s, bool *muted) {
#endif
    struct userdata *u = s->userdata;
    int ret = 0;
    bool b;

    pa_assert(u);
    pa_assert(u->hw_module && u->hw_module->device);

    pa_droid_hw_module_lock(u->hw_module);
    if (u->hw_module->device->get_mic_mute(u->hw_module->device, &b) < 0) {
        pa_log("Failed to get mute state.");
        ret = -1;
    }
    pa_droid_hw_module_unlock(u->hw_module);

#if (PULSEAUDIO_VERSION == 5)
    if (ret == 0)
        s->muted = b;
#elif (PULSEAUDIO_VERSION >= 6)
    if (ret == 0)
        *muted = b;

    return ret;
#endif
}

static void source_set_mute_cb(pa_source *s) {
    struct userdata *u = s->userdata;

    pa_assert(u);
    pa_assert(u->hw_module && u->hw_module->device);

    pa_droid_hw_module_lock(u->hw_module);
    if (u->hw_module->device->set_mic_mute(u->hw_module->device, s->muted) < 0)
        pa_log("Failed to set mute state to %smuted.", s->muted ? "" : "un");
    pa_droid_hw_module_unlock(u->hw_module);
}

static void source_set_mute_control(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->hw_module && u->hw_module->device);

    if (u->hw_module->device->set_mic_mute) {
        pa_log_info("Using hardware mute control for %s", u->source->name);
        pa_source_set_get_mute_callback(u->source, source_get_mute_cb);
        pa_source_set_set_mute_callback(u->source, source_set_mute_cb);
    } else {
        pa_log_info("Using software mute control for %s", u->source->name);
        pa_source_set_get_mute_callback(u->source, NULL);
        pa_source_set_set_mute_callback(u->source, NULL);
    }
}

/* Called from main and IO context */
static void update_latency(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->source);
    pa_assert(u->stream);

    u->buffer_size = pa_droid_stream_buffer_size(u->stream);

    if (u->source_buffer_size) {
        u->buffer_size = pa_droid_buffer_size_round_up(u->source_buffer_size, u->buffer_size);
        pa_log_info("Using buffer size %u (requested %u).", u->buffer_size, u->source_buffer_size);
    } else
        pa_log_info("Using buffer size %u.", u->buffer_size);

    if (pa_thread_mq_get())
        pa_source_set_fixed_latency_within_thread(u->source, pa_bytes_to_usec(u->buffer_size, &u->stream->input->sample_spec));
    else
        pa_source_set_fixed_latency(u->source, pa_bytes_to_usec(u->buffer_size, &u->stream->input->sample_spec));

    pa_log_debug("Set fixed latency %" PRIu64 " usec", pa_bytes_to_usec(u->buffer_size, &u->stream->input->sample_spec));
}

/* Called from IO context. */
static pa_hook_result_t input_buffer_size_changed_cb(pa_droid_hw_module *module,
                                                     pa_droid_stream *stream,
                                                     struct userdata *u) {
    pa_assert(module);
    pa_assert(stream);
    pa_assert(u);

    if (stream != u->stream)
        return PA_HOOK_OK;

    update_latency(u);

    return PA_HOOK_OK;
}

/* Called from IO context. */
static pa_hook_result_t input_channel_map_changed_cb(pa_droid_hw_module *module,
                                                     pa_droid_stream *stream,
                                                     struct userdata *u) {
    pa_assert(module);
    pa_assert(stream);
    pa_assert(u);

    if (stream != u->stream)
        return PA_HOOK_OK;

    if (u->stream->input->input_channel_map.channels != u->source->channel_map.channels) {
        if (u->resampler)
            pa_resampler_free(u->resampler);

        u->resampler = pa_resampler_new(u->core->mempool,
                                        &u->stream->input->input_sample_spec, &u->stream->input->input_channel_map,
                                        &u->source->sample_spec, &u->source->channel_map,
                                        u->core->lfe_crossover_freq,
                                        PA_RESAMPLER_COPY,
                                        0);
    } else if (u->resampler) {
        pa_resampler_free(u->resampler);
        u->resampler = NULL;
    }

    return PA_HOOK_OK;
}

pa_source *pa_droid_source_new(pa_module *m,
                                 pa_modargs *ma,
                                 const char *driver,
                                 audio_devices_t device,
                                 pa_droid_card_data *card_data,
                                 pa_droid_mapping *am,
                                 pa_card *card) {

    struct userdata *u = NULL;
    char *thread_name = NULL;
    pa_source_new_data data;
    const char *module_id = NULL;
    const char *tmp;
    uint32_t alternate_sample_rate;
    audio_devices_t dev_in;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    const char *format;
    bool namereg_fail = false;
    pa_droid_config_audio *config = NULL; /* Only used when source is created without card */
    uint32_t source_buffer = 0;

    pa_assert(m);
    pa_assert(ma);
    pa_assert(driver);

    /* When running under card use hw module name for source by default. */
    if (am)
        module_id = am->input->module->name;
    else
        module_id = pa_modargs_get_value(ma, "module_id", DEFAULT_MODULE_ID);

    sample_spec = m->core->default_sample_spec;
    channel_map = m->core->default_channel_map;

    /* First parse both sample spec and channel map, then see if source_* override some
     * of the values. */
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &sample_spec, &channel_map, PA_CHANNEL_MAP_AIFF) < 0) {
        pa_log("Failed to parse source sample specification and channel map.");
        goto fail;
    }

    if (pa_modargs_get_value(ma, "source_channel_map", NULL)) {
        if (pa_modargs_get_channel_map(ma, "source_channel_map", &channel_map) < 0) {
            pa_log("Failed to parse source channel map.");
            goto fail;
        }

        sample_spec.channels = channel_map.channels;
    }

    if ((format = pa_modargs_get_value(ma, "source_format", NULL))) {
        if ((sample_spec.format = pa_parse_sample_format(format)) < 0) {
            pa_log("Failed to parse source format.");
            goto fail;
        }
    }

    if (pa_modargs_get_value_u32(ma, "source_rate", &sample_spec.rate) < 0) {
        pa_log("Failed to parse source_rate.");
        goto fail;
    }

    if (!pa_sample_spec_valid(&sample_spec)) {
        pa_log("Sample spec is not valid.");
        goto fail;
    }

    alternate_sample_rate = m->core->alternate_sample_rate;
    if (pa_modargs_get_alternate_sample_rate(ma, &alternate_sample_rate) < 0) {
        pa_log("Failed to parse alternate sample rate.");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "source_buffer", &source_buffer) < 0) {
        pa_log("Failed to parse source_buffer. Needs to be integer >= 0.");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->stream_valid = true;
    u->core = m->core;
    u->module = m;
    u->card = card;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

    if (card_data) {
        pa_assert(card);
        u->card_data = card_data;
        pa_assert_se((u->hw_module = pa_droid_hw_module_get(u->core, NULL, card_data->module_id)));
    } else {
        /* Stand-alone source */

        if (!(u->hw_module = pa_droid_hw_module_get(u->core, NULL, module_id))) {
            if (!(config = pa_droid_config_load(ma)))
                goto fail;

            /* Ownership of config transfers to hw_module if opening of hw module succeeds. */
            if (!(u->hw_module = pa_droid_hw_module_get(u->core, config, module_id)))
                goto fail;
        }
    }

    /* Default routing */
    if (device)
        dev_in = device;
    else {
        /* FIXME So while setting routing through stream with HALv2 API fails, creation of stream
         * requires HALv2 style device to work properly. So until that oddity is resolved we always
         * set AUDIO_DEVICE_IN_BUILTIN_MIC as initial device here. */
        pa_log_info("FIXME: Setting AUDIO_DEVICE_IN_BUILTIN_MIC as initial device.");
        pa_assert_se(pa_string_convert_input_device_str_to_num("AUDIO_DEVICE_IN_BUILTIN_MIC", &dev_in));

        if ((tmp = pa_modargs_get_value(ma, "input_devices", NULL))) {
            audio_devices_t tmp_dev;

            if (parse_device_list(tmp, &tmp_dev) && tmp_dev)
                dev_in = tmp_dev;

            pa_log_debug("Set initial devices %s", tmp);
        }
    }

    if (am)
        u->stream = pa_droid_open_input_stream(u->hw_module, &sample_spec, &channel_map, dev_in, am);
    else
        u->stream = pa_droid_open_input_stream(u->hw_module, &sample_spec, &channel_map, dev_in, NULL);

    if (!u->stream) {
        pa_log("Failed to open input stream.");
        goto fail;
    }

    pa_source_new_data_init(&data);
    data.driver = driver;
    data.module = m;
    data.card = card;
    data.suspend_cause = PA_SUSPEND_IDLE;

    if (am)
        source_set_name(ma, &data, am->name);
    else
        source_set_name(ma, &data, module_id);

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, PROP_DROID_API_STRING);

    /* We need to give pa_modargs_get_value_boolean() a pointer to a local
     * variable instead of using &data.namereg_fail directly, because
     * data.namereg_fail is a bitfield and taking the address of a bitfield
     * variable is impossible. */
    namereg_fail = data.namereg_fail;
    if (pa_modargs_get_value_boolean(ma, "namereg_fail", &namereg_fail) < 0) {
        pa_log("Failed to parse namereg_fail argument.");
        pa_source_new_data_done(&data);
        goto fail;
    }
    data.namereg_fail = namereg_fail;

    pa_source_new_data_set_sample_spec(&data, &u->stream->input->sample_spec);
    pa_source_new_data_set_channel_map(&data, &u->stream->input->channel_map);
    pa_source_new_data_set_alternate_sample_rate(&data, alternate_sample_rate);

    if (am && card)
        pa_droid_add_ports(data.ports, am, card);

    u->source = pa_source_new(m->core, &data, PA_SOURCE_HARDWARE);
    pa_source_new_data_done(&data);

    if (!u->source) {
        pa_log("Failed to create source.");
        goto fail;
    }

    u->source->userdata = u;

    u->source->parent.process_msg = source_process_msg;

    source_set_mute_control(u);

    u->source->set_port = source_set_port_cb;

    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);

    /* Disable rewind for droid source */
    pa_source_set_max_rewind(u->source, 0);

    thread_name = pa_sprintf_malloc("droid-source-%s", module_id);
    if (!(u->thread = pa_thread_new(thread_name, thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }
    pa_xfree(thread_name);
    thread_name = NULL;

    update_latency(u);

    if (u->source->active_port)
        source_set_port_cb(u->source, u->source->active_port);

    u->input_buffer_size_changed_slot = pa_hook_connect(&pa_droid_hooks(u->hw_module)[PA_DROID_HOOK_INPUT_BUFFER_SIZE_CHANGED],
                                                        PA_HOOK_NORMAL,
                                                        (pa_hook_cb_t) input_buffer_size_changed_cb, u);

    u->input_channel_map_changed_slot = pa_hook_connect(&pa_droid_hooks(u->hw_module)[PA_DROID_HOOK_INPUT_CHANNEL_MAP_CHANGED],
                                                        PA_HOOK_NORMAL,
                                                        (pa_hook_cb_t) input_channel_map_changed_cb, u);

    pa_droid_stream_set_data(u->stream, u->source);
    pa_source_put(u->source);

    return u->source;

fail:
    pa_xfree(thread_name);

    if (config)
        pa_xfree(config);

    if (u)
        userdata_free(u);

    return NULL;
}

void pa_droid_source_free(pa_source *s) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    userdata_free(u);
}

static void userdata_free(struct userdata *u) {
    pa_assert(u);

    if (u->input_channel_map_changed_slot)
        pa_hook_slot_free(u->input_channel_map_changed_slot);

    if (u->input_buffer_size_changed_slot)
        pa_hook_slot_free(u->input_buffer_size_changed_slot);

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->source)
        pa_source_unref(u->source);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->stream)
        pa_droid_stream_unref(u->stream);

    // Stand alone source
    if (u->hw_module)
        pa_droid_hw_module_unref(u->hw_module);

    pa_xfree(u);
}
