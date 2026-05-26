#include "qemu/osdep.h"
#include "anemoi/cache.h"
#include "anemoi/rmap.h"
#include "qemu/madvise.h"
#include "qemu/thread.h"

#ifdef CONFIG_LINUX
#include "qemu/userfaultfd.h"
#endif

typedef struct AnemoiCacheLine {
    uint64_t gfn;
    bool valid;
    bool dirty;
    bool pinned;
    uint64_t lruk_ts[ANEMOI_LRU_K];
    void *hva;
} AnemoiCacheLine;

typedef struct AnemoiCacheSet {
    AnemoiCacheLine *ways;
    QemuMutex lock;
} AnemoiCacheSet;

struct AnemoiCache {
    uint32_t vmid;
    uint64_t guest_pages;
    uint64_t local_cache_pages;
    uint64_t num_sets;
    AnemoiCacheRegion *regions;
    uint32_t nr_regions;
    bool uffd_wp_enabled;
    AnemoiBackend *backend;
    AnemoiRMap *rmap;
    AnemoiCacheSet *sets;
    int uffd_fd;
    uint64_t clock;
    AnemoiCacheStats stats;
};

static void anemoi_cache_update_rmap_dirty(AnemoiCache *cache,
                                           uint64_t gfn, bool dirty)
{
    if (!dirty || !cache->rmap) {
        return;
    }
    anemoi_rmap_clear(cache->rmap, gfn);
}

static void anemoi_cache_set_line_dirty(AnemoiCache *cache,
                                        AnemoiCacheLine *line, bool dirty)
{
    if (line->dirty != dirty) {
        line->dirty = dirty;
    }
    anemoi_cache_update_rmap_dirty(cache, line->gfn, dirty);
}

static uint64_t anemoi_cache_next_clock(AnemoiCache *cache)
{
    return qatomic_fetch_inc(&cache->clock) + 1;
}

static void anemoi_cache_bump_lruk(AnemoiCache *cache,
                                   AnemoiCacheLine *line)
{
    for (int i = ANEMOI_LRU_K - 1; i > 0; i--) {
        line->lruk_ts[i] = line->lruk_ts[i - 1];
    }
    line->lruk_ts[0] = anemoi_cache_next_clock(cache);
}

static AnemoiCacheSet *anemoi_cache_set_for_gfn(AnemoiCache *cache,
                                                uint64_t gfn)
{
    return &cache->sets[gfn % cache->num_sets];
}

static int anemoi_cache_lookup_way(AnemoiCacheSet *set, uint64_t gfn)
{
    for (uint32_t i = 0; i < ANEMOI_CACHE_WAYS; i++) {
        AnemoiCacheLine *line = &set->ways[i];

        if (line->valid && line->gfn == gfn) {
            return (int)i;
        }
    }
    return -1;
}

static int anemoi_cache_pick_victim(AnemoiCacheSet *set, Error **errp)
{
    uint64_t best_key = UINT64_MAX;
    int best = -1;

    for (uint32_t i = 0; i < ANEMOI_CACHE_WAYS; i++) {
        AnemoiCacheLine *line = &set->ways[i];
        uint64_t key;

        if (!line->valid) {
            return (int)i;
        }
        if (line->pinned) {
            continue;
        }
        key = line->lruk_ts[ANEMOI_LRU_K - 1];
        if (key < best_key) {
            best_key = key;
            best = (int)i;
        }
    }

    if (best < 0) {
        error_setg(errp, "all AnemoiCache ways in set are pinned");
    }
    return best;
}

static int anemoi_cache_writeback_line(AnemoiCache *cache,
                                       AnemoiCacheLine *line,
                                       Error **errp)
{
    if (!line->valid || !line->dirty) {
        return 0;
    }
    if (!line->hva) {
        error_setg(errp, "dirty AnemoiCache line gfn=%" PRIu64
                   " has no HVA for writeback", line->gfn);
        return -1;
    }
    if (anemoi_backend_writeback(cache->backend, cache->vmid, line->gfn,
                                 line->hva, errp) != 0) {
        return -1;
    }
    cache->stats.dirty_writebacks++;
    anemoi_cache_set_line_dirty(cache, line, false);
    return 0;
}

