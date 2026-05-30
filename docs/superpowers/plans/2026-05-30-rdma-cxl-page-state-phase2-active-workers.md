# RDMA CXL Page-State Phase 2 Active Workers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move from Phase 1 shadow page-state accounting to active page-state driven CXL/RDMA workers, while preserving real RDMA placement to destination RAM and removing whole-region CXL republish from the active path.

**Architecture:** The shared CXL page-state array becomes the arbitration point for worker-owned pages. The source control worker is split conceptually into a one-page CXL fault poller plus a CXL high/low priority worker, while RDMA consumes region descriptors with per-page claims and completes pages to `PUBLISHED@DST_LOCAL`. Remap changes start with a pure longest-valid-span helper before touching the destination remap path.

**Tech Stack:** QEMU migration C code, CXL shared DAX control/data mapping, qatomic CAS helpers, QemuThread/QemuMutex/QemuCond, real RDMA verbs sidecar, GLib unit tests, Meson/Ninja, Python experiment parser.

---

## Current Baseline

Work in:

```bash
cd /home/xiexinchen/.config/superpowers/worktrees/qemu/rdma-cxl-parallel-hybrid
```

Required prior checkpoint:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-control --tap
./build/tests/unit/test-cxl-hybrid-region --tap
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py \
                   scripts/cxl-hybrid-warm-experiment-test.py
```

Phase 1 commits that this plan builds on:

```text
6cfc4df373 migration/cxl: add page state transition helpers
8131bc878d migration/cxl: map shared page state control array
016f39a394 migration/cxl: mirror visible pages into page state
89ab433676 migration/cxl: add page transfer queues
20af230e35 migration/cxl: add shadow bulk scheduler classification
932a99e2ed tests: stub cxl rdma backing in hybrid control test
```

Design spec:

```text
docs/superpowers/specs/2026-05-30-rdma-cxl-page-state-control-plane-design.md
```

## Phase 2 Scope

Phase 2 makes page-state workers active, but keeps the old region control code available until final cleanup. Do not delete region-ready or republish APIs in this phase unless a task explicitly replaces every caller.

Exit criteria:

- destination remap span selection is page-state based and tested as a pure helper;
- CXL queue APIs can pop only CXL work and preserve high-priority ordering;
- the fault poller copies only the demanded page before waking the fault;
- the fault poller enqueues remaining dirty pages from the fault region into `CXL_HIGH`;
- the CXL worker consumes CXL descriptors and completes pages through page-state;
- RDMA descriptors carry per-page claims and complete only matching pages to `PUBLISHED@DST_LOCAL`;
- posted RDMA write race behavior is covered by tests before postcopy RDMA prefetch is enabled;
- bulk scheduling can enqueue both CXL and RDMA page-owned work without letting RDMA clear an entire region before CXL can claim its share;
- metrics report CXL worker bytes/time and RDMA completed bytes/time separately.

## Stop-And-Discuss Gates

Stop before continuing if any of these happen:

- RDMA remote MR cannot safely point at destination RAM before descriptors are posted;
- a page fault can arrive while an RDMA write covering that page is posted but cannot be waited on or failed back to CXL safely;
- CXL worker queue wakeup requires a new migration lifecycle hook that is larger than this phase;
- speculative span scan adds measurable fault tail latency before a scan cap exists;
- the main migration thread still performs CXL bulk memcpy after the worker path is enabled.

## File Structure

Modify these existing files:

- `migration/cxl.h`: remap span structs/prototypes, queue pop/depth helpers, worker stats, RDMA descriptor types.
- `migration/cxl-page-state.c`: small pure helpers that classify consumable page-state words.
- `migration/cxl-page-transfer.c`: lane-specific queue pop/depth helpers and descriptor cloning rules.
- `migration/cxl-hybrid-control.c`: request-worker split, CXL high/low worker queues, one-page poller demand service.
- `migration/cxl.c`: CXL descriptor copy/completion helpers, longest-valid-span integration, query metrics.
- `migration/cxl-rdma.c`: RDMA region descriptor page masks, per-page completion, posted-write state.
- `migration/ram.c`: active bulk scheduler classification and per-page claim/enqueue.
- `scripts/cxl-hybrid-warm-experiment.py`: parse lane bytes/time and page-state counters.
- `scripts/cxl-hybrid-warm-experiment-test.py`: parser tests.
- `tests/unit/test-cxl-hybrid-control.c`: page-state, queue, poller, and remap span tests.
- `tests/unit/test-cxl-hybrid-region.c`: RDMA descriptor and posted-write race tests.

## Task 6: Add Longest Valid CXL Remap Span Helper

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-page-state.c`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing span tests**

Add these tests near the existing page-state tests in `tests/unit/test-cxl-hybrid-control.c`:

```c
static void test_remap_span_grows_over_contiguous_cxl_pages(void)
{
    uint64_t page_state[8];
    CXLHybridRemapSpan span = { 0 };
    uint32_t generation = 13;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_published(
            generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    }
    page_state[1] = cxl_hybrid_page_state_make_dirty(generation, 2);
    page_state[6] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL, 0);

    g_assert_true(cxl_hybrid_page_state_longest_cxl_span(
        page_state, G_N_ELEMENTS(page_state), 4, generation, 0, &span));
    g_assert_cmpuint(span.first_page, ==, 2);
    g_assert_cmpuint(span.nr_pages, ==, 4);
}

static void test_remap_span_rejects_non_cxl_fault_page(void)
{
    uint64_t page_state[4];
    CXLHybridRemapSpan span = { 0 };
    uint32_t generation = 3;

    page_state[0] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    page_state[1] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL, 0);
    page_state[2] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    page_state[3] = cxl_hybrid_page_state_make_not_sent(generation);

    g_assert_false(cxl_hybrid_page_state_longest_cxl_span(
        page_state, G_N_ELEMENTS(page_state), 1, generation, 0, &span));
}

static void test_remap_span_honors_scan_cap(void)
{
    uint64_t page_state[16];
    CXLHybridRemapSpan span = { 0 };
    uint32_t generation = 9;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_published(
            generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);
    }

    g_assert_true(cxl_hybrid_page_state_longest_cxl_span(
        page_state, G_N_ELEMENTS(page_state), 8, generation, 5, &span));
    g_assert_cmpuint(span.first_page, ==, 6);
    g_assert_cmpuint(span.nr_pages, ==, 5);
}
```

