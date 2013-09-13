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
    audio_devices_t enabled_devices;
    pa_bool_t routing_changes_enabled;

    size_t buffer_size;
    pa_usec_t timestamp;

    pa_droid_card_data *card_data;
    pa_droid_hw_module *hw_module;
    audio_stream_in_t *stream;
};

#define DEFAULT_MODULE_ID "primary"

static void userdata_free(struct userdata *u);

static pa_bool_t do_routing(struct userdata *u, audio_devices_t devices) {
    char tmp[32];
    char *devlist;

    pa_assert(u);
    pa_assert(u->stream);

    if (!u->routing_changes_enabled) {
        pa_log_debug("Skipping routing change.");
        return FALSE;
    }

    if (u->primary_devices == devices)
        pa_log_debug("Refresh active device routing.");

    u->enabled_devices &= ~u->primary_devices;
    u->primary_devices = devices;
    u->enabled_devices |= u->primary_devices;

    devlist = pa_list_string_input_device(devices);
    pa_assert(devlist);
    pa_snprintf(tmp, sizeof(tmp), "routing=%u", devices);
    pa_log_debug("set_parameters(): %s (%s)", tmp, devlist);
    pa_xfree(devlist);
#ifdef DROID_DEVICE_MAKO
#warning Using mako set_parameters hack.
    u->card_data->set_parameters(u->card_data, tmp);
#else
    u->stream->common.set_parameters(&u->stream->common, tmp);
#endif

    return TRUE;
}

static pa_bool_t parse_device_list(const char *str, audio_devices_t *dst) {
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
            return FALSE;
        }

        *dst |= d;

        pa_xfree(dev);
    }

    return TRUE;
}

static int thread_read(struct userdata *u) {
    void *p;
    ssize_t readd;
    pa_memchunk chunk;

    chunk.memblock = pa_memblock_new(u->core->mempool, (size_t) u->buffer_size);

    p = pa_memblock_acquire(chunk.memblock);
    readd = u->stream->read(u->stream, (uint8_t*) p, pa_memblock_get_length(chunk.memblock));
    pa_memblock_release(chunk.memblock);

    if (readd < 0) {
        pa_log("Failed to read from stream. (err %i)", readd);
        goto end;
    }

    u->timestamp += pa_bytes_to_usec(readd, &u->source->sample_spec);

    chunk.index = 0;
    chunk.length = readd;

    if (chunk.length > 0)
        pa_source_post(u->source, &chunk);

end:
    pa_memblock_unref(chunk.memblock);

    return 0;
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

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
        if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
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

    ret = u->stream->common.standby(&u->stream->common);

    if (ret == 0)
        pa_log_info("Device suspended.");

    return ret;
}

