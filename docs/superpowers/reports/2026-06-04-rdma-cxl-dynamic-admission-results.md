# RDMA CXL Dynamic Admission Results

Date: 2026-06-04
Branch: `rdma-cxl-parallel-hybrid`

## Command

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

The converged command treats `--x-cxl-rdma-sidecar-max-inflight-regions=0` as
an automatic transport/resource hint. It has no fixed coverage parameter. The
postcopy dirty RDMA lane is enabled by default for the current RDMA/CXL
parallel experiment.

## Verification

- QEMU build: `ninja -C build qemu-system-x86_64 tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region tests/unit/test-migration-postcopy`
- C unit tests: `test-cxl-hybrid-control`, `test-cxl-hybrid-region`, and `test-migration-postcopy` TAP runs passed
- Python compile: `python3 -m py_compile scripts/rdma_cxl_parallel_experiment.py scripts/rdma_cxl_parallel_experiment_test.py`
- Python runner tests: `python3 scripts/rdma_cxl_parallel_experiment_test.py`
  passed 6 tests
- Dynamic admission run: completed
- Destination still running: True
- Guest latency valid: True
- Guest latency dump source: primary
- Post-BDP-cap validation run: completed with primary in-memory dump and
  destination still running

For new samples, the valid result surface is the simplified runner's
`summary.json` and `summary.csv`. A converged run is valid when
`final_status=completed`, `dst_running=true`, `dst_status=running`,
`rdma_admission_accepted_regions > 0`, postcopy dirty RDMA completed bytes are
non-zero for feature samples, stale pages are zero for correctness samples, and
`stderr_error_count=0`.

## Dynamic Admission Metrics

| run directory | status | total ms | SQ cap | final window | BDP regions | accepted RDMA | overflow CXL | RDMA goodput B/ns | RDMA latency ns | CXL bytes | RDMA bytes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| /tmp/cxl-hybrid-warm-exp-hfeajir2/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01 | completed | 67 | 8 | 1 | 2 | 9 | 28 | 1.6435218267284948 | 1290575 | 35045376 | 8388608 |
| /tmp/cxl-hybrid-warm-exp-rvc16ak1/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01 | completed | 62 | 8 | 1 | 2 | 7 | 30 | 1.4829019524823313 | 1468869 | 34467840 | 12582912 |
| /tmp/cxl-hybrid-warm-exp-fed6uchc/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01 | completed | 56 | 8 | 2 | 2 | 9 | 28 | 4.1750082220221 | 588325 | 21917696 | 8388608 |

## Lane Time Breakdown

For the post-BDP-cap validation runs:

| run | precopy wall ms | RDMA completed ms | CXL worker ms | RDMA MiB | CXL MiB | RDMA byte share | CXL byte share | RDMA MiB/s | CXL MiB/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| BDP hard cap | 10.589699 | 9.482069 | 5.277465 | 12.00 | 32.87 | 26.7% | 73.3% | 1265.5 | 6228.6 |
| probe first | 3.610789 | 3.247525 | 2.285807 | 8.00 | 20.90 | 27.7% | 72.3% | 2463.4 | 9144.4 |

The RDMA and CXL lane times are cumulative lane work counters, so they overlap
inside the precopy wall-clock window and should not be summed. The sample shows
that both lanes are active, but not evenly balanced: CXL carries most bytes and
RDMA remains a constrained fast lane. The final effective RDMA window was 1
region, with BDP estimated at 2 regions and 4 goodput-drop events, so the
controller kept admission conservative and overflowed 30 candidate regions to
CXL.

This run exposed a policy issue rather than an RDMA transport failure. With a
2 MiB region and about 1.47 ms RDMA completion latency, a one-region window caps
RDMA near 1.3 GiB/s, matching the measured 1265.5 MiB/s. The follow-up
controller change probes to the SQ safety cap before applying self-estimated
BDP as a hard cap, and it requires a material goodput drop plus a material
latency rise before reducing the window.

