# RDMA CXL Page-State Control Plane Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace region-authoritative RDMA/CXL control with a shared CXL per-page state machine, then migrate data movement to a CXL single-page fault poller, CXL high/low priority worker, and RDMA page-claimed region descriptors.

**Architecture:** Implement the refactor in checkpoints. First add pure page-state helpers and shadow control-plane storage without changing migration behavior. Then route scheduling through per-page descriptors, split the CXL poller and worker behavior, convert RDMA from region-ready state to per-page completion, and finally replace fixed-region remap with longest-valid-span speculative remap.

**Tech Stack:** QEMU C migration code, CXL shared DAX mapping, qatomic release/acquire helpers, QemuThread/QemuMutex/QemuCond, RDMA verbs sidecar, RAM dirty bitmap helpers, GLib unit tests, Meson/Ninja, Python experiment runner.

---

## Current Baseline

Work in:

```bash
cd /home/xiexinchen/.config/superpowers/worktrees/qemu/rdma-cxl-parallel-hybrid
```

Design spec:

```text
docs/superpowers/specs/2026-05-30-rdma-cxl-page-state-control-plane-design.md
```

Important current behavior:

- `migration/cxl-region.c` owns RDMA state with region bitmaps.
- `migration/ram.c:cxl_hybrid_rdma_enqueue_bulk_region()` can clear a whole region from the dirty bitmap before the CXL path sees those pages.
- `migration/cxl-hybrid-control.c:cxl_hybrid_ctrl_request_worker_thread()` currently dequeues CXL fault requests and calls page or region publish helpers directly.
- `migration/cxl.c:cxl_hybrid_publish_fault_region_request_core()` still publishes complete fault regions.
- `migration/cxl-rdma.c` already has a real sidecar thread shape with queue, post, and completion polling.

Keep the existing mode name `hybrid_parallel_rdma_cxl`.

## Stop-And-Discuss Gates

Stop and ask before continuing if any of these happen:

- the shared CXL control layout cannot fit the page-state array without moving the data backing offsets;
- 64-bit atomic compare-and-swap is not available or not valid on the mapped CXL control memory on the test host;
- RDMA remote memory registration cannot target destination RAM in the current migration lifecycle;
- a posted RDMA write to destination RAM cannot be made safe against CXL fault wakeup without blocking the poller for unacceptable time;
- speculative remap span scans add measurable tail latency before a configurable cap is added.

## File Structure

Create these focused files:

- `migration/cxl-page-state.c`: pure page-state word helpers and CAS transitions.
- `migration/cxl-page-transfer.c`: CXL/RDMA page descriptor queues, priority handling, and pure queue helpers.

Modify these existing files:

- `migration/cxl.h`: page-state enums, helper prototypes, queue descriptor types, stats structs.
- `migration/meson.build`: add new migration source files to `migration_files` and `system_ss`.
- `tests/unit/test-cxl-hybrid-control.c`: page-state control layout, visibility shadowing, and remap span tests.
- `tests/unit/test-cxl-hybrid-region.c`: RDMA descriptor and region-as-transfer-unit tests.
- `migration/cxl-hybrid-control-header.c`: pure page-state layout helpers and state transition wrappers.
- `migration/cxl-hybrid-control.c`: shared page-state array mapping, CXL poller fast path, CXL worker queues.
- `migration/cxl.c`: CXL copy completion, speculative remap span selection, query stats.
- `migration/cxl-rdma.c`: RDMA descriptor consumption and per-page completion.
- `migration/ram.c`: bulk scheduler classification and page-state claims.
- `scripts/cxl-hybrid-warm-experiment.py`: page-state and lane metrics.
- `scripts/cxl-hybrid-warm-experiment-test.py`: parser and summary tests for new metrics.

## Phase 1 Scope

Phase 1 is the safe foundation. It must not change migration behavior by default.

Exit criteria:

- page-state word helpers pass unit tests;
- the shared CXL control mapping has a page-state array in shadow mode;
- existing visible bitmap updates can mirror into page state;
- current CXL/RDMA data movement still works as before when shadow mode is enabled.

## Task 1: Add Pure Page-State Word Helpers

**Files:**
- Create: `migration/cxl-page-state.c`
- Modify: `migration/cxl.h`
- Modify: `migration/meson.build`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing page-state tests**

Add these tests to `tests/unit/test-cxl-hybrid-control.c` near the existing control-header tests:

```c
static void test_page_state_claim_and_complete_cxl(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_not_sent(7);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_try_claim(
        &slot, CXL_HYBRID_PAGE_OWNER_CXL, 7, &claim));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(slot), ==,
                     CXL_HYBRID_PAGE_STATE_IN_FLIGHT);
    g_assert_cmpuint(cxl_hybrid_page_state_owner(slot), ==,
                     CXL_HYBRID_PAGE_OWNER_CXL);

    g_assert_true(cxl_hybrid_page_state_complete(
        &slot, &claim, CXL_HYBRID_PAGE_LOCATION_CXL));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(slot), ==,
                     CXL_HYBRID_PAGE_STATE_PUBLISHED);
    g_assert_cmpuint(cxl_hybrid_page_state_location(slot), ==,
                     CXL_HYBRID_PAGE_LOCATION_CXL);
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, 7, CXL_HYBRID_PAGE_LOCATION_CXL));
}

static void test_page_state_dirty_makes_rdma_completion_stale(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_dirty(3, 11);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_try_claim(
        &slot, CXL_HYBRID_PAGE_OWNER_RDMA, 3, &claim));
    cxl_hybrid_page_state_mark_dirty(&slot, 3, 12);
    g_assert_false(cxl_hybrid_page_state_complete(
        &slot, &claim, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(slot), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_cmpuint(cxl_hybrid_page_state_dirty_seq(slot), ==, 12);
}

static void test_page_state_rejects_double_claim(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_not_sent(9);
    CXLHybridPageClaim cxl_claim = { 0 };
    CXLHybridPageClaim rdma_claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_try_claim(
        &slot, CXL_HYBRID_PAGE_OWNER_CXL, 9, &cxl_claim));
    g_assert_false(cxl_hybrid_page_state_try_claim(
        &slot, CXL_HYBRID_PAGE_OWNER_RDMA, 9, &rdma_claim));
}
```

Register them in `main()`:

```c
g_test_add_func("/cxl-hybrid-control/page-state-claim-complete-cxl",
                test_page_state_claim_and_complete_cxl);
g_test_add_func("/cxl-hybrid-control/page-state-dirty-stales-rdma",
                test_page_state_dirty_makes_rdma_completion_stale);
g_test_add_func("/cxl-hybrid-control/page-state-rejects-double-claim",
                test_page_state_rejects_double_claim);
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: FAIL at compile or link time because `CXLHybridPageClaim` and the `cxl_hybrid_page_state_*` helpers do not exist.

- [ ] **Step 3: Add page-state public types**

Add this block to `migration/cxl.h` near the other CXL hybrid control types:

```c
typedef enum CXLHybridPageStateKind {
    CXL_HYBRID_PAGE_STATE_NOT_SENT = 0,
    CXL_HYBRID_PAGE_STATE_IN_FLIGHT = 1,
    CXL_HYBRID_PAGE_STATE_PUBLISHED = 2,
    CXL_HYBRID_PAGE_STATE_DIRTY = 3,
} CXLHybridPageStateKind;

typedef enum CXLHybridPageOwner {
    CXL_HYBRID_PAGE_OWNER_NONE = 0,
    CXL_HYBRID_PAGE_OWNER_CXL = 1,
    CXL_HYBRID_PAGE_OWNER_RDMA = 2,
} CXLHybridPageOwner;

typedef enum CXLHybridPageLocation {
    CXL_HYBRID_PAGE_LOCATION_NONE = 0,
    CXL_HYBRID_PAGE_LOCATION_CXL = 1,
    CXL_HYBRID_PAGE_LOCATION_DST_LOCAL = 2,
    CXL_HYBRID_PAGE_LOCATION_ZERO = 3,
} CXLHybridPageLocation;

typedef struct CXLHybridPageClaim {
    uint64_t observed;
    uint32_t generation;
    uint32_t dirty_seq;
    CXLHybridPageOwner owner;
} CXLHybridPageClaim;
```

Add these prototypes to `migration/cxl.h`:

```c
uint64_t cxl_hybrid_page_state_make_not_sent(uint32_t generation);
uint64_t cxl_hybrid_page_state_make_dirty(uint32_t generation,
                                          uint32_t dirty_seq);
uint64_t cxl_hybrid_page_state_make_published(
    uint32_t generation,
    CXLHybridPageLocation location,
    uint32_t dirty_seq);
CXLHybridPageStateKind cxl_hybrid_page_state_kind(uint64_t word);
CXLHybridPageOwner cxl_hybrid_page_state_owner(uint64_t word);
CXLHybridPageLocation cxl_hybrid_page_state_location(uint64_t word);
uint32_t cxl_hybrid_page_state_generation(uint64_t word);
uint32_t cxl_hybrid_page_state_dirty_seq(uint64_t word);
bool cxl_hybrid_page_state_try_claim(uint64_t *slot,
                                     CXLHybridPageOwner owner,
                                     uint32_t generation,
                                     CXLHybridPageClaim *claim);
