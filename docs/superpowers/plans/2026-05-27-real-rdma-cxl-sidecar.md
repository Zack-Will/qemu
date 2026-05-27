# Real RDMA CXL Sidecar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the memcpy/shadow RDMA+CXL sidecar with a real RDMA verbs sidecar that runs as an asynchronous secondary bulk stream while CXL remains the primary migration and visibility path.

**Architecture:** Keep the main hybrid migration URI on the existing CXL path, normally `unix:<mig_sock>`, and add a separate RDMA CM/verbs sidecar endpoint configured with the native QEMU RDMA address shape. The sidecar worker opportunistically claims whole dirty bulk regions, posts real `IBV_WR_RDMA_WRITE` operations, and only marks source-authoritative `rdma_ready` state when the completion is still current. CXL publication is always allowed to win the race; RDMA-ready regions are committed to destination visibility only through the existing CXL hybrid control path after final dirty synchronization.

**Tech Stack:** QEMU C migration code, QAPI, rdma-core (`librdmacm`, `libibverbs`), QemuThread/QemuMutex, migration dirty bitmap helpers, trace events, GLib unit tests, Python experiment runner/unittest.

---

## Current Baseline

Work in:

```bash
cd /home/xiexinchen/.config/superpowers/worktrees/qemu/rdma-cxl-parallel-hybrid
```

The design spec is:

```text
docs/superpowers/specs/2026-05-26-rdma-cxl-real-sidecar-design.md
```

The current branch contains a measurement spike that must be replaced:

- `migration/cxl-rdma.c` uses `memcpy()` and `cxl_rdma_sidecar_submit_shadow_region()`.
- `migration/cxl-region.c` uses exclusive RDMA/CXL region ownership, so RDMA in-flight work blocks CXL bulk publication.
- `migration/ram.c:ram_save_host_page()` chooses and completes RDMA bulk work inline in the main RAM scan.
- `scripts/cxl-hybrid-warm-experiment.py` enables `x-cxl-rdma-sidecar=true` but does not pass a real sidecar RDMA endpoint.

Keep the existing user-facing mode name `hybrid_parallel_rdma_cxl`. Native RDMA precopy remains separate and still uses a main migration URI of `rdma:<host>:<port>`.

## File Structure

Modify these existing files:

- `qapi/migration.json`: add sidecar parameters and query-migrate fields using `MigrationAddress`.
- `migration/options.h`: expose sidecar address and budget accessors.
- `migration/options.c`: default, apply, clone/free, and validate the new sidecar parameters.
- `migration/cxl.h`: replace exclusive ownership structs with region state for candidate, in-flight, ready, stale, committed, and invalidated regions; add sidecar transport stats.
- `migration/cxl-region.c`: implement region-state transitions, stale completion, CXL race accounting, invalidated-region republish accounting, and test helpers.
- `migration/cxl-rdma.h`: expose a real sidecar lifecycle API and remove memcpy/shadow submit APIs.
- `migration/cxl-rdma.c`: implement the real RDMA CM/verbs endpoint, worker loop, memory registration, write posting, completion polling, stats, and `CONFIG_RDMA` stubs.
- `migration/cxl.c`: initialize/stop the sidecar around mapped CXL backing setup, provide region geometry for RDMA writes, and remove inline memcpy completion.
- `migration/ram.c`: remove main-loop RDMA ownership/completion; add a narrow dirty-bitmap claim helper used by the sidecar scheduler.
- `migration/trace-events`: add trace points for sidecar connect, register, schedule, post, complete, stale, failure, and commit.
- `scripts/cxl-hybrid-warm-experiment.py`: build the sidecar RDMA address from `--rdma-host/--rdma-port`, keep main URI as `unix:`, pass budgets, parse new metrics.
- `scripts/cxl-hybrid-warm-experiment-test.py`: test URI, params, capability, parser, and summary behavior.
- `tests/unit/test-cxl-hybrid-region.c`: replace old memcpy/exclusive-ownership tests with opportunistic state and claim/commit tests.
- `migration/meson.build`: change only if a split source file is introduced; otherwise keep `cxl-rdma.c` always built with `#ifdef CONFIG_RDMA` internally.

Do not add a new native migration transport. The sidecar is owned by CXL hybrid code and must not call native RDMA as the main RAM migration path.

## Task 1: QAPI And Script Parameter Plumbing

**Files:**
- Modify: `qapi/migration.json`
- Modify: `migration/options.h`
- Modify: `migration/options.c`
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing script tests for sidecar address plumbing**

Add tests to `scripts/cxl-hybrid-warm-experiment-test.py` that assert the sidecar mode keeps the main URI on `unix:` and passes a native RDMA-shaped sidecar address. Use the existing `WarmExperimentScriptTest` helpers near the current native RDMA tests.

```python
def test_parallel_rdma_cxl_uses_unix_main_uri_and_sidecar_rdma_param(self):
    args = self.parse_args([
        "--mode", "hybrid_parallel_rdma_cxl",
        "--rdma-host", "192.0.2.10",
        "--rdma-port", "7471",
    ])

    uri = exp.build_migration_uri(args, "hybrid_parallel_rdma_cxl", 3,
                                  pathlib.Path("/tmp/run"))
    self.assertTrue(uri.startswith("unix:"))

    params = exp.build_migration_params(args, "hybrid_parallel_rdma_cxl", 3)
    self.assertTrue(params["x-cxl-rdma-sidecar"])
    self.assertEqual(params["x-cxl-rdma-sidecar-address"], {
        "transport": "rdma",
        "rdma": {"host": "192.0.2.10", "port": "7474"},
    })
    self.assertEqual(params["x-cxl-rdma-sidecar-max-inflight-regions"], 1)
    self.assertEqual(params["x-cxl-rdma-sidecar-max-cover-percent"], 25)


def test_parallel_rdma_cxl_uses_existing_rdma_pin_all_capability(self):
    args = self.parse_args([
        "--mode", "hybrid_parallel_rdma_cxl",
        "--rdma-host", "192.0.2.10",
        "--rdma-port", "7471",
        "--rdma-pin-all",
    ])

    caps = exp.build_migration_caps(args, "hybrid_parallel_rdma_cxl")
    self.assertIn({"capability": "rdma-pin-all", "state": True}, caps)
```

Run:

```bash
python3 -m unittest scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
```

Expected: FAIL because `x-cxl-rdma-sidecar-address` and sidecar budgets are not implemented.

- [ ] **Step 2: Add QAPI migration parameters**

