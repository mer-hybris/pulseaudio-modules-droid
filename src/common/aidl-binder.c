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
 * Parcel layout follows the AIDL NDK backend: every parcelable and union
 * field is preceded by an int32 non-null marker, parcelables also carry
 * their byte size so unknown trailing fields can be skipped, unions are
 * an int32 tag followed by the value, byte-backed enums travel as int32.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <gbinder.h>

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include <hardware/audio.h>

#include "droid/aidl-binder.h"
#include "droid/aidl-eventloop.h"
#include "droid/aidl-fmq.h"

#define LOG_PFX "aidl: "

/* PULSE_AIDL_DEBUG=1 hexdumps port config requests and every reply. */
static bool debug_enabled(void) {
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("PULSE_AIDL_DEBUG") != NULL;
    return enabled;
}

static void debug_dump(const char *what, const void *data, gsize size) {
    const guint8 *d = data;
    GString *hex;

    if (!debug_enabled() || !d)
        return;
    hex = g_string_new(NULL);
    for (gsize i = 0; i < size; i++)
        g_string_append_printf(hex, "%02x%s", d[i], (i % 4 == 3) ? " " : "");
    pa_log(LOG_PFX "%s (%zu bytes): %s", what, size, hex->str);
    g_string_free(hex, TRUE);
}

#define IMODULE_INSTANCE     "android.hardware.audio.core.IModule/default"
#define IMODULE_IFACE        "android.hardware.audio.core.IModule"
#define ITELEPHONY_IFACE     "android.hardware.audio.core.ITelephony"
#define ISTREAM_OUT_IFACE    "android.hardware.audio.core.IStreamOut"
#define ISTREAM_IN_IFACE     "android.hardware.audio.core.IStreamIn"
#define ISTREAM_COMMON_IFACE "android.hardware.audio.core.IStreamCommon"

#define SERVICE_WAIT_MS          20000
#define SERVICE_POLL_MS          250
#define STREAM_REPLY_TIMEOUT_MS  5000
#define DEFAULT_BUFFER_FRAMES    480

/* Transaction codes are FIRST_CALL_TRANSACTION (1) plus the method's index
 * in the interface. Interface versions only append methods. */
enum {
    IMODULE_GET_TELEPHONY = 2,
    IMODULE_GET_AUDIO_PORTS = 11,
    IMODULE_GET_AUDIO_ROUTES = 12,
    IMODULE_OPEN_INPUT_STREAM = 14,
    IMODULE_OPEN_OUTPUT_STREAM = 15,
    IMODULE_SET_AUDIO_PATCH = 17,
    IMODULE_SET_AUDIO_PORT_CONFIG = 18,
    IMODULE_RESET_AUDIO_PATCH = 19,
    IMODULE_RESET_AUDIO_PORT_CONFIG = 20,
    IMODULE_GET_MASTER_MUTE = 21,
    IMODULE_SET_MASTER_MUTE = 22,
    IMODULE_GET_MASTER_VOLUME = 23,
    IMODULE_SET_MASTER_VOLUME = 24,
    IMODULE_GET_MIC_MUTE = 25,
    IMODULE_SET_MIC_MUTE = 26,
    IMODULE_UPDATE_AUDIO_MODE = 28,
    IMODULE_GET_VENDOR_PARAMETERS = 33,
    IMODULE_SET_VENDOR_PARAMETERS = 34,
};

enum {
    ITELEPHONY_SWITCH_AUDIO_MODE = 2,
    ITELEPHONY_SET_TELECOM_CONFIG = 3,
};

enum {
    ISTREAM_OUT_GET_STREAM_COMMON = 1,
    ISTREAM_OUT_SET_HW_VOLUME = 5,
};

enum {
    ISTREAM_IN_GET_STREAM_COMMON = 1,
};

enum {
    ISTREAM_COMMON_CLOSE = 1,
    ISTREAM_COMMON_PREPARE_TO_CLOSE = 2,
};

/* binder::Status exception codes. */
enum {
    EX_NONE = 0,
    EX_ILLEGAL_ARGUMENT = -3,
    EX_ILLEGAL_STATE = -5,
    EX_UNSUPPORTED_OPERATION = -7,
    EX_SERVICE_SPECIFIC = -8,
    EX_HAS_NOTED_APPOPS_REPLY_HEADER = -127,
    EX_HAS_REPLY_HEADER = -128,
};

/* android.media.audio.common enums and union tags. */
enum { FORMAT_TYPE_PCM = 1 };

enum {
    PCM_UINT_8_BIT = 0,
    PCM_INT_16_BIT = 1,
    PCM_INT_32_BIT = 2,
    PCM_FIXED_Q_8_24 = 3,
    PCM_FLOAT_32_BIT = 4,
    PCM_INT_24_BIT = 5,
};

enum {
    CHANNEL_LAYOUT_NONE = 0,
    CHANNEL_LAYOUT_INVALID = 1,
    CHANNEL_LAYOUT_INDEX_MASK = 2,
    CHANNEL_LAYOUT_LAYOUT_MASK = 3,
    CHANNEL_LAYOUT_VOICE_MASK = 4,
};

enum { IO_FLAGS_INPUT = 0, IO_FLAGS_OUTPUT = 1 };

enum {
    PORT_EXT_UNSPECIFIED = 0,
    PORT_EXT_DEVICE = 1,
    PORT_EXT_MIX = 2,
    PORT_EXT_SESSION = 3,
};

enum {
    DEVICE_ADDRESS_ID = 0,
    DEVICE_ADDRESS_MAC = 1,
    DEVICE_ADDRESS_IPV4 = 2,
    DEVICE_ADDRESS_IPV6 = 3,
    DEVICE_ADDRESS_ALSA = 4,
};

enum { AUDIO_BUFFER_FMQ = 0 };
enum { TTY_MODE_UNSPECIFIED = -1 };
enum { AUDIO_MODE_LAST = 4 /* CALL_SCREEN */ };

/* AudioDeviceType */
enum {
    ADT_IN_DEFAULT = 1,
    ADT_IN_ACCESSORY = 2,
    ADT_IN_AFE_PROXY = 3,
    ADT_IN_DEVICE = 4,
    ADT_IN_ECHO_REFERENCE = 5,
    ADT_IN_FM_TUNER = 6,
    ADT_IN_HEADSET = 7,
    ADT_IN_LOOPBACK = 8,
    ADT_IN_MICROPHONE = 9,
    ADT_IN_MICROPHONE_BACK = 10,
    ADT_IN_SUBMIX = 11,
    ADT_IN_TELEPHONY_RX = 12,
    ADT_IN_TV_TUNER = 13,
    ADT_IN_DOCK = 14,
    ADT_OUT_DEFAULT = 129,
    ADT_OUT_ACCESSORY = 130,
    ADT_OUT_AFE_PROXY = 131,
    ADT_OUT_CARKIT = 132,
    ADT_OUT_DEVICE = 133,
    ADT_OUT_ECHO_CANCELLER = 134,
    ADT_OUT_FM = 135,
    ADT_OUT_HEADPHONE = 136,
    ADT_OUT_HEADSET = 137,
    ADT_OUT_HEARING_AID = 138,
    ADT_OUT_LINE_AUX = 139,
    ADT_OUT_SPEAKER = 140,
    ADT_OUT_SPEAKER_EARPIECE = 141,
    ADT_OUT_SPEAKER_SAFE = 142,
    ADT_OUT_SUBMIX = 143,
    ADT_OUT_TELEPHONY_TX = 144,
    ADT_OUT_DOCK = 145,
    ADT_OUT_BROADCAST = 146,
    ADT_OUT_MULTICHANNEL_GROUP = 147,
};

/* StreamDescriptor.State */
enum {
    STATE_STANDBY = 1,
    STATE_IDLE = 2,
    STATE_ACTIVE = 3,
    STATE_PAUSED = 4,
    STATE_ERROR = 100,
};

/* StreamDescriptor.Command tags */
enum {
    CMD_START = 2,
    CMD_BURST = 3,
    CMD_STANDBY = 5,
    CMD_PAUSE = 6,
    CMD_FLUSH = 7,
};

/* Fixed-size FMQ elements, same layout as the generated C++ types. */
struct stream_command {
    int32_t tag;
    int32_t value;
};

struct stream_position {
    int64_t frames;
    int64_t time_ns;
};

struct stream_reply {
    int32_t status;
    int32_t fmq_byte_count;
    struct stream_position observable;
    struct stream_position hardware;
    int32_t latency_ms;
    int32_t xrun_frames;
    int32_t state;
};

_Static_assert(sizeof(struct stream_command) == 8, "Command layout");
_Static_assert(sizeof(struct stream_reply) == 56, "Reply layout");

struct port_info {
    int32_t id;
    int port_type;
    int io_flags_tag;
    int32_t io_flags;
    GBytes *device_ext;        /* AudioPortDeviceExt parcelable payload */
};

struct aidl_audio_module {
    pa_core *core;
    GBinderServiceManager *sm;
    GBinderRemoteObject *module_obj;
    GBinderClient *module;
    GBinderRemoteObject *telephony_obj;
    GBinderClient *telephony;
    gulong death_id;
    bool dead;
    pthread_mutex_t lock;      /* serialises HAL calls, guards ports */
    GHashTable *ports;         /* port id -> struct port_info */
};

struct aidl_audio_stream {
    aidl_audio_module *module;
    int direction;
    GBinderRemoteObject *stream_obj;
    GBinderClient *stream;
    GBinderRemoteObject *common_obj;
    GBinderClient *common;
    aidl_fmq *command;
    aidl_fmq *reply;
    aidl_fmq *data;
    int32_t state;
    bool failed;
    int32_t frame_size_bytes;
    int64_t buffer_size_frames;
    int32_t latency_ms;
    int64_t observable_frames;
    pthread_mutex_t lock;
};

/* ------------------------------------------------------------------------ *
 * Parcel helpers
 * ------------------------------------------------------------------------ */

