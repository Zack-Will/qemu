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
    bool connected;
    char *host;
    char *port;
    uint64_t total_regions;
    uint64_t bytes_per_region;
    uint64_t pages_per_region;
    uint32_t max_inflight_regions;
    uint8_t max_cover_percent;
    bool pin_all;
    bool src_mr_inflight;
    bool draining;
    CXLHybridRDMABulkClaim *queue;
    uint32_t queue_capacity;
    uint32_t queue_head;
    uint32_t queue_len;
    CXLHybridRDMABulkClaim *inflight_claims;
    uint32_t inflight_capacity;
    uint32_t inflight_len;
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
    CXLHybridRDMASidecarOps ops;
    CXLHybridRDMASidecarBulkStats stats;
} CXLRDMASidecarContext;

typedef struct CXLRDMASidecarDestinationRegister {
    CXLRDMASidecarContext *ctx;
    Error **errp;
    void *host;
    uint64_t length;
} CXLRDMASidecarDestinationRegister;

static CXLRDMASidecarContext *cxl_rdma_sidecar;

static void cxl_rdma_sidecar_drop_claim(CXLRDMASidecarContext *ctx,
                                        const CXLHybridRDMABulkClaim *claim)
{
    if (ctx->ops.drop_bulk_claim) {
        ctx->ops.drop_bulk_claim(claim, ctx->ops.opaque);
    }
}

static bool cxl_rdma_sidecar_stopped(CXLRDMASidecarContext *ctx)
{
    bool stopped;

    qemu_mutex_lock(&ctx->lock);
    stopped = ctx->stop;
    qemu_mutex_unlock(&ctx->lock);
    return stopped;
}

static void cxl_rdma_sidecar_mark_failed(CXLRDMASidecarContext *ctx,
                                         Error *err)
{
    qemu_mutex_lock(&ctx->lock);
    if (!ctx->stop) {
        ctx->failed = true;
    }
    ctx->setup_done = true;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);

    if (!ctx->failed) {
        error_free(err);
        return;
    }

    cxl_hybrid_account_rdma_sidecar_failed(UINT64_MAX);
    if (err) {
        if (ctx->ops.propagate_error) {
            ctx->ops.propagate_error(error_copy(err), ctx->ops.opaque);
        }
        error_report_err(err);
    }
}

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
    if (!remote_len || remote_len < ctx->bytes_per_region) {
        error_setg(errp,
                   "RDMA sidecar destination MR too small: %" PRIu64
                   " < %" PRIu64,
                   remote_len, ctx->bytes_per_region);
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
    ctx->connected = true;
    return 0;
}

static int cxl_rdma_sidecar_register_destination_ramblock(RAMBlock *block,
                                                          void *opaque)
{
    CXLRDMASidecarDestinationRegister *reg = opaque;

    if (!block->host || !block->used_length) {
        return 0;
    }

    if (!reg->host || block->used_length > reg->length) {
        reg->host = block->host;
        reg->length = block->used_length;
    }
    return 0;
}

