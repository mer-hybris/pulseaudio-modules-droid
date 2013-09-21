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
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

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
#include <pulsecore/sink.h>
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

#include "droid-sink.h"
#include "droid-util.h"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_card *card;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_bool_t deferred_volume; /* TODO */

    pa_memblockq *memblockq;
    pa_memchunk silence;
    size_t buffer_count;
    size_t buffer_size;
    pa_usec_t buffer_latency;
    pa_usec_t timestamp;

    audio_devices_t primary_devices;
    audio_devices_t extra_devices;

    pa_bool_t use_hw_volume;

    pa_hook_slot *sink_input_put_hook_slot;
    pa_hook_slot *sink_input_unlink_hook_slot;

    pa_droid_card_data *card_data;
    pa_droid_hw_module *hw_module;
    struct audio_stream_out *stream_out;
};

#define DEFAULT_MODULE_ID "primary"

#define PROP_DROID_ROUTE "droid.device.additional-route"

static void userdata_free(struct userdata *u);

static void set_primary_devices(struct userdata *u, audio_devices_t devices) {
    pa_assert(u);
    pa_assert(devices);

    u->primary_devices = devices;
}

static void add_extra_devices(struct userdata *u, audio_devices_t devices) {
    pa_assert(u);
    pa_assert(devices);

    u->extra_devices |= devices;
}

static void remove_extra_devices(struct userdata *u, audio_devices_t devices) {
    pa_assert(u);
    pa_assert(devices);

    u->extra_devices &= ~devices;
}

static pa_bool_t do_routing(struct userdata *u) {
    audio_devices_t routing;
    char tmp[32];

    pa_assert(u);
    pa_assert(u->stream_out);

    routing = u->primary_devices | u->extra_devices;

    pa_snprintf(tmp, sizeof(tmp), "routing=%u;", routing);
    pa_log_debug("set_parameters(): %s", tmp);
    pa_droid_hw_module_lock(u->hw_module);
    u->stream_out->common.set_parameters(&u->stream_out->common, tmp);
    pa_droid_hw_module_unlock(u->hw_module);

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

        if (!pa_string_convert_output_device_str_to_num(dev, &d)) {
            pa_log_warn("Unknown device %s", dev);
            pa_xfree(dev);
            return FALSE;
        }

        *dst |= d;

        pa_xfree(dev);
    }

    return TRUE;
}

static int thread_write(struct userdata *u) {
    pa_memchunk c;
    const void *p;
    ssize_t wrote;

    pa_memblockq_peek_fixed_size(u->memblockq, u->buffer_size, &c);

    /* We should be able to write everything in one go as long as memblock size
     * is multiples of buffer_size. */

    for (;;) {
        p = pa_memblock_acquire(c.memblock);
        wrote = u->stream_out->write(u->stream_out, (const uint8_t*) p + c.index, c.length);
        pa_memblock_release(c.memblock);

        if (wrote < 0) {
            pa_memblockq_drop(u->memblockq, c.length);
            pa_memblock_unref(c.memblock);
            return -1;
        }

        if (wrote < (ssize_t) c.length) {
            c.index += wrote;
            c.length -= wrote;
            continue;
        }

        pa_memblockq_drop(u->memblockq, c.length);
        pa_memblock_unref(c.memblock);

        break;
    }

    return 0;
}
static void thread_render(struct userdata *u) {
    size_t length;
    size_t missing;

    length = pa_memblockq_get_length(u->memblockq);
    missing = u->buffer_size * u->buffer_count - length;

    if (missing > 0) {
        pa_memchunk c;
        pa_sink_render_full(u->sink, missing, &c);
        pa_memblockq_push_align(u->memblockq, &c);
        pa_memblock_unref(c.memblock);
    }
}