In `qapi/migration.json`, add these `MigrationParameter` enum entries next to `x-cxl-rdma-sidecar`:

```json
{ "name": "x-cxl-rdma-sidecar-address",
  "features": [ "unstable" ] },
{ "name": "x-cxl-rdma-sidecar-max-inflight-regions",
  "features": [ "unstable" ] },
{ "name": "x-cxl-rdma-sidecar-max-cover-percent",
  "features": [ "unstable" ] },
{ "name": "x-cxl-rdma-sidecar-region-bytes",
  "features": [ "unstable" ] }
```

Add the documented fields to `MigrationParameters`:

```json
"*x-cxl-rdma-sidecar-address": {
    "type": "MigrationAddress", "features": [ "unstable" ] },
"*x-cxl-rdma-sidecar-max-inflight-regions": {
    "type": "uint32", "features": [ "unstable" ] },
"*x-cxl-rdma-sidecar-max-cover-percent": {
    "type": "uint8", "features": [ "unstable" ] },
"*x-cxl-rdma-sidecar-region-bytes": {
    "type": "size", "features": [ "unstable" ] }
```

Update the QAPI comments so the sidecar address is explicitly described as a secondary endpoint and not the main migration URI.

- [ ] **Step 3: Add option defaults and accessors**

In `migration/options.c`, define defaults near the other CXL defaults:

```c
#define DEFAULT_MIGRATE_X_CXL_RDMA_SIDECAR_MAX_INFLIGHT_REGIONS 1
#define DEFAULT_MIGRATE_X_CXL_RDMA_SIDECAR_MAX_COVER_PERCENT 25
#define DEFAULT_MIGRATE_X_CXL_RDMA_SIDECAR_REGION_BYTES 0
```

Add helpers in `migration/options.h`:

```c
const MigrationAddress *migrate_cxl_rdma_sidecar_address(void);
uint32_t migrate_cxl_rdma_sidecar_max_inflight_regions(void);
uint8_t migrate_cxl_rdma_sidecar_max_cover_percent(void);
uint64_t migrate_cxl_rdma_sidecar_region_bytes(void);
```

Implement them in `migration/options.c`:

```c
const MigrationAddress *migrate_cxl_rdma_sidecar_address(void)
{
    MigrationState *s = migrate_get_current();

    return s->parameters.x_cxl_rdma_sidecar_address;
}

uint32_t migrate_cxl_rdma_sidecar_max_inflight_regions(void)
{
    MigrationState *s = migrate_get_current();

    return s->parameters.x_cxl_rdma_sidecar_max_inflight_regions;
}

uint8_t migrate_cxl_rdma_sidecar_max_cover_percent(void)
{
    MigrationState *s = migrate_get_current();

    return s->parameters.x_cxl_rdma_sidecar_max_cover_percent;
}

uint64_t migrate_cxl_rdma_sidecar_region_bytes(void)
{
    MigrationState *s = migrate_get_current();

    return s->parameters.x_cxl_rdma_sidecar_region_bytes;
}
```

Update `migrate_params_init()`, `migrate_mark_all_params_present()`, `migrate_params_test_apply()`, and `migrate_params_apply()` so all new fields are copied. Use `QAPI_CLONE(MigrationAddress, ...)` and `qapi_free_MigrationAddress(...)` for the address field.

- [ ] **Step 4: Add validation**

In `migrate_params_check()`, add explicit validation:

```c
if (params->x_cxl_rdma_sidecar) {
    if (!migrate_cxl_hybrid()) {
        error_setg(errp, "x-cxl-rdma-sidecar requires x-cxl-hybrid");
        return false;
    }
    if (!params->x_cxl_rdma_sidecar_address ||
        params->x_cxl_rdma_sidecar_address->transport !=
            MIGRATION_ADDRESS_TYPE_RDMA) {
        error_setg(errp,
                   "x-cxl-rdma-sidecar requires an RDMA sidecar address");
        return false;
    }
    if (params->x_cxl_rdma_sidecar_max_inflight_regions < 1) {
        error_setg(errp,
                   "x-cxl-rdma-sidecar-max-inflight-regions must be at least 1");
        return false;
    }
    if (params->x_cxl_rdma_sidecar_max_cover_percent > 100) {
        error_setg(errp,
                   "x-cxl-rdma-sidecar-max-cover-percent must be between 0 and 100");
        return false;
    }
}
```

Add this `CONFIG_RDMA` build check in the same block:

```c
#ifndef CONFIG_RDMA
    if (params->x_cxl_rdma_sidecar) {
        error_setg(errp,
                   "x-cxl-rdma-sidecar requires QEMU to be built with RDMA support");
        return false;
    }
#endif
```

- [ ] **Step 5: Update experiment script params and CLI**

In `scripts/cxl-hybrid-warm-experiment.py`, add CLI options:

```python
parser.add_argument("--x-cxl-rdma-sidecar-max-inflight-regions",
                    type=int, default=1)
parser.add_argument("--x-cxl-rdma-sidecar-max-cover-percent",
                    type=int, default=25)
parser.add_argument("--x-cxl-rdma-sidecar-region-bytes",
                    type=int, default=0)
```

Add a helper:

```python
def build_rdma_sidecar_address(args, run_index: int) -> dict[str, object]:
    port = str(int(args.rdma_port) + run_index)
    return {
        "transport": "rdma",
        "rdma": {"host": args.rdma_host, "port": port},
    }
```

In the sidecar mode params, set:

```python
params["x-cxl-rdma-sidecar"] = True
params["x-cxl-rdma-sidecar-address"] = build_rdma_sidecar_address(
    args, run_index)
params["x-cxl-rdma-sidecar-max-inflight-regions"] = \
    args.x_cxl_rdma_sidecar_max_inflight_regions
params["x-cxl-rdma-sidecar-max-cover-percent"] = \
    args.x_cxl_rdma_sidecar_max_cover_percent
params["x-cxl-rdma-sidecar-region-bytes"] = \
    args.x_cxl_rdma_sidecar_region_bytes
```

Change `set_caps()` so `rdma-pin-all` is enabled when either native RDMA or sidecar RDMA is active:

```python
if args.rdma_pin_all and (mode_uses_rdma(mode) or
                          mode_uses_cxl_rdma_sidecar(mode)):
    caps.append({"capability": "rdma-pin-all", "state": True})
```

- [ ] **Step 6: Run QAPI and script tests**

Run:

