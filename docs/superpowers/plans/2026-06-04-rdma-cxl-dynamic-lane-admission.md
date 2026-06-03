# RDMA CXL Dynamic Lane Admission Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace fixed RDMA/CXL bulk lane splitting with dynamic RDMA admission based on sidecar window, SQ capacity, BDP estimate, and recent goodput, with overflow going directly to the CXL worker lane.

**Architecture:** RDMA owns a small O(1) admission controller with reservation semantics. The RAM bulk scheduler tries to reserve RDMA before claiming page-state; if admission is closed, it falls through to CXL_LOW without touching RDMA claims. RDMA completions update EWMA goodput/latency and grow or shrink the dynamic window within the SQ safety cap.

**Tech Stack:** QEMU migration C, CXL page-state control, RDMA verbs sidecar, qatomic/QemuMutex, QAPI migration stats, GLib unit tests, Python experiment parser tests, Meson/Ninja.

---

## Current Baseline

Work in:

```bash
cd /home/xiexinchen/.config/superpowers/worktrees/qemu/rdma-cxl-parallel-hybrid
```

Design spec:

```text
docs/superpowers/specs/2026-06-04-rdma-cxl-dynamic-lane-admission-design.md
```

Important current behavior to change:

- `migration/ram.c:ram_save_host_page()` calls CXL enqueue first, then RDMA enqueue.
- `migration/ram.c:cxl_hybrid_cxl_enqueue_bulk_page()` uses a hard-coded 1:1 `CXLHybridSchedulerPolicy`.
- `migration/ram.c:cxl_hybrid_rdma_enqueue_bulk_region()` uses the same fixed policy before RDMA page-state claim.
- `migration/cxl-rdma.c` sets `queue_capacity` and `inflight_capacity` from `max_inflight_regions`; this value should become a safety cap, not a fixed desired depth.
- `x-cxl-rdma-sidecar-max-cover-percent` must stop controlling active lane split.

Before editing, note that this worktree may contain unrelated dirty cleanup/remap changes. Do not stage or revert those changes unless the user explicitly asks.

## File Structure

Modify these files:

- `migration/cxl-rdma.h`: admission state/snapshot/reservation types, public sidecar admission APIs, dynamic stats fields.
- `migration/cxl-rdma.c`: pure admission controller helpers, runtime reservation wiring, completion-driven window updates, non-RDMA stubs.
- `migration/ram.c`: dynamic RDMA-first scheduler flow and CXL overflow fallback.
- `migration/cxl.c`: `query-migrate` population for dynamic admission metrics.
- `qapi/migration.json`: `X-CXLInfo` metric documentation and schema fields.
- `scripts/cxl-hybrid-warm-experiment.py`: parser metric list and summary fields.
- `scripts/cxl-hybrid-warm-experiment-test.py`: QAPI/parser/static scheduler tests.
- `tests/unit/test-cxl-hybrid-region.c`: C unit tests for pure admission controller behavior.
- `docs/superpowers/reports/2026-06-04-rdma-cxl-current-experiment-config.md`: update experiment guidance after code and parser support land.

Do not create a new scheduler thread. Do not move page-state ownership out of the existing page-state helpers.

## Task 1: Add Pure RDMA Admission Controller

**Files:**
- Modify: `migration/cxl-rdma.h`
- Modify: `migration/cxl-rdma.c`
- Test: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write failing admission controller tests**

Add these tests near the existing RDMA sidecar tests in `tests/unit/test-cxl-hybrid-region.c`:

```c
static void test_rdma_admission_rejects_full_dynamic_window(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;
    CXLHybridRDMASidecarAdmissionReservation reservation = { 0 };

    cxl_rdma_sidecar_admission_state_init(&state, 8, 2 * MiB);

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 1);
    g_assert_false(snap.accept_rdma);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 1);
    g_assert_cmpuint(snap.sq_capacity_regions, ==, 8);
    g_assert_cmpuint(snap.inflight_len, ==, 1);

    g_assert_false(cxl_rdma_sidecar_admission_try_reserve(
        &state, true, true, false, false, false, 0, 1, &reservation, &snap));
    g_assert_false(reservation.valid);
    g_assert_cmpuint(state.overflow_cxl_regions, ==, 1);
}

static void test_rdma_admission_reserve_and_cancel_updates_outstanding(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;
    CXLHybridRDMASidecarAdmissionReservation reservation = { 0 };

    cxl_rdma_sidecar_admission_state_init(&state, 4, 2 * MiB);

    g_assert_true(cxl_rdma_sidecar_admission_try_reserve(
        &state, true, true, false, false, false, 0, 0, &reservation, &snap));
    g_assert_true(reservation.valid);
    g_assert_cmpuint(state.reserved_regions, ==, 1);
    g_assert_cmpuint(state.accepted_regions, ==, 1);

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);
    g_assert_false(snap.accept_rdma);
    g_assert_cmpuint(snap.outstanding_regions, ==, 1);

    cxl_rdma_sidecar_admission_cancel_reserve(&state, &reservation);
    g_assert_false(reservation.valid);
    g_assert_cmpuint(state.reserved_regions, ==, 0);

    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);
    g_assert_true(snap.accept_rdma);
}

static void test_rdma_admission_completion_grows_window_and_bdp(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 8, 2 * MiB);

    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);

    g_assert_cmpuint(snap.dynamic_window_regions, ==, 2);
    g_assert_cmpuint(snap.bdp_estimate_regions, >=, 1);
    g_assert_cmpfloat(snap.goodput_ewma_bytes_per_ns, >, 0.0);
    g_assert_cmpuint(snap.completion_latency_ewma_ns, ==, 1000000);
}

static void test_rdma_admission_goodput_regression_halves_window(void)
{
    CXLHybridRDMASidecarAdmissionState state;
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    cxl_rdma_sidecar_admission_state_init(&state, 8, 2 * MiB);

    cxl_rdma_sidecar_admission_note_completion(&state, 2 * MiB, 1000000);
    cxl_rdma_sidecar_admission_note_completion(&state, 4 * MiB, 1000000);
    cxl_rdma_sidecar_admission_note_completion(&state, 4 * MiB, 1000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);
    g_assert_cmpuint(snap.dynamic_window_regions, ==, 4);

    cxl_rdma_sidecar_admission_note_completion(&state, 1 * MiB, 4000000);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &state, true, true, false, false, false, 0, 0);

    g_assert_cmpuint(snap.dynamic_window_regions, ==, 2);
    g_assert_cmpuint(state.goodput_drop_events, ==, 1);
}
```

