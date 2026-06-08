/*
 * CXL devdax mapping core shared by Anemoi baselines and hybrid paths.
 */

#ifndef ANEMOI_CXL_DAX_H
#define ANEMOI_CXL_DAX_H

#include "qapi/error.h"

typedef struct AnemoiCxlDaxRegion {
    void *addr;
    uint64_t size;
    int fd;
    uint64_t device_align;
    uint64_t device_size;
    int numa_node;
    bool map_sync_active;
} AnemoiCxlDaxRegion;

int anemoi_cxl_dax_open(AnemoiCxlDaxRegion *region, const char *dax_path,
                        uint64_t requested_size, bool use_map_sync,
                        int numa_node, Error **errp);
void anemoi_cxl_dax_close(AnemoiCxlDaxRegion *region);

#endif /* ANEMOI_CXL_DAX_H */
