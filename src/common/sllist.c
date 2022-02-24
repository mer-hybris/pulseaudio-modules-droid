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
#include <pulse/xmalloc.h>
#include <pulsecore/macro.h>

#include "droid/sllist.h"

dm_list *dm_list_new(void) {
    return pa_xnew0(dm_list, 1);
}

void dm_list_free(dm_list *list, pa_free_cb_t free_cb) {
    pa_assert(list);

    while (list->head) {
        void *data = dm_list_steal_first(list);

        if (free_cb)
            free_cb(data);
    }

    pa_xfree(list);
}

bool dm_list_remove(dm_list *list, dm_list_entry *entry) {
    dm_list_entry *i;
    bool removed = false;

    for (i = list->head; i; i = i->next) {
        if (i == entry) {
            removed = true;
            if (list->head == entry)
                list->head = entry->next;
            if (list->tail == entry)
                list->tail = entry->prev;
            if (entry->next)
                entry->next->prev = entry->prev;
            if (entry->prev)
                entry->prev->next = entry->next;
            pa_xfree(entry);
            break;
        }
    }

    return removed;
}

void dm_list_prepend(dm_list *list, void *data) {
    dm_list_entry *entry;

    pa_assert(list);

    entry = pa_xnew0(dm_list_entry, 1);
    entry->data = data;

    if (!list->tail)
        list->tail = entry;

    if (list->head) {
        entry->next = list->head;
        list->head->prev = entry;
    }

    list->head = entry;
    list->size++;
}

void dm_list_push_back(dm_list *list, void *data) {
    dm_list_entry *entry;

    pa_assert(list);

    entry = pa_xnew0(dm_list_entry, 1);
    entry->data = data;

    if (!list->head)
        list->head = entry;

    if (list->tail) {
        list->tail->next = entry;
        entry->prev = list->tail;
    }

    list->tail = entry;
    list->size++;
}

dm_list_entry *dm_list_last(dm_list *list) {
    pa_assert(list);

    return list->tail;
}

void *dm_list_steal_first(dm_list *list) {
    dm_list_entry *entry;
    void *data = NULL;

    pa_assert(list);

    if (list->head) {
        data = list->head->data;
        entry = list->head;
        if (list->head == list->tail) {
            list->head = NULL;
            list->tail = NULL;
        } else {
            list->head->next->prev = NULL;
            list->head = list->head->next;
        }
        pa_xfree(entry);
        list->size--;
    }

    return data;
}

ssize_t dm_list_size(dm_list *list) {
    pa_assert(list);

    return list->size;
}

/* For iteration */

void *dm_list_first_data(dm_list *list, void **state) {
    pa_assert(list);
    pa_assert(state);

    *state = list->head;

    if (list->head)
        return list->head->data;
    else
        return NULL;
}

void *dm_list_next_data(dm_list *list, void **state) {
    dm_list_entry *entry;

    pa_assert(list);
    pa_assert(state);

    entry = *state;
    *state = entry->next;

    if (entry->next)
        return entry->next->data;
    else
        return NULL;
}