bool cxl_hybrid_page_state_complete(uint64_t *slot,
                                    const CXLHybridPageClaim *claim,
                                    CXLHybridPageLocation location);
void cxl_hybrid_page_state_mark_dirty(uint64_t *slot,
                                      uint32_t generation,
                                      uint32_t dirty_seq);
bool cxl_hybrid_page_state_can_consume(uint64_t word,
                                       uint32_t generation,
                                       CXLHybridPageLocation location);
```

- [ ] **Step 4: Implement the pure helpers**

Create `migration/cxl-page-state.c`:

```c
#include "qemu/osdep.h"
#include "migration/cxl.h"
#include "qemu/atomic.h"

#define CXL_PAGE_KIND_SHIFT      0
#define CXL_PAGE_OWNER_SHIFT     3
#define CXL_PAGE_LOCATION_SHIFT  6
#define CXL_PAGE_GENERATION_SHIFT 16
#define CXL_PAGE_DIRTY_SEQ_SHIFT 32
#define CXL_PAGE_KIND_MASK       0x7ULL
#define CXL_PAGE_OWNER_MASK      0x7ULL
#define CXL_PAGE_LOCATION_MASK   0x7ULL
#define CXL_PAGE_U16_MASK        0xffffULL
#define CXL_PAGE_U32_MASK        0xffffffffULL

static uint64_t cxl_hybrid_page_state_pack(CXLHybridPageStateKind kind,
                                           CXLHybridPageOwner owner,
                                           CXLHybridPageLocation location,
                                           uint32_t generation,
                                           uint32_t dirty_seq)
{
    return ((uint64_t)kind << CXL_PAGE_KIND_SHIFT) |
           ((uint64_t)owner << CXL_PAGE_OWNER_SHIFT) |
           ((uint64_t)location << CXL_PAGE_LOCATION_SHIFT) |
           (((uint64_t)generation & CXL_PAGE_U16_MASK) <<
            CXL_PAGE_GENERATION_SHIFT) |
           ((uint64_t)dirty_seq << CXL_PAGE_DIRTY_SEQ_SHIFT);
}

uint64_t cxl_hybrid_page_state_make_not_sent(uint32_t generation)
{
    return cxl_hybrid_page_state_pack(CXL_HYBRID_PAGE_STATE_NOT_SENT,
                                      CXL_HYBRID_PAGE_OWNER_NONE,
                                      CXL_HYBRID_PAGE_LOCATION_NONE,
                                      generation, 0);
}

uint64_t cxl_hybrid_page_state_make_dirty(uint32_t generation,
                                          uint32_t dirty_seq)
{
    return cxl_hybrid_page_state_pack(CXL_HYBRID_PAGE_STATE_DIRTY,
                                      CXL_HYBRID_PAGE_OWNER_NONE,
                                      CXL_HYBRID_PAGE_LOCATION_NONE,
                                      generation, dirty_seq);
}

uint64_t cxl_hybrid_page_state_make_published(
    uint32_t generation,
    CXLHybridPageLocation location,
    uint32_t dirty_seq)
{
    return cxl_hybrid_page_state_pack(CXL_HYBRID_PAGE_STATE_PUBLISHED,
                                      CXL_HYBRID_PAGE_OWNER_NONE,
                                      location, generation, dirty_seq);
}

CXLHybridPageStateKind cxl_hybrid_page_state_kind(uint64_t word)
{
    return (word >> CXL_PAGE_KIND_SHIFT) & CXL_PAGE_KIND_MASK;
}

CXLHybridPageOwner cxl_hybrid_page_state_owner(uint64_t word)
{
    return (word >> CXL_PAGE_OWNER_SHIFT) & CXL_PAGE_OWNER_MASK;
}

CXLHybridPageLocation cxl_hybrid_page_state_location(uint64_t word)
{
    return (word >> CXL_PAGE_LOCATION_SHIFT) & CXL_PAGE_LOCATION_MASK;
}

uint32_t cxl_hybrid_page_state_generation(uint64_t word)
{
    return (word >> CXL_PAGE_GENERATION_SHIFT) & CXL_PAGE_U16_MASK;
}

uint32_t cxl_hybrid_page_state_dirty_seq(uint64_t word)
{
    return (word >> CXL_PAGE_DIRTY_SEQ_SHIFT) & CXL_PAGE_U32_MASK;
}

