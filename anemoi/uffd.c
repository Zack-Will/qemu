#include "qemu/osdep.h"
#include "anemoi/uffd.h"
#include "qemu/error-report.h"
#include "qemu/madvise.h"
#include "qemu/thread.h"

#ifdef CONFIG_LINUX
#include "qemu/userfaultfd.h"
#include <poll.h>
#include <sys/eventfd.h>

struct AnemoiFaultService {
    AnemoiCache *cache;
    int uffd_fd;
    int stop_fd;
    int stopping;
    int failed;
    QemuThread thread;
    AnemoiFaultRange *ranges;
    uint32_t nr_ranges;
};

static void anemoi_fault_service_report_error(AnemoiFaultService *service,
                                              Error *err)
{
    qatomic_set(&service->failed, 1);
    error_report_err(err);
}

static int anemoi_fault_service_register_range(AnemoiFaultService *service,
                                               const AnemoiFaultRange *range,
                                               bool discard_on_start,
                                               Error **errp)
{
    uint64_t ioctls;

    if (!range->host || !range->length ||
        !QEMU_IS_ALIGNED((uintptr_t)range->host, ANEMOI_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(range->length, ANEMOI_PAGE_SIZE)) {
        error_setg(errp, "invalid Anemoi userfaultfd range %s",
                   range->idstr ? range->idstr : "(unnamed)");
        return -1;
    }

    qemu_madvise(range->host, range->length, QEMU_MADV_NOHUGEPAGE);
    if (uffd_register_memory(service->uffd_fd, range->host, range->length,
                             UFFDIO_REGISTER_MODE_MISSING |
                             UFFDIO_REGISTER_MODE_WP, &ioctls) != 0) {
        error_setg_errno(errp, errno,
                         "failed to register Anemoi userfaultfd range %s",
                         range->idstr ? range->idstr : "(unnamed)");
        return -1;
    }
    if (!(ioctls & (1ULL << _UFFDIO_COPY))) {
        error_setg(errp, "Anemoi userfaultfd range %s does not support COPY",
                   range->idstr ? range->idstr : "(unnamed)");
        return -1;
    }
    if (!(ioctls & (1ULL << _UFFDIO_WRITEPROTECT))) {
        error_setg(errp, "Anemoi userfaultfd range %s lacks WP support",
                   range->idstr ? range->idstr : "(unnamed)");
        return -1;
    }
    if (discard_on_start &&
        qemu_madvise(range->host, range->length, QEMU_MADV_DONTNEED) != 0) {
        error_setg_errno(errp, errno,
                         "failed to discard Anemoi userfaultfd range %s",
                         range->idstr ? range->idstr : "(unnamed)");
        return -1;
    }
    return 0;
}

static void anemoi_fault_service_unregister_ranges(AnemoiFaultService *service)
{
    for (uint32_t i = 0; i < service->nr_ranges; i++) {
        AnemoiFaultRange *range = &service->ranges[i];

        if (range->host && range->length) {
            uffd_unregister_memory(service->uffd_fd, range->host,
                                   range->length);
            qemu_madvise(range->host, range->length, QEMU_MADV_HUGEPAGE);
        }
    }
}

static void anemoi_fault_service_handle_msg(AnemoiFaultService *service,
                                            const struct uffd_msg *msg)
{
    Error *local_err = NULL;
    uintptr_t fault_addr;
    bool is_write;

    if (msg->event != UFFD_EVENT_PAGEFAULT) {
        error_report("Anemoi userfaultfd received unexpected event %u",
                     msg->event);
        qatomic_set(&service->failed, 1);
        return;
    }

    fault_addr = msg->arg.pagefault.address;
    is_write = msg->arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE;
    if (anemoi_cache_handle_fault(service->cache, fault_addr, is_write,
                                  msg->arg.pagefault.flags &
                                  UFFD_PAGEFAULT_FLAG_WP,
                                  &local_err) != 0) {
        anemoi_fault_service_report_error(service, local_err);
    }
}

static void anemoi_fault_service_drain_stop_fd(AnemoiFaultService *service)
{
    uint64_t value;

    while (read(service->stop_fd, &value, sizeof(value)) == sizeof(value)) {
        /* Drain all pending notifications. */
    }
}

static void *anemoi_fault_service_thread(void *opaque)
{
    AnemoiFaultService *service = opaque;
    struct pollfd pollfds[2] = {
        { .fd = service->uffd_fd, .events = POLLIN },
        { .fd = service->stop_fd, .events = POLLIN },
    };

    while (!qatomic_read(&service->stopping)) {
        int ret = poll(pollfds, G_N_ELEMENTS(pollfds), -1);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_report("Anemoi userfaultfd poll failed: %s",
                         strerror(errno));
            qatomic_set(&service->failed, 1);
            break;
        }
        if (pollfds[1].revents & POLLIN) {
            anemoi_fault_service_drain_stop_fd(service);
            break;
        }
        if (pollfds[0].revents & POLLIN) {
            struct uffd_msg msgs[16];
            int nr;

            do {
                nr = uffd_read_events(service->uffd_fd, msgs,
                                      G_N_ELEMENTS(msgs));
                if (nr < 0) {
                    qatomic_set(&service->failed, 1);
                    break;
                }
                for (int i = 0; i < nr; i++) {
                    anemoi_fault_service_handle_msg(service, &msgs[i]);
                }
            } while (nr == G_N_ELEMENTS(msgs));
        }
    }
    return NULL;
}

