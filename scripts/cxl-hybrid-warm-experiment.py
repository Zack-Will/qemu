#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

QEMU = Path("build/qemu-system-x86_64").resolve()
TRACE_EVENTS = Path("trace-events").resolve()
BOOT_ASM = Path("scripts/cxl-hybrid-warm-boot.asm").resolve()
PAGE_SIZE = 4096
MULTIFD_CHANNELS = 2
REMAP_GRANULE = 64 * 1024
QMP_TIMEOUT_SECS = 1.0
MAX_QMP_PROBE_FAILURES = 2
TRACE_TAIL_LINES = 80
STDERR_TAIL_LINES = 80
GDB_TIMEOUT_SECS = 5.0
SAMPLE_INTERVAL_SECS = 0.02
COMPLETION_TAIL_SAMPLES = 2
HEARTBEAT_SOCKET_TIMEOUT_SECS = 0.05
HEARTBEAT_JOIN_TIMEOUT_SECS = 1.0
HEARTBEAT_PORT = 0xE9
DEFAULT_MIGRATION_TIMEOUT_SECS = 60.0
POSTCOPY_START_RETRY_TIMEOUT_SECS = 0.5
FAULT_HIT_TRACE_RE = re.compile(
    r"\blen=(?P<len>\d+)\b.*\bread_time_ns=(?P<read_time_ns>\d+)\b"
)
FAULT_PLACE_TRACE_RE = re.compile(
    r"\bplace_time_ns=(?P<place_time_ns>\d+)\b"
)
PRESSURE_LEVELS = {
    "light": {
        "start_addr": 0x00020000,
        "end_addr": 0x00040000,
        "writes_per_page": 1,
        "outer_spin": 8192,
    },
    "medium": {
        "start_addr": 0x00020000,
        "end_addr": 0x00070000,
        "writes_per_page": 2,
        "outer_spin": 1,
    },
    "heavy": {
        "start_addr": 0x00020000,
        "end_addr": 0x000a0000,
        "writes_per_page": 8,
        "outer_spin": 0,
    },
    "remap_mid": {
        "start_addr": 0x00100000,
        "end_addr": 0x00300000,
        "writes_per_page": 8,
        "outer_spin": 0,
    },
    "remap_heavy": {
        "start_addr": 0x00100000,
        "end_addr": 0x00900000,
        "writes_per_page": 8,
        "outer_spin": 0,
    },
    "remap_heavy_random_rw": {
        "start_addr": 0x00100000,
        "end_addr": 0x00900000,
        "writes_per_page": 1,
        "outer_spin": 0,
        "page_order_random": True,
        "access_pattern_random_rw": True,
        "random_page_stride": 73,
        "random_epoch_seed_step": 1,
    },
}
PRESSURE_BATCH_PAGES = {
    "light": 32,
    "medium": 64,
    "heavy": 128,
    "remap_mid": 256,
    "remap_heavy": 512,
    "remap_heavy_random_rw": 512,
}
MIGRATION_MODES = (
    "hybrid_postcopy_payload",
    "hybrid_postcopy_auto",
    "hybrid_postcopy_cxl_offset",
    "hybrid_postcopy_manual",
    "native_postcopy_stream",
    "pure_precopy",
    "redirect_precopy",
)
DEFAULT_MIGRATION_MODE_CSV = "hybrid_postcopy_auto,redirect_precopy"
THRESHOLD_PROFILES = {
    "conservative": {
        "name": "conservative",
        "x-cxl-switch-dirty-threshold": 1,
        "x-cxl-switch-max-iters": 40,
        "x-cxl-switch-max-precopy-ms": 0,
        "x-cxl-switch-min-remaining": 4 * 1024 * 1024,
    },
    "balanced": {
        "name": "balanced",
        "x-cxl-switch-dirty-threshold": 1,
        "x-cxl-switch-max-iters": 20,
        "x-cxl-switch-max-precopy-ms": 0,
        "x-cxl-switch-min-remaining": 8 * 1024 * 1024,
    },
    "aggressive": {
        "name": "aggressive",
        "x-cxl-switch-dirty-threshold": 1,
        "x-cxl-switch-max-iters": 8,
        "x-cxl-switch-max-precopy-ms": 0,
        "x-cxl-switch-min-remaining": 16 * 1024 * 1024,
    },
}


def resolve_threshold_profile(name, dirty_threshold=None,
                              max_iters=None, max_precopy_ms=None,
                              min_remaining=None):
    profile = dict(THRESHOLD_PROFILES[name])
    if dirty_threshold is not None:
        profile["x-cxl-switch-dirty-threshold"] = dirty_threshold
    if max_iters is not None:
        profile["x-cxl-switch-max-iters"] = max_iters
    if max_precopy_ms is not None:
        profile["x-cxl-switch-max-precopy-ms"] = max_precopy_ms
    if min_remaining is not None:
        profile["x-cxl-switch-min-remaining"] = min_remaining
    profile["name"] = name if (
        dirty_threshold is None and max_iters is None and
        max_precopy_ms is None and min_remaining is None
    ) else "custom"
    return profile


def mode_uses_cxl_hybrid(mode: str) -> bool:
    return mode.startswith("hybrid_postcopy")


def mode_uses_postcopy(mode: str) -> bool:
    return mode_uses_cxl_hybrid(mode) or mode == "native_postcopy_stream"


def mode_uses_mapped_ram(mode: str) -> bool:
    return mode != "native_postcopy_stream"


def mode_uses_write_redirect(mode: str) -> bool:
    return mode_uses_cxl_hybrid(mode) or mode == "redirect_precopy"


def mode_uses_manual_postcopy_trigger(mode: str) -> bool:
    return mode in ("native_postcopy_stream", "hybrid_postcopy_manual")


def mode_requires_postcopy_warm(mode: str) -> bool:
    return mode_uses_cxl_hybrid(mode)


def postcopy_control_template(mode: str):
    if mode_uses_manual_postcopy_trigger(mode):
        request_mode = (
            "manual-hybrid"
            if mode_uses_cxl_hybrid(mode)
            else "manual-native"
        )
        requested = False
    elif mode_uses_cxl_hybrid(mode):
        request_mode = "auto-hybrid"
        requested = None
    else:
        request_mode = None
        requested = None
    return {
        "requested": requested,
        "request_mode": request_mode,
        "request_error": None,
    }


class ExperimentFailure(RuntimeError):
    def __init__(self, payload):
        super().__init__(payload["reason"])
        self.payload = payload


class QMPConnection:
    def __init__(self, sock, stream, path):
        self.sock = sock
        self.stream = stream
        self.path = path

    def close(self):
        try:
            self.stream.close()
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


class GuestHeartbeatCollector:
    def __init__(self, sockets):
        self.sockets = sockets
        self._events = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._threads = []

    def start(self):
        for side, sock in self.sockets.items():
            sock.settimeout(HEARTBEAT_SOCKET_TIMEOUT_SECS)
            thread = threading.Thread(target=self._reader,
                                      args=(side, sock),
                                      daemon=True)
            thread.start()
            self._threads.append(thread)

    def _reader(self, side, sock):
        while not self._stop.is_set():
            try:
                data = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break

            if not data:
                break

            ts = time.time()
            events = [{"ts": ts, "side": side} for _byte in data]
            with self._lock:
                self._events.extend(events)

    def snapshot(self):
        with self._lock:
            return sorted((dict(item) for item in self._events),
                          key=lambda item: item["ts"])

    def close(self):
        self._stop.set()
        for sock in self.sockets.values():
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass
        for thread in self._threads:
            thread.join(timeout=HEARTBEAT_JOIN_TIMEOUT_SECS)


