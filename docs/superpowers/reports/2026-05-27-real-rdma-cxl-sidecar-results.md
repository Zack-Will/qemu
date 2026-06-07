# Real RDMA CXL Sidecar Results

## Configuration

- Host date: 2026-05-28
- Worktree: `/home/xiexinchen/.config/superpowers/worktrees/qemu/rdma-cxl-parallel-hybrid`
- Main migration URI: `unix:<runtime>/mig.sock`
- Sidecar RDMA endpoint: `rdma:10.0.0.2:<port>`
- RDMA device: `mlx5_0`, link `ACTIVE`, netdev `ens2f0np0`, IP `10.0.0.2/24`
- RDMA CM sanity: `rping -s/-c -a 10.0.0.2 -p 7471 -C 1 -v` succeeded
- rdma-pin-all: `true`
- cxl-path: `/dev/dax0.1`
- workload: `remap_xlarge_random_rw`

## Results

| run | mode | final status | total ms | precopy ms | postcopy ms | QMP tail ms | CXL publish pages | CXL publish time ns | RDMA posted bytes | RDMA completed bytes | stale regions | invalidated regions |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| CXL-only throttled control | hybrid_postcopy_auto | completed | 984 | 876.037 | 100.881 | 41.541 | 6379 | 9790829 | 0 | 0 | 0 | 0 |
| default sidecar smoke | hybrid_parallel_rdma_cxl | postcopy-active, dst SIGSEGV during cleanup | 39 | n/a | n/a | n/a | 16118 | 6905724 | 0 | 0 | 0 | 0 |
| throttled sidecar dataplane proof | hybrid_parallel_rdma_cxl | postcopy-active, dst SIGSEGV during cleanup | 937 | n/a | n/a | n/a | 6930 | 4662848 | 67108864 | 67108864 | 1 | 18 |
| throttled sidecar after CQ cleanup fix | hybrid_parallel_rdma_cxl | completed | 928 | 875.954 | 45.746 | 42.577 | 7135 | 5259586 | 65011712 | 65011712 | 0 | 18 |

## Optimized Setup Experiment

After moving source pin-all registration into sidecar setup and waiting for source/destination setup before continuing migration, the same throttled parameters were rerun on the real RoCE device. Completed samples only:

| sample | mode | total ms | precopy ms | postcopy ms | QMP tail ms | setup ms | active-to-completion ms | completion ms | region wait ms | region publish ms | RDMA completed regions | RDMA completed bytes | RDMA connect ms | stale regions | CXL race lost | invalidated regions | republish regions | republish pages |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `/tmp/cxl-hybrid-warm-exp-4zhd_rou` | hybrid_postcopy_auto | 990 | 876.036 | 104.253 | 19.604 | 9 | 97.628 | 0.605 | 10.220 | 9.862 | 0 | 0 | 0.000 | 0 | 0 | 0 | 0 | 0 |
| `/tmp/cxl-hybrid-warm-exp-68llmiwi` | hybrid_postcopy_auto | 981 | 876.065 | 98.366 | 42.923 | 6 | 91.949 | 0.543 | 9.678 | 9.181 | 0 | 0 | 0.000 | 0 | 0 | 0 | 0 | 0 |
| `/tmp/cxl-hybrid-warm-exp-h2flext6` run01 | hybrid_postcopy_auto | 980 | 876.245 | 96.966 | 45.001 | 6 | 90.569 | 0.354 | 8.449 | 8.264 | 0 | 0 | 0.000 | 0 | 0 | 0 | 0 | 0 |
| `/tmp/cxl-hybrid-warm-exp-h2flext6` run02 | hybrid_postcopy_auto | 942 | 875.786 | 58.496 | 43.341 | 7 | 53.873 | 0.320 | 7.519 | 7.105 | 0 | 0 | 0.000 | 0 | 0 | 0 | 0 | 0 |
| `/tmp/cxl-hybrid-warm-exp-h2flext6` run03 | hybrid_postcopy_auto | 948 | 875.758 | 64.447 | 41.252 | 7 | 60.000 | 0.436 | 6.926 | 6.689 | 0 | 0 | 0.000 | 0 | 0 | 0 | 0 | 0 |
| `/tmp/cxl-hybrid-warm-exp-4zhd_rou` | hybrid_parallel_rdma_cxl | 939 | 875.707 | 28.891 | 57.228 | 34 | 17.304 | 6.734 | 3.755 | 3.995 | 31 | 65011712 | 27.195 | 0 | 0 | 18 | 14 | 7168 |
| `/tmp/cxl-hybrid-warm-exp-68llmiwi` | hybrid_parallel_rdma_cxl | 937 | 875.755 | 24.685 | 60.955 | 35 | 15.710 | 4.100 | 3.145 | 3.298 | 32 | 67108864 | 28.775 | 1 | 1 | 18 | 14 | 7168 |
| `/tmp/cxl-hybrid-warm-exp-h2flext6` run03 | hybrid_parallel_rdma_cxl | 963 | 875.748 | 40.529 | 50.250 | 46 | 32.008 | 3.718 | 3.853 | 4.384 | 31 | 65011712 | 40.019 | 0 | 0 | 18 | 14 | 7168 |