static int cxl_rdma_sidecar_register_destination(CXLRDMASidecarContext *ctx,
                                                 Error **errp)
{
    CXLRDMASidecarDestinationRegister reg = {
        .ctx = ctx,
        .errp = errp,
    };
    int access = IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ;
    int ret;

    if (!ctx->incoming) {
        return 0;
    }

    if (!ctx->ops.foreach_ramblock) {
        error_setg(errp,
                   "RDMA sidecar destination RAM registration requires a RAMBlock iterator");
        return -1;
    }

    ret = ctx->ops.foreach_ramblock(
        cxl_rdma_sidecar_register_destination_ramblock, &reg, ctx->ops.opaque);
    if (ret) {
        return -1;
    }
    if (!reg.host || !reg.length) {
        error_setg(errp, "RDMA sidecar destination RAMBlock is not mapped");
        return -1;
    }
    if (reg.length < ctx->bytes_per_region) {
        error_setg(errp,
                   "RDMA sidecar destination RAM MR too small: %" PRIu64
                   " < %" PRIu64, reg.length, ctx->bytes_per_region);
        return -1;
    }

    ctx->dst_mr = ibv_reg_mr(ctx->pd, reg.host, reg.length, access);
    if (!ctx->dst_mr) {
        error_setg_errno(errp, errno,
                         "RDMA sidecar failed to register destination RAM MR");
        return -1;
    }
    cxl_hybrid_account_rdma_sidecar_registered(reg.length);
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
    if (!ctx->ops.foreach_ramblock) {
        error_setg(errp,
                   "RDMA sidecar pin-all requires a RAMBlock iterator");
        return -1;
    }

    ret = ctx->ops.foreach_ramblock(cxl_rdma_sidecar_register_one_ramblock,
                                    ctx, ctx->ops.opaque);
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
    if (!ctx->pin_all &&
        cxl_rdma_sidecar_register_source_region(ctx, claim, errp)) {
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
    uint64_t *region_index,
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
        if (region_index) {
            *region_index = wc.wr_id;
        }
        cxl_hybrid_account_rdma_sidecar_failed(wc.wr_id);
        error_setg(errp, "RDMA sidecar write failed: %s",
                   ibv_wc_status_str(wc.status));
        return -1;
    }

    if (!ctx->pin_all) {
        ctx->src_mr_inflight = false;
    }
    if (region_index) {
        *region_index = wc.wr_id;
    }
    qatomic_inc(&ctx->stats.rdma_bulk_regions);
    qatomic_add(&ctx->stats.rdma_bulk_bytes, ctx->bytes_per_region);
    cxl_hybrid_account_rdma_sidecar_completed(wc.wr_id,
                                              ctx->bytes_per_region);
    return 1;
}

static void cxl_rdma_sidecar_backoff(CXLRDMASidecarContext *ctx, int ms)
{
    qemu_mutex_lock(&ctx->lock);
    if (!ctx->stop) {
        qemu_cond_timedwait(&ctx->cond, &ctx->lock, ms);
    }
    qemu_mutex_unlock(&ctx->lock);
}

static bool cxl_rdma_sidecar_dequeue_bulk_claim(CXLRDMASidecarContext *ctx,
                                                CXLHybridRDMABulkClaim *claim)
{
    bool running;
    bool postcopy;
    bool failed;
    bool bulk_active;

    qemu_mutex_lock(&ctx->lock);
    while (!ctx->stop && !ctx->queue_len) {
        qemu_cond_timedwait(&ctx->cond, &ctx->lock, 1);

        running = !ctx->ops.migration_running ||
                  ctx->ops.migration_running(ctx->ops.opaque);
        postcopy = ctx->ops.migration_postcopy &&
                   ctx->ops.migration_postcopy(ctx->ops.opaque);
        failed = ctx->ops.migration_failed &&
                 ctx->ops.migration_failed(ctx->ops.opaque);
        bulk_active = !ctx->ops.bulk_active ||
                      ctx->ops.bulk_active(ctx->ops.opaque);
        if (!running || postcopy || failed || !bulk_active) {
            break;
        }
    }

    if (!ctx->stop && !ctx->draining && ctx->queue_len) {
        *claim = ctx->queue[ctx->queue_head];
        ctx->queue_head = (ctx->queue_head + 1) % ctx->queue_capacity;
        ctx->queue_len--;
        qemu_mutex_unlock(&ctx->lock);
        return true;
    }

    qemu_mutex_unlock(&ctx->lock);
    return false;
}

static uint32_t cxl_rdma_sidecar_inflight_capacity(
    const CXLRDMASidecarContext *ctx)
{
    if (!ctx->pin_all) {
        return 1;
    }
    return MAX((uint32_t)1, ctx->inflight_capacity);
}

static void cxl_rdma_sidecar_add_inflight_claim(
    CXLRDMASidecarContext *ctx,
    const CXLHybridRDMABulkClaim *claim)
{
    qemu_mutex_lock(&ctx->lock);
    assert(ctx->inflight_len < ctx->inflight_capacity);
    ctx->inflight_claims[ctx->inflight_len++] = *claim;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
}

