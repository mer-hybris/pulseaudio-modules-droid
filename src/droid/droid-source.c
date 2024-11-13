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
#include <pulse/util.h>
#include <pulse/version.h>

#include "droid-source.h"
#include <droid/droid-util.h>
#include <droid/conversion.h>

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_card *card;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_memchunk memchunk;

    size_t source_buffer_size;
    size_t buffer_size;
    pa_usec_t timestamp;

    pa_resampler *resampler;

    pa_droid_card_data *card_data;
    pa_droid_hw_module *hw_module;
    pa_droid_stream *stream;
    bool stream_valid;
};

#define DEFAULT_MODULE_ID "primary"

#define DROID_AUDIO_SOURCE "droid.audio_source"
#define DROID_AUDIO_SOURCE_UNDEFINED "undefined"

static void userdata_free(struct userdata *u);
static int suspend(struct userdata *u);
static void unsuspend(struct userdata *u);
static void source_reconfigure(struct userdata *u,
                               const pa_sample_spec *reconfigure_sample_spec,
                               const pa_channel_map *reconfigure_channel_map,
                               const pa_proplist *proplist,
                               dm_config_port *update_device_port);

/* Our droid source may be left in a state of not having an input stream
 * if reconfiguration fails and fallback to previously active values fails
 * as well. In this case just avoid using the stream but don't die. */
#define assert_stream(x, action) if (!x) do { pa_log_warn("Assert " #x " failed."); action; } while(0)

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
            pa_silence_memory(p, chunk.length, &u->source->sample_spec);
            pa_source_post(u->source, &chunk);
            pa_memblock_release(chunk.memblock);
            goto end;
        }
    }

    p = pa_memblock_acquire(chunk.memblock);
    readd = pa_droid_stream_read(u->stream, p, pa_memblock_get_length(chunk.memblock));
    pa_memblock_release(chunk.memblock);

    if (readd < 0) {
        pa_log("Failed to read from stream. (err %zd)", readd);
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
#if PA_CHECK_VERSION(13,0,0)
        pa_thread_make_realtime(u->core->realtime_priority);
#else
        pa_make_realtime(u->core->realtime_priority);
#endif

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

    pa_assert(u);
    assert_stream(u->stream, return 0);

    ret = pa_droid_stream_suspend(u->stream, true);

    if (ret == 0)
        pa_log_info("Device suspended.");

    return ret;
}

/* Called from IO context */
static void unsuspend(struct userdata *u) {
    pa_assert(u);

    if (!u->stream) {
        assert_stream(u->stream, u->stream_valid = false);
    } else if (pa_droid_stream_suspend(u->stream, false) >= 0) {
        u->stream_valid = true;
        pa_log_info("Resuming...");
    } else
        u->stream_valid = false;
}

/* Called from IO context */
static int source_set_state_in_io_thread_cb(pa_source *s, pa_source_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct userdata *u;
    int r;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    /* It may be that only the suspend cause is changing, in which case there's
     * nothing more to do. */
    if (new_state == s->thread_info.state)
        return 0;

    switch (new_state) {
        case PA_SOURCE_SUSPENDED:
            if (PA_SOURCE_IS_OPENED(u->source->thread_info.state)) {
                if ((r = suspend(u)) < 0)
                    return r;
            }

            break;

        case PA_SOURCE_IDLE:
            /* Fall through */
        case PA_SOURCE_RUNNING:
            if (u->source->thread_info.state == PA_SOURCE_SUSPENDED) {
                unsuspend(u);
                u->timestamp = pa_rtclock_now();
            }
            break;

        case PA_SOURCE_UNLINKED:
            /* Suspending since some implementations do not want to free running stream. */
            suspend(u);
            break;

        /* not needed */
        case PA_SOURCE_INIT:
        case PA_SOURCE_INVALID_STATE:
            break;
    }

    return 0;
}

