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
#include <pulsecore/hashmap.h>
#include <pulsecore/core-subscribe.h>
#include <pulse/util.h>
#include <pulse/version.h>

#include "droid-sink.h"
#include <droid/droid-util.h>
#include <droid/conversion.h>
#include <droid/sllist.h>

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_card *card;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    bool deferred_volume; /* TODO */

    pa_memblockq *memblockq;
    pa_memchunk silence;
    size_t buffer_size;
    pa_usec_t buffer_time;
    pa_usec_t write_time;
    pa_usec_t write_threshold;

    dm_config_port *active_device_port;
    dm_config_port *override_device_port;
    dm_list *extra_devices_stack;

    bool use_hw_volume;
    bool use_voice_volume;
    char *voice_property_key;
    char *voice_property_value;
    pa_hook_slot *sink_input_volume_changed_hook_slot;

    pa_hook_slot *sink_input_put_hook_slot;
    pa_hook_slot *sink_input_unlink_hook_slot;
    pa_hook_slot *sink_proplist_changed_hook_slot;
    pa_hashmap *parameters;

    pa_droid_card_data *card_data;
    pa_droid_hw_module *hw_module;
    pa_droid_stream *stream;
};

#define DEFAULT_MODULE_ID "primary"

/* sink properties */
#define PROP_DROID_PARAMETER_PREFIX "droid.parameter."
typedef struct droid_parameter_mapping {
    char *key;
    char *value;
} droid_parameter_mapping;

/* sink-input properties */
#define PROP_DROID_ROUTE "droid.device.additional-route"

/* Voice call volume control.
 * With defaults defined below, whenever sink-input with proplist key "media.role" with
 * value "phone" connects to the sink AND voice volume control is enabled, that connected
 * sink-input's absolute volume is used for HAL voice volume. */
#define DEFAULT_VOICE_CONTROL_PROPERTY_KEY      "media.role"
#define DEFAULT_VOICE_CONTROL_PROPERTY_VALUE    "phone"

static void parameter_free(droid_parameter_mapping *m);
static void userdata_free(struct userdata *u);
static void set_voice_volume(struct userdata *u, pa_sink_input *i);
static void apply_volume(pa_sink *s);
static pa_sink_input *find_volume_control_sink_input(struct userdata *u);

static bool add_extra_devices(struct userdata *u, audio_devices_t device) {
    dm_list_entry *prev;
    dm_config_port *device_port;

    pa_assert(u);
    pa_assert(u->extra_devices_stack);

    if (!(device_port = dm_config_find_device_port(u->active_device_port, device))) {
        pa_log("Unknown device port %u", device);
        return false;
    }

    prev = dm_list_last(u->extra_devices_stack);

    dm_list_push_back(u->extra_devices_stack, device_port);

    if (prev) {
        dm_config_port *last_port = prev->data;
        if (dm_config_port_equal(last_port, device_port))
            return false;
    }

    u->override_device_port = device_port;

    return true;
}

static bool remove_extra_devices(struct userdata *u, audio_devices_t device) {
    dm_config_port *device_port;
    dm_list_entry *remove = NULL, *i = NULL;
    bool need_update = false;

    pa_assert(u);
    pa_assert(u->extra_devices_stack);

    if (!(device_port = dm_config_find_device_port(u->active_device_port, device))) {
        pa_log("Unknown device port %u", device);
        return false;
    }

    DM_LIST_FOREACH(i, u->extra_devices_stack) {
        if (dm_config_port_equal(i->data, device_port)) {
            remove = i;
            break;
        }
    }

    if (remove && dm_list_last(u->extra_devices_stack) == remove)
        need_update = true;

    if (remove)
        dm_list_remove(u->extra_devices_stack, remove);

    return need_update;
}

static void clear_extra_devices(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->extra_devices_stack);

    while (dm_list_steal_first(u->extra_devices_stack));
    u->override_device_port = NULL;
}