static bool cxl_rdma_sidecar_finish_inflight_claim(
    CXLRDMASidecarContext *ctx,
    uint64_t region_index,
    CXLHybridRDMABulkClaim *claim)
{
    bool found = false;

    qemu_mutex_lock(&ctx->lock);
    for (uint32_t i = 0; i < ctx->inflight_len; i++) {
        if (ctx->inflight_claims[i].region_index == region_index) {
            if (claim) {
                *claim = ctx->inflight_claims[i];
            }
            ctx->inflight_claims[i] =
                ctx->inflight_claims[ctx->inflight_len - 1];
            ctx->inflight_len--;
            found = true;
            break;
        }
    }
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
    return found;
}

static void cxl_rdma_sidecar_drop_inflight_claims(
    CXLRDMASidecarContext *ctx)
{
    CXLHybridRDMABulkClaim *claims;
    uint32_t len;

    if (!ctx || !ctx->inflight_capacity) {
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    len = ctx->inflight_len;
    claims = len ? g_new(CXLHybridRDMABulkClaim, len) : NULL;
    for (uint32_t i = 0; i < len; i++) {
        claims[i] = ctx->inflight_claims[i];
    }
    ctx->inflight_len = 0;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);

    for (uint32_t i = 0; i < len; i++) {
        cxl_rdma_sidecar_drop_claim(ctx, &claims[i]);
    }
    g_free(claims);
}

static int cxl_rdma_sidecar_poll_inflight_completion(
    CXLRDMASidecarContext *ctx,
    Error **errp)
{
    CXLHybridRDMABulkClaim claim = { 0 };
    uint64_t region_index = UINT64_MAX;
    uint32_t completed_pages = 0;
    uint32_t stale_pages = 0;
    uint64_t completed_time_ns = 0;
    uint64_t page_bytes;
    int ret;

    ret = cxl_rdma_sidecar_poll_completion(ctx, &region_index, errp);
    if (ret <= 0) {
        return ret;
    }

    if (!cxl_rdma_sidecar_finish_inflight_claim(ctx, region_index, &claim)) {
        error_setg(errp,
                   "RDMA sidecar completion for unknown region %" PRIu64,
                   region_index);
        return -1;
    }
    if (claim.page_desc && ctx->ops.complete_bulk_claim) {
        ctx->ops.complete_bulk_claim(&claim, &completed_pages, &stale_pages,
                                     ctx->ops.opaque);
        if (claim.post_time_ns) {
            completed_time_ns =
                qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - claim.post_time_ns;
        }
        qatomic_add(&ctx->stats.page_state_rdma_completed_pages,
                    completed_pages);
        page_bytes = ctx->pages_per_region ?
            ctx->bytes_per_region / ctx->pages_per_region : 0;
        qatomic_add(&ctx->stats.page_state_rdma_completed_bytes,
                    (uint64_t)completed_pages * page_bytes);
        qatomic_add(&ctx->stats.page_state_rdma_completed_time_ns,
                    completed_time_ns);
        qatomic_add(&ctx->stats.page_state_rdma_stale_pages, stale_pages);
    }
    cxl_hybrid_region_drop_rdma(claim.region_index);
    cxl_hybrid_rdma_bulk_claim_release(&claim);
    return 1;
}

static bool cxl_rdma_sidecar_can_schedule_more(CXLRDMASidecarContext *ctx)
{
    bool can_schedule;

    qemu_mutex_lock(&ctx->lock);
    can_schedule = !ctx->draining &&
                   ctx->inflight_len <
                   cxl_rdma_sidecar_inflight_capacity(ctx);
    qemu_mutex_unlock(&ctx->lock);
    return can_schedule;
}

