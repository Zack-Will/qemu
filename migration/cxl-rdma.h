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
    uint64_t page_state_rdma_active_time_ns;
    uint64_t page_state_rdma_transport_completed_time_ns;
    uint64_t page_state_rdma_transport_active_time_ns;
    uint64_t page_state_rdma_publish_time_ns;
    uint64_t page_state_rdma_stale_pages;
    uint64_t page_state_rdma_precopy_completed_bytes;
    uint64_t page_state_rdma_precopy_completed_time_ns;
    uint64_t page_state_rdma_precopy_active_time_ns;
    uint64_t page_state_rdma_precopy_transport_completed_time_ns;
    uint64_t page_state_rdma_precopy_transport_active_time_ns;
    uint64_t page_state_rdma_precopy_publish_time_ns;
    uint64_t page_state_rdma_postcopy_dirty_completed_bytes;
    uint64_t page_state_rdma_postcopy_dirty_completed_time_ns;
    uint64_t page_state_rdma_postcopy_dirty_active_time_ns;
    uint64_t page_state_rdma_postcopy_dirty_transport_completed_time_ns;
    uint64_t page_state_rdma_postcopy_dirty_transport_active_time_ns;
    uint64_t page_state_rdma_postcopy_dirty_publish_time_ns;
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
    uint64_t rdma_sidecar_postcopy_dirty_posted_spans;
    uint64_t rdma_sidecar_postcopy_dirty_posted_bytes;
    uint64_t rdma_sidecar_postcopy_dirty_completed_spans;
    uint64_t rdma_sidecar_postcopy_dirty_completed_bytes;
    uint64_t rdma_sidecar_postcopy_dirty_completed_pages;
    uint64_t rdma_sidecar_postcopy_dirty_stale_pages;
    uint64_t rdma_sidecar_postcopy_dirty_overflow_cxl_spans;
    uint64_t rdma_sidecar_postcopy_dirty_min_span_cxl_spans;
    uint64_t rdma_sidecar_postcopy_dirty_max_span_bytes;
    uint32_t rdma_sidecar_postcopy_dirty_max_inflight_wr;
    uint32_t rdma_sidecar_postcopy_dirty_queue_wr;
    uint32_t rdma_sidecar_postcopy_dirty_inflight_wr;
    uint64_t rdma_sidecar_postcopy_dirty_max_inflight_bytes;
    uint64_t rdma_sidecar_postcopy_dirty_queue_bytes;
    uint64_t rdma_sidecar_postcopy_dirty_inflight_bytes;
    uint64_t rdma_sidecar_postcopy_dirty_min_span_bytes;
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

typedef struct CXLHybridRDMAPostcopyDirtyAdmissionSnapshot {
    bool accept_rdma;
    uint32_t max_inflight_wr;
    uint32_t queue_wr;
    uint32_t inflight_wr;
    uint32_t reserved_wr;
    uint32_t outstanding_wr;
    uint64_t max_inflight_bytes;
    uint64_t queue_bytes;
    uint64_t inflight_bytes;
    uint64_t reserved_bytes;
    uint64_t outstanding_bytes;
    uint64_t min_span_bytes;
    uint64_t accepted_spans;
    uint64_t overflow_cxl_spans;
    uint64_t min_span_cxl_spans;
} CXLHybridRDMAPostcopyDirtyAdmissionSnapshot;

typedef struct CXLHybridRDMAPostcopyDirtyAdmissionState {
    uint32_t max_inflight_wr;
    uint32_t reserved_wr;
    uint64_t max_inflight_bytes;
    uint64_t reserved_bytes;
    uint64_t min_span_bytes;
    uint64_t accepted_spans;
    uint64_t overflow_cxl_spans;
    uint64_t min_span_cxl_spans;
} CXLHybridRDMAPostcopyDirtyAdmissionState;

typedef struct CXLHybridRDMAPostcopyDirtyAdmissionReservation {
    bool valid;
    uint64_t owner;
    uint64_t bytes;
} CXLHybridRDMAPostcopyDirtyAdmissionReservation;

