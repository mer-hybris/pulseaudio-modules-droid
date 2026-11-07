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
 * Mirrors system/libfmq MessageQueueBase and EventFlag: a ring buffer in
 * shared memory with 64-bit read/write counters and a futex word for
 * blocking. The HAL owns the memory; we only map what its descriptor
 * grants us.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/futex.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <pulse/xmalloc.h>
#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "droid/aidl-fmq.h"

enum {
    GRANTOR_READ_PTR = 0,
    GRANTOR_WRITE_PTR,
    GRANTOR_DATA,
    GRANTOR_EVENT_FLAG,
};

struct aidl_fmq {
    size_t quantum;
    size_t ring_size;          /* bytes */
    struct {
        void *base;
        size_t len;
    } maps[AIDL_FMQ_MAX_GRANTORS];
    uint64_t *read_ptr;
    uint64_t *write_ptr;
    uint8_t *ring;
    uint32_t *event_flag;
};

void aidl_fmq_desc_init(struct aidl_fmq_desc *d) {
    memset(d, 0, sizeof(*d));
    for (int i = 0; i < AIDL_FMQ_MAX_FDS; i++)
        d->fds[i] = -1;
}

void aidl_fmq_desc_clear(struct aidl_fmq_desc *d) {
    for (int i = 0; i < AIDL_FMQ_MAX_FDS; i++) {
        if (d->fds[i] >= 0)
            close(d->fds[i]);
        d->fds[i] = -1;
    }
    d->n_fds = 0;
    d->n_grantors = 0;
}

static void *map_grantor(aidl_fmq *q, const struct aidl_fmq_desc *d, int idx) {
    const struct aidl_fmq_grantor *g = &d->grantors[idx];
    long page = sysconf(_SC_PAGESIZE);
    off_t map_off;
    size_t delta, len;
    void *base;

    if (g->fd_index < 0 || g->fd_index >= d->n_fds || d->fds[g->fd_index] < 0 ||
        g->offset < 0 || g->extent <= 0) {
        pa_log("aidl-fmq: bad grantor %d (fd %d offset %d extent %" PRId64 ")",
               idx, g->fd_index, g->offset, g->extent);
        return NULL;
    }

    map_off = g->offset & ~(page - 1);
    delta = g->offset - map_off;
    if ((uint64_t) g->extent > SIZE_MAX - delta)
        return NULL;
    len = delta + (size_t) g->extent;

    base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, d->fds[g->fd_index], map_off);
    if (base == MAP_FAILED) {
        pa_log("aidl-fmq: mmap of grantor %d failed: %s", idx, pa_cstrerror(errno));
        return NULL;
    }

    q->maps[idx].base = base;
    q->maps[idx].len = len;
    return (uint8_t *) base + delta;
}

aidl_fmq *aidl_fmq_new(struct aidl_fmq_desc *d) {
    aidl_fmq *q;

    pa_assert(d);

    q = pa_xnew0(aidl_fmq, 1);

    if (d->n_grantors < GRANTOR_DATA + 1 || d->n_grantors > AIDL_FMQ_MAX_GRANTORS ||
        d->n_fds <= 0 || d->n_fds > AIDL_FMQ_MAX_FDS || d->quantum <= 0 || d->flags != 1) {
        pa_log("aidl-fmq: descriptor has %d grantors, quantum %d", d->n_grantors, d->quantum);
        goto fail;
    }

    for (int i = 0; i < d->n_grantors; i++) {
        size_t alignment = i == GRANTOR_EVENT_FLAG ? sizeof(uint32_t) : sizeof(uint64_t);
        int64_t minimum = i == GRANTOR_DATA ? d->quantum : (int64_t) alignment;

        if (d->grantors[i].offset < 0 || d->grantors[i].offset % alignment ||
            d->grantors[i].extent < minimum || (uint64_t) d->grantors[i].extent > SIZE_MAX)
            goto fail;
    }
    if (d->grantors[GRANTOR_DATA].extent % d->quantum)
        goto fail;

    q->quantum = (size_t) d->quantum;
    q->ring_size = (size_t) d->grantors[GRANTOR_DATA].extent;

    if (!(q->read_ptr = map_grantor(q, d, GRANTOR_READ_PTR)) ||
        !(q->write_ptr = map_grantor(q, d, GRANTOR_WRITE_PTR)) ||
        !(q->ring = map_grantor(q, d, GRANTOR_DATA)))
        goto fail;

    if (d->n_grantors > GRANTOR_EVENT_FLAG &&
        !(q->event_flag = map_grantor(q, d, GRANTOR_EVENT_FLAG)))
        goto fail;

    /* Like AidlMessageQueue(desc): the queue was just created for us. */
    __atomic_store_n(q->read_ptr, 0, __ATOMIC_RELEASE);
    __atomic_store_n(q->write_ptr, 0, __ATOMIC_RELEASE);

    aidl_fmq_desc_clear(d);
    return q;

fail:
    aidl_fmq_desc_clear(d);
    aidl_fmq_free(q);
    return NULL;
}