static void process_rewind(struct userdata *u) {
    size_t rewind_nbytes;
    size_t max_rewind_nbytes;
    size_t queue_length;

    pa_assert(u);

    if (u->sink->thread_info.rewind_nbytes == 0) {
        pa_sink_process_rewind(u->sink, 0);
        return;
    }

    rewind_nbytes = u->sink->thread_info.rewind_nbytes;
    u->sink->thread_info.rewind_nbytes = 0;

    pa_assert(rewind_nbytes > 0);
    pa_log_debug("Requested to rewind %lu bytes.", (unsigned long) rewind_nbytes);

    queue_length = pa_memblockq_get_length(u->memblockq);
    if (queue_length <= u->buffer_size)
        goto do_nothing;
    max_rewind_nbytes = queue_length - u->buffer_size;
    if (max_rewind_nbytes == 0)
        goto do_nothing;

    if (rewind_nbytes > max_rewind_nbytes)
        rewind_nbytes = max_rewind_nbytes;

    pa_memblockq_drop(u->memblockq, rewind_nbytes);

    pa_sink_process_rewind(u->sink, rewind_nbytes);

    pa_log_debug("Rewound %lu bytes.", (unsigned long) rewind_nbytes);
    return;

do_nothing:
    pa_log_debug("Rewound 0 bytes.");
    pa_sink_process_rewind(u->sink, 0);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up.");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);

    u->timestamp = 0;

    for (;;) {
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {

            u->timestamp = pa_rtclock_now();

            if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
                process_rewind(u);
            else
                thread_render(u);

            if (pa_rtpoll_timer_elapsed(u->rtpoll)) {
                pa_usec_t now, sleept;

                thread_write(u);

                now = pa_rtclock_now();

                if (now - u->timestamp > u->buffer_latency / 2)
                    sleept = 0;
                else
                    sleept = u->buffer_latency / 2 - (now - u->timestamp) ;

                pa_rtpoll_set_timer_relative(u->rtpoll, sleept);
            }
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
    size_t length;

    pa_assert(u);
    pa_assert(u->sink);
    pa_assert(u->stream_out);

    ret = u->stream_out->common.standby(&u->stream_out->common);

    if (ret == 0) {
        pa_sink_set_max_request_within_thread(u->sink, 0);
        pa_log_info("Device suspended.");
    } else
        pa_log("Couldn't set standby, err %d", ret);

    /* Clear memblockq */
    if ((length = pa_memblockq_get_length(u->memblockq)) > 0)
        pa_memblockq_drop(u->memblockq, length);

    return ret;
}

static int unsuspend(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->sink);

    /* HAL resumes automagically when writing to standby stream, but let's set max request */
    pa_sink_set_max_request_within_thread(u->sink, u->buffer_size);

    pa_log_info("Resuming...");

    return 0;
}

/* Called from IO context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t r = 0;

            /* HAL reports milliseconds */
            if (u->stream_out)
                r = u->stream_out->get_latency(u->stream_out) * PA_USEC_PER_MSEC * u->buffer_count;

            *((pa_usec_t*) data) = r;

            return 0;
        }

        case PA_SINK_MESSAGE_SET_STATE: {
            switch ((pa_sink_state_t) PA_PTR_TO_UINT(data)) {
                case PA_SINK_SUSPENDED: {
                    int r;

                    pa_assert(PA_SINK_IS_OPENED(u->sink->thread_info.state));

                    if ((r = suspend(u)) < 0)
                        return r;

                    break;
                }

                case PA_SINK_IDLE:
                    /* Fall through */
                case PA_SINK_RUNNING: {
                    int r;
                    u->timestamp = 0;

                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                        if ((r = unsuspend(u)) < 0)
                            return r;
                    }

                    pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
                    break;
                }

                /* not needed */
                case PA_SINK_UNLINKED:
                case PA_SINK_INIT:
                case PA_SINK_INVALID_STATE:
                    ;
            }
            break;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int sink_set_port_cb(pa_sink *s, pa_device_port *p) {
    struct userdata *u = s->userdata;
    pa_droid_port_data *data;

    pa_assert(u);
    pa_assert(p);

    data = PA_DEVICE_PORT_DATA(p);

    if (!data->device) {
        /* If there is no device defined, just return 0 to say everything is ok.
         * Then next port change can be whatever sink port, even the one enabled
         * before parking. */
        pa_log_debug("Sink set port to parking");
        return 0;
    }

    pa_log_debug("Sink set port %u", data->device);

    set_primary_devices(u, data->device);
    do_routing(u);

    return 0;
}

