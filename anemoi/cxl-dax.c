#include "qemu/osdep.h"
#include "anemoi/cxl-dax.h"
#include "qemu/error-report.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_SHARED_VALIDATE
#define MAP_SHARED_VALIDATE 0x03
#endif

#ifndef MAP_SYNC
#define MAP_SYNC 0x80000
#endif

#define ANEMOI_CXL_DEFAULT_DEVDAX_ALIGN (2ULL * 1024ULL * 1024ULL)

static const char *anemoi_cxl_dax_name_from_path(const char *dax_path)
{
    const char *slash = strrchr(dax_path, '/');

    return slash ? slash + 1 : dax_path;
}

static int anemoi_cxl_read_sysfs_u64(const char *path, uint64_t *value)
{
    FILE *f = fopen(path, "r");
    unsigned long long tmp;

    if (!f) {
        return -1;
    }
    if (fscanf(f, "%llu", &tmp) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *value = (uint64_t)tmp;
    return 0;
}

static int anemoi_cxl_read_sysfs_i32(const char *path, int *value)
{
    FILE *f = fopen(path, "r");
    int tmp;

    if (!f) {
        return -1;
    }
    if (fscanf(f, "%d", &tmp) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *value = tmp;
    return 0;
}

static uint64_t anemoi_cxl_probe_dax_align(const char *dax_name)
{
    char path[256];
    uint64_t align;

    snprintf(path, sizeof(path), "/sys/bus/dax/devices/%s/align", dax_name);
    if (anemoi_cxl_read_sysfs_u64(path, &align) == 0 && align != 0) {
        return align;
    }
    warn_report("Anemoi CXL: could not read %s; defaulting devdax align to %llu",
                path, (unsigned long long)ANEMOI_CXL_DEFAULT_DEVDAX_ALIGN);
    return ANEMOI_CXL_DEFAULT_DEVDAX_ALIGN;
}

static int anemoi_cxl_probe_dax_target_node(const char *dax_name)
{
    char path[256];
    int node;

    snprintf(path, sizeof(path), "/sys/bus/dax/devices/%s/target_node",
             dax_name);
    if (anemoi_cxl_read_sysfs_i32(path, &node) == 0) {
        return node;
    }
    warn_report("Anemoi CXL: could not read %s; CXL NUMA node unknown", path);
    return -1;
}

static int anemoi_cxl_probe_dax_size(const char *dax_name, uint64_t *size,
                                     Error **errp)
{
    char path[256];

    snprintf(path, sizeof(path), "/sys/bus/dax/devices/%s/size", dax_name);
    if (anemoi_cxl_read_sysfs_u64(path, size) != 0 || *size == 0) {
        error_setg(errp, "failed to read CXL devdax size from %s", path);
        return -1;
    }
    return 0;
}

static bool anemoi_cxl_align_up_u64(uint64_t value, uint64_t align,
                                    uint64_t *aligned)
{
    uint64_t rem;

    if (!align) {
        return false;
    }
    rem = value % align;
    if (!rem) {
        *aligned = value;
        return true;
    }
    if (value > UINT64_MAX - (align - rem)) {
        return false;
    }
    *aligned = value + align - rem;
    return true;
}

int anemoi_cxl_dax_open(AnemoiCxlDaxRegion *region, const char *dax_path,
                        uint64_t requested_size, bool use_map_sync,
                        int numa_node, Error **errp)
{
    const char *dax_name;
    uint64_t mapped_size;
    int flags;

    if (!region || !dax_path || !requested_size) {
        error_setg(errp, "invalid Anemoi CXL devdax open request");
        return -1;
    }

    memset(region, 0, sizeof(*region));
    region->addr = MAP_FAILED;
    region->fd = -1;
    region->numa_node = -1;

    dax_name = anemoi_cxl_dax_name_from_path(dax_path);
    region->device_align = anemoi_cxl_probe_dax_align(dax_name);
    region->numa_node = anemoi_cxl_probe_dax_target_node(dax_name);
    if (anemoi_cxl_probe_dax_size(dax_name, &region->device_size, errp) != 0) {
        return -1;
    }

    if (numa_node >= 0) {
        if (region->numa_node < 0) {
            error_setg(errp, "CXL devdax %s NUMA node is unknown", dax_path);
            return -1;
        }
        if (region->numa_node != numa_node) {
            error_setg(errp, "CXL devdax %s NUMA node mismatch: got %d expected %d",
                       dax_path, region->numa_node, numa_node);
            return -1;
        }
    }

    if (!anemoi_cxl_align_up_u64(requested_size, region->device_align,
                                 &mapped_size) || mapped_size > SIZE_MAX) {
        error_setg(errp, "Anemoi CXL devdax mapping size overflows");
        return -1;
    }
    if (region->device_size < requested_size) {
        error_setg(errp, "CXL devdax %s size %" PRIu64
                   " bytes is smaller than requested %" PRIu64,
                   dax_path, region->device_size, requested_size);
        return -1;
    }
    if (region->device_size < mapped_size) {
        error_setg(errp, "CXL devdax %s size %" PRIu64
                   " bytes is smaller than aligned mapping %" PRIu64,
                   dax_path, region->device_size, mapped_size);
        return -1;
    }
    if (mapped_size != requested_size) {
        warn_report("Anemoi CXL: requested region size %" PRIu64
                    " is not aligned to devdax granule %" PRIu64
                    "; rounding up to %" PRIu64,
                    requested_size, region->device_align, mapped_size);
    }

    region->fd = open(dax_path, O_RDWR);
    if (region->fd < 0) {
        error_setg_errno(errp, errno, "failed to open CXL devdax %s", dax_path);
        return -1;
    }

    flags = MAP_SHARED;
    if (use_map_sync) {
        flags = MAP_SHARED | MAP_SHARED_VALIDATE | MAP_SYNC;
    }
    region->addr = mmap(NULL, (size_t)mapped_size, PROT_READ | PROT_WRITE,
                        flags, region->fd, 0);
    if (region->addr == MAP_FAILED && use_map_sync) {
        warn_report("Anemoi CXL: mmap MAP_SYNC failed for %s: %s; retrying without MAP_SYNC",
                    dax_path, strerror(errno));
        region->addr = mmap(NULL, (size_t)mapped_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, region->fd, 0);
    } else if (use_map_sync) {
        region->map_sync_active = true;
    }
    if (region->addr == MAP_FAILED) {
        error_setg_errno(errp, errno, "failed to mmap CXL devdax %s", dax_path);
        close(region->fd);
        region->fd = -1;
        return -1;
    }

    region->size = mapped_size;
    return 0;
}

void anemoi_cxl_dax_close(AnemoiCxlDaxRegion *region)
{
    if (!region) {
        return;
    }
    if (region->addr && region->addr != MAP_FAILED) {
        munmap(region->addr, region->size);
    }
    if (region->fd >= 0) {
        close(region->fd);
    }
    memset(region, 0, sizeof(*region));
    region->addr = MAP_FAILED;
    region->fd = -1;
}
