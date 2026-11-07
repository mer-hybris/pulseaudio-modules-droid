#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "droid/aidl-fmq.h"

struct fixture {
    int fd;
    uint64_t *memory;
};

static struct aidl_fmq_desc descriptor(struct fixture *f) {
    struct aidl_fmq_desc d;

    aidl_fmq_desc_init(&d);
    d.n_grantors = 4;
    d.n_fds = 1;
    d.fds[0] = dup(f->fd);
    assert(d.fds[0] >= 0);
    d.quantum = sizeof(uint64_t);
    d.flags = 1;
    d.grantors[0] = (struct aidl_fmq_grantor) {0, 0, 8};
    d.grantors[1] = (struct aidl_fmq_grantor) {0, 8, 8};
    d.grantors[2] = (struct aidl_fmq_grantor) {0, 16, 64};
    d.grantors[3] = (struct aidl_fmq_grantor) {0, 80, 4};
    return d;
}

static void check_ring(struct fixture *f) {
    struct aidl_fmq_desc d = descriptor(f);
    aidl_fmq *q = aidl_fmq_new(&d);
    uint64_t input[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t output[8];

    assert(q);
    assert(d.fds[0] == -1);
    assert(aidl_fmq_capacity(q) == 8);
    assert(!aidl_fmq_read(q, output, 1));
    assert(aidl_fmq_write(q, input, 6));
    assert(aidl_fmq_read(q, output, 4));
    assert(!memcmp(input, output, 4 * sizeof(uint64_t)));
    assert(aidl_fmq_write(q, input, 6));
    assert(!aidl_fmq_write(q, input, 1));
    assert(aidl_fmq_read(q, output, 8));
    assert(output[0] == 5 && output[1] == 6);
    assert(!memcmp(input, output + 2, 6 * sizeof(uint64_t)));
    assert(!aidl_fmq_write(q, input, SIZE_MAX / 8 + 1));
    assert(!aidl_fmq_read(q, output, SIZE_MAX / 8 + 1));

    /* Corrupt counters must not underflow the writable-space calculation. */
    f->memory[1] = f->memory[0] + 72;
    assert(aidl_fmq_available_to_read(q) == 0);
    assert(aidl_fmq_available_to_write(q) == 0);
    assert(!aidl_fmq_write(q, input, 1));
    assert(!aidl_fmq_read(q, output, 1));
    assert(aidl_fmq_available_to_write(q) == 8);
    aidl_fmq_free(q);
}

static void check_descriptors(struct fixture *f) {
    for (int i = 0; i < 8; i++) {
        struct aidl_fmq_desc d = descriptor(f);

        switch (i) {
            case 0: d.quantum = 0; break;
            case 1: d.flags = 2; break;
            case 2: d.grantors[0].extent = 4; break;
            case 3: d.grantors[1].offset = 1; break;
            case 4: d.grantors[2].extent = 63; break;
            case 5: d.grantors[2].extent = -1; break;
            case 6: d.grantors[3].extent = 1; break;
            case 7: d.grantors[2].fd_index = 1; break;
        }
        assert(!aidl_fmq_new(&d));
        assert(d.fds[0] == -1);
    }
}

static void *delayed_write(void *data) {
    aidl_fmq *q = data;
    uint64_t value = 42;

    usleep(10000);
    assert(aidl_fmq_write_blocking(q, &value, 1, 1000) == 0);
    return NULL;
}

static void check_blocking(struct fixture *f) {
    struct aidl_fmq_desc d = descriptor(f);
    aidl_fmq *q = aidl_fmq_new(&d);
    uint64_t value = 0;
    pthread_t writer;

    assert(q);
    assert(aidl_fmq_read_blocking(q, &value, 1, 10) == -ETIMEDOUT);
    assert(pthread_create(&writer, NULL, delayed_write, q) == 0);
    assert(aidl_fmq_read_blocking(q, &value, 1, 1000) == 0);
    assert(value == 42);
    assert(pthread_join(writer, NULL) == 0);
    aidl_fmq_free(q);

    d = descriptor(f);
    d.n_grantors = 3;
    q = aidl_fmq_new(&d);
    assert(q);
    assert(aidl_fmq_read_blocking(q, &value, 1, 10) == -ENOSYS);
    aidl_fmq_free(q);
}

int main(void) {
    struct fixture f;

    f.fd = memfd_create("aidl-fmq-test", MFD_CLOEXEC);
    assert(f.fd >= 0);
    assert(ftruncate(f.fd, 4096) == 0);
    f.memory = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f.fd, 0);
    assert(f.memory != MAP_FAILED);
    check_ring(&f);
    check_descriptors(&f);
    check_blocking(&f);
    munmap(f.memory, 4096);
    close(f.fd);
    return 0;
}
