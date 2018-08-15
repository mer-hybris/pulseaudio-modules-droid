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
#ifndef foomoduledroidcardsymdeffoo
#define foomoduledroidcardsymdeffoo

#include <pulsecore/core.h>
#include <pulsecore/module.h>

#define pa__init module_droid_card_LTX_pa__init
#define pa__done module_droid_card_LTX_pa__done
#define pa__get_author module_droid_card_LTX_pa__get_author
#define pa__get_description module_droid_card_LTX_pa__get_description
#define pa__get_usage module_droid_card_LTX_pa__get_usage
#define pa__get_version module_droid_card_LTX_pa__get_version

int pa__init(struct pa_module*m);
void pa__done(struct pa_module*m);

const char* pa__get_author(void);
const char* pa__get_description(void);
const char* pa__get_usage(void);
const char* pa__get_version(void);

#endif
