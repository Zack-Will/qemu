#include "qemu/osdep.h"
#include "anemoi/rdma.h"
#include "anemoi/constants.h"
#include "anemoi/pool.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/memalign.h"
#include "qemu/thread.h"

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>

#define ANEMOI_RDMA_WIRE_MAGIC 0x414e454d4f493031ULL
#define ANEMOI_RDMA_WIRE_VERSION 1U
#define ANEMOI_RDMA_CONNECT_RETRIES 200
#define ANEMOI_RDMA_CONNECT_RETRY_NS 10000000L
#define ANEMOI_RDMA_DEFAULT_CQ_DEPTH 256
#define ANEMOI_RDMA_DEFAULT_MAX_SEND_WR 256
#define ANEMOI_RDMA_DEFAULT_MAX_RECV_WR 1
#define ANEMOI_RDMA_DEFAULT_MAX_RD_ATOMIC 32

typedef struct AnemoiRDMAQPInfo {
    uint16_t lid;
    uint32_t qp_num;
    uint32_t psn;
    union ibv_gid gid;
} AnemoiRDMAQPInfo;

typedef struct AnemoiRDMAMemInfo {
    uintptr_t addr;
    uint64_t length;
    uint32_t rkey;
} AnemoiRDMAMemInfo;

typedef struct AnemoiRDMAPeerInfo {
    AnemoiRDMAQPInfo qp;
    AnemoiRDMAMemInfo mem;
} AnemoiRDMAPeerInfo;

typedef struct AnemoiRDMALinkConfig {
    const char *rdma_dev;
    uint8_t ib_port;
    int gid_idx;
    int cq_depth;
    int max_send_wr;
    int max_recv_wr;
    int max_rd_atomic;
    enum ibv_mtu mtu;
} AnemoiRDMALinkConfig;

typedef struct AnemoiRDMALink {
    struct ibv_context *verbs;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    void *mr_addr;
    size_t mr_len;
    AnemoiRDMAQPInfo local_qp;
    AnemoiRDMAMemInfo local_mem;
    uint8_t ib_port;
    int gid_idx;
    uint8_t max_rd_atomic;
    enum ibv_mtu mtu;
} AnemoiRDMALink;

typedef struct AnemoiRDMABackend {
    AnemoiRDMALink link;
    AnemoiRDMAMemInfo remote_mem;
    QemuMutex lock;
    void *staging;
    uint32_t vm_capacity;
    uint64_t pages_per_vm;
} AnemoiRDMABackend;

typedef struct AnemoiRDMAWireInfo {
    uint64_t magic;
    uint32_t version;
    uint16_t lid;
    uint32_t qp_num;
    uint32_t psn;
    uint8_t gid[16];
    uint64_t addr;
    uint64_t length;
    uint32_t rkey;
} QEMU_PACKED AnemoiRDMAWireInfo;

static volatile sig_atomic_t anemoi_rdma_pool_stop;

static void anemoi_rdma_pool_signal(int sig)
{
    (void)sig;
    anemoi_rdma_pool_stop = 1;
}

static uint32_t anemoi_rdma_make_psn(void)
{
    struct timespec ts;
    uint64_t x;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    x = (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 16) ^
        (uint64_t)ts.tv_sec;
    return (uint32_t)(x & 0xffffffU);
}

static void anemoi_rdma_link_config_defaults(AnemoiRDMALinkConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->rdma_dev = "mlx5_0";
    cfg->ib_port = ANEMOI_RDMA_DEFAULT_IB_PORT;
    cfg->gid_idx = ANEMOI_RDMA_DEFAULT_GID_IDX;
    cfg->cq_depth = ANEMOI_RDMA_DEFAULT_CQ_DEPTH;
    cfg->max_send_wr = ANEMOI_RDMA_DEFAULT_MAX_SEND_WR;
    cfg->max_recv_wr = ANEMOI_RDMA_DEFAULT_MAX_RECV_WR;
    cfg->max_rd_atomic = ANEMOI_RDMA_DEFAULT_MAX_RD_ATOMIC;
    cfg->mtu = IBV_MTU_4096;
}