```bash
ninja -C build qapi-gen
python3 -m unittest scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: QAPI generation succeeds and the new script tests pass.

- [ ] **Step 7: Commit parameter plumbing**

Run:

```bash
git add qapi/migration.json migration/options.h migration/options.c \
        scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration: add real rdma sidecar parameters"
```

## Task 2: Sidecar Metrics And Trace Events

**Files:**
- Modify: `qapi/migration.json`
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-region.c`
- Modify: `migration/cxl.c`
- Modify: `migration/trace-events`
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`
- Modify: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write failing unit tests for new stats**

Replace the old accounting-only expectations with stats that distinguish posted, completed, stale, failed, budget, and CXL-race outcomes.

Add this test in `tests/unit/test-cxl-hybrid-region.c`:

```c
static void test_rdma_sidecar_transport_stats_accounting(void)
{
    CXLHybridRDMASidecarStats stats;

    cxl_hybrid_reset_rdma_sidecar_stats_for_test();
    cxl_hybrid_account_rdma_sidecar_connect(1000);
    cxl_hybrid_account_rdma_sidecar_registered(2 * MiB);
    cxl_hybrid_account_rdma_sidecar_posted(3, 2 * MiB);
    cxl_hybrid_account_rdma_sidecar_completed(3, 2 * MiB);
    cxl_hybrid_account_rdma_sidecar_stale(4, 2 * MiB, true);
    cxl_hybrid_account_rdma_sidecar_failed(5);
    cxl_hybrid_account_rdma_sidecar_no_candidate();
    cxl_hybrid_account_rdma_sidecar_budget_skip();
    cxl_hybrid_get_rdma_sidecar_stats(&stats);

    g_assert_cmpuint(stats.rdma_sidecar_connect_time_ns, ==, 1000);
    g_assert_cmpuint(stats.rdma_sidecar_registered_bytes, ==, 2 * MiB);
    g_assert_cmpuint(stats.rdma_sidecar_posted_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_posted_bytes, ==, 2 * MiB);
    g_assert_cmpuint(stats.rdma_sidecar_completed_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_completed_bytes, ==, 2 * MiB);
    g_assert_cmpuint(stats.rdma_sidecar_stale_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_cxl_race_lost_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_failed_regions, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_no_candidate_events, ==, 1);
    g_assert_cmpuint(stats.rdma_sidecar_budget_skip_events, ==, 1);

    cxl_hybrid_reset_rdma_sidecar_stats_for_test();
}
```

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: FAIL because the new stats helpers and fields do not exist.

- [ ] **Step 2: Extend C stats structs**

In `migration/cxl.h`, extend `CXLHybridRDMASidecarStats`:

```c
uint64_t rdma_sidecar_connect_time_ns;
uint64_t rdma_sidecar_registered_bytes;
uint64_t rdma_sidecar_posted_regions;
uint64_t rdma_sidecar_posted_bytes;
uint64_t rdma_sidecar_completed_regions;
uint64_t rdma_sidecar_completed_bytes;
uint64_t rdma_sidecar_stale_regions;
uint64_t rdma_sidecar_cxl_race_lost_regions;
uint64_t rdma_sidecar_failed_regions;
uint64_t rdma_sidecar_no_candidate_events;
uint64_t rdma_sidecar_budget_skip_events;
uint32_t rdma_sidecar_max_inflight_regions;
uint8_t rdma_sidecar_max_cover_percent;
bool rdma_sidecar_failed;
```

Add helper declarations:

```c
void cxl_hybrid_account_rdma_sidecar_connect(uint64_t time_ns);
void cxl_hybrid_account_rdma_sidecar_registered(uint64_t bytes);
void cxl_hybrid_account_rdma_sidecar_posted(uint64_t region_index,
                                            uint64_t bytes);
void cxl_hybrid_account_rdma_sidecar_completed(uint64_t region_index,
                                               uint64_t bytes);
void cxl_hybrid_account_rdma_sidecar_stale(uint64_t region_index,
                                           uint64_t bytes,
                                           bool cxl_race_lost);
void cxl_hybrid_account_rdma_sidecar_failed(uint64_t region_index);
void cxl_hybrid_account_rdma_sidecar_no_candidate(void);
void cxl_hybrid_account_rdma_sidecar_budget_skip(void);
void cxl_hybrid_set_rdma_sidecar_budget_stats(uint32_t max_inflight,
                                              uint8_t max_cover_percent);
```

- [ ] **Step 3: Add trace events**

In `migration/trace-events`, add:

```text
cxl_rdma_sidecar_connect_start(const char *host, const char *port) "host=%s port=%s"
cxl_rdma_sidecar_connect_complete(uint64_t time_ns) "time_ns=%" PRIu64
cxl_rdma_sidecar_register(uint64_t bytes) "bytes=%" PRIu64
cxl_rdma_sidecar_schedule(uint64_t region, uint64_t bytes) "region=%" PRIu64 " bytes=%" PRIu64
cxl_rdma_sidecar_post(uint64_t region, uint64_t local_addr, uint64_t remote_addr, uint64_t bytes) "region=%" PRIu64 " local=0x%" PRIx64 " remote=0x%" PRIx64 " bytes=%" PRIu64
cxl_rdma_sidecar_complete(uint64_t region, uint64_t bytes) "region=%" PRIu64 " bytes=%" PRIu64
cxl_rdma_sidecar_stale(uint64_t region, uint64_t bytes, bool cxl_race_lost) "region=%" PRIu64 " bytes=%" PRIu64 " cxl_race_lost=%d"
cxl_rdma_sidecar_failed(uint64_t region) "region=%" PRIu64
cxl_rdma_sidecar_no_candidate(void) ""
cxl_rdma_sidecar_budget_skip(void) ""
```

- [ ] **Step 4: Export QMP stats**

In `qapi/migration.json`, add the same sidecar fields to `CXLMigrationStats`. In `migration/cxl.c`, where `CXLMigrationStats` is filled, copy the values from `cxl_hybrid_get_rdma_sidecar_stats()`.

Keep the existing `rdma_bulk_regions` and `rdma_bulk_bytes` fields if present, but redefine their source as real RDMA posted/completed bytes after Task 5. Do not delete historical CSV columns yet because the experiment parser depends on stable output names.

- [ ] **Step 5: Extend script parser and CSV summary**

In `scripts/cxl-hybrid-warm-experiment.py`, add new metric keys with zero defaults:

```python
RDMA_SIDECAR_METRICS = [
    "rdma_sidecar_connect_time_ns",
    "rdma_sidecar_registered_bytes",
    "rdma_sidecar_posted_regions",
    "rdma_sidecar_posted_bytes",
    "rdma_sidecar_completed_regions",
    "rdma_sidecar_completed_bytes",
    "rdma_sidecar_stale_regions",
    "rdma_sidecar_cxl_race_lost_regions",
    "rdma_sidecar_failed_regions",
    "rdma_sidecar_no_candidate_events",
    "rdma_sidecar_budget_skip_events",
    "rdma_sidecar_max_inflight_regions",
    "rdma_sidecar_max_cover_percent",
]
```

Add parser tests that feed trace lines for `cxl_rdma_sidecar_post`, `cxl_rdma_sidecar_complete`, and `cxl_rdma_sidecar_stale`, then assert the resulting metrics.

- [ ] **Step 6: Run unit and script tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
python3 -m unittest scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
```