Aggregate over completed samples:

| mode | samples | total ms mean | precopy ms mean | postcopy ms mean | QMP tail ms mean | region wait ms mean | region publish ms mean | RDMA completed bytes mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| hybrid_postcopy_auto | 5 | 968.2 | 875.978 | 84.506 | 38.424 | 8.558 | 8.220 | 0 |
| hybrid_parallel_rdma_cxl | 3 | 946.3 | 875.736 | 31.368 | 56.144 | 3.584 | 3.892 | 65710763 |

Rejected setup samples, excluded from performance aggregates:

| case | port | final status | stderr |
| --- | ---: | --- | --- |
| `/tmp/cxl-hybrid-warm-exp-h2flext6/.../hybrid_parallel_rdma_cxl-custom-run01` | 7480 | setup failure | `RDMA sidecar expected CM event RDMA_CM_EVENT_ESTABLISHED, got RDMA_CM_EVENT_REJECTED` |
| `/tmp/cxl-hybrid-warm-exp-h2flext6/.../hybrid_parallel_rdma_cxl-custom-run02` | 7481 | setup failure | `RDMA sidecar expected CM event RDMA_CM_EVENT_ESTABLISHED, got RDMA_CM_EVENT_REJECTED` |
| `/tmp/cxl-hybrid-warm-exp-vpfudx09/.../hybrid_parallel_rdma_cxl-custom-run01` | 7480 | setup failure | `RDMA sidecar expected CM event RDMA_CM_EVENT_ESTABLISHED, got RDMA_CM_EVENT_REJECTED` |

## No CXL Backing Rate Limit Experiment

The throttled experiments above used `--x-cxl-backing-rate 1048576` to stretch the bulk window. The following run removed that parameter while keeping the other knobs unchanged.

| sample | mode | total ms | precopy ms | postcopy ms | QMP tail ms | setup ms | switch reason | backing sleep count | CXL backing write bytes | CXL backing write ms | region publish pages | region publish ms | RDMA connect ms | RDMA completed bytes | RDMA no-candidate events |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `/tmp/cxl-hybrid-warm-exp-w1h4p4bg` | hybrid_postcopy_auto | 34 | 15.356 | 12.168 | 49.314 | 6 | precopy-complete | 0 | 20520960 | 13.115 | 16288 | 6.528 | 0.000 | 0 | 0 |
| `/tmp/cxl-hybrid-warm-exp-w1h4p4bg` | hybrid_parallel_rdma_cxl | 62 | 11.179 | 13.834 | 47.989 | 36 | precopy-complete | 0 | 20631552 | 12.303 | 16154 | 7.244 | 27.376 | 0 | 2 |
| `/tmp/cxl-hybrid-warm-exp-l0ey53x1` run01 | hybrid_parallel_rdma_cxl | 64 | 10.946 | 16.555 | 30.787 | 36 | precopy-complete | 0 | 19865600 | 10.511 | 16128 | 8.879 | 30.000 | 0 | 2 |
| `/tmp/cxl-hybrid-warm-exp-l0ey53x1` run02 | hybrid_parallel_rdma_cxl | 55 | 8.838 | 11.529 | 17.684 | 34 | precopy-complete | 0 | 20471808 | 9.174 | 16370 | 6.939 | 26.397 | 0 | 2 |
| `/tmp/cxl-hybrid-warm-exp-l0ey53x1` run03 | hybrid_parallel_rdma_cxl | 60 | 11.631 | 12.382 | 41.602 | 35 | precopy-complete | 0 | 20566016 | 11.197 | 16180 | 6.985 | 27.567 | 0 | 2 |

