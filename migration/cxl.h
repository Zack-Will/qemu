/*
 * Copyright (c) 2024 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_MIGRATION_CXL_H
#define QEMU_MIGRATION_CXL_H

#include "qemu/typedefs.h"
#include "qapi/qapi-types-migration.h"
#include "qemu/thread.h"
#include "io/channel.h"
#include "io/task.h"
#include "channel.h"
#include "multifd.h"

#define CXL_HYBRID_METADATA_VERSION 1
#define CXL_HYBRID_CTRL_MAGIC 0x43584c48U
#define CXL_HYBRID_CTRL_VERSION 7
#define CXL_HYBRID_CTRL_REQUEST_ORDER 10
#define CXL_HYBRID_CTRL_COMPLETION_F_QUIESCE (1U << 0)
#define CXL_FAULT_REGION_GRANULE_DEFAULT (2 * 1024 * 1024)
#define CXL_REMAP_GRANULE_DEFAULT (64 * 1024)
#define CXL_REMAP_GRANULE_ENV "QEMU_CXL_REMAP_GRANULE"
#define CXL_CLEAN_REMAP_DEBUG_ENV "QEMU_CXL_CLEAN_REMAP_DEBUG"

typedef enum CXLHybridPhase {
    CXL_HYBRID_PHASE_DISABLED = 0,
    CXL_HYBRID_PHASE_PRECOPY_BULK,
    CXL_HYBRID_PHASE_PRECOPY_BRAKE,
    CXL_HYBRID_PHASE_SWITCHING,
    CXL_HYBRID_PHASE_POSTCOPY_WARM,
    CXL_HYBRID_PHASE_CLEANUP,
} CXLHybridPhase;

typedef enum CXLHybridSwitchAction {
    CXL_HYBRID_SWITCH_ACTION_NONE = 0,
    CXL_HYBRID_SWITCH_ACTION_ENTER_BRAKE,
    CXL_HYBRID_SWITCH_ACTION_START_POSTCOPY,
} CXLHybridSwitchAction;

typedef struct CXLHybridSwitchPolicyInput {
    CXLHybridPhase phase;
    bool brake_enabled;
    bool dirty_trigger;
    bool max_iters_trigger;
    bool gain_trigger;
    bool remaining_trigger;
    bool time_cap_trigger;
    bool completion_ready;
    uint64_t staged_pages;
    uint8_t remap_coverage_threshold;
    uint8_t remap_coverage;
} CXLHybridSwitchPolicyInput;

typedef struct CXLHybridSwitchDecision {
    CXLHybridSwitchAction action;
    CXLMigrationSwitchReason reason;
} CXLHybridSwitchDecision;

typedef struct CXLHybridMetadataEntry {
    char *ramblock;
    uint64_t offset;
    uint64_t length;
    uint32_t flags;
    uint32_t heat;
} CXLHybridMetadataEntry;

typedef struct CXLHybridMetadata {
    uint32_t version;
    uint32_t generation;
    uint32_t nr_entries;
    CXLHybridMetadataEntry *entries;
} CXLHybridMetadata;

typedef struct CXLHybridControlHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t request_ring_order;
    uint32_t generation;
    uint32_t visible_page_words;
    uint32_t page_state_words;
    uint32_t page_state_word_size;
    uint32_t visible_region_words;
    uint32_t owned_region_words;
    uint32_t region_granule_shift;
    uint32_t target_page_shift;
    uint64_t total_pages;
    uint64_t total_regions;
    uint64_t region_granule;
    uint64_t request_prod;
    uint64_t request_cons;
    uint64_t active_enqueue_count;
    uint64_t active_request_count;
    uint64_t source_write_count;
    uint32_t completed_generation;
    uint32_t completion_flags;
} CXLHybridControlHeader;

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

typedef struct CXLHybridFaultRequestRecord {
    uint64_t seq;
    uint64_t page_index;
    uint64_t demand_page;
    uint32_t generation;
    uint32_t flags;
    uint32_t nr_pages;
    uint64_t request_ts_ns;
} CXLHybridFaultRequestRecord;

typedef int (*CXLHybridPostcopyPlacePageFunc)(MigrationIncomingState *mis,
                                              void *host, void *from,
                                              RAMBlock *rb);

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
    CXLHybridPageClaim claim;
    bool has_claim;
} CXLHybridPageDescriptor;

typedef struct CXLHybridRemapSpan {
    uint64_t first_page;
    uint32_t nr_pages;
} CXLHybridRemapSpan;

typedef struct CXLHybridTransferQueue {
    QemuMutex lock;
    GQueue classes[CXL_HYBRID_TRANSFER_CLASS_COUNT];
    bool lock_ready;
} CXLHybridTransferQueue;

typedef struct CXLHybridSchedulerPolicy {
    uint64_t rdma_budget_pages;
    uint64_t cxl_background_pages;
} CXLHybridSchedulerPolicy;

typedef struct CXLHybridPageStateSnapshot {
    uint64_t total_pages;
    uint64_t generation_mismatch;
    uint64_t not_sent;
    uint64_t dirty;
    uint64_t in_flight;
    uint64_t in_flight_cxl;
    uint64_t in_flight_rdma;
    uint64_t published;
    uint64_t published_cxl;
    uint64_t published_dst_local;
    uint64_t published_zero;
    uint64_t visible;
    uint64_t published_invisible;
    uint64_t other;
} CXLHybridPageStateSnapshot;

typedef bool (*CXLHybridPageResolveFunc)(uint64_t page_index, void *opaque);

typedef struct CXLHybridFaultRegionGeometry {
    uint64_t global_offset;
    uint64_t block_offset;
    uint64_t cxl_offset;
    uint64_t region_len;
    uint64_t first_page_index;
    uint32_t nr_pages;
    uint64_t region_index;
} CXLHybridFaultRegionGeometry;

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
bool cxl_hybrid_fault_request_demand_visible(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    const CXLHybridFaultRequestRecord *record);
int cxl_hybrid_fault_request_completed_status(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    const CXLHybridFaultRequestRecord *record,
    Error **errp);

#define CXL_HYBRID_FAULT_REQUEST_F_REGION       (1U << 0)

typedef struct CXLHybridWarmStats {
    uint64_t source_heat_updates;
    uint64_t source_warm_queue_pages;
    uint64_t source_warm_sent_pages;
    uint64_t source_warm_sent_bytes;
    uint64_t source_warm_skip_received;
    uint64_t source_warm_skip_unstaged;
    uint64_t source_warm_last_miss_offset;
} CXLHybridWarmStats;

typedef struct CXLHybridPublishStats {
    uint64_t requests;
    uint64_t copied_pages;
    uint64_t copied_bytes;
    uint64_t skip_ready;
    uint64_t failures;
} CXLHybridPublishStats;

typedef struct CXLHybridPublishedPageState {
    bool valid;
    bool ready;
    bool copied;
    uint32_t generation;
    uint64_t cxl_offset;
} CXLHybridPublishedPageState;

typedef enum CXLHybridPublishSource {
    CXL_HYBRID_PUBLISH_SOURCE_UNSPECIFIED = 0,
    CXL_HYBRID_PUBLISH_SOURCE_WARM_PUSH,
    CXL_HYBRID_PUBLISH_SOURCE_FAULT_PRIMARY,
    CXL_HYBRID_PUBLISH_SOURCE_FAULT_BURST,
    CXL_HYBRID_PUBLISH_SOURCE_COMPLETION,
} CXLHybridPublishSource;

typedef struct CXLHybridDstStagingStats {
    uint64_t capacity_bytes;
    uint64_t slots;
    uint64_t present_slots;
    uint64_t fault_hits;
    uint64_t fault_misses;
    uint64_t fault_read_bytes;
    uint64_t fault_read_time_ns;
    uint64_t fault_place_successes;
    uint64_t fault_place_failures;
} CXLHybridDstStagingStats;

typedef struct CXLHybridDstRegionState {
    unsigned long *remapped_bmap;
    unsigned long *copy_owned_bmap;
    unsigned long *remapping_bmap;
    QemuMutex lock;
    QemuCond cond;
    uint64_t total_regions;
    bool lock_ready;
    bool cond_ready;
} CXLHybridDstRegionState;

typedef struct CXLHybridRDMASidecarStats {
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
    uint64_t rdma_ready_regions;
    uint64_t rdma_ready_pages;
    uint64_t rdma_invalidated_regions;
    uint64_t rdma_ready_pages_lost;
    uint64_t cxl_republish_regions_due_to_rdma_invalidate;
    uint64_t cxl_republish_pages_due_to_rdma_invalidate;
} CXLHybridRDMASidecarStats;

typedef struct CXLHybridRDMASidecarState {
    unsigned long *candidate_bmap;
    unsigned long *inflight_bmap;
    unsigned long *ready_bmap;
    unsigned long *stale_bmap;
    unsigned long *cxl_published_bmap;
    unsigned long *invalidated_bmap;
    unsigned long *republished_bmap;
    unsigned long *committed_bmap;
    uint64_t total_regions;
    uint64_t pages_per_region;
    uint64_t accepted_regions;
    uint64_t max_accepted_regions;
    CXLHybridRDMASidecarStats stats;
} CXLHybridRDMASidecarState;

typedef struct CXLHybridRDMAPageDescriptor CXLHybridRDMAPageDescriptor;

typedef struct CXLHybridRDMABulkClaim {
    RAMBlock *block;
    ram_addr_t block_offset;
    uint64_t global_offset;
    uint64_t cxl_offset;
    uint64_t region_index;
    uint64_t bytes;
    uint64_t pages;
    uint64_t post_time_ns;
    void *src;
    void *dst;
    CXLHybridRDMAPageDescriptor *page_desc;
} CXLHybridRDMABulkClaim;

struct CXLHybridRDMAPageDescriptor {
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
};

void cxl_hybrid_metadata_cleanup(CXLHybridMetadata *meta);
int cxl_hybrid_metadata_encoded_len(const CXLHybridMetadata *meta,
                                    size_t *len,
                                    Error **errp);
int cxl_hybrid_metadata_encode(const CXLHybridMetadata *meta,
                               uint8_t *buf,
                               size_t len,
                               Error **errp);
int cxl_hybrid_metadata_decode(CXLHybridMetadata *meta,
                               const uint8_t *buf,
                               size_t len,
                               Error **errp);
int cxl_hybrid_metadata_snapshot_source(CXLHybridMetadata *meta,
                                        Error **errp);
int cxl_hybrid_metadata_send(QEMUFile *f, Error **errp);
int cxl_hybrid_metadata_recv(const uint8_t *buf, size_t len, Error **errp);
void cxl_hybrid_metadata_cleanup_incoming(void);
int cxl_hybrid_dst_staging_init_path(const char *path, size_t capacity,
                                     Error **errp);
int cxl_hybrid_dst_staging_init_path_at(const char *path, size_t capacity,
                                        uint64_t base_offset, Error **errp);
int cxl_hybrid_dst_staging_apply_metadata(const CXLHybridMetadata *meta,
                                          Error **errp);
int cxl_hybrid_dst_staging_store_page(const char *ramblock, uint64_t offset,
                                      const void *buf, size_t len,
                                      Error **errp);
int cxl_hybrid_dst_staging_register_external_page(const char *ramblock,
                                                  uint64_t guest_offset,
                                                  uint64_t cxl_offset,
                                                  size_t len,
                                                  Error **errp);
int cxl_hybrid_dst_staging_read_external(uint64_t cxl_offset, void *buf,
                                         size_t len, Error **errp);
int cxl_hybrid_dst_staging_read_page(const char *ramblock, uint64_t offset,
                                     void *buf, size_t len, Error **errp);
bool cxl_hybrid_dst_staging_page_present(const char *ramblock,
                                         uint64_t offset);
bool cxl_hybrid_dst_staging_range_present(const char *ramblock,
                                          uint64_t offset,
                                          size_t len);
bool cxl_hybrid_dst_staging_is_active(void);
void cxl_hybrid_dst_staging_get_stats(CXLHybridDstStagingStats *stats);
void cxl_hybrid_dst_staging_account_fault_miss(void);
void cxl_hybrid_dst_staging_account_fault_hit(size_t len,
                                              uint64_t read_time_ns);
void cxl_hybrid_dst_staging_account_fault_place_result(bool success);
void cxl_hybrid_dst_staging_cleanup(void);
int cxl_hybrid_try_resolve_fault(MigrationIncomingState *mis, RAMBlock *rb,
                                 ram_addr_t offset,
                                 int (*place_page)(MigrationIncomingState *mis,
                                                   void *host, void *from,
                                                   RAMBlock *rb),
                                 Error **errp);
bool cxl_hybrid_fault_place_result_satisfied(int ret, bool received);
int cxl_hybrid_postcopy_install_remaining_pages(
    MigrationIncomingState *mis,
    CXLHybridPostcopyPlacePageFunc place_page,
    Error **errp);
int cxl_hybrid_wait_and_resolve_fault(MigrationIncomingState *mis,
                                      RAMBlock *rb,
                                      ram_addr_t offset,
                                      uint64_t haddr,
                                      uint32_t tid,
                                      int (*place_page)(MigrationIncomingState *mis,
                                                        void *host,
                                                        void *from,
                                                        RAMBlock *rb),
                                      Error **errp);
bool cxl_hybrid_postcopy_fault_can_use_cxl(RAMBlock *rb,
                                           ram_addr_t offset);
uint32_t cxl_hybrid_fault_publish_generation(void);
uint32_t cxl_hybrid_fault_publish_generation_begin_source_run(void);
void cxl_hybrid_fault_publish_generation_end_source_run(void);
bool cxl_hybrid_fault_resolve_mode_emits_burst(
    CXLHybridFaultResolveMode mode);
int cxl_hybrid_fault_region_compute(uint64_t block_global_base,
                                    uint64_t block_used_len,
                                    uint64_t block_cxl_pages_offset,
                                    uint64_t fault_block_offset,
                                    uint64_t granule,
                                    uint64_t target_page_size,
                                    CXLHybridFaultRegionGeometry *out,
                                    Error **errp);
bool cxl_hybrid_fault_region_can_cover(uint64_t block_global_base,
                                       uint64_t block_used_len,
                                       uint64_t block_cxl_pages_offset,
                                       uint64_t fault_block_offset,
                                       uint64_t granule,
                                       uint64_t target_page_size);
uint64_t cxl_hybrid_choose_fault_region_granule(uint64_t align,
                                                uint64_t configured,
                                                uint64_t total_ram);
uint64_t cxl_hybrid_choose_source_remap_granule(uint64_t min_align,
                                                uint64_t configured,
                                                uint64_t total_ram);
uint8_t cxl_hybrid_calculate_source_remap_coverage(uint64_t staged_pages,
                                                   uint64_t remapped_pages);
CXLHybridSwitchDecision cxl_hybrid_switch_decide(
    const CXLHybridSwitchPolicyInput *input);
bool cxl_hybrid_source_remap_region_clean(const unsigned long *migrated_bmap,
                                          size_t migrated_first_page,
                                          const unsigned long *dirty_bmap,
                                          size_t dirty_first_page,
                                          size_t npages);
bool cxl_hybrid_warm_page_eligible_for_push(
    const unsigned long *migrated_bmap,
    const unsigned long *warm_sent_bmap,
    const unsigned long *dst_sent_bmap,
    const unsigned long *cxl_visible_bmap,
    size_t page_idx);
bool cxl_hybrid_clean_remap_budget_allows(uint64_t budget_bytes,
                                          uint64_t used_bytes,
                                          uint64_t region_len);
bool cxl_hybrid_clean_remap_region_is_candidate(bool epoch_seen,
                                                bool dirty_now,
                                                bool already_remapped,
                                                bool in_flight);
bool cxl_hybrid_clean_remap_debug_scan_only(const char *mode);
bool cxl_hybrid_clean_remap_debug_copy_only(const char *mode);
bool cxl_hybrid_clean_remap_debug_read_only(const char *mode);
bool cxl_hybrid_clean_remap_debug_write_only(const char *mode);
bool cxl_hybrid_clean_remap_debug_defer_remap(const char *mode);
bool cxl_hybrid_clean_remap_prefault_valid(CXLCleanRemapPrefaultMode mode);
bool cxl_hybrid_clean_remap_prefault_enabled(CXLCleanRemapPrefaultMode mode);
uint64_t cxl_hybrid_mapped_ram_pages_offset_alignment(
    uint64_t file_align,
    uint64_t dax_align,
    uint64_t remap_granule,
    bool use_region_remap);
bool cxl_hybrid_mapped_ram_layout_next(uint64_t *offsetp,
                                       uint64_t used_length,
                                       uint64_t pages_align,
                                       uint64_t target_page_size,
                                       uint64_t *pages_offsetp,
                                       uint64_t *pages_lenp);
void cxl_hybrid_transfer_queue_init_for_test(CXLHybridTransferQueue *queue);
void cxl_hybrid_transfer_queue_destroy_for_test(CXLHybridTransferQueue *queue);
void cxl_hybrid_transfer_queue_push(CXLHybridTransferQueue *queue,
                                    CXLHybridTransferClass klass,
                                    const CXLHybridPageDescriptor *desc);
uint32_t cxl_hybrid_transfer_queue_push_batch(
    CXLHybridTransferQueue *queue,
    CXLHybridTransferClass klass,
    const CXLHybridPageDescriptor *descs,
    uint32_t count);
bool cxl_hybrid_transfer_queue_pop(CXLHybridTransferQueue *queue,
                                   CXLHybridPageDescriptor *desc);
bool cxl_hybrid_transfer_queue_pop_cxl(CXLHybridTransferQueue *queue,
                                       CXLHybridPageDescriptor *desc,
                                       CXLHybridTransferClass *klass);
uint32_t cxl_hybrid_transfer_queue_pop_cxl_batch(
    CXLHybridTransferQueue *queue,
    CXLHybridPageDescriptor *descs,
    uint32_t max_descs,
    CXLHybridTransferClass *klass);
bool cxl_hybrid_transfer_queue_pop_rdma(CXLHybridTransferQueue *queue,
                                        CXLHybridPageDescriptor *desc,
                                        CXLHybridTransferClass *klass);
uint64_t cxl_hybrid_transfer_queue_depth(CXLHybridTransferQueue *queue,
                                         CXLHybridTransferClass klass);
void cxl_hybrid_dst_region_state_init_for_test(CXLHybridDstRegionState *state,
                                               uint64_t total_regions);
void cxl_hybrid_dst_region_state_destroy_for_test(
    CXLHybridDstRegionState *state);
bool cxl_hybrid_dst_region_copy_owned(const CXLHybridDstRegionState *state,
                                      uint64_t region_index);
bool cxl_hybrid_dst_region_remapped(const CXLHybridDstRegionState *state,
                                    uint64_t region_index);
bool cxl_hybrid_dst_region_remapping(const CXLHybridDstRegionState *state,
                                     uint64_t region_index);
void cxl_hybrid_dst_region_wait_not_remapping(
    CXLHybridDstRegionState *state,
    uint64_t region_index);
void cxl_hybrid_dst_region_mark_copy_owned(CXLHybridDstRegionState *state,
                                           uint64_t region_index);
bool cxl_hybrid_dst_region_try_mark_copy_owned(CXLHybridDstRegionState *state,
                                               uint64_t region_index);
bool cxl_hybrid_dst_region_can_remap(const CXLHybridDstRegionState *state,
                                     uint64_t region_index);
bool cxl_hybrid_dst_region_try_begin_remap(CXLHybridDstRegionState *state,
                                           uint64_t region_index);
void cxl_hybrid_dst_region_finish_remap(CXLHybridDstRegionState *state,
                                        uint64_t region_index,
                                        bool success);
void cxl_hybrid_rdma_sidecar_state_init_for_test(
    CXLHybridRDMASidecarState *state,
    uint64_t total_regions,
    uint64_t pages_per_region);
void cxl_hybrid_rdma_sidecar_state_destroy_for_test(
    CXLHybridRDMASidecarState *state);
bool cxl_hybrid_rdma_sidecar_region_is_rdma_owned(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_try_own_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_try_own_cxl_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_region_is_cxl_owned(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_region_cxl_bulk_allowed(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_try_start_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
void cxl_hybrid_rdma_sidecar_configure_budget_for_test(
    CXLHybridRDMASidecarState *state,
    uint64_t max_inflight_regions,
    uint64_t max_cover_percent);
bool cxl_hybrid_rdma_sidecar_pick_pending_region_for_test(
    CXLHybridRDMASidecarState *state,
    const unsigned long *dirty_bmap,
    uint64_t total_pages,
    uint64_t *region_out);
bool cxl_hybrid_rdma_sidecar_complete_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_region_inflight(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index);
void cxl_hybrid_rdma_sidecar_drop_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_mark_ready(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_invalidate_ready(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_region_ready_current(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_commit_ready_region(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_region_committed(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_region_invalidated(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_region_stale(
    const CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_note_cxl_publish(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
bool cxl_hybrid_rdma_sidecar_note_cxl_republish(
    CXLHybridRDMASidecarState *state,
    uint64_t region_index);
uint64_t cxl_hybrid_rdma_sidecar_invalidate_dirty_ready_regions(
    CXLHybridRDMASidecarState *state,
    const unsigned long *dirty_bmap,
    uint64_t dirty_pages);
void cxl_hybrid_rdma_sidecar_get_stats(
    const CXLHybridRDMASidecarState *state,
    CXLHybridRDMASidecarStats *stats);
void cxl_hybrid_reset_rdma_sidecar_stats(void);
void cxl_hybrid_reset_rdma_sidecar_stats_for_test(void);
void cxl_hybrid_rdma_sidecar_global_init(uint64_t total_regions,
                                         uint64_t pages_per_region);
void cxl_hybrid_rdma_sidecar_global_configure_budget(uint32_t max_inflight,
                                                     uint8_t max_cover_percent);
void cxl_hybrid_rdma_sidecar_global_destroy(void);
CXLHybridTransferClass cxl_hybrid_scheduler_choose_bulk_lane(
    const CXLHybridSchedulerPolicy *policy,
    uint64_t page_index);
CXLHybridTransferClass cxl_hybrid_scheduler_choose_zero_page_lane(
    const CXLHybridSchedulerPolicy *policy);
void cxl_hybrid_account_shadow_bulk_candidate(RAMBlock *block,
                                               ram_addr_t block_offset);
void cxl_hybrid_account_rdma_ready(uint64_t region_index,
                                   uint64_t pages);
void cxl_hybrid_account_rdma_invalidate(uint64_t region_index,
                                        uint64_t pages);
void cxl_hybrid_account_rdma_cxl_republish(uint64_t region_index,
                                           uint64_t pages);
void cxl_hybrid_get_rdma_sidecar_stats(CXLHybridRDMASidecarStats *stats);
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
bool cxl_hybrid_region_is_rdma_owned(uint64_t region_index);
bool cxl_hybrid_region_try_own_rdma(uint64_t region_index);
bool cxl_hybrid_region_try_own_cxl(uint64_t region_index);
bool cxl_hybrid_region_is_cxl_owned(uint64_t region_index);
bool cxl_hybrid_region_cxl_bulk_allowed(uint64_t region_index);
void cxl_hybrid_region_drop_rdma(uint64_t region_index);
void cxl_hybrid_mark_region_rdma_ready(uint64_t region_index);
void cxl_hybrid_invalidate_region_rdma_ready(uint64_t region_index);
bool cxl_hybrid_region_rdma_ready_current(uint64_t region_index);
bool cxl_hybrid_region_commit_rdma_ready(uint64_t region_index);
bool cxl_hybrid_region_note_cxl_republish(uint64_t region_index);
bool cxl_hybrid_commit_rdma_ready_region(uint64_t region_index,
                                         uint32_t generation);
bool cxl_hybrid_region_can_use_rdma_bulk(RAMBlock *block,
                                         ram_addr_t block_offset);
bool cxl_hybrid_rdma_bulk_claim_init(CXLHybridRDMABulkClaim *claim,
                                     RAMBlock *block,
                                     ram_addr_t block_offset);
void cxl_hybrid_rdma_drop_bulk_claim(const CXLHybridRDMABulkClaim *claim);
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
void cxl_hybrid_rdma_descriptor_drop_pages_for_test(
    CXLHybridRDMAPageDescriptor *desc,
    uint64_t *page_state,
    uint64_t total_pages);
void cxl_hybrid_cxl_descriptor_complete_pages_for_test(
    uint64_t *page_state,
    uint64_t total_pages,
    uint64_t first_page,
    const CXLHybridPageClaim *claims,
    uint32_t nr_pages,
    uint32_t *completedp,
    uint32_t *stalep);
void cxl_hybrid_rdma_descriptor_destroy(CXLHybridRDMAPageDescriptor *desc);
void cxl_hybrid_rdma_bulk_claim_release(CXLHybridRDMABulkClaim *claim);
bool cxl_hybrid_rdma_sidecar_get_backing(void **basep, size_t *sizep);
bool cxl_hybrid_start_rdma_sidecar(bool incoming, bool wait_for_setup,
                                   Error **errp);
void cxl_hybrid_invalidate_rdma_ready_region_for_page(RAMBlock *block,
                                                      ram_addr_t block_offset);
void cxl_hybrid_sync_rdma_dirty_for_postcopy(void);
void cxl_hybrid_commit_rdma_ready_regions_for_postcopy(void);
bool cxl_hybrid_rdma_brake_fallback_enabled(bool rdma_sidecar_enabled,
                                            bool brake_enabled);

void cxl_cleanup_outgoing_migration(void);

bool cxl_send_channel_create(gpointer opaque, Error **errp);
bool cxl_sender_access_begin(void);
void cxl_sender_access_end(void);
void cxl_sender_access_shutdown(void);

int cxl_write_ramblock_iov(QIOChannel *ioc, const struct iovec *iov,
                           int niov, MultiFDPages_t *pages, Error **errp);

int multifd_cxl_recv_data(MultiFDRecvParams *p, Error **errp);

bool cxl_use_mapped_ram_backing(void);
QIOChannel *cxl_open_mapped_ram_outgoing(Error **errp);
QIOChannel *cxl_open_mapped_ram_incoming(Error **errp);
bool cxl_create_incoming_mapped_ram_channels(Error **errp);
uint64_t cxl_mapped_ram_alignment(void);
uint64_t cxl_mapped_ram_pages_alignment(void);
uint64_t cxl_hybrid_fault_region_granule(void);
void cxl_hybrid_prefault_wait_before_postcopy(void);
void cxl_populate_migration_info(MigrationInfo *info);
uint64_t cxl_hybrid_align_mapping_bytes(uint64_t bytes, uint64_t align);
uint64_t cxl_hybrid_fault_control_region_allocation_bytes(uint64_t align);
uint64_t cxl_hybrid_reserved_region_bytes(uint64_t align,
                                          bool use_fault_control);
int cxl_hybrid_dst_staging_init_fixed_fd(int fd,
                                         size_t capacity,
                                         uint64_t base_offset,
                                         uint64_t file_limit,
                                         bool shared_map,
                                         Error **errp);

/* Write-redirect support */
bool cxl_page_is_remapped(ram_addr_t offset);
uint64_t cxl_clear_remapped_dirty_bits(RAMBlock *block);
void cxl_account_dirty_sync_ns(uint64_t ns);
bool cxl_hybrid_init_source(void);
int cxl_hybrid_begin_source_control_run(Error **errp);
void cxl_hybrid_enter_phase(CXLHybridPhase phase,
                            CXLMigrationSwitchReason reason,
                            uint64_t iteration);