bool cxl_hybrid_page_state_try_claim(uint64_t *slot,
                                     CXLHybridPageOwner owner,
                                     uint32_t generation,
                                     CXLHybridPageClaim *claim)
{
    uint64_t old;
    uint64_t next;
    CXLHybridPageStateKind kind;
    uint32_t dirty_seq;

    if (!slot || !claim || owner == CXL_HYBRID_PAGE_OWNER_NONE) {
        return false;
    }

    for (;;) {
        old = qatomic_load_acquire(slot);
        kind = cxl_hybrid_page_state_kind(old);
        if (kind != CXL_HYBRID_PAGE_STATE_NOT_SENT &&
            kind != CXL_HYBRID_PAGE_STATE_DIRTY) {
            return false;
        }
        if (cxl_hybrid_page_state_generation(old) != generation) {
            return false;
        }

        dirty_seq = cxl_hybrid_page_state_dirty_seq(old);
        next = cxl_hybrid_page_state_pack(CXL_HYBRID_PAGE_STATE_IN_FLIGHT,
                                          owner,
                                          CXL_HYBRID_PAGE_LOCATION_NONE,
                                          generation, dirty_seq);
        if (qatomic_cmpxchg(slot, old, next) == old) {
            claim->observed = next;
            claim->generation = generation;
            claim->dirty_seq = dirty_seq;
            claim->owner = owner;
            return true;
        }
    }
}

bool cxl_hybrid_page_state_complete(uint64_t *slot,
                                    const CXLHybridPageClaim *claim,
                                    CXLHybridPageLocation location)
{
    uint64_t published;

    if (!slot || !claim || location == CXL_HYBRID_PAGE_LOCATION_NONE) {
        return false;
    }

    published = cxl_hybrid_page_state_make_published(
        claim->generation, location, claim->dirty_seq);
    return qatomic_cmpxchg(slot, claim->observed, published) ==
           claim->observed;
}

void cxl_hybrid_page_state_mark_dirty(uint64_t *slot,
                                      uint32_t generation,
                                      uint32_t dirty_seq)
{
    uint64_t old;
    uint64_t next;

    if (!slot) {
        return;
    }

    next = cxl_hybrid_page_state_make_dirty(generation, dirty_seq);
    do {
        old = qatomic_load_acquire(slot);
    } while (qatomic_cmpxchg(slot, old, next) != old);
}

bool cxl_hybrid_page_state_can_consume(uint64_t word,
                                       uint32_t generation,
                                       CXLHybridPageLocation location)
{
    return cxl_hybrid_page_state_kind(word) ==
           CXL_HYBRID_PAGE_STATE_PUBLISHED &&
           cxl_hybrid_page_state_generation(word) == generation &&
           cxl_hybrid_page_state_location(word) == location;
}
```

- [ ] **Step 5: Add the new file to Meson**

Modify `migration/meson.build` by adding this line immediately after `'cxl-hybrid-control-header.c',` in `migration_files`:

```meson
  'cxl-page-state.c',