Expected: PASS.

- [ ] **Step 7: Commit metrics**

Run:

```bash
git add qapi/migration.json migration/cxl.h migration/cxl-region.c \
        migration/cxl.c migration/trace-events \
        scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py \
        tests/unit/test-cxl-hybrid-region.c
git commit -m "migration: add rdma sidecar transport metrics"
```

## Task 3: Replace Exclusive Ownership With Opportunistic Region State

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-region.c`
- Modify: `migration/cxl.c`
- Modify: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write failing state-machine tests**

Replace `test_rdma_sidecar_region_ownership_is_exclusive()` and `test_rdma_sidecar_cxl_bulk_excludes_rdma_owned_region()` with:

```c
static void test_rdma_sidecar_inflight_does_not_block_cxl(void)
{
    CXLHybridRDMASidecarState state = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_inflight(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_cxl_bulk_allowed(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_note_cxl_publish(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_complete_region(&state, 3));
    g_assert_false(cxl_hybrid_rdma_sidecar_region_ready_current(&state, 3));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_stale(&state, 3));

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_ready_commit_and_dirty_invalidate(void)
{
    CXLHybridRDMASidecarState state = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_complete_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_ready_current(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_committed(&state, 1));

    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_complete_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_invalidate_ready(&state, 2));
    g_assert_false(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_invalidated(&state, 2));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_cxl_bulk_allowed(&state, 2));

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}
```

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: FAIL because the new state functions do not exist and current state blocks CXL.

- [ ] **Step 2: Change the state struct**

In `migration/cxl.h`, replace the old bitmap names:

```c
unsigned long *candidate_bmap;
unsigned long *inflight_bmap;
unsigned long *ready_bmap;
unsigned long *stale_bmap;
unsigned long *cxl_published_bmap;
unsigned long *invalidated_bmap;
unsigned long *republished_bmap;
unsigned long *committed_bmap;
uint64_t accepted_regions;
uint64_t max_accepted_regions;
```

Remove `owned_bmap` and `cxl_owned_bmap` from the state model. Keep wrapper names only if needed temporarily for compatibility, but make them call the new state functions.

- [ ] **Step 3: Implement state transitions**

In `migration/cxl-region.c`, implement these semantics:

```c
bool cxl_hybrid_rdma_sidecar_try_start_region(CXLHybridRDMASidecarState *state,
                                              uint64_t region_index)
{
    if (!valid || ready || inflight || committed || invalidated || stale) {
        return false;
    }
    if (state->max_accepted_regions &&
        state->accepted_regions >= state->max_accepted_regions) {
        cxl_hybrid_account_rdma_sidecar_budget_skip();
        return false;
    }
    set candidate;
    set inflight;
    state->accepted_regions++;
    return true;
}

bool cxl_hybrid_rdma_sidecar_complete_region(CXLHybridRDMASidecarState *state,
                                             uint64_t region_index)
{
    if (!valid || !inflight) {
        return false;
    }
    clear inflight;
    if (cxl_published || invalidated || committed) {
        set stale;
        account stale with cxl_race_lost = cxl_published;
        return true;
    }
    set ready;
    account ready;
    return true;
}

bool cxl_hybrid_rdma_sidecar_note_cxl_publish(CXLHybridRDMASidecarState *state,
                                              uint64_t region_index)
{
    if (!valid) {
        return false;
    }
    set cxl_published;
    if (ready) {
        invalidate_ready(region);
    }
    return true;
}
```

Do not make `region_cxl_bulk_allowed()` depend on `inflight_bmap`. It should return false only for committed or otherwise permanently unavailable regions.

- [ ] **Step 4: Fix republish accounting**

Update `cxl_hybrid_region_note_cxl_republish()` so a CXL publication for a previously invalidated RDMA-ready region updates both local state stats and global QMP/trace stats.

Use this shape:

```c
bool cxl_hybrid_region_note_cxl_republish(uint64_t region_index)
{
    if (cxl_hybrid_rdma_sidecar_note_cxl_republish(
            &cxl_hybrid_rdma_sidecar_state, region_index)) {
        cxl_hybrid_account_rdma_cxl_republish(
            region_index,
            cxl_hybrid_rdma_sidecar_state.pages_per_region);
        return true;
    }
    return cxl_hybrid_rdma_sidecar_note_cxl_publish(
        &cxl_hybrid_rdma_sidecar_state, region_index);
}
```

This addresses the observed `cxl_republish=0` problem: the old implementation updated only local sidecar stats on the invalidated-region path and skipped global QMP/trace accounting.

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS.

- [ ] **Step 6: Commit state model**

Run:

```bash
git add migration/cxl.h migration/cxl-region.c migration/cxl.c \
        tests/unit/test-cxl-hybrid-region.c
git commit -m "migration: make rdma sidecar opportunistic"
```

## Task 4: Dirty Bitmap Claim API For Bulk Regions

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl.c`
- Modify: `migration/ram.c`
- Modify: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write failing claim-selection tests**

Add tests that model pending bulk regions with a page bitmap. The selector must accept dirty/pending regions, not clean regions.

```c
static void test_rdma_sidecar_selector_accepts_dirty_bulk_region(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    unsigned long *dirty = bitmap_new(8 * 512);
    uint64_t region = UINT64_MAX;

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);
    bitmap_set(dirty, 2 * 512, 512);

    g_assert_true(cxl_hybrid_rdma_sidecar_pick_pending_region_for_test(
        &state, dirty, 8 * 512, &region));
    g_assert_cmpuint(region, ==, 2);
    g_assert_true(cxl_hybrid_rdma_sidecar_region_inflight(&state, 2));

    g_free(dirty);
    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}

static void test_rdma_sidecar_selector_skips_clean_visible_and_budgeted_regions(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    unsigned long *dirty = bitmap_new(8 * 512);
    uint64_t region = UINT64_MAX;

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);
    cxl_hybrid_rdma_sidecar_configure_budget_for_test(&state, 8, 12);
    bitmap_set(dirty, 0, 512);
    bitmap_set(dirty, 1 * 512, 512);
    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 0));

    g_assert_false(cxl_hybrid_rdma_sidecar_pick_pending_region_for_test(
        &state, dirty, 8 * 512, &region));

    g_free(dirty);
    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}
```

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: FAIL because no selector exists.

