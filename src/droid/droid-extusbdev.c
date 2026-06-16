/***
  Copyright (C) 2026 Jolla Mobile Ltd
  Author(s): Enni Hämäläinen <enni.hamalainen@jolla.com>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/types.h>

#include <pulsecore/device-port.h>
#include <pulsecore/card.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>

#include <alsa/asoundlib.h>
#include <libudev.h>

#include <droid/droid-util.h>

#include "droid-extusbdev.h"

#define MAX_SAMPLE_RATES (17)

struct pa_droid_extusbdev {
    pa_card *card;
    pa_droid_hw_module *hw_module;

    struct udev* udev;
    struct udev_monitor *monitor;
    pa_io_event *udev_io;

    pa_hashmap *cards;
};

typedef struct {
    bool playback;
    bool capture;
    int card;
    int device;
} audio_device_caps_t;

typedef struct {
    char *devpath;
    audio_device_caps_t caps;
} usb_card_t;

static void handle_usb_device_added(pa_droid_extusbdev *u, struct udev_device *dev);

static const char *usb_headset_ports[] = {
    "output-usb_headset",
    "input-usb_headset",
    NULL
};

static const char *usb_device_ports[] = {
    "output-usb_device",
    NULL
};

static const char *usb_mic_ports[] = {
    "input-usb_device",
    NULL
};

static int get_card_id(const char *path)
{
    const char *last;
    int id = -1;

    if (!path)
        return id;

    if (!(last = strrchr(path, '/')))
        return id;

    if (strlen(last) < 6 || !pa_startswith(last, "/card"))
        return id;

    pa_atoi(last + 5, &id);

    return id;
}

static const char *card_type_to_string(audio_device_caps_t *caps) {
    if (caps->playback && caps->capture)
        return "headset";

    if (caps->playback || caps->capture)
        return "device";

    return "invalid";
}

static void inspect_card(pa_droid_extusbdev *u, int card, audio_device_caps_t *devcaps)
{
    snd_ctl_t *ctl;
    char ctlname[32];

    memset(devcaps, 0, sizeof(audio_device_caps_t));

    snprintf(ctlname, sizeof(ctlname), "hw:%d", card);

    if (snd_ctl_open(&ctl, ctlname, 0) < 0)
        return;

    int dev = -1;

    /* Lookup devices until one with both playback and capture is
     * found. If just one is available we iterate through all. */
    while (!devcaps->playback && !devcaps->capture) {
        if (snd_ctl_pcm_next_device(ctl, &dev) < 0)
            break;

        if (dev < 0)
            break;

        char pcmname[64];
        snd_pcm_t *pcm;

        pa_snprintf(pcmname, sizeof(pcmname), "hw:%d,%d", card, dev);

        if (snd_pcm_open(&pcm,
                         pcmname,
                         SND_PCM_STREAM_PLAYBACK,
                         SND_PCM_NONBLOCK) >= 0) {
            devcaps->playback = true;
            snd_pcm_close(pcm);
        }

        if (snd_pcm_open(&pcm,
                         pcmname,
                         SND_PCM_STREAM_CAPTURE,
                         SND_PCM_NONBLOCK) >= 0) {
            devcaps->capture = true;
            snd_pcm_close(pcm);
        }

        devcaps->card = card;
        devcaps->device = dev;
    }

    snd_ctl_close(ctl);
}

static void enumerate_existing(pa_droid_extusbdev *u, struct udev *udev)
{
    pa_log_debug("Check existing USB devices");

    struct udev_enumerate *e = udev_enumerate_new(udev);

    udev_enumerate_add_match_subsystem(e, "sound");
    udev_enumerate_scan_devices(e);

    struct udev_list_entry *devices = udev_enumerate_get_list_entry(e);
    struct udev_list_entry *entry;

    udev_list_entry_foreach(entry, devices) {
        const char *path = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, path);

        if (!dev)
            continue;

        struct udev_device *parent = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");

        if (parent)
            handle_usb_device_added(u, dev);

        udev_device_unref(dev);
    }

    udev_enumerate_unref(e);
}

static void connect_card(pa_droid_extusbdev *u, usb_card_t *card, bool connect) {
    const char **ports;

    pa_assert(u);
    pa_assert(card);

    if (card->caps.playback && card->caps.capture) {
        ports = usb_headset_ports;
    } else if (card->caps.playback) {
        ports = usb_device_ports;
    } else if (card->caps.capture) {
        ports = usb_mic_ports;
    } else {
        pa_log("No devices to %sconnect.", connect ? "" : "dis");
        return;
    }

    pa_log_info("USB %s (hw:%d,%d) %sconnected", card_type_to_string(&card->caps),
                                                 card->caps.card, 
                                                 card->caps.device,
                                                 connect ? "" : "dis");

    for (int i = 0; ports[i]; i++) {
        pa_device_port *p = pa_hashmap_get(u->card->ports, ports[i]);
        if (p) {
            if (connect)
                pa_droid_device_port_set_usb_connected(p, card->caps.card, card->caps.device);
            else
                pa_droid_device_port_set_usb_disconnected(p);
            pa_device_port_set_available(p, connect ? PA_AVAILABLE_YES : PA_AVAILABLE_NO);
        }
    }
}

