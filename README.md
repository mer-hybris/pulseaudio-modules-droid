PulseAudio Droid modules
========================

For adapdations for Android versions 4 to 10,
see [pulseaudio-modules-droid-jb2q](https://github.com/mer-hybris/pulseaudio-modules-droid-jb2q)

Building of droid modules is split to two packages
* **common** (and **common-devel**) which contains shared library code for use in
  PulseAudio modules in this package and for inclusion in other projects
* **droid** with actual PulseAudio modules

Linking to libdroid is **not encouraged**, usually only HAL functions are needed
which can be accessed using the pulsecore shared API (see below).

Supported Android versions:

* 11.x

Headers for defining devices and strings for different droid versions are in
src/common/droid-util-audio.h.

When new devices with relevant new enums appear, add enum check to configure.ac.
CC_CHECK_DROID_ENUM macro will create macros HAVE_ENUM_FOO, STRING_ENTRY_IF_FOO
and FANCY_ENTRY_IF_FOO if enum FOO exists in HAL audio.h.

For example:

    # configure.ac:
    CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_IP])
    CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_OTHER_NEW])

    # and then in droid-util-audio.h add macros to proper tables:
    /* string_conversion_table_output_device[] */
    STRING_ENTRY_IF_OUT_IP
    STRING_ENTRY_IF_OUT_OTHER_NEW

    /* string_conversion_table_output_device_fancy[] */
    FANCY_ENTRY_IF_OUT_IP("output-ip")
    FANCY_ENTRY_IF_OUT_OTHER_NEW("output-other_new")

In addition to the above macros there are also now defines
HAVE_ENUM_AUDIO_DEVICE_OUT_IP and HAVE_ENUM_AUDIO_DEVICE_OUT_OTHER_NEW.

The purpose of droid-modules is to "replace AudioFlinger". Many hardware
adaptations use ALSA as the kernel interface, but there is no saying that
someday vendor would create and use something proprietary or otherwise
different from ALSA. Also the ALSA implementation in droid devices may contain
funny ways to achieve things (notable example is voicecall) which might be
difficult to do if interfacing directly with ALSA to replace AudioFlinger.
Also using ALSA directly would mean that the whole HAL adaptation would need to
be ported for each new device adaptation. With droid-modules this is much more
simpler, with somewhat stable HAL (HALv3 as of now, also different vendors add
their own incompatible extensions) API. In best scenarios using droid-modules
with new device is just compiling against target.

Components
==========

common
------

The common part of PulseAudio Droid modules contains library for handling
most operations towards audio HAL.

### Audio policy configuration parsing

Configuration parser reads audio policy xml files.

### Configuration files

If the configuration is in non-default location for some reason "config"
module argument can be used to point to the configuration file location.

By default files are tried in following order,

    /odm/etc/audio_policy_configuration.xml
    /vendor/etc/audio/audio_policy_configuration.xml
    /vendor/etc/audio_policy_configuration.xml
    /system/etc/audio_policy_configuration.xml

module-droid-card
-----------------

Ideally only module-droid-card is loaded and then droid-card loads
configuration, creates profiles and loads sinks and sources based on the
selected profile.

default profile
---------------

When module-droid-card is loaded with default arguments, droid-card will
create a default profile (called unsurprisingly "default"). The default
profile will merge supported output and input streams to one profile,
to allow use of possible low latency or deep buffer outputs.

virtual profiles
----------------

In addition to aforementioned card profile, droid-card creates some additional
virtual profiles. These virtual profiles are used when enabling voicecall
routings etc. When virtual profile is enabled, possible sinks and sources
previously active profile had are not removed.

As an illustration, following command line sequence enables voicecall mode and
routes audio to internal handsfree (ihf - "handsfree speaker"):

    pactl set-card-profile droid_card.primary voicecall
    pactl set-sink-port sink.primary output-parking
    pactl set-sink-port sink.primary output-speaker

After this, when there is an active voicecall (created by ofono for example),
voice audio starts to flow between modem and audio chip.

To disable voicecall and return to media audio:

    pactl set-card-profile droid_card.primary default
    pactl set-sink-port sink.primary output-parking
    pactl set-sink-port sink.primary output-speaker