- [ ] **Step 2: Add a narrow claim result type**

In `migration/cxl.h`, add:

```c
typedef struct CXLHybridRDMABulkClaim {
    RAMBlock *block;
    ram_addr_t block_offset;
    uint64_t global_offset;
    uint64_t cxl_offset;
    uint64_t region_index;
    uint64_t bytes;
    uint64_t pages;
    void *src;
    void *dst;
} CXLHybridRDMABulkClaim;
```

Add declarations:

```c
bool cxl_hybrid_rdma_try_claim_bulk_region(CXLHybridRDMABulkClaim *claim);
void cxl_hybrid_rdma_drop_bulk_claim(const CXLHybridRDMABulkClaim *claim);
```

- [ ] **Step 3: Implement test selector in region state**

In `migration/cxl-region.c`, implement `cxl_hybrid_rdma_sidecar_pick_pending_region_for_test()` by scanning whole-region dirty coverage:

```c
for (region = 0; region < state->total_regions; region++) {
    uint64_t first = region * state->pages_per_region;
    uint64_t npages = MIN(state->pages_per_region, total_pages - first);

    if (first >= total_pages) {
        break;
    }
    if (!bitmap_count_one_with_offset(dirty_bmap, first, npages)) {
        continue;
    }
    if (cxl_hybrid_rdma_sidecar_try_start_region(state, region)) {
        *region_out = region;
        return true;
    }
}
cxl_hybrid_account_rdma_sidecar_no_candidate();
return false;
```

- [ ] **Step 4: Implement runtime claim under the RAM bitmap lock**

In `migration/ram.c`, implement `cxl_hybrid_rdma_try_claim_bulk_region()` with the same locking discipline required by `ram_save_host_page()`: hold `ram_state.bitmap_mutex` while testing and clearing current dirty bits for the selected whole region.

The core behavior must be:

```c
qemu_mutex_lock(&ram_state.bitmap_mutex);
RAMBLOCK_FOREACH_NOT_IGNORED(block) {
    for (offset = 0; offset + region_len <= block->used_length;
         offset += region_len) {
        if (!cxl_hybrid_region_index_from_block_offset(block, offset,
                                                       &region_index)) {
            continue;
        }
        first_page = offset >> TARGET_PAGE_BITS;
        npages = region_len >> TARGET_PAGE_BITS;
        if (!bitmap_count_one_with_offset(block->bmap, first_page, npages)) {
            continue;
        }
        if (!cxl_hybrid_rdma_sidecar_try_start_region(global_state,
                                                      region_index)) {
            continue;
        }
        bitmap_clear(block->bmap, first_page, npages);
        *claim = (CXLHybridRDMABulkClaim) {
            .block = block,
            .block_offset = offset,
            .global_offset = region_index * region_len,
            .cxl_offset = block->pages_offset + offset,
            .region_index = region_index,
            .bytes = region_len,
            .pages = npages,
            .src = block->host + offset,
            .dst = (uint8_t *)cxl_state.mmap_base + block->pages_offset +
                   offset,
        };
        qemu_mutex_unlock(&ram_state.bitmap_mutex);
        return true;
    }
}
qemu_mutex_unlock(&ram_state.bitmap_mutex);
return false;
```

Do not use "not dirty" as a candidate filter. During bulk, dirty bits are the pending work queue.

- [ ] **Step 5: Remove old inline ownership from RAM scan**

In `ram_save_host_page()`, delete the `rdma_start_page`/`rdma_end_page` path that expands the main scan to an RDMA-owned region and calls `cxl_hybrid_complete_rdma_bulk_region()` at the end.

Keep the existing `cxl_hybrid_invalidate_rdma_ready_region_for_page()` call when CXL migrates a dirty page; that invalidates already-ready RDMA snapshots.

- [ ] **Step 6: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS.

- [ ] **Step 7: Commit claim API**

Run:

```bash
git add migration/cxl.h migration/cxl.c migration/ram.c \
        tests/unit/test-cxl-hybrid-region.c
git commit -m "migration: claim dirty bulk regions for rdma sidecar"
```

## Task 5: Real RDMA Sidecar Transport

**Files:**
- Modify: `migration/cxl-rdma.h`
- Modify: `migration/cxl-rdma.c`
- Modify: `migration/cxl.c`
- Modify: `migration/trace-events`
- Modify: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Delete memcpy/shadow tests and add transport-stub tests**

Remove the old test `test_rdma_sidecar_bulk_submit_marks_ready_without_commit()`. Add a test that proves no local direct-copy completion API remains:

```c
static void test_rdma_sidecar_requires_transport_for_completion(void)
{
    CXLHybridRDMASidecarBulkStats stats = { 0 };

    cxl_rdma_sidecar_stop();
    cxl_rdma_sidecar_get_stats(&stats);
    g_assert_cmpuint(stats.rdma_bulk_regions, ==, 0);
    g_assert_cmpuint(stats.rdma_bulk_bytes, ==, 0);
}
```

Run:

```bash
rg -n "submit_shadow_region|submit_region\\(|complete_owned_region|memcpy\\(" migration/cxl-rdma.c migration/cxl-rdma.h tests/unit/test-cxl-hybrid-region.c
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected before implementation: `rg` finds the old APIs and `memcpy()` in `migration/cxl-rdma.c`.

- [ ] **Step 2: Replace the public sidecar API**

In `migration/cxl-rdma.h`, replace the old functions with:

```c
typedef struct CXLHybridRDMASidecarConfig {
    const MigrationAddress *addr;
    uint64_t total_regions;
    uint64_t bytes_per_region;
    uint64_t pages_per_region;
    uint32_t max_inflight_regions;
    uint8_t max_cover_percent;
    bool pin_all;
    bool incoming;
} CXLHybridRDMASidecarConfig;

