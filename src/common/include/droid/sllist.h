#ifndef foosllistfoo
#define foosllistfoo

/*
 * Copyright (C) 2018 Jolla Ltd.
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

#define SLLIST_APPEND(t, head, item)                                \
    do {                                                            \
        item->next = NULL;                                          \
        if (!head) {                                                \
            head = item;                                            \
        } else {                                                    \
            t *_list;                                               \
            for (_list = head; _list->next; _list = _list->next);   \
            _list->next = item;                                     \
        }                                                           \
    } while (0)

#define SLLIST_FOREACH(i, head)                                     \
    for (i = (head); i; i = i->next)

#define SLLIST_STEAL_FIRST(i, head)                                 \
    do {                                                            \
        if (head) {                                                 \
            i = head;                                               \
            head = head->next;                                      \
        } else                                                      \
            i = NULL;                                               \
    } while (0)

#endif
