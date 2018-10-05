#ifndef foodroidconfigfoo
#define foodroidconfigfoo

/*
 * Copyright (C) 2018 Jolla Ltd.
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

#include <droid/version.h>

typedef struct pa_droid_config_audio pa_droid_config_audio;
typedef struct pa_droid_config_hw_module pa_droid_config_hw_module;

#define AUDIO_MAX_SAMPLING_RATES (32)

typedef struct pa_droid_config_global {
    uint32_t audio_hal_version;
    audio_devices_t attached_output_devices;
    audio_devices_t default_output_device;
    audio_devices_t attached_input_devices;
} pa_droid_config_global;

typedef struct pa_droid_config_device {
    const pa_droid_config_hw_module *module;

    char *name;
    uint32_t sampling_rates[AUDIO_MAX_SAMPLING_RATES]; /* (uint32_t) -1 -> dynamic */
    audio_channel_mask_t channel_masks; /* 0 -> dynamic */
    audio_format_t formats; /* 0 -> dynamic */
    audio_devices_t devices;
    /* Instead of using audio_output_flags_t and audio_input_flags_t
     * unify the flags as uint32_t so that we can have single struct for both
     * output and input configurations.
     * audio_input_flags_t was introduced in APIs 2 & 3, depending on adaptation,
     * so having input flags as uint32_t is simpler from input implementation
     * point of view as well. */
    uint32_t flags;
    pa_direction_t direction;

    struct pa_droid_config_device *next;
} pa_droid_config_device;

struct pa_droid_config_hw_module {
    const pa_droid_config_audio *config;

    char *name;
    /* If global config is not defined for module, use root global config. */
    pa_droid_config_global *global_config;
    pa_droid_config_device *outputs;
    pa_droid_config_device *inputs;

    struct pa_droid_config_hw_module *next;
};

struct pa_droid_config_audio {
    pa_droid_config_global *global_config;
    pa_droid_config_hw_module *hw_modules;
};

/* Config parser */
pa_droid_config_audio *pa_droid_config_load(pa_modargs *ma);
pa_droid_config_audio *pa_droid_config_dup(const pa_droid_config_audio *config);
void pa_droid_config_free(pa_droid_config_audio *config);
pa_droid_config_audio *pa_parse_droid_audio_config_legacy(const char *filename);
pa_droid_config_audio *pa_parse_droid_audio_config_xml(const char *filename);
/* autodetect config type from filename and parse */
pa_droid_config_audio *pa_parse_droid_audio_config(const char *filename);

const pa_droid_config_hw_module *pa_droid_config_find_module(const pa_droid_config_audio *config, const char* module_id);

pa_droid_config_hw_module *pa_droid_config_hw_module_new(const pa_droid_config_audio *config, const char *name);
void pa_droid_config_hw_module_free(pa_droid_config_hw_module *hw_module);
pa_droid_config_device *pa_droid_config_device_new(const pa_droid_config_hw_module *module,
                                                   pa_direction_t direction,
                                                   const char *name);
void pa_droid_config_device_free(pa_droid_config_device *device);

#endif
