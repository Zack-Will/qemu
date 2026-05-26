#include "qemu/osdep.h"
#include "anemoi/glue.h"
#include "anemoi/runtime.h"
#include "qapi/qapi-commands-migration.h"

static AnemoiRuntime *anemoi_global_runtime;

static const char *anemoi_boot_mode_to_qapi(AnemoiRuntimeBootMode mode)
{
    switch (mode) {
    case ANEMOI_RUNTIME_BOOT_SOURCE_SEED:
        return "source-seed";
    case ANEMOI_RUNTIME_BOOT_DESTINATION_ATTACH:
        return "destination-attach";
    default:
        return "unknown";
    }
}

static bool anemoi_parse_backend_kind(const char *backend,
                                      AnemoiRuntimeBackendKind *kind,
                                      Error **errp)
{
    if (!backend || !g_strcmp0(backend, "memory")) {
        *kind = ANEMOI_RUNTIME_BACKEND_MEMORY;
        return true;
    }
    if (!g_strcmp0(backend, "rdma")) {
        *kind = ANEMOI_RUNTIME_BACKEND_RDMA;
        return true;
    }

    error_setg(errp, "unknown Anemoi backend '%s'", backend);
    return false;
}

static bool anemoi_parse_rdma_role(const char *role,
                                   AnemoiRDMARole *rdma_role,
                                   Error **errp)
{
    if (!role || !g_strcmp0(role, "client")) {
        *rdma_role = ANEMOI_RDMA_ROLE_CLIENT;
        return true;
    }
    if (!g_strcmp0(role, "server")) {
        *rdma_role = ANEMOI_RDMA_ROLE_SERVER;
        return true;
    }

    error_setg(errp, "unknown Anemoi RDMA role '%s'", role);
    return false;
}

int anemoi_migration_switchover_hook(Error **errp)
{
    if (!anemoi_global_runtime) {
        return 0;
    }
    return anemoi_runtime_prepare_switchover(anemoi_global_runtime,
                                             ANEMOI_LM_BRANCH_NO_REPLICA,
                                             errp);
}

static void anemoi_qmp_start_runtime(AnemoiRuntimeBootMode boot_mode,
                                     bool has_vmid, uint32_t vmid,
                                     bool has_local_cache_pages,
                                     uint64_t local_cache_pages,
                                     const char *backend,
                                     const char *rdma_role,
                                     const char *rdma_peer_host,
                                     bool has_rdma_port, uint16_t rdma_port,
                                     const char *rdma_dev,
                                     bool has_rdma_ib_port,
                                     uint8_t rdma_ib_port,
                                     bool has_rdma_gid_idx,
                                     int64_t rdma_gid_idx,
                                     bool has_rdma_vm_capacity,
                                     uint32_t rdma_vm_capacity,
                                     bool has_rdma_pages_per_vm,
                                     uint64_t rdma_pages_per_vm,
                                     bool has_auto_pause, bool auto_pause,
                                     bool has_auto_resume, bool auto_resume,
                                     Error **errp)
{
    AnemoiRuntimeBackendKind backend_kind;
    AnemoiRDMARole parsed_rdma_role;
    AnemoiRuntimeConfig cfg = {
        .vmid = has_vmid ? vmid : 0,
        .local_cache_pages = has_local_cache_pages ? local_cache_pages : 0,
        .boot_mode = boot_mode,
        .auto_pause = has_auto_pause ? auto_pause : true,
        .auto_resume = has_auto_resume ? auto_resume : true,
    };

    if (!anemoi_parse_backend_kind(backend, &backend_kind, errp) ||
        !anemoi_parse_rdma_role(rdma_role, &parsed_rdma_role, errp)) {
        return;
    }

    if (has_rdma_gid_idx &&
        (rdma_gid_idx < INT_MIN || rdma_gid_idx > INT_MAX)) {
        error_setg(errp, "Anemoi RDMA gid index is out of int range");
        return;
    }

    if (backend_kind == ANEMOI_RUNTIME_BACKEND_RDMA &&
        parsed_rdma_role == ANEMOI_RDMA_ROLE_CLIENT && !rdma_peer_host) {
        error_setg(errp, "Anemoi RDMA client backend requires rdma-peer-host");
        return;
    }

    if (anemoi_global_runtime) {
        error_setg(errp, "Anemoi runtime is already active");
        return;
    }

    cfg.backend_kind = backend_kind;
    cfg.rdma = (AnemoiRDMAConfig) {
        .role = parsed_rdma_role,
        .peer_host = rdma_peer_host,
        .tcp_port = has_rdma_port ? rdma_port : ANEMOI_RDMA_DEFAULT_TCP_PORT,
        .rdma_dev = rdma_dev,
        .ib_port = has_rdma_ib_port ? rdma_ib_port :
                   ANEMOI_RDMA_DEFAULT_IB_PORT,
        .gid_idx = has_rdma_gid_idx ? (int)rdma_gid_idx :
                   ANEMOI_RDMA_DEFAULT_GID_IDX,
        .vm_capacity = has_rdma_vm_capacity ? rdma_vm_capacity : 0,
        .pages_per_vm = has_rdma_pages_per_vm ? rdma_pages_per_vm : 0,
    };

    anemoi_global_runtime = anemoi_runtime_start(&cfg, errp);
}