static int anemoi_rdma_write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t ret = write(fd, p, len);

        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret <= 0) {
            return -1;
        }
        p += ret;
        len -= ret;
    }
    return 0;
}

static int anemoi_rdma_read_full(int fd, void *buf, size_t len)
{
    char *p = buf;

    while (len > 0) {
        ssize_t ret = read(fd, p, len);

        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret <= 0) {
            return -1;
        }
        p += ret;
        len -= ret;
    }
    return 0;
}

static int anemoi_rdma_connect_tcp(const char *host, uint16_t port,
                                   Error **errp)
{
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    g_autofree char *port_s = g_strdup_printf("%u", port);
    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = ANEMOI_RDMA_CONNECT_RETRY_NS,
    };
    struct addrinfo *res = NULL;
    int fd = -1;
    int ret;

    if (!host || !*host) {
        error_setg(errp, "Anemoi RDMA client requires a peer host");
        return -1;
    }

    ret = getaddrinfo(host, port_s, &hints, &res);
    if (ret != 0) {
        error_setg(errp, "failed to resolve Anemoi RDMA peer %s:%u: %s",
                   host, port, gai_strerror(ret));
        return -1;
    }

    for (int attempt = 0;
         attempt < ANEMOI_RDMA_CONNECT_RETRIES && fd < 0;
         attempt++) {
        for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
            fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
                break;
            }
            close(fd);
            fd = -1;
        }
        if (fd < 0) {
            nanosleep(&delay, NULL);
        }
    }
    freeaddrinfo(res);

    if (fd < 0) {
        error_setg_errno(errp, errno,
                         "failed to connect to Anemoi RDMA peer %s:%u",
                         host, port);
    }
    return fd;
}

static int anemoi_rdma_accept_tcp(uint16_t port, Error **errp)
{
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_PASSIVE,
    };
    g_autofree char *port_s = g_strdup_printf("%u", port);
    struct addrinfo *res = NULL;
    int listen_fd = -1;
    int fd = -1;
    int one = 1;
    int ret;

    ret = getaddrinfo(NULL, port_s, &hints, &res);
    if (ret != 0) {
        error_setg(errp, "failed to bind Anemoi RDMA TCP port %u: %s",
                   port, gai_strerror(ret));
        return -1;
    }

    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd < 0) {
            continue;
        }
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0 &&
            listen(listen_fd, 1) == 0) {
            fd = accept(listen_fd, NULL, NULL);
            close(listen_fd);
            break;
        }
        close(listen_fd);
        listen_fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        error_setg_errno(errp, errno,
                         "failed to accept Anemoi RDMA bootstrap on port %u",
                         port);
    }
    return fd;
}

static AnemoiRDMAWireInfo anemoi_rdma_to_wire(
    const AnemoiRDMAPeerInfo *info)
{
    AnemoiRDMAWireInfo wire = {0};

    wire.magic = cpu_to_be64(ANEMOI_RDMA_WIRE_MAGIC);
    wire.version = cpu_to_be32(ANEMOI_RDMA_WIRE_VERSION);
    wire.lid = cpu_to_be16(info->qp.lid);
    wire.qp_num = cpu_to_be32(info->qp.qp_num);
    wire.psn = cpu_to_be32(info->qp.psn);
    memcpy(wire.gid, info->qp.gid.raw, sizeof(wire.gid));
    wire.addr = cpu_to_be64((uint64_t)info->mem.addr);
    wire.length = cpu_to_be64(info->mem.length);
    wire.rkey = cpu_to_be32(info->mem.rkey);
    return wire;
}

static bool anemoi_rdma_from_wire(const AnemoiRDMAWireInfo *wire,
                                  AnemoiRDMAPeerInfo *info)
{
    if (be64_to_cpu(wire->magic) != ANEMOI_RDMA_WIRE_MAGIC ||
        be32_to_cpu(wire->version) != ANEMOI_RDMA_WIRE_VERSION) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->qp.lid = be16_to_cpu(wire->lid);
    info->qp.qp_num = be32_to_cpu(wire->qp_num);
    info->qp.psn = be32_to_cpu(wire->psn);
    memcpy(info->qp.gid.raw, wire->gid, sizeof(wire->gid));
    info->mem.addr = (uintptr_t)be64_to_cpu(wire->addr);
    info->mem.length = be64_to_cpu(wire->length);
    info->mem.rkey = be32_to_cpu(wire->rkey);
    return true;
}