The clean post-probe run completed with primary in-memory dump and no
stderr/log cleanup, UFFD, error, failed, or traceback matches. It reduced the
precopy wall time from 10.589699 ms to 3.610789 ms and raised RDMA completed
goodput from 1265.5 MiB/s to 2463.4 MiB/s. It still does not produce equal byte
balance: CXL carried 72.3% of measured lane bytes. An earlier post-probe run at
`/tmp/cxl-hybrid-warm-exp-i56t13_m` completed but had `uffd_copy_page() failed`
messages in destination stderr, so it is excluded from the clean sample table.

## RDMA Active Wall Time And UFFD Cleanup Root Cause

The validation run below was collected before the simplified runner replaced
the old warm-experiment script. Keep it as historical evidence for the cleanup
root cause; use the canonical command in the current experiment configuration
for new samples.

```bash
legacy warm-experiment runner, remap_xlarge_random_rw,
hybrid_parallel_rdma_cxl, minimal trace, rdma pinning enabled,
8-region compatibility hint
```

Run directory:

```text
/tmp/cxl-hybrid-warm-exp-75pf97x7/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01
```

Result:

| status | dst running | precopy ms | postcopy ms | SQ cap | window | BDP | accepted RDMA | overflow CXL | drops | RDMA MiB | RDMA active ms | RDMA active MiB/s | RDMA completed ms | RDMA completed MiB/s | CXL MiB | CXL worker ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| completed | true | 9.025874 | 6.325962 | 8 | 8 | 8 | 21 | 16 | 0 | 32.00 | 8.624391 | 3710.4 | 39.224944 | 815.8 | 18.89 | 2.452315 |

`page-state-rdma-completed-time-ns` remains the cumulative post-to-completion
latency across descriptors. It is not a lane wall-clock metric under concurrent
RDMA, because overlapping WR latencies are summed. The new
`page-state-rdma-active-time-ns` measures wall-clock epochs where at least one
RDMA descriptor is in flight. For this run, the old completed-time denominator
reports only 815.8 MiB/s, while active wall time reports 3710.4 MiB/s for the
same 32 MiB of useful RDMA completions.

The earlier UFFD error was caused by destination cleanup after successful
`MAP_FIXED` CXL remaps. The failing page `pc.ram/0x2f7000` was inside a traced
destination remap span `pc.ram/0x220000 len=2097152`, but the remapped-page
bitmap helper required `qemu_ram_pagesize()` alignment. Some destination remap
spans are 4 KiB aligned but not 2 MiB aligned, so the bitmap was not marked.
Cleanup then attempted `UFFDIO_COPY`, received `-ENOENT` because the address
had already been remapped out of the UFFD registration, and misclassified the
result as a placement failure.

The helper now maps destination remap offsets to page-state indices using the
target page size, not the host RAM page size. The validation run above completed
with destination still running and no stderr matches for `uffd`, `cleanup`,
`error`, `failed`, or `Traceback`; stderr contained only the script's final
SIGTERM shutdown line.

## Interpretation

RDMA admission is valid for this sample only if the run completed,
destination-side in-memory guest latency used a primary dump, and
accepted plus overflow regions is non-zero. This run meets those
checks. In the post-BDP-cap validation run, RDMA accepted regions were
non-zero and dynamic admission overflowed additional candidates to CXL while
keeping the SQ cap at 8. The effective final admission window was 1 region with
an observed BDP estimate of 2 regions, so the scheduler kept RDMA below the SQ
safety cap and sent most excess work to CXL. Compare RDMA completed bytes with
CXL worker bytes to determine whether the dynamic window kept both lanes active
or overflowed most work to CXL.

## Latest Run Time-Balance Audit

The `/tmp/cxl-hybrid-warm-exp-75pf97x7` run did not achieve time balance.
RDMA active wall time was 8.624391 ms, which occupied almost the whole
9.025874 ms precopy interval. The CXL worker copied 18.89 MiB in 2.452315 ms.
This is not a case where CXL was waiting for RDMA and both lanes finished
together; the RDMA lane was the long tail.