typedef struct CXLHybridRDMASidecarConfig {
    const MigrationAddress *addr;
    uint64_t total_regions;
    uint64_t bytes_per_region;
    uint64_t pages_per_region;
    uint32_t max_inflight_regions;
    uint64_t postcopy_dirty_min_span_bytes;
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
void cxl_rdma_sidecar_bulk_stats_note_active_epoch(
    CXLHybridRDMASidecarBulkStats *stats,
    uint64_t useful_bytes,
    uint64_t active_time_ns);
void cxl_rdma_sidecar_bulk_stats_note_active_epoch_for_kind(
    CXLHybridRDMASidecarBulkStats *stats,
    CXLHybridRDMAClaimKind kind,
    uint64_t useful_bytes,
    uint64_t active_time_ns);
void cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch(
    CXLHybridRDMASidecarBulkStats *stats,
    uint64_t useful_bytes,
    uint64_t active_time_ns);
void cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch_for_kind(
    CXLHybridRDMASidecarBulkStats *stats,
    CXLHybridRDMAClaimKind kind,
    uint64_t useful_bytes,
    uint64_t active_time_ns);
void cxl_rdma_sidecar_bulk_stats_note_publish_time(
    CXLHybridRDMASidecarBulkStats *stats,
    uint64_t publish_time_ns);
uint32_t cxl_rdma_sidecar_effective_inflight_capacity(
    uint64_t total_regions,
    uint32_t resource_capacity_regions,
    bool pin_all);
bool cxl_rdma_sidecar_schedule_allowed(bool postcopy,
                                       bool bulk_active,
                                       bool draining);
bool cxl_rdma_sidecar_claim_schedule_allowed(
    CXLHybridRDMAClaimKind kind,
    bool postcopy,
    bool bulk_active,
    bool draining);
bool cxl_rdma_sidecar_should_wait_for_sq_room_for_test(int post_send_ret,
                                                       uint32_t inflight_len);
bool cxl_rdma_sidecar_should_poll_after_cq_wait_for_test(int wait_ret);
bool cxl_rdma_sidecar_should_retry_after_stale_cq_event_for_test(
    int wait_ret,
    int completion_ret);
void cxl_rdma_sidecar_postcopy_dirty_admission_state_init(
    CXLHybridRDMAPostcopyDirtyAdmissionState *state,
    uint32_t max_inflight_wr,
    uint64_t max_inflight_bytes,
    uint64_t min_span_bytes);
CXLHybridRDMAPostcopyDirtyAdmissionSnapshot
cxl_rdma_sidecar_postcopy_dirty_admission_snapshot(
    const CXLHybridRDMAPostcopyDirtyAdmissionState *state,
    bool running,
    bool postcopy,
    bool bulk_active,
    bool draining,
    bool failed,
    uint64_t span_bytes,
    uint32_t queue_wr,
    uint32_t inflight_wr,
    uint64_t queue_bytes,
    uint64_t inflight_bytes);
bool cxl_rdma_sidecar_postcopy_dirty_admission_try_reserve(
    CXLHybridRDMAPostcopyDirtyAdmissionState *state,
    bool running,
    bool postcopy,
    bool bulk_active,
    bool draining,
    bool failed,
    uint64_t span_bytes,
    uint32_t queue_wr,
    uint32_t inflight_wr,
    uint64_t queue_bytes,
    uint64_t inflight_bytes,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation,
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot *snapshot);
bool cxl_rdma_sidecar_postcopy_dirty_admission_try_reserve_for_claims_for_test(
    CXLHybridRDMAPostcopyDirtyAdmissionState *state,
    bool running,
    bool postcopy,
    bool bulk_active,
    bool draining,
    bool failed,
    uint64_t span_bytes,
    const CXLHybridRDMABulkClaim *queued_claims,
    uint32_t queued_claims_len,
    const CXLHybridRDMABulkClaim *inflight_claims,
    uint32_t inflight_claims_len,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation,
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot *snapshot);
void cxl_rdma_sidecar_postcopy_dirty_admission_cancel_reserve(
    CXLHybridRDMAPostcopyDirtyAdmissionState *state,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation);
void cxl_rdma_sidecar_postcopy_dirty_admission_consume_reserve(
    CXLHybridRDMAPostcopyDirtyAdmissionState *state,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation);

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
bool cxl_rdma_sidecar_try_reserve_postcopy_dirty_admission(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation,
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot *snapshot);
bool cxl_rdma_sidecar_enqueue_reserved_postcopy_dirty_claim(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation);
void cxl_rdma_sidecar_cancel_postcopy_dirty_admission(
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation);
bool cxl_rdma_sidecar_enqueue_bulk_claim(
    const CXLHybridRDMABulkClaim *claim);
void cxl_rdma_sidecar_drain_bulk_claims(void);
void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats);

#endif
