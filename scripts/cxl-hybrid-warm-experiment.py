#!/usr/bin/env python3
import argparse
import json
import math
import os
import re
import shutil
import signal
import socket
import statistics
import struct
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
GUEST_RAM_BYTES = 64 * 1024 * 1024
MULTIFD_CHANNELS = 2
REMAP_GRANULE = 2 * 1024 * 1024
QMP_TIMEOUT_SECS = 1.0
QMP_DUMP_TIMEOUT_SECS = 120.0
IN_MEMORY_DUMP_CHUNK_BYTES = 256 * 1024
MAX_QMP_PROBE_FAILURES = 2
TRACE_TAIL_LINES = 80
STDERR_TAIL_LINES = 80
GDB_TIMEOUT_SECS = 5.0
SAMPLE_INTERVAL_SECS = 0.02
COMPLETION_TAIL_SAMPLES = 2
HEARTBEAT_SOCKET_TIMEOUT_SECS = 0.05
HEARTBEAT_JOIN_TIMEOUT_SECS = 1.0
HEARTBEAT_POST_COMPLETE_GRACE_SECS = 0.05
HEARTBEAT_POST_COMPLETE_MAX_WAIT_SECS = 1.0
HEARTBEAT_POST_COMPLETE_MIN_DST_EVENTS = 2
HEARTBEAT_PORT = 0xE9
IN_MEMORY_LATENCY_MAGIC = 0x4D4C5843
IN_MEMORY_LATENCY_VERSION = 1
IN_MEMORY_LATENCY_ADDR = 0x01200000
IN_MEMORY_LATENCY_RECORDS = 4194304
IN_MEMORY_LATENCY_HEADER_BYTES = 32
IN_MEMORY_LATENCY_DUMP_BYTES = (
    IN_MEMORY_LATENCY_HEADER_BYTES + IN_MEMORY_LATENCY_RECORDS * 4
)
IN_MEMORY_MARKER_MAGIC = 0x4D4B5843
IN_MEMORY_MARKER_VERSION = 1
IN_MEMORY_MARKER_ADDR = 0x02800000
IN_MEMORY_MARKER_HEADER_BYTES = 32
IN_MEMORY_MARKER_BYTES = (
    IN_MEMORY_MARKER_HEADER_BYTES + IN_MEMORY_LATENCY_RECORDS * 4
)
DEFAULT_MIGRATION_TIMEOUT_SECS = 60.0
POSTCOPY_START_RETRY_TIMEOUT_SECS = 0.5
DEFAULT_RDMA_HOST = "10.0.0.2"
DEFAULT_RDMA_PORT = 4444
PERF_RECORD_SECS = 8
FAULT_HIT_TRACE_RE = re.compile(
    r"\blen=(?P<len>\d+)\b.*\bread_time_ns=(?P<read_time_ns>\d+)\b"
)
FAULT_PLACE_TRACE_RE = re.compile(
    r"\bplace_time_ns=(?P<place_time_ns>\d+)\b"
)
REGION_PUBLISH_TRACE_RE = re.compile(
    r"\bpages=(?P<pages>\d+)\b.*\bpublished=(?P<published>-?\d+)\b"
    r"(?:.*\belapsed_ns=(?P<elapsed_ns>\d+)\b)?"
)
REGION_WAIT_COMPLETE_TRACE_RE = re.compile(
    r"\bret=(?P<ret>-?\d+)\b"
    r"(?:.*\belapsed_ns=(?P<elapsed_ns>\d+)\b)?"
)
PUBLISH_WAIT_COMPLETE_TRACE_RE = re.compile(
    r"\bwait_time_ns=(?P<wait_time_ns>\d+)\b"
    r".*\bret=(?P<ret>-?\d+)\b"
)
RDMA_REGION_TRACE_RE = re.compile(
    r"\bregion=(?P<region>\d+)\b.*\bpages=(?P<pages>\d+)\b"
)
PUBLISH_SPAN_TRACE_RE = re.compile(
    r"\blen=0x(?P<len>[0-9a-fA-F]+)\b"
    r"(?:.*\bkind=(?P<kind>\d+)\b)?"
    r"(?:.*\breason=(?P<reason>\d+)\b)?"
)
POSTCOPY_TIMELINE_TRACE_RE = re.compile(
    r"\bmigration_(?:precopy|postcopy)_timeline\s+(?P<stage>\S+)\b"
    r".*\bnow_ns=(?P<now_ns>\d+)\b"
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
        "sample_interval_pages": 64,
    },
    "remap_xlarge_random_rw": {
        "start_addr": 0x00100000,
        "end_addr": 0x01100000,
        "writes_per_page": 1,
        "outer_spin": 0,
        "page_order_random": True,
        "access_pattern_random_rw": True,
        "random_page_stride": 73,
        "random_epoch_seed_step": 1,
        "sample_interval_pages": 64,
    },
    "remap_xlarge_random_read": {
        "start_addr": 0x00100000,
        "end_addr": 0x01100000,
        "writes_per_page": 1,
        "outer_spin": 0,
        "page_order_random": True,
        "access_pattern_read_only": True,
        "random_page_stride": 73,
        "random_epoch_seed_step": 1,
        "sample_interval_pages": 64,
    },
}
PRESSURE_BATCH_PAGES = {
    "light": 32,
    "medium": 64,
    "heavy": 128,
    "remap_mid": 256,
    "remap_heavy": 512,
    "remap_heavy_random_rw": 512,
    "remap_xlarge_random_rw": 1024,
    "remap_xlarge_random_read": 1024,
}
MIGRATION_MODES = (
    "hybrid_postcopy_payload",
    "hybrid_postcopy_auto",
    "hybrid_postcopy_cxl_offset",
    "hybrid_postcopy_manual",
    "hybrid_parallel_rdma_cxl",
    "native_postcopy_stream",
    "native_rdma_precopy",
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
    return (
        mode.startswith("hybrid_postcopy") or
        mode == "hybrid_parallel_rdma_cxl"
    )


def mode_uses_rdma(mode: str) -> bool:
    return mode == "native_rdma_precopy"


def mode_uses_postcopy(mode: str) -> bool:
    return mode_uses_cxl_hybrid(mode) or mode == "native_postcopy_stream"


def mode_uses_mapped_ram(mode: str) -> bool:
    return mode not in ("native_postcopy_stream", "native_rdma_precopy")


def mode_uses_multifd(mode: str) -> bool:
    return not mode_uses_rdma(mode)


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
            mono_ns = time.monotonic_ns()
            events = []
            for byte in data:
                item = {"ts": ts, "mono_ns": mono_ns, "side": side,
                        "byte": byte}
                if 32 <= byte < 127:
                    item["char"] = chr(byte)
                events.append(item)
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


def build_migration_uri(mode: str, mig_sock: Path, rdma_host=DEFAULT_RDMA_HOST,
                        rdma_port=DEFAULT_RDMA_PORT):
    if mode_uses_rdma(mode):
        return f"rdma:{rdma_host}:{rdma_port}"
    return f"unix:{mig_sock}"


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


def build_boot_image(base: Path, pressure: str,
                     in_memory_guest_latency=False) -> Path:
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
    if config.get("access_pattern_read_only", False):
        nasm_args.extend([
            "-D",
            "PRESSURE_ACCESS_PATTERN_READ_ONLY=1",
        ])
    if config.get("sample_interval_pages", 0):
        nasm_args.extend([
            "-D",
            "PRESSURE_SAMPLE_INTERVAL_PAGES="
            f"{config.get('sample_interval_pages', 0)}",
        ])
    if in_memory_guest_latency:
        nasm_args.extend([
            "-D",
            "PRESSURE_IN_MEMORY_LATENCY=1",
            "-D",
            f"PRESSURE_IN_MEMORY_LOG_ADDR=0x{IN_MEMORY_LATENCY_ADDR:08x}",
            "-D",
            f"PRESSURE_IN_MEMORY_LOG_RECORDS={IN_MEMORY_LATENCY_RECORDS}",
            "-D",
            f"PRESSURE_IN_MEMORY_MARKER_ADDR=0x{IN_MEMORY_MARKER_ADDR:08x}",
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


def set_caps(f, mode: str, rdma_pin_all=False):
    hybrid_mode = mode_uses_cxl_hybrid(mode)
    qmp_ok(f, "migrate-set-capabilities", {
        "capabilities": [
            {"capability": "mapped-ram",
             "state": mode_uses_mapped_ram(mode)},
            {"capability": "postcopy-ram",
             "state": mode_uses_postcopy(mode)},
            {"capability": "x-cxl-hybrid",
             "state": hybrid_mode},
            {"capability": "multifd", "state": mode_uses_multifd(mode)},
            {"capability": "rdma-pin-all",
             "state": bool(rdma_pin_all) if mode_uses_rdma(mode) else False},
        ]
    })


def build_migration_parameters(args, mode: str, cxl_path=None,
                               shared_backing: bool = True,
                               thresholds=None):
    pressure = getattr(args, "pressure", "light")
    if isinstance(pressure, str):
        pressure = next(
            item.strip() for item in pressure.split(",") if item.strip()
        )
    hybrid_mode = mode_uses_cxl_hybrid(mode)
    params = {
        "max-bandwidth": getattr(args, "max_bandwidth", 0),
    }
    if mode_uses_multifd(mode):
        params["multifd-channels"] = MULTIFD_CHANNELS
    max_postcopy_bandwidth = getattr(args, "max_postcopy_bandwidth", None)
    if max_postcopy_bandwidth is not None:
        params["max-postcopy-bandwidth"] = max_postcopy_bandwidth
    if mode_uses_mapped_ram(mode):
        params["cxl-path"] = cxl_path
        params["x-cxl-brake-remap-granule"] = getattr(
            args, "x_cxl_brake_remap_granule", REMAP_GRANULE
        )
    if hybrid_mode:
        if mode == "hybrid_postcopy_payload":
            raise ValueError("payload hybrid mode is disabled for CXL-only postcopy")
        profile = thresholds or resolve_threshold_profile(
            getattr(args, "threshold_profile", "balanced"),
            dirty_threshold=getattr(args, "x_cxl_switch_dirty_threshold", None),
            max_iters=getattr(args, "x_cxl_switch_max_iters", None),
            max_precopy_ms=getattr(args, "x_cxl_switch_max_precopy_ms", None),
            min_remaining=getattr(args, "x_cxl_switch_min_remaining", None),
        )
        if not shared_backing:
            raise ValueError(
                f"{mode} requires explicit shared_backing confirmation"
            )
        prefetch_rate = getattr(args, "x_cxl_prefetch_rate", None)
        backing_rate = getattr(args, "x_cxl_backing_rate", None)
        fault_resolve_mode = getattr(args, "x_cxl_fault_resolve_mode", None)
        remap_coverage = getattr(args, "x_cxl_switch_remap_coverage", None)
        clean_remap_enable = getattr(args, "x_cxl_clean_remap_enable", False)
        clean_remap_copy_budget = getattr(
            args, "x_cxl_clean_remap_copy_budget", None
        )
        clean_remap_throttle_us = getattr(
            args, "x_cxl_clean_remap_throttle_us", None
        )
        clean_remap_prefault_mode = getattr(
            args, "x_cxl_clean_remap_prefault_mode", None
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
            "x-cxl-brake-enable": getattr(args, "x_cxl_brake_enable", True),
            "x-cxl-prefetch-rate": (
                prefetch_rate
                if prefetch_rate is not None
                else 0
            ),
            "x-cxl-prefetch-batch-pages": PRESSURE_BATCH_PAGES[pressure],
            "x-cxl-prefetch-heat-window-ms": 250,
            "x-cxl-shared-backing": shared_backing,
        })
        if remap_coverage is not None:
            params["x-cxl-switch-remap-coverage"] = remap_coverage
        if backing_rate is not None:
            params["x-cxl-backing-rate"] = backing_rate
        if fault_resolve_mode is not None:
            params["x-cxl-fault-resolve-mode"] = fault_resolve_mode
        if clean_remap_enable:
            params["x-cxl-clean-remap-enable"] = True
        if clean_remap_copy_budget is not None:
            params["x-cxl-clean-remap-copy-budget"] = (
                clean_remap_copy_budget
            )
        if clean_remap_throttle_us is not None:
            params["x-cxl-clean-remap-throttle-us"] = (
                clean_remap_throttle_us
            )
        if clean_remap_prefault_mode is not None:
            params["x-cxl-clean-remap-prefault-mode"] = (
                clean_remap_prefault_mode
            )
    return params


def set_params(f, cxl_path: str, mode: str, pressure: str,
               shared_backing: bool = False, thresholds=None,
               prefetch_rate=None, dst_install_policy=None,
               fault_control_plane=None, fault_resolve_mode=None,
               max_bandwidth=0, max_postcopy_bandwidth=None,
               cxl_backing_rate=None,
               brake_remap_granule=REMAP_GRANULE,
               brake_enable=True,
               switch_remap_coverage=None,
               clean_remap_enable=False,
               clean_remap_copy_budget=None,
               clean_remap_throttle_us=None,
               clean_remap_prefault_mode=None):
    args = argparse.Namespace(
        pressure=pressure,
        threshold_profile="balanced",
        x_cxl_switch_dirty_threshold=None,
        x_cxl_switch_max_iters=None,
        x_cxl_switch_max_precopy_ms=None,
        x_cxl_switch_min_remaining=None,
        x_cxl_switch_remap_coverage=switch_remap_coverage,
        x_cxl_prefetch_rate=prefetch_rate,
        x_cxl_backing_rate=cxl_backing_rate,
        x_cxl_dst_install_policy=dst_install_policy,
        x_cxl_fault_control_plane=fault_control_plane,
        x_cxl_fault_resolve_mode=fault_resolve_mode,
        x_cxl_brake_remap_granule=brake_remap_granule,
        x_cxl_brake_enable=brake_enable,
        x_cxl_clean_remap_enable=clean_remap_enable,
        x_cxl_clean_remap_copy_budget=clean_remap_copy_budget,
        x_cxl_clean_remap_throttle_us=clean_remap_throttle_us,
        x_cxl_clean_remap_prefault_mode=clean_remap_prefault_mode,
        max_bandwidth=max_bandwidth,
        max_postcopy_bandwidth=max_postcopy_bandwidth,
    )
    params = build_migration_parameters(
        args, mode, cxl_path=cxl_path, shared_backing=shared_backing,
        thresholds=thresholds,
    )
    qmp_ok(f, "migrate-set-parameters", params)


def build_qemu_env(mode: str, is_source: bool, clean_remap_debug_mode=None):
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
    if is_source and clean_remap_debug_mode:
        env["QEMU_CXL_CLEAN_REMAP_DEBUG"] = clean_remap_debug_mode
    else:
        env.pop("QEMU_CXL_CLEAN_REMAP_DEBUG", None)
    return env


def with_guest_timeline_env(env, enabled):
    env = dict(env)
    if enabled:
        env["QEMU_CXL_GUEST_TIMELINE_MARKER_ADDR"] = (
            f"0x{IN_MEMORY_MARKER_ADDR:08x}"
        )
    else:
        env.pop("QEMU_CXL_GUEST_TIMELINE_MARKER_ADDR", None)
    return env


def build_common_args(boot_img: Path, accel="tcg"):
    return [
        str(QEMU),
        "-machine", f"pc,accel={accel}",
        "-global", "apic-common.vapic=false",
        "-m", f"{GUEST_RAM_BYTES // (1024 * 1024)}M",
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


def start_qemu_perf(case_dir: Path, src, dst, enabled=False, frequency=4000):
    if not enabled:
        return None, {}

    perf_path = case_dir / "qemu-perf.data"
    pids = f"{src.pid},{dst.pid}"
    argv = [
        "perf", "record", "--clockid", "mono",
        "-T", "-F", str(frequency), "-g",
        "-p", pids,
        "-o", str(perf_path),
        "--", "sleep", str(PERF_RECORD_SECS),
    ]
    proc = subprocess.Popen(
        argv,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return proc, {
        "qemu_perf_path": str(perf_path),
        "qemu_perf_pids": pids,
        "qemu_perf_frequency": frequency,
    }


def stop_qemu_perf(proc, info):
    if proc is None:
        return

    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=PERF_RECORD_SECS + 2)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
    info["qemu_perf_returncode"] = proc.returncode


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
        "publish_wait_time_ns": 0,
        "max_publish_wait_time_ns": 0,
        "region_publish_complete": 0,
        "region_publish_pages": 0,
        "region_publish_published_pages": 0,
        "region_publish_time_ns": 0,
        "region_wait_begin": 0,
        "region_wait_complete": 0,
        "region_wait_complete_failures": 0,
        "region_wait_time_ns": 0,
        "max_region_wait_time_ns": 0,
        "dst_region_remap": 0,
        "rdma_ready_regions": 0,
        "rdma_ready_pages": 0,
        "rdma_invalidated_regions": 0,
        "rdma_ready_pages_lost": 0,
        "cxl_republish_regions_due_to_rdma_invalidate": 0,
        "cxl_republish_pages_due_to_rdma_invalidate": 0,
        "rdma_invalidate_publish_amplification": 0.0,
        "ram_stream_publish_span": 0,
        "ram_stream_publish_span_pages": 0,
        "ram_stream_publish_span_max_pages": 0,
        "ram_stream_publish_span_single_page": 0,
        "ram_stream_publish_span_written": 0,
        "ram_stream_publish_span_written_pages": 0,
        "ram_stream_publish_span_source_remapped": 0,
        "ram_stream_publish_span_source_remapped_pages": 0,
        "ram_stream_publish_span_destination_owned": 0,
        "ram_stream_publish_span_destination_owned_pages": 0,
        "ram_stream_publish_span_already_visible": 0,
        "ram_stream_publish_span_already_visible_pages": 0,
        "ram_stream_publish_span_unknown_reason": 0,
        "ram_stream_publish_span_unknown_reason_pages": 0,
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
            match = PUBLISH_WAIT_COMPLETE_TRACE_RE.search(line)
            if match:
                wait_time_ns = int(match.group("wait_time_ns"))
                counts["publish_wait_time_ns"] += wait_time_ns
                counts["max_publish_wait_time_ns"] = max(
                    counts["max_publish_wait_time_ns"], wait_time_ns)
        elif "cxl_hybrid_region_publish_complete " in line:
            counts["region_publish_complete"] += 1
            match = REGION_PUBLISH_TRACE_RE.search(line)
            if match:
                counts["region_publish_pages"] += int(match.group("pages"))
                counts["region_publish_published_pages"] += int(
                    match.group("published"))
                if match.group("elapsed_ns") is not None:
                    counts["region_publish_time_ns"] += int(
                        match.group("elapsed_ns"))
        elif "cxl_hybrid_region_wait_begin " in line:
            counts["region_wait_begin"] += 1
        elif "cxl_hybrid_region_wait_complete " in line:
            counts["region_wait_complete"] += 1
            match = REGION_WAIT_COMPLETE_TRACE_RE.search(line)
            if match:
                if int(match.group("ret")) != 0:
                    counts["region_wait_complete_failures"] += 1
                if match.group("elapsed_ns") is not None:
                    elapsed_ns = int(match.group("elapsed_ns"))
                    counts["region_wait_time_ns"] += elapsed_ns
                    counts["max_region_wait_time_ns"] = max(
                        counts["max_region_wait_time_ns"], elapsed_ns)
        elif "cxl_hybrid_dst_region_remap " in line:
            counts["dst_region_remap"] += 1
        elif "cxl_hybrid_rdma_ready " in line:
            match = RDMA_REGION_TRACE_RE.search(line)
            if match:
                counts["rdma_ready_regions"] += 1
                counts["rdma_ready_pages"] += int(match.group("pages"))
        elif "cxl_hybrid_rdma_invalidate " in line:
            match = RDMA_REGION_TRACE_RE.search(line)
            if match:
                counts["rdma_invalidated_regions"] += 1
                counts["rdma_ready_pages_lost"] += int(match.group("pages"))
        elif "cxl_hybrid_rdma_cxl_republish " in line:
            match = RDMA_REGION_TRACE_RE.search(line)
            if match:
                counts[
                    "cxl_republish_regions_due_to_rdma_invalidate"
                ] += 1
                counts[
                    "cxl_republish_pages_due_to_rdma_invalidate"
                ] += int(match.group("pages"))
        elif "cxl_hybrid_ram_stream_publish_span " in line:
            counts["ram_stream_publish_span"] += 1
            match = PUBLISH_SPAN_TRACE_RE.search(line)
            if match:
                pages = int(match.group("len"), 16) // PAGE_SIZE
                kind = (int(match.group("kind"))
                        if match.group("kind") is not None else None)
                reason = (int(match.group("reason"))
                          if match.group("reason") is not None else None)
                counts["ram_stream_publish_span_pages"] += pages
                counts["ram_stream_publish_span_max_pages"] = max(
                    counts["ram_stream_publish_span_max_pages"], pages)
                if pages == 1:
                    counts["ram_stream_publish_span_single_page"] += 1
                if kind == 0:
                    counts["ram_stream_publish_span_written"] += 1
                    counts["ram_stream_publish_span_written_pages"] += pages
                elif reason == 1:
                    counts["ram_stream_publish_span_source_remapped"] += 1
                    counts[
                        "ram_stream_publish_span_source_remapped_pages"
                    ] += pages
                elif reason == 2:
                    counts["ram_stream_publish_span_destination_owned"] += 1
                    counts[
                        "ram_stream_publish_span_destination_owned_pages"
                    ] += pages
                elif kind == 1:
                    counts["ram_stream_publish_span_already_visible"] += 1
                    counts[
                        "ram_stream_publish_span_already_visible_pages"
                    ] += pages
                else:
                    counts["ram_stream_publish_span_unknown_reason"] += 1
                    counts[
                        "ram_stream_publish_span_unknown_reason_pages"
                    ] += pages
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
    counts["rdma_invalidate_publish_amplification"] = (
        counts["cxl_republish_pages_due_to_rdma_invalidate"] /
        max(counts["rdma_ready_pages_lost"], 1)
    )
    return counts


def parse_postcopy_timeline(trace_file: Path):
    timeline = {}
    if not trace_file.exists():
        return timeline

    for line in trace_file.read_text(
            encoding="utf-8", errors="replace").splitlines():
        match = POSTCOPY_TIMELINE_TRACE_RE.search(line)
        if not match:
            continue
        stage = match.group("stage")
        now_ns = int(match.group("now_ns"))
        timeline.setdefault(stage, []).append(now_ns)
    return timeline


def first_timeline_ns(timeline, stage):
    values = (timeline or {}).get(stage) or []
    return values[0] if values else None


def elapsed_ms(start_ns, end_ns):
    if start_ns is None or end_ns is None:
        return None
    return max((end_ns - start_ns) / 1_000_000.0, 0.0)


def elapsed_us(start_ns, end_ns):
    if start_ns is None or end_ns is None:
        return None
    return max((end_ns - start_ns) / 1_000.0, 0.0)


def read_host_tsc_khz():
    for path in (
        Path("/sys/devices/system/cpu/cpu0/tsc_freq_khz"),
        Path("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"),
    ):
        try:
            value = path.read_text(encoding="ascii").strip()
            if value:
                return int(float(value))
        except (OSError, ValueError):
            pass

    try:
        text = Path("/proc/cpuinfo").read_text(encoding="ascii",
                                               errors="ignore")
    except OSError:
        return None

    match = re.search(r"^cpu MHz\s*:\s*([0-9.]+)", text, re.MULTILINE)
    if not match:
        return None
    return int(float(match.group(1)) * 1000)


def cycles_to_us(cycles, tsc_khz):
    if cycles is None or not tsc_khz:
        return None
    return (float(cycles) * 1000.0) / float(tsc_khz)


def percentile(values, pct):
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * (pct / 100.0)
    lower = int(math.floor(rank))
    upper = int(math.ceil(rank))
    if lower == upper:
        return ordered[lower]
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize_cycle_records(records, tsc_khz=None, baseline_cycles=None):
    if not records:
        return {
            "count": 0,
            "baseline_cycles": baseline_cycles,
        }

    ordered = sorted(records)
    baseline = (
        statistics.median(ordered)
        if baseline_cycles is None else baseline_cycles
    )
    excess = [max(float(value) - float(baseline), 0.0)
              for value in records]
    total_excess_cycles = sum(excess)
    max_excess_cycles = max(excess) if excess else 0.0

    return {
        "count": len(records),
        "min_cycles": ordered[0],
        "mean_cycles": statistics.mean(records),
        "median_cycles": statistics.median(ordered),
        "p95_cycles": percentile(ordered, 95),
        "p99_cycles": percentile(ordered, 99),
        "max_cycles": ordered[-1],
        "baseline_cycles": baseline,
        "total_excess_cycles": total_excess_cycles,
        "max_excess_cycles": max_excess_cycles,
        "total_excess_ms": (
            cycles_to_us(total_excess_cycles, tsc_khz) / 1000.0
            if tsc_khz else None
        ),
        "max_excess_us": cycles_to_us(max_excess_cycles, tsc_khz),
    }


def in_memory_samples_per_epoch(pressure, sample_interval_pages):
    config = PRESSURE_LEVELS.get(pressure)
    if not config or not sample_interval_pages:
        return None
    workset_pages = (
        (config["end_addr"] - config["start_addr"]) // PAGE_SIZE
    )
    return max(workset_pages // sample_interval_pages, 1)


def is_epoch_marker_following_record(index, samples_per_epoch):
    if not samples_per_epoch:
        return False
    return index > 0 and (index % samples_per_epoch) == 0


def filter_epoch_marker_records(records, samples_per_epoch, start_index=0):
    return [
        value for offset, value in enumerate(records)
        if not is_epoch_marker_following_record(start_index + offset,
                                                samples_per_epoch)
    ]


def reorder_ring_records(raw_records, total_count, capacity):
    if total_count <= capacity:
        return list(raw_records[:min(total_count, len(raw_records))]), 0

    readable = min(capacity, len(raw_records))
    start = total_count - readable
    ring_start = start % capacity
    ordered = []
    for i in range(readable):
        ordered.append(raw_records[(ring_start + i) % capacity])
    return ordered, start


def epoch_marker_events(heartbeat_events):
    markers = []
    for item in heartbeat_events or []:
        if item.get("char") == "." or item.get("byte") == ord("."):
            if item.get("ts") is not None:
                markers.append(item)
    return sorted(markers, key=lambda item: item["ts"])


def estimate_sample_index_for_ts(ts, markers, samples_per_epoch):
    if ts is None or not markers or not samples_per_epoch:
        return None

    indexed = [
        (item["ts"], (idx + 1) * samples_per_epoch)
        for idx, item in enumerate(markers)
    ]
    if len(indexed) == 1:
        return indexed[0][1] if ts >= indexed[0][0] else 0

    first_ts, first_index = indexed[0]
    if ts <= first_ts:
        next_ts, _next_index = indexed[1]
        span = max(next_ts - first_ts, 1e-9)
        return max(int(round(first_index -
                             ((first_ts - ts) / span) * samples_per_epoch)),
                   0)

    for (prev_ts, prev_index), (next_ts, next_index) in zip(indexed,
                                                           indexed[1:]):
        if prev_ts <= ts <= next_ts:
            span = max(next_ts - prev_ts, 1e-9)
            fraction = (ts - prev_ts) / span
            return int(round(prev_index +
                             fraction * (next_index - prev_index)))

    prev_ts, prev_index = indexed[-2]
    last_ts, last_index = indexed[-1]
    span = max(last_ts - prev_ts, 1e-9)
    return int(round(last_index + ((ts - last_ts) / span) *
                     samples_per_epoch))


def clamp_window_indices(start_ts, end_ts, markers, samples_per_epoch,
                         record_count):
    start = estimate_sample_index_for_ts(start_ts, markers, samples_per_epoch)
    end = estimate_sample_index_for_ts(end_ts, markers, samples_per_epoch)
    if start is None or end is None:
        return None, None
    start = min(max(start, 0), record_count)
    end = min(max(end, 0), record_count)
    if end < start:
        start, end = end, start
    return start, end


def parse_in_memory_marker_payload(payload):
    if len(payload) < IN_MEMORY_MARKER_HEADER_BYTES:
        return {
            "valid": False,
            "error": "short in-memory marker dump",
            "bytes": len(payload),
        }

    magic, version, count, capacity, last_seq, last_event, reserved0, \
        reserved1 = struct.unpack_from("<8I", payload, 0)
    report = {
        "valid": magic == IN_MEMORY_MARKER_MAGIC and
        version == IN_MEMORY_MARKER_VERSION,
        "magic": magic,
        "version": version,
        "samples_total": count,
        "capacity": capacity,
        "last_seq": last_seq,
        "last_event": last_event,
        "bytes": len(payload),
        "reserved0": reserved0,
        "reserved1": reserved1,
    }
    if not report["valid"]:
        report["error"] = "invalid in-memory marker header"
        return report

    available = max((len(payload) - IN_MEMORY_MARKER_HEADER_BYTES) // 4, 0)
    raw_readable = min(capacity, available)
    if raw_readable:
        raw_records = list(struct.unpack_from(
            f"<{raw_readable}I", payload, IN_MEMORY_MARKER_HEADER_BYTES
        ))
    else:
        raw_records = []
    records, sample_index_start = reorder_ring_records(
        raw_records, count, capacity
    )
    report.update({
        "samples_read": len(records),
        "sample_index_start": sample_index_start,
        "sample_index_end": sample_index_start + len(records),
        "records": records,
    })
    return report


def marker_window_indices(marker_report, start_event, complete_event):
    records = (marker_report or {}).get("records") or []
    sample_index_start = (marker_report or {}).get("sample_index_start", 0)
    start = None
    end = None
    for local_index, seq in enumerate(records):
        absolute_index = sample_index_start + local_index
        if start is None and seq == start_event:
            start = absolute_index
        if start is not None and seq == complete_event:
            end = absolute_index + 1
            break
    if start is not None and end is not None and end < start:
        start, end = end, start
    return start, end


DEFAULT_IN_MEMORY_WINDOW_SPECS = (
    ("precopy_window", 1, 2),
    ("precopy_bulk_window", 1, 7),
    ("precopy_brake_window", 7, 8),
    ("postcopy_handoff_window", 8, 6),
    ("postcopy_window", 6, 10),
    ("completion_tail_window", 10, 5),
)


def parse_in_memory_latency_payload(payload, pressure, tsc_khz=None,
                                    heartbeat_events=None,
                                    migration_start_ts=None,
                                    migration_complete_ts=None,
                                    corrected_start_ts=None,
                                    corrected_complete_ts=None,
                                    marker_payload=None,
                                    marker_start_event=None,
                                    marker_complete_event=None,
                                    window_specs=None):
    if len(payload) < IN_MEMORY_LATENCY_HEADER_BYTES:
        return {
            "valid": False,
            "error": "short in-memory latency dump",
            "bytes": len(payload),
        }

    magic, version, sample_interval_pages, capacity, count, dropped, \
        last_tsc_low, last_tsc_high = struct.unpack_from("<8I", payload, 0)
    report = {
        "valid": magic == IN_MEMORY_LATENCY_MAGIC and
        version == IN_MEMORY_LATENCY_VERSION,
        "magic": magic,
        "version": version,
        "sample_interval_pages": sample_interval_pages,
        "capacity": capacity,
        "samples_total": count,
        "dropped_samples": dropped,
        "last_tsc_low": last_tsc_low,
        "last_tsc_high": last_tsc_high,
        "bytes": len(payload),
    }
    if not report["valid"]:
        report["error"] = "invalid in-memory latency header"
        return report

    available = max((len(payload) - IN_MEMORY_LATENCY_HEADER_BYTES) // 4, 0)
    raw_readable = min(capacity, available)
    if raw_readable:
        raw_records = list(struct.unpack_from(
            f"<{raw_readable}I", payload, IN_MEMORY_LATENCY_HEADER_BYTES
        ))
    else:
        raw_records = []
    records, sample_index_start = reorder_ring_records(
        raw_records, count, capacity
    )
    readable = len(records)

    tsc_khz = tsc_khz or read_host_tsc_khz()
    samples_per_epoch = in_memory_samples_per_epoch(
        pressure, sample_interval_pages
    )
    marker_events = epoch_marker_events(heartbeat_events)
    no_epoch_records = filter_epoch_marker_records(records, samples_per_epoch)
    marker_report = (
        parse_in_memory_marker_payload(marker_payload)
        if marker_payload is not None else None
    )

    absolute_baseline_start, _baseline_end = clamp_window_indices(
        migration_start_ts, migration_start_ts, marker_events,
        samples_per_epoch, count
    )
    baseline_start = (
        absolute_baseline_start - sample_index_start
        if absolute_baseline_start is not None else None
    )
    baseline_pool = no_epoch_records
    if baseline_start is not None and baseline_start > 8:
        baseline_pool = filter_epoch_marker_records(
            records[:baseline_start], samples_per_epoch
        ) or no_epoch_records
    baseline_cycles = (
        statistics.median(baseline_pool)
        if baseline_pool else None
    )

    report.update({
        "samples_read": readable,
        "sample_index_start": sample_index_start,
        "sample_index_end": sample_index_start + readable,
        "tsc_khz": tsc_khz,
        "samples_per_epoch": samples_per_epoch,
        "epoch_markers": len(marker_events),
        "baseline_cycles": baseline_cycles,
        "all": summarize_cycle_records(records, tsc_khz, baseline_cycles),
        "no_epoch_marker": summarize_cycle_records(
            no_epoch_records, tsc_khz, baseline_cycles
        ),
        "marker_valid": (marker_report or {}).get("valid"),
        "marker_samples_read": (marker_report or {}).get("samples_read"),
        "marker_start_event": marker_start_event,
        "marker_complete_event": marker_complete_event,
    })

    def add_sample_window(prefix, start, end):
        report[f"{prefix}_sample_start"] = start
        report[f"{prefix}_sample_end"] = end
        if start is None or end is None:
            report[prefix] = summarize_cycle_records([], tsc_khz,
                                                     baseline_cycles)
            report[f"{prefix}_no_epoch_marker"] = summarize_cycle_records(
                [], tsc_khz, baseline_cycles
            )
            return
        local_start = min(max(start - sample_index_start, 0), len(records))
        local_end = min(max(end - sample_index_start, 0), len(records))
        if local_end < local_start:
            local_start, local_end = local_end, local_start
        report[f"{prefix}_local_sample_start"] = local_start
        report[f"{prefix}_local_sample_end"] = local_end
        window_records = records[local_start:local_end]
        report[prefix] = summarize_cycle_records(
            window_records, tsc_khz, baseline_cycles
        )
        report[f"{prefix}_no_epoch_marker"] = summarize_cycle_records(
            filter_epoch_marker_records(window_records, samples_per_epoch,
                                        start_index=local_start +
                                        sample_index_start),
            tsc_khz,
            baseline_cycles,
        )

    def add_window(prefix, start_ts, end_ts):
        start, end = None, None
        if (prefix == "corrected_window" and marker_report and
                marker_report.get("valid") and
                marker_start_event is not None and
                marker_complete_event is not None):
            start, end = marker_window_indices(
                marker_report, marker_start_event, marker_complete_event
            )
        if start is None or end is None:
            start, end = clamp_window_indices(
                start_ts, end_ts, marker_events, samples_per_epoch, count
            )
        add_sample_window(prefix, start, end)

    add_window("migration_window", migration_start_ts, migration_complete_ts)
    add_window("corrected_window", corrected_start_ts, corrected_complete_ts)
    if marker_report and marker_report.get("valid"):
        for prefix, start_event, complete_event in (
                window_specs or DEFAULT_IN_MEMORY_WINDOW_SPECS):
            start, end = marker_window_indices(
                marker_report, start_event, complete_event
            )
            add_sample_window(prefix, start, end)
    return report


def parse_in_memory_latency_dump(path: Path, pressure, **kwargs):
    return parse_in_memory_latency_payload(
        path.read_bytes(), pressure, **kwargs
    )


def dump_guest_physical_memory(conn, addr, size, path: Path):
    quoted_path = str(path).replace("\\", "\\\\").replace('"', '\\"')
    previous_timeout = conn.sock.gettimeout()
    conn.sock.settimeout(QMP_DUMP_TIMEOUT_SECS)
    try:
        response = qmp_ok(conn, "human-monitor-command", {
            "command-line": f'pmemsave 0x{addr:x} {size} "{quoted_path}"',
        })
    finally:
        conn.sock.settimeout(previous_timeout)
    if isinstance(response, str) and response.strip():
        raise RuntimeError(response.strip())


def dump_guest_physical_memory_chunked(conn, addr, size, path: Path):
    tmp_path = path.with_name(path.name + ".part")
    if tmp_path.exists():
        tmp_path.unlink()

    try:
        remaining = size
        current_addr = addr
        with open(tmp_path, "ab") as out:
            while remaining:
                chunk = min(remaining, IN_MEMORY_DUMP_CHUNK_BYTES)
                chunk_path = path.with_name(path.name + ".chunk")
                dump_guest_physical_memory(conn, current_addr, chunk,
                                           chunk_path)
                out.write(chunk_path.read_bytes())
                if chunk_path.exists():
                    chunk_path.unlink()
                current_addr += chunk
                remaining -= chunk
        tmp_path.replace(path)
    finally:
        chunk_path = path.with_name(path.name + ".chunk")
        if chunk_path.exists():
            chunk_path.unlink()
        if tmp_path.exists():
            tmp_path.unlink()


def in_memory_ring_dump_size(path: Path, magic, version, header_bytes,
                             count_index, capacity_index):
    if not path.exists() or path.stat().st_size < header_bytes:
        return header_bytes

    header = path.read_bytes()[:header_bytes]
    fields = struct.unpack_from("<8I", header, 0)
    header_magic, header_version = fields[0], fields[1]
    if header_magic != magic or header_version != version:
        return header_bytes

    count = fields[count_index]
    capacity = fields[capacity_index]
    readable = min(count, capacity)
    return header_bytes + readable * 4


def dump_in_memory_ring(conn, addr, max_size, path: Path, magic, version,
                        header_bytes, count_index, capacity_index):
    dump_guest_physical_memory(conn, addr, header_bytes, path)
    size = in_memory_ring_dump_size(path, magic, version, header_bytes,
                                    count_index, capacity_index)
    size = min(max(size, header_bytes), max_size)
    if size > header_bytes:
        dump_guest_physical_memory_chunked(conn, addr, size, path)


def collect_in_memory_guest_latency(conn, path: Path, pressure, **kwargs):
    marker_path = kwargs.pop("marker_path", None)
    fallback_conn = kwargs.pop("fallback_conn", None)
    fallback_path = kwargs.pop("fallback_path", None)
    fallback_marker_path = kwargs.pop("fallback_marker_path", None)
    primary_dump_source = kwargs.pop("primary_dump_source", "primary")
    base_kwargs = dict(kwargs)

    def collect_from(active_conn, active_path, active_marker_path,
                     dump_source):
        parse_kwargs = dict(base_kwargs)

        def dump_marker_if_requested():
            if active_marker_path is None:
                return
            dump_in_memory_ring(
                active_conn, IN_MEMORY_MARKER_ADDR, IN_MEMORY_MARKER_BYTES,
                active_marker_path, IN_MEMORY_MARKER_MAGIC,
                IN_MEMORY_MARKER_VERSION, IN_MEMORY_MARKER_HEADER_BYTES,
                count_index=2, capacity_index=3,
            )
            parse_kwargs["marker_payload"] = active_marker_path.read_bytes()

        dump_in_memory_ring(
            active_conn, IN_MEMORY_LATENCY_ADDR, IN_MEMORY_LATENCY_DUMP_BYTES,
            active_path, IN_MEMORY_LATENCY_MAGIC, IN_MEMORY_LATENCY_VERSION,
            IN_MEMORY_LATENCY_HEADER_BYTES, count_index=4, capacity_index=3,
        )
        dump_marker_if_requested()
        report = parse_in_memory_latency_dump(
            active_path, pressure, **parse_kwargs
        )
        report["dump_path"] = str(active_path)
        report["dump_source"] = dump_source
        if active_marker_path is not None:
            report["marker_dump_path"] = str(active_marker_path)
        return report

    def parse_partial_primary(exc):
        if not path.exists() or path.stat().st_size < \
                IN_MEMORY_LATENCY_HEADER_BYTES:
            return None

        parse_kwargs = dict(base_kwargs)
        try:
            if marker_path is not None:
                if not marker_path.exists():
                    try:
                        dump_in_memory_ring(
                            conn, IN_MEMORY_MARKER_ADDR,
                            IN_MEMORY_MARKER_BYTES, marker_path,
                            IN_MEMORY_MARKER_MAGIC,
                            IN_MEMORY_MARKER_VERSION,
                            IN_MEMORY_MARKER_HEADER_BYTES,
                            count_index=2, capacity_index=3,
                        )
                    except Exception:
                        pass
                if marker_path.exists():
                    parse_kwargs["marker_payload"] = marker_path.read_bytes()
            report = parse_in_memory_latency_dump(
                path, pressure, **parse_kwargs
            )
            if report.get("valid"):
                report["dump_path"] = str(path)
                report["dump_source"] = "primary"
                if marker_path is not None:
                    report["marker_dump_path"] = str(marker_path)
                report["partial_dump"] = (
                    path.stat().st_size < IN_MEMORY_LATENCY_DUMP_BYTES
                )
                report["dump_error"] = str(exc)
                return report
        except Exception:
            return None
        return None

    def fallback_report(primary_error):
        if fallback_conn is None:
            return None
        active_path = fallback_path or path.with_name(path.stem + "-src.bin")
        active_marker_path = fallback_marker_path
        if active_marker_path is None and marker_path is not None:
            active_marker_path = marker_path.with_name(
                marker_path.stem + "-src.bin"
            )
        report = collect_from(
            fallback_conn, active_path, active_marker_path, "fallback"
        )
        report["primary_dump_error"] = primary_error
        report["primary_partial_bytes"] = (
            path.stat().st_size if path.exists() else 0
        )
        report["fallback_source_only"] = True
        return report

    try:
        return collect_from(conn, path, marker_path, primary_dump_source)
    except Exception as exc:
        primary_error = str(exc)
        partial = parse_partial_primary(exc)
        if partial and partial.get("samples_read", 0) > 0:
            return partial
        try:
            fallback = fallback_report(primary_error)
            if fallback is not None:
                return fallback
        except Exception as fallback_exc:
            if partial:
                partial["fallback_dump_error"] = str(fallback_exc)
                return partial
            return {
                "valid": False,
                "error": primary_error,
                "fallback_dump_error": str(fallback_exc),
                "dump_path": str(path),
            }
        if partial:
            return partial
        return {
            "valid": False,
            "error": primary_error,
            "dump_path": str(path),
        }


def summarize_handoff_breakdown(src_trace_file: Path, dst_trace_file: Path,
                                guest_latency=None, trace_counts=None,
                                summary=None, heartbeat_events=None):
    guest_latency = guest_latency or {}
    summary = summary or {}
    combined_trace = ((trace_counts or {}).get("combined") or {})
    src_timeline = parse_postcopy_timeline(src_trace_file)
    dst_timeline = parse_postcopy_timeline(dst_trace_file)
    fault_publish_wait_time_ns = (
        summary.get("max_fault_publish_wait_time_ns", 0) or
        combined_trace.get("max_publish_wait_time_ns", 0)
    )
    region_wait_samples = summary.get("dst_region_wait_samples", 0)
    if region_wait_samples:
        region_wait_time_ns = summary.get("dst_region_wait_time_ns", 0)
        max_region_wait_time_ns = summary.get("max_dst_region_wait_time_ns", 0)
    else:
        region_wait_samples = combined_trace.get("region_wait_complete", 0)
        region_wait_time_ns = combined_trace.get("region_wait_time_ns", 0)
        max_region_wait_time_ns = combined_trace.get("max_region_wait_time_ns", 0)

    region_publish_requests = summary.get("region_publish_requests", 0)
    if region_publish_requests:
        region_publish_pages = summary.get("region_publish_pages", 0)
        region_publish_time_ns = summary.get("region_publish_time_ns", 0)
    else:
        region_publish_requests = combined_trace.get("region_publish_complete", 0)
        region_publish_pages = combined_trace.get("region_publish_pages", 0)
        region_publish_time_ns = combined_trace.get("region_publish_time_ns", 0)

    src_estimate_ns = first_timeline_ns(src_timeline, "estimate")
    src_enter_brake_ns = first_timeline_ns(src_timeline, "enter-brake")
    src_request_postcopy_ns = first_timeline_ns(src_timeline,
                                                "request-postcopy")
    src_start_ns = first_timeline_ns(src_timeline, "start")
    src_downtime_end_ns = first_timeline_ns(src_timeline, "downtime-end")
    src_postcopy_active_ns = first_timeline_ns(src_timeline,
                                               "state-postcopy-active")
    src_completion_enter_ns = first_timeline_ns(src_timeline,
                                                "completion-enter")
    src_completion_prepare_done_ns = first_timeline_ns(
        src_timeline, "completion-prepare-done")
    src_completed_ns = first_timeline_ns(src_timeline, "completed")
    dst_run_ns = first_timeline_ns(dst_timeline, "dst-postcopy-run-cmd")
    dst_vm_started_ns = first_timeline_ns(
        dst_timeline, "dst-postcopy-bh-vm-started")
    dst_ack_ns = first_timeline_ns(dst_timeline, "dst-postcopy-bh-ack-sent")
    dst_heartbeat_ns = None
    if dst_vm_started_ns is not None:
        dst_heartbeats = sorted(
            item.get("mono_ns")
            for item in (heartbeat_events or [])
            if item.get("side") == "dst" and item.get("mono_ns") is not None
        )
        for mono_ns in dst_heartbeats:
            if mono_ns >= dst_vm_started_ns:
                dst_heartbeat_ns = mono_ns
                break

    return {
        "control_src_estimate_to_enter_brake_ms":
            elapsed_ms(src_estimate_ns, src_enter_brake_ns),
        "control_src_start_to_enter_brake_ms":
            elapsed_ms(src_estimate_ns or src_start_ns, src_enter_brake_ns),
        "control_src_enter_brake_to_request_postcopy_ms":
            elapsed_ms(src_enter_brake_ns, src_request_postcopy_ns),
        "control_src_request_postcopy_to_postcopy_start_ms":
            elapsed_ms(src_request_postcopy_ns, src_start_ns),
        "control_src_start_to_dst_vm_started_ms":
            elapsed_ms(src_start_ns, dst_vm_started_ns),
        "control_src_start_to_dst_ack_ms":
            elapsed_ms(src_start_ns, dst_ack_ns),
        "control_src_start_to_downtime_end_ms":
            elapsed_ms(src_start_ns, src_downtime_end_ns),
        "control_src_downtime_end_to_postcopy_active_ms":
            elapsed_ms(src_downtime_end_ns, src_postcopy_active_ns),
        "control_src_postcopy_active_to_completion_enter_ms":
            elapsed_ms(src_postcopy_active_ns, src_completion_enter_ns),
        "control_src_completion_enter_to_prepare_done_ms":
            elapsed_ms(src_completion_enter_ns,
                       src_completion_prepare_done_ns),
        "control_src_completion_enter_to_completed_ms":
            elapsed_ms(src_completion_enter_ns, src_completed_ns),
        "control_src_completion_prepare_done_to_completed_ms":
            elapsed_ms(src_completion_prepare_done_ns, src_completed_ns),
        "control_src_downtime_end_to_dst_run_ms":
            elapsed_ms(src_downtime_end_ns, dst_run_ns),
        "control_dst_run_to_vm_started_ms":
            elapsed_ms(dst_run_ns, dst_vm_started_ns),
        "control_dst_vm_started_to_ack_us":
            elapsed_us(dst_vm_started_ns, dst_ack_ns),
        "guest_baseline_gap_ms": guest_latency.get("baseline_gap_ms"),
        "guest_handoff_gap_ms": guest_latency.get("handoff_gap_ms"),
        "guest_handoff_stall_ms": guest_latency.get("handoff_stall_ms"),
        "src_tail_gap_mean_ms": guest_latency.get("src_tail_gap_mean_ms"),
        "src_tail_gap_max_ms": guest_latency.get("src_tail_gap_max_ms"),
        "dst_initial_gap_1_ms": guest_latency.get("dst_initial_gap_1_ms"),
        "dst_initial_gap_2_ms": guest_latency.get("dst_initial_gap_2_ms"),
        "dst_initial_excess_stall_ms":
            guest_latency.get("dst_initial_excess_stall_ms"),
        "post_vm_start_wallclock_alignment_available":
            dst_heartbeat_ns is not None,
        "post_vm_start_to_first_dst_heartbeat_ms":
            elapsed_ms(dst_vm_started_ns, dst_heartbeat_ns),
        "postcopy_fault_request":
            combined_trace.get("postcopy_fault_request", 0),
        "postcopy_fault_read_time_ns":
            combined_trace.get("fault_hit_read_time_ns", 0),
        "postcopy_fault_place_time_ns":
            combined_trace.get("fault_place_time_ns", 0),
        "postcopy_fault_publish_wait_time_ns":
            fault_publish_wait_time_ns,
        "postcopy_fault_total_time_ns":
            combined_trace.get("fault_hit_read_time_ns", 0) +
            combined_trace.get("fault_place_time_ns", 0) +
            fault_publish_wait_time_ns,
        "trace_dst_region_remap": combined_trace.get("dst_region_remap", 0),
        "postcopy_region_wait_samples":
            region_wait_samples,
        "dst_region_wait_time_ns":
            region_wait_time_ns,
        "postcopy_region_wait_time_ns":
            region_wait_time_ns,
        "max_dst_region_wait_time_ns":
            max_region_wait_time_ns,
        "postcopy_region_publish_requests":
            region_publish_requests,
        "postcopy_region_publish_pages":
            region_publish_pages,
        "postcopy_region_publish_time_ns":
            region_publish_time_ns,
        "max_dst_region_map_time_ns":
            summary.get("max_dst_region_map_time_ns", 0),
        "ram_save_queue_pages": combined_trace.get("ram_save_queue_pages", 0),
        "get_queued_page": combined_trace.get("get_queued_page", 0),
        "region_publish_pages":
            region_publish_pages,
        "region_publish_time_ns":
            region_publish_time_ns,
        "ram_stream_publish_span_pages":
            combined_trace.get("ram_stream_publish_span_pages", 0),
        "ram_stream_publish_span_written_pages":
            combined_trace.get("ram_stream_publish_span_written_pages", 0),
        "ram_stream_publish_span_source_remapped_pages":
            combined_trace.get(
                "ram_stream_publish_span_source_remapped_pages", 0),
        "ram_stream_publish_span_destination_owned_pages":
            combined_trace.get(
                "ram_stream_publish_span_destination_owned_pages", 0),
        "ram_stream_publish_span_already_visible_pages":
            combined_trace.get(
                "ram_stream_publish_span_already_visible_pages", 0),
        "ram_stream_publish_span_unknown_reason_pages":
            combined_trace.get(
                "ram_stream_publish_span_unknown_reason_pages", 0),
    }


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
    max_dst_region_map_attempts = 0
    max_dst_region_map_successes = 0
    max_dst_region_map_failures = 0
    max_dst_region_map_time_ns = 0
    max_dst_region_wake_failures = 0
    dst_region_wait_samples = 0
    dst_region_wait_time_ns = 0
    max_dst_region_wait_time_ns = 0
    max_dst_region_fallback_copies = 0
    max_ram_postcopy_requests = 0
    max_dirty_pages_rate = 0
    max_warm_desc_sent_pages = 0
    max_warm_payload_fallback_pages = 0
    max_warm_desc_skip_unremapped = 0
    max_warm_publish_pages = 0
    max_fault_publish_requests = 0
    max_fault_publish_waits = 0
    max_fault_publish_wait_time_ns = 0
    region_publish_requests = 0
    region_publish_pages = 0
    region_publish_time_ns = 0
    rdma_ready_regions = 0
    rdma_ready_pages = 0
    rdma_invalidated_regions = 0
    rdma_ready_pages_lost = 0
    cxl_republish_regions_due_to_rdma_invalidate = 0
    cxl_republish_pages_due_to_rdma_invalidate = 0
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
        max_dst_region_map_attempts = max(
            max_dst_region_map_attempts,
            src_xcxl.get("dst-region-map-attempts", 0),
            dst_xcxl.get("dst-region-map-attempts", 0))
        max_dst_region_map_successes = max(
            max_dst_region_map_successes,
            src_xcxl.get("dst-region-map-successes", 0),
            dst_xcxl.get("dst-region-map-successes", 0))
        max_dst_region_map_failures = max(
            max_dst_region_map_failures,
            src_xcxl.get("dst-region-map-failures", 0),
            dst_xcxl.get("dst-region-map-failures", 0))
        max_dst_region_map_time_ns = max(
            max_dst_region_map_time_ns,
            src_xcxl.get("dst-region-map-time-ns", 0),
            dst_xcxl.get("dst-region-map-time-ns", 0))
        max_dst_region_wake_failures = max(
            max_dst_region_wake_failures,
            src_xcxl.get("dst-region-wake-failures", 0),
            dst_xcxl.get("dst-region-wake-failures", 0))
        dst_region_wait_samples = max(
            dst_region_wait_samples,
            src_xcxl.get("dst-region-wait-samples", 0),
            dst_xcxl.get("dst-region-wait-samples", 0))
        dst_region_wait_time_ns = max(
            dst_region_wait_time_ns,
            src_xcxl.get("dst-region-wait-time-ns", 0),
            dst_xcxl.get("dst-region-wait-time-ns", 0))
        max_dst_region_wait_time_ns = max(
            max_dst_region_wait_time_ns,
            src_xcxl.get("max-dst-region-wait-time-ns", 0),
            dst_xcxl.get("max-dst-region-wait-time-ns", 0))
        max_dst_region_fallback_copies = max(
            max_dst_region_fallback_copies,
            src_xcxl.get("dst-region-fallback-copies", 0),
            dst_xcxl.get("dst-region-fallback-copies", 0))
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
        region_publish_requests = max(
            region_publish_requests,
            src_xcxl.get("region-publish-requests", 0),
            dst_xcxl.get("region-publish-requests", 0))
        region_publish_pages = max(
            region_publish_pages,
            src_xcxl.get("region-publish-pages", 0),
            dst_xcxl.get("region-publish-pages", 0))
        region_publish_time_ns = max(
            region_publish_time_ns,
            src_xcxl.get("region-publish-time-ns", 0),
            dst_xcxl.get("region-publish-time-ns", 0))
        rdma_ready_regions = max(
            rdma_ready_regions,
            src_xcxl.get("rdma-ready-regions", 0),
            dst_xcxl.get("rdma-ready-regions", 0))
        rdma_ready_pages = max(
            rdma_ready_pages,
            src_xcxl.get("rdma-ready-pages", 0),
            dst_xcxl.get("rdma-ready-pages", 0))
        rdma_invalidated_regions = max(
            rdma_invalidated_regions,
            src_xcxl.get("rdma-invalidated-regions", 0),
            dst_xcxl.get("rdma-invalidated-regions", 0))
        rdma_ready_pages_lost = max(
            rdma_ready_pages_lost,
            src_xcxl.get("rdma-ready-pages-lost", 0),
            dst_xcxl.get("rdma-ready-pages-lost", 0))
        cxl_republish_regions_due_to_rdma_invalidate = max(
            cxl_republish_regions_due_to_rdma_invalidate,
            src_xcxl.get("cxl-republish-regions-due-to-rdma-invalidate", 0),
            dst_xcxl.get("cxl-republish-regions-due-to-rdma-invalidate", 0))
        cxl_republish_pages_due_to_rdma_invalidate = max(
            cxl_republish_pages_due_to_rdma_invalidate,
            src_xcxl.get("cxl-republish-pages-due-to-rdma-invalidate", 0),
            dst_xcxl.get("cxl-republish-pages-due-to-rdma-invalidate", 0))
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
        "max_dst_region_map_attempts": max_dst_region_map_attempts,
        "max_dst_region_map_successes": max_dst_region_map_successes,
        "max_dst_region_map_failures": max_dst_region_map_failures,
        "max_dst_region_map_time_ns": max_dst_region_map_time_ns,
        "max_dst_region_wake_failures": max_dst_region_wake_failures,
        "dst_region_wait_samples": dst_region_wait_samples,
        "dst_region_wait_time_ns": dst_region_wait_time_ns,
        "max_dst_region_wait_time_ns": max_dst_region_wait_time_ns,
        "max_dst_region_fallback_copies": max_dst_region_fallback_copies,
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
        "region_publish_requests": region_publish_requests,
        "region_publish_pages": region_publish_pages,
        "region_publish_time_ns": region_publish_time_ns,
        "rdma_ready_regions": rdma_ready_regions,
        "rdma_ready_pages": rdma_ready_pages,
        "rdma_invalidated_regions": rdma_invalidated_regions,
        "rdma_ready_pages_lost": rdma_ready_pages_lost,
        "cxl_republish_regions_due_to_rdma_invalidate":
            cxl_republish_regions_due_to_rdma_invalidate,
        "cxl_republish_pages_due_to_rdma_invalidate":
            cxl_republish_pages_due_to_rdma_invalidate,
        "rdma_invalidate_publish_amplification": (
            cxl_republish_pages_due_to_rdma_invalidate /
            max(rdma_ready_pages_lost, 1)
        ),
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


def corrected_window_from_total_time(migration_start_ts,
                                     migration_complete_ts,
                                     final_info):
    total_time_ms = (final_info or {}).get("total-time")
    if total_time_ms is None:
        return None, None

    duration_s = float(total_time_ms) / 1000.0
    if migration_start_ts is not None and (
        migration_complete_ts is None or migration_start_ts <= migration_complete_ts
    ):
        start = migration_start_ts
        complete = migration_start_ts + duration_s
        return start, complete

    if migration_complete_ts is None:
        return None, None

    complete = migration_complete_ts
    start = complete - duration_s
    return start, complete


def _summarize_guest_heartbeats_for_window(events, migration_start_ts=None,
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
    src_same_side_gaps_ms = [
        item["gap_ms"] for item in gaps
        if item["start_side"] == "src" and item["end_side"] == "src"
    ]
    dst_same_side_gaps_ms = [
        item["gap_ms"] for item in gaps
        if item["start_side"] == "dst" and item["end_side"] == "dst"
    ]
    src_tail_gaps_ms = src_same_side_gaps_ms[-5:]
    src_tail_gap_mean_ms = (
        statistics.mean(src_tail_gaps_ms) if src_tail_gaps_ms else None
    )
    src_tail_gap_max_ms = (
        max(src_tail_gaps_ms) if src_tail_gaps_ms else None
    )
    dst_initial_gap_1_ms = (
        dst_same_side_gaps_ms[0] if len(dst_same_side_gaps_ms) >= 1 else None
    )
    dst_initial_gap_2_ms = (
        dst_same_side_gaps_ms[1] if len(dst_same_side_gaps_ms) >= 2 else None
    )
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
                stall_start_ts = item["start_ts"] + (baseline_gap_ms / 1000.0)
                stall_overlap_start_ts = max(stall_start_ts,
                                             migration_start_ts)
                stall_overlap_end_ts = min(item["end_ts"],
                                           migration_complete_ts)
                total_stall_during_migration_ms += max(
                    (stall_overlap_end_ts - stall_overlap_start_ts) * 1000.0,
                    0.0,
                )

    def stall_ms(gap_ms):
        if gap_ms is None or baseline_gap_ms is None:
            return None
        return max(gap_ms - baseline_gap_ms, 0.0)

    first_destination_gap_stall_ms = None
    destination_rest_stall_ms = None
    if baseline_gap_ms is not None and migration_start_ts is not None and \
       migration_complete_ts is not None:
        def gap_stall_overlap_ms(item):
            stall_start_ts = item["start_ts"] + (baseline_gap_ms / 1000.0)
            overlap_start_ts = max(stall_start_ts, migration_start_ts)
            overlap_end_ts = min(item["end_ts"], migration_complete_ts)
            return max((overlap_end_ts - overlap_start_ts) * 1000.0, 0.0)

        dst_gaps = [
            item for item in gaps
            if item["end_side"] == "dst" and
            item["start_ts"] <= migration_complete_ts and
            item["end_ts"] >= migration_start_ts
        ]
        if dst_gaps:
            first_destination_gap_stall_ms = gap_stall_overlap_ms(dst_gaps[0])
            rest = 0.0
            for item in dst_gaps[1:]:
                rest += gap_stall_overlap_ms(item)
            destination_rest_stall_ms = rest

    return {
        "events_total": len(ordered),
        "events_src": len(src_events),
        "events_dst": len(dst_events),
        "baseline_gap_ms": baseline_gap_ms,
        "handoff_gap_ms": handoff_gap_ms,
        "handoff_stall_ms": stall_ms(handoff_gap_ms),
        "src_tail_gap_mean_ms": src_tail_gap_mean_ms,
        "src_tail_gap_max_ms": src_tail_gap_max_ms,
        "dst_initial_gap_1_ms": dst_initial_gap_1_ms,
        "dst_initial_gap_2_ms": dst_initial_gap_2_ms,
        "dst_initial_excess_stall_ms": stall_ms(dst_initial_gap_1_ms),
        "max_gap_ms": max_gap_ms,
        "max_gap_during_migration_ms": max_gap_during_migration_ms,
        "total_gap_during_migration_ms": total_gap_during_migration_ms,
        "total_stall_during_migration_ms": total_stall_during_migration_ms,
        "max_gap_stall_ms": stall_ms(max_gap_during_migration_ms or max_gap_ms),
        "first_destination_gap_stall_ms": first_destination_gap_stall_ms,
        "destination_rest_stall_ms": destination_rest_stall_ms,
    }


def summarize_guest_heartbeats(events, migration_start_ts=None,
                               migration_complete_ts=None,
                               corrected_start_ts=None,
                               corrected_complete_ts=None):
    observed = _summarize_guest_heartbeats_for_window(
        events, migration_start_ts, migration_complete_ts
    )
    corrected = _summarize_guest_heartbeats_for_window(
        events,
        corrected_start_ts if corrected_start_ts is not None
        else migration_start_ts,
        corrected_complete_ts if corrected_complete_ts is not None
        else migration_complete_ts,
    )
    observed["corrected_window"] = corrected
    return observed


def collect_heartbeat_events_with_grace(heartbeat_collector):
    if heartbeat_collector is None:
        return []

    events = heartbeat_collector.snapshot()
    min_dst_events = HEARTBEAT_POST_COMPLETE_MIN_DST_EVENTS

    def has_enough_destination_events(items):
        return sum(1 for item in items if item.get("side") == "dst") >= \
            min_dst_events

    if has_enough_destination_events(events):
        return events

    polls = max(1, math.ceil(HEARTBEAT_POST_COMPLETE_MAX_WAIT_SECS /
                             HEARTBEAT_POST_COMPLETE_GRACE_SECS))
    for _ in range(polls):
        time.sleep(HEARTBEAT_POST_COMPLETE_GRACE_SECS)
        events = heartbeat_collector.snapshot()
        if has_enough_destination_events(events):
            break
    return events


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
             cxl_path_override=None, max_bandwidth=0,
             max_postcopy_bandwidth=None,
             cxl_backing_rate=None,
             brake_remap_granule=REMAP_GRANULE,
             brake_enable=True,
             switch_remap_coverage=None,
             clean_remap_enable=False,
             clean_remap_copy_budget=None,
             clean_remap_throttle_us=None,
             clean_remap_prefault_mode=None,
             clean_remap_debug_mode=None,
             rdma_host=DEFAULT_RDMA_HOST,
             rdma_port=DEFAULT_RDMA_PORT,
             rdma_pin_all=False,
             accel="tcg",
             qemu_perf=False,
             in_memory_guest_latency=False,
             in_memory_guest_latency_source_first=False):
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
    in_memory_latency_file = case_dir / "guest-in-memory-latency.bin"
    in_memory_marker_file = case_dir / "guest-in-memory-marker.bin"
    in_memory_latency_src_file = case_dir / "guest-in-memory-latency-src.bin"
    in_memory_marker_src_file = case_dir / "guest-in-memory-marker-src.bin"
    src_heartbeat_sock = runtime_paths["src_heartbeat_sock"]
    dst_heartbeat_sock = runtime_paths["dst_heartbeat_sock"]
    if in_memory_guest_latency:
        boot_img = build_boot_image(
            case_dir, pressure,
            in_memory_guest_latency=True,
        )
    else:
        boot_img = build_boot_image(case_dir, pressure)
    if mode_uses_mapped_ram(mode) and cxl_path_override is None:
        cxl_backing.write_bytes(b"")
        with open(cxl_backing, "wb") as f:
            f.truncate(256 * 1024 * 1024)

    trace_events = case_dir / "trace-events"
    trace_events.write_text(
        "\n".join([
            "cxl_hybrid_warm_page_skip_received",
            "cxl_hybrid_warm_page_skip_unstaged",
            "cxl_hybrid_warm_page_queued",
            "cxl_hybrid_fault_hit",
            "cxl_hybrid_fault_place",
            "cxl_hybrid_fault_miss",
            "cxl_hybrid_publish_request_send",
            "cxl_hybrid_publish_request_recv",
            "cxl_hybrid_completion_prepare_begin",
            "cxl_hybrid_completion_prepare_end",
            "cxl_hybrid_publish_wait_begin",
            "cxl_hybrid_publish_wait_complete",
            "cxl_hybrid_region_publish_complete",
            "cxl_hybrid_region_wait_begin",
            "cxl_hybrid_region_wait_begin_ts",
            "cxl_hybrid_region_wait_complete",
            "cxl_hybrid_region_wait_complete_ts",
            "cxl_hybrid_dst_region_remap",
            "cxl_hybrid_dst_region_remap_ts",
            "cxl_hybrid_ram_stream_publish_span",
            "cxl_hybrid_region_request_enqueue",
            "cxl_hybrid_region_request_dequeue",
            "postcopy_ram_fault_thread_request",
            "postcopy_ram_fault_thread_request_ts",
            "postcopy_page_req_add",
            "postcopy_page_req_add_ts",
            "postcopy_page_req_del",
            "postcopy_page_req_del_ts",
            "postcopy_request_shared_page",
            "postcopy_request_shared_page_present",
            "ram_save_queue_pages",
            "get_queued_page",
            "get_queued_page_not_dirty",
            "cxl_hybrid_phase_transition",
            "cxl_hybrid_iteration_profile",
            "cxl_hybrid_iteration_profile_ts",
            "cxl_hybrid_ram_iterate_profile_time",
            "cxl_hybrid_ram_iterate_profile_count",
            "cxl_hybrid_ram_iterate_profile_scan",
            "cxl_hybrid_remap_drain_profile",
            "cxl_hybrid_clean_remap_scan_summary_ts",
            "cxl_hybrid_clean_remap_copy_begin_ts",
            "cxl_hybrid_clean_remap_copy_end_ts",
            "cxl_hybrid_clean_remap_throttle_begin",
            "cxl_hybrid_clean_remap_throttle_end",
            "memory_notdirty_write_access",
            "memory_notdirty_set_dirty",
            "cxl_hybrid_switch_policy",
            "migrate_pending_estimate",
            "migrate_pending_exact",
            "migration_precopy_timeline",
            "migration_postcopy_timeline",
            "postcopy_start",
            "postcopy_start_set_run",
            "migration_completion_postcopy_end",
            "migration_completion_postcopy_end_after_complete",
        ]) + "\n",
        encoding="ascii",
    )

    common = build_common_args(boot_img, accel=accel)
    src_vm_args = [
        "-trace", f"events={trace_events},file={src_trace_file}",
    ]
    dst_vm_args = [
        "-trace", f"events={trace_events},file={dst_trace_file}",
        "-incoming", "defer",
    ]
    if not in_memory_guest_latency:
        src_vm_args.extend(build_heartbeat_args("src_hb", src_heartbeat_sock))
        dst_vm_args.extend(build_heartbeat_args("dst_hb", dst_heartbeat_sock))
    migration_uri = build_migration_uri(
        mode, mig_sock, rdma_host=rdma_host, rdma_port=rdma_port
    )

    src_env = build_qemu_env(
        mode, is_source=True,
        clean_remap_debug_mode=clean_remap_debug_mode,
    )
    src_env = with_guest_timeline_env(src_env, in_memory_guest_latency)
    dst_env = build_qemu_env(
        mode, is_source=False,
        clean_remap_debug_mode=clean_remap_debug_mode,
    )
    dst_env = with_guest_timeline_env(dst_env, in_memory_guest_latency)
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
    guest_in_memory_latency = None
    postcopy_control = postcopy_control_template(mode)
    qemu_perf_proc = None
    qemu_perf_info = {}

    try:
        wait_sock(str(src_sock), src, proc_list)
        wait_sock(str(dst_sock), dst, proc_list)
        src_qmp = connect_qmp(str(src_sock))
        dst_qmp = connect_qmp(str(dst_sock))
        if not in_memory_guest_latency:
            wait_sock(str(src_heartbeat_sock), src, proc_list)
            wait_sock(str(dst_heartbeat_sock), dst, proc_list)
            heartbeat_collector = GuestHeartbeatCollector({
                "src": connect_stream_socket(str(src_heartbeat_sock)),
                "dst": connect_stream_socket(str(dst_heartbeat_sock)),
            })
            heartbeat_collector.start()
        qemu_perf_proc, qemu_perf_info = start_qemu_perf(
            case_dir, src, dst, enabled=qemu_perf,
        )

        set_caps(src_qmp, mode, rdma_pin_all=rdma_pin_all)
        set_caps(dst_qmp, mode, rdma_pin_all=rdma_pin_all)
        set_params(src_qmp, cxl_backing_path, mode, pressure,
                   shared_backing=True, thresholds=profile,
                   prefetch_rate=prefetch_rate,
                   dst_install_policy=dst_install_policy,
                   fault_control_plane=fault_control_plane,
                   fault_resolve_mode=fault_resolve_mode,
                   max_bandwidth=max_bandwidth,
                   brake_remap_granule=brake_remap_granule,
                   brake_enable=brake_enable,
                   switch_remap_coverage=switch_remap_coverage,
                   cxl_backing_rate=cxl_backing_rate,
                   clean_remap_enable=clean_remap_enable,
                   clean_remap_copy_budget=clean_remap_copy_budget,
                   clean_remap_throttle_us=clean_remap_throttle_us,
                   clean_remap_prefault_mode=clean_remap_prefault_mode,
                   max_postcopy_bandwidth=max_postcopy_bandwidth)
        set_params(dst_qmp, cxl_backing_path, mode, pressure,
                   shared_backing=True, thresholds=profile,
                   prefetch_rate=prefetch_rate,
                   dst_install_policy=dst_install_policy,
                   fault_control_plane=fault_control_plane,
                   fault_resolve_mode=fault_resolve_mode,
                   max_bandwidth=max_bandwidth,
                   brake_remap_granule=brake_remap_granule,
                   brake_enable=brake_enable,
                   switch_remap_coverage=switch_remap_coverage,
                   cxl_backing_rate=cxl_backing_rate,
                   clean_remap_enable=clean_remap_enable,
                   clean_remap_copy_budget=clean_remap_copy_budget,
                   clean_remap_throttle_us=clean_remap_throttle_us,
                   clean_remap_prefault_mode=clean_remap_prefault_mode,
                   max_postcopy_bandwidth=max_postcopy_bandwidth)

        qmp_ok(dst_qmp, "migrate-incoming", {"uri": migration_uri})
        qmp_ok(src_qmp, "cont")
        qmp_ok(dst_qmp, "cont")
        time.sleep(0.5)

        migration_start_ts = time.time()
        qmp_ok(src_qmp, "migrate", {"uri": migration_uri})
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
        stop_qemu_perf(qemu_perf_proc, qemu_perf_info)
        qemu_perf_proc = None
        migration_complete_ts = first_status_ts(samples, "completed")
        if migration_complete_ts is None and samples:
            migration_complete_ts = samples[-1].get("ts")
        corrected_start_ts, corrected_complete_ts = (
            corrected_window_from_total_time(
                migration_start_ts, migration_complete_ts, last
            )
        )
        if in_memory_guest_latency:
            heartbeat_events_for_in_memory = (
                heartbeat_collector.snapshot() if heartbeat_collector else []
            )
            try:
                qmp_ok(dst_qmp, "stop")
            except Exception:
                pass
            latency_conn = dst_qmp
            latency_path = in_memory_latency_file
            latency_marker_path = in_memory_marker_file
            latency_kwargs = {
                "fallback_conn": src_qmp,
                "fallback_path": in_memory_latency_src_file,
                "fallback_marker_path": in_memory_marker_src_file,
            }
            if in_memory_guest_latency_source_first:
                latency_conn = src_qmp
                latency_path = in_memory_latency_src_file
                latency_marker_path = in_memory_marker_src_file
                latency_kwargs = {
                    "primary_dump_source": "source-first",
                }
            guest_in_memory_latency = collect_in_memory_guest_latency(
                latency_conn,
                latency_path,
                pressure,
                marker_path=latency_marker_path,
                **latency_kwargs,
                heartbeat_events=heartbeat_events_for_in_memory,
                migration_start_ts=migration_start_ts,
                migration_complete_ts=migration_complete_ts,
                corrected_start_ts=corrected_start_ts,
                corrected_complete_ts=corrected_complete_ts,
                marker_start_event=1,
                marker_complete_event=6
                if mode_uses_postcopy(mode) else 5,
            )
        heartbeat_events = collect_heartbeat_events_with_grace(
            heartbeat_collector
        )
        write_json(heartbeats_file, heartbeat_events)
        guest_latency = summarize_guest_heartbeats(
            heartbeat_events,
            migration_start_ts=migration_start_ts,
            migration_complete_ts=migration_complete_ts,
            corrected_start_ts=corrected_start_ts,
            corrected_complete_ts=corrected_complete_ts,
        )
        observed_window_ms = observed_migration_window_ms(
            migration_start_ts, migration_complete_ts
        )
        qemu_total_time_window_ms = last.get("total-time")
        qmp_poll_tail_ms = None
        if observed_window_ms is not None and qemu_total_time_window_ms is not None:
            qmp_poll_tail_ms = max(
                observed_window_ms - float(qemu_total_time_window_ms), 0.0
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
        handoff_breakdown = summarize_handoff_breakdown(
            src_trace_file,
            dst_trace_file,
            guest_latency=guest_latency,
            trace_counts=trace_counts,
            summary=summary,
            heartbeat_events=heartbeat_events,
        )
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
                "qemu_total_time_window_ms": qemu_total_time_window_ms,
                "qmp_poll_tail_ms": qmp_poll_tail_ms,
            },
            "guest_latency": guest_latency,
            "guest_in_memory_latency": guest_in_memory_latency,
            "handoff_breakdown": handoff_breakdown,
            "trace": trace_counts,
            "summary": summary,
            "postcopy_control": postcopy_control,
            "qemu_perf": qemu_perf_info,
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
                "qemu_total_time_window_ms": qemu_total_time_window_ms,
                "qmp_poll_tail_ms": qmp_poll_tail_ms,
            },
            "guest_latency": guest_latency,
            "guest_in_memory_latency": guest_in_memory_latency,
            "handoff_breakdown": handoff_breakdown,
            "trace": trace_counts,
            "summary": summary,
            "postcopy_control": postcopy_control,
            "qemu_perf": qemu_perf_info,
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
        stop_qemu_perf(qemu_perf_proc, qemu_perf_info)
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
    guest_in_memory = (result or {}).get("guest_in_memory_latency") or {}
    inmem_corrected = guest_in_memory.get("corrected_window") or {}
    inmem_corrected_no_epoch = (
        guest_in_memory.get("corrected_window_no_epoch_marker") or {}
    )
    inmem_precopy = guest_in_memory.get("precopy_window") or {}
    inmem_precopy_bulk = guest_in_memory.get("precopy_bulk_window") or {}
    inmem_precopy_brake = guest_in_memory.get("precopy_brake_window") or {}
    inmem_postcopy_handoff = (
        guest_in_memory.get("postcopy_handoff_window") or {}
    )
    inmem_postcopy = guest_in_memory.get("postcopy_window") or {}
    inmem_completion_tail = (
        guest_in_memory.get("completion_tail_window") or {}
    )
    final_x_cxl = (result or {}).get("final_x_cxl") or {}
    corrected_latency = guest_latency.get("corrected_window") or {}
    handoff_breakdown = (result or {}).get("handoff_breakdown") or {}

    def mean_ns(total_key, samples_key):
        samples = summary.get(samples_key, 0)
        if not samples:
            return 0
        return summary.get(total_key, 0) // samples

    rdma_ready_pages_lost = max(
        summary.get("rdma_ready_pages_lost", 0),
        trace.get("rdma_ready_pages_lost", 0),
    )
    cxl_republish_pages_due_to_rdma_invalidate = max(
        summary.get("cxl_republish_pages_due_to_rdma_invalidate", 0),
        trace.get("cxl_republish_pages_due_to_rdma_invalidate", 0),
    )
    rdma_amplification = (
        cxl_republish_pages_due_to_rdma_invalidate /
        max(rdma_ready_pages_lost, 1)
    )

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
        if mode_uses_cxl_hybrid(mode) else 0,
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
        "max_dst_region_map_attempts":
            summary.get("max_dst_region_map_attempts", 0),
        "max_dst_region_map_successes":
            summary.get("max_dst_region_map_successes", 0),
        "max_dst_region_map_failures":
            summary.get("max_dst_region_map_failures", 0),
        "max_dst_region_map_time_ns":
            summary.get("max_dst_region_map_time_ns", 0),
        "dst_region_map_mean_ns": (
            summary.get("max_dst_region_map_time_ns", 0) //
            summary.get("max_dst_region_map_attempts", 0)
            if summary.get("max_dst_region_map_attempts", 0) else 0
        ),
        "max_dst_region_wake_failures":
            summary.get("max_dst_region_wake_failures", 0),
        "dst_region_wait_samples":
            summary.get("dst_region_wait_samples", 0),
        "dst_region_wait_time_ns":
            summary.get("dst_region_wait_time_ns", 0),
        "max_dst_region_wait_time_ns":
            summary.get("max_dst_region_wait_time_ns", 0),
        "dst_region_wait_mean_ns":
            mean_ns("dst_region_wait_time_ns", "dst_region_wait_samples"),
        "max_dst_region_fallback_copies":
            summary.get("max_dst_region_fallback_copies", 0),
        "region_publish_requests":
            summary.get("region_publish_requests", 0),
        "region_publish_pages":
            summary.get("region_publish_pages", 0),
        "region_publish_time_ns":
            summary.get("region_publish_time_ns", 0),
        "region_publish_mean_ns":
            mean_ns("region_publish_time_ns", "region_publish_requests"),
        "rdma_ready_regions":
            max(summary.get("rdma_ready_regions", 0),
                trace.get("rdma_ready_regions", 0)),
        "rdma_ready_pages":
            max(summary.get("rdma_ready_pages", 0),
                trace.get("rdma_ready_pages", 0)),
        "rdma_invalidated_regions":
            max(summary.get("rdma_invalidated_regions", 0),
                trace.get("rdma_invalidated_regions", 0)),
        "rdma_ready_pages_lost":
            rdma_ready_pages_lost,
        "cxl_republish_regions_due_to_rdma_invalidate":
            max(
                summary.get(
                    "cxl_republish_regions_due_to_rdma_invalidate", 0),
                trace.get(
                    "cxl_republish_regions_due_to_rdma_invalidate", 0)),
        "cxl_republish_pages_due_to_rdma_invalidate":
            cxl_republish_pages_due_to_rdma_invalidate,
        "rdma_invalidate_publish_amplification":
            max(summary.get("rdma_invalidate_publish_amplification", 0.0),
                trace.get("rdma_invalidate_publish_amplification", 0.0),
                rdma_amplification),
        "trace_region_publish_complete":
            trace.get("region_publish_complete", 0),
        "trace_region_publish_pages":
            trace.get("region_publish_pages", 0),
        "trace_region_publish_published_pages":
            trace.get("region_publish_published_pages", 0),
        "trace_region_wait_begin": trace.get("region_wait_begin", 0),
        "trace_region_wait_complete": trace.get("region_wait_complete", 0),
        "trace_region_wait_complete_failures":
            trace.get("region_wait_complete_failures", 0),
        "trace_dst_region_remap": trace.get("dst_region_remap", 0),
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
        "final_backing_write_bytes":
            final_x_cxl.get("backing-write-bytes", 0),
        "final_remap_coverage":
            final_x_cxl.get("remap-coverage", 0),
        "clean_remap_scan_calls":
            final_x_cxl.get("clean-remap-scan-calls", 0),
        "clean_remap_candidate_regions":
            final_x_cxl.get("clean-remap-candidate-regions", 0),
        "clean_remap_copy_bytes":
            final_x_cxl.get("clean-remap-copy-bytes", 0),
        "clean_remap_copy_time_ns":
            final_x_cxl.get("clean-remap-copy-time-ns", 0),
        "clean_remap_abandoned_dirty":
            final_x_cxl.get("clean-remap-abandoned-dirty", 0),
        "clean_remap_budget_exhaustions":
            final_x_cxl.get("clean-remap-budget-exhaustions", 0),
        "clean_remap_pending_bytes":
            final_x_cxl.get("clean-remap-pending-bytes", 0),
        "clean_remap_coverage":
            final_x_cxl.get("clean-remap-coverage", 0),
        "clean_remap_prefault_bytes":
            final_x_cxl.get("clean-remap-prefault-bytes", 0),
        "clean_remap_prefault_time_ns":
            final_x_cxl.get("clean-remap-prefault-time-ns", 0),
        "clean_remap_prefault_errors":
            final_x_cxl.get("clean-remap-prefault-errors", 0),
        "total_time_ms": latency.get("total_time_ms"),
        "setup_time_ms": latency.get("setup_time_ms"),
        "downtime_ms": latency.get("downtime_ms"),
        "stop_to_start_time_ms": latency.get("stop_to_start_time_ms"),
        "observed_migration_window_ms":
            latency.get("observed_migration_window_ms"),
        "qemu_total_time_window_ms":
            latency.get("qemu_total_time_window_ms"),
        "qmp_poll_tail_ms":
            latency.get("qmp_poll_tail_ms"),
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
        "guest_corrected_total_stall_during_migration_ms":
            corrected_latency.get("total_stall_during_migration_ms"),
        "guest_corrected_max_gap_stall_ms":
            corrected_latency.get("max_gap_stall_ms"),
        "guest_corrected_handoff_stall_ms":
            corrected_latency.get("handoff_stall_ms"),
        "guest_corrected_first_destination_gap_stall_ms":
            corrected_latency.get("first_destination_gap_stall_ms"),
        "guest_corrected_destination_rest_stall_ms":
            corrected_latency.get("destination_rest_stall_ms"),
        "guest_in_memory_valid": guest_in_memory.get("valid"),
        "guest_in_memory_samples_read":
            guest_in_memory.get("samples_read"),
        "guest_in_memory_epoch_markers":
            guest_in_memory.get("epoch_markers"),
        "guest_in_memory_baseline_cycles":
            guest_in_memory.get("baseline_cycles"),
        "guest_in_memory_corrected_total_excess_ms":
            inmem_corrected.get("total_excess_ms"),
        "guest_in_memory_corrected_max_excess_us":
            inmem_corrected.get("max_excess_us"),
        "guest_in_memory_corrected_no_epoch_total_excess_ms":
            inmem_corrected_no_epoch.get("total_excess_ms"),
        "guest_in_memory_corrected_no_epoch_max_excess_us":
            inmem_corrected_no_epoch.get("max_excess_us"),
        "guest_in_memory_precopy_total_excess_ms":
            inmem_precopy.get("total_excess_ms"),
        "guest_in_memory_precopy_max_excess_us":
            inmem_precopy.get("max_excess_us"),
        "guest_in_memory_precopy_bulk_total_excess_ms":
            inmem_precopy_bulk.get("total_excess_ms"),
        "guest_in_memory_precopy_bulk_max_excess_us":
            inmem_precopy_bulk.get("max_excess_us"),
        "guest_in_memory_precopy_brake_total_excess_ms":
            inmem_precopy_brake.get("total_excess_ms"),
        "guest_in_memory_precopy_brake_max_excess_us":
            inmem_precopy_brake.get("max_excess_us"),
        "guest_in_memory_postcopy_handoff_total_excess_ms":
            inmem_postcopy_handoff.get("total_excess_ms"),
        "guest_in_memory_postcopy_handoff_max_excess_us":
            inmem_postcopy_handoff.get("max_excess_us"),
        "guest_in_memory_postcopy_total_excess_ms":
            inmem_postcopy.get("total_excess_ms"),
        "guest_in_memory_postcopy_max_excess_us":
            inmem_postcopy.get("max_excess_us"),
        "guest_in_memory_completion_tail_total_excess_ms":
            inmem_completion_tail.get("total_excess_ms"),
        "guest_in_memory_completion_tail_max_excess_us":
            inmem_completion_tail.get("max_excess_us"),
        "handoff_control_src_estimate_to_enter_brake_ms":
            handoff_breakdown.get(
                "control_src_estimate_to_enter_brake_ms"),
        "handoff_control_src_start_to_enter_brake_ms":
            handoff_breakdown.get("control_src_start_to_enter_brake_ms"),
        "handoff_control_src_enter_brake_to_request_postcopy_ms":
            handoff_breakdown.get(
                "control_src_enter_brake_to_request_postcopy_ms"),
        "handoff_control_src_request_postcopy_to_postcopy_start_ms":
            handoff_breakdown.get(
                "control_src_request_postcopy_to_postcopy_start_ms"),
        "handoff_control_src_start_to_dst_vm_started_ms":
            handoff_breakdown.get("control_src_start_to_dst_vm_started_ms"),
        "handoff_control_src_start_to_dst_ack_ms":
            handoff_breakdown.get("control_src_start_to_dst_ack_ms"),
        "handoff_control_src_start_to_downtime_end_ms":
            handoff_breakdown.get("control_src_start_to_downtime_end_ms"),
        "handoff_control_src_downtime_end_to_postcopy_active_ms":
            handoff_breakdown.get(
                "control_src_downtime_end_to_postcopy_active_ms"),
        "handoff_control_src_postcopy_active_to_completion_enter_ms":
            handoff_breakdown.get(
                "control_src_postcopy_active_to_completion_enter_ms"),
        "handoff_control_src_completion_enter_to_prepare_done_ms":
            handoff_breakdown.get(
                "control_src_completion_enter_to_prepare_done_ms"),
        "handoff_control_src_completion_enter_to_completed_ms":
            handoff_breakdown.get(
                "control_src_completion_enter_to_completed_ms"),
        "handoff_control_src_completion_prepare_done_to_completed_ms":
            handoff_breakdown.get(
                "control_src_completion_prepare_done_to_completed_ms"),
        "handoff_control_src_downtime_end_to_dst_run_ms":
            handoff_breakdown.get("control_src_downtime_end_to_dst_run_ms"),
        "handoff_control_dst_run_to_vm_started_ms":
            handoff_breakdown.get("control_dst_run_to_vm_started_ms"),
        "handoff_control_dst_vm_started_to_ack_us":
            handoff_breakdown.get("control_dst_vm_started_to_ack_us"),
        "handoff_guest_baseline_gap_ms":
            handoff_breakdown.get("guest_baseline_gap_ms"),
        "handoff_guest_handoff_gap_ms":
            handoff_breakdown.get("guest_handoff_gap_ms"),
        "handoff_guest_handoff_stall_ms":
            handoff_breakdown.get("guest_handoff_stall_ms"),
        "handoff_src_tail_gap_mean_ms":
            handoff_breakdown.get("src_tail_gap_mean_ms"),
        "handoff_src_tail_gap_max_ms":
            handoff_breakdown.get("src_tail_gap_max_ms"),
        "handoff_dst_initial_gap_1_ms":
            handoff_breakdown.get("dst_initial_gap_1_ms"),
        "handoff_dst_initial_gap_2_ms":
            handoff_breakdown.get("dst_initial_gap_2_ms"),
        "handoff_dst_initial_excess_stall_ms":
            handoff_breakdown.get("dst_initial_excess_stall_ms"),
        "handoff_postcopy_fault_request":
            handoff_breakdown.get("postcopy_fault_request"),
        "handoff_postcopy_fault_read_time_ns":
            handoff_breakdown.get("postcopy_fault_read_time_ns"),
        "handoff_postcopy_fault_place_time_ns":
            handoff_breakdown.get("postcopy_fault_place_time_ns"),
        "handoff_postcopy_fault_publish_wait_time_ns":
            handoff_breakdown.get("postcopy_fault_publish_wait_time_ns"),
        "handoff_postcopy_fault_total_time_ns":
            handoff_breakdown.get("postcopy_fault_total_time_ns"),
        "handoff_trace_dst_region_remap":
            handoff_breakdown.get("trace_dst_region_remap"),
        "handoff_postcopy_region_wait_samples":
            handoff_breakdown.get("postcopy_region_wait_samples"),
        "handoff_postcopy_region_wait_time_ns":
            handoff_breakdown.get("postcopy_region_wait_time_ns"),
        "handoff_dst_region_wait_time_ns":
            handoff_breakdown.get("dst_region_wait_time_ns"),
        "handoff_max_dst_region_wait_time_ns":
            handoff_breakdown.get("max_dst_region_wait_time_ns"),
        "handoff_postcopy_region_publish_requests":
            handoff_breakdown.get("postcopy_region_publish_requests"),
        "handoff_postcopy_region_publish_pages":
            handoff_breakdown.get("postcopy_region_publish_pages"),
        "handoff_postcopy_region_publish_time_ns":
            handoff_breakdown.get("postcopy_region_publish_time_ns"),
        "handoff_max_dst_region_map_time_ns":
            handoff_breakdown.get("max_dst_region_map_time_ns"),
        "handoff_ram_save_queue_pages":
            handoff_breakdown.get("ram_save_queue_pages"),
        "handoff_get_queued_page":
            handoff_breakdown.get("get_queued_page"),
        "handoff_region_publish_pages":
            handoff_breakdown.get("region_publish_pages"),
        "handoff_region_publish_time_ns":
            handoff_breakdown.get("region_publish_time_ns"),
        "handoff_ram_stream_publish_span_pages":
            handoff_breakdown.get("ram_stream_publish_span_pages"),
        "handoff_ram_stream_publish_span_written_pages":
            handoff_breakdown.get("ram_stream_publish_span_written_pages"),
        "handoff_ram_stream_publish_span_source_remapped_pages":
            handoff_breakdown.get(
                "ram_stream_publish_span_source_remapped_pages"),
        "handoff_ram_stream_publish_span_destination_owned_pages":
            handoff_breakdown.get(
                "ram_stream_publish_span_destination_owned_pages"),
        "handoff_ram_stream_publish_span_already_visible_pages":
            handoff_breakdown.get(
                "ram_stream_publish_span_already_visible_pages"),
        "handoff_ram_stream_publish_span_unknown_reason_pages":
            handoff_breakdown.get(
                "ram_stream_publish_span_unknown_reason_pages"),
        "ram_stream_publish_span":
            trace.get("ram_stream_publish_span", 0),
        "ram_stream_publish_span_pages":
            trace.get("ram_stream_publish_span_pages", 0),
        "ram_stream_publish_span_max_pages":
            trace.get("ram_stream_publish_span_max_pages", 0),
        "ram_stream_publish_span_single_page":
            trace.get("ram_stream_publish_span_single_page", 0),
        "ram_stream_publish_span_written_pages":
            trace.get("ram_stream_publish_span_written_pages", 0),
        "ram_stream_publish_span_source_remapped_pages":
            trace.get("ram_stream_publish_span_source_remapped_pages", 0),
        "ram_stream_publish_span_destination_owned_pages":
            trace.get("ram_stream_publish_span_destination_owned_pages", 0),
        "ram_stream_publish_span_already_visible_pages":
            trace.get("ram_stream_publish_span_already_visible_pages", 0),
        "ram_stream_publish_span_unknown_reason_pages":
            trace.get("ram_stream_publish_span_unknown_reason_pages", 0),
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
        "qemu_total_time_window_ms",
        "qmp_poll_tail_ms",
        "downtime_ms",
        "stop_to_start_time_ms",
        "guest_total_gap_during_migration_ms",
        "guest_total_stall_during_migration_ms",
        "guest_max_gap_stall_ms",
        "guest_corrected_total_stall_during_migration_ms",
        "guest_corrected_max_gap_stall_ms",
        "guest_corrected_handoff_stall_ms",
        "guest_corrected_first_destination_gap_stall_ms",
        "guest_corrected_destination_rest_stall_ms",
        "guest_in_memory_samples_read",
        "guest_in_memory_epoch_markers",
        "guest_in_memory_baseline_cycles",
        "guest_in_memory_corrected_total_excess_ms",
        "guest_in_memory_corrected_max_excess_us",
        "guest_in_memory_corrected_no_epoch_total_excess_ms",
        "guest_in_memory_corrected_no_epoch_max_excess_us",
        "guest_in_memory_precopy_total_excess_ms",
        "guest_in_memory_precopy_max_excess_us",
        "guest_in_memory_precopy_bulk_total_excess_ms",
        "guest_in_memory_precopy_bulk_max_excess_us",
        "guest_in_memory_precopy_brake_total_excess_ms",
        "guest_in_memory_precopy_brake_max_excess_us",
        "guest_in_memory_postcopy_handoff_total_excess_ms",
        "guest_in_memory_postcopy_handoff_max_excess_us",
        "guest_in_memory_postcopy_total_excess_ms",
        "guest_in_memory_postcopy_max_excess_us",
        "guest_in_memory_completion_tail_total_excess_ms",
        "guest_in_memory_completion_tail_max_excess_us",
        "handoff_control_src_estimate_to_enter_brake_ms",
        "handoff_control_src_start_to_enter_brake_ms",
        "handoff_control_src_enter_brake_to_request_postcopy_ms",
        "handoff_control_src_request_postcopy_to_postcopy_start_ms",
        "handoff_control_src_start_to_dst_vm_started_ms",
        "handoff_control_src_start_to_dst_ack_ms",
        "handoff_control_src_start_to_downtime_end_ms",
        "handoff_control_src_downtime_end_to_postcopy_active_ms",
        "handoff_control_src_postcopy_active_to_completion_enter_ms",
        "handoff_control_src_completion_enter_to_prepare_done_ms",
        "handoff_control_src_completion_enter_to_completed_ms",
        "handoff_control_src_completion_prepare_done_to_completed_ms",
        "handoff_control_src_downtime_end_to_dst_run_ms",
        "handoff_control_dst_run_to_vm_started_ms",
        "handoff_control_dst_vm_started_to_ack_us",
        "handoff_guest_baseline_gap_ms",
        "handoff_guest_handoff_gap_ms",
        "handoff_guest_handoff_stall_ms",
        "handoff_src_tail_gap_mean_ms",
        "handoff_src_tail_gap_max_ms",
        "handoff_dst_initial_gap_1_ms",
        "handoff_dst_initial_gap_2_ms",
        "handoff_dst_initial_excess_stall_ms",
        "handoff_postcopy_fault_request",
        "handoff_postcopy_fault_read_time_ns",
        "handoff_postcopy_fault_place_time_ns",
        "handoff_postcopy_fault_publish_wait_time_ns",
        "handoff_postcopy_fault_total_time_ns",
        "handoff_trace_dst_region_remap",
        "handoff_postcopy_region_wait_samples",
        "handoff_postcopy_region_wait_time_ns",
        "handoff_dst_region_wait_time_ns",
        "handoff_max_dst_region_wait_time_ns",
        "handoff_max_dst_region_map_time_ns",
        "handoff_postcopy_region_publish_requests",
        "handoff_postcopy_region_publish_pages",
        "handoff_postcopy_region_publish_time_ns",
        "handoff_ram_save_queue_pages",
        "handoff_get_queued_page",
        "handoff_region_publish_pages",
        "handoff_region_publish_time_ns",
        "handoff_ram_stream_publish_span_pages",
        "handoff_ram_stream_publish_span_written_pages",
        "handoff_ram_stream_publish_span_source_remapped_pages",
        "handoff_ram_stream_publish_span_destination_owned_pages",
        "handoff_ram_stream_publish_span_already_visible_pages",
        "handoff_ram_stream_publish_span_unknown_reason_pages",
        "ram_stream_publish_span",
        "ram_stream_publish_span_pages",
        "ram_stream_publish_span_max_pages",
        "ram_stream_publish_span_single_page",
        "ram_stream_publish_span_written_pages",
        "ram_stream_publish_span_source_remapped_pages",
        "ram_stream_publish_span_destination_owned_pages",
        "ram_stream_publish_span_already_visible_pages",
        "ram_stream_publish_span_unknown_reason_pages",
        "max_fault_publish_waits",
        "max_fault_publish_wait_time_ns",
        "max_dst_fault_read_time_ns",
        "fault_hit_read_time_ns",
        "max_fault_hit_read_time_ns",
        "fault_place_time_ns",
        "max_fault_place_time_ns",
        "max_dst_region_map_successes",
        "max_dst_region_map_failures",
        "max_dst_region_map_time_ns",
        "dst_region_map_mean_ns",
        "dst_region_wait_samples",
        "dst_region_wait_time_ns",
        "max_dst_region_wait_time_ns",
        "dst_region_wait_mean_ns",
        "max_dst_region_fallback_copies",
        "region_publish_requests",
        "region_publish_pages",
        "region_publish_time_ns",
        "region_publish_mean_ns",
        "rdma_ready_regions",
        "rdma_ready_pages",
        "rdma_invalidated_regions",
        "rdma_ready_pages_lost",
        "cxl_republish_regions_due_to_rdma_invalidate",
        "cxl_republish_pages_due_to_rdma_invalidate",
        "rdma_invalidate_publish_amplification",
        "trace_dst_region_remap",
        "final_backing_write_bytes",
        "final_remap_coverage",
        "clean_remap_scan_calls",
        "clean_remap_candidate_regions",
        "clean_remap_copy_bytes",
        "clean_remap_copy_time_ns",
        "clean_remap_abandoned_dirty",
        "clean_remap_budget_exhaustions",
        "clean_remap_pending_bytes",
        "clean_remap_coverage",
        "clean_remap_prefault_bytes",
        "clean_remap_prefault_time_ns",
        "clean_remap_prefault_errors",
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
        "guest_in_memory_latency": payload.get("guest_in_memory_latency"),
        "handoff_breakdown": payload.get("handoff_breakdown") or {},
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
                        cxl_path_override=None, max_bandwidth=0,
                        max_postcopy_bandwidth=None,
                        cxl_backing_rate=None,
                        brake_remap_granule=REMAP_GRANULE,
                        brake_enable=True,
                        switch_remap_coverage=None,
                        clean_remap_enable=False,
                        clean_remap_copy_budget=None,
                        clean_remap_throttle_us=None,
                        clean_remap_prefault_mode=None,
                        clean_remap_debug_mode=None,
                        rdma_host=DEFAULT_RDMA_HOST,
                        rdma_port=DEFAULT_RDMA_PORT,
                        rdma_pin_all=False,
                        accel="tcg",
                        qemu_perf=False,
                        in_memory_guest_latency=False,
                        in_memory_guest_latency_source_first=False):
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
                kwargs["max_bandwidth"] = max_bandwidth
                if max_postcopy_bandwidth is not None:
                    kwargs["max_postcopy_bandwidth"] = max_postcopy_bandwidth
                if cxl_backing_rate is not None:
                    kwargs["cxl_backing_rate"] = cxl_backing_rate
                kwargs["brake_remap_granule"] = brake_remap_granule
                if not brake_enable:
                    kwargs["brake_enable"] = False
                kwargs["accel"] = accel
                kwargs["qemu_perf"] = qemu_perf
                if in_memory_guest_latency:
                    kwargs["in_memory_guest_latency"] = True
                if in_memory_guest_latency_source_first:
                    kwargs["in_memory_guest_latency_source_first"] = True
                if switch_remap_coverage is not None:
                    kwargs["switch_remap_coverage"] = switch_remap_coverage
                kwargs["clean_remap_enable"] = clean_remap_enable
                if clean_remap_copy_budget is not None:
                    kwargs["clean_remap_copy_budget"] = clean_remap_copy_budget
                if clean_remap_throttle_us is not None:
                    kwargs["clean_remap_throttle_us"] = clean_remap_throttle_us
                if clean_remap_prefault_mode is not None:
                    kwargs["clean_remap_prefault_mode"] = (
                        clean_remap_prefault_mode
                    )
                if clean_remap_debug_mode is not None:
                    kwargs["clean_remap_debug_mode"] = clean_remap_debug_mode
                if mode_uses_rdma(mode):
                    kwargs["rdma_host"] = rdma_host
                    kwargs["rdma_port"] = rdma_port + run_index - 1
                    kwargs["rdma_pin_all"] = rdma_pin_all
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


def parse_args(argv=None):
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
    parser.add_argument("--x-cxl-switch-remap-coverage", type=int)
    parser.add_argument("--x-cxl-brake-remap-granule", type=int,
                        default=REMAP_GRANULE)
    parser.add_argument("--x-cxl-disable-brake",
                        dest="x_cxl_brake_enable",
                        action="store_false",
                        default=True,
                        help="Disable hybrid brake phase; switch directly from bulk to postcopy")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--migration-timeout", type=float,
                        default=DEFAULT_MIGRATION_TIMEOUT_SECS)
    parser.add_argument("--max-bandwidth", type=int, default=0)
    parser.add_argument("--max-postcopy-bandwidth", type=int)
    parser.add_argument("--x-cxl-backing-rate", type=int)
    parser.add_argument("--x-cxl-clean-remap-enable",
                        action="store_true",
                        help="Enable experimental CXL clean-remap-only precopy")
    parser.add_argument("--x-cxl-clean-remap-copy-budget",
                        type=int,
                        default=None,
                        help="Bytes copied by one CXL clean-remap drain")
    parser.add_argument("--x-cxl-clean-remap-throttle-us",
                        type=int,
                        default=None,
                        help="Microseconds to sleep after each clean-remap region copy")
    parser.add_argument("--x-cxl-clean-remap-prefault-mode",
                        choices=("off", "madvise", "touch"),
                        default=None,
                        help="Synchronously prefault clean-remap CXL copy targets")
    parser.add_argument("--x-cxl-clean-remap-debug-mode",
                        choices=("scan-only", "copy-only", "read-only",
                                 "write-only", "defer-remap"),
                        default=None,
                        help="Debug decomposition mode for CXL clean-remap")
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
    parser.add_argument("--rdma-host", default=DEFAULT_RDMA_HOST)
    parser.add_argument("--rdma-port", type=int, default=DEFAULT_RDMA_PORT)
    parser.add_argument("--rdma-pin-all", action="store_true")
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg",
                        help="QEMU accelerator for source and destination VMs")
    parser.add_argument("--qemu-perf", action="store_true",
                        help="Record perf data for source and destination QEMU PIDs")
    parser.add_argument("--in-memory-guest-latency", action="store_true",
                        help="Record per-sample guest TSC deltas in RAM and dump them after migration")
    parser.add_argument("--in-memory-guest-latency-source-first",
                        action="store_true",
                        help="Dump the in-memory latency log from source QMP first; source-side windows only")
    return parser.parse_args(argv)


def main():
    maybe_reexec_with_sudo()

    args = parse_args()

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
    if args.rdma_port <= 0 or args.rdma_port > 65535:
        raise SystemExit("rdma-port must be in 1..65535")
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
                                     cxl_path_override=args.cxl_path,
                                     max_bandwidth=args.max_bandwidth,
                                     max_postcopy_bandwidth=
                                     args.max_postcopy_bandwidth,
                                     cxl_backing_rate=
                                     args.x_cxl_backing_rate,
                                     brake_remap_granule=
                                     args.x_cxl_brake_remap_granule,
                                     brake_enable=args.x_cxl_brake_enable,
                                     switch_remap_coverage=
                                     args.x_cxl_switch_remap_coverage,
                                     clean_remap_enable=
                                     args.x_cxl_clean_remap_enable,
                                     clean_remap_copy_budget=
                                     args.x_cxl_clean_remap_copy_budget,
                                     clean_remap_throttle_us=
                                     args.x_cxl_clean_remap_throttle_us,
                                     clean_remap_prefault_mode=
                                     args.x_cxl_clean_remap_prefault_mode,
                                     clean_remap_debug_mode=
                                     args.x_cxl_clean_remap_debug_mode,
                                     rdma_host=args.rdma_host,
                                     rdma_port=args.rdma_port,
                                     rdma_pin_all=args.rdma_pin_all,
                                     accel=args.accel,
                                     qemu_perf=args.qemu_perf,
                                     in_memory_guest_latency=
                                     args.in_memory_guest_latency,
                                     in_memory_guest_latency_source_first=
                                     args.in_memory_guest_latency_source_first)
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