static void cxl_rdma_sidecar_source_loop(CXLRDMASidecarContext *ctx)
{
    while (!cxl_rdma_sidecar_stopped(ctx)) {
        Error *local_err = NULL;
        bool running = !ctx->ops.migration_running ||
                       ctx->ops.migration_running(ctx->ops.opaque);
        bool postcopy = ctx->ops.migration_postcopy &&
                        ctx->ops.migration_postcopy(ctx->ops.opaque);
        bool failed = ctx->ops.migration_failed &&
                      ctx->ops.migration_failed(ctx->ops.opaque);
        bool bulk_active = !ctx->ops.bulk_active ||
                           ctx->ops.bulk_active(ctx->ops.opaque);
        bool made_progress = false;
        int ret;

        if (!running || failed) {
            return;
        }

        ret = cxl_rdma_sidecar_poll_inflight_completion(ctx, &local_err);
        if (ret < 0) {
            cxl_rdma_sidecar_drop_inflight_claims(ctx);
            cxl_rdma_sidecar_mark_failed(ctx, local_err);
            return;
        }
        made_progress = ret > 0;

        while (!postcopy && bulk_active &&
               cxl_rdma_sidecar_can_schedule_more(ctx)) {
            CXLHybridRDMABulkClaim claim = { 0 };

            if (!cxl_rdma_sidecar_dequeue_bulk_claim(ctx, &claim)) {
                break;
            }

            trace_cxl_rdma_sidecar_schedule(claim.region_index, claim.bytes);
            claim.post_time_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            if (cxl_rdma_sidecar_post_write(ctx, &claim, &local_err) < 0) {
                cxl_rdma_sidecar_drop_claim(ctx, &claim);
                cxl_hybrid_account_rdma_sidecar_failed(claim.region_index);
                cxl_rdma_sidecar_mark_failed(ctx, local_err);
                return;
            }
            cxl_rdma_sidecar_add_inflight_claim(ctx, &claim);
            made_progress = true;
        }

        if (!made_progress) {
            cxl_rdma_sidecar_backoff(ctx, 1);
        }
    }
}

bool cxl_rdma_sidecar_enqueue_bulk_claim(
    const CXLHybridRDMABulkClaim *claim)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool bulk_active;
    uint32_t tail;

    if (!ctx || !claim) {
        return false;
    }

    qemu_mutex_lock(&ctx->lock);
    bulk_active = !ctx->ops.bulk_active ||
                  ctx->ops.bulk_active(ctx->ops.opaque);
    if (ctx->incoming || !ctx->running || ctx->failed || ctx->stop ||
        ctx->draining || !bulk_active || !ctx->queue_capacity ||
        ctx->queue_len >= ctx->queue_capacity) {
        qemu_mutex_unlock(&ctx->lock);
        return false;
    }

    tail = (ctx->queue_head + ctx->queue_len) % ctx->queue_capacity;
    ctx->queue[tail] = *claim;
    ctx->queue_len++;
    qemu_cond_broadcast(&ctx->cond);
    qemu_mutex_unlock(&ctx->lock);
    return true;
}

