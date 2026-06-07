# RDMA + CXL Page-State Control Plane Redesign

Date: 2026-05-30
Branch: `rdma-cxl-parallel-hybrid`
Status: design spec and feasibility assessment

Supersedes the region-ownership and region-republish parts of:

- `docs/superpowers/specs/2026-05-23-rdma-cxl-parallel-hybrid-design.md`
- `docs/superpowers/specs/2026-05-26-rdma-cxl-real-sidecar-design.md`

It does not supersede the requirement that the RDMA sidecar must use real RDMA
verbs and an independent sidecar connection.

## Summary

The next architecture should make the CXL shared control area the only
cross-node truth for page migration state. RDMA regions remain useful only as
large transfer and prefetch descriptors. They must stop being ownership,
visibility, invalidation, or republish units.

The target model is:

- control plane: per-page state machine in shared CXL memory, updated with
  atomic compare-and-swap and release/acquire fences;
- scheduler: the main migration flow scans dirty state, classifies work, claims
  pages, and enqueues descriptors, but does not copy bulk page data;
- data plane: a CXL fault poller moves only the faulting page, CXL/RDMA workers
  consume lane-local queues, and all lanes complete pages back into the shared
  page state;
- RDMA target: destination RAM, matching the intended native-RDMA-like data
  placement, not CXL backing and not a new staging buffer;
- postcopy faults: the poller serves the faulting page immediately, then
  enqueues dirty pages in the containing region to the CXL worker high-priority
  queue for prefetch/backfill;
- remap: use linear GPA-to-DAX offsets so adjacent page remaps use contiguous
  file offsets and can be merged by the kernel.

## Current Architecture Problems

The current branch still treats region state as control state:

- `migration/cxl-region.c` stores RDMA candidate, in-flight, ready, stale,
  CXL-published, invalidated, republished, and committed bitmaps per region.
- `migration/ram.c:ram_save_host_page()` calls
  `cxl_hybrid_rdma_enqueue_bulk_region()` before normal CXL save, clears dirty
  bits for the whole region, and hands the claim to RDMA.
- RDMA completion calls `cxl_hybrid_mark_region_rdma_ready()`, so one RDMA
  completion affects a whole region.
- dirtying one page of an RDMA-ready region calls
  `cxl_hybrid_invalidate_region_rdma_ready()`, and CXL republish accounting is
  region-based.
- the CXL request worker in `migration/cxl-hybrid-control.c` dequeues fault
  requests and directly performs page or region publication without a per-page
  ownership arbiter.
- postcopy region mode still validates and waits for a complete region before
  remap.

That model explains the last experiment result: RDMA can remove too much work
from the CXL bulk path, region invalidation amplifies fallback work, and the
metrics cannot cleanly answer which pages were truly served by CXL, RDMA, or a
stale fallback.

## Goals

- Keep real RDMA as an independent secondary hardware data lane.
- Keep RDMA writes targeted at destination RAM.
- Make per-page CXL state the single arbiter for CXL and RDMA ownership.
- Remove the rule "RDMA region invalidated means whole region CXL republish".
- Serve stale or faulted pages on demand at page granularity.
- Keep region as an RDMA transfer or prefetch unit only.
- Keep the main migration flow as a scheduler/classifier, not a bulk memcpy
  path.
- Keep the CXL poller as the fault fast path and allow it to copy exactly the
  demanded page.
- Allow workers to exist during both precopy and postcopy.
- Allow urgent postcopy demand to preempt queued background/prefetch work.
- Use CXL worker high/low priority queues, with de-duplication guaranteed by
  the shared per-page state CAS rules.
- Use linear GPA-to-DAX offsets so remap fragmentation is transient and can
  converge as adjacent VMAs become identical.

## Non-Goals

- No memcpy or shadow-copy RDMA sidecar fallback.
- No conversion of hybrid migration into QEMU native RDMA migration.
- No region-wide CXL republish caused by one stale RDMA page.
- No cross-lane merge of CXL and RDMA work. Within one CXL worker transfer,
  adjacent pages from the same priority queue may be merged.
- No source-side brake policy work in this redesign.

## Shared CXL Page State

Add a versioned per-page state array to the existing shared CXL control region.
The existing visible bitmap and visible-region bitmap may remain as derived
compatibility accelerators during migration, but they must stop being the
authority for arbitration.

Each page has one atomic state word. The exact bit layout can be refined during
implementation, but it must encode:

