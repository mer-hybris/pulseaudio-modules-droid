/*
 * Copyright (C) 2013 Jolla Ltd.
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

#ifndef _ANDROID_UTIL_V412_H_
#define _ANDROID_UTIL_V412_H_

#ifdef DROID_DEVICE_SBJ
#define QCOM_HARDWARE
#endif

#include <hardware/audio.h>
#include <hardware_legacy/audio_policy_conf.h>

#include <pulse/channelmap.h>

// PulseAudio value    -    Android value

uint32_t conversion_table_output_channel[][2] = {
    { PA_CHANNEL_POSITION_MONO,                     AUDIO_CHANNEL_OUT_MONO },
    { PA_CHANNEL_POSITION_FRONT_LEFT,               AUDIO_CHANNEL_OUT_FRONT_LEFT },
    { PA_CHANNEL_POSITION_FRONT_RIGHT,              AUDIO_CHANNEL_OUT_FRONT_RIGHT},
    { PA_CHANNEL_POSITION_FRONT_CENTER,             AUDIO_CHANNEL_OUT_FRONT_CENTER },
    { PA_CHANNEL_POSITION_SUBWOOFER,                AUDIO_CHANNEL_OUT_LOW_FREQUENCY },
    { PA_CHANNEL_POSITION_REAR_LEFT,                AUDIO_CHANNEL_OUT_BACK_LEFT },
    { PA_CHANNEL_POSITION_REAR_RIGHT,               AUDIO_CHANNEL_OUT_BACK_RIGHT },
    { PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,     AUDIO_CHANNEL_OUT_FRONT_LEFT_OF_CENTER },
    { PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,    AUDIO_CHANNEL_OUT_FRONT_RIGHT_OF_CENTER },
    { PA_CHANNEL_POSITION_REAR_CENTER,              AUDIO_CHANNEL_OUT_BACK_CENTER },
    { PA_CHANNEL_POSITION_SIDE_LEFT,                AUDIO_CHANNEL_OUT_SIDE_LEFT },
    { PA_CHANNEL_POSITION_SIDE_RIGHT,               AUDIO_CHANNEL_OUT_SIDE_RIGHT },
    { PA_CHANNEL_POSITION_TOP_CENTER,               AUDIO_CHANNEL_OUT_TOP_CENTER },
    { PA_CHANNEL_POSITION_TOP_FRONT_LEFT,           AUDIO_CHANNEL_OUT_TOP_FRONT_LEFT },
    { PA_CHANNEL_POSITION_TOP_FRONT_CENTER,         AUDIO_CHANNEL_OUT_TOP_FRONT_CENTER },
    { PA_CHANNEL_POSITION_TOP_FRONT_RIGHT,          AUDIO_CHANNEL_OUT_TOP_FRONT_RIGHT },
    { PA_CHANNEL_POSITION_TOP_REAR_LEFT,            AUDIO_CHANNEL_OUT_TOP_BACK_LEFT },
    { PA_CHANNEL_POSITION_TOP_REAR_CENTER,          AUDIO_CHANNEL_OUT_TOP_BACK_CENTER },
    { PA_CHANNEL_POSITION_TOP_REAR_RIGHT,           AUDIO_CHANNEL_OUT_TOP_BACK_RIGHT }
};

uint32_t conversion_table_input_channel[][2] = {
    { PA_CHANNEL_POSITION_MONO,                     AUDIO_CHANNEL_IN_MONO },
    { PA_CHANNEL_POSITION_FRONT_LEFT,               AUDIO_CHANNEL_IN_LEFT },
    { PA_CHANNEL_POSITION_FRONT_RIGHT,              AUDIO_CHANNEL_IN_RIGHT},
    { PA_CHANNEL_POSITION_FRONT_CENTER,             AUDIO_CHANNEL_IN_FRONT },
    { PA_CHANNEL_POSITION_REAR_CENTER,              AUDIO_CHANNEL_IN_BACK },
    /* Following are missing suitable counterparts on PulseAudio side. */
    { AUDIO_CHANNEL_IN_LEFT_PROCESSED,              AUDIO_CHANNEL_IN_LEFT_PROCESSED },
    { AUDIO_CHANNEL_IN_RIGHT_PROCESSED,             AUDIO_CHANNEL_IN_RIGHT_PROCESSED },
    { AUDIO_CHANNEL_IN_FRONT_PROCESSED,             AUDIO_CHANNEL_IN_FRONT_PROCESSED },
    { AUDIO_CHANNEL_IN_BACK_PROCESSED,              AUDIO_CHANNEL_IN_BACK_PROCESSED },
    { AUDIO_CHANNEL_IN_PRESSURE,                    AUDIO_CHANNEL_IN_PRESSURE },
    { AUDIO_CHANNEL_IN_X_AXIS,                      AUDIO_CHANNEL_IN_X_AXIS },
    { AUDIO_CHANNEL_IN_Y_AXIS,                      AUDIO_CHANNEL_IN_Y_AXIS },
    { AUDIO_CHANNEL_IN_Z_AXIS,                      AUDIO_CHANNEL_IN_Z_AXIS },
    { AUDIO_CHANNEL_IN_VOICE_UPLINK,                AUDIO_CHANNEL_IN_VOICE_UPLINK },
    { AUDIO_CHANNEL_IN_VOICE_DNLINK,                AUDIO_CHANNEL_IN_VOICE_DNLINK }
};