Register:

```c
g_test_add_func("/cxl-hybrid-control/remap-span-grows-contiguous-cxl",
                test_remap_span_grows_over_contiguous_cxl_pages);
g_test_add_func("/cxl-hybrid-control/remap-span-rejects-non-cxl-fault-page",
                test_remap_span_rejects_non_cxl_fault_page);
g_test_add_func("/cxl-hybrid-control/remap-span-honors-scan-cap",
                test_remap_span_honors_scan_cap);
```

- [ ] **Step 2: Verify the tests fail**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: FAIL because `CXLHybridRemapSpan` and `cxl_hybrid_page_state_longest_cxl_span()` do not exist.

- [ ] **Step 3: Add the public span type and prototype**

Add to `migration/cxl.h` after `CXLHybridPageDescriptor`:

```c
typedef struct CXLHybridRemapSpan {
    uint64_t first_page;
    uint32_t nr_pages;
} CXLHybridRemapSpan;
```

Add prototype near the page-state helper prototypes:

```c
bool cxl_hybrid_page_state_longest_cxl_span(const uint64_t *page_state,
                                            uint64_t total_pages,
                                            uint64_t fault_page,
                                            uint32_t generation,
                                            uint32_t max_pages,
                                            CXLHybridRemapSpan *span);
```

- [ ] **Step 4: Implement the pure helper**

Add to `migration/cxl-page-state.c`:

```c
static bool cxl_hybrid_page_state_word_is_cxl_published(uint64_t word,
                                                        uint32_t generation)
{
    return cxl_hybrid_page_state_can_consume(
        word, generation, CXL_HYBRID_PAGE_LOCATION_CXL);
}

bool cxl_hybrid_page_state_longest_cxl_span(const uint64_t *page_state,
                                            uint64_t total_pages,
                                            uint64_t fault_page,
                                            uint32_t generation,
                                            uint32_t max_pages,
                                            CXLHybridRemapSpan *span)
{
    uint64_t first;
    uint64_t last;
    uint64_t cap_left;
    uint64_t cap_right;

    if (!page_state || !span || fault_page >= total_pages ||
        !cxl_hybrid_page_state_word_is_cxl_published(
            qatomic_load_acquire(&page_state[fault_page]), generation)) {
        return false;
    }

    first = fault_page;
    last = fault_page + 1;
    cap_left = max_pages ? (max_pages - 1) / 2 : UINT64_MAX;
    cap_right = max_pages ? (max_pages - 1) - cap_left : UINT64_MAX;

    while (first > 0 && cap_left > 0 &&
           cxl_hybrid_page_state_word_is_cxl_published(
               qatomic_load_acquire(&page_state[first - 1]), generation)) {
        first--;
        cap_left--;
    }
    while (last < total_pages && cap_right > 0 &&
           cxl_hybrid_page_state_word_is_cxl_published(
               qatomic_load_acquire(&page_state[last]), generation)) {
        last++;
        cap_right--;
    }

    span->first_page = first;
    span->nr_pages = last - first;
    return span->nr_pages > 0 && span->nr_pages <= UINT32_MAX;
}
```

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS, including the three `/cxl-hybrid-control/remap-span-*` tests.

- [ ] **Step 6: Commit**

Run:

```bash
git add migration/cxl.h migration/cxl-page-state.c \
        tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: add page-state remap span helper"
```

## Task 7: Add Lane-Specific Transfer Queue Operations

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-page-transfer.c`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing queue tests**

Add to `tests/unit/test-cxl-hybrid-control.c` near existing transfer queue tests:

```c
static void test_transfer_queue_pop_cxl_ignores_rdma(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor rdma = { .page_index = 10, .nr_pages = 2 };
    CXLHybridPageDescriptor cxl = { .page_index = 20, .nr_pages = 1 };
    CXLHybridPageDescriptor out = { 0 };
    CXLHybridTransferClass klass = CXL_HYBRID_TRANSFER_CLASS_COUNT;

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_RDMA_BULK,
                                   &rdma);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_LOW,
                                   &cxl);

    g_assert_true(cxl_hybrid_transfer_queue_pop_cxl(&queue, &out, &klass));
    g_assert_cmpuint(klass, ==, CXL_HYBRID_TRANSFER_CXL_LOW);
    g_assert_cmpuint(out.page_index, ==, 20);
    g_assert_cmpuint(cxl_hybrid_transfer_queue_depth(
                         &queue, CXL_HYBRID_TRANSFER_RDMA_BULK), ==, 1);

    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}

static void test_transfer_queue_pop_rdma_ignores_cxl(void)
{
    CXLHybridTransferQueue queue;
    CXLHybridPageDescriptor cxl = { .page_index = 7, .nr_pages = 1 };
    CXLHybridPageDescriptor rdma = { .page_index = 8, .nr_pages = 4 };
    CXLHybridPageDescriptor out = { 0 };
    CXLHybridTransferClass klass = CXL_HYBRID_TRANSFER_CLASS_COUNT;

    cxl_hybrid_transfer_queue_init_for_test(&queue);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_CXL_HIGH,
                                   &cxl);
    cxl_hybrid_transfer_queue_push(&queue, CXL_HYBRID_TRANSFER_RDMA_PREFETCH,
                                   &rdma);

    g_assert_true(cxl_hybrid_transfer_queue_pop_rdma(&queue, &out, &klass));
    g_assert_cmpuint(klass, ==, CXL_HYBRID_TRANSFER_RDMA_PREFETCH);
    g_assert_cmpuint(out.page_index, ==, 8);
    g_assert_cmpuint(cxl_hybrid_transfer_queue_depth(
                         &queue, CXL_HYBRID_TRANSFER_CXL_HIGH), ==, 1);

    cxl_hybrid_transfer_queue_destroy_for_test(&queue);
}
```

Register:

```c
g_test_add_func("/cxl-hybrid-control/transfer-queue-pop-cxl-ignores-rdma",
                test_transfer_queue_pop_cxl_ignores_rdma);
g_test_add_func("/cxl-hybrid-control/transfer-queue-pop-rdma-ignores-cxl",
                test_transfer_queue_pop_rdma_ignores_cxl);