void cxl_hybrid_cleanup_source(void);
bool cxl_hybrid_enabled(void);
CXLHybridPhase cxl_hybrid_phase(void);
bool cxl_hybrid_init_destination(Error **errp);
uint64_t cxl_hybrid_source_staged_pages(void);
uint8_t cxl_hybrid_source_remap_coverage(void);
bool cxl_hybrid_clean_remap_enabled(void);
uint64_t cxl_hybrid_clean_remap_pending_bytes(void);
uint8_t cxl_hybrid_clean_remap_coverage(void);
bool cxl_hybrid_clean_remap_should_throttle(uint64_t throttle_us,
                                            bool copied_region);
uint64_t cxl_hybrid_source_remap_granule(void);
void cxl_hybrid_clean_remap_note_scan(void);
void cxl_hybrid_clean_remap_scan_region(RAMBlock *block,
                                        ram_addr_t block_offset,
                                        size_t region_len,
                                        bool dirty_now);
bool cxl_hybrid_clean_remap_has_candidates(void);
bool cxl_hybrid_clean_remap_defer_remap_enabled(void);
bool cxl_hybrid_clean_remap_region_inflight(RAMBlock *block,
                                            ram_addr_t block_offset);
int cxl_hybrid_clean_remap_drain(Error **errp);
void cxl_hybrid_clean_remap_finalize_deferred(void);
void cxl_hybrid_drain_source_remaps(void);
void cxl_hybrid_record_warm_miss(const char *rbname, ram_addr_t start);
void cxl_hybrid_account_warm_dirty(const char *rbname, ram_addr_t offset,
                                   ram_addr_t len);