Aggregate over completed no-limit samples:

| mode | samples | total ms mean | precopy ms mean | postcopy ms mean | setup ms mean | CXL backing write ms mean | region publish ms mean | RDMA completed bytes mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| hybrid_postcopy_auto | 1 | 34.0 | 15.356 | 12.168 | 6.0 | 13.115 | 6.528 | 0 |
| hybrid_parallel_rdma_cxl | 4 | 60.3 | 10.649 | 13.575 | 35.3 | 10.796 | 7.512 | 0 |

## No-Limit Bulk Parallel Fix Experiment

The earlier no-limit run showed that RDMA setup was real but the dataplane did
not participate in bulk. Two implementation issues were fixed before rerunning:

- The source sidecar worker now keeps multiple RDMA writes in flight instead of
  posting one region and synchronously waiting for its completion.
- `x-cxl-rdma-sidecar-max-inflight-regions` now controls only the transport
  in-flight window. Total RDMA coverage is controlled by
  `x-cxl-rdma-sidecar-max-cover-percent`; previously the in-flight value also
  capped total accepted regions, so `8` prevented the configured 50% coverage
  from being reached.

The following no-limit runs used:

- `--max-bandwidth 0`
- no `--x-cxl-backing-rate`
- `--x-cxl-rdma-sidecar-max-inflight-regions 8`
- `--x-cxl-rdma-sidecar-max-cover-percent 50`
- `--rdma-pin-all`

| sample | mode | total ms | setup ms | precopy/bulk ms | postcopy ms | QMP tail ms | RDMA completed regions | RDMA completed bytes | region publish pages | region publish ms | RDMA invalidated regions | CXL republish regions | CXL republish pages |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `/tmp/cxl-hybrid-warm-exp-959iejou` | hybrid_postcopy_auto | 35 | 6 | 16.541 | 12.162 | 39.086 | 0 | 0 | 16163 | 6.194 | 0 | 0 | 0 |
| `/tmp/cxl-hybrid-warm-exp-959iejou` | hybrid_parallel_rdma_cxl, queued claim baseline | 52 | 30 | 7.440 | 13.160 | 23.022 | 7 | 14680064 | 16409 | 7.236 | 7 | 7 | 3584 |
| `/tmp/cxl-hybrid-warm-exp-s0gwney6` | hybrid_postcopy_auto | 34 | 8 | 12.942 | 11.869 | 31.417 | 0 | 0 | 16260 | 6.322 | 0 | 0 | 0 |
| `/tmp/cxl-hybrid-warm-exp-s0gwney6` | hybrid_parallel_rdma_cxl, multi-inflight only | 60 | 40 | 6.522 | 12.952 | 50.389 | 8 | 16777216 | 16108 | 7.154 | 8 | 8 | 4096 |
| `/tmp/cxl-hybrid-warm-exp-_2c6bl7d` | hybrid_postcopy_auto | 35 | 6 | 13.601 | 14.206 | 32.864 | 0 | 0 | 16197 | 7.368 | 0 | 0 | 0 |
| `/tmp/cxl-hybrid-warm-exp-_2c6bl7d` | hybrid_parallel_rdma_cxl, multi-inflight plus coverage fix | 51 | 35 | 3.798 | 10.496 | 22.125 | 16 | 33554432 | 13806 | 5.560 | 11 | 11 | 5632 |

Interpretation:

- Native-style RDMA setup still appears in total/setup time, which is expected
  and is not the main acceptance metric for this phase.
- The key bulk metric now moves in the expected direction without artificial CXL
  throttling. In the final paired run, CXL-only precopy/bulk was 13.601 ms while
  RDMA+CXL was 3.798 ms.
