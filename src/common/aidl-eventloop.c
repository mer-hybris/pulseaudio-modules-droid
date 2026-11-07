/*
 * Copyright (C) 2026 TheKit <thekit@disroot.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * GBinderEventLoopIntegration on top of pa_mainloop_api.
 *
 * libgbinder schedules callbacks from its binder looper threads, so
 * callback_schedule() must be thread safe: callbacks go into a locked
 * queue and an eventfd wakes the PulseAudio main loop to run them.
 * Timeouts are only ever added from the main thread.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <glib.h>
#include <gbinder_eventloop.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "droid/aidl-eventloop.h"

struct el_callback {
    GBinderEventLoopCallback base;
    GBinderEventLoopCallbackFunc func;
    gpointer data;
    GDestroyNotify finalize;
    int refcount;
    bool queued;
};

struct el_timeout {
    GBinderEventLoopTimeout base;
    pa_time_event *event;
    GSourceFunc func;
    gpointer data;
    guint millis;
};

static struct {
    pa_core *core;
    int users;
    int efd;
    pa_io_event *io;
    pthread_mutex_t lock;
    GQueue queue;              /* struct el_callback*, holds a reference each */
} el = { .efd = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

static const GBinderEventLoopIntegration el_integration;

static void callback_unref(GBinderEventLoopCallback *cb);

static void callback_run_queued(void) {
    for (;;) {
        struct el_callback *cb;

        pthread_mutex_lock(&el.lock);
        cb = g_queue_pop_head(&el.queue);
        if (cb)
            cb->queued = false;
        pthread_mutex_unlock(&el.lock);

        if (!cb)
            break;

        cb->func(cb->data);
        callback_unref(&cb->base);
    }
}

static void io_cb(pa_mainloop_api *a, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
    uint64_t v;

    if (read(fd, &v, sizeof(v)) < 0 && errno != EAGAIN)
        pa_log_warn("aidl-eventloop: eventfd read: %s", pa_cstrerror(errno));

    callback_run_queued();
}

static void timeout_cb(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv, void *userdata) {
    struct el_timeout *t = userdata;

    if (t->func(t->data) == G_SOURCE_CONTINUE) {
        pa_core_rttime_restart(el.core, e, pa_rtclock_now() + (pa_usec_t) t->millis * PA_USEC_PER_MSEC);
        return;
    }

    a->time_free(e);
    pa_xfree(t);
}

static GBinderEventLoopTimeout *timeout_add(guint millis, GSourceFunc func, gpointer data) {
    struct el_timeout *t = pa_xnew0(struct el_timeout, 1);

    t->base.eventloop = &el_integration;
    t->func = func;
    t->data = data;
    t->millis = millis;
    t->event = pa_core_rttime_new(el.core, pa_rtclock_now() + (pa_usec_t) millis * PA_USEC_PER_MSEC, timeout_cb, t);
    return &t->base;
}

static void timeout_remove(GBinderEventLoopTimeout *timeout) {
    struct el_timeout *t = (struct el_timeout *) timeout;

    if (el.core)
        el.core->mainloop->time_free(t->event);
    pa_xfree(t);
}

static GBinderEventLoopCallback *callback_new(GBinderEventLoopCallbackFunc func, gpointer data, GDestroyNotify finalize) {
    struct el_callback *cb = pa_xnew0(struct el_callback, 1);

    cb->base.eventloop = &el_integration;
    cb->func = func;
    cb->data = data;
    cb->finalize = finalize;
    cb->refcount = 1;
    return &cb->base;
}

static void callback_ref(GBinderEventLoopCallback *base) {
    struct el_callback *cb = (struct el_callback *) base;

    __atomic_add_fetch(&cb->refcount, 1, __ATOMIC_SEQ_CST);
}

static void callback_unref(GBinderEventLoopCallback *base) {
    struct el_callback *cb = (struct el_callback *) base;

    if (__atomic_sub_fetch(&cb->refcount, 1, __ATOMIC_SEQ_CST) == 0) {
        if (cb->finalize)
            cb->finalize(cb->data);
        pa_xfree(cb);
    }
}

static void callback_schedule(GBinderEventLoopCallback *base) {
    struct el_callback *cb = (struct el_callback *) base;
    uint64_t one = 1;
    bool added = false;

    /* Looper threads may still schedule after teardown; the fd is
     * written under the lock so it cannot be closed and reused meanwhile. */
    pthread_mutex_lock(&el.lock);
    if (!cb->queued && el.efd >= 0) {
        cb->queued = true;
        callback_ref(base);
        g_queue_push_tail(&el.queue, cb);
        added = true;
    }
    if (added && write(el.efd, &one, sizeof(one)) < 0)
        pa_log_warn("aidl-eventloop: eventfd write: %s", pa_cstrerror(errno));
    pthread_mutex_unlock(&el.lock);
}

static void callback_cancel(GBinderEventLoopCallback *base) {
    struct el_callback *cb = (struct el_callback *) base;
    bool removed = false;

    pthread_mutex_lock(&el.lock);
    if (cb->queued) {
        g_queue_remove(&el.queue, cb);
        cb->queued = false;
        removed = true;
    }
    pthread_mutex_unlock(&el.lock);

    if (removed)
        callback_unref(base);
}

/* Drops queued callbacks without running them, like callback_cancel(). */
static void callback_drop_queued(void) {
    for (;;) {
        struct el_callback *cb;

        pthread_mutex_lock(&el.lock);
        cb = g_queue_pop_head(&el.queue);
        if (cb)
            cb->queued = false;
        pthread_mutex_unlock(&el.lock);

        if (!cb)
            break;
        callback_unref(&cb->base);
    }
}

static void teardown(void) {
    if (el.io)
        el.core->mainloop->io_free(el.io);
    el.io = NULL;

    pthread_mutex_lock(&el.lock);
    if (el.efd >= 0)
        close(el.efd);
    el.efd = -1;
    pthread_mutex_unlock(&el.lock);

    callback_drop_queued();
    el.core = NULL;
}

/* Called by libgbinder when the integration is replaced, which also
 * happens from its destructor at process exit, after the PulseAudio
 * main loop is gone. By then droid_aidl_eventloop_done() has already
 * released everything that needs the main loop. */
static void cleanup(void) {
    if (el.core)
        teardown();
}

static const GBinderEventLoopIntegration el_integration = {
    .timeout_add = timeout_add,
    .timeout_remove = timeout_remove,
    .callback_new = callback_new,
    .callback_ref = callback_ref,
    .callback_unref = callback_unref,
    .callback_schedule = callback_schedule,
    .callback_cancel = callback_cancel,
    .cleanup = cleanup,
};

bool droid_aidl_eventloop_init(pa_core *core) {
    pa_assert(core);

    if (el.core) {
        pa_assert(el.core == core);
        el.users++;
        return true;
    }

    el.efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (el.efd < 0) {
        pa_log("aidl-eventloop: eventfd: %s", pa_cstrerror(errno));
        return false;
    }

    el.core = core;
    el.users = 1;
    el.io = core->mainloop->io_new(core->mainloop, el.efd, PA_IO_EVENT_INPUT, io_cb, NULL);

    gbinder_eventloop_set(&el_integration);
    return true;
}

void droid_aidl_eventloop_done(void) {
    pa_assert(el.users > 0);

    if (--el.users > 0)
        return;

    teardown();
    gbinder_eventloop_set(NULL);
}