void cxl_hybrid_account_dst_page_sent(const char *rbname, ram_addr_t offset,
                                      ram_addr_t len);
void cxl_hybrid_account_dst_pages_sent(RAMBlock *block, ram_addr_t offset,
                                       ram_addr_t len, uint32_t generation);
void cxl_hybrid_account_stream_write(uint64_t bytes, uint64_t elapsed_ns);
bool cxl_hybrid_fault_pressure_active(void);
uint64_t cxl_hybrid_wait_fault_pressure_clear(void);
bool cxl_hybrid_source_page_visible(RAMBlock *block, ram_addr_t offset,
                                    uint32_t generation);
void cxl_hybrid_warm_stats(CXLHybridWarmStats *stats);
bool cxl_hybrid_start_warm_push(MigrationState *s);
void cxl_hybrid_stop_warm_push(void);
int cxl_hybrid_warm_push_iteration(MigrationState *s, Error **errp);
bool cxl_hybrid_source_page_cxl_offset(const char *ramblock,
                                       uint64_t guest_offset,
                                       uint64_t *cxl_offsetp);
int cxl_hybrid_copy_page_to_stable_cxl(RAMBlock *block,
                                       uint64_t guest_offset,
                                       uint64_t cxl_offset,
                                       uint32_t page_len,
                                       Error **errp);
