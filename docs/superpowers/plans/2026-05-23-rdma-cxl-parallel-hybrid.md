# RDMA + CXL Parallel Hybrid Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an RDMA sidecar bulk path to normal hybrid migration while keeping CXL authoritative for destination visibility and using no-brake as the default experiment path.

**Architecture:** Branch implementation from `hybrid-experiment-infra`. Keep RDMA as a hybrid sidecar, not native QEMU RDMA postcopy. Track whole 2 MiB region ownership and source-authoritative RDMA-ready state; commit only still-current regions at switch. Disable source-side brake by default with `--x-cxl-disable-brake`, and leave dirty brake republish as postponed work.

**Tech Stack:** QEMU C, QAPI, migration control-plane code, trace events, unit tests under `tests/unit`, experiment scripts under `scripts/`, reports under `docs/superpowers/reports`.

---

## Current Baseline

Use `hybrid-experiment-infra` as the base for this plan. It contains:

- `abeeaddc7e scripts: add source fallback for guest latency dump`
- `65042ee02f scripts: add source-first guest latency dump mode`
- `cee142add3 scripts: add hybrid no-brake experiment option`
- `88817225fc docs: report no-brake hybrid baseline`

Do not branch RDMA+CXL work from `explore-hot-brake-cxl`. That worktree records
the hot-brake exploration and should remain frozen unless explicitly resumed.

The default experiment comparison for this plan is no-brake normal hybrid:

```bash
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
  --in-memory-guest-latency-source-first
```

### Paused Work

Do not implement the previous Task 4 brake fallback in this branch. The old
idea, "keep brake CXL-owned and add dirty republish fallback", is postponed
until a measured brake policy beats no-brake.

## Task 0: Create the isolated RDMA+CXL worktree

**Files:**
- No source files changed.

- [ ] **Step 1: Verify the infra worktree is clean**

Run:

```bash
git -C /home/xiexinchen/.config/superpowers/worktrees/qemu/hybrid-experiment-infra \
  status --short --branch
```

Expected: output begins with `## hybrid-experiment-infra` and has no modified
or untracked source files.

- [ ] **Step 2: Create the implementation worktree from infra**

Run:

```bash
git -C /home/xiexinchen/qemu worktree add \
  /home/xiexinchen/.config/superpowers/worktrees/qemu/rdma-cxl-parallel-hybrid \
  -b rdma-cxl-parallel-hybrid hybrid-experiment-infra
```

Expected: worktree is created at the path above and starts at the
`hybrid-experiment-infra` commit containing this plan/spec.

- [ ] **Step 3: Verify the implementation worktree**

Run:

```bash
git -C /home/xiexinchen/.config/superpowers/worktrees/qemu/rdma-cxl-parallel-hybrid \
  status --short --branch
git -C /home/xiexinchen/.config/superpowers/worktrees/qemu/rdma-cxl-parallel-hybrid \
  log --oneline -5
```

Expected: branch is `rdma-cxl-parallel-hybrid`, status is clean, and the recent
log includes the infra commits.

## Task 1: Add script-level mode and no-brake guardrails

**Files:**
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing script tests**

Add tests that assert:

1. `hybrid_parallel_rdma_cxl` is accepted as a migration mode;
2. `hybrid_parallel_rdma_cxl` uses CXL hybrid capabilities, mapped-ram, and postcopy;
3. `hybrid_parallel_rdma_cxl` does not use the native RDMA migration URI;
4. benchmark command construction for `hybrid_parallel_rdma_cxl` forwards
   `x-cxl-brake-enable=false` when `--x-cxl-disable-brake` is used;
5. default RDMA+CXL comparison rows include no-brake metrics.

Run:

```bash
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
```

Expected: the new tests fail because the new mode does not exist yet.

- [ ] **Step 2: Implement the minimal mode plumbing**

Update `MIGRATION_MODES`, `mode_uses_cxl_hybrid()`, `mode_uses_rdma()`,
`mode_uses_multifd()`, and command generation so:

