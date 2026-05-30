/*
 * Pure helpers for CXL hybrid per-page state words.
 *
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "migration/cxl.h"
#include "qemu/atomic.h"

#define CXL_PAGE_KIND_SHIFT       0
#define CXL_PAGE_OWNER_SHIFT      3
#define CXL_PAGE_LOCATION_SHIFT   6
#define CXL_PAGE_GENERATION_SHIFT 16
#define CXL_PAGE_DIRTY_SEQ_SHIFT  32
#define CXL_PAGE_KIND_MASK        0x7ULL
#define CXL_PAGE_OWNER_MASK       0x7ULL
#define CXL_PAGE_LOCATION_MASK    0x7ULL
#define CXL_PAGE_U16_MASK         0xffffULL
#define CXL_PAGE_U32_MASK         0xffffffffULL

static uint64_t cxl_hybrid_page_state_pack(CXLHybridPageStateKind kind,
                                           CXLHybridPageOwner owner,
                                           CXLHybridPageLocation location,
                                           uint32_t generation,
                                           uint32_t dirty_seq)
{
    return ((uint64_t)kind << CXL_PAGE_KIND_SHIFT) |
           ((uint64_t)owner << CXL_PAGE_OWNER_SHIFT) |
           ((uint64_t)location << CXL_PAGE_LOCATION_SHIFT) |
           (((uint64_t)generation & CXL_PAGE_U16_MASK) <<
            CXL_PAGE_GENERATION_SHIFT) |
           (((uint64_t)dirty_seq & CXL_PAGE_U32_MASK) <<
            CXL_PAGE_DIRTY_SEQ_SHIFT);
}

static uint32_t cxl_hybrid_page_state_encoded_generation(uint32_t generation)
{
    return generation & CXL_PAGE_U16_MASK;
}

uint64_t cxl_hybrid_page_state_make_not_sent(uint32_t generation)
{
    return cxl_hybrid_page_state_pack(CXL_HYBRID_PAGE_STATE_NOT_SENT,
                                      CXL_HYBRID_PAGE_OWNER_NONE,
                                      CXL_HYBRID_PAGE_LOCATION_NONE,
                                      generation, 0);
}

uint64_t cxl_hybrid_page_state_make_dirty(uint32_t generation,
                                          uint32_t dirty_seq)
{
    return cxl_hybrid_page_state_pack(CXL_HYBRID_PAGE_STATE_DIRTY,
                                      CXL_HYBRID_PAGE_OWNER_NONE,
                                      CXL_HYBRID_PAGE_LOCATION_NONE,
                                      generation, dirty_seq);
}

uint64_t cxl_hybrid_page_state_make_published(
    uint32_t generation,
    CXLHybridPageLocation location,
    uint32_t dirty_seq)
{
    return cxl_hybrid_page_state_pack(CXL_HYBRID_PAGE_STATE_PUBLISHED,
                                      CXL_HYBRID_PAGE_OWNER_NONE,
                                      location, generation, dirty_seq);
}

CXLHybridPageStateKind cxl_hybrid_page_state_kind(uint64_t word)
{
    return (word >> CXL_PAGE_KIND_SHIFT) & CXL_PAGE_KIND_MASK;
}

CXLHybridPageOwner cxl_hybrid_page_state_owner(uint64_t word)
{
    return (word >> CXL_PAGE_OWNER_SHIFT) & CXL_PAGE_OWNER_MASK;
}

CXLHybridPageLocation cxl_hybrid_page_state_location(uint64_t word)
{
    return (word >> CXL_PAGE_LOCATION_SHIFT) & CXL_PAGE_LOCATION_MASK;
}

uint32_t cxl_hybrid_page_state_generation(uint64_t word)
{
    return (word >> CXL_PAGE_GENERATION_SHIFT) & CXL_PAGE_U16_MASK;
}

uint32_t cxl_hybrid_page_state_dirty_seq(uint64_t word)
{
    return (word >> CXL_PAGE_DIRTY_SEQ_SHIFT) & CXL_PAGE_U32_MASK;
}

bool cxl_hybrid_page_state_try_claim(uint64_t *slot,
                                     CXLHybridPageOwner owner,
                                     uint32_t generation,
                                     CXLHybridPageClaim *claim)
{
    uint64_t old;
    uint64_t next;
    CXLHybridPageStateKind kind;
    uint32_t encoded_generation;
    uint32_t dirty_seq;

    if (!slot || !claim || owner == CXL_HYBRID_PAGE_OWNER_NONE) {
        return false;
    }

    encoded_generation = cxl_hybrid_page_state_encoded_generation(generation);
    for (;;) {
        old = qatomic_load_acquire(slot);
        kind = cxl_hybrid_page_state_kind(old);
        if (kind != CXL_HYBRID_PAGE_STATE_NOT_SENT &&
            kind != CXL_HYBRID_PAGE_STATE_DIRTY) {
            return false;
        }
        if (cxl_hybrid_page_state_generation(old) != encoded_generation) {
            return false;
        }

        dirty_seq = cxl_hybrid_page_state_dirty_seq(old);
        next = cxl_hybrid_page_state_pack(CXL_HYBRID_PAGE_STATE_IN_FLIGHT,
                                          owner,
                                          CXL_HYBRID_PAGE_LOCATION_NONE,
                                          encoded_generation, dirty_seq);
        if (qatomic_cmpxchg(slot, old, next) == old) {
            claim->observed = next;
            claim->generation = encoded_generation;
            claim->dirty_seq = dirty_seq;
            claim->owner = owner;
            return true;
        }
    }
}

bool cxl_hybrid_page_state_claim_for_cxl(uint64_t *slot,
                                         uint32_t generation,
                                         CXLHybridPageClaim *claim)
{
    return cxl_hybrid_page_state_try_claim(
        slot, CXL_HYBRID_PAGE_OWNER_CXL, generation, claim);
}

bool cxl_hybrid_page_state_claim_for_rdma(uint64_t *slot,
                                          uint32_t generation,
                                          CXLHybridPageClaim *claim)
{
    return cxl_hybrid_page_state_try_claim(
        slot, CXL_HYBRID_PAGE_OWNER_RDMA, generation, claim);
}

bool cxl_hybrid_page_state_complete(uint64_t *slot,
                                    const CXLHybridPageClaim *claim,
                                    CXLHybridPageLocation location)
{
    uint64_t published;

    if (!slot || !claim || location == CXL_HYBRID_PAGE_LOCATION_NONE) {
        return false;
    }

    published = cxl_hybrid_page_state_make_published(
        claim->generation, location, claim->dirty_seq);
    return qatomic_cmpxchg(slot, claim->observed, published) ==
           claim->observed;
}

bool cxl_hybrid_page_state_complete_cxl(uint64_t *slot,
                                        const CXLHybridPageClaim *claim)
{
    return claim && claim->owner == CXL_HYBRID_PAGE_OWNER_CXL &&
           cxl_hybrid_page_state_complete(
               slot, claim, CXL_HYBRID_PAGE_LOCATION_CXL);
}

bool cxl_hybrid_page_state_complete_rdma(uint64_t *slot,
                                         const CXLHybridPageClaim *claim)
{
    return claim && claim->owner == CXL_HYBRID_PAGE_OWNER_RDMA &&
           cxl_hybrid_page_state_complete(
               slot, claim, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);
}

void cxl_hybrid_page_state_mark_dirty(uint64_t *slot,
                                      uint32_t generation,
                                      uint32_t dirty_seq)
{
    uint64_t old;
    uint64_t next;

    if (!slot) {
        return;
    }

    next = cxl_hybrid_page_state_make_dirty(generation, dirty_seq);
    do {
        old = qatomic_load_acquire(slot);
    } while (qatomic_cmpxchg(slot, old, next) != old);
}

bool cxl_hybrid_page_state_can_consume(uint64_t word,
                                       uint32_t generation,
                                       CXLHybridPageLocation location)
{
    return cxl_hybrid_page_state_kind(word) ==
           CXL_HYBRID_PAGE_STATE_PUBLISHED &&
           cxl_hybrid_page_state_generation(word) ==
           cxl_hybrid_page_state_encoded_generation(generation) &&
           cxl_hybrid_page_state_location(word) == location;
}

static bool cxl_hybrid_page_state_word_is_cxl_published(uint64_t word,
                                                        uint32_t generation)
{
    return cxl_hybrid_page_state_can_consume(
        word, generation, CXL_HYBRID_PAGE_LOCATION_CXL);
}

bool cxl_hybrid_page_state_longest_cxl_span(const uint64_t *page_state,
                                            uint64_t total_pages,
                                            uint64_t fault_page,
                                            uint32_t generation,
                                            uint32_t max_pages,
                                            CXLHybridRemapSpan *span)
{
    uint64_t first;
    uint64_t last;
    uint64_t span_len;
    uint64_t left_target;
    uint64_t right_target;
    uint64_t left_used;
    uint64_t right_used;

    if (!page_state || !span || fault_page >= total_pages ||
        !cxl_hybrid_page_state_word_is_cxl_published(
            qatomic_load_acquire(&page_state[fault_page]), generation)) {
        return false;
    }

    first = fault_page;
    last = fault_page + 1;

    if (max_pages) {
        left_target = (max_pages - 1) / 2;
        right_target = (max_pages - 1) - left_target;
        left_used = 0;
        right_used = 0;

        while (left_used < left_target && first > 0 &&
               cxl_hybrid_page_state_word_is_cxl_published(
                   qatomic_load_acquire(&page_state[first - 1]), generation)) {
            first--;
            left_used++;
        }
        right_target += left_target - left_used;

        while (right_used < right_target && last < total_pages &&
               cxl_hybrid_page_state_word_is_cxl_published(
                   qatomic_load_acquire(&page_state[last]), generation)) {
            last++;
            right_used++;
        }
        left_target += right_target - right_used;

        while (left_used < left_target && first > 0 &&
               cxl_hybrid_page_state_word_is_cxl_published(
                   qatomic_load_acquire(&page_state[first - 1]), generation)) {
            first--;
            left_used++;
        }
    } else {
        while (first > 0 &&
               cxl_hybrid_page_state_word_is_cxl_published(
                   qatomic_load_acquire(&page_state[first - 1]), generation)) {
            first--;
        }
        while (last < total_pages &&
               cxl_hybrid_page_state_word_is_cxl_published(
                   qatomic_load_acquire(&page_state[last]), generation)) {
            last++;
        }
    }

    span_len = last - first;
    if (span_len == 0 || span_len > UINT32_MAX) {
        return false;
    }

    span->first_page = first;
    span->nr_pages = span_len;
    return true;
}