static GBinderLocalRequest *request_new(GBinderClient *client, guint32 code, GBinderWriter *w) {
    GBinderLocalRequest *req = gbinder_client_new_request2(client, code);

    if (w)
        gbinder_local_request_init_writer(req, w);
    return req;
}

static void debug_dump_writer(const char *what, GBinderWriter *w) {
    gsize size = 0;
    const void *d;

    if (!debug_enabled())
        return;
    d = gbinder_writer_get_data(w, &size);
    debug_dump(what, d, size);
}

static void write_string(GBinderWriter *w, const char *s) {
    gbinder_writer_append_string16(w, s ? s : "");
}

static void write_null(GBinderWriter *w) {
    gbinder_writer_append_int32(w, 0);
}

static void write_union_header(GBinderWriter *w, int32_t tag) {
    gbinder_writer_append_int32(w, 1);
    gbinder_writer_append_int32(w, tag);
}

static void write_int32_array(GBinderWriter *w, const int *values, int n) {
    gbinder_writer_append_int32(w, n);
    for (int i = 0; i < n; i++)
        gbinder_writer_append_int32(w, values[i]);
}

static bool read_int32(GBinderReader *r, int32_t *v) {
    gint32 tmp;

    if (!gbinder_reader_read_int32(r, &tmp))
        return false;
    *v = tmp;
    return true;
}

static bool read_string(GBinderReader *r, char *dst, size_t size) {
    char *s = NULL;

    if (!gbinder_reader_read_nullable_string16(r, &s))
        return false;
    pa_strlcpy(dst, s ? s : "", size);
    g_free(s);
    return true;
}

/* Reads the marker and tag of a (possibly nullable) union field. */
static bool read_union_header(GBinderReader *r, int32_t *tag, bool *non_null) {
    int32_t marker;

    if (!read_int32(r, &marker))
        return false;
    if (marker == 0) {
        *non_null = false;
        *tag = -1;
        return true;
    }
    *non_null = true;
    return read_int32(r, tag);
}

/* AudioChannelLayout: mask is 0 unless the tag is layoutMask or indexMask. */
static bool read_channel_layout(GBinderReader *r, int32_t *mask) {
    int32_t tag, value;
    bool non_null;

    *mask = 0;
    if (!read_union_header(r, &tag, &non_null))
        return false;
    if (!non_null)
        return true;
    if (!read_int32(r, &value))
        return false;
    if (tag == CHANNEL_LAYOUT_LAYOUT_MASK || tag == CHANNEL_LAYOUT_INDEX_MASK)
        *mask = value;
    return true;
}

static bool skip_int32_array(GBinderReader *r) {
    int32_t n, v;

    if (!read_int32(r, &n))
        return false;
    for (int32_t i = 0; i < n; i++)
        if (!read_int32(r, &v))
            return false;
    return true;
}

static bool skip_parcelable_array(GBinderReader *r) {
    int32_t n;

    if (!read_int32(r, &n))
        return false;
    for (int32_t i = 0; i < n; i++)
        if (!gbinder_reader_skip_parcelable(r))
            return false;
    return true;
}

static bool skip_byte_array(GBinderReader *r) {
    gsize len;

    return gbinder_reader_read_byte_array(r, &len) != NULL;
}

static int exception_to_errno(int32_t ex) {
    switch (ex) {
        case EX_UNSUPPORTED_OPERATION: return -ENOSYS;
        case EX_ILLEGAL_ARGUMENT:      return -EINVAL;
        case EX_ILLEGAL_STATE:         return -EBUSY;
        default:                       return -EIO;
    }
}

/* binder::Status header at the start of every reply. */
static int read_status(GBinderReader *r, const char *what) {
    int32_t ex = 0, code = 0, header = 0;
    char *msg = NULL;

    if (!read_int32(r, &ex)) {
        pa_log(LOG_PFX "%s: truncated reply", what);
        return -EIO;
    }
    if (ex == EX_NONE)
        return 0;
    if (ex == EX_UNSUPPORTED_OPERATION) {
        pa_log_debug(LOG_PFX "%s: not supported by the HAL", what);
        return -ENOSYS;
    }
    if (ex == EX_HAS_REPLY_HEADER || ex == EX_HAS_NOTED_APPOPS_REPLY_HEADER) {
        pa_log(LOG_PFX "%s: unexpected reply header", what);
        return -EIO;
    }

    gbinder_reader_read_nullable_string16(r, &msg);
    /* Remote stack trace header, then the service specific code. */
    if (ex == EX_SERVICE_SPECIFIC && read_int32(r, &header) && header == 0)
        read_int32(r, &code);

    pa_log(LOG_PFX "%s: exception %d code %d%s%s", what, ex, code,
           msg ? ": " : "", msg ? msg : "");
    g_free(msg);
    return exception_to_errno(ex);
}

/* Runs a transaction, consumes req, and leaves r after the status header.
 * Returns NULL with *err set on any failure. */
static GBinderRemoteReply *transact(GBinderClient *client, guint32 code, GBinderLocalRequest *req,
                                    GBinderReader *r, const char *what, int *err) {
    GBinderRemoteReply *reply;
    int status = GBINDER_STATUS_OK;

    reply = gbinder_client_transact_sync_reply(client, code, req, &status);
    if (req)
        gbinder_local_request_unref(req);

    if (!reply) {
        /* libgbinder refuses to talk to a dead object and leaves status 0. */
        if (status == GBINDER_STATUS_OK || status == GBINDER_STATUS_DEAD_OBJECT) {
            pa_log(LOG_PFX "%s: audio HAL is gone", what);
            *err = -ENOTCONN;
        } else {
            pa_log(LOG_PFX "%s: transaction failed (%d)", what, status);
            *err = -EIO;
        }
        return NULL;
    }

    gbinder_remote_reply_init_reader(reply, r);
    if (debug_enabled()) {
        gsize size = 0;
        const void *d = gbinder_reader_get_data(r, &size);
        debug_dump(what, d, size);
    }
    if ((*err = read_status(r, what)) < 0) {
        gbinder_remote_reply_unref(reply);
        return NULL;
    }
    return reply;
}

/* For methods whose reply carries nothing but the status. */
static int transact_void(GBinderClient *client, guint32 code, GBinderLocalRequest *req, const char *what) {
    GBinderReader r;
    GBinderRemoteReply *reply;
    int err = 0;

    if ((reply = transact(client, code, req, &r, what, &err)))
        gbinder_remote_reply_unref(reply);
    return err;
}

static int transact_set_bool(GBinderClient *client, guint32 code, bool value, const char *what) {
    GBinderWriter w;
    GBinderLocalRequest *req = request_new(client, code, &w);

    gbinder_writer_append_bool(&w, value);
    return transact_void(client, code, req, what);
}

static int transact_get_bool(GBinderClient *client, guint32 code, bool *value, const char *what) {
    GBinderReader r;
    GBinderRemoteReply *reply;
    gboolean b = FALSE;
    int err = 0;

    if (!(reply = transact(client, code, request_new(client, code, NULL), &r, what, &err)))
        return err;
    if (!gbinder_reader_read_bool(&r, &b))
        err = -EIO;
    gbinder_remote_reply_unref(reply);
    *value = b;
    return err;
}

static int transact_set_float(GBinderClient *client, guint32 code, float value, const char *what) {
    GBinderWriter w;
    GBinderLocalRequest *req = request_new(client, code, &w);

    gbinder_writer_append_float(&w, value);
    return transact_void(client, code, req, what);
}

static int transact_get_float(GBinderClient *client, guint32 code, float *value, const char *what) {
    GBinderReader r;
    GBinderRemoteReply *reply;
    gfloat f = 0;
    int err = 0;

    if (!(reply = transact(client, code, request_new(client, code, NULL), &r, what, &err)))
        return err;
    if (!gbinder_reader_read_float(&r, &f))
        err = -EIO;
    gbinder_remote_reply_unref(reply);
    *value = f;
    return err;
}

static int transact_set_int32(GBinderClient *client, guint32 code, int32_t value, const char *what) {
    GBinderWriter w;
    GBinderLocalRequest *req = request_new(client, code, &w);

    gbinder_writer_append_int32(&w, value);
    return transact_void(client, code, req, what);
}

/* ------------------------------------------------------------------------ *
 * Type conversions
 * ------------------------------------------------------------------------ */

static int pcm_type_to_format(int32_t pcm) {
    switch (pcm) {
        case PCM_INT_16_BIT:   return AIDL_AUDIO_FORMAT_S16LE;
        case PCM_INT_32_BIT:   return AIDL_AUDIO_FORMAT_S32LE;
        case PCM_FLOAT_32_BIT: return AIDL_AUDIO_FORMAT_FLOAT32LE;
        case PCM_INT_24_BIT:   return AIDL_AUDIO_FORMAT_S24LE;
        case PCM_UINT_8_BIT:   return AIDL_AUDIO_FORMAT_U8;
        default:               return -1;
    }
}

static int32_t format_to_pcm_type(int format) {
    switch (format) {
        case AIDL_AUDIO_FORMAT_S16LE:     return PCM_INT_16_BIT;
        case AIDL_AUDIO_FORMAT_S32LE:     return PCM_INT_32_BIT;
        case AIDL_AUDIO_FORMAT_FLOAT32LE: return PCM_FLOAT_32_BIT;
        case AIDL_AUDIO_FORMAT_S24LE:     return PCM_INT_24_BIT;
        case AIDL_AUDIO_FORMAT_U8:        return PCM_UINT_8_BIT;
        default:                          return PCM_INT_16_BIT;
    }
}

/* AIDL folds several legacy devices into one AudioDeviceType and tells
 * them apart by the connection string. The result is what the XML policy
 * path would have produced, so the rest of the modules stay unchanged. */
