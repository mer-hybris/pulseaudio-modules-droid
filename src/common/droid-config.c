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

#include "droid/version.h"
#include "droid/droid-config.h"
#include "droid/sllist.h"
#include "config-parser-xml.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/modargs.h>

#include <hardware/audio.h>

#define ODM_AUDIO_POLICY_CONFIG_XML_FILE            "/odm/etc/audio_policy_configuration.xml"
#define VENDOR_AUDIO_AUDIO_POLICY_CONFIG_XML_FILE   "/vendor/etc/audio/audio_policy_configuration.xml"
#define VENDOR_AUDIO_POLICY_CONFIG_XML_FILE         "/vendor/etc/audio_policy_configuration.xml"
#define SYSTEM_AUDIO_POLICY_CONFIG_XML_FILE         "/system/etc/audio_policy_configuration.xml"


dm_config_device *dm_config_load(pa_modargs *ma) {
    dm_config_device *config = NULL;
    const char *manual_config;
    const char *config_location[] = {
        ODM_AUDIO_POLICY_CONFIG_XML_FILE,
        VENDOR_AUDIO_AUDIO_POLICY_CONFIG_XML_FILE,
        VENDOR_AUDIO_POLICY_CONFIG_XML_FILE,
        SYSTEM_AUDIO_POLICY_CONFIG_XML_FILE,
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

static dm_config_profile *config_profile_dup(const dm_config_profile *profile) {
    dm_config_profile *copy = pa_xnew0(dm_config_profile, 1);

    copy->name = pa_xstrdup(profile->name);
    copy->format = profile->format;
    memcpy(copy->sampling_rates,
           profile->sampling_rates,
           sizeof(profile->sampling_rates));
    memcpy(copy->channel_masks,
           profile->channel_masks,
           sizeof(profile->channel_masks));

    return copy;
}

static dm_config_port *config_port_dup(const dm_config_port *port, dm_config_module *module) {
    dm_config_port *copy = pa_xnew0(dm_config_port, 1);
    const dm_list_entry *i;

    copy->module = module;
    copy->port_type = port->port_type;
    copy->name = pa_xstrdup(port->name);
    copy->role = port->role;
    copy->profiles = dm_list_new();

    DM_LIST_FOREACH(i, port->profiles)
        dm_list_push_back(copy->profiles, config_profile_dup(i->data));

    if (port->port_type == DM_CONFIG_TYPE_DEVICE_PORT) {
        copy->type = port->type;
        copy->address = pa_xstrdup(port->address);
    }

    if (port->port_type == DM_CONFIG_TYPE_MIX_PORT) {
        copy->flags = port->flags;
        copy->max_open_count = port->max_open_count;
        copy->max_active_count = port->max_active_count;
    }

    return copy;
}

static dm_config_route *config_route_dup(const dm_config_route *route, dm_list *ports) {
    dm_config_route *copy = pa_xnew0(dm_config_route, 1);
    dm_config_port *port_copy, *port;
    void *state, *state2;

    copy->type = route->type;
    copy->sources = dm_list_new();

    DM_LIST_FOREACH_DATA(port, route->sources, state) {
        DM_LIST_FOREACH_DATA(port_copy, ports, state2) {
            if (dm_config_port_equal(port, port_copy)) {
                dm_list_push_back(copy->sources, port_copy);
                break;
            }
        }
    }

    DM_LIST_FOREACH_DATA(port_copy, ports, state) {
        if (dm_config_port_equal(port_copy, route->sink)) {
            copy->sink = port_copy;
            break;
        }
    }

    return copy;
}

static dm_config_module *config_module_dup(const dm_config_module *module) {
    dm_config_module *copy = pa_xnew0(dm_config_module, 1);
    dm_config_port *device_port, *attached_device, *mix_port;
    dm_config_route *route;
    void *state, *state2;

    copy = pa_xnew0(dm_config_module, 1);
    copy->name = pa_xstrdup(module->name);
    copy->version_major = module->version_major;
    copy->version_minor = module->version_minor;
    copy->attached_devices = dm_list_new();
    copy->default_output_device = NULL;
    copy->mix_ports = dm_list_new();
    copy->device_ports = dm_list_new();
    copy->ports = dm_list_new();
    copy->routes = dm_list_new();

    DM_LIST_FOREACH_DATA(device_port, module->device_ports, state) {
        dm_config_port *device_port_copy = config_port_dup(device_port, copy);
        dm_list_push_back(copy->device_ports, device_port_copy);
        dm_list_push_back(copy->ports, device_port_copy);
        if (module->default_output_device == device_port)
            copy->default_output_device = device_port_copy;
        DM_LIST_FOREACH_DATA(attached_device, module->attached_devices, state2) {
            if (attached_device == device_port) {
                dm_list_push_back(copy->attached_devices, device_port_copy);
                break;
            }
        }
    }

    DM_LIST_FOREACH_DATA(mix_port, module->mix_ports, state) {
        dm_config_port *mix_port_copy = config_port_dup(mix_port, copy);
        dm_list_push_back(copy->mix_ports, mix_port_copy);
        dm_list_push_back(copy->ports, mix_port_copy);
    }

    DM_LIST_FOREACH_DATA(route, module->routes, state)
        dm_list_push_back(copy->routes, config_route_dup(route, copy->ports));

    return copy;
}

dm_config_device *dm_config_dup(const dm_config_device *config) {
    dm_config_device *copy;
    dm_config_module *module;
    void *state;

    pa_assert(config);

    copy = pa_xnew0(dm_config_device, 1);
    copy->global_config = dm_list_new();
    copy->modules = dm_list_new();

    if (config->global_config) {
        dm_config_global *global, *global_copy;

        DM_LIST_FOREACH_DATA(global, config->global_config, state) {
            global_copy = pa_xnew0(dm_config_global, 1);
            global_copy->key = pa_xstrdup(global->key);
            global_copy->value = pa_xstrdup(global->value);
            dm_list_push_back(copy->global_config, global_copy);
        }
    }

    DM_LIST_FOREACH_DATA(module, config->modules, state)
        dm_list_push_back(copy->modules, config_module_dup(module));

    return copy;
}

dm_config_device *pa_parse_droid_audio_config(const char *filename) {
    return pa_parse_droid_audio_config_xml(filename);
}

static void config_global_free(void *data) {
    dm_config_global *global = data;

    pa_xfree(global->key);
    pa_xfree(global->value);
    pa_xfree(global);
}

static void config_profile_free(void *data) {
    dm_config_profile *profile = data;

    pa_xfree(profile->name);
    pa_xfree(profile);
}

static void config_port_free(void *data) {
    dm_config_port *port = data;

    pa_xfree(port->name);
    pa_xfree(port->address);
    dm_list_free(port->profiles, config_profile_free);
    pa_xfree(port);
}

static void config_route_free(void *data) {
    dm_config_route *route = data;

    dm_list_free(route->sources, NULL);
    pa_xfree(route);
}

static void config_module_free(void *data) {
    dm_config_module *module = data;

    pa_xfree(module->name);
    dm_list_free(module->attached_devices, NULL);
    dm_list_free(module->ports, config_port_free);
    dm_list_free(module->device_ports, NULL);
    dm_list_free(module->mix_ports, NULL);
    dm_list_free(module->routes, config_route_free);
    pa_xfree(module);
}

void dm_config_free(dm_config_device *config) {
    if (!config)
        return;

    dm_list_free(config->global_config, config_global_free);
    dm_list_free(config->modules, config_module_free);
    pa_xfree(config);
}

dm_config_module *dm_config_find_module(dm_config_device *config, const char* module_id) {
    dm_config_module *module;
    void *state;

    pa_assert(config);
    pa_assert(module_id);

    DM_LIST_FOREACH_DATA(module, config->modules, state) {
        if (pa_streq(module_id, module->name))
            return module;
    }

    return NULL;
}

dm_config_port *dm_config_find_port(dm_config_module *module, const char* name) {
    dm_config_port *port;
    void *state;

    pa_assert(module);
    pa_assert(name);

    DM_LIST_FOREACH_DATA(port, module->ports, state) {
        if (pa_streq(name, port->name))
            return port;
    }

    return NULL;
}

dm_config_port *dm_config_default_output_device(dm_config_module *module) {
    pa_assert(module);

    if (module->default_output_device)
        return module->default_output_device;
    else {
        pa_log("Module %s doesn't have default output device.", module->name);
        return 0;
    }
}

char *dm_config_escape_string(const char *string) {
    if (!string)
        return NULL;

    /* Just replace whitespace with underscores for now. */

    return pa_replace(string, " ", "_");
}

dm_config_port *dm_config_find_device_port(dm_config_port *port, audio_devices_t device) {
    dm_config_port *device_port;
    void *state;

    pa_assert(port);

    DM_LIST_FOREACH_DATA(device_port, port->module->device_ports, state) {
        if (device_port->type == device)
            return device_port;
    }

    return NULL;
}

bool dm_config_port_equal(const dm_config_port *a, const dm_config_port *b) {
    if ((!a && b) || (a && !b))
        return false;

    else if (!a && !b)
        return true;

    return (pa_streq(a->name, b->name) && a->type == b->type);
}

dm_config_port *dm_config_find_mix_port(dm_config_module *module, const char *name) {
    dm_config_port *mix_port = NULL;
    void *state;

    DM_LIST_FOREACH_DATA(mix_port, module->mix_ports, state) {
        if (pa_streq(mix_port->name, name))
            return mix_port;
    }

    return NULL;
}
