# RDMA CXL Dynamic Admission Results

Date: 2026-06-04
Branch: `rdma-cxl-parallel-hybrid`

## Command

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

The run treated `--x-cxl-rdma-sidecar-max-inflight-regions` as a safety
cap and did not pass a fixed RDMA coverage parameter.

## Verification

- QEMU build: `ninja -C build qemu-system-x86_64 tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region tests/unit/test-migration-postcopy`
- C unit tests: `test-cxl-hybrid-control`, `test-cxl-hybrid-region`, and `test-migration-postcopy` TAP runs passed
- Python compile: `python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py scripts/cxl-hybrid-warm-experiment-test.py`
- Python parser tests: `python3 scripts/cxl-hybrid-warm-experiment-test.py` passed 255 tests
- Dynamic admission run: completed
- Destination still running: True
- Guest latency valid: True
- Guest latency dump source: primary
- Post-BDP-cap validation run: completed with primary in-memory dump and
  destination still running

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
