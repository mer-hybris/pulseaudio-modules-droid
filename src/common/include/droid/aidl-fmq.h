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

/*
 * Client side of an Android fast message queue (libfmq) described by an
 * android.hardware.common.fmq.MQDescriptor received over binder.
 */

#ifndef DROID_AIDL_FMQ_H
#define DROID_AIDL_FMQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIDL_FMQ_MAX_GRANTORS 4
#define AIDL_FMQ_MAX_FDS      4

/* EventFlag bits used by libfmq's readBlocking()/writeBlocking(). */
#define AIDL_FMQ_NOT_FULL  0x01
#define AIDL_FMQ_NOT_EMPTY 0x02

struct aidl_fmq_grantor {
    int32_t fd_index;
    int32_t offset;
    int64_t extent;
};

struct aidl_fmq_desc {
    struct aidl_fmq_grantor grantors[AIDL_FMQ_MAX_GRANTORS];
    int n_grantors;
    int fds[AIDL_FMQ_MAX_FDS];
    int n_fds;
    int32_t quantum;           /* item size in bytes */
    int32_t flags;             /* 1 = SynchronizedReadWrite, 2 = UnsynchronizedWrite */
};

typedef struct aidl_fmq aidl_fmq;

void aidl_fmq_desc_init(struct aidl_fmq_desc *d);
/* Closes any fds still held by the descriptor. */
void aidl_fmq_desc_clear(struct aidl_fmq_desc *d);

/* Maps the queue. The descriptor's fds are closed afterwards in any case. */
aidl_fmq *aidl_fmq_new(struct aidl_fmq_desc *d);
void aidl_fmq_free(aidl_fmq *q);

size_t aidl_fmq_item_size(const aidl_fmq *q);
size_t aidl_fmq_capacity(const aidl_fmq *q);      /* in items */
size_t aidl_fmq_available_to_read(aidl_fmq *q);   /* in items */
size_t aidl_fmq_available_to_write(aidl_fmq *q);  /* in items */

bool aidl_fmq_read(aidl_fmq *q, void *data, size_t items);
bool aidl_fmq_write(aidl_fmq *q, const void *data, size_t items);

/* libfmq readBlocking()/writeBlocking() with the default notification
 * bits. timeout_ms < 0 waits forever. Returns 0, -ETIMEDOUT, -ENOSYS when
 * the queue has no event flag, or another negative errno. */
int aidl_fmq_read_blocking(aidl_fmq *q, void *data, size_t items, int timeout_ms);
int aidl_fmq_write_blocking(aidl_fmq *q, const void *data, size_t items, int timeout_ms);

#endif /* DROID_AIDL_FMQ_H */
