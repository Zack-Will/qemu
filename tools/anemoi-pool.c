#include "qemu/osdep.h"
#include "anemoi/constants.h"
#include "anemoi/rdma.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"

enum {
    OPTION_ROLE = 256,
    OPTION_PEER,
    OPTION_PORT,
    OPTION_RDMA_DEV,
    OPTION_RDMA_PORT,
    OPTION_GID_IDX,
    OPTION_VM_CAPACITY,
    OPTION_PAGES_PER_VM,
    OPTION_VM_SIZE_MB,
};

static void anemoi_pool_usage(const char *argv0)
{
    printf("Usage: %s [OPTIONS]\n"
           "\n"
           "AnemoiM RDMA pool process for the Anemoi baseline.\n"
           "\n"
           "Options:\n"
           "  --role client|server       TCP bootstrap role (default: server)\n"
           "  --peer HOST                Peer host when role is client\n"
           "  --port PORT                TCP bootstrap port (default: 18515)\n"
           "  --rdma-dev DEV             RDMA device (default: mlx5_0)\n"
           "  --rdma-port PORT           RDMA HCA port (default: 1)\n"
           "  --gid-idx IDX              RoCE GID index (default: 3)\n"
           "  --vm-capacity N            Number of VM slots (default: 1)\n"
           "  --pages-per-vm N           4 KiB pages per VM slot\n"
           "  --vm-size-mb MB            Convenience form for --pages-per-vm\n"
           "  -h, --help                 Show this help\n",
           argv0);
}

static int parse_u64_opt(const char *name, const char *value, uint64_t *out)
{
    if (qemu_strtou64(value, NULL, 0, out) < 0) {
        error_report("invalid %s: %s", name, value);
        return -1;
    }
    return 0;
}

static int parse_u32_opt(const char *name, const char *value, uint32_t *out)
{
    uint64_t tmp;

    if (parse_uint_full(value, 0, &tmp) < 0 || tmp > UINT32_MAX) {
        error_report("invalid %s: %s", name, value);
        return -1;
    }
    *out = tmp;
    return 0;
}

static int parse_u16_opt(const char *name, const char *value, uint16_t *out)
{
    uint64_t tmp;

    if (parse_uint_full(value, 0, &tmp) < 0 || tmp > UINT16_MAX) {
        error_report("invalid %s: %s", name, value);
        return -1;
    }
    *out = tmp;
    return 0;
}

static int parse_u8_opt(const char *name, const char *value, uint8_t *out)
{
    uint64_t tmp;

    if (parse_uint_full(value, 0, &tmp) < 0 || tmp > UINT8_MAX) {
        error_report("invalid %s: %s", name, value);
        return -1;
    }
    *out = tmp;
    return 0;
}

static int parse_int_opt(const char *name, const char *value, int *out)
{
    int64_t tmp;

    if (qemu_strtoi64(value, NULL, 0, &tmp) < 0 ||
        tmp < INT_MIN || tmp > INT_MAX) {
        error_report("invalid %s: %s", name, value);
        return -1;
    }
    *out = tmp;
    return 0;
}

static int parse_role(const char *value, AnemoiRDMARole *role)
{
    if (!g_strcmp0(value, "server")) {
        *role = ANEMOI_RDMA_ROLE_SERVER;
        return 0;
    }
    if (!g_strcmp0(value, "client")) {
        *role = ANEMOI_RDMA_ROLE_CLIENT;
        return 0;
    }

    error_report("invalid role: %s", value);
    return -1;
}

int main(int argc, char **argv)
{
    static const struct option long_opts[] = {
        { "role", required_argument, NULL, OPTION_ROLE },
        { "peer", required_argument, NULL, OPTION_PEER },
        { "port", required_argument, NULL, OPTION_PORT },
        { "rdma-dev", required_argument, NULL, OPTION_RDMA_DEV },
        { "rdma-port", required_argument, NULL, OPTION_RDMA_PORT },
        { "gid-idx", required_argument, NULL, OPTION_GID_IDX },
        { "vm-capacity", required_argument, NULL, OPTION_VM_CAPACITY },
        { "pages-per-vm", required_argument, NULL, OPTION_PAGES_PER_VM },
        { "vm-size-mb", required_argument, NULL, OPTION_VM_SIZE_MB },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    AnemoiRDMAPoolConfig cfg = {
        .role = ANEMOI_RDMA_ROLE_SERVER,
        .tcp_port = ANEMOI_RDMA_DEFAULT_TCP_PORT,
        .ib_port = ANEMOI_RDMA_DEFAULT_IB_PORT,
        .gid_idx = ANEMOI_RDMA_DEFAULT_GID_IDX,
        .vm_capacity = 1,
    };
    Error *local_err = NULL;
    int opt;

    error_init(argv[0]);

    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case OPTION_ROLE:
            if (parse_role(optarg, &cfg.role) != 0) {
                return EXIT_FAILURE;
            }
            break;
        case OPTION_PEER:
            cfg.peer_host = optarg;
            break;
        case OPTION_PORT:
            if (parse_u16_opt("--port", optarg, &cfg.tcp_port) != 0) {
                return EXIT_FAILURE;
            }
            break;
        case OPTION_RDMA_DEV:
            cfg.rdma_dev = optarg;
            break;
        case OPTION_RDMA_PORT:
            if (parse_u8_opt("--rdma-port", optarg, &cfg.ib_port) != 0) {
                return EXIT_FAILURE;
            }
            break;
        case OPTION_GID_IDX:
            if (parse_int_opt("--gid-idx", optarg, &cfg.gid_idx) != 0) {
                return EXIT_FAILURE;
            }
            break;
        case OPTION_VM_CAPACITY:
            if (parse_u32_opt("--vm-capacity", optarg,
                              &cfg.vm_capacity) != 0) {
                return EXIT_FAILURE;
            }
            break;
        case OPTION_PAGES_PER_VM:
            if (parse_u64_opt("--pages-per-vm", optarg,
                              &cfg.pages_per_vm) != 0) {
                return EXIT_FAILURE;
            }
            break;
        case OPTION_VM_SIZE_MB:
        {
            uint64_t vm_size_mb;

            if (parse_u64_opt("--vm-size-mb", optarg, &vm_size_mb) != 0) {
                return EXIT_FAILURE;
            }
            cfg.pages_per_vm = vm_size_mb * MiB / ANEMOI_PAGE_SIZE;
            break;
        }
        case 'h':
            anemoi_pool_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            anemoi_pool_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind != argc) {
        error_report("unexpected positional argument: %s", argv[optind]);
        return EXIT_FAILURE;
    }
    if (!cfg.pages_per_vm) {
        error_report("--pages-per-vm or --vm-size-mb is required");
        return EXIT_FAILURE;
    }
    if (cfg.role == ANEMOI_RDMA_ROLE_CLIENT && !cfg.peer_host) {
        error_report("--peer is required when --role=client");
        return EXIT_FAILURE;
    }

    if (anemoi_rdma_pool_serve(&cfg, &local_err) != 0) {
        error_report_err(local_err);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