- RDMA posted and completed 16 whole 2 MiB regions, 32 MiB total, with
  `rdma_sidecar_failed=false`.
- Postcopy did not regress. In the final pair it was 14.206 ms for CXL-only and
  10.496 ms for RDMA+CXL.
- Region publish work fell from 16197 pages / 7.368 ms to 13806 pages /
  5.560 ms. This matches the expected dataplane split: CXL remains the primary
  stream, while RDMA removes a large part of bulk data movement.
- RDMA invalidation is visible and safe: 11 completed RDMA-ready regions were
  invalidated before commit and republished by CXL, covering 5632 pages. These
  invalidations are fallback correctness work, not RDMA transport failure.

Case directories:

- default sidecar smoke: `/tmp/cxl-hybrid-warm-exp-b8zj53d4/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01`
- throttled sidecar dataplane proof: `/tmp/cxl-hybrid-warm-exp-a31tfp5p/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01`
- CXL-only throttled control: `/tmp/cxl-hybrid-warm-exp-rtg0pg7f/remap_xlarge_random_rw/hybrid_postcopy_auto-custom-run01`
- throttled sidecar after CQ cleanup fix: `/tmp/cxl-hybrid-warm-exp-nu710cz3/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01`
- optimized setup comparison 1: `/tmp/cxl-hybrid-warm-exp-4zhd_rou/remap_xlarge_random_rw`
- optimized setup comparison 2: `/tmp/cxl-hybrid-warm-exp-68llmiwi/remap_xlarge_random_rw`
- optimized setup repeat/diagnostics: `/tmp/cxl-hybrid-warm-exp-h2flext6/remap_xlarge_random_rw`
- optimized setup rejected-port repro: `/tmp/cxl-hybrid-warm-exp-vpfudx09/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01`
- no backing rate limit comparison: `/tmp/cxl-hybrid-warm-exp-w1h4p4bg/remap_xlarge_random_rw`
- no backing rate limit hybrid repeat: `/tmp/cxl-hybrid-warm-exp-l0ey53x1/remap_xlarge_random_rw`
- no-limit queued-claim baseline: `/tmp/cxl-hybrid-warm-exp-959iejou/remap_xlarge_random_rw`
- no-limit multi-inflight only: `/tmp/cxl-hybrid-warm-exp-s0gwney6/remap_xlarge_random_rw`
- no-limit multi-inflight plus coverage fix: `/tmp/cxl-hybrid-warm-exp-_2c6bl7d/remap_xlarge_random_rw`

## Analysis

