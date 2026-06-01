# RDMA CXL Page-State Phase 2 Results

## Commands

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
    --x-cxl-switch-max-precopy-ms 1 \
    --x-cxl-disable-brake \
    --rdma-host 10.0.0.2 \
    --rdma-port 7750 \
    --rdma-pin-all \
    --x-cxl-rdma-sidecar-max-inflight-regions 8 \
    --x-cxl-rdma-sidecar-max-cover-percent 50
```

Successful run directory:

```text
/tmp/cxl-hybrid-warm-exp-nelwwulw/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01
```

One previous retry failed before bulk at RDMA connection setup:

```text
/tmp/cxl-hybrid-warm-exp-c8k2_xmq/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01
RDMA sidecar expected CM event RDMA_CM_EVENT_ESTABLISHED, got RDMA_CM_EVENT_REJECTED
```

The failed retry posted no RDMA bytes and copied no CXL worker bytes; it is a setup-path CM reject, not a bulk data-plane result.

## Checkpoint

- QEMU build: PASS
- control unit tests: PASS, 70/70
- region unit tests: PASS, 57/57
- postcopy helper tests: PASS, 24/24
- parser compile: PASS
- parser unit tests: PASS, 225/225

## Bulk Data Plane

| lane | bytes | time ms | bandwidth MiB/s |
| --- | ---: | ---: | ---: |
| CXL worker | 38,801,408 | 7.379 | 5,015.1 |
| RDMA completed | 33,554,432 | 42.786 | 747.9 |

Additional counters:

- `rdma-sidecar-posted-bytes`: 33,554,432
- `rdma-sidecar-completed-bytes`: 33,554,432
- `rdma-sidecar-completed-regions`: 16
- `rdma-sidecar-registered-bytes`: 67,641,344
- `rdma-sidecar-connect-time-ns`: 52,482,955
- `stream-write-bytes`: 0
- `backing-write-bytes`: 8,192

The page-state data path is now active on both lanes. RDMA completed exactly the posted 32 MiB through real verbs completions to destination RAM. CXL worker bytes are non-zero and slightly exceed half the VM-sized payload because the run also copied postcopy-warm primary pages through the CXL worker after bulk dirties.

## Fault And Stale Behavior

- trace `phase_postcopy_warm`: 1
- `postcopy_control.requested`: true
- `max_ram_postcopy_requests`: 0
- `region_publish_pages`: 1,153
- `region_publish_requests`: 32
- `region_publish_time_ns`: 1,087,370
- `page_state_rdma_stale_pages`: 0
- `page_state_cas_failures`: 0
- `cxl-republish-pages-due-to-rdma-invalidate`: 0
- `cxl-republish-regions-due-to-rdma-invalidate`: 0
- guest max gap stall during migration: 0.562 ms
- guest total stall during migration: 47.189 ms
- heartbeat p50/p95/p99 stall: not emitted by this harness run

Latency:

- `total_time_ms`: 76
- `setup_time_ms`: 59
- `precopy_time_ms`: 8.069
- `postcopy_time_ms`: 8.836
- `stop_to_start_time_ms`: 2
- `qmp_poll_tail_ms`: 58.079

## Interpretation

Both CXL and RDMA moved non-zero physical bytes during the same migration run, satisfying the Phase 2 data-plane gate. RDMA no longer dominates all page movement: it completed 32.0 MiB, while the CXL worker copied 37.0 MiB. The CXL total includes postcopy-warm publication work, so it should not be read as a pure 50/50 bulk-only split.

The postcopy demand path did not regress in this run because there were no postcopy demand faults. The postcopy-warm trace still fired, and the warm publication work used CXL page-state publication rather than RAM stream writes.

The RDMA invalidation path stayed quiet: zero stale RDMA pages, zero CAS failures, and zero region-wide CXL republish due to RDMA invalidation. This matches the Phase 2 rule that stale pages are handled at page granularity rather than by whole-region republish.