```

Add the same line immediately after `'cxl-hybrid-control.c',` in the `system_ss.add(files(` source list.

- [ ] **Step 6: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS, including the three new `/cxl-hybrid-control/page-state-*` tests.

- [ ] **Step 7: Commit**

Run:

```bash
git add migration/cxl.h migration/cxl-page-state.c migration/meson.build \
        tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: add page state transition helpers"
```

## Task 2: Add Shared Control Page-State Array In Shadow Mode

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-hybrid-control.c`
- Modify: `migration/cxl-hybrid-control-header.c`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing layout tests**

Add tests to `tests/unit/test-cxl-hybrid-control.c`:

```c
static void test_page_state_bitmap_bytes_match_total_pages(void)
{
    g_assert_cmpuint(cxl_hybrid_control_page_state_bytes(0), ==, 0);
    g_assert_cmpuint(cxl_hybrid_control_page_state_bytes(1), ==,
                     sizeof(uint64_t));
    g_assert_cmpuint(cxl_hybrid_control_page_state_bytes(65), ==,
                     65 * sizeof(uint64_t));
}

static void test_header_reset_initializes_page_state_words(void)
{
    CXLHybridControlHeader hdr = { 0 };
    uint64_t page_state[4] = { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX };

    cxl_hybrid_control_reset_run_state(&hdr, NULL,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, 5);

    g_assert_cmpuint(hdr.page_state_words, ==, G_N_ELEMENTS(page_state));
    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[i]), ==,
                         CXL_HYBRID_PAGE_STATE_NOT_SENT);
        g_assert_cmpuint(cxl_hybrid_page_state_generation(page_state[i]), ==, 5);
    }
}
```

Register:

```c
g_test_add_func("/cxl-hybrid-control/page-state-bytes",
                test_page_state_bitmap_bytes_match_total_pages);
g_test_add_func("/cxl-hybrid-control/header-reset-page-state",
                test_header_reset_initializes_page_state_words);
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: FAIL because `page_state_words`, `cxl_hybrid_control_page_state_bytes()`, and the extended reset signature do not exist.

- [ ] **Step 3: Extend the control header**

In `migration/cxl.h`, add fields to `CXLHybridControlHeader` after `visible_page_words`:

```c
uint32_t page_state_words;
uint32_t page_state_word_size;
```

Add prototypes:

```c
size_t cxl_hybrid_control_page_state_bytes(uint64_t pages);
```

Extend `cxl_hybrid_control_reset_run_state()` signature to include:

```c
uint64_t *page_state,
uint64_t page_state_words,
```

Place those arguments immediately after `visible_pages`:

```c
void cxl_hybrid_control_reset_run_state(CXLHybridControlHeader *hdr,
                                        unsigned long *visible_bitmap,
                                        uint64_t visible_pages,
                                        uint64_t *page_state,
                                        uint64_t page_state_words,
                                        unsigned long *visible_region_bitmap,
                                        uint64_t visible_regions,
                                        unsigned long *owned_region_bitmap,
                                        uint64_t total_regions,
                                        uint64_t region_granule,
                                        uint32_t target_page_shift,
                                        uint32_t generation);
```

- [ ] **Step 4: Implement header reset behavior**

In `migration/cxl-hybrid-control-header.c`, implement:

```c
size_t cxl_hybrid_control_page_state_bytes(uint64_t pages)
{
    return pages * sizeof(uint64_t);
}
```

In `cxl_hybrid_control_reset_run_state()`, set:

```c
hdr->page_state_words = page_state_words;
hdr->page_state_word_size = sizeof(uint64_t);
if (page_state) {
    for (uint64_t page = 0; page < page_state_words; page++) {
        page_state[page] = cxl_hybrid_page_state_make_not_sent(generation);
    }
}
```

Update every caller of `cxl_hybrid_control_reset_run_state()` and `cxl_hybrid_control_reset_header_for_run()` with the new arguments.

Use this replacement in `cxl_hybrid_control_reset_header_for_run()`:

```c
cxl_hybrid_control_reset_run_state(hdr, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                                   0, 0, generation);
```

- [ ] **Step 5: Map the page-state array**

In `migration/cxl-hybrid-control.c`, extend `CXLHybridControlRegion` and `CXLHybridControlState`:

```c
uint64_t *page_state;
uint32_t page_state_words;
```

In `cxl_hybrid_fault_control_region_bytes()`, include:

```c
size_t page_state_bytes =
    cxl_hybrid_control_page_state_bytes(total_pages);
```

In `cxl_hybrid_ctrl_region_ensure()`, lay out memory as:

```text
header
visible page bitmap
page-state array
visible region bitmap
owned region bitmap
request ring
```

Set `cxl_hybrid_control_region.page_state` to the address immediately after the visible bitmap.

- [ ] **Step 6: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS.

- [ ] **Step 7: Commit**

Run:

```bash
git add migration/cxl.h migration/cxl-hybrid-control.c \
        migration/cxl-hybrid-control-header.c \
        tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: map shared page state control array"
```

## Task 3: Mirror Existing Visibility Into Page State

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-hybrid-control.c`
- Modify: `migration/cxl-hybrid-control-header.c`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing mirror tests**

Add:

```c
static void test_mark_page_visible_sets_page_state_cxl(void)
{
    CXLHybridControlHeader hdr = { 0 };
    unsigned long visible[1] = { 0 };
    uint64_t page_state[8];

    cxl_hybrid_control_reset_run_state(&hdr, visible,
                                       G_N_ELEMENTS(page_state),
                                       page_state,
                                       G_N_ELEMENTS(page_state),
                                       NULL, 0, NULL, 0,
                                       64 * 1024, 12, 4);

    cxl_hybrid_control_mark_page_visible_generation(
        &hdr, visible, page_state, 3, 4,
        CXL_HYBRID_PAGE_LOCATION_CXL);

    g_assert_true(test_bit(3, visible));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[3]), ==,
                     CXL_HYBRID_PAGE_STATE_PUBLISHED);
    g_assert_cmpuint(cxl_hybrid_page_state_location(page_state[3]), ==,
                     CXL_HYBRID_PAGE_LOCATION_CXL);
}
```

Register it:

```c
g_test_add_func("/cxl-hybrid-control/page-visible-mirrors-page-state",
                test_mark_page_visible_sets_page_state_cxl);
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: FAIL because the page visibility helper has not been extended with page-state mirroring.

- [ ] **Step 3: Extend visible helpers**

Update these prototypes in `migration/cxl.h` and implementations in `migration/cxl-hybrid-control-header.c` so they accept an optional `uint64_t *page_state`:

```c
void cxl_hybrid_control_mark_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    CXLHybridPageLocation location);
```

For range helpers, pass the same `location` for every page in the range. Existing callers that only maintain the visible bitmap should pass `state->page_state` and `CXL_HYBRID_PAGE_LOCATION_CXL`.

- [ ] **Step 4: Preserve release ordering**

In `cxl_hybrid_control_mark_page_visible_generation()`, write page data before state publication in callers. Inside the helper, use:

```c
if (page_state && page_index < hdr->page_state_words) {
    smp_mb_release();
    qatomic_set(&page_state[page_index],
                cxl_hybrid_page_state_make_published(generation,
                                                     location, 0));
}
set_bit_atomic(page_index, visible_bitmap);
```

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
git add migration/cxl.h migration/cxl-page-state.c \
        migration/cxl-hybrid-control.c \
        migration/cxl-hybrid-control-header.c \
        tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: mirror visible pages into page state"
```

## Task 4: Add CXL Transfer Queue Types

**Files:**
- Create: `migration/cxl-page-transfer.c`
- Modify: `migration/cxl.h`
- Modify: `migration/meson.build`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing queue tests**

Add:

```c
static void test_cxl_transfer_queue_drains_high_before_low(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor out = { 0 };

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_LOW,
        &(CXLHybridPageDescriptor) { .page_index = 10 });
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
        &(CXLHybridPageDescriptor) { .page_index = 20 });

    g_assert_true(cxl_hybrid_transfer_queue_pop(&queue, &out));
    g_assert_cmpuint(out.page_index, ==, 20);
    g_assert_true(cxl_hybrid_transfer_queue_pop(&queue, &out));
    g_assert_cmpuint(out.page_index, ==, 10);
    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_cxl_transfer_queue_duplicate_descriptor_is_allowed(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor out = { 0 };

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
        &(CXLHybridPageDescriptor) { .page_index = 7 });
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
        &(CXLHybridPageDescriptor) { .page_index = 7 });

    g_assert_true(cxl_hybrid_transfer_queue_pop(&queue, &out));
    g_assert_cmpuint(out.page_index, ==, 7);
    g_assert_true(cxl_hybrid_transfer_queue_pop(&queue, &out));
    g_assert_cmpuint(out.page_index, ==, 7);
    g_assert_false(cxl_hybrid_transfer_queue_pop(&queue, &out));
    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}
```

Register:

```c
g_test_add_func("/cxl-hybrid-control/transfer-queue-high-before-low",
                test_cxl_transfer_queue_drains_high_before_low);
g_test_add_func("/cxl-hybrid-control/transfer-queue-allows-duplicates",
                test_cxl_transfer_queue_duplicate_descriptor_is_allowed);
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: FAIL because transfer queue types do not exist.

- [ ] **Step 3: Add queue types**

Add to `migration/cxl.h`:

```c
typedef enum CXLHybridTransferClass {
    CXL_HYBRID_TRANSFER_CXL_HIGH = 0,
    CXL_HYBRID_TRANSFER_CXL_LOW = 1,
    CXL_HYBRID_TRANSFER_RDMA_BULK = 2,
    CXL_HYBRID_TRANSFER_RDMA_PREFETCH = 3,
    CXL_HYBRID_TRANSFER_CLASS_COUNT,
} CXLHybridTransferClass;

typedef struct CXLHybridPageDescriptor {
    RAMBlock *block;
    ram_addr_t block_offset;
    uint64_t page_index;
    uint64_t cxl_offset;
    uint32_t generation;
    uint32_t nr_pages;
} CXLHybridPageDescriptor;

typedef struct CXLHybridTransferQueue {
    QemuMutex lock;
    GQueue classes[CXL_HYBRID_TRANSFER_CLASS_COUNT];
    bool lock_ready;
} CXLHybridTransferQueue;
```

Expose test helpers:

```c
void cxl_hybrid_transfer_queue_init_for_test(CXLHybridTransferQueue *queue);
void cxl_hybrid_transfer_queue_destroy_for_test(CXLHybridTransferQueue *queue);
void cxl_hybrid_transfer_queue_push(CXLHybridTransferQueue *queue,
                                    CXLHybridTransferClass klass,
                                    const CXLHybridPageDescriptor *desc);
bool cxl_hybrid_transfer_queue_pop(CXLHybridTransferQueue *queue,
                                   CXLHybridPageDescriptor *desc);
```

- [ ] **Step 4: Implement queue helpers**

Create `migration/cxl-page-transfer.c`:

```c
#include "qemu/osdep.h"
#include "migration/cxl.h"

void cxl_hybrid_transfer_queue_init_for_test(CXLHybridTransferQueue *queue)
{
    int i;

    memset(queue, 0, sizeof(*queue));
    qemu_mutex_init(&queue->lock);
    queue->lock_ready = true;
    for (i = 0; i < CXL_HYBRID_TRANSFER_CLASS_COUNT; i++) {
        g_queue_init(&queue->classes[i]);
    }
}

void cxl_hybrid_transfer_queue_destroy_for_test(CXLHybridTransferQueue *queue)
{
    int i;

    if (!queue || !queue->lock_ready) {
        return;
    }
    for (i = 0; i < CXL_HYBRID_TRANSFER_CLASS_COUNT; i++) {
        g_queue_clear_full(&queue->classes[i], g_free);
    }
    qemu_mutex_destroy(&queue->lock);
    queue->lock_ready = false;
}

void cxl_hybrid_transfer_queue_push(CXLHybridTransferQueue *queue,
                                    CXLHybridTransferClass klass,
                                    const CXLHybridPageDescriptor *desc)
{
    CXLHybridPageDescriptor *copy;

    if (!queue || !queue->lock_ready || !desc ||
        klass >= CXL_HYBRID_TRANSFER_CLASS_COUNT) {
        return;
    }

    copy = g_new(CXLHybridPageDescriptor, 1);
    *copy = *desc;
    qemu_mutex_lock(&queue->lock);
    g_queue_push_tail(&queue->classes[klass], copy);
    qemu_mutex_unlock(&queue->lock);
}

bool cxl_hybrid_transfer_queue_pop(CXLHybridTransferQueue *queue,
                                   CXLHybridPageDescriptor *desc)
{
    static const CXLHybridTransferClass order[] = {
        CXL_HYBRID_TRANSFER_CXL_HIGH,
        CXL_HYBRID_TRANSFER_CXL_LOW,
        CXL_HYBRID_TRANSFER_RDMA_BULK,
        CXL_HYBRID_TRANSFER_RDMA_PREFETCH,
    };
    CXLHybridPageDescriptor *copy;
    size_t i;

    if (!queue || !queue->lock_ready || !desc) {
        return false;
    }

    qemu_mutex_lock(&queue->lock);
    for (i = 0; i < G_N_ELEMENTS(order); i++) {
        copy = g_queue_pop_head(&queue->classes[order[i]]);
        if (copy) {
            *desc = *copy;
            g_free(copy);
            qemu_mutex_unlock(&queue->lock);
            return true;
        }
    }
    qemu_mutex_unlock(&queue->lock);
    return false;
}
```

- [ ] **Step 5: Add file to Meson**

Add this line immediately after `'cxl-page-state.c',` in both `migration_files` and the `system_ss.add(files(` source list in `migration/meson.build`:

```meson
  'cxl-page-transfer.c',
```

- [ ] **Step 6: Run tests and commit**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS.

Commit:

```bash
git add migration/cxl.h migration/cxl-page-transfer.c migration/meson.build \
        tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: add page transfer queues"
```

## Task 5: Shadow Bulk Scheduler Classification

**Files:**
- Modify: `migration/ram.c`
- Modify: `migration/cxl.h`
- Modify: `migration/cxl.c`
- Modify: `migration/cxl-page-transfer.c`
- Test: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write failing selector tests**

Add pure tests in `tests/unit/test-cxl-hybrid-region.c`:

```c
static void test_scheduler_prefers_cxl_low_when_rdma_budget_exhausted(void)
{
    CXLHybridSchedulerPolicy policy = {
        .rdma_budget_pages = 0,
        .cxl_background_pages = 512,
    };

    g_assert_cmpuint(cxl_hybrid_scheduler_choose_bulk_lane(&policy, 12), ==,
                     CXL_HYBRID_TRANSFER_CXL_LOW);
}

static void test_scheduler_skips_zero_page_for_rdma(void)
{
    CXLHybridSchedulerPolicy policy = {
        .rdma_budget_pages = 512,
        .cxl_background_pages = 512,
    };

    g_assert_cmpuint(cxl_hybrid_scheduler_choose_zero_page_lane(&policy), ==,
                     CXL_HYBRID_TRANSFER_CXL_LOW);
}
```

Register them in `main()`:

```c
g_test_add_func("/cxl/region/scheduler-rdma-budget-exhausted",
                test_scheduler_prefers_cxl_low_when_rdma_budget_exhausted);
g_test_add_func("/cxl/region/scheduler-zero-page-cxl",
                test_scheduler_skips_zero_page_for_rdma);
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
```

Expected: FAIL because `CXLHybridSchedulerPolicy` and scheduler helpers do not exist.

- [ ] **Step 3: Add shadow policy helpers**

Declare in `migration/cxl.h`:

```c
typedef struct CXLHybridSchedulerPolicy {
    uint64_t rdma_budget_pages;
    uint64_t cxl_background_pages;
} CXLHybridSchedulerPolicy;

CXLHybridTransferClass cxl_hybrid_scheduler_choose_bulk_lane(
    const CXLHybridSchedulerPolicy *policy,
    uint64_t page_index);
CXLHybridTransferClass cxl_hybrid_scheduler_choose_zero_page_lane(
    const CXLHybridSchedulerPolicy *policy);
void cxl_hybrid_account_shadow_bulk_candidate(RAMBlock *block,
                                               ram_addr_t block_offset);
```

Implement in `migration/cxl-page-transfer.c`:

```c
CXLHybridTransferClass cxl_hybrid_scheduler_choose_zero_page_lane(
    const CXLHybridSchedulerPolicy *policy)
{
    (void)policy;

    return CXL_HYBRID_TRANSFER_CXL_LOW;
}

CXLHybridTransferClass cxl_hybrid_scheduler_choose_bulk_lane(
    const CXLHybridSchedulerPolicy *policy,
    uint64_t page_index)
{
    (void)page_index;

    if (policy && policy->rdma_budget_pages) {
        return CXL_HYBRID_TRANSFER_RDMA_BULK;
    }
    return CXL_HYBRID_TRANSFER_CXL_LOW;
}
```

Implement the shadow accounting entry point in `migration/cxl.c` as a no-op in Phase 1 so the scheduler hook is compiled but cannot affect migration behavior:

```c
void cxl_hybrid_account_shadow_bulk_candidate(RAMBlock *block,
                                               ram_addr_t block_offset)
{
    (void)block;
    (void)block_offset;
}
```

- [ ] **Step 4: Add shadow classification call**

In `migration/ram.c`, add a shadow helper near `cxl_hybrid_rdma_enqueue_bulk_region()`:

```c
static void cxl_hybrid_shadow_classify_bulk_page(RAMState *rs,
                                                 PageSearchStatus *pss)
{
    (void)rs;

    if (!migrate_cxl_hybrid()) {
        return;
    }
    cxl_hybrid_account_shadow_bulk_candidate(pss->block,
                                             pss->page << TARGET_PAGE_BITS);
}
```

Call it before the existing RDMA region claim:

```c
cxl_hybrid_shadow_classify_bulk_page(rs, pss);
```

This task must not change dirty-bit clearing or data movement.

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
git add migration/cxl.h migration/cxl.c migration/ram.c \
        migration/cxl-page-transfer.c tests/unit/test-cxl-hybrid-region.c
git commit -m "migration/cxl: add shadow bulk scheduler classification"
```

## Phase 1 Verification Checkpoint

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-control --tap
./build/tests/unit/test-cxl-hybrid-region --tap
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py \
                   scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: all commands PASS.

Commit any parser-only fixes separately with:

```bash
git add scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "scripts: report cxl page state shadow metrics"
```

## Phase 2 Preview

Do not start Phase 2 until Phase 1 verification passes.

Phase 2 tasks will implement:

- CXL poller single-page copy using `CXL_HYBRID_PAGE_OWNER_CXL`;
- CXL worker high/low queue draining and lane-local merge;
- fault-region dirty-page enqueue into `CXL_HIGH`;
- RDMA descriptor masks and per-page completion to `PUBLISHED@DST_LOCAL`;
- posted RDMA write race tests;
- longest-valid-span remap helper and destination remap integration.

The first Phase 2 task should be a pure test for longest-valid-span selection so the remap policy is locked before changing the fault path.