static int anemoi_cache_discard_line(AnemoiCache *cache,
                                     AnemoiCacheLine *line,
                                     Error **errp)
{
    if (!line->valid || !line->hva || cache->uffd_fd < 0) {
        return 0;
    }
    if (qemu_madvise(line->hva, ANEMOI_PAGE_SIZE, QEMU_MADV_DONTNEED) != 0) {
        error_setg_errno(errp, errno,
                         "failed to discard evicted AnemoiCache page gfn=%"
                         PRIu64, line->gfn);
        return -1;
    }
    return 0;
}

static int anemoi_cache_copy_to_hva(AnemoiCache *cache, void *hva_4k,
                                    void *src_4k, Error **errp)
{
#ifdef CONFIG_LINUX
    if (cache->uffd_fd >= 0) {
        int ret = uffd_copy_page(cache->uffd_fd, hva_4k, src_4k,
                                 ANEMOI_PAGE_SIZE, false);

        if (ret == 0) {
            return 0;
        }
        if (ret == -EEXIST) {
            ret = uffd_wakeup(cache->uffd_fd, hva_4k, ANEMOI_PAGE_SIZE);
            if (ret == 0) {
                return 0;
            }
        }
        error_setg(errp, "UFFDIO_COPY failed for AnemoiCache HVA %p", hva_4k);
        return -1;
    }
#else
    if (cache->uffd_fd >= 0) {
        error_setg(errp, "AnemoiCache userfaultfd is unavailable on this host");
        return -1;
    }
#endif

    memcpy(hva_4k, src_4k, ANEMOI_PAGE_SIZE);
    return 0;
}

static int anemoi_cache_protect_hva(AnemoiCache *cache, void *hva_4k,
                                    Error **errp)
{
#ifdef CONFIG_LINUX
    if (cache->uffd_wp_enabled &&
        uffd_change_protection(cache->uffd_fd, hva_4k, ANEMOI_PAGE_SIZE,
                               true, false) != 0) {
        error_setg(errp, "UFFDIO_WRITEPROTECT failed for AnemoiCache HVA %p",
                   hva_4k);
        return -1;
    }
#else
    if (cache->uffd_wp_enabled) {
        error_setg(errp, "AnemoiCache userfaultfd is unavailable on this host");
        return -1;
    }
#endif
    return 0;
}

static int anemoi_cache_unprotect_hva(AnemoiCache *cache, void *hva_4k,
                                      Error **errp)
{
#ifdef CONFIG_LINUX
    if (cache->uffd_wp_enabled &&
        uffd_change_protection(cache->uffd_fd, hva_4k, ANEMOI_PAGE_SIZE,
                               false, false) != 0) {
        error_setg(errp, "UFFDIO_WRITEPROTECT clear failed for AnemoiCache HVA %p",
                   hva_4k);
        return -1;
    }
#else
    if (cache->uffd_wp_enabled) {
        error_setg(errp, "AnemoiCache userfaultfd is unavailable on this host");
        return -1;
    }
#endif
    return 0;
}

static int anemoi_cache_wake_hva(AnemoiCache *cache, void *hva_4k,
                                 Error **errp)
{
#ifdef CONFIG_LINUX
    if (cache->uffd_fd >= 0 &&
        uffd_wakeup(cache->uffd_fd, hva_4k, ANEMOI_PAGE_SIZE) != 0) {
        error_setg(errp, "UFFDIO_WAKE failed for AnemoiCache HVA %p", hva_4k);
        return -1;
    }
#else
    if (cache->uffd_fd >= 0) {
        error_setg(errp, "AnemoiCache userfaultfd is unavailable on this host");
        return -1;
    }
#endif
    return 0;
}

static bool anemoi_cache_gfn_valid(AnemoiCache *cache, uint64_t gfn,
                                   Error **errp)
{
    if (gfn >= cache->guest_pages) {
        error_setg(errp, "gfn %" PRIu64 " exceeds guest page count %" PRIu64,
                   gfn, cache->guest_pages);
        return false;
    }
    return true;
}