static void sink_set_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume r;

    /* Shift up by the base volume */
    pa_sw_cvolume_divide_scalar(&r, &s->real_volume, s->base_volume);

    if (r.channels == 1) {
        float val = pa_sw_volume_to_linear(r.values[0]);
        pa_log_debug("Set hw volume %f", val);
        pa_droid_hw_module_lock(u->hw_module);
        if (u->stream_out->set_volume(u->stream_out, val, val) < 0)
            pa_log_warn("Failed to set hw volume.");
        pa_droid_hw_module_unlock(u->hw_module);
    } else if (r.channels == 2) {
        float val[2];
        for (unsigned i = 0; i < 2; i++)
            val[i] = pa_sw_volume_to_linear(r.values[i]);
        pa_log_debug("Set hw volume %f : %f", val[0], val[1]);
        pa_droid_hw_module_lock(u->hw_module);
        if (u->stream_out->set_volume(u->stream_out, val[0], val[1]) < 0)
            pa_log_warn("Failed to set hw volume.");
        pa_droid_hw_module_unlock(u->hw_module);
    }
}

static void sink_set_voice_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume r;
    float val;

    /* Shift up by the base volume */
    pa_sw_cvolume_divide_scalar(&r, &s->real_volume, s->base_volume);

    val = pa_sw_volume_to_linear(pa_cvolume_avg(&r));
    pa_log_debug("Set voice volume %f", val);

    pa_droid_hw_module_lock(u->hw_module);
    if (u->hw_module->device->set_voice_volume(u->hw_module->device, val) < 0)
        pa_log_warn("Failed to set voice volume.");
    pa_droid_hw_module_unlock(u->hw_module);
}

static void update_volumes(struct userdata *u) {
    int ret = -1;

    /* set_volume returns 0 if hw volume control is implemented, < 0 otherwise. */
    pa_droid_hw_module_lock(u->hw_module);
    if (u->stream_out->set_volume) {
        pa_log_debug("Probe hw volume support for %s", u->sink->name);
        ret = u->stream_out->set_volume(u->stream_out, 1.0f, 1.0f);
    }
    pa_droid_hw_module_unlock(u->hw_module);

    u->use_hw_volume = (ret == 0);

    /* Apply callbacks */
    pa_droid_sink_set_voice_control(u->sink, FALSE);
}

static void set_sink_name(pa_modargs *ma, pa_sink_new_data *data, const char *module_id) {
    const char *tmp;

    pa_assert(ma);
    pa_assert(data);
    pa_assert(module_id);

    if ((tmp = pa_modargs_get_value(ma, "sink_name", NULL))) {
        pa_sink_new_data_set_name(data, tmp);
        data->namereg_fail = TRUE;
    } else {
        char *tt = pa_sprintf_malloc("sink.%s", module_id);
        pa_sink_new_data_set_name(data, tt);
        pa_xfree(tt);
        data->namereg_fail = FALSE;
    }
}

void pa_droid_sink_set_voice_control(pa_sink* sink, pa_bool_t enable) {
    struct userdata *u = sink->userdata;

    pa_assert(u);
    pa_assert(sink);

    if (enable) {
        pa_log_debug("Using voice volume control for %s", u->sink->name);
        pa_sink_set_set_volume_callback(u->sink, sink_set_voice_volume_cb);
    } else {
        if (u->use_hw_volume) {
            pa_log_debug("Using hardware volume control for %s", u->sink->name);
            pa_sink_set_set_volume_callback(u->sink, sink_set_volume_cb);
        } else {
            pa_log_debug("Using software volume control for %s", u->sink->name);
            pa_sink_set_set_volume_callback(u->sink, NULL);
        }
    }
}

/* When sink-input with proper proplist variable appears, do extra routing configuration
 * for the lifetime of that sink-input. */
static pa_hook_result_t sink_input_put_hook_cb(pa_core *c, pa_sink_input *sink_input, struct userdata *u) {
    const char *dev_str;
    audio_devices_t devices;

    if ((dev_str = pa_proplist_gets(sink_input->proplist, PROP_DROID_ROUTE))) {

        if (parse_device_list(dev_str, &devices) && devices) {

            pa_log_debug("Add extra route %s (%u).", dev_str, devices);

            add_extra_devices(u, devices);
            do_routing(u);
        }
    }

    return PA_HOOK_OK;
}

/* Remove extra routing when sink-inputs disappear. */
static pa_hook_result_t sink_input_unlink_hook_cb(pa_core *c, pa_sink_input *sink_input, struct userdata *u) {
    const char *dev_str;
    audio_devices_t devices;

    if ((dev_str = pa_proplist_gets(sink_input->proplist, PROP_DROID_ROUTE))) {

        if (parse_device_list(dev_str, &devices) && devices) {

            pa_log_debug("Remove extra route %s (%u).", dev_str, devices);

            remove_extra_devices(u, devices);
            do_routing(u);
        }
    }

    return PA_HOOK_OK;
}

