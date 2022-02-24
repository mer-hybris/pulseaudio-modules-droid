#ifndef foodroidconfigfoo
#define foodroidconfigfoo

/*
 * Copyright (C) 2018-2022 Jolla Ltd.
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
#include <pulsecore/modargs.h>

#include <android-config.h>
#include <hardware/audio.h>

#include <droid/sllist.h>
#include <droid/version.h>

#define AUDIO_MAX_SAMPLING_RATES    (32)
#define AUDIO_MAX_CHANNEL_MASKS     (32)

typedef struct dm_config_global dm_config_global;
typedef struct dm_config_port dm_config_port;
typedef struct dm_config_route dm_config_route;
typedef struct dm_config_module dm_config_module;
typedef struct dm_config_device dm_config_device;
typedef struct dm_config_profile dm_config_profile;

struct dm_config_global {
    char *key;
    char *value;
};

struct dm_config_profile {
    char *name;
    audio_format_t format; /* 0 -> dynamic TODO check that this is still true */
    uint32_t sampling_rates[AUDIO_MAX_SAMPLING_RATES]; /* sampling_rates[0] == 0 -> dynamic, otherwise 0 terminates list */
    audio_channel_mask_t channel_masks[AUDIO_MAX_CHANNEL_MASKS]; /* channel_masks[0] == 0 -> dynamic */
};

typedef enum dm_config_role {
    DM_CONFIG_ROLE_SINK,
    DM_CONFIG_ROLE_SOURCE,
} dm_config_role_t;

typedef enum dm_config_type {
    DM_CONFIG_TYPE_MIX,
    DM_CONFIG_TYPE_DEVICE_PORT,
    DM_CONFIG_TYPE_MIX_PORT,
} dm_config_type_t;

struct dm_config_port {
    dm_config_module *module;

    /* common values */

    dm_config_type_t port_type; /* either mixPort or devicePort */
    char *name;
    dm_config_role_t role;
    dm_list *profiles; /* dm_config_profile* */

    /* devicePort specific values */

    audio_devices_t type;
    char *address;

    /* mixPort specific values */

    uint32_t flags; /* audio_output_flag_t or audio_input_flag_t */
    int max_open_count; /* 0 == not defined */
    int max_active_count; /* 0 == not defined */
};

struct dm_config_route {
    dm_config_type_t type;
    dm_config_port *sink;
    dm_list *sources; /* dm_config_port* */
};

struct dm_config_module {
    dm_config_device *config;

    char *name;
    int version_major;
    int version_minor;

    dm_list *attached_devices; /* dm_config_port* owned by device_ports list below */
    dm_config_port *default_output_device; /* owned by device_ports list below */
    dm_list *ports; /* dm_config_port* - for convenience port types are filtered to two lists below: */
    dm_list *mix_ports; /* dm_config_port* */
    dm_list *device_ports; /* dm_config_port* */
    dm_list *routes; /* dm_config_route* */
};

struct dm_config_device {
    dm_list *global_config; /* dm_config_global* */
    dm_list *modules; /* dm_config_module* */
};


/* Config parser */
dm_config_device *dm_config_load(pa_modargs *ma);
dm_config_device *dm_config_dup(const dm_config_device *config);
void dm_config_free(dm_config_device *config);
/* autodetect config type from filename and parse */
dm_config_device *pa_parse_droid_audio_config(const char *filename);

dm_config_module *dm_config_find_module(dm_config_device *config, const char* module_id);
dm_config_port *dm_config_find_port(dm_config_module *module, const char* name);
dm_config_port *dm_config_default_output_device(dm_config_module *module);
dm_config_port *dm_config_find_device_port(dm_config_port *port, audio_devices_t device);
char *dm_config_escape_string(const char *string);

bool dm_config_port_equal(const dm_config_port *a, const dm_config_port *b);

dm_config_port *dm_config_find_mix_port(dm_config_module *module, const char *name);

#endif
