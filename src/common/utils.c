/*
 * Copyright (C) 2022 Jolla Ltd.
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

#include <string.h>
#include <strings.h>

#include <pulsecore/core-util.h>
#include <pulse/xmalloc.h>
#include "droid/utils.h"

void dm_replace_in_place(char **string, const char *a, const char *b) {
    char *tmp;

    pa_assert(*string);
    pa_assert(a);
    pa_assert(b);

    tmp = pa_replace(*string, a, b);
    pa_xfree(*string);
    *string = tmp;
}

/* Simple strcasestr replacement. */
bool dm_strcasestr(const char *haystack, const char *needle) {
    size_t len_haystack, len_needle;

    len_haystack = strlen(haystack);
    len_needle = strlen(needle);

    if (len_needle > len_haystack)
        return false;

    for (size_t i = 0; i < len_haystack; i++) {
        if (len_needle > len_haystack - i)
            return false;

        if (strncasecmp(haystack + i, needle, len_needle) == 0)
            return true;
    }

    return false;
}