- The real RDMA CM/verbs path works on this host. The throttled sidecar run posted and completed 32 whole 2 MiB regions, 64 MiB total, with `rdma_sidecar_failed=false` and no failed regions.
- The default sidecar parameters did not post RDMA work. It connected successfully (`rdma_sidecar_connect_time_ns=13324613`), but the 16 MiB workload and fast CXL publish path left no candidate region by the time the source sidecar could schedule work.
- The throttled dataplane proof used `--x-cxl-backing-rate 1048576`, `--x-cxl-switch-max-precopy-ms 100`, `--x-cxl-rdma-sidecar-max-inflight-regions 8`, and `--x-cxl-rdma-sidecar-max-cover-percent 50`. This intentionally made the CXL bulk window long enough for the RDMA connection/setup cost to be amortized.
- The same throttled CXL-only control completed normally. The original throttled sidecar run reached `ram.remaining=0` but did not report `completed`; diagnostics show destination return code `-11` and `cleanup_range: userfault unregister Invalid argument`. A later core-based debug run showed both source and destination could fault in `__ibv_destroy_cq_1_1()` from `cxl_rdma_sidecar_cleanup()`.
- The sidecar cleanup crash root cause was RDMA verbs resource lifetime, not CXL postcopy republish. The sidecar destroyed the QP/CQ without first disconnecting the RDMA CM connection, and it also overwrote `cm_id->send_cq`, `cm_id->recv_cq`, and completion-channel fields after `rdma_create_qp()`. The fix now mirrors QEMU native RDMA more closely: mark the sidecar connected after `RDMA_CM_EVENT_ESTABLISHED`, call `rdma_disconnect()` before tearing down verbs resources, and do not overwrite the CM id CQ/channel fields.
- The fixed throttled sidecar run completed end to end with no RDMA sidecar failures: total 928 ms, precopy 875.954 ms, postcopy 45.746 ms, and QMP tail 42.577 ms. It posted and completed 31 whole 2 MiB RDMA regions, 65,011,712 bytes total.
- RDMA invalidation behavior is visible in the throttled sidecar run: 31 ready regions, 18 invalidated regions, 9216 ready pages lost, 14 CXL republish regions, and 7168 CXL republish pages. This is the expected safety behavior when regions completed by RDMA are dirtied or otherwise superseded before final commit; CXL remains authoritative for destination visibility.
- CXL remained active in the fixed sidecar run: CXL published 7135 pages while RDMA completed about 62 MiB. This matches the intended architecture of RDMA as a secondary bulk stream, not as the main migration transport.
- The optimized setup experiments show that the pre-copy window is still dominated by the explicit `x-cxl-switch-max-precopy-ms=100` policy and the throttled backing rate, so the RDMA sidecar does not materially change `precopy_time_ms`: CXL-only mean 875.978 ms, RDMA+CXL mean 875.736 ms.
- The main win appears after the postcopy transition. Completed RDMA+CXL samples reduced mean `postcopy_time_ms` from 84.506 ms to 31.368 ms, mean destination region wait from 8.558 ms to 3.584 ms, and mean region publish time from 8.220 ms to 3.892 ms.
- RDMA setup is now paid before the sidecar enters the bulk loop. The completed RDMA+CXL runs reported 27.195-40.019 ms of sidecar connect/setup time and 79,695,872 registered bytes with `rdma-pin-all`.
- `guest_latency.total_stall_during_migration_ms` is 0.0 in these runs, but this run shape uses `--in-memory-guest-latency --in-memory-guest-latency-source-first`, so normal heartbeat-based guest stall events are disabled. The source-side in-memory log is valid and captured the precopy window, but epoch markers were 0 and postcopy/completion sub-windows had no samples. The reliable guest signal from this run is therefore the source-side precopy excess time, not a full guest stall breakdown.
- The optimized setup also exposed an RDMA CM stability issue: ports 7480 and 7481 repeatedly failed before data movement with `RDMA_CM_EVENT_REJECTED`, while `rping` on the same ports succeeded and ports 7475, 7482, and 7490 completed QEMU sidecar runs. This is not included in performance averages; it needs separate diagnosis with sidecar connect/register/failed trace events enabled.
- With `--x-cxl-backing-rate` removed, CXL completes the bulk work before the RDMA sidecar can claim a whole dirty region. The sidecar still connects and registers memory (`rdma_sidecar_connect_time_ns` around 26.4-30.0 ms and registered bytes 79,695,872), but `rdma_sidecar_posted_bytes` and `rdma_sidecar_completed_bytes` are zero across all four no-limit hybrid samples.
- The no-limit comparison therefore does not show useful RDMA/CXL dataplane parallelism. CXL-only finished in 34 ms in the single paired run, while hybrid finished in 55-64 ms because it paid 34-36 ms of RDMA setup without moving data. This confirms that the earlier RDMA bulk contribution depended on the artificial CXL backing rate limit.
- The no-limit bulk parallel fix changes that conclusion for the current code.
  After the source worker was changed to keep multiple WRs in flight and the
  coverage cap was separated from the in-flight window, the final no-limit run
  completed 16 RDMA regions and reduced the bulk/precopy window from 13.601 ms
  to 3.798 ms. This is the expected phase result: total time still includes
  RDMA setup, but bulk time decreases because real RDMA and CXL now move data in
  parallel.

## Postcopy Fault Timeline Diagnostic

Diagnostic tracepoints were added for the asynchronous CXL worker:

- `cxl_hybrid_cxl_worker_enqueue`: page, generation, lane class, timestamp.
- `cxl_hybrid_cxl_worker_complete`: page, generation, bytes copied, elapsed
  time, copy return code, and whether the page-state CAS published the page.