static int anemoi_rdma_exchange(AnemoiRDMARole role, const char *peer_host,
                                uint16_t port,
                                const AnemoiRDMAPeerInfo *local,
                                AnemoiRDMAPeerInfo *remote, Error **errp)
{
    AnemoiRDMAWireInfo local_wire = anemoi_rdma_to_wire(local);
    AnemoiRDMAWireInfo remote_wire;
    int fd;
    int ret = -1;

    fd = role == ANEMOI_RDMA_ROLE_SERVER ?
         anemoi_rdma_accept_tcp(port, errp) :
         anemoi_rdma_connect_tcp(peer_host, port, errp);
    if (fd < 0) {
        return -1;
    }

    if (role == ANEMOI_RDMA_ROLE_SERVER) {
        if (anemoi_rdma_read_full(fd, &remote_wire, sizeof(remote_wire)) != 0 ||
            anemoi_rdma_write_full(fd, &local_wire, sizeof(local_wire)) != 0) {
            error_setg(errp, "failed Anemoi RDMA server bootstrap exchange");
            goto out;
        }
    } else {
        if (anemoi_rdma_write_full(fd, &local_wire, sizeof(local_wire)) != 0 ||
            anemoi_rdma_read_full(fd, &remote_wire, sizeof(remote_wire)) != 0) {
            error_setg(errp, "failed Anemoi RDMA client bootstrap exchange");
            goto out;
        }
    }

    if (!anemoi_rdma_from_wire(&remote_wire, remote)) {
        error_setg(errp, "invalid Anemoi RDMA bootstrap peer info");
        goto out;
    }
    ret = 0;

out:
    close(fd);
    return ret;
}

static struct ibv_device *anemoi_rdma_find_device(
    const char *name, struct ibv_device ***list_out)
{
    struct ibv_device **list;
    struct ibv_device *chosen = NULL;
    int ndev = 0;

    list = ibv_get_device_list(&ndev);
    if (!list || ndev <= 0) {
        return NULL;
    }

    for (int i = 0; i < ndev; i++) {
        if (!name || !*name ||
            strcmp(ibv_get_device_name(list[i]), name) == 0) {
            chosen = list[i];
            break;
        }
    }

    *list_out = list;
    return chosen;
}

static void anemoi_rdma_link_close(AnemoiRDMALink *link)
{
    if (!link) {
        return;
    }
    if (link->mr) {
        ibv_dereg_mr(link->mr);
    }
    if (link->qp) {
        ibv_destroy_qp(link->qp);
    }
    if (link->cq) {
        ibv_destroy_cq(link->cq);
    }
    if (link->pd) {
        ibv_dealloc_pd(link->pd);
    }
    if (link->verbs) {
        ibv_close_device(link->verbs);
    }
    memset(link, 0, sizeof(*link));
}

static int anemoi_rdma_modify_qp_init(AnemoiRDMALink *link)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = link->ib_port,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                           IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE,
    };
    int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
               IBV_QP_ACCESS_FLAGS;

    return ibv_modify_qp(link->qp, &attr, mask);
}

