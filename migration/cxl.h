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
#include "io/channel.h"
#include "io/task.h"
#include "channel.h"
#include "multifd.h"

#define CXL_HYBRID_METADATA_VERSION 1
#define CXL_HYBRID_CTRL_MAGIC 0x43584c48U
#define CXL_HYBRID_CTRL_VERSION 1
#define CXL_HYBRID_CTRL_REQUEST_ORDER 10
#define CXL_HYBRID_CTRL_READY_ORDER 11

typedef enum CXLHybridPhase {
    CXL_HYBRID_PHASE_DISABLED = 0,
    CXL_HYBRID_PHASE_PRECOPY_BULK,
    CXL_HYBRID_PHASE_PRECOPY_BRAKE,
    CXL_HYBRID_PHASE_SWITCHING,
    CXL_HYBRID_PHASE_POSTCOPY_WARM,
    CXL_HYBRID_PHASE_CLEANUP,
} CXLHybridPhase;

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

typedef struct CXLHybridWarmPage {
    char *ramblock;
    uint64_t offset;
    uint32_t page_len;
    uint8_t *data;
} CXLHybridWarmPage;

typedef struct CXLHybridWarmDescriptor {
    char *ramblock;
    uint64_t guest_offset;
    uint64_t cxl_offset;
    uint32_t page_len;
    uint32_t flags;
    uint32_t generation;
} CXLHybridWarmDescriptor;

typedef struct CXLHybridWarmDescRange {
    char *ramblock;
    uint64_t guest_offset;
    uint64_t cxl_offset;
    uint32_t page_len;
    uint32_t flags;
} CXLHybridWarmDescRange;

typedef struct CXLHybridWarmDescBatch {
    uint32_t generation;
    uint32_t nr_entries;
    CXLHybridWarmDescRange *entries;
} CXLHybridWarmDescBatch;

typedef struct CXLHybridPublishNotify {
    char *ramblock;
    uint64_t guest_offset;
    uint64_t cxl_offset;
    uint32_t page_len;
    uint32_t generation;
} CXLHybridPublishNotify;

typedef struct CXLHybridControlHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t request_ring_order;
    uint32_t ready_ring_order;
    uint32_t generation;
    uint32_t visible_page_words;
    uint64_t request_prod;
    uint64_t request_cons;
    uint64_t ready_prod;
    uint64_t ready_cons;
} CXLHybridControlHeader;

typedef struct CXLHybridFaultRequestRecord {
    uint64_t seq;
    uint64_t page_index;
    uint32_t generation;
    uint32_t flags;
    uint64_t request_ts_ns;
} CXLHybridFaultRequestRecord;

typedef struct CXLHybridFaultReadyRecord {
    uint64_t seq;
    uint64_t page_index;
    uint64_t cxl_offset;
    uint32_t generation;
    uint32_t flags;
    uint64_t ready_ts_ns;
} CXLHybridFaultReadyRecord;

typedef int (*CXLHybridFaultReadyConsumer)(
    const CXLHybridFaultReadyRecord *record, Error **errp);

#define CXL_HYBRID_WARM_DESC_F_SHARED_CXL      (1U << 0)
#define CXL_HYBRID_WARM_DESC_F_SOURCE_REMAPPED (1U << 1)
#define CXL_HYBRID_FAULT_READY_F_PRIMARY        (1U << 0)
#define CXL_HYBRID_FAULT_READY_F_BURST_NEIGHBOR (1U << 1)
#define CXL_HYBRID_FAULT_READY_F_SOURCE_REMAPPED (1U << 2)