```

- [ ] **Step 2: Verify the tests fail**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: FAIL because the lane-specific pop and depth helpers do not exist.

- [ ] **Step 3: Add prototypes**

Add to `migration/cxl.h` after `cxl_hybrid_transfer_queue_pop()`:

```c
bool cxl_hybrid_transfer_queue_pop_cxl(CXLHybridTransferQueue *queue,
                                       CXLHybridPageDescriptor *desc,
                                       CXLHybridTransferClass *klass);
bool cxl_hybrid_transfer_queue_pop_rdma(CXLHybridTransferQueue *queue,
                                        CXLHybridPageDescriptor *desc,
                                        CXLHybridTransferClass *klass);
uint64_t cxl_hybrid_transfer_queue_depth(CXLHybridTransferQueue *queue,
                                         CXLHybridTransferClass klass);
```

- [ ] **Step 4: Implement lane-specific pop**

Add to `migration/cxl-page-transfer.c`:

```c
static bool cxl_hybrid_transfer_queue_pop_ordered(
    CXLHybridTransferQueue *queue,
    const CXLHybridTransferClass *order,
    size_t order_len,
    CXLHybridPageDescriptor *desc,
    CXLHybridTransferClass *klass)
{
    CXLHybridPageDescriptor *copy;

    if (!queue || !queue->lock_ready || !desc || !order || !order_len) {
        return false;
    }

    qemu_mutex_lock(&queue->lock);
    for (size_t i = 0; i < order_len; i++) {
        copy = g_queue_pop_head(&queue->classes[order[i]]);
        if (copy) {
            *desc = *copy;
            if (klass) {
                *klass = order[i];
            }
            g_free(copy);
            qemu_mutex_unlock(&queue->lock);
            return true;
        }
    }
    qemu_mutex_unlock(&queue->lock);
    return false;
}

bool cxl_hybrid_transfer_queue_pop_cxl(CXLHybridTransferQueue *queue,
                                       CXLHybridPageDescriptor *desc,
                                       CXLHybridTransferClass *klass)
{
    static const CXLHybridTransferClass order[] = {
        CXL_HYBRID_TRANSFER_CXL_HIGH,
        CXL_HYBRID_TRANSFER_CXL_LOW,
    };

    return cxl_hybrid_transfer_queue_pop_ordered(
        queue, order, G_N_ELEMENTS(order), desc, klass);
}

bool cxl_hybrid_transfer_queue_pop_rdma(CXLHybridTransferQueue *queue,
                                        CXLHybridPageDescriptor *desc,
                                        CXLHybridTransferClass *klass)
{
    static const CXLHybridTransferClass order[] = {
        CXL_HYBRID_TRANSFER_RDMA_BULK,
        CXL_HYBRID_TRANSFER_RDMA_PREFETCH,
    };

    return cxl_hybrid_transfer_queue_pop_ordered(
        queue, order, G_N_ELEMENTS(order), desc, klass);
}

uint64_t cxl_hybrid_transfer_queue_depth(CXLHybridTransferQueue *queue,
                                         CXLHybridTransferClass klass)
{
    uint64_t depth;

    if (!queue || !queue->lock_ready || (int)klass < 0 ||
        klass >= CXL_HYBRID_TRANSFER_CLASS_COUNT) {
        return 0;
    }

    qemu_mutex_lock(&queue->lock);
    depth = g_queue_get_length(&queue->classes[klass]);
    qemu_mutex_unlock(&queue->lock);
    return depth;
}
```

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS, including the two new lane queue tests.

- [ ] **Step 6: Commit**

Run:

```bash
git add migration/cxl.h migration/cxl-page-transfer.c \
        tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: add lane-specific transfer queue operations"
```

## Task 8: Add Page-State CXL Claim And Completion Wrappers

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-page-state.c`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing wrapper tests**

Add:

```c
static void test_cxl_page_claim_complete_publishes_cxl(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_dirty(4, 88);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_claim_for_cxl(&slot, 4, &claim));
    g_assert_cmpuint(claim.owner, ==, CXL_HYBRID_PAGE_OWNER_CXL);
    g_assert_true(cxl_hybrid_page_state_complete_cxl(&slot, &claim));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, 4, CXL_HYBRID_PAGE_LOCATION_CXL));
}

static void test_rdma_page_claim_complete_publishes_dst_local(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_not_sent(5);
    CXLHybridPageClaim claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_claim_for_rdma(&slot, 5, &claim));
    g_assert_cmpuint(claim.owner, ==, CXL_HYBRID_PAGE_OWNER_RDMA);
    g_assert_true(cxl_hybrid_page_state_complete_rdma(&slot, &claim));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, 5, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
}
```

Register:

```c
g_test_add_func("/cxl-hybrid-control/page-state-cxl-claim-complete-wrapper",
                test_cxl_page_claim_complete_publishes_cxl);
g_test_add_func("/cxl-hybrid-control/page-state-rdma-claim-complete-wrapper",
                test_rdma_page_claim_complete_publishes_dst_local);
```

- [ ] **Step 2: Verify failure**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: FAIL because wrapper helpers do not exist.

- [ ] **Step 3: Add prototypes**

Add to `migration/cxl.h`:

```c
bool cxl_hybrid_page_state_claim_for_cxl(uint64_t *slot,
                                         uint32_t generation,
                                         CXLHybridPageClaim *claim);
bool cxl_hybrid_page_state_claim_for_rdma(uint64_t *slot,
                                          uint32_t generation,
                                          CXLHybridPageClaim *claim);
bool cxl_hybrid_page_state_complete_cxl(uint64_t *slot,
                                        const CXLHybridPageClaim *claim);
bool cxl_hybrid_page_state_complete_rdma(uint64_t *slot,
                                         const CXLHybridPageClaim *claim);
```

- [ ] **Step 4: Implement wrappers**

Add to `migration/cxl-page-state.c`:

```c
bool cxl_hybrid_page_state_claim_for_cxl(uint64_t *slot,
                                         uint32_t generation,
                                         CXLHybridPageClaim *claim)
{
    return cxl_hybrid_page_state_try_claim(
        slot, CXL_HYBRID_PAGE_OWNER_CXL, generation, claim);
}

bool cxl_hybrid_page_state_claim_for_rdma(uint64_t *slot,
                                          uint32_t generation,
                                          CXLHybridPageClaim *claim)
{
    return cxl_hybrid_page_state_try_claim(
        slot, CXL_HYBRID_PAGE_OWNER_RDMA, generation, claim);
}

bool cxl_hybrid_page_state_complete_cxl(uint64_t *slot,
                                        const CXLHybridPageClaim *claim)
{
    return claim && claim->owner == CXL_HYBRID_PAGE_OWNER_CXL &&
           cxl_hybrid_page_state_complete(
               slot, claim, CXL_HYBRID_PAGE_LOCATION_CXL);
}

bool cxl_hybrid_page_state_complete_rdma(uint64_t *slot,
                                         const CXLHybridPageClaim *claim)
{
    return claim && claim->owner == CXL_HYBRID_PAGE_OWNER_RDMA &&
           cxl_hybrid_page_state_complete(
               slot, claim, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL);
}
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
        tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: add page-state lane claim wrappers"
```

## Task 9: Add RDMA Region Descriptor Page Claims

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-page-transfer.c`
- Test: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write failing RDMA descriptor tests**

Add to `tests/unit/test-cxl-hybrid-region.c`:

```c
static void test_rdma_descriptor_claims_only_requested_pages(void)
{
    uint64_t page_state[8];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 2;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }
    page_state[3] = cxl_hybrid_page_state_make_published(
        generation, CXL_HYBRID_PAGE_LOCATION_CXL, 0);

    g_assert_true(cxl_hybrid_rdma_descriptor_claim_pages_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 8, generation));
    g_assert_cmpuint(desc.first_page, ==, 0);
    g_assert_cmpuint(desc.nr_pages, ==, 8);
    g_assert_cmpuint(desc.claimed_pages, ==, 7);
    g_assert_false(cxl_hybrid_rdma_descriptor_page_claimed(&desc, 3));
    g_assert_true(cxl_hybrid_rdma_descriptor_page_claimed(&desc, 4));

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}

static void test_rdma_descriptor_completion_ignores_stale_page(void)
{
    uint64_t page_state[4];
    CXLHybridRDMAPageDescriptor desc = { 0 };
    uint32_t generation = 6;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i + 1);
    }
    g_assert_true(cxl_hybrid_rdma_descriptor_claim_pages_for_test(
        &desc, page_state, G_N_ELEMENTS(page_state), 0, 4, generation));
    cxl_hybrid_page_state_mark_dirty(&page_state[1], generation, 99);

    cxl_hybrid_rdma_descriptor_complete_pages_for_test(&desc, page_state,
                                                       G_N_ELEMENTS(page_state));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[0], generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[1]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[2], generation, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
    g_assert_cmpuint(desc.completed_pages, ==, 3);
    g_assert_cmpuint(desc.stale_pages, ==, 1);

    cxl_hybrid_rdma_descriptor_destroy(&desc);
}
```

Register:

```c
g_test_add_func("/cxl/region/rdma-descriptor-claims-requested-pages",
                test_rdma_descriptor_claims_only_requested_pages);
g_test_add_func("/cxl/region/rdma-descriptor-completion-ignores-stale-page",
                test_rdma_descriptor_completion_ignores_stale_page);
```

- [ ] **Step 2: Verify failure**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
```

Expected: FAIL because `CXLHybridRDMAPageDescriptor` and helper functions do not exist.

- [ ] **Step 3: Add descriptor type and prototypes**

Add to `migration/cxl.h` after `CXLHybridRDMABulkClaim`:

```c
typedef struct CXLHybridRDMAPageDescriptor {
    RAMBlock *block;
    ram_addr_t block_offset;
    uint64_t first_page;
    uint32_t nr_pages;
    unsigned long *claimed_bmap;
    CXLHybridPageClaim *claims;
    uint32_t generation;
    uint32_t claimed_pages;
    uint32_t posted_pages;
    uint32_t completed_pages;
    uint32_t stale_pages;
} CXLHybridRDMAPageDescriptor;
```

Add prototypes:

```c
bool cxl_hybrid_rdma_descriptor_claim_pages_for_test(
    CXLHybridRDMAPageDescriptor *desc,
    uint64_t *page_state,
    uint64_t total_pages,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation);
bool cxl_hybrid_rdma_descriptor_page_claimed(
    const CXLHybridRDMAPageDescriptor *desc,
    uint32_t page_offset);
void cxl_hybrid_rdma_descriptor_complete_pages_for_test(
    CXLHybridRDMAPageDescriptor *desc,
    uint64_t *page_state,
    uint64_t total_pages);
void cxl_hybrid_rdma_descriptor_destroy(CXLHybridRDMAPageDescriptor *desc);
```

- [ ] **Step 4: Implement testable descriptor helpers**

Add to `migration/cxl-page-transfer.c`:

```c
bool cxl_hybrid_rdma_descriptor_page_claimed(
    const CXLHybridRDMAPageDescriptor *desc,
    uint32_t page_offset)
{
    return desc && desc->claimed_bmap && page_offset < desc->nr_pages &&
           test_bit(page_offset, desc->claimed_bmap);
}

void cxl_hybrid_rdma_descriptor_destroy(CXLHybridRDMAPageDescriptor *desc)
{
    if (!desc) {
        return;
    }
    g_free(desc->claimed_bmap);
    g_free(desc->claims);
    memset(desc, 0, sizeof(*desc));
}

bool cxl_hybrid_rdma_descriptor_claim_pages_for_test(
    CXLHybridRDMAPageDescriptor *desc,
    uint64_t *page_state,
    uint64_t total_pages,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation)
{
    if (!desc || !page_state || !nr_pages ||
        first_page >= total_pages || nr_pages > total_pages - first_page) {
        return false;
    }

    memset(desc, 0, sizeof(*desc));
    desc->first_page = first_page;
    desc->nr_pages = nr_pages;
    desc->generation = generation;
    desc->claimed_bmap = bitmap_new(nr_pages);
    desc->claims = g_new0(CXLHybridPageClaim, nr_pages);

    for (uint32_t i = 0; i < nr_pages; i++) {
        if (cxl_hybrid_page_state_claim_for_rdma(&page_state[first_page + i],
                                                 generation,
                                                 &desc->claims[i])) {
            set_bit(i, desc->claimed_bmap);
            desc->claimed_pages++;
        }
    }

    return desc->claimed_pages > 0;
}

void cxl_hybrid_rdma_descriptor_complete_pages_for_test(
    CXLHybridRDMAPageDescriptor *desc,
    uint64_t *page_state,
    uint64_t total_pages)
{
    if (!desc || !page_state || desc->first_page >= total_pages) {
        return;
    }

    for (uint32_t i = 0; i < desc->nr_pages &&
         desc->first_page + i < total_pages; i++) {
        if (!cxl_hybrid_rdma_descriptor_page_claimed(desc, i)) {
            continue;
        }
        if (cxl_hybrid_page_state_complete_rdma(
                &page_state[desc->first_page + i], &desc->claims[i])) {
            desc->completed_pages++;
        } else {
            desc->stale_pages++;
        }
    }
}
```

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS, including the two new RDMA descriptor tests.