AnemoiFaultService *anemoi_fault_service_start(
    const AnemoiFaultServiceConfig *cfg, Error **errp)
{
    AnemoiFaultService *service;

    if (!cfg || !cfg->cache || !cfg->ranges || !cfg->nr_ranges) {
        error_setg(errp, "invalid Anemoi userfaultfd service configuration");
        return NULL;
    }

    service = g_new0(AnemoiFaultService, 1);
    service->cache = cfg->cache;
    service->uffd_fd = -1;
    service->stop_fd = -1;
    service->nr_ranges = cfg->nr_ranges;
    service->ranges = g_new(AnemoiFaultRange, cfg->nr_ranges);
    memcpy(service->ranges, cfg->ranges,
           sizeof(*service->ranges) * cfg->nr_ranges);

    service->uffd_fd = uffd_create_fd(UFFD_FEATURE_PAGEFAULT_FLAG_WP, true);
    if (service->uffd_fd < 0) {
        error_setg_errno(errp, errno, "failed to create Anemoi userfaultfd");
        goto fail;
    }

    service->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (service->stop_fd < 0) {
        error_setg_errno(errp, errno,
                         "failed to create Anemoi userfaultfd stop eventfd");
        goto fail;
    }

    for (uint32_t i = 0; i < service->nr_ranges; i++) {
        if (anemoi_fault_service_register_range(service, &service->ranges[i],
                                                cfg->discard_on_start,
                                                errp) != 0) {
            goto fail_unregister;
        }
    }

    anemoi_cache_attach_uffd(service->cache, service->uffd_fd);
    qemu_thread_create(&service->thread, "anemoi-uffd",
                       anemoi_fault_service_thread, service,
                       QEMU_THREAD_JOINABLE);
    return service;

fail_unregister:
        anemoi_fault_service_unregister_ranges(service);
fail:
    if (service->stop_fd >= 0) {
        close(service->stop_fd);
    }
    if (service->uffd_fd >= 0) {
        close(service->uffd_fd);
    }
    g_free(service->ranges);
    g_free(service);
    return NULL;
}

void anemoi_fault_service_stop(AnemoiFaultService *service)
{
    uint64_t one = 1;

    if (!service) {
        return;
    }
    qatomic_set(&service->stopping, 1);
    if (service->stop_fd >= 0) {
        if (write(service->stop_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
            error_report("failed to stop Anemoi userfaultfd thread: %s",
                         strerror(errno));
        }
    }
    qemu_thread_join(&service->thread);
    anemoi_cache_attach_uffd(service->cache, -1);
    anemoi_fault_service_unregister_ranges(service);

    if (service->stop_fd >= 0) {
        close(service->stop_fd);
    }
    if (service->uffd_fd >= 0) {
        close(service->uffd_fd);
    }
    g_free(service->ranges);
    g_free(service);
}

int anemoi_fault_service_fd(const AnemoiFaultService *service)
{
    return service ? service->uffd_fd : -1;
}

bool anemoi_fault_service_failed(const AnemoiFaultService *service)
{
    return service && qatomic_read(&service->failed);
}

#else /* CONFIG_LINUX */

struct AnemoiFaultService {
    int unused;
};

AnemoiFaultService *anemoi_fault_service_start(
    const AnemoiFaultServiceConfig *cfg, Error **errp)
{
    (void)cfg;
    error_setg(errp, "Anemoi userfaultfd service requires Linux");
    return NULL;
}

void anemoi_fault_service_stop(AnemoiFaultService *service)
{
    g_free(service);
}

int anemoi_fault_service_fd(const AnemoiFaultService *service)
{
    (void)service;
    return -1;
}

bool anemoi_fault_service_failed(const AnemoiFaultService *service)
{
    (void)service;
    return true;
}

#endif /* CONFIG_LINUX */