static void cxl_rdma_sidecar_drop_queued_claims(CXLRDMASidecarContext *ctx)
{
    CXLHybridRDMABulkClaim *claims;
    uint32_t len;

    if (!ctx || !ctx->queue_capacity) {
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    len = ctx->queue_len;
    claims = len ? g_new(CXLHybridRDMABulkClaim, len) : NULL;
    for (uint32_t i = 0; i < len; i++) {
        claims[i] = ctx->queue[(ctx->queue_head + i) % ctx->queue_capacity];
    }
    ctx->queue_head = 0;
    ctx->queue_len = 0;
    qemu_mutex_unlock(&ctx->lock);

    for (uint32_t i = 0; i < len; i++) {
        cxl_rdma_sidecar_drop_claim(ctx, &claims[i]);
    }
    g_free(claims);
}

void cxl_rdma_sidecar_drain_bulk_claims(void)
{
    CXLRDMASidecarContext *ctx = cxl_rdma_sidecar;
    bool running;

    if (!ctx) {
        return;
    }

    qemu_mutex_lock(&ctx->lock);
    running = ctx->running && !ctx->failed && !ctx->stop;
    ctx->draining = true;
    qemu_cond_broadcast(&ctx->cond);
    while (running && ctx->inflight_len) {
        qemu_cond_timedwait(&ctx->cond, &ctx->lock, 1);
        running = ctx->running && !ctx->failed && !ctx->stop;
    }
    qemu_mutex_unlock(&ctx->lock);
    if (!running) {
        cxl_rdma_sidecar_drop_inflight_claims(ctx);
    }
    cxl_rdma_sidecar_drop_queued_claims(ctx);
}

static void cxl_rdma_sidecar_cleanup(CXLRDMASidecarContext *ctx)
{
    if (!ctx) {
        return;
    }

    cxl_rdma_sidecar_drop_queued_claims(ctx);
    cxl_rdma_sidecar_drop_inflight_claims(ctx);
    g_free(ctx->queue);
    ctx->queue = NULL;
    ctx->queue_capacity = 0;
    g_free(ctx->inflight_claims);
    ctx->inflight_claims = NULL;
    ctx->inflight_capacity = 0;

    if (ctx->cm_id && ctx->connected) {
        rdma_disconnect(ctx->cm_id);
        ctx->connected = false;
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
        if (ctx->pin_all &&
            cxl_rdma_sidecar_register_source_pin_all(ctx, &local_err)) {
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

    if (!ctx->incoming) {
        cxl_rdma_sidecar_source_loop(ctx);
    } else {
        qemu_mutex_lock(&ctx->lock);
        while (!ctx->stop) {
            qemu_cond_timedwait(&ctx->cond, &ctx->lock, 100);
        }
        qemu_mutex_unlock(&ctx->lock);
    }
    return NULL;

fail:
    cxl_rdma_sidecar_mark_failed(ctx, local_err);
    cxl_rdma_sidecar_cleanup(ctx);
    return NULL;
}

static int cxl_rdma_sidecar_start_internal(
    const CXLHybridRDMASidecarConfig *cfg,
    bool wait_for_setup,
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
    ctx->queue_capacity = cfg->max_inflight_regions;
    ctx->queue = g_new0(CXLHybridRDMABulkClaim, ctx->queue_capacity);
    ctx->inflight_capacity = cfg->max_inflight_regions;
    ctx->inflight_claims =
        g_new0(CXLHybridRDMABulkClaim, ctx->inflight_capacity);
    if (cfg->ops) {
        ctx->ops = *cfg->ops;
    }

    if (cxl_rdma_sidecar_parse_addr(ctx, cfg->addr, errp)) {
        g_free(ctx->queue);
        g_free(ctx->inflight_claims);
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
    if (!wait_for_setup) {
        return 0;
    }

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

int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp)
{
    return cxl_rdma_sidecar_start_internal(cfg, true, errp);
}

bool cxl_rdma_sidecar_start_async(const CXLHybridRDMASidecarConfig *cfg,
                                  Error **errp)
{
    return cxl_rdma_sidecar_start_internal(cfg, false, errp) == 0;
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
    stats->page_state_rdma_completed_pages =
        qatomic_read(&ctx->stats.page_state_rdma_completed_pages);
    stats->page_state_rdma_completed_bytes =
        qatomic_read(&ctx->stats.page_state_rdma_completed_bytes);
    stats->page_state_rdma_completed_time_ns =
        qatomic_read(&ctx->stats.page_state_rdma_completed_time_ns);
    stats->page_state_rdma_stale_pages =
        qatomic_read(&ctx->stats.page_state_rdma_stale_pages);
}
#else
int cxl_rdma_sidecar_start(const CXLHybridRDMASidecarConfig *cfg,
                           Error **errp)
{
    error_setg(errp,
               "x-cxl-rdma-sidecar requires QEMU to be built with RDMA support");
    return -1;
}

bool cxl_rdma_sidecar_start_async(const CXLHybridRDMASidecarConfig *cfg,
                                  Error **errp)
{
    return cxl_rdma_sidecar_start(cfg, errp) == 0;
}

void cxl_rdma_sidecar_stop(void)
{
}

bool cxl_rdma_sidecar_running(void)
{
    return false;
}

bool cxl_rdma_sidecar_enqueue_bulk_claim(
    const CXLHybridRDMABulkClaim *claim)
{
    return false;
}

void cxl_rdma_sidecar_drain_bulk_claims(void)
{
}

void cxl_rdma_sidecar_get_stats(CXLHybridRDMASidecarBulkStats *stats)
{
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
}
#endif
