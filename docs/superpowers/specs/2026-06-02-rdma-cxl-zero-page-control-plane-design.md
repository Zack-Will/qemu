# RDMA + CXL Zero-Page Control Plane Design

Date: 2026-06-02
Branch: `rdma-cxl-parallel-hybrid`
Status: design spec

Extends:

- `docs/superpowers/specs/2026-05-30-rdma-cxl-page-state-control-plane-design.md`

## Summary

The page-state model must treat zero pages as a first-class published
location. The main migration loop should detect zero pages before enqueuing
CXL or RDMA work. Fully zero regions should not be transferred. Partially zero
regions should use zero-page information to choose the CXL or RDMA lane, while
preserving each lane's placement semantics.

The intended behavior is:

- full-zero region: publish pages as `PUBLISHED@ZERO`, clear dirty bits, and
  enqueue no data work;
- partial-zero region sent by CXL: publish zero pages as `PUBLISHED@ZERO`,
  enqueue only non-zero pages to the CXL worker, and never materialize zero
  pages in CXL backing;
- partial-zero region sent by RDMA: send the full region to destination RAM and
  publish completed pages as `PUBLISHED@DST_LOCAL`, including pages that were
  zero at classification time;
- non-zero region: use the normal CXL/RDMA scheduling path.

This keeps CXL backing space proportional to useful non-zero data and avoids
polluting RDMA with sparse-page descriptors.

## Current Problem

The current branch has zero-page scaffolding but not an integrated zero-page
data path:

- `CXL_HYBRID_PAGE_LOCATION_ZERO` exists in page-state encoding.
- `cxl_hybrid_scheduler_choose_zero_page_lane()` always returns CXL, but the
  scheduler does not classify zero pages before CXL/RDMA enqueue.
- `save_zero_page()` can still detect zero pages in the legacy RAM path, but in
  CXL hybrid mapped-RAM mode it materializes a zero page in the mapped backing
  and then marks it visible as CXL.
- region-remap fault resolution currently rejects `PUBLISHED@ZERO`.

That means zero pages can consume CXL backing and worker time, while RDMA and
CXL scheduling cannot account for the real amount of data that needs transfer.

## Goals

- Detect zero pages in the main loop before CXL/RDMA enqueue.
- Make full-zero regions control-plane-only.
- Avoid writing zero pages to CXL backing.
- Keep RDMA simple: partial-zero RDMA regions are full-region writes to
  destination RAM and complete as `DST_LOCAL`.
- Let postcopy faults consume `PUBLISHED@ZERO` with the normal zero-page UFFD
  mechanism.
- Use zero-page density as an input to lane selection.
- Preserve the page-state generation and CAS rules used by CXL/RDMA workers.

## Non-Goals

- No sparse RDMA descriptor format.
- No CXL materialization of known zero pages.
- No optimization of the legacy non-CXL zero-page migration path.
- No change to the requirement that the main loop performs classification and
  enqueue only; bulk data movement remains in workers or RDMA sidecar.

## Classification Model

The main loop scans at fault-region granularity before calling the CXL or RDMA
bulk enqueue helpers.

For each candidate region, the classifier records:

- total pages in the region;
- dirty pages that still require action;
- zero dirty pages;
- non-zero dirty pages;
- effective CXL bytes: `non_zero_dirty_pages * TARGET_PAGE_SIZE`;
- effective RDMA bytes: `region_size` when any non-zero page is assigned to
  RDMA, because RDMA sends whole regions;
- whether every actionable dirty page in the region is zero.

Only dirty pages need classification. Clean or already published pages are not
new transfer work.

Zero detection must happen before RDMA claims pages. A full-zero region should
publish `ZERO` and clear the corresponding migration dirty bits without ever
creating an RDMA claim or CXL descriptor.

## Scheduling Policy

Lane selection should consider effective transfer cost:

- CXL cost is the number of non-zero dirty bytes.
- RDMA cost is the full region size.
- Full-zero cost is zero and bypasses both lanes.

For partial-zero regions, the scheduler may still choose RDMA if the RDMA lane
has capacity and the dynamic balance favors it. When RDMA is chosen, the whole
region is transferred and completed as `DST_LOCAL`.

When CXL is chosen, only non-zero pages are enqueued to the CXL worker. Zero
pages are published as `ZERO` immediately after successful page-state claim.

The initial implementation can use a conservative deterministic policy:

1. bypass full-zero regions;
2. treat partial-zero regions as lower CXL cost in the existing balance
   calculation;
3. keep RDMA full-region behavior unchanged;
4. expose counters so experiments can decide whether a more adaptive policy is
   needed.

## Page-State Semantics

Publishing a zero page follows the same generation rules as other locations:

```text
NOT_SENT or DIRTY
  --classify zero + claim current dirty seq-->
PUBLISHED(location=ZERO)
```

The publish operation must:

1. confirm the page is still zero after reading the guest page;
2. claim or transition the page state for the current generation;
3. publish the page as `PUBLISHED@ZERO` with release ordering;
4. update derived visible bitmaps;
5. clear the migration dirty bit only after the page-state transition succeeds.

If the page changes or the CAS fails, the scheduler leaves it dirty or returns
it to dirty state so a later scan can transfer it normally.

For RDMA partial-zero regions, pages do not publish as `ZERO`; RDMA completion
publishes claimed pages as `DST_LOCAL`.

For CXL partial-zero regions, zero pages publish as `ZERO`; non-zero pages are
claimed by CXL and later publish as `CXL`.

## Destination Fault Handling

When a postcopy fault sees `PUBLISHED@ZERO`, the destination resolves it with
the existing zero-page placement path:

- use `postcopy_place_page_zero()` or equivalent UFFD zero-page helper;
- wake the faulting vCPU after the zero page is installed;
- do not remap CXL backing for that page;
- do not wait for region-level CXL visibility.

For a region containing mixed `ZERO` and `CXL` pages, CXL remap is only valid
for contiguous spans of `PUBLISHED@CXL`. A fault on a zero page should resolve
as zero; a fault on a CXL page should remap the longest contiguous CXL span.

## Metrics

Add counters that distinguish zero-page behavior from CXL and RDMA byte counts:

- zero pages classified;
- full-zero regions bypassed;
- partial-zero regions;
- zero pages published;
- zero publish CAS failures;
- CXL zero pages skipped;
- effective CXL bytes after zero filtering;
- RDMA bytes sent for partial-zero regions;
- destination zero faults resolved.

These counters should make it clear when CXL backing savings are real and when
RDMA intentionally sent full regions that contained zero pages.

## Tests

Unit tests should cover:

- scheduler classifies a full-zero region as control-plane-only;
- full-zero publish marks pages `PUBLISHED@ZERO` and clears dirty bits only on
  successful page-state transition;
- partial-zero CXL enqueue skips zero pages and enqueues only non-zero pages;
- partial-zero RDMA claim keeps full-region behavior and completes pages as
  `DST_LOCAL`;
- destination region fault resolves `PUBLISHED@ZERO` without CXL remap;
- mixed `ZERO` and `CXL` region remap finds only contiguous CXL spans.

Experiment tests should assert that page-state snapshots can report non-zero
`published_zero` on a zero-heavy workload and that CXL worker bytes decrease
relative to an equivalent run without zero-page bypass.
