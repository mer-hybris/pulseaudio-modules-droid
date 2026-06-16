#ifndef foodroidextusbdevhfoo
#define foodroidextusbdevhfoo

/***
  Copyright (C) 2026 Jolla Mobile Ltd.
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

#include <droid/droid-util.h>

typedef struct pa_droid_extusbdev pa_droid_extusbdev;

pa_droid_extusbdev *pa_droid_extusbdev_new(pa_droid_hw_module *hw, pa_card *c);

void pa_droid_extusbdev_free(pa_droid_extusbdev *);

#endif