- state: `NOT_SENT`, `IN_FLIGHT`, `PUBLISHED`, or `DIRTY`;
- owner when in flight: `CXL` or `RDMA`;
- location when published: `CXL`, `DST_LOCAL`, or `ZERO`;
- generation or epoch;
- dirty sequence captured at claim time;
- flags for `STALE`, `DEMAND`, `PREFETCHED`, and `REMAP_CANDIDATE`.

The important rule is that a page can be owned by only one lane at a time:

```text
NOT_SENT or DIRTY
  --CAS(owner=CXL or RDMA, captured_dirty_seq)-->
IN_FLIGHT(owner)
  --worker completion with matching seq + release fence-->
PUBLISHED(location)
  --source dirty event-->
DIRTY
```

If a page is dirtied while a transfer is in flight, the dirty path increments
the page dirty sequence or sets the stale flag. The worker completion may still
arrive, but it can only publish if the state word still matches the owner and
captured sequence it claimed. Otherwise it records a stale completion and does
not make the page visible.

Data publication order must be:

1. write page data to CXL, destination local RAM, or zero-page metadata;
2. issue release fence;
3. CAS or store-release the page state to `PUBLISHED(location)`;
4. optionally update derived visible bitmaps.

Consumers must load the state with acquire semantics before trusting the data.

## Scheduler

The main migration flow becomes the bulk scheduler. It is allowed to scan,
classify, perform atomic page claims, and enqueue descriptors. It is not allowed
to copy guest page bytes for bulk migration.

Precopy bulk scheduling:

- walk the dirty bitmap under the same locking discipline used today;
- identify zero pages before RDMA claiming, and publish them as `ZERO` instead
  of sending them over RDMA;
- CAS page state from `NOT_SENT` or `DIRTY` to `IN_FLIGHT`;
- enqueue lane-local descriptors;
- clear the migration dirty bit only after a page claim succeeds;
- if CAS fails, leave the page for the owner or a later scan.

Postcopy demand scheduling:

- a faulted page is always high priority;
- if the page is not already consumable, the CXL poller claims and copies
  exactly that page;
- after the fault page is published, enqueue dirty pages from the containing
  region into the CXL worker high-priority queue;
- the CXL worker may also consume low-priority background bulk descriptors;
- RDMA prefetch can still be queued for pages not already claimed or published,
  but a later CXL fault path can preempt queued RDMA work before RDMA posts it.

To keep the experiment aligned with the intended CXL+RDMA parallel hardware
benefit, the implementation should keep a configurable CXL background share for
bulk pages. A literal "all bulk to RDMA, only demand to CXL" policy would again
make the CXL lane idle during precopy and would not measure two PCIe paths
working in parallel.

## Data Lanes

### CXL Fault Poller

The CXL poller remains a dedicated fault fast path. It may perform data
movement, but only for the single demanded page. Its job is to minimize guest
stall, not to batch a whole region.

On each fault request, the poller:

1. reads the page state;
2. if the page is already consumable, wakes/remaps without copying;
3. if the page can be claimed by CXL, CASes it to `IN_FLIGHT(owner=CXL)` and
   copies exactly one page into the linear CXL backing offset;
4. publishes the page as `PUBLISHED@CXL`;
5. enqueues the rest of the dirty pages in the containing region to the CXL
   worker high-priority queue.

If the page is covered by an already-posted RDMA write to destination RAM, the
poller does not copy and wake the same local page. It waits for RDMA completion
or failure, then either consumes `PUBLISHED@DST_LOCAL` or requeues the page as
CXL demand.

### CXL Lane

The CXL worker should be a refactor near the current CXL poller/request-worker
shape, not a completely new thread model. Unlike the poller, it is allowed to
batch. It consumes page descriptors instead of making region-level control
decisions itself.

Use two queues:

- high priority: dirty pages from regions that recently had a postcopy fault;
- low priority: background bulk or prefetch pages.

The worker drains high priority first. It may keep a low-priority transfer in
progress to completion, but it should check the high-priority queue between
lane-local merged transfers. Queue de-duplication is optional; correctness
comes from the per-page CAS. Duplicate descriptors that fail CAS are dropped and
counted.

For adjacent pages in the same CXL lane queue, the worker may coalesce into one
contiguous memcpy into the linear DAX offset range. It must complete each page
state separately, or complete a range only if every page still has the matching
`IN_FLIGHT(owner=CXL, seq)` state.

The CXL lane is responsible for:

- fault-adjacent high-priority prefetch/backfill after the poller serves the
  demanded page;
- optional background bulk share;
- stale RDMA page fallback;
- pages that RDMA cannot claim because of budget, MR limits, or active fault
  priority.