int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp);
void cxl_rdma_sidecar_stop(void);
bool cxl_rdma_sidecar_running(void);
void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats);
```

Remove declarations for:

```c
cxl_rdma_sidecar_submit_region
cxl_rdma_sidecar_complete_owned_region
cxl_rdma_sidecar_submit_shadow_region
```

- [ ] **Step 3: Define a compact sidecar wire protocol**

In `migration/cxl-rdma.c`, add:

```c
#define CXL_RDMA_SIDECAR_MAGIC 0x43585244u
#define CXL_RDMA_SIDECAR_VERSION 1

typedef struct CXLRDMASidecarHello {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t remote_base;
    uint64_t remote_len;
    uint32_t remote_rkey;
    uint32_t region_shift;
    uint32_t page_shift;
    uint32_t reserved;
} QEMU_PACKED CXLRDMASidecarHello;
```

Destination sends `remote_base`, `remote_len`, and `remote_rkey` after registering the mapped CXL backing. Source validates magic/version/region geometry before posting writes.

- [ ] **Step 4: Add `CONFIG_RDMA` stubs**

At the bottom of `migration/cxl-rdma.c`, provide this behavior when QEMU lacks RDMA:

```c
#ifndef CONFIG_RDMA
int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp)
{
    error_setg(errp,
               "x-cxl-rdma-sidecar requires QEMU to be built with RDMA support");
    return -1;
}

void cxl_rdma_sidecar_stop(void)
{
}

bool cxl_rdma_sidecar_running(void)
{
    return false;
}
#endif
```

The stub must not mark a region ready, increment completion counters, or copy bytes.

- [ ] **Step 5: Implement endpoint setup with RDMA CM/verbs**

Inside `#ifdef CONFIG_RDMA`, include:

```c
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
```

Implement a sidecar context that contains:

```c
typedef struct CXLRDMASidecarContext {
    QemuThread thread;
    QemuMutex lock;
    bool thread_created;
    bool stop;
    bool running;
    bool failed;
    bool incoming;
    char *host;
    char *port;
    uint64_t total_regions;
    uint64_t bytes_per_region;
    uint64_t pages_per_region;
    uint32_t max_inflight_regions;
    uint8_t max_cover_percent;
    bool pin_all;
    struct rdma_event_channel *channel;
    struct rdma_cm_id *listen_id;
    struct rdma_cm_id *cm_id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_channel;
    struct ibv_qp *qp;
    struct ibv_mr *dst_mr;
    struct ibv_mr *src_mr;
    uint64_t remote_base;
    uint64_t remote_len;
    uint32_t remote_rkey;
    CXLHybridRDMASidecarBulkStats stats;
} CXLRDMASidecarContext;
```

Use native RDMA as the reference for connection sequencing:

- `migration/rdma.c:qemu_rdma_resolve_host()`
- `migration/rdma.c:qemu_rdma_alloc_pd_cq()`
- `migration/rdma.c:qemu_rdma_alloc_qp()`
- `migration/rdma.c:qemu_rdma_connect()`
- `migration/rdma.c:qemu_rdma_write_one()`

Do not call those functions directly because native `RDMAContext` and helpers are static and own the main migration transport.

- [ ] **Step 6: Register destination CXL backing and source RAM**

Destination side:

```c
ctx->dst_mr = ibv_reg_mr(ctx->pd, cxl_state.mmap_base, cxl_state.mmap_size,
                         IBV_ACCESS_LOCAL_WRITE |
                         IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);
```

Source side:

```c
ctx->src_mr = ibv_reg_mr(ctx->pd, claim->src, claim->bytes,
                         IBV_ACCESS_LOCAL_WRITE |
                         IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);
```

For `rdma-pin-all`, register each migratable RAMBlock once and look up the MR by claim address. For the first implementation, if `rdma-pin-all=false`, region-sized registration is acceptable, but it must be counted in `rdma_sidecar_registered_bytes`.

- [ ] **Step 7: Post real RDMA writes**

Implement:

```c
static int cxl_rdma_sidecar_post_write(CXLRDMASidecarContext *ctx,
                                       const CXLHybridRDMABulkClaim *claim,
                                       Error **errp)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)claim->src,
        .length = claim->bytes,
        .lkey = source_lkey_for_claim(ctx, claim),
    };
    struct ibv_send_wr wr = {
        .wr_id = claim->region_index,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr = ctx->remote_base + claim->cxl_offset,
        .wr.rdma.rkey = ctx->remote_rkey,
    };
    struct ibv_send_wr *bad_wr = NULL;

    if (ibv_post_send(ctx->qp, &wr, &bad_wr)) {
        error_setg_errno(errp, errno, "RDMA sidecar ibv_post_send failed");
        return -1;
    }
    cxl_hybrid_account_rdma_sidecar_posted(claim->region_index, claim->bytes);
    trace_cxl_rdma_sidecar_post(claim->region_index, (uintptr_t)claim->src,
                                wr.wr.rdma.remote_addr, claim->bytes);
    return 0;
}
```

Completion polling must use `ibv_poll_cq()` or the completion channel and call `cxl_hybrid_rdma_sidecar_complete_region()` only after a successful `IBV_WC_SUCCESS` for the posted write.

- [ ] **Step 8: Prove memcpy/shadow paths are gone**

Run:

```bash
rg -n "submit_shadow_region|submit_region\\(|complete_owned_region|memcpy\\(" migration/cxl-rdma.c migration/cxl-rdma.h migration/cxl.c migration/ram.c tests/unit/test-cxl-hybrid-region.c
```

Expected: no matches for sidecar submit/shadow/complete-owned APIs. A `memcpy()` match is acceptable only if it is unrelated to `migration/cxl-rdma.c`; `migration/cxl-rdma.c` must not contain `memcpy()`.

- [ ] **Step 9: Build and unit test**

Run:

```bash
ninja -C build migration/cxl-rdma.o
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS.

- [ ] **Step 10: Commit transport**

Run:

```bash
git add migration/cxl-rdma.h migration/cxl-rdma.c migration/cxl.c \
        migration/trace-events tests/unit/test-cxl-hybrid-region.c
git commit -m "migration: replace cxl rdma memcpy sidecar with verbs transport"
```

## Task 6: Worker Thread And Scheduler Integration

**Files:**
- Modify: `migration/cxl-rdma.c`
- Modify: `migration/cxl-rdma.h`
- Modify: `migration/cxl.c`
- Modify: `migration/ram.c`
- Modify: `migration/cxl.h`
- Modify: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write scheduler tests**

Add a test that verifies budget configuration limits accepted regions:

```c
static void test_rdma_sidecar_budget_limits_accepted_regions(void)
{
    CXLHybridRDMASidecarState state = { 0 };

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 8, 512);
    cxl_hybrid_rdma_sidecar_configure_budget_for_test(&state, 8, 25);

    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 0));
    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 1));
    g_assert_false(cxl_hybrid_rdma_sidecar_try_start_region(&state, 2));

    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}