static int device_type_to_legacy(int32_t type, const char *conn) {
    bool bt_a2dp = pa_streq(conn, "bt-a2dp");
    bool bt_le = pa_streq(conn, "bt-le");
    bool bt_sco = pa_streq(conn, "bt-sco");
    bool usb = pa_streq(conn, "usb");
    bool analog = pa_streq(conn, "analog");
    bool hdmi = pa_streq(conn, "hdmi");
    bool hdmi_arc = pa_streq(conn, "hdmi-arc") || pa_streq(conn, "hdmi-earc");
    bool spdif = pa_streq(conn, "spdif");
    bool ip = pa_streq(conn, "ip-v4");

    switch (type) {
        case ADT_OUT_SPEAKER_EARPIECE: return AUDIO_DEVICE_OUT_EARPIECE;
        case ADT_OUT_SPEAKER:
            return (bt_a2dp || bt_le) ? AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER : AUDIO_DEVICE_OUT_SPEAKER;
        case ADT_OUT_HEADSET:
            if (bt_sco) return AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET;
            if (bt_le)  return AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES;
            if (usb)    return AUDIO_DEVICE_OUT_USB_HEADSET;
            return AUDIO_DEVICE_OUT_WIRED_HEADSET;
        case ADT_OUT_HEADPHONE:
            return bt_a2dp ? AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES : AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
        case ADT_OUT_TELEPHONY_TX:   return AUDIO_DEVICE_OUT_TELEPHONY_TX;
        case ADT_OUT_SPEAKER_SAFE:   return AUDIO_DEVICE_OUT_SPEAKER_SAFE;
        case ADT_OUT_SUBMIX:         return AUDIO_DEVICE_OUT_REMOTE_SUBMIX;
        case ADT_OUT_DEVICE:
            if (bt_sco)   return AUDIO_DEVICE_OUT_BLUETOOTH_SCO;
            if (bt_a2dp)  return AUDIO_DEVICE_OUT_BLUETOOTH_A2DP;
            if (hdmi)     return AUDIO_DEVICE_OUT_AUX_DIGITAL;
            if (usb)      return AUDIO_DEVICE_OUT_USB_DEVICE;
            if (ip)       return AUDIO_DEVICE_OUT_IP;
            if (spdif)    return AUDIO_DEVICE_OUT_SPDIF;
            if (hdmi_arc) return AUDIO_DEVICE_OUT_HDMI_ARC;
            return AUDIO_DEVICE_OUT_BUS;
        case ADT_OUT_ACCESSORY:      return AUDIO_DEVICE_OUT_USB_ACCESSORY;
        case ADT_OUT_DOCK:
            return analog ? AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET : AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET;
        case ADT_OUT_HEARING_AID:    return AUDIO_DEVICE_OUT_HEARING_AID;
        case ADT_OUT_LINE_AUX:       return AUDIO_DEVICE_OUT_LINE;
        case ADT_OUT_FM:             return AUDIO_DEVICE_OUT_FM;
        case ADT_OUT_BROADCAST:      return AUDIO_DEVICE_OUT_BLUETOOTH_A2DP;
        case ADT_OUT_MULTICHANNEL_GROUP: return AUDIO_DEVICE_OUT_BUS;
        case ADT_OUT_DEFAULT:        return AUDIO_DEVICE_OUT_DEFAULT;
        case ADT_OUT_ECHO_CANCELLER: return AUDIO_DEVICE_OUT_ECHO_CANCELLER;
        case ADT_OUT_AFE_PROXY:      return AUDIO_DEVICE_OUT_PROXY;
        case ADT_OUT_CARKIT:         return AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT;

        case ADT_IN_MICROPHONE:      return AUDIO_DEVICE_IN_BUILTIN_MIC;
        case ADT_IN_MICROPHONE_BACK: return AUDIO_DEVICE_IN_BACK_MIC;
        case ADT_IN_HEADSET:
            if (bt_sco) return AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
            if (bt_le)  return AUDIO_DEVICE_IN_BLUETOOTH_BLE;
            if (usb)    return AUDIO_DEVICE_IN_USB_HEADSET;
            return AUDIO_DEVICE_IN_WIRED_HEADSET;
        case ADT_IN_TELEPHONY_RX:    return AUDIO_DEVICE_IN_TELEPHONY_RX;
        case ADT_IN_FM_TUNER:        return AUDIO_DEVICE_IN_FM_TUNER;
        case ADT_IN_TV_TUNER:        return AUDIO_DEVICE_IN_TV_TUNER;
        case ADT_IN_LOOPBACK:        return AUDIO_DEVICE_IN_LOOPBACK;
        case ADT_IN_SUBMIX:          return AUDIO_DEVICE_IN_REMOTE_SUBMIX;
        case ADT_IN_DEVICE:
            if (bt_a2dp)  return AUDIO_DEVICE_IN_BLUETOOTH_A2DP;
            if (bt_le)    return AUDIO_DEVICE_IN_BLUETOOTH_BLE;
            if (hdmi)     return AUDIO_DEVICE_IN_AUX_DIGITAL;
            if (usb)      return AUDIO_DEVICE_IN_USB_DEVICE;
            if (analog)   return AUDIO_DEVICE_IN_LINE;
            if (spdif)    return AUDIO_DEVICE_IN_SPDIF;
            if (ip)       return AUDIO_DEVICE_IN_IP;
            if (hdmi_arc) return AUDIO_DEVICE_IN_HDMI_ARC;
            return AUDIO_DEVICE_IN_BUS;
        case ADT_IN_ACCESSORY:       return AUDIO_DEVICE_IN_USB_ACCESSORY;
        case ADT_IN_DOCK:
            return analog ? AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET : AUDIO_DEVICE_IN_DGTL_DOCK_HEADSET;
        case ADT_IN_AFE_PROXY:       return AUDIO_DEVICE_IN_PROXY;
        case ADT_IN_ECHO_REFERENCE:  return AUDIO_DEVICE_IN_ECHO_REFERENCE;
        case ADT_IN_DEFAULT:         return AUDIO_DEVICE_IN_DEFAULT;
        default:                     return 0;
    }
}

/* ------------------------------------------------------------------------ *
 * Ports and routes
 * ------------------------------------------------------------------------ */

static void port_info_free(gpointer data) {
    struct port_info *info = data;

    if (info->device_ext)
        g_bytes_unref(info->device_ext);
    pa_xfree(info);
}

static bool read_format(GBinderReader *r, int *format) {
    GBinderReader p;
    gboolean non_null;
    int32_t type = 0, pcm = 0;
    bool ok;

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    ok = read_int32(&p, &type) && read_int32(&p, &pcm);
    gbinder_reader_finish_parcelable(&p);

    *format = type == FORMAT_TYPE_PCM ? pcm_type_to_format(pcm) : -1;
    return ok;
}

static bool read_profile(GBinderReader *r, struct aidl_audio_profile *dst) {
    GBinderReader p;
    gboolean non_null;
    int32_t n = 0;
    bool ok;

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    memset(dst, 0, sizeof(*dst));

    ok = gbinder_reader_skip_string16(&p) && read_format(&p, &dst->format);

    ok = ok && read_int32(&p, &n);
    for (int32_t i = 0; ok && i < n; i++) {
        int32_t mask;

        ok = read_channel_layout(&p, &mask);
        if (ok && mask && dst->channel_masks_count < AIDL_AUDIO_MAX_CHANNELS_PER_PROFILE)
            dst->channel_masks[dst->channel_masks_count++] = mask;
    }

    ok = ok && read_int32(&p, &n);
    for (int32_t i = 0; ok && i < n; i++) {
        int32_t rate;

        ok = read_int32(&p, &rate);
        if (ok && dst->sample_rates_count < AIDL_AUDIO_MAX_RATES_PER_PROFILE)
            dst->sample_rates[dst->sample_rates_count++] = rate;
    }

    gbinder_reader_finish_parcelable(&p);
    return ok;
}

/* AudioDevice: { AudioDeviceDescription type; AudioDeviceAddress address } */
static bool read_device(GBinderReader *r, struct aidl_audio_port *dst) {
    GBinderReader p, d;
    gboolean non_null;
    int32_t tag, type = 0;
    bool ok, addr_non_null;

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    ok = gbinder_reader_start_parcelable(&p, &d, &non_null) && non_null;
    if (ok) {
        ok = read_int32(&d, &type) &&
             read_string(&d, dst->device_connection, sizeof(dst->device_connection));
        gbinder_reader_finish_parcelable(&d);
    }
    dst->aidl_device_type = type;

    ok = ok && read_union_header(&p, &tag, &addr_non_null);
    if (ok && addr_non_null) {
        switch (tag) {
            case DEVICE_ADDRESS_ID:
                ok = read_string(&p, dst->device_address, sizeof(dst->device_address));
                break;
            case DEVICE_ADDRESS_MAC:
            case DEVICE_ADDRESS_IPV4:
                ok = skip_byte_array(&p);
                break;
            default:
                ok = skip_int32_array(&p);
                break;
        }
    }

    gbinder_reader_finish_parcelable(&p);
    return ok;
}

static bool read_device_ext(GBinderReader *r, struct aidl_audio_port *dst, struct port_info *info) {
    GBinderReader raw, p;
    const void *blob;
    gsize size = 0;
    gboolean non_null, ok = FALSE;

    /* Keep the raw parcelable: port configs for device ports echo it back. */
    gbinder_reader_copy(&raw, r);
    blob = gbinder_reader_read_parcelable2(&raw, &size, &ok);
    if (!blob || !ok)
        return false;
    info->device_ext = g_bytes_new(blob, size);

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    ok = read_device(&p, dst) && read_int32(&p, &dst->device_flags);
    gbinder_reader_finish_parcelable(&p);

    dst->port_type = AIDL_AUDIO_PORT_TYPE_DEVICE;
    dst->direction = dst->aidl_device_type >= ADT_OUT_DEFAULT ? AIDL_AUDIO_DIR_OUTPUT : AIDL_AUDIO_DIR_INPUT;
    dst->legacy_device_type = device_type_to_legacy(dst->aidl_device_type, dst->device_connection);
    return ok;
}

