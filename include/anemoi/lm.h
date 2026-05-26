/*
 * AnemoiLM orchestration skeleton.
 */

#ifndef ANEMOI_LM_H
#define ANEMOI_LM_H

#include "anemoi/cache.h"
#include "anemoi/rmap.h"
#include "qapi/error.h"

typedef struct AnemoiLM AnemoiLM;

typedef struct AnemoiLMConfig {
    uint32_t vmid;
    uint64_t guest_pages;
    AnemoiCache *cache;
    AnemoiRMap *rmap;
} AnemoiLMConfig;

typedef enum AnemoiLMBranch {
    ANEMOI_LM_BRANCH_NO_REPLICA = 0,
    ANEMOI_LM_BRANCH_WITH_REPLICA = 1,
} AnemoiLMBranch;

AnemoiLM *anemoi_lm_new(const AnemoiLMConfig *cfg, Error **errp);
void anemoi_lm_free(AnemoiLM *lm);

int anemoi_lm_mark_dirty_range(AnemoiLM *lm, uint64_t start_gfn,
                               uint64_t nr_pages, Error **errp);
int anemoi_lm_prepare_switchover(AnemoiLM *lm, AnemoiLMBranch branch,
                                 Error **errp);

#endif /* ANEMOI_LM_H */
