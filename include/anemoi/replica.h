/*
 * Degenerated Anemoi memory replica: zero-page elision + Zstd.
 */

#ifndef ANEMOI_REPLICA_H
#define ANEMOI_REPLICA_H

#include "anemoi/backend.h"
#include "qapi/error.h"

typedef struct AnemoiReplica AnemoiReplica;

typedef struct AnemoiReplicaStats {
    uint64_t nr_pages;
    uint64_t zero_pages;
    uint64_t compressed_pages;
    uint64_t payload_bytes;
    uint64_t uncompressed_bytes;
} AnemoiReplicaStats;

AnemoiReplica *anemoi_replica_new(uint64_t nr_pages, Error **errp);
void anemoi_replica_free(AnemoiReplica *replica);

int anemoi_replica_build_from_backend(AnemoiReplica *replica,
                                      AnemoiBackend *backend, uint32_t vmid,
                                      Error **errp);
int anemoi_replica_fetch_page(AnemoiReplica *replica, uint64_t gfn,
                              void *dst_4k, Error **errp);
int anemoi_replica_restore_to_backend(AnemoiReplica *replica,
                                      AnemoiBackend *backend, uint32_t vmid,
                                      Error **errp);
const AnemoiReplicaStats *anemoi_replica_stats(const AnemoiReplica *replica);

#endif /* ANEMOI_REPLICA_H */