/* Called from main context during voice calls, and from IO context during media operation. */
static void do_routing(struct userdata *u) {
    dm_config_port *routing = NULL;

    pa_assert(u);
    pa_assert(u->stream);

    if (u->use_voice_volume && u->override_device_port)
        clear_extra_devices(u);

    if (u->override_device_port)
        routing = u->override_device_port;
    else
        routing = u->active_device_port;

    pa_droid_stream_set_route(u->stream, routing);
}

static bool parse_device_list(const char *str, audio_devices_t *dst) {
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
            return false;
        }

        *dst |= d;

        pa_xfree(dev);
    }

    return true;
}

static int thread_write(struct userdata *u) {
    pa_memchunk c;
    const void *p;
    ssize_t wrote;

    pa_memblockq_peek_fixed_size(u->memblockq, u->buffer_size, &c);

    /* We should be able to write everything in one go as long as memblock size
     * is multiples of buffer_size. */

    u->write_time = pa_rtclock_now();

    for (;;) {
        p = pa_memblock_acquire_chunk(&c);
        wrote = pa_droid_stream_write(u->stream, p, c.length);
        pa_memblock_release(c.memblock);

        if (wrote < 0) {
            pa_memblockq_drop(u->memblockq, c.length);
            pa_memblock_unref(c.memblock);
            u->write_time = 0;
            pa_log("failed to write stream (%zd)", wrote);
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

    u->write_time = pa_rtclock_now() - u->write_time;

    return 0;
}
static void thread_render(struct userdata *u) {
    size_t length;
    size_t missing;

    length = pa_memblockq_get_length(u->memblockq);
    missing = u->buffer_size - length;

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
#if PA_CHECK_VERSION(13,0,0)
        pa_thread_make_realtime(u->core->realtime_priority);
#else
        pa_make_realtime(u->core->realtime_priority);
#endif

    pa_thread_mq_install(&u->thread_mq);

    for (;;) {
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {

            if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
                process_rewind(u);

            if (pa_rtpoll_timer_elapsed(u->rtpoll)) {
                pa_usec_t sleept = 0;

                if (u->use_hw_volume)
                    pa_sink_volume_change_apply(u->sink, NULL);

                thread_render(u);
                thread_write(u);

                if (u->write_time > u->write_threshold)
                    sleept = u->buffer_time;

                pa_rtpoll_set_timer_relative(u->rtpoll, sleept);

                if (u->use_hw_volume)
                    pa_sink_volume_change_apply(u->sink, NULL);
            }
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
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

    ret = pa_droid_stream_suspend(u->stream, true);

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

/* Called from IO context */
static int unsuspend(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->sink);

    /* HAL resumes automagically when writing to standby stream, but let's set max request */
    pa_sink_set_max_request_within_thread(u->sink, u->buffer_size);

    pa_log_info("Resuming...");

    apply_volume(u->sink);

    pa_droid_stream_suspend(u->stream, false);

    return 0;
}

/* Called from IO context */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct userdata *u;
    int r;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    /* It may be that only the suspend cause is changing, in which case there's
     * nothing more to do. */
    if (new_state == s->thread_info.state)
        return 0;

    switch (new_state) {
        case PA_SINK_SUSPENDED:
            pa_assert(PA_SINK_IS_OPENED(u->sink->thread_info.state));

            if ((r = suspend(u)) < 0)
                return r;

            break;

        case PA_SINK_IDLE:
            /* Fall through */
        case PA_SINK_RUNNING:
            if (u->sink->thread_info.state == PA_SINK_SUSPENDED) {
                if ((r = unsuspend(u)) < 0)
                    return r;
            }

            pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
            break;

        case PA_SINK_UNLINKED:
            /* Suspending since some implementations do not want to free running stream. */
            suspend(u);
            break;

        /* not needed */
        case PA_SINK_INIT:
        case PA_SINK_INVALID_STATE:
            break;
    }

    return 0;
}

/* Called from IO context */
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY: {
            *((pa_usec_t*) data) = pa_droid_stream_get_latency(u->stream);
            return 0;
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

    if (!data->device_port) {
        /* If there is no device defined, just return 0 to say everything is ok.
         * Then next port change can be whatever sink port, even the one enabled
         * before parking. */
        pa_log_debug("Sink set port to parking");
        return 0;
    }

    pa_log_debug("Sink set port %#010x (%s)", data->device_port->type, data->device_port->name);

    u->active_device_port = data->device_port;
    do_routing(u);

    return 0;
}

static void apply_volume(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume r;
    float val;

    if (u->use_voice_volume)
        return;

    if (!u->use_hw_volume)
        return;

    /* Shift up by the base volume */
    pa_sw_cvolume_divide_scalar(&r, &s->real_volume, s->base_volume);

    /* So far every hal implementation doing volume control expects
     * both channels to have equal value, so we can just average the value
     * from all channels. */
    val = pa_sw_volume_to_linear(pa_cvolume_avg(&r));

    pa_log_debug("Set %s volume -> %f", s->name, val);
    pa_droid_hw_module_lock(u->hw_module);
    if (u->stream->output->stream->set_volume(u->stream->output->stream, val, val) < 0)
        pa_log_warn("Failed to set volume.");
    pa_droid_hw_module_unlock(u->hw_module);
}

static void sink_set_volume_cb(pa_sink *s) {
    (void) s;
    /* noop */
}

static void sink_write_volume_cb(pa_sink *s) {
    apply_volume(s);
}

/* Called from main thread */
static void set_voice_volume(struct userdata *u, pa_sink_input *i) {
    pa_cvolume vol;
    float val;

    pa_assert_ctl_context();
    pa_assert(u);
    pa_assert(i);

    pa_sink_input_get_volume(i, &vol, true);

    val = pa_sw_volume_to_linear(pa_cvolume_avg(&vol));
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
    if (u->stream->output->stream->set_volume) {
        ret = u->stream->output->stream->set_volume(u->stream->output->stream, 1.0f, 1.0f);
        pa_log_debug("Probe hw volume support for %s (ret %d)", u->sink->name, ret);
    }
    pa_droid_hw_module_unlock(u->hw_module);

    u->use_hw_volume = (ret == 0);
    if (u->use_hw_volume &&
#if defined(HAVE_ENUM_AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
        !(u->stream->mix_port->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) &&
#endif
        !pa_droid_option(u->hw_module, DM_OPTION_HW_VOLUME)) {
        pa_log_info("Forcing software volume control with %s", u->sink->name);
        u->use_hw_volume = false;
    } else {
        pa_log_debug("Using %s volume control with %s",
                     u->use_hw_volume ? "hardware" : "software", u->sink->name);
    }

    if (u->use_hw_volume) {
        pa_sink_set_set_volume_callback(u->sink, sink_set_volume_cb);
        pa_sink_set_write_volume_callback(u->sink, sink_write_volume_cb);
    }
}

static void set_sink_name(pa_modargs *ma, pa_sink_new_data *data, pa_droid_mapping *am, const char *name) {
    const char *tmp;

    pa_assert(ma);
    pa_assert(data);

    if ((tmp = pa_modargs_get_value(ma, "sink_name", NULL))) {
        pa_sink_new_data_set_name(data, tmp);
        data->namereg_fail = true;
        pa_proplist_sets(data->proplist, PA_PROP_DEVICE_DESCRIPTION, "Droid sink");
    } else {
        char *full_name;
        pa_assert(name);
        pa_assert(am);
        full_name = pa_sprintf_malloc("sink.%s", name);
        pa_sink_new_data_set_name(data, full_name);
        pa_xfree(full_name);
        data->namereg_fail = false;
        pa_proplist_setf(data->proplist, PA_PROP_DEVICE_DESCRIPTION, "Droid sink %s", am->name);
    }
}

static bool sink_input_is_voice_control(struct userdata *u, pa_sink_input *si) {
    const char *val;

    pa_assert(u);
    pa_assert(si);

    if ((val = pa_proplist_gets(si->proplist, u->voice_property_key))) {
        if (pa_streq(val, u->voice_property_value))
            return true;
    }

    return false;
}

/* Called from main thread */
static pa_sink_input *find_volume_control_sink_input(struct userdata *u) {
    uint32_t idx;
    pa_sink_input *i;

    pa_assert_ctl_context();
    pa_assert(u);
    pa_assert(u->sink);

    PA_IDXSET_FOREACH(i, u->sink->inputs, idx) {
        if (sink_input_is_voice_control(u, i))
            return i;
    }

    return NULL;
}

/* Called from main thread */
static pa_hook_result_t sink_input_volume_changed_hook_cb(pa_core *c, pa_sink_input *sink_input, struct userdata *u) {
    pa_assert(c);
    pa_assert(sink_input);
    pa_assert(u);

    if (!u->use_voice_volume)
        return PA_HOOK_OK;

    if (sink_input_is_voice_control(u, sink_input))
        set_voice_volume(u, sink_input);

    return PA_HOOK_OK;
}

/* Called from main thread */
void pa_droid_sink_set_voice_control(pa_sink* sink, bool enable) {
    pa_sink_input *i;
    struct userdata *u;

    pa_assert_ctl_context();
    pa_assert(sink);

    u = sink->userdata;
    pa_assert(u);
    pa_assert(u->sink == sink);

    if (!pa_droid_stream_is_primary(u->stream)) {
        pa_log_debug("Skipping voice volume control with non-primary sink %s", u->sink->name);
        return;
    }

    if (u->use_voice_volume == enable)
        return;

    u->use_voice_volume = enable;

    if (u->use_voice_volume) {
        pa_log_debug("Using voice volume control with %s", u->sink->name);

        pa_assert(!u->sink_input_volume_changed_hook_slot);

        u->sink_input_volume_changed_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_VOLUME_CHANGED],
                PA_HOOK_LATE+10, (pa_hook_cb_t) sink_input_volume_changed_hook_cb, u);

        if ((i = find_volume_control_sink_input(u))) {
            set_voice_volume(u, i);
        }

    } else {
        pa_assert(u->sink_input_volume_changed_hook_slot);

        pa_hook_slot_free(u->sink_input_volume_changed_hook_slot);
        u->sink_input_volume_changed_hook_slot = NULL;

        pa_log_debug("Using %s volume control with %s",
                     u->use_hw_volume ? "hardware" : "software", u->sink->name);
    }
}

/* When sink-input with proper proplist variable appears, do extra routing configuration
 * for the lifetime of that sink-input. */
static pa_hook_result_t sink_input_put_hook_cb(pa_core *c, pa_sink_input *sink_input, struct userdata *u) {
    const char *dev_str;
    const char *media_str;
    audio_devices_t devices;

    if (u->use_voice_volume && sink_input_is_voice_control(u, sink_input)) {
        set_voice_volume(u, sink_input);
    }

    /* Dynamic routing changes do not apply during active voice call. */
    if (u->use_voice_volume)
        return PA_HOOK_OK;

    if ((dev_str = pa_proplist_gets(sink_input->proplist, PROP_DROID_ROUTE))) {

        /* Do not change routing for gstreamer pulsesink probe. Workaround for unnecessary routing changes when gst-plugin
         * pulsesink connects to our sink. Not the best fix or the best place for a fix, but let's have this here
         * for now anyway. */
        if ((media_str = pa_proplist_gets(sink_input->proplist, PA_PROP_MEDIA_NAME)) && pa_streq(media_str, "pulsesink probe"))
            return PA_HOOK_OK;

        if (parse_device_list(dev_str, &devices) && devices) {

            pa_log_debug("%s: Add extra route %s (%u).", u->sink->name, dev_str, devices);

            /* if this device was not routed to previously post routing change */
            if (add_extra_devices(u, devices))
                do_routing(u);
        }
    }

    return PA_HOOK_OK;
}

/* Remove extra routing when sink-inputs disappear. */
static pa_hook_result_t sink_input_unlink_hook_cb(pa_core *c, pa_sink_input *sink_input, struct userdata *u) {
    const char *dev_str;
    const char *media_str;
    audio_devices_t devices;

    /* Dynamic routing changes do not apply during active voice call. */
    if (u->use_voice_volume)
        return PA_HOOK_OK;

    if ((dev_str = pa_proplist_gets(sink_input->proplist, PROP_DROID_ROUTE))) {

        /* Do not change routing for gstreamer pulsesink probe. Workaround for unnecessary routing changes when gst-plugin
         * pulsesink connects to our sink. Not the best fix or the best place for a fix, but let's have this here
         * for now anyway. */
        if ((media_str = pa_proplist_gets(sink_input->proplist, PA_PROP_MEDIA_NAME)) && pa_streq(media_str, "pulsesink probe"))
            return PA_HOOK_OK;

        if (parse_device_list(dev_str, &devices) && devices) {

            pa_log_debug("Remove extra route %s (%u).", dev_str, devices);

            /* if this device no longer exists in extra devices map post routing change */
            if (remove_extra_devices(u, devices))
                do_routing(u);
        }
    }

    return PA_HOOK_OK;
}

/* Watch for properties starting with droid.parameter. and translate them directly to
 * HAL set_parameters() calls. */
static pa_hook_result_t sink_proplist_changed_hook_cb(pa_core *c, pa_sink *sink, struct userdata *u) {
    bool changed = false;
    const char *pkey;
    const char *key;
    const char *value;
    char *tmp;
    void *state = NULL;
    droid_parameter_mapping *parameter = NULL;

    pa_assert(sink);
    pa_assert(u);

    if (u->sink != sink)
        return PA_HOOK_OK;

    while ((key = pa_proplist_iterate(sink->proplist, &state))) {
        if (!pa_startswith(key, PROP_DROID_PARAMETER_PREFIX))
            continue;

        pkey = key + strlen(PROP_DROID_PARAMETER_PREFIX);
        if (pkey[0] == '\0')
            continue;

        changed = false;

        if (!(parameter = pa_hashmap_get(u->parameters, pkey))) {
            parameter = pa_xnew0(droid_parameter_mapping, 1);
            parameter->key = pa_xstrdup(pkey);
            parameter->value = pa_xstrdup(pa_proplist_gets(sink->proplist, key));
            pa_hashmap_put(u->parameters, parameter->key, parameter);
            changed = true;
        } else {
            value = pa_proplist_gets(sink->proplist, key);
            if (!pa_streq(parameter->value, value)) {
                pa_xfree(parameter->value);
                parameter->value = pa_xstrdup(value);
                changed = true;
            }
        }

        if (changed) {
            pa_assert(parameter);
            tmp = pa_sprintf_malloc("%s=%s;", parameter->key, parameter->value);
            pa_log_debug("set_parameters(): %s", tmp);
            pa_droid_stream_set_parameters(u->stream, tmp);
            pa_xfree(tmp);
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
    dm_config_port *mix_port = NULL;
    dm_config_port *device_port = NULL;
    bool deferred_volume = false;
    char *thread_name = NULL;
    pa_sink_new_data data;
    const char *module_id = NULL;
    char *list = NULL;
    uint32_t alternate_sample_rate;
    const char *format;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    bool namereg_fail = false;
    pa_usec_t latency;
    uint32_t sink_buffer = 0;
    char *sink_name = NULL;

    pa_assert(m);
    pa_assert(ma);
    pa_assert(driver);

    pa_log_info("Create new droid-sink");

    deferred_volume = m->core->deferred_volume;
    if (pa_modargs_get_value_boolean(ma, "deferred_volume", &deferred_volume) < 0) {
        pa_log("Failed to parse deferred_volume argument.");
        goto fail;
    }

    if (card && am) {
        mix_port = am->mix_port;
        module_id = mix_port->name;
    } else
        module_id = pa_modargs_get_value(ma, "module_id", DEFAULT_MODULE_ID);

    sample_spec = m->core->default_sample_spec;
    channel_map = m->core->default_channel_map;

    /* First parse both sample spec and channel map, then see if sink_* override some
     * of the values. */

    if (pa_modargs_get_sample_spec(ma, &sample_spec) < 0) {
        pa_log("Failed to parse sink sample specification.");
        goto fail;
    }

    if (pa_modargs_get_channel_map(ma, NULL, &channel_map) < 0) {
        pa_log("Failed to parse sink channel map.");
        goto fail;
    }

    /* Possible overrides. */

    if (pa_modargs_get_channel_map(ma, "sink_channel_map", &channel_map) < 0) {
        pa_log("Failed to parse sink channel map.");
        goto fail;
    }

    if ((format = pa_modargs_get_value(ma, "sink_format", NULL))) {
        if ((sample_spec.format = pa_parse_sample_format(format)) < 0) {
            pa_log("Failed to parse sink format.");
            goto fail;
        }
    }

    if (pa_modargs_get_value_u32(ma, "rate", &sample_spec.rate) < 0) {
        pa_log("Failed to parse sink samplerate");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "sink_rate", &sample_spec.rate) < 0) {
        pa_log("Failed to parse sink samplerate");
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

    if (pa_modargs_get_value_u32(ma, "sink_buffer", &sink_buffer) < 0) {
        pa_log("Failed to parse sink_buffer. Needs to be integer >= 0.");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->card = card;
    u->deferred_volume = deferred_volume;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);
    u->parameters = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                        NULL, (pa_free_cb_t) parameter_free);
    u->voice_property_key   = pa_xstrdup(pa_modargs_get_value(ma, "voice_property_key", DEFAULT_VOICE_CONTROL_PROPERTY_KEY));
    u->voice_property_value = pa_xstrdup(pa_modargs_get_value(ma, "voice_property_value", DEFAULT_VOICE_CONTROL_PROPERTY_VALUE));
    u->extra_devices_stack = dm_list_new();

    if (card_data) {
        u->card_data = card_data;
        pa_assert(card);
        pa_assert_se((u->hw_module = pa_droid_hw_module_get(u->core, NULL, card_data->module_id)));
    } else {
        const char *output_name;

        if (!(output_name = pa_modargs_get_value(ma, "output", NULL))) {
            pa_log("No output name defined.");
            goto fail;
        }

        /* Sink wasn't created from inside card module, so we'll need to open
         * hw module ourself. */

        if (!(u->hw_module = pa_droid_hw_module_get2(u->core, ma, module_id)))
            goto fail;

        if (!(mix_port = dm_config_find_port(u->hw_module->enabled_module, output_name)) ||
             mix_port->port_type != DM_CONFIG_TYPE_MIX_PORT) {
            pa_log("Could not find output %s from module %s.", output_name, u->hw_module->enabled_module->name);
            goto fail;
        }
    }

    pa_assert(mix_port);

    /* Start with default output device */
    device_port = dm_config_default_output_device(mix_port->module);

    u->stream = pa_droid_open_output_stream(u->hw_module, &sample_spec, &channel_map, mix_port, device_port);

    if (!u->stream) {
        pa_log("Failed to open output stream.");
        goto fail;
    }

    u->buffer_size = pa_droid_stream_buffer_size(u->stream);
    if (sink_buffer) {
        u->buffer_size = pa_droid_buffer_size_round_up(sink_buffer, u->buffer_size);
        pa_log_info("Using buffer size %zu (requested %u).", u->buffer_size, sink_buffer);
    } else
        pa_log_info("Using buffer size %zu.", u->buffer_size);

    u->buffer_time = pa_bytes_to_usec(u->buffer_size, &u->stream->output->sample_spec);
    u->write_threshold = u->buffer_time - u->buffer_time / 6;

    pa_silence_memchunk_get(&u->core->silence_cache, u->core->mempool, &u->silence, &u->stream->output->sample_spec, u->buffer_size);
    u->memblockq = pa_memblockq_new("droid-sink", 0, u->buffer_size, u->buffer_size, &u->stream->output->sample_spec, 1, 0, 0, &u->silence);

    pa_sink_new_data_init(&data);
    data.driver = driver;
    data.module = m;
    data.card = card;

    sink_name = dm_config_escape_string(module_id);
    set_sink_name(ma, &data, am, sink_name);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, PROP_DROID_API_STRING);

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

    pa_sink_new_data_set_sample_spec(&data, &u->stream->output->sample_spec);
    pa_sink_new_data_set_channel_map(&data, &u->stream->output->channel_map);
    pa_sink_new_data_set_alternate_sample_rate(&data, alternate_sample_rate);

    if (flags) {
        if (!(list = pa_list_string_flags(flags))) {
            pa_log("Couldn't format flag list string.");
            goto fail;
        }
    } else
        list = NULL;

    pa_proplist_sets(data.proplist, PROP_DROID_FLAGS, list ? list : "");
    pa_xfree(list);

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
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    /* Rewind internal memblockq */
    pa_sink_set_max_rewind(u->sink, 0);

    thread_name = pa_sprintf_malloc("droid-sink-%s", sink_name);
    if (!(u->thread = pa_thread_new(thread_name, thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }
    pa_xfree(thread_name);
    thread_name = NULL;

    /* HAL latencies are in milliseconds. */
    latency = pa_droid_stream_get_latency(u->stream);
    pa_sink_set_fixed_latency(u->sink, latency);
    pa_log_debug("Set fixed latency %" PRIu64 " usec", latency);
    pa_sink_set_max_request(u->sink, u->buffer_size);

    if (u->sink->active_port)
        sink_set_port_cb(u->sink, u->sink->active_port);

    if (pa_droid_stream_is_primary(u->stream)) {
        /* Hooks to track appearance and disappearance of sink-inputs.
         * Hook a little bit earlier and later than module-role-ducking.
         * Used only in primary sink. */
        u->sink_input_put_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE+10,
                (pa_hook_cb_t) sink_input_put_hook_cb, u);
        u->sink_input_unlink_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_EARLY-10,
                (pa_hook_cb_t) sink_input_unlink_hook_cb, u);
        u->sink_proplist_changed_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PROPLIST_CHANGED], PA_HOOK_EARLY,
                (pa_hook_cb_t) sink_proplist_changed_hook_cb, u);

        /* Port changes are done only in primary sink. */
        u->sink->set_port = sink_set_port_cb;
    }

    update_volumes(u);

    pa_droid_stream_suspend(u->stream, false);
    pa_droid_stream_set_data(u->stream, u->sink);
    pa_sink_put(u->sink);

    pa_xfree(sink_name);

    return u->sink;

fail:
    pa_xfree(thread_name);
    pa_xfree(sink_name);

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

static void parameter_free(droid_parameter_mapping *m) {
    pa_assert(m);

    pa_xfree(m->key);
    pa_xfree(m->value);
    pa_xfree(m);
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

    if (u->sink_input_volume_changed_hook_slot)
        pa_hook_slot_free(u->sink_input_volume_changed_hook_slot);

    if (u->sink_proplist_changed_hook_slot)
        pa_hook_slot_free(u->sink_proplist_changed_hook_slot);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->parameters)
        pa_hashmap_free(u->parameters);

    if (u->stream)
        pa_droid_stream_unref(u->stream);

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

    if (u->silence.memblock)
        pa_memblock_unref(u->silence.memblock);

    if (u->hw_module)
        pa_droid_hw_module_unref(u->hw_module);

    if (u->voice_property_key)
        pa_xfree(u->voice_property_key);
    if (u->voice_property_value)
        pa_xfree(u->voice_property_value);

    if (u->extra_devices_stack)
        dm_list_free(u->extra_devices_stack, NULL);

    pa_xfree(u);
}
