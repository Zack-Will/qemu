/*
 * Copyright (c) 2026 CXL Migration Contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "qapi/error.h"
#include "migration.h"
#include "cxl.h"
#include "cxl-rdma.h"
#include "system/ramblock.h"
#include "trace.h"

#ifdef CONFIG_RDMA
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#endif

#define CXL_RDMA_SIDECAR_MAGIC 0x43585244u
#define CXL_RDMA_SIDECAR_VERSION 1
#define CXL_RDMA_RESOLVE_TIMEOUT_MS 10000
#define CXL_RDMA_CQ_DEPTH 1024

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

#ifdef CONFIG_RDMA
typedef struct CXLRDMASidecarMR {
    void *host;
    uint64_t length;
    struct ibv_mr *mr;
} CXLRDMASidecarMR;

typedef struct CXLRDMASidecarContext {
    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    bool thread_created;
    bool stop;
    bool running;
    bool failed;
    bool setup_done;
    bool incoming;
    char *host;
    char *port;
    uint64_t total_regions;
    uint64_t bytes_per_region;
    uint64_t pages_per_region;
    uint32_t max_inflight_regions;
    uint8_t max_cover_percent;
    bool pin_all;
    bool src_mr_inflight;
    struct rdma_event_channel *channel;
    struct rdma_cm_id *listen_id;
    struct rdma_cm_id *cm_id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_channel;
    struct ibv_qp *qp;
    struct ibv_mr *dst_mr;
    struct ibv_mr *src_mr;
    CXLRDMASidecarMR *source_mrs;
    uint32_t nr_source_mrs;
    struct ibv_mr *hello_send_mr;
    struct ibv_mr *hello_recv_mr;
    CXLRDMASidecarHello hello_send;
    CXLRDMASidecarHello hello_recv;
    uint64_t remote_base;
    uint64_t remote_len;
    uint32_t remote_rkey;
    CXLHybridRDMASidecarBulkStats stats;
} CXLRDMASidecarContext;

static CXLRDMASidecarContext *cxl_rdma_sidecar;

static bool cxl_rdma_sidecar_geometry_shift(uint64_t bytes, uint32_t *shiftp)
{
    if (!bytes || !is_power_of_2(bytes) || bytes > UINT32_MAX) {
        return false;
    }

    *shiftp = ctz64(bytes);
    return true;
}

static int cxl_rdma_sidecar_parse_addr(CXLRDMASidecarContext *ctx,
                                       const MigrationAddress *addr,
                                       Error **errp)
{
    if (!addr || addr->transport != MIGRATION_ADDRESS_TYPE_RDMA) {
        error_setg(errp, "RDMA sidecar requires an rdma migration address");
        return -1;
    }
    if (!addr->u.rdma.host || !addr->u.rdma.port) {
        error_setg(errp, "RDMA sidecar address requires host and port");
        return -1;
    }

    ctx->host = g_strdup(addr->u.rdma.host);
    ctx->port = g_strdup(addr->u.rdma.port);
    return 0;
}

static int cxl_rdma_sidecar_wait_event(CXLRDMASidecarContext *ctx,
                                       enum rdma_cm_event_type expected,
                                       struct rdma_cm_event **eventp,
                                       Error **errp)
{
    struct rdma_cm_event *event;
    struct pollfd pfd = {
        .fd = ctx->channel->fd,
        .events = POLLIN,
    };

    for (;;) {
        int ret;

        qemu_mutex_lock(&ctx->lock);
        if (ctx->stop) {
            qemu_mutex_unlock(&ctx->lock);
            error_setg(errp, "RDMA sidecar stopped while waiting for CM event");
            return -1;
        }
        qemu_mutex_unlock(&ctx->lock);

        ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_setg_errno(errp, errno,
                             "RDMA sidecar failed to poll CM event channel");
            return -1;
        }
        if (ret > 0) {
            break;
        }
    }

    if (rdma_get_cm_event(ctx->channel, &event) < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar failed to get CM event");
        return -1;
    }
    if (event->event != expected) {
        error_setg(errp, "RDMA sidecar expected CM event %s, got %s",
                   rdma_event_str(expected), rdma_event_str(event->event));
        rdma_ack_cm_event(event);
        return -1;
    }

    *eventp = event;
    return 0;
}

static int cxl_rdma_sidecar_wait_wc(CXLRDMASidecarContext *ctx,
                                    uint64_t wr_id,
                                    Error **errp)
{
    struct ibv_wc wc;

    for (;;) {
        int ret;

        qemu_mutex_lock(&ctx->lock);
        if (ctx->stop) {
            qemu_mutex_unlock(&ctx->lock);
            error_setg(errp, "RDMA sidecar stopped while waiting for CQE");
            return -1;
        }
        qemu_mutex_unlock(&ctx->lock);

        ret = ibv_poll_cq(ctx->cq, 1, &wc);
        if (ret < 0) {
            error_setg_errno(errp, errno,
                             "RDMA sidecar failed to poll completion queue");
            return -1;
        }
        if (ret == 0) {
            g_usleep(1000);
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            error_setg(errp, "RDMA sidecar CQE failed: %s",
                       ibv_wc_status_str(wc.status));
            return -1;
        }
        if (wc.wr_id != wr_id) {
            error_setg(errp,
                       "RDMA sidecar expected CQE wr_id %" PRIu64
                       ", got %" PRIu64,
                       wr_id, wc.wr_id);
            return -1;
        }
        return 0;
    }
}

static int cxl_rdma_sidecar_resolve_source(CXLRDMASidecarContext *ctx,
                                           Error **errp)
{
    g_autofree char *port_str = NULL;
    struct rdma_addrinfo *res = NULL;
    struct rdma_addrinfo *e;
    struct rdma_cm_event *event = NULL;
    int ret;

    ctx->channel = rdma_create_event_channel();
    if (!ctx->channel) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not create event channel");
        return -1;
    }

    ret = rdma_create_id(ctx->channel, &ctx->cm_id, NULL, RDMA_PS_TCP);
    if (ret < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar could not create CM id");
        return -1;
    }

    port_str = g_strdup(ctx->port);
    ret = rdma_getaddrinfo(ctx->host, port_str, NULL, &res);
    if (ret) {
        error_setg(errp, "RDMA sidecar could not resolve %s:%s",
                   ctx->host, ctx->port);
        return -1;
    }

    for (e = res; e; e = e->ai_next) {
        ret = rdma_resolve_addr(ctx->cm_id, NULL, e->ai_dst_addr,
                                CXL_RDMA_RESOLVE_TIMEOUT_MS);
        if (ret >= 0) {
            break;
        }
    }
    rdma_freeaddrinfo(res);
    if (!e) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not resolve address");
        return -1;
    }

    if (cxl_rdma_sidecar_wait_event(ctx, RDMA_CM_EVENT_ADDR_RESOLVED,
                                    &event, errp)) {
        return -1;
    }
    rdma_ack_cm_event(event);

    if (rdma_resolve_route(ctx->cm_id, CXL_RDMA_RESOLVE_TIMEOUT_MS) < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar could not resolve route");
        return -1;
    }
    if (cxl_rdma_sidecar_wait_event(ctx, RDMA_CM_EVENT_ROUTE_RESOLVED,
                                    &event, errp)) {
        return -1;
    }
    rdma_ack_cm_event(event);
    return 0;
}

static int cxl_rdma_sidecar_listen_dest(CXLRDMASidecarContext *ctx,
                                        Error **errp)
{
    struct rdma_addrinfo *res = NULL;
    struct rdma_addrinfo *e;
    int reuse = 1;
    int ret;

    ctx->channel = rdma_create_event_channel();
    if (!ctx->channel) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not create event channel");
        return -1;
    }

    ret = rdma_create_id(ctx->channel, &ctx->listen_id, NULL, RDMA_PS_TCP);
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not create listen CM id");
        return -1;
    }

    ret = rdma_getaddrinfo(ctx->host, ctx->port, NULL, &res);
    if (ret) {
        error_setg(errp, "RDMA sidecar could not resolve listen address %s:%s",
                   ctx->host, ctx->port);
        return -1;
    }

    ret = rdma_set_option(ctx->listen_id, RDMA_OPTION_ID,
                          RDMA_OPTION_ID_REUSEADDR, &reuse, sizeof(reuse));
    if (ret < 0) {
        rdma_freeaddrinfo(res);
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not set REUSEADDR");
        return -1;
    }

    for (e = res; e; e = e->ai_next) {
        ret = rdma_bind_addr(ctx->listen_id, e->ai_dst_addr);
        if (ret >= 0) {
            break;
        }
    }
    rdma_freeaddrinfo(res);
    if (!e) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar could not bind listen address");
        return -1;
    }

    if (rdma_listen(ctx->listen_id, 1) < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar listen failed");
        return -1;
    }
    return 0;
}

static int cxl_rdma_sidecar_accept_id(CXLRDMASidecarContext *ctx, Error **errp)
{
    struct rdma_cm_event *event = NULL;

    if (cxl_rdma_sidecar_wait_event(ctx, RDMA_CM_EVENT_CONNECT_REQUEST,
                                    &event, errp)) {
        return -1;
    }

    ctx->cm_id = event->id;
    rdma_ack_cm_event(event);
    return 0;
}

static int cxl_rdma_sidecar_alloc_pd_cq_qp(CXLRDMASidecarContext *ctx,
                                           Error **errp)
{
    struct ibv_qp_init_attr attr = { 0 };

    ctx->pd = ibv_alloc_pd(ctx->cm_id->verbs);
    if (!ctx->pd) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to allocate protection domain");
        return -1;
    }
    ctx->cm_id->pd = ctx->pd;

    ctx->comp_channel = ibv_create_comp_channel(ctx->cm_id->verbs);
    if (!ctx->comp_channel) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to allocate completion channel");
        return -1;
    }

    ctx->cq = ibv_create_cq(ctx->cm_id->verbs, CXL_RDMA_CQ_DEPTH, NULL,
                            ctx->comp_channel, 0);
    if (!ctx->cq) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to allocate completion queue");
        return -1;
    }

    attr.cap.max_send_wr = MAX((uint32_t)1, ctx->max_inflight_regions);
    attr.cap.max_recv_wr = 2;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.send_cq = ctx->cq;
    attr.recv_cq = ctx->cq;
    attr.qp_type = IBV_QPT_RC;

    if (rdma_create_qp(ctx->cm_id, ctx->pd, &attr) < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar failed to create QP");
        return -1;
    }

    ctx->qp = ctx->cm_id->qp;
    ctx->cm_id->send_cq = ctx->cq;
    ctx->cm_id->recv_cq = ctx->cq;
    ctx->cm_id->send_cq_channel = ctx->comp_channel;
    ctx->cm_id->recv_cq_channel = ctx->comp_channel;
    return 0;
}

static int cxl_rdma_sidecar_fill_hello(CXLRDMASidecarContext *ctx,
                                       CXLRDMASidecarHello *hello,
                                       Error **errp)
{
    uint32_t region_shift = 0;
    uint32_t page_shift = 0;

    if (!ctx->pages_per_region ||
        !cxl_rdma_sidecar_geometry_shift(ctx->bytes_per_region,
                                         &region_shift) ||
        !cxl_rdma_sidecar_geometry_shift(ctx->bytes_per_region /
                                         ctx->pages_per_region, &page_shift)) {
        error_setg(errp, "RDMA sidecar region/page geometry is invalid");
        return -1;
    }

    *hello = (CXLRDMASidecarHello) {
        .magic = cpu_to_be32(CXL_RDMA_SIDECAR_MAGIC),
        .version = cpu_to_be16(CXL_RDMA_SIDECAR_VERSION),
        .flags = 0,
        .remote_base = cpu_to_be64(ctx->dst_mr ?
                                   (uintptr_t)ctx->dst_mr->addr : 0),
        .remote_len = cpu_to_be64(ctx->dst_mr ? ctx->dst_mr->length : 0),
        .remote_rkey = cpu_to_be32(ctx->dst_mr ? ctx->dst_mr->rkey : 0),
        .region_shift = cpu_to_be32(region_shift),
        .page_shift = cpu_to_be32(page_shift),
        .reserved = 0,
    };
    return 0;
}

static int cxl_rdma_sidecar_validate_hello(CXLRDMASidecarContext *ctx,
                                           const CXLRDMASidecarHello *hello,
                                           Error **errp)
{
    uint32_t region_shift = 0;
    uint32_t page_shift = 0;
    uint64_t remote_len;

    if (!cxl_rdma_sidecar_geometry_shift(ctx->bytes_per_region,
                                         &region_shift) ||
        !cxl_rdma_sidecar_geometry_shift(ctx->bytes_per_region /
                                         ctx->pages_per_region, &page_shift)) {
        error_setg(errp, "RDMA sidecar region/page geometry is invalid");
        return -1;
    }

    if (be32_to_cpu(hello->magic) != CXL_RDMA_SIDECAR_MAGIC ||
        be16_to_cpu(hello->version) != CXL_RDMA_SIDECAR_VERSION) {
        error_setg(errp, "RDMA sidecar hello version mismatch");
        return -1;
    }
    if (be32_to_cpu(hello->region_shift) != region_shift ||
        be32_to_cpu(hello->page_shift) != page_shift) {
        error_setg(errp, "RDMA sidecar region geometry mismatch");
        return -1;
    }

    remote_len = be64_to_cpu(hello->remote_len);
    if (!remote_len || remote_len < ctx->total_regions * ctx->bytes_per_region) {
        error_setg(errp,
                   "RDMA sidecar destination MR too small: %" PRIu64
                   " < %" PRIu64,
                   remote_len, ctx->total_regions * ctx->bytes_per_region);
        return -1;
    }

    ctx->remote_base = be64_to_cpu(hello->remote_base);
    ctx->remote_len = remote_len;
    ctx->remote_rkey = be32_to_cpu(hello->remote_rkey);
    return 0;
}

static int cxl_rdma_sidecar_post_hello_recv(CXLRDMASidecarContext *ctx,
                                            Error **errp)
{
    int ret;

    if (ctx->hello_recv_mr) {
        return 0;
    }

    ctx->hello_recv_mr = ibv_reg_mr(ctx->pd, &ctx->hello_recv,
                                    sizeof(ctx->hello_recv),
                                    IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->hello_recv_mr) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to register hello recv buffer");
        return -1;
    }

    ret = rdma_post_recv(ctx->cm_id, (void *)(uintptr_t)2,
                         &ctx->hello_recv, sizeof(ctx->hello_recv),
                         ctx->hello_recv_mr);
    if (ret) {
        error_setg_errno(errp, ret,
                         "RDMA sidecar failed to post destination hello recv");
        return -1;
    }
    return 0;
}

static int cxl_rdma_sidecar_exchange_hello(CXLRDMASidecarContext *ctx,
                                           Error **errp)
{
    int ret;

    if (ctx->incoming) {
        if (cxl_rdma_sidecar_fill_hello(ctx, &ctx->hello_send, errp)) {
            return -1;
        }
        ctx->hello_send_mr = ibv_reg_mr(ctx->pd, &ctx->hello_send,
                                        sizeof(ctx->hello_send),
                                        IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->hello_send_mr) {
            error_setg_errno(
                errp, errno,
                "RDMA sidecar failed to register hello send buffer");
            return -1;
        }

        ret = rdma_post_send(ctx->cm_id, (void *)(uintptr_t)1,
                             &ctx->hello_send, sizeof(ctx->hello_send),
                             ctx->hello_send_mr, IBV_SEND_SIGNALED);
        if (ret) {
            error_setg_errno(errp, ret,
                             "RDMA sidecar failed to send destination hello");
            return -1;
        }
        return cxl_rdma_sidecar_wait_wc(ctx, 1, errp);
    }

    if (cxl_rdma_sidecar_post_hello_recv(ctx, errp)) {
        return -1;
    }
    if (cxl_rdma_sidecar_wait_wc(ctx, 2, errp)) {
        return -1;
    }
    return cxl_rdma_sidecar_validate_hello(ctx, &ctx->hello_recv, errp);
}

static int cxl_rdma_sidecar_connect_qp(CXLRDMASidecarContext *ctx, Error **errp)
{
    struct rdma_conn_param conn_param = {
        .initiator_depth = 2,
        .responder_resources = 2,
        .retry_count = 5,
    };
    struct rdma_cm_event *event = NULL;
    int ret;

    if (ctx->incoming) {
        ret = rdma_accept(ctx->cm_id, &conn_param);
    } else {
        ret = rdma_connect(ctx->cm_id, &conn_param);
    }
    if (ret < 0) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar connection handshake failed");
        return -1;
    }

    if (cxl_rdma_sidecar_wait_event(ctx, RDMA_CM_EVENT_ESTABLISHED,
                                    &event, errp)) {
        return -1;
    }
    rdma_ack_cm_event(event);
    return 0;
}

static int cxl_rdma_sidecar_register_destination(CXLRDMASidecarContext *ctx,
                                                 Error **errp)
{
    void *base = NULL;
    size_t size = 0;

    if (!ctx->incoming) {
        return 0;
    }

    if (!cxl_hybrid_rdma_sidecar_get_backing(&base, &size) || !base || !size) {
        error_setg(errp, "RDMA sidecar destination CXL backing is not mapped");
        return -1;
    }

    ctx->dst_mr = ibv_reg_mr(ctx->pd, base, size,
                             IBV_ACCESS_LOCAL_WRITE |
                             IBV_ACCESS_REMOTE_WRITE |
                             IBV_ACCESS_REMOTE_READ);
    if (!ctx->dst_mr) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to register destination CXL MR");
        return -1;
    }
    cxl_hybrid_account_rdma_sidecar_registered(size);
    return 0;
}

static int cxl_rdma_sidecar_register_source_region(CXLRDMASidecarContext *ctx,
                                                   const CXLHybridRDMABulkClaim *claim,
                                                   Error **errp)
{
    int access = IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ;

    if (!claim || !claim->src || !claim->bytes) {
        error_setg(errp, "RDMA sidecar source claim is empty");
        return -1;
    }
    if (ctx->src_mr_inflight) {
        error_setg(errp,
                   "RDMA sidecar source MR already has an in-flight write");
        return -1;
    }
    if (ctx->src_mr) {
        ibv_dereg_mr(ctx->src_mr);
        ctx->src_mr = NULL;
    }

    ctx->src_mr = ibv_reg_mr(ctx->pd, claim->src, claim->bytes, access);
    if (!ctx->src_mr) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to register source RAM MR");
        return -1;
    }
    cxl_hybrid_account_rdma_sidecar_registered(claim->bytes);
    ctx->src_mr_inflight = true;
    return 0;
}

static int cxl_rdma_sidecar_register_one_ramblock(RAMBlock *block,
                                                  void *opaque)
{
    CXLRDMASidecarContext *ctx = opaque;
    CXLRDMASidecarMR entry;
    int access = IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ;

    if (!block->host || !block->used_length) {
        return 0;
    }

    entry = (CXLRDMASidecarMR) {
        .host = block->host,
        .length = block->used_length,
        .mr = ibv_reg_mr(ctx->pd, block->host, block->used_length, access),
    };
    if (!entry.mr) {
        return -1;
    }

    ctx->source_mrs = g_renew(CXLRDMASidecarMR, ctx->source_mrs,
                              ctx->nr_source_mrs + 1);
    ctx->source_mrs[ctx->nr_source_mrs++] = entry;
    cxl_hybrid_account_rdma_sidecar_registered(block->used_length);
    return 0;
}

static int cxl_rdma_sidecar_register_source_pin_all(
    CXLRDMASidecarContext *ctx,
    Error **errp)
{
    int ret;

    if (ctx->source_mrs) {
        return 0;
    }

    ret = foreach_not_ignored_block(cxl_rdma_sidecar_register_one_ramblock,
                                    ctx);
    if (ret) {
        error_setg(errp, "RDMA sidecar failed to register source RAMBlocks");
        return -1;
    }
    return 0;
}

static uint32_t cxl_rdma_sidecar_source_lkey_for_claim(
    CXLRDMASidecarContext *ctx,
    const CXLHybridRDMABulkClaim *claim)
{
    uintptr_t src = (uintptr_t)claim->src;
    uintptr_t end = src + claim->bytes;

    if (ctx->pin_all) {
        for (uint32_t i = 0; i < ctx->nr_source_mrs; i++) {
            CXLRDMASidecarMR *entry = &ctx->source_mrs[i];
            uintptr_t mr_start = (uintptr_t)entry->host;
            uintptr_t mr_end = mr_start + entry->length;

            if (src >= mr_start && end <= mr_end) {
                return entry->mr->lkey;
            }
        }
        return 0;
    }

    assert(ctx->src_mr);
    assert(src >= (uintptr_t)ctx->src_mr->addr);
    assert(end <= (uintptr_t)ctx->src_mr->addr + ctx->src_mr->length);
    return ctx->src_mr->lkey;
}

static int G_GNUC_UNUSED cxl_rdma_sidecar_post_write(
    CXLRDMASidecarContext *ctx,
    const CXLHybridRDMABulkClaim *claim,
    Error **errp)
{
    struct ibv_sge sge;
    struct ibv_send_wr wr = { 0 };
    struct ibv_send_wr *bad_wr = NULL;
    int ret;

    if (!ctx || !ctx->qp || !claim) {
        error_setg(errp, "RDMA sidecar is not ready to post writes");
        return -1;
    }
    if (claim->cxl_offset > ctx->remote_len ||
        claim->bytes > ctx->remote_len - claim->cxl_offset) {
        error_setg(errp, "RDMA sidecar claim exceeds destination MR");
        return -1;
    }
    if (ctx->pin_all) {
        if (cxl_rdma_sidecar_register_source_pin_all(ctx, errp)) {
            return -1;
        }
    } else if (cxl_rdma_sidecar_register_source_region(ctx, claim, errp)) {
        return -1;
    }

    sge = (struct ibv_sge) {
        .addr = (uintptr_t)claim->src,
        .length = claim->bytes,
        .lkey = cxl_rdma_sidecar_source_lkey_for_claim(ctx, claim),
    };
    if (!sge.lkey) {
        if (!ctx->pin_all) {
            ctx->src_mr_inflight = false;
        }
        error_setg(errp, "RDMA sidecar claim has no registered source MR");
        return -1;
    }
    wr = (struct ibv_send_wr) {
        .wr_id = claim->region_index,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr = ctx->remote_base + claim->cxl_offset,
        .wr.rdma.rkey = ctx->remote_rkey,
    };

    ret = ibv_post_send(ctx->qp, &wr, &bad_wr);
    if (ret) {
        if (!ctx->pin_all) {
            ctx->src_mr_inflight = false;
        }
        error_setg_errno(errp, ret, "RDMA sidecar ibv_post_send failed");
        return -1;
    }
    cxl_hybrid_account_rdma_sidecar_posted(claim->region_index, claim->bytes);
    trace_cxl_rdma_sidecar_post(claim->region_index, (uintptr_t)claim->src,
                                wr.wr.rdma.remote_addr, claim->bytes);
    return 0;
}

static int G_GNUC_UNUSED cxl_rdma_sidecar_poll_completion(
    CXLRDMASidecarContext *ctx,
    Error **errp)
{
    struct ibv_wc wc;
    int ret;

    ret = ibv_poll_cq(ctx->cq, 1, &wc);
    if (ret < 0) {
        error_setg_errno(errp, errno, "RDMA sidecar ibv_poll_cq failed");
        return -1;
    }
    if (ret == 0) {
        return 0;
    }
    if (wc.status != IBV_WC_SUCCESS) {
        if (!ctx->pin_all) {
            ctx->src_mr_inflight = false;
        }
        cxl_hybrid_account_rdma_sidecar_failed(wc.wr_id);
        error_setg(errp, "RDMA sidecar write failed: %s",
                   ibv_wc_status_str(wc.status));
        return -1;
    }

    if (!ctx->pin_all) {
        ctx->src_mr_inflight = false;
    }
    qatomic_inc(&ctx->stats.rdma_bulk_regions);
    qatomic_add(&ctx->stats.rdma_bulk_bytes, ctx->bytes_per_region);
    cxl_hybrid_account_rdma_sidecar_completed(wc.wr_id,
                                              ctx->bytes_per_region);
    cxl_hybrid_mark_region_rdma_ready(wc.wr_id);
    return 1;
}

static void cxl_rdma_sidecar_cleanup(CXLRDMASidecarContext *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->src_mr) {
        ibv_dereg_mr(ctx->src_mr);
        ctx->src_mr = NULL;
        ctx->src_mr_inflight = false;
    }
    for (uint32_t i = 0; i < ctx->nr_source_mrs; i++) {
        if (ctx->source_mrs[i].mr) {
            ibv_dereg_mr(ctx->source_mrs[i].mr);
            ctx->source_mrs[i].mr = NULL;
        }
    }
    g_free(ctx->source_mrs);
    ctx->source_mrs = NULL;
    ctx->nr_source_mrs = 0;
    if (ctx->hello_recv_mr) {
        ibv_dereg_mr(ctx->hello_recv_mr);
        ctx->hello_recv_mr = NULL;
    }
    if (ctx->hello_send_mr) {
        ibv_dereg_mr(ctx->hello_send_mr);
        ctx->hello_send_mr = NULL;
    }
    if (ctx->dst_mr) {
        ibv_dereg_mr(ctx->dst_mr);
        ctx->dst_mr = NULL;
    }
    if (ctx->qp && ctx->cm_id) {
        rdma_destroy_qp(ctx->cm_id);
        ctx->qp = NULL;
    }
    if (ctx->cq) {
        ibv_destroy_cq(ctx->cq);
        ctx->cq = NULL;
    }
    if (ctx->comp_channel) {
        ibv_destroy_comp_channel(ctx->comp_channel);
        ctx->comp_channel = NULL;
    }
    if (ctx->pd) {
        ibv_dealloc_pd(ctx->pd);
        ctx->pd = NULL;
    }
    if (ctx->cm_id) {
        rdma_destroy_id(ctx->cm_id);
        ctx->cm_id = NULL;
    }
    if (ctx->listen_id) {
        rdma_destroy_id(ctx->listen_id);
        ctx->listen_id = NULL;
    }
    if (ctx->channel) {
        rdma_destroy_event_channel(ctx->channel);
        ctx->channel = NULL;
    }
}

static void *cxl_rdma_sidecar_thread(void *opaque)
{
    CXLRDMASidecarContext *ctx = opaque;
    Error *local_err = NULL;
    uint64_t start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (ctx->incoming) {
        if (cxl_rdma_sidecar_listen_dest(ctx, &local_err)) {
            goto fail;
        }

        qemu_mutex_lock(&ctx->lock);
        ctx->setup_done = true;
        qemu_cond_broadcast(&ctx->cond);
        qemu_mutex_unlock(&ctx->lock);

        if (cxl_rdma_sidecar_accept_id(ctx, &local_err) ||
            cxl_rdma_sidecar_alloc_pd_cq_qp(ctx, &local_err) ||
            cxl_rdma_sidecar_register_destination(ctx, &local_err) ||
            cxl_rdma_sidecar_connect_qp(ctx, &local_err) ||
            cxl_rdma_sidecar_exchange_hello(ctx, &local_err)) {
            goto fail;
        }
    } else {
        if (cxl_rdma_sidecar_resolve_source(ctx, &local_err) ||
            cxl_rdma_sidecar_alloc_pd_cq_qp(ctx, &local_err) ||
            cxl_rdma_sidecar_post_hello_recv(ctx, &local_err) ||
            cxl_rdma_sidecar_connect_qp(ctx, &local_err) ||
            cxl_rdma_sidecar_exchange_hello(ctx, &local_err)) {
            goto fail;
        }
    }

    qemu_mutex_lock(&ctx->lock);
    ctx->running = true;
    ctx->setup_done = true;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
    cxl_hybrid_account_rdma_sidecar_connect(
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - start_ns);

    qemu_mutex_lock(&ctx->lock);
    while (!ctx->stop) {
        qemu_cond_timedwait(&ctx->cond, &ctx->lock, 100);
    }
    qemu_mutex_unlock(&ctx->lock);
    return NULL;

fail:
    qemu_mutex_lock(&ctx->lock);
    if (!ctx->stop) {
        ctx->failed = true;
    }
    ctx->setup_done = true;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
    if (ctx->failed) {
        error_report_err(local_err);
    } else {
        error_free(local_err);
    }
    cxl_rdma_sidecar_cleanup(ctx);
    return NULL;
}

int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp)
{
    CXLRDMASidecarContext *ctx;

    if (!cfg || !cfg->addr || !cfg->total_regions || !cfg->bytes_per_region ||
        !cfg->pages_per_region || !cfg->max_inflight_regions) {
        error_setg(errp, "RDMA sidecar configuration is incomplete");
        return -1;
    }
    if (!cxl_rdma_sidecar_geometry_shift(cfg->bytes_per_region,
                                         &(uint32_t) { 0 })) {
        error_setg(errp, "RDMA sidecar region size must be a power of two");
        return -1;
    }
    if (cxl_rdma_sidecar_running()) {
        return 0;
    }

    cxl_rdma_sidecar_stop();

    ctx = g_new0(CXLRDMASidecarContext, 1);
    qemu_mutex_init(&ctx->lock);
    qemu_cond_init(&ctx->cond);
    ctx->incoming = cfg->incoming;
    ctx->total_regions = cfg->total_regions;
    ctx->bytes_per_region = cfg->bytes_per_region;
    ctx->pages_per_region = cfg->pages_per_region;
    ctx->max_inflight_regions = cfg->max_inflight_regions;
    ctx->max_cover_percent = cfg->max_cover_percent;
    ctx->pin_all = cfg->pin_all;

    if (cxl_rdma_sidecar_parse_addr(ctx, cfg->addr, errp)) {
        qemu_cond_destroy(&ctx->cond);
        qemu_mutex_destroy(&ctx->lock);
        g_free(ctx);
        return -1;
    }

    trace_cxl_rdma_sidecar_connect_start(ctx->host, ctx->port);
    cxl_rdma_sidecar = ctx;
    qemu_thread_create(&ctx->thread, "cxl-rdma-sidecar",
                       cxl_rdma_sidecar_thread, ctx, QEMU_THREAD_JOINABLE);
    ctx->thread_created = true;
    qemu_mutex_lock(&ctx->lock);
    while (!ctx->setup_done) {
        qemu_cond_wait(&ctx->cond, &ctx->lock);
    }
    if (ctx->failed) {
        qemu_mutex_unlock(&ctx->lock);
        cxl_rdma_sidecar_stop();
        error_setg(errp, "RDMA sidecar transport setup failed");
        return -1;
    }
    qemu_mutex_unlock(&ctx->lock);
    return 0;
}

void cxl_rdma_sidecar_stop(void)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;

    if (!ctx) {
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    ctx->stop = true;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);

    if (ctx->thread_created) {
        qemu_thread_join(&ctx->thread);
        ctx->thread_created = false;
    }

    cxl_rdma_sidecar_cleanup(ctx);
    qemu_cond_destroy(&ctx->cond);
    qemu_mutex_destroy(&ctx->lock);
    g_free(ctx->host);
    g_free(ctx->port);
    g_free(ctx);
    cxl_rdma_sidecar = NULL;
}

bool cxl_rdma_sidecar_running(void)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool running;

    if (!ctx) {
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    running = ctx->running && !ctx->failed && !ctx->stop;
    qemu_mutex_unlock(&ctx->lock);
    return running;
}

void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;

    if (!stats) {
        return;
    }
    if (!ctx) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    stats->rdma_bulk_regions =
        qatomic_read(&ctx->stats.rdma_bulk_regions);
    stats->rdma_bulk_bytes =
        qatomic_read(&ctx->stats.rdma_bulk_bytes);
}
#else
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

void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats)
{
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
}
#endif
