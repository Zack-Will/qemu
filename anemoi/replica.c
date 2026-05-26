#include "qemu/osdep.h"
#include "anemoi/replica.h"
#include "anemoi/constants.h"
#include <zstd.h>

typedef struct AnemoiReplicaPage {
    bool zero;
    uint64_t offset;
    uint32_t compressed_len;
} AnemoiReplicaPage;

struct AnemoiReplica {
    uint64_t nr_pages;
    AnemoiReplicaPage *pages;
    GByteArray *payload;
    AnemoiReplicaStats stats;
};

static bool anemoi_page_is_zero(const uint8_t *page)
{
    for (uint64_t i = 0; i < ANEMOI_PAGE_SIZE; i++) {
        if (page[i]) {
            return false;
        }
    }
    return true;
}

static void anemoi_replica_reset(AnemoiReplica *replica)
{
    memset(replica->pages, 0,
           sizeof(*replica->pages) * (size_t)replica->nr_pages);
    g_byte_array_set_size(replica->payload, 0);
    memset(&replica->stats, 0, sizeof(replica->stats));
    replica->stats.nr_pages = replica->nr_pages;
    replica->stats.uncompressed_bytes = replica->nr_pages * ANEMOI_PAGE_SIZE;
}

AnemoiReplica *anemoi_replica_new(uint64_t nr_pages, Error **errp)
{
    AnemoiReplica *replica;

    if (!nr_pages || nr_pages > SIZE_MAX / sizeof(AnemoiReplicaPage)) {
        error_setg(errp, "invalid Anemoi replica page count: %" PRIu64,
                   nr_pages);
        return NULL;
    }

    replica = g_new0(AnemoiReplica, 1);
    replica->nr_pages = nr_pages;
    replica->pages = g_new0(AnemoiReplicaPage, (size_t)nr_pages);
    replica->payload = g_byte_array_new();
    replica->stats.nr_pages = nr_pages;
    replica->stats.uncompressed_bytes = nr_pages * ANEMOI_PAGE_SIZE;
    return replica;
}

void anemoi_replica_free(AnemoiReplica *replica)
{
    if (!replica) {
        return;
    }
    if (replica->payload) {
        g_byte_array_unref(replica->payload);
    }
    g_free(replica->pages);
    g_free(replica);
}

int anemoi_replica_build_from_backend(AnemoiReplica *replica,
                                      AnemoiBackend *backend, uint32_t vmid,
                                      Error **errp)
{
    g_autofree uint8_t *page = NULL;
    g_autofree uint8_t *compressed = NULL;
    size_t bound = ZSTD_compressBound(ANEMOI_PAGE_SIZE);

    if (!replica || !backend) {
        error_setg(errp, "invalid Anemoi replica build request");
        return -1;
    }

    page = g_malloc(ANEMOI_PAGE_SIZE);
    compressed = g_malloc(bound);
    anemoi_replica_reset(replica);

    for (uint64_t gfn = 0; gfn < replica->nr_pages; gfn++) {
        AnemoiReplicaPage *entry = &replica->pages[gfn];
        size_t ret;

        if (anemoi_backend_fetch(backend, vmid, gfn, page, errp) != 0) {
            return -1;
        }
        if (anemoi_page_is_zero(page)) {
            entry->zero = true;
            replica->stats.zero_pages++;
            continue;
        }

        ret = ZSTD_compress(compressed, bound, page, ANEMOI_PAGE_SIZE, 1);
        if (ZSTD_isError(ret)) {
            error_setg(errp, "Zstd compression failed for gfn %" PRIu64
                       ": %s", gfn, ZSTD_getErrorName(ret));
            return -1;
        }
        if (ret > UINT32_MAX) {
            error_setg(errp, "compressed Anemoi replica page is too large");
            return -1;
        }

        entry->offset = replica->payload->len;
        entry->compressed_len = (uint32_t)ret;
        g_byte_array_append(replica->payload, compressed, (guint)ret);
        replica->stats.compressed_pages++;
    }

    replica->stats.payload_bytes = replica->payload->len;
    return 0;
}

int anemoi_replica_fetch_page(AnemoiReplica *replica, uint64_t gfn,
                              void *dst_4k, Error **errp)
{
    AnemoiReplicaPage *entry;
    size_t ret;

    if (!replica || !dst_4k) {
        error_setg(errp, "invalid Anemoi replica fetch request");
        return -1;
    }
    if (gfn >= replica->nr_pages) {
        error_setg(errp, "Anemoi replica gfn %" PRIu64
                   " exceeds page count %" PRIu64, gfn, replica->nr_pages);
        return -1;
    }

    entry = &replica->pages[gfn];
    if (entry->zero) {
        memset(dst_4k, 0, ANEMOI_PAGE_SIZE);
        return 0;
    }
    if (!entry->compressed_len ||
        entry->offset + entry->compressed_len > replica->payload->len) {
        error_setg(errp, "Anemoi replica page %" PRIu64
                   " has invalid compressed extent", gfn);
        return -1;
    }

    ret = ZSTD_decompress(dst_4k, ANEMOI_PAGE_SIZE,
                          replica->payload->data + entry->offset,
                          entry->compressed_len);
    if (ZSTD_isError(ret)) {
        error_setg(errp, "Zstd decompression failed for gfn %" PRIu64
                   ": %s", gfn, ZSTD_getErrorName(ret));
        return -1;
    }
    if (ret != ANEMOI_PAGE_SIZE) {
        error_setg(errp, "Zstd decompressed %" PRIu64
                   " bytes for gfn %" PRIu64 ", expected %" PRIu64,
                   (uint64_t)ret, gfn, (uint64_t)ANEMOI_PAGE_SIZE);
        return -1;
    }
    return 0;
}

int anemoi_replica_restore_to_backend(AnemoiReplica *replica,
                                      AnemoiBackend *backend, uint32_t vmid,
                                      Error **errp)
{
    g_autofree uint8_t *page = NULL;

    if (!replica || !backend) {
        error_setg(errp, "invalid Anemoi replica restore request");
        return -1;
    }

    page = g_malloc(ANEMOI_PAGE_SIZE);
    for (uint64_t gfn = 0; gfn < replica->nr_pages; gfn++) {
        if (anemoi_replica_fetch_page(replica, gfn, page, errp) != 0) {
            return -1;
        }
        if (anemoi_backend_writeback(backend, vmid, gfn, page, errp) != 0) {
            return -1;
        }
    }
    return 0;
}

const AnemoiReplicaStats *anemoi_replica_stats(const AnemoiReplica *replica)
{
    return replica ? &replica->stats : NULL;
}