int cxl_hybrid_publish_page_to_cxl(const char *ramblock,
                                   uint64_t guest_offset,
                                   uint32_t page_len,
                                   uint32_t generation,
                                   CXLHybridPublishSource source,
                                   uint64_t *cxl_offsetp,
                                   Error **errp);
void cxl_hybrid_note_cxl_worker_page_visible(uint64_t page_index);
bool cxl_hybrid_postcopy_source_drained(void);
int cxl_hybrid_begin_source_run_with_precopy_remaps(Error **errp);
int cxl_hybrid_publish_staged_pages_for_postcopy(Error **errp);
void cxl_hybrid_iteration_snapshot_begin(uint64_t ram_pages);
void cxl_hybrid_iteration_snapshot_end(uint64_t ram_pages);
bool cxl_hybrid_global_page_offset(const RAMBlock *block,
                                   uint64_t guest_offset,
                                   size_t page_size,
                                   ram_addr_t *global_offsetp);
uint64_t cxl_hybrid_mapped_ram_required_bytes(uint64_t align);
bool cxl_hybrid_lookup_global_page(size_t page_index,
                                   RAMBlock **blockp,
                                   ram_addr_t *block_offsetp);
int cxl_hybrid_control_init_source(Error **errp);
int cxl_hybrid_control_init_destination(Error **errp);
int cxl_hybrid_control_begin_source_run(Error **errp);
bool cxl_hybrid_control_source_drained(void);
int cxl_hybrid_control_complete_source_run(Error **errp);
bool cxl_hybrid_control_source_run_completed(uint32_t generation);
int cxl_hybrid_control_activate_destination(Error **errp);
void cxl_hybrid_ctrl_trace_page_state_snapshot(const char *tag);
void cxl_hybrid_control_cleanup_source(void);
void cxl_hybrid_control_cleanup_destination(void);
uint64_t cxl_hybrid_fault_control_region_bytes(void);
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
bool cxl_hybrid_page_state_claim_for_cxl(uint64_t *slot,
                                         uint32_t generation,
                                         CXLHybridPageClaim *claim);