```

With 8 total regions and 25 percent coverage, only 2 regions may be accepted.

- [ ] **Step 2: Start sidecar during CXL remap initialization**

In `migration/cxl.c:cxl_remap_state_init()`, replace `cxl_rdma_sidecar_init()` with:

```c
if (migrate_cxl_rdma_sidecar()) {
    CXLHybridRDMASidecarConfig cfg = {
        .addr = migrate_cxl_rdma_sidecar_address(),
        .total_regions = cxl_state.total_regions,
        .bytes_per_region = cxl_state.remap_granule,
        .pages_per_region = DIV_ROUND_UP(cxl_state.remap_granule,
                                         TARGET_PAGE_SIZE),
        .max_inflight_regions =
            migrate_cxl_rdma_sidecar_max_inflight_regions(),
        .max_cover_percent =
            migrate_cxl_rdma_sidecar_max_cover_percent(),
        .pin_all = migrate_rdma_pin_all(),
        .incoming = runstate_check(RUN_STATE_INMIGRATE),
    };

    cxl_hybrid_rdma_sidecar_global_init(cfg.total_regions,
                                        cfg.pages_per_region);
    cxl_hybrid_set_rdma_sidecar_budget_stats(cfg.max_inflight_regions,
                                             cfg.max_cover_percent);
    if (cxl_rdma_sidecar_start(&cfg, &local_err) < 0) {
        error_report_err(local_err);
        migrate_set_error(migrate_get_current(), local_err);
    }
}
```

Use the actual incoming/source-side predicate already used in nearby migration code if `RUN_STATE_INMIGRATE` is not sufficient in this tree.

- [ ] **Step 3: Stop sidecar during cleanup**

Replace old destroy calls with:

```c
cxl_rdma_sidecar_stop();
cxl_hybrid_rdma_sidecar_global_destroy();
```

Call this in both normal cleanup and error cleanup paths already invoking `cxl_rdma_sidecar_destroy()`.

- [ ] **Step 4: Implement worker loop**

In `migration/cxl-rdma.c`, the source-side worker should:

```c
while (!ctx->stop) {
    CXLHybridRDMABulkClaim claim = { 0 };

    if (!cxl_hybrid_rdma_try_claim_bulk_region(&claim)) {
        cxl_hybrid_account_rdma_sidecar_no_candidate();
        qemu_cond_timedwait_or_usleep(1000);
        continue;
    }

    trace_cxl_rdma_sidecar_schedule(claim.region_index, claim.bytes);
    if (cxl_rdma_sidecar_post_write(ctx, &claim, &err) < 0) {
        cxl_hybrid_rdma_drop_bulk_claim(&claim);
        cxl_hybrid_account_rdma_sidecar_failed(claim.region_index);
        continue;
    }

    ret = cxl_rdma_sidecar_wait_one_completion(ctx, claim.region_index, &err);
    if (ret == 0) {
        cxl_hybrid_region_complete_rdma_sidecar(claim.region_index);
        cxl_hybrid_account_rdma_sidecar_completed(claim.region_index,
                                                  claim.bytes);
    } else {
        cxl_hybrid_rdma_drop_bulk_claim(&claim);
        cxl_hybrid_account_rdma_sidecar_failed(claim.region_index);
    }
}
```

With default `max_inflight_regions=1`, a simple post-and-wait loop is acceptable. If `max_inflight_regions > 1`, store claims in a small in-flight array indexed by `wr_id`.

- [ ] **Step 5: Ensure CXL is never blocked by RDMA in-flight**

Audit CXL publication paths:

```bash
rg -n "rdma|cxl_bulk_allowed|try_own|region_is_rdma" migration/cxl.c migration/ram.c migration/cxl-region.c
```

Remove any condition that skips CXL bulk copy solely because a region is RDMA in-flight. Keep checks for committed RDMA-ready regions where CXL visibility has already been published.

- [ ] **Step 6: Build and test**

Run:

```bash
ninja -C build migration/cxl-rdma.o migration/cxl.o migration/ram.o
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS.

- [ ] **Step 7: Commit scheduler integration**

Run:

```bash
git add migration/cxl-rdma.c migration/cxl-rdma.h migration/cxl.c \
        migration/ram.c migration/cxl.h tests/unit/test-cxl-hybrid-region.c
git commit -m "migration: run rdma sidecar as async bulk worker"
```

## Task 7: Final Dirty Sync, Commit, And Postcopy Fallback

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-region.c`
- Modify: `migration/cxl.c`
- Modify: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write final-sync tests**

Add a test that asserts only current ready regions commit:

```c
static void test_rdma_sidecar_final_sync_commits_only_current_ready_regions(void)
{
    CXLHybridRDMASidecarState state = { 0 };
    unsigned long *dirty = bitmap_new(4 * 512);

    cxl_hybrid_rdma_sidecar_state_init_for_test(&state, 4, 512);
    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 0));
    g_assert_true(cxl_hybrid_rdma_sidecar_complete_region(&state, 0));
    g_assert_true(cxl_hybrid_rdma_sidecar_try_start_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_complete_region(&state, 1));
    bitmap_set(dirty, 1 * 512, 1);

    g_assert_cmpuint(cxl_hybrid_rdma_sidecar_invalidate_dirty_ready_regions(
                         &state, dirty, 4 * 512), ==, 1);
    g_assert_true(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 0));
    g_assert_false(cxl_hybrid_rdma_sidecar_commit_ready_region(&state, 1));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_committed(&state, 0));
    g_assert_true(cxl_hybrid_rdma_sidecar_region_invalidated(&state, 1));

    g_free(dirty);
    cxl_hybrid_rdma_sidecar_state_destroy_for_test(&state);
}
```

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: FAIL until final-sync semantics match the new state model.

- [ ] **Step 2: Update final dirty sync**

In `cxl_hybrid_sync_rdma_dirty_for_postcopy()`, keep the whole-region invalidation policy but update it to the new state names. Dirty after claim or completion invalidates the whole ready region for v1.

The function must:

- scan only ready regions;
- test current dirty bits for the region;
- call `cxl_hybrid_invalidate_region_rdma_ready(region_idx)` once per region;
- leave stale or in-flight regions to CXL fallback;
- not mark invalidated regions committed.

- [ ] **Step 3: Commit ready regions through CXL visibility**

Update the commit helper used at switch/postcopy:

```c
bool cxl_hybrid_region_commit_rdma_ready(uint64_t region_index)
{
    if (!cxl_hybrid_rdma_sidecar_commit_ready_region(
            &cxl_hybrid_rdma_sidecar_state, region_index)) {
        return false;
    }
    return true;
}
```

The source-side visibility publish stays in `cxl_hybrid_commit_rdma_ready_region(region_index, generation)`: after `cxl_hybrid_region_commit_rdma_ready()` succeeds, it marks the region's pages in `cxl_state.cxl_visible_bmap`, clears them from `remaining_bmap`, and calls `cxl_hybrid_ctrl_publish_pages_visible(first_page, npages, generation)`. Do not let RDMA completion directly publish destination visibility.

- [ ] **Step 4: Ensure postcopy fallback is normal CXL**

For invalidated and stale regions, destination faults must continue through `cxl_hybrid_wait_and_resolve_fault()` and CXL region publication. Confirm with:

```bash
rg -n "commit_rdma_ready|invalidate_rdma|note_cxl_republish|wait_and_resolve_fault|fault" migration/cxl.c migration/cxl-region.c
```

The postcopy path should not contain an RDMA retry. Republish happens by CXL.

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS.

- [ ] **Step 6: Commit final-sync behavior**

Run:

```bash
git add migration/cxl.h migration/cxl-region.c migration/cxl.c \
        tests/unit/test-cxl-hybrid-region.c
