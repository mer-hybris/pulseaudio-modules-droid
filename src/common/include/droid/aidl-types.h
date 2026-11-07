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
 * Plain C view of the AIDL audio HAL objects (ports, routes, port
 * configs, stream parameters) shared between the binder client and the
 * legacy HAL emulation.
 */

#ifndef DROID_AIDL_TYPES_H
#define DROID_AIDL_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* PCM sample format of a profile or port config. */
#define AIDL_AUDIO_FORMAT_S16LE     0
#define AIDL_AUDIO_FORMAT_S32LE     1
#define AIDL_AUDIO_FORMAT_FLOAT32LE 2
#define AIDL_AUDIO_FORMAT_S24LE     3
#define AIDL_AUDIO_FORMAT_U8        4

#define AIDL_AUDIO_PORT_TYPE_DEVICE  0
#define AIDL_AUDIO_PORT_TYPE_MIX     1

#define AIDL_AUDIO_DIR_OUTPUT  0
#define AIDL_AUDIO_DIR_INPUT   1

#define AIDL_AUDIO_MAX_PROFILES_PER_PORT     16
#define AIDL_AUDIO_MAX_RATES_PER_PROFILE     16
#define AIDL_AUDIO_MAX_CHANNELS_PER_PROFILE  16
#define AIDL_AUDIO_MAX_SOURCES_PER_ROUTE     32

/* AudioPortDeviceExt.flags bit FLAG_INDEX_DEFAULT_DEVICE. */
#define AIDL_AUDIO_PORT_DEVICE_FLAG_DEFAULT  (1 << 0)

struct aidl_audio_profile {
    int format;                /* AIDL_AUDIO_FORMAT_*, -1 for non-PCM */
    int sample_rates_count;
    int sample_rates[AIDL_AUDIO_MAX_RATES_PER_PROFILE];
    int channel_masks_count;
    /* AudioChannelLayout layoutMask / indexMask values. */
    int channel_masks[AIDL_AUDIO_MAX_CHANNELS_PER_PROFILE];
};

struct aidl_audio_port {
    int id;                    /* AIDL port id */
    char name[64];
    int direction;             /* AIDL_AUDIO_DIR_* */
    int port_type;             /* AIDL_AUDIO_PORT_TYPE_* */

    /* Device ports. */
    int aidl_device_type;      /* AudioDeviceType */
    int legacy_device_type;    /* audio_devices_t */
    int device_flags;          /* AudioPortDeviceExt.flags */
    char device_connection[64];/* AudioDeviceDescription.connection, empty when attached */
    char device_address[64];

    /* Mix ports. */
    int io_flags;              /* audio_output_flags_t / audio_input_flags_t bitfield */
    int aidl_io_flags;         /* AudioIoFlags value */
    int max_open_count;
    int max_active_count;

    int profiles_count;
    struct aidl_audio_profile profiles[AIDL_AUDIO_MAX_PROFILES_PER_PORT];
};

struct aidl_audio_route {
    int sink_port_id;
    int sources_count;
    int source_port_ids[AIDL_AUDIO_MAX_SOURCES_PER_ROUTE];
    bool is_exclusive;
};

/* Stream parameters known after open. */
struct aidl_stream_info {
    int sample_rate;
    int channels;
    int format;                /* AIDL_AUDIO_FORMAT_* */
    int buffer_frames;
    int frame_bytes;
};

struct aidl_stream_params {
    int mix_port_config_id;
    int audio_usage;           /* output: AudioUsage, 0 = UNKNOWN */
    int audio_content_type;    /* output: AudioContentType */
    int audio_source;          /* input: AudioSource */
    int requested_buffer_frames; /* 0 = let the HAL pick */
};

/* AudioPortConfig. Unset fields use the sentinels given below. */
struct aidl_port_config {
    int config_id;             /* In: 0 = new. Out: HAL-assigned id. */
    int port_id;
    int sample_rate;           /* 0 = unset */
    int channel_mask;          /* 0 = unset */
    int format;                /* -1 = unset */
    int io_flags;              /* -1 = unset, 0 = use the port's own flags */
};

#endif /* DROID_AIDL_TYPES_H */