static bool anemoi_cache_region_valid(const AnemoiCacheRegion *region,
                                      uint64_t guest_pages, Error **errp)
{
    uint64_t region_pages;

    if (!region->hva_base || !region->hva_size ||
        !QEMU_IS_ALIGNED(region->hva_base, ANEMOI_PAGE_SIZE) ||
        !QEMU_IS_ALIGNED(region->hva_size, ANEMOI_PAGE_SIZE)) {
        error_setg(errp, "invalid AnemoiCache HVA region");
        return false;
    }

    region_pages = region->hva_size / ANEMOI_PAGE_SIZE;
    if (region->gfn_base >= guest_pages ||
        region_pages > guest_pages - region->gfn_base) {
        error_setg(errp, "AnemoiCache region gfn range [%" PRIu64
                   ", %" PRIu64 ") exceeds guest page count %" PRIu64,
                   region->gfn_base, region->gfn_base + region_pages,
                   guest_pages);
        return false;
    }
    return true;
}

static int anemoi_cache_copy_regions(AnemoiCache *cache,
                                     const AnemoiCacheConfig *cfg,
                                     Error **errp)
{
    if (cfg->nr_regions) {
        if (!cfg->regions) {
            error_setg(errp, "AnemoiCache region count without regions");
            return -1;
        }
        cache->regions = g_new(AnemoiCacheRegion, cfg->nr_regions);
        cache->nr_regions = cfg->nr_regions;
        memcpy(cache->regions, cfg->regions,
               sizeof(*cache->regions) * cfg->nr_regions);
    } else {
        AnemoiCacheRegion region = {
            .hva_base = cfg->hva_base,
            .hva_size = cfg->hva_size,
            .gfn_base = 0,
        };

        cache->regions = g_new(AnemoiCacheRegion, 1);
        cache->regions[0] = region;
        cache->nr_regions = 1;
    }

    for (uint32_t i = 0; i < cache->nr_regions; i++) {
        if (!anemoi_cache_region_valid(&cache->regions[i],
                                       cfg->guest_pages, errp)) {
            return -1;
        }
    }
    return 0;
}

static bool anemoi_cache_fault_to_gfn(AnemoiCache *cache, uintptr_t page,
                                      uint64_t *gfn)
{
    for (uint32_t i = 0; i < cache->nr_regions; i++) {
        AnemoiCacheRegion *region = &cache->regions[i];

        if (page >= region->hva_base &&
            page - region->hva_base < region->hva_size) {
            *gfn = region->gfn_base +
                   (page - region->hva_base) / ANEMOI_PAGE_SIZE;
            return true;
        }
    }
    return false;
}

AnemoiCache *anemoi_cache_new(const AnemoiCacheConfig *cfg, Error **errp)
{
    AnemoiCache *cache;
    uint64_t num_sets;

    if (!cfg || !cfg->backend || !cfg->guest_pages ||
        !cfg->local_cache_pages) {
        error_setg(errp, "invalid AnemoiCache configuration");
        return NULL;
    }
    if (cfg->local_cache_pages > cfg->guest_pages) {
        error_setg(errp, "AnemoiCache local pages exceed guest pages");
        return NULL;
    }

    num_sets = cfg->local_cache_pages / ANEMOI_CACHE_WAYS;
    if (cfg->local_cache_pages % ANEMOI_CACHE_WAYS) {
        num_sets++;
    }
    num_sets = MAX(num_sets, 1);

    cache = g_new0(AnemoiCache, 1);
    cache->vmid = cfg->vmid;
    cache->guest_pages = cfg->guest_pages;
    cache->local_cache_pages = cfg->local_cache_pages;
    cache->num_sets = num_sets;
    cache->backend = cfg->backend;
    cache->rmap = cfg->rmap;
    cache->uffd_fd = -1;

    if (anemoi_cache_copy_regions(cache, cfg, errp) != 0) {
        g_free(cache);
        return NULL;
    }

    cache->sets = g_new0(AnemoiCacheSet, num_sets);

    for (uint64_t i = 0; i < num_sets; i++) {
        cache->sets[i].ways = g_new0(AnemoiCacheLine, ANEMOI_CACHE_WAYS);
        qemu_mutex_init(&cache->sets[i].lock);
    }
    return cache;
}

void anemoi_cache_free(AnemoiCache *cache)
{
    if (!cache) {
        return;
    }
    if (cache->sets) {
        for (uint64_t i = 0; i < cache->num_sets; i++) {
            qemu_mutex_destroy(&cache->sets[i].lock);
            g_free(cache->sets[i].ways);
        }
    }
    g_free(cache->regions);
    g_free(cache->sets);
    g_free(cache);
}

