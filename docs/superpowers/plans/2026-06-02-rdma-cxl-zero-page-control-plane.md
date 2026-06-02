# RDMA CXL Zero-Page Control Plane Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make zero pages a first-class page-state location so full-zero regions bypass data transfer, CXL skips zero pages, RDMA keeps full-region writes, and destination postcopy faults resolve zero pages through UFFD zero-page placement.

**Architecture:** Add a `PUBLISHED@ZERO` source control path that uses the same generation and CAS rules as CXL/RDMA claims. Move zero-page classification into the RAM main loop before CXL/RDMA enqueue, publish full-zero work as control-plane-only, split partial-zero CXL work into non-zero runs, and keep partial-zero RDMA completion as `DST_LOCAL`. Expose counters in QAPI and experiment summaries so performance runs can distinguish useful CXL/RDMA bytes from skipped zero bytes.

**Tech Stack:** QEMU C migration code, CXL hybrid shared page-state control plane, RAM dirty bitmap helpers, `buffer_is_zero()`, `postcopy_place_page_zero()`, GLib unit tests, Meson/Ninja, QAPI migration stats, Python experiment runner.

---

## Current Baseline

Work in:

```bash
cd /home/xiexinchen/.config/superpowers/worktrees/qemu/rdma-cxl-parallel-hybrid
```

Design spec:

```text
docs/superpowers/specs/2026-06-02-rdma-cxl-zero-page-control-plane-design.md
```

Important current behavior:

- `migration/cxl.h` already defines `CXL_HYBRID_PAGE_LOCATION_ZERO`.
- `migration/cxl-page-state.c` counts published zero pages in snapshots, but there is no source-side `complete_zero` helper.
- `migration/cxl-hybrid-control.c:cxl_hybrid_ctrl_set_page_visible()` and `cxl_hybrid_ctrl_set_pages_visible()` publish as CXL.
- `migration/ram.c:ram_save_host_page()` tries CXL and RDMA bulk enqueue before legacy `save_zero_page()`.
- `migration/ram.c:save_zero_page()` can still materialize a zero page into mapped CXL backing in fallback paths.
- `migration/cxl.c:cxl_hybrid_wait_and_resolve_region_fault()` rejects `CXL_HYBRID_PAGE_LOCATION_ZERO`.
- Recent performance work expects the main loop to do control-plane classification/enqueue only; bulk data movement should remain in workers or RDMA sidecar.

## File Structure

Modify these files:

- `migration/cxl.h`: add ZERO owner/helper prototypes and QAPI-facing stat storage declarations.
- `migration/cxl-page-state.c`: add zero-page claim/complete helper and snapshot owner accounting.
- `migration/cxl-hybrid-control.c`: add source wrapper to publish a single zero page through page state, update visible bitmap, and count CAS failures.
- `migration/cxl.c`: add zero-fault resolution with `postcopy_place_page_zero()` and publish new counters through `cxl_hybrid_populate_info()`.
- `migration/ram.c`: add region zero classifier, full-zero bypass, partial-zero CXL non-zero run enqueue, and partial-zero RDMA accounting.
- `qapi/migration.json`: add unstable zero-page migration counters to `CXLMigrationStats`.
- `scripts/cxl-hybrid-warm-experiment.py`: include zero-page counters in summaries.
- `scripts/cxl-hybrid-warm-experiment-test.py`: add structure and summary tests for the new path.
- `tests/unit/test-cxl-hybrid-control.c`: add page-state/control tests for `PUBLISHED@ZERO`.

No new source file is required. Keep this scoped to the existing page-state/control/RAM scheduling modules.

## Stop-And-Discuss Gates

Stop and ask before continuing if any of these happen:

- publishing ZERO cannot be expressed with the existing page-state CAS model without weakening dirty-generation safety;
- `buffer_is_zero()` scanning of a full fault region is measurably more expensive than the avoided transfer on the target benchmark;
- CXL worker queue batching cannot handle non-zero runs separated by zero pages without a larger queue redesign;
- `postcopy_place_page_zero()` cannot be called from the region-remap fault path without violating postcopy locking or wakeup ordering;
- a zero-heavy workload still shows CXL worker bytes equal to the old full-region copy volume after this plan is implemented.

## Task 1: Add Page-State ZERO Claim And Source Publish Helper

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-page-state.c`
- Modify: `migration/cxl-hybrid-control.c`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing page-state tests**

Add these tests to `tests/unit/test-cxl-hybrid-control.c` near the existing page-state tests:

```c
static void test_zero_page_claim_complete_publishes_zero(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_dirty(15, 23);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_claim_for_zero(&slot, 15, &claim));
    g_assert_cmpuint(claim.owner, ==, CXL_HYBRID_PAGE_OWNER_ZERO);
    g_assert_true(cxl_hybrid_page_state_complete_zero(&slot, &claim));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(slot), ==,
                     CXL_HYBRID_PAGE_STATE_PUBLISHED);
    g_assert_cmpuint(cxl_hybrid_page_state_location(slot), ==,
                     CXL_HYBRID_PAGE_LOCATION_ZERO);
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, 15, CXL_HYBRID_PAGE_LOCATION_ZERO));
}

static void test_zero_page_claim_completion_stales_after_dirty(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_dirty(16, 44);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_claim_for_zero(&slot, 16, &claim));
    cxl_hybrid_page_state_mark_dirty(&slot, 16, 45);
    g_assert_false(cxl_hybrid_page_state_complete_zero(&slot, &claim));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(slot), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_dirty_seq(slot), ==, 45);
}
```

Register them in `main()` near the other page-state registrations:

```c
g_test_add_func("/cxl-hybrid-control/zero-page-claim-complete-publishes-zero",
                test_zero_page_claim_complete_publishes_zero);
g_test_add_func("/cxl-hybrid-control/zero-page-claim-stales-after-dirty",
                test_zero_page_claim_completion_stales_after_dirty);
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: compile failure because `CXL_HYBRID_PAGE_OWNER_ZERO`, `cxl_hybrid_page_state_claim_for_zero()`, and `cxl_hybrid_page_state_complete_zero()` do not exist.

- [ ] **Step 3: Add ZERO page-state owner and helpers**

Modify `migration/cxl.h`:

```c
typedef enum CXLHybridPageOwner {
    CXL_HYBRID_PAGE_OWNER_NONE = 0,
    CXL_HYBRID_PAGE_OWNER_CXL = 1,
    CXL_HYBRID_PAGE_OWNER_RDMA = 2,
    CXL_HYBRID_PAGE_OWNER_ZERO = 3,
} CXLHybridPageOwner;
```

Add prototypes near the existing CXL/RDMA claim helpers:

```c
bool cxl_hybrid_page_state_claim_for_zero(uint64_t *slot,
                                          uint32_t generation,
                                          CXLHybridPageClaim *claim);
bool cxl_hybrid_page_state_complete_zero(uint64_t *slot,
                                         const CXLHybridPageClaim *claim);
```

Modify `migration/cxl-page-state.c`:

```c
bool cxl_hybrid_page_state_claim_for_zero(uint64_t *slot,
                                          uint32_t generation,
                                          CXLHybridPageClaim *claim)
{
    return cxl_hybrid_page_state_try_claim(
        slot, CXL_HYBRID_PAGE_OWNER_ZERO, generation, claim);
}

bool cxl_hybrid_page_state_complete_zero(uint64_t *slot,
                                         const CXLHybridPageClaim *claim)
{
    return claim && claim->owner == CXL_HYBRID_PAGE_OWNER_ZERO &&
           cxl_hybrid_page_state_complete(
               slot, claim, CXL_HYBRID_PAGE_LOCATION_ZERO);
}
```

In `cxl_hybrid_page_state_snapshot()`, keep `CXL_HYBRID_PAGE_OWNER_ZERO` out of `in_flight_cxl` and `in_flight_rdma`; do not add a new QAPI snapshot counter in this task.

- [ ] **Step 4: Run page-state tests and verify they pass**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS for the two new tests and existing page-state tests.

- [ ] **Step 5: Write failing source publish helper test**

Add this test near `test_mark_page_visible_sets_page_state_cxl()`:

```c
static void test_complete_zero_page_visible_marks_zero_not_cxl(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[4];
    uint32_t generation = 17;
    CXLHybridPageClaim claim = { 0 };

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, generation);
    cxl_hybrid_control_mark_page_dirty_generation(
        &hdr, visible, page_state, 2, generation, 99);
    g_assert_true(cxl_hybrid_page_state_claim_for_zero(&page_state[2],
                                                       generation, &claim));

    g_assert_true(cxl_hybrid_control_complete_zero_page_visible_generation(
        &hdr, visible, page_state, 2, generation, &claim));
    g_assert_true(test_bit(2, visible));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[2], generation, CXL_HYBRID_PAGE_LOCATION_ZERO));
    g_assert_false(cxl_hybrid_page_state_can_consume(
        page_state[2], generation, CXL_HYBRID_PAGE_LOCATION_CXL));
}
```

Register it:

```c
g_test_add_func("/cxl-hybrid-control/complete-zero-page-visible-marks-zero",
                test_complete_zero_page_visible_marks_zero_not_cxl);
```

- [ ] **Step 6: Run the helper test and verify it fails**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: compile failure because `cxl_hybrid_control_complete_zero_page_visible_generation()` is not declared.

- [ ] **Step 7: Add pure control completion helper and source wrapper**

Add this prototype to `migration/cxl.h` near `cxl_hybrid_control_complete_rdma_page_visible_generation()`:

```c
bool cxl_hybrid_control_complete_zero_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    const CXLHybridPageClaim *claim);
```

Add this prototype near the source control wrappers:

```c
bool cxl_hybrid_ctrl_publish_zero_page(uint64_t page_index,
                                       uint32_t generation);
```

Implement the pure helper in `migration/cxl-hybrid-control.c` beside the CXL/RDMA completion helpers:

```c
bool cxl_hybrid_control_complete_zero_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    const CXLHybridPageClaim *claim)
{
    if (!hdr || !cxl_hybrid_control_generation_matches(hdr, generation) ||
        !visible_bitmap || !page_state || page_index >= hdr->total_pages) {
        return false;
    }
    if (!cxl_hybrid_page_state_complete_zero(&page_state[page_index], claim)) {
        return false;
    }
    set_bit(page_index, visible_bitmap);
    return true;
}
```

Implement the source wrapper in `migration/cxl-hybrid-control.c` near `cxl_hybrid_ctrl_complete_rdma_page_visible()`:

```c
bool cxl_hybrid_ctrl_publish_zero_page(uint64_t page_index,
                                       uint32_t generation)
{
    CXLHybridControlState *state = &cxl_hybrid_control_source;
    CXLHybridPageClaim claim = { 0 };

    if (!state->hdr || !state->visible_bitmap || !state->page_state ||
        page_index >= state->page_state_words ||
        !cxl_hybrid_control_generation_matches(state->hdr, generation)) {
        return false;
    }
    if (!cxl_hybrid_page_state_claim_for_zero(&state->page_state[page_index],
                                              generation, &claim)) {
        return false;
    }
    return cxl_hybrid_control_complete_zero_page_visible_generation(
        state->hdr, state->visible_bitmap, state->page_state, page_index,
        generation, &claim);
}
```

- [ ] **Step 8: Run control tests and commit**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS.

Commit:

```bash
git add migration/cxl.h migration/cxl-page-state.c migration/cxl-hybrid-control.c tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: add zero page-state publish helper"
```

## Task 2: Resolve Destination ZERO Faults With UFFD Zero Page

**Files:**
- Modify: `migration/cxl.c`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing structural test for zero fault handling**

Add this test to `scripts/cxl-hybrid-warm-experiment-test.py` near the existing region remap tests:

```python
    def test_region_remap_zero_location_uses_postcopy_zero_page(self):
        cxl_text = (REPO_ROOT / "migration" / "cxl.c").read_text()

        self.assertIn("static int cxl_hybrid_resolve_zero_fault(", cxl_text)
        helper_start = cxl_text.index("static int cxl_hybrid_resolve_zero_fault(")
        helper_end = cxl_text.index(
            "static int cxl_hybrid_wait_and_resolve_region_fault(")
        helper = cxl_text[helper_start:helper_end]
        self.assertIn("postcopy_place_page_zero(mis, fault_host, rb)",
                      helper)
        self.assertIn("dst_zero_faults_resolved", helper)

        resolver_start = cxl_text.index(
            "static int cxl_hybrid_wait_and_resolve_region_fault(")
        resolver_end = cxl_text.index(
            "static int cxl_hybrid_try_resolve_region_fault_fast(")
        resolver = cxl_text[resolver_start:resolver_end]
        zero_branch = resolver.index(
            "location == CXL_HYBRID_PAGE_LOCATION_ZERO")
        self.assertIn("return cxl_hybrid_resolve_zero_fault(",
                      resolver[zero_branch:zero_branch + 400])
        self.assertNotIn("unsupported zero location",
                         resolver[zero_branch:zero_branch + 400])
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_region_remap_zero_location_uses_postcopy_zero_page
```