typedef struct CXLHybridWarmStats {
    uint64_t source_heat_updates;
    uint64_t source_warm_queue_pages;
    uint64_t source_warm_sent_pages;
    uint64_t source_warm_sent_bytes;
    uint64_t source_warm_desc_sent_pages;
    uint64_t source_warm_desc_sent_bytes;
    uint64_t source_warm_payload_fallback_pages;
    uint64_t source_warm_desc_skip_unremapped;
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
    CXL_HYBRID_PUBLISH_SOURCE_PENDING_READY,
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

void cxl_hybrid_metadata_cleanup(CXLHybridMetadata *meta);
void cxl_hybrid_warm_page_cleanup(CXLHybridWarmPage *page);
void cxl_hybrid_warm_desc_cleanup(CXLHybridWarmDescriptor *desc);
void cxl_hybrid_warm_desc_batch_cleanup(CXLHybridWarmDescBatch *batch);
void cxl_hybrid_publish_notify_cleanup(CXLHybridPublishNotify *notify);
int cxl_hybrid_metadata_encoded_len(const CXLHybridMetadata *meta,
                                    size_t *len,
                                    Error **errp);
int cxl_hybrid_warm_page_encoded_len(const CXLHybridWarmPage *page,
                                     size_t *len,
                                     Error **errp);
int cxl_hybrid_warm_desc_encoded_len(const CXLHybridWarmDescriptor *desc,
                                     size_t *len,
                                     Error **errp);
int cxl_hybrid_warm_desc_batch_encoded_len(const CXLHybridWarmDescBatch *batch,
                                           size_t *len,
                                           Error **errp);
int cxl_hybrid_publish_notify_encoded_len(const CXLHybridPublishNotify *notify,
                                          size_t *len,
                                          Error **errp);
int cxl_hybrid_metadata_encode(const CXLHybridMetadata *meta,
                               uint8_t *buf,
                               size_t len,
                               Error **errp);
int cxl_hybrid_warm_page_encode(const CXLHybridWarmPage *page,
                                uint8_t *buf,
                                size_t len,
                                Error **errp);
int cxl_hybrid_warm_desc_encode(const CXLHybridWarmDescriptor *desc,
                                uint8_t *buf,
                                size_t len,
                                Error **errp);
int cxl_hybrid_warm_desc_batch_encode(const CXLHybridWarmDescBatch *batch,
                                      uint8_t *buf,
                                      size_t len,
                                      Error **errp);
int cxl_hybrid_publish_notify_encode(const CXLHybridPublishNotify *notify,
                                     uint8_t *buf,
                                     size_t len,
                                     Error **errp);
int cxl_hybrid_metadata_decode(CXLHybridMetadata *meta,
                               const uint8_t *buf,
                               size_t len,
                               Error **errp);
int cxl_hybrid_warm_page_decode(CXLHybridWarmPage *page,
                                const uint8_t *buf,
                                size_t len,
                                Error **errp);
int cxl_hybrid_warm_desc_decode(CXLHybridWarmDescriptor *desc,
                                const uint8_t *buf,
                                size_t len,
                                Error **errp);
int cxl_hybrid_warm_desc_batch_decode(CXLHybridWarmDescBatch *batch,
                                      const uint8_t *buf,
                                      size_t len,
                                      Error **errp);
int cxl_hybrid_publish_notify_decode(CXLHybridPublishNotify *notify,
                                     const uint8_t *buf,
                                     size_t len,
                                     Error **errp);
int cxl_hybrid_warm_page_store(const CXLHybridWarmPage *page, Error **errp);
int cxl_hybrid_warm_desc_store(const CXLHybridWarmDescriptor *desc,
                               Error **errp);
int cxl_hybrid_warm_desc_batch_store(const CXLHybridWarmDescBatch *batch,
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
int cxl_hybrid_dst_staging_read_page(const char *ramblock, uint64_t offset,
                                     void *buf, size_t len, Error **errp);
bool cxl_hybrid_dst_staging_page_present(const char *ramblock,
                                         uint64_t offset);
bool cxl_hybrid_dst_staging_range_present(const char *ramblock,
                                          uint64_t offset,
                                          size_t len);
int cxl_hybrid_dst_staging_wait_page_present(const char *ramblock,
                                             uint64_t offset,
                                             Error **errp);
int cxl_hybrid_dst_staging_wait_range_present(const char *ramblock,
                                              uint64_t offset,
                                              size_t len,
                                              Error **errp);
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
uint32_t cxl_hybrid_fault_publish_generation(void);

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
void cxl_hybrid_enter_phase(CXLHybridPhase phase,
                            CXLMigrationSwitchReason reason,
                            uint64_t iteration);
void cxl_hybrid_cleanup_source(void);
bool cxl_hybrid_enabled(void);
CXLHybridPhase cxl_hybrid_phase(void);
bool cxl_hybrid_init_destination(Error **errp);
void cxl_hybrid_record_warm_miss(const char *rbname, ram_addr_t start);
void cxl_hybrid_account_warm_dirty(const char *rbname, ram_addr_t offset,
                                   ram_addr_t len);
void cxl_hybrid_account_dst_page_sent(const char *rbname, ram_addr_t offset,
                                      ram_addr_t len);
void cxl_hybrid_warm_stats(CXLHybridWarmStats *stats);
bool cxl_hybrid_start_warm_push(MigrationState *s);
void cxl_hybrid_stop_warm_push(void);
int cxl_hybrid_warm_push_iteration(MigrationState *s, Error **errp);
bool cxl_hybrid_source_page_cxl_offset(const char *ramblock,
                                       uint64_t guest_offset,
                                       uint64_t *cxl_offsetp);
int cxl_hybrid_publish_page_to_cxl(const char *ramblock,
                                   uint64_t guest_offset,
                                   uint32_t page_len,
                                   uint32_t generation,
                                   CXLHybridPublishSource source,
                                   uint64_t *cxl_offsetp,
                                   Error **errp);
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
int cxl_hybrid_send_warm_descriptor(QEMUFile *f, const char *ramblock,
                                    uint64_t guest_offset, Error **errp);
int cxl_hybrid_send_warm_page(QEMUFile *f, const char *ramblock,
                              uint64_t offset, const uint8_t *data,
                              size_t len, Error **errp);
int cxl_hybrid_control_init_source(Error **errp);
int cxl_hybrid_control_init_destination(Error **errp);
int cxl_hybrid_control_begin_source_run(Error **errp);
int cxl_hybrid_control_activate_destination(Error **errp);
void cxl_hybrid_control_cleanup_source(void);
void cxl_hybrid_control_cleanup_destination(void);
uint64_t cxl_hybrid_fault_control_region_bytes(void);
size_t cxl_hybrid_control_visible_bitmap_words(uint64_t pages);
size_t cxl_hybrid_control_visible_bitmap_bytes(uint64_t pages);
bool cxl_hybrid_control_page_visible(const CXLHybridControlHeader *hdr,
                                     const unsigned long *visible_bitmap,
                                     uint64_t page_index,
                                     uint32_t generation);
void cxl_hybrid_control_mark_page_visible(const CXLHybridControlHeader *hdr,
                                          unsigned long *visible_bitmap,
                                          uint64_t page_index);
void cxl_hybrid_control_clear_page_visible(const CXLHybridControlHeader *hdr,
                                           unsigned long *visible_bitmap,
                                           uint64_t page_index);
void cxl_hybrid_control_reset_run_state(CXLHybridControlHeader *hdr,
                                        unsigned long *visible_bitmap,
                                        uint32_t generation,
                                        uint32_t visible_page_words);
void cxl_hybrid_control_reset_header_for_run(CXLHybridControlHeader *hdr,
                                             uint32_t generation);
bool cxl_hybrid_ctrl_page_visible(uint64_t page_index, uint32_t generation);
void cxl_hybrid_ctrl_set_page_visible(uint64_t page_index,
                                      uint32_t generation);
void cxl_hybrid_ctrl_clear_page_visible(uint64_t page_index);
int cxl_hybrid_ctrl_wait_page_visible(uint64_t page_index,
                                      uint32_t generation,
                                      Error **errp);
int cxl_hybrid_ctrl_enqueue_fault_request(uint64_t page_index,
                                          uint32_t generation,
                                          uint64_t request_ts_ns,
                                          Error **errp);
bool cxl_hybrid_ctrl_dequeue_fault_request(CXLHybridFaultRequestRecord *record);
int cxl_hybrid_ctrl_enqueue_fault_ready(
    const CXLHybridFaultReadyRecord *record, Error **errp);
bool cxl_hybrid_ctrl_dequeue_fault_ready(CXLHybridFaultReadyRecord *record);

void cxl_hybrid_get_publish_stats(CXLHybridPublishStats *stats);
bool cxl_hybrid_get_published_page_state(const char *ramblock,
                                         uint64_t guest_offset,
                                         CXLHybridPublishedPageState *state);
void cxl_hybrid_note_publish_request_received(const char *ramblock,
                                              uint64_t guest_offset,
                                              uint32_t generation,
                                              uint64_t req_recv_ns);
void cxl_hybrid_record_publish_req_recv_time(uint64_t elapsed_ns);
void cxl_hybrid_record_publish_ready_recv_time(uint64_t elapsed_ns);
int cxl_hybrid_handle_publish_request(const char *ramblock,
                                      uint64_t guest_offset,
                                      uint32_t page_len,
                                      uint32_t generation,
                                      uint64_t req_recv_ns,
                                      Error **errp);
int cxl_hybrid_handle_fault_ready_record(
    const CXLHybridFaultReadyRecord *record, Error **errp);
int cxl_hybrid_publish_fault_request_core(const char *ramblock,
                                          uint64_t guest_offset,
                                          uint32_t page_len,
                                          uint32_t generation,
                                          bool emit_burst,
                                          CXLHybridFaultReadyRecord *primary_ready,
                                          CXLHybridFaultReadyConsumer ready_consumer,
                                          Error **errp);
int cxl_hybrid_handle_publish_quiesce(MigrationIncomingState *mis,
                                      Error **errp);
int cxl_hybrid_send_pending_publish_ready(QEMUFile *f, Error **errp);
int cxl_hybrid_handle_publish_ready(const CXLHybridPublishNotify *notify,
                                    bool fault_primary,
                                    uint64_t ready_recv_ns,
                                    Error **errp);
int cxl_hybrid_completion_publish_remaining_pages(MigrationState *s,
                                                  Error **errp);
void cxl_hybrid_mark_completion_pending_publish_ready(void);
void cxl_hybrid_mark_completion_publish_ready_flushed(void);
uint64_t cxl_hybrid_pending_publish_ready(void);

#endif
