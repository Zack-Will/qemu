# CXL Hot Brake Strategy Exploration Round 4

Date: 2026-05-25

## Goal

Add a no-brake baseline for normal hybrid migration.

The baseline disables the source-side hot brake/remap phase and leaves the
remaining CXL publication work to postcopy. This is needed to answer whether
the current brake strategies actually reduce stall, or only move work from
postcopy into the precopy/brake window.

## Version State

Worktree:

```text
/home/xiexinchen/.config/superpowers/worktrees/qemu/explore-hot-brake-cxl
```

New script commit:

```text
edb4c9f780 scripts: add hybrid no-brake experiment option
```

Rollback for the no-brake experiment option:

```bash
git revert edb4c9f780
```

The QEMU migration core is unchanged in this round. The new CLI option only
sets the existing migration parameter:

```text
--x-cxl-disable-brake -> x-cxl-brake-enable=false
```

## Validation

Script tests and syntax check:

```bash
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_build_migration_parameters_can_disable_hybrid_brake \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_disable_brake_cli_sets_brake_enable_false \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_run_pressure_matrix_forwards_brake_enable \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_fault_resolve_mode_cli_passes_override_to_run_matrix \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_main_passes_prefetch_install_fault_control_and_cxl_path_overrides_to_run_matrix

python3 -m py_compile \
  scripts/cxl-hybrid-warm-experiment.py \
  scripts/cxl-hybrid-warm-experiment-test.py
```

Result:

- 5/5 targeted tests passed.
- Python syntax check passed.

One unrelated existing test remains inconsistent with the current script
defaults:

```text
test_set_params_hybrid_auto_defaults_prefetch_rate_to_zero
```

It expects `max-bandwidth=8 MiB/s`, while the current helper default is
`max_bandwidth=0`. This predates the no-brake change and was not modified in
this round.

## Command

No-brake baseline:

```bash
timeout 1200s sudo -n env \
  QEMU_CXL_HOT_BRAKE_DELTA=1 \
  QEMU_CXL_HOT_BRAKE_DELTA_BUDGET_PAGES=64 \
  QEMU_CXL_HOT_BRAKE_MIN_SAVED_PAGES=8 \
  QEMU_CXL_HOT_BRAKE_NO_PROGRESS_DRAINS=0 \
  QEMU_CXL_HOT_BRAKE_MIN_PAGES_PER_MS=0 \
  numactl --cpunodebind=4-7 --membind=4-7 \
  python3 scripts/cxl-hybrid-warm-experiment.py \
    --keep-dir \
    --pressure remap_xlarge_random_rw \
    --mode hybrid_postcopy_auto \
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
    --in-memory-guest-latency-source-first \
  | tee /tmp/cxl-hot-brake-disabled-sourcefirst-r1.json
```

Raw output:

| case | output JSON | result dir |
| --- | --- | --- |
| no-brake | `/tmp/cxl-hot-brake-disabled-sourcefirst-r1.json` | `/tmp/cxl-hybrid-warm-exp-4tqmj7vz/remap_xlarge_random_rw/hybrid_postcopy_auto-custom-run01` |

Comparison inputs from earlier rounds:

| case | output JSON | result dir |
| --- | --- | --- |
| no-progress=1 | `/tmp/cxl-hot-brake-noprogress1-sourcefirst-r1.json` | `/tmp/cxl-hybrid-warm-exp-9jgi0e23/remap_xlarge_random_rw/hybrid_postcopy_auto-custom-run01` |
| marginal=4096 | `/tmp/cxl-hot-brake-marginal4096-stats-r1.json` | `/tmp/cxl-hybrid-warm-exp-x1fqh199/remap_xlarge_random_rw/hybrid_postcopy_auto-custom-run01` |

## Results

No-brake behavior was visible in the final counters:

- `remap-attempts=0`
- `remap-successes=0`
- `remapped-regions=0`
- `remap-coverage=0`
- `remap-scan-calls=0`
- `remap-pause-calls=0`
- `control_src_enter_brake_to_request_postcopy_ms=null`

The switch reason changed from `gain-collapsed` in the brake-enabled samples to
`precopy-complete` in this baseline. Postcopy still handled region publication:
`region-publish-pages=15934` and `trace_dst_region_remap=13`.

### End-to-End