uint32_t conversion_table_format[][2] = {
    { PA_SAMPLE_U8,             AUDIO_FORMAT_PCM_8_BIT },
    { PA_SAMPLE_S16LE,          AUDIO_FORMAT_PCM_16_BIT },
    { PA_SAMPLE_S32LE,          AUDIO_FORMAT_PCM_32_BIT },
    { PA_SAMPLE_S24LE,          AUDIO_FORMAT_PCM_8_24_BIT }
};

uint32_t conversion_table_default_audio_source[][2] = {
    { AUDIO_DEVICE_IN_ALL,                          AUDIO_SOURCE_DEFAULT }
};

struct string_conversion {
    uint32_t value;
    const char *str;
};

#if defined(STRING_ENTRY) || defined(STRING_ENTRY)
#error STRING_ENTRY already defined somewhere, fix this lib.
#endif
#define STRING_ENTRY(str) { str, #str }
/* Output devices */
struct string_conversion string_conversion_table_output_device[] = {
    STRING_ENTRY(AUDIO_DEVICE_OUT_EARPIECE),
    STRING_ENTRY(AUDIO_DEVICE_OUT_SPEAKER),
    STRING_ENTRY(AUDIO_DEVICE_OUT_WIRED_HEADSET),
    STRING_ENTRY(AUDIO_DEVICE_OUT_WIRED_HEADPHONE),
    STRING_ENTRY(AUDIO_DEVICE_OUT_BLUETOOTH_SCO),
    STRING_ENTRY(AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET),
    STRING_ENTRY(AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT),
    STRING_ENTRY(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP),
    STRING_ENTRY(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES),
    STRING_ENTRY(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER),
    STRING_ENTRY(AUDIO_DEVICE_OUT_AUX_DIGITAL),
    STRING_ENTRY(AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET),
    STRING_ENTRY(AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET),
    STRING_ENTRY(AUDIO_DEVICE_OUT_USB_ACCESSORY),
    STRING_ENTRY(AUDIO_DEVICE_OUT_USB_DEVICE),
    STRING_ENTRY(AUDIO_DEVICE_OUT_FM),
    STRING_ENTRY(AUDIO_DEVICE_OUT_FM_TX),
    STRING_ENTRY(AUDIO_DEVICE_OUT_ANC_HEADSET),
    STRING_ENTRY(AUDIO_DEVICE_OUT_ANC_HEADPHONE),
    STRING_ENTRY(AUDIO_DEVICE_OUT_PROXY),
    STRING_ENTRY(AUDIO_DEVICE_OUT_ALL),
    STRING_ENTRY(AUDIO_DEVICE_OUT_ALL_A2DP),
    STRING_ENTRY(AUDIO_DEVICE_OUT_ALL_SCO),
    STRING_ENTRY(AUDIO_DEVICE_OUT_ALL_USB),
    { 0, NULL }
};