pa_sink *pa_droid_sink_new(pa_module *m,
                             pa_modargs *ma,
                             const char *driver,
                             pa_droid_card_data *card_data,
                             audio_output_flags_t flags,
                             pa_droid_mapping *am,
                             pa_card *card) {

    struct userdata *u = NULL;
    pa_bool_t deferred_volume = FALSE;
    char *thread_name = NULL;
    pa_sink_new_data data;
    const char *module_id = NULL;
    const char *tmp;
    char *list = NULL;
    uint32_t alternate_sample_rate;
    uint32_t sample_rate;
    audio_devices_t dev_out;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    pa_bool_t namereg_fail = FALSE;
    uint32_t total_latency;
    pa_droid_config_audio *config = NULL; /* Only used when used without card */
    int ret;

    audio_format_t hal_audio_format = 0;
    audio_channel_mask_t hal_channel_mask = 0;

    pa_assert(m);
    pa_assert(ma);
    pa_assert(driver);

    deferred_volume = m->core->deferred_volume;
    if (pa_modargs_get_value_boolean(ma, "deferred_volume", &deferred_volume) < 0) {
        pa_log("Failed to parse deferred_volume argument.");
        goto fail;
    }

    if (card && am)
        module_id = am->output->module->name;
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
    u->deferred_volume = deferred_volume;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

    if (card_data) {
        u->card_data = card_data;
        pa_assert(card);
        pa_assert_se((u->hw_module = pa_droid_hw_module_get(u->core, NULL, card_data->module_id)));
    } else {
        /* Sink wasn't created from inside card module, so we'll need to open
         * hw module ourselves.
         * TODO some way to share hw module between other sinks/sources since
         * opening same module from different places likely isn't a good thing. */

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
        if (!pa_convert_output_channel(channel_map.map[i], CONV_FROM_PA, &c)) {
            pa_log("Failed to convert channel map.");
            goto fail;
        }
        hal_channel_mask |= c;
    }

    struct audio_config config_out = {
        .sample_rate = sample_spec.rate,
        .channel_mask = hal_channel_mask,
        .format = hal_audio_format
    };

    /* Default routing */
    dev_out = AUDIO_DEVICE_OUT_DEFAULT;

    if ((tmp = pa_modargs_get_value(ma, "output_devices", NULL))) {
        audio_devices_t tmp_dev;

        if (parse_device_list(tmp, &tmp_dev) && tmp_dev)
            dev_out = tmp_dev;

        pa_log_debug("Set initial devices %s", tmp);
    }

    if (am)
        flags = am->output->flags;

    pa_droid_hw_module_lock(u->hw_module);
    ret = u->hw_module->device->open_output_stream(u->hw_module->device,
                                                   u->hw_module->stream_out_id++,
                                                   dev_out,
                                                   flags,
                                                   &config_out,
                                                   &u->stream_out);
    pa_droid_hw_module_unlock(u->hw_module);

    if (!u->stream_out) {
        pa_log("Failed to open output stream. (errno %d)", ret);
        goto fail;
    }

    u->buffer_size = u->stream_out->common.get_buffer_size(&u->stream_out->common);
    u->buffer_latency = pa_bytes_to_usec(u->buffer_size, &sample_spec);
    /* Disable internal rewinding for now. */
    u->buffer_count = 1;

    if ((sample_rate = u->stream_out->common.get_sample_rate(&u->stream_out->common)) != sample_spec.rate) {
        pa_log_warn("Requested sample rate %u but got %u instead.", sample_spec.rate, sample_rate);
        sample_spec.rate = sample_rate;
    }

    pa_log_info("Created Android stream with device: %u flags: %u sample rate: %u channel mask: %u format: %u buffer size: %u",
            dev_out,
            flags,
            sample_rate,
            config_out.channel_mask,
            config_out.format,
            u->buffer_size);


    pa_silence_memchunk_get(&u->core->silence_cache, u->core->mempool, &u->silence, &sample_spec, 0);
    u->memblockq = pa_memblockq_new("droid-sink", 0, u->buffer_size * u->buffer_count, u->buffer_size * u->buffer_count, &sample_spec, 1, 0, 0, &u->silence);

    pa_sink_new_data_init(&data);
    data.driver = driver;
    data.module = m;
    data.card = card;

    set_sink_name(ma, &data, module_id);

    /* We need to give pa_modargs_get_value_boolean() a pointer to a local
     * variable instead of using &data.namereg_fail directly, because
     * data.namereg_fail is a bitfield and taking the address of a bitfield
     * variable is impossible. */
    namereg_fail = data.namereg_fail;
    if (pa_modargs_get_value_boolean(ma, "namereg_fail", &namereg_fail) < 0) {
        pa_log("Failed to parse namereg_fail argument.");
        pa_sink_new_data_done(&data);
        goto fail;
    }
    data.namereg_fail = namereg_fail;

    pa_sink_new_data_set_sample_spec(&data, &sample_spec);
    pa_sink_new_data_set_channel_map(&data, &channel_map);
    pa_sink_new_data_set_alternate_sample_rate(&data, alternate_sample_rate);

    /*
    if (!(list = pa_list_string_output_device(dev_out))) {
        pa_log("Couldn't format device list string.");
        goto fail;
    }
    pa_proplist_sets(data.proplist, PROP_DROID_DEVICES, list);
    pa_xfree(list);

    if (flags) {
        if (!(list = pa_list_string_flags(flags))) {
            pa_log("Couldn't format flag list string.");
            goto fail;
        }
    } else
        list = NULL;

    pa_proplist_sets(data.proplist, PROP_DROID_FLAGS, list ? list : "");
    pa_xfree(list);
    */

    if (am)
        pa_droid_add_ports(data.ports, am, card);

    u->sink = pa_sink_new(m->core, &data, PA_SINK_HARDWARE | PA_SINK_LATENCY | PA_SINK_FLAT_VOLUME);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink.");
        goto fail;
    }

    u->sink->userdata = u;

    u->sink->parent.process_msg = sink_process_msg;

    u->sink->set_port = sink_set_port_cb;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    /* Rewind internal memblockq */
    pa_sink_set_max_rewind(u->sink, u->buffer_size * (u->buffer_count - 1));

    thread_name = pa_sprintf_malloc("droid-sink-%s", module_id);
    if (!(u->thread = pa_thread_new(thread_name, thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }
    pa_xfree(thread_name);
    thread_name = NULL;

    /* Latency consists of HAL latency + our memblockq latency */
    total_latency = u->stream_out->get_latency(u->stream_out) + (uint32_t) pa_bytes_to_usec(u->buffer_size * u->buffer_count, &sample_spec);
    pa_sink_set_fixed_latency(u->sink, total_latency);
    pa_log_debug("Set fixed latency %lu usec", (unsigned long) pa_bytes_to_usec(total_latency, &sample_spec));
    pa_sink_set_max_request(u->sink, u->buffer_size * u->buffer_count);

    if (u->sink->active_port)
        sink_set_port_cb(u->sink, u->sink->active_port);

    /* Hooks to track appearance and disappearance of sink-inputs. */
    /* Hook a little bit later than module-role-cork. */
    u->sink_input_put_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE+10,
            (pa_hook_cb_t) sink_input_put_hook_cb, u);
    u->sink_input_unlink_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE+10,
            (pa_hook_cb_t) sink_input_unlink_hook_cb, u);

    update_volumes(u);

    pa_sink_put(u->sink);

    if (config)
        pa_xfree(config);

    return u->sink;

fail:
    pa_xfree(thread_name);

    if (config)
        pa_xfree(config);

    if (u)
        userdata_free(u);

    return NULL;
}

void pa_droid_sink_free(pa_sink *s) {
    struct userdata *u;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    userdata_free(u);
}

static void userdata_free(struct userdata *u) {

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink_input_put_hook_slot)
        pa_hook_slot_free(u->sink_input_put_hook_slot);

    if (u->sink_input_unlink_hook_slot)
        pa_hook_slot_free(u->sink_input_unlink_hook_slot);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->hw_module && u->stream_out) {
        pa_droid_hw_module_lock(u->hw_module);
        u->hw_module->device->close_output_stream(u->hw_module->device, u->stream_out);
        pa_droid_hw_module_unlock(u->hw_module);
    }

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

    if (u->silence.memblock)
        pa_memblock_unref(u->silence.memblock);

    if (u->hw_module)
        pa_droid_hw_module_unref(u->hw_module);

    pa_xfree(u);
}