Expected: FAIL because the current resolver rejects zero location.

- [ ] **Step 3: Implement zero fault resolver**

In `migration/cxl.c`, add a counter field to `cxl_state`:

```c
uint64_t dst_zero_faults_resolved;
```

Add this helper before `cxl_hybrid_wait_and_resolve_region_fault()`:

```c
static int cxl_hybrid_resolve_zero_fault(MigrationIncomingState *mis,
                                         RAMBlock *rb,
                                         ram_addr_t offset,
                                         void *fault_host,
                                         Error **errp)
{
    int ret;

    ret = postcopy_place_page_zero(mis, fault_host, rb);
    if (ret) {
        error_setg(errp,
                   "CXL hybrid zero fault page %s/0x%" PRIx64
                   " failed zero-page placement: %d",
                   qemu_ram_get_idstr(rb), (uint64_t)offset, ret);
        return ret;
    }

    qatomic_inc(&cxl_state.dst_zero_faults_resolved);
    return 0;
}
```

Replace the zero-location error branch in `cxl_hybrid_wait_and_resolve_region_fault()`:

```c
if (location == CXL_HYBRID_PAGE_LOCATION_ZERO) {
    return cxl_hybrid_resolve_zero_fault(mis, rb, offset, fault_host, errp);
}
```

- [ ] **Step 4: Run structural test and migration build**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_region_remap_zero_location_uses_postcopy_zero_page
ninja -C build qemu-system-x86_64
```

Expected: Python test PASS and build PASS.

- [ ] **Step 5: Commit**

```bash
git add migration/cxl.c scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration/cxl: resolve zero page-state faults"
```

## Task 3: Add RAM Region Zero Classifier

**Files:**
- Modify: `migration/ram.c`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing structural test for classifier placement**

Add this test to `scripts/cxl-hybrid-warm-experiment-test.py` near existing `ram_save_host_page()` structure tests:

```python
    def test_ram_main_loop_scans_zero_pages_before_cxl_and_rdma_enqueue(self):
        ram_text = (REPO_ROOT / "migration" / "ram.c").read_text()

        self.assertIn("typedef struct CXLHybridZeroRegionScan", ram_text)
        self.assertIn("static bool cxl_hybrid_zero_scan_region(", ram_text)
        self.assertIn("buffer_is_zero(host, TARGET_PAGE_SIZE)", ram_text)

        fn_start = ram_text.index("static int ram_save_host_page(")
        fn_end = ram_text.index("/* Update host page boundary information */",
                                fn_start)
        prologue = ram_text[fn_start:fn_end]

        zero_scan = prologue.index("cxl_hybrid_zero_scan_region(")
        cxl_enqueue = prologue.index("cxl_hybrid_cxl_enqueue_bulk_page(")
        rdma_enqueue = prologue.index("cxl_hybrid_rdma_enqueue_bulk_region(")
        self.assertLess(zero_scan, cxl_enqueue)
        self.assertLess(zero_scan, rdma_enqueue)
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_ram_main_loop_scans_zero_pages_before_cxl_and_rdma_enqueue
```

Expected: FAIL because the classifier does not exist.

- [ ] **Step 3: Add scan struct and helper**

Move or keep `#define CXL_HYBRID_CXL_BULK_MAX_PAGES 1024` above this struct,
then add the struct near the CXL/RDMA bulk helpers in `migration/ram.c`:

```c
typedef struct CXLHybridZeroRegionScan {
    bool valid;
    bool any_dirty;
    bool all_dirty_zero;
    bool partial_zero;
    unsigned long dirty_zero_bmap[BITS_TO_LONGS(CXL_HYBRID_CXL_BULK_MAX_PAGES)];
    unsigned long first_page_in_block;
    uint32_t dirty_pages;
    uint32_t zero_dirty_pages;
    uint32_t nonzero_dirty_pages;
    uint32_t pages;
    ram_addr_t block_offset;
    ram_addr_t region_len;
    uint64_t global_offset;
    uint64_t first_state_page;
    uint64_t region_index;
    uint64_t cxl_offset;
};
```

Add this helper near `cxl_hybrid_shadow_classify_bulk_page()`:

```c
static bool cxl_hybrid_zero_scan_region(PageSearchStatus *pss,
                                        CXLHybridZeroRegionScan *scan)
{
    RAMBlock *block;
    ram_addr_t region_offset;
    ram_addr_t region_len;
    uint64_t cxl_offset;
    uint64_t global_offset;
    uint64_t scan_global_offset;
    unsigned long block_pages;
    uint32_t pages;

    memset(scan, 0, sizeof(*scan));
    if (!migrate_cxl_hybrid() || !pss || !pss->block ||
        postcopy_preempt_active()) {
        return false;
    }

    block = pss->block;
    region_len = cxl_hybrid_fault_region_granule();
    if (!region_len || !QEMU_IS_ALIGNED(region_len, TARGET_PAGE_SIZE)) {
        return false;
    }

    scan->block_offset = ((ram_addr_t)pss->page) << TARGET_PAGE_BITS;
    region_offset = QEMU_ALIGN_DOWN(scan->block_offset, region_len);
    if (!cxl_hybrid_global_page_offset(block, region_offset,
                                       TARGET_PAGE_SIZE, &global_offset) ||
        !cxl_hybrid_source_page_cxl_offset(block->idstr, region_offset,
                                           &cxl_offset)) {
        return false;
    }

    block_pages = block->used_length >> TARGET_PAGE_BITS;
    if ((region_offset >> TARGET_PAGE_BITS) >= block_pages) {
        return false;
    }
    pages = MIN((uint64_t)((block->used_length - region_offset) >>
                           TARGET_PAGE_BITS),
                region_len >> TARGET_PAGE_BITS);
    if (!pages || pages > CXL_HYBRID_CXL_BULK_MAX_PAGES) {
        return false;
    }

    bitmap_zero(scan->dirty_zero_bmap, CXL_HYBRID_CXL_BULK_MAX_PAGES);
    scan->valid = true;
    scan->first_page_in_block = region_offset >> TARGET_PAGE_BITS;
    scan->pages = pages;
    scan->region_len = region_len;
    scan->global_offset = global_offset;
    scan->first_state_page = global_offset >> TARGET_PAGE_BITS;
    scan->region_index = global_offset / region_len;
    scan->cxl_offset = cxl_offset;

    for (uint32_t i = 0; i < pages; i++) {
        unsigned long bitmap_page = scan->first_page_in_block + i;
        uint8_t *host = block->host + region_offset +
                        (ram_addr_t)i * TARGET_PAGE_SIZE;

        if (!test_bit(bitmap_page, block->bmap)) {
            continue;
        }
        scan->any_dirty = true;
        scan->dirty_pages++;
        if (buffer_is_zero(host, TARGET_PAGE_SIZE)) {
            set_bit(i, scan->dirty_zero_bmap);
            scan->zero_dirty_pages++;
        } else {
            scan->nonzero_dirty_pages++;
        }
    }

    scan_global_offset = global_offset + scan->pages * TARGET_PAGE_SIZE;
    scan->all_dirty_zero = scan->dirty_pages && !scan->nonzero_dirty_pages;
    scan->partial_zero = scan->zero_dirty_pages && scan->nonzero_dirty_pages;
    return scan_global_offset >= global_offset && scan->any_dirty;
}
```

- [ ] **Step 4: Call scanner before CXL/RDMA enqueue**

Modify the top of `ram_save_host_page()`:

```c
CXLHybridZeroRegionScan zero_scan;
bool have_zero_scan;
```

Then replace the current prologue with:

```c
cxl_hybrid_shadow_classify_bulk_page(rs, pss);
have_zero_scan = cxl_hybrid_zero_scan_region(pss, &zero_scan);
(void)have_zero_scan;
res = cxl_hybrid_cxl_enqueue_bulk_page(rs, pss);
if (res) {
    return res;
}
res = cxl_hybrid_rdma_enqueue_bulk_region(rs, pss);
if (res) {
    return res;
}
```

This step only proves placement. The next tasks will remove `(void)have_zero_scan`,
pass `zero_scan` into the enqueue helpers, and use it.

- [ ] **Step 5: Run structural test and build**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_ram_main_loop_scans_zero_pages_before_cxl_and_rdma_enqueue
ninja -C build qemu-system-x86_64
```

Expected: test PASS and build PASS.

## Task 4: Full-Zero Region Bypass

**Files:**
- Modify: `migration/ram.c`
- Modify: `migration/cxl.c`
- Modify: `migration/cxl.h`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing structural test for full-zero bypass**

Add this test:

```python
    def test_full_zero_region_bypasses_cxl_and_rdma_work(self):
        ram_text = (REPO_ROOT / "migration" / "ram.c").read_text()

        self.assertIn("static int cxl_hybrid_publish_full_zero_region(",
                      ram_text)
        helper_start = ram_text.index(
            "static int cxl_hybrid_publish_full_zero_region(")
        helper_end = ram_text.index(
            "static int cxl_hybrid_cxl_enqueue_bulk_page(")
        helper = ram_text[helper_start:helper_end]
        self.assertIn("cxl_hybrid_ctrl_publish_zero_page", helper)
        self.assertIn("migration_bitmap_clear_dirty", helper)
        self.assertIn("zero_full_regions_bypassed", helper)

        fn_start = ram_text.index("static int ram_save_host_page(")
        fn_end = ram_text.index("/* Update host page boundary information */",
                                fn_start)
        prologue = ram_text[fn_start:fn_end]
        zero_bypass = prologue.index("cxl_hybrid_publish_full_zero_region(")
        cxl_enqueue = prologue.index("cxl_hybrid_cxl_enqueue_bulk_page(")
        rdma_enqueue = prologue.index("cxl_hybrid_rdma_enqueue_bulk_region(")
        self.assertLess(zero_bypass, cxl_enqueue)
        self.assertLess(zero_bypass, rdma_enqueue)
```

- [ ] **Step 2: Run test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_full_zero_region_bypasses_cxl_and_rdma_work
```

Expected: FAIL because the bypass helper does not exist.

- [ ] **Step 3: Add counters**

In `migration/cxl.c` `cxl_state`, add:

```c
uint64_t zero_pages_classified;
uint64_t zero_full_regions_bypassed;
uint64_t zero_partial_regions;
uint64_t zero_pages_published;
uint64_t zero_publish_cas_failures;
uint64_t cxl_zero_pages_skipped;
uint64_t cxl_effective_bytes_after_zero_filter;
uint64_t rdma_partial_zero_bytes_sent;
```

Add prototypes in `migration/cxl.h`:

```c
void cxl_hybrid_account_zero_scan(uint32_t zero_pages, bool partial_zero);
void cxl_hybrid_account_full_zero_region_bypassed(uint32_t pages);
void cxl_hybrid_account_zero_publish(bool success);
void cxl_hybrid_account_cxl_zero_pages_skipped(uint32_t pages);
void cxl_hybrid_account_cxl_effective_zero_filtered_bytes(uint64_t bytes);
void cxl_hybrid_account_rdma_partial_zero_bytes(uint64_t bytes);
```

Implement these in `migration/cxl.c`:

```c
void cxl_hybrid_account_zero_scan(uint32_t zero_pages, bool partial_zero)
{
    if (zero_pages) {
        qatomic_add(&cxl_state.zero_pages_classified, zero_pages);
    }
    if (partial_zero) {
        qatomic_inc(&cxl_state.zero_partial_regions);
    }
}

void cxl_hybrid_account_full_zero_region_bypassed(uint32_t pages)
{
    qatomic_inc(&cxl_state.zero_full_regions_bypassed);
    if (pages) {
        qatomic_add(&cxl_state.cxl_zero_pages_skipped, pages);
    }
}

void cxl_hybrid_account_zero_publish(bool success)
{
    if (success) {
        qatomic_inc(&cxl_state.zero_pages_published);
    } else {
        qatomic_inc(&cxl_state.zero_publish_cas_failures);
    }
}

void cxl_hybrid_account_cxl_zero_pages_skipped(uint32_t pages)
{
    if (pages) {
        qatomic_add(&cxl_state.cxl_zero_pages_skipped, pages);
    }
}

void cxl_hybrid_account_cxl_effective_zero_filtered_bytes(uint64_t bytes)
{
    if (bytes) {
        qatomic_add(&cxl_state.cxl_effective_bytes_after_zero_filter, bytes);
    }
}

void cxl_hybrid_account_rdma_partial_zero_bytes(uint64_t bytes)
{
    if (bytes) {
        qatomic_add(&cxl_state.rdma_partial_zero_bytes_sent, bytes);
    }
}
```

- [ ] **Step 4: Account classifier output**

At the end of `cxl_hybrid_zero_scan_region()` after `scan->partial_zero` is assigned, add:

```c
cxl_hybrid_account_zero_scan(scan->zero_dirty_pages, scan->partial_zero);
```

- [ ] **Step 5: Add full-zero publish helper**

Add this helper before `cxl_hybrid_cxl_enqueue_bulk_page()`:

```c
static int cxl_hybrid_publish_full_zero_region(RAMState *rs,
                                               PageSearchStatus *pss,
                                               const CXLHybridZeroRegionScan *scan)
{
    uint32_t generation;
    uint32_t published = 0;

    if (!rs || !pss || !scan || !scan->valid ||
        !scan->all_dirty_zero || migration_in_postcopy()) {
        return 0;
    }

    generation = cxl_hybrid_fault_publish_generation();
    for (uint32_t i = 0; i < scan->pages; i++) {
        unsigned long bitmap_page = scan->first_page_in_block + i;
        uint64_t state_page = scan->first_state_page + i;
        bool ok;

        if (!test_bit(bitmap_page, pss->block->bmap) ||
            !test_bit(i, scan->dirty_zero_bmap)) {
            continue;
        }
        ok = cxl_hybrid_ctrl_publish_zero_page(state_page, generation);
        cxl_hybrid_account_zero_publish(ok);
        if (!ok) {
            continue;
        }
        migration_bitmap_clear_dirty(rs, pss->block, bitmap_page);
        published++;
    }

    if (!published) {
        return 0;
    }
    cxl_hybrid_account_full_zero_region_bypassed(published);
    pss->page = scan->first_page_in_block + scan->pages;
    return published;
}
```

- [ ] **Step 6: Call full-zero helper before data enqueue**

Modify `ram_save_host_page()` prologue:

```c
cxl_hybrid_shadow_classify_bulk_page(rs, pss);
have_zero_scan = cxl_hybrid_zero_scan_region(pss, &zero_scan);
if (have_zero_scan && zero_scan.all_dirty_zero) {
    res = cxl_hybrid_publish_full_zero_region(rs, pss, &zero_scan);
    if (res) {
        return res;
    }
}
res = cxl_hybrid_cxl_enqueue_bulk_page(rs, pss);
```

Remove the temporary `(void)have_zero_scan;` line added in Task 3.

- [ ] **Step 7: Run test and build**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_full_zero_region_bypasses_cxl_and_rdma_work
ninja -C build qemu-system-x86_64
```

Expected: PASS and build PASS.

Do not commit yet; Task 5 will consume the same classifier in CXL.

## Task 5: Partial-Zero CXL Enqueues Only Non-Zero Runs

**Files:**
- Modify: `migration/ram.c`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing structural test for CXL non-zero runs**

Add this test:

```python
    def test_partial_zero_cxl_skips_zero_pages_and_enqueues_nonzero_runs(self):
        ram_text = (REPO_ROOT / "migration" / "ram.c").read_text()

        fn_start = ram_text.index("static int cxl_hybrid_cxl_enqueue_bulk_page(")
        fn_end = ram_text.index(
            "static int cxl_hybrid_rdma_enqueue_bulk_region(", fn_start)
        helper = ram_text[fn_start:fn_end]

        self.assertIn("const CXLHybridZeroRegionScan *zero_scan", helper)
        self.assertIn("cxl_hybrid_ctrl_publish_zero_page", helper)
        self.assertIn("cxl_hybrid_ctrl_enqueue_cxl_pages", helper)
        self.assertIn("cxl_hybrid_account_cxl_zero_pages_skipped", helper)
        self.assertIn("cxl_hybrid_account_cxl_effective_zero_filtered_bytes",
                      helper)
        self.assertIn("!test_bit(page, zero_scan->dirty_zero_bmap)", helper)
```

- [ ] **Step 2: Run test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_partial_zero_cxl_skips_zero_pages_and_enqueues_nonzero_runs
```

Expected: FAIL because CXL enqueue helper has no zero-scan parameter.

- [ ] **Step 3: Change CXL helper signature and call site**

Change the helper signature in `migration/ram.c`:

```c
static int cxl_hybrid_cxl_enqueue_bulk_page(RAMState *rs,
                                            PageSearchStatus *pss,
                                            const CXLHybridZeroRegionScan *zero_scan)
```

Change the call in `ram_save_host_page()`:

```c
res = cxl_hybrid_cxl_enqueue_bulk_page(
    rs, pss, have_zero_scan ? &zero_scan : NULL);
```

- [ ] **Step 4: Publish zero pages on the CXL lane**

Inside `cxl_hybrid_cxl_enqueue_bulk_page()`, after generation/page geometry is available and before enqueueing non-zero pages, add:

```c
if (zero_scan && zero_scan->valid && zero_scan->partial_zero &&
    zero_scan->first_page_in_block == pss->page) {
    uint32_t zero_published = 0;

    for (uint32_t page = 0; page < zero_scan->pages; page++) {
        unsigned long bitmap_page = zero_scan->first_page_in_block + page;
        uint64_t state_page = zero_scan->first_state_page + page;
        bool ok;

        if (!test_bit(page, zero_scan->dirty_zero_bmap) ||
            !test_bit(bitmap_page, pss->block->bmap)) {
            continue;
        }
        ok = cxl_hybrid_ctrl_publish_zero_page(state_page, generation);
        cxl_hybrid_account_zero_publish(ok);
        if (!ok) {
            continue;
        }
        migration_bitmap_clear_dirty(rs, pss->block, bitmap_page);
        zero_published++;
    }
    cxl_hybrid_account_cxl_zero_pages_skipped(zero_published);
}
```