uint64_t anemoi_cache_num_sets(const AnemoiCache *cache)
{
    return cache ? cache->num_sets : 0;
}

uint64_t anemoi_cache_guest_pages(const AnemoiCache *cache)
{
    return cache ? cache->guest_pages : 0;
}

uint64_t anemoi_cache_local_pages(const AnemoiCache *cache)
{
    return cache ? cache->local_cache_pages : 0;
}

const AnemoiCacheStats *anemoi_cache_stats(const AnemoiCache *cache)
{
    return cache ? &cache->stats : NULL;
}

void anemoi_cache_attach_uffd(AnemoiCache *cache, int uffd_fd)
{
    if (cache) {
        cache->uffd_fd = uffd_fd;
        cache->uffd_wp_enabled = uffd_fd >= 0;
    }
}

int anemoi_cache_reset_rmap_clean(AnemoiCache *cache, Error **errp)
{
    if (!cache) {
        error_setg(errp, "AnemoiCache is not initialized");
        return -1;
    }
    if (!cache->rmap) {
        error_setg(errp, "AnemoiCache has no rmap to reset");
        return -1;
    }

    anemoi_rmap_fill(cache->rmap);
    return 0;
}

int anemoi_cache_install_page(AnemoiCache *cache, uint64_t gfn, void *hva_4k,
                              bool is_write, Error **errp)
{
    g_autofree uint8_t *landing = NULL;
    AnemoiCacheSet *set;
    AnemoiCacheLine *line;
    int way;

    if (!cache || !hva_4k) {
        error_setg(errp, "invalid AnemoiCache install request");
        return -1;
    }
    if (!anemoi_cache_gfn_valid(cache, gfn, errp)) {
        return -1;
    }

    set = anemoi_cache_set_for_gfn(cache, gfn);
    qemu_mutex_lock(&set->lock);

    way = anemoi_cache_lookup_way(set, gfn);
    if (way >= 0) {
        line = &set->ways[way];
        if (is_write) {
            anemoi_cache_set_line_dirty(cache, line, true);
        }
        line->hva = hva_4k;
        anemoi_cache_bump_lruk(cache, line);
        cache->stats.hits++;
        if (is_write &&
            anemoi_cache_unprotect_hva(cache, hva_4k, errp) != 0) {
            cache->stats.install_errors++;
            qemu_mutex_unlock(&set->lock);
            return -1;
        }
        if (anemoi_cache_wake_hva(cache, hva_4k, errp) != 0) {
            cache->stats.install_errors++;
            qemu_mutex_unlock(&set->lock);
            return -1;
        }
        qemu_mutex_unlock(&set->lock);
        return 0;
    }

    cache->stats.misses++;
    way = anemoi_cache_pick_victim(set, errp);
    if (way < 0) {
        qemu_mutex_unlock(&set->lock);
        return -1;
    }
    line = &set->ways[way];
    line->pinned = true;
    if (anemoi_cache_writeback_line(cache, line, errp) != 0) {
        line->pinned = false;
        cache->stats.install_errors++;
        qemu_mutex_unlock(&set->lock);
        return -1;
    }
    if (anemoi_cache_discard_line(cache, line, errp) != 0) {
        line->pinned = false;
        cache->stats.install_errors++;
        qemu_mutex_unlock(&set->lock);
        return -1;
    }
    if (line->valid) {
        cache->stats.evictions++;
    }

    /* Once the old HVA is discarded, never leave stale-valid metadata behind. */
    memset(line, 0, sizeof(*line));

    landing = g_malloc(ANEMOI_PAGE_SIZE);
    if (anemoi_backend_fetch(cache->backend, cache->vmid, gfn, landing,
                             errp) != 0) {
        cache->stats.install_errors++;
        qemu_mutex_unlock(&set->lock);
        return -1;
    }
    if (anemoi_cache_copy_to_hva(cache, hva_4k, landing, errp) != 0) {
        cache->stats.install_errors++;
        qemu_mutex_unlock(&set->lock);
        return -1;
    }
    if (!is_write &&
        anemoi_cache_protect_hva(cache, hva_4k, errp) != 0) {
        cache->stats.install_errors++;
        qemu_mutex_unlock(&set->lock);
        return -1;
    }

    line->gfn = gfn;
    line->valid = true;
    line->hva = hva_4k;
    anemoi_cache_set_line_dirty(cache, line, is_write);
    anemoi_cache_bump_lruk(cache, line);
    qemu_mutex_unlock(&set->lock);
    return 0;
}