### RDMA Lane

The RDMA worker keeps the current sidecar thread shape: a dedicated real RDMA
connection, an internal queue, a post loop, and a completion poll loop. The
change is what the thread consumes and completes. It consumes region
descriptors, but the descriptor contains a page ownership mask or page-span
list. Region is only the batching envelope.

The worker may merge adjacent owned pages inside the same descriptor into RDMA
writes. It must not merge across lanes, and it must not publish pages whose
state no longer matches the RDMA claim.

RDMA writes to destination RAM. That fixes the data placement decision and also
fixes the race rule: CXL demand may preempt RDMA work only while the page is
still queued and no RDMA work request covering that page has been posted. Once
the RDMA worker posts a write for a page, the page remains `IN_FLIGHT(owner=RDMA)`
until RDMA completion or failure. A fault on that page waits for the RDMA result
or for an explicit RDMA failure path to requeue it as CXL demand. CXL must not
publish and wake the same destination local page while a stale RDMA write can
still land there.

Fault injection must cover this posted-write race before postcopy RDMA prefetch
is enabled.

## Fault Handling

On destination fault:

1. compute the fault page index;
2. read the shared page state with acquire semantics;
3. if `PUBLISHED@CXL`, remap or place the page from CXL and wake the fault;
4. if `PUBLISHED@DST_LOCAL`, mark the local page received and wake the fault;
5. if `PUBLISHED@ZERO`, install or mark the zero page and wake the fault;
6. if `NOT_SENT`, `DIRTY`, `STALE`, or queued-but-not-consumable, let the CXL
   poller claim and copy exactly the faulting page;
7. after the faulting page is published, enqueue dirty pages in the containing
   region to the CXL worker high-priority queue;
8. optionally enqueue RDMA prefetch descriptors for nearby pages that are not
   demanded, not published, not already in flight, and not in the CXL
   high-priority queue.

The fault path must not request "publish this whole region via CXL" for RDMA
staleness. Region remains only a source for candidate pages; the poller copies
one page and the worker claims pages individually.

## Remap And GPA-to-DAX Mapping

The DAX data area should be laid out so guest page `N` maps to a stable linear
DAX offset for the RAMBlock:

```text
cxl_offset = ramblock->pages_offset + ramblock_page_offset
```

The control region must stay outside the RAMBlock data range so it does not
create holes inside guest RAM backing. For adjacent guest pages, the file
offsets, protection, and mapping flags should also be adjacent and identical.
That allows Linux to merge adjacent VMAs after page/subrange remap.

Remap policy changes:

- remap is no longer required at region granularity;
- destination remap is speculative and span-based: on each remap attempt, scan
  left and right from the fault page and choose the longest contiguous valid
  CXL-backed span;
- the span is not fixed to one region and is not fixed to one page;
- only hot pages or hot contiguous spans need CXL remap;
- background RDMA pages published to destination local RAM do not require CXL
  remap;
- fragmentation is acceptable only as a temporary state and must converge when
  adjacent pages share the same file, offset continuity, protection, and flags.

A page is valid for inclusion in a speculative remap span only if:

- its page state is current and `PUBLISHED@CXL`;
- its DAX offset is contiguous with the previous page;
- it is not dirty, stale, or in flight;
- it belongs to the same RAMBlock and maps with the same protection and flags.

The span scan should stop at the first invalid page on each side. The first
implementation can add a configurable maximum scan window if needed to protect
fault latency, but correctness must not depend on a region boundary.

The experiment should measure VMA count and `/proc/<qemu-pid>/maps` convergence
after a postcopy run with repeated hot-page remaps.

## What To Remove

Remove or demote these region-control concepts:

- `CXLHybridRDMASidecarState` as an authoritative region ownership state;
- `candidate_bmap`, `inflight_bmap`, `ready_bmap`, `stale_bmap`,
  `cxl_published_bmap`, `invalidated_bmap`, `republished_bmap`, and
  `committed_bmap` as correctness state;
- `cxl_hybrid_region_try_own_rdma()` and `cxl_hybrid_region_try_own_cxl()` as
  arbitration points;
- `cxl_hybrid_invalidate_region_rdma_ready()` as the dirty invalidation unit;
- `cxl_hybrid_region_note_cxl_republish()` and
  `cxl_republish_*_due_to_rdma_invalidate` semantics;
- `cxl_hybrid_commit_rdma_ready_regions_for_postcopy()` as a region scan;
- the source request-worker behavior that independently decides region
  publication. The poller/worker thread shapes may remain, but the poller must
  copy only the fault page and the worker must consume per-page descriptors;
