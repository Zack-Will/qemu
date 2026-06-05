# Current RDMA CXL Experiment Configuration

Date: 2026-06-04

This document fixes the current converged experiment shape for the
`rdma-cxl-parallel-hybrid` branch. New runs use the simplified RDMA/CXL
parallel runner and exercise both current lanes: precopy RDMA full-region
claims with CXL overflow, and postcopy dirty RDMA contiguous spans with CXL
fallback.

## Canonical Command

```bash
sudo -n env QEMU_CXL_WARM_ALLOW_UNPRIVILEGED=1 \
/usr/local/bin/numactl --cpunodebind=4-7 --membind=4-7 \
/usr/bin/python3 scripts/rdma_cxl_parallel_experiment.py \
  --keep-dir \
  --pressure remap_xlarge_random_rw \
  --mode hybrid_parallel_rdma_cxl \
  --repeat 1 \
  --migration-timeout 120 \
  --trace-profile minimal \
  --rdma-pin-all \
  --accel kvm \
  --x-cxl-rdma-sidecar-max-inflight-regions 0 \
  --postcopy-dirty-rdma \
  --postcopy-dirty-rdma-min-bytes 65536
```

Required points:

- Run under `sudo -n`; the CXL/DAX and KVM paths depend on privileged setup.
- Bind CPU and memory to NUMA nodes `4-7`.
- Keep `--accel kvm`; TCG results are not comparable for guest-stall analysis.
- Use `remap_xlarge_random_rw`.
- Use `hybrid_parallel_rdma_cxl` only when measuring the current RDMA+CXL path.
- RDMA sidecar in-flight parameter is an auto hint. Treat
  `--x-cxl-rdma-sidecar-max-inflight-regions=0` as automatic
  transport/resource sizing.
- Keep `--postcopy-dirty-rdma` enabled for converged RDMA/CXL samples. Disable
  it only for an explicit ablation.
- Use `--trace-profile minimal` for performance runs.
- The simplified runner writes `summary.json` and `summary.csv` in the run
  directory. Use those files as the primary result surface.

## Brake Policy

The older RDMA+CXL comparison experiments intentionally skipped the source-side
brake phase:

- they passed `--x-cxl-disable-brake`;
- they also usually passed `--in-memory-guest-latency-source-first`;
- their guest-stall numbers should be interpreted as source/precopy-oriented
  unless a destination dump was explicitly valid.

The current canonical command above does not pass `--x-cxl-disable-brake`.
That leaves the branch's normal balanced policy in control. In the latest
validated runs, the policy still did not enter a brake phase:

- `final_x_cxl.switch-reason=precopy-complete`;
- `handoff_breakdown.control_src_enter_brake_to_request_postcopy_ms=null`;
- `final_x_cxl.remap-coverage=0`;
- `final_x_cxl.clean-remap-coverage=0`;
- `guest_in_memory_latency.precopy_brake_window.count=0`.

So the practical result of these samples is also no-brake, but the reason is
different. Historical runs explicitly disabled brake with a CLI flag; current
runs leave brake enabled and observe that the balanced policy switches at
precopy completion before a brake window exists.

Use `--x-cxl-disable-brake` only when reproducing the historical no-brake
baseline. Do not add it to the current destination-stall command unless the
experiment is explicitly about forced no-brake comparability.

## Success Checks

For each result directory, treat the run as a valid converged sample only if
these fields hold in `summary.json`:

- `final_status=completed`;
- `dst_running=true` and `dst_status=running`;
- `rdma_admission_accepted_regions > 0`;
- `rdma_postcopy_dirty_completed_bytes > 0`;
- `rdma_postcopy_dirty_stale_pages = 0` for correctness samples;
- `rdma_admission_overflow_cxl_regions >= 0`;
- `rdma_postcopy_dirty_overflow_cxl_spans >= 0`;
- `stderr_error_count = 0`.

## Postcopy Dirty RDMA Variant

Postcopy dirty RDMA is no longer a separate optional variant for the converged
profile. It is part of the current RDMA/CXL parallel architecture. The ablation
command is the canonical command plus `--no-postcopy-dirty-rdma`; compare it
only against a same-branch run collected with the same trace profile and
resource hints.

## Recent Reference Runs

The following runs used the current profile after the destination dump cleanup
fixes:

After dynamic admission and postcopy dirty RDMA converge, historical fixed
policy runs remain useful only as baselines. New runs should report dynamic
window, SQ cap, BDP estimate, RDMA accepted/overflow counts, postcopy dirty
RDMA completed bytes/spans, and CXL fallback counts.

| run directory | status | dump | total ms | setup ms | precopy ms | postcopy ms | corrected stall ms | note |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| `/tmp/cxl-hybrid-warm-exp-9h8_s3p8/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01` | completed | primary | 58 | 36 | 8.34 | 13.08 | 12.20 | first fixed destination dump sample |
| `/tmp/cxl-hybrid-warm-exp-plt89glv/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01` | completed | primary | 76 | 53 | 11.13 | 10.90 | 10.81 | RDMA connect variance dominated setup |
| `/tmp/cxl-hybrid-warm-exp-5lci1fsd/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01` | completed | primary | 61 | 38 | 11.04 | 11.79 | n/a | marker completion window incomplete |

For the second run, the corrected in-memory stall split was:

- precopy window: `2.159 ms`;
- postcopy handoff window: `8.684 ms`;
- first destination sample/handoff gap dominated the postcopy handoff excess.

Trace-side postcopy work was small in that run:

- `postcopy_region_wait_time_ns=6856`;
- `max_dst_region_map_time_ns=717521`;
- `trace_dst_region_remap=20`.

This means the measured guest stall was dominated by the handoff gap around the
first destination sample, not by ongoing destination-side region remap or fault
service after the guest was running.
