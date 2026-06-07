# RDMA CXL Dynamic Lane Admission Design

Date: 2026-06-04
Branch: `rdma-cxl-parallel-hybrid`
Status: implemented for converged experiment cleanup

Supersedes and removes the fixed RDMA/CXL bulk split policy from the earlier
`max-inflight-regions` plus fixed-coverage experiments. It preserves the
page-state worker architecture from:

- `docs/superpowers/specs/2026-05-30-rdma-cxl-page-state-control-plane-design.md`
- `docs/superpowers/reports/2026-06-04-rdma-cxl-current-experiment-config.md`

## Summary

The current RDMA+CXL bulk scheduler uses fixed lane split policy. In practice,
that is not a good control mechanism because RDMA capacity is determined by the
send queue, BDP, completion rate, and setup/runtime variance, not by a static
region percentage. The new policy makes RDMA an opportunistic fast lane with a
dynamic admission window. CXL remains the overflow and correctness lane.

On each bulk scheduling decision, the RAM scheduler reads an RDMA admission
snapshot. If RDMA is running, has window space, and recent goodput still
justifies more outstanding work, the scheduler may claim a region for RDMA. If
not, the pages are sent directly to the CXL low-priority worker queue. The
scheduler must not claim RDMA page-state and then discover that the RDMA queue
cannot accept the work.

## Goals

- Stop using fixed RDMA region percentage as the lane balancing policy.
- Stop using page-index or region-index modulo policy for RDMA/CXL split.
- Make RDMA in-flight depth dynamic and bounded by SQ/BDP observations.
- Monitor RDMA goodput and completion latency online.
- Send overflow directly to CXL without RDMA claim/drop/dirty rollback.
- Keep admission O(1) and cheap enough for performance runs.
- Preserve page-state as the only cross-node ownership arbiter.
- Preserve CXL poller and CXL worker semantics.
- Keep RDMA writes targeted at destination RAM.

## Non-Goals

- No new scheduler thread in this phase.
- No cross-lane migration of already claimed pages.
- No per-4 KiB-page RDMA admission check.
- No destination-side postcopy RDMA prefetch enablement.
- No source-side brake policy changes.
- No automatic tuning based on workload names.

## Current Problems

Two fixed controls drove the old RDMA scheduling path:

- `x-cxl-rdma-sidecar-max-inflight-regions`;
- `x-cxl-rdma-sidecar-max-cover-percent`.

The converged cleanup removes the fixed coverage parameter and the modulo
scheduler. `x-cxl-rdma-sidecar-max-inflight-regions` remains only as an
automatic transport/resource sizing hint.

This creates three problems:

- RDMA can be underfilled when SQ/BDP would support more outstanding work.
- RDMA can be over-admitted when completion latency or goodput says it is no
  longer useful.
- If RDMA enqueue fails after page-state claims, the code has to drop claims and
  dirty the pages again, adding avoidable control-plane churn.

## Dynamic Admission Model

RDMA admission becomes a runtime decision:

```text
for each bulk region or CXL batch candidate:
    snapshot = rdma_sidecar_admission_snapshot()
    if snapshot.accept_rdma:
        claim pages for RDMA
        enqueue RDMA bulk claim
    else:
        enqueue pages to CXL_LOW
```

Admission is evaluated at region or batch granularity. For the current 2 MiB
fault region size, a 64 MiB run performs about 32 RDMA admission checks. It must
not perform one admission check per 4 KiB page.

The sidecar owns the dynamic window:

- `sq_capacity_regions`: ceiling derived from QP/SQ creation, CQ/device
  resources, and migration geometry;
- `window_regions`: current dynamic admission window;
- `queue_len + inflight_len`: current outstanding RDMA demand;
- `goodput_ewma_bytes_per_ns`: recent completed RDMA data rate;
- `completion_latency_ewma_ns`: recent completion latency;
- `last_completed_bytes` and `last_completed_ns`: measurement inputs.

The scheduler only reads a snapshot. It does not compute goodput from history,
walk RDMA queues, or inspect CQ state.

## Window Adjustment

The sidecar updates the window on completion and, optionally, on short idle
ticks. The controller must probe RDMA in bounded waves up to transport/resource
capacity before treating CXL as the steady overflow lane. A BDP estimate derived from
the current window is self-limiting if it is applied too early: a one-region
window produces one-region goodput, which then keeps the BDP estimate small.

A conservative implementation should therefore use probe-first additive
increase and multiplicative decrease:

- Start with a bounded initial probe window, then grow additively while
  transport goodput holds.
- Until the controller observes a material goodput drop with a material latency
  rise, do not use self-estimated BDP as a hard cap on admission.
- On completions that improve or maintain goodput without raising completion
  latency materially, keep or increase the window up to `sq_capacity_regions`.
- If goodput falls materially while latency rises materially, halve the window,
  but keep it at least one region.
- If the sidecar sees CQ errors, migration failure, drain, or postcopy entry,
  close admission immediately.
- If the queue remains full while completions are slow, stop accepting new RDMA
  work until outstanding drops below the current window.

BDP can be inferred in region units:

```text
bdp_regions =
    ceil(goodput_ewma_bytes_per_ns * completion_latency_ewma_ns /
         bytes_per_region)
```

The effective window is:

```text
probe_window_regions = clamp(controller_window, 1, sq_capacity_regions)
target_regions = clamp(bdp_regions, 1, sq_capacity_regions)
```

After the probe phase has observed a material regression, the effective window
may use `target_regions` as an upper bound. It should expose both values so
later experiments can compare the controller decision against observed BDP.

