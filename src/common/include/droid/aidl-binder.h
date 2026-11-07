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
 * Client for the android.hardware.audio.core AIDL HAL (IModule,
 * ITelephony, IStreamOut, IStreamIn) built on libgbinder. Stream data
 * goes through the HAL's fast message queues, see aidl-fmq.h.
 *
 * All functions return 0 or a negative errno unless stated otherwise.
 */

#ifndef DROID_AIDL_BINDER_H
#define DROID_AIDL_BINDER_H

#include <stdbool.h>
#include <stdint.h>

#include <pulsecore/core.h>

#include "aidl-types.h"

typedef struct aidl_audio_module aidl_audio_module;
typedef struct aidl_audio_stream aidl_audio_stream;

/* Connects to IModule/default, waiting for it to register if needed. */
aidl_audio_module *aidl_audio_module_connect(pa_core *core);
/* Streams must be closed before this. */
void aidl_audio_module_disconnect(aidl_audio_module *m);

/* Arrays are allocated with pa_xmalloc and owned by the caller. */
int aidl_audio_module_get_ports(aidl_audio_module *m, struct aidl_audio_port **ports, int *count);
int aidl_audio_module_get_routes(aidl_audio_module *m, struct aidl_audio_route **routes, int *count);

/* mode is an audio_mode_t value. */
int aidl_audio_module_set_mode(aidl_audio_module *m, int mode);

int aidl_audio_module_set_master_volume(aidl_audio_module *m, float volume);
int aidl_audio_module_get_master_volume(aidl_audio_module *m, float *volume);
int aidl_audio_module_set_master_mute(aidl_audio_module *m, bool muted);
int aidl_audio_module_get_master_mute(aidl_audio_module *m, bool *muted);
int aidl_audio_module_set_mic_mute(aidl_audio_module *m, bool muted);
int aidl_audio_module_get_mic_mute(aidl_audio_module *m, bool *muted);
int aidl_audio_module_set_voice_volume(aidl_audio_module *m, float volume);

/* "key=value;key=value" in, each pair sent as one VendorParameter id. */
int aidl_audio_module_set_vendor_parameters(aidl_audio_module *m, const char *kv_pairs);
/* "key;key" in, "id;id" out (pa_xmalloc'd) or NULL. */
char *aidl_audio_module_get_vendor_parameters(aidl_audio_module *m, const char *keys);

/* IModule.setAudioPortConfig, resubmitting the HAL's suggestion once when
 * it was not applied straight away. */
int aidl_audio_module_set_port_config(aidl_audio_module *m,
                                      const struct aidl_port_config *config,
                                      struct aidl_port_config *suggested,
                                      int *applied);
int aidl_audio_module_reset_port_config(aidl_audio_module *m, int config_id);

int aidl_audio_module_create_patch(aidl_audio_module *m,
                                   const int *source_port_config_ids, int n_sources,
                                   const int *sink_port_config_ids, int n_sinks,
                                   int *patch_id);
int aidl_audio_module_release_patch(aidl_audio_module *m, int patch_id);

aidl_audio_stream *aidl_audio_open_output_stream(aidl_audio_module *m,
                                                 const struct aidl_stream_params *params,
                                                 struct aidl_stream_info *info);
aidl_audio_stream *aidl_audio_open_input_stream(aidl_audio_module *m,
                                                const struct aidl_stream_params *params,
                                                struct aidl_stream_info *info);
int aidl_audio_stream_close(aidl_audio_stream *s);

/* Return bytes transferred, which may be fewer than asked for. */
int aidl_audio_stream_write(aidl_audio_stream *s, const void *buffer, int bytes);
int aidl_audio_stream_read(aidl_audio_stream *s, void *buffer, int bytes);

int aidl_audio_stream_start(aidl_audio_stream *s);
int aidl_audio_stream_standby(aidl_audio_stream *s);
int aidl_audio_stream_get_latency_ms(aidl_audio_stream *s);
int aidl_audio_stream_get_render_position(aidl_audio_stream *s, uint32_t *frames);
int aidl_audio_stream_set_hw_volume(aidl_audio_stream *s, float left, float right);

#endif /* DROID_AIDL_BINDER_H */