struct string_conversion string_conversion_table_output_device_fancy[] = {
    { AUDIO_DEVICE_OUT_EARPIECE,                    "output-earpiece" },
    { AUDIO_DEVICE_OUT_SPEAKER,                     "output-speaker" },
    { AUDIO_DEVICE_OUT_SPEAKER
        | AUDIO_DEVICE_OUT_WIRED_HEADPHONE,         "output-speaker+wired_headphone" },
    { AUDIO_DEVICE_OUT_WIRED_HEADSET,               "output-wired_headset" },
    { AUDIO_DEVICE_OUT_WIRED_HEADPHONE,             "output-wired_headphone" },
    { AUDIO_DEVICE_OUT_BLUETOOTH_SCO,               "output-bluetooth_sco" },
    { AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET,       "output-sco_headset" },
    { AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT,        "output-sco_carkit" },
    { AUDIO_DEVICE_OUT_BLUETOOTH_A2DP,              "output-a2dp" },
    { AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES,   "output-a2dp_headphones" },
    { AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER,      "output-a2dp_speaker" },
    { AUDIO_DEVICE_OUT_AUX_DIGITAL,                 "output-aux_digital" },
    { AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET,           "output-analog_dock_headset" },
    { AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET,           "output-digital_dock_headset" },
    { AUDIO_DEVICE_OUT_USB_ACCESSORY,               "output-usb_accessory" },
    { AUDIO_DEVICE_OUT_USB_DEVICE,                  "output-usb_device" },
    { AUDIO_DEVICE_OUT_FM,                          "output-fm" },
    { AUDIO_DEVICE_OUT_FM_TX,                       "output-fm_tx" },
    { AUDIO_DEVICE_OUT_ANC_HEADSET,                 "output-anc_headset" },
    { AUDIO_DEVICE_OUT_ANC_HEADPHONE,               "output-anc_headphone" },
    { AUDIO_DEVICE_OUT_PROXY,                       "output-proxy" },
    { 0, NULL }
};

/* Input devices */
struct string_conversion string_conversion_table_input_device[] = {
    STRING_ENTRY(AUDIO_DEVICE_IN_COMMUNICATION),
    STRING_ENTRY(AUDIO_DEVICE_IN_AMBIENT),
    STRING_ENTRY(AUDIO_DEVICE_IN_BUILTIN_MIC),
    STRING_ENTRY(AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET),
    STRING_ENTRY(AUDIO_DEVICE_IN_WIRED_HEADSET),
    STRING_ENTRY(AUDIO_DEVICE_IN_AUX_DIGITAL),
    STRING_ENTRY(AUDIO_DEVICE_IN_VOICE_CALL),
    STRING_ENTRY(AUDIO_DEVICE_IN_BACK_MIC),
    STRING_ENTRY(AUDIO_DEVICE_IN_ANC_HEADSET),
    STRING_ENTRY(AUDIO_DEVICE_IN_FM_RX),
    STRING_ENTRY(AUDIO_DEVICE_IN_FM_RX_A2DP),
    STRING_ENTRY(AUDIO_DEVICE_IN_PROXY),
    STRING_ENTRY(AUDIO_DEVICE_IN_DEFAULT),
    /* Combination entries consisting of multiple devices defined above.
     * These don't require counterpart in string_conversion_table_input_device_fancy. */
    STRING_ENTRY(AUDIO_DEVICE_IN_ALL),
    STRING_ENTRY(AUDIO_DEVICE_IN_ALL_SCO),
    { 0, NULL }
};

