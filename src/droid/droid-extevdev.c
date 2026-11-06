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
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/types.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>

#include <pulsecore/device-port.h>
#include <pulsecore/card.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>

#include <droid/sllist.h>

#include "droid-extevdev.h"

#define DEV_INPUT_EVENT "/dev/input"

/* Jack switches for one physical plug can arrive from different evdev
 * devices, each closed by its own SYN_REPORT. Hold the port update until
 * every followed device has reported, or until this timer elapses. */
#define HEADSET_SYNC_TIMEOUT_USEC ((pa_usec_t) 300 * PA_USEC_PER_MSEC)

typedef struct extevdev_device {
    struct pa_droid_extevdev *parent;
    int fd;
    int fd_type;
    pa_io_event *io;
    char *path;
    bool dead;

    /* Switch values reported by this device. */
    int sw_headphone_insert;
    int sw_microphone_insert;
    int sw_lineout_insert;

    /* Set for the current event batch. */
    bool got_event;
    bool got_syn;
} extevdev_device;

struct pa_droid_extevdev {
    pa_card *card;
    dm_list *devs;
    pa_time_event *event_timer;

    /* Switch values combined from every followed device. */
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

    pa_log_debug("headset switches: headphone=%d microphone=%d lineout=%d",
                 u->sw_headphone_insert, u->sw_microphone_insert, u->sw_lineout_insert);

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

static void recompute_switches(pa_droid_extevdev *u) {
    extevdev_device *dev;
    void *state;

    u->sw_headphone_insert = 0;
    u->sw_microphone_insert = 0;
    u->sw_lineout_insert = 0;

    DM_LIST_FOREACH_DATA(dev, u->devs, state) {
        u->sw_headphone_insert |= dev->sw_headphone_insert;
        u->sw_microphone_insert |= dev->sw_microphone_insert;
        u->sw_lineout_insert |= dev->sw_lineout_insert;
    }
}

static void clear_report_flags(pa_droid_extevdev *u) {
    extevdev_device *dev;
    void *state;

    DM_LIST_FOREACH_DATA(dev, u->devs, state) {
        dev->got_event = false;
        dev->got_syn = false;
    }
}

static void stop_event_timer(pa_droid_extevdev *u) {
    if (!u->event_timer)
        return;

    u->card->core->mainloop->time_free(u->event_timer);
    u->event_timer = NULL;
}

/* True when every device that can still send events has delivered a switch
 * event and the SYN_REPORT that closes it. */
static bool all_devices_reported(pa_droid_extevdev *u) {
    extevdev_device *dev;
    void *state;
    bool any = false;

    DM_LIST_FOREACH_DATA(dev, u->devs, state) {
        if (dev->dead)
            continue;

        if (!dev->got_event || !dev->got_syn)
            return false;

        any = true;
    }

    return any;
}

static void apply_device_state(pa_droid_extevdev *u, const char *why) {
    pa_log_debug("applying headset state (%s)", why);
    stop_event_timer(u);
    recompute_switches(u);
    notify_ports(u);
    clear_report_flags(u);
}

static void device_event_timeout_cb(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv, void *userdata) {
    pa_droid_extevdev *u = userdata;

    pa_assert(u);
    pa_assert(e == u->event_timer);
    (void) a;
    (void) tv;

    apply_device_state(u, "event timer elapsed");
}

/* Start the timer on the first event of a batch. Later events from the same
 * batch do not extend it. */
static void setup_event_timer(pa_droid_extevdev *u) {
    if (u->event_timer)
        return;

    u->event_timer = pa_core_rttime_new(u->card->core,
                                        pa_rtclock_now() + HEADSET_SYNC_TIMEOUT_USEC,
                                        device_event_timeout_cb, u);
    if (!u->event_timer) {
        apply_device_state(u, "timer failed");
        return;
    }
}

static void query_device_state(extevdev_device *dev) {
    uint32_t bitmask[NBITS(SW_MAX)];

    memset(bitmask, 0, sizeof(bitmask));

    if (ioctl(dev->fd, EVIOCGSW(sizeof(bitmask)), &bitmask) < 0) {
        pa_log("failed to query current connected state of %s: %s",
               dev->path, pa_cstrerror(errno));
        return;
    }

    dev->sw_headphone_insert = !!TEST_BIT(SW_HEADPHONE_INSERT, bitmask);
    dev->sw_microphone_insert = !!TEST_BIT(SW_MICROPHONE_INSERT, bitmask);
    dev->sw_lineout_insert = !!TEST_BIT(SW_LINEOUT_INSERT, bitmask);

    pa_log_debug("%s: headphone is %sconnected", dev->path, dev->sw_headphone_insert ? "" : "dis");
    pa_log_debug("%s: microphone is %sconnected", dev->path, dev->sw_microphone_insert ? "" : "dis");
    pa_log_debug("%s: lineout is %sconnected", dev->path, dev->sw_lineout_insert ? "" : "dis");
}

static void process_event(extevdev_device *dev, const struct input_event *ev) {
    if (ev->type == EV_SYN) {
        if (ev->code == SYN_DROPPED) {
            pa_log_debug("%s: events dropped, resyncing switch state", dev->path);
            query_device_state(dev);
            dev->got_event = true;
            dev->got_syn = true;
            return;
        }

        if (ev->code != SYN_REPORT || !dev->got_event)
            return;

        dev->got_syn = true;

        return;
    }

    if (ev->type != EV_SW) {
        pa_log_debug("ignoring input event type %d from %s", ev->type, dev->path);
        return;
    }

    switch (ev->code) {
        case SW_HEADPHONE_INSERT:
            dev->sw_headphone_insert = ev->value;
            pa_log_debug("%s: SW_HEADPHONE_INSERT %d", dev->path, ev->value);
            break;
        case SW_MICROPHONE_INSERT:
            dev->sw_microphone_insert = ev->value;
            pa_log_debug("%s: SW_MICROPHONE_INSERT %d", dev->path, ev->value);
            break;
        case SW_LINEOUT_INSERT:
            dev->sw_lineout_insert = ev->value;
            pa_log_debug("%s: SW_LINEOUT_INSERT %d", dev->path, ev->value);
            break;
        default:
            return;
    }

    dev->got_event = true;
}

static void event_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
    extevdev_device *dev = userdata;
    pa_droid_extevdev *u = dev->parent;