- [ ] **Step 5: Enqueue only contiguous non-zero dirty runs**

Replace the single contiguous dirty-run enqueue block with run-based logic when `zero_scan && zero_scan->partial_zero`:

```c
if (zero_scan && zero_scan->valid && zero_scan->partial_zero &&
    zero_scan->first_page_in_block == pss->page) {
    uint32_t total_enqueued = 0;

    for (uint32_t page = 0; page < zero_scan->pages; ) {
        uint32_t run_pages = 0;
        uint32_t enqueued_run;
        unsigned long bitmap_page = zero_scan->first_page_in_block + page;

        if (test_bit(page, zero_scan->dirty_zero_bmap) ||
            !test_bit(bitmap_page, pss->block->bmap)) {
            page++;
            continue;
        }
        while (page + run_pages < zero_scan->pages) {
            unsigned long run_bitmap_page =
                zero_scan->first_page_in_block + page + run_pages;

            if (test_bit(page + run_pages, zero_scan->dirty_zero_bmap) ||
                !test_bit(run_bitmap_page, pss->block->bmap)) {
                break;
            }
            run_pages++;
        }

        cxl_hybrid_ctrl_mark_dirty_pages(
            pss->block->bmap, bitmap_page,
            zero_scan->first_state_page + page, run_pages, generation);
        enqueued_run = cxl_hybrid_ctrl_enqueue_cxl_pages(
            pss->block,
            zero_scan->block_offset + (ram_addr_t)page * TARGET_PAGE_SIZE,
            zero_scan->first_state_page + page,
            zero_scan->cxl_offset + (uint64_t)page * TARGET_PAGE_SIZE,
            generation, CXL_HYBRID_TRANSFER_CXL_LOW, run_pages);
        if (!enqueued_run) {
            page += run_pages;
            continue;
        }
        cxl_hybrid_invalidate_rdma_ready_region_for_page(
            pss->block,
            zero_scan->block_offset + (ram_addr_t)page * TARGET_PAGE_SIZE);
        for (uint32_t i = 0; i < enqueued_run; i++) {
            migration_bitmap_clear_dirty(rs, pss->block, bitmap_page + i);
        }
        total_enqueued += enqueued_run;
        page += MAX(enqueued_run, 1);
    }

    if (total_enqueued) {
        cxl_hybrid_account_cxl_effective_zero_filtered_bytes(
            (uint64_t)total_enqueued * TARGET_PAGE_SIZE);
        pss->page = zero_scan->first_page_in_block + zero_scan->pages;
        return total_enqueued;
    }
    return 0;
}
```

Leave the existing non-zero fast path unchanged after this block.

- [ ] **Step 6: Run tests and build**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_partial_zero_cxl_skips_zero_pages_and_enqueues_nonzero_runs
ninja -C build qemu-system-x86_64 tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS and build PASS.

- [ ] **Step 7: Commit Tasks 3-5**

```bash
git add migration/ram.c migration/cxl.c migration/cxl.h scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration/cxl: bypass zero pages in bulk scheduling"
```

## Task 6: Partial-Zero RDMA Keeps Full-Region DST_LOCAL Semantics

**Files:**
- Modify: `migration/ram.c`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing structural test for RDMA partial-zero behavior**

Add this test:

```python
    def test_partial_zero_rdma_keeps_full_region_dst_local(self):
        ram_text = (REPO_ROOT / "migration" / "ram.c").read_text()

        fn_start = ram_text.index("static int cxl_hybrid_rdma_enqueue_bulk_region(")
        fn_end = ram_text.index("void cxl_hybrid_rdma_drop_bulk_claim(",
                                fn_start)
        helper = ram_text[fn_start:fn_end]

        self.assertIn("const CXLHybridZeroRegionScan *zero_scan", helper)
        self.assertIn("cxl_hybrid_account_rdma_partial_zero_bytes", helper)
        self.assertIn("claim.bytes", helper)
        self.assertNotIn("cxl_hybrid_ctrl_publish_zero_page", helper)
```

- [ ] **Step 2: Run test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_partial_zero_rdma_keeps_full_region_dst_local
```

Expected: FAIL because RDMA helper has no zero-scan parameter or metric.

- [ ] **Step 3: Change RDMA helper signature and call site**

Change signature:

```c
static int cxl_hybrid_rdma_enqueue_bulk_region(RAMState *rs,
                                               PageSearchStatus *pss,
                                               const CXLHybridZeroRegionScan *zero_scan)
```

Change call in `ram_save_host_page()`:

```c
res = cxl_hybrid_rdma_enqueue_bulk_region(
    rs, pss, have_zero_scan ? &zero_scan : NULL);
```

- [ ] **Step 4: Add partial-zero RDMA accounting only**

After successful `cxl_rdma_sidecar_enqueue_bulk_claim(&claim)` and before returning, add:

```c
if (zero_scan && zero_scan->valid && zero_scan->partial_zero &&
    zero_scan->region_index == claim.region_index) {
    cxl_hybrid_account_rdma_partial_zero_bytes(claim.bytes);
}
```

Do not publish any page as `ZERO` in this helper. The existing RDMA completion path must still call `cxl_hybrid_ctrl_complete_rdma_pages()`, which publishes claimed pages as `DST_LOCAL`.

- [ ] **Step 5: Run tests and build**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_partial_zero_rdma_keeps_full_region_dst_local
ninja -C build qemu-system-x86_64
```

Expected: PASS and build PASS.

- [ ] **Step 6: Commit**

```bash
git add migration/ram.c scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration/cxl: preserve rdma semantics for partial zero regions"
```

## Task 7: Export Zero-Page Metrics Through QAPI And Experiment Summary

**Files:**
- Modify: `qapi/migration.json`
- Modify: `migration/cxl.c`
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Write failing QAPI/summary test**

Add this test near `test_region_remap_stats_are_exported_to_qapi_and_summary()`:

```python
    def test_zero_page_stats_are_exported_to_qapi_and_summary(self):
        qapi_text = (REPO_ROOT / "qapi" / "migration.json").read_text()
        cxl_text = (REPO_ROOT / "migration" / "cxl.c").read_text()

        for field in (
            "zero-pages-classified",
            "zero-full-regions-bypassed",
            "zero-partial-regions",
            "zero-pages-published",
            "zero-publish-cas-failures",
            "cxl-zero-pages-skipped",
            "cxl-effective-bytes-after-zero-filter",
            "rdma-partial-zero-bytes-sent",
            "dst-zero-faults-resolved",
        ):
            self.assertIn(f"'{field}'", qapi_text)

        for field in (
            "zero_pages_classified",
            "zero_full_regions_bypassed",
            "zero_partial_regions",
            "zero_pages_published",
            "zero_publish_cas_failures",
            "cxl_zero_pages_skipped",
            "cxl_effective_bytes_after_zero_filter",
            "rdma_partial_zero_bytes_sent",
            "dst_zero_faults_resolved",
        ):
            self.assertIn(f"info->x_cxl->{field}", cxl_text)

        summary = self.mod.extract_summary([
            {
                "x-cxl": {
                    "zero-pages-classified": 10,
                    "zero-full-regions-bypassed": 1,
                    "zero-partial-regions": 2,
                    "zero-pages-published": 8,
                    "zero-publish-cas-failures": 1,
                    "cxl-zero-pages-skipped": 7,
                    "cxl-effective-bytes-after-zero-filter": 12288,
                    "rdma-partial-zero-bytes-sent": 65536,
                },
                "dst-query-migrate": {
                    "x-cxl": {
                        "dst-zero-faults-resolved": 3,
                    }
                },
            }
        ])

        self.assertEqual(summary["zero_pages_classified"], 10)
        self.assertEqual(summary["zero_full_regions_bypassed"], 1)
        self.assertEqual(summary["zero_partial_regions"], 2)
        self.assertEqual(summary["zero_pages_published"], 8)
        self.assertEqual(summary["zero_publish_cas_failures"], 1)
        self.assertEqual(summary["cxl_zero_pages_skipped"], 7)
        self.assertEqual(summary["cxl_effective_bytes_after_zero_filter"],
                         12288)
        self.assertEqual(summary["rdma_partial_zero_bytes_sent"], 65536)
        self.assertEqual(summary["dst_zero_faults_resolved"], 3)
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_zero_page_stats_are_exported_to_qapi_and_summary
```

Expected: FAIL because QAPI and summary fields do not exist.

- [ ] **Step 3: Add QAPI fields**

In `qapi/migration.json`, add these unstable `CXLMigrationStats` fields near existing page-state worker metrics:

```python
'zero-pages-classified': {
    'type': 'uint64', 'features': [ 'unstable' ] },
'zero-full-regions-bypassed': {
    'type': 'uint64', 'features': [ 'unstable' ] },
'zero-partial-regions': {
    'type': 'uint64', 'features': [ 'unstable' ] },
'zero-pages-published': {
    'type': 'uint64', 'features': [ 'unstable' ] },
'zero-publish-cas-failures': {
    'type': 'uint64', 'features': [ 'unstable' ] },
'cxl-zero-pages-skipped': {
    'type': 'uint64', 'features': [ 'unstable' ] },
'cxl-effective-bytes-after-zero-filter': {
    'type': 'uint64', 'features': [ 'unstable' ] },
'rdma-partial-zero-bytes-sent': {
    'type': 'uint64', 'features': [ 'unstable' ] },
'dst-zero-faults-resolved': {
    'type': 'uint64', 'features': [ 'unstable' ] },
```

Also extend the `@unstable` comment list with these field names.

- [ ] **Step 4: Populate QAPI fields**

In `migration/cxl.c:cxl_hybrid_populate_info()`, add:

```c
info->x_cxl->zero_pages_classified =
    qatomic_read(&cxl_state.zero_pages_classified);
info->x_cxl->zero_full_regions_bypassed =
    qatomic_read(&cxl_state.zero_full_regions_bypassed);
info->x_cxl->zero_partial_regions =
    qatomic_read(&cxl_state.zero_partial_regions);
info->x_cxl->zero_pages_published =
    qatomic_read(&cxl_state.zero_pages_published);
info->x_cxl->zero_publish_cas_failures =
    qatomic_read(&cxl_state.zero_publish_cas_failures);
info->x_cxl->cxl_zero_pages_skipped =
    qatomic_read(&cxl_state.cxl_zero_pages_skipped);
info->x_cxl->cxl_effective_bytes_after_zero_filter =
    qatomic_read(&cxl_state.cxl_effective_bytes_after_zero_filter);
info->x_cxl->rdma_partial_zero_bytes_sent =
    qatomic_read(&cxl_state.rdma_partial_zero_bytes_sent);
info->x_cxl->dst_zero_faults_resolved =
    qatomic_read(&cxl_state.dst_zero_faults_resolved);
```

- [ ] **Step 5: Add summary extraction**

In `scripts/cxl-hybrid-warm-experiment.py:extract_summary()`, initialize:

```python
zero_pages_classified = 0
zero_full_regions_bypassed = 0
zero_partial_regions = 0
zero_pages_published = 0
zero_publish_cas_failures = 0
cxl_zero_pages_skipped = 0
cxl_effective_bytes_after_zero_filter = 0
rdma_partial_zero_bytes_sent = 0
dst_zero_faults_resolved = 0
```

Inside the sample loop, add max aggregation:

```python
zero_pages_classified = max(
    zero_pages_classified,
    src_xcxl.get("zero-pages-classified", 0),
    dst_xcxl.get("zero-pages-classified", 0))
zero_full_regions_bypassed = max(
    zero_full_regions_bypassed,
    src_xcxl.get("zero-full-regions-bypassed", 0),
    dst_xcxl.get("zero-full-regions-bypassed", 0))
zero_partial_regions = max(
    zero_partial_regions,
    src_xcxl.get("zero-partial-regions", 0),
    dst_xcxl.get("zero-partial-regions", 0))
zero_pages_published = max(
    zero_pages_published,
    src_xcxl.get("zero-pages-published", 0),
    dst_xcxl.get("zero-pages-published", 0))
zero_publish_cas_failures = max(
    zero_publish_cas_failures,
    src_xcxl.get("zero-publish-cas-failures", 0),
    dst_xcxl.get("zero-publish-cas-failures", 0))
cxl_zero_pages_skipped = max(
    cxl_zero_pages_skipped,
    src_xcxl.get("cxl-zero-pages-skipped", 0),
    dst_xcxl.get("cxl-zero-pages-skipped", 0))
cxl_effective_bytes_after_zero_filter = max(
    cxl_effective_bytes_after_zero_filter,
    src_xcxl.get("cxl-effective-bytes-after-zero-filter", 0),
    dst_xcxl.get("cxl-effective-bytes-after-zero-filter", 0))
rdma_partial_zero_bytes_sent = max(
    rdma_partial_zero_bytes_sent,
    src_xcxl.get("rdma-partial-zero-bytes-sent", 0),
    dst_xcxl.get("rdma-partial-zero-bytes-sent", 0))
dst_zero_faults_resolved = max(
    dst_zero_faults_resolved,
    src_xcxl.get("dst-zero-faults-resolved", 0),
    dst_xcxl.get("dst-zero-faults-resolved", 0))
```