static int anemoi_rdma_link_init(AnemoiRDMALink *link,
                                 const AnemoiRDMALinkConfig *cfg,
                                 void *mr_addr, size_t mr_len, int mr_access,
                                 Error **errp)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_device *dev;
    struct ibv_port_attr port_attr;
    struct ibv_device_attr dev_attr;
    struct ibv_qp_init_attr qp_init;

    memset(link, 0, sizeof(*link));

    dev = anemoi_rdma_find_device(cfg->rdma_dev, &dev_list);
    if (!dev) {
        error_setg(errp, "Anemoi RDMA device not found: %s",
                   cfg->rdma_dev ? cfg->rdma_dev : "(first available)");
        if (dev_list) {
            ibv_free_device_list(dev_list);
        }
        return -1;
    }

    link->verbs = ibv_open_device(dev);
    ibv_free_device_list(dev_list);
    if (!link->verbs) {
        error_setg_errno(errp, errno, "failed to open Anemoi RDMA device");
        return -1;
    }

    link->ib_port = cfg->ib_port;
    link->gid_idx = cfg->gid_idx;
    link->mtu = cfg->mtu;
    link->max_rd_atomic = (uint8_t)cfg->max_rd_atomic;

    if (ibv_query_port(link->verbs, link->ib_port, &port_attr) != 0) {
        error_setg_errno(errp, errno, "failed to query Anemoi RDMA port");
        goto fail;
    }
    if (link->mtu > port_attr.active_mtu) {
        link->mtu = port_attr.active_mtu;
    }
    if (link->gid_idx >= 0 &&
        ibv_query_gid(link->verbs, link->ib_port, link->gid_idx,
                      &link->local_qp.gid) != 0) {
        error_setg_errno(errp, errno, "failed to query Anemoi RDMA GID");
        goto fail;
    }
    if (ibv_query_device(link->verbs, &dev_attr) != 0) {
        error_setg_errno(errp, errno, "failed to query Anemoi RDMA device");
        goto fail;
    }
    if (link->max_rd_atomic > dev_attr.max_qp_rd_atom) {
        link->max_rd_atomic = (uint8_t)dev_attr.max_qp_rd_atom;
    }
    if (link->max_rd_atomic > dev_attr.max_qp_init_rd_atom) {
        link->max_rd_atomic = (uint8_t)dev_attr.max_qp_init_rd_atom;
    }
    if (!link->max_rd_atomic) {
        error_setg(errp, "Anemoi RDMA device reports zero RDMA READ depth");
        goto fail;
    }

    link->pd = ibv_alloc_pd(link->verbs);
    if (!link->pd) {
        error_setg_errno(errp, errno, "failed to allocate Anemoi RDMA PD");
        goto fail;
    }
    link->cq = ibv_create_cq(link->verbs, cfg->cq_depth, NULL, NULL, 0);
    if (!link->cq) {
        error_setg_errno(errp, errno, "failed to create Anemoi RDMA CQ");
        goto fail;
    }

    memset(&qp_init, 0, sizeof(qp_init));
    qp_init.qp_type = IBV_QPT_RC;
    qp_init.send_cq = link->cq;
    qp_init.recv_cq = link->cq;
    qp_init.cap.max_send_wr = cfg->max_send_wr;
    qp_init.cap.max_recv_wr = cfg->max_recv_wr;
    qp_init.cap.max_send_sge = 1;
    qp_init.cap.max_recv_sge = 1;
    qp_init.sq_sig_all = 0;
    link->qp = ibv_create_qp(link->pd, &qp_init);
    if (!link->qp) {
        error_setg_errno(errp, errno, "failed to create Anemoi RDMA QP");
        goto fail;
    }
    if (anemoi_rdma_modify_qp_init(link) != 0) {
        error_setg_errno(errp, errno,
                         "failed to transition Anemoi RDMA QP to INIT");
        goto fail;
    }

    link->local_qp.lid = port_attr.lid;
    link->local_qp.qp_num = link->qp->qp_num;
    link->local_qp.psn = anemoi_rdma_make_psn();

    if (mr_addr && mr_len > 0) {
        link->mr = ibv_reg_mr(link->pd, mr_addr, mr_len, mr_access);
        if (!link->mr) {
            error_setg_errno(errp, errno, "failed to register Anemoi RDMA MR");
            goto fail;
        }
        link->mr_addr = mr_addr;
        link->mr_len = mr_len;
        link->local_mem.addr = (uintptr_t)mr_addr;
        link->local_mem.length = mr_len;
        link->local_mem.rkey = link->mr->rkey;
    }
    return 0;

fail:
    anemoi_rdma_link_close(link);
    return -1;
}

static AnemoiRDMAPeerInfo anemoi_rdma_local_peer_info(
    const AnemoiRDMALink *link)
{
    AnemoiRDMAPeerInfo info = {
        .qp = link->local_qp,
        .mem = link->local_mem,
    };

    return info;
}