- [ ] **Step 6: Commit**

Run:

```bash
git add migration/cxl.h migration/cxl-page-transfer.c \
        tests/unit/test-cxl-hybrid-region.c
git commit -m "migration/cxl: add rdma page descriptor claims"
```

## Task 10: Convert Fault Region Requests To One-Page Poller Plus CXL High Queue

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-hybrid-control.c`
- Modify: `migration/cxl.c`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing request classification tests**

Add pure helper tests to `tests/unit/test-cxl-hybrid-control.c`:

```c
static void test_fault_region_plans_one_demand_and_prefetch_remainder(void)
{
    CXLHybridFaultRegionPlan plan = { 0 };

    g_assert_true(cxl_hybrid_fault_region_plan(100, 8, 103, &plan));
    g_assert_cmpuint(plan.demand_page, ==, 103);
    g_assert_cmpuint(plan.prefetch_first_page, ==, 100);
    g_assert_cmpuint(plan.prefetch_nr_pages, ==, 8);
    g_assert_cmpuint(plan.prefetch_skip_page, ==, 103);
}

static void test_fault_region_plan_rejects_out_of_span_demand(void)
{
    CXLHybridFaultRegionPlan plan = { 0 };

    g_assert_false(cxl_hybrid_fault_region_plan(100, 8, 108, &plan));
}
```

Register:

```c
g_test_add_func("/cxl-hybrid-control/fault-region-plan-demand-plus-prefetch",
                test_fault_region_plans_one_demand_and_prefetch_remainder);
g_test_add_func("/cxl-hybrid-control/fault-region-plan-rejects-out-of-span",
                test_fault_region_plan_rejects_out_of_span_demand);
```

- [ ] **Step 2: Verify failure**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: FAIL because `CXLHybridFaultRegionPlan` and `cxl_hybrid_fault_region_plan()` do not exist.

- [ ] **Step 3: Add plan type and prototype**

Add to `migration/cxl.h` after `CXLHybridFaultRegionGeometry`:

```c
typedef struct CXLHybridFaultRegionPlan {
    uint64_t demand_page;
    uint64_t prefetch_first_page;
    uint32_t prefetch_nr_pages;
    uint64_t prefetch_skip_page;
} CXLHybridFaultRegionPlan;

bool cxl_hybrid_fault_region_plan(uint64_t first_page,
                                  uint32_t nr_pages,
                                  uint64_t demand_page,
                                  CXLHybridFaultRegionPlan *plan);
```

- [ ] **Step 4: Implement pure plan helper**

Add to `migration/cxl-page-transfer.c`:

```c
bool cxl_hybrid_fault_region_plan(uint64_t first_page,
                                  uint32_t nr_pages,
                                  uint64_t demand_page,
                                  CXLHybridFaultRegionPlan *plan)
{
    if (!plan || !nr_pages || demand_page < first_page ||
        demand_page >= first_page + nr_pages) {
        return false;
    }

    plan->demand_page = demand_page;
    plan->prefetch_first_page = first_page;
    plan->prefetch_nr_pages = nr_pages;
    plan->prefetch_skip_page = demand_page;
    return true;
}
```

- [ ] **Step 5: Route region requests through the plan**

In `migration/cxl-hybrid-control.c:cxl_hybrid_ctrl_request_worker_thread()` replace the `CXL_HYBRID_FAULT_REQUEST_F_REGION` branch so it:

1. validates the span with `cxl_hybrid_ctrl_validate_region_span()`;
2. calls `cxl_hybrid_fault_region_plan(record.page_index, record.nr_pages, record.page_index, &plan)`;
3. publishes only `plan.demand_page` with `cxl_hybrid_publish_fault_request_core(..., emit_burst=false, ...)`;
4. enqueues every other page in the region to `CXL_HYBRID_TRANSFER_CXL_HIGH`.

Use this outline:

```c
CXLHybridFaultRegionPlan plan;

if (!cxl_hybrid_fault_region_plan(record.page_index, record.nr_pages,
                                  record.page_index, &plan)) {
    cxl_hybrid_ctrl_abort_generation(state, record.generation);
    goto request_done;
}
if (!cxl_hybrid_lookup_global_page(plan.demand_page, &block, &block_offset)) {
    cxl_hybrid_ctrl_abort_generation(state, record.generation);
    goto request_done;
}
ret = cxl_hybrid_publish_fault_request_core(qemu_ram_get_idstr(block),
                                            block_offset,
                                            TARGET_PAGE_SIZE,
                                            record.generation,
                                            false,
                                            &local_err);
if (ret) {
    cxl_hybrid_ctrl_abort_generation(state, record.generation);
    goto request_done;
}
cxl_hybrid_ctrl_enqueue_cxl_region_prefetch(state, &plan,
                                            record.generation);
goto request_done;
```

Add `cxl_hybrid_ctrl_enqueue_cxl_region_prefetch()` in the same file. It should resolve pages with `cxl_hybrid_lookup_global_page()`, skip `plan.prefetch_skip_page`, and push `CXLHybridPageDescriptor` entries to the CXL high queue. In this task the CXL high queue can be a control-state member initialized by `cxl_hybrid_transfer_queue_init_for_test()` style code; Task 11 starts the worker that drains it.

- [ ] **Step 6: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
./build/tests/unit/test-cxl-hybrid-control --tap
```

Expected: PASS. Existing region publication tests should still pass, but region request runtime now has a one-page demand path and high-priority queue population.