static bool read_mix_ext(GBinderReader *r, struct aidl_audio_port *dst) {
    GBinderReader p;
    gboolean non_null;
    int32_t handle, tag, value;
    bool ok, usecase_non_null;

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    ok = read_int32(&p, &handle) &&
         read_union_header(&p, &tag, &usecase_non_null) &&
         (!usecase_non_null || read_int32(&p, &value)) &&
         read_int32(&p, &dst->max_open_count) &&
         read_int32(&p, &dst->max_active_count);
    gbinder_reader_finish_parcelable(&p);

    dst->port_type = AIDL_AUDIO_PORT_TYPE_MIX;
    return ok;
}

static bool read_port(GBinderReader *r, struct aidl_audio_port *dst, struct port_info *info) {
    GBinderReader p;
    gboolean non_null;
    int32_t n = 0, tag, value;
    bool ok, ext_non_null;

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    memset(dst, 0, sizeof(*dst));

    ok = read_int32(&p, &dst->id) && read_string(&p, dst->name, sizeof(dst->name));

    ok = ok && read_int32(&p, &n);
    for (int32_t i = 0; ok && i < n; i++) {
        struct aidl_audio_profile profile;

        ok = read_profile(&p, &profile);
        if (ok && dst->profiles_count < AIDL_AUDIO_MAX_PROFILES_PER_PORT)
            dst->profiles[dst->profiles_count++] = profile;
    }

    /* flags: AudioIoFlags, bit positions match the legacy flag enums. */
    ok = ok && read_union_header(&p, &tag, &ext_non_null) && ext_non_null && read_int32(&p, &value);
    if (ok) {
        info->io_flags_tag = tag;
        info->io_flags = value;
        dst->aidl_io_flags = value;
        dst->io_flags = value;
        dst->direction = tag == IO_FLAGS_OUTPUT ? AIDL_AUDIO_DIR_OUTPUT : AIDL_AUDIO_DIR_INPUT;
    }

    ok = ok && skip_parcelable_array(&p) && skip_parcelable_array(&p);  /* extraAudioDescriptors, gains */

    ok = ok && read_union_header(&p, &tag, &ext_non_null) && ext_non_null;
    if (ok) {
        switch (tag) {
            case PORT_EXT_DEVICE:
                ok = read_device_ext(&p, dst, info);
                break;
            case PORT_EXT_MIX:
                ok = read_mix_ext(&p, dst);
                break;
            default:
                ok = read_int32(&p, &value);
                dst->port_type = -1;
                break;
        }
    }

    gbinder_reader_finish_parcelable(&p);

    info->id = dst->id;
    info->port_type = dst->port_type;
    return ok;
}

int aidl_audio_module_get_ports(aidl_audio_module *m, struct aidl_audio_port **ports, int *count) {
    GBinderReader r;
    GBinderRemoteReply *reply;
    struct aidl_audio_port *arr;
    int32_t n = 0;
    int err = 0;

    pa_assert(m);
    pa_assert(ports);
    pa_assert(count);

    pthread_mutex_lock(&m->lock);

    reply = transact(m->module, IMODULE_GET_AUDIO_PORTS,
                     request_new(m->module, IMODULE_GET_AUDIO_PORTS, NULL), &r, "getAudioPorts", &err);
    if (!reply)
        goto out;

    if (!read_int32(&r, &n) || n < 0) {
        err = -EIO;
        goto out_reply;
    }

    arr = pa_xnew0(struct aidl_audio_port, n > 0 ? n : 1);
    g_hash_table_remove_all(m->ports);

    for (int32_t i = 0; i < n; i++) {
        struct port_info *info = pa_xnew0(struct port_info, 1);

        if (!read_port(&r, &arr[i], info)) {
            pa_log(LOG_PFX "getAudioPorts: cannot parse port %d", i);
            port_info_free(info);
            pa_xfree(arr);
            err = -EIO;
            goto out_reply;
        }
        g_hash_table_insert(m->ports, GINT_TO_POINTER(info->id), info);
    }

    pa_log_info(LOG_PFX "enumerated %d audio ports", n);
    *ports = arr;
    *count = n;

out_reply:
    gbinder_remote_reply_unref(reply);
out:
    pthread_mutex_unlock(&m->lock);
    return err;
}

static bool read_route(GBinderReader *r, struct aidl_audio_route *dst) {
    GBinderReader p;
    gboolean non_null, exclusive = FALSE;
    int32_t n = 0;
    bool ok;

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    memset(dst, 0, sizeof(*dst));

    ok = read_int32(&p, &n);
    for (int32_t i = 0; ok && i < n; i++) {
        int32_t id;

        ok = read_int32(&p, &id);
        if (ok && dst->sources_count < AIDL_AUDIO_MAX_SOURCES_PER_ROUTE)
            dst->source_port_ids[dst->sources_count++] = id;
    }

    ok = ok && read_int32(&p, &dst->sink_port_id) && gbinder_reader_read_bool(&p, &exclusive);
    dst->is_exclusive = exclusive;

    gbinder_reader_finish_parcelable(&p);
    return ok;
}

int aidl_audio_module_get_routes(aidl_audio_module *m, struct aidl_audio_route **routes, int *count) {
    GBinderReader r;
    GBinderRemoteReply *reply;
    struct aidl_audio_route *arr;
    int32_t n = 0;
    int err = 0;

    pa_assert(m);
    pa_assert(routes);
    pa_assert(count);

    pthread_mutex_lock(&m->lock);

    reply = transact(m->module, IMODULE_GET_AUDIO_ROUTES,
                     request_new(m->module, IMODULE_GET_AUDIO_ROUTES, NULL), &r, "getAudioRoutes", &err);
    if (!reply)
        goto out;

    if (!read_int32(&r, &n) || n < 0) {
        err = -EIO;
        goto out_reply;
    }

    arr = pa_xnew0(struct aidl_audio_route, n > 0 ? n : 1);
    for (int32_t i = 0; i < n; i++) {
        if (!read_route(&r, &arr[i])) {
            pa_log(LOG_PFX "getAudioRoutes: cannot parse route %d", i);
            pa_xfree(arr);
            err = -EIO;
            goto out_reply;
        }
    }

    pa_log_info(LOG_PFX "enumerated %d audio routes", n);
    *routes = arr;
    *count = n;

out_reply:
    gbinder_remote_reply_unref(reply);
out:
    pthread_mutex_unlock(&m->lock);
    return err;
}

/* ------------------------------------------------------------------------ *
 * Mode, volume, mute, vendor parameters
 * ------------------------------------------------------------------------ */

int aidl_audio_module_set_mode(aidl_audio_module *m, int mode) {
    int err;

    pa_assert(m);

    if (mode < 0 || mode > AUDIO_MODE_LAST)
        return -EINVAL;

    pthread_mutex_lock(&m->lock);

    /* Same order as AOSP's DeviceHalAidl: ITelephony first, then IModule. */
    err = m->telephony
        ? transact_set_int32(m->telephony, ITELEPHONY_SWITCH_AUDIO_MODE, mode, "switchAudioMode")
        : 0;
    if (err == 0)
        err = transact_set_int32(m->module, IMODULE_UPDATE_AUDIO_MODE, mode, "updateAudioMode");
    if (err == 0)
        pa_log_info(LOG_PFX "set mode %d", mode);

    pthread_mutex_unlock(&m->lock);
    return err;
}

static int module_set_float(aidl_audio_module *m, guint32 code, float value, const char *what) {
    int err;

    pa_assert(m);

    pthread_mutex_lock(&m->lock);
    err = transact_set_float(m->module, code, value, what);
    pthread_mutex_unlock(&m->lock);
    return err;
}

static int module_get_float(aidl_audio_module *m, guint32 code, float *value, const char *what) {
    int err;

    pa_assert(m);
    pa_assert(value);

    pthread_mutex_lock(&m->lock);
    err = transact_get_float(m->module, code, value, what);
    pthread_mutex_unlock(&m->lock);
    return err;
}

static int module_set_bool(aidl_audio_module *m, guint32 code, bool value, const char *what) {
    int err;

    pa_assert(m);

    pthread_mutex_lock(&m->lock);
    err = transact_set_bool(m->module, code, value, what);
    pthread_mutex_unlock(&m->lock);
    return err;
}

static int module_get_bool(aidl_audio_module *m, guint32 code, bool *value, const char *what) {
    int err;

    pa_assert(m);
    pa_assert(value);

    pthread_mutex_lock(&m->lock);
    err = transact_get_bool(m->module, code, value, what);
    pthread_mutex_unlock(&m->lock);
    return err;
}

static int module_set_int32(aidl_audio_module *m, guint32 code, int32_t value, const char *what) {
    int err;

    pa_assert(m);

    pthread_mutex_lock(&m->lock);
    err = transact_set_int32(m->module, code, value, what);
    pthread_mutex_unlock(&m->lock);
    return err;
}

int aidl_audio_module_set_master_volume(aidl_audio_module *m, float volume) {
    return module_set_float(m, IMODULE_SET_MASTER_VOLUME, volume, "setMasterVolume");
}

int aidl_audio_module_get_master_volume(aidl_audio_module *m, float *volume) {
    return module_get_float(m, IMODULE_GET_MASTER_VOLUME, volume, "getMasterVolume");
}

int aidl_audio_module_set_master_mute(aidl_audio_module *m, bool muted) {
    return module_set_bool(m, IMODULE_SET_MASTER_MUTE, muted, "setMasterMute");
}

int aidl_audio_module_get_master_mute(aidl_audio_module *m, bool *muted) {
    return module_get_bool(m, IMODULE_GET_MASTER_MUTE, muted, "getMasterMute");
}

int aidl_audio_module_set_mic_mute(aidl_audio_module *m, bool muted) {
    return module_set_bool(m, IMODULE_SET_MIC_MUTE, muted, "setMicMute");
}

int aidl_audio_module_get_mic_mute(aidl_audio_module *m, bool *muted) {
    return module_get_bool(m, IMODULE_GET_MIC_MUTE, muted, "getMicMute");
}

