# Real RDMA CXL Sidecar Results

## Configuration

- Main migration URI: `unix:<runtime>/mig.sock`
- Sidecar RDMA endpoint: `rdma:<destination-rdma-ip>:<port>`
- rdma-pin-all: `<true|false>`
- max-inflight-regions: `<value>`
- max-cover-percent: `<value>`
- cxl-path: `<path>`
- workload: `<pressure>`

## Results

| mode | total ms | precopy ms | postcopy ms | guest stall ms | CXL publish pages | CXL publish time ns | RDMA posted bytes | RDMA completed bytes | stale regions | failed regions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| hybrid_postcopy_auto | TBD | TBD | TBD | TBD | TBD | TBD | 0 | 0 | 0 | 0 |
| hybrid_parallel_rdma_cxl | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |

## Analysis

- Bulk-phase change: compare `precopy_time_ms`, `cxl_publish_pages`, `cxl_publish_time_ns`, `rdma_sidecar_posted_bytes`, and `rdma_sidecar_completed_bytes`.
- Postcopy-phase change: compare `postcopy_time_ms`, `postcopy_fault_request`, `handoff_postcopy_region_publish_pages`, and `guest_stall_ms`.
- Guest stall: use `guest_stall_ms` and the in-memory windows (`guest_in_memory_precopy_*`, `guest_in_memory_postcopy_*`) when enabled.
- RDMA stale or CXL-race losses: use `rdma_sidecar_stale_regions`, `rdma_sidecar_cxl_race_lost_regions`, `rdma_invalidated_regions`, and `cxl_republish_pages_due_to_rdma_invalidate`.
- Whether CXL remained primary: CXL remains primary when `cxl_publish_pages` and CXL publish time stay non-zero while RDMA coverage is bounded by `rdma_sidecar_max_cover_percent`.

## Current Implementation Status

- `hybrid_parallel_rdma_cxl` keeps the main migration URI on `unix:` and uses a separate RDMA sidecar endpoint.
- RDMA sidecar bytes are counted from sidecar schedule/post/verbs completion paths, not a local memcpy/shadow-copy path.
- The report summary exposes `rdma_sidecar_endpoint`, `rdma_sidecar_max_inflight_regions`, `rdma_sidecar_max_cover_percent`, `rdma_sidecar_posted_bytes`, `rdma_sidecar_completed_bytes`, `rdma_sidecar_stale_regions`, `rdma_sidecar_cxl_race_lost_regions`, `cxl_publish_pages`, `cxl_publish_time_ns`, `guest_stall_ms`, `total_time_ms`, `precopy_time_ms`, and `postcopy_time_ms`.

## Command

```bash
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
  --rdma-host <destination-rdma-ip> \
  --rdma-port 7471 \
  --rdma-pin-all \
  --x-cxl-rdma-sidecar-max-inflight-regions 1 \
  --x-cxl-rdma-sidecar-max-cover-percent 25 \
  --in-memory-guest-latency \
  --in-memory-guest-latency-source-first
```

## Conclusion

TBD after running on the RDMA-capable host pair. The expected validation signal is non-zero RDMA posted/completed bytes from sidecar completion events, non-zero CXL publish work, and no memcpy/shadow sidecar completion path.