Register the tests in `main()`:

```c
g_test_add_func("/cxl/region/rdma-admission-rejects-full-window",
                test_rdma_admission_rejects_full_dynamic_window);
g_test_add_func("/cxl/region/rdma-admission-reserve-cancel",
                test_rdma_admission_reserve_and_cancel_updates_outstanding);
g_test_add_func("/cxl/region/rdma-admission-completion-grows-window",
                test_rdma_admission_completion_grows_window_and_bdp);
g_test_add_func("/cxl/region/rdma-admission-goodput-regression-halves-window",
                test_rdma_admission_goodput_regression_halves_window);
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
```

Expected: compile failure because `CXLHybridRDMASidecarAdmissionState`,
`CXLHybridRDMASidecarAdmissionSnapshot`, `CXLHybridRDMASidecarAdmissionReservation`,
and the `cxl_rdma_sidecar_admission_*` helpers do not exist.

- [ ] **Step 3: Add admission types and prototypes**

Add to `migration/cxl-rdma.h` after `CXLHybridRDMASidecarBulkStats`:

```c
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
} CXLHybridRDMASidecarAdmissionReservation;
```

Add these prototypes:

```c
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
```

- [ ] **Step 4: Implement the pure controller**

Add this code near the top of `migration/cxl-rdma.c`, before `#ifdef CONFIG_RDMA`:

```c
#define CXL_RDMA_ADMISSION_MIN_WINDOW 1U
#define CXL_RDMA_ADMISSION_EWMA_WEIGHT 8.0

static uint32_t cxl_rdma_admission_clamp_window(uint32_t value,
                                                uint32_t cap)
{
    if (!cap) {
        return 0;
    }
    return MIN(MAX(value, CXL_RDMA_ADMISSION_MIN_WINDOW), cap);
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
    state->dynamic_window_regions = CXL_RDMA_ADMISSION_MIN_WINDOW;
    state->bytes_per_region = bytes_per_region;
}

static uint32_t cxl_rdma_sidecar_admission_bdp_regions(
    const CXLHybridRDMASidecarAdmissionState *state)
{
    double bytes_in_flight;
    uint64_t estimated_bytes;
    uint64_t bytes_per_region;

    if (!state || state->goodput_ewma_bytes_per_ns <= 0.0 ||
        !state->completion_latency_ewma_ns || !state->bytes_per_region) {
        return 1;
    }

    bytes_per_region = state->bytes_per_region;
    bytes_in_flight = state->goodput_ewma_bytes_per_ns *
                      (double)state->completion_latency_ewma_ns;
    estimated_bytes = bytes_in_flight >= (double)UINT64_MAX ?
                      UINT64_MAX : (uint64_t)bytes_in_flight;
    if (!estimated_bytes) {
        return 1;
    }
    return cxl_rdma_admission_clamp_window(
        (uint32_t)DIV_ROUND_UP(estimated_bytes, bytes_per_region),
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
    uint32_t window;

    if (!state || !state->sq_capacity_regions) {
        return snap;
    }

    window = cxl_rdma_admission_clamp_window(
        state->dynamic_window_regions, state->sq_capacity_regions);
    snap.dynamic_window_regions = window;
    snap.sq_capacity_regions = state->sq_capacity_regions;
    snap.queue_len = queue_len;
    snap.inflight_len = inflight_len;
    snap.reserved_regions = state->reserved_regions;
    snap.outstanding_regions = queue_len + inflight_len +
                               state->reserved_regions;
    snap.goodput_ewma_bytes_per_ns = state->goodput_ewma_bytes_per_ns;
    snap.completion_latency_ewma_ns = state->completion_latency_ewma_ns;
    snap.bdp_estimate_regions =
        cxl_rdma_sidecar_admission_bdp_regions(state);
    snap.accepted_regions = state->accepted_regions;
    snap.overflow_cxl_regions = state->overflow_cxl_regions;
    snap.admission_closed_events = state->admission_closed_events;
    snap.goodput_drop_events = state->goodput_drop_events;
    snap.accept_rdma = running && bulk_active && !draining && !failed &&
                       !postcopy && snap.outstanding_regions < window;
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

    reservation->valid = false;
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
    reservation->valid = false;
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
                 sample_goodput < old_goodput &&
                 latency_ns > old_latency;

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
```

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: build succeeds and the new `/cxl/region/rdma-admission-*` tests pass.

- [ ] **Step 6: Commit**

```bash
git add migration/cxl-rdma.h migration/cxl-rdma.c tests/unit/test-cxl-hybrid-region.c
git commit -m "migration/cxl: add rdma dynamic admission controller"
```

## Task 2: Wire Admission Reservations Into RDMA Sidecar Runtime

**Files:**
- Modify: `migration/cxl-rdma.h`
- Modify: `migration/cxl-rdma.c`
- Test: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write failing runtime API tests**

Add this static-source test to `scripts/cxl-hybrid-warm-experiment-test.py` near other RDMA sidecar source checks:

```python
    def test_rdma_sidecar_runtime_uses_admission_reservation(self):
        rdma_text = (REPO_ROOT / "migration" / "cxl-rdma.c").read_text()

        reserve = rdma_text.index(
            "cxl_rdma_sidecar_try_reserve_bulk_admission")
        enqueue = rdma_text.index(
            "cxl_rdma_sidecar_enqueue_reserved_bulk_claim")
        complete = rdma_text.index(
            "cxl_rdma_sidecar_admission_note_completion")

        self.assertLess(reserve, enqueue)
        self.assertIn("CXLHybridRDMASidecarAdmissionState admission",
                      rdma_text)
        self.assertIn("CXLHybridRDMASidecarAdmissionReservation",
                      rdma_text)
        self.assertIn("ctx->admission", rdma_text)
        self.assertGreater(complete, enqueue)
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_rdma_sidecar_runtime_uses_admission_reservation
```

Expected: FAIL because the runtime reservation APIs and `ctx->admission` field do not exist.

- [ ] **Step 3: Add runtime API prototypes**

Add to `migration/cxl-rdma.h` after `cxl_rdma_sidecar_running()`:

```c
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
```

- [ ] **Step 4: Add admission state to the runtime context**

In `migration/cxl-rdma.c`, add this field to `CXLRDMASidecarContext`:

```c
    CXLHybridRDMASidecarAdmissionState admission;
```

Initialize it in `cxl_rdma_sidecar_start_internal()` after `ctx->inflight_capacity` is set:

```c
    cxl_rdma_sidecar_admission_state_init(&ctx->admission,
                                          ctx->inflight_capacity,
                                          ctx->bytes_per_region);
```

- [ ] **Step 5: Implement runtime admission APIs**

Add these functions in the `#ifdef CONFIG_RDMA` section after `cxl_rdma_sidecar_running()`:

```c
static bool cxl_rdma_sidecar_runtime_bulk_active(CXLRDMASidecarContext *ctx)
{
    return !ctx->ops.bulk_active || ctx->ops.bulk_active(ctx->ops.opaque);
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
        ctx->running && !ctx->stop,
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

    if (!ctx || !reservation) {
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    accepted = cxl_rdma_sidecar_admission_try_reserve(
        &ctx->admission,
        ctx->running && !ctx->stop,
        cxl_rdma_sidecar_runtime_bulk_active(ctx),
        ctx->draining,
        ctx->failed,
        ctx->ops.migration_postcopy &&
            ctx->ops.migration_postcopy(ctx->ops.opaque),
        ctx->queue_len,
        ctx->inflight_len,
        reservation,
        snapshot);
    qemu_mutex_unlock(&ctx->lock);
    return accepted;
}

void cxl_rdma_sidecar_cancel_bulk_admission(
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;

    if (!ctx || !reservation || !reservation->valid) {
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    cxl_rdma_sidecar_admission_cancel_reserve(&ctx->admission, reservation);
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
}

bool cxl_rdma_sidecar_enqueue_reserved_bulk_claim(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool bulk_active;
    uint32_t tail;

    if (!ctx || !claim || !reservation || !reservation->valid) {
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    bulk_active = cxl_rdma_sidecar_runtime_bulk_active(ctx);
    if (ctx->incoming || !ctx->running || ctx->failed || ctx->stop ||
        ctx->draining || !bulk_active || !ctx->queue_capacity ||
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
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
    return true;
}
```

Keep `cxl_rdma_sidecar_enqueue_bulk_claim()` for temporary compatibility by making it reserve and then call the reserved enqueue path:

```c
bool cxl_rdma_sidecar_enqueue_bulk_claim(
    const CXLHybridRDMABulkClaim *claim)
{
    CXLHybridRDMASidecarAdmissionReservation reservation = { 0 };

    if (!cxl_rdma_sidecar_try_reserve_bulk_admission(&reservation, NULL)) {
        return false;
    }
    return cxl_rdma_sidecar_enqueue_reserved_bulk_claim(claim, &reservation);
}
```

- [ ] **Step 6: Update completion-driven window accounting**

In `cxl_rdma_sidecar_poll_inflight_completion()`, after `completed_time_ns` and `page_bytes` are computed, add:

```c
        if (completed_pages && completed_time_ns) {
            cxl_rdma_sidecar_admission_note_completion(
                &ctx->admission,
                (uint64_t)completed_pages * page_bytes,
                completed_time_ns);
        }
```

Place it before releasing `claim.page_desc` so completed page counts are still available.

- [ ] **Step 7: Add non-RDMA stubs**

In the `#else` section of `migration/cxl-rdma.c`, add:

```c
bool cxl_rdma_sidecar_try_reserve_bulk_admission(
    CXLHybridRDMASidecarAdmissionReservation *reservation,
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot)
{
    if (reservation) {
        reservation->valid = false;
    }
    if (snapshot) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return false;
}

bool cxl_rdma_sidecar_enqueue_reserved_bulk_claim(
    const CXLHybridRDMABulkClaim *claim,
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    if (reservation) {
        reservation->valid = false;
    }
    return false;
}

void cxl_rdma_sidecar_cancel_bulk_admission(
    CXLHybridRDMASidecarAdmissionReservation *reservation)
{
    if (reservation) {
        reservation->valid = false;
    }
}

bool cxl_rdma_sidecar_get_admission_snapshot(
    CXLHybridRDMASidecarAdmissionSnapshot *snapshot)
{
    if (snapshot) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return false;
}
```

- [ ] **Step 8: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_rdma_sidecar_runtime_uses_admission_reservation
```

Expected: all commands pass.

- [ ] **Step 9: Commit**

```bash
git add migration/cxl-rdma.h migration/cxl-rdma.c \
        tests/unit/test-cxl-hybrid-region.c \
        scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration/cxl: wire rdma admission reservations"
