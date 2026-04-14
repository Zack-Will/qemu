/*
 * Copyright (c) 2024 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_MIGRATION_CXL_H
#define QEMU_MIGRATION_CXL_H

#include "qapi/qapi-types-migration.h"
#include "io/channel.h"
#include "io/task.h"
#include "channel.h"
#include "multifd.h"

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

/* Write-redirect support */
bool cxl_page_is_remapped(ram_addr_t offset);
uint64_t cxl_clear_remapped_dirty_bits(RAMBlock *block);
void cxl_account_dirty_sync_ns(uint64_t ns);

#endif
