/*
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "qapi/error.h"
#include "cxl.h"
#include "cxl-rdma.h"
#include "system/ramblock.h"
#include "trace.h"

#define CXL_RDMA_ADMISSION_MIN_WINDOW 1U
#define CXL_RDMA_ADMISSION_INITIAL_WINDOW 8U
#define CXL_RDMA_ADMISSION_EWMA_WEIGHT 8.0
#define CXL_RDMA_ADMISSION_DROP_GOODPUT_FACTOR 0.80
#define CXL_RDMA_ADMISSION_DROP_LATENCY_FACTOR 1.10
#define CXL_RDMA_CLAIM_KIND_COUNT 2

static int cxl_rdma_sidecar_claim_kind_index(CXLHybridRDMAClaimKind kind)
{
    switch (kind) {
    case CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION:
    case CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN:
        return kind;
    default:
        return -1;
    }
}

static uint32_t cxl_rdma_admission_clamp_window(uint32_t value,
                                                uint32_t cap)
{
    if (!cap) {
        return 0;
    }
    return MIN(MAX(value, CXL_RDMA_ADMISSION_MIN_WINDOW), cap);
}

static void cxl_rdma_sidecar_admission_reservation_clear(
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    if (!reservation) {
        return;
    }

    reservation->valid = false;
    reservation->owner = 0;
}

void cxl_rdma_sidecar_admission_state_init(
    CXLHybridRDMASidecarAdmissionState *state,
    uint32_t sq_capacity_regions,
    uint64_t bytes_per_region)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->sq_capacity_regions = MAX((uint32_t)1, sq_capacity_regions);
    state->dynamic_window_regions = cxl_rdma_admission_clamp_window(
        MIN(CXL_RDMA_ADMISSION_INITIAL_WINDOW, state->sq_capacity_regions),
        state->sq_capacity_regions);
    state->bytes_per_region = bytes_per_region;
}

static uint32_t cxl_rdma_sidecar_admission_bdp_regions(
    const CXLHybridRDMASidecarAdmissionState *state)
{
    double bytes_in_flight;
    uint64_t estimated_bytes;
    uint64_t bytes_per_region;
    uint64_t estimated_regions;

    if (!state || state->goodput_ewma_bytes_per_ns <= 0.0 ||
        !state->completion_latency_ewma_ns || !state->bytes_per_region) {
        return 0;
    }

    bytes_per_region = state->bytes_per_region;
    bytes_in_flight = state->goodput_ewma_bytes_per_ns *
                      (double)state->completion_latency_ewma_ns;
    estimated_bytes = bytes_in_flight >= (double)UINT64_MAX ?
                      UINT64_MAX : (uint64_t)bytes_in_flight;
    if (!estimated_bytes) {
        return 1;
    }
    estimated_regions = estimated_bytes / bytes_per_region;
    if (estimated_bytes % bytes_per_region) {
        estimated_regions++;
    }
    if (estimated_regions >= state->sq_capacity_regions) {
        return state->sq_capacity_regions;
    }
    return cxl_rdma_admission_clamp_window((uint32_t)estimated_regions,
                                           state->sq_capacity_regions);
}

CXLHybridRDMASidecarAdmissionSnapshot cxl_rdma_sidecar_admission_snapshot(
    const CXLHybridRDMASidecarAdmissionState *state,
    bool running,
    bool bulk_active,
    bool draining,
    bool failed,
    bool postcopy,
    uint32_t queue_len,
    uint32_t inflight_len)
{
    CXLHybridRDMASidecarAdmissionSnapshot snap = { 0 };
    uint64_t outstanding_regions;
    uint32_t window;
    uint32_t bdp_regions;

    if (!state || !state->sq_capacity_regions) {
        return snap;
    }

    outstanding_regions = (uint64_t)queue_len + inflight_len +
                          state->reserved_regions;
    window = cxl_rdma_admission_clamp_window(
        state->dynamic_window_regions, state->sq_capacity_regions);
    bdp_regions = cxl_rdma_sidecar_admission_bdp_regions(state);
    if (bdp_regions && state->goodput_drop_events) {
        window = MIN(window, bdp_regions);
    }
    snap.dynamic_window_regions = window;
    snap.sq_capacity_regions = state->sq_capacity_regions;
    snap.queue_len = queue_len;
    snap.inflight_len = inflight_len;
    snap.reserved_regions = state->reserved_regions;
    snap.outstanding_regions = MIN(outstanding_regions, (uint64_t)UINT32_MAX);
    snap.goodput_ewma_bytes_per_ns = state->goodput_ewma_bytes_per_ns;
    snap.completion_latency_ewma_ns = state->completion_latency_ewma_ns;
    snap.bdp_estimate_regions = bdp_regions;
    snap.accepted_regions = state->accepted_regions;
    snap.overflow_cxl_regions = state->overflow_cxl_regions;
    snap.admission_closed_events = state->admission_closed_events;
    snap.goodput_drop_events = state->goodput_drop_events;
    snap.accept_rdma = running && bulk_active && !draining && !failed &&
                       !postcopy && outstanding_regions < window;
    return snap;
}

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
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot)
{
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    if (!state || !reservation) {
        return false;
    }

    cxl_rdma_sidecar_admission_reservation_clear(reservation);
    snap = cxl_rdma_sidecar_admission_snapshot(
        state, running, bulk_active, draining, failed, postcopy, queue_len,
        inflight_len);
    if (!snap.accept_rdma) {
        if (!running || !bulk_active || draining || failed || postcopy) {
            state->admission_closed_events++;
        } else {
            state->overflow_cxl_regions++;
        }
        if (snapshot) {
            *snapshot = cxl_rdma_sidecar_admission_snapshot(
                state, running, bulk_active, draining, failed, postcopy,
                queue_len, inflight_len);
        }
        return false;
    }

    state->reserved_regions++;
    state->accepted_regions++;
    reservation->valid = true;
    if (snapshot) {
        *snapshot = cxl_rdma_sidecar_admission_snapshot(
            state, running, bulk_active, draining, failed, postcopy, queue_len,
            inflight_len);
    }
    return true;
}

void cxl_rdma_sidecar_admission_cancel_reserve(
    CXLHybridRDMASidecarAdmissionState *state,
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    if (!state || !reservation || !reservation->valid) {
        return;
    }
    assert(state->reserved_regions > 0);
    state->reserved_regions--;
    cxl_rdma_sidecar_admission_reservation_clear(reservation);
}

void cxl_rdma_sidecar_admission_consume_reserve(
    CXLHybridRDMASidecarAdmissionState *state,
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    cxl_rdma_sidecar_admission_cancel_reserve(state, reservation);
}

void cxl_rdma_sidecar_admission_note_completion(
    CXLHybridRDMASidecarAdmissionState *state,
    uint64_t useful_bytes,
    uint64_t latency_ns)
{
    double sample_goodput;
    double old_goodput;
    uint64_t old_latency;
    bool regression;

    if (!state || !useful_bytes || !latency_ns ||
        !state->sq_capacity_regions) {
        return;
    }

    sample_goodput = (double)useful_bytes / (double)latency_ns;
    old_goodput = state->goodput_ewma_bytes_per_ns;
    old_latency = state->completion_latency_ewma_ns;
    regression = old_goodput > 0.0 && old_latency > 0 &&
                 sample_goodput <
                 old_goodput * CXL_RDMA_ADMISSION_DROP_GOODPUT_FACTOR &&
                 (double)latency_ns >
                 (double)old_latency *
                 CXL_RDMA_ADMISSION_DROP_LATENCY_FACTOR;

    if (old_goodput == 0.0) {
        state->goodput_ewma_bytes_per_ns = sample_goodput;
    } else {
        state->goodput_ewma_bytes_per_ns =
            ((old_goodput * (CXL_RDMA_ADMISSION_EWMA_WEIGHT - 1.0)) +
             sample_goodput) / CXL_RDMA_ADMISSION_EWMA_WEIGHT;
    }

    if (!old_latency) {
        state->completion_latency_ewma_ns = latency_ns;
    } else {
        state->completion_latency_ewma_ns =
            (uint64_t)((((double)old_latency *
                         (CXL_RDMA_ADMISSION_EWMA_WEIGHT - 1.0)) +
                        (double)latency_ns) /
                       CXL_RDMA_ADMISSION_EWMA_WEIGHT);
    }

    state->bdp_estimate_regions =
        cxl_rdma_sidecar_admission_bdp_regions(state);
    if (regression) {
        state->dynamic_window_regions =
            cxl_rdma_admission_clamp_window(
                state->dynamic_window_regions / 2,
                state->sq_capacity_regions);
        state->goodput_drop_events++;
        return;
    }

    if (state->dynamic_window_regions < state->sq_capacity_regions) {
        state->dynamic_window_regions++;
    }
}

void cxl_rdma_sidecar_bulk_stats_note_active_epoch(
    CXLHybridRDMASidecarBulkStats *stats,
    uint64_t useful_bytes,
    uint64_t active_time_ns)
{
    if (!stats || !useful_bytes || !active_time_ns) {
        return;
    }

    qatomic_add(&stats->page_state_rdma_active_time_ns, active_time_ns);
}

static void cxl_rdma_sidecar_bulk_stats_note_active_bucket_for_kind(
    CXLHybridRDMASidecarBulkStats *stats,
    CXLHybridRDMAClaimKind kind,
    uint64_t useful_bytes,
    uint64_t active_time_ns)
{
    if (!stats || !useful_bytes || !active_time_ns) {
        return;
    }

    switch (kind) {
    case CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION:
        qatomic_add(&stats->page_state_rdma_precopy_active_time_ns,
                    active_time_ns);
        break;
    case CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN:
        qatomic_add(&stats->page_state_rdma_postcopy_dirty_active_time_ns,
                    active_time_ns);
        break;
    default:
        break;
    }
}

void cxl_rdma_sidecar_bulk_stats_note_active_epoch_for_kind(
    CXLHybridRDMASidecarBulkStats *stats,
    CXLHybridRDMAClaimKind kind,
    uint64_t useful_bytes,
    uint64_t active_time_ns)
{
    cxl_rdma_sidecar_bulk_stats_note_active_epoch(stats, useful_bytes,
                                                  active_time_ns);
    cxl_rdma_sidecar_bulk_stats_note_active_bucket_for_kind(
        stats, kind, useful_bytes, active_time_ns);
}

void cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch(
    CXLHybridRDMASidecarBulkStats *stats,
    uint64_t useful_bytes,
    uint64_t active_time_ns)
{
    if (!stats || !useful_bytes || !active_time_ns) {
        return;
    }

    qatomic_add(&stats->page_state_rdma_transport_active_time_ns,
                active_time_ns);
}

static void
cxl_rdma_sidecar_bulk_stats_note_transport_active_bucket_for_kind(
    CXLHybridRDMASidecarBulkStats *stats,
    CXLHybridRDMAClaimKind kind,
    uint64_t useful_bytes,
    uint64_t active_time_ns)
{
    if (!stats || !useful_bytes || !active_time_ns) {
        return;
    }

    switch (kind) {
    case CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION:
        qatomic_add(
            &stats->page_state_rdma_precopy_transport_active_time_ns,
            active_time_ns);
        break;
    case CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN:
        qatomic_add(
            &stats->page_state_rdma_postcopy_dirty_transport_active_time_ns,
            active_time_ns);
        break;
    default:
        break;
    }
}

void cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch_for_kind(
    CXLHybridRDMASidecarBulkStats *stats,
    CXLHybridRDMAClaimKind kind,
    uint64_t useful_bytes,
    uint64_t active_time_ns)
{
    cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch(
        stats, useful_bytes, active_time_ns);
    cxl_rdma_sidecar_bulk_stats_note_transport_active_bucket_for_kind(
        stats, kind, useful_bytes, active_time_ns);
}

void cxl_rdma_sidecar_bulk_stats_note_publish_time(
    CXLHybridRDMASidecarBulkStats *stats,
    uint64_t publish_time_ns)
{
    if (!stats || !publish_time_ns) {
        return;
    }

    qatomic_add(&stats->page_state_rdma_publish_time_ns, publish_time_ns);
}

static void cxl_rdma_sidecar_bulk_stats_note_completion_for_kind(
    CXLHybridRDMASidecarBulkStats *stats,
    CXLHybridRDMAClaimKind kind,
    uint64_t completed_bytes,
    uint64_t completed_time_ns,
    uint64_t transport_completed_time_ns,
    uint64_t publish_time_ns)
{
    if (!stats) {
        return;
    }

    qatomic_add(&stats->page_state_rdma_completed_bytes, completed_bytes);
    qatomic_add(&stats->page_state_rdma_completed_time_ns,
                completed_time_ns);
    qatomic_add(&stats->page_state_rdma_transport_completed_time_ns,
                transport_completed_time_ns);
    cxl_rdma_sidecar_bulk_stats_note_publish_time(stats, publish_time_ns);

    switch (kind) {
    case CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION:
        qatomic_add(&stats->page_state_rdma_precopy_completed_bytes,
                    completed_bytes);
        qatomic_add(&stats->page_state_rdma_precopy_completed_time_ns,
                    completed_time_ns);
        qatomic_add(
            &stats->page_state_rdma_precopy_transport_completed_time_ns,
            transport_completed_time_ns);
        qatomic_add(&stats->page_state_rdma_precopy_publish_time_ns,
                    publish_time_ns);
        break;
    case CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN:
        qatomic_add(&stats->page_state_rdma_postcopy_dirty_completed_bytes,
                    completed_bytes);
        qatomic_add(
            &stats->page_state_rdma_postcopy_dirty_completed_time_ns,
            completed_time_ns);
        qatomic_add(
            &stats->page_state_rdma_postcopy_dirty_transport_completed_time_ns,
            transport_completed_time_ns);
        qatomic_add(&stats->page_state_rdma_postcopy_dirty_publish_time_ns,
                    publish_time_ns);
        break;
    default:
        break;
    }
}

static void cxl_rdma_sidecar_bulk_stats_note_max_u64(uint64_t *field,
                                                     uint64_t value)
{
    uint64_t old;

    if (!field || !value) {
        return;
    }

    old = qatomic_read(field);
    while (value > old &&
           qatomic_cmpxchg(field, old, value) != old) {
        old = qatomic_read(field);
    }
}

uint32_t cxl_rdma_sidecar_effective_inflight_capacity(
    uint64_t total_regions,
    uint32_t resource_capacity_regions,
    bool pin_all)
{
    uint64_t capacity;

    if (!pin_all) {
        return 1;
    }

    capacity = total_regions ? total_regions : 1;
    capacity = MIN(capacity,
                   (uint64_t)MAX((uint32_t)1, resource_capacity_regions));
    return (uint32_t)MIN(capacity, (uint64_t)UINT32_MAX);
}

bool cxl_rdma_sidecar_schedule_allowed(bool postcopy,
                                       bool bulk_active,
                                       bool draining)
{
    return !postcopy && (bulk_active || draining);
}

bool cxl_rdma_sidecar_claim_schedule_allowed(
    CXLHybridRDMAClaimKind kind,
    bool postcopy,
    bool bulk_active,
    bool draining)
{
    switch (kind) {
    case CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION:
        return !postcopy && bulk_active && !draining;
    case CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN:
        return postcopy && !bulk_active;
    default:
        return false;
    }
}

static void cxl_rdma_sidecar_postcopy_dirty_reservation_clear(
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation)
{
    if (reservation) {
        memset(reservation, 0, sizeof(*reservation));
    }
}

static void cxl_rdma_sidecar_postcopy_dirty_count_claims(
    const CXLHybridRDMABulkClaim *claims,
    uint32_t claims_len,
    uint32_t *wrp,
    uint64_t *bytesp)
{
    uint32_t wr = 0;
    uint64_t bytes = 0;

    if (claims) {
        for (uint32_t i = 0; i < claims_len; i++) {
            if (claims[i].kind != CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN) {
                continue;
            }

            wr++;
            if (bytes > UINT64_MAX - claims[i].bytes) {
                bytes = UINT64_MAX;
            } else {
                bytes += claims[i].bytes;
            }
        }
    }

    if (wrp) {
        *wrp = wr;
    }
    if (bytesp) {
        *bytesp = bytes;
    }
}

void cxl_rdma_sidecar_postcopy_dirty_admission_state_init(
    CXLHybridRDMAPostcopyDirtyAdmissionState *state,
    uint32_t max_inflight_wr,
    uint64_t max_inflight_bytes,
    uint64_t min_span_bytes)
{
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->max_inflight_wr = MAX((uint32_t)1, max_inflight_wr);
    state->max_inflight_bytes = MAX((uint64_t)TARGET_PAGE_SIZE,
                                    max_inflight_bytes);
    state->min_span_bytes = MAX((uint64_t)TARGET_PAGE_SIZE,
                                min_span_bytes);
}

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
    uint64_t inflight_bytes)
{
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot snap = { 0 };
    uint64_t outstanding_bytes;
    uint64_t outstanding_wr;

    if (!state) {
        return snap;
    }

    outstanding_wr = (uint64_t)queue_wr + inflight_wr + state->reserved_wr;
    if (outstanding_wr > UINT32_MAX) {
        outstanding_wr = UINT32_MAX;
    }
    outstanding_bytes = queue_bytes + inflight_bytes;
    if (outstanding_bytes < queue_bytes ||
        outstanding_bytes > UINT64_MAX - state->reserved_bytes) {
        outstanding_bytes = UINT64_MAX;
    } else {
        outstanding_bytes += state->reserved_bytes;
    }

    snap.max_inflight_wr = state->max_inflight_wr;
    snap.queue_wr = queue_wr;
    snap.inflight_wr = inflight_wr;
    snap.reserved_wr = state->reserved_wr;
    snap.outstanding_wr = outstanding_wr;
    snap.max_inflight_bytes = state->max_inflight_bytes;
    snap.queue_bytes = queue_bytes;
    snap.inflight_bytes = inflight_bytes;
    snap.reserved_bytes = state->reserved_bytes;
    snap.outstanding_bytes = outstanding_bytes;
    snap.min_span_bytes = state->min_span_bytes;
    snap.accepted_spans = state->accepted_spans;
    snap.overflow_cxl_spans = state->overflow_cxl_spans;
    snap.min_span_cxl_spans = state->min_span_cxl_spans;
    snap.accept_rdma =
        running && postcopy && !bulk_active && !failed &&
        span_bytes >= state->min_span_bytes &&
        outstanding_wr < state->max_inflight_wr &&
        outstanding_bytes <= state->max_inflight_bytes &&
        span_bytes <= state->max_inflight_bytes - outstanding_bytes;
    return snap;
}

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
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot *snapshot)
{
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot snap;

    cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
    if (!state || !reservation) {
        return false;
    }

    snap = cxl_rdma_sidecar_postcopy_dirty_admission_snapshot(
        state, running, postcopy, bulk_active, draining, failed, span_bytes,
        queue_wr, inflight_wr, queue_bytes, inflight_bytes);
    if (!snap.accept_rdma) {
        if (span_bytes && span_bytes < state->min_span_bytes) {
            state->min_span_cxl_spans++;
        } else {
            state->overflow_cxl_spans++;
        }
        if (snapshot) {
            *snapshot = cxl_rdma_sidecar_postcopy_dirty_admission_snapshot(
                state, running, postcopy, bulk_active, draining, failed,
                span_bytes, queue_wr, inflight_wr, queue_bytes,
                inflight_bytes);
        }
        return false;
    }

    state->reserved_wr++;
    state->reserved_bytes += span_bytes;
    state->accepted_spans++;
    reservation->valid = true;
    reservation->bytes = span_bytes;
    if (snapshot) {
        *snapshot = cxl_rdma_sidecar_postcopy_dirty_admission_snapshot(
            state, running, postcopy, bulk_active, draining, failed,
            span_bytes, queue_wr, inflight_wr, queue_bytes, inflight_bytes);
    }
    return true;
}

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
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot *snapshot)
{
    uint32_t queue_wr = 0;
    uint32_t inflight_wr = 0;
    uint64_t queue_bytes = 0;
    uint64_t inflight_bytes = 0;

    cxl_rdma_sidecar_postcopy_dirty_count_claims(
        queued_claims, queued_claims_len, &queue_wr, &queue_bytes);
    cxl_rdma_sidecar_postcopy_dirty_count_claims(
        inflight_claims, inflight_claims_len, &inflight_wr, &inflight_bytes);

    return cxl_rdma_sidecar_postcopy_dirty_admission_try_reserve(
        state, running, postcopy, bulk_active, draining, failed, span_bytes,
        queue_wr, inflight_wr, queue_bytes, inflight_bytes, reservation,
        snapshot);
}

void cxl_rdma_sidecar_postcopy_dirty_admission_cancel_reserve(
    CXLHybridRDMAPostcopyDirtyAdmissionState *state,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation)
{
    if (!state || !reservation || !reservation->valid) {
        return;
    }

    assert(state->reserved_wr > 0);
    assert(state->reserved_bytes >= reservation->bytes);
    state->reserved_wr--;
    state->reserved_bytes -= reservation->bytes;
    cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
}

void cxl_rdma_sidecar_postcopy_dirty_admission_consume_reserve(
    CXLHybridRDMAPostcopyDirtyAdmissionState *state,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation)
{
    cxl_rdma_sidecar_postcopy_dirty_admission_cancel_reserve(state,
                                                             reservation);
}

bool cxl_rdma_sidecar_should_wait_for_sq_room_for_test(int post_send_ret,
                                                       uint32_t inflight_len)
{
    return post_send_ret == ENOMEM && inflight_len > 0;
}

bool cxl_rdma_sidecar_should_poll_after_cq_wait_for_test(int wait_ret)
{
    return wait_ret >= 0;
}

bool cxl_rdma_sidecar_should_retry_after_stale_cq_event_for_test(
    int wait_ret,
    int completion_ret)
{
    return wait_ret > 0 && completion_ret == 0;
}

#ifdef CONFIG_RDMA
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#endif

#define CXL_RDMA_SIDECAR_MAGIC 0x43585244u
#define CXL_RDMA_SIDECAR_VERSION 1
#define CXL_RDMA_RESOLVE_TIMEOUT_MS 10000
#define CXL_RDMA_CQ_DEPTH 1024
#define CXL_RDMA_POSTCOPY_DIRTY_MIN_SPAN_BYTES_DEFAULT (64 * 1024)

typedef struct CXLRDMASidecarHello {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t remote_base;
    uint64_t remote_len;
    uint32_t remote_rkey;
    uint32_t region_shift;
    uint32_t page_shift;
    uint32_t reserved;
} QEMU_PACKED CXLRDMASidecarHello;

#ifdef CONFIG_RDMA
typedef struct CXLRDMASidecarMR {
    void *host;
    uint64_t length;
    struct ibv_mr *mr;
} CXLRDMASidecarMR;

typedef struct CXLRDMASidecarContext {
    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    bool thread_created;
    bool stop;
    bool running;
    bool failed;
    bool setup_done;
    bool incoming;
    bool connected;
    char *host;
    char *port;
    uint64_t total_regions;
    uint64_t bytes_per_region;
    uint64_t pages_per_region;
    uint32_t max_inflight_regions;
    uint64_t postcopy_dirty_min_span_bytes;
    bool pin_all;
    bool src_mr_inflight;
    bool draining;
    CXLHybridRDMABulkClaim *queue;
    uint32_t queue_capacity;
    uint32_t queue_head;
    uint32_t queue_len;
    CXLHybridRDMABulkClaim *inflight_claims;
    uint32_t inflight_capacity;
    uint32_t inflight_len;
    uint64_t next_claim_id;
    uint64_t rdma_active_start_ns;
    uint64_t rdma_active_completed_bytes;
    uint32_t rdma_active_inflight_by_kind[CXL_RDMA_CLAIM_KIND_COUNT];
    uint64_t rdma_active_start_ns_by_kind[CXL_RDMA_CLAIM_KIND_COUNT];
    uint64_t rdma_active_completed_bytes_by_kind[CXL_RDMA_CLAIM_KIND_COUNT];
    CXLHybridRDMASidecarAdmissionState admission;
    CXLHybridRDMAPostcopyDirtyAdmissionState postcopy_dirty_admission;
    uint64_t admission_owner;
    uint64_t postcopy_dirty_admission_owner;
    uint64_t queue_bytes;
    uint64_t inflight_bytes;
    struct rdma_event_channel *channel;
    struct rdma_cm_id *listen_id;
    struct rdma_cm_id *cm_id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_channel;
    struct ibv_qp *qp;
    struct ibv_mr *dst_mr;
    struct ibv_mr *src_mr;
    CXLRDMASidecarMR *source_mrs;
    uint32_t nr_source_mrs;
    struct ibv_mr *hello_send_mr;
    struct ibv_mr *hello_recv_mr;
    CXLRDMASidecarHello hello_send;
    CXLRDMASidecarHello hello_recv;
    uint64_t remote_base;
    uint64_t remote_len;
    uint32_t remote_rkey;
    CXLHybridRDMASidecarOps ops;
    CXLHybridRDMASidecarBulkStats stats;
} CXLRDMASidecarContext;

typedef struct CXLRDMASidecarDestinationRegister {
    CXLRDMASidecarContext *ctx;
    Error **errp;
    void *host;
    uint64_t length;
} CXLRDMASidecarDestinationRegister;

static CXLRDMASidecarContext *cxl_rdma_sidecar;
static uint64_t cxl_rdma_sidecar_admission_owner_counter;

static void cxl_rdma_sidecar_apply_transport_capacity(
    CXLRDMASidecarContext *ctx,
    uint32_t capacity)
{
    uint64_t postcopy_dirty_window_bytes;

    capacity = MAX((uint32_t)1, capacity);
    assert(!ctx->queue_len);
    assert(!ctx->inflight_len);

    ctx->queue_capacity = MIN(ctx->queue_capacity, capacity);
    ctx->inflight_capacity = MIN(ctx->inflight_capacity, capacity);
    cxl_rdma_sidecar_admission_state_init(&ctx->admission,
                                          ctx->inflight_capacity,
                                          ctx->bytes_per_region);
    if (ctx->inflight_capacity &&
        ctx->bytes_per_region <= UINT64_MAX / ctx->inflight_capacity) {
        postcopy_dirty_window_bytes =
            ctx->bytes_per_region * (uint64_t)ctx->inflight_capacity;
    } else {
        postcopy_dirty_window_bytes = UINT64_MAX;
    }
    cxl_rdma_sidecar_postcopy_dirty_admission_state_init(
        &ctx->postcopy_dirty_admission, ctx->inflight_capacity,
        postcopy_dirty_window_bytes,
        ctx->postcopy_dirty_min_span_bytes);
}

static uint64_t cxl_rdma_sidecar_next_admission_owner(void)
{
    uint64_t owner;

    do {
        owner = qatomic_inc_fetch(&cxl_rdma_sidecar_admission_owner_counter);
    } while (!owner);

    return owner;
}

static uint64_t cxl_rdma_sidecar_next_claim_id(CXLRDMASidecarContext *ctx)
{
    uint64_t id = ++ctx->next_claim_id;

    if (!id) {
        id = ++ctx->next_claim_id;
    }
    return id;
}

static void cxl_rdma_sidecar_add_queue_bytes(CXLRDMASidecarContext *ctx,
                                             uint64_t bytes)
{
    assert(ctx->queue_bytes <= UINT64_MAX - bytes);
    ctx->queue_bytes += bytes;
}

static void cxl_rdma_sidecar_sub_queue_bytes(CXLRDMASidecarContext *ctx,
                                             uint64_t bytes)
{
    assert(ctx->queue_bytes >= bytes);
    ctx->queue_bytes -= bytes;
}

static void cxl_rdma_sidecar_add_inflight_bytes(CXLRDMASidecarContext *ctx,
                                                uint64_t bytes)
{
    assert(ctx->inflight_bytes <= UINT64_MAX - bytes);
    ctx->inflight_bytes += bytes;
}

static void cxl_rdma_sidecar_sub_inflight_bytes(CXLRDMASidecarContext *ctx,
                                                uint64_t bytes)
{
    assert(ctx->inflight_bytes >= bytes);
    ctx->inflight_bytes -= bytes;
}

static void cxl_rdma_sidecar_drop_claim(CXLRDMASidecarContext *ctx,
                                        const CXLHybridRDMABulkClaim *claim)
{
    if (ctx->ops.drop_bulk_claim) {
        ctx->ops.drop_bulk_claim(claim, ctx->ops.opaque);
    }
}

static bool cxl_rdma_sidecar_stopped(CXLRDMASidecarContext *ctx)
{
    bool stopped;

    qemu_mutex_lock(&ctx->lock);
    stopped = ctx->stop;
    qemu_mutex_unlock(&ctx->lock);
    return stopped;
}

static void cxl_rdma_sidecar_mark_failed(CXLRDMASidecarContext *ctx,
                                         Error *err)
{
    qemu_mutex_lock(&ctx->lock);
    if (!ctx->stop) {
        ctx->failed = true;
    }
    ctx->setup_done = true;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);

    if (!ctx->failed) {
        error_free(err);
        return;
    }

    if (err) {
        if (ctx->ops.propagate_error) {
            ctx->ops.propagate_error(error_copy(err), ctx->ops.opaque);
        }
        error_report_err(err);
    }
}

static bool cxl_rdma_sidecar_geometry_shift(uint64_t bytes, uint32_t *shiftp)
{
    if (!bytes || !is_power_of_2(bytes) || bytes > UINT32_MAX) {
        return false;
    }

    *shiftp = ctz64(bytes);
    return true;
}

static int cxl_rdma_sidecar_parse_addr(CXLRDMASidecarContext *ctx,
                                       const MigrationAddress *addr,
                                       Error **errp)
{
    if (!addr || addr->transport != MIGRATION_ADDRESS_TYPE_RDMA) {
        error_setg(errp, "RDMA sidecar requires an rdma migration address");
        return -1;
    }
    if (!addr->u.rdma.host || !addr->u.rdma.port) {
        error_setg(errp, "RDMA sidecar address requires host and port");
        return -1;
    }

    ctx->host = g_strdup(addr->u.rdma.host);
    ctx->port = g_strdup(addr->u.rdma.port);
    return 0;
}

static int cxl_rdma_sidecar_wait_event(CXLRDMASidecarContext *ctx,
                                       enum rdma_cm_event_type expected,
                                       struct rdma_cm_event **eventp,
                                       Error **errp)
{
    struct rdma_cm_event *event;
    struct pollfd pfd = {
        .fd = ctx->channel->fd,
        .events = POLLIN,
    };

    for (;;) {
        int ret;

        qemu_mutex_lock(&ctx->lock);
        if (ctx->stop) {
            qemu_mutex_unlock(&ctx->lock);
            error_setg(errp, "RDMA sidecar stopped while waiting for CM event");
            return -1;
        }
        qemu_mutex_unlock(&ctx->lock);

        ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_setg_errno(errp, errno,
                             "RDMA sidecar failed to poll CM event channel");
            return -1;
        }
        if (ret > 0) {
            break;
        }
    }

    if (rdma_get_cm_event(ctx->channel, &event) < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar failed to get CM event");
        return -1;
    }
    if (event->event != expected) {
        error_setg(errp, "RDMA sidecar expected CM event %s, got %s",
                   rdma_event_str(expected), rdma_event_str(event->event));
        rdma_ack_cm_event(event);
        return -1;
    }

    *eventp = event;
    return 0;
}

static int cxl_rdma_sidecar_wait_wc(CXLRDMASidecarContext *ctx,
                                    uint64_t wr_id,
                                    Error **errp)
{
    struct ibv_wc wc;

    for (;;) {
        int ret;

        qemu_mutex_lock(&ctx->lock);
        if (ctx->stop) {
            qemu_mutex_unlock(&ctx->lock);
            error_setg(errp, "RDMA sidecar stopped while waiting for CQE");
            return -1;
        }
        qemu_mutex_unlock(&ctx->lock);

        ret = ibv_poll_cq(ctx->cq, 1, &wc);
        if (ret < 0) {
            error_setg_errno(errp, errno,
                             "RDMA sidecar failed to poll completion queue");
            return -1;
        }
        if (ret == 0) {
            g_usleep(1000);
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            error_setg(errp, "RDMA sidecar CQE failed: %s",
                       ibv_wc_status_str(wc.status));
            return -1;
        }
        if (wc.wr_id != wr_id) {
            error_setg(errp,
                       "RDMA sidecar expected CQE wr_id %" PRIu64
                       ", got %" PRIu64,
                       wr_id, wc.wr_id);
            return -1;
        }
        return 0;
    }
}

static int cxl_rdma_sidecar_resolve_source(CXLRDMASidecarContext *ctx,
                                           Error **errp)
{
    g_autofree char *port_str = NULL;
    struct rdma_addrinfo *res = NULL;
    struct rdma_addrinfo *e;
    struct rdma_cm_event *event = NULL;
    int ret;

    ctx->channel = rdma_create_event_channel();
    if (!ctx->channel) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not create event channel");
        return -1;
    }

    ret = rdma_create_id(ctx->channel, &ctx->cm_id, NULL, RDMA_PS_TCP);
    if (ret < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar could not create CM id");
        return -1;
    }

    port_str = g_strdup(ctx->port);
    ret = rdma_getaddrinfo(ctx->host, port_str, NULL, &res);
    if (ret) {
        error_setg(errp, "RDMA sidecar could not resolve %s:%s",
                   ctx->host, ctx->port);
        return -1;
    }

    for (e = res; e; e = e->ai_next) {
        ret = rdma_resolve_addr(ctx->cm_id, NULL, e->ai_dst_addr,
                                CXL_RDMA_RESOLVE_TIMEOUT_MS);
        if (ret >= 0) {
            break;
        }
    }
    rdma_freeaddrinfo(res);
    if (!e) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not resolve address");
        return -1;
    }

    if (cxl_rdma_sidecar_wait_event(ctx, RDMA_CM_EVENT_ADDR_RESOLVED,
                                    &event, errp)) {
        return -1;
    }
    rdma_ack_cm_event(event);

    if (rdma_resolve_route(ctx->cm_id, CXL_RDMA_RESOLVE_TIMEOUT_MS) < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar could not resolve route");
        return -1;
    }
    if (cxl_rdma_sidecar_wait_event(ctx, RDMA_CM_EVENT_ROUTE_RESOLVED,
                                    &event, errp)) {
        return -1;
    }
    rdma_ack_cm_event(event);
    return 0;
}

static int cxl_rdma_sidecar_listen_dest(CXLRDMASidecarContext *ctx,
                                        Error **errp)
{
    struct rdma_addrinfo *res = NULL;
    struct rdma_addrinfo *e;
    int reuse = 1;
    int ret;

    ctx->channel = rdma_create_event_channel();
    if (!ctx->channel) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not create event channel");
        return -1;
    }

    ret = rdma_create_id(ctx->channel, &ctx->listen_id, NULL, RDMA_PS_TCP);
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not create listen CM id");
        return -1;
    }

    ret = rdma_getaddrinfo(ctx->host, ctx->port, NULL, &res);
    if (ret) {
        error_setg(errp, "RDMA sidecar could not resolve listen address %s:%s",
                   ctx->host, ctx->port);
        return -1;
    }

    ret = rdma_set_option(ctx->listen_id, RDMA_OPTION_ID,
                          RDMA_OPTION_ID_REUSEADDR, &reuse, sizeof(reuse));
    if (ret < 0) {
        rdma_freeaddrinfo(res);
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not set REUSEADDR");
        return -1;
    }

    for (e = res; e; e = e->ai_next) {
        ret = rdma_bind_addr(ctx->listen_id, e->ai_dst_addr);
        if (ret >= 0) {
            break;
        }
    }
    rdma_freeaddrinfo(res);
    if (!e) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not bind listen address");
        return -1;
    }

    if (rdma_listen(ctx->listen_id, 1) < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar listen failed");
        return -1;
    }
    return 0;
}

static int cxl_rdma_sidecar_accept_id(CXLRDMASidecarContext *ctx, Error **errp)
{
    struct rdma_cm_event *event = NULL;

    if (cxl_rdma_sidecar_wait_event(ctx, RDMA_CM_EVENT_CONNECT_REQUEST,
                                    &event, errp)) {
        return -1;
    }

    ctx->cm_id = event->id;
    rdma_ack_cm_event(event);
    return 0;
}

static int cxl_rdma_sidecar_alloc_pd_cq_qp(CXLRDMASidecarContext *ctx,
                                           Error **errp)
{
    struct ibv_qp_init_attr attr = { 0 };
    struct ibv_device_attr device_attr = { 0 };
    uint32_t max_send_wr;

    ctx->pd = ibv_alloc_pd(ctx->cm_id->verbs);
    if (!ctx->pd) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to allocate protection domain");
        return -1;
    }
    ctx->cm_id->pd = ctx->pd;

    ctx->comp_channel = ibv_create_comp_channel(ctx->cm_id->verbs);
    if (!ctx->comp_channel) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to allocate completion channel");
        return -1;
    }

    ctx->cq = ibv_create_cq(ctx->cm_id->verbs, CXL_RDMA_CQ_DEPTH, NULL,
                            ctx->comp_channel, 0);
    if (!ctx->cq) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to allocate completion queue");
        return -1;
    }

    max_send_wr = MAX((uint32_t)1, ctx->inflight_capacity);
    if (!ibv_query_device(ctx->cm_id->verbs, &device_attr) &&
        device_attr.max_qp_wr > 0) {
        max_send_wr = MIN(max_send_wr, (uint32_t)device_attr.max_qp_wr);
    }
    cxl_rdma_sidecar_apply_transport_capacity(ctx, max_send_wr);

    attr.cap.max_send_wr = ctx->inflight_capacity;
    attr.cap.max_recv_wr = 2;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.send_cq = ctx->cq;
    attr.recv_cq = ctx->cq;
    attr.qp_type = IBV_QPT_RC;

    if (rdma_create_qp(ctx->cm_id, ctx->pd, &attr) < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar failed to create QP");
        return -1;
    }

    ctx->qp = ctx->cm_id->qp;
    return 0;
}

static int cxl_rdma_sidecar_fill_hello(CXLRDMASidecarContext *ctx,
                                       CXLRDMASidecarHello *hello,
                                       Error **errp)
{
    uint32_t region_shift = 0;
    uint32_t page_shift = 0;

    if (!ctx->pages_per_region ||
        !cxl_rdma_sidecar_geometry_shift(ctx->bytes_per_region,
                                         &region_shift) ||
        !cxl_rdma_sidecar_geometry_shift(ctx->bytes_per_region /
                                         ctx->pages_per_region, &page_shift)) {
        error_setg(errp, "RDMA sidecar region/page geometry is invalid");
        return -1;
    }

    *hello = (CXLRDMASidecarHello) {
        .magic = cpu_to_be32(CXL_RDMA_SIDECAR_MAGIC),
        .version = cpu_to_be16(CXL_RDMA_SIDECAR_VERSION),
        .flags = 0,
        .remote_base = cpu_to_be64(ctx->dst_mr ?
                                   (uintptr_t)ctx->dst_mr->addr : 0),
        .remote_len = cpu_to_be64(ctx->dst_mr ? ctx->dst_mr->length : 0),
        .remote_rkey = cpu_to_be32(ctx->dst_mr ? ctx->dst_mr->rkey : 0),
        .region_shift = cpu_to_be32(region_shift),
        .page_shift = cpu_to_be32(page_shift),
        .reserved = 0,
    };
    return 0;
}

static int cxl_rdma_sidecar_validate_hello(CXLRDMASidecarContext *ctx,
                                           const CXLRDMASidecarHello *hello,
                                           Error **errp)
{
    uint32_t region_shift = 0;
    uint32_t page_shift = 0;
    uint64_t remote_len;

    if (!cxl_rdma_sidecar_geometry_shift(ctx->bytes_per_region,
                                         &region_shift) ||
        !cxl_rdma_sidecar_geometry_shift(ctx->bytes_per_region /
                                         ctx->pages_per_region, &page_shift)) {
        error_setg(errp, "RDMA sidecar region/page geometry is invalid");
        return -1;
    }

    if (be32_to_cpu(hello->magic) != CXL_RDMA_SIDECAR_MAGIC ||
        be16_to_cpu(hello->version) != CXL_RDMA_SIDECAR_VERSION) {
        error_setg(errp, "RDMA sidecar hello version mismatch");
        return -1;
    }
    if (be32_to_cpu(hello->region_shift) != region_shift ||
        be32_to_cpu(hello->page_shift) != page_shift) {
        error_setg(errp, "RDMA sidecar region geometry mismatch");
        return -1;
    }

    remote_len = be64_to_cpu(hello->remote_len);
    if (!remote_len || remote_len < ctx->bytes_per_region) {
        error_setg(errp,
                   "RDMA sidecar destination MR too small: %" PRIu64
                   " < %" PRIu64,
                   remote_len, ctx->bytes_per_region);
        return -1;
    }

    ctx->remote_base = be64_to_cpu(hello->remote_base);
    ctx->remote_len = remote_len;
    ctx->remote_rkey = be32_to_cpu(hello->remote_rkey);
    return 0;
}

static int cxl_rdma_sidecar_post_hello_recv(CXLRDMASidecarContext *ctx,
                                            Error **errp)
{
    int ret;

    if (ctx->hello_recv_mr) {
        return 0;
    }

    ctx->hello_recv_mr = ibv_reg_mr(ctx->pd, &ctx->hello_recv,
                                    sizeof(ctx->hello_recv),
                                    IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->hello_recv_mr) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to register hello recv buffer");
        return -1;
    }

    ret = rdma_post_recv(ctx->cm_id, (void *)(uintptr_t)2,
                         &ctx->hello_recv, sizeof(ctx->hello_recv),
                         ctx->hello_recv_mr);
    if (ret) {
        error_setg_errno(errp, ret,
                         "RDMA sidecar failed to post destination hello recv");
        return -1;
    }
    return 0;
}

static int cxl_rdma_sidecar_exchange_hello(CXLRDMASidecarContext *ctx,
                                           Error **errp)
{
    int ret;

    if (ctx->incoming) {
        if (cxl_rdma_sidecar_fill_hello(ctx, &ctx->hello_send, errp)) {
            return -1;
        }
        ctx->hello_send_mr = ibv_reg_mr(ctx->pd, &ctx->hello_send,
                                        sizeof(ctx->hello_send),
                                        IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->hello_send_mr) {
            error_setg_errno(
                errp, errno,
                "RDMA sidecar failed to register hello send buffer");
            return -1;
        }

        ret = rdma_post_send(ctx->cm_id, (void *)(uintptr_t)1,
                             &ctx->hello_send, sizeof(ctx->hello_send),
                             ctx->hello_send_mr, IBV_SEND_SIGNALED);
        if (ret) {
            error_setg_errno(errp, ret,
                             "RDMA sidecar failed to send destination hello");
            return -1;
        }
        return cxl_rdma_sidecar_wait_wc(ctx, 1, errp);
    }

    if (cxl_rdma_sidecar_post_hello_recv(ctx, errp)) {
        return -1;
    }
    if (cxl_rdma_sidecar_wait_wc(ctx, 2, errp)) {
        return -1;
    }
    return cxl_rdma_sidecar_validate_hello(ctx, &ctx->hello_recv, errp);
}

static int cxl_rdma_sidecar_connect_qp(CXLRDMASidecarContext *ctx, Error **errp)
{
    struct rdma_conn_param conn_param = {
        .initiator_depth = 2,
        .responder_resources = 2,
        .retry_count = 5,
    };
    struct rdma_cm_event *event = NULL;
    int ret;

    if (ctx->incoming) {
        ret = rdma_accept(ctx->cm_id, &conn_param);
    } else {
        ret = rdma_connect(ctx->cm_id, &conn_param);
    }
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar connection handshake failed");
        return -1;
    }

    if (cxl_rdma_sidecar_wait_event(ctx, RDMA_CM_EVENT_ESTABLISHED,
                                    &event, errp)) {
        return -1;
    }
    rdma_ack_cm_event(event);
    ctx->connected = true;
    return 0;
}

static int cxl_rdma_sidecar_register_destination_ramblock(RAMBlock *block,
                                                          void *opaque)
{
    CXLRDMASidecarDestinationRegister *reg = opaque;

    if (!block->host || !block->used_length) {
        return 0;
    }

    if (!reg->host || block->used_length > reg->length) {
        reg->host = block->host;
        reg->length = block->used_length;
    }
    return 0;
}

static int cxl_rdma_sidecar_register_destination(CXLRDMASidecarContext *ctx,
                                                 Error **errp)
{
    CXLRDMASidecarDestinationRegister reg = {
        .ctx = ctx,
        .errp = errp,
    };
    int access = IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ;
    int ret;

    if (!ctx->incoming) {
        return 0;
    }

    if (!ctx->ops.foreach_ramblock) {
        error_setg(errp,
                   "RDMA sidecar destination RAM registration requires a RAMBlock iterator");
        return -1;
    }

    ret = ctx->ops.foreach_ramblock(
        cxl_rdma_sidecar_register_destination_ramblock, &reg, ctx->ops.opaque);
    if (ret) {
        return -1;
    }
    if (!reg.host || !reg.length) {
        error_setg(errp, "RDMA sidecar destination RAMBlock is not mapped");
        return -1;
    }
    if (reg.length < ctx->bytes_per_region) {
        error_setg(errp,
                   "RDMA sidecar destination RAM MR too small: %" PRIu64
                   " < %" PRIu64, reg.length, ctx->bytes_per_region);
        return -1;
    }

    ctx->dst_mr = ibv_reg_mr(ctx->pd, reg.host, reg.length, access);
    if (!ctx->dst_mr) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to register destination RAM MR");
        return -1;
    }
    cxl_hybrid_account_rdma_sidecar_registered(reg.length);
    return 0;
}

static int cxl_rdma_sidecar_register_source_region(CXLRDMASidecarContext *ctx,
                                                   const CXLHybridRDMABulkClaim *claim,
                                                   Error **errp)
{
    int access = IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ;

    if (!claim || !claim->src || !claim->bytes) {
        error_setg(errp, "RDMA sidecar source claim is empty");
        return -1;
    }
    if (ctx->src_mr_inflight) {
        error_setg(errp,
                   "RDMA sidecar source MR already has an in-flight write");
        return -1;
    }
    if (ctx->src_mr) {
        ibv_dereg_mr(ctx->src_mr);
        ctx->src_mr = NULL;
    }

    ctx->src_mr = ibv_reg_mr(ctx->pd, claim->src, claim->bytes, access);
    if (!ctx->src_mr) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to register source RAM MR");
        return -1;
    }
    cxl_hybrid_account_rdma_sidecar_registered(claim->bytes);
    ctx->src_mr_inflight = true;
    return 0;
}

static int cxl_rdma_sidecar_register_one_ramblock(RAMBlock *block,
                                                  void *opaque)
{
    CXLRDMASidecarContext *ctx = opaque;
    CXLRDMASidecarMR entry;
    int access = IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ;

    if (!block->host || !block->used_length) {
        return 0;
    }

    entry = (CXLRDMASidecarMR) {
        .host = block->host,
        .length = block->used_length,
        .mr = ibv_reg_mr(ctx->pd, block->host, block->used_length, access),
    };
    if (!entry.mr) {
        return -1;
    }

    ctx->source_mrs = g_renew(CXLRDMASidecarMR, ctx->source_mrs,
                              ctx->nr_source_mrs + 1);
    ctx->source_mrs[ctx->nr_source_mrs++] = entry;
    cxl_hybrid_account_rdma_sidecar_registered(block->used_length);
    return 0;
}

static int cxl_rdma_sidecar_register_source_pin_all(
    CXLRDMASidecarContext *ctx,
    Error **errp)
{
    int ret;

    if (ctx->source_mrs) {
        return 0;
    }
    if (!ctx->ops.foreach_ramblock) {
        error_setg(errp,
                   "RDMA sidecar pin-all requires a RAMBlock iterator");
        return -1;
    }

    ret = ctx->ops.foreach_ramblock(cxl_rdma_sidecar_register_one_ramblock,
                                    ctx, ctx->ops.opaque);
    if (ret) {
        error_setg(errp, "RDMA sidecar failed to register source RAMBlocks");
        return -1;
    }
    return 0;
}

static uint32_t cxl_rdma_sidecar_source_lkey_for_claim(
    CXLRDMASidecarContext *ctx,
    const CXLHybridRDMABulkClaim *claim)
{
    uintptr_t src = (uintptr_t)claim->src;
    uintptr_t end = src + claim->bytes;

    if (ctx->pin_all) {
        for (uint32_t i = 0; i < ctx->nr_source_mrs; i++) {
            CXLRDMASidecarMR *entry = &ctx->source_mrs[i];
            uintptr_t mr_start = (uintptr_t)entry->host;
            uintptr_t mr_end = mr_start + entry->length;

            if (src >= mr_start && end <= mr_end) {
                return entry->mr->lkey;
            }
        }
        return 0;
    }

    assert(ctx->src_mr);
    assert(src >= (uintptr_t)ctx->src_mr->addr);
    assert(end <= (uintptr_t)ctx->src_mr->addr + ctx->src_mr->length);
    return ctx->src_mr->lkey;
}

static int G_GNUC_UNUSED cxl_rdma_sidecar_post_write(
    CXLRDMASidecarContext *ctx,
    const CXLHybridRDMABulkClaim *claim,
    Error **errp)
{
    struct ibv_sge sge;
    struct ibv_send_wr wr = { 0 };
    struct ibv_send_wr *bad_wr = NULL;
    uint64_t wrid;
    int ret;

    if (!ctx || !ctx->qp || !claim) {
        error_setg(errp, "RDMA sidecar is not ready to post writes");
        return -1;
    }
    if (claim->cxl_offset > ctx->remote_len ||
        claim->bytes > ctx->remote_len - claim->cxl_offset) {
        error_setg(errp, "RDMA sidecar claim exceeds destination MR");
        return -1;
    }
    if (!ctx->pin_all &&
        cxl_rdma_sidecar_register_source_region(ctx, claim, errp)) {
        return -1;
    }
    wrid = cxl_hybrid_rdma_claim_wrid(claim);

    sge = (struct ibv_sge) {
        .addr = (uintptr_t)claim->src,
        .length = claim->bytes,
        .lkey = cxl_rdma_sidecar_source_lkey_for_claim(ctx, claim),
    };
    if (!sge.lkey) {
        if (!ctx->pin_all) {
            ctx->src_mr_inflight = false;
        }
        error_setg(errp, "RDMA sidecar claim has no registered source MR");
        return -1;
    }
    wr = (struct ibv_send_wr) {
        .wr_id = wrid,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr = ctx->remote_base + claim->cxl_offset,
        .wr.rdma.rkey = ctx->remote_rkey,
    };

    ret = ibv_post_send(ctx->qp, &wr, &bad_wr);
    if (ret) {
        uint32_t inflight_len;

        if (!ctx->pin_all) {
            ctx->src_mr_inflight = false;
        }
        qemu_mutex_lock(&ctx->lock);
        inflight_len = ctx->inflight_len;
        qemu_mutex_unlock(&ctx->lock);
        if (cxl_rdma_sidecar_should_wait_for_sq_room_for_test(
                ret, inflight_len)) {
            return -EAGAIN;
        }
        error_setg_errno(errp, ret, "RDMA sidecar ibv_post_send failed");
        return -1;
    }
    if (claim->kind == CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN) {
        qatomic_inc(&ctx->stats.rdma_sidecar_postcopy_dirty_posted_spans);
        qatomic_add(&ctx->stats.rdma_sidecar_postcopy_dirty_posted_bytes,
                    claim->bytes);
        cxl_rdma_sidecar_bulk_stats_note_max_u64(
            &ctx->stats.rdma_sidecar_postcopy_dirty_max_span_bytes,
            claim->bytes);
    } else if (cxl_hybrid_rdma_claim_has_region_ownership(claim)) {
        cxl_hybrid_account_rdma_sidecar_posted(claim->claim_id,
                                               claim->region_index,
                                               claim->bytes);
    }
    trace_cxl_rdma_sidecar_post(wrid, claim->kind, claim->region_index,
                                (uintptr_t)claim->src,
                                wr.wr.rdma.remote_addr, claim->bytes);
    return 0;
}

static int G_GNUC_UNUSED cxl_rdma_sidecar_poll_completion(
    CXLRDMASidecarContext *ctx,
    uint64_t *claim_id,
    uint64_t *cqe_time_ns,
    Error **errp)
{
    struct ibv_wc wc;
    int ret;

    ret = ibv_poll_cq(ctx->cq, 1, &wc);
    if (ret < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar ibv_poll_cq failed");
        return -1;
    }
    if (ret == 0) {
        return 0;
    }
    if (wc.status != IBV_WC_SUCCESS) {
        if (!ctx->pin_all) {
            ctx->src_mr_inflight = false;
        }
        if (claim_id) {
            *claim_id = wc.wr_id;
        }
        error_setg(errp, "RDMA sidecar write failed: %s",
                   ibv_wc_status_str(wc.status));
        return -1;
    }

    if (!ctx->pin_all) {
        ctx->src_mr_inflight = false;
    }
    if (claim_id) {
        *claim_id = wc.wr_id;
    }
    if (cqe_time_ns) {
        *cqe_time_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }
    return 1;
}

static int cxl_rdma_sidecar_arm_cq_event(CXLRDMASidecarContext *ctx,
                                         Error **errp)
{
    if (!ctx || !ctx->comp_channel) {
        return 0;
    }

    if (ibv_req_notify_cq(ctx->cq, 0)) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar ibv_req_notify_cq failed");
        return -1;
    }
    return 1;
}

static int cxl_rdma_sidecar_wait_cq_event(CXLRDMASidecarContext *ctx,
                                          Error **errp)
{
    struct ibv_cq *cq = NULL;
    void *cq_context = NULL;
    struct pollfd pfd;
    int ret;

    if (!ctx || !ctx->comp_channel) {
        return 0;
    }

    pfd = (struct pollfd) {
        .fd = ctx->comp_channel->fd,
        .events = POLLIN,
    };
    ret = poll(&pfd, 1, 10);
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to poll CQ event channel");
        return -1;
    }
    if (ret == 0) {
        return 0;
    }

    if (ibv_get_cq_event(ctx->comp_channel, &cq, &cq_context)) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar ibv_get_cq_event failed");
        return -1;
    }
    ibv_ack_cq_events(cq, 1);
    return 1;
}

static uint32_t cxl_rdma_sidecar_snapshot_inflight_len(
    CXLRDMASidecarContext *ctx)
{
    uint32_t inflight_len;

    qemu_mutex_lock(&ctx->lock);
    inflight_len = ctx->inflight_len;
    qemu_mutex_unlock(&ctx->lock);
    return inflight_len;
}

static void cxl_rdma_sidecar_backoff(CXLRDMASidecarContext *ctx, int ms)
{
    qemu_mutex_lock(&ctx->lock);
    if (!ctx->stop) {
        qemu_cond_timedwait(&ctx->cond, &ctx->lock, ms);
    }
    qemu_mutex_unlock(&ctx->lock);
}

static uint32_t cxl_rdma_sidecar_inflight_capacity(
    const CXLRDMASidecarContext *ctx)
{
    return MAX((uint32_t)1, ctx->inflight_capacity);
}

static bool cxl_rdma_sidecar_dequeue_bulk_claim(CXLRDMASidecarContext *ctx,
                                                CXLHybridRDMABulkClaim *claim)
{
    bool running;
    bool postcopy;
    bool failed;
    bool bulk_active;

    qemu_mutex_lock(&ctx->lock);
    while (!ctx->stop && !ctx->queue_len) {
        if (ctx->inflight_len) {
            break;
        }
        qemu_cond_timedwait(&ctx->cond, &ctx->lock, 1);

        running = !ctx->ops.migration_running ||
                  ctx->ops.migration_running(ctx->ops.opaque);
        failed = ctx->ops.migration_failed &&
                 ctx->ops.migration_failed(ctx->ops.opaque);
        if (!running || failed) {
            break;
        }
    }

    while (!ctx->stop && ctx->queue_len) {
        CXLHybridRDMABulkClaim head = ctx->queue[ctx->queue_head];
        CXLHybridRDMASidecarAdmissionSnapshot precopy_snap;
        bool allowed;
        bool capacity;

        postcopy = ctx->ops.migration_postcopy &&
                   ctx->ops.migration_postcopy(ctx->ops.opaque);
        bulk_active = !ctx->ops.bulk_active ||
                      ctx->ops.bulk_active(ctx->ops.opaque);
        allowed = cxl_rdma_sidecar_claim_schedule_allowed(
            head.kind, postcopy, bulk_active, ctx->draining);
        capacity = ctx->inflight_len < cxl_rdma_sidecar_inflight_capacity(ctx);
        switch (head.kind) {
        case CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION:
            precopy_snap = cxl_rdma_sidecar_admission_snapshot(
                &ctx->admission,
                ctx->running && !ctx->stop && !ctx->incoming,
                bulk_active,
                ctx->draining,
                ctx->failed,
                postcopy,
                ctx->queue_len,
                ctx->inflight_len);
            capacity = capacity &&
                       ctx->inflight_len < precopy_snap.dynamic_window_regions;
            break;
        case CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN:
        {
            uint32_t dirty_inflight_wr = 0;
            uint64_t dirty_inflight_bytes = 0;

            cxl_rdma_sidecar_postcopy_dirty_count_claims(
                ctx->inflight_claims, ctx->inflight_len, &dirty_inflight_wr,
                &dirty_inflight_bytes);
            capacity =
                capacity &&
                dirty_inflight_wr <
                ctx->postcopy_dirty_admission.max_inflight_wr &&
                head.bytes <=
                ctx->postcopy_dirty_admission.max_inflight_bytes -
                MIN(dirty_inflight_bytes,
                    ctx->postcopy_dirty_admission.max_inflight_bytes);
            break;
        }
        default:
            allowed = false;
            break;
        }

        if (allowed && capacity) {
            *claim = head;
            ctx->queue_head = (ctx->queue_head + 1) % ctx->queue_capacity;
            ctx->queue_len--;
            cxl_rdma_sidecar_sub_queue_bytes(ctx, head.bytes);
            qemu_mutex_unlock(&ctx->lock);
            return true;
        }
        if (allowed) {
            break;
        }

        ctx->queue_head = (ctx->queue_head + 1) % ctx->queue_capacity;
        ctx->queue_len--;
        cxl_rdma_sidecar_sub_queue_bytes(ctx, head.bytes);
        qemu_mutex_unlock(&ctx->lock);
        cxl_rdma_sidecar_drop_claim(ctx, &head);
        qemu_mutex_lock(&ctx->lock);
    }

    qemu_mutex_unlock(&ctx->lock);
    return false;
}

static void cxl_rdma_sidecar_add_inflight_claim(
    CXLRDMASidecarContext *ctx,
    const CXLHybridRDMABulkClaim *claim)
{
    int kind_index = cxl_rdma_sidecar_claim_kind_index(claim->kind);

    qemu_mutex_lock(&ctx->lock);
    assert(ctx->inflight_len < ctx->inflight_capacity);
    if (!ctx->inflight_len) {
        ctx->rdma_active_start_ns = claim->post_time_ns;
        ctx->rdma_active_completed_bytes = 0;
    }
    if (kind_index >= 0) {
        if (!ctx->rdma_active_inflight_by_kind[kind_index]) {
            ctx->rdma_active_start_ns_by_kind[kind_index] =
                claim->post_time_ns;
            ctx->rdma_active_completed_bytes_by_kind[kind_index] = 0;
        }
        ctx->rdma_active_inflight_by_kind[kind_index]++;
    }
    ctx->inflight_claims[ctx->inflight_len++] = *claim;
    cxl_rdma_sidecar_add_inflight_bytes(ctx, claim->bytes);
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
}

static bool cxl_rdma_sidecar_finish_inflight_claim(
    CXLRDMASidecarContext *ctx,
    uint64_t claim_id,
    CXLHybridRDMABulkClaim *claim,
    bool *drained)
{
    bool found = false;

    if (drained) {
        *drained = false;
    }
    qemu_mutex_lock(&ctx->lock);
    for (uint32_t i = 0; i < ctx->inflight_len; i++) {
        if (ctx->inflight_claims[i].claim_id == claim_id) {
            uint64_t claim_bytes = ctx->inflight_claims[i].bytes;
            int kind_index = cxl_rdma_sidecar_claim_kind_index(
                ctx->inflight_claims[i].kind);

            if (claim) {
                *claim = ctx->inflight_claims[i];
            }
            if (kind_index >= 0) {
                assert(ctx->rdma_active_inflight_by_kind[kind_index] > 0);
                ctx->rdma_active_inflight_by_kind[kind_index]--;
            }
            ctx->inflight_claims[i] =
                ctx->inflight_claims[ctx->inflight_len - 1];
            ctx->inflight_len--;
            cxl_rdma_sidecar_sub_inflight_bytes(ctx, claim_bytes);
            if (drained) {
                *drained = ctx->inflight_len == 0;
            }
            found = true;
            break;
        }
    }
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
    return found;
}

static void cxl_rdma_sidecar_drop_inflight_claims(
    CXLRDMASidecarContext *ctx)
{
    CXLHybridRDMABulkClaim *claims;
    uint32_t len;

    if (!ctx || !ctx->inflight_capacity) {
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    len = ctx->inflight_len;
    claims = len ? g_new(CXLHybridRDMABulkClaim, len) : NULL;
    for (uint32_t i = 0; i < len; i++) {
        claims[i] = ctx->inflight_claims[i];
    }
    ctx->inflight_len = 0;
    ctx->inflight_bytes = 0;
    memset(ctx->rdma_active_inflight_by_kind, 0,
           sizeof(ctx->rdma_active_inflight_by_kind));
    memset(ctx->rdma_active_start_ns_by_kind, 0,
           sizeof(ctx->rdma_active_start_ns_by_kind));
    memset(ctx->rdma_active_completed_bytes_by_kind, 0,
           sizeof(ctx->rdma_active_completed_bytes_by_kind));
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);

    for (uint32_t i = 0; i < len; i++) {
        cxl_rdma_sidecar_drop_claim(ctx, &claims[i]);
    }
    g_free(claims);
}

bool cxl_rdma_sidecar_running(void)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool running;

    if (!ctx) {
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    running = ctx->running && !ctx->failed && !ctx->stop;
    qemu_mutex_unlock(&ctx->lock);
    return running;
}

static bool cxl_rdma_sidecar_runtime_bulk_active(CXLRDMASidecarContext *ctx)
{
    return !ctx->ops.bulk_active || ctx->ops.bulk_active(ctx->ops.opaque);
}

static void cxl_rdma_sidecar_postcopy_dirty_count_context_locked(
    CXLRDMASidecarContext *ctx,
    uint32_t *queue_wrp,
    uint32_t *inflight_wrp,
    uint64_t *queue_bytesp,
    uint64_t *inflight_bytesp)
{
    uint32_t queue_wr = 0;
    uint64_t queue_bytes = 0;

    if (ctx) {
        for (uint32_t i = 0; i < ctx->queue_len; i++) {
            CXLHybridRDMABulkClaim *queued =
                &ctx->queue[(ctx->queue_head + i) % ctx->queue_capacity];

            if (queued->kind != CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN) {
                continue;
            }
            queue_wr++;
            if (queue_bytes > UINT64_MAX - queued->bytes) {
                queue_bytes = UINT64_MAX;
            } else {
                queue_bytes += queued->bytes;
            }
        }

        cxl_rdma_sidecar_postcopy_dirty_count_claims(
            ctx->inflight_claims, ctx->inflight_len, inflight_wrp,
            inflight_bytesp);
    } else {
        if (inflight_wrp) {
            *inflight_wrp = 0;
        }
        if (inflight_bytesp) {
            *inflight_bytesp = 0;
        }
    }

    if (queue_wrp) {
        *queue_wrp = queue_wr;
    }
    if (queue_bytesp) {
        *queue_bytesp = queue_bytes;
    }
}

static bool cxl_rdma_sidecar_admission_reservation_matches(
    CXLRDMASidecarContext *ctx,
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    if (!ctx || !reservation || !reservation->valid) {
        cxl_rdma_sidecar_admission_reservation_clear(reservation);
        return false;
    }
    if (reservation->owner != ctx->admission_owner) {
        cxl_rdma_sidecar_admission_reservation_clear(reservation);
        return false;
    }
    return true;
}

static bool cxl_rdma_sidecar_postcopy_dirty_reservation_matches(
    CXLRDMASidecarContext *ctx,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation)
{
    if (!ctx || !reservation || !reservation->valid) {
        cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
        return false;
    }
    if (reservation->owner != ctx->postcopy_dirty_admission_owner) {
        cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
        return false;
    }
    return true;
}

bool cxl_rdma_sidecar_get_admission_snapshot(
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;

    if (!snapshot) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!ctx) {
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    *snapshot = cxl_rdma_sidecar_admission_snapshot(
        &ctx->admission,
        ctx->running && !ctx->stop && !ctx->incoming,
        cxl_rdma_sidecar_runtime_bulk_active(ctx),
        ctx->draining,
        ctx->failed,
        ctx->ops.migration_postcopy &&
            ctx->ops.migration_postcopy(ctx->ops.opaque),
        ctx->queue_len,
        ctx->inflight_len);
    qemu_mutex_unlock(&ctx->lock);
    return true;
}

bool cxl_rdma_sidecar_try_reserve_bulk_admission(
    CXLHybridRDMASidecarAdmissionReservation *reservation,
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool accepted;

    if (snapshot) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    if (!reservation) {
        return false;
    }
    cxl_rdma_sidecar_admission_reservation_clear(reservation);
    if (!ctx) {
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    accepted = cxl_rdma_sidecar_admission_try_reserve(
        &ctx->admission,
        ctx->running && !ctx->stop && !ctx->incoming,
        cxl_rdma_sidecar_runtime_bulk_active(ctx),
        ctx->draining,
        ctx->failed,
        ctx->ops.migration_postcopy &&
            ctx->ops.migration_postcopy(ctx->ops.opaque),
        ctx->queue_len,
        ctx->inflight_len,
        reservation,
        snapshot);
    if (accepted) {
        reservation->owner = ctx->admission_owner;
    } else {
        cxl_rdma_sidecar_admission_reservation_clear(reservation);
    }
    qemu_mutex_unlock(&ctx->lock);
    return accepted;
}

void cxl_rdma_sidecar_cancel_bulk_admission(
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;

    if (!reservation) {
        return;
    }
    if (!ctx || !reservation->valid) {
        cxl_rdma_sidecar_admission_reservation_clear(reservation);
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    if (!cxl_rdma_sidecar_admission_reservation_matches(ctx, reservation)) {
        qemu_mutex_unlock(&ctx->lock);
        return;
    }
    cxl_rdma_sidecar_admission_cancel_reserve(&ctx->admission, reservation);
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
}

bool cxl_rdma_sidecar_try_reserve_postcopy_dirty_admission(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation,
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot *snapshot)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool accepted;
    bool postcopy;
    bool bulk_active;
    uint32_t dirty_queue_wr = 0;
    uint32_t dirty_inflight_wr = 0;
    uint64_t dirty_queue_bytes = 0;
    uint64_t dirty_inflight_bytes = 0;

    if (snapshot) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    if (!reservation) {
        return false;
    }
    cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
    if (!claim ||
        claim->kind != CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN ||
        !claim->bytes ||
        !ctx) {
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    postcopy = ctx->ops.migration_postcopy &&
               ctx->ops.migration_postcopy(ctx->ops.opaque);
    bulk_active = cxl_rdma_sidecar_runtime_bulk_active(ctx);
    cxl_rdma_sidecar_postcopy_dirty_count_context_locked(
        ctx, &dirty_queue_wr, &dirty_inflight_wr, &dirty_queue_bytes,
        &dirty_inflight_bytes);
    accepted = cxl_rdma_sidecar_postcopy_dirty_admission_try_reserve(
        &ctx->postcopy_dirty_admission,
        ctx->running && !ctx->stop && !ctx->incoming,
        postcopy,
        bulk_active,
        ctx->draining,
        ctx->failed,
        claim->bytes,
        dirty_queue_wr,
        dirty_inflight_wr,
        dirty_queue_bytes,
        dirty_inflight_bytes,
        reservation,
        snapshot);
    if (accepted) {
        reservation->owner = ctx->postcopy_dirty_admission_owner;
    } else {
        cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
    }
    qemu_mutex_unlock(&ctx->lock);
    return accepted;
}

void cxl_rdma_sidecar_cancel_postcopy_dirty_admission(
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;

    if (!reservation) {
        return;
    }
    if (!ctx || !reservation->valid) {
        cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    if (!cxl_rdma_sidecar_postcopy_dirty_reservation_matches(ctx,
                                                             reservation)) {
        qemu_mutex_unlock(&ctx->lock);
        return;
    }
    cxl_rdma_sidecar_postcopy_dirty_admission_cancel_reserve(
        &ctx->postcopy_dirty_admission, reservation);
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
}

bool cxl_rdma_sidecar_enqueue_reserved_bulk_claim(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool bulk_active;
    bool postcopy;
    uint32_t tail;

    if (!reservation) {
        return false;
    }
    if (!ctx || !claim || !reservation->valid) {
        cxl_rdma_sidecar_admission_reservation_clear(reservation);
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    if (!cxl_rdma_sidecar_admission_reservation_matches(ctx, reservation)) {
        qemu_mutex_unlock(&ctx->lock);
        return false;
    }
    bulk_active = cxl_rdma_sidecar_runtime_bulk_active(ctx);
    postcopy = ctx->ops.migration_postcopy &&
               ctx->ops.migration_postcopy(ctx->ops.opaque);
    if (ctx->incoming || !ctx->running || ctx->failed || ctx->stop ||
        ctx->draining || postcopy || !bulk_active || !ctx->queue_capacity ||
        ctx->queue_len >= ctx->queue_capacity) {
        cxl_rdma_sidecar_admission_cancel_reserve(&ctx->admission,
                                                  reservation);
        qemu_mutex_unlock(&ctx->lock);
        return false;
    }

    cxl_rdma_sidecar_admission_consume_reserve(&ctx->admission, reservation);
    tail = (ctx->queue_head + ctx->queue_len) % ctx->queue_capacity;
    ctx->queue[tail] = *claim;
    ctx->queue_len++;
    cxl_rdma_sidecar_add_queue_bytes(ctx, claim->bytes);
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
    return true;
}

bool cxl_rdma_sidecar_enqueue_reserved_postcopy_dirty_claim(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool bulk_active;
    bool postcopy;
    uint32_t tail;

    if (!reservation) {
        return false;
    }
    if (!ctx || !claim ||
        claim->kind != CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN ||
        !claim->bytes ||
        !reservation->valid) {
        cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    if (!cxl_rdma_sidecar_postcopy_dirty_reservation_matches(ctx,
                                                             reservation)) {
        qemu_mutex_unlock(&ctx->lock);
        return false;
    }
    if (reservation->bytes != claim->bytes) {
        cxl_rdma_sidecar_postcopy_dirty_admission_cancel_reserve(
            &ctx->postcopy_dirty_admission, reservation);
        qemu_mutex_unlock(&ctx->lock);
        return false;
    }
    bulk_active = cxl_rdma_sidecar_runtime_bulk_active(ctx);
    postcopy = ctx->ops.migration_postcopy &&
               ctx->ops.migration_postcopy(ctx->ops.opaque);
    if (ctx->incoming || !ctx->running || ctx->failed || ctx->stop ||
        !cxl_rdma_sidecar_claim_schedule_allowed(claim->kind, postcopy,
                                                 bulk_active,
                                                 ctx->draining) ||
        !ctx->queue_capacity ||
        ctx->queue_len >= ctx->queue_capacity) {
        cxl_rdma_sidecar_postcopy_dirty_admission_cancel_reserve(
            &ctx->postcopy_dirty_admission, reservation);
        qemu_mutex_unlock(&ctx->lock);
        return false;
    }

    cxl_rdma_sidecar_postcopy_dirty_admission_consume_reserve(
        &ctx->postcopy_dirty_admission, reservation);
    tail = (ctx->queue_head + ctx->queue_len) % ctx->queue_capacity;
    ctx->queue[tail] = *claim;
    ctx->queue_len++;
    cxl_rdma_sidecar_add_queue_bytes(ctx, claim->bytes);
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
    return true;
}

bool cxl_rdma_sidecar_enqueue_bulk_claim(
    const CXLHybridRDMABulkClaim *claim)
{
    CXLHybridRDMASidecarAdmissionReservation reservation = { 0 };

    if (!claim) {
        return false;
    }
    if (!cxl_rdma_sidecar_try_reserve_bulk_admission(&reservation, NULL)) {
        return false;
    }
    return cxl_rdma_sidecar_enqueue_reserved_bulk_claim(claim, &reservation);
}

static int cxl_rdma_sidecar_poll_inflight_completion(
    CXLRDMASidecarContext *ctx,
    Error **errp);

static int cxl_rdma_sidecar_poll_or_wait_one_completion(
    CXLRDMASidecarContext *ctx,
    Error **errp)
{
    int ret;

    ret = cxl_rdma_sidecar_poll_inflight_completion(ctx, errp);
    if (ret != 0) {
        return ret;
    }

    while (true) {
        int wait_ret;

        ret = cxl_rdma_sidecar_arm_cq_event(ctx, errp);
        if (ret <= 0) {
            return ret;
        }

        ret = cxl_rdma_sidecar_poll_inflight_completion(ctx, errp);
        if (ret != 0) {
            return ret;
        }

        wait_ret = cxl_rdma_sidecar_wait_cq_event(ctx, errp);
        if (!cxl_rdma_sidecar_should_poll_after_cq_wait_for_test(wait_ret)) {
            return wait_ret;
        }

        ret = cxl_rdma_sidecar_poll_inflight_completion(ctx, errp);
        if (!cxl_rdma_sidecar_should_retry_after_stale_cq_event_for_test(
                wait_ret, ret)) {
            return ret;
        }
    }
}

static int cxl_rdma_sidecar_poll_inflight_completion(
    CXLRDMASidecarContext *ctx,
    Error **errp)
{
    CXLHybridRDMABulkClaim claim = { 0 };
    uint64_t claim_id = UINT64_MAX;
    uint32_t completed_pages = 0;
    uint32_t stale_pages = 0;
    uint64_t completed_time_ns = 0;
    uint64_t transport_completed_time_ns = 0;
    uint64_t completed_bytes = 0;
    uint64_t page_bytes;
    uint64_t cqe_time_ns = 0;
    uint64_t now_ns = 0;
    uint64_t active_time_ns = 0;
    uint64_t transport_active_time_ns = 0;
    uint64_t publish_time_ns = 0;
    uint64_t active_completed_bytes = 0;
    uint64_t kind_active_completed_bytes = 0;
    int kind_index;
    bool drained = false;
    int ret;

    ret = cxl_rdma_sidecar_poll_completion(ctx, &claim_id, &cqe_time_ns,
                                           errp);
    if (ret == 0) {
        return ret;
    }
    if (ret < 0 && claim_id == UINT64_MAX) {
        return ret;
    }

    if (!cxl_rdma_sidecar_finish_inflight_claim(ctx, claim_id, &claim,
                                                &drained)) {
        if (ret < 0) {
            return ret;
        }
        error_setg(errp,
                   "RDMA sidecar completion for unknown claim %" PRIu64,
                   claim_id);
        return -1;
    }
    if (ret < 0) {
        if (cxl_hybrid_rdma_claim_has_region_ownership(&claim)) {
            cxl_hybrid_account_rdma_sidecar_failed_no_trace(
                claim.region_index);
        }
        trace_cxl_rdma_sidecar_failed(claim.claim_id, claim.kind,
                                      claim.region_index);
        cxl_rdma_sidecar_drop_claim(ctx, &claim);
        return ret;
    }
    if (cxl_hybrid_rdma_claim_has_region_ownership(&claim)) {
        qatomic_inc(&ctx->stats.rdma_bulk_regions);
        qatomic_add(&ctx->stats.rdma_bulk_bytes, claim.bytes);
        cxl_hybrid_account_rdma_sidecar_completed(claim.claim_id,
                                                  claim.region_index,
                                                  claim.bytes);
    }
    if (claim.page_desc && ctx->ops.complete_bulk_claim) {
        ctx->ops.complete_bulk_claim(&claim, &completed_pages, &stale_pages,
                                     ctx->ops.opaque);
        now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (claim.post_time_ns) {
            completed_time_ns = now_ns - claim.post_time_ns;
            if (cqe_time_ns > claim.post_time_ns) {
                transport_completed_time_ns =
                    cqe_time_ns - claim.post_time_ns;
            }
        }
        if (cqe_time_ns && now_ns > cqe_time_ns) {
            publish_time_ns = now_ns - cqe_time_ns;
        }
        qatomic_add(&ctx->stats.page_state_rdma_completed_pages,
                    completed_pages);
        if (claim.kind == CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN) {
            completed_bytes = (uint64_t)completed_pages * TARGET_PAGE_SIZE;
            qatomic_inc(
                &ctx->stats.rdma_sidecar_postcopy_dirty_completed_spans);
            qatomic_add(
                &ctx->stats.rdma_sidecar_postcopy_dirty_completed_pages,
                completed_pages);
            qatomic_add(
                &ctx->stats.rdma_sidecar_postcopy_dirty_completed_bytes,
                completed_bytes);
            qatomic_add(
                &ctx->stats.rdma_sidecar_postcopy_dirty_stale_pages,
                stale_pages);
        } else {
            page_bytes = ctx->pages_per_region ?
                ctx->bytes_per_region / ctx->pages_per_region : 0;
            completed_bytes = (uint64_t)completed_pages * page_bytes;
        }
        cxl_rdma_sidecar_bulk_stats_note_completion_for_kind(
            &ctx->stats, claim.kind, completed_bytes, completed_time_ns,
            transport_completed_time_ns, publish_time_ns);
        qatomic_add(&ctx->stats.page_state_rdma_stale_pages, stale_pages);
        if (claim.kind == CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION &&
            completed_bytes && transport_completed_time_ns) {
            qemu_mutex_lock(&ctx->lock);
            cxl_rdma_sidecar_admission_note_completion(
                &ctx->admission, completed_bytes,
                transport_completed_time_ns);
            qemu_mutex_unlock(&ctx->lock);
        }
        kind_index = cxl_rdma_sidecar_claim_kind_index(claim.kind);
        if (kind_index >= 0) {
            qemu_mutex_lock(&ctx->lock);
            if (completed_bytes) {
                ctx->rdma_active_completed_bytes_by_kind[kind_index] +=
                    completed_bytes;
            }
            kind_active_completed_bytes =
                ctx->rdma_active_completed_bytes_by_kind[kind_index];
            if (!ctx->rdma_active_inflight_by_kind[kind_index] &&
                ctx->rdma_active_start_ns_by_kind[kind_index]) {
                if (kind_active_completed_bytes &&
                    now_ns > ctx->rdma_active_start_ns_by_kind[kind_index]) {
                    active_time_ns =
                        now_ns - ctx->rdma_active_start_ns_by_kind[kind_index];
                    transport_active_time_ns = 0;
                    if (cqe_time_ns >
                        ctx->rdma_active_start_ns_by_kind[kind_index]) {
                        transport_active_time_ns =
                            cqe_time_ns -
                            ctx->rdma_active_start_ns_by_kind[kind_index];
                    }
                    cxl_rdma_sidecar_bulk_stats_note_active_bucket_for_kind(
                        &ctx->stats, claim.kind, kind_active_completed_bytes,
                        active_time_ns);
                    cxl_rdma_sidecar_bulk_stats_note_transport_active_bucket_for_kind(
                        &ctx->stats, claim.kind, kind_active_completed_bytes,
                        transport_active_time_ns);
                }
                ctx->rdma_active_start_ns_by_kind[kind_index] = 0;
                ctx->rdma_active_completed_bytes_by_kind[kind_index] = 0;
            }
            qemu_mutex_unlock(&ctx->lock);
        }
        if (drained) {
            qemu_mutex_lock(&ctx->lock);
            ctx->rdma_active_completed_bytes += completed_bytes;
            active_completed_bytes = ctx->rdma_active_completed_bytes;
            if (active_completed_bytes &&
                ctx->rdma_active_start_ns &&
                now_ns > ctx->rdma_active_start_ns) {
                active_time_ns = now_ns - ctx->rdma_active_start_ns;
                if (cqe_time_ns > ctx->rdma_active_start_ns) {
                    transport_active_time_ns =
                        cqe_time_ns - ctx->rdma_active_start_ns;
                }
                cxl_rdma_sidecar_bulk_stats_note_active_epoch(
                    &ctx->stats,
                    active_completed_bytes,
                    active_time_ns);
                cxl_rdma_sidecar_bulk_stats_note_transport_active_epoch(
                    &ctx->stats,
                    active_completed_bytes,
                    transport_active_time_ns);
            }
            ctx->rdma_active_start_ns = 0;
            ctx->rdma_active_completed_bytes = 0;
            qemu_mutex_unlock(&ctx->lock);
        } else if (completed_bytes) {
            qemu_mutex_lock(&ctx->lock);
            ctx->rdma_active_completed_bytes += completed_bytes;
            qemu_mutex_unlock(&ctx->lock);
        }
    } else if (drained) {
        qemu_mutex_lock(&ctx->lock);
        ctx->rdma_active_start_ns = 0;
        ctx->rdma_active_completed_bytes = 0;
        qemu_mutex_unlock(&ctx->lock);
    }
    if (cxl_hybrid_rdma_claim_has_region_ownership(&claim)) {
        cxl_hybrid_region_drop_rdma(claim.region_index);
    }
    cxl_hybrid_rdma_bulk_claim_release(&claim);
    return 1;
}

static void cxl_rdma_sidecar_source_loop(CXLRDMASidecarContext *ctx)
{
    while (!cxl_rdma_sidecar_stopped(ctx)) {
        Error *local_err = NULL;
        bool running = !ctx->ops.migration_running ||
                       ctx->ops.migration_running(ctx->ops.opaque);
        bool failed = ctx->ops.migration_failed &&
                      ctx->ops.migration_failed(ctx->ops.opaque);
        bool made_progress = false;
        int ret;

        if (!running || failed) {
            return;
        }

        ret = cxl_rdma_sidecar_poll_inflight_completion(ctx, &local_err);
        if (ret < 0) {
            cxl_rdma_sidecar_drop_inflight_claims(ctx);
            cxl_rdma_sidecar_mark_failed(ctx, local_err);
            return;
        }
        made_progress = ret > 0;

        while (true) {
            CXLHybridRDMABulkClaim claim = { 0 };

            if (!cxl_rdma_sidecar_dequeue_bulk_claim(ctx, &claim)) {
                break;
            }

            if (!claim.claim_id) {
                claim.claim_id = cxl_rdma_sidecar_next_claim_id(ctx);
            }
            claim.post_time_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            ret = cxl_rdma_sidecar_post_write(ctx, &claim, &local_err);
            if (ret == -EAGAIN) {
                ret = cxl_rdma_sidecar_poll_or_wait_one_completion(ctx,
                                                                   &local_err);
                if (ret <= 0) {
                    if (!local_err) {
                        error_setg(&local_err,
                                   "RDMA sidecar ibv_post_send ENOMEM and no completion became available");
                    }
                    ret = -1;
                } else {
                    made_progress = true;
                    claim.post_time_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                    ret = cxl_rdma_sidecar_post_write(ctx, &claim, &local_err);
                    if (ret == -EAGAIN && !local_err) {
                        error_setg(&local_err,
                                   "RDMA sidecar ibv_post_send ENOMEM after retry");
                    }
                }
            }
            if (ret < 0) {
                cxl_rdma_sidecar_drop_claim(ctx, &claim);
                if (cxl_hybrid_rdma_claim_has_region_ownership(&claim)) {
                    cxl_hybrid_account_rdma_sidecar_failed_no_trace(
                        claim.region_index);
                }
                trace_cxl_rdma_sidecar_failed(claim.claim_id, claim.kind,
                                              claim.region_index);
                cxl_rdma_sidecar_mark_failed(ctx, local_err);
                return;
            }
            cxl_rdma_sidecar_add_inflight_claim(ctx, &claim);
            made_progress = true;
        }

        if (!made_progress) {
            if (cxl_rdma_sidecar_snapshot_inflight_len(ctx)) {
                ret = cxl_rdma_sidecar_poll_or_wait_one_completion(ctx,
                                                                   &local_err);
                if (ret < 0) {
                    cxl_rdma_sidecar_drop_inflight_claims(ctx);
                    cxl_rdma_sidecar_mark_failed(ctx, local_err);
                    return;
                }
                if (ret > 0 || ctx->comp_channel) {
                    continue;
                }
            }
            cxl_rdma_sidecar_backoff(ctx, 1);
        }
    }
}

static void cxl_rdma_sidecar_drop_queued_claims(CXLRDMASidecarContext *ctx)
{
    CXLHybridRDMABulkClaim *claims;
    uint32_t len;

    if (!ctx || !ctx->queue_capacity) {
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    len = ctx->queue_len;
    claims = len ? g_new(CXLHybridRDMABulkClaim, len) : NULL;
    for (uint32_t i = 0; i < len; i++) {
        claims[i] = ctx->queue[(ctx->queue_head + i) % ctx->queue_capacity];
    }
    ctx->queue_head = 0;
    ctx->queue_len = 0;
    ctx->queue_bytes = 0;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);

    for (uint32_t i = 0; i < len; i++) {
        cxl_rdma_sidecar_drop_claim(ctx, &claims[i]);
    }
    g_free(claims);
}

void cxl_rdma_sidecar_drain_bulk_claims(void)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool running;
    bool postcopy;

    if (!ctx) {
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    running = ctx->running && !ctx->failed && !ctx->stop;
    postcopy = ctx->ops.migration_postcopy &&
               ctx->ops.migration_postcopy(ctx->ops.opaque);
    ctx->draining = true;
    qemu_cond_broadcast(&ctx->cond);
    while (running && !postcopy && (ctx->queue_len || ctx->inflight_len)) {
        qemu_cond_timedwait(&ctx->cond, &ctx->lock, 1);
        running = ctx->running && !ctx->failed && !ctx->stop;
        postcopy = ctx->ops.migration_postcopy &&
                   ctx->ops.migration_postcopy(ctx->ops.opaque);
    }
    qemu_mutex_unlock(&ctx->lock);
    if (!running || postcopy) {
        cxl_rdma_sidecar_drop_inflight_claims(ctx);
        cxl_rdma_sidecar_drop_queued_claims(ctx);
    }
}

static void cxl_rdma_sidecar_cleanup(CXLRDMASidecarContext *ctx)
{
    if (!ctx) {
        return;
    }

    cxl_rdma_sidecar_drop_queued_claims(ctx);
    cxl_rdma_sidecar_drop_inflight_claims(ctx);
    g_free(ctx->queue);
    ctx->queue = NULL;
    ctx->queue_capacity = 0;
    g_free(ctx->inflight_claims);
    ctx->inflight_claims = NULL;
    ctx->inflight_capacity = 0;

    if (ctx->cm_id && ctx->connected) {
        rdma_disconnect(ctx->cm_id);
        ctx->connected = false;
    }

    if (ctx->src_mr) {
        ibv_dereg_mr(ctx->src_mr);
        ctx->src_mr = NULL;
        ctx->src_mr_inflight = false;
    }
    for (uint32_t i = 0; i < ctx->nr_source_mrs; i++) {
        if (ctx->source_mrs[i].mr) {
            ibv_dereg_mr(ctx->source_mrs[i].mr);
            ctx->source_mrs[i].mr = NULL;
        }
    }
    g_free(ctx->source_mrs);
    ctx->source_mrs = NULL;
    ctx->nr_source_mrs = 0;
    if (ctx->hello_recv_mr) {
        ibv_dereg_mr(ctx->hello_recv_mr);
        ctx->hello_recv_mr = NULL;
    }
    if (ctx->hello_send_mr) {
        ibv_dereg_mr(ctx->hello_send_mr);
        ctx->hello_send_mr = NULL;
    }
    if (ctx->dst_mr) {
        ibv_dereg_mr(ctx->dst_mr);
        ctx->dst_mr = NULL;
    }
    if (ctx->qp && ctx->cm_id) {
        rdma_destroy_qp(ctx->cm_id);
        ctx->qp = NULL;
    }
    if (ctx->cq) {
        ibv_destroy_cq(ctx->cq);
        ctx->cq = NULL;
    }
    if (ctx->comp_channel) {
        ibv_destroy_comp_channel(ctx->comp_channel);
        ctx->comp_channel = NULL;
    }
    if (ctx->pd) {
        ibv_dealloc_pd(ctx->pd);
        ctx->pd = NULL;
    }
    if (ctx->cm_id) {
        rdma_destroy_id(ctx->cm_id);
        ctx->cm_id = NULL;
    }
    if (ctx->listen_id) {
        rdma_destroy_id(ctx->listen_id);
        ctx->listen_id = NULL;
    }
    if (ctx->channel) {
        rdma_destroy_event_channel(ctx->channel);
        ctx->channel = NULL;
    }
}

static void *cxl_rdma_sidecar_thread(void *opaque)
{
    CXLRDMASidecarContext *ctx = opaque;
    Error *local_err = NULL;
    uint64_t start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (ctx->incoming) {
        if (cxl_rdma_sidecar_listen_dest(ctx, &local_err)) {
            goto fail;
        }

        qemu_mutex_lock(&ctx->lock);
        ctx->setup_done = true;
        qemu_cond_broadcast(&ctx->cond);
        qemu_mutex_unlock(&ctx->lock);

        if (cxl_rdma_sidecar_accept_id(ctx, &local_err) ||
            cxl_rdma_sidecar_alloc_pd_cq_qp(ctx, &local_err) ||
            cxl_rdma_sidecar_register_destination(ctx, &local_err) ||
            cxl_rdma_sidecar_connect_qp(ctx, &local_err) ||
            cxl_rdma_sidecar_exchange_hello(ctx, &local_err)) {
            goto fail;
        }
    } else {
        if (cxl_rdma_sidecar_resolve_source(ctx, &local_err) ||
            cxl_rdma_sidecar_alloc_pd_cq_qp(ctx, &local_err) ||
            cxl_rdma_sidecar_post_hello_recv(ctx, &local_err) ||
            cxl_rdma_sidecar_connect_qp(ctx, &local_err) ||
            cxl_rdma_sidecar_exchange_hello(ctx, &local_err)) {
            goto fail;
        }
        if (ctx->pin_all &&
            cxl_rdma_sidecar_register_source_pin_all(ctx, &local_err)) {
            goto fail;
        }
    }

    qemu_mutex_lock(&ctx->lock);
    ctx->running = true;
    ctx->setup_done = true;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
    cxl_hybrid_account_rdma_sidecar_connect(
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - start_ns);

    if (!ctx->incoming) {
        cxl_rdma_sidecar_source_loop(ctx);
    } else {
        qemu_mutex_lock(&ctx->lock);
        while (!ctx->stop) {
            qemu_cond_timedwait(&ctx->cond, &ctx->lock, 100);
        }
        qemu_mutex_unlock(&ctx->lock);
    }
    return NULL;

fail:
    cxl_rdma_sidecar_mark_failed(ctx, local_err);
    cxl_rdma_sidecar_cleanup(ctx);
    return NULL;
}

static int cxl_rdma_sidecar_start_internal(
    const CXLHybridRDMASidecarConfig *cfg,
    bool wait_for_setup,
    Error **errp)
{
    CXLRDMASidecarContext *ctx;

    if (!cfg || !cfg->addr || !cfg->total_regions || !cfg->bytes_per_region ||
        !cfg->pages_per_region) {
        error_setg(errp, "RDMA sidecar configuration is incomplete");
        return -1;
    }
    if (!cxl_rdma_sidecar_geometry_shift(cfg->bytes_per_region,
                                         &(uint32_t) { 0 })) {
        error_setg(errp, "RDMA sidecar region size must be a power of two");
        return -1;
    }
    if (cxl_rdma_sidecar_running()) {
        return 0;
    }

    cxl_rdma_sidecar_stop();

    ctx = g_new0(CXLRDMASidecarContext, 1);
    qemu_mutex_init(&ctx->lock);
    qemu_cond_init(&ctx->cond);
    ctx->incoming = cfg->incoming;
    ctx->total_regions = cfg->total_regions;
    ctx->bytes_per_region = cfg->bytes_per_region;
    ctx->pages_per_region = cfg->pages_per_region;
    ctx->max_inflight_regions = cfg->max_inflight_regions;
    ctx->postcopy_dirty_min_span_bytes = cfg->postcopy_dirty_min_span_bytes ?
        cfg->postcopy_dirty_min_span_bytes :
        CXL_RDMA_POSTCOPY_DIRTY_MIN_SPAN_BYTES_DEFAULT;
    ctx->pin_all = cfg->pin_all;
    ctx->inflight_capacity = cxl_rdma_sidecar_effective_inflight_capacity(
        cfg->total_regions, CXL_RDMA_CQ_DEPTH, cfg->pin_all);
    ctx->queue_capacity = ctx->inflight_capacity;
    ctx->queue = g_new0(CXLHybridRDMABulkClaim, ctx->queue_capacity);
    ctx->inflight_claims =
        g_new0(CXLHybridRDMABulkClaim, ctx->inflight_capacity);
    cxl_rdma_sidecar_apply_transport_capacity(ctx, ctx->inflight_capacity);
    ctx->admission_owner = cxl_rdma_sidecar_next_admission_owner();
    ctx->postcopy_dirty_admission_owner =
        cxl_rdma_sidecar_next_admission_owner();
    if (cfg->ops) {
        ctx->ops = *cfg->ops;
    }

    if (cxl_rdma_sidecar_parse_addr(ctx, cfg->addr, errp)) {
        g_free(ctx->queue);
        g_free(ctx->inflight_claims);
        qemu_cond_destroy(&ctx->cond);
        qemu_mutex_destroy(&ctx->lock);
        g_free(ctx);
        return -1;
    }

    trace_cxl_rdma_sidecar_connect_start(ctx->host, ctx->port);
    cxl_rdma_sidecar = ctx;
    qemu_thread_create(&ctx->thread, "cxl-rdma-sidecar",
                       cxl_rdma_sidecar_thread, ctx, QEMU_THREAD_JOINABLE);
    ctx->thread_created = true;
    if (!wait_for_setup) {
        return 0;
    }

    qemu_mutex_lock(&ctx->lock);
    while (!ctx->setup_done) {
        qemu_cond_wait(&ctx->cond, &ctx->lock);
    }
    if (ctx->failed) {
        qemu_mutex_unlock(&ctx->lock);
        cxl_rdma_sidecar_stop();
        error_setg(errp, "RDMA sidecar transport setup failed");
        return -1;
    }
    qemu_mutex_unlock(&ctx->lock);
    return 0;
}

int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp)
{
    return cxl_rdma_sidecar_start_internal(cfg, true, errp);
}

bool cxl_rdma_sidecar_start_async(const CXLHybridRDMASidecarConfig *cfg,
                                  Error **errp)
{
    return cxl_rdma_sidecar_start_internal(cfg, false, errp) == 0;
}

void cxl_rdma_sidecar_stop(void)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;

    if (!ctx) {
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    ctx->stop = true;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);

    if (ctx->thread_created) {
        qemu_thread_join(&ctx->thread);
        ctx->thread_created = false;
    }

    cxl_rdma_sidecar_cleanup(ctx);
    qemu_cond_destroy(&ctx->cond);
    qemu_mutex_destroy(&ctx->lock);
    g_free(ctx->host);
    g_free(ctx->port);
    g_free(ctx);
    cxl_rdma_sidecar = NULL;
}

void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    CXLHybridRDMASidecarBulkStats *ctx_stats;
    CXLHybridRDMASidecarAdmissionSnapshot snap;
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot dirty_snap;
    uint64_t active_time_ns;
    uint64_t transport_active_time_ns;
    uint64_t active_start_ns;
    uint64_t precopy_active_time_ns;
    uint64_t precopy_transport_active_time_ns;
    uint64_t postcopy_dirty_active_time_ns;
    uint64_t postcopy_dirty_transport_active_time_ns;
    uint64_t now_ns;
    uint32_t dirty_queue_wr = 0;
    uint32_t dirty_inflight_wr = 0;
    uint64_t dirty_queue_bytes = 0;
    uint64_t dirty_inflight_bytes = 0;
    bool bulk_active;
    bool postcopy;

    if (!stats) {
        return;
    }
    if (!ctx) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    ctx_stats = &ctx->stats;

    stats->rdma_bulk_regions =
        qatomic_read(&ctx_stats->rdma_bulk_regions);
    stats->rdma_bulk_bytes =
        qatomic_read(&ctx_stats->rdma_bulk_bytes);
    stats->page_state_rdma_completed_pages =
        qatomic_read(&ctx_stats->page_state_rdma_completed_pages);
    stats->page_state_rdma_completed_bytes =
        qatomic_read(&ctx_stats->page_state_rdma_completed_bytes);
    stats->page_state_rdma_completed_time_ns =
        qatomic_read(&ctx_stats->page_state_rdma_completed_time_ns);
    active_time_ns = qatomic_read(&ctx_stats->page_state_rdma_active_time_ns);
    stats->page_state_rdma_transport_completed_time_ns =
        qatomic_read(
            &ctx_stats->page_state_rdma_transport_completed_time_ns);
    transport_active_time_ns =
        qatomic_read(&ctx_stats->page_state_rdma_transport_active_time_ns);
    stats->page_state_rdma_publish_time_ns =
        qatomic_read(&ctx_stats->page_state_rdma_publish_time_ns);
    stats->page_state_rdma_stale_pages =
        qatomic_read(&ctx_stats->page_state_rdma_stale_pages);
    stats->page_state_rdma_precopy_completed_bytes =
        qatomic_read(&ctx_stats->page_state_rdma_precopy_completed_bytes);
    stats->page_state_rdma_precopy_completed_time_ns =
        qatomic_read(&ctx_stats->page_state_rdma_precopy_completed_time_ns);
    precopy_active_time_ns =
        qatomic_read(&ctx_stats->page_state_rdma_precopy_active_time_ns);
    stats->page_state_rdma_precopy_transport_completed_time_ns =
        qatomic_read(
            &ctx_stats->page_state_rdma_precopy_transport_completed_time_ns);
    precopy_transport_active_time_ns =
        qatomic_read(
            &ctx_stats->page_state_rdma_precopy_transport_active_time_ns);
    stats->page_state_rdma_precopy_publish_time_ns =
        qatomic_read(&ctx_stats->page_state_rdma_precopy_publish_time_ns);
    stats->page_state_rdma_postcopy_dirty_completed_bytes =
        qatomic_read(
            &ctx_stats->page_state_rdma_postcopy_dirty_completed_bytes);
    stats->page_state_rdma_postcopy_dirty_completed_time_ns =
        qatomic_read(
            &ctx_stats->page_state_rdma_postcopy_dirty_completed_time_ns);
    postcopy_dirty_active_time_ns =
        qatomic_read(
            &ctx_stats->page_state_rdma_postcopy_dirty_active_time_ns);
    stats->page_state_rdma_postcopy_dirty_transport_completed_time_ns =
        qatomic_read(
            &ctx_stats->
                page_state_rdma_postcopy_dirty_transport_completed_time_ns);
    postcopy_dirty_transport_active_time_ns =
        qatomic_read(
            &ctx_stats->
                page_state_rdma_postcopy_dirty_transport_active_time_ns);
    stats->page_state_rdma_postcopy_dirty_publish_time_ns =
        qatomic_read(
            &ctx_stats->page_state_rdma_postcopy_dirty_publish_time_ns);
    stats->rdma_sidecar_postcopy_dirty_posted_spans =
        qatomic_read(
            &ctx_stats->rdma_sidecar_postcopy_dirty_posted_spans);
    stats->rdma_sidecar_postcopy_dirty_posted_bytes =
        qatomic_read(
            &ctx_stats->rdma_sidecar_postcopy_dirty_posted_bytes);
    stats->rdma_sidecar_postcopy_dirty_completed_spans =
        qatomic_read(
            &ctx_stats->rdma_sidecar_postcopy_dirty_completed_spans);
    stats->rdma_sidecar_postcopy_dirty_completed_bytes =
        qatomic_read(
            &ctx_stats->rdma_sidecar_postcopy_dirty_completed_bytes);
    stats->rdma_sidecar_postcopy_dirty_completed_pages =
        qatomic_read(
            &ctx_stats->rdma_sidecar_postcopy_dirty_completed_pages);
    stats->rdma_sidecar_postcopy_dirty_stale_pages =
        qatomic_read(
            &ctx_stats->rdma_sidecar_postcopy_dirty_stale_pages);
    stats->rdma_sidecar_postcopy_dirty_max_span_bytes =
        qatomic_read(
            &ctx_stats->rdma_sidecar_postcopy_dirty_max_span_bytes);

    qemu_mutex_lock(&ctx->lock);
    cxl_rdma_sidecar_postcopy_dirty_count_context_locked(
        ctx, &dirty_queue_wr, &dirty_inflight_wr, &dirty_queue_bytes,
        &dirty_inflight_bytes);
    bulk_active = !ctx->ops.bulk_active ||
                  ctx->ops.bulk_active(ctx->ops.opaque);
    postcopy = ctx->ops.migration_postcopy &&
               ctx->ops.migration_postcopy(ctx->ops.opaque);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &ctx->admission,
        ctx->running && !ctx->stop && !ctx->incoming,
        bulk_active,
        ctx->draining,
        ctx->failed,
        postcopy,
        ctx->queue_len,
        ctx->inflight_len);
    dirty_snap = cxl_rdma_sidecar_postcopy_dirty_admission_snapshot(
        &ctx->postcopy_dirty_admission,
        ctx->running && !ctx->stop && !ctx->incoming,
        postcopy,
        bulk_active,
        ctx->draining,
        ctx->failed,
        0,
        dirty_queue_wr,
        dirty_inflight_wr,
        dirty_queue_bytes,
        dirty_inflight_bytes);
    active_start_ns = ctx->rdma_active_start_ns;
    if (ctx->inflight_len) {
        now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (active_start_ns && now_ns > active_start_ns) {
            active_time_ns += now_ns - active_start_ns;
            transport_active_time_ns += now_ns - active_start_ns;
        }
        active_start_ns = ctx->rdma_active_start_ns_by_kind[
            CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION];
        if (ctx->rdma_active_inflight_by_kind[
                CXL_HYBRID_RDMA_CLAIM_PRECOPY_REGION] &&
            active_start_ns && now_ns > active_start_ns) {
            precopy_active_time_ns += now_ns - active_start_ns;
            precopy_transport_active_time_ns += now_ns - active_start_ns;
        }
        active_start_ns = ctx->rdma_active_start_ns_by_kind[
            CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN];
        if (ctx->rdma_active_inflight_by_kind[
                CXL_HYBRID_RDMA_CLAIM_POSTCOPY_DIRTY_SPAN] &&
            active_start_ns && now_ns > active_start_ns) {
            postcopy_dirty_active_time_ns += now_ns - active_start_ns;
            postcopy_dirty_transport_active_time_ns +=
                now_ns - active_start_ns;
        }
    }
    qemu_mutex_unlock(&ctx->lock);

    stats->page_state_rdma_active_time_ns = active_time_ns;
    stats->page_state_rdma_transport_active_time_ns =
        transport_active_time_ns;
    stats->page_state_rdma_precopy_active_time_ns =
        precopy_active_time_ns;
    stats->page_state_rdma_precopy_transport_active_time_ns =
        precopy_transport_active_time_ns;
    stats->page_state_rdma_postcopy_dirty_active_time_ns =
        postcopy_dirty_active_time_ns;
    stats->page_state_rdma_postcopy_dirty_transport_active_time_ns =
        postcopy_dirty_transport_active_time_ns;
    stats->rdma_sidecar_dynamic_window_regions =
        snap.dynamic_window_regions;
    stats->rdma_sidecar_sq_capacity_regions = snap.sq_capacity_regions;
    stats->rdma_sidecar_queue_len = snap.queue_len;
    stats->rdma_sidecar_inflight_len = snap.inflight_len;
    stats->rdma_sidecar_goodput_ewma_bytes_per_ns =
        snap.goodput_ewma_bytes_per_ns;
    stats->rdma_sidecar_completion_latency_ewma_ns =
        snap.completion_latency_ewma_ns;
    stats->rdma_sidecar_bdp_estimate_regions = snap.bdp_estimate_regions;
    stats->rdma_sidecar_admission_accepted_regions = snap.accepted_regions;
    stats->rdma_sidecar_admission_overflow_cxl_regions =
        snap.overflow_cxl_regions;
    stats->rdma_sidecar_admission_closed_events =
        snap.admission_closed_events;
    stats->rdma_sidecar_admission_goodput_drop_events =
        snap.goodput_drop_events;
    stats->rdma_sidecar_postcopy_dirty_overflow_cxl_spans =
        dirty_snap.overflow_cxl_spans;
    stats->rdma_sidecar_postcopy_dirty_min_span_cxl_spans =
        dirty_snap.min_span_cxl_spans;
    stats->rdma_sidecar_postcopy_dirty_max_inflight_wr =
        dirty_snap.max_inflight_wr;
    stats->rdma_sidecar_postcopy_dirty_queue_wr = dirty_snap.queue_wr;
    stats->rdma_sidecar_postcopy_dirty_inflight_wr = dirty_snap.inflight_wr;
    stats->rdma_sidecar_postcopy_dirty_max_inflight_bytes =
        dirty_snap.max_inflight_bytes;
    stats->rdma_sidecar_postcopy_dirty_queue_bytes = dirty_snap.queue_bytes;
    stats->rdma_sidecar_postcopy_dirty_inflight_bytes =
        dirty_snap.inflight_bytes;
    stats->rdma_sidecar_postcopy_dirty_min_span_bytes =
        dirty_snap.min_span_bytes;
}
#else
int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp)
{
    error_setg(errp,
               "x-cxl-rdma-sidecar requires QEMU to be built with RDMA support");
    return -1;
}

bool cxl_rdma_sidecar_start_async(const CXLHybridRDMASidecarConfig *cfg,
                                  Error **errp)
{
    return cxl_rdma_sidecar_start(cfg, errp) == 0;
}

void cxl_rdma_sidecar_stop(void)
{
}

bool cxl_rdma_sidecar_running(void)
{
    return false;
}

bool cxl_rdma_sidecar_try_reserve_bulk_admission(
    CXLHybridRDMASidecarAdmissionReservation *reservation,
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot)
{
    cxl_rdma_sidecar_admission_reservation_clear(reservation);
    if (snapshot) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return false;
}

bool cxl_rdma_sidecar_enqueue_reserved_bulk_claim(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    cxl_rdma_sidecar_admission_reservation_clear(reservation);
    return false;
}

void cxl_rdma_sidecar_cancel_bulk_admission(
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    cxl_rdma_sidecar_admission_reservation_clear(reservation);
}

bool cxl_rdma_sidecar_get_admission_snapshot(
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot)
{
    if (snapshot) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return false;
}

bool cxl_rdma_sidecar_try_reserve_postcopy_dirty_admission(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation,
    CXLHybridRDMAPostcopyDirtyAdmissionSnapshot *snapshot)
{
    cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
    if (snapshot) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return false;
}

bool cxl_rdma_sidecar_enqueue_reserved_postcopy_dirty_claim(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation)
{
    cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
    return false;
}

void cxl_rdma_sidecar_cancel_postcopy_dirty_admission(
    CXLHybridRDMAPostcopyDirtyAdmissionReservation *reservation)
{
    cxl_rdma_sidecar_postcopy_dirty_reservation_clear(reservation);
}

bool cxl_rdma_sidecar_enqueue_bulk_claim(
    const CXLHybridRDMABulkClaim *claim)
{
    return false;
}

void cxl_rdma_sidecar_drain_bulk_claims(void)
{
}

void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats)
{
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
}
#endif