```

## Task 3: Replace Fixed RAM Bulk Lane Split With Dynamic Overflow

**Files:**
- Modify: `migration/ram.c`
- Test: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing static scheduler tests**

Add this test to `scripts/cxl-hybrid-warm-experiment-test.py` near the RDMA sidecar source tests:

```python
    def test_ram_bulk_scheduler_uses_dynamic_rdma_before_cxl_overflow(self):
        ram_text = (REPO_ROOT / "migration" / "ram.c").read_text()
        save_host = ram_text[ram_text.index(
            "static int ram_save_host_page("):]
        save_host = save_host[:save_host.index("/* Update host page")]

        rdma_call = save_host.index("cxl_hybrid_rdma_enqueue_bulk_region")
        cxl_call = save_host.index("cxl_hybrid_cxl_enqueue_bulk_page")
        self.assertLess(rdma_call, cxl_call)

        self.assertNotIn(".rdma_budget_pages = 1", ram_text)
        self.assertNotIn(".cxl_background_pages = 1", ram_text)
        self.assertIn("cxl_rdma_sidecar_try_reserve_bulk_admission", ram_text)
        self.assertIn("cxl_rdma_sidecar_enqueue_reserved_bulk_claim", ram_text)
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_ram_bulk_scheduler_uses_dynamic_rdma_before_cxl_overflow
```

Expected: FAIL because `ram_save_host_page()` currently calls CXL before RDMA and hard-codes the 1:1 scheduler policy.

- [ ] **Step 3: Include the RDMA sidecar header in `migration/ram.c`**

If `migration/ram.c` does not already include it, add:

```c
#include "cxl-rdma.h"
```

- [ ] **Step 4: Remove fixed policy skip from CXL enqueue**

In `cxl_hybrid_cxl_enqueue_bulk_page()`, delete this fixed lane block:

```c
    if (!postcopy &&
        cxl_hybrid_scheduler_choose_bulk_lane(
            &(CXLHybridSchedulerPolicy) {
                .rdma_budget_pages = 1,
                .cxl_background_pages = 1,
            },
            region_index) != CXL_HYBRID_TRANSFER_CXL_LOW) {
        return 0;
    }
```

Leave the region boundary and batch logic intact so CXL remains bounded to the current region during precopy.

- [ ] **Step 5: Reserve RDMA before page-state claim**

At the beginning of `cxl_hybrid_rdma_enqueue_bulk_region()`, after the postcopy checks and before `cxl_hybrid_rdma_bulk_claim_init()`, declare:

```c
    CXLHybridRDMASidecarAdmissionReservation reservation = { 0 };
```

After `block_offset` and `region_offset` are computed, add:

```c
    if (!cxl_rdma_sidecar_try_reserve_bulk_admission(&reservation, NULL)) {
        return 0;
    }
```

For every return path after this reservation and before successful enqueue, cancel it:

```c
        cxl_rdma_sidecar_cancel_bulk_admission(&reservation);
```

The claim-failure path should look like:

```c
        trace_cxl_hybrid_rdma_bulk_skip(claim.region_index, pss->page,
                                        "claim");
        cxl_hybrid_ctrl_drop_rdma_pages(claim.page_desc);
        cxl_hybrid_rdma_bulk_claim_release(&claim);
        cxl_rdma_sidecar_cancel_bulk_admission(&reservation);
        return 0;
```

- [ ] **Step 6: Replace fixed RDMA policy with reserved enqueue**

Delete this block from `cxl_hybrid_rdma_enqueue_bulk_region()`:

```c
    if (cxl_hybrid_scheduler_choose_bulk_lane(
            &(CXLHybridSchedulerPolicy) {
                .rdma_budget_pages = 1,
                .cxl_background_pages = 1,
            },
            claim.region_index) != CXL_HYBRID_TRANSFER_RDMA_BULK) {
        return 0;
    }