static int anemoi_rdma_connect_qp(AnemoiRDMALink *link,
                                  const AnemoiRDMAQPInfo *remote_qp,
                                  Error **errp)
{
    struct ibv_qp_attr attr;
    int mask;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = link->mtu;
    attr.dest_qp_num = remote_qp->qp_num;
    attr.rq_psn = remote_qp->psn;
    attr.max_dest_rd_atomic = link->max_rd_atomic;
    attr.min_rnr_timer = 12;
    attr.ah_attr.dlid = remote_qp->lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = link->ib_port;
    attr.ah_attr.is_global = link->gid_idx >= 0;
    if (attr.ah_attr.is_global) {
        attr.ah_attr.grh.dgid = remote_qp->gid;
        attr.ah_attr.grh.sgid_index = link->gid_idx;
        attr.ah_attr.grh.hop_limit = 64;
    }
    mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
           IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
           IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(link->qp, &attr, mask) != 0) {
        error_setg_errno(errp, errno,
                         "failed to transition Anemoi RDMA QP to RTR");
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = link->local_qp.psn;
    attr.max_rd_atomic = link->max_rd_atomic;
    mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
           IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(link->qp, &attr, mask) != 0) {
        error_setg_errno(errp, errno,
                         "failed to transition Anemoi RDMA QP to RTS");
        return -1;
    }

    return 0;
}

static bool anemoi_rdma_translate(AnemoiRDMABackend *rdma,
                                  uint32_t vmid, uint64_t gfn,
                                  uint64_t *offset, Error **errp)
{
    uint64_t page_index;
    uint64_t byte_offset;

    if (vmid >= rdma->vm_capacity) {
        error_setg(errp, "Anemoi RDMA vmid %u exceeds capacity %u",
                   vmid, rdma->vm_capacity);
        return false;
    }
    if (gfn >= rdma->pages_per_vm) {
        error_setg(errp, "Anemoi RDMA gfn %" PRIu64
                   " exceeds pages_per_vm %" PRIu64,
                   gfn, rdma->pages_per_vm);
        return false;
    }
    page_index = anemoi_pool_page_offset(rdma->pages_per_vm, vmid, gfn);
    if (umul64_overflow(page_index, ANEMOI_PAGE_SIZE, &byte_offset) ||
        byte_offset > rdma->remote_mem.length ||
        ANEMOI_PAGE_SIZE > rdma->remote_mem.length - byte_offset) {
        error_setg(errp, "Anemoi RDMA page offset is outside remote MR");
        return false;
    }

    *offset = byte_offset;
    return true;
}

static int anemoi_rdma_poll_one(AnemoiRDMALink *link, uint64_t wr_id,
                                Error **errp)
{
    struct ibv_wc wc;

    for (;;) {
        int ret = ibv_poll_cq(link->cq, 1, &wc);

        if (ret < 0) {
            error_setg(errp, "Anemoi RDMA CQ poll failed");
            return -1;
        }
        if (ret == 0) {
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            error_setg(errp, "Anemoi RDMA CQE wr_id=%" PRIu64
                       " failed: %s", (uint64_t)wc.wr_id,
                       ibv_wc_status_str(wc.status));
            return -1;
        }
        if (wc.wr_id == wr_id) {
            return 0;
        }
    }
}

static int anemoi_rdma_post_page_op(AnemoiRDMABackend *rdma,
                                    enum ibv_wr_opcode opcode,
                                    uint64_t remote_offset,
                                    uint64_t wr_id, Error **errp)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)rdma->staging,
        .length = ANEMOI_PAGE_SIZE,
        .lkey = rdma->link.mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = wr_id,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = opcode,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr = rdma->remote_mem.addr + remote_offset,
        .wr.rdma.rkey = rdma->remote_mem.rkey,
    };
    struct ibv_send_wr *bad = NULL;

    if (ibv_post_send(rdma->link.qp, &wr, &bad) != 0) {
        error_setg_errno(errp, errno, "failed to post Anemoi RDMA work request");
        return -1;
    }
    return anemoi_rdma_poll_one(&rdma->link, wr_id, errp);
}

