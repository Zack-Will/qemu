/*
 * AnemoiM RDMA backend.
 *
 * This is the baseline fetch backend from the spec: AnemoiCache calls
 * fetch/writeback(VMID, GFN), and this layer translates it into one-sided
 * 4 KiB RDMA READ/WRITE operations against a remote AnemoiM pool.
 */

#ifndef ANEMOI_RDMA_H
#define ANEMOI_RDMA_H

#include "anemoi/backend.h"
#include "qapi/error.h"

#define ANEMOI_RDMA_DEFAULT_TCP_PORT 18515U
#define ANEMOI_RDMA_DEFAULT_IB_PORT 1U
#define ANEMOI_RDMA_DEFAULT_GID_IDX 3

typedef enum AnemoiRDMARole {
    ANEMOI_RDMA_ROLE_CLIENT = 0,
    ANEMOI_RDMA_ROLE_SERVER,
} AnemoiRDMARole;

typedef struct AnemoiRDMAConfig {
    AnemoiRDMARole role;
    const char *peer_host;
    uint16_t tcp_port;
    const char *rdma_dev;
    uint8_t ib_port;
    int gid_idx;
    uint32_t vm_capacity;
    uint64_t pages_per_vm;
} AnemoiRDMAConfig;

typedef struct AnemoiRDMAPoolConfig {
    AnemoiRDMARole role;
    const char *peer_host;
    uint16_t tcp_port;
    const char *rdma_dev;
    uint8_t ib_port;
    int gid_idx;
    uint32_t vm_capacity;
    uint64_t pages_per_vm;
} AnemoiRDMAPoolConfig;

#ifdef CONFIG_RDMA
AnemoiBackend *anemoi_rdma_backend_new(const AnemoiRDMAConfig *cfg,
                                       Error **errp);
int anemoi_rdma_pool_serve(const AnemoiRDMAPoolConfig *cfg, Error **errp);
#else
static inline AnemoiBackend *anemoi_rdma_backend_new(
    const AnemoiRDMAConfig *cfg, Error **errp)
{
    (void)cfg;
    error_setg(errp, "Anemoi RDMA backend requires CONFIG_RDMA");
    return NULL;
}

static inline int anemoi_rdma_pool_serve(const AnemoiRDMAPoolConfig *cfg,
                                         Error **errp)
{
    (void)cfg;
    error_setg(errp, "Anemoi RDMA pool requires CONFIG_RDMA");
    return -1;
}
#endif

#endif /* ANEMOI_RDMA_H */