- [ ] **Step 7: Commit**

Run:

```bash
git add migration/cxl.h migration/cxl-page-transfer.c \
        migration/cxl-hybrid-control.c migration/cxl.c \
        tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: route fault regions through cxl high queue"
```

## Task 11: Start CXL Worker And Complete CXL Descriptors Through Page-State

**Files:**
- Modify: `migration/cxl.h`
- Modify: `migration/cxl-hybrid-control.c`
- Modify: `migration/cxl.c`
- Test: `tests/unit/test-cxl-hybrid-control.c`

- [ ] **Step 1: Write failing worker unit test**

Add a pure single-descriptor completion test:

```c
static void test_cxl_descriptor_completion_skips_stale_page(void)
{
    uint64_t page_state[3];
    CXLHybridPageClaim claims[3];
    uint32_t generation = 11;
    uint32_t completed = 0;
    uint32_t stale = 0;

    for (size_t i = 0; i < G_N_ELEMENTS(page_state); i++) {
        page_state[i] = cxl_hybrid_page_state_make_dirty(generation, i);
        g_assert_true(cxl_hybrid_page_state_claim_for_cxl(
            &page_state[i], generation, &claims[i]));
    }
    cxl_hybrid_page_state_mark_dirty(&page_state[1], generation, 44);

    cxl_hybrid_cxl_descriptor_complete_pages_for_test(
        page_state, G_N_ELEMENTS(page_state), 0, claims, 3,
        &completed, &stale);
    g_assert_cmpuint(completed, ==, 2);
    g_assert_cmpuint(stale, ==, 1);
    g_assert_true(cxl_hybrid_page_state_can_consume(
        page_state[0], generation, CXL_HYBRID_PAGE_LOCATION_CXL));
    g_assert_cmpuint(cxl_hybrid_page_state_kind(page_state[1]), ==,
                     CXL_HYBRID_PAGE_STATE_DIRTY);
}
```

Register:

```c
g_test_add_func("/cxl-hybrid-control/cxl-descriptor-completion-skips-stale",
                test_cxl_descriptor_completion_skips_stale_page);
```

- [ ] **Step 2: Verify failure**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control
```

Expected: FAIL because `cxl_hybrid_cxl_descriptor_complete_pages_for_test()` does not exist.

- [ ] **Step 3: Add prototype**

Add to `migration/cxl.h`:

```c
void cxl_hybrid_cxl_descriptor_complete_pages_for_test(
    uint64_t *page_state,
    uint64_t total_pages,
    uint64_t first_page,
    const CXLHybridPageClaim *claims,
    uint32_t nr_pages,
    uint32_t *completedp,
    uint32_t *stalep);
```

- [ ] **Step 4: Implement completion helper**

Add to `migration/cxl-page-transfer.c`:

```c
void cxl_hybrid_cxl_descriptor_complete_pages_for_test(
    uint64_t *page_state,
    uint64_t total_pages,
    uint64_t first_page,
    const CXLHybridPageClaim *claims,
    uint32_t nr_pages,
    uint32_t *completedp,
    uint32_t *stalep)
{
    for (uint32_t i = 0; page_state && claims && i < nr_pages &&
         first_page + i < total_pages; i++) {
        if (cxl_hybrid_page_state_complete_cxl(&page_state[first_page + i],
                                               &claims[i])) {
            if (completedp) {
                (*completedp)++;
            }
        } else if (stalep) {
            (*stalep)++;
        }
    }
}
```

- [ ] **Step 5: Add runtime CXL worker state**

Extend `CXLHybridControlState` in `migration/cxl-hybrid-control.c`:

```c
CXLHybridTransferQueue transfer_queue;
QemuThread cxl_worker;
bool cxl_worker_running;
```

Initialize/destroy the queue with the same lifecycle as the request worker. The worker loop should:

1. pop with `cxl_hybrid_transfer_queue_pop_cxl()`;
2. claim each page with `cxl_hybrid_page_state_claim_for_cxl()`;
3. copy adjacent claimed pages with the existing CXL publish/copy path;
4. complete every copied page with `cxl_hybrid_page_state_complete_cxl()`;
5. update visible bitmap through `cxl_hybrid_ctrl_publish_pages_visible()`;
6. count CXL worker copied bytes and stale pages.

The first implementation may copy one descriptor at a time. Lane-local merging can be added in the same worker once correctness tests pass; do not merge CXL and RDMA descriptors.

- [ ] **Step 6: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-control --tap
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS.

- [ ] **Step 7: Commit**

Run:

```bash
git add migration/cxl.h migration/cxl-page-transfer.c \
        migration/cxl-hybrid-control.c migration/cxl.c \
        tests/unit/test-cxl-hybrid-control.c
git commit -m "migration/cxl: complete cxl worker pages through page state"
```

## Task 12: Wire RDMA Worker Completion To Page-State Descriptors

**Files:**
- Modify: `migration/cxl-rdma.c`
- Modify: `migration/cxl-rdma.h`
- Modify: `migration/cxl.h`
- Test: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write posted-write race test**

Add to `tests/unit/test-cxl-hybrid-region.c`:

```c
static void test_rdma_posted_page_blocks_cxl_publish_until_completion(void)
{
    uint64_t slot = cxl_hybrid_page_state_make_dirty(8, 1);
    CXLHybridPageClaim rdma_claim = { 0 };
    CXLHybridPageClaim cxl_claim = { 0 };

    g_assert_true(cxl_hybrid_page_state_claim_for_rdma(&slot, 8,
                                                       &rdma_claim));
    g_assert_false(cxl_hybrid_page_state_claim_for_cxl(&slot, 8,
                                                       &cxl_claim));
    g_assert_true(cxl_hybrid_page_state_complete_rdma(&slot, &rdma_claim));
    g_assert_true(cxl_hybrid_page_state_can_consume(
        slot, 8, CXL_HYBRID_PAGE_LOCATION_DST_LOCAL));
}
```

Register:

```c
g_test_add_func("/cxl/region/rdma-posted-page-blocks-cxl-publish",
                test_rdma_posted_page_blocks_cxl_publish_until_completion);