static int anemoi_rdma_fetch(AnemoiBackend *backend, uint32_t vmid,
                             uint64_t gfn, void *dst_4k, Error **errp)
{
    AnemoiRDMABackend *rdma = backend->opaque;
    uint64_t remote_offset;
    int ret;

    if (!anemoi_rdma_translate(rdma, vmid, gfn, &remote_offset, errp)) {
        return -1;
    }

    qemu_mutex_lock(&rdma->lock);
    ret = anemoi_rdma_post_page_op(rdma, IBV_WR_RDMA_READ, remote_offset,
                                   1, errp);
    if (ret == 0) {
        memcpy(dst_4k, rdma->staging, ANEMOI_PAGE_SIZE);
    }
    qemu_mutex_unlock(&rdma->lock);
    return ret;
}

static int anemoi_rdma_writeback(AnemoiBackend *backend, uint32_t vmid,
                                 uint64_t gfn, const void *src_4k,
                                 Error **errp)
{
    AnemoiRDMABackend *rdma = backend->opaque;
    uint64_t remote_offset;
    int ret;

    if (!anemoi_rdma_translate(rdma, vmid, gfn, &remote_offset, errp)) {
        return -1;
    }

    qemu_mutex_lock(&rdma->lock);
    memcpy(rdma->staging, src_4k, ANEMOI_PAGE_SIZE);
    ret = anemoi_rdma_post_page_op(rdma, IBV_WR_RDMA_WRITE, remote_offset,
                                   2, errp);
    qemu_mutex_unlock(&rdma->lock);
    return ret;
}

static void anemoi_rdma_shutdown(AnemoiBackend *backend)
{
    AnemoiRDMABackend *rdma;

    if (!backend) {
        return;
    }
    rdma = backend->opaque;
    if (rdma) {
        anemoi_rdma_link_close(&rdma->link);
        qemu_vfree(rdma->staging);
        qemu_mutex_destroy(&rdma->lock);
        g_free(rdma);
    }
    g_free(backend);
}

static const AnemoiBackendOps anemoi_rdma_ops = {
    .fetch = anemoi_rdma_fetch,
    .writeback = anemoi_rdma_writeback,
    .shutdown = anemoi_rdma_shutdown,
};

AnemoiBackend *anemoi_rdma_backend_new(const AnemoiRDMAConfig *cfg,
                                       Error **errp)
{
    AnemoiBackend *backend = NULL;
    AnemoiRDMABackend *rdma = NULL;
    AnemoiRDMALinkConfig link_cfg;
    AnemoiRDMAPeerInfo local_info;
    AnemoiRDMAPeerInfo remote_info;
    uint32_t max_send_wr;

    if (!cfg || !cfg->vm_capacity || !cfg->pages_per_vm) {
        error_setg(errp, "invalid Anemoi RDMA backend configuration");
        return NULL;
    }

    anemoi_rdma_link_config_defaults(&link_cfg);
    link_cfg.rdma_dev = cfg->rdma_dev ? cfg->rdma_dev : link_cfg.rdma_dev;
    link_cfg.ib_port = cfg->ib_port ? cfg->ib_port : link_cfg.ib_port;
    link_cfg.gid_idx = cfg->gid_idx;
    max_send_wr = MAX(ANEMOI_RDMA_DEFAULT_MAX_SEND_WR, 2U);
    link_cfg.max_send_wr = max_send_wr;
    link_cfg.cq_depth = max_send_wr + 8;

    rdma = g_new0(AnemoiRDMABackend, 1);
    qemu_mutex_init(&rdma->lock);
    rdma->vm_capacity = cfg->vm_capacity;
    rdma->pages_per_vm = cfg->pages_per_vm;
    rdma->staging = qemu_try_memalign(ANEMOI_PAGE_SIZE, ANEMOI_PAGE_SIZE);
    if (!rdma->staging) {
        error_setg(errp, "failed to allocate Anemoi RDMA staging page");
        goto fail;
    }
    memset(rdma->staging, 0, ANEMOI_PAGE_SIZE);

    if (anemoi_rdma_link_init(&rdma->link, &link_cfg, rdma->staging,
                              ANEMOI_PAGE_SIZE, IBV_ACCESS_LOCAL_WRITE,
                              errp) != 0) {
        goto fail;
    }

    local_info = anemoi_rdma_local_peer_info(&rdma->link);
    if (anemoi_rdma_exchange(cfg->role, cfg->peer_host, cfg->tcp_port,
                             &local_info, &remote_info, errp) != 0 ||
        anemoi_rdma_connect_qp(&rdma->link, &remote_info.qp, errp) != 0) {
        goto fail;
    }
    rdma->remote_mem = remote_info.mem;

    backend = g_new0(AnemoiBackend, 1);
    backend->name = "anemoi-rdma-pool";
    backend->kind = ANEMOI_BACKEND_RDMA;
    backend->page_size = ANEMOI_PAGE_SIZE;
    backend->ops = &anemoi_rdma_ops;
    backend->opaque = rdma;
    return backend;

fail:
    if (rdma) {
        anemoi_rdma_link_close(&rdma->link);
        qemu_vfree(rdma->staging);
        qemu_mutex_destroy(&rdma->lock);
        g_free(rdma);
    }
    return NULL;
}