With this example sequence sinks and sources are the ones from default
card profile, and they are maintained for the whole duration of the voicecall
and after.

This sequence follows the droid HAL idea that when changing audio mode the mode
change is done when next routing change happens. output-parking and
input-parking ports are just convenience for PulseAudio, where setting already
active port is a no-op (output/input-parking doesn't do any real routing
changes).

Current virtual profiles are:
* voicecall
* voicecall-record
* communication
* ringtone

Communication profile is used for VoIP-like applications, to enable some
voicecall related algorithms without being in voicecall. Ringtone profile
should be used when ringtone is playing, to again enable possible loudness
related optimizations etc. Voicecall-record profile can be enabled when
voicecall profile is active.

If mix port with flag AUDIO_OUTPUT_FLAG_VOIP_RX exists when communication
virtual profile is enabled additional droid-sink is created with the config
defined in the mix port. Voip audio should then be played to this new sink.

module-droid-sink and module-droid-source
-----------------------------------------

Normally user should not need to load droid-sink or droid-source modules by
hand, but droid-card loads appropriate modules based on the active card
profile.

Changing output routing is as simple as

    pactl set-sink-port sink.primary output-wired_headphone

Sinks or sources do not track possible headphone/other wired accessory
plugging, but this needs to be handled elsewhere and then that other entity
needs to control sinks and sources. (For example in SailfishOS this entity is
OHM with accessory-plugin and pulseaudio-policy-enforcement module for
actually making the port switching)

Droid source automatic reconfiguration
--------------------------------------

As droid HAL makes assumptions on (input) routing based on what the parameters
for the stream are (device, sample rate, channels, format, etc.) normal
PulseAudio sources are a bit inflexible as only sample rate can change after
source creation and even then there are restrictions based on alternative
sample rate value.

To overcome this and to allow some more variables affecting the stream being
passed to the input stream droid source is modified to reconfigure itself
with the source-output that connects to it. This means, that just looking at
inactive source from "pactl list" listing doesn't tell the whole story.

Droid source is always reconfigured with the *last* source-output that
connects to it, possibly already connected source-outputs will continue
to read from the source but through resampler.

For example,

1) source-output 44100Hz, stereo connects (so1)
    1) source is configured with 44100Hz, stereo
    2) so1 connects to the source without resampler
2) source-output 16000Hz, mono connects (so2)
    1) so1 is detached from the source
    2) source is configured with 16000Hz, mono
    3) so2 connects to the source without resampler
    4) resampler is created for so1, 16000Hz, mono -> 44100Hz stereo
    5) so1 is re-attached to the source through resampler
3) source-output 16000Hz, mono connects (so3)
    1) so1 and so2 are detached from the source
    2) so3 connects to the source without resampler
    3) so1 is re-attached to the source through resampler
    4) so2 is attached to the source

Classifying sinks and sources
-----------------------------

Certain property values are set to all active sinks and sources based on their
functionality to ease device classification.

Currently following properties are set:

* For droid sinks
    * droid.output.primary
    * droid.output.low_latency
    * droid.output.media_latency
    * droid.output.offload
    * droid.output.voip
* For droid sources
    * droid.input.builtin
    * droid.input.external

If the property is set and with value "true", the sink or source should be
used for the property type. If the property is not defined or contains
value "false" it shouldn't be used for the property type.

For example, we might have sink.primary and sink.low_latency with following
properties:

* sink.primary
    * droid.output.primary "true"
    * droid.output.media_latency "true"
* sink.low_latency
    * droid.output.low_latency "true"

There also may be just one sink, with all the properties defined as "true"
and so on.

Right now there exists only one source (input device) which will always have
both properties as true.

Options
-------

There are some adaptations that require hacks to get things working. These
hacks can be enabled or disabled with module argument "options". Some options
are enabled by default with some adaptations etc. There are also some more
generic options.

Currently there are following options:

* input_atoi
    * Enabled by default with Android versions 5 and up.
    * Due to how atoi works in bionic vs libc we need to pass the input
      route a bit funny. If input routing doesn't work switch this on or off.