def maybe_reexec_with_sudo():
    if os.geteuid() == 0:
        return
    if os.environ.get("QEMU_CXL_WARM_ALLOW_UNPRIVILEGED") == "1":
        return

    proc = subprocess.run(
        ["sysctl", "-n", "vm.unprivileged_userfaultfd"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode == 0 and proc.stdout.strip() == "1":
        return

    os.execvp("sudo", ["sudo", "-n", sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]])


def stop_all(procs):
    for proc in procs:
        if proc.poll() is None:
            proc.terminate()
    for proc in procs:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def fail(msg, procs):
    stop_all(procs)
    raise RuntimeError(msg)


def wait_sock(path, proc, procs, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return
        if proc.poll() is not None:
            fail(f"process exited before QMP socket appeared: {proc.args}", procs)
        time.sleep(0.05)
    fail(f"socket not created: {path}", procs)


def connect_stream_socket(path, timeout=10.0):
    deadline = time.time() + timeout
    last_error = None

    while time.time() < deadline:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(path)
            return sock
        except OSError as exc:
            last_error = exc
            sock.close()
            time.sleep(0.05)

    raise RuntimeError(f"failed to connect stream socket {path}: {last_error}")


def connect_qmp(path):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(QMP_TIMEOUT_SECS)
    sock.connect(path)
    stream = sock.makefile("rwb", buffering=0)
    conn = QMPConnection(sock, stream, path)
    banner = qmp_read_response(conn, "banner")
    assert "QMP" in banner, banner
    qmp(conn, "qmp_capabilities")
    return conn


def qmp_read_response(conn, cmd):
    while True:
        try:
            line = conn.stream.readline()
        except (socket.timeout, TimeoutError, OSError) as exc:
            raise TimeoutError(f"{cmd} timed out on {conn.path}") from exc
        if not line:
            raise EOFError(f"{cmd} hit EOF on {conn.path}")
        resp = json.loads(line)
        if "event" in resp:
            continue
        return resp


def qmp(conn, cmd, args=None):
    payload = {"execute": cmd}
    if args is not None:
        payload["arguments"] = args
    conn.stream.write(json.dumps(payload).encode() + b"\n")
    return qmp_read_response(conn, cmd)


def qmp_ok(conn, cmd, args=None):
    out = qmp(conn, cmd, args)
    if "return" not in out:
        raise AssertionError(out)
    return out["return"]


def qmp_probe(conn, cmd, args=None):
    try:
        return qmp_ok(conn, cmd, args), None
    except Exception as exc:
        return None, str(exc)


def request_native_postcopy_start(src_qmp):
    deadline = time.time() + POSTCOPY_START_RETRY_TIMEOUT_SECS
    last_error = None
    last_status = None

    while time.time() < deadline:
        try:
            qmp_ok(src_qmp, "migrate-start-postcopy")
            return
        except Exception as exc:
            last_error = str(exc)

        info, info_err = qmp_probe(src_qmp, "query-migrate")
        if info is not None:
            last_status = info.get("status")
            if last_status and last_status != "none":
                break
        elif info_err:
            last_error = info_err
        time.sleep(SAMPLE_INTERVAL_SECS)

    status_suffix = f" (status={last_status})" if last_status else ""
    raise RuntimeError(
        f"failed to start native postcopy: {last_error}{status_suffix}"
    )


def flush_trace_files(*conns):
    errors = {}

    for conn in conns:
        if conn is None:
            continue
        try:
            qmp_ok(conn, "trace-file-flush")
        except Exception as exc:
            errors[conn.path] = str(exc)

    return errors


def make_runtime_socket_paths():
    socket_dir = Path(tempfile.mkdtemp(prefix="cxhw-sock-"))
    return {
        "socket_dir": socket_dir,
        "src_qmp": socket_dir / "src.qmp",
        "dst_qmp": socket_dir / "dst.qmp",
        "mig_sock": socket_dir / "mig.sock",
        "src_heartbeat_sock": socket_dir / "src-hb.sock",
        "dst_heartbeat_sock": socket_dir / "dst-hb.sock",
    }


def build_boot_image(base: Path, pressure: str) -> Path:
    if pressure not in PRESSURE_LEVELS:
        raise RuntimeError(f"unsupported pressure level: {pressure}")

    config = PRESSURE_LEVELS[pressure]
    workset_pages = (
        (config["end_addr"] - config["start_addr"]) // PAGE_SIZE
    )
    random_page_order = bool(config.get("page_order_random", False))
    img = base / f"warm-boot-{pressure}.img"

    if random_page_order and (
        workset_pages <= 0 or (workset_pages & (workset_pages - 1)) != 0
    ):
        raise RuntimeError(
            f"random page order requires power-of-two workset pages: {pressure}"
        )

    nasm_args = [
        "nasm",
        "-D",
        f"PRESSURE_START_ADDR=0x{config['start_addr']:08x}",
        "-D",
        f"PRESSURE_END_ADDR=0x{config['end_addr']:08x}",
        "-D",
        f"PRESSURE_WRITES_PER_PAGE={config['writes_per_page']}",
        "-D",
        f"PRESSURE_OUTER_SPIN={config['outer_spin']}",
    ]
    if random_page_order:
        nasm_args.extend([
            "-D",
            "PRESSURE_PAGE_ORDER_RANDOM=1",
            "-D",
            "PRESSURE_RANDOM_PAGE_STRIDE="
            f"{config.get('random_page_stride', 73)}",
            "-D",
            "PRESSURE_RANDOM_EPOCH_SEED_STEP="
            f"{config.get('random_epoch_seed_step', 1)}",
        ])
    if config.get("access_pattern_random_rw", False):
        nasm_args.extend([
            "-D",
            "PRESSURE_ACCESS_PATTERN_RANDOM_RW=1",
        ])
    nasm_args.extend([
        "-f",
        "bin",
        "-o",
        str(img),
        str(BOOT_ASM),
    ])

    subprocess.run(
        nasm_args,
        check=True,
    )
    if img.stat().st_size != 512:
        raise RuntimeError(f"boot image size mismatch: {img.stat().st_size}")
    return img


def set_caps(f, mode: str):
    hybrid_mode = mode_uses_cxl_hybrid(mode)
    qmp_ok(f, "migrate-set-capabilities", {
        "capabilities": [
            {"capability": "mapped-ram",
             "state": mode_uses_mapped_ram(mode)},
            {"capability": "postcopy-ram",
             "state": mode_uses_postcopy(mode)},
            {"capability": "x-cxl-hybrid",
             "state": hybrid_mode},
            {"capability": "multifd", "state": True},
        ]
    })


def set_params(f, cxl_path: str, mode: str, pressure: str,
               shared_backing: bool = False, thresholds=None,
               prefetch_rate=None, dst_install_policy=None,
               fault_control_plane=None, fault_resolve_mode=None):
    hybrid_mode = mode_uses_cxl_hybrid(mode)
    params = {
        "max-bandwidth": 8 * 1024 * 1024,
        "multifd-channels": MULTIFD_CHANNELS,
    }
    if mode_uses_mapped_ram(mode):
        params["cxl-path"] = cxl_path
        params["x-cxl-brake-remap-granule"] = REMAP_GRANULE
    if hybrid_mode:
        if mode == "hybrid_postcopy_payload":
            raise ValueError("payload hybrid mode is disabled for CXL-only postcopy")
        profile = thresholds or resolve_threshold_profile("balanced")
        warm_transport = {
            "hybrid_postcopy_auto": "cxl-offset",
            "hybrid_postcopy_cxl_offset": "cxl-offset",
            "hybrid_postcopy_manual": "cxl-offset",
        }[mode]
        if not shared_backing:
            raise ValueError(
                f"{mode} requires explicit shared_backing confirmation"
            )
        params.update({
            "x-cxl-switch-dirty-threshold":
                profile["x-cxl-switch-dirty-threshold"],
            "x-cxl-switch-max-iters":
                profile["x-cxl-switch-max-iters"],
            "x-cxl-switch-max-precopy-ms":
                profile["x-cxl-switch-max-precopy-ms"],
            "x-cxl-switch-min-remaining":
                profile["x-cxl-switch-min-remaining"],
            "x-cxl-brake-enable": True,
            "x-cxl-prefetch-rate": (
                prefetch_rate
                if prefetch_rate is not None
                else 0
            ),
            "x-cxl-prefetch-batch-pages": PRESSURE_BATCH_PAGES[pressure],
            "x-cxl-prefetch-heat-window-ms": 250,
            "x-cxl-shared-backing": shared_backing,
            "x-cxl-warm-transport": warm_transport,
            "x-cxl-dst-install-policy": (
                dst_install_policy or "on-demand"
            ),
        })
        if fault_control_plane is not None:
            params["x-cxl-fault-control-plane"] = fault_control_plane
        if fault_resolve_mode is not None:
            params["x-cxl-fault-resolve-mode"] = fault_resolve_mode
    qmp_ok(f, "migrate-set-parameters", params)


def build_qemu_env(mode: str, is_source: bool):
    env = os.environ.copy()
    hybrid_mode = mode_uses_cxl_hybrid(mode)
    if not hybrid_mode:
        env["QEMU_CXL_HYBRID_WARM_DISABLE"] = "1"
    else:
        env.pop("QEMU_CXL_HYBRID_WARM_DISABLE", None)
    if is_source:
        env["QEMU_CXL_WRITE_REDIRECT"] = (
            "1" if mode_uses_write_redirect(mode) else "0"
        )
    else:
        env.pop("QEMU_CXL_WRITE_REDIRECT", None)
    return env


def build_common_args(boot_img: Path):
    return [
        str(QEMU),
        "-machine", "pc,accel=tcg",
        "-global", "apic-common.vapic=false",
        "-m", "64M",
        "-nodefaults",
        "-display", "none",
        "-no-reboot",
        "-S",
        "-drive", f"file={boot_img},format=raw,if=floppy",
        "-boot", "a",
    ]


def build_heartbeat_args(chardev_id: str, sock_path: Path):
    return [
        "-chardev",
        f"socket,id={chardev_id},path={sock_path},server=on,wait=off",
        "-device",
        f"isa-debugcon,iobase=0x{HEARTBEAT_PORT:x},chardev={chardev_id}",
    ]


def start_vm(common, qmp_sock, extra_args, stderr_path: Path, env=None):
    with open(stderr_path, "w", encoding="utf-8") as stderr:
        return subprocess.Popen(
            common + extra_args + ["-qmp", f"unix:{qmp_sock},server=on,wait=off"],
            stdout=subprocess.DEVNULL,
            stderr=stderr,
            text=True,
            env=env,
        )


def trace_count_template():
    return {
        "phase_postcopy_warm": 0,
        "warm_send": 0,
        "warm_recv": 0,
        "warm_desc_send": 0,
        "warm_desc_recv": 0,
        "warm_skip_received": 0,
        "warm_skip_unstaged": 0,
        "warm_queued": 0,
        "fault_hit": 0,
        "fault_hit_read_bytes": 0,
        "fault_hit_read_time_ns": 0,
        "max_fault_hit_read_time_ns": 0,
        "fault_place": 0,
        "fault_place_time_ns": 0,
        "max_fault_place_time_ns": 0,
        "fault_miss": 0,
        "publish_request_send": 0,
        "publish_request_recv": 0,
        "publish_ready_send": 0,
        "publish_ready_recv": 0,
        "publish_ready_queue": 0,
        "publish_ready_pop": 0,
        "publish_ready_drain_begin": 0,
        "publish_ready_drain_end": 0,
        "completion_prepare_begin": 0,
        "completion_prepare_end": 0,
        "publish_wait_begin": 0,
        "publish_wait_complete": 0,
        "postcopy_fault_request": 0,
        "postcopy_page_req_add": 0,
        "postcopy_page_req_del": 0,
        "postcopy_shared_request": 0,
        "postcopy_shared_request_present": 0,
        "ram_save_queue_pages": 0,
        "get_queued_page": 0,
        "get_queued_page_not_dirty": 0,
    }


def parse_trace_log(trace_file: Path):
    out = trace_file.read_text(encoding="utf-8", errors="replace").splitlines()
    counts = {
        **trace_count_template(),
    }
    for line in out:
        if "cxl_hybrid_phase_transition " in line and " new=4 " in line:
            counts["phase_postcopy_warm"] += 1
        elif "cxl_hybrid_warm_page_send " in line:
            counts["warm_send"] += 1
        elif "cxl_hybrid_warm_page_recv " in line:
            counts["warm_recv"] += 1
        elif "cxl_hybrid_warm_desc_send " in line:
            counts["warm_desc_send"] += 1
        elif "cxl_hybrid_warm_desc_recv " in line:
            counts["warm_desc_recv"] += 1
        elif "cxl_hybrid_warm_page_skip_received " in line:
            counts["warm_skip_received"] += 1
        elif "cxl_hybrid_warm_page_skip_unstaged " in line:
            counts["warm_skip_unstaged"] += 1
        elif "cxl_hybrid_warm_page_queued " in line:
            counts["warm_queued"] += 1
        elif "cxl_hybrid_fault_hit " in line:
            counts["fault_hit"] += 1
            match = FAULT_HIT_TRACE_RE.search(line)
            if match:
                read_bytes = int(match.group("len"))
                read_time_ns = int(match.group("read_time_ns"))
                counts["fault_hit_read_bytes"] += read_bytes
                counts["fault_hit_read_time_ns"] += read_time_ns
                counts["max_fault_hit_read_time_ns"] = max(
                    counts["max_fault_hit_read_time_ns"], read_time_ns)
        elif "cxl_hybrid_fault_place " in line:
            counts["fault_place"] += 1
            match = FAULT_PLACE_TRACE_RE.search(line)
            if match:
                place_time_ns = int(match.group("place_time_ns"))
                counts["fault_place_time_ns"] += place_time_ns
                counts["max_fault_place_time_ns"] = max(
                    counts["max_fault_place_time_ns"], place_time_ns)
        elif "cxl_hybrid_fault_miss " in line:
            counts["fault_miss"] += 1
        elif "cxl_hybrid_publish_request_send " in line:
            counts["publish_request_send"] += 1
        elif "cxl_hybrid_publish_request_recv " in line:
            counts["publish_request_recv"] += 1
        elif "cxl_hybrid_publish_ready_send " in line:
            counts["publish_ready_send"] += 1
        elif "cxl_hybrid_publish_ready_recv " in line:
            counts["publish_ready_recv"] += 1
        elif "cxl_hybrid_publish_ready_queue " in line:
            counts["publish_ready_queue"] += 1
        elif "cxl_hybrid_publish_ready_pop " in line:
            counts["publish_ready_pop"] += 1
        elif "cxl_hybrid_publish_ready_drain_begin " in line:
            counts["publish_ready_drain_begin"] += 1
        elif "cxl_hybrid_publish_ready_drain_end " in line:
            counts["publish_ready_drain_end"] += 1
        elif "cxl_hybrid_completion_prepare_begin " in line:
            counts["completion_prepare_begin"] += 1
        elif "cxl_hybrid_completion_prepare_end " in line:
            counts["completion_prepare_end"] += 1
        elif "cxl_hybrid_publish_wait_begin " in line:
            counts["publish_wait_begin"] += 1
        elif "cxl_hybrid_publish_wait_complete " in line:
            counts["publish_wait_complete"] += 1
        elif "postcopy_ram_fault_thread_request " in line:
            counts["postcopy_fault_request"] += 1
        elif "postcopy_page_req_add " in line:
            counts["postcopy_page_req_add"] += 1
        elif "postcopy_page_req_del " in line:
            counts["postcopy_page_req_del"] += 1
        elif "postcopy_request_shared_page_present " in line:
            counts["postcopy_shared_request_present"] += 1
        elif "postcopy_request_shared_page " in line:
            counts["postcopy_shared_request"] += 1
        elif "ram_save_queue_pages " in line:
            counts["ram_save_queue_pages"] += 1
        elif "get_queued_page_not_dirty " in line:
            counts["get_queued_page_not_dirty"] += 1
        elif "get_queued_page " in line:
            counts["get_queued_page"] += 1
    return counts


def query_status(conn):
    return qmp_ok(conn, "query-status")


def poll_migration_sample(src_qmp, dst_qmp):
    src_info, src_info_err = qmp_probe(src_qmp, "query-migrate")
    dst_info, dst_info_err = qmp_probe(dst_qmp, "query-migrate")
    src_status, src_status_err = qmp_probe(src_qmp, "query-status")
    dst_status, dst_status_err = qmp_probe(dst_qmp, "query-status")
    errors = {}

    if src_info_err:
        errors["src-query-migrate"] = src_info_err
    if dst_info_err:
        errors["dst-query-migrate"] = dst_info_err
    if src_status_err:
        errors["src-query-status"] = src_status_err
    if dst_status_err:
        errors["dst-query-status"] = dst_status_err

    return {
        "ts": time.time(),
        "status": src_info.get("status") if src_info else None,
        "x-cxl": (src_info or {}).get("x-cxl"),
        "ram": (src_info or {}).get("ram"),
        "src-query-migrate": src_info,
        "dst-query-migrate": dst_info,
        "src-query-status": src_status,
        "dst-query-status": dst_status,
        "errors": errors,
    }


def collect_until_complete(src_qmp, dst_qmp, samples_path: Path, timeout=60.0,
                           max_probe_failures=MAX_QMP_PROBE_FAILURES):
    deadline = time.time() + timeout
    completed_tail_seen = 0
    samples = []
    last = None
    last_src_status = None
    last_dst_status = None
    probe_failures = 0
    stop_reason = "timeout"
    while time.time() < deadline:
        sample = poll_migration_sample(src_qmp, dst_qmp)
        info = sample.get("src-query-migrate")

        samples.append(sample)
        write_json(samples_path, samples)

        if info is not None:
            last = info
        if sample.get("src-query-status") is not None:
            last_src_status = sample["src-query-status"]
        if sample.get("dst-query-status") is not None:
            last_dst_status = sample["dst-query-status"]

        if sample["errors"]:
            probe_failures += 1
        else:
            probe_failures = 0

        if info and info.get("status") in ("failed", "cancelled"):
            stop_reason = info.get("status")
            break
        if info and info.get("status") == "completed":
            stop_reason = "completed"
            completed_tail_seen += 1
            if completed_tail_seen > COMPLETION_TAIL_SAMPLES:
                break
        elif completed_tail_seen:
            break
        if probe_failures >= max_probe_failures:
            stop_reason = "qmp-unresponsive"
            break
        time.sleep(SAMPLE_INTERVAL_SECS)
    return last, samples, last_src_status, last_dst_status, stop_reason


def extract_summary(samples):
    def pick_latest_event(best, candidate):
        if not candidate:
            return best
        if candidate.get("count", 0) <= 0:
            return best
        if not best or candidate.get("count", 0) >= best.get("count", 0):
            return dict(candidate)
        return best

    max_present_slots = 0
    max_fault_hits = 0
    max_fault_misses = 0
    max_staged_pages = 0
    max_remap_attempts = 0
    max_remap_successes = 0
    max_remapped_regions = 0
    max_dst_stage_slots = 0
    max_dst_fault_read_bytes = 0
    max_dst_fault_read_time_ns = 0
    max_dst_fault_place_successes = 0
    max_dst_fault_place_failures = 0
    max_ram_postcopy_requests = 0
    max_dirty_pages_rate = 0
    max_warm_desc_sent_pages = 0
    max_warm_payload_fallback_pages = 0
    max_warm_desc_skip_unremapped = 0
    max_warm_publish_pages = 0
    max_fault_publish_requests = 0
    max_fault_publish_waits = 0
    max_fault_publish_wait_time_ns = 0
    fault_publish_primary_samples = 0
    fault_publish_primary_time_ns = 0
    max_fault_publish_primary_time_ns = 0
    fault_publish_burst_samples = 0
    fault_publish_burst_time_ns = 0
    max_fault_publish_burst_time_ns = 0
    fault_primary_ready_send_samples = 0
    fault_primary_ready_send_time_ns = 0
    max_fault_primary_ready_send_time_ns = 0
    fault_publish_req_recv_samples = 0
    fault_publish_req_recv_time_ns = 0
    max_fault_publish_req_recv_time_ns = 0
    fault_publish_req_handle_samples = 0
    fault_publish_req_handle_time_ns = 0
    max_fault_publish_req_handle_time_ns = 0
    fault_primary_ready_drain_samples = 0
    fault_primary_ready_drain_time_ns = 0
    max_fault_primary_ready_drain_time_ns = 0
    fault_primary_ready_write_samples = 0
    fault_primary_ready_write_time_ns = 0
    max_fault_primary_ready_write_time_ns = 0
    fault_primary_ready_recv_samples = 0
    fault_primary_ready_recv_time_ns = 0
    max_fault_primary_ready_recv_time_ns = 0
    fault_primary_ready_handle_samples = 0
    fault_primary_ready_handle_time_ns = 0
    max_fault_primary_ready_handle_time_ns = 0
    fault_wait_ready_recv_samples = 0
    fault_wait_ready_recv_time_ns = 0
    max_fault_wait_ready_recv_time_ns = 0
    fault_wait_after_ready_recv_samples = 0
    fault_wait_after_ready_recv_time_ns = 0
    max_fault_wait_after_ready_recv_time_ns = 0
    max_pending_publish_ready = 0
    max_completion_pending_publish_ready = 0
    last_publish_request = None
    last_publish_ready = None
    last_completion_publish_ready = None
    last_publish_ready_recv = None
    last_publish_wait_begin = None
    last_publish_wait_complete = None
    saw_postcopy_warm = False

    for sample in samples:
        src_xcxl = sample.get("x-cxl") or {}
        dst_xcxl = (sample.get("dst-query-migrate") or {}).get("x-cxl") or {}

        max_present_slots = max(max_present_slots,
                                src_xcxl.get("dst-stage-present-slots", 0),
                                dst_xcxl.get("dst-stage-present-slots", 0))
        max_dst_stage_slots = max(max_dst_stage_slots,
                                  src_xcxl.get("dst-stage-slots", 0),
                                  dst_xcxl.get("dst-stage-slots", 0))
        max_fault_hits = max(max_fault_hits,
                             src_xcxl.get("dst-fault-hits", 0),
                             dst_xcxl.get("dst-fault-hits", 0))
        max_fault_misses = max(max_fault_misses,
                               src_xcxl.get("dst-fault-misses", 0),
                               dst_xcxl.get("dst-fault-misses", 0))
        max_dst_fault_read_bytes = max(
            max_dst_fault_read_bytes,
            src_xcxl.get("dst-fault-read-bytes", 0),
            dst_xcxl.get("dst-fault-read-bytes", 0))
        max_dst_fault_read_time_ns = max(
            max_dst_fault_read_time_ns,
            src_xcxl.get("dst-fault-read-time-ns", 0),
            dst_xcxl.get("dst-fault-read-time-ns", 0))
        max_dst_fault_place_successes = max(
            max_dst_fault_place_successes,
            src_xcxl.get("dst-fault-place-successes", 0),
            dst_xcxl.get("dst-fault-place-successes", 0))
        max_dst_fault_place_failures = max(
            max_dst_fault_place_failures,
            src_xcxl.get("dst-fault-place-failures", 0),
            dst_xcxl.get("dst-fault-place-failures", 0))
        max_staged_pages = max(max_staged_pages, src_xcxl.get("staged-pages", 0))
        max_remap_attempts = max(max_remap_attempts,
                                 src_xcxl.get("remap-attempts", 0),
                                 dst_xcxl.get("remap-attempts", 0))
        max_remap_successes = max(max_remap_successes,
                                  src_xcxl.get("remap-successes", 0),
                                  dst_xcxl.get("remap-successes", 0))
        max_remapped_regions = max(max_remapped_regions,
                                   src_xcxl.get("remapped-regions", 0),
                                   dst_xcxl.get("remapped-regions", 0))
        ram = sample.get("ram") or {}
        max_ram_postcopy_requests = max(max_ram_postcopy_requests,
                                        ram.get("postcopy-requests", 0))
        max_dirty_pages_rate = max(max_dirty_pages_rate,
                                   ram.get("dirty-pages-rate", 0))
        max_warm_desc_sent_pages = max(max_warm_desc_sent_pages,
                                       src_xcxl.get("warm-desc-sent-pages", 0),
                                       dst_xcxl.get("warm-desc-sent-pages", 0))
        max_warm_payload_fallback_pages = max(
            max_warm_payload_fallback_pages,
            src_xcxl.get("warm-payload-fallback-pages", 0),
            dst_xcxl.get("warm-payload-fallback-pages", 0))
        max_warm_desc_skip_unremapped = max(
            max_warm_desc_skip_unremapped,
            src_xcxl.get("warm-desc-skip-unremapped", 0),
            dst_xcxl.get("warm-desc-skip-unremapped", 0))
        max_warm_publish_pages = max(
            max_warm_publish_pages,
            src_xcxl.get("warm-publish-pages", 0),
            dst_xcxl.get("warm-publish-pages", 0))
        max_fault_publish_requests = max(
            max_fault_publish_requests,
            src_xcxl.get("fault-publish-requests", 0),
            dst_xcxl.get("fault-publish-requests", 0))
        max_fault_publish_waits = max(
            max_fault_publish_waits,
            src_xcxl.get("fault-publish-waits", 0),
            dst_xcxl.get("fault-publish-waits", 0))
        max_fault_publish_wait_time_ns = max(
            max_fault_publish_wait_time_ns,
            src_xcxl.get("fault-publish-wait-time-ns", 0),
            dst_xcxl.get("fault-publish-wait-time-ns", 0))
        fault_publish_primary_samples = max(
            fault_publish_primary_samples,
            src_xcxl.get("fault-publish-primary-samples", 0),
            dst_xcxl.get("fault-publish-primary-samples", 0))
        fault_publish_primary_time_ns = max(
            fault_publish_primary_time_ns,
            src_xcxl.get("fault-publish-primary-time-ns", 0),
            dst_xcxl.get("fault-publish-primary-time-ns", 0))
        max_fault_publish_primary_time_ns = max(
            max_fault_publish_primary_time_ns,
            src_xcxl.get("max-fault-publish-primary-time-ns", 0),
            dst_xcxl.get("max-fault-publish-primary-time-ns", 0))
        fault_publish_burst_samples = max(
            fault_publish_burst_samples,
            src_xcxl.get("fault-publish-burst-samples", 0),
            dst_xcxl.get("fault-publish-burst-samples", 0))
        fault_publish_burst_time_ns = max(
            fault_publish_burst_time_ns,
            src_xcxl.get("fault-publish-burst-time-ns", 0),
            dst_xcxl.get("fault-publish-burst-time-ns", 0))
        max_fault_publish_burst_time_ns = max(
            max_fault_publish_burst_time_ns,
            src_xcxl.get("max-fault-publish-burst-time-ns", 0),
            dst_xcxl.get("max-fault-publish-burst-time-ns", 0))
        fault_primary_ready_send_samples = max(
            fault_primary_ready_send_samples,
            src_xcxl.get("fault-primary-ready-send-samples", 0),
            dst_xcxl.get("fault-primary-ready-send-samples", 0))
        fault_primary_ready_send_time_ns = max(
            fault_primary_ready_send_time_ns,
            src_xcxl.get("fault-primary-ready-send-time-ns", 0),
            dst_xcxl.get("fault-primary-ready-send-time-ns", 0))
        max_fault_primary_ready_send_time_ns = max(
            max_fault_primary_ready_send_time_ns,
            src_xcxl.get("max-fault-primary-ready-send-time-ns", 0),
            dst_xcxl.get("max-fault-primary-ready-send-time-ns", 0))
        fault_publish_req_recv_samples = max(
            fault_publish_req_recv_samples,
            src_xcxl.get("fault-publish-req-recv-samples", 0),
            dst_xcxl.get("fault-publish-req-recv-samples", 0))
        fault_publish_req_recv_time_ns = max(
            fault_publish_req_recv_time_ns,
            src_xcxl.get("fault-publish-req-recv-time-ns", 0),
            dst_xcxl.get("fault-publish-req-recv-time-ns", 0))
        max_fault_publish_req_recv_time_ns = max(
            max_fault_publish_req_recv_time_ns,
            src_xcxl.get("max-fault-publish-req-recv-time-ns", 0),
            dst_xcxl.get("max-fault-publish-req-recv-time-ns", 0))
        fault_publish_req_handle_samples = max(
            fault_publish_req_handle_samples,
            src_xcxl.get("fault-publish-req-handle-samples", 0),
            dst_xcxl.get("fault-publish-req-handle-samples", 0))
        fault_publish_req_handle_time_ns = max(
            fault_publish_req_handle_time_ns,
            src_xcxl.get("fault-publish-req-handle-time-ns", 0),
            dst_xcxl.get("fault-publish-req-handle-time-ns", 0))
        max_fault_publish_req_handle_time_ns = max(
            max_fault_publish_req_handle_time_ns,
            src_xcxl.get("max-fault-publish-req-handle-time-ns", 0),
            dst_xcxl.get("max-fault-publish-req-handle-time-ns", 0))
        fault_primary_ready_drain_samples = max(
            fault_primary_ready_drain_samples,
            src_xcxl.get("fault-primary-ready-drain-samples", 0),
            dst_xcxl.get("fault-primary-ready-drain-samples", 0))
        fault_primary_ready_drain_time_ns = max(
            fault_primary_ready_drain_time_ns,
            src_xcxl.get("fault-primary-ready-drain-time-ns", 0),
            dst_xcxl.get("fault-primary-ready-drain-time-ns", 0))
        max_fault_primary_ready_drain_time_ns = max(
            max_fault_primary_ready_drain_time_ns,
            src_xcxl.get("max-fault-primary-ready-drain-time-ns", 0),
            dst_xcxl.get("max-fault-primary-ready-drain-time-ns", 0))
        fault_primary_ready_write_samples = max(
            fault_primary_ready_write_samples,
            src_xcxl.get("fault-primary-ready-write-samples", 0),
            dst_xcxl.get("fault-primary-ready-write-samples", 0))
        fault_primary_ready_write_time_ns = max(
            fault_primary_ready_write_time_ns,
            src_xcxl.get("fault-primary-ready-write-time-ns", 0),
            dst_xcxl.get("fault-primary-ready-write-time-ns", 0))
        max_fault_primary_ready_write_time_ns = max(
            max_fault_primary_ready_write_time_ns,
            src_xcxl.get("max-fault-primary-ready-write-time-ns", 0),
            dst_xcxl.get("max-fault-primary-ready-write-time-ns", 0))
        fault_primary_ready_recv_samples = max(
            fault_primary_ready_recv_samples,
            src_xcxl.get("fault-primary-ready-recv-samples", 0),
            dst_xcxl.get("fault-primary-ready-recv-samples", 0))
        fault_primary_ready_recv_time_ns = max(
            fault_primary_ready_recv_time_ns,
            src_xcxl.get("fault-primary-ready-recv-time-ns", 0),
            dst_xcxl.get("fault-primary-ready-recv-time-ns", 0))
        max_fault_primary_ready_recv_time_ns = max(
            max_fault_primary_ready_recv_time_ns,
            src_xcxl.get("max-fault-primary-ready-recv-time-ns", 0),
            dst_xcxl.get("max-fault-primary-ready-recv-time-ns", 0))
        fault_primary_ready_handle_samples = max(
            fault_primary_ready_handle_samples,
            src_xcxl.get("fault-primary-ready-handle-samples", 0),
            dst_xcxl.get("fault-primary-ready-handle-samples", 0))
        fault_primary_ready_handle_time_ns = max(
            fault_primary_ready_handle_time_ns,
            src_xcxl.get("fault-primary-ready-handle-time-ns", 0),
            dst_xcxl.get("fault-primary-ready-handle-time-ns", 0))
        max_fault_primary_ready_handle_time_ns = max(
            max_fault_primary_ready_handle_time_ns,
            src_xcxl.get("max-fault-primary-ready-handle-time-ns", 0),
            dst_xcxl.get("max-fault-primary-ready-handle-time-ns", 0))
        fault_wait_ready_recv_samples = max(
            fault_wait_ready_recv_samples,
            src_xcxl.get("fault-wait-ready-recv-samples", 0),
            dst_xcxl.get("fault-wait-ready-recv-samples", 0))
        fault_wait_ready_recv_time_ns = max(
            fault_wait_ready_recv_time_ns,
            src_xcxl.get("fault-wait-ready-recv-time-ns", 0),
            dst_xcxl.get("fault-wait-ready-recv-time-ns", 0))
        max_fault_wait_ready_recv_time_ns = max(
            max_fault_wait_ready_recv_time_ns,
            src_xcxl.get("max-fault-wait-ready-recv-time-ns", 0),
            dst_xcxl.get("max-fault-wait-ready-recv-time-ns", 0))
        fault_wait_after_ready_recv_samples = max(
            fault_wait_after_ready_recv_samples,
            src_xcxl.get("fault-wait-after-ready-recv-samples", 0),
            dst_xcxl.get("fault-wait-after-ready-recv-samples", 0))
        fault_wait_after_ready_recv_time_ns = max(
            fault_wait_after_ready_recv_time_ns,
            src_xcxl.get("fault-wait-after-ready-recv-time-ns", 0),
            dst_xcxl.get("fault-wait-after-ready-recv-time-ns", 0))
        max_fault_wait_after_ready_recv_time_ns = max(
            max_fault_wait_after_ready_recv_time_ns,
            src_xcxl.get("max-fault-wait-after-ready-recv-time-ns", 0),
            dst_xcxl.get("max-fault-wait-after-ready-recv-time-ns", 0))
        max_pending_publish_ready = max(
            max_pending_publish_ready,
            src_xcxl.get("pending-publish-ready", 0),
            dst_xcxl.get("pending-publish-ready", 0))
        max_completion_pending_publish_ready = max(
            max_completion_pending_publish_ready,
            src_xcxl.get("completion-pending-publish-ready", 0),
            dst_xcxl.get("completion-pending-publish-ready", 0))
        last_publish_request = pick_latest_event(
            last_publish_request,
            src_xcxl.get("last-publish-request"))
        last_publish_request = pick_latest_event(
            last_publish_request,
            dst_xcxl.get("last-publish-request"))
        last_publish_ready = pick_latest_event(
            last_publish_ready,
            src_xcxl.get("last-publish-ready"))
        last_publish_ready = pick_latest_event(
            last_publish_ready,
            dst_xcxl.get("last-publish-ready"))
        last_completion_publish_ready = pick_latest_event(
            last_completion_publish_ready,
            src_xcxl.get("last-completion-publish-ready"))
        last_completion_publish_ready = pick_latest_event(
            last_completion_publish_ready,
            dst_xcxl.get("last-completion-publish-ready"))
        last_publish_ready_recv = pick_latest_event(
            last_publish_ready_recv,
            src_xcxl.get("last-publish-ready-recv"))
        last_publish_ready_recv = pick_latest_event(
            last_publish_ready_recv,
            dst_xcxl.get("last-publish-ready-recv"))
        last_publish_wait_begin = pick_latest_event(
            last_publish_wait_begin,
            src_xcxl.get("last-publish-wait-begin"))
        last_publish_wait_begin = pick_latest_event(
            last_publish_wait_begin,
            dst_xcxl.get("last-publish-wait-begin"))
        last_publish_wait_complete = pick_latest_event(
            last_publish_wait_complete,
            src_xcxl.get("last-publish-wait-complete"))
        last_publish_wait_complete = pick_latest_event(
            last_publish_wait_complete,
            dst_xcxl.get("last-publish-wait-complete"))
        saw_postcopy_warm = saw_postcopy_warm or \
            src_xcxl.get("phase") == "postcopy-warm"

    return {
        "max_dst_stage_slots": max_dst_stage_slots,
        "max_dst_stage_present_slots": max_present_slots,
        "max_dst_fault_hits": max_fault_hits,
        "max_dst_fault_misses": max_fault_misses,
        "max_dst_fault_read_bytes": max_dst_fault_read_bytes,
        "max_dst_fault_read_time_ns": max_dst_fault_read_time_ns,
        "max_dst_fault_place_successes": max_dst_fault_place_successes,
        "max_dst_fault_place_failures": max_dst_fault_place_failures,
        "max_staged_pages": max_staged_pages,
        "max_remap_attempts": max_remap_attempts,
        "max_remap_successes": max_remap_successes,
        "max_remapped_regions": max_remapped_regions,
        "max_ram_postcopy_requests": max_ram_postcopy_requests,
        "max_dirty_pages_rate": max_dirty_pages_rate,
        "max_warm_desc_sent_pages": max_warm_desc_sent_pages,
        "max_warm_payload_fallback_pages": max_warm_payload_fallback_pages,
        "max_warm_desc_skip_unremapped": max_warm_desc_skip_unremapped,
        "max_warm_publish_pages": max_warm_publish_pages,
        "max_fault_publish_requests": max_fault_publish_requests,
        "max_fault_publish_waits": max_fault_publish_waits,
        "max_fault_publish_wait_time_ns": max_fault_publish_wait_time_ns,
        "fault_publish_primary_samples": fault_publish_primary_samples,
        "fault_publish_primary_time_ns": fault_publish_primary_time_ns,
        "max_fault_publish_primary_time_ns": max_fault_publish_primary_time_ns,
        "fault_publish_burst_samples": fault_publish_burst_samples,
        "fault_publish_burst_time_ns": fault_publish_burst_time_ns,
        "max_fault_publish_burst_time_ns": max_fault_publish_burst_time_ns,
        "fault_primary_ready_send_samples": fault_primary_ready_send_samples,
        "fault_primary_ready_send_time_ns": fault_primary_ready_send_time_ns,
        "max_fault_primary_ready_send_time_ns": max_fault_primary_ready_send_time_ns,
        "fault_publish_req_recv_samples": fault_publish_req_recv_samples,
        "fault_publish_req_recv_time_ns": fault_publish_req_recv_time_ns,
        "max_fault_publish_req_recv_time_ns": max_fault_publish_req_recv_time_ns,
        "fault_publish_req_handle_samples": fault_publish_req_handle_samples,
        "fault_publish_req_handle_time_ns": fault_publish_req_handle_time_ns,
        "max_fault_publish_req_handle_time_ns": max_fault_publish_req_handle_time_ns,
        "fault_primary_ready_drain_samples": fault_primary_ready_drain_samples,
        "fault_primary_ready_drain_time_ns": fault_primary_ready_drain_time_ns,
        "max_fault_primary_ready_drain_time_ns": max_fault_primary_ready_drain_time_ns,
        "fault_primary_ready_write_samples": fault_primary_ready_write_samples,
        "fault_primary_ready_write_time_ns": fault_primary_ready_write_time_ns,
        "max_fault_primary_ready_write_time_ns": max_fault_primary_ready_write_time_ns,
        "fault_primary_ready_recv_samples": fault_primary_ready_recv_samples,
        "fault_primary_ready_recv_time_ns": fault_primary_ready_recv_time_ns,
        "max_fault_primary_ready_recv_time_ns": max_fault_primary_ready_recv_time_ns,
        "fault_primary_ready_handle_samples": fault_primary_ready_handle_samples,
        "fault_primary_ready_handle_time_ns": fault_primary_ready_handle_time_ns,
        "max_fault_primary_ready_handle_time_ns": max_fault_primary_ready_handle_time_ns,
        "fault_wait_ready_recv_samples": fault_wait_ready_recv_samples,
        "fault_wait_ready_recv_time_ns": fault_wait_ready_recv_time_ns,
        "max_fault_wait_ready_recv_time_ns": max_fault_wait_ready_recv_time_ns,
        "fault_wait_after_ready_recv_samples": fault_wait_after_ready_recv_samples,
        "fault_wait_after_ready_recv_time_ns": fault_wait_after_ready_recv_time_ns,
        "max_fault_wait_after_ready_recv_time_ns": max_fault_wait_after_ready_recv_time_ns,
        "max_pending_publish_ready": max_pending_publish_ready,
        "max_completion_pending_publish_ready":
            max_completion_pending_publish_ready,
        "last_publish_request": last_publish_request,
        "last_publish_ready": last_publish_ready,
        "last_completion_publish_ready": last_completion_publish_ready,
        "last_publish_ready_recv": last_publish_ready_recv,
        "last_publish_wait_begin": last_publish_wait_begin,
        "last_publish_wait_complete": last_publish_wait_complete,
        "saw_postcopy_warm": saw_postcopy_warm,
    }


def trace_saw_postcopy_warm(trace_counts):
    if not trace_counts:
        return False
    combined = trace_counts.get("combined") or {}
    return combined.get("phase_postcopy_warm", 0) > 0


def write_json(path: Path, payload):
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def read_tail(path: Path, max_lines: int):
    if not path.exists():
        return []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return lines[-max_lines:]


def run_capture(cmd, timeout=None):
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        check=False,
        timeout=timeout,
    )
    return {
        "argv": cmd,
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
    }


def write_backtrace(proc, path: Path):
    if proc.poll() is not None:
        return None
    try:
        result = run_capture(
            [
                "gdb",
                "-batch",
                "-ex",
                "thread apply all bt",
                "-p",
                str(proc.pid),
            ],
            timeout=GDB_TIMEOUT_SECS,
        )
    except subprocess.TimeoutExpired:
        path.write_text("gdb backtrace timed out\n", encoding="utf-8")
        return str(path)

    path.write_text(
        (result["stdout"] or "") + (result["stderr"] or ""),
        encoding="utf-8",
    )
    return str(path)


def collect_failure_diagnostics(case_dir: Path, procs, stderr_paths, trace_paths,
                                qmp_sockets=None):
    pids = [str(proc.pid) for proc in procs.values()]
    payload = {
        "timestamp": time.time(),
        "processes": {
            label: {
                "pid": proc.pid,
                "returncode": proc.poll(),
            }
            for label, proc in procs.items()
        },
        "ps": run_capture(
            [
                "ps",
                "-o",
                "pid,ppid,stat,%cpu,%mem,etime,wchan,cmd",
                "-p",
                ",".join(pids),
            ]
        ) if pids else None,
        "stderr_tail": {
            label: read_tail(path, STDERR_TAIL_LINES)
            for label, path in stderr_paths.items()
        },
        "trace_tail": {
            label: read_tail(path, TRACE_TAIL_LINES)
            for label, path in trace_paths.items()
        },
        "qmp_sockets": {
            label: str((qmp_sockets or {}).get(label, case_dir / f"{label}.qmp"))
            for label in procs
        },
        "gdb_backtraces": {},
    }

    for label, proc in procs.items():
        backtrace_path = case_dir / f"{label}-gdb.txt"
        saved = write_backtrace(proc, backtrace_path)
        if saved:
            payload["gdb_backtraces"][label] = saved

    write_json(case_dir / "diagnostics.json", payload)
    return payload


def first_postcopy_warm_state(samples):
    for sample in samples:
        xcxl = sample.get("x-cxl") or {}
        if xcxl.get("phase") == "postcopy-warm":
            return sample.get("src-query-migrate")
    return None


def first_status_ts(samples, status):
    for sample in samples:
        info = sample.get("src-query-migrate") or {}
        if info.get("status") == status:
            return sample.get("ts")
    return None


def observed_migration_window_ms(migration_start_ts, migration_complete_ts):
    if migration_start_ts is None or migration_complete_ts is None:
        return None
    return max((migration_complete_ts - migration_start_ts) * 1000.0, 0.0)


def summarize_guest_heartbeats(events, migration_start_ts=None,
                               migration_complete_ts=None):
    ordered = sorted(
        (
            {"ts": item["ts"], "side": item["side"]}
            for item in events
            if "ts" in item and "side" in item
        ),
        key=lambda item: item["ts"],
    )

    same_side_gaps_ms = []
    gaps = []
    src_events = [item for item in ordered if item["side"] == "src"]
    dst_events = [item for item in ordered if item["side"] == "dst"]

    for prev, curr in zip(ordered, ordered[1:]):
        gap_ms = max((curr["ts"] - prev["ts"]) * 1000.0, 0.0)
        gaps.append({
            "start_ts": prev["ts"],
            "end_ts": curr["ts"],
            "gap_ms": gap_ms,
            "start_side": prev["side"],
            "end_side": curr["side"],
        })
        if prev["side"] == curr["side"]:
            same_side_gaps_ms.append(gap_ms)

    baseline_gap_ms = (
        statistics.median(same_side_gaps_ms)
        if same_side_gaps_ms else None
    )

    handoff_gap_ms = None
    if src_events and dst_events:
        last_src_ts = src_events[-1]["ts"]
        for item in dst_events:
            if item["ts"] >= last_src_ts:
                handoff_gap_ms = max((item["ts"] - last_src_ts) * 1000.0, 0.0)
                break

    max_gap_ms = max((item["gap_ms"] for item in gaps), default=None)
    max_gap_during_migration_ms = None
    total_gap_during_migration_ms = None
    total_stall_during_migration_ms = None
    if migration_start_ts is not None and migration_complete_ts is not None:
        overlapping = [
            item["gap_ms"] for item in gaps
            if item["start_ts"] <= migration_complete_ts and
            item["end_ts"] >= migration_start_ts
        ]
        if overlapping:
            max_gap_during_migration_ms = max(overlapping)
        total_gap_during_migration_ms = 0.0
        total_stall_during_migration_ms = 0.0
        for item in gaps:
            if item["start_ts"] > migration_complete_ts or \
               item["end_ts"] < migration_start_ts:
                continue
            overlap_start_ts = max(item["start_ts"], migration_start_ts)
            overlap_end_ts = min(item["end_ts"], migration_complete_ts)
            overlap_gap_ms = max((overlap_end_ts - overlap_start_ts) * 1000.0,
                                 0.0)
            total_gap_during_migration_ms += overlap_gap_ms
            if baseline_gap_ms is not None:
                total_stall_during_migration_ms += max(
                    overlap_gap_ms - baseline_gap_ms, 0.0
                )

    def stall_ms(gap_ms):
        if gap_ms is None or baseline_gap_ms is None:
            return None
        return max(gap_ms - baseline_gap_ms, 0.0)

    return {
        "events_total": len(ordered),
        "events_src": len(src_events),
        "events_dst": len(dst_events),
        "baseline_gap_ms": baseline_gap_ms,
        "handoff_gap_ms": handoff_gap_ms,
        "handoff_stall_ms": stall_ms(handoff_gap_ms),
        "max_gap_ms": max_gap_ms,
        "max_gap_during_migration_ms": max_gap_during_migration_ms,
        "total_gap_during_migration_ms": total_gap_during_migration_ms,
        "total_stall_during_migration_ms": total_stall_during_migration_ms,
        "max_gap_stall_ms": stall_ms(max_gap_during_migration_ms or max_gap_ms),
    }


def build_failure(label, reason, started, last, samples, case_dir,
                  trace_counts=None, src_status=None, dst_status=None,
                  stop_reason=None, procs=None, stderr_paths=None,
                  trace_paths=None, heartbeats_path=None,
                  guest_latency=None, qmp_sockets=None,
                  postcopy_control=None):
    diagnostics = None
    if procs and stderr_paths and trace_paths:
        diagnostics = collect_failure_diagnostics(case_dir, procs,
                                                  stderr_paths, trace_paths,
                                                  qmp_sockets=qmp_sockets)

    payload = {
        "case": label,
        "reason": reason,
        "stop_reason": stop_reason,
        "started": started,
        "last": last,
        "src_status": src_status,
        "dst_status": dst_status,
        "summary": extract_summary(samples),
        "trace": trace_counts,
        "samples_path": str(case_dir / "samples.json"),
        "src_trace_path": str(case_dir / "src-trace.bin"),
        "dst_trace_path": str(case_dir / "dst-trace.bin"),
        "case_dir": str(case_dir),
        "heartbeats_path": str(heartbeats_path) if heartbeats_path else None,
        "guest_latency": guest_latency,
        "postcopy_control": postcopy_control,
        "diagnostics_path": str(case_dir / "diagnostics.json")
        if diagnostics is not None else None,
    }
    write_json(case_dir / "samples.json", samples)
    if trace_counts is not None:
        write_json(case_dir / "trace-summary.json", trace_counts)
    raise ExperimentFailure(payload)


def run_case(base: Path, mode: str, pressure: str,
             threshold_profile=None, run_index=1,
             migration_timeout=DEFAULT_MIGRATION_TIMEOUT_SECS,
             prefetch_rate=None, dst_install_policy=None,
             fault_control_plane=None, fault_resolve_mode=None,
             cxl_path_override=None):
    profile = threshold_profile or resolve_threshold_profile("balanced")
    if mode_uses_cxl_hybrid(mode):
        case_name = f"{mode}-{profile['name']}-run{run_index:02d}"
    else:
        case_name = f"{mode}-run{run_index:02d}"
    case_root = base if base.name == pressure else (base / pressure)
    case_dir = case_root / case_name
    case_dir.mkdir(parents=True, exist_ok=True)
    runtime_paths = make_runtime_socket_paths()
    socket_dir = runtime_paths["socket_dir"]
    src_sock = runtime_paths["src_qmp"]
    dst_sock = runtime_paths["dst_qmp"]
    mig_sock = runtime_paths["mig_sock"]
    cxl_backing = case_dir / "cxl-backing.img"
    cxl_backing_path = None
    if mode_uses_mapped_ram(mode):
        cxl_backing_path = cxl_path_override or str(cxl_backing)
    src_trace_file = case_dir / "src-trace.bin"
    dst_trace_file = case_dir / "dst-trace.bin"
    src_stderr_file = case_dir / "src.stderr"
    dst_stderr_file = case_dir / "dst.stderr"
    samples_file = case_dir / "samples.json"
    heartbeats_file = case_dir / "heartbeats.json"
    src_heartbeat_sock = runtime_paths["src_heartbeat_sock"]
    dst_heartbeat_sock = runtime_paths["dst_heartbeat_sock"]
    boot_img = build_boot_image(case_dir, pressure)
    if mode_uses_mapped_ram(mode) and cxl_path_override is None:
        cxl_backing.write_bytes(b"")
        with open(cxl_backing, "wb") as f:
            f.truncate(256 * 1024 * 1024)

    trace_events = case_dir / "trace-events"
    trace_events.write_text(
        "\n".join([
            "cxl_hybrid_warm_page_send",
            "cxl_hybrid_warm_page_recv",
            "cxl_hybrid_warm_desc_send",
            "cxl_hybrid_warm_desc_recv",
            "cxl_hybrid_warm_page_skip_received",
            "cxl_hybrid_warm_page_skip_unstaged",
            "cxl_hybrid_warm_page_queued",
            "cxl_hybrid_fault_hit",
            "cxl_hybrid_fault_place",
            "cxl_hybrid_fault_miss",
            "cxl_hybrid_publish_request_send",
            "cxl_hybrid_publish_request_recv",
            "cxl_hybrid_publish_ready_send",
            "cxl_hybrid_publish_ready_recv",
            "cxl_hybrid_publish_ready_queue",
            "cxl_hybrid_publish_ready_pop",
            "cxl_hybrid_publish_ready_drain_begin",
            "cxl_hybrid_publish_ready_drain_end",
            "cxl_hybrid_completion_prepare_begin",
            "cxl_hybrid_completion_prepare_end",
            "cxl_hybrid_publish_wait_begin",
            "cxl_hybrid_publish_wait_complete",
            "postcopy_ram_fault_thread_request",
            "postcopy_page_req_add",
            "postcopy_page_req_del",
            "postcopy_request_shared_page",
            "postcopy_request_shared_page_present",
            "ram_save_queue_pages",
            "get_queued_page",
            "get_queued_page_not_dirty",
            "cxl_hybrid_phase_transition",
            "cxl_hybrid_iteration_profile",
            "migrate_pending_estimate",
            "migrate_pending_exact",
            "migration_postcopy_timeline",
            "postcopy_start",
            "postcopy_start_set_run",
            "migration_completion_postcopy_end",
            "migration_completion_postcopy_end_after_complete",
        ]) + "\n",
        encoding="ascii",
    )

    common = build_common_args(boot_img)
    src_vm_args = [
        "-trace", f"events={trace_events},file={src_trace_file}",
        *build_heartbeat_args("src_hb", src_heartbeat_sock),
    ]
    dst_vm_args = [
        "-trace", f"events={trace_events},file={dst_trace_file}",
        *build_heartbeat_args("dst_hb", dst_heartbeat_sock),
        "-incoming", "defer",
    ]

    src_env = build_qemu_env(mode, is_source=True)
    dst_env = build_qemu_env(mode, is_source=False)
    src = start_vm(common, str(src_sock), src_vm_args, src_stderr_file,
                   env=src_env)
    dst = start_vm(common, str(dst_sock), dst_vm_args, dst_stderr_file,
                   env=dst_env)
    proc_list = [src, dst]
    procs = {
        "src": src,
        "dst": dst,
    }
    stderr_paths = {
        "src": src_stderr_file,
        "dst": dst_stderr_file,
    }
    trace_paths = {
        "src": src_trace_file,
        "dst": dst_trace_file,
    }
    qmp_sockets = {
        "src": src_sock,
        "dst": dst_sock,
    }
    src_qmp = None
    dst_qmp = None
    heartbeat_collector = None
    guest_latency = None
    postcopy_control = postcopy_control_template(mode)

    try:
        wait_sock(str(src_sock), src, proc_list)
        wait_sock(str(dst_sock), dst, proc_list)
        wait_sock(str(src_heartbeat_sock), src, proc_list)
        wait_sock(str(dst_heartbeat_sock), dst, proc_list)
        src_qmp = connect_qmp(str(src_sock))
        dst_qmp = connect_qmp(str(dst_sock))
        heartbeat_collector = GuestHeartbeatCollector({
            "src": connect_stream_socket(str(src_heartbeat_sock)),
            "dst": connect_stream_socket(str(dst_heartbeat_sock)),
        })
        heartbeat_collector.start()

        set_caps(src_qmp, mode)
        set_caps(dst_qmp, mode)
        set_params(src_qmp, cxl_backing_path, mode, pressure,
                   shared_backing=True, thresholds=profile,
                   prefetch_rate=prefetch_rate,
                   dst_install_policy=dst_install_policy,
                   fault_control_plane=fault_control_plane,
                   fault_resolve_mode=fault_resolve_mode)
        set_params(dst_qmp, cxl_backing_path, mode, pressure,
                   shared_backing=True, thresholds=profile,
                   prefetch_rate=prefetch_rate,
                   dst_install_policy=dst_install_policy,
                   fault_control_plane=fault_control_plane,
                   fault_resolve_mode=fault_resolve_mode)

        qmp_ok(dst_qmp, "migrate-incoming", {"uri": f"unix:{mig_sock}"})
        qmp_ok(src_qmp, "cont")
        qmp_ok(dst_qmp, "cont")
        time.sleep(0.5)

        migration_start_ts = time.time()
        qmp_ok(src_qmp, "migrate", {"uri": f"unix:{mig_sock}"})
        if mode_uses_manual_postcopy_trigger(mode):
            try:
                request_native_postcopy_start(src_qmp)
                postcopy_control["requested"] = True
            except RuntimeError as exc:
                postcopy_control["request_error"] = str(exc)
                src_status, _ = qmp_probe(src_qmp, "query-status")
                dst_status, _ = qmp_probe(dst_qmp, "query-status")
                build_failure(
                    mode, "native-postcopy-start-failed", None, None, [],
                    case_dir,
                    src_status=src_status,
                    dst_status=dst_status,
                    procs=procs,
                    stderr_paths=stderr_paths,
                    trace_paths=trace_paths,
                    heartbeats_path=heartbeats_file,
                    guest_latency=guest_latency,
                    qmp_sockets=qmp_sockets,
                    postcopy_control=postcopy_control,
                )

        last, samples, src_status, dst_status, stop_reason = (
            collect_until_complete(src_qmp, dst_qmp, samples_file,
                                   timeout=migration_timeout)
        )
        migration_complete_ts = first_status_ts(samples, "completed")
        if migration_complete_ts is None and samples:
            migration_complete_ts = samples[-1].get("ts")
        heartbeat_events = (
            heartbeat_collector.snapshot() if heartbeat_collector else []
        )
        write_json(heartbeats_file, heartbeat_events)
        guest_latency = summarize_guest_heartbeats(
            heartbeat_events,
            migration_start_ts=migration_start_ts,
            migration_complete_ts=migration_complete_ts,
        )
        observed_window_ms = observed_migration_window_ms(
            migration_start_ts, migration_complete_ts
        )
        trace_flush_errors = flush_trace_files(src_qmp, dst_qmp)
        if trace_flush_errors:
            samples.append({
                "ts": time.time(),
                "status": last.get("status") if last else None,
                "errors": {
                    "trace-file-flush": trace_flush_errors,
                },
            })
            write_json(samples_file, samples)
        trace_counts = {
            "src": parse_trace_log(src_trace_file),
            "dst": parse_trace_log(dst_trace_file),
            "combined": trace_count_template(),
        }
        for key in trace_counts["src"]:
            trace_counts["combined"][key] = (
                trace_counts["src"].get(key, 0) +
                trace_counts["dst"].get(key, 0)
            )
        write_json(case_dir / "trace-summary.json", trace_counts)
        summary = extract_summary(samples)
        started = first_postcopy_warm_state(samples)
        saw_postcopy_warm = summary["saw_postcopy_warm"] or \
            trace_saw_postcopy_warm(trace_counts)
        if mode_uses_cxl_hybrid(mode):
            postcopy_control["requested"] = saw_postcopy_warm

        if mode_requires_postcopy_warm(mode) and not saw_postcopy_warm:
            build_failure(mode, "postcopy-warm-not-reached", started, last,
                          samples, case_dir, trace_counts,
                          src_status=src_status, dst_status=dst_status,
                          stop_reason=stop_reason, procs=procs,
                          stderr_paths=stderr_paths, trace_paths=trace_paths,
                          heartbeats_path=heartbeats_file,
                          guest_latency=guest_latency,
                          qmp_sockets=qmp_sockets,
                          postcopy_control=postcopy_control)

        if not last or last.get("status") != "completed":
            build_failure(mode, "migration-not-completed", started, last,
                          samples, case_dir, trace_counts,
                          src_status=src_status, dst_status=dst_status,
                          stop_reason=stop_reason, procs=procs,
                          stderr_paths=stderr_paths, trace_paths=trace_paths,
                          heartbeats_path=heartbeats_file,
                          guest_latency=guest_latency,
                          qmp_sockets=qmp_sockets,
                          postcopy_control=postcopy_control)

        write_json(case_dir / "result.json", {
            "mode": mode,
            "pressure": pressure,
            "final_status": last.get("status"),
            "final_x_cxl": last.get("x-cxl"),
            "src_status": src_status,
            "dst_status": dst_status,
            "latency": {
                "total_time_ms": last.get("total-time"),
                "setup_time_ms": last.get("setup-time"),
                "downtime_ms": last.get("downtime"),
                "stop_to_start_time_ms": last.get("stop-to-start-time"),
                "observed_migration_window_ms": observed_window_ms,
            },
            "guest_latency": guest_latency,
            "trace": trace_counts,
            "summary": summary,
            "postcopy_control": postcopy_control,
        })
        return {
            "mode": mode,
            "pressure": pressure,
            "final_status": last.get("status"),
            "final_x_cxl": last.get("x-cxl"),
            "src_status": src_status,
            "dst_status": dst_status,
            "latency": {
                "total_time_ms": last.get("total-time"),
                "setup_time_ms": last.get("setup-time"),
                "downtime_ms": last.get("downtime"),
                "stop_to_start_time_ms": last.get("stop-to-start-time"),
                "observed_migration_window_ms": observed_window_ms,
            },
            "guest_latency": guest_latency,
            "trace": trace_counts,
            "summary": summary,
            "postcopy_control": postcopy_control,
            "stage_population_observed": max(
                summary["max_dst_stage_present_slots"],
                trace_counts["combined"]["warm_recv"],
            ),
            "case_dir": str(case_dir),
        }
    finally:
        if heartbeat_collector is not None:
            heartbeat_collector.close()
        if src_qmp is not None:
            src_qmp.close()
        if dst_qmp is not None:
            dst_qmp.close()
        shutil.rmtree(socket_dir, ignore_errors=True)
        stop_all(proc_list)


def effectiveness_report(cold, warm):
    cold_sum = cold["summary"]
    warm_sum = warm["summary"]
    cold_trace = cold["trace"]["combined"]
    warm_trace = warm["trace"]["combined"]
    cold_stage_population = max(cold_sum["max_dst_stage_present_slots"],
                                cold_trace["warm_recv"])
    warm_stage_population = max(warm_sum["max_dst_stage_present_slots"],
                                warm_trace["warm_recv"])

    report = {
        "warm_activity_seen": warm_trace["warm_send"] > 0 and warm_trace["warm_recv"] > 0,
        "stage_population_observed_cold": cold_stage_population,
        "stage_population_observed_warm": warm_stage_population,
        "stage_population_improved":
            warm_stage_population > cold_stage_population,
        "fault_hits_improved_or_equal":
            warm_sum["max_dst_fault_hits"] >= cold_sum["max_dst_fault_hits"],
        "faults_observed":
            max(cold_trace["fault_hit"] + cold_trace["fault_miss"],
                warm_trace["fault_hit"] + warm_trace["fault_miss"]) > 0,
    }
    report["effective"] = (
        report["warm_activity_seen"] and
        report["stage_population_improved"] and
        report["fault_hits_improved_or_equal"]
    )
    return report


def summarize_single_result(pressure, mode, threshold_profile, run_index, result):
    config = PRESSURE_LEVELS[pressure]
    profile = threshold_profile or resolve_threshold_profile("balanced")
    summary = (result or {}).get("summary") or {}
    trace = ((result or {}).get("trace") or {}).get("combined") or {}
    latency = (result or {}).get("latency") or {}
    guest_latency = (result or {}).get("guest_latency") or {}

    def mean_ns(total_key, samples_key):
        samples = summary.get(samples_key, 0)
        if not samples:
            return 0
        return summary.get(total_key, 0) // samples

    return {
        "pressure": pressure,
        "mode": mode,
        "threshold_profile": profile["name"],
        "run_index": run_index,
        "failed": (result or {}).get("failed", False),
        "error_reason": (result or {}).get("error_reason"),
        "stop_reason": (result or {}).get("stop_reason"),
        "x-cxl-switch-dirty-threshold":
            profile["x-cxl-switch-dirty-threshold"],
        "x-cxl-switch-max-iters":
            profile["x-cxl-switch-max-iters"],
        "x-cxl-switch-max-precopy-ms":
            profile["x-cxl-switch-max-precopy-ms"],
        "x-cxl-switch-min-remaining":
            profile["x-cxl-switch-min-remaining"],
        "workset_pages":
            (config["end_addr"] - config["start_addr"]) // PAGE_SIZE,
        "writes_per_page": config["writes_per_page"],
        "batch_pages": PRESSURE_BATCH_PAGES[pressure]
        if mode.startswith("hybrid_postcopy") else 0,
        "warm_send": trace.get("warm_send", 0),
        "warm_recv": trace.get("warm_recv", 0),
        "warm_desc_send": trace.get("warm_desc_send", 0),
        "warm_desc_recv": trace.get("warm_desc_recv", 0),
        "stage_population_observed":
            (result or {}).get("stage_population_observed", 0),
        "max_dst_stage_present_slots":
            summary.get("max_dst_stage_present_slots", 0),
        "max_dst_fault_hits": summary.get("max_dst_fault_hits", 0),
        "max_dst_fault_misses": summary.get("max_dst_fault_misses", 0),
        "max_dst_fault_read_bytes":
            summary.get("max_dst_fault_read_bytes", 0),
        "max_dst_fault_read_time_ns":
            summary.get("max_dst_fault_read_time_ns", 0),
        "max_dst_fault_place_successes":
            summary.get("max_dst_fault_place_successes", 0),
        "max_dst_fault_place_failures":
            summary.get("max_dst_fault_place_failures", 0),
        "fault_hit_read_bytes": trace.get("fault_hit_read_bytes", 0),
        "fault_hit_read_time_ns": trace.get("fault_hit_read_time_ns", 0),
        "max_fault_hit_read_time_ns":
            trace.get("max_fault_hit_read_time_ns", 0),
        "fault_hit_read_mean_ns": (
            trace.get("fault_hit_read_time_ns", 0) //
            trace.get("fault_hit", 0)
            if trace.get("fault_hit", 0) else 0
        ),
        "fault_place": trace.get("fault_place", 0),
        "fault_place_time_ns": trace.get("fault_place_time_ns", 0),
        "max_fault_place_time_ns":
            trace.get("max_fault_place_time_ns", 0),
        "fault_place_mean_ns": (
            trace.get("fault_place_time_ns", 0) //
            trace.get("fault_place", 0)
            if trace.get("fault_place", 0) else 0
        ),
        "max_warm_desc_sent_pages":
            summary.get("max_warm_desc_sent_pages", 0),
        "max_warm_payload_fallback_pages":
            summary.get("max_warm_payload_fallback_pages", 0),
        "max_warm_desc_skip_unremapped":
            summary.get("max_warm_desc_skip_unremapped", 0),
        "max_warm_publish_pages":
            summary.get("max_warm_publish_pages", 0),
        "max_fault_publish_requests":
            summary.get("max_fault_publish_requests", 0),
        "max_fault_publish_waits":
            summary.get("max_fault_publish_waits", 0),
        "max_fault_publish_wait_time_ns":
            summary.get("max_fault_publish_wait_time_ns", 0),
        "fault_publish_primary_samples":
            summary.get("fault_publish_primary_samples", 0),
        "fault_publish_primary_time_ns":
            summary.get("fault_publish_primary_time_ns", 0),
        "max_fault_publish_primary_time_ns":
            summary.get("max_fault_publish_primary_time_ns", 0),
        "fault_publish_primary_mean_ns":
            mean_ns("fault_publish_primary_time_ns",
                    "fault_publish_primary_samples"),
        "fault_publish_burst_samples":
            summary.get("fault_publish_burst_samples", 0),
        "fault_publish_burst_time_ns":
            summary.get("fault_publish_burst_time_ns", 0),
        "max_fault_publish_burst_time_ns":
            summary.get("max_fault_publish_burst_time_ns", 0),
        "fault_publish_burst_mean_ns":
            mean_ns("fault_publish_burst_time_ns",
                    "fault_publish_burst_samples"),
        "fault_primary_ready_send_samples":
            summary.get("fault_primary_ready_send_samples", 0),
        "fault_primary_ready_send_time_ns":
            summary.get("fault_primary_ready_send_time_ns", 0),
        "max_fault_primary_ready_send_time_ns":
            summary.get("max_fault_primary_ready_send_time_ns", 0),
        "fault_primary_ready_send_mean_ns":
            mean_ns("fault_primary_ready_send_time_ns",
                    "fault_primary_ready_send_samples"),
        "fault_publish_req_recv_samples":
            summary.get("fault_publish_req_recv_samples", 0),
        "fault_publish_req_recv_time_ns":
            summary.get("fault_publish_req_recv_time_ns", 0),
        "max_fault_publish_req_recv_time_ns":
            summary.get("max_fault_publish_req_recv_time_ns", 0),
        "fault_publish_req_recv_mean_ns":
            mean_ns("fault_publish_req_recv_time_ns",
                    "fault_publish_req_recv_samples"),
        "fault_publish_req_handle_samples":
            summary.get("fault_publish_req_handle_samples", 0),
        "fault_publish_req_handle_time_ns":
            summary.get("fault_publish_req_handle_time_ns", 0),
        "max_fault_publish_req_handle_time_ns":
            summary.get("max_fault_publish_req_handle_time_ns", 0),
        "fault_publish_req_handle_mean_ns":
            mean_ns("fault_publish_req_handle_time_ns",
                    "fault_publish_req_handle_samples"),
        "fault_primary_ready_drain_samples":
            summary.get("fault_primary_ready_drain_samples", 0),
        "fault_primary_ready_drain_time_ns":
            summary.get("fault_primary_ready_drain_time_ns", 0),
        "max_fault_primary_ready_drain_time_ns":
            summary.get("max_fault_primary_ready_drain_time_ns", 0),
        "fault_primary_ready_drain_mean_ns":
            mean_ns("fault_primary_ready_drain_time_ns",
                    "fault_primary_ready_drain_samples"),
        "fault_primary_ready_write_samples":
            summary.get("fault_primary_ready_write_samples", 0),
        "fault_primary_ready_write_time_ns":
            summary.get("fault_primary_ready_write_time_ns", 0),
        "max_fault_primary_ready_write_time_ns":
            summary.get("max_fault_primary_ready_write_time_ns", 0),
        "fault_primary_ready_write_mean_ns":
            mean_ns("fault_primary_ready_write_time_ns",
                    "fault_primary_ready_write_samples"),
        "fault_primary_ready_recv_samples":
            summary.get("fault_primary_ready_recv_samples", 0),
        "fault_primary_ready_recv_time_ns":
            summary.get("fault_primary_ready_recv_time_ns", 0),
        "max_fault_primary_ready_recv_time_ns":
            summary.get("max_fault_primary_ready_recv_time_ns", 0),
        "fault_primary_ready_recv_mean_ns":
            mean_ns("fault_primary_ready_recv_time_ns",
                    "fault_primary_ready_recv_samples"),
        "fault_primary_ready_handle_samples":
            summary.get("fault_primary_ready_handle_samples", 0),
        "fault_primary_ready_handle_time_ns":
            summary.get("fault_primary_ready_handle_time_ns", 0),
        "max_fault_primary_ready_handle_time_ns":
            summary.get("max_fault_primary_ready_handle_time_ns", 0),
        "fault_primary_ready_handle_mean_ns":
            mean_ns("fault_primary_ready_handle_time_ns",
                    "fault_primary_ready_handle_samples"),
        "fault_wait_ready_recv_samples":
            summary.get("fault_wait_ready_recv_samples", 0),
        "fault_wait_ready_recv_time_ns":
            summary.get("fault_wait_ready_recv_time_ns", 0),
        "max_fault_wait_ready_recv_time_ns":
            summary.get("max_fault_wait_ready_recv_time_ns", 0),
        "fault_wait_ready_recv_mean_ns":
            mean_ns("fault_wait_ready_recv_time_ns",
                    "fault_wait_ready_recv_samples"),
        "fault_wait_after_ready_recv_samples":
            summary.get("fault_wait_after_ready_recv_samples", 0),
        "fault_wait_after_ready_recv_time_ns":
            summary.get("fault_wait_after_ready_recv_time_ns", 0),
        "max_fault_wait_after_ready_recv_time_ns":
            summary.get("max_fault_wait_after_ready_recv_time_ns", 0),
        "fault_wait_after_ready_recv_mean_ns":
            mean_ns("fault_wait_after_ready_recv_time_ns",
                    "fault_wait_after_ready_recv_samples"),
        "max_pending_publish_ready":
            summary.get("max_pending_publish_ready", 0),
        "max_completion_pending_publish_ready":
            summary.get("max_completion_pending_publish_ready", 0),
        "last_publish_request_count":
            (summary.get("last_publish_request") or {}).get("count", 0),
        "last_publish_ready_count":
            (summary.get("last_publish_ready") or {}).get("count", 0),
        "last_completion_publish_ready_count":
            (summary.get("last_completion_publish_ready") or {}).get("count", 0),
        "last_publish_ready_recv_count":
            (summary.get("last_publish_ready_recv") or {}).get("count", 0),
        "last_publish_wait_begin_count":
            (summary.get("last_publish_wait_begin") or {}).get("count", 0),
        "last_publish_wait_complete_count":
            (summary.get("last_publish_wait_complete") or {}).get("count", 0),
        "postcopy_fault_request": trace.get("postcopy_fault_request", 0),
        "postcopy_page_req_add": trace.get("postcopy_page_req_add", 0),
        "postcopy_page_req_del": trace.get("postcopy_page_req_del", 0),
        "postcopy_shared_request": trace.get("postcopy_shared_request", 0),
        "postcopy_shared_request_present":
            trace.get("postcopy_shared_request_present", 0),
        "ram_save_queue_pages": trace.get("ram_save_queue_pages", 0),
        "get_queued_page": trace.get("get_queued_page", 0),
        "get_queued_page_not_dirty": trace.get("get_queued_page_not_dirty", 0),
        "max_ram_postcopy_requests":
            summary.get("max_ram_postcopy_requests", 0),
        "max_remap_attempts": summary.get("max_remap_attempts", 0),
        "max_remap_successes": summary.get("max_remap_successes", 0),
        "max_remapped_regions": summary.get("max_remapped_regions", 0),
        "max_dirty_pages_rate": summary.get("max_dirty_pages_rate", 0),
        "max_staged_pages": summary.get("max_staged_pages", 0),
        "total_time_ms": latency.get("total_time_ms"),
        "setup_time_ms": latency.get("setup_time_ms"),
        "downtime_ms": latency.get("downtime_ms"),
        "stop_to_start_time_ms": latency.get("stop_to_start_time_ms"),
        "observed_migration_window_ms":
            latency.get("observed_migration_window_ms"),
        "guest_baseline_gap_ms":
            guest_latency.get("baseline_gap_ms"),
        "guest_handoff_gap_ms":
            guest_latency.get("handoff_gap_ms"),
        "guest_handoff_stall_ms":
            guest_latency.get("handoff_stall_ms"),
        "guest_max_gap_during_migration_ms":
            guest_latency.get("max_gap_during_migration_ms"),
        "guest_total_gap_during_migration_ms":
            guest_latency.get("total_gap_during_migration_ms"),
        "guest_total_stall_during_migration_ms":
            guest_latency.get("total_stall_during_migration_ms"),
        "guest_max_gap_stall_ms":
            guest_latency.get("max_gap_stall_ms"),
        "final_status": (result or {}).get("final_status"),
    }


def summarize_run_matrix(pressure, mode, threshold_profile, run_results):
    return [
        summarize_single_result(pressure, mode, threshold_profile,
                                item["run_index"], item["result"])
        for item in run_results
    ]


def summarize_grouped_runs(pressure, mode, threshold_profile, run_results, rows):
    profile = threshold_profile or resolve_threshold_profile("balanced")
    grouped = {
        "pressure": pressure,
        "mode": mode,
        "threshold_profile": profile["name"],
        "repeat": len(run_results),
        "x-cxl-switch-dirty-threshold":
            profile["x-cxl-switch-dirty-threshold"],
        "x-cxl-switch-max-iters":
            profile["x-cxl-switch-max-iters"],
        "x-cxl-switch-max-precopy-ms":
            profile["x-cxl-switch-max-precopy-ms"],
        "x-cxl-switch-min-remaining":
            profile["x-cxl-switch-min-remaining"],
    }

    def add_stats(metric):
        values = [row[metric] for row in rows if row.get(metric) is not None]
        grouped[f"{metric}_mean"] = statistics.mean(values) if values else None
        grouped[f"{metric}_min"] = min(values) if values else None
        grouped[f"{metric}_max"] = max(values) if values else None

    for metric in (
        "total_time_ms",
        "observed_migration_window_ms",
        "downtime_ms",
        "stop_to_start_time_ms",
        "guest_total_gap_during_migration_ms",
        "guest_total_stall_during_migration_ms",
        "guest_max_gap_stall_ms",
        "max_fault_publish_waits",
        "max_fault_publish_wait_time_ns",
        "max_dst_fault_read_time_ns",
        "fault_hit_read_time_ns",
        "max_fault_hit_read_time_ns",
        "fault_place_time_ns",
        "max_fault_place_time_ns",
    ):
        add_stats(metric)

    def run_seen_postcopy_warm(result):
        summary = (result.get("summary") or {})
        trace = result.get("trace") or {}
        return summary.get("saw_postcopy_warm", False) or \
            trace_saw_postcopy_warm(trace)

    grouped["guardrail_all_completed"] = all(
        item["result"].get("final_status") == "completed"
        for item in run_results
    )
    grouped["guardrail_zero_payload_fallback"] = (
        all(
            (item["result"].get("summary") or {}).get(
                "max_warm_payload_fallback_pages", 0
            ) == 0
            for item in run_results
        )
        if mode_uses_cxl_hybrid(mode)
        else None
    )
    grouped["guardrail_zero_ram_postcopy_requests"] = (
        all(
            (item["result"].get("summary") or {}).get(
                "max_ram_postcopy_requests", 0
            ) == 0
            for item in run_results
        )
        if mode_uses_cxl_hybrid(mode)
        else None
    )
    grouped["guardrail_postcopy_warm_seen"] = (
        all(
            run_seen_postcopy_warm(item["result"])
            for item in run_results
        )
        if mode_uses_cxl_hybrid(mode)
        else None
    )
    grouped["guardrail_postcopy_request_issued"] = (
        all(
            (item["result"].get("postcopy_control") or {}).get("requested")
            for item in run_results
        )
        if mode_uses_manual_postcopy_trigger(mode)
        else None
    )
    grouped["guardrail_ram_postcopy_requests_observed"] = (
        all(
            (item["result"].get("summary") or {}).get(
                "max_ram_postcopy_requests", 0
            ) > 0
            for item in run_results
        )
        if mode == "native_postcopy_stream"
        else None
    )
    return grouped


def experiment_failure_result(mode, pressure, payload):
    last = payload.get("last") or {}
    return {
        "mode": mode,
        "pressure": pressure,
        "final_status": last.get("status"),
        "final_x_cxl": last.get("x-cxl"),
        "src_status": payload.get("src_status"),
        "dst_status": payload.get("dst_status"),
        "latency": {
            "total_time_ms": last.get("total-time"),
            "setup_time_ms": last.get("setup-time"),
            "downtime_ms": last.get("downtime"),
            "stop_to_start_time_ms": last.get("stop-to-start-time"),
        },
        "guest_latency": payload.get("guest_latency") or {},
        "trace": payload.get("trace") or {"combined": trace_count_template()},
        "summary": payload.get("summary") or {},
        "postcopy_control": payload.get("postcopy_control"),
        "stage_population_observed": max(
            (payload.get("summary") or {}).get("max_dst_stage_present_slots", 0),
            ((payload.get("trace") or {}).get("combined") or {}).get("warm_recv", 0),
        ),
        "case_dir": payload.get("case_dir"),
        "diagnostics_path": payload.get("diagnostics_path"),
        "failed": True,
        "error_reason": payload.get("reason"),
        "stop_reason": payload.get("stop_reason"),
    }


def run_pressure_matrix(base: Path, pressures, modes, threshold_profile=None,
                        repeat=1,
                        migration_timeout=DEFAULT_MIGRATION_TIMEOUT_SECS,
                        prefetch_rate=None, dst_install_policy=None,
                        fault_control_plane=None, fault_resolve_mode=None,
                        cxl_path_override=None):
    results = {}
    summary = []
    summary_grouped = []
    profile = threshold_profile or resolve_threshold_profile("balanced")

    for pressure in pressures:
        pressure_dir = base / pressure
        pressure_dir.mkdir(parents=True, exist_ok=True)
        results[pressure] = {}
        for mode in modes:
            run_results = []
            for run_index in range(1, repeat + 1):
                kwargs = {}
                if migration_timeout != DEFAULT_MIGRATION_TIMEOUT_SECS:
                    kwargs["migration_timeout"] = migration_timeout
                if prefetch_rate is not None:
                    kwargs["prefetch_rate"] = prefetch_rate
                if dst_install_policy is not None:
                    kwargs["dst_install_policy"] = dst_install_policy
                if fault_control_plane is not None:
                    kwargs["fault_control_plane"] = fault_control_plane
                if fault_resolve_mode is not None:
                    kwargs["fault_resolve_mode"] = fault_resolve_mode
                if cxl_path_override is not None:
                    kwargs["cxl_path_override"] = cxl_path_override
                if threshold_profile is None and repeat == 1:
                    try:
                        result = run_case(pressure_dir, mode, pressure, **kwargs)
                    except ExperimentFailure as exc:
                        result = experiment_failure_result(mode, pressure,
                                                           exc.payload)
                elif threshold_profile is None:
                    try:
                        result = run_case(pressure_dir, mode, pressure,
                                          run_index=run_index,
                                          **kwargs)
                    except ExperimentFailure as exc:
                        result = experiment_failure_result(mode, pressure,
                                                           exc.payload)
                else:
                    try:
                        result = run_case(
                            pressure_dir, mode, pressure,
                            threshold_profile=threshold_profile,
                            run_index=run_index,
                            **kwargs,
                        )
                    except ExperimentFailure as exc:
                        result = experiment_failure_result(mode, pressure,
                                                           exc.payload)
                run_results.append({
                    "run_index": run_index,
                    "result": result,
                })
            rows = summarize_run_matrix(pressure, mode, profile, run_results)
            summary.extend(rows)
            summary_grouped.append(
                summarize_grouped_runs(pressure, mode, profile, run_results, rows)
            )
            results[pressure][mode] = run_results[0]["result"] if repeat == 1 else {
                "runs": [item["result"] for item in run_results],
            }

    return {
        "pressures": list(pressures),
        "modes": list(modes),
        "results": results,
        "summary": summary,
        "summary_grouped": summary_grouped,
    }


def main():
    maybe_reexec_with_sudo()

    parser = argparse.ArgumentParser()
    parser.add_argument("--keep-dir", action="store_true")
    parser.add_argument("--pressure", default="light,medium,heavy")
    parser.add_argument("--mode",
                        default=DEFAULT_MIGRATION_MODE_CSV)
    parser.add_argument("--threshold-profile", default="balanced")
    parser.add_argument("--x-cxl-switch-dirty-threshold", type=int)
    parser.add_argument("--x-cxl-switch-max-iters", type=int)
    parser.add_argument("--x-cxl-switch-max-precopy-ms", type=int)
    parser.add_argument("--x-cxl-switch-min-remaining", type=int)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--migration-timeout", type=float,
                        default=DEFAULT_MIGRATION_TIMEOUT_SECS)
    parser.add_argument("--x-cxl-prefetch-rate", type=int)
    parser.add_argument("--x-cxl-dst-install-policy",
                        choices=("on-demand", "eager"))
    parser.add_argument("--x-cxl-fault-control-plane",
                        choices=("stream", "cxl"))
    parser.add_argument("--x-cxl-fault-resolve-mode",
                        choices=("copy", "region-remap",
                                 "region-remap-fallback-copy"),
                        default=None)
    parser.add_argument("--cxl-path")
    args = parser.parse_args()

    if not QEMU.exists():
        raise SystemExit(f"missing qemu binary: {QEMU}")
    if not TRACE_EVENTS.exists():
        raise SystemExit(f"missing trace-events-all: {TRACE_EVENTS}")
    if not BOOT_ASM.exists():
        raise SystemExit(f"missing boot asm: {BOOT_ASM}")

    pressures = [item.strip() for item in args.pressure.split(",") if item.strip()]
    if not pressures:
        raise SystemExit("no pressure levels requested")
    for pressure in pressures:
        if pressure not in PRESSURE_LEVELS:
            raise SystemExit(f"unsupported pressure level: {pressure}")
    modes = [item.strip() for item in args.mode.split(",") if item.strip()]
    if not modes:
        raise SystemExit("no migration modes requested")
    for mode in modes:
        if mode not in MIGRATION_MODES:
            raise SystemExit(f"unsupported migration mode: {mode}")
    if args.threshold_profile not in THRESHOLD_PROFILES:
        raise SystemExit(
            f"unsupported threshold profile: {args.threshold_profile}"
        )
    if args.repeat < 1:
        raise SystemExit("repeat must be >= 1")
    if args.migration_timeout <= 0:
        raise SystemExit("migration-timeout must be > 0")
    threshold_profile = resolve_threshold_profile(
        args.threshold_profile,
        dirty_threshold=args.x_cxl_switch_dirty_threshold,
        max_iters=args.x_cxl_switch_max_iters,
        max_precopy_ms=args.x_cxl_switch_max_precopy_ms,
        min_remaining=args.x_cxl_switch_min_remaining,
    )

    base = Path(tempfile.mkdtemp(prefix="cxl-hybrid-warm-exp-"))
    try:
        matrix = run_pressure_matrix(base, pressures, modes, threshold_profile,
                                     repeat=args.repeat,
                                     migration_timeout=args.migration_timeout,
                                     prefetch_rate=args.x_cxl_prefetch_rate,
                                     dst_install_policy=
                                     args.x_cxl_dst_install_policy,
                                     fault_control_plane=
                                     args.x_cxl_fault_control_plane,
                                     fault_resolve_mode=
                                     args.x_cxl_fault_resolve_mode,
                                     cxl_path_override=args.cxl_path)
        result = {
            "base_dir": str(base),
            **matrix,
        }
        print(json.dumps(result, indent=2, sort_keys=True))
        if not args.keep_dir:
            shutil.rmtree(base)
    except Exception as exc:
        if args.keep_dir:
            if isinstance(exc, ExperimentFailure):
                payload = {
                    "base_dir": str(base),
                    "error": exc.payload,
                }
            else:
                payload = {
                    "base_dir": str(base),
                    "error": str(exc),
                }
            print(json.dumps(payload, indent=2, sort_keys=True))
        raise


if __name__ == "__main__":
    main()