- `hybrid_parallel_rdma_cxl` is a CXL hybrid mode;
- it keeps the unix migration socket rather than native `rdma:host:port`;
- it can later enable an RDMA sidecar parameter without using native RDMA mode;
- no-brake remains controlled by the existing `x-cxl-brake-enable=false`
  parameter.

- [ ] **Step 3: Run script tests**

Run:

```bash
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
python3 -m py_compile \
  scripts/cxl-hybrid-warm-experiment.py \
  scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: the new mode tests pass and Python syntax check passes.

- [ ] **Step 4: Commit the script mode**

Run:

```bash
git add scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "scripts: add parallel rdma cxl hybrid experiment mode"
```

## Task 2: Add measurement fields for RDMA invalidation amplification

**Files:**
- Modify: `trace-events`
- Modify: `migration/trace-events` if this tree uses a migration-local trace file
- Modify: `migration/cxl.c`
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-region.c`
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`
- Test: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write failing tests for counter accounting**

Add unit coverage for whole-region RDMA accounting helpers:

- marking a region RDMA-ready increments ready region/page counters;
- invalidating that region increments invalidated region and lost-page counters;
- a CXL fallback publication caused by RDMA invalidation increments
  `cxl_republish_pages_due_to_rdma_invalidate`;
- invalidating the same stale region twice does not double count lost pages.

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: the new tests fail because the accounting helpers do not exist yet.

- [ ] **Step 2: Add trace and query fields**

Add fields with these exact names to the script parser output and CSV rows:

- `rdma_ready_regions`
- `rdma_ready_pages`
- `rdma_invalidated_regions`
- `rdma_ready_pages_lost`
- `cxl_republish_regions_due_to_rdma_invalidate`
- `cxl_republish_pages_due_to_rdma_invalidate`
- `rdma_invalidate_publish_amplification`

The amplification value is computed as:

```text
cxl_republish_pages_due_to_rdma_invalidate / max(rdma_ready_pages_lost, 1)
```

- [ ] **Step 3: Implement minimal source-side accounting**

Add source-side counters and trace events only. This task does not need to move
data over RDMA yet. A region can be marked ready or invalidated by test-only
helpers or by a shadow path.

- [ ] **Step 4: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
```

Expected: unit and script tests pass.

- [ ] **Step 5: Commit the measurement counters**

Run:

```bash
git add trace-events migration/cxl.c migration/cxl.h migration/cxl-region.c \
        scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py \
        tests/unit/test-cxl-hybrid-region.c
git commit -m "migration/cxl: measure rdma fallback amplification"
```

## Task 3: Add handoff CXL capacity reporting

**Files:**
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`
- Modify: `migration/migration.c`
- Modify: `migration/trace-events` if additional timeline events are needed

- [ ] **Step 1: Write failing parser tests**

Add script tests that feed synthetic trace lines and assert these fields are
reported:

- `handoff_control_src_start_to_dst_vm_started_ms`
- `handoff_control_src_start_to_dst_ack_ms`
- `handoff_control_src_start_to_downtime_end_ms`
- `handoff_control_src_downtime_end_to_postcopy_active_ms`
- `handoff_control_src_postcopy_active_to_completion_enter_ms`
- `handoff_control_src_completion_enter_to_completed_ms`
- `handoff_region_publish_pages`
- `handoff_region_publish_time_ns`
- `handoff_postcopy_region_wait_time_ns`
- `handoff_postcopy_fault_total_time_ns`

Run:

```bash
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
```

Expected: any new or renamed fields fail before parser support is complete.

- [ ] **Step 2: Extend timeline parsing only where needed**

Reuse existing timeline events where they already provide the fields. Add trace
points only for missing boundaries that cannot be derived from current QEMU
trace output.

- [ ] **Step 3: Run script tests**

Run:

```bash
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
python3 -m py_compile \
  scripts/cxl-hybrid-warm-experiment.py \
  scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: handoff reporting tests pass.

- [ ] **Step 4: Commit the handoff reporting**