- complete-region validation for normal postcopy demand service.

Derived region counters may remain for reporting RDMA transfer efficiency, but
they must not drive correctness.

## What To Add

Add these units:

- shared per-page state layout and helpers, preferably in a focused
  `migration/cxl-page-state.c` unit with pure tests;
- page claim/complete/dirty/stale APIs:
  `try_claim_page(owner)`, `complete_page(owner, location)`,
  `mark_page_dirty()`, `mark_page_stale()`, and `page_can_consume()`;
- scheduler queues with priority classes:
  `CXL_HIGH`, `CXL_LOW`, `RDMA_BULK`, and `RDMA_PREFETCH`;
- a CXL poller fast path for single-page fault service;
- CXL worker high/low priority queues, reusing the current poller-like worker
  shape where possible;
- worker lifecycle shared by precopy and postcopy, reusing the current RDMA
  sidecar thread shape where possible;
- RDMA descriptors that contain region base plus owned page spans or a page
  mask;
- CXL descriptors that contain page spans and demand/fallback reason;
- explicit handling for zero pages as a published location or skipped transfer;
- metrics for per-lane claimed bytes, completed bytes, stale pages, CAS
  failures, demand latency, worker queue depth, zero-page skips, RDMA posted
  bytes, and CXL memcpy bytes.

## What To Modify

- `migration/ram.c`: convert the bulk scan from copy/claim-and-clear to
  classify/claim/enqueue. The main path should stop calling RDMA region claim
  before CXL has a chance to classify pages.
- `migration/cxl-hybrid-control.c`: extend the shared control layout and split
  the current request-worker behavior into a CXL poller fast path plus CXL
  worker queues. The poller may copy the fault page immediately; the worker
  consumes per-page high/low priority descriptors and updates page state.
- `migration/cxl-hybrid-control-header.c`: add pure helpers for the page state
  word, including CAS transitions and release/acquire publication helpers.
- `migration/cxl.c`: change fault publish paths from page-or-region copy
  functions into CXL worker descriptor creation and page-state completion.
- `migration/cxl-rdma.c`: keep the real RDMA connection and sidecar thread
  structure, but consume scheduler descriptors and complete page states, not
  region-ready state. The remote MR should be destination RAM.
- `migration/cxl-region.c`: keep geometry helpers; remove region ownership as
  the control plane.
- `scripts/cxl-hybrid-warm-experiment.py`: report lane-balanced data movement
  from page-state metrics rather than region republish counters.
- `tests/unit/test-cxl-hybrid-control.c` and
  `tests/unit/test-cxl-hybrid-region.c`: replace region invalidation tests with
  page-state transition and race tests.

## Metrics And Acceptance Criteria

The redesign is successful when:

- no normal hybrid bulk or demand path performs page memcpy in the main
  migration thread;
- the CXL poller copies at most the faulting page before waking the fault;
- fault-adjacent dirty pages are served by the CXL worker high-priority queue;
- a stale RDMA page causes one-page CXL demand service, not whole-region CXL
  republish;
- RDMA completed bytes are real RDMA verbs completions;
- CXL completed bytes are measured from the CXL worker memcpy path;
- zero pages are not counted as RDMA data bytes unless explicitly configured;
- CXL and RDMA lanes both carry non-zero physical bytes in the balanced bulk
  experiment;
- postcopy P95/P99 demand wait does not regress against current CXL-only
  postcopy;
- RDMA completion after dirtying is counted stale and cannot make a page
  consumable;
- CXL demand cannot wake a destination local page while an RDMA write covering
  that page is already posted;
- VMA count does not grow without bound after page/subrange CXL remap;
- destination remap chooses the longest contiguous valid CXL span around the
  fault page instead of a fixed region or fixed single page;
- removing region republish counters does not remove observability: per-page
  stale and fallback counters replace them.

## Feasibility Assessment

This redesign is feasible, but it is a major refactor rather than a small patch.
The current tree already has useful foundations:

- a shared CXL control mapping;
- generation checks;
- release/acquire bitmap publication patterns;
- a request ring;
- real RDMA sidecar connection setup;
- mapped CXL data backing;
- enough tests around control header and region geometry to extend.

The highest-risk parts are:

- correctness of atomic CAS and fences on the shared CXL mapping;
- dirty/re-dirty races between bitmap scan, page-state claim, worker completion,
  and postcopy switch;