bool cxl_hybrid_page_state_claim_for_rdma(uint64_t *slot,
                                          uint32_t generation,
                                          CXLHybridPageClaim *claim);
bool cxl_hybrid_page_state_complete(uint64_t *slot,
                                    const CXLHybridPageClaim *claim,
                                    CXLHybridPageLocation location);
bool cxl_hybrid_page_state_complete_cxl(uint64_t *slot,
                                        const CXLHybridPageClaim *claim);
bool cxl_hybrid_page_state_complete_rdma(uint64_t *slot,
                                         const CXLHybridPageClaim *claim);
bool cxl_hybrid_page_state_drop_claim(uint64_t *slot,
                                      const CXLHybridPageClaim *claim);
void cxl_hybrid_page_state_mark_dirty(uint64_t *slot,
                                      uint32_t generation,
                                      uint32_t dirty_seq);
bool cxl_hybrid_page_state_can_consume(uint64_t word,
                                       uint32_t generation,
                                       CXLHybridPageLocation location);
bool cxl_hybrid_page_state_longest_cxl_span(const uint64_t *page_state,
                                            uint64_t total_pages,
                                            uint64_t fault_page,
                                            uint32_t generation,
                                            uint32_t max_pages,
                                            CXLHybridRemapSpan *span);
void cxl_hybrid_page_state_snapshot(const uint64_t *page_state,
                                    const unsigned long *visible_bitmap,
                                    uint64_t total_pages,
                                    uint32_t generation,
                                    CXLHybridPageStateSnapshot *snapshot);
