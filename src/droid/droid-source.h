#ifndef foodroidsourcefoo
#define foodroidsourcefoo

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

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/source.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/card.h>

#include "droid-util.h"

/* If device is non-zero, it will override whatever is set in modargs for input device. */
pa_source *pa_droid_source_new(pa_module *m,
                                 pa_modargs *ma,
                                 const char *driver,
                                 audio_devices_t device,
                                 pa_droid_card_data *card_data,
                                 pa_droid_mapping *am,
                                 pa_card *card);
void pa_droid_source_free(pa_source *s);

#endif