void qmp_x_anemoi_start(bool has_vmid, uint32_t vmid,
                        bool has_local_cache_pages,
                        uint64_t local_cache_pages,
                        const char *backend,
                        const char *rdma_role,
                        const char *rdma_peer_host,
                        bool has_rdma_port, uint16_t rdma_port,
                        const char *rdma_dev,
                        bool has_rdma_ib_port, uint8_t rdma_ib_port,
                        bool has_rdma_gid_idx, int64_t rdma_gid_idx,
                        bool has_rdma_vm_capacity,
                        uint32_t rdma_vm_capacity,
                        bool has_rdma_pages_per_vm,
                        uint64_t rdma_pages_per_vm,
                        bool has_auto_pause, bool auto_pause,
                        bool has_auto_resume, bool auto_resume,
                        Error **errp)
{
    anemoi_qmp_start_runtime(ANEMOI_RUNTIME_BOOT_SOURCE_SEED,
                             has_vmid, vmid,
                             has_local_cache_pages, local_cache_pages,
                             backend, rdma_role, rdma_peer_host,
                             has_rdma_port, rdma_port, rdma_dev,
                             has_rdma_ib_port, rdma_ib_port,
                             has_rdma_gid_idx, rdma_gid_idx,
                             has_rdma_vm_capacity, rdma_vm_capacity,
                             has_rdma_pages_per_vm, rdma_pages_per_vm,
                             has_auto_pause, auto_pause,
                             has_auto_resume, auto_resume, errp);
}

void qmp_x_anemoi_prepare_incoming(bool has_vmid, uint32_t vmid,
                                   bool has_local_cache_pages,
                                   uint64_t local_cache_pages,
                                   const char *backend,
                                   const char *rdma_role,
                                   const char *rdma_peer_host,
                                   bool has_rdma_port, uint16_t rdma_port,
                                   const char *rdma_dev,
                                   bool has_rdma_ib_port,
                                   uint8_t rdma_ib_port,
                                   bool has_rdma_gid_idx,
                                   int64_t rdma_gid_idx,
                                   bool has_rdma_vm_capacity,
                                   uint32_t rdma_vm_capacity,
                                   bool has_rdma_pages_per_vm,
                                   uint64_t rdma_pages_per_vm,
                                   bool has_auto_pause, bool auto_pause,
                                   bool has_auto_resume, bool auto_resume,
                                   Error **errp)
{
    anemoi_qmp_start_runtime(ANEMOI_RUNTIME_BOOT_DESTINATION_ATTACH,
                             has_vmid, vmid,
                             has_local_cache_pages, local_cache_pages,
                             backend, rdma_role, rdma_peer_host,
                             has_rdma_port, rdma_port, rdma_dev,
                             has_rdma_ib_port, rdma_ib_port,
                             has_rdma_gid_idx, rdma_gid_idx,
                             has_rdma_vm_capacity, rdma_vm_capacity,
                             has_rdma_pages_per_vm, rdma_pages_per_vm,
                             has_auto_pause, auto_pause,
                             has_auto_resume, auto_resume, errp);
}

void qmp_x_anemoi_stop(Error **errp)
{
    if (!anemoi_global_runtime) {
        error_setg(errp, "Anemoi runtime is not active");
        return;
    }

    anemoi_runtime_stop(anemoi_global_runtime);
    anemoi_global_runtime = NULL;
}

XAnemoiInfo *qmp_query_anemoi(Error **errp)
{
    XAnemoiInfo *info = g_new0(XAnemoiInfo, 1);
    AnemoiRuntimeStats stats;

    (void)errp;

    if (!anemoi_global_runtime) {
        info->enabled = false;
        return info;
    }

    anemoi_runtime_get_stats(anemoi_global_runtime, &stats);
    info->enabled = true;
    info->vmid = stats.vmid;
    info->boot_mode = g_strdup(anemoi_boot_mode_to_qapi(stats.boot_mode));
    info->guest_pages = stats.guest_pages;
    info->local_cache_pages = stats.local_cache_pages;
    info->ramblocks = stats.nr_ramblocks;
    info->fault_service_failed = stats.fault_service_failed;
    info->cache_hits = stats.cache.hits;
    info->cache_misses = stats.cache.misses;
    info->cache_evictions = stats.cache.evictions;
    info->cache_dirty_writebacks = stats.cache.dirty_writebacks;
    info->cache_install_errors = stats.cache.install_errors;
    info->backend_fetches = stats.backend.fetches;
    info->backend_writebacks = stats.backend.writebacks;
    info->backend_prefetches = stats.backend.prefetches;
    info->backend_failed_fetches = stats.backend.failed_fetches;
    info->backend_failed_writebacks = stats.backend.failed_writebacks;
    return info;
}

