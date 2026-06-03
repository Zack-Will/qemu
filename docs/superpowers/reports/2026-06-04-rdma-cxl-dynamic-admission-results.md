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
- Python parser tests: `python3 scripts/cxl-hybrid-warm-experiment-test.py` passed 254 tests
- Dynamic admission run: completed
- Destination still running: True
- Guest latency valid: True
- Guest latency dump source: primary

## Dynamic Admission Metrics

| run directory | status | total ms | SQ cap | final window | BDP regions | accepted RDMA | overflow CXL | RDMA goodput B/ns | RDMA latency ns | CXL bytes | RDMA bytes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| /tmp/cxl-hybrid-warm-exp-hfeajir2/remap_xlarge_random_rw/hybrid_parallel_rdma_cxl-balanced-run01 | completed | 67 | 8 | 1 | 2 | 9 | 28 | 1.6435218267284948 | 1290575 | 35045376 | 8388608 |

## Interpretation

RDMA admission is valid for this sample only if the run completed,
destination-side in-memory guest latency used a primary dump, and
accepted plus overflow regions is non-zero. This run meets those
checks: RDMA accepted regions were non-zero and dynamic admission
overflowed additional candidates to CXL while keeping the SQ cap at 8.
Compare RDMA completed bytes with CXL worker bytes to determine whether
the dynamic window kept both lanes active or overflowed most work to CXL.
