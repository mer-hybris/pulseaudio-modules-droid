#ifndef foosllistfoo
#define foosllistfoo

/*
 * Copyright (C) 2018-2022 Jolla Ltd.
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
#include <stdbool.h>
#include <pulse/def.h>

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

typedef struct dm_list_entry dm_list_entry;
typedef struct dm_list dm_list;

struct dm_list_entry {
    struct dm_list_entry *next;
    struct dm_list_entry *prev;
    void *data;
};

struct dm_list {
    struct dm_list_entry *head;
    struct dm_list_entry *tail;
    ssize_t size;
};

dm_list *dm_list_new(void);
void dm_list_free(dm_list *list, pa_free_cb_t free_cb);
bool dm_list_remove(dm_list *list, dm_list_entry *entry);
void dm_list_prepend(dm_list *list, void *data);
void dm_list_push_back(dm_list *list, void *data);
dm_list_entry *dm_list_last(dm_list *list);
void *dm_list_steal_first(dm_list *list);
ssize_t dm_list_size(dm_list *list);
void *dm_list_first_data(dm_list *list, void **state);
void *dm_list_next_data(dm_list *list, void **state);
/* For example
 * dm_list *list;
 * void *state;
 * my_data *data;
 * DM_LIST_FOREACH_DATA(data, list, state) {
 *    do_something_with_my(data);
 * }
 */
#define DM_LIST_FOREACH_DATA(i, list, state)                        \
    for (i = dm_list_first_data(list, &(state)); state; i = dm_list_next_data(list, &(state)))
/* Access i->data */
#define DM_LIST_FOREACH(i, list)                                    \
    for (i = list->head; i; i = i->next)

#endif
