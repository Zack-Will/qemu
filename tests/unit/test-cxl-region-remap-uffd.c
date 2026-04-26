/*
 * UFFD proof for MAP_FIXED remap over a missing-fault range.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/memfd.h"
#include "qemu/thread.h"
#include "qemu/units.h"
#include "qemu/userfaultfd.h"

#ifdef CONFIG_LINUX
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#endif

typedef struct UffdRemapCase {
    int uffd;
    void *addr;
    size_t len;
    QemuSemaphore done;
    volatile uint8_t value;
} UffdRemapCase;

static void *faulting_reader(void *opaque)
{
    UffdRemapCase *c = opaque;

    c->value = *(volatile uint8_t *)c->addr;
    qemu_sem_post(&c->done);
    return NULL;
}

static void test_map_fixed_then_uffd_wake(void)
{
#ifndef CONFIG_LINUX
    g_test_skip("Linux userfaultfd is required");
#else
    const size_t page_size = qemu_real_host_page_size();
    const size_t len = 2 * MiB;
    const uint8_t expected = 0x5a;
    UffdRemapCase c = {
        .uffd = -1,
        .addr = MAP_FAILED,
        .len = len,
    };
    QemuThread thread;
    struct uffd_msg msg;
    struct pollfd pfd;
    uint64_t ioctls = 0;
    void *remapped;
    int memfd = -1;
    int ret;
    bool sem_inited = false;
    bool thread_started = false;
    bool thread_joined = false;

    c.uffd = uffd_create_fd(0, true);
    if (c.uffd < 0) {
        g_test_skip("userfaultfd is unavailable");
        return;
    }

    c.addr = mmap(NULL, c.len, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_assert_true(c.addr != MAP_FAILED);

    ret = uffd_register_memory(c.uffd, c.addr, c.len,
                               UFFDIO_REGISTER_MODE_MISSING, &ioctls);
    if (ret) {
        g_test_skip("userfaultfd missing-mode registration is unavailable");
        goto out;
    }
    if (!(ioctls & (1ULL << _UFFDIO_WAKE))) {
        g_test_skip("userfaultfd UFFDIO_WAKE is unavailable");
        goto out;
    }

    memfd = qemu_memfd_create("cxl-region-remap-uffd", c.len, false, 0, 0,
                              NULL);
    if (memfd < 0) {
        g_test_skip("memfd is unavailable");
        goto out;
    }
    g_assert_cmpint(pwrite(memfd, &expected, sizeof(expected), 0), ==,
                    sizeof(expected));

    qemu_sem_init(&c.done, 0);
    sem_inited = true;
    qemu_thread_create(&thread, "uffd-remap-reader", faulting_reader, &c,
                       QEMU_THREAD_JOINABLE);
    thread_started = true;

    pfd = (struct pollfd) {
        .fd = c.uffd,
        .events = POLLIN,
    };
    ret = poll(&pfd, 1, 10000);
    g_assert_cmpint(ret, ==, 1);
    g_assert_cmpint(pfd.revents & POLLIN, !=, 0);

    ret = uffd_read_events(c.uffd, &msg, 1);
    g_assert_cmpint(ret, ==, 1);
    g_assert_cmpint(msg.event, ==, UFFD_EVENT_PAGEFAULT);
    g_assert_cmphex(msg.arg.pagefault.address & ~(page_size - 1), ==,
                    (uintptr_t)c.addr);

    remapped = mmap(c.addr, c.len, PROT_READ | PROT_WRITE,
                    MAP_FIXED | MAP_SHARED, memfd, 0);
    g_assert_true(remapped != MAP_FAILED);
    g_assert_true(remapped == c.addr);

    ret = uffd_wakeup(c.uffd, c.addr, c.len);
    if (ret == -EINVAL) {
        ret = uffd_unregister_memory(c.uffd, c.addr, c.len);
        g_assert_cmpint(ret, ==, 0);
        g_assert_cmpint(qemu_sem_timedwait(&c.done, 10000), ==, 0);
        qemu_thread_join(&thread);
        thread_joined = true;
        g_assert_cmphex(c.value, ==, expected);
        g_test_skip("UFFDIO_WAKE after MAP_FIXED returned EINVAL");
        goto out;
    }
    g_assert_cmpint(ret, ==, 0);

    g_assert_cmpint(qemu_sem_timedwait(&c.done, 10000), ==, 0);
    qemu_thread_join(&thread);
    thread_joined = true;
    g_assert_cmphex(c.value, ==, expected);

out:
    g_assert_true(!thread_started || thread_joined);
    if (sem_inited) {
        qemu_sem_destroy(&c.done);
    }
    if (memfd >= 0) {
        close(memfd);
    }
    if (c.addr != MAP_FAILED) {
        munmap(c.addr, c.len);
    }
    if (c.uffd >= 0) {
        close(c.uffd);
    }
#endif
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/cxl/region-remap-uffd/map-fixed-wake",
                    test_map_fixed_then_uffd_wake);
    return g_test_run();
}