The text trace supports that conclusion. The first precopy scan ended after
about 2.665 ms of RAM iteration time and then requested postcopy. At that
point the trace had completed region 9, while regions 11, 22, 24, 26, 27, 29,
and 31 completed only after `postcopy-start-call` and before `postcopy_start`.
The page-state snapshot immediately before postcopy start then showed all
pages visible. In other words, CXL and the RAM scan finished early, while the
source waited for RDMA drain before postcopy could actually start.

The 3710.4 MiB/s RDMA active-goodput number is still below the expected
loopback RDMA ceiling. It should not be treated as proof that RDMA is saturated
at the hardware limit. In this run the controller saw no goodput-drop events,
kept the dynamic window at the SQ cap of 8, and reported BDP at the same cap.
That means the current data distinguishes neither a real RDMA transport limit
nor an artificial cap/window limit. The run also has only 16 posted 2 MiB WRs,
so active wall time includes startup and drain tail rather than a long steady
state.

The removed fixed coverage parameter is no longer present in current command
lines or QAPI. Before using this run to tune overflow policy, collect converged
RDMA/CXL samples with automatic sizing and compare the reported dynamic window,
SQ capacity, BDP estimate, accepted regions, CXL overflow regions, and
postcopy dirty RDMA completion counters. Those metrics distinguish a transport
limit from controller backpressure and CXL fallback.

## RDMA Cap Removal Probe

The current QMP parameter now accepts
`x-cxl-rdma-sidecar-max-inflight-regions=0` as automatic sizing. Older cap
probes below remain historical context; do not treat them as the current
converged experiment shape.

| cap | status | clean stderr | precopy ms | request-to-postcopy ms | RDMA MiB | RDMA active ms | RDMA active MiB/s | CXL MiB | CXL worker ms | posted RDMA | overflow CXL | BDP |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | completed | yes | 9.025874 | 6.328959 | 32 | 8.624391 | 3710.4 | 18.89 | 2.452315 | 16 | 16 | 8 |
| 16 | completed | no, 1 UFFD copy error | 18.287226 | 14.868645 | 48 | 17.642188 | 2720.8 | 16.48 | 1.406549 | 24 | 8 | 16 |
| 32 | completed | no, 9 UFFD copy errors | 32.855646 | 31.275357 | 64 | 32.398678 | 1975.4 | 16.66 | 1.667185 | 32 | 1 | 32 |
| 64 | completed | no, 3 UFFD copy errors | 33.715267 | 32.134471 | 64 | 33.420964 | 1915.0 | 16.68 | 1.452785 | 32 | 0 | 32 |

Run directories:

- cap 8:
  `/tmp/cxl-hybrid-warm-exp-75pf97x7/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01`
- cap 16:
  `/tmp/cxl-hybrid-warm-exp-rawd03wx/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01`
- cap 32:
  `/tmp/cxl-hybrid-warm-exp-prtl_h72/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01`
- cap 64:
  `/tmp/cxl-hybrid-warm-exp-nd0kvdq0/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01`

This probe does not support removing the cap as the next policy step. Raising
the cap fills RDMA with more of the 64 MiB payload, but the active RDMA
goodput falls from 3710.4 MiB/s at cap 8 to about 1.9-2.0 GiB/s at cap 32/64.
The postcopy request-to-start gap also grows from 6.33 ms to about 32 ms,
showing a larger RDMA drain tail. The trace confirms the pattern: at cap 32
and cap 64 the source posts all 32 RDMA regions during the first scan and most
or all completions arrive only after `postcopy-start-call`.

The high-cap runs also reintroduce destination `uffd_copy_page() failed`
messages with `errno=2`. They completed with the destination still running, but
they are not clean correctness samples. The errors correlate with many more
destination remaps: 14 or 18 remaps versus 4 in the clean cap-8 run. Treat the
RDMA source-side throughput trend as useful evidence, but fix or further
instrument the destination cleanup/remap path before relying on the high-cap
samples for end-to-end correctness.

The slowdown is not just byte-level imbalance. The measured per-WR
post-to-completion latency grows with depth:

| cap | posted WRs | RDMA completed-time sum ms | mean post-to-complete ms/WR | RDMA active MiB/s |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 16 | 39.224944 | 2.452 | 3710.4 |
| 16 | 24 | 149.162187 | 6.215 | 2720.8 |
| 32 | 32 | 505.517650 | 15.797 | 1975.4 |
| 64 | 32 | 537.467311 | 16.796 | 1915.0 |

That means RDMA service quality changes when the queue is made deeper. This
can happen even on hardware RDMA with a single RC QP and large 2 MiB signaled
WRs: posting more WRs than the path can use does not create more useful
parallelism, and it can add NIC/QP queueing, memory-system contention, CQ
pressure, and a larger drain tail. The current controller also only updates
goodput when an active RDMA epoch drains. At high caps the first scan can post
nearly the whole workload before any feedback is applied, so there is no
mid-epoch chance to stop admission after latency starts rising.

This should not be over-interpreted as pure RDMA wire bandwidth falling. The
current `page-state-rdma-active-time-ns` and completed-time metrics stop the
clock after the sidecar has polled a CQE and run `complete_bulk_claim()`.
`complete_bulk_claim()` marks the 2 MiB claim staged and completes the RDMA
page-state descriptor one 4 KiB page at a time. Therefore the measured active
goodput includes RDMA transport, CQ polling, page-state CAS/visible-bit
publication, and drain scheduling. With cap 32 or 64, those 16,384 per-page
completion operations are concentrated in the postcopy-start drain path. The
existing data proves that end-to-end RDMA-lane service time worsens at high
depth, but it does not yet prove whether the lost time is in the NIC/verbs
path, CQ polling, or QEMU page-state completion. Separate timestamp counters
are needed for transport-only post-to-CQE time and CQE-to-visible time.

## RDMA Transport/Publish Split Rerun

The sidecar now exports:

- `page-state-rdma-transport-completed-time-ns`: per-WR post-to-CQE
  latency sum;
- `page-state-rdma-transport-active-time-ns`: wall-clock active epoch from
  first post to last CQE;
- `page-state-rdma-publish-time-ns`: CQE-to-page-state-visible publication
  time.

The old `page-state-rdma-active-time-ns` remains the end-to-end RDMA lane
service time and still stops after `complete_bulk_claim()`.

Verification before the current converged runner replacement:

- `python3 scripts/rdma_cxl_parallel_experiment_test.py` passed 6 tests.
- `./build/tests/unit/test-cxl-hybrid-region --tap` passed 71 tests.
- `./build/tests/unit/test-cxl-hybrid-control --tap` passed 86 tests.
- `./build/tests/unit/test-migration-postcopy --tap` passed 31 tests.
- `python3 -m py_compile scripts/rdma_cxl_parallel_experiment.py
  scripts/rdma_cxl_parallel_experiment_test.py` passed.
- `git diff --check` passed.
- `ninja -C build qemu-system-x86_64 tests/unit/test-cxl-hybrid-control
  tests/unit/test-cxl-hybrid-region tests/unit/test-migration-postcopy`
  completed.

Rerun command shape was the canonical `remap_xlarge_random_rw`
`hybrid_parallel_rdma_cxl` command with `--rdma-pin-all` and cap swept over
8, 16, 32, and 64.

| cap | status | clean stderr | precopy ms | request-to-postcopy ms | RDMA MiB | RDMA transport active ms | RDMA transport MiB/s | RDMA e2e active ms | RDMA publish sum ms | CXL MiB | CXL worker ms | CXL MiB/s | posted RDMA | overflow CXL | BDP |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | completed | yes | 8.756704 | 6.287093 | 30.00 | 8.509300 | 3525.6 | 8.527506 | 0.307306 | 19.00 | 2.020088 | 9403.6 | 15 | 17 | 8 |
| 16 | completed | no, 1 UFFD copy error | 17.056337 | 14.851050 | 48.00 | 16.553760 | 2899.6 | 16.585059 | 0.807868 | 16.45 | 1.634807 | 10061.9 | 24 | 9 | 16 |
| 32 | completed | yes | 33.648985 | 31.189832 | 64.00 | 33.125981 | 1932.0 | 33.150233 | 0.788615 | 16.70 | 1.693000 | 9861.4 | 32 | 1 | 32 |
| 64 | completed | no, 6 UFFD copy errors | 33.744383 | 32.181158 | 64.00 | 33.426410 | 1914.7 | 33.453207 | 0.854618 | 16.66 | 1.999915 | 8332.4 | 32 | 0 | 32 |

