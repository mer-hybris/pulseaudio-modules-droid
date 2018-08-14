#ifndef foodroidkeepalivefoo
#define foodroidkeepalivefoo

/*
 * Copyright (C) 2013-2018 Jolla Ltd.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/dbus-shared.h>
#include <pulsecore/atomic.h>

typedef struct pa_droid_keepalive pa_droid_keepalive;

pa_droid_keepalive* pa_droid_keepalive_new(pa_core *c);
void pa_droid_keepalive_free(pa_droid_keepalive *k);

void pa_droid_keepalive_start(pa_droid_keepalive *k);
void pa_droid_keepalive_stop(pa_droid_keepalive *k);


#endif
