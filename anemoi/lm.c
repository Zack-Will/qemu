#include "qemu/osdep.h"
#include "anemoi/lm.h"

struct AnemoiLM {
    uint32_t vmid;
    uint64_t guest_pages;
    AnemoiCache *cache;
    AnemoiRMap *rmap;
};

AnemoiLM *anemoi_lm_new(const AnemoiLMConfig *cfg, Error **errp)
{
    AnemoiLM *lm;

    if (!cfg || !cfg->cache || !cfg->rmap || !cfg->guest_pages) {
        error_setg(errp, "invalid AnemoiLM configuration");
        return NULL;
    }
    if (anemoi_cache_guest_pages(cfg->cache) != cfg->guest_pages ||
        anemoi_rmap_nr_pages(cfg->rmap) != cfg->guest_pages) {
        error_setg(errp, "AnemoiLM cache/rmap page counts do not match");
        return NULL;
    }

    lm = g_new0(AnemoiLM, 1);
    lm->vmid = cfg->vmid;
    lm->guest_pages = cfg->guest_pages;
    lm->cache = cfg->cache;
    lm->rmap = cfg->rmap;
    return lm;
}

void anemoi_lm_free(AnemoiLM *lm)
{
    g_free(lm);
}

int anemoi_lm_mark_dirty_range(AnemoiLM *lm, uint64_t start_gfn,
                               uint64_t nr_pages, Error **errp)
{
    if (!lm) {
        error_setg(errp, "AnemoiLM is not initialized");
        return -1;
    }
    if (start_gfn >= lm->guest_pages ||
        nr_pages > lm->guest_pages - start_gfn) {
        error_setg(errp, "AnemoiLM dirty range [%" PRIu64 ", %" PRIu64
                   ") exceeds guest page count %" PRIu64,
                   start_gfn, start_gfn + nr_pages, lm->guest_pages);
        return -1;
    }

    anemoi_rmap_clear_range(lm->rmap, start_gfn, nr_pages);
    for (uint64_t gfn = start_gfn; gfn < start_gfn + nr_pages; gfn++) {
        if (anemoi_cache_mark_dirty(lm->cache, gfn, true, errp) != 0) {
            return -1;
        }
    }
    return 0;
}

int anemoi_lm_prepare_switchover(AnemoiLM *lm, AnemoiLMBranch branch,
                                 Error **errp)
{
    if (!lm) {
        error_setg(errp, "AnemoiLM is not initialized");
        return -1;
    }

    /*
     * Spec invariant 4.2 #1: before stop-and-copy, all dirty cachelines must
     * be flushed to M so Q' can link the same canonical pool.
     */
    if (anemoi_cache_flush_all(lm->cache, errp) != 0) {
        return -1;
    }

    switch (branch) {
    case ANEMOI_LM_BRANCH_NO_REPLICA:
        return 0;
    case ANEMOI_LM_BRANCH_WITH_REPLICA:
        /*
         * Delta application and cmap writeback are implemented by the replica
         * agent path.  The common switchover invariant is the same here.
         */
        return 0;
    default:
        error_setg(errp, "unknown AnemoiLM branch %d", branch);
        return -1;
    }
}