| case | total ms | downtime ms | switch reason | switch iter | remap regions | coverage | backing write MiB |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| no-progress=1 | 42 | 0 | gain-collapsed | 6 | 3 | 3% | 67.82 |
| marginal=4096 | 38 | 1 | gain-collapsed | 4 | 3 | 3% | 35.90 |
| no-brake | 34 | 1 | precopy-complete | 2 | 0 | 0% | 19.49 |

### Brake And Postcopy Cost

| case | brake control ms | postcopy active->completion ms | completion enter->completed ms | publish pages | publish ms | region wait ms | stream fault pause ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| no-progress=1 | 10.255 | 8.216 | 2.887 | 16014 | 7.746 | 5.158 | 4.964 |
| marginal=4096 | 3.112 | 9.259 | 2.924 | 16176 | 8.994 | 6.561 | 6.412 |
| no-brake | n/a | 6.192 | 2.480 | 15934 | 5.259 | 3.106 | 3.270 |

### Source-Side In-Memory Guest Stall

Source-first dump observes source-side windows. It does not provide a complete
destination postcopy guest window.

| case | precopy stall ms | bulk stall ms | brake stall ms | note |
| --- | ---: | ---: | ---: | --- |
| no-progress=1 | 4.740 | 1.680 | 3.050 | complete source brake window |
| marginal=4096 | n/a | 1.888 | n/a | brake window ended too quickly for complete source markers |
| no-brake | 2.158 | n/a | n/a | no brake markers by design |

### Brake Internal Work

| case | remap pause calls | remap pause ms | remap copy KiB | remap scan calls | dirty sync calls |
| --- | ---: | ---: | ---: | ---: | ---: |
| no-progress=1 | 2 | 0.328 | 252 | 112 | 8 |
| marginal=4096 | 1 | 0.247 | 192 | 38 | 5 |
| no-brake | 0 | 0.000 | 0 | 0 | 2 |

## Analysis

This baseline is a strong negative signal for the current brake strategy under
`remap_xlarge_random_rw`.

The brake-enabled policies remapped only 3 regions, or 3% coverage. That small
coverage did not noticeably reduce the postcopy region-publish footprint:

- no-progress=1 published 16014 pages
- marginal=4096 published 16176 pages
- no-brake published 15934 pages

In this sample, no-brake was also better on the measured postcopy control path:

- region wait fell from 5.158-6.561 ms to 3.106 ms
- stream fault pause fell from 4.964-6.412 ms to 3.270 ms
- postcopy active-to-completion fell from 8.216-9.259 ms to 6.192 ms

This means the current brake work did not buy enough postcopy reduction to pay
for its own source-side stall. The no-progress rule is still useful as a safety
stop compared with unbounded brake, but this round shows that even bounded brake
is not automatically better than skipping brake entirely.

The most likely reason is not just the remap granule. It is the marginal value
of source-side remap under this workload and policy shape:

- brake remaps only a few clean continuous regions;
- most pages still need postcopy region publication;
- the source still pays dirty sync, scan, pause, and remap bookkeeping before
  reaching postcopy;
- aggressive marginal stopping reduces source brake time but can increase
  postcopy wait/publish.

No-brake also wrote less backing data in this sample. That does not prove a
general bandwidth win yet, because single-run variance and switch reason
differences matter, but it reinforces that current brake is not reducing enough
work in this workload.

## Implications

Before adding more brake policy complexity, the next criterion should be:

```text
enable source-side brake only when it can prove positive marginal value
against the no-brake baseline
```

A practical gate could be:

- run a very small bounded brake probe;
- measure remapped pages and postcopy-avoided pages per millisecond;
- continue brake only if the measured yield exceeds the no-brake cost model;
- otherwise switch immediately without further brake drains.

Round 4 also suggests that future experiments should report no-brake in every
strategy table. Without it, a strategy can look better than another brake
variant while still losing to postcopy-only handling.

## Next Round

Recommended next step:

1. Repeat no-brake for at least 3 runs to estimate variance.
2. Compare against `NO_PROGRESS_DRAINS=1` and `MIN_PAGES_PER_MS=4096` with the
   same repeat count.
3. If no-brake remains equal or better, change the exploration target from
   "better brake drain" to "when should brake be enabled at all?"

Do not promote marginal stop or no-progress stop as default yet. The current
best experimental default for this workload is no-brake, but this is based on a
single baseline run.