```

Replace the enqueue call:

```c
    if (!cxl_rdma_sidecar_enqueue_bulk_claim(&claim)) {
```

with:

```c
    if (!cxl_rdma_sidecar_enqueue_reserved_bulk_claim(&claim,
                                                     &reservation)) {
```

The enqueue-failure path should keep the existing dirty/drop rollback because a runtime failure after reservation can still happen:

```c
        trace_cxl_hybrid_rdma_bulk_skip(claim.region_index, pss->page,
                                        "enqueue");
        cxl_hybrid_rdma_dirty_claimed_region(rs, &claim);
        cxl_hybrid_ctrl_drop_rdma_pages(claim.page_desc);
        cxl_hybrid_rdma_bulk_claim_release(&claim);
        return 0;
```

- [ ] **Step 7: Try RDMA before CXL in `ram_save_host_page()`**

Change the top of `ram_save_host_page()` from:

```c
    res = cxl_hybrid_cxl_enqueue_bulk_page(rs, pss);
    if (res) {
        return res;
    }
    res = cxl_hybrid_rdma_enqueue_bulk_region(rs, pss);
    if (res) {
        return res;
    }
```

to:

```c
    res = cxl_hybrid_rdma_enqueue_bulk_region(rs, pss);
    if (res) {
        return res;
    }
    res = cxl_hybrid_cxl_enqueue_bulk_page(rs, pss);
    if (res) {
        return res;
    }
```

This order makes CXL the overflow path when RDMA admission is closed.

- [ ] **Step 8: Run tests**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_ram_bulk_scheduler_uses_dynamic_rdma_before_cxl_overflow
ninja -C build tests/unit/test-cxl-hybrid-region tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-region --tap
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: all commands pass.

- [ ] **Step 9: Commit**

```bash
git add migration/ram.c scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration/ram: route cxl overflow from rdma admission"
```

## Task 4: Export Dynamic Admission Metrics Through Query-Migrate

**Files:**
- Modify: `migration/cxl-rdma.h`
- Modify: `migration/cxl-rdma.c`
- Modify: `migration/cxl.c`
- Modify: `qapi/migration.json`
- Test: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing QAPI/parser source test**

Add this test near `test_region_remap_stats_are_exported_to_qapi_and_summary`:

```python
    def test_rdma_dynamic_admission_metrics_are_exported_to_qapi(self):
        qapi_text = (REPO_ROOT / "qapi" / "migration.json").read_text()
        cxl_text = (REPO_ROOT / "migration" / "cxl.c").read_text()
        rdma_header = (REPO_ROOT / "migration" / "cxl-rdma.h").read_text()

        for field in (
            "rdma-sidecar-dynamic-window-regions",
            "rdma-sidecar-sq-capacity-regions",
            "rdma-sidecar-queue-len",
            "rdma-sidecar-inflight-len",
            "rdma-sidecar-goodput-ewma-bytes-per-ns",
            "rdma-sidecar-completion-latency-ewma-ns",
            "rdma-sidecar-bdp-estimate-regions",
            "rdma-sidecar-admission-accepted-regions",
            "rdma-sidecar-admission-overflow-cxl-regions",
            "rdma-sidecar-admission-closed-events",
            "rdma-sidecar-admission-goodput-drop-events",
        ):
            self.assertIn(f"'{field}'", qapi_text)

        self.assertIn("rdma_sidecar_dynamic_window_regions", rdma_header)
        self.assertIn("info->x_cxl->rdma_sidecar_dynamic_window_regions",
                      cxl_text)
        self.assertIn("rdma_bulk_stats.rdma_sidecar_dynamic_window_regions",
                      cxl_text)
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_rdma_dynamic_admission_metrics_are_exported_to_qapi
```

Expected: FAIL because the new QAPI and C fields do not exist.

- [ ] **Step 3: Add fields to sidecar bulk stats**

Extend `CXLHybridRDMASidecarBulkStats` in `migration/cxl-rdma.h`:

```c
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
```

- [ ] **Step 4: Populate bulk stats from the admission snapshot**

In `cxl_rdma_sidecar_get_stats()`, after existing page-state RDMA fields, add:

```c
    CXLHybridRDMASidecarAdmissionSnapshot snap;

    qemu_mutex_lock(&ctx->lock);
    snap = cxl_rdma_sidecar_admission_snapshot(
        &ctx->admission,
        ctx->running && !ctx->stop,
        !ctx->ops.bulk_active || ctx->ops.bulk_active(ctx->ops.opaque),
        ctx->draining,
        ctx->failed,
        ctx->ops.migration_postcopy &&
            ctx->ops.migration_postcopy(ctx->ops.opaque),
        ctx->queue_len,
        ctx->inflight_len);
    qemu_mutex_unlock(&ctx->lock);

    stats->rdma_sidecar_dynamic_window_regions =
        snap.dynamic_window_regions;
    stats->rdma_sidecar_sq_capacity_regions =
        snap.sq_capacity_regions;
    stats->rdma_sidecar_queue_len = snap.queue_len;
    stats->rdma_sidecar_inflight_len = snap.inflight_len;
    stats->rdma_sidecar_goodput_ewma_bytes_per_ns =
        snap.goodput_ewma_bytes_per_ns;
    stats->rdma_sidecar_completion_latency_ewma_ns =
        snap.completion_latency_ewma_ns;
    stats->rdma_sidecar_bdp_estimate_regions =
        snap.bdp_estimate_regions;
    stats->rdma_sidecar_admission_accepted_regions =
        snap.accepted_regions;
    stats->rdma_sidecar_admission_overflow_cxl_regions =
        snap.overflow_cxl_regions;
    stats->rdma_sidecar_admission_closed_events =
        snap.admission_closed_events;
    stats->rdma_sidecar_admission_goodput_drop_events =
        snap.goodput_drop_events;
```

Keep the non-RDMA `memset(stats, 0, sizeof(*stats))` behavior.

- [ ] **Step 5: Add QAPI schema fields**

In `qapi/migration.json`, add documentation near the existing RDMA sidecar metric comments:

```text
# @rdma-sidecar-dynamic-window-regions: current dynamic RDMA admission
#     window in region units.  (Since 11.0)
#
# @rdma-sidecar-sq-capacity-regions: RDMA admission hard cap derived
#     from the configured sidecar SQ/in-flight capacity.  (Since 11.0)
#
# @rdma-sidecar-queue-len: queued RDMA sidecar bulk claims.  (Since 11.0)
#
# @rdma-sidecar-inflight-len: posted RDMA sidecar bulk claims waiting
#     for completion.  (Since 11.0)
#
# @rdma-sidecar-goodput-ewma-bytes-per-ns: EWMA useful RDMA completion
#     goodput in bytes per nanosecond.  (Since 11.0)
#
# @rdma-sidecar-completion-latency-ewma-ns: EWMA post-to-completion
#     latency for useful RDMA completions.  (Since 11.0)
#
# @rdma-sidecar-bdp-estimate-regions: BDP estimate in region units.
#     (Since 11.0)
#
# @rdma-sidecar-admission-accepted-regions: RDMA bulk regions accepted
#     by dynamic admission.  (Since 11.0)
#
# @rdma-sidecar-admission-overflow-cxl-regions: RDMA candidate regions
#     sent to CXL because the dynamic window was full.  (Since 11.0)
#
# @rdma-sidecar-admission-closed-events: admission checks rejected
#     because RDMA was closed by lifecycle state.  (Since 11.0)
#
# @rdma-sidecar-admission-goodput-drop-events: dynamic-window decreases
#     caused by goodput or latency regression.  (Since 11.0)
#
```

Add fields to the `X-CXLInfo` data object:

```json
            'rdma-sidecar-dynamic-window-regions': {
                'type': 'uint32', 'features': [ 'unstable' ] },
            'rdma-sidecar-sq-capacity-regions': {
                'type': 'uint32', 'features': [ 'unstable' ] },
            'rdma-sidecar-queue-len': {
                'type': 'uint32', 'features': [ 'unstable' ] },
            'rdma-sidecar-inflight-len': {
                'type': 'uint32', 'features': [ 'unstable' ] },
            'rdma-sidecar-goodput-ewma-bytes-per-ns': {
                'type': 'number', 'features': [ 'unstable' ] },
            'rdma-sidecar-completion-latency-ewma-ns': {
                'type': 'uint64', 'features': [ 'unstable' ] },
            'rdma-sidecar-bdp-estimate-regions': {
                'type': 'uint32', 'features': [ 'unstable' ] },
            'rdma-sidecar-admission-accepted-regions': {
                'type': 'uint64', 'features': [ 'unstable' ] },
            'rdma-sidecar-admission-overflow-cxl-regions': {
                'type': 'uint64', 'features': [ 'unstable' ] },
            'rdma-sidecar-admission-closed-events': {
                'type': 'uint64', 'features': [ 'unstable' ] },
            'rdma-sidecar-admission-goodput-drop-events': {
                'type': 'uint64', 'features': [ 'unstable' ] },
```

- [ ] **Step 6: Populate QAPI output**

In `migration/cxl.c`, after existing RDMA sidecar fields are populated in query stats, add:

```c
    info->x_cxl->rdma_sidecar_dynamic_window_regions =
        rdma_bulk_stats.rdma_sidecar_dynamic_window_regions;
    info->x_cxl->rdma_sidecar_sq_capacity_regions =
        rdma_bulk_stats.rdma_sidecar_sq_capacity_regions;
    info->x_cxl->rdma_sidecar_queue_len =
        rdma_bulk_stats.rdma_sidecar_queue_len;
    info->x_cxl->rdma_sidecar_inflight_len =
        rdma_bulk_stats.rdma_sidecar_inflight_len;
    info->x_cxl->rdma_sidecar_goodput_ewma_bytes_per_ns =
        rdma_bulk_stats.rdma_sidecar_goodput_ewma_bytes_per_ns;
    info->x_cxl->rdma_sidecar_completion_latency_ewma_ns =
        rdma_bulk_stats.rdma_sidecar_completion_latency_ewma_ns;
    info->x_cxl->rdma_sidecar_bdp_estimate_regions =
        rdma_bulk_stats.rdma_sidecar_bdp_estimate_regions;
    info->x_cxl->rdma_sidecar_admission_accepted_regions =
        rdma_bulk_stats.rdma_sidecar_admission_accepted_regions;
    info->x_cxl->rdma_sidecar_admission_overflow_cxl_regions =
        rdma_bulk_stats.rdma_sidecar_admission_overflow_cxl_regions;
    info->x_cxl->rdma_sidecar_admission_closed_events =
        rdma_bulk_stats.rdma_sidecar_admission_closed_events;
    info->x_cxl->rdma_sidecar_admission_goodput_drop_events =
        rdma_bulk_stats.rdma_sidecar_admission_goodput_drop_events;
```

- [ ] **Step 7: Run QAPI and source tests**

Run:

```bash
ninja -C build qemu-system-x86_64 tests/unit/test-cxl-hybrid-region
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_rdma_dynamic_admission_metrics_are_exported_to_qapi
```

Expected: build succeeds and the source test passes.

- [ ] **Step 8: Commit**

```bash
git add migration/cxl-rdma.h migration/cxl-rdma.c migration/cxl.c \
        qapi/migration.json scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration/cxl: export rdma admission metrics"
```

## Task 5: Parse Dynamic Admission Metrics In Experiment Summaries

**Files:**
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing parser summary test**

Add this test near the existing RDMA sidecar parser tests:

```python
    def test_extract_summary_includes_rdma_dynamic_admission_metrics(self):
        summary = self.mod.extract_summary([
            {
                "x-cxl": {
                    "rdma-sidecar-dynamic-window-regions": 3,
                    "rdma-sidecar-sq-capacity-regions": 8,
                    "rdma-sidecar-queue-len": 1,
                    "rdma-sidecar-inflight-len": 2,
                    "rdma-sidecar-goodput-ewma-bytes-per-ns": 2.5,
                    "rdma-sidecar-completion-latency-ewma-ns": 1000000,
                    "rdma-sidecar-bdp-estimate-regions": 3,
                    "rdma-sidecar-admission-accepted-regions": 10,
                    "rdma-sidecar-admission-overflow-cxl-regions": 4,
                    "rdma-sidecar-admission-closed-events": 1,
                    "rdma-sidecar-admission-goodput-drop-events": 2,
                },
                "dst-query-migrate": {
                    "x-cxl": {
                        "rdma-sidecar-dynamic-window-regions": 2,
                        "rdma-sidecar-sq-capacity-regions": 8,
                    }
                },
            }
        ])

        self.assertEqual(summary["rdma_sidecar_dynamic_window_regions"], 3)
        self.assertEqual(summary["rdma_sidecar_sq_capacity_regions"], 8)
        self.assertEqual(summary["rdma_sidecar_queue_len"], 1)
        self.assertEqual(summary["rdma_sidecar_inflight_len"], 2)
        self.assertEqual(summary["rdma_sidecar_goodput_ewma_bytes_per_ns"], 2.5)
        self.assertEqual(summary["rdma_sidecar_completion_latency_ewma_ns"],
                         1000000)
        self.assertEqual(summary["rdma_sidecar_bdp_estimate_regions"], 3)
        self.assertEqual(summary["rdma_sidecar_admission_accepted_regions"], 10)
        self.assertEqual(
            summary["rdma_sidecar_admission_overflow_cxl_regions"], 4)
        self.assertEqual(summary["rdma_sidecar_admission_closed_events"], 1)
        self.assertEqual(
            summary["rdma_sidecar_admission_goodput_drop_events"], 2)
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_extract_summary_includes_rdma_dynamic_admission_metrics
```

Expected: FAIL because the new metrics are not in `RDMA_SIDECAR_METRICS`.

- [ ] **Step 3: Extend parser metric list**

In `scripts/cxl-hybrid-warm-experiment.py`, extend `RDMA_SIDECAR_METRICS`:

```python
    "rdma_sidecar_dynamic_window_regions",
    "rdma_sidecar_sq_capacity_regions",
    "rdma_sidecar_queue_len",
    "rdma_sidecar_inflight_len",
    "rdma_sidecar_goodput_ewma_bytes_per_ns",
    "rdma_sidecar_completion_latency_ewma_ns",
    "rdma_sidecar_bdp_estimate_regions",
    "rdma_sidecar_admission_accepted_regions",
    "rdma_sidecar_admission_overflow_cxl_regions",
    "rdma_sidecar_admission_closed_events",
    "rdma_sidecar_admission_goodput_drop_events",
```

The existing loop that converts underscores to hyphens should then include the new fields automatically.

- [ ] **Step 4: Verify grouped summary uses the extended metric list**

Run:

```bash
rg -n "\\*RDMA_SIDECAR_METRICS" scripts/cxl-hybrid-warm-experiment.py
```

Expected: at least one match inside `summarize_grouped_runs()`. The new metrics are included there because Step 3 extended `RDMA_SIDECAR_METRICS`.

- [ ] **Step 5: Run parser tests**

Run:

```bash
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py \
                   scripts/cxl-hybrid-warm-experiment-test.py
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_extract_summary_includes_rdma_dynamic_admission_metrics
python3 scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: compile succeeds and parser tests pass.

- [ ] **Step 6: Commit**

```bash
git add scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "scripts: summarize rdma admission metrics"
```

## Task 6: Deprecate Fixed Coverage Semantics In Code And Docs

**Files:**
- Modify: `qapi/migration.json`
- Modify: `migration/options.c`
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`
- Modify: `docs/superpowers/reports/2026-06-04-rdma-cxl-current-experiment-config.md`

- [ ] **Step 1: Write failing deprecation source test**

Add this test near the existing RDMA sidecar parameter tests:

```python
    def test_rdma_sidecar_cover_percent_is_documented_as_deprecated(self):
        qapi_text = (REPO_ROOT / "qapi" / "migration.json").read_text()
        options_text = (REPO_ROOT / "migration" / "options.c").read_text()
        docs_text = (
            REPO_ROOT / "docs" / "superpowers" / "reports" /
            "2026-06-04-rdma-cxl-current-experiment-config.md"
        ).read_text()

        self.assertIn("deprecated; dynamic admission ignores this value",
                      qapi_text)
        self.assertIn("x-cxl-rdma-sidecar-max-cover-percent is deprecated",
                      options_text)
        self.assertNotIn("--x-cxl-rdma-sidecar-max-cover-percent 50",
                         docs_text)
        self.assertIn("RDMA sidecar in-flight parameter is a safety cap",
                      docs_text)
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_rdma_sidecar_cover_percent_is_documented_as_deprecated
```

Expected: FAIL because the QAPI/options/docs still describe fixed coverage as active policy.

- [ ] **Step 3: Update QAPI parameter comments**

In `qapi/migration.json`, replace the `x-cxl-rdma-sidecar-max-cover-percent` comment with:

```text
# @x-cxl-rdma-sidecar-max-cover-percent: Deprecated; dynamic admission
#     ignores this value and routes RDMA overflow to CXL based on runtime
#     sidecar state.  Kept only for compatibility with older experiment
#     commands.  (Since 11.0)
```

Replace the `x-cxl-rdma-sidecar-max-inflight-regions` comment with:

```text
# @x-cxl-rdma-sidecar-max-inflight-regions: Hard safety cap for the
#     CXL hybrid RDMA sidecar SQ and dynamic admission window.  It no
#     longer expresses the desired fixed in-flight depth.  (Since 11.0)
```

- [ ] **Step 4: Warn when deprecated cover percent is explicitly set**

In `migration/options.c`, keep validation that the value is between 0 and 100, but add a warning in the parameter application path when `has_x_cxl_rdma_sidecar_max_cover_percent` is true:

```c
        warn_report("x-cxl-rdma-sidecar-max-cover-percent is deprecated; "
                    "dynamic RDMA admission ignores this value");
```

Place it next to the existing assignment to `dest->x_cxl_rdma_sidecar_max_cover_percent` or `s->parameters.x_cxl_rdma_sidecar_max_cover_percent`.

- [ ] **Step 5: Update the canonical experiment documentation**

In `docs/superpowers/reports/2026-06-04-rdma-cxl-current-experiment-config.md`:

Remove this argument from the canonical command:

```bash
  --x-cxl-rdma-sidecar-max-cover-percent 50 \
```

Replace the bullet:

```text
- Keep RDMA sidecar coverage at `50%`.
```

with:

```text
- Treat `--x-cxl-rdma-sidecar-max-inflight-regions` as a safety cap for the
  dynamic RDMA admission window, not as a fixed desired depth.
- Do not pass `--x-cxl-rdma-sidecar-max-cover-percent`; dynamic admission
  ignores fixed coverage and sends overflow to CXL.
```

Add this sentence to the reference-run interpretation section:

```text
After dynamic admission lands, historical `8/50` runs remain useful only as
fixed-policy baselines; new runs should report dynamic window, SQ cap, goodput
EWMA, BDP estimate, and CXL overflow counts.
```

- [ ] **Step 6: Run tests**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
    WarmExperimentScriptTest.test_rdma_sidecar_cover_percent_is_documented_as_deprecated
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py \
                   scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: tests pass and script compile succeeds.

- [ ] **Step 7: Commit**

```bash
git add qapi/migration.json migration/options.c \
        scripts/cxl-hybrid-warm-experiment-test.py \
        docs/superpowers/reports/2026-06-04-rdma-cxl-current-experiment-config.md
git commit -m "docs: deprecate fixed rdma coverage policy"
```

## Task 7: Full Verification And Experiment Gate

**Files:**
- No code edits unless verification exposes a defect in the previous tasks.
- Create report after a successful hardware run: `docs/superpowers/reports/2026-06-04-rdma-cxl-dynamic-admission-results.md`

- [ ] **Step 1: Run build and unit test checkpoint**

Run:

```bash
ninja -C build qemu-system-x86_64 \
    tests/unit/test-cxl-hybrid-control \
    tests/unit/test-cxl-hybrid-region \
    tests/unit/test-migration-postcopy
./build/tests/unit/test-cxl-hybrid-control --tap
./build/tests/unit/test-cxl-hybrid-region --tap
./build/tests/unit/test-migration-postcopy --tap
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py \
                   scripts/cxl-hybrid-warm-experiment-test.py
python3 scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: build succeeds, C unit tests pass, Python compile succeeds, parser tests pass.

- [ ] **Step 2: Run current dynamic-admission destination-stall experiment**

Run:

```bash
sudo -n env QEMU_CXL_WARM_ALLOW_UNPRIVILEGED=1 \
/usr/local/bin/numactl --cpunodebind=4-7 --membind=4-7 \
/usr/bin/python3 scripts/cxl-hybrid-warm-experiment.py \
  --keep-dir \
  --pressure remap_xlarge_random_rw \
  --mode hybrid_parallel_rdma_cxl \
  --threshold-profile balanced \
  --repeat 1 \
  --migration-timeout 120 \
  --trace-profile minimal \
  --x-cxl-rdma-sidecar-max-inflight-regions 8 \
  --accel kvm \
  --in-memory-guest-latency
```

Expected:

- `final_status=completed`;
- destination is still running;
- `guest_in_memory_latency.valid=true`;
- `guest_in_memory_latency.dump_source=primary`;
- `rdma_sidecar_dynamic_window_regions > 0`;
- `rdma_sidecar_sq_capacity_regions == 8`;
- `rdma_sidecar_admission_accepted_regions + rdma_sidecar_admission_overflow_cxl_regions > 0`;
- no destination cleanup/UFFD failure after completion.

- [ ] **Step 3: Optional forced overflow smoke run**

Run the same command with:

```bash
  --x-cxl-rdma-sidecar-max-inflight-regions 1
```

Expected:

- dynamic window remains capped at `1`;
- CXL worker bytes are non-zero;
- `rdma_sidecar_admission_overflow_cxl_regions` is non-zero when there are more RDMA candidates than the one-region cap can absorb.

- [ ] **Step 4: Write result report**

Generate the report from the newest dynamic-admission result JSON:

```bash
RESULT_JSON="$(
  find /tmp -path '*remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-*run01/result.json' \
    -printf '%T@ %p\n' | sort -nr | head -1 | cut -d' ' -f2-
)"
python3 - "$RESULT_JSON" > \
  docs/superpowers/reports/2026-06-04-rdma-cxl-dynamic-admission-results.md <<'PY'
import json
import sys
from pathlib import Path

result_path = Path(sys.argv[1])
result = json.loads(result_path.read_text())
summary = result.get("summary") or {}
latency = result.get("latency") or {}
guest = result.get("guest_latency") or {}

print("# RDMA CXL Dynamic Admission Results")
print()
print("Date: 2026-06-04")
print("Branch: `rdma-cxl-parallel-hybrid`")
print()
print("## Command")
print()
print("Dynamic admission run used the current canonical destination-stall")
print("shape with `--x-cxl-rdma-sidecar-max-inflight-regions` treated as")
print("a safety cap and no fixed RDMA coverage parameter.")
print()
print("## Verification")
print()
print("- QEMU build: recorded before this report")
print("- C unit tests: recorded before this report")
print("- Python parser tests: recorded before this report")
print(f"- Dynamic admission run: {result.get('final_status')}")
print(f"- Guest latency valid: {guest.get('valid')}")
print(f"- Guest latency dump source: {guest.get('dump_source')}")
print()
print("## Dynamic Admission Metrics")
print()
print("| run directory | status | total ms | SQ cap | final window | "
      "BDP regions | accepted RDMA | overflow CXL | RDMA goodput B/ns | "
      "RDMA latency ns | CXL bytes | RDMA bytes |")
print("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
      "---: | ---: | ---: |")
print("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
    result_path.parent,
    result.get("final_status"),
    latency.get("total_time_ms"),
    summary.get("rdma_sidecar_sq_capacity_regions", 0),
    summary.get("rdma_sidecar_dynamic_window_regions", 0),
    summary.get("rdma_sidecar_bdp_estimate_regions", 0),
    summary.get("rdma_sidecar_admission_accepted_regions", 0),
    summary.get("rdma_sidecar_admission_overflow_cxl_regions", 0),
    summary.get("rdma_sidecar_goodput_ewma_bytes_per_ns", 0),
    summary.get("rdma_sidecar_completion_latency_ewma_ns", 0),
    summary.get("page_state_cxl_worker_bytes", 0),
    summary.get("page_state_rdma_completed_bytes", 0),
))
print()
print("## Interpretation")
print()
print("RDMA admission is valid for this sample only if the run completed,")
print("destination-side in-memory guest latency used a primary dump, and")
print("accepted plus overflow regions is non-zero. Compare RDMA completed")
print("bytes with CXL worker bytes to determine whether the dynamic window")
print("kept both lanes active or overflowed most work to CXL.")
PY
```

Expected: the report contains one table row populated from `result.json`; every cell in that row is a concrete value from the run.

- [ ] **Step 5: Commit verification report**

```bash
git add docs/superpowers/reports/2026-06-04-rdma-cxl-dynamic-admission-results.md
git commit -m "docs: report rdma dynamic admission results"
```

## Final Acceptance Checklist

Before marking the branch ready:

- [ ] `migration/ram.c` has no active `.rdma_budget_pages = 1` or `.cxl_background_pages = 1` lane split.
- [ ] RDMA reservation happens before RDMA page-state claim.
- [ ] CXL bulk enqueue is the fallback after RDMA admission rejects.
- [ ] `x-cxl-rdma-sidecar-max-cover-percent` is documented as deprecated and ignored for active lane split.
- [ ] Dynamic admission metrics appear in `query-migrate` and parser summaries.
- [ ] C unit tests and parser tests pass.
- [ ] At least one KVM destination-stall run completes with primary in-memory guest latency dump.