size_t cxl_hybrid_control_visible_bitmap_words(uint64_t pages);
size_t cxl_hybrid_control_visible_bitmap_bytes(uint64_t pages);
size_t cxl_hybrid_control_page_state_words(uint64_t pages);
size_t cxl_hybrid_control_page_state_bytes(uint64_t pages);
size_t cxl_hybrid_control_visible_region_bitmap_words(uint64_t regions);
size_t cxl_hybrid_control_visible_region_bitmap_bytes(uint64_t regions);
size_t cxl_hybrid_control_owned_region_bitmap_words(uint64_t regions);
size_t cxl_hybrid_control_owned_region_bitmap_bytes(uint64_t regions);
uint32_t cxl_hybrid_control_region_granule_shift(uint64_t region_granule);
uint32_t cxl_hybrid_control_generation(const CXLHybridControlHeader *hdr);
bool cxl_hybrid_control_generation_matches(const CXLHybridControlHeader *hdr,
                                           uint32_t generation);
bool cxl_hybrid_control_abort_generation(CXLHybridControlHeader *hdr,
                                         uint32_t generation);
uint32_t cxl_hybrid_select_fault_publish_generation(bool incoming_valid,
                                                    uint32_t incoming_generation,
                                                    bool source_run_valid,
                                                    uint32_t source_run_generation,
                                                    uint64_t phase_transitions);