* close_input
    * Enabled by default.
    * Close input stream when not in use instead of suspending the stream.
      Cannot be changed when multiple inputs are merged to single source.
* unload_no_close
    * Disabled by default.
    * Don't call audio_hw_device_close() for the hw module when unloading.
      Mostly useful for tracking module unload issues.
* hw_volume
    * Enabled by default.
    * Some broken implementations are incorrectly probed for supporting hw
      volume control. This is manifested by always full volume with volume
      control not affecting volume level. To fix this disable this option.
* realcall
    * Disabled by default.
    * Some vendors apply custom realcall parameter to HAL device when
      doing voicecall routing. If there is no voicecall audio you can
      try enabling this option so that the realcall parameter is applied
      when switching to voicecall profile.
* unload_call_exit
    * Disabled by default.
    * Some HAL module implementations get stuck in mutex or segfault when
      trying to unload the module. To avoid confusing segfaults call
      exit(0) instead of calling unload for the module.
* output_fast
    * Enabled by default.
    * Create separate sink if AUDIO_OUTPUT_FLAG_FAST is found. If this sink
      is misbehaving try disabling this option.
* output_deep_buffer
    * Enabled by default.
    * Create separate sink if AUDIO_OUTPUT_FLAG_DEEP_BUFFER is found. If
      this sink is misbehaving try disabling this option.
* audio_cal_wait
    * Disabled by default.
    * Certain devices do audio calibration during hw module open and
      writing audio too early will break the calibration. In these cases
      this option can be enabled and 10 seconds of sleep is added after
      opening hw module.
* speaker_before_voice
    * Disabled by default.
    * Set route to speaker before changing audio mode to AUDIO_MODE_IN_CALL.
      Some devices don't get routing right if the route is something else
      (like AUDIO_DEVICE_OUT_WIRED_HEADSET) before calling set_mode().
      If routing is wrong when call starts with wired accessory connected
      try enabling this option.
* output_voip_rx
    * Enabled by default.
    * When audio configuration has AUDIO_OUTPUT_FLAG_VOIP_RX special voip
      sink is created when AUDIO_MODE_IN_COMMUNICATION is active and the
      sink is classified as droid.output.voip. If this is not desired then
      by disabling this option the voip sink is not classified but is still
      created normally.
* record_voice_16k
    * Disabled by default.
    * When enabled voice call recording source is forced to sample rate
      of 16kHz.

Options can be enabled or disabled normally as module arguments, for example:

    load-module module-droid-card hw_volume=false record_voice_16k=true

Volume control during voicecall
-------------------------------

When voicecall virtual profile is enabled, active droid-sink is internally
switched to voicecall volume control mode. What this means is changing the sink
volume or volume of normal streams connected to the sink do not change active
voicecall volume. Special stream is needed to control the voicecall volume
level. By default this stream is identified by stream property media.role,
with value "phone". This can be changed by providing module arguments
voice_property_key and voice_property_value to module-droid-card.

Usually droid HAL has 6 volume levels for voicecall.

Temporary sink audio routing
----------------------------

It is possible to add temporary route to sink audio routing with specific
stream property. When stream with property key
droid.device.additional-route connects to droid-sink, this extra route is set
(if possible) as the enabled route for the duration of the stream.

For example, if droid-sink has active port output-wired_headphone:

    paplay --property=droid.device.additional-route=AUDIO_DEVICE_OUT_SPEAKER a.wav

As long as the new stream is connected to droid-sink, output routing is
SPEAKER.

HAL API
-------

If there is need to call HAL directly from other modules it can be done with
function pointer API stored in PulseAudio shared map.

Once the function pointers are acquired when called they will work the same
way as defined in Android audio.h. For example:

    void   *handle;
    int   (*set_parameters)(void *handle, const char *key_value_pairs);
    char* (*get_parameters)(void *handle, const char *keys);

    handle = pa_shared_get(core, "droid.handle.v1");
    set_parameters = pa_shared_get(core, "droid.set_parameters.v1");
    get_parameters = pa_shared_get(core, "droid.get_parameters.v1");

    set_parameters(handle, "route=2;");
    char *value = get_parameters(handle, "connected");
