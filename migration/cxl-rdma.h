/*
 * CXL hybrid RDMA sidecar transport helpers.
 */

#ifndef MIGRATION_CXL_RDMA_H
#define MIGRATION_CXL_RDMA_H

#include "qapi/qapi-types-migration.h"
#include "system/ramlist.h"

typedef struct CXLHybridRDMABulkClaim CXLHybridRDMABulkClaim;

typedef struct CXLHybridRDMASidecarOps {
    bool (*migration_running)(void *opaque);
    bool (*migration_postcopy)(void *opaque);
    bool (*migration_failed)(void *opaque);
    bool (*bulk_active)(void *opaque);
    void (*complete_bulk_claim)(CXLHybridRDMABulkClaim *claim,
                                uint32_t *completed_pages,
                                uint32_t *stale_pages,
                                void *opaque);
    void (*drop_bulk_claim)(const CXLHybridRDMABulkClaim *claim, void *opaque);
    void (*propagate_error)(Error *err, void *opaque);
    int (*foreach_ramblock)(RAMBlockIterFunc func, void *iter_opaque,
                            void *opaque);
    void *opaque;
} CXLHybridRDMASidecarOps;

typedef struct CXLHybridRDMASidecarBulkStats {
    uint64_t rdma_bulk_regions;
    uint64_t rdma_bulk_bytes;
    uint64_t page_state_rdma_completed_pages;
    uint64_t page_state_rdma_completed_bytes;
    uint64_t page_state_rdma_completed_time_ns;
    uint64_t page_state_rdma_stale_pages;
    uint32_t rdma_sidecar_dynamic_window_regions;
    uint32_t rdma_sidecar_sq_capacity_regions;
    uint32_t rdma_sidecar_queue_len;
    uint32_t rdma_sidecar_inflight_len;
    double rdma_sidecar_goodput_ewma_bytes_per_ns;
    uint64_t rdma_sidecar_completion_latency_ewma_ns;
    uint32_t rdma_sidecar_bdp_estimate_regions;
    uint64_t rdma_sidecar_admission_accepted_regions;
    uint64_t rdma_sidecar_admission_overflow_cxl_regions;
    uint64_t rdma_sidecar_admission_closed_events;
    uint64_t rdma_sidecar_admission_goodput_drop_events;
} CXLHybridRDMASidecarBulkStats;

typedef struct CXLHybridRDMASidecarAdmissionSnapshot {
    bool accept_rdma;
    uint32_t dynamic_window_regions;
    uint32_t sq_capacity_regions;
    uint32_t queue_len;
    uint32_t inflight_len;
    uint32_t reserved_regions;
    uint32_t outstanding_regions;
    double goodput_ewma_bytes_per_ns;
    uint64_t completion_latency_ewma_ns;
    uint32_t bdp_estimate_regions;
    uint64_t accepted_regions;
    uint64_t overflow_cxl_regions;
    uint64_t admission_closed_events;
    uint64_t goodput_drop_events;
} CXLHybridRDMASidecarAdmissionSnapshot;

typedef struct CXLHybridRDMASidecarAdmissionState {
    uint32_t sq_capacity_regions;
    uint32_t dynamic_window_regions;
    uint32_t reserved_regions;
    uint64_t bytes_per_region;
    double goodput_ewma_bytes_per_ns;
    uint64_t completion_latency_ewma_ns;
    uint32_t bdp_estimate_regions;
    uint64_t accepted_regions;
    uint64_t overflow_cxl_regions;
    uint64_t admission_closed_events;
    uint64_t goodput_drop_events;
} CXLHybridRDMASidecarAdmissionState;

typedef struct CXLHybridRDMASidecarAdmissionReservation {
    bool valid;
    uint64_t owner;
} CXLHybridRDMASidecarAdmissionReservation;

typedef struct CXLHybridRDMASidecarConfig {
    const MigrationAddress *addr;
    uint64_t total_regions;
    uint64_t bytes_per_region;
    uint64_t pages_per_region;
    uint32_t max_inflight_regions;
    uint8_t max_cover_percent;
    bool pin_all;
    bool incoming;
    const CXLHybridRDMASidecarOps *ops;
} CXLHybridRDMASidecarConfig;

void cxl_rdma_sidecar_admission_state_init(
    CXLHybridRDMASidecarAdmissionState *state,
    uint32_t sq_capacity_regions,
    uint64_t bytes_per_region);
CXLHybridRDMASidecarAdmissionSnapshot cxl_rdma_sidecar_admission_snapshot(
    const CXLHybridRDMASidecarAdmissionState *state,
    bool running,
    bool bulk_active,
    bool draining,
    bool failed,
    bool postcopy,
    uint32_t queue_len,
    uint32_t inflight_len);
bool cxl_rdma_sidecar_admission_try_reserve(
    CXLHybridRDMASidecarAdmissionState *state,
    bool running,
    bool bulk_active,
    bool draining,
    bool failed,
    bool postcopy,
    uint32_t queue_len,
    uint32_t inflight_len,
    CXLHybridRDMASidecarAdmissionReservation *reservation,
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot);
void cxl_rdma_sidecar_admission_cancel_reserve(
    CXLHybridRDMASidecarAdmissionState *state,
    CXLHybridRDMASidecarAdmissionReservation *reservation);
void cxl_rdma_sidecar_admission_consume_reserve(
    CXLHybridRDMASidecarAdmissionState *state,
    CXLHybridRDMASidecarAdmissionReservation *reservation);
void cxl_rdma_sidecar_admission_note_completion(
    CXLHybridRDMASidecarAdmissionState *state,
    uint64_t useful_bytes,
    uint64_t latency_ns);

int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp);
bool cxl_rdma_sidecar_start_async(const CXLHybridRDMASidecarConfig *cfg,
                                  Error **errp);
void cxl_rdma_sidecar_stop(void);
bool cxl_rdma_sidecar_running(void);
bool cxl_rdma_sidecar_try_reserve_bulk_admission(
    CXLHybridRDMASidecarAdmissionReservation *reservation,
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot);
bool cxl_rdma_sidecar_enqueue_reserved_bulk_claim(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMASidecarAdmissionReservation *reservation);
void cxl_rdma_sidecar_cancel_bulk_admission(
    CXLHybridRDMASidecarAdmissionReservation *reservation);
bool cxl_rdma_sidecar_get_admission_snapshot(
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot);
bool cxl_rdma_sidecar_enqueue_bulk_claim(
    const CXLHybridRDMABulkClaim *claim);
void cxl_rdma_sidecar_drain_bulk_claims(void);
void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats);

#endif