uint64_t cxl_hybrid_control_source_write_count(
    const CXLHybridControlHeader *hdr);
uint64_t cxl_hybrid_control_source_write_begin(CXLHybridControlHeader *hdr);
uint64_t cxl_hybrid_control_source_write_end(CXLHybridControlHeader *hdr);
bool cxl_hybrid_control_fault_pressure(const CXLHybridControlHeader *hdr,
                                       uint32_t generation);
bool cxl_hybrid_control_page_range_resolved(uint64_t first_page,
                                            uint32_t nr_pages,
                                            CXLHybridPageResolveFunc resolve,
                                            void *opaque,
                                            uint64_t *unresolved_page);
bool cxl_hybrid_control_region_span_index(const CXLHybridControlHeader *hdr,
                                          uint64_t first_page,
                                          uint32_t nr_pages,
                                          uint64_t *region_indexp);
bool cxl_hybrid_control_region_span_valid(const CXLHybridControlHeader *hdr,
                                          uint64_t first_page,
                                          uint32_t nr_pages);
bool cxl_hybrid_control_page_visible(const CXLHybridControlHeader *hdr,
                                     const unsigned long *visible_bitmap,
                                     uint64_t page_index,
                                     uint32_t generation);
bool cxl_hybrid_control_page_location(const CXLHybridControlHeader *hdr,
                                      const unsigned long *visible_bitmap,
                                      const uint64_t *page_state,
                                      uint64_t page_index,
                                      uint32_t generation,
                                      CXLHybridPageLocation *locationp);
bool cxl_hybrid_control_page_requires_destination_install(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    const uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    bool received,
    CXLHybridPageLocation *locationp);
bool cxl_hybrid_control_page_requires_postcopy_discard(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    const uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation);
bool cxl_hybrid_control_region_visible(const CXLHybridControlHeader *hdr,
                                       const unsigned long *visible_bitmap,
                                       const unsigned long *visible_region_bitmap,
                                       uint64_t first_page,
                                       uint32_t nr_pages,
                                       uint32_t generation);
bool cxl_hybrid_control_region_bit_visible(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_region_bitmap,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation);
bool cxl_hybrid_control_region_visible_or_synthesize(
    const CXLHybridControlHeader *hdr,
    const unsigned long *visible_bitmap,
    unsigned long *visible_region_bitmap,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation);
void cxl_hybrid_control_mark_page_visible(const CXLHybridControlHeader *hdr,
                                          unsigned long *visible_bitmap,
                                          uint64_t page_index);
void cxl_hybrid_control_mark_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    CXLHybridPageLocation location);
void cxl_hybrid_control_mark_page_dirty_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    uint32_t dirty_seq);
uint64_t cxl_hybrid_control_mark_dirty_pages_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    const unsigned long *dirty_bitmap,
    uint64_t dirty_first_page,
    uint64_t state_first_page,
    uint64_t nr_pages,
    uint32_t generation,
    uint32_t dirty_seq);
bool cxl_hybrid_control_complete_cxl_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    const CXLHybridPageClaim *claim);
bool cxl_hybrid_control_complete_rdma_page_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t page_index,
    uint32_t generation,
    const CXLHybridPageClaim *claim);
void cxl_hybrid_control_mark_pages_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    uint64_t *page_state,
    uint64_t first_page,
    uint64_t nr_pages,
    uint32_t generation,
    CXLHybridPageLocation location);
void cxl_hybrid_control_clear_page_visible(const CXLHybridControlHeader *hdr,
                                           unsigned long *visible_bitmap,
                                           uint64_t page_index);
void cxl_hybrid_control_mark_region_visible(const CXLHybridControlHeader *hdr,
                                            unsigned long *visible_region_bitmap,
                                            uint64_t region_index);
void cxl_hybrid_control_mark_region_visible_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_region_bitmap,
    uint64_t region_index,
    uint32_t generation);
bool cxl_hybrid_control_mark_region_visible_for_span_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_region_bitmap,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation);
bool cxl_hybrid_control_mark_visible_region_span_generation(
    const CXLHybridControlHeader *hdr,
    unsigned long *visible_bitmap,
    unsigned long *visible_region_bitmap,
    uint64_t *page_state,
    uint64_t first_page,
    uint32_t nr_pages,
    uint32_t generation,
    CXLHybridPageLocation location);
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
void cxl_hybrid_control_reset_header_for_run(CXLHybridControlHeader *hdr,
                                             uint32_t generation);
bool cxl_hybrid_control_region_owned(const CXLHybridControlHeader *hdr,
                                     const unsigned long *owned_bitmap,
                                     uint64_t region_index,
                                     uint32_t generation);
void cxl_hybrid_control_mark_region_owned(const CXLHybridControlHeader *hdr,
                                          unsigned long *owned_bitmap,
                                          uint64_t region_index);