```

- [ ] **Step 2: Run the test**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS if Task 8 wrappers are correct. This test locks the race rule before runtime RDMA integration.

- [ ] **Step 3: Replace region-ready completion in RDMA worker**

In `migration/cxl-rdma.c`, where a bulk region completion currently accounts `rdma_bulk_regions`, `rdma_bulk_bytes`, or calls region-ready helpers, switch the active descriptor path to:

```c
for (uint32_t i = 0; i < desc->nr_pages; i++) {
    if (!cxl_hybrid_rdma_descriptor_page_claimed(desc, i)) {
        continue;
    }
    if (cxl_hybrid_page_state_complete_rdma(
            &page_state[desc->first_page + i], &desc->claims[i])) {
        completed_pages++;
    } else {
        stale_pages++;
    }
}
```

Keep old region stats only as compatibility reporting until Task 15 cleanup. New correctness must use page state.

- [ ] **Step 4: Ensure RDMA bytes are real verbs completions**

The RDMA worker must increment new completed bytes only from completion CQ events, not enqueue attempts:

```c
qatomic_add(&ctx->stats.page_state_rdma_completed_pages, completed_pages);
qatomic_add(&ctx->stats.page_state_rdma_completed_bytes,
            (uint64_t)completed_pages * TARGET_PAGE_SIZE);
qatomic_add(&ctx->stats.page_state_rdma_stale_pages, stale_pages);
```

Add fields to the RDMA stats struct used by `cxl_rdma_sidecar_get_stats()` and expose them through `MigrationInfo` in Task 14.

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
git add migration/cxl-rdma.c migration/cxl-rdma.h migration/cxl.h \
        tests/unit/test-cxl-hybrid-region.c
git commit -m "migration/cxl: complete rdma descriptors through page state"
```

## Task 13: Replace Bulk Region Ownership With Page Claims And Balanced Enqueue

**Files:**
- Modify: `migration/ram.c`
- Modify: `migration/cxl.c`
- Modify: `migration/cxl.h`
- Test: `tests/unit/test-cxl-hybrid-region.c`

- [ ] **Step 1: Write scheduler policy tests**

Add to `tests/unit/test-cxl-hybrid-region.c`:

```c
static void test_scheduler_balances_bulk_pages_between_cxl_and_rdma(void)
{
    CXLHybridSchedulerPolicy policy = {
        .rdma_budget_pages = 4,
        .cxl_background_pages = 4,
    };

    g_assert_cmpuint(cxl_hybrid_scheduler_choose_bulk_lane(&policy, 0), ==,
                     CXL_HYBRID_TRANSFER_CXL_LOW);
    g_assert_cmpuint(cxl_hybrid_scheduler_choose_bulk_lane(&policy, 1), ==,
                     CXL_HYBRID_TRANSFER_RDMA_BULK);
    g_assert_cmpuint(cxl_hybrid_scheduler_choose_bulk_lane(&policy, 2), ==,
                     CXL_HYBRID_TRANSFER_CXL_LOW);
    g_assert_cmpuint(cxl_hybrid_scheduler_choose_bulk_lane(&policy, 3), ==,
                     CXL_HYBRID_TRANSFER_RDMA_BULK);
}
```

Register:

```c
g_test_add_func("/cxl/region/scheduler-balances-cxl-rdma-bulk",
                test_scheduler_balances_bulk_pages_between_cxl_and_rdma);
```

- [ ] **Step 2: Verify failure**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-region
```

Expected: FAIL because the current selector sends all budgeted bulk pages to RDMA.

- [ ] **Step 3: Implement deterministic balanced selector**

In `migration/cxl-page-transfer.c`, replace the current bulk selector with:

```c
CXLHybridTransferClass cxl_hybrid_scheduler_choose_bulk_lane(
    const CXLHybridSchedulerPolicy *policy,
    uint64_t page_index)
{
    uint64_t total_share;

    if (!policy || !policy->rdma_budget_pages) {
        return CXL_HYBRID_TRANSFER_CXL_LOW;
    }
    if (!policy->cxl_background_pages) {
        return CXL_HYBRID_TRANSFER_RDMA_BULK;
    }

    total_share = policy->rdma_budget_pages + policy->cxl_background_pages;
    if (!total_share || total_share < policy->rdma_budget_pages) {
        return CXL_HYBRID_TRANSFER_CXL_LOW;
    }

    return (page_index % total_share) < policy->cxl_background_pages ?
           CXL_HYBRID_TRANSFER_CXL_LOW :
           CXL_HYBRID_TRANSFER_RDMA_BULK;
}
```

- [ ] **Step 4: Convert `ram_save_host_page()` active hybrid path**

In `migration/ram.c`, replace `cxl_hybrid_rdma_enqueue_bulk_region()` for the active page-state worker path with a per-page classifier:

1. detect zero pages first and publish `PUBLISHED@ZERO`;
2. choose CXL/RDMA lane with `cxl_hybrid_scheduler_choose_bulk_lane()`;
3. CAS claim the page with the selected lane owner;
4. clear the migration dirty bit only after the claim succeeds;
5. enqueue descriptor to CXL low or RDMA bulk queue;
6. return claimed pages without calling `save_normal_page()` for those pages.

Keep the old RDMA region function compiled for rollback until Task 15 removes it, but do not let it run before the balanced classifier in `hybrid_parallel_rdma_cxl`.

- [ ] **Step 5: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-control --tap
./build/tests/unit/test-cxl-hybrid-region --tap
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
git add migration/ram.c migration/cxl.c migration/cxl.h \
        migration/cxl-page-transfer.c tests/unit/test-cxl-hybrid-region.c
git commit -m "migration/cxl: balance bulk scheduler page claims"
```

## Task 14: Add Page-State Lane Metrics To Query And Scripts

**Files:**
- Modify: `qapi/migration.json`
- Modify: `migration/cxl.c`
- Modify: `scripts/cxl-hybrid-warm-experiment.py`
- Modify: `scripts/cxl-hybrid-warm-experiment-test.py`

- [ ] **Step 1: Add parser test data**

In `scripts/cxl-hybrid-warm-experiment-test.py`, add a fixture with these QMP fields under `x-cxl`:

```python
"page-state-cxl-worker-bytes": 33554432,
"page-state-cxl-worker-time-ns": 4000000,
"page-state-rdma-completed-bytes": 33554432,
"page-state-rdma-completed-time-ns": 3000000,
"page-state-rdma-stale-pages": 2,
"page-state-cas-failures": 5,
```