int anemoi_cache_handle_fault(AnemoiCache *cache, uintptr_t fault_addr,
                              bool is_write, bool is_wp, Error **errp)
{
    uintptr_t page = QEMU_ALIGN_DOWN(fault_addr, ANEMOI_PAGE_SIZE);
    uint64_t gfn;

    if (!cache || !cache->nr_regions) {
        error_setg(errp, "AnemoiCache HVA range is not configured");
        return -1;
    }
    if (!anemoi_cache_fault_to_gfn(cache, page, &gfn)) {
        error_setg(errp, "fault address 0x%" PRIxPTR
                   " outside AnemoiCache HVA range", fault_addr);
        return -1;
    }
    return anemoi_cache_install_page(cache, gfn, (void *)page,
                                     is_write || is_wp, errp);
}

int anemoi_cache_mark_dirty(AnemoiCache *cache, uint64_t gfn, bool dirty,
                            Error **errp)
{
    AnemoiCacheSet *set;
    int way;

    if (!cache || !anemoi_cache_gfn_valid(cache, gfn, errp)) {
        return -1;
    }

    set = anemoi_cache_set_for_gfn(cache, gfn);
    qemu_mutex_lock(&set->lock);
    way = anemoi_cache_lookup_way(set, gfn);
    if (way >= 0) {
        anemoi_cache_set_line_dirty(cache, &set->ways[way], dirty);
        anemoi_cache_bump_lruk(cache, &set->ways[way]);
    } else {
        anemoi_cache_update_rmap_dirty(cache, gfn, dirty);
    }
    qemu_mutex_unlock(&set->lock);
    return 0;
}

int anemoi_cache_flush_all(AnemoiCache *cache, Error **errp)
{
    if (!cache) {
        error_setg(errp, "AnemoiCache is not initialized");
        return -1;
    }

    for (uint64_t i = 0; i < cache->num_sets; i++) {
        AnemoiCacheSet *set = &cache->sets[i];

        qemu_mutex_lock(&set->lock);
        for (uint32_t w = 0; w < ANEMOI_CACHE_WAYS; w++) {
            if (anemoi_cache_writeback_line(cache, &set->ways[w], errp) != 0) {
                qemu_mutex_unlock(&set->lock);
                return -1;
            }
        }
        qemu_mutex_unlock(&set->lock);
    }
    return 0;
}

static int anemoi_cache_validate_metadata(AnemoiCache *cache,
                                          const AnemoiCacheMetadata *metadata,
                                          Error **errp)
{
    uint64_t expected_lines;
    uint64_t pos = 0;
    GHashTable *seen;

    if (!cache || !metadata) {
        error_setg(errp, "invalid AnemoiCache metadata import request");
        return -1;
    }
    if (metadata->ways != ANEMOI_CACHE_WAYS) {
        error_setg(errp, "AnemoiCache metadata ways mismatch: got %u expected %u",
                   metadata->ways, ANEMOI_CACHE_WAYS);
        return -1;
    }
    if (metadata->num_sets != cache->num_sets) {
        error_setg(errp, "AnemoiCache metadata set count mismatch: got %" PRIu64
                   " expected %" PRIu64, metadata->num_sets, cache->num_sets);
        return -1;
    }
    if (metadata->num_sets > UINT64_MAX / metadata->ways) {
        error_setg(errp, "AnemoiCache metadata line count overflows");
        return -1;
    }

    expected_lines = metadata->num_sets * metadata->ways;
    if (metadata->nr_lines != expected_lines) {
        error_setg(errp, "AnemoiCache metadata line count mismatch: got %" PRIu64
                   " expected %" PRIu64, metadata->nr_lines, expected_lines);
        return -1;
    }
    if (metadata->nr_lines && !metadata->lines) {
        error_setg(errp, "AnemoiCache metadata has no line array");
        return -1;
    }

    seen = g_hash_table_new(g_int64_hash, g_int64_equal);
    for (uint64_t i = 0; i < metadata->num_sets; i++) {
        for (uint32_t w = 0; w < metadata->ways; w++, pos++) {
            const AnemoiCacheLineSnapshot *snap = &metadata->lines[pos];

            if (snap->valid > 1 || snap->dirty > 1 || snap->pinned > 1) {
                error_setg(errp, "AnemoiCache metadata line %" PRIu64
                           " has non-boolean flags", pos);
                goto fail;
            }
            if (!snap->valid) {
                if (snap->dirty || snap->pinned) {
                    error_setg(errp, "AnemoiCache metadata line %" PRIu64
                               " has invalid-line state", pos);
                    goto fail;
                }
                continue;
            }
            if (!anemoi_cache_gfn_valid(cache, snap->gfn, errp)) {
                goto fail;
            }
            if (snap->gfn % cache->num_sets != i) {
                error_setg(errp, "AnemoiCache metadata line %" PRIu64
                           " gfn=%" PRIu64 " belongs to set %" PRIu64
                           ", not set %" PRIu64, pos, snap->gfn,
                           snap->gfn % cache->num_sets, i);
                goto fail;
            }
            if (g_hash_table_contains(seen, &snap->gfn)) {
                error_setg(errp, "AnemoiCache metadata duplicates gfn=%" PRIu64,
                           snap->gfn);
                goto fail;
            }
            g_hash_table_add(seen, (gpointer)&snap->gfn);
        }
    }

    g_hash_table_destroy(seen);
    return 0;

fail:
    g_hash_table_destroy(seen);
    return -1;
}