static int telephony_set_voice_volume(aidl_audio_module *m, float volume) {
    GBinderWriter w, cfg, vol;
    GBinderLocalRequest *req;

    req = request_new(m->telephony, ITELEPHONY_SET_TELECOM_CONFIG, &w);

    /* TelecomConfig { @nullable Float voiceVolume; TtyMode ttyMode; @nullable Boolean isHacEnabled } */
    gbinder_writer_start_parcelable(&w, &cfg);
    gbinder_writer_start_parcelable(&cfg, &vol);
    gbinder_writer_append_float(&vol, volume);
    gbinder_writer_finish_parcelable(&vol);
    gbinder_writer_append_int32(&cfg, TTY_MODE_UNSPECIFIED);
    write_null(&cfg);
    gbinder_writer_finish_parcelable(&cfg);

    return transact_void(m->telephony, ITELEPHONY_SET_TELECOM_CONFIG, req, "setTelecomConfig");
}

int aidl_audio_module_set_voice_volume(aidl_audio_module *m, float volume) {
    char buf[64];
    int err = -ENOSYS;

    pa_assert(m);

    pthread_mutex_lock(&m->lock);

    if (m->telephony)
        err = telephony_set_voice_volume(m, volume);

    pthread_mutex_unlock(&m->lock);

    /* HALs without ITelephony may still take it as a vendor parameter. */
    if (err < 0) {
        snprintf(buf, sizeof(buf), "voice_volume=%.3f", volume);
        if (aidl_audio_module_set_vendor_parameters(m, buf) == 0)
            err = 0;
    }

    return err < 0 ? -ENOSYS : 0;
}

/* VendorParameter { String id; ParcelableHolder ext } with an empty holder. */
static void write_vendor_parameter(GBinderWriter *w, const char *id) {
    GBinderWriter p;

    gbinder_writer_start_parcelable(w, &p);
    write_string(&p, id);
    gbinder_writer_append_int32(&p, 1);   /* holder non-null */
    gbinder_writer_append_int32(&p, 1);   /* STABILITY_VINTF */
    gbinder_writer_append_int32(&p, 0);   /* no payload */
    gbinder_writer_finish_parcelable(&p);
}

int aidl_audio_module_set_vendor_parameters(aidl_audio_module *m, const char *kv_pairs) {
    GBinderWriter w;
    GBinderLocalRequest *req;
    char **items;
    int n = 0, err;

    pa_assert(m);
    pa_assert(kv_pairs);

    items = g_strsplit(kv_pairs, ";", -1);
    for (char **it = items; *it; it++)
        if (**it)
            n++;

    pthread_mutex_lock(&m->lock);

    req = request_new(m->module, IMODULE_SET_VENDOR_PARAMETERS, &w);
    gbinder_writer_append_int32(&w, n);
    for (char **it = items; *it; it++)
        if (**it)
            write_vendor_parameter(&w, *it);
    gbinder_writer_append_bool(&w, FALSE);   /* async */

    err = transact_void(m->module, IMODULE_SET_VENDOR_PARAMETERS, req, "setVendorParameters");

    pthread_mutex_unlock(&m->lock);
    g_strfreev(items);
    return err;
}

char *aidl_audio_module_get_vendor_parameters(aidl_audio_module *m, const char *keys) {
    GBinderWriter w;
    GBinderReader r;
    GBinderLocalRequest *req;
    GBinderRemoteReply *reply;
    GString *out = NULL;
    char **items;
    int32_t n = 0;
    int err = 0;

    pa_assert(m);
    pa_assert(keys);

    items = g_strsplit(keys, ";", -1);
    for (char **it = items; *it; it++)
        if (**it)
            n++;

    pthread_mutex_lock(&m->lock);

    req = request_new(m->module, IMODULE_GET_VENDOR_PARAMETERS, &w);
    gbinder_writer_append_int32(&w, n);
    for (char **it = items; *it; it++)
        if (**it)
            write_string(&w, *it);

    reply = transact(m->module, IMODULE_GET_VENDOR_PARAMETERS, req, &r, "getVendorParameters", &err);
    if (reply) {
        out = g_string_new(NULL);

        /* The typed payload sits in a ParcelableHolder we cannot decode, so
         * return the ids like the legacy "key=value" convention expects. */
        if (read_int32(&r, &n)) {
            for (int32_t i = 0; i < n; i++) {
                GBinderReader p;
                gboolean non_null;
                char *id = NULL;

                if (!gbinder_reader_start_parcelable(&r, &p, &non_null) || !non_null)
                    break;
                gbinder_reader_read_nullable_string16(&p, &id);
                gbinder_reader_finish_parcelable(&p);

                if (id) {
                    if (out->len)
                        g_string_append_c(out, ';');
                    g_string_append(out, id);
                    g_free(id);
                }
            }
        }
        gbinder_remote_reply_unref(reply);
    }

    pthread_mutex_unlock(&m->lock);
    g_strfreev(items);

    if (!out)
        return NULL;

    char *ret = pa_xstrdup(out->str);
    g_string_free(out, TRUE);
    return ret;
}

/* ------------------------------------------------------------------------ *
 * Port configs and patches
 * ------------------------------------------------------------------------ */

static void write_port_config(GBinderWriter *w, const struct aidl_port_config *cfg, const struct port_info *info) {
    GBinderWriter p, sub;

    gbinder_writer_start_parcelable(w, &p);
    gbinder_writer_append_int32(&p, cfg->config_id);
    gbinder_writer_append_int32(&p, cfg->port_id);

    /* @nullable Int sampleRate; start_parcelable writes the non-null marker. */
    if (cfg->sample_rate > 0) {
        gbinder_writer_start_parcelable(&p, &sub);
        gbinder_writer_append_int32(&sub, cfg->sample_rate);
        gbinder_writer_finish_parcelable(&sub);
    } else
        write_null(&p);

    /* @nullable AudioChannelLayout channelMask */
    if (cfg->channel_mask != 0) {
        write_union_header(&p, CHANNEL_LAYOUT_LAYOUT_MASK);
        gbinder_writer_append_int32(&p, cfg->channel_mask);
    } else
        write_null(&p);

    /* @nullable AudioFormatDescription format */
    if (cfg->format >= 0) {
        gbinder_writer_start_parcelable(&p, &sub);
        gbinder_writer_append_int32(&sub, FORMAT_TYPE_PCM);
        gbinder_writer_append_int32(&sub, format_to_pcm_type(cfg->format));
        write_string(&sub, "");
        gbinder_writer_finish_parcelable(&sub);
    } else
        write_null(&p);

    /* @nullable AudioGainConfig gain */
    write_null(&p);

    /* @nullable AudioIoFlags flags: a mix port config must repeat the
     * port's own flags, AUDIO_*_FLAG_NONE from upstream code must not
     * clear them or the HAL rejects the config. */
    if (info->port_type == AIDL_AUDIO_PORT_TYPE_MIX) {
        write_union_header(&p, info->io_flags_tag);
        gbinder_writer_append_int32(&p, cfg->io_flags > 0 ? cfg->io_flags : info->io_flags);
    } else
        write_null(&p);

    /* AudioPortExt ext */
    if (info->port_type == AIDL_AUDIO_PORT_TYPE_DEVICE && info->device_ext) {
        gsize size;
        const void *blob = g_bytes_get_data(info->device_ext, &size);

        write_union_header(&p, PORT_EXT_DEVICE);
        gbinder_writer_append_parcelable(&p, blob, size);
    } else {
        write_union_header(&p, PORT_EXT_UNSPECIFIED);
        gbinder_writer_append_bool(&p, FALSE);
    }

    gbinder_writer_finish_parcelable(&p);
}

static bool read_port_config(GBinderReader *r, struct aidl_port_config *cfg) {
    GBinderReader p, sub;
    gboolean non_null;
    int32_t tag, value;
    bool ok, present;

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    memset(cfg, 0, sizeof(*cfg));
    cfg->format = -1;
    cfg->io_flags = -1;

    ok = read_int32(&p, &cfg->config_id) && read_int32(&p, &cfg->port_id);

    /* sampleRate */
    ok = ok && gbinder_reader_start_parcelable(&p, &sub, &non_null);
    if (ok && non_null) {
        ok = read_int32(&sub, &cfg->sample_rate);
        gbinder_reader_finish_parcelable(&sub);
    }

    /* channelMask */
    ok = ok && read_channel_layout(&p, &cfg->channel_mask);

    /* format */
    ok = ok && gbinder_reader_start_parcelable(&p, &sub, &non_null);
    if (ok && non_null) {
        int32_t type = 0, pcm = 0;

        ok = read_int32(&sub, &type) && read_int32(&sub, &pcm);
        gbinder_reader_finish_parcelable(&sub);
        cfg->format = type == FORMAT_TYPE_PCM ? pcm_type_to_format(pcm) : -1;
    }

    /* gain */
    ok = ok && gbinder_reader_skip_parcelable(&p);

    /* flags */
    ok = ok && read_union_header(&p, &tag, &present);
    if (ok && present) {
        ok = read_int32(&p, &value);
        cfg->io_flags = value;
    }

    /* ext is not needed by callers. */
    gbinder_reader_finish_parcelable(&p);
    return ok;
}

/* Sends setAudioPortConfig with the body already written to req. On
 * return *suggested holds the HAL's answer and raw/raw_size point into
 * the reply at the suggested parcelable's payload. */
static GBinderRemoteReply *set_port_config_transact(aidl_audio_module *m, GBinderLocalRequest *req,
                                                    struct aidl_port_config *suggested, bool *applied,
                                                    const void **raw, gsize *raw_size, int *err) {
    GBinderReader r, copy;
    GBinderRemoteReply *reply;
    gboolean b = FALSE, ok = FALSE;

    reply = transact(m->module, IMODULE_SET_AUDIO_PORT_CONFIG, req, &r, "setAudioPortConfig", err);
    if (!reply)
        return NULL;

    gbinder_reader_copy(&copy, &r);
    if (!gbinder_reader_read_bool(&r, &b) || !read_port_config(&r, suggested)) {
        pa_log(LOG_PFX "setAudioPortConfig: cannot parse reply");
        gbinder_remote_reply_unref(reply);
        *err = -EIO;
        return NULL;
    }
    *applied = b;

    gbinder_reader_read_bool(&copy, &b);
    *raw = gbinder_reader_read_parcelable2(&copy, raw_size, &ok);
    if (!ok)
        *raw = NULL;
    return reply;
}