## Scheduler Behavior

`migration/ram.c` should stop asking a fixed
`CXLHybridSchedulerPolicy { rdma_budget_pages, cxl_background_pages }` to choose
a lane for the bulk path.

The new scheduler rules:

- If postcopy or postcopy preemption is active, do not admit RDMA bulk.
- If RDMA sidecar is absent, failed, draining, not running, or not in bulk
  phase, enqueue CXL.
- If RDMA outstanding work is at or above the dynamic window, enqueue CXL.
- If RDMA recent goodput state is below the admission threshold, enqueue CXL.
- If RDMA is admitted, claim page-state for RDMA before enqueue.
- If the RDMA enqueue still fails, drop the claim and fall back to CXL in the
  same scheduling attempt where possible.
- Zero pages remain non-RDMA and publish through the zero-page/page-state path.

The critical ordering is that RDMA admission must be checked before RDMA
page-state claim. This avoids the current claim/drop/dirty rollback path as the
normal overflow behavior.

## Cost Estimate

Admission must be O(1).

Expected cost at region granularity:

- 2 MiB region size;
- 64 MiB migration payload gives about 32 admission checks;
- each snapshot reads a handful of atomics or a short lock-protected struct;
- even at 1 microsecond per check, the total is about 32 microseconds.

That is negligible compared with millisecond-scale bulk copy, RDMA completion,
and postcopy handoff timings.

Per-page admission is explicitly rejected:

- 64 MiB contains 16,384 4 KiB pages;
- 1 microsecond per page would add about 16 milliseconds;
- that would distort the performance experiment.

For this reason, the scheduler must evaluate RDMA admission at region or merged
batch boundaries only.

## Metrics

Add query and parser fields for:

- `rdma-sidecar-dynamic-window-regions`;
- `rdma-sidecar-sq-capacity-regions`;
- `rdma-sidecar-queue-len`;
- `rdma-sidecar-inflight-len`;
- `rdma-sidecar-goodput-ewma-bytes-per-ns`;
- `rdma-sidecar-completion-latency-ewma-ns`;
- `rdma-sidecar-bdp-estimate-regions`;
- `rdma-sidecar-admission-accepted-regions`;
- `rdma-sidecar-admission-overflow-cxl-regions`;
- `rdma-sidecar-admission-closed-events`;
- `rdma-sidecar-admission-goodput-drop-events`.

Existing lane byte/time fields remain important:

- `page-state-cxl-worker-bytes`;
- `page-state-cxl-worker-time-ns`;
- `page-state-rdma-completed-bytes`;
- `page-state-rdma-completed-time-ns`.

Reports should compute lane goodput from the existing byte/time fields and
explain how many candidate regions overflowed to CXL because RDMA admission was
closed.

## CLI And Compatibility

The fixed policy semantics are removed from the converged experiment surface.

- `x-cxl-rdma-sidecar-max-cover-percent` is no longer a QAPI/script
  parameter.
- `x-cxl-rdma-sidecar-max-inflight-regions` no longer controls fixed coverage
  or desired in-flight depth.

For compatibility, `max-inflight-regions` may remain accepted as an auto hint
for older command lines, but it must not cap SQ/window size or total RDMA
coverage.

The canonical performance command uses `scripts/rdma_cxl_parallel_experiment.py`
with automatic sizing and postcopy dirty RDMA enabled.

## Failure And Fallback

RDMA admission must close and overflow to CXL when:

- the sidecar is not connected or not running;
- migration leaves precopy bulk;
- drain begins;
- RDMA post or CQ reports failure;
- outstanding work reaches the dynamic window;
- goodput/latency controller marks RDMA saturated.

CXL fallback remains page-state based. Pages not admitted to RDMA should be
claimed by CXL normally, not dirtied again after a failed RDMA claim.

If an RDMA post fails after admission and claim, the existing RDMA drop path
must still return claims to a dirty/CXL-consumable state. That path becomes an
exception path, not the steady-state overflow mechanism.

## Testing

Unit tests should cover:

- admission accepts when running, bulk-active, and outstanding is below window;
- admission rejects when queue plus in-flight equals window;
- admission rejects when sidecar is failed, draining, or postcopy begins;
- completion updates goodput EWMA and completion latency EWMA;
- additive increase grows the dynamic window within SQ cap;
- multiplicative decrease shrinks the window after goodput/latency regression;
- RAM scheduler sends overflow to CXL without first claiming RDMA pages;
- fixed coverage strings no longer appear in active migration/QAPI/scripts/tests;
- zero pages are never admitted to RDMA.

Parser tests should cover all new metrics and grouped summary fields.

Integration experiments should compare:

- fixed historical runs for reference only;
- dynamic admission with the current canonical destination-stall command;
- a forced low SQ cap to prove overflow-to-CXL behavior;
- a high cap to prove the window grows only while goodput improves.

## Acceptance Criteria

- No fixed region percentage controls RDMA/CXL lane split.
- No page-index modulo policy remains in the active bulk scheduler.
- RDMA admission is checked before RDMA page-state claim.
- RDMA overflow goes directly to CXL low-priority worker queue.
- Dynamic window and goodput metrics appear in `query-migrate` and experiment
  summaries.
- Admission check cost is O(1) and performed at region or batch granularity.
- Current destination-stall success checks remain valid.
- Both lanes can carry non-zero bytes when RDMA goodput supports it.
- CXL remains active and absorbs overflow when RDMA is saturated.