void aidl_fmq_free(aidl_fmq *q) {
    if (!q)
        return;

    for (int i = 0; i < AIDL_FMQ_MAX_GRANTORS; i++)
        if (q->maps[i].base)
            munmap(q->maps[i].base, q->maps[i].len);

    pa_xfree(q);
}

size_t aidl_fmq_item_size(const aidl_fmq *q) {
    return q->quantum;
}

size_t aidl_fmq_capacity(const aidl_fmq *q) {
    return q->ring_size / q->quantum;
}

static uint64_t bytes_to_read(aidl_fmq *q) {
    uint64_t w = __atomic_load_n(q->write_ptr, __ATOMIC_ACQUIRE);
    uint64_t r = __atomic_load_n(q->read_ptr, __ATOMIC_ACQUIRE);
    return w - r;
}

size_t aidl_fmq_available_to_read(aidl_fmq *q) {
    uint64_t used = bytes_to_read(q);
    return used > q->ring_size ? 0 : (size_t) used / q->quantum;
}

size_t aidl_fmq_available_to_write(aidl_fmq *q) {
    uint64_t used = bytes_to_read(q);
    return used > q->ring_size ? 0 : (q->ring_size - used) / q->quantum;
}

bool aidl_fmq_write(aidl_fmq *q, const void *data, size_t items) {
    size_t bytes;
    uint64_t w;
    size_t off, first;

    if (items > aidl_fmq_available_to_write(q))
        return false;
    bytes = items * q->quantum;
    if (bytes == 0)
        return true;
    if (bytes > q->ring_size)
        return false;

    w = __atomic_load_n(q->write_ptr, __ATOMIC_RELAXED);
    off = (size_t) (w % q->ring_size);
    first = PA_MIN(bytes, q->ring_size - off);

    memcpy(q->ring + off, data, first);
    if (bytes > first)
        memcpy(q->ring, (const uint8_t *) data + first, bytes - first);

    __atomic_store_n(q->write_ptr, w + bytes, __ATOMIC_RELEASE);
    return true;
}