int aidl_audio_module_set_port_config(aidl_audio_module *m,
                                      const struct aidl_port_config *config,
                                      struct aidl_port_config *suggested,
                                      int *applied) {
    GBinderWriter w;
    GBinderLocalRequest *req;
    GBinderRemoteReply *reply;
    struct port_info *info;
    struct aidl_port_config result;
    const void *raw = NULL;
    gsize raw_size = 0;
    bool is_applied = false;
    int err = 0;

    pa_assert(m);
    pa_assert(config);

    pthread_mutex_lock(&m->lock);

    info = g_hash_table_lookup(m->ports, GINT_TO_POINTER(config->port_id));
    if (!info) {
        pa_log(LOG_PFX "setAudioPortConfig: unknown port id %d", config->port_id);
        err = -ENOENT;
        goto out;
    }

    req = request_new(m->module, IMODULE_SET_AUDIO_PORT_CONFIG, &w);
    write_port_config(&w, config, info);
    debug_dump_writer("setAudioPortConfig request", &w);

    reply = set_port_config_transact(m, req, &result, &is_applied, &raw, &raw_size, &err);
    if (!reply)
        goto out;

    /* Not applied means "resubmit what I suggested to commit it". The
     * suggestion is echoed back verbatim, extension and all. */
    if (!is_applied && raw) {
        GBinderRemoteReply *retry;
        struct aidl_port_config result2;
        const void *raw2;
        gsize raw2_size;
        bool applied2 = false;
        int err2 = 0;

        req = request_new(m->module, IMODULE_SET_AUDIO_PORT_CONFIG, &w);
        gbinder_writer_append_parcelable(&w, raw, raw_size);
        debug_dump_writer("setAudioPortConfig retry request", &w);

        retry = set_port_config_transact(m, req, &result2, &applied2, &raw2, &raw2_size, &err2);
        if (retry) {
            result = result2;
            is_applied = applied2;
            gbinder_remote_reply_unref(retry);
        } else
            pa_log(LOG_PFX "setAudioPortConfig: retry for port %d failed (%d)", config->port_id, -err2);
    }
    gbinder_remote_reply_unref(reply);

    if (suggested)
        *suggested = result;
    if (applied)
        *applied = is_applied;

out:
    pthread_mutex_unlock(&m->lock);
    return err;
}

int aidl_audio_module_reset_port_config(aidl_audio_module *m, int config_id) {
    int err;

    pa_assert(m);

    if (config_id <= 0)
        return 0;

    err = module_set_int32(m, IMODULE_RESET_AUDIO_PORT_CONFIG, config_id, "resetAudioPortConfig");
    /* EX_ILLEGAL_ARGUMENT: already gone. */
    return err == -EINVAL ? 0 : err;
}

int aidl_audio_module_create_patch(aidl_audio_module *m,
                                   const int *source_port_config_ids, int n_sources,
                                   const int *sink_port_config_ids, int n_sinks,
                                   int *patch_id) {
    GBinderWriter w, p;
    GBinderReader r, rp;
    GBinderLocalRequest *req;
    GBinderRemoteReply *reply;
    gboolean non_null;
    int err = 0;

    pa_assert(m);
    pa_assert(patch_id);

    pthread_mutex_lock(&m->lock);

    req = request_new(m->module, IMODULE_SET_AUDIO_PATCH, &w);

    /* AudioPatch { int id; int[] sourcePortConfigIds; int[] sinkPortConfigIds;
     *              int minimumStreamBufferSizeFrames; int[] latenciesMs } */
    gbinder_writer_start_parcelable(&w, &p);
    gbinder_writer_append_int32(&p, 0);
    write_int32_array(&p, source_port_config_ids, n_sources);
    write_int32_array(&p, sink_port_config_ids, n_sinks);
    gbinder_writer_append_int32(&p, 0);
    gbinder_writer_append_int32(&p, 0);
    gbinder_writer_finish_parcelable(&p);

    reply = transact(m->module, IMODULE_SET_AUDIO_PATCH, req, &r, "setAudioPatch", &err);
    if (!reply)
        goto out;

    if (gbinder_reader_start_parcelable(&r, &rp, &non_null) && non_null) {
        if (!read_int32(&rp, patch_id))
            err = -EIO;
        gbinder_reader_finish_parcelable(&rp);
    } else
        err = -EIO;

    gbinder_remote_reply_unref(reply);

out:
    pthread_mutex_unlock(&m->lock);
    return err;
}

int aidl_audio_module_release_patch(aidl_audio_module *m, int patch_id) {
    int err;

    pa_assert(m);

    if (patch_id <= 0)
        return 0;

    err = module_set_int32(m, IMODULE_RESET_AUDIO_PATCH, patch_id, "resetAudioPatch");
    return err == -EINVAL ? 0 : err;
}

/* ------------------------------------------------------------------------ *
 * Streams
 * ------------------------------------------------------------------------ */

static bool read_mq_descriptor(GBinderReader *r, struct aidl_fmq_desc *d) {
    GBinderReader p, h;
    gboolean non_null;
    int32_t n = 0;
    bool ok;

    aidl_fmq_desc_init(d);

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    /* GrantorDescriptor[] grantors */
    ok = read_int32(&p, &n) && n >= 3 && n <= AIDL_FMQ_MAX_GRANTORS;
    for (int32_t i = 0; ok && i < n; i++) {
        GBinderReader g;
        int32_t fd_index = 0, offset = 0;
        gint64 extent = 0;

        ok = gbinder_reader_start_parcelable(&p, &g, &non_null) && non_null;
        if (!ok)
            break;
        ok = read_int32(&g, &fd_index) && read_int32(&g, &offset) && gbinder_reader_read_int64(&g, &extent);
        gbinder_reader_finish_parcelable(&g);

        if (ok && d->n_grantors < AIDL_FMQ_MAX_GRANTORS) {
            d->grantors[d->n_grantors].fd_index = fd_index;
            d->grantors[d->n_grantors].offset = offset;
            d->grantors[d->n_grantors].extent = extent;
            d->n_grantors++;
        } else
            ok = false;
    }

    /* NativeHandle handle { ParcelFileDescriptor[] fds; int[] ints } */
    ok = ok && gbinder_reader_start_parcelable(&p, &h, &non_null) && non_null;
    if (ok) {
        ok = read_int32(&h, &n) && n > 0 && n <= AIDL_FMQ_MAX_FDS;
        for (int32_t i = 0; ok && i < n; i++) {
            int32_t marker = 0, has_comm = 0;
            int fd;

            ok = read_int32(&h, &marker) && marker == 1 && read_int32(&h, &has_comm);
            if (!ok)
                break;

            fd = gbinder_reader_read_dup_fd(&h);
            if (fd < 0 || d->n_fds >= AIDL_FMQ_MAX_FDS) {
                if (fd >= 0)
                    close(fd);
                ok = false;
                break;
            }
            d->fds[d->n_fds++] = fd;

            if (has_comm) {
                int comm = gbinder_reader_read_dup_fd(&h);
                if (comm >= 0)
                    close(comm);
            }
        }
        ok = ok && skip_int32_array(&h);
        gbinder_reader_finish_parcelable(&h);
    }

    ok = ok && read_int32(&p, &d->quantum) && read_int32(&p, &d->flags);

    gbinder_reader_finish_parcelable(&p);
    if (!ok)
        aidl_fmq_desc_clear(d);
    return ok;
}

/* StreamDescriptor { MQDescriptor command; MQDescriptor reply; int frameSizeBytes;
 *                    long bufferSizeFrames; AudioBuffer audio } */
static bool read_stream_descriptor(GBinderReader *r, aidl_audio_stream *s,
                                   struct aidl_fmq_desc *command, struct aidl_fmq_desc *reply,
                                   struct aidl_fmq_desc *data) {
    GBinderReader p;
    gboolean non_null;
    gint64 frames = 0;
    int32_t tag;
    bool ok, buf_non_null;

    if (!gbinder_reader_start_parcelable(r, &p, &non_null) || !non_null)
        return false;

    ok = read_mq_descriptor(&p, command) &&
         read_mq_descriptor(&p, reply) &&
         read_int32(&p, &s->frame_size_bytes) &&
         gbinder_reader_read_int64(&p, &frames) &&
         read_union_header(&p, &tag, &buf_non_null) && buf_non_null;
    s->buffer_size_frames = frames;

    if (ok) {
        if (tag == AUDIO_BUFFER_FMQ)
            ok = read_mq_descriptor(&p, data);
        else {
            pa_log(LOG_PFX "MMAP streams are not supported");
            ok = false;
        }
    }

    gbinder_reader_finish_parcelable(&p);
    return ok;
}

static void write_source_metadata(GBinderWriter *w, const struct aidl_stream_params *params) {
    GBinderWriter meta, track;

    /* SourceMetadata { PlaybackTrackMetadata[] tracks } */
    gbinder_writer_start_parcelable(w, &meta);
    gbinder_writer_append_int32(&meta, 1);
    gbinder_writer_start_parcelable(&meta, &track);
    gbinder_writer_append_int32(&track, params->audio_usage);
    gbinder_writer_append_int32(&track, params->audio_content_type);
    gbinder_writer_append_float(&track, 1.0f);          /* gain */
    write_union_header(&track, CHANNEL_LAYOUT_NONE);    /* channelMask */
    gbinder_writer_append_int32(&track, 0);
    write_null(&track);                                 /* sourceDevice */
    gbinder_writer_append_int32(&track, 0);             /* tags */
    gbinder_writer_finish_parcelable(&track);
    gbinder_writer_finish_parcelable(&meta);
}