static void handle_usb_device_added(pa_droid_extusbdev *u, struct udev_device *dev) {
    int card_id = -1;
    audio_device_caps_t caps = {0};

    if (pa_hashmap_get(u->cards, udev_device_get_devpath(dev)))
        return;

    card_id = get_card_id(udev_device_get_devpath(dev));

    if (card_id >= 0)
        inspect_card(u, card_id, &caps);

    if (!caps.playback && !caps.capture)
        return;

    usb_card_t *card = pa_xnew0(usb_card_t, 1);
    card->devpath = pa_xstrdup(udev_device_get_devpath(dev));
    card->caps = caps;

    pa_hashmap_put(u->cards, card->devpath, card);

    pa_log_info("USB %s added, %s", card_type_to_string(&card->caps), udev_device_get_devpath(dev));

    connect_card(u, card, true);
}

static void handle_usb_device_removed(pa_droid_extusbdev *u, struct udev_device *dev) {
    usb_card_t *card;

    if (!(card = pa_hashmap_get(u->cards, udev_device_get_devpath(dev))))
        return;

    pa_log_info("USB %s removed, %s", card_type_to_string(&card->caps), card->devpath);

    connect_card(u, card, false);

    pa_hashmap_remove_and_free(u->cards, udev_device_get_devpath(dev));
}

static void monitor_cb(pa_mainloop_api *a,
                       pa_io_event *e,
                       int fd,
                       pa_io_event_flags_t events,
                       void *userdata) {

    struct pa_droid_extusbdev *u = userdata;
    struct udev_device *dev = NULL;

    pa_assert(a);

    if (!(dev = udev_monitor_receive_device(u->monitor))) {
        pa_log("Failed to get udev device object from monitor.");
        return;
    }

    if (udev_device_get_property_value(dev, "PULSE_IGNORE")) {
        pa_log_debug("Ignoring %s, because marked so.", udev_device_get_devpath(dev));
        return;
    }

    const char *ff;
    if ((ff = udev_device_get_property_value(dev, "SOUND_CLASS")) &&
        pa_streq(ff, "modem")) {
        pa_log_debug("Ignoring %s, because it is a modem.", udev_device_get_devpath(dev));
        return;
    }

    const char *action = udev_device_get_action(dev);

    if (pa_safe_streq(action, "remove")) {
        handle_usb_device_removed(u, dev);
    } else if (pa_safe_streq(action, "change") || udev_device_get_property_value(dev, "SOUND_INITIALIZED")) {
        struct udev_device *parent = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");

        if (!parent) {
            /* Not USB audio device. */
            goto done;
        }

        handle_usb_device_added(u, dev);
    }

done:
    udev_device_unref(dev);
}

static void usb_card_free(usb_card_t *card) {
    pa_xfree(card->devpath);
    pa_xfree(card);
}

pa_droid_extusbdev *pa_droid_extusbdev_new(pa_droid_hw_module *hw, pa_card *c) {
    pa_droid_extusbdev *u = pa_xnew0(pa_droid_extusbdev, 1);

    u->card = c;
    u->hw_module = hw;

    int fd;

    if (!(u->udev = udev_new())) {
        pa_log("Failed to initialize udev library.");
        goto fail;
    }

    u->cards = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                   pa_idxset_string_compare_func,
                                   NULL,
                                   (pa_free_cb_t) usb_card_free);

    enumerate_existing(u, u->udev);

    if (!(u->monitor = udev_monitor_new_from_netlink(u->udev, "udev"))) {
        pa_log("Failed to initialize monitor.");
        goto fail;
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(u->monitor, "sound", NULL) < 0) {
        pa_log("Failed to subscribe to sound devices.");
        goto fail;
    }

    errno = 0;
    if (udev_monitor_enable_receiving(u->monitor) < 0) {
        pa_log("Failed to enable udev monitor: %s", pa_cstrerror(errno));
        goto fail;
    }

    if ((fd = udev_monitor_get_fd(u->monitor)) < 0) {
        pa_log("Failed to get udev monitor fd.");
        goto fail;
    }

    pa_assert_se(u->udev_io = u->card->core->mainloop->io_new(u->card->core->mainloop, fd, PA_IO_EVENT_INPUT, monitor_cb, u));

fail:
    return u;
}

void pa_droid_extusbdev_free(pa_droid_extusbdev *u) {
    if (!u)
        return;

    pa_xfree(u);
}