static int source_set_port_cb(pa_source *s, pa_device_port *p) {
    struct userdata *u = s->userdata;
    pa_droid_port_data *data;

    pa_assert(u);
    pa_assert(p);

    data = PA_DEVICE_PORT_DATA(p);

    if (!data->device_port) {
        /* If there is no device defined, just return 0 to say everything is ok.
         * Then next port change can be whatever source port, even the one enabled
         * before parking. */
        pa_log_debug("Source set port to parking");
        return 0;
    }

    pa_log_debug("Source set port %#010x (%s)", data->device_port->type, data->device_port->name);

    if (!PA_SOURCE_IS_OPENED(u->source->state))
        pa_droid_stream_set_route(u->stream, data->device_port);
    else
        source_reconfigure(u, NULL, NULL, NULL, data->device_port);

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

static int source_get_mute_cb(pa_source *s, bool *muted) {
    struct userdata *u = s->userdata;

    pa_assert(u);
    pa_assert(u->hw_module);

    return pa_droid_hw_mic_get_mute(u->hw_module, muted);
}

static void source_set_mute_cb(pa_source *s) {
    struct userdata *u = s->userdata;

    pa_assert(u);

    pa_droid_hw_mic_set_mute(u->hw_module, s->muted);
}

static void source_set_mute_control(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->hw_module && u->hw_module->device);

    if (pa_droid_hw_has_mic_control(u->hw_module)) {
        pa_source_set_get_mute_callback(u->source, source_get_mute_cb);
        pa_source_set_set_mute_callback(u->source, source_set_mute_cb);
    }
}

/* Called from main and IO context */
static void update_latency(struct userdata *u) {
    pa_assert(u);
    pa_assert(u->source);

    if (u->stream)
        u->buffer_size = pa_droid_stream_buffer_size(u->stream);
    else
        u->buffer_size = 1024; /* Random valid value */

    assert_stream(u->stream, return);

    if (u->source_buffer_size) {
        u->buffer_size = pa_droid_buffer_size_round_up(u->source_buffer_size, u->buffer_size);
        pa_log_info("Using buffer size %zu (requested %zu).", u->buffer_size, u->source_buffer_size);
    } else
        pa_log_info("Using buffer size %zu.", u->buffer_size);

    if (pa_thread_mq_get())
        pa_source_set_fixed_latency_within_thread(u->source, pa_bytes_to_usec(u->buffer_size, pa_droid_stream_sample_spec(u->stream)));
    else
        pa_source_set_fixed_latency(u->source, pa_bytes_to_usec(u->buffer_size, pa_droid_stream_sample_spec(u->stream)));

    pa_log_debug("Set fixed latency %" PRIu64 " usec", pa_bytes_to_usec(u->buffer_size, pa_droid_stream_sample_spec(u->stream)));
}

static void source_reconfigure(struct userdata *u,
                               const pa_sample_spec *reconfigure_sample_spec,
                               const pa_channel_map *reconfigure_channel_map,
                               const pa_proplist *proplist,
                               dm_config_port *update_device_port) {
    pa_channel_map old_channel_map;
    pa_sample_spec old_sample_spec;
    pa_channel_map new_channel_map;
    pa_sample_spec new_sample_spec;
    pa_queue *source_outputs = NULL;

    if (pa_source_used_by(u->source)) {
        /* If we already have connected source outputs detach those
         * so that when re-attaching them to our source resampling etc.
         * is renegotiated correctly. */
        source_outputs = pa_source_move_all_start(u->source, NULL);
    }

    pa_source_suspend(u->source, true, PA_SUSPEND_UNAVAILABLE);

    old_channel_map = *pa_droid_stream_channel_map(u->stream);
    old_sample_spec = *pa_droid_stream_sample_spec(u->stream);
    new_channel_map = reconfigure_channel_map ? *reconfigure_channel_map : old_channel_map;
    new_sample_spec = reconfigure_sample_spec ? *reconfigure_sample_spec : old_sample_spec;

    if (update_device_port)
        pa_droid_stream_set_route(u->stream, update_device_port);

    if (pa_droid_stream_reconfigure_input(u->stream, &new_sample_spec, &new_channel_map, proplist))
        pa_log_info("Source reconfigured.");
    else
        pa_log_info("Failed to reconfigure input stream, no worries, using defaults.");

    /* We need to be really careful here as we are modifying
     * quite profound internal structures. */
    new_sample_spec = *pa_droid_stream_sample_spec(u->stream);
    new_channel_map = *pa_droid_stream_channel_map(u->stream);
    u->source->channel_map = new_channel_map;
    u->source->sample_spec = new_sample_spec;
    pa_assert_se(pa_cvolume_remap(&u->source->reference_volume, &old_channel_map, &new_channel_map));
    pa_assert_se(pa_cvolume_remap(&u->source->real_volume, &old_channel_map, &new_channel_map));
    pa_assert_se(pa_cvolume_remap(&u->source->soft_volume, &old_channel_map, &new_channel_map));

    update_latency(u);
    pa_source_suspend(u->source, false, PA_SUSPEND_UNAVAILABLE);

    if (source_outputs && u->source) {
        pa_source_move_all_finish(u->source, source_outputs, false);
    }
}

