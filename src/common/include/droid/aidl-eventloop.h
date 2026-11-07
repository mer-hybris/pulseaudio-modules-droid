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

#ifndef DROID_AIDL_EVENTLOOP_H
#define DROID_AIDL_EVENTLOOP_H

#include <stdbool.h>

#include <pulsecore/core.h>

/* Makes libgbinder dispatch its main-loop work (death notifications,
 * deferred releases, timers) on the PulseAudio main loop instead of a
 * GLib one. Call from the main thread before creating any libgbinder
 * object; later calls are no-ops. */
bool droid_aidl_eventloop_init(pa_core *core);

/* Undoes one droid_aidl_eventloop_init(). The last call releases the main
 * loop resources and hands libgbinder back its default integration. */
void droid_aidl_eventloop_done(void);

#endif /* DROID_AIDL_EVENTLOOP_H */