int anemoi_cache_export_metadata(AnemoiCache *cache,
                                 AnemoiCacheMetadata *metadata, Error **errp)
{
    uint64_t nr_lines;
    uint64_t pos = 0;

    if (!cache || !metadata) {
        error_setg(errp, "invalid AnemoiCache metadata export request");
        return -1;
    }
    nr_lines = cache->num_sets * ANEMOI_CACHE_WAYS;
    if (nr_lines > SIZE_MAX / sizeof(*metadata->lines)) {
        error_setg(errp, "AnemoiCache metadata is too large");
        return -1;
    }

    memset(metadata, 0, sizeof(*metadata));
    metadata->ways = ANEMOI_CACHE_WAYS;
    metadata->num_sets = cache->num_sets;
    metadata->nr_lines = nr_lines;
    metadata->lines = g_new0(AnemoiCacheLineSnapshot, nr_lines);

    for (uint64_t i = 0; i < cache->num_sets; i++) {
        AnemoiCacheSet *set = &cache->sets[i];

        qemu_mutex_lock(&set->lock);
        for (uint32_t w = 0; w < ANEMOI_CACHE_WAYS; w++, pos++) {
            AnemoiCacheLine *line = &set->ways[w];
            AnemoiCacheLineSnapshot *snap = &metadata->lines[pos];

            snap->gfn = line->gfn;
            snap->valid = line->valid;
            snap->dirty = line->dirty;
            snap->pinned = line->pinned;
            memcpy(snap->lruk_ts, line->lruk_ts, sizeof(snap->lruk_ts));
        }
        qemu_mutex_unlock(&set->lock);
    }
    return 0;
}

int anemoi_cache_import_metadata(AnemoiCache *cache,
                                 const AnemoiCacheMetadata *metadata,
                                 Error **errp)
{
    if (anemoi_cache_validate_metadata(cache, metadata, errp) != 0) {
        return -1;
    }

    /*
     * P2 receives source cache metadata before the destination has local HVA
     * copies.  Until metadata has a separate materialized bit, importing
     * valid lines would turn first destination UFFD misses into false hits.
     */
    for (uint64_t i = 0; i < cache->num_sets; i++) {
        AnemoiCacheSet *set = &cache->sets[i];

        qemu_mutex_lock(&set->lock);
        memset(set->ways, 0, sizeof(*set->ways) * ANEMOI_CACHE_WAYS);
        qemu_mutex_unlock(&set->lock);
    }
    qatomic_set(&cache->clock, 0);
    return 0;
}

void anemoi_cache_metadata_destroy(AnemoiCacheMetadata *metadata)
{
    if (!metadata) {
        return;
    }
    g_free(metadata->lines);
    memset(metadata, 0, sizeof(*metadata));
}
