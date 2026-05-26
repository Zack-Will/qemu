#include "qemu/osdep.h"
#include "anemoi/backend.h"
#include "anemoi/constants.h"

static bool anemoi_backend_valid(AnemoiBackend *backend, Error **errp)
{
    if (!backend || !backend->ops) {
        error_setg(errp, "Anemoi backend is not initialized");
        return false;
    }
    if (backend->page_size != ANEMOI_PAGE_SIZE) {
        error_setg(errp, "Anemoi backend %s uses unsupported page size %" PRIu64,
                   backend->name ? backend->name : "(unnamed)",
                   backend->page_size);
        return false;
    }
    return true;
}

int anemoi_backend_fetch(AnemoiBackend *backend, uint32_t vmid, uint64_t gfn,
                         void *dst_4k, Error **errp)
{
    int ret;

    if (!anemoi_backend_valid(backend, errp)) {
        return -1;
    }
    if (!backend->ops->fetch) {
        error_setg(errp, "Anemoi backend %s has no fetch implementation",
                   backend->name ? backend->name : "(unnamed)");
        return -1;
    }
    if (!dst_4k) {
        error_setg(errp, "Anemoi fetch destination is NULL");
        return -1;
    }

    ret = backend->ops->fetch(backend, vmid, gfn, dst_4k, errp);
    if (ret == 0) {
        backend->stats.fetches++;
    } else {
        backend->stats.failed_fetches++;
    }
    return ret;
}

int anemoi_backend_writeback(AnemoiBackend *backend, uint32_t vmid,
                             uint64_t gfn, const void *src_4k, Error **errp)
{
    int ret;

    if (!anemoi_backend_valid(backend, errp)) {
        return -1;
    }
    if (!backend->ops->writeback) {
        error_setg(errp, "Anemoi backend %s has no writeback implementation",
                   backend->name ? backend->name : "(unnamed)");
        return -1;
    }
    if (!src_4k) {
        error_setg(errp, "Anemoi writeback source is NULL");
        return -1;
    }

    ret = backend->ops->writeback(backend, vmid, gfn, src_4k, errp);
    if (ret == 0) {
        backend->stats.writebacks++;
    } else {
        backend->stats.failed_writebacks++;
    }
    return ret;
}

int anemoi_backend_prefetch(AnemoiBackend *backend, uint32_t vmid,
                            uint64_t gfn, uint32_t nr_pages, Error **errp)
{
    int ret;

    if (!nr_pages) {
        return 0;
    }
    if (!anemoi_backend_valid(backend, errp)) {
        return -1;
    }
    if (!backend->ops->prefetch) {
        return 0;
    }

    ret = backend->ops->prefetch(backend, vmid, gfn, nr_pages, errp);
    if (ret == 0) {
        backend->stats.prefetches += nr_pages;
    }
    return ret;
}

void anemoi_backend_shutdown(AnemoiBackend *backend)
{
    if (!backend) {
        return;
    }
    if (backend->ops && backend->ops->shutdown) {
        backend->ops->shutdown(backend);
    }
}
