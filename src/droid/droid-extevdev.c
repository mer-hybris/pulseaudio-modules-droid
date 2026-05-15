/***
  This file is part of PulseAudio.

  Copyright (C) 2019 UBports foundation.
  Author(s): Ratchanan Srirattanamet <ratchanan@ubports.com>

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
#include <linux/input.h>
#include <linux/types.h>

#include <pulsecore/device-port.h>
#include <pulsecore/card.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>

#include "droid-extevdev.h"

#define DEV_INPUT_EVENT "/dev/input"

struct pa_droid_extevdev {
    pa_card *card;
    int fd;
    int fd_type;
    pa_io_event *event;

    /* Switch values */
    int sw_headphone_insert;
    int sw_microphone_insert;
    int sw_lineout_insert;
};

#define BITS_PER_U32 (sizeof(uint32_t) * 8)
#define NBITS(x) ((((x) - 1) / BITS_PER_U32) + 1)

#define TEST_BIT(n, bits) (((bits)[(n) / BITS_PER_U32]) &       \
                           (0x1 << ((n) % BITS_PER_U32)))

/* Put the port we want to be active (for each direction) later in the list.
 * module-switch-on-port-available will switch to the available port as it
 * become available, so the last port available will stay active. */

static const char *headphone_ports[] = {
    "output-wired_headphone",
};

static const char *headset_ports[] = {
    "output-wired_headset",
    "input-wired_headset",
};

static void notify_ports(pa_droid_extevdev *u) {
    unsigned int i;

    pa_available_t has_headphone =
        ((u->sw_headphone_insert || u->sw_lineout_insert)
        && !u->sw_microphone_insert) ? PA_AVAILABLE_YES : PA_AVAILABLE_NO;

    for (i = 0; i < PA_ELEMENTSOF(headphone_ports); i++) {
        pa_device_port *p = pa_hashmap_get(u->card->ports, headphone_ports[i]);
        if (p)
            pa_device_port_set_available(p, has_headphone);
    }

    pa_available_t has_headset =
        ((u->sw_headphone_insert || u->sw_lineout_insert)
        && u->sw_microphone_insert) ? PA_AVAILABLE_YES : PA_AVAILABLE_NO;

    for (i = 0; i < PA_ELEMENTSOF(headset_ports); i++) {
        pa_device_port *p = pa_hashmap_get(u->card->ports, headset_ports[i]);
        if (p)
            pa_device_port_set_available(p, has_headset);
    }
}

static void event_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
    pa_droid_extevdev *u = userdata;

    if (events & PA_IO_EVENT_HANGUP) {
        pa_log("input device closed unexpectedly");
        return;
    }

    if (events & PA_IO_EVENT_ERROR) {
        pa_log("input device had an I/O error");
        return;
    }

    if (events & PA_IO_EVENT_INPUT) {
        struct input_event ev;

        if (pa_loop_read(u->fd, &ev, sizeof(ev), &u->fd_type) <= 0) {
            pa_log("Failed to read from event device: %s", pa_cstrerror(errno));
            return;
        }

        if (ev.type != EV_SW && ev.type != EV_SYN) {
            pa_log("Ignoring input event type %d", ev.type);
            return;
        }

        if (ev.type == EV_SYN) {
            notify_ports(u);
            return;
        }

        #define LOG_SW(x) pa_log_debug(#x " %d", u->x)

        switch (ev.code) {
            case SW_HEADPHONE_INSERT:   u->sw_headphone_insert  = ev.value; LOG_SW(sw_headphone_insert);    break;
            case SW_MICROPHONE_INSERT:  u->sw_microphone_insert = ev.value; LOG_SW(sw_microphone_insert);   break;
            case SW_LINEOUT_INSERT:     u->sw_lineout_insert    = ev.value; LOG_SW(sw_lineout_insert);      break;
        }

        #undef LOG_SW
    }
    return;
}

static void query_initial_state(pa_droid_extevdev *u)
{
    uint32_t bitmask[NBITS(KEY_MAX)];

    memset(bitmask, 0, sizeof(bitmask));

    if (ioctl(u->fd, EVIOCGSW(sizeof(bitmask)), &bitmask) < 0) {
        pa_log("failed to query current connected state: %s",
               pa_cstrerror(errno));
        return;
    }

    u->sw_headphone_insert      = TEST_BIT(SW_HEADPHONE_INSERT    , bitmask);
    u->sw_microphone_insert     = TEST_BIT(SW_MICROPHONE_INSERT   , bitmask);
    u->sw_lineout_insert        = TEST_BIT(SW_LINEOUT_INSERT      , bitmask);

    pa_log_debug("headphone is %sconnected" , u->sw_headphone_insert  ? "" : "dis");
    pa_log_debug("microphone is %sconnected", u->sw_microphone_insert ? "" : "dis");
    pa_log_debug("lineout is %sconnected"   , u->sw_lineout_insert    ? "" : "dis");

    notify_ports(u);
}

static bool check_device(int fd)
{
    unsigned long evbit[NBITS(EV_MAX)];
    unsigned long swbit[NBITS(SW_MAX)];

    memset(&evbit, 0, sizeof(evbit));
    memset(&swbit, 0, sizeof(swbit));

    /* Query supported event types */
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
        pa_log_warn("Could not get features: %s", pa_cstrerror(errno));
        return false;
    }

    if (!TEST_BIT(EV_SW, evbit)) {
        pa_log_debug("Device does NOT support EV_SW");
        return false;
    }

    /* Query supported switch codes */
    if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(swbit)), swbit) < 0) {
        pa_log_warn("Could not get switch codes: %s", pa_cstrerror(errno));
        return false;
    }

    if (!TEST_BIT(SW_HEADPHONE_INSERT, swbit)) {
        pa_log_debug("Device does NOT have SW_HEADPHONE_INSERT");
        return false;
    }

    return true;
}

static int find_input_device()
{
    DIR *dir;
    struct dirent *de;
    char path[PATH_MAX];
    int fd = -1;

    if (!(dir = opendir(DEV_INPUT_EVENT))) {
        pa_log("failed to open directory " DEV_INPUT_EVENT);
        return fd;
    }

    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_CHR && de->d_type != DT_LNK)
            continue;

        pa_snprintf(path, sizeof(path), DEV_INPUT_EVENT "/%s", de->d_name);

        if ((fd = open(path, O_RDONLY)) < 0) {
            pa_log("failed to open %s for reading", path);
            continue;
        }

        if (check_device(fd)) {
            pa_log_info("input device found at %s", path);
            break;
        } else {
            close(fd);
            fd = -1;
        }
    }

    closedir(dir);

    return fd;
}

static bool setup(pa_droid_extevdev *u) {
    u->fd = find_input_device();

    if (u->fd < 0) {
        pa_log("could not start input device detection.");
        return false;
    }

    u->event = u->card->core->mainloop->io_new(u->card->core->mainloop,
                                               u->fd,
                                               PA_IO_EVENT_INPUT|PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR,
                                               event_callback,
                                               u);

    query_initial_state(u);

    return true;
}

pa_droid_extevdev *pa_droid_extevdev_new(pa_card *card) {
    pa_droid_extevdev *u = pa_xnew0(pa_droid_extevdev, 1);

    pa_assert(card);

    u->card = card;

    if (!setup(u))
        goto fail;

    return u;

fail:
    pa_droid_extevdev_free(u);
    return NULL;
}

void pa_droid_extevdev_free(pa_droid_extevdev *u) {
    if (!u)
        return;

    u->card->core->mainloop->io_free(u->event);

    pa_xfree(u);
}