struct string_conversion string_conversion_table_input_device_fancy[] = {
    { AUDIO_DEVICE_IN_COMMUNICATION,            "input-communication" },
    { AUDIO_DEVICE_IN_AMBIENT,                  "input-ambient" },
    { AUDIO_DEVICE_IN_BUILTIN_MIC,              "input-builtin_mic" },
    { AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET,    "input-bluetooth_sco_headset" },
    { AUDIO_DEVICE_IN_WIRED_HEADSET,            "input-wired_headset" },
    { AUDIO_DEVICE_IN_AUX_DIGITAL,              "input-aux_digital" },
    { AUDIO_DEVICE_IN_VOICE_CALL,               "input-voice_call" },
    { AUDIO_DEVICE_IN_BACK_MIC,                 "input-back_mic" },
    { AUDIO_DEVICE_IN_ANC_HEADSET,              "input-anc_headset" },
    { AUDIO_DEVICE_IN_FM_RX,                    "input-fm_rx" },
    { AUDIO_DEVICE_IN_FM_RX_A2DP,               "input-fm_rx_a2dp" },
    { AUDIO_DEVICE_IN_PROXY,                    "input-in_proxy" },
    { AUDIO_DEVICE_IN_DEFAULT,                  "input-default" },
    { 0, NULL }
};

struct string_conversion string_conversion_table_audio_source_fancy[] = {
    { AUDIO_SOURCE_DEFAULT,                         "default" },
    { AUDIO_SOURCE_MIC,                             "mic" },
    { AUDIO_SOURCE_VOICE_UPLINK,                    "voice uplink" },
    { AUDIO_SOURCE_VOICE_DOWNLINK,                  "voice downlink" },
    { AUDIO_SOURCE_VOICE_CALL,                      "voice call" },
    { AUDIO_SOURCE_CAMCORDER,                       "camcorder" },
    { AUDIO_SOURCE_VOICE_RECOGNITION,               "voice recognition" },
    { AUDIO_SOURCE_VOICE_COMMUNICATION,             "voice communication" },
    { AUDIO_SOURCE_FM_RX,                           "fm rx" },
    { AUDIO_SOURCE_FM_RX_A2DP,                      "fm rx a2dp" },
    { (uint32_t)-1, NULL }
};

/* Flags */
struct string_conversion string_conversion_table_output_flag[] = {
    STRING_ENTRY(AUDIO_OUTPUT_FLAG_NONE),
    STRING_ENTRY(AUDIO_OUTPUT_FLAG_DIRECT),
    STRING_ENTRY(AUDIO_OUTPUT_FLAG_PRIMARY),
    STRING_ENTRY(AUDIO_OUTPUT_FLAG_FAST),
    STRING_ENTRY(AUDIO_OUTPUT_FLAG_DEEP_BUFFER),
    /* Qualcomm flags */
    STRING_ENTRY(AUDIO_OUTPUT_FLAG_LPA),
    STRING_ENTRY(AUDIO_OUTPUT_FLAG_TUNNEL),
    STRING_ENTRY(AUDIO_OUTPUT_FLAG_VOIP_RX),
    { 0, NULL }
};