static void write_sink_metadata(GBinderWriter *w, const struct aidl_stream_params *params) {
    GBinderWriter meta, track;

    /* SinkMetadata { RecordTrackMetadata[] tracks } */
    gbinder_writer_start_parcelable(w, &meta);
    gbinder_writer_append_int32(&meta, 1);
    gbinder_writer_start_parcelable(&meta, &track);
    gbinder_writer_append_int32(&track, params->audio_source);
    gbinder_writer_append_float(&track, 1.0f);          /* gain */
    write_null(&track);                                 /* destinationDevice */
    write_union_header(&track, CHANNEL_LAYOUT_NONE);    /* channelMask */
    gbinder_writer_append_int32(&track, 0);
    gbinder_writer_append_int32(&track, 0);             /* tags */
    gbinder_writer_finish_parcelable(&track);
    gbinder_writer_finish_parcelable(&meta);
}

static void stream_free(aidl_audio_stream *s) {
    if (s->common)
        gbinder_client_unref(s->common);
    if (s->common_obj)
        gbinder_remote_object_unref(s->common_obj);
    if (s->stream)
        gbinder_client_unref(s->stream);
    if (s->stream_obj)
        gbinder_remote_object_unref(s->stream_obj);
    aidl_fmq_free(s->command);
    aidl_fmq_free(s->reply);
    aidl_fmq_free(s->data);
    pthread_mutex_destroy(&s->lock);
    pa_xfree(s);
}

static aidl_audio_stream *stream_open(aidl_audio_module *m, int direction,
                                      const struct aidl_stream_params *params,
                                      struct aidl_stream_info *info) {
    const bool output = direction == AIDL_AUDIO_DIR_OUTPUT;
    const guint32 open_code = output ? IMODULE_OPEN_OUTPUT_STREAM : IMODULE_OPEN_INPUT_STREAM;
    const guint32 common_code = output ? ISTREAM_OUT_GET_STREAM_COMMON : ISTREAM_IN_GET_STREAM_COMMON;
    const char *open_name = output ? "openOutputStream" : "openInputStream";
    GBinderWriter w, args;
    GBinderReader r, ret;
    GBinderLocalRequest *req;
    GBinderRemoteReply *reply;
    struct aidl_fmq_desc command, reply_desc, data;
    aidl_audio_stream *s;
    gboolean non_null;
    bool ok;
    int err = 0;

    s = pa_xnew0(aidl_audio_stream, 1);
    s->module = m;
    s->direction = direction;
    s->state = STATE_STANDBY;
    s->observable_frames = -1;
    pthread_mutex_init(&s->lock, NULL);

    aidl_fmq_desc_init(&command);
    aidl_fmq_desc_init(&reply_desc);
    aidl_fmq_desc_init(&data);

    pthread_mutex_lock(&m->lock);

    req = request_new(m->module, open_code, &w);

    /* Open{Output,Input}StreamArguments. The trailing nullable callbacks
     * of the output variant are left out, which reads back as null. */
    gbinder_writer_start_parcelable(&w, &args);
    gbinder_writer_append_int32(&args, params->mix_port_config_id);
    if (output) {
        write_source_metadata(&args, params);
        write_null(&args);   /* offloadInfo */
    } else
        write_sink_metadata(&args, params);
    gbinder_writer_append_int64(&args, params->requested_buffer_frames > 0
                                       ? params->requested_buffer_frames : DEFAULT_BUFFER_FRAMES);
    gbinder_writer_finish_parcelable(&args);

    reply = transact(m->module, open_code, req, &r, open_name, &err);
    if (!reply) {
        pthread_mutex_unlock(&m->lock);
        stream_free(s);
        return NULL;
    }

    /* Open{Output,Input}StreamReturn { IStream stream; StreamDescriptor desc } */
    ok = gbinder_reader_start_parcelable(&r, &ret, &non_null) && non_null;
    if (ok) {
        ok = gbinder_reader_read_nullable_object(&ret, &s->stream_obj) && s->stream_obj &&
             read_stream_descriptor(&ret, s, &command, &reply_desc, &data);
        gbinder_reader_finish_parcelable(&ret);
    }
    gbinder_remote_reply_unref(reply);
    pthread_mutex_unlock(&m->lock);

    if (!s->stream_obj) {
        pa_log(LOG_PFX "%s: no stream in reply", open_name);
        goto fail;
    }
    s->stream = gbinder_client_new(s->stream_obj, output ? ISTREAM_OUT_IFACE : ISTREAM_IN_IFACE);

    /* IStreamCommon is what closes the stream, get it before anything else can fail. */
    reply = transact(s->stream, common_code, request_new(s->stream, common_code, NULL), &r, "getStreamCommon", &err);
    if (reply) {
        gbinder_reader_read_nullable_object(&r, &s->common_obj);
        gbinder_remote_reply_unref(reply);
    }
    if (!s->common_obj) {
        pa_log(LOG_PFX "getStreamCommon: no object in reply");
        goto fail;
    }
    s->common = gbinder_client_new(s->common_obj, ISTREAM_COMMON_IFACE);

    if (!ok) {
        pa_log(LOG_PFX "%s: cannot parse stream descriptor", open_name);
        goto fail_close;
    }

    s->command = aidl_fmq_new(&command);
    s->reply = aidl_fmq_new(&reply_desc);
    s->data = aidl_fmq_new(&data);
    if (!s->command || !s->reply || !s->data) {
        pa_log(LOG_PFX "%s: cannot map stream queues", open_name);
        goto fail_close;
    }
    if (aidl_fmq_item_size(s->command) != sizeof(struct stream_command) ||
        aidl_fmq_item_size(s->reply) != sizeof(struct stream_reply) ||
        aidl_fmq_item_size(s->data) != 1) {
        pa_log(LOG_PFX "%s: unexpected queue item sizes %zu/%zu", open_name,
               aidl_fmq_item_size(s->command), aidl_fmq_item_size(s->reply));
        goto fail_close;
    }

    if (s->frame_size_bytes <= 0 || s->buffer_size_frames <= 0 ||
        s->buffer_size_frames > INT_MAX ||
        (uint64_t) s->buffer_size_frames > aidl_fmq_capacity(s->data) / s->frame_size_bytes) {
        pa_log(LOG_PFX "%s: invalid stream buffer size", open_name);
        goto fail_close;
    }

    if (info) {
        info->buffer_frames = (int) s->buffer_size_frames;
        info->frame_bytes = s->frame_size_bytes;
    }

    pa_log_info(LOG_PFX "%s: frame %d bytes, buffer %" PRId64 " frames, data queue %zu bytes",
                open_name, s->frame_size_bytes, s->buffer_size_frames,
                aidl_fmq_capacity(s->data) * aidl_fmq_item_size(s->data));
    return s;

fail_close:
    transact_void(s->common, ISTREAM_COMMON_CLOSE, request_new(s->common, ISTREAM_COMMON_CLOSE, NULL), "close");
fail:
    aidl_fmq_desc_clear(&command);
    aidl_fmq_desc_clear(&reply_desc);
    aidl_fmq_desc_clear(&data);
    stream_free(s);
    return NULL;
}

aidl_audio_stream *aidl_audio_open_output_stream(aidl_audio_module *m,
                                                 const struct aidl_stream_params *params,
                                                 struct aidl_stream_info *info) {
    pa_assert(m);
    pa_assert(params);

    return stream_open(m, AIDL_AUDIO_DIR_OUTPUT, params, info);
}

aidl_audio_stream *aidl_audio_open_input_stream(aidl_audio_module *m,
                                                const struct aidl_stream_params *params,
                                                struct aidl_stream_info *info) {
    pa_assert(m);
    pa_assert(params);

    return stream_open(m, AIDL_AUDIO_DIR_INPUT, params, info);
}

static bool module_is_dead(aidl_audio_module *m) {
    return __atomic_load_n(&m->dead, __ATOMIC_ACQUIRE);
}

/* Sends one command and waits for the reply. */
static int stream_command(aidl_audio_stream *s, int32_t tag, int32_t value, struct stream_reply *reply) {
    struct stream_command cmd = { tag, value };
    int ret;

    /* Nobody would answer, every command would wait for the timeout. */
    if (module_is_dead(s->module))
        return -ENOTCONN;

    /* Replies have no sequence number; a timeout requires reopening the stream. */
    if (s->failed)
        return -EIO;

    ret = aidl_fmq_write_blocking(s->command, &cmd, 1, STREAM_REPLY_TIMEOUT_MS);
    if (ret < 0) {
        pa_log(LOG_PFX "command %d: cannot queue (%d)", tag, -ret);
        s->failed = true;
        return ret;
    }

    ret = aidl_fmq_read_blocking(s->reply, reply, 1, STREAM_REPLY_TIMEOUT_MS);
    if (ret < 0) {
        pa_log(LOG_PFX "command %d: no reply (%d)", tag, -ret);
        s->failed = true;
        return ret;
    }

    s->state = reply->state;
    if (reply->latency_ms > 0)
        s->latency_ms = reply->latency_ms;
    if (reply->observable.frames >= 0)
        s->observable_frames = reply->observable.frames;

    if (reply->status != 0) {
        pa_log_debug(LOG_PFX "command %d: status %d, state %d", tag, reply->status, reply->state);
        return reply->status < 0 ? reply->status : -EIO;
    }
    if (tag == CMD_BURST && (reply->fmq_byte_count < 0 || reply->fmq_byte_count > value)) {
        s->failed = true;
        return -EIO;
    }
    return 0;
}

/* The legacy API has no explicit start: leave STANDBY on first use. */
static int stream_ensure_started(aidl_audio_stream *s) {
    struct stream_reply reply;
    int ret;

    if (s->state != STATE_STANDBY)
        return 0;

    if ((ret = stream_command(s, CMD_START, 0, &reply)) < 0)
        return ret;

    if (s->state != STATE_IDLE && s->state != STATE_ACTIVE) {
        pa_log_warn(LOG_PFX "unexpected state %d after start", s->state);
        return -EIO;
    }
    return 0;
}