- RDMA writes to destination RAM racing with CXL demand and destination wakeup;
- maintaining low postcopy demand latency while the old request poller is
  refactored into a one-page poller plus CXL worker queues;
- avoiding a new imbalance where all precopy bulk goes to RDMA and CXL becomes
  demand-only;
- bounding speculative remap-span scans so they do not add tail latency;
- preserving QEMU migration lifecycle cleanup when workers persist across
  precopy and postcopy;
- proving that linear DAX offsets actually allow VMA merge on the test kernel.

The RDMA destination target is destination RAM. This avoids adding a staging
copy and keeps the design close to the current sidecar/native-RDMA placement,
but it makes the posted-write race rule mandatory: after a page is covered by a
posted RDMA write, CXL demand must wait for RDMA completion/failure rather than
waking that same local page independently.

## Suggested Implementation Phases

### Phase 1: Page State Shadow Mode

Add the shared page-state layout and pure transition tests. Mirror existing CXL
visibility into the new page state without changing data movement.

Exit criteria:

- unit tests cover claim, complete, dirty, stale, generation mismatch, and
  release/acquire publication;
- query-migrate exposes shadow page-state counters;
- current experiments are behaviorally unchanged.

### Phase 2: Scheduler Queues Without Data Movement Changes

Add scheduler-owned queues and make the dirty scan classify pages into shadow
descriptors while the old data path still sends data.

Exit criteria:

- descriptors match dirty bitmap coverage;
- zero pages are classified before RDMA;
- CAS failure and lane selection metrics are stable.

### Phase 3: CXL Poller And Worker Data Path

Split CXL data movement into a poller fast path and worker queues. The poller
copies only the fault page. The worker handles high-priority region-dirty pages
and low-priority background descriptors.

Exit criteria:

- main migration thread does not memcpy CXL page data in the worker-enabled
  path;
- poller copies one page per fault request and never publishes a whole region;
- CXL worker high-priority queue receives dirty pages from the fault region;
- postcopy demand latency is no worse than current CXL-only baseline;
- CXL worker bytes/time replace ambiguous backing-write accounting.

### Phase 4: RDMA Worker On Page Claims

Change RDMA from region ownership to region descriptors with per-page ownership
masks. RDMA completion marks only pages whose state still matches the claim.

Exit criteria:

- no region-ready or region-invalidated correctness state remains;
- dirtying one page invalidates only that page;
- stale RDMA completions cannot publish pages;
- RDMA zero-page bytes disappear or are separately accounted.

### Phase 5: Fault Prefetch And Priority

Use faulted pages as poller demand and use containing regions as CXL
high-priority page candidates plus RDMA prefetch hints. Demand can preempt
queued prefetch before RDMA posts it.

Exit criteria:

- a postcopy fault on an RDMA-stale page causes one-page CXL demand;
- pending RDMA prefetch never blocks the CXL poller;
- CXL worker high-priority work outranks low-priority background work;
- queued RDMA prefetch can be preempted before post;
- posted RDMA pages make the fault wait for RDMA completion/failure;
- posted RDMA race rule is tested with fault injection.

### Phase 6: Linear And Speculative Remap Policy

Remove the assumption that remap must be region-sized. Use the linear
GPA-to-DAX offset mapping for hot pages or hot spans, and remap the longest
valid contiguous CXL-backed span around a fault.

Exit criteria:

- adjacent remapped pages have contiguous DAX offsets;
- remap attempts are not fixed to region size or single-page size;
- invalid pages stop span growth on each side;
- `/proc/<pid>/maps` shows VMA merging or bounded fragmentation;
- remap success/failure metrics are per span/page rather than only per region.

### Phase 7: Cleanup And Experiment Gate

Remove region republish metrics and old region ownership APIs. Update scripts
and reports to use page-state and lane metrics.

Exit criteria:

- old `cxl_republish_*_due_to_rdma_invalidate` fields are gone or marked
  historical;
- reports compute CXL bandwidth from CXL worker bytes/time and RDMA bandwidth
  from RDMA completion bytes/time;
- balanced bulk experiment shows both lanes active and explains any imbalance.

## Recommendation

Proceed with this redesign, but treat it as a new task sequence. The current
real RDMA sidecar work remains valuable for connection setup and verbs posting,
but the region ownership layer should be removed instead of extended.

The implementation plan should preserve the current thread shapes where they
fit: CXL poller as the one-page fault fast path, CXL worker as high/low priority
queue consumers, and RDMA worker as the current sidecar thread with page-state
completion. The main structural change is the control authority and granularity,
not the existence of the worker threads.