The experiment parser now reports worker enqueue/complete pages, copied bytes,
copy time, published pages, stale pages, and failed pages. In-memory guest
latency collection also falls back to the source QMP dump when the destination
dump succeeds but contains an invalid all-zero header.

Successful diagnostic run:
`/tmp/cxl-hybrid-warm-exp-qs942xw3/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01`

Parameters matched the no-limit parallel run:

- `--max-bandwidth 0`
- `--x-cxl-switch-max-precopy-ms 1`
- `--x-cxl-rdma-sidecar-max-inflight-regions 8`
- `--x-cxl-rdma-sidecar-max-cover-percent 50`
- `--rdma-pin-all`
- `--in-memory-guest-latency`

Key results:

| metric | value |
| --- | ---: |
| total time | 118 ms |
| setup time | 60 ms |
| precopy time | 45.519 ms |
| postcopy time | 11.536 ms |
| bulk iterate time | 45.201 ms |
| bulk pages scanned | 16514 |
| postcopy-active iterate time | 2.716 ms |
| postcopy-active pages scanned | 4257 |
| RDMA completed | 16 regions / 32 MiB |
| CXL worker copied | 8320 pages / 32.5 MiB |
| CXL worker published | 8320 pages |
| CXL worker stale/failed pages | 0 / 0 |
| completion region scans | 32 regions / 16384 pages |
| completion region published pages | 0 |
| postcopy fault requests | 1 |

Bandwidth interpretation for the bulk iterate window:

| path | bytes | timing basis | bandwidth |
| --- | ---: | --- | ---: |
| total bulk data | 64.51 MiB | 45.201 ms wall time | 1427 MiB/s |
| CXL lane contribution | 32.5 MiB | same bulk wall time | 719 MiB/s |
| RDMA lane contribution | 32 MiB | same bulk wall time | 708 MiB/s |
| CXL worker inner copy | 32.5 MiB | QMP copy time 7.098 ms | 4579 MiB/s |
| CXL worker trace sum | 32.5 MiB | per-page process sum 11.058 ms | 2939 MiB/s |

Timeline evidence:

- Last CXL worker completion happened at `1314142793583564 ns`.
- Source entered postcopy-active at `1314142798617860 ns`, so all CXL worker
  pages were published about 5.034 ms before postcopy-active.
- Destination VM started at `1314142798993030 ns`; the last CXL worker
  completion was about 5.409 ms before VM start.
- The only postcopy fault was for offset `0x2800000`, before VM start:
  request at `1314142798942056 ns`, VM start at `1314142798993030 ns`.
- That page (`page=10240`) had already been published by the CXL worker at
  `1314142776741579 ns`, about 22.200 ms before the fault.
- Completion prepare scanned all 32 regions but published 0 pages. The earlier
  dirty set had already been made visible by the worker/RDMA paths and the
  postcopy-active RAM iteration.

Interpretation:

- The postcopy stage did not rely on completion prepare to republish dirty
  pages. Completion prepare was a validation scan in this run.
- The CXL worker published all of its pages before source postcopy-active and
  before destination VM start. There were 0 CXL worker pages published during or
  after postcopy-active.
- The one observed fault was not evidence of guest workload stalling after
  resume. It happened before `dst-postcopy-bh-vm-started` and hit a page that
  was already visible in CXL, so fault service was only a demand placement from
  CXL-visible backing into destination RAM.
- Destination in-memory latency dump still reads an invalid zero header in this
  setup, but the source fallback is valid: 1008362 samples, baseline 1042
  cycles. The source-side log captures the precopy window; postcopy/destination
  guest latency windows remain unavailable without a destination-side marker.

Rejected setup samples from the same diagnostic pass:

| port | result | stderr |
| ---: | --- | --- |
| 7750 | setup failure | `RDMA_CM_EVENT_REJECTED` |
| 7770 | setup failure | `RDMA_CM_EVENT_REJECTED` |
| 7780 | setup failure | `RDMA_CM_EVENT_REJECTED` |
| 7760 | completed | n/a |

These rejected samples failed before any data movement, so they are excluded
from dataplane analysis. They do show that RDMA CM setup remains unstable and
needs separate diagnosis if reproducibility becomes the next target.