int aidl_audio_stream_write(aidl_audio_stream *s, const void *buffer, int bytes) {
    struct stream_reply reply;
    size_t quantum, avail, to_write;
    int ret;

    pa_assert(s);
    pa_assert(buffer);

    if (s->direction != AIDL_AUDIO_DIR_OUTPUT || bytes < 0)
        return -EINVAL;

    pthread_mutex_lock(&s->lock);

    if ((ret = stream_ensure_started(s)) < 0)
        goto out;

    quantum = aidl_fmq_item_size(s->data);
    avail = aidl_fmq_available_to_write(s->data) * quantum;
    to_write = PA_MIN((size_t) bytes, avail) / quantum * quantum;
    if (to_write == 0) {
        ret = 0;
        goto out;
    }

    if (!aidl_fmq_write(s->data, buffer, to_write / quantum)) {
        ret = -EIO;
        goto out;
    }

    if ((ret = stream_command(s, CMD_BURST, (int32_t) to_write, &reply)) == 0)
        ret = reply.fmq_byte_count;

out:
    pthread_mutex_unlock(&s->lock);
    return ret;
}

int aidl_audio_stream_read(aidl_audio_stream *s, void *buffer, int bytes) {
    struct stream_reply reply;
    size_t quantum, to_read;
    int ret;

    pa_assert(s);
    pa_assert(buffer);

    if (s->direction != AIDL_AUDIO_DIR_INPUT || bytes <= 0)
        return -EINVAL;

    pthread_mutex_lock(&s->lock);

    if ((ret = stream_ensure_started(s)) < 0)
        goto out;

    if ((ret = stream_command(s, CMD_BURST, bytes, &reply)) < 0)
        goto out;

    quantum = aidl_fmq_item_size(s->data);
    to_read = PA_MIN((size_t) PA_MAX(reply.fmq_byte_count, 0), (size_t) bytes) / quantum * quantum;
    if (to_read == 0) {
        ret = 0;
        goto out;
    }

    ret = aidl_fmq_read(s->data, buffer, to_read / quantum) ? (int) to_read : -EIO;

out:
    pthread_mutex_unlock(&s->lock);
    return ret;
}

int aidl_audio_stream_start(aidl_audio_stream *s) {
    struct stream_reply reply;
    int ret;

    pa_assert(s);

    pthread_mutex_lock(&s->lock);
    ret = stream_command(s, CMD_START, 0, &reply);
    if (ret == 0 && s->state != STATE_IDLE && s->state != STATE_ACTIVE) {
        pa_log_warn(LOG_PFX "unexpected state %d after start", s->state);
        ret = -EIO;
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

/* ACTIVE -pause-> PAUSED -flush-> IDLE -standby-> STANDBY, entering
 * wherever the stream currently is. */
static int stream_standby_locked(aidl_audio_stream *s) {
    struct stream_reply reply;
    int ret;

    if (s->state == STATE_ACTIVE && (ret = stream_command(s, CMD_PAUSE, 0, &reply)) < 0)
        return ret;
    if (s->state == STATE_PAUSED && (ret = stream_command(s, CMD_FLUSH, 0, &reply)) < 0)
        return ret;
    if (s->state == STATE_IDLE && (ret = stream_command(s, CMD_STANDBY, 0, &reply)) < 0)
        return ret;

    if (s->state != STATE_STANDBY) {
        pa_log_warn(LOG_PFX "expected STANDBY, stream is in state %d", s->state);
        return -EIO;
    }
    return 0;
}

int aidl_audio_stream_standby(aidl_audio_stream *s) {
    int ret;

    pa_assert(s);

    pthread_mutex_lock(&s->lock);
    ret = stream_standby_locked(s);
    /* PulseAudio cannot reject suspension, even when the HAL has died. */
    if (ret < 0) {
        s->failed = true;
        s->state = STATE_STANDBY;
    }
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int aidl_audio_stream_close(aidl_audio_stream *s) {
    pa_assert(s);

    pthread_mutex_lock(&s->lock);
    if (s->state != STATE_STANDBY)
        stream_standby_locked(s);

    transact_void(s->common, ISTREAM_COMMON_PREPARE_TO_CLOSE,
                  request_new(s->common, ISTREAM_COMMON_PREPARE_TO_CLOSE, NULL), "prepareToClose");
    transact_void(s->common, ISTREAM_COMMON_CLOSE,
                  request_new(s->common, ISTREAM_COMMON_CLOSE, NULL), "close");
    pthread_mutex_unlock(&s->lock);

    stream_free(s);
    return 0;
}

int aidl_audio_stream_get_latency_ms(aidl_audio_stream *s) {
    pa_assert(s);

    int latency;

    pthread_mutex_lock(&s->lock);
    latency = s->latency_ms;
    pthread_mutex_unlock(&s->lock);
    return latency;
}

int aidl_audio_stream_get_render_position(aidl_audio_stream *s, uint32_t *frames) {
    int ret = 0;

    pa_assert(s);
    pa_assert(frames);

    if (s->direction != AIDL_AUDIO_DIR_OUTPUT)
        return -EINVAL;

    pthread_mutex_lock(&s->lock);
    if (s->observable_frames < 0)
        ret = -ENOSYS;
    else
        *frames = (uint32_t) s->observable_frames;
    pthread_mutex_unlock(&s->lock);
    return ret;
}

int aidl_audio_stream_set_hw_volume(aidl_audio_stream *s, float left, float right) {
    GBinderWriter w;
    GBinderLocalRequest *req;
    int err;

    pa_assert(s);

    if (s->direction != AIDL_AUDIO_DIR_OUTPUT)
        return -ENOSYS;

    pthread_mutex_lock(&s->lock);
    req = request_new(s->stream, ISTREAM_OUT_SET_HW_VOLUME, &w);
    gbinder_writer_append_int32(&w, 2);
    gbinder_writer_append_float(&w, left);
    gbinder_writer_append_float(&w, right);
    err = transact_void(s->stream, ISTREAM_OUT_SET_HW_VOLUME, req, "setHwVolume");
    pthread_mutex_unlock(&s->lock);

    return err < 0 ? -ENOSYS : 0;
}

/* ------------------------------------------------------------------------ *
 * Connection
 * ------------------------------------------------------------------------ */

static void module_died(GBinderRemoteObject *obj, void *user_data) {
    aidl_audio_module *m = user_data;

    __atomic_store_n(&m->dead, true, __ATOMIC_RELEASE);
    pa_log_error(LOG_PFX "audio HAL died, reload the droid card to recover");
}

static bool connect_telephony(aidl_audio_module *m) {
    GBinderReader r;
    GBinderRemoteReply *reply;
    GBinderRemoteObject *obj = NULL;
    int err = 0;

    reply = transact(m->module, IMODULE_GET_TELEPHONY,
                     request_new(m->module, IMODULE_GET_TELEPHONY, NULL), &r, "getTelephony", &err);
    if (!reply)
        return false;

    if (gbinder_reader_read_nullable_object(&r, &obj) && obj) {
        m->telephony_obj = obj;
        m->telephony = gbinder_client_new(obj, ITELEPHONY_IFACE);
    }
    gbinder_remote_reply_unref(reply);
    return m->telephony != NULL;
}

aidl_audio_module *aidl_audio_module_connect(pa_core *core) {
    aidl_audio_module *m;
    struct aidl_audio_port *ports;
    int count;

    pa_assert(core);

    if (!droid_aidl_eventloop_init(core))
        return NULL;

    m = pa_xnew0(aidl_audio_module, 1);
    m->core = core;
    pthread_mutex_init(&m->lock, NULL);
    m->ports = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, port_info_free);

    if (!(m->sm = gbinder_servicemanager_new(GBINDER_DEFAULT_BINDER))) {
        pa_log(LOG_PFX "cannot open " GBINDER_DEFAULT_BINDER);
        goto fail;
    }

    for (int waited = 0; ; waited += SERVICE_POLL_MS) {
        GBinderRemoteObject *obj;
        int status = 0;

        if ((obj = gbinder_servicemanager_get_service_sync(m->sm, IMODULE_INSTANCE, &status))) {
            m->module_obj = gbinder_remote_object_ref(obj);
            break;
        }
        if (waited >= SERVICE_WAIT_MS) {
            pa_log(LOG_PFX "service " IMODULE_INSTANCE " not found (%d)", status);
            goto fail;
        }
        g_usleep(SERVICE_POLL_MS * 1000);
    }

    m->module = gbinder_client_new(m->module_obj, IMODULE_IFACE);
    m->death_id = gbinder_remote_object_add_death_handler(m->module_obj, module_died, m);

    if (!connect_telephony(m))
        pa_log_info(LOG_PFX "HAL has no ITelephony, voice calls use fallbacks");

    /* Port configs need the port table, fetch it now. */
    if (aidl_audio_module_get_ports(m, &ports, &count) < 0)
        goto fail;
    pa_xfree(ports);

    pa_log_info(LOG_PFX "connected to " IMODULE_INSTANCE);
    return m;

fail:
    aidl_audio_module_disconnect(m);
    return NULL;
}

void aidl_audio_module_disconnect(aidl_audio_module *m) {
    if (!m)
        return;

    if (m->telephony)
        gbinder_client_unref(m->telephony);
    if (m->telephony_obj)
        gbinder_remote_object_unref(m->telephony_obj);
    if (m->module)
        gbinder_client_unref(m->module);
    if (m->module_obj) {
        if (m->death_id)
            gbinder_remote_object_remove_handler(m->module_obj, m->death_id);
        gbinder_remote_object_unref(m->module_obj);
    }
    if (m->sm)
        gbinder_servicemanager_unref(m->sm);
    if (m->ports)
        g_hash_table_destroy(m->ports);
    pthread_mutex_destroy(&m->lock);
    pa_xfree(m);

    droid_aidl_eventloop_done();
}