Run directories:

- cap 8:
  `/tmp/cxl-hybrid-warm-exp-zn66x_vg/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01`
- cap 16:
  `/tmp/cxl-hybrid-warm-exp-ejbw1hx7/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01`
- cap 32:
  `/tmp/cxl-hybrid-warm-exp-7t11e6m8/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01`
- cap 64:
  `/tmp/cxl-hybrid-warm-exp-3fb965ad/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01`

The split shows that CQE-to-visible publication is not the reason RDMA slows
at high queue depth. The end-to-end active time and transport-only active time
differ by only tens of microseconds in each run, and the cumulative publish
sum stays below 0.9 ms even when 32 2 MiB WRs complete. The transport active
goodput itself falls from 3525.6 MiB/s at cap 8 to about 1.9 GiB/s at
cap 32/64.

The per-WR transport latency sum confirms the queue-depth effect:

| cap | posted WRs | transport completed-time sum ms | mean post-to-CQE ms/WR | transport active MiB/s |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 15 | 37.585629 | 2.506 | 3525.6 |
| 16 | 24 | 134.433128 | 5.601 | 2899.6 |
| 32 | 32 | 517.632174 | 16.176 | 1932.0 |
| 64 | 32 | 537.323507 | 16.791 | 1914.7 |

This rerun changes the diagnosis: page-state publication overhead should be
kept out of data-link bandwidth reporting, but it does not explain the observed
bandwidth loss. The slowdown is already visible in post-to-CQE transport time.
The current policy still cannot react inside one large probe epoch: at cap 32
or 64 the source can post essentially all RDMA-eligible regions before the
first useful feedback closes or shrinks admission. A better next controller
step is to probe RDMA in bounded waves and update admission from
transport-active goodput before allowing the next wave to overflow CXL.

## No-Hard-Cap Auto Admission Rerun

After removing `x-cxl-rdma-sidecar-max-inflight-regions` as a hard policy cap,
the validation command was:

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

Run directory:

```text
/tmp/cxl-hybrid-warm-exp-qt64h52i/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01
```

Result:

| status | dst running | dump | precopy ms | postcopy ms | legacy max-inflight | SQ cap | final window | BDP | accepted RDMA | posted RDMA | overflow CXL | drops | RDMA MiB | RDMA transport active ms | RDMA transport MiB/s | RDMA publish ms | CXL MiB | CXL worker ms | CXL MiB/s |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| completed | true | primary | 3.118776 | 6.850378 | 0 | 33 | 1 | 2 | 11 | 10 | 26 | 9 | 20.00 | 2.683887 | 7451.9 | 0.189825 | 17.37 | 1.672266 | 10385.4 |

The stderr check found no `uffd`, `UFFD`, `uffd_copy_page`, `cleanup`,
`error`, `failed`, or `Traceback` matches. The only stderr lines were the
script's final SIGTERM shutdown line.

This confirms the cap-removal behavior: the legacy max-inflight field reports
0, but the actual RDMA SQ/admission capacity is 33 regions, derived from the
64 MiB/2 MiB geometry and transport resource ceiling. The run no longer has an
8-region hard cap.

The bounded-wave controller is now more reactive than the previous cap sweep.
It posted 10 RDMA regions, overflowed 26 candidates to CXL, and reduced the
final window to 1 after 9 transport-goodput/latency drop events. RDMA transport
active goodput improved to 7451.9 MiB/s for this sample, while the CXL worker
reported 10385.4 MiB/s. Lane active times are still not equal, but they are
much closer than the earlier high-cap drain-tail runs: RDMA transport active
time was 2.684 ms and CXL worker time was 1.672 ms inside a 3.119 ms precopy
window.
