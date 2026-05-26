/*
 * Anemoi fetch backend ABI.
 *
 * The AnemoiCache miss path calls only this interface.  The baseline backend
 * is AnemoiM over RDMA ({VMID, GFN} -> 4 KiB page); the CXL comparison can
 * reuse the same seam with a CXL-backed implementation.
 */

#ifndef ANEMOI_BACKEND_H
#define ANEMOI_BACKEND_H

#include "qapi/error.h"

typedef struct AnemoiBackend AnemoiBackend;

typedef enum AnemoiBackendKind {
    ANEMOI_BACKEND_MEMORY = 0,
    ANEMOI_BACKEND_RDMA,
    ANEMOI_BACKEND_CXL,
} AnemoiBackendKind;

typedef struct AnemoiBackendStats {
    uint64_t fetches;
    uint64_t writebacks;
    uint64_t prefetches;
    uint64_t failed_fetches;
    uint64_t failed_writebacks;
} AnemoiBackendStats;

typedef struct AnemoiBackendOps {
    int (*fetch)(AnemoiBackend *backend, uint32_t vmid, uint64_t gfn,
                 void *dst_4k, Error **errp);
    int (*writeback)(AnemoiBackend *backend, uint32_t vmid, uint64_t gfn,
                     const void *src_4k, Error **errp);
    int (*prefetch)(AnemoiBackend *backend, uint32_t vmid, uint64_t gfn,
                    uint32_t nr_pages, Error **errp);
    void (*shutdown)(AnemoiBackend *backend);
} AnemoiBackendOps;

struct AnemoiBackend {
    const char *name;
    AnemoiBackendKind kind;
    uint64_t page_size;
    const AnemoiBackendOps *ops;
    void *opaque;
    AnemoiBackendStats stats;
};

int anemoi_backend_fetch(AnemoiBackend *backend, uint32_t vmid, uint64_t gfn,
                         void *dst_4k, Error **errp);
int anemoi_backend_writeback(AnemoiBackend *backend, uint32_t vmid,
                             uint64_t gfn, const void *src_4k, Error **errp);
int anemoi_backend_prefetch(AnemoiBackend *backend, uint32_t vmid,
                            uint64_t gfn, uint32_t nr_pages, Error **errp);
void anemoi_backend_shutdown(AnemoiBackend *backend);

#endif /* ANEMOI_BACKEND_H */