/* Called from IO context */
static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {
        case PA_SOURCE_MESSAGE_SET_STATE: {
            switch ((pa_source_state_t) PA_PTR_TO_UINT(data)) {
                case PA_SOURCE_SUSPENDED: {
                    int r;

                    pa_assert(PA_SOURCE_IS_OPENED(u->source->thread_info.state));

                    if ((r = suspend(u)) < 0)
                        return r;

                    break;
                }

                case PA_SOURCE_IDLE:
                    break;
                case PA_SOURCE_RUNNING: {
                    pa_log_info("Resuming...");
                    u->timestamp = pa_rtclock_now();
                    break;
                }

                /* not needed */
                case PA_SOURCE_UNLINKED:
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

    pa_log_debug("Source set port %u", data->device);

    do_routing(u, data->device);

    return 0;
}


static void source_set_name(pa_modargs *ma, pa_source_new_data *data, const char *module_id) {
    const char *tmp;

    pa_assert(ma);
    pa_assert(data);
    pa_assert(module_id);

    if ((tmp = pa_modargs_get_value(ma, "source_name", NULL))) {
        pa_source_new_data_set_name(data, tmp);
        data->namereg_fail = TRUE;
    } else {
        char *tt = pa_sprintf_malloc("source.%s", module_id);
        pa_source_new_data_set_name(data, tt);
        pa_xfree(tt);
        data->namereg_fail = FALSE;
    }
}

static void source_get_mute_cb(pa_source *s) {
    struct userdata *u = s->userdata;
    pa_bool_t b;

    pa_assert(u);
    pa_assert(u->hw_module && u->hw_module->device);

    pa_droid_hw_module_lock(u->hw_module);
    if (u->hw_module->device->get_mic_mute(u->hw_module->device, &b) < 0) {
        pa_log("Failed to get mute state.");
        pa_droid_hw_module_unlock(u->hw_module);
        return;
    }
    pa_droid_hw_module_unlock(u->hw_module);

    s->muted = b;
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

void pa_droid_source_set_routing(pa_source *s, pa_bool_t enabled) {
    struct userdata *u = s->userdata;

    pa_assert(s);
    pa_assert(s->userdata);

    if (u->routing_changes_enabled != enabled)
        pa_log_debug("%s source routing changes.", enabled ? "Enabling" : "Disabling");
    u->routing_changes_enabled = enabled;
}

pa_source *pa_droid_source_new(pa_module *m,
                                 pa_modargs *ma,
                                 const char *driver,
                                 pa_droid_card_data *card_data,
                                 pa_droid_mapping *am,
                                 pa_card *card) {

    struct userdata *u = NULL;
    char *thread_name = NULL;
    pa_source_new_data data;
    const char *module_id = NULL;
    const char *tmp;
    uint32_t sample_rate;
    uint32_t alternate_sample_rate;
    audio_devices_t dev_in;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    pa_bool_t namereg_fail = FALSE;
    pa_droid_config_audio *config = NULL; /* Only used when used without card */
    int ret;

    audio_format_t hal_audio_format = 0;
    audio_channel_mask_t hal_channel_mask = 0;

    pa_assert(m);
    pa_assert(ma);
    pa_assert(driver);

    /* When running under card use hw module name for source by default. */
    if (card && ma)
        module_id = am->input->module->name;
    else
        module_id = pa_modargs_get_value(ma, "module_id", DEFAULT_MODULE_ID);

    sample_spec = m->core->default_sample_spec;
    channel_map = m->core->default_channel_map;

    if (pa_modargs_get_sample_spec_and_channel_map(ma, &sample_spec, &channel_map, PA_CHANNEL_MAP_AIFF) < 0) {
        pa_log("Failed to parse sample specification and channel map.");
        goto fail;
    }

    alternate_sample_rate = m->core->alternate_sample_rate;
    if (pa_modargs_get_alternate_sample_rate(ma, &alternate_sample_rate) < 0) {
        pa_log("Failed to parse alternate sample rate.");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->card = card;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

    /* Enabled routing changes by default. */
    u->routing_changes_enabled = TRUE;

    if (card_data) {
        pa_assert(card);
        u->card_data = card_data;
        pa_assert_se((u->hw_module = pa_droid_hw_module_get(u->core, NULL, card_data->module_id)));
    } else {
        /* Stand-alone source */

        if (!(config = pa_droid_config_load(ma)))
            goto fail;

        if (!(u->hw_module = pa_droid_hw_module_get(u->core, config, module_id)))
            goto fail;
    }

    if (!pa_convert_format(sample_spec.format, CONV_FROM_PA, &hal_audio_format)) {
        pa_log("Sample spec format %u not supported.", sample_spec.format);
        goto fail;
    }

    for (int i = 0; i < channel_map.channels; i++) {
        audio_channel_mask_t c;
        if (!pa_convert_input_channel(channel_map.map[i], CONV_FROM_PA, &c)) {
            pa_log("Failed to convert channel map.");
            goto fail;
        }
        hal_channel_mask |= c;
    }

    struct audio_config config_in = {
        .sample_rate = sample_spec.rate,
        .channel_mask = hal_channel_mask,
        .format = hal_audio_format
    };

    /* Default routing */
    /* FIXME So while setting routing through stream with HALv2 API fails, creation of stream
     * requires HALv2 style device to work properly. So until that oddity is resolved we always
     * set AUDIO_DEVICE_IN_BUILTIN_MIC as initial device here. */
#if 0
    pa_assert_se(pa_string_convert_input_device_str_to_num("AUDIO_DEVICE_IN_BUILTIN_MIC", &dev_in));

    if ((tmp = pa_modargs_get_value(ma, "input_devices", NULL))) {
        audio_devices_t tmp_dev;

        if (parse_device_list(tmp, &tmp_dev) && tmp_dev)
            dev_in = tmp_dev;

        pa_log_debug("Set initial devices %s", tmp);
    }
#else
    pa_log_info("FIXME: Setting AUDIO_DEVICE_IN_BUILTIN_MIC as initial device.");
    dev_in = AUDIO_DEVICE_IN_BUILTIN_MIC;
#endif
    dev_in = AUDIO_DEVICE_IN_DEFAULT;
    pa_droid_hw_module_lock(u->hw_module);
    ret = u->hw_module->device->open_input_stream(u->hw_module->device,
                                                  u->hw_module->stream_in_id++,
                                                  dev_in,
                                                  &config_in,
                                                  &u->stream);
    pa_droid_hw_module_unlock(u->hw_module);

    if (ret < 0) {
        pa_log("Failed to open input stream.");
        goto fail;
    }

    u->buffer_size = u->stream->common.get_buffer_size(&u->stream->common);

    if ((sample_rate = u->stream->common.get_sample_rate(&u->stream->common)) != sample_spec.rate) {
        pa_log_warn("Requested sample rate %u but got %u instead.", sample_spec.rate, sample_rate);
        sample_spec.rate = sample_rate;
    }

    pa_log_info("Created Android stream with device: %u sample rate: %u channel mask: %u format: %u buffer size: %u",
            dev_in,
            sample_rate,
            config_in.channel_mask,
            config_in.format,
            u->buffer_size);


    pa_source_new_data_init(&data);
    data.driver = driver;
    data.module = m;
    data.card = card;

    source_set_name(ma, &data, module_id);

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

    pa_source_new_data_set_sample_spec(&data, &sample_spec);
    pa_source_new_data_set_channel_map(&data, &channel_map);
    pa_source_new_data_set_alternate_sample_rate(&data, alternate_sample_rate);

    if (am)
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

    pa_source_set_fixed_latency(u->source, pa_bytes_to_usec(u->buffer_size, &sample_spec));
    pa_log_debug("Set fixed latency %" PRIu64 " usec", pa_bytes_to_usec(u->buffer_size, &sample_spec));

    if (u->source->active_port)
        source_set_port_cb(u->source, u->source->active_port);

    pa_source_put(u->source);

    if (config)
        pa_xfree(config);

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

    if (u->hw_module && u->stream) {
        pa_droid_hw_module_lock(u->hw_module);
        u->hw_module->device->close_input_stream(u->hw_module->device, u->stream);
        pa_droid_hw_module_unlock(u->hw_module);
    }

    // Stand alone source
    if (u->hw_module)
        pa_droid_hw_module_unref(u->hw_module);

    pa_xfree(u);
}
