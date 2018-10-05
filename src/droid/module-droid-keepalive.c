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

#include <signal.h>
#include <stdio.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/idxset.h>

#include "keepalive.h"

#include "module-droid-keepalive-symdef.h"

PA_MODULE_AUTHOR("Juho Hämäläinen");
PA_MODULE_DESCRIPTION("Droid keepalive. Send cpu wakeup heartbeat while streams are active.");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE(
        "-"
);

static const char* const valid_modargs[] = {
    NULL,
};

struct userdata {
    pa_core *core;
    pa_module *module;

    pa_droid_keepalive *keepalive;
    bool active;
    pa_hook_slot *sink_state_changed_slot;
    pa_hook_slot *source_state_changed_slot;
};

static void start(struct userdata *u) {
    if (u->active)
        return;

    u->active = true;

    pa_droid_keepalive_start(u->keepalive);
}

static void stop(struct userdata *u) {
    void *state = NULL;
    pa_sink *sink;
    pa_source *source;

    if (!u->active)
        return;

    while ((sink = pa_idxset_iterate(u->core->sinks, &state, NULL))) {
        if (pa_sink_get_state(sink) != PA_SINK_SUSPENDED)
            return;
    }

    state = NULL;
    while ((source = pa_idxset_iterate(u->core->sources, &state, NULL))) {
        if (source->monitor_of)
            continue;
        if (pa_source_get_state(source) != PA_SOURCE_SUSPENDED)
            return;
    }

    /* We get here if all sinks and sources are in suspended state. */
    pa_droid_keepalive_stop(u->keepalive);
    u->active = false;
}

static void update_sink(pa_sink *sink, struct userdata *u) {
    pa_assert(sink);
    pa_assert(u);

    if (pa_sink_get_state(sink) != PA_SINK_SUSPENDED)
        start(u);
    else
        stop(u);
}

static void update_source(pa_source *source, struct userdata *u) {
    pa_assert(source);
    pa_assert(u);

    /* Don't react on monitor state changes. */
    if (!source->monitor_of) {
        if (pa_source_get_state(source) != PA_SOURCE_SUSPENDED)
            start(u);
        else
            stop(u);
    }
}

static pa_hook_result_t device_state_changed_hook_cb(pa_core *c, pa_object *o, struct userdata *u) {
    pa_assert(c);
    pa_object_assert_ref(o);
    pa_assert(u);

    if (pa_source_isinstance(o))
        update_source(PA_SOURCE(o), u);
    else if (pa_sink_isinstance(o))
        update_sink(PA_SINK(o), u);

    return PA_HOOK_OK;
}


int pa__init(pa_module *m) {
    uint32_t idx = 0;
    pa_sink *sink;
    pa_source *source;
    struct userdata *u;

    pa_assert(m);

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->active = false;
    u->module = m;
    m->userdata = u;

    if (!(u->keepalive = pa_droid_keepalive_new(u->core))) {
        pa_log("Failed to create keepalive handler.");
        goto fail;
    }

    u->sink_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) device_state_changed_hook_cb, u);
    u->source_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) device_state_changed_hook_cb, u);

    PA_IDXSET_FOREACH(source, u->core->sources, idx)
        update_source(source, u);

    PA_IDXSET_FOREACH(sink, u->core->sinks, idx)
        update_sink(sink, u);

    return 0;

fail:
    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if ((u = m->userdata)) {

        if (u->sink_state_changed_slot)
            pa_hook_slot_free(u->sink_state_changed_slot);
        if (u->source_state_changed_slot)
            pa_hook_slot_free(u->source_state_changed_slot);

        if (u->keepalive) {
            pa_droid_keepalive_stop(u->keepalive);
            pa_droid_keepalive_free(u->keepalive);
        }

        pa_xfree(u);
    }
}