Run:

```bash
git add scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py \
        migration/migration.c migration/trace-events
git commit -m "scripts: report handoff cxl capacity metrics"
```

## Task 4: Add source-authoritative RDMA sidecar state

**Files:**
- Modify: `qapi/migration.json`
- Modify: `migration/options.c`
- Modify: `migration/options.h`
- Modify: `migration/cxl.h`
- Modify: `migration/cxl.c`
- Modify: `migration/cxl-region.c`
- Test: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write failing unit tests**

Add unit coverage for:

1. a region can be owned by exactly one bulk lane at a time;
2. RDMA-ready pages are cleared when the source dirties them again;
3. switch-time commit only exposes pages still current after final dirty sync;
4. brake-disabled mode does not call RDMA brake fallback code.

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: tests fail because ownership and RDMA-ready helpers do not exist yet.

- [ ] **Step 2: Add the experimental migration option**

Add an off-by-default migration parameter named `x-cxl-rdma-sidecar` and wire it
through QAPI and migration options. This option enables the sidecar only for
hybrid modes.

- [ ] **Step 3: Implement whole-region ownership state**

Add helper APIs equivalent to:

```c
bool cxl_hybrid_region_is_rdma_owned(uint64_t region_index);
bool cxl_hybrid_region_try_own_rdma(uint64_t region_index);
void cxl_hybrid_region_drop_rdma(uint64_t region_index);
void cxl_hybrid_mark_region_rdma_ready(uint64_t region_index);
void cxl_hybrid_invalidate_region_rdma_ready(uint64_t region_index);
```

Keep the first version whole-region only. Do not add 256 KiB or 4 KiB ownership
in this task.

- [ ] **Step 4: Run unit tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: ownership and invalidation tests pass.

- [ ] **Step 5: Commit the state model**

Run:

```bash
git add qapi/migration.json migration/options.c migration/options.h \
        migration/cxl.h migration/cxl.c migration/cxl-region.c \
        tests/unit/test-cxl-hybrid-region.c
git commit -m "migration/cxl: add rdma sidecar ownership state"
```

## Task 5: Add a measurement-only RDMA sidecar spike

**Files:**
- Create: `migration/cxl-rdma.c`
- Create: `migration/cxl-rdma.h`
- Modify: `migration/meson.build`
- Modify: `migration/cxl.c`
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing tests**

Add tests for:

1. enabling `hybrid_parallel_rdma_cxl` sets `x-cxl-rdma-sidecar=true`;
2. parsed output includes RDMA bulk bytes and RDMA-ready pages;
3. no-brake remains enabled in the RDMA+CXL smoke command;
4. baseline normal hybrid command generation remains unchanged.

Run:

```bash
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
```

Expected: the new RDMA sidecar assertions fail.

- [ ] **Step 2: Implement the minimal sidecar module**

Add a sidecar module that can accept whole 2 MiB region work items and report:

- `rdma_bulk_regions`
- `rdma_bulk_bytes`
- `rdma_ready_regions`
- `rdma_ready_pages`

The first spike may use a direct local copy or a shadow data path if real RDMA
transport setup would obscure the ownership and measurement logic. It must not
publish destination visibility directly.

- [ ] **Step 3: Run script tests and a no-brake smoke benchmark**

Run:

```bash
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
timeout 1200s sudo -n numactl --cpunodebind=4-7 --membind=4-7 \
  python3 scripts/cxl-hybrid-warm-experiment.py \
    --keep-dir \
    --pressure remap_xlarge_random_rw \
    --mode hybrid_parallel_rdma_cxl \
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

Expected: tests pass and the smoke run emits RDMA sidecar measurement fields.

- [ ] **Step 4: Commit the measurement spike**

Run:

```bash
git add migration/cxl-rdma.c migration/cxl-rdma.h migration/meson.build \
        migration/cxl.c scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration/cxl: add rdma sidecar measurement spike"
