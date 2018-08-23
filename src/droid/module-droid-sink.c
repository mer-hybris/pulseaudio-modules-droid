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

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include <droid/droid-util.h>
#include <droid/conversion.h>
#include "droid-sink.h"

#include "module-droid-sink-symdef.h"

PA_MODULE_AUTHOR("Juho Hämäläinen");
PA_MODULE_DESCRIPTION("Droid sink");
PA_MODULE_USAGE("master_sink=<sink to connect to> "
                "sink_name=<name of created sink>");
PA_MODULE_VERSION(PACKAGE_VERSION);

static const char* const valid_modargs[] = {
    "rate",
    "format",
    "channels",
    "channel_map",
    "sink_rate",
    "sink_format",
    "sink_channel_map",
    "sink_mix_route",
    "flags",
    "output_devices",
    "sink_name",
    "module_id",
    "mute_routing_before",
    "mute_routing_after",
    "prewrite_on_resume",
    "sink_buffer",
    "deferred_volume",
    "voice_property_key",
    "voice_property_value",
    NULL,
};

void pa__done(pa_module *m) {
    pa_sink *sink;

    pa_assert(m);

    if ((sink = m->userdata))
        pa_droid_sink_free(sink);
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    const char *flags_str;
    audio_output_flags_t flags = 0;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if ((flags_str = pa_modargs_get_value(ma, "flags", NULL))) {
        if (!pa_string_convert_flag_str_to_num(flags_str, &flags)) {
            pa_log("Failed to parse flags");
            goto fail;
        }
    }

    if (!(m->userdata = pa_droid_sink_new(m, ma, __FILE__, NULL, flags, NULL, NULL)))
        goto fail;

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}