Add these keys to the returned summary dictionary:

```python
"zero_pages_classified": zero_pages_classified,
"zero_full_regions_bypassed": zero_full_regions_bypassed,
"zero_partial_regions": zero_partial_regions,
"zero_pages_published": zero_pages_published,
"zero_publish_cas_failures": zero_publish_cas_failures,
"cxl_zero_pages_skipped": cxl_zero_pages_skipped,
"cxl_effective_bytes_after_zero_filter": cxl_effective_bytes_after_zero_filter,
"rdma_partial_zero_bytes_sent": rdma_partial_zero_bytes_sent,
"dst_zero_faults_resolved": dst_zero_faults_resolved,
```

- [ ] **Step 6: Run QAPI generation/build and summary tests**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py \
  CXLHybridWarmExperimentTest.test_zero_page_stats_are_exported_to_qapi_and_summary
ninja -C build qemu-system-x86_64
```

Expected: PASS and build PASS.

- [ ] **Step 7: Commit**

```bash
git add qapi/migration.json migration/cxl.c scripts/cxl-hybrid-warm-experiment.py scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration/cxl: export zero page-state metrics"
```

## Task 8: End-To-End Verification And Performance Experiment

**Files:**
- Modify only if verification finds defects in files already touched above.

- [ ] **Step 1: Run focused unit and script tests**

Run:

```bash
ninja -C build qemu-system-x86_64 tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region tests/unit/test-migration-postcopy
./build/tests/unit/test-cxl-hybrid-control --tap
./build/tests/unit/test-cxl-hybrid-region --tap
./build/tests/unit/test-migration-postcopy --tap
python3 scripts/cxl-hybrid-warm-experiment-test.py
git diff --check
```

Expected:

- all unit tests PASS;
- script test exits 0;
- `git diff --check` prints no whitespace errors.

- [ ] **Step 2: Run a warm migration experiment with brake disabled**

Run:

```bash
python3 scripts/cxl-hybrid-warm-experiment.py \
  --keep-dir \
  --pressure remap_xlarge_random_rw \
  --mode hybrid_parallel_rdma_cxl \
  --threshold-profile balanced \
  --repeat 1 \
  --migration-timeout 120 \
  --x-cxl-disable-brake \
  --x-cxl-rdma-sidecar-max-inflight-regions 8 \
  --x-cxl-rdma-sidecar-max-cover-percent 50 \
  --rdma-pin-all
```

Expected summary properties:

- `zero_pages_classified` is present;
- zero-heavy workloads show non-zero `zero_pages_published` or explain why the workload produced no zero dirty pages;
- `cxl_effective_bytes_after_zero_filter <= page_state_cxl_worker_bytes`;
- if `zero_pages_published > 0`, destination can resolve `PUBLISHED@ZERO` faults without unsupported-location errors;
- `rdma_partial_zero_bytes_sent` may be non-zero and represents full-region RDMA writes, not sparse zero transfer.

- [ ] **Step 3: Inspect timing breakdown**

From the experiment summary and trace output, record:

```text
precopy bulk time:
postcopy active/drain time:
downtime end to postcopy active:
page_state_cxl_worker_bytes:
cxl_effective_bytes_after_zero_filter:
rdma_bulk_bytes:
rdma_partial_zero_bytes_sent:
zero_pages_classified:
zero_pages_published:
cxl_zero_pages_skipped:
dst_zero_faults_resolved:
dst_region_wait_time_ns:
max_dst_region_wait_time_ns:
```

Expected interpretation:

- full-zero bypass reduces CXL/RDMA enqueue count for zero-heavy regions;
- partial-zero CXL reduces CXL worker bytes because only non-zero runs are copied;
- partial-zero RDMA keeps `DST_LOCAL` completion and may not reduce RDMA bytes;
- destination zero faults do not fall back to cleanup copy.

- [ ] **Step 4: Fix any verification failures**

For each failure, make the smallest code change that addresses the failing assertion or build error, then rerun the exact failing command. Do not change the RDMA partial-zero semantic to sparse RDMA.

- [ ] **Step 5: Final commit**

If Step 4 changed files:

```bash
git add migration/cxl.h migration/cxl-page-state.c migration/cxl-hybrid-control.c migration/cxl.c migration/ram.c qapi/migration.json scripts/cxl-hybrid-warm-experiment.py scripts/cxl-hybrid-warm-experiment-test.py tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: verify zero page-state control plane"
```

If Step 4 did not change files, do not create an empty commit.

## Final Acceptance Criteria

- Full-zero dirty regions publish `PUBLISHED@ZERO`, clear dirty bits only after successful publish, and enqueue no CXL/RDMA data work.
- Partial-zero CXL publishes zero pages as `ZERO`, enqueues only non-zero dirty runs, and never materializes known zero pages in CXL backing.
- Partial-zero RDMA transfers the full region and completes all claimed pages as `DST_LOCAL`.
- Destination postcopy faults on `PUBLISHED@ZERO` call `postcopy_place_page_zero()` and do not attempt CXL remap.
- Mixed `ZERO` and `CXL` regions still remap only contiguous `PUBLISHED@CXL` spans.
- QAPI and experiment summaries expose zero classification, skipped CXL pages, effective CXL bytes, partial-zero RDMA bytes, and destination zero fault resolution.
- The performance experiment can explain whether any remaining postcopy delay is fault wait, CXL copy, RDMA sidecar, or control-plane overhead.