    (void) fd;

    /* The fd is non-blocking, so take the whole pending report before
     * deciding whether every device has checked in. */
    if (events & PA_IO_EVENT_INPUT) {
        for (;;) {
            struct input_event ev;
            ssize_t r;

            r = pa_loop_read(dev->fd, &ev, sizeof(ev), &dev->fd_type);
            if (r < 0) {
                if (errno == EAGAIN)
                    break;

                pa_log("failed to read from %s: %s", dev->path, pa_cstrerror(errno));
                dev->dead = true;
                break;
            }

            if (r == 0 || (size_t) r != sizeof(ev)) {
                if (r == 0)
                    pa_log("input device %s closed unexpectedly", dev->path);
                else
                    pa_log("short read from %s (%zd bytes)", dev->path, r);
                dev->dead = true;
                break;
            }

            process_event(dev, &ev);
        }
    }

    if (!dev->dead && (events & (PA_IO_EVENT_HANGUP | PA_IO_EVENT_ERROR))) {
        pa_log("input device %s %s", dev->path,
               (events & PA_IO_EVENT_ERROR) ? "had an I/O error" : "closed unexpectedly");
        dev->dead = true;
        io->io_enable(e, 0);
        return;
    }

    if (all_devices_reported(u))
        apply_device_state(u, "all devices reported");
    else
        setup_event_timer(u);
}

static bool check_device(int fd) {
    unsigned long evbit[NBITS(EV_MAX)];
    unsigned long swbit[NBITS(SW_MAX)];
    bool has_headphone, has_microphone, has_lineout;

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

    has_headphone = !!TEST_BIT(SW_HEADPHONE_INSERT, swbit);
    has_microphone = !!TEST_BIT(SW_MICROPHONE_INSERT, swbit);
    has_lineout = !!TEST_BIT(SW_LINEOUT_INSERT, swbit);

    if (!has_headphone && !has_microphone && !has_lineout) {
        pa_log_debug("device has no headset switches");
        return false;
    }

    pa_log_debug("device headset switches: headphone=%d microphone=%d lineout=%d",
                 has_headphone, has_microphone, has_lineout);
    return true;
}

static bool device_is_open(pa_droid_extevdev *u, const char *path) {
    extevdev_device *dev;
    void *state;

    DM_LIST_FOREACH_DATA(dev, u->devs, state) {
        if (pa_streq(dev->path, path))
            return true;
    }

    return false;
}

/* Takes ownership of fd. */
static bool add_device(pa_droid_extevdev *u, int fd, const char *path) {
    extevdev_device *dev;
    char name[256];

    dev = pa_xnew0(struct extevdev_device, 1);
    dev->parent = u;
    dev->fd = fd;
    dev->path = pa_xstrdup(path);

    dev->io = u->card->core->mainloop->io_new(u->card->core->mainloop,
                                              fd,
                                              PA_IO_EVENT_INPUT | PA_IO_EVENT_HANGUP | PA_IO_EVENT_ERROR,
                                              event_callback,
                                              dev);
    if (!dev->io) {
        pa_log("failed to watch input device %s", path);
        pa_xfree(dev->path);
        pa_xfree(dev);
        close(fd);
        return false;
    }

    dm_list_push_back(u->devs, dev);

    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && name[0])
        pa_log_info("following input device %s (%s)", path, name);
    else
        pa_log_info("following input device %s", path);

    return true;
}

