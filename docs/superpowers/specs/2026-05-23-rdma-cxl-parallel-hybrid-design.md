# RDMA + CXL Parallel Hybrid Design

Date: 2026-05-23
Revision: 2026-05-25 after no-brake baseline

## Goal

Add an RDMA sidecar bulk data plane to normal hybrid migration so cold regions
can move to the destination in parallel with the CXL path. For the next v1
round, CXL remains the authority for postcopy visibility, fault publication,
and remap state, while source-side brake/remap is disabled by default.

The intended v1 end state is:

- bulk: RDMA and CXL can move disjoint whole fault regions in parallel;
- switch: source-authoritative ready state is committed to destination-visible
  state after the final dirty sync;
- postcopy: CXL fault publication remains the correctness fallback;
- brake: skipped in the default experiment path with `--x-cxl-disable-brake`.

## Context From Brake Exploration

The hot-brake exploration produced a no-brake baseline on 2026-05-25:

- command output: `/tmp/cxl-hot-brake-disabled-sourcefirst-r1.json`
- result dir:
  `/tmp/cxl-hybrid-warm-exp-4tqmj7vz/remap_xlarge_random_rw/hybrid_postcopy_auto-custom-run01`
- total time: 34 ms
- downtime: 1 ms
- switch reason: `precopy-complete`
- remap attempts/successes/coverage: `0/0/0%`
- region publish pages: 15934
- region wait: 3.106 ms
- stream fault pause: 3.270 ms
- source-side in-memory precopy stall: 2.158 ms

The brake-enabled policies tested in that round remapped only 3 regions and did
not reduce region publication enough to pay for their source-side stall. Until
new data says otherwise, the RDMA+CXL path should use no-brake as its default
baseline and treat brake integration as postponed work.

## Why This Is Not Native RDMA

QEMU native RDMA cannot be reused as-is:

- native RDMA currently owns the RAM save path;
- the current capability model rejects RDMA together with postcopy;
- hybrid still needs mapped-ram and postcopy semantics.

This design introduces a separate RDMA mechanism for hybrid, not a mode switch
to native RDMA.

## Updated Constraints

- Default experiments must pass `--x-cxl-disable-brake`.
- Do not optimize brake while implementing the first RDMA+CXL path.
- Do not depend on native RDMA being postcopy-compatible.
- Keep source authoritative during precopy and at switch.
- Keep the first implementation deterministic and easy to measure.
- Use whole 2 MiB postcopy fault regions as the v1 ownership unit.
- Avoid mixed RDMA/CXL ownership inside one fault region in v1.
- Treat CXL postcopy publication as the correctness fallback when RDMA-ready
  data becomes stale or unavailable.

## Core Model

The design separates three states:

1. page data is present on the destination;
2. source still considers that data current;
3. the destination may consume that page without fault publication.

RDMA bulk only advances state 1 plus a source-authoritative ready bit. It must
not directly publish destination visibility.

CXL bulk and postcopy fault handling continue to advance the existing staged,
published, and visible state. At switch, the source commits still-current
RDMA-ready regions into the same destination-visible contract used by CXL.

## Ownership Model

Use whole 2 MiB fault regions as the first ownership unit.

- RDMA-owned region: copied to destination final memory, tracked as RDMA-ready
  only if it survives later dirty invalidation.
- CXL-owned region: follows the current mapped-ram, publish, and remap path.
- Dirty after RDMA-ready: clears RDMA-ready and returns the region to the CXL
  fallback path.
- Partially stale RDMA region in v1: invalidate the whole region rather than
  mixing RDMA and CXL inside one fault region.

Whole-region invalidation can amplify CXL fallback work. That amplification is
acceptable only if it is measured and remains smaller than the bulk benefit.

## Recommended Approach

Use a no-brake, measurement-first rollout.

Alternative A is to implement RDMA bulk ownership first and keep postcopy CXL as
the only fallback. This is the recommended path because it isolates the value of
parallel bulk transfer from the unresolved brake policy.

Alternative B is to make CXL continue transmitting during handoff before adding
RDMA. This may help, but the handoff window appears small enough that it should
be measured before being treated as a main optimization.

Alternative C is to resume hot-brake policy work before RDMA. Current data does
not justify this; it risks spending effort on source-side work that no-brake
already avoids.

## Measurement Gates

### Gate 1: RDMA Invalidation Amplification

Before treating the parallel path as a win, report these fields for each run:

- `rdma_ready_regions`
- `rdma_ready_pages`
- `rdma_invalidated_regions`
- `rdma_ready_pages_lost`
- `cxl_republish_regions_due_to_rdma_invalidate`
- `cxl_republish_pages_due_to_rdma_invalidate`
- `rdma_invalidate_publish_amplification`

The amplification ratio is:

```text
cxl_republish_pages_due_to_rdma_invalidate / max(rdma_ready_pages_lost, 1)
```

If the ratio is high but region wait and stream fault pause remain low, the
fallback may still be acceptable. The decision criterion is guest stall, not
only bytes.

### Gate 2: Handoff CXL Concurrency

Measure the handoff window before assuming CXL can do useful concurrent work
while RDMA is unavailable.

Required timing fields already exist or should be extended from the current
timeline parser:

- `handoff_control_src_start_to_dst_vm_started_ms`
- `handoff_control_src_start_to_dst_ack_ms`
- `handoff_control_src_start_to_downtime_end_ms`
- `handoff_control_src_downtime_end_to_postcopy_active_ms`
- `handoff_control_src_postcopy_active_to_completion_enter_ms`
- `handoff_control_src_completion_enter_to_completed_ms`
- `handoff_region_publish_pages`
- `handoff_region_publish_time_ns`
- `handoff_postcopy_region_wait_time_ns`
- `handoff_postcopy_fault_total_time_ns`

Only add handoff-concurrent CXL transfer if the measured window can hold enough
CXL publish work to reduce guest stall. If the window is only a few
milliseconds, keep the first RDMA implementation focused on bulk.

## Phased Plan

### Phase 0: Baseline Preservation

Preserve the experiment infrastructure now in `hybrid-experiment-infra`:

- source-first in-memory guest latency dump;
- source fallback for guest latency dump;
- `--x-cxl-disable-brake`;
- no-brake baseline report.

New RDMA+CXL implementation work should branch from this infra base, not from
the hot-brake exploration branch.

### Phase 0.5: Measurement Before Behavior

Add the counters and parser fields for RDMA invalidation amplification and
handoff CXL capacity. This phase may use trace-only or shadow-state data. It
should not require brake changes.

### Phase 1: Source-Authoritative RDMA State

Add the state needed to make the sidecar safe:

- RDMA-ready bitmap or equivalent per-region state;
- dirty invalidation for RDMA-ready regions;
- region ownership map;
- switch-time commit path into destination-visible state.

The CXL brake path remains disabled in default experiments.

### Phase 2: Bulk Parallel Prototype

Split bulk work by whole 2 MiB regions:

- RDMA handles selected cold regions;
- CXL handles the remaining regions and all fallback publication;
- switch-time final dirty sync downgrades stale RDMA-ready regions before
  committing visibility.

The scheduler should prefer simple disjoint ownership over fine-grained
interleaving. Mixed 256 KiB or 4 KiB ownership is deferred until whole-region
amplification is measured.

### Phase 3: Evaluation And Policy Decision

Compare against:

- no-brake normal hybrid;
- pure precopy;
- native postcopy;
- native RDMA precopy if available.

Primary success criteria:

- lower in-memory guest stall than no-brake normal hybrid;
- no increase in downtime;
- lower or equal postcopy region wait and stream fault pause;
- RDMA invalidation amplification is bounded and explained;
- no hidden brake work in the default path.

### Paused Phase: Brake Integration

Do not implement dirty brake republish in the RDMA+CXL v1 branch. Brake work can
resume later only if a measured policy shows positive marginal value against the
no-brake baseline.

## Open Interface Question

The current CXL visible map may be too CXL-specific if RDMA pages are direct
destination memory rather than CXL backing.

If that happens, the implementation should introduce either:

- a generic destination-visible bitmap, or
- an ownership layer above the existing CXL visible map.

This must be decided during Phase 1, before making RDMA pages visible to the
destination.

## Non-Goals

- No source-side brake optimization in v1.
- No mixed 256 KiB or 4 KiB RDMA/CXL interleaving before the 2 MiB model is
  measured.
- No RDMA participation in brake.
- No native RDMA postcopy compatibility work.
- No workload-specific policy tuned only for `remap_xlarge_random_rw`.

## Validation Plan

Use the existing experimental setup:

- same NUMA binding;
- same DAX-backed CXL setup;
- same in-memory source-first guest stall metric;
- same postcopy region wait and publish breakdowns;
- no-brake flag enabled for normal hybrid and RDMA+CXL hybrid comparisons.

Every RDMA+CXL result should include:

- total time and downtime;
- in-memory source precopy stall;
- postcopy handoff stall if available;
- region publish pages and time;
- region wait time;
- fault read/place/publish/total time;
- RDMA-ready, invalidated, and committed region/page counts;
- CXL fallback pages caused by RDMA invalidation.

## Deliverable Order

1. revised plan/spec committed in `hybrid-experiment-infra`;
2. isolated `rdma-cxl-parallel-hybrid` worktree from `hybrid-experiment-infra`;
3. measurement counters and parser fields;
4. source-authoritative RDMA ownership state;
5. bulk parallel prototype with no-brake default;
6. small benchmark report under `docs/superpowers/reports`.