bool cxl_hybrid_ctrl_page_visible(uint64_t page_index, uint32_t generation);
bool cxl_hybrid_ctrl_page_location(uint64_t page_index, uint32_t generation,
                                   CXLHybridPageLocation *locationp);
bool cxl_hybrid_ctrl_page_requires_destination_install(
    uint64_t page_index,
    uint32_t generation,
    bool received,
    CXLHybridPageLocation *locationp);
bool cxl_hybrid_ctrl_page_requires_postcopy_discard(uint64_t page_index,
                                                    uint32_t generation);
void cxl_hybrid_ctrl_set_page_visible(uint64_t page_index,
                                      uint32_t generation);
void cxl_hybrid_ctrl_set_pages_visible(uint64_t first_page,
                                       uint64_t nr_pages,
                                       uint32_t generation);
void cxl_hybrid_ctrl_mark_page_dirty(uint64_t page_index,
                                     uint32_t generation);
uint64_t cxl_hybrid_ctrl_mark_dirty_pages(const unsigned long *dirty_bitmap,
                                          uint64_t dirty_first_page,
                                          uint64_t state_first_page,
                                          uint64_t nr_pages,
                                          uint32_t generation);
bool cxl_hybrid_ctrl_complete_rdma_page_visible(
    uint64_t page_index,
    uint32_t generation,
    const CXLHybridPageClaim *claim);
bool cxl_hybrid_ctrl_enqueue_cxl_page(RAMBlock *block,
                                      ram_addr_t block_offset,
                                      uint64_t page_index,
                                      uint64_t cxl_offset,
                                      uint32_t generation,
                                      CXLHybridTransferClass klass);
uint32_t cxl_hybrid_ctrl_enqueue_cxl_pages(RAMBlock *block,
                                           ram_addr_t block_offset,
                                           uint64_t first_page_index,
                                           uint64_t cxl_offset,
                                           uint32_t generation,
                                           CXLHybridTransferClass klass,
                                           uint32_t nr_pages);
bool cxl_hybrid_ctrl_claim_rdma_pages(CXLHybridRDMAPageDescriptor *desc,
                                      RAMBlock *block,
                                      ram_addr_t block_offset,
                                      uint64_t first_page,
                                      uint32_t nr_pages,
                                      uint32_t generation);
void cxl_hybrid_ctrl_drop_rdma_pages(CXLHybridRDMAPageDescriptor *desc);
void cxl_hybrid_ctrl_complete_rdma_pages(CXLHybridRDMAPageDescriptor *desc,
                                         uint32_t *completedp,
                                         uint32_t *stalep);
void cxl_hybrid_ctrl_clear_page_visible(uint64_t page_index);
int cxl_hybrid_ctrl_wait_page_visible(uint64_t page_index,
                                      uint32_t generation,
                                      Error **errp);
int cxl_hybrid_ctrl_enqueue_fault_request(uint64_t page_index,
                                          uint32_t generation,
                                          uint64_t request_ts_ns,
                                          bool *queuedp,
                                          Error **errp);
bool cxl_hybrid_ctrl_region_visible(uint64_t first_page,
                                    uint32_t nr_pages,
                                    uint32_t generation);
bool cxl_hybrid_ctrl_region_bit_visible(uint64_t first_page,
                                        uint32_t nr_pages,
                                        uint32_t generation);
void cxl_hybrid_ctrl_set_region_visible(uint64_t first_page,
                                        uint32_t nr_pages,
                                        uint32_t generation);
bool cxl_hybrid_ctrl_synthesize_region_visible(uint64_t first_page,
                                               uint32_t nr_pages,
                                               uint32_t generation);
int cxl_hybrid_ctrl_wait_region_visible(uint64_t first_page,
                                        uint32_t nr_pages,
                                        uint32_t generation,
                                        Error **errp);
int cxl_hybrid_ctrl_enqueue_fault_region_request(uint64_t first_page,
                                                 uint32_t nr_pages,
                                                 uint64_t demand_page,
                                                 uint32_t generation,
                                                 uint64_t request_ts_ns,
                                                 bool *queuedp,
                                                 Error **errp);
bool cxl_hybrid_ctrl_region_owned(uint64_t region_index,
                                  uint32_t generation);
void cxl_hybrid_ctrl_mark_region_owned(uint64_t region_index,
                                       uint32_t generation);
bool cxl_hybrid_ctrl_fault_pressure(uint32_t generation);
void cxl_hybrid_ctrl_source_write_begin(void);
void cxl_hybrid_ctrl_source_write_end(void);
bool cxl_hybrid_ctrl_dequeue_fault_request(CXLHybridFaultRequestRecord *record);

void cxl_hybrid_get_publish_stats(CXLHybridPublishStats *stats);
bool cxl_hybrid_get_published_page_state(const char *ramblock,
                                         uint64_t guest_offset,
                                         CXLHybridPublishedPageState *state);
void cxl_hybrid_note_publish_request_received(const char *ramblock,
                                              uint64_t guest_offset,
                                              uint32_t generation,
                                              uint64_t req_recv_ns);
int cxl_hybrid_publish_fault_request_core(const char *ramblock,
                                          uint64_t guest_offset,
                                          uint32_t page_len,
                                          uint32_t generation,
                                          bool emit_burst,
                                          Error **errp);
int cxl_hybrid_publish_fault_region_request_core(uint64_t first_page,
                                                 uint32_t nr_pages,
                                                 uint32_t generation,
                                                 uint64_t req_recv_ns,
                                                 Error **errp);
bool cxl_hybrid_source_region_owned_by_destination_generation(
    RAMBlock *block,
    ram_addr_t offset,
    uint32_t generation);
bool cxl_hybrid_source_region_owned_by_destination(RAMBlock *block,
                                                   ram_addr_t offset);
int cxl_hybrid_completion_publish_remaining_pages(MigrationState *s,
                                                  Error **errp);
int cxl_hybrid_completion_publish_remaining_regions(Error **errp);

#endif
