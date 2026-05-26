/*
 * AnemoiCache metadata and miss-path core.
 */

#ifndef ANEMOI_CACHE_H
#define ANEMOI_CACHE_H

#include "anemoi/backend.h"
#include "anemoi/constants.h"
#include "qapi/error.h"

typedef struct AnemoiCache AnemoiCache;
typedef struct AnemoiRMap AnemoiRMap;

typedef struct AnemoiCacheRegion {
    uintptr_t hva_base;
    uint64_t hva_size;
    uint64_t gfn_base;
} AnemoiCacheRegion;

typedef struct AnemoiCacheConfig {
    uint32_t vmid;
    uint64_t guest_pages;
    uint64_t local_cache_pages;
    uintptr_t hva_base;
    uint64_t hva_size;
    const AnemoiCacheRegion *regions;
    uint32_t nr_regions;
    AnemoiBackend *backend;
    AnemoiRMap *rmap;
} AnemoiCacheConfig;

typedef struct AnemoiCacheStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t dirty_writebacks;
    uint64_t install_errors;
} AnemoiCacheStats;

typedef struct AnemoiCacheLineSnapshot {
    uint64_t gfn;
    uint8_t valid;
    uint8_t dirty;
    uint8_t pinned;
    uint64_t lruk_ts[ANEMOI_LRU_K];
} AnemoiCacheLineSnapshot;

typedef struct AnemoiCacheMetadata {
    uint32_t ways;
    uint64_t num_sets;
    uint64_t nr_lines;
    AnemoiCacheLineSnapshot *lines;
} AnemoiCacheMetadata;

AnemoiCache *anemoi_cache_new(const AnemoiCacheConfig *cfg, Error **errp);
void anemoi_cache_free(AnemoiCache *cache);

uint64_t anemoi_cache_num_sets(const AnemoiCache *cache);
uint64_t anemoi_cache_guest_pages(const AnemoiCache *cache);
uint64_t anemoi_cache_local_pages(const AnemoiCache *cache);
const AnemoiCacheStats *anemoi_cache_stats(const AnemoiCache *cache);
void anemoi_cache_attach_uffd(AnemoiCache *cache, int uffd_fd);
int anemoi_cache_reset_rmap_clean(AnemoiCache *cache, Error **errp);

int anemoi_cache_install_page(AnemoiCache *cache, uint64_t gfn, void *hva_4k,
                              bool is_write, Error **errp);
int anemoi_cache_handle_fault(AnemoiCache *cache, uintptr_t fault_addr,
                              bool is_write, bool is_wp, Error **errp);
int anemoi_cache_mark_dirty(AnemoiCache *cache, uint64_t gfn, bool dirty,
                            Error **errp);
int anemoi_cache_flush_all(AnemoiCache *cache, Error **errp);

int anemoi_cache_export_metadata(AnemoiCache *cache,
                                 AnemoiCacheMetadata *metadata, Error **errp);
void anemoi_cache_metadata_destroy(AnemoiCacheMetadata *metadata);

#endif /* ANEMOI_CACHE_H */