## Current Implementation Status

- `hybrid_parallel_rdma_cxl` keeps the main migration URI on `unix:` and uses a separate RDMA sidecar endpoint.
- RDMA sidecar bytes are counted from sidecar schedule/post/verbs completion paths, not a local memcpy/shadow-copy path.
- The report summary exposes `rdma_sidecar_endpoint`, `rdma_sidecar_max_inflight_regions`, `rdma_sidecar_max_cover_percent`, `rdma_sidecar_posted_bytes`, `rdma_sidecar_completed_bytes`, `rdma_sidecar_stale_regions`, `rdma_sidecar_cxl_race_lost_regions`, `cxl_publish_pages`, `cxl_publish_time_ns`, `guest_stall_ms`, `total_time_ms`, `precopy_time_ms`, and `postcopy_time_ms`.
- The experiment script now emits the native QMP `MigrationAddress` shape for RDMA sidecar addresses: `{ "transport": "rdma", "host": "...", "port": "..." }`.
- The experiment script also has a diagnostic-only `--gdb-dst` option. It is useful for early QEMU failures, but it perturbs RDMA timing and should not be used for performance runs.

## Command

```bash
sudo -n env QEMU_CXL_WARM_ALLOW_UNPRIVILEGED=1 \
python3 scripts/cxl-hybrid-warm-experiment.py \
  --keep-dir \
  --pressure remap_xlarge_random_rw \
  --mode hybrid_postcopy_auto,hybrid_parallel_rdma_cxl \
  --repeat 1 \
  --migration-timeout 120 \
  --accel kvm \
  --max-bandwidth 0 \
  --cxl-path /dev/dax0.1 \
  --x-cxl-switch-min-remaining 0 \
  --x-cxl-switch-remap-coverage 50 \
  --x-cxl-switch-max-precopy-ms 50 \
  --x-cxl-disable-brake \
  --rdma-host 10.0.0.2 \
  --rdma-port 7471 \
  --rdma-pin-all \
  --x-cxl-rdma-sidecar-max-inflight-regions 1 \
  --x-cxl-rdma-sidecar-max-cover-percent 25 \
  --in-memory-guest-latency \
  --in-memory-guest-latency-source-first
```

The dataplane proof changed the sidecar command to:

```bash
sudo -n env QEMU_CXL_WARM_ALLOW_UNPRIVILEGED=1 \
python3 scripts/cxl-hybrid-warm-experiment.py \
  --keep-dir \
  --pressure remap_xlarge_random_rw \
  --mode hybrid_parallel_rdma_cxl \
  --repeat 1 \
  --migration-timeout 120 \
  --accel kvm \
  --max-bandwidth 0 \
  --cxl-path /dev/dax0.1 \
  --x-cxl-switch-min-remaining 0 \
  --x-cxl-switch-remap-coverage 50 \
  --x-cxl-switch-max-precopy-ms 100 \
  --x-cxl-disable-brake \
  --x-cxl-backing-rate 1048576 \
  --rdma-host 10.0.0.2 \
  --rdma-port 7472 \
  --rdma-pin-all \
  --x-cxl-rdma-sidecar-max-inflight-regions 8 \
  --x-cxl-rdma-sidecar-max-cover-percent 50 \
  --in-memory-guest-latency \
  --in-memory-guest-latency-source-first
```

## Debug Notes

- `cleanup_range: userfault unregister Invalid argument` also appears in the completed CXL-only control. It is a shared CXL region-remap cleanup artifact, not the proven sidecar crash root cause.
- Running the destination under gdb from process start can perturb KVM/RDMA CM timing. The useful backtrace came from normal execution with a temporary core pattern; the cores showed `cxl_rdma_sidecar_cleanup()` calling `ibv_destroy_cq()` on both sides.

## Conclusion

The host hardware test confirms the sidecar uses real RDMA and can move CXL bulk regions through verbs completions. The cleanup crash has been traced and fixed; the current fixed run completes end to end while preserving CXL as the primary migration stream and RDMA as a secondary bulk stream. Further performance comparisons should use the fixed sidecar run, not the earlier dataplane-proof run that crashed during cleanup.