static bool open_path(pa_droid_extevdev *u, const char *path) {
    int fd;

    if (device_is_open(u, path)) {
        pa_log_debug("input device %s is already followed", path);
        return true;
    }

    if ((fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)) < 0) {
        pa_log("failed to open %s for reading: %s", path, pa_cstrerror(errno));
        return false;
    }

    if (!check_device(fd)) {
        pa_log("not following %s, no headset switches", path);
        close(fd);
        return false;
    }

    return add_device(u, fd, path);
}

static dm_list *split_comma_list(const char *list) {
    const char *split_state = NULL;
    char *token;
    dm_list *items;

    items = dm_list_new();

    if (!list || !*list)
        return items;

    while ((token = pa_split(list, ",", &split_state))) {
        char *item = pa_strip(token);

        if (item && *item)
            dm_list_push_back(items, pa_xstrdup(item));

        pa_xfree(token);
    }

    return items;
}

static bool name_matches(const char *name, dm_list *matches) {
    char *match;
    void *state;

    DM_LIST_FOREACH_DATA(match, matches, state) {
        if (strstr(name, match))
            return true;
    }

    return false;
}

static void scan_input_devices(pa_droid_extevdev *u, const char *match_list) {
    DIR *dir;
    struct dirent *de;
    dm_list *matches;

    matches = split_comma_list(match_list);

    if (!(dir = opendir(DEV_INPUT_EVENT))) {
        pa_log("failed to open directory " DEV_INPUT_EVENT ": %s", pa_cstrerror(errno));
        dm_list_free(matches, pa_xfree);
        return;
    }

    while ((de = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        char name[256];
        int fd;

        if (de->d_type != DT_CHR && de->d_type != DT_LNK)
            continue;

        pa_snprintf(path, sizeof(path), DEV_INPUT_EVENT "/%s", de->d_name);

        if (device_is_open(u, path))
            continue;

        if ((fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)) < 0) {
            pa_log("failed to open %s for reading: %s", path, pa_cstrerror(errno));
            continue;
        }

        if (dm_list_size(matches) > 0) {
            memset(name, 0, sizeof(name));

            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
                pa_log_warn("could not get device name of %s: %s", path, pa_cstrerror(errno));
                close(fd);
                continue;
            }

            if (!name_matches(name, matches)) {
                close(fd);
                continue;
            }

            pa_log_debug("input device %s (%s) matches", path, name);
        }

        if (!check_device(fd)) {
            close(fd);
            continue;
        }

        add_device(u, fd, path);
    }

    closedir(dir);
    dm_list_free(matches, pa_xfree);
}

static void extevdev_device_free(void *p) {
    extevdev_device *dev = p;

    pa_assert(dev);

    if (dev->io)
        dev->parent->card->core->mainloop->io_free(dev->io);

    if (dev->fd >= 0)
        close(dev->fd);

    pa_xfree(dev->path);
    pa_xfree(dev);
}

static bool setup(const char *evdev_device, const char *evdev_match, pa_droid_extevdev *u) {
    extevdev_device *dev;
    void *state;

    if (evdev_device) {
        char *path;
        dm_list *paths;

        paths = split_comma_list(evdev_device);
        DM_LIST_FOREACH_DATA(path, paths, state)
            open_path(u, path);
        dm_list_free(paths, pa_xfree);
    } else {
        scan_input_devices(u, evdev_match);
    }

    if (dm_list_size(u->devs) == 0) {
        pa_log("could not start input device detection.");
        return false;
    }

    DM_LIST_FOREACH_DATA(dev, u->devs, state)
        query_device_state(dev);

    recompute_switches(u);
    notify_ports(u);

    return true;
}

pa_droid_extevdev *pa_droid_extevdev_new(const char *evdev_device, const char *evdev_match, pa_card *card) {
    pa_droid_extevdev *u = pa_xnew0(pa_droid_extevdev, 1);

    pa_assert(card);

    u->card = card;
    u->devs = dm_list_new();

    if (!setup(evdev_device, evdev_match, u))
        goto fail;

    return u;

fail:
    pa_droid_extevdev_free(u);
    return NULL;
}

void pa_droid_extevdev_free(pa_droid_extevdev *u) {
    if (!u)
        return;

    stop_event_timer(u);

    if (u->devs)
        dm_list_free(u->devs, extevdev_device_free);

    pa_xfree(u);
}
