/*
 * Anemoi read-only page map.
 *
 * A set bit means the page is reachable from AnemoiM / replica and may be
 * represented by a one-byte read-only flag during RAM bulk transfer.
 */

#ifndef ANEMOI_RMAP_H
#define ANEMOI_RMAP_H

#include "qapi/error.h"

typedef struct AnemoiRMap AnemoiRMap;

AnemoiRMap *anemoi_rmap_new(uint64_t nr_pages, bool initially_readonly,
                            Error **errp);
void anemoi_rmap_free(AnemoiRMap *rmap);

uint64_t anemoi_rmap_nr_pages(const AnemoiRMap *rmap);
bool anemoi_rmap_test(const AnemoiRMap *rmap, uint64_t gfn);
void anemoi_rmap_set(AnemoiRMap *rmap, uint64_t gfn);
void anemoi_rmap_clear(AnemoiRMap *rmap, uint64_t gfn);
void anemoi_rmap_set_range(AnemoiRMap *rmap, uint64_t start_gfn,
                           uint64_t nr_pages);
void anemoi_rmap_clear_range(AnemoiRMap *rmap, uint64_t start_gfn,
                             uint64_t nr_pages);
void anemoi_rmap_fill(AnemoiRMap *rmap);
void anemoi_rmap_zero(AnemoiRMap *rmap);
uint64_t anemoi_rmap_count_readonly(const AnemoiRMap *rmap);

#endif /* ANEMOI_RMAP_H */