Assert that the summary computes:

```python
assert row["cxl_worker_bw_mib_s"] == pytest.approx(8000.0)
assert row["rdma_completed_bw_mib_s"] == pytest.approx(10666.666, rel=1e-3)
assert row["page_state_rdma_stale_pages"] == 2
assert row["page_state_cas_failures"] == 5
```

- [ ] **Step 2: Verify parser failure**

Run:

```bash
python3 -m pytest scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: FAIL because fields are not parsed yet. If pytest is not available in the environment, run:

```bash
python3 scripts/cxl-hybrid-warm-experiment-test.py
```

and record the exact failure mode in the commit message body.

- [ ] **Step 3: Add QAPI fields**

Add fields to `MigrationInfo` CXL stats in `qapi/migration.json` using the existing `x-cxl` naming style:

```json
"page-state-cxl-worker-bytes": "uint64",
"page-state-cxl-worker-time-ns": "uint64",
"page-state-rdma-completed-bytes": "uint64",
"page-state-rdma-completed-time-ns": "uint64",
"page-state-rdma-stale-pages": "uint64",
"page-state-cas-failures": "uint64"
```

Regenerate/build through Ninja; do not hand-edit generated QAPI files.

- [ ] **Step 4: Populate query metrics**

In `migration/cxl.c:cxl_populate_migration_info()`, populate the new fields from CXL worker stats and RDMA sidecar stats. CXL bandwidth must use CXL worker memcpy bytes/time, not backing-write counters. RDMA bandwidth must use RDMA completion bytes/time, not posted bytes.

- [ ] **Step 5: Update script parsing**

In `scripts/cxl-hybrid-warm-experiment.py`, add columns:

```text
cxl_worker_bytes
cxl_worker_time_ms
cxl_worker_bw_mib_s
rdma_completed_bytes
rdma_completed_time_ms
rdma_completed_bw_mib_s
page_state_rdma_stale_pages
page_state_cas_failures
```

Compute bandwidth as:

```python
def mib_s(bytes_value: int, time_ns: int) -> float:
    return 0.0 if time_ns <= 0 else (bytes_value / (1024 * 1024)) / (time_ns / 1e9)
```

- [ ] **Step 6: Run tests**

Run:

```bash
ninja -C build tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-control --tap
./build/tests/unit/test-cxl-hybrid-region --tap
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py \
                   scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: PASS.

- [ ] **Step 7: Commit**

Run:

```bash
git add qapi/migration.json migration/cxl.c \
        scripts/cxl-hybrid-warm-experiment.py \
        scripts/cxl-hybrid-warm-experiment-test.py
git commit -m "migration/cxl: report page-state lane metrics"
```

## Task 15: Phase 2 Experiment Gate

**Files:**
- Modify: `docs/superpowers/reports/2026-05-30-rdma-cxl-page-state-phase2-results.md`

- [ ] **Step 1: Build the QEMU targets used by the experiment**

Run:

```bash
ninja -C build qemu-system-x86_64 tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region
```

Expected: PASS.

- [ ] **Step 2: Run the unit and parser checkpoint**

Run:

```bash
./build/tests/unit/test-cxl-hybrid-control --tap
./build/tests/unit/test-cxl-hybrid-region --tap
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py \
                   scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: PASS.

- [ ] **Step 3: Run the balanced bulk experiment**

Use the same local RDMA/CXL parameters as the previous real RDMA sidecar experiment, but disable migration bandwidth limiting. The important experimental condition is that both lanes are active during bulk:

```bash
python3 scripts/cxl-hybrid-warm-experiment.py \
    --case hybrid_parallel_rdma_cxl \
    --vm-mem 64M \
    --dirty-workload bulk \
    --disable-bandwidth-limit \
    --rdma-real \
    --summary-json /tmp/cxl-phase2-balanced.json
```

If the script uses different flag names in this worktree, inspect `--help` and use the exact equivalent flags. Record the final command in the report.

- [ ] **Step 4: Write the report**

Create `docs/superpowers/reports/2026-05-30-rdma-cxl-page-state-phase2-results.md` with:

````markdown
# RDMA CXL Page-State Phase 2 Results

## Commands

```bash
<exact commands run>
```

## Checkpoint

- control unit tests:
- region unit tests:
- parser compile:
- QEMU build:

## Bulk Data Plane

| lane | bytes | time ms | bandwidth MiB/s |
| --- | ---: | ---: | ---: |
| CXL worker |  |  |  |
| RDMA completed |  |  |  |

## Fault And Stale Behavior

- guest stall p50/p95/p99:
- RDMA stale pages:
- CXL demand pages:
- CXL high queue pages:
- CAS failures:

## Interpretation

- Did CXL and RDMA both move non-zero physical bytes during bulk?
- Did RDMA still dominate page ownership?
- Did postcopy demand latency regress against the current CXL-only baseline?
- Did any stale RDMA page trigger more than one-page CXL demand?
````

- [ ] **Step 5: Commit**

Run:

```bash
git add docs/superpowers/reports/2026-05-30-rdma-cxl-page-state-phase2-results.md
git commit -m "docs: report rdma cxl page-state phase2 results"
```

## Phase 2 Verification Checkpoint

Run:

```bash
ninja -C build qemu-system-x86_64 \
    tests/unit/test-cxl-hybrid-control tests/unit/test-cxl-hybrid-region
./build/tests/unit/test-cxl-hybrid-control --tap
./build/tests/unit/test-cxl-hybrid-region --tap
python3 -m py_compile scripts/cxl-hybrid-warm-experiment.py \
                   scripts/cxl-hybrid-warm-experiment-test.py
```

Expected: all commands PASS.

## Phase 3 Preview

Do not start Phase 3 until the Phase 2 experiment report shows both lanes carrying non-zero physical bytes and no postcopy demand regression.

Phase 3 should remove or demote the old region-control correctness state:

- remove RDMA ready/invalidated/republished region bitmaps as correctness inputs;
- replace whole-region dirty invalidation with page dirty/stale transitions;
- remove region-wide CXL republish metrics from active reports;
- integrate longest-valid-span helper into destination remap attempts;
- add bounded span scan metrics and `/proc/<pid>/maps` convergence reporting.