bool aidl_fmq_read(aidl_fmq *q, void *data, size_t items) {
    size_t bytes;
    uint64_t w, r;
    size_t off, first;

    if (items > aidl_fmq_capacity(q))
        return false;
    bytes = items * q->quantum;
    if (bytes == 0)
        return true;

    w = __atomic_load_n(q->write_ptr, __ATOMIC_ACQUIRE);
    r = __atomic_load_n(q->read_ptr, __ATOMIC_RELAXED);

    if (w - r > q->ring_size) {
        /* Writer overran us; libfmq resets the reader in this case. */
        __atomic_store_n(q->read_ptr, w, __ATOMIC_RELEASE);
        return false;
    }
    if (w - r < bytes)
        return false;

    off = (size_t) (r % q->ring_size);
    first = PA_MIN(bytes, q->ring_size - off);

    memcpy(data, q->ring + off, first);
    if (bytes > first)
        memcpy((uint8_t *) data + first, q->ring, bytes - first);

    __atomic_store_n(q->read_ptr, r + bytes, __ATOMIC_RELEASE);
    return true;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* EventFlag::wait(): returns 0 with *state set, -EINTR on a spurious
 * wakeup, -ETIMEDOUT, or another negative errno. */
static int event_wait(aidl_fmq *q, uint32_t mask, uint32_t *state, int timeout_ms) {
    uint32_t old = __atomic_fetch_and(q->event_flag, ~mask, __ATOMIC_SEQ_CST);
    uint32_t expected = old & ~mask;
    struct timespec deadline, *tsp = NULL;
    long rc;

    if (old & mask) {
        *state = old & mask;
        return 0;
    }

    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        tsp = &deadline;
    }

    rc = syscall(SYS_futex, q->event_flag, FUTEX_WAIT_BITSET, expected, tsp, NULL, mask);
    if (rc < 0 && errno != EAGAIN && errno != EINTR)
        return -errno;

    old = __atomic_fetch_and(q->event_flag, ~mask, __ATOMIC_SEQ_CST);
    *state = old & mask;
    return *state ? 0 : -EINTR;
}

static void event_wake(aidl_fmq *q, uint32_t mask) {
    uint32_t old = __atomic_fetch_or(q->event_flag, mask, __ATOMIC_SEQ_CST);

    if (~old & mask)
        syscall(SYS_futex, q->event_flag, FUTEX_WAKE_BITSET, INT_MAX, NULL, NULL, mask);
}

static int wait_for(aidl_fmq *q, uint32_t mask, int64_t deadline_ms) {
    for (;;) {
        uint32_t state;
        int remaining = -1;
        int ret;

        if (deadline_ms >= 0) {
            int64_t left = deadline_ms - now_ms();
            if (left <= 0)
                return -ETIMEDOUT;
            remaining = (int) PA_MIN(left, (int64_t) INT_MAX);
        }

        ret = event_wait(q, mask, &state, remaining);
        if (ret != -EINTR)
            return ret;
    }
}

int aidl_fmq_write_blocking(aidl_fmq *q, const void *data, size_t items, int timeout_ms) {
    int64_t deadline = timeout_ms < 0 ? -1 : now_ms() + timeout_ms;

    if (!q->event_flag)
        return -ENOSYS;
    if (items > aidl_fmq_capacity(q))
        return -EINVAL;

    for (;;) {
        int ret;

        if (aidl_fmq_write(q, data, items)) {
            event_wake(q, AIDL_FMQ_NOT_EMPTY);
            return 0;
        }

        ret = wait_for(q, AIDL_FMQ_NOT_FULL, deadline);
        if (ret == -ETIMEDOUT) {
            if (aidl_fmq_write(q, data, items)) {
                event_wake(q, AIDL_FMQ_NOT_EMPTY);
                return 0;
            }
            return -ETIMEDOUT;
        }
        if (ret < 0)
            return ret;
    }
}

int aidl_fmq_read_blocking(aidl_fmq *q, void *data, size_t items, int timeout_ms) {
    int64_t deadline = timeout_ms < 0 ? -1 : now_ms() + timeout_ms;

    if (!q->event_flag)
        return -ENOSYS;
    if (items > aidl_fmq_capacity(q))
        return -EINVAL;

    for (;;) {
        int ret;

        if (aidl_fmq_read(q, data, items)) {
            event_wake(q, AIDL_FMQ_NOT_FULL);
            return 0;
        }

        ret = wait_for(q, AIDL_FMQ_NOT_EMPTY, deadline);
        if (ret == -ETIMEDOUT) {
            if (aidl_fmq_read(q, data, items)) {
                event_wake(q, AIDL_FMQ_NOT_FULL);
                return 0;
            }
            return -ETIMEDOUT;
        }
        if (ret < 0)
            return ret;
    }
}
