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

#include "version.h"
#include "droid-config.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/core-error.h>
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
#include <pulsecore/refcnt.h>
#include <pulsecore/shared.h>
#include <pulsecore/mutex.h>
#include <pulsecore/strlist.h>
#include <pulsecore/atomic.h>

#include <hardware/audio.h>
#include <hardware_legacy/audio_policy_conf.h>

#define VENDOR_AUDIO_POLICY_CONFIG_XML_FILE "/vendor/etc/audio_policy_configuration.xml"
#define SYSTEM_AUDIO_POLICY_CONFIG_XML_FILE "/system/etc/audio_policy_configuration.xml"


pa_droid_config_audio *pa_droid_config_load(pa_modargs *ma) {
    pa_droid_config_audio *config = NULL;
    const char *manual_config;
    const char *config_location[] = {
        VENDOR_AUDIO_POLICY_CONFIG_XML_FILE,
        AUDIO_POLICY_VENDOR_CONFIG_FILE,
        SYSTEM_AUDIO_POLICY_CONFIG_XML_FILE,
        AUDIO_POLICY_CONFIG_FILE,
        NULL};

    pa_assert(ma);

    if ((manual_config = pa_modargs_get_value(ma, "config", NULL))) {
        if (!(config = pa_parse_droid_audio_config(manual_config)))
            pa_log("Failed to parse configuration from %s", manual_config);
    } else {
        int i;
        for (i = 0; config_location[i]; i++) {
            if ((config = pa_parse_droid_audio_config(config_location[i])))
                break;
            else
                pa_log_debug("Failed to parse configuration from %s", config_location[i]);
        }

    }

    if (!config)
        pa_log("Failed to parse any configuration.");

    return config;
}

pa_droid_config_audio *pa_parse_droid_audio_config(const char *filename) {
    const char *suffix;

    pa_assert(filename);

    if ((suffix = rindex(filename, '.'))) {
        if (strlen(suffix) == 4 && pa_streq(suffix, ".xml"))
            return pa_parse_droid_audio_config_xml(filename);
        else if (strlen(suffix) == 5 && pa_streq(suffix, ".conf"))
            return pa_parse_droid_audio_config_legacy(filename);
    }

    return NULL;
}

void pa_droid_config_free(pa_droid_config_audio *config) {
    pa_droid_config_hw_module *module;
    pa_droid_config_device *device;

    if (!config)
        return;

    while (config->hw_modules) {
        SLLIST_STEAL_FIRST(module, config->hw_modules);

        while (module->outputs) {
            SLLIST_STEAL_FIRST(device, module->outputs);
            pa_droid_config_device_free(device);
        }

        while (module->inputs) {
            SLLIST_STEAL_FIRST(device, module->inputs);
            pa_droid_config_device_free(device);
        }

        pa_droid_config_hw_module_free(module);
    }

    pa_xfree(config->global_config);
    pa_xfree(config);
}

const pa_droid_config_hw_module *pa_droid_config_find_module(const pa_droid_config_audio *config, const char* module_id) {
    pa_droid_config_hw_module *module;

    pa_assert(config);
    pa_assert(module_id);

    SLLIST_FOREACH(module, config->hw_modules) {
        if (pa_streq(module_id, module->name))
            return module;
    }

    return NULL;
}

pa_droid_config_hw_module *pa_droid_config_hw_module_new(const pa_droid_config_audio *config, const char *name) {
    pa_droid_config_hw_module *hw_module;

    pa_assert(config);
    pa_assert(name);

    hw_module = pa_xnew0(pa_droid_config_hw_module, 1);
    hw_module->config = config;
    hw_module->name = pa_xstrndup(name, AUDIO_HARDWARE_MODULE_ID_MAX_LEN);

    return hw_module;
}

void pa_droid_config_hw_module_free(pa_droid_config_hw_module *hw_module) {
    if (!hw_module)
        return;

    pa_xfree(hw_module->name);
    pa_xfree(hw_module->global_config);
    pa_xfree(hw_module);
}

pa_droid_config_device *pa_droid_config_device_new(const pa_droid_config_hw_module *module,
                                                   pa_direction_t direction,
                                                   const char *name) {
    pa_droid_config_device *device;

    pa_assert(module);
    pa_assert(direction == PA_DIRECTION_OUTPUT || direction == PA_DIRECTION_INPUT);
    pa_assert(name);

    device = pa_xnew0(pa_droid_config_device, 1);
    device->module = module;
    device->direction = direction;
    device->name = pa_replace(name, " ", "_");

    return device;
}

void pa_droid_config_device_free(pa_droid_config_device *device) {
    if (!device)
        return;

    pa_xfree(device->name);
    pa_xfree(device);
}