```

## Task 6: Wire whole-region bulk parallel ownership

**Files:**
- Modify: `migration/cxl.c`
- Modify: `migration/cxl-region.c`
- Modify: `migration/postcopy.c`
- Modify: `migration/migration.c`
- Modify: `tests/unit/test-cxl-hybrid-region.c`
- Modify: `tests/unit/test-migration-postcopy.c`

- [ ] **Step 1: Write failing unit tests**

Add tests that show:

1. RDMA-owned regions stay out of the CXL bulk/remap path;
2. CXL-owned regions never become RDMA-ready in the same generation;
3. dirty invalidation clears RDMA-ready before switch;
4. switch commits only RDMA-ready regions that survived final dirty sync;
5. invalidated RDMA regions fall back to CXL postcopy publication, not brake.

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region tests/unit/test-migration-postcopy
./build/tests/unit/test-cxl-hybrid-region --tap
./build/tests/unit/test-migration-postcopy --tap
```

Expected: ownership and switch assertions fail before implementation.

- [ ] **Step 2: Implement region scheduling**

Update the hybrid bulk path so:

- RDMA handles selected cold whole regions;
- CXL handles hot, dirty, or unassigned regions;
- final dirty sync downgrades stale RDMA-ready regions;
- no-brake switch goes directly to postcopy without source-side brake drains.

- [ ] **Step 3: Run unit and script tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region tests/unit/test-migration-postcopy
./build/tests/unit/test-cxl-hybrid-region --tap
./build/tests/unit/test-migration-postcopy --tap
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
```

Expected: tests pass.

- [ ] **Step 4: Commit the bulk wiring**

Run:

```bash
git add migration/cxl.c migration/cxl-region.c migration/postcopy.c \
        migration/migration.c tests/unit/test-cxl-hybrid-region.c \
        tests/unit/test-migration-postcopy.c
git commit -m "migration/cxl: wire rdma cxl parallel bulk"
```

## Task 7: Run a small benchmark matrix and write the report

**Files:**
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`
- Create: `docs/superpowers/reports/2026-05-25-rdma-cxl-parallel-hybrid-results.md`

- [ ] **Step 1: Add benchmark matrix tests**

Add a script test that asserts the RDMA+CXL matrix can run:

- `hybrid_postcopy_auto` with `--x-cxl-disable-brake`;
- `hybrid_parallel_rdma_cxl` with `--x-cxl-disable-brake`;
- `pure_precopy`;
- `native_postcopy_stream`;
- `native_rdma_precopy` when an RDMA endpoint is configured.

Run:

```bash
python3 -m unittest \
  scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
```

Expected: matrix tests pass after any needed orchestration updates.

- [ ] **Step 2: Run the no-brake comparison**

Run at least one repeat for:

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

Expected: both modes complete and emit comparable stall, handoff, postcopy, and
RDMA amplification fields.

- [ ] **Step 3: Write the report**

Create `docs/superpowers/reports/2026-05-25-rdma-cxl-parallel-hybrid-results.md`
with:

- exact branch and commit;
- exact command;
- result JSON and result directory paths;
- total time and downtime;
- in-memory guest stall;
- postcopy region wait and fault time;
- region publish pages and time;
- RDMA-ready, invalidated, and committed counts;
- CXL fallback amplification;
- conclusion on whether to keep whole-region ownership, reduce granularity, or
  pause RDMA+CXL.

- [ ] **Step 4: Commit the report**

Run:

```bash
git add scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py \
        docs/superpowers/reports/2026-05-25-rdma-cxl-parallel-hybrid-results.md
git commit -m "docs: report rdma cxl parallel hybrid baseline"
```

## Rollback Notes

The RDMA+CXL implementation branch can be abandoned without affecting:

- `hybrid-postcopy-migration` main worktree;
- `explore-hot-brake-cxl` hot-brake records;
- `hybrid-experiment-infra` shared experiment scripts.

If an individual implementation commit needs rollback, use `git revert <commit>`
from the `rdma-cxl-parallel-hybrid` worktree. Do not reset shared branches.