/* Channels */
struct string_conversion string_conversion_table_output_channels[] = {
    STRING_ENTRY(AUDIO_CHANNEL_OUT_FRONT_LEFT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_FRONT_RIGHT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_FRONT_CENTER),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_LOW_FREQUENCY),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_BACK_LEFT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_BACK_RIGHT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_FRONT_LEFT_OF_CENTER),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_FRONT_RIGHT_OF_CENTER),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_BACK_CENTER),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_SIDE_LEFT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_SIDE_RIGHT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_TOP_CENTER),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_TOP_FRONT_LEFT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_TOP_FRONT_CENTER),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_TOP_FRONT_RIGHT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_TOP_BACK_LEFT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_TOP_BACK_CENTER),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_TOP_BACK_RIGHT),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_MONO),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_STEREO),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_QUAD),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_SURROUND),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_7POINT1),
    STRING_ENTRY(AUDIO_CHANNEL_OUT_ALL),
    { 0, NULL }
};
struct string_conversion string_conversion_table_input_channels[] = {
    STRING_ENTRY(AUDIO_CHANNEL_IN_LEFT),
    STRING_ENTRY(AUDIO_CHANNEL_IN_RIGHT),
    STRING_ENTRY(AUDIO_CHANNEL_IN_FRONT),
    STRING_ENTRY(AUDIO_CHANNEL_IN_BACK),
    STRING_ENTRY(AUDIO_CHANNEL_IN_LEFT_PROCESSED),
    STRING_ENTRY(AUDIO_CHANNEL_IN_RIGHT_PROCESSED),
    STRING_ENTRY(AUDIO_CHANNEL_IN_FRONT_PROCESSED),
    STRING_ENTRY(AUDIO_CHANNEL_IN_BACK_PROCESSED),
    STRING_ENTRY(AUDIO_CHANNEL_IN_PRESSURE),
    STRING_ENTRY(AUDIO_CHANNEL_IN_X_AXIS),
    STRING_ENTRY(AUDIO_CHANNEL_IN_Y_AXIS),
    STRING_ENTRY(AUDIO_CHANNEL_IN_Z_AXIS),
    STRING_ENTRY(AUDIO_CHANNEL_IN_VOICE_UPLINK),
    STRING_ENTRY(AUDIO_CHANNEL_IN_VOICE_DNLINK),
    STRING_ENTRY(AUDIO_CHANNEL_IN_MONO),
    STRING_ENTRY(AUDIO_CHANNEL_IN_STEREO),
    STRING_ENTRY(AUDIO_CHANNEL_IN_5POINT1),
    STRING_ENTRY(AUDIO_CHANNEL_IN_VOICE_UPLINK_MONO),
    STRING_ENTRY(AUDIO_CHANNEL_IN_VOICE_DNLINK_MONO),
    STRING_ENTRY(AUDIO_CHANNEL_IN_VOICE_CALL_MONO),
    STRING_ENTRY(AUDIO_CHANNEL_IN_ALL),
    { 0, NULL }
};

/* Formats */
struct string_conversion string_conversion_table_format[] = {
    STRING_ENTRY(AUDIO_FORMAT_DEFAULT),
    STRING_ENTRY(AUDIO_FORMAT_PCM),
    STRING_ENTRY(AUDIO_FORMAT_MP3),
    STRING_ENTRY(AUDIO_FORMAT_AMR_NB),
    STRING_ENTRY(AUDIO_FORMAT_AMR_WB),
    STRING_ENTRY(AUDIO_FORMAT_AAC),
    STRING_ENTRY(AUDIO_FORMAT_HE_AAC_V1),
    STRING_ENTRY(AUDIO_FORMAT_HE_AAC_V2),
    STRING_ENTRY(AUDIO_FORMAT_VORBIS),
    STRING_ENTRY(AUDIO_FORMAT_EVRC),
    STRING_ENTRY(AUDIO_FORMAT_QCELP),
    STRING_ENTRY(AUDIO_FORMAT_AC3),
    STRING_ENTRY(AUDIO_FORMAT_AC3_PLUS),
    STRING_ENTRY(AUDIO_FORMAT_DTS),
    STRING_ENTRY(AUDIO_FORMAT_WMA),
    STRING_ENTRY(AUDIO_FORMAT_WMA_PRO),
    STRING_ENTRY(AUDIO_FORMAT_AAC_ADIF),
    STRING_ENTRY(AUDIO_FORMAT_EVRCB),
    STRING_ENTRY(AUDIO_FORMAT_EVRCWB),
    STRING_ENTRY(AUDIO_FORMAT_EAC3),
    STRING_ENTRY(AUDIO_FORMAT_DTS_LBR),
    STRING_ENTRY(AUDIO_FORMAT_AMR_WB_PLUS),
    /* Currently we support only PCM formats, but keep all formats
     * here so audio_policy.conf can be parsed. */
    STRING_ENTRY(AUDIO_FORMAT_PCM_16_BIT),
    STRING_ENTRY(AUDIO_FORMAT_PCM_8_BIT),
    STRING_ENTRY(AUDIO_FORMAT_PCM_32_BIT),
    STRING_ENTRY(AUDIO_FORMAT_PCM_8_24_BIT),
    { 0, NULL }
};
#undef STRING_ENTRY

#endif