void qmp_x_anemoi_prepare_switchover(Error **errp)
{
    if (!anemoi_global_runtime) {
        error_setg(errp, "Anemoi runtime is not active");
        return;
    }

    anemoi_runtime_prepare_switchover(anemoi_global_runtime,
                                      ANEMOI_LM_BRANCH_NO_REPLICA, errp);
}

static XAnemoiCacheMetadata *
anemoi_qapi_cache_metadata_from_internal(const AnemoiCacheMetadata *metadata)
{
    XAnemoiCacheMetadata *qapi = g_new0(XAnemoiCacheMetadata, 1);
    XAnemoiCacheLineList **tail = &qapi->lines;

    qapi->ways = metadata->ways;
    qapi->num_sets = metadata->num_sets;
    qapi->nr_lines = metadata->nr_lines;

    for (uint64_t i = 0; i < metadata->nr_lines; i++) {
        const AnemoiCacheLineSnapshot *snap = &metadata->lines[i];
        XAnemoiCacheLineList *node = g_new0(XAnemoiCacheLineList, 1);
        XAnemoiCacheLine *line = g_new0(XAnemoiCacheLine, 1);

        line->gfn = snap->gfn;
        line->valid = !!snap->valid;
        line->dirty = !!snap->dirty;
        line->pinned = !!snap->pinned;
        line->lruk_ts0 = snap->lruk_ts[0];
        line->lruk_ts1 = snap->lruk_ts[1];

        node->value = line;
        *tail = node;
        tail = &node->next;
    }

    return qapi;
}

static int anemoi_qapi_cache_metadata_to_internal(
    const XAnemoiCacheMetadata *qapi, AnemoiCacheMetadata *metadata,
    Error **errp)
{
    const XAnemoiCacheLineList *node;
    uint64_t pos = 0;

    if (!qapi || !metadata) {
        error_setg(errp, "invalid AnemoiCache metadata QMP payload");
        return -1;
    }
    if (qapi->nr_lines > SIZE_MAX / sizeof(*metadata->lines)) {
        error_setg(errp, "AnemoiCache metadata QMP payload is too large");
        return -1;
    }

    memset(metadata, 0, sizeof(*metadata));
    metadata->ways = qapi->ways;
    metadata->num_sets = qapi->num_sets;
    metadata->nr_lines = qapi->nr_lines;
    metadata->lines = qapi->nr_lines ?
                      g_new0(AnemoiCacheLineSnapshot, qapi->nr_lines) : NULL;

    for (node = qapi->lines; node; node = node->next) {
        const XAnemoiCacheLine *line = node->value;
        AnemoiCacheLineSnapshot *snap;

        if (pos == metadata->nr_lines) {
            error_setg(errp, "AnemoiCache metadata QMP payload has too many lines");
            goto fail;
        }
        if (!line) {
            error_setg(errp, "AnemoiCache metadata QMP payload has a null line");
            goto fail;
        }

        snap = &metadata->lines[pos++];
        snap->gfn = line->gfn;
        snap->valid = line->valid;
        snap->dirty = line->dirty;
        snap->pinned = line->pinned;
        snap->lruk_ts[0] = line->lruk_ts0;
        snap->lruk_ts[1] = line->lruk_ts1;
    }

    if (pos != metadata->nr_lines) {
        error_setg(errp, "AnemoiCache metadata QMP payload has only %" PRIu64
                   " lines, expected %" PRIu64, pos, metadata->nr_lines);
        goto fail;
    }

    return 0;

fail:
    anemoi_cache_metadata_destroy(metadata);
    return -1;
}

XAnemoiCacheMetadata *qmp_x_anemoi_export_cache_metadata(Error **errp)
{
    AnemoiCacheMetadata metadata;
    XAnemoiCacheMetadata *qapi;

    if (!anemoi_global_runtime) {
        error_setg(errp, "Anemoi runtime is not active");
        return NULL;
    }

    if (anemoi_cache_export_metadata(
            anemoi_runtime_cache(anemoi_global_runtime), &metadata, errp) != 0) {
        return NULL;
    }

    qapi = anemoi_qapi_cache_metadata_from_internal(&metadata);
    anemoi_cache_metadata_destroy(&metadata);
    return qapi;
}

void qmp_x_anemoi_import_cache_metadata(XAnemoiCacheMetadata *metadata,
                                        Error **errp)
{
    AnemoiCacheMetadata internal;

    if (!anemoi_global_runtime) {
        error_setg(errp, "Anemoi runtime is not active");
        return;
    }

    if (anemoi_qapi_cache_metadata_to_internal(metadata, &internal, errp) != 0) {
        return;
    }

    anemoi_cache_import_metadata(anemoi_runtime_cache(anemoi_global_runtime),
                                 &internal, errp);
    anemoi_cache_metadata_destroy(&internal);
}
