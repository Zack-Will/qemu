# RDMA+CXL Parallel Hybrid Baseline

Date: 2026-05-26
Branch: `rdma-cxl-parallel-hybrid`
Commit: `0b1b9307562d0708e426885dc2487b40909eb599`

## Command

```bash
timeout 1200s sudo -n numactl --cpunodebind=4-7 --membind=4-7 \
  python3 scripts/cxl-hybrid-warm-experiment.py \
    --keep-dir \
    --pressure remap_xlarge_random_rw \
    --mode hybrid_postcopy_auto,hybrid_parallel_rdma_cxl \
    --repeat 1 \
    --migration-timeout 120 \
    --accel kvm \
    --max-bandwidth 0 \
    --cxl-path /dev/dax0.1 \
    --x-cxl-brake-remap-granule 262144 \
    --x-cxl-switch-min-remaining 0 \
    --x-cxl-switch-remap-coverage 50 \
    --x-cxl-switch-max-precopy-ms 50 \
    --x-cxl-disable-brake \
    --in-memory-guest-latency \
    --in-memory-guest-latency-source-first
```

Aggregate JSON captured from stdout:
`/tmp/task7-nobrake-rdma-cxl-experiment.log`

Result directory:
`/tmp/cxl-hybrid-warm-exp-khd_l_vz`

Per-case result JSON:

- `/tmp/cxl-hybrid-warm-exp-khd_l_vz/remap_xlarge_random_rw/hybrid_postcopy_auto-custom-run01/result.json`
- `/tmp/cxl-hybrid-warm-exp-khd_l_vz/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-custom-run01/result.json`

Both runs completed.

## Results

| Metric | CXL no-brake | RDMA+CXL | Delta |
|---|---:|---:|---:|
| QEMU total time | 37 ms | 31 ms | -6 ms / -16.2% |
| Downtime | 1 ms | 0 ms | -1 ms |
| Stop-to-start | 5 ms | 4 ms | -1 ms |
| Observed script window | 68.182 ms | 94.337 ms | +26.155 ms |
| QMP poll tail | 31.182 ms | 63.337 ms | +32.155 ms |
| Trace precopy estimate -> request-postcopy | 14.044 ms | 12.803 ms | -1.240 ms |
| Trace bulk iterate begin -> end | 14.019 ms | 12.776 ms | -1.244 ms |
| Postcopy start -> downtime end | 0.665 ms | 0.702 ms | +0.037 ms |
| Downtime end -> postcopy active | 3.035 ms | 2.029 ms | -1.006 ms |
| Postcopy active -> completion enter | 7.099 ms | 6.920 ms | -0.180 ms |
| Completion enter -> prepare done | 3.012 ms | 0.094 ms | -2.917 ms |
| Completion enter -> completed | 3.015 ms | 0.268 ms | -2.747 ms |

`observed_migration_window_ms` includes the script's QMP sampling tail. In this
run the RDMA+CXL QMP tail is larger, so QEMU `total-time` and trace timeline
fields are the better comparison for migration behavior.

## Guest Stall

The run used `--in-memory-guest-latency-source-first`, so the usable guest
latency window is source/precopy oriented. The postcopy and corrected windows
had zero samples and should not be interpreted as destination stall.

| Metric | CXL no-brake | RDMA+CXL | Delta |
|---|---:|---:|---:|
| In-memory precopy total excess | 2.072 ms | 2.147 ms | +0.075 ms |
| In-memory precopy max excess | 12.172 us | 24.721 us | +12.548 us |
| Heartbeat stall | unavailable | unavailable | in-memory mode disables heartbeat sampling |

RDMA+CXL did not reduce the measured source/precopy stall in this single run.
The absolute total excess stayed around 2 ms, but the max excess was higher.

## Postcopy And Region Work

| Metric | CXL no-brake | RDMA+CXL | Delta |
|---|---:|---:|---:|
| Postcopy region wait samples | 13 | 13 | 0 |
| Postcopy region wait time | 3,618,084 ns | 3,381,445 ns | -236,639 ns / -6.5% |
| Max region wait | 529,139 ns | 466,443 ns | -62,696 ns |
| Postcopy fault read/place/publish-wait time | 0 ns | 0 ns | 0 |
| Region publish pages | 16,146 | 6,605 | -9,541 pages / -59.1% |
| Region publish requests | 45 | 26 | -19 |
| Region publish time | 6,158,354 ns | 3,115,082 ns | -3,043,272 ns / -49.4% |
| Completion publish pages | 833 | 874 | +41 |

This matches the expected shape: RDMA+CXL mostly reduces CXL region publish
work, while the postcopy fault/wait path is broadly unchanged. The destination
still waits for CXL visibility at region granularity; the sidecar does not
remove that control dependency.

## RDMA Sidecar Counters

| Metric | RDMA+CXL |
|---|---:|
| RDMA bulk regions | 32 |
| RDMA bulk bytes | 64 MiB |
| RDMA ready regions/pages | 32 / 16,384 |
| RDMA invalidated regions | 13 |
| RDMA ready pages lost | 6,656 pages / 26 MiB |
| Inferred surviving ready regions/pages | 19 / 9,728 pages / 38 MiB |
| CXL republish due to RDMA invalidate | 0 regions / 0 pages |
| RDMA invalidate publish amplification | 0.0 |

The script does not yet expose a separate committed-region counter. The
surviving RDMA-ready count above is inferred as ready minus invalidated.

## Interpretation

RDMA+CXL improved QEMU `total-time` by 6 ms in this run. The direct bulk/precopy
gain is smaller: trace bulk iterate time dropped by about 1.24 ms. The rest of
the total-time improvement comes from shorter postcopy handoff/completion work,
especially completion prepare, because less CXL publication remained.

The bulk phase is only slightly faster because the workload is small and CXL
still remains authoritative for visibility. RDMA marked 64 MiB ready, but 13 of
32 whole regions were invalidated at postcopy start, losing 26 MiB of RDMA-ready
coverage. The remaining useful RDMA coverage reduces region publish pages, but
does not remove the destination's CXL visibility wait path.

RDMA invalidation did occur in this run and it did not create extra CXL
republish amplification: `cxl_republish_pages_due_to_rdma_invalidate=0` and
amplification is `0.0`. The measured cost is lost sidecar usefulness rather
than a postcopy stall regression. Postcopy region wait was nearly unchanged and
slightly lower in RDMA+CXL.

## Decision

Keep whole-region ownership for the next stage. The Task 7 target is met:
RDMA+CXL completes in the no-brake comparison, emits the required accounting,
shows lower QEMU total time, and exercises RDMA invalidation without postcopy
fallback amplification.

Do not pause RDMA+CXL. Do not reduce granularity yet. The next useful change is
dirty-aware RDMA admission or commit filtering so the sidecar avoids regions
likely to be invalidated before postcopy. Revisit smaller granularity only if
repeat runs show whole-region invalidation consistently discarding too much
ready coverage.
