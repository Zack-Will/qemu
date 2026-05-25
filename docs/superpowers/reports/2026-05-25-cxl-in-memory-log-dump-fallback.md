# CXL In-Memory Log Dump Fallback

Date: 2026-05-25

## Problem

After early no-progress brake exit, destination-side HMP `pmemsave` repeatedly
dumped only the 32-byte in-memory latency header and then timed out while
reading the ring body.

Observed failed header:

```text
magic=0x4d4c5843 version=1 capacity=4194304 count=1048569
target dump size=4194308 bytes
actual dump size=32 bytes
```

This means the guest log was initialized and the header page was readable. The
failure is in reading the cold ring body from destination guest RAM after
postcopy/CXL completion.

## Fix

The experiment script now falls back to source QMP when destination dumping
fails without any usable latency records.

Behavior:

- Try destination latency and marker dump first.
- Preserve the old partial-dump behavior if the destination dump contains
  usable samples.
- If destination has only a header or no records, dump latency and marker rings
  from source QMP into `guest-in-memory-*-src.bin`.
- Mark fallback reports with:
  - `dump_source: "fallback"`
  - `fallback_source_only: true`
  - `primary_dump_error`
  - `primary_partial_bytes`

Interpretation:

- Source fallback restores source-side windows such as precopy bulk and brake.
- It does not claim corrected full-window or postcopy stall if the source marker
  does not contain destination-side events.

## Validation

Targeted tests:

```bash
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_dump_guest_physical_memory_quotes_hmp_filename \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_dump_guest_physical_memory_uses_long_timeout_temporarily \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_run_case_in_memory_latency_omits_debugcon_heartbeat \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_run_case_in_memory_latency_passes_source_fallback_dump \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_collect_in_memory_guest_latency_parses_partial_dump_after_timeout \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_collect_in_memory_guest_latency_dumps_only_recorded_samples \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_collect_in_memory_guest_latency_collects_marker_after_latency_timeout \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_collect_in_memory_guest_latency_falls_back_to_source_dump \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest.test_dump_guest_physical_memory_chunked_appends_chunks

Ran 9 tests in 1.024s
OK

python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py scripts/cxl-hybrid-warm-experiment-test.py
```

Integration check:

```text
result JSON: /tmp/cxl-hot-brake-noprogress3-logfix-r1.json
result dir:  /tmp/cxl-hybrid-warm-exp-mmra8rca
```

Key fields:

| field | value |
| --- | ---: |
| `dump_source` | `fallback` |
| `fallback_source_only` | `true` |
| `primary_partial_bytes` | 32 |
| source latency samples | 845576 |
| source marker samples | 845560 |
| precopy bulk no-marker stall | 2.011 ms |
| precopy brake no-marker stall | 3.806 ms |
| brake control | 11.916 ms |
| total migration time | 47 ms |

The destination full-window and postcopy windows remain unavailable in this
run, which is correct for source fallback.
