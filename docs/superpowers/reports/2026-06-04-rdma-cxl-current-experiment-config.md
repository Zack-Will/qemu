# Current RDMA CXL Experiment Configuration

Date: 2026-06-04

This document fixes the current performance/stall experiment shape for the
`rdma-cxl-parallel-hybrid` branch. It supersedes the older ad-hoc command lines
for new `remap_xlarge_random_rw` destination-stall measurements, but it does not
rewrite the historical no-brake/source-first results.

## Canonical Command

```bash
sudo -n env QEMU_CXL_WARM_ALLOW_UNPRIVILEGED=1 \
/usr/local/bin/numactl --cpunodebind=4-7 --membind=4-7 \
/usr/bin/python3 scripts/cxl-hybrid-warm-experiment.py \
  --keep-dir \
  --pressure remap_xlarge_random_rw \
  --mode hybrid_parallel_rdma_cxl \
  --threshold-profile balanced \
  --repeat 1 \
  --migration-timeout 120 \
  --trace-profile minimal \
  --x-cxl-rdma-sidecar-max-inflight-regions 8 \
  --accel kvm \
  --in-memory-guest-latency
```

Required points:

- Run under `sudo -n`; the CXL/DAX and KVM paths depend on privileged setup.
- Bind CPU and memory to NUMA nodes `4-7`.
- Keep `--accel kvm`; TCG results are not comparable for guest-stall analysis.
- Use `remap_xlarge_random_rw`.
- Use `hybrid_parallel_rdma_cxl` only when measuring the current RDMA+CXL path.
- Use `--threshold-profile balanced`.
- RDMA sidecar in-flight parameter is a safety cap. Treat
  `--x-cxl-rdma-sidecar-max-inflight-regions` as a safety cap for the dynamic
  RDMA admission window, not as a fixed desired depth or target lane ratio.
- Do not pass `--x-cxl-rdma-sidecar-max-cover-percent`; dynamic admission
  ignores fixed coverage and sends overflow to CXL.
- Use `--trace-profile minimal` for performance runs.
- Use destination-side `--in-memory-guest-latency`.

Do not add `--in-memory-guest-latency-source-first` for the current
destination-stall runs. Source-first mode is useful for source/precopy-only
diagnostics, but it hides the destination-side handoff window that this
experiment is meant to measure.

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

For each result directory, treat the run as a valid current-profile sample only
if these fields hold:

- `final_status=completed`;
- `dst_status.running=true` and `dst_status.status=running`;
- `guest_in_memory_latency.valid=true`;
- `guest_in_memory_latency.dump_source=primary`;
- `guest_in_memory_latency.dump_error=null`;
- `guest_in_memory_latency.partial_dump=null`;
- `guest_in_memory_latency.marker_samples_read > 0`;
- destination stderr has no cleanup/UFFD failure after migration completion.

If `dump_source=fallback`, the run may still be useful for source-side
diagnostics, but do not use it for destination-side guest-stall distribution.

## Recent Reference Runs

The following runs used the current profile after the destination dump cleanup
fixes:

After dynamic admission lands, historical `8/50` runs remain useful only as
fixed-policy baselines; new runs should report dynamic window, SQ cap, goodput
EWMA, BDP estimate, and CXL overflow counts.

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