int anemoi_rdma_pool_serve(const AnemoiRDMAPoolConfig *cfg, Error **errp)
{
    AnemoiRDMALinkConfig link_cfg;
    AnemoiRDMALink link;
    AnemoiRDMAPeerInfo local_info;
    AnemoiRDMAPeerInfo remote_info;
    void *pool = NULL;
    uint64_t total_pages;
    uint64_t total_bytes;
    int ret = -1;

    if (!cfg || !cfg->vm_capacity || !cfg->pages_per_vm) {
        error_setg(errp, "invalid Anemoi RDMA pool configuration");
        return -1;
    }
    if (umul64_overflow((uint64_t)cfg->vm_capacity, cfg->pages_per_vm,
                        &total_pages) ||
        umul64_overflow(total_pages, ANEMOI_PAGE_SIZE, &total_bytes) ||
        total_bytes > SIZE_MAX) {
        error_setg(errp, "Anemoi RDMA pool size overflows host size_t");
        return -1;
    }

    pool = qemu_try_memalign(ANEMOI_PAGE_SIZE, (size_t)total_bytes);
    if (!pool) {
        error_setg(errp, "failed to allocate %" PRIu64
                   " bytes for Anemoi RDMA pool", total_bytes);
        return -1;
    }
    memset(pool, 0, (size_t)total_bytes);

    anemoi_rdma_link_config_defaults(&link_cfg);
    link_cfg.rdma_dev = cfg->rdma_dev ? cfg->rdma_dev : link_cfg.rdma_dev;
    link_cfg.ib_port = cfg->ib_port ? cfg->ib_port : link_cfg.ib_port;
    link_cfg.gid_idx = cfg->gid_idx;

    if (anemoi_rdma_link_init(&link, &link_cfg, pool, (size_t)total_bytes,
                              IBV_ACCESS_LOCAL_WRITE |
                              IBV_ACCESS_REMOTE_READ |
                              IBV_ACCESS_REMOTE_WRITE, errp) != 0) {
        goto out_pool;
    }

    local_info = anemoi_rdma_local_peer_info(&link);
    if (anemoi_rdma_exchange(cfg->role, cfg->peer_host, cfg->tcp_port,
                             &local_info, &remote_info, errp) != 0 ||
        anemoi_rdma_connect_qp(&link, &remote_info.qp, errp) != 0) {
        goto out_link;
    }

    printf("anemoi-pool: vm-capacity=%u pages-per-vm=%" PRIu64
           " bytes=%" PRIu64 " port=%u\n",
           cfg->vm_capacity, cfg->pages_per_vm, total_bytes, cfg->tcp_port);
    printf("anemoi-pool: QP connected; press Ctrl-C to stop\n");
    fflush(stdout);

    anemoi_rdma_pool_stop = 0;
    signal(SIGINT, anemoi_rdma_pool_signal);
    signal(SIGTERM, anemoi_rdma_pool_signal);
    while (!anemoi_rdma_pool_stop) {
        sleep(3600);
    }
    ret = 0;

out_link:
    anemoi_rdma_link_close(&link);
out_pool:
    qemu_vfree(pool);
    return ret;
}