static pa_hook_result_t source_output_new_hook_callback(void *hook_data,
                                                        void *call_data,
                                                        void *slot_data) {
    pa_source_output_new_data *new_data = call_data;
    struct userdata *u = slot_data;
    pa_droid_stream *primary_output;

    /* Not meant for us */
    if (new_data->source != u->source)
        return PA_HOOK_OK;

    if (!pa_droid_stream_reconfigure_input_needed(u->stream,
                                                  &new_data->sample_spec,
                                                  &new_data->channel_map,
                                                  new_data->proplist))
        return PA_HOOK_OK;

    pa_log_info("New source-output connecting and our source needs to be reconfigured.");

    /* Workaround for fm-radio loopback */
    if (pa_safe_streq(pa_proplist_gets(new_data->proplist, "media.name"), "fmradio-loopback-source") &&
        (primary_output = pa_droid_hw_primary_output_stream(u->hw_module))) {
        pa_log_debug("Workaround for fm-radio loopback.");
        source_reconfigure(u,
                           pa_droid_stream_sample_spec(primary_output),
                           pa_droid_stream_channel_map(primary_output),
                           new_data->proplist,
                           NULL);

    } else
        source_reconfigure(u, &new_data->sample_spec, &new_data->channel_map, new_data->proplist, NULL);

    return PA_HOOK_OK;
}

static void source_reconfigure_after_changes(struct userdata *u) {
    pa_source_output *so = NULL;
    pa_source_output *so_i;
    void *state = NULL;

    if (!pa_source_used_by(u->source))
        return;

    /* Find last inserted source-output */
    so = pa_idxset_iterate(u->source->outputs, &state, NULL);
    if (so) {
        while ((so_i = pa_idxset_iterate(u->source->outputs, &state, NULL)))
            so = so_i;
    }

    if (so && pa_droid_stream_reconfigure_input_needed(u->stream,
                                                       &so->sample_spec,
                                                       &so->channel_map,
                                                       so->proplist)) {
        pa_log_info("Source-output disconnected and our source needs to be reconfigured.");
        source_reconfigure(u, &so->sample_spec, &so->channel_map, so->proplist, NULL);
    }
}

static pa_hook_result_t source_output_unlink_post_hook_callback(void *hook_data,
                                                                void *call_data,
                                                                void *slot_data) {
    source_reconfigure_after_changes(slot_data);
    return PA_HOOK_OK;
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
    uint32_t alternate_sample_rate;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    const char *format;
    bool namereg_fail = false;
    uint32_t source_buffer = 0;

    pa_assert(m);
    pa_assert(ma);
    pa_assert(driver);

    pa_log_info("Create new droid-source");

    /* When running under card use hw module name for source by default. */
    if (am)
        module_id = am->mix_port->name;
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
        /* Source wasn't created from inside card module, so we'll need to open
         * hw module ourself. */

        if (!(u->hw_module = pa_droid_hw_module_get2(u->core, ma, module_id)))
            goto fail;
    }

    u->stream = pa_droid_open_input_stream(u->hw_module, &sample_spec, &channel_map, am->mix_port->name);

    if (!u->stream) {
        pa_log("Failed to open input stream.");
        goto fail;
    }

    pa_source_new_data_init(&data);
    data.driver = driver;
    data.module = m;
    data.card = card;
    /* Start suspended */
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

    pa_source_new_data_set_sample_spec(&data, pa_droid_stream_sample_spec(u->stream));
    pa_source_new_data_set_channel_map(&data, pa_droid_stream_channel_map(u->stream));
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

    u->source->parent.process_msg = pa_source_process_msg;
    u->source->set_state_in_io_thread = source_set_state_in_io_thread_cb;

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

    /* Since we started in suspended mode suspend our stream immediately as well. */
    pa_droid_stream_suspend(u->stream, true);

    pa_droid_stream_set_data(u->stream, u->source);
    pa_source_put(u->source);

    /* As late as possible */
    pa_module_hook_connect(u->module,
                           &u->module->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW],
                           PA_HOOK_LATE * 2,
                           source_output_new_hook_callback, u);

    pa_module_hook_connect(u->module,
                           &u->module->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK_POST],
                           PA_HOOK_LATE * 2,
                           source_output_unlink_post_hook_callback, u);

    return u->source;

fail:
    pa_xfree(thread_name);

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

    if (u->hw_module)
        pa_droid_hw_module_unref(u->hw_module);

    pa_xfree(u);
}
