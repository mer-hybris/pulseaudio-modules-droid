/***
  This file is part of PulseAudio.

  Copyright (C) 2019 UBports foundation.
  Author(s): Ratchanan Srirattanamet <ratchanan@ubports.com>

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

#define _GNU_SOURCE // For scandir and versionsort

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

#include <libevdev/libevdev.h>

#include <pulsecore/core-util.h>
#include <pulsecore/device-port.h>

#include "droid-extevdev.h"

#define DEV_INPUT_EVENT "/dev/input"
#define EVENT_DEV_NAME "event"

struct pa_droid_extevdev {
    pa_card *card;
    struct libevdev *evdev_dev;
    pa_io_event *event;

    /* Switch values */
    bool sw_headphone_insert : 1;
    bool sw_microphone_insert : 1;
    bool sw_lineout_insert : 1;
};

static int is_event_device(const struct dirent *dir) {
    return strncmp(EVENT_DEV_NAME, dir->d_name, 5) == 0;
}

static struct libevdev *find_switch_evdev(void) {
    struct dirent **namelist;
    int ndev, i;
    struct libevdev *ret = NULL;

    ndev = scandir(DEV_INPUT_EVENT, &namelist, is_event_device, versionsort);

    for (i = 0; i < ndev; i++) {
        char fname[PATH_MAX];
        int fd;
        struct libevdev *dev;
        int err;

        snprintf(fname, sizeof(fname), "%s/%s",
                 DEV_INPUT_EVENT, namelist[i]->d_name);

        pa_log_debug("Checking %s for headphone switch.", fname);

        fd = open(fname, O_RDONLY|O_NONBLOCK);
        if (fd < 0) {
            err = errno;
            pa_log_warn("Unable to open device %s, ignored: %s",
                        fname, strerror(err));
            continue;
        }

        if ((err = libevdev_new_from_fd(fd, &dev)) < 0) {
            err = -err;
            pa_log_warn("Unable to create libevdev device for %s, ignored: %s",
                        fname, strerror(err));
            close(fd);
            continue;
        }

        if (libevdev_has_event_code(dev, EV_SW, SW_HEADPHONE_INSERT)) {
            ret = dev;
            break;
        }

        libevdev_free(dev);
        close(fd);
    }

    for (i = 0; i < ndev; i++)
        free(namelist[i]);
    free(namelist);

    return ret;
}

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

/* Called from IO context */
static void evdev_cb(pa_mainloop_api *a, pa_io_event *e, int fd,
                     pa_io_event_flags_t events, void *userdata) {
    pa_droid_extevdev *u = userdata;

    unsigned int flags = LIBEVDEV_READ_FLAG_NORMAL;
    int err;
    struct input_event ev;

    while (1) {
        err = libevdev_next_event(u->evdev_dev, flags, &ev);

        if (err == -EAGAIN) {
            if (flags == LIBEVDEV_READ_FLAG_SYNC) {
                /* Switch the flag back to read next normal events. */
                flags = LIBEVDEV_READ_FLAG_NORMAL;
                continue;
            } else {
                /* We run out of event. */
                break;
            }
        } else if (err == LIBEVDEV_READ_STATUS_SYNC) {
            if (flags == LIBEVDEV_READ_FLAG_NORMAL) {
                /* Handle dropped events by switching to SYNC mode. */
                flags = LIBEVDEV_READ_FLAG_SYNC;
                continue;
            } /* Otherwise we're in the middle of handling it. */
        } else if (err < 0) {
            pa_log_error("Error in reading the event from evdev: %s",
                        strerror(-err));
            /* TODO: Should we just remove the event source? */
            break;
        }

        /* ev now contains the current event. */
        if (ev.type == EV_SW) {
            switch (ev.code) {
                case SW_HEADPHONE_INSERT:
                    u->sw_headphone_insert = ev.value;
                    break;
                case SW_MICROPHONE_INSERT:
                    u->sw_microphone_insert = ev.value;
                    break;
                case SW_LINEOUT_INSERT:
                    u->sw_lineout_insert = ev.value;
                    break;
                default:
                    /* Ignore unknown switch. */
                    break;
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            notify_ports(u);
        }
    }
}

static void read_initial_switch_values(pa_droid_extevdev *u) {
    /* A local variable is needed because sw_* are bitfields. */
    int value;

#define INIT_SW(code, sw_var) \
    if (libevdev_fetch_event_value(u->evdev_dev, EV_SW, code, &value)) \
        u->sw_var = value; \
    else \
        u->sw_var = false;

    INIT_SW(SW_HEADPHONE_INSERT, sw_headphone_insert)
    INIT_SW(SW_MICROPHONE_INSERT, sw_microphone_insert)
    INIT_SW(SW_LINEOUT_INSERT, sw_lineout_insert)

#undef INIT_SW

    notify_ports(u);
}

pa_droid_extevdev *pa_droid_extevdev_new(pa_core *core, pa_card *card) {
    pa_droid_extevdev *u = pa_xnew0(pa_droid_extevdev, 1);

    pa_assert(core);
    pa_assert(card);

    u->card = card;
    u->evdev_dev = find_switch_evdev();

    if (!u->evdev_dev)
        goto fail;

    pa_assert_se(u->event = core->mainloop->io_new(core->mainloop,
                libevdev_get_fd(u->evdev_dev), PA_IO_EVENT_INPUT, evdev_cb, u));

    read_initial_switch_values(u);

    return u;

fail:
    pa_droid_extevdev_free(u);
    return NULL;
}

void pa_droid_extevdev_free(pa_droid_extevdev *u) {
    if (u->event)
        u->card->core->mainloop->io_free(u->event);

    if (u->evdev_dev) {
        int fd = libevdev_get_fd(u->evdev_dev);
        libevdev_free(u->evdev_dev);
        close(fd);
    }

    pa_xfree(u);
}
