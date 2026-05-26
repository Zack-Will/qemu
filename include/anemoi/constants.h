/*
 * Anemoi baseline constants.
 *
 * These values are pinned to Yu et al., TPDS 2025, Section V setup:
 * 4096-way set-associative AnemoiCache with 4 KiB cachelines and LRU-K
 * with K=2.
 */

#ifndef ANEMOI_CONSTANTS_H
#define ANEMOI_CONSTANTS_H

#include <stdint.h>

#define ANEMOI_PAGE_SIZE 4096ULL
#define ANEMOI_CACHE_WAYS 4096U
#define ANEMOI_LRU_K 2U

static inline uint64_t anemoi_pages_for_bytes(uint64_t bytes)
{
    return (bytes + ANEMOI_PAGE_SIZE - 1) / ANEMOI_PAGE_SIZE;
}

#endif /* ANEMOI_CONSTANTS_H */
