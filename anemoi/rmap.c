#include "qemu/osdep.h"
#include "anemoi/rmap.h"
#include "qemu/bitmap.h"
#include "qemu/bitops.h"

struct AnemoiRMap {
    uint64_t nr_pages;
    unsigned long *bits;
};

static bool anemoi_rmap_in_bounds(const AnemoiRMap *rmap, uint64_t gfn)
{
    return rmap && gfn < rmap->nr_pages;
}

AnemoiRMap *anemoi_rmap_new(uint64_t nr_pages, bool initially_readonly,
                            Error **errp)
{
    AnemoiRMap *rmap;

    if (!nr_pages || nr_pages > LONG_MAX) {
        error_setg(errp, "invalid Anemoi rmap page count: %" PRIu64,
                   nr_pages);
        return NULL;
    }

    rmap = g_new0(AnemoiRMap, 1);
    rmap->nr_pages = nr_pages;
    rmap->bits = bitmap_new((long)nr_pages);
    if (initially_readonly) {
        bitmap_fill(rmap->bits, (long)nr_pages);
    }
    return rmap;
}

void anemoi_rmap_free(AnemoiRMap *rmap)
{
    if (!rmap) {
        return;
    }
    g_free(rmap->bits);
    g_free(rmap);
}

uint64_t anemoi_rmap_nr_pages(const AnemoiRMap *rmap)
{
    return rmap ? rmap->nr_pages : 0;
}

bool anemoi_rmap_test(const AnemoiRMap *rmap, uint64_t gfn)
{
    if (!anemoi_rmap_in_bounds(rmap, gfn)) {
        return false;
    }
    return test_bit((long)gfn, rmap->bits);
}

void anemoi_rmap_set(AnemoiRMap *rmap, uint64_t gfn)
{
    if (anemoi_rmap_in_bounds(rmap, gfn)) {
        set_bit((long)gfn, rmap->bits);
    }
}

void anemoi_rmap_clear(AnemoiRMap *rmap, uint64_t gfn)
{
    if (anemoi_rmap_in_bounds(rmap, gfn)) {
        clear_bit((long)gfn, rmap->bits);
    }
}

void anemoi_rmap_set_range(AnemoiRMap *rmap, uint64_t start_gfn,
                           uint64_t nr_pages)
{
    if (!rmap || start_gfn >= rmap->nr_pages || !nr_pages) {
        return;
    }
    nr_pages = MIN(nr_pages, rmap->nr_pages - start_gfn);
    bitmap_set(rmap->bits, (long)start_gfn, (long)nr_pages);
}

void anemoi_rmap_clear_range(AnemoiRMap *rmap, uint64_t start_gfn,
                             uint64_t nr_pages)
{
    if (!rmap || start_gfn >= rmap->nr_pages || !nr_pages) {
        return;
    }
    nr_pages = MIN(nr_pages, rmap->nr_pages - start_gfn);
    bitmap_clear(rmap->bits, (long)start_gfn, (long)nr_pages);
}

void anemoi_rmap_fill(AnemoiRMap *rmap)
{
    if (rmap) {
        bitmap_fill(rmap->bits, (long)rmap->nr_pages);
    }
}

void anemoi_rmap_zero(AnemoiRMap *rmap)
{
    if (rmap) {
        bitmap_zero(rmap->bits, (long)rmap->nr_pages);
    }
}

uint64_t anemoi_rmap_count_readonly(const AnemoiRMap *rmap)
{
    if (!rmap) {
        return 0;
    }
    return slow_bitmap_count_one(rmap->bits, (long)rmap->nr_pages);
}