git commit -m "migration: commit only current rdma-ready cxl regions"
```

## Task 8: End-To-End Experiment And Report Update

**Files:**
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`
- Create: `docs/superpowers/reports/2026-05-27-real-rdma-cxl-sidecar-results.md`

- [ ] **Step 1: Add report fields**

In the script summary output, include:

```text
rdma_sidecar_endpoint
rdma_sidecar_max_inflight_regions
rdma_sidecar_max_cover_percent
rdma_sidecar_posted_bytes
rdma_sidecar_completed_bytes
rdma_sidecar_stale_regions
rdma_sidecar_cxl_race_lost_regions
cxl_publish_pages
cxl_publish_time_ns
guest_stall_ms
total_time_ms
precopy_time_ms
postcopy_time_ms
```

The report must distinguish CXL publish work from RDMA work. Do not describe `rdma_bulk_bytes` as real RDMA unless it comes from verbs completions.

- [ ] **Step 2: Run non-RDMA correctness tests**

Run:

```bash
python3 -m unittest scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py scripts/cxl-hybrid-warm-experiment-test.py
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS.

- [ ] **Step 3: Run a real RDMA smoke experiment**

On the RDMA-capable host pair, run one repeat with the existing no-brake CXL baseline and the new sidecar mode:

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

Expected:

- `hybrid_parallel_rdma_cxl` uses a `unix:` main migration URI.
- trace contains `cxl_rdma_sidecar_post` and `cxl_rdma_sidecar_complete`.
- QMP/query output has non-zero `rdma_sidecar_posted_bytes`.
- CXL publish counters remain non-zero under default 25 percent coverage.
- no `migration/cxl-rdma.c` path reports local `memcpy` completion.

- [ ] **Step 4: Run budget sensitivity**

Repeat sidecar mode with:

```bash
--x-cxl-rdma-sidecar-max-cover-percent 0
--x-cxl-rdma-sidecar-max-cover-percent 25
--x-cxl-rdma-sidecar-max-cover-percent 50
```

Expected:

- 0 percent produces no accepted RDMA regions and should match CXL baseline except for sidecar setup overhead.
- 25 percent keeps CXL dominant and should show bounded RDMA bytes.
- 50 percent increases RDMA bytes; if CXL publish pages collapse unexpectedly, the scheduler is still taking too much work.

- [ ] **Step 5: Write report**

Create `docs/superpowers/reports/2026-05-27-real-rdma-cxl-sidecar-results.md` with:

```markdown
# Real RDMA CXL Sidecar Results

## Configuration

- Main migration URI:
- Sidecar RDMA endpoint:
- rdma-pin-all:
- max-inflight-regions:
- max-cover-percent:
- cxl-path:
- workload:

## Results

| mode | total ms | precopy ms | postcopy ms | guest stall ms | CXL publish pages | RDMA posted bytes | RDMA completed bytes | stale regions | failed regions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |

## Analysis

- Bulk-phase change:
- Postcopy-phase change:
- Guest stall:
- RDMA stale or CXL-race losses:
- Whether CXL remained primary:

## Conclusion

State whether this task validates real RDMA+CXL parallelism and what remains to tune.
```

- [ ] **Step 6: Commit experiment updates**

Run:

```bash
git add scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py \
        docs/superpowers/reports/2026-05-27-real-rdma-cxl-sidecar-results.md
git commit -m "docs: report real rdma cxl sidecar experiment"
```

## Cross-Task Verification

Run these before declaring implementation complete:

```bash
rg -n "submit_shadow_region|complete_owned_region|memcpy\\(" migration/cxl-rdma.c migration/cxl-rdma.h migration/cxl.c migration/ram.c
python3 -m unittest scripts.cxl-hybrid-warm-experiment-test.WarmExperimentScriptTest
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py scripts/cxl-hybrid-warm-experiment-test.py
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected:

- no sidecar shadow/direct-copy API remains;
- `migration/cxl-rdma.c` contains no `memcpy()`;
- script tests pass;
- unit tests pass.

If RDMA hardware is available, also run the smoke experiment from Task 8 and verify that `rdma_sidecar_posted_bytes` and `rdma_sidecar_completed_bytes` come from verbs completion events, not local copy counters.

## Acceptance Criteria

- `hybrid_parallel_rdma_cxl` fails validation when sidecar RDMA is enabled without a real RDMA address or without `CONFIG_RDMA`.
- The main migration URI for `hybrid_parallel_rdma_cxl` remains `unix:`.
- The sidecar uses RDMA CM/verbs and posts `IBV_WR_RDMA_WRITE`.
- No memcpy/shadow sidecar completion path remains.
- RDMA only participates in bulk phase in this revision.
- RDMA candidates are dirty/pending bulk regions.
- CXL can migrate or publish a region while RDMA is in flight.
- RDMA completion after CXL publication is counted stale and does not block or roll back CXL.
- RDMA-ready regions are committed only after final dirty synchronization.
- Invalidated RDMA-ready regions fall back to CXL postcopy publication and increment republish stats.
- Default policy keeps CXL primary with `max-inflight=1` and `max-cover-percent=25`.
