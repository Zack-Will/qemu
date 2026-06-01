# RDMA CXL Page-State Performance Review

Date: 2026-06-01
Branch: `rdma-cxl-parallel-hybrid`

## Scope

Reviewed the active page-state worker model against the Phase 2 requirement:
the migration main loop should act as control plane only. Bulk CXL/RDMA data
movement must run through workers, except for the CXL fault poller one-page
demand path.

## Baseline Evidence

Recent pre-optimization run:

```text
/tmp/cxl-hybrid-warm-exp-lq6pb7yz/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01
```

Key metrics:

- precopy: 4.121 ms
- postcopy: 10.137 ms
- `downtime-end -> postcopy-active`: 4.247 ms
- `completion-enter -> prepare-done`: 4.065 ms
- completion prepare begin: `cxl_low_depth=3328`
- old region publish requests/pages in postcopy handoff: 0 / 0
- page-state CAS failures: 0
- RDMA stale pages: 0

Root cause: postcopy warm push still called `cxl_hybrid_publish_page_to_cxl()`
from the migration thread. That path performed synchronous CXL page copies in
the control loop and masked worker queue tail by doing 1024 pages of main-thread
data work.

## Changes

- Routed postcopy warm push through `cxl_hybrid_ctrl_enqueue_cxl_page()`.
- For pages already visible through shared page-state, warm push now only syncs
  local derived bitmaps (`cxl_visible`, `dst_sent`, `remaining`, `warm_sent`).
- Increased CXL worker batch size from 256 to 1024 pages to match the bulk
  enqueue span and reduce tail drain overhead.

The old postcopy discard semantics remain intact: dirty/CXL-owned pages still
drive discard decisions through page-state, and old region completion publish is
not used during an active page-state source run.

## Results

Best post-optimization run:

```text
/tmp/cxl-hybrid-warm-exp-hzfsk8om/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01
```

- precopy: 3.317 ms
- postcopy: 6.146 ms
- `downtime-end -> postcopy-active`: 4.447 ms
- `completion-enter -> prepare-done`: 0.181 ms
- `postcopy-active`: `cxl_low_depth=0`, `active_cxl=0`
- completion prepare begin: `cxl_low_depth=0`, `active_cxl=0`
- CXL worker bytes/time: 50,860,032 bytes / 7.173 ms
- RDMA completed bytes/time: 33,554,432 bytes / 16.161 ms
- page-state CAS failures: 0
- RDMA stale pages: 0
- stream-write bytes: 0

Second post-optimization run:

```text
/tmp/cxl-hybrid-warm-exp-gpdxtzbv/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01
```

- precopy: 4.102 ms
- postcopy: 8.603 ms
- `downtime-end -> postcopy-active`: 2.205 ms
- `completion-enter -> prepare-done`: 4.406 ms
- active/completion tail had 2048 CXL low pages because destination package
  loaded earlier than the worker drained the queue.
- CXL worker bytes/time: 50,868,224 bytes / 7.098 ms
- page-state CAS failures: 0
- RDMA stale pages: 0

## Interpretation

The worker model is now the active data path for warm/postcopy bulk work. The
remaining postcopy spread is timing overlap between destination package loading
and CXL worker drain. When package loading takes longer, the worker drains before
`POSTCOPY_ACTIVE` and postcopy lands near 6 ms. When package loading is early,
the same worker work appears in completion prepare and postcopy lands around
8.6 ms.

No old region publish path or main-thread CXL bulk copy is needed for the
measured runs.
