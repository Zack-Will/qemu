#!/usr/bin/env python3
import argparse
import csv
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
QEMU = REPO_ROOT / "build" / "qemu-system-x86_64"
BOOT_ASM = REPO_ROOT / "scripts" / "cxl-hybrid-warm-boot.asm"

PAGE_SIZE = 4096
GUEST_RAM_BYTES = 64 * 1024 * 1024
REMAP_GRANULE = 2 * 1024 * 1024
MULTIFD_CHANNELS = 2
QMP_TIMEOUT_SECS = 1.0
SAMPLE_INTERVAL_SECS = 0.02
COMPLETION_TAIL_SAMPLES = 2
DEFAULT_MIGRATION_TIMEOUT_SECS = 120.0
DEFAULT_RDMA_HOST = "10.0.0.2"
DEFAULT_RDMA_PORT = 4444

PRESSURES = {
    "remap_xlarge_random_rw": {
        "start_addr": 0x00100000,
        "end_addr": 0x01100000,
        "writes_per_page": 1,
        "outer_spin": 0,
        "page_order_random": True,
        "access_pattern_random_rw": True,
        "random_page_stride": 73,
        "random_epoch_seed_step": 1,
        "prefetch_batch_pages": 1024,
    },
}
MODES = ("hybrid_parallel_rdma_cxl",)

THRESHOLDS = {
    "x-cxl-switch-dirty-threshold": 1,
    "x-cxl-switch-max-iters": 20,
    "x-cxl-switch-max-precopy-ms": 0,
    "x-cxl-switch-min-remaining": 8 * 1024 * 1024,
}

MINIMAL_TRACE_EVENTS = (
    "cxl_hybrid_phase_transition",
    "cxl_hybrid_rdma_bulk_region",
    "cxl_rdma_sidecar_connect_start",
    "cxl_rdma_sidecar_connect_complete",
    "cxl_rdma_sidecar_register",
    "cxl_rdma_sidecar_schedule",
    "cxl_rdma_sidecar_post",
    "cxl_rdma_sidecar_complete",
    "cxl_rdma_sidecar_stale",
    "cxl_rdma_sidecar_failed",
    "migration_precopy_timeline",
    "migration_postcopy_timeline",
)

DEBUG_TRACE_EVENTS = MINIMAL_TRACE_EVENTS + (
    "cxl_hybrid_publish_request_send",
    "cxl_hybrid_publish_request_recv",
    "cxl_hybrid_completion_prepare_begin",
    "cxl_hybrid_completion_prepare_end",
    "cxl_hybrid_publish_wait_begin",
    "cxl_hybrid_publish_wait_complete",
    "cxl_hybrid_region_publish_complete",
    "cxl_hybrid_rdma_cxl_priority",
    "cxl_hybrid_cxl_worker_enqueue",
    "cxl_hybrid_cxl_worker_complete",
    "cxl_hybrid_ram_stream_publish_span",
    "postcopy_ram_fault_thread_request",
    "postcopy_page_req_add",
    "postcopy_page_req_del",
    "postcopy_request_shared_page",
    "postcopy_request_shared_page_present",
    "ram_save_queue_pages",
    "get_queued_page",
    "get_queued_page_not_dirty",
)

TRACE_PROFILES = {
    "off": (),
    "minimal": MINIMAL_TRACE_EVENTS,
    "rdma-cxl-debug": DEBUG_TRACE_EVENTS,
}

STDERR_ERROR_RE = re.compile(r"\b(?:UFFD|cleanup|error|failed|Traceback)\b",
                             re.IGNORECASE)
TIMELINE_RE = re.compile(
    r"\bmigration_(?:precopy|postcopy)_timeline\s+(?P<stage>\S+)\b"
)

SUMMARY_FIELDS = [
    "run_dir",
    "mode",
    "pressure",
    "run_index",
    "final_status",
    "dst_running",
    "dst_status",
    "total_time_ms",
    "precopy_time_ms",
    "postcopy_time_ms",
    "cxl_worker_bytes",
    "cxl_worker_time_ns",
    "cxl_worker_precopy_bytes",
    "cxl_worker_precopy_time_ns",
    "cxl_worker_postcopy_bytes",
    "cxl_worker_postcopy_time_ns",
    "rdma_completed_bytes",
    "rdma_completed_time_ns",
    "rdma_precopy_completed_bytes",
    "rdma_precopy_completed_time_ns",
    "rdma_precopy_active_time_ns",
    "rdma_precopy_transport_completed_time_ns",
    "rdma_precopy_transport_active_time_ns",
    "rdma_precopy_publish_time_ns",
    "rdma_postcopy_dirty_completed_bytes",
    "rdma_postcopy_dirty_completed_time_ns",
    "rdma_postcopy_dirty_active_time_ns",
    "rdma_postcopy_dirty_transport_completed_time_ns",
    "rdma_postcopy_dirty_transport_active_time_ns",
    "rdma_postcopy_dirty_publish_time_ns",
    "rdma_postcopy_dirty_completed_spans",
    "rdma_postcopy_dirty_stale_pages",
    "rdma_dynamic_window_regions",
    "rdma_sq_capacity_regions",
    "rdma_bdp_estimate_regions",
    "rdma_admission_accepted_regions",
    "rdma_admission_overflow_cxl_regions",
    "rdma_admission_closed_events",
    "rdma_admission_goodput_drop_events",
    "rdma_postcopy_dirty_overflow_cxl_spans",
    "rdma_postcopy_dirty_min_span_cxl_spans",
    "stderr_error_count",
    "stderr_error_summary",
]


class ExperimentFailure(RuntimeError):
    def __init__(self, reason, payload=None):
        super().__init__(reason)
        self.reason = reason
        self.payload = payload or {}


class QMPConnection:
    def __init__(self, path):
        self.path = str(path)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.file = None

    def connect(self, timeout=10.0):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock.connect(self.path)
                self.sock.settimeout(QMP_TIMEOUT_SECS)
                self.file = self.sock.makefile("rwb", buffering=0)
                self._read_message()
                self.command("qmp_capabilities")
                return self
            except FileNotFoundError:
                if time.time() >= deadline:
                    raise
                time.sleep(0.02)
            except ConnectionRefusedError:
                if time.time() >= deadline:
                    raise
                time.sleep(0.02)

    def close(self):
        try:
            if self.file is not None:
                self.file.close()
        finally:
            self.sock.close()

    def _read_message(self):
        line = self.file.readline()
        if not line:
            raise RuntimeError(f"QMP closed: {self.path}")
        return json.loads(line.decode("utf-8"))

    def command(self, execute, arguments=None):
        payload = {"execute": execute}
        if arguments is not None:
            payload["arguments"] = arguments
        self.file.write(json.dumps(payload).encode("utf-8") + b"\r\n")
        while True:
            response = self._read_message()
            if "event" in response:
                continue
            if "error" in response:
                error = response["error"]
                raise RuntimeError(
                    f"{execute}: {error.get('class')}: {error.get('desc')}"
                )
            return response.get("return")


def positive_int(value):
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("value must be >= 1")
    return parsed


def non_negative_int(value):
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be >= 0")
    return parsed


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Run the converged RDMA/CXL parallel migration experiment")
    parser.add_argument("--pressure", choices=tuple(PRESSURES),
                        default="remap_xlarge_random_rw")
    parser.add_argument("--mode", choices=MODES,
                        default="hybrid_parallel_rdma_cxl")
    parser.add_argument("--repeat", type=positive_int, default=1)
    parser.add_argument("--keep-dir", action="store_true")
    parser.add_argument("--migration-timeout", type=float,
                        default=DEFAULT_MIGRATION_TIMEOUT_SECS)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--rdma-host", default=DEFAULT_RDMA_HOST)
    parser.add_argument("--rdma-port", type=positive_int,
                        default=DEFAULT_RDMA_PORT)
    parser.add_argument("--rdma-pin-all", action="store_true")
    parser.add_argument("--x-cxl-rdma-sidecar-max-inflight-regions",
                        type=non_negative_int, default=0)
    parser.add_argument("--x-cxl-rdma-cxl-priority-threshold-bytes",
                        type=non_negative_int, default=0)
    dirty = parser.add_mutually_exclusive_group()
    dirty.add_argument("--postcopy-dirty-rdma",
                       dest="postcopy_dirty_rdma",
                       action="store_true", default=True)
    dirty.add_argument("--no-postcopy-dirty-rdma",
                       dest="postcopy_dirty_rdma",
                       action="store_false")
    parser.add_argument("--postcopy-dirty-rdma-min-bytes",
                        type=positive_int, default=64 * 1024)
    parser.add_argument("--trace-profile", choices=tuple(TRACE_PROFILES),
                        default="minimal")
    parser.add_argument("--numactl-cpunodes")
    parser.add_argument("--numactl-memnodes")
    return parser.parse_args(argv)


def build_rdma_sidecar_address(args, run_index):
    return {
        "transport": "rdma",
        "host": args.rdma_host,
        "port": str(int(args.rdma_port) + int(run_index)),
    }


def build_migration_parameters(args, cxl_path, run_index=0):
    pressure = PRESSURES[args.pressure]
    params = {
        "max-bandwidth": 0,
        "multifd-channels": MULTIFD_CHANNELS,
        "cxl-path": cxl_path,
        "x-cxl-brake-remap-granule": REMAP_GRANULE,
        "x-cxl-switch-dirty-threshold":
            THRESHOLDS["x-cxl-switch-dirty-threshold"],
        "x-cxl-switch-max-iters":
            THRESHOLDS["x-cxl-switch-max-iters"],
        "x-cxl-switch-max-precopy-ms":
            THRESHOLDS["x-cxl-switch-max-precopy-ms"],
        "x-cxl-switch-min-remaining":
            THRESHOLDS["x-cxl-switch-min-remaining"],
        "x-cxl-brake-enable": True,
        "x-cxl-prefetch-rate": 0,
        "x-cxl-prefetch-batch-pages": pressure["prefetch_batch_pages"],
        "x-cxl-prefetch-heat-window-ms": 250,
        "x-cxl-shared-backing": True,
        "x-cxl-rdma-sidecar": True,
        "x-cxl-rdma-sidecar-address": build_rdma_sidecar_address(
            args, run_index),
        "x-cxl-rdma-sidecar-max-inflight-regions":
            args.x_cxl_rdma_sidecar_max_inflight_regions,
        "x-cxl-rdma-sidecar-postcopy-dirty": args.postcopy_dirty_rdma,
        "x-cxl-rdma-sidecar-postcopy-dirty-min-bytes":
            args.postcopy_dirty_rdma_min_bytes,
        "x-cxl-rdma-cxl-priority-threshold-bytes":
            args.x_cxl_rdma_cxl_priority_threshold_bytes,
    }
    return params


def trace_events_for_profile(profile):
    return list(TRACE_PROFILES[profile])


def build_boot_image(base, pressure_name):
    config = PRESSURES[pressure_name]
    img = base / f"boot-{pressure_name}.img"
    args = [
        "nasm",
        "-D", f"PRESSURE_START_ADDR=0x{config['start_addr']:08x}",
        "-D", f"PRESSURE_END_ADDR=0x{config['end_addr']:08x}",
        "-D", f"PRESSURE_WRITES_PER_PAGE={config['writes_per_page']}",
        "-D", f"PRESSURE_OUTER_SPIN={config['outer_spin']}",
        "-D", "PRESSURE_PAGE_ORDER_RANDOM=1",
        "-D", "PRESSURE_ACCESS_PATTERN_RANDOM_RW=1",
        "-D", f"PRESSURE_RANDOM_PAGE_STRIDE={config['random_page_stride']}",
        "-D",
        "PRESSURE_RANDOM_EPOCH_SEED_STEP="
        f"{config['random_epoch_seed_step']}",
        "-f", "bin",
        "-o", str(img),
        str(BOOT_ASM),
    ]
    subprocess.run(args, check=True)
    if img.stat().st_size != 512:
        raise RuntimeError(f"boot image size mismatch: {img.stat().st_size}")
    return img


def wait_for_socket(path, proc, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"process exited before socket appeared: {path}")
        if Path(path).exists():
            return
        time.sleep(0.02)
    raise TimeoutError(f"timed out waiting for socket: {path}")


def qmp_connect(path, proc=None):
    if proc is not None:
        wait_for_socket(path, proc)
    return QMPConnection(path).connect()


def qmp_probe(conn, command, arguments=None):
    try:
        return conn.command(command, arguments), None
    except Exception as exc:
        return None, str(exc)


def build_qemu_env(is_source):
    env = os.environ.copy()
    env.pop("QEMU_CXL_HYBRID_WARM_DISABLE", None)
    env["QEMU_CXL_WRITE_REDIRECT"] = "1" if is_source else "0"
    return env


def numactl_prefix(args):
    if not args.numactl_cpunodes and not args.numactl_memnodes:
        return []
    exe = shutil.which("numactl") or "/usr/local/bin/numactl"
    prefix = [exe]
    if args.numactl_cpunodes:
        prefix.append(f"--cpunodebind={args.numactl_cpunodes}")
    if args.numactl_memnodes:
        prefix.append(f"--membind={args.numactl_memnodes}")
    return prefix


def build_common_qemu_args(boot_img, accel):
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


def start_vm(common_args, qmp_sock, stderr_path, trace_file=None,
             trace_events_file=None, incoming=False, env=None, prefix=None):
    argv = list(prefix or []) + list(common_args)
    if trace_file and trace_events_file:
        argv.extend(["-trace", f"events={trace_events_file},file={trace_file}"])
    if incoming:
        argv.extend(["-incoming", "defer"])
    argv.extend(["-qmp", f"unix:{qmp_sock},server=on,wait=off"])
    stderr = open(stderr_path, "w", encoding="utf-8")
    try:
        return subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                                stderr=stderr, text=True, env=env)
    finally:
        stderr.close()


def stop_processes(procs):
    for proc in procs:
        if proc and proc.poll() is None:
            proc.terminate()
    deadline = time.time() + 3.0
    for proc in procs:
        if not proc:
            continue
        while proc.poll() is None and time.time() < deadline:
            time.sleep(0.02)
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=1.0)


def write_json(path, payload):
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


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
        "src-query-migrate": src_info,
        "dst-query-migrate": dst_info,
        "src-query-status": src_status,
        "dst-query-status": dst_status,
        "errors": errors,
    }


def collect_until_complete(src_qmp, dst_qmp, samples_path, timeout):
    deadline = time.time() + timeout
    tail = 0
    samples = []
    stop_reason = "timeout"
    while time.time() < deadline:
        sample = poll_migration_sample(src_qmp, dst_qmp)
        samples.append(sample)
        write_json(samples_path, samples)
        info = sample.get("src-query-migrate") or {}
        status = info.get("status")
        if status in ("failed", "cancelled"):
            stop_reason = status
            break
        if status == "completed":
            stop_reason = "completed"
            tail += 1
            if tail > COMPLETION_TAIL_SAMPLES:
                break
        else:
            tail = 0
        time.sleep(SAMPLE_INTERVAL_SECS)
    return samples, stop_reason


def latest_value(samples, key):
    for sample in reversed(samples):
        value = sample.get(key)
        if value is not None:
            return value
    return None


def xcxl_max(samples, qmp_key):
    best = 0
    for sample in samples:
        for side in ("src-query-migrate", "dst-query-migrate"):
            xcxl = (sample.get(side) or {}).get("x-cxl") or {}
            value = xcxl.get(qmp_key)
            if isinstance(value, bool):
                best = max(best, int(value))
            elif isinstance(value, (int, float)):
                best = max(best, value)
    return best


def parse_timeline(trace_file):
    timeline = {}
    if not trace_file or not trace_file.exists():
        return timeline
    for line in trace_file.read_text(
            encoding="utf-8", errors="replace").splitlines():
        match = TIMELINE_RE.search(line)
        if not match:
            continue
        now_match = re.search(r"\bnow_ns=(\d+)\b", line)
        if not now_match:
            continue
        timeline.setdefault(match.group("stage"), []).append(
            int(now_match.group(1)))
    return timeline


def first_timeline_ns(timeline, stage):
    values = timeline.get(stage) or []
    return values[0] if values else None


def elapsed_ms(start_ns, end_ns):
    if start_ns is None or end_ns is None or end_ns < start_ns:
        return None
    return (end_ns - start_ns) / 1e6


def timeline_summary(src_trace_file):
    timeline = parse_timeline(src_trace_file)
    start = (
        first_timeline_ns(timeline, "iterate-begin") or
        first_timeline_ns(timeline, "estimate")
    )
    request = first_timeline_ns(timeline, "request-postcopy")
    active = (
        first_timeline_ns(timeline, "state-postcopy-active") or
        first_timeline_ns(timeline, "postcopy-active")
    )
    completed = first_timeline_ns(timeline, "completed")
    return {
        "precopy_time_ms": elapsed_ms(start, request),
        "postcopy_time_ms": elapsed_ms(active, completed),
    }


def classify_stderr(stderr_by_source):
    matches = []
    for source, text in stderr_by_source.items():
        for lineno, line in enumerate(text.splitlines(), start=1):
            if STDERR_ERROR_RE.search(line):
                matches.append({
                    "source": source,
                    "line": lineno,
                    "text": line.strip(),
                })
    return {
        "error_count": len(matches),
        "matches": matches,
    }


def stderr_summary_text(stderr_summary):
    parts = []
    for item in stderr_summary.get("matches", []):
        parts.append(
            f"{item['source']}:{item['line']}:{item['text']}"
        )
    return "; ".join(parts)


def extract_summary(samples, run_dir, mode, pressure, run_index,
                    stderr_summary, timing=None):
    final_info = latest_value(samples, "src-query-migrate") or {}
    dst_status = latest_value(samples, "dst-query-status") or {}
    timing = timing or {}
    sidecar_postcopy_bytes = xcxl_max(
        samples, "rdma-sidecar-postcopy-dirty-completed-bytes")
    page_state_postcopy_bytes = xcxl_max(
        samples, "page-state-rdma-postcopy-dirty-completed-bytes")
    return {
        "run_dir": str(run_dir),
        "mode": mode,
        "pressure": pressure,
        "run_index": run_index,
        "final_status": final_info.get("status"),
        "dst_running": bool(dst_status.get("running")),
        "dst_status": dst_status.get("status"),
        "total_time_ms": final_info.get("total-time"),
        "precopy_time_ms": timing.get("precopy_time_ms"),
        "postcopy_time_ms": timing.get("postcopy_time_ms"),
        "cxl_worker_bytes": xcxl_max(
            samples, "page-state-cxl-worker-bytes"),
        "cxl_worker_time_ns": xcxl_max(
            samples, "page-state-cxl-worker-time-ns"),
        "cxl_worker_precopy_bytes": xcxl_max(
            samples, "page-state-cxl-worker-precopy-bytes"),
        "cxl_worker_precopy_time_ns": xcxl_max(
            samples, "page-state-cxl-worker-precopy-time-ns"),
        "cxl_worker_postcopy_bytes": xcxl_max(
            samples, "page-state-cxl-worker-postcopy-bytes"),
        "cxl_worker_postcopy_time_ns": xcxl_max(
            samples, "page-state-cxl-worker-postcopy-time-ns"),
        "rdma_completed_bytes": xcxl_max(
            samples, "page-state-rdma-completed-bytes"),
        "rdma_completed_time_ns": xcxl_max(
            samples, "page-state-rdma-completed-time-ns"),
        "rdma_precopy_completed_bytes": xcxl_max(
            samples, "page-state-rdma-precopy-completed-bytes"),
        "rdma_precopy_completed_time_ns": xcxl_max(
            samples, "page-state-rdma-precopy-completed-time-ns"),
        "rdma_precopy_active_time_ns": xcxl_max(
            samples, "page-state-rdma-precopy-active-time-ns"),
        "rdma_precopy_transport_completed_time_ns": xcxl_max(
            samples,
            "page-state-rdma-precopy-transport-completed-time-ns"),
        "rdma_precopy_transport_active_time_ns": xcxl_max(
            samples, "page-state-rdma-precopy-transport-active-time-ns"),
        "rdma_precopy_publish_time_ns": xcxl_max(
            samples, "page-state-rdma-precopy-publish-time-ns"),
        "rdma_postcopy_dirty_completed_bytes":
            sidecar_postcopy_bytes or page_state_postcopy_bytes,
        "rdma_postcopy_dirty_completed_time_ns": xcxl_max(
            samples, "page-state-rdma-postcopy-dirty-completed-time-ns"),
        "rdma_postcopy_dirty_active_time_ns": xcxl_max(
            samples, "page-state-rdma-postcopy-dirty-active-time-ns"),
        "rdma_postcopy_dirty_transport_completed_time_ns": xcxl_max(
            samples,
            "page-state-rdma-postcopy-dirty-transport-completed-time-ns"),
        "rdma_postcopy_dirty_transport_active_time_ns": xcxl_max(
            samples,
            "page-state-rdma-postcopy-dirty-transport-active-time-ns"),
        "rdma_postcopy_dirty_publish_time_ns": xcxl_max(
            samples, "page-state-rdma-postcopy-dirty-publish-time-ns"),
        "rdma_postcopy_dirty_completed_spans": xcxl_max(
            samples, "rdma-sidecar-postcopy-dirty-completed-spans"),
        "rdma_postcopy_dirty_stale_pages": xcxl_max(
            samples, "rdma-sidecar-postcopy-dirty-stale-pages"),
        "rdma_dynamic_window_regions": xcxl_max(
            samples, "rdma-sidecar-dynamic-window-regions"),
        "rdma_sq_capacity_regions": xcxl_max(
            samples, "rdma-sidecar-sq-capacity-regions"),
        "rdma_bdp_estimate_regions": xcxl_max(
            samples, "rdma-sidecar-bdp-estimate-regions"),
        "rdma_admission_accepted_regions": xcxl_max(
            samples, "rdma-sidecar-admission-accepted-regions"),
        "rdma_admission_overflow_cxl_regions": xcxl_max(
            samples, "rdma-sidecar-admission-overflow-cxl-regions"),
        "rdma_admission_closed_events": xcxl_max(
            samples, "rdma-sidecar-admission-closed-events"),
        "rdma_admission_goodput_drop_events": xcxl_max(
            samples, "rdma-sidecar-admission-goodput-drop-events"),
        "rdma_postcopy_dirty_overflow_cxl_spans": xcxl_max(
            samples, "rdma-sidecar-postcopy-dirty-overflow-cxl-spans"),
        "rdma_postcopy_dirty_min_span_cxl_spans": xcxl_max(
            samples, "rdma-sidecar-postcopy-dirty-min-span-cxl-spans"),
        "stderr_error_count": stderr_summary.get("error_count", 0),
        "stderr_error_summary": stderr_summary_text(stderr_summary),
    }


def write_summary_files(out_dir, rows):
    json_path = out_dir / "summary.json"
    csv_path = out_dir / "summary.csv"
    write_json(json_path, {"runs": rows})
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=SUMMARY_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key) for key in SUMMARY_FIELDS})
    return {
        "json": json_path,
        "csv": csv_path,
    }


def read_file(path):
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def run_case(base_dir, args, run_index):
    case_dir = base_dir / args.pressure / f"{args.mode}-run{run_index:02d}"
    case_dir.mkdir(parents=True, exist_ok=True)
    cxl_backing = case_dir / "cxl-backing.img"
    cxl_backing.write_bytes(b"")
    with cxl_backing.open("r+b") as f:
        f.truncate(256 * 1024 * 1024)

    boot_img = build_boot_image(case_dir, args.pressure)
    trace_events = trace_events_for_profile(args.trace_profile)
    trace_events_file = None
    if trace_events:
        trace_events_file = case_dir / "trace-events"
        trace_events_file.write_text("\n".join(trace_events) + "\n",
                                     encoding="ascii")

    socket_dir = Path(tempfile.mkdtemp(prefix="rdma-cxl-sock-"))
    src_qmp_path = socket_dir / "src.qmp"
    dst_qmp_path = socket_dir / "dst.qmp"
    mig_sock = socket_dir / "migration.sock"
    src_trace = case_dir / "src-trace.txt"
    dst_trace = case_dir / "dst-trace.txt"
    src_stderr = case_dir / "src.stderr"
    dst_stderr = case_dir / "dst.stderr"
    samples_path = case_dir / "samples.json"
    common = build_common_qemu_args(boot_img, args.accel)
    prefix = numactl_prefix(args)
    procs = []
    src_qmp = None
    dst_qmp = None

    try:
        src = start_vm(common, src_qmp_path, src_stderr, src_trace,
                       trace_events_file, env=build_qemu_env(True),
                       prefix=prefix)
        dst = start_vm(common, dst_qmp_path, dst_stderr, dst_trace,
                       trace_events_file, incoming=True,
                       env=build_qemu_env(False), prefix=prefix)
        procs.extend([src, dst])
        src_qmp = qmp_connect(src_qmp_path, src)
        dst_qmp = qmp_connect(dst_qmp_path, dst)

        caps = [
            {"capability": "mapped-ram", "state": True},
            {"capability": "postcopy-ram", "state": True},
            {"capability": "x-cxl-hybrid", "state": True},
            {"capability": "multifd", "state": True},
            {"capability": "rdma-pin-all", "state": bool(args.rdma_pin_all)},
        ]
        src_qmp.command("migrate-set-capabilities", {"capabilities": caps})
        dst_qmp.command("migrate-set-capabilities", {"capabilities": caps})
        params = build_migration_parameters(
            args, str(cxl_backing), run_index=run_index - 1)
        src_qmp.command("migrate-set-parameters", params)
        dst_qmp.command("migrate-set-parameters", params)

        migration_uri = f"unix:{mig_sock}"
        dst_qmp.command("migrate-incoming", {"uri": migration_uri})
        src_qmp.command("cont")
        dst_qmp.command("cont")
        time.sleep(0.5)
        src_qmp.command("migrate", {"uri": migration_uri})
        samples, stop_reason = collect_until_complete(
            src_qmp, dst_qmp, samples_path, args.migration_timeout)
        qmp_probe(src_qmp, "trace-file-flush")
        qmp_probe(dst_qmp, "trace-file-flush")
        stderr = {
            "src": read_file(src_stderr),
            "dst": read_file(dst_stderr),
        }
        timing = timeline_summary(src_trace)
        row = extract_summary(
            samples, case_dir, args.mode, args.pressure, run_index,
            classify_stderr(stderr), timing=timing)
        row["stop_reason"] = stop_reason
        write_json(case_dir / "result.json", row)
        return row
    finally:
        if src_qmp is not None:
            src_qmp.close()
        if dst_qmp is not None:
            dst_qmp.close()
        stop_processes(procs)
        shutil.rmtree(socket_dir, ignore_errors=True)


def validate_args(args):
    if args.migration_timeout <= 0:
        raise SystemExit("migration-timeout must be > 0")
    if args.rdma_port > 65535:
        raise SystemExit("rdma-port must be in 1..65535")
    if args.postcopy_dirty_rdma_min_bytes < PAGE_SIZE:
        raise SystemExit("postcopy dirty RDMA minimum must be at least 4096")
    if not QEMU.exists():
        raise SystemExit(f"missing qemu binary: {QEMU}")
    if not BOOT_ASM.exists():
        raise SystemExit(f"missing boot asm: {BOOT_ASM}")


def main(argv=None):
    args = parse_args(argv)
    validate_args(args)
    base_dir = Path(tempfile.mkdtemp(prefix="rdma-cxl-parallel-"))
    rows = []
    try:
        for run_index in range(1, args.repeat + 1):
            rows.append(run_case(base_dir, args, run_index))
        paths = write_summary_files(base_dir, rows)
        result = {
            "run_dir": str(base_dir),
            "summary_json": str(paths["json"]),
            "summary_csv": str(paths["csv"]),
            "runs": rows,
        }
        print(json.dumps(result, indent=2, sort_keys=True))
        if not args.keep_dir:
            shutil.rmtree(base_dir, ignore_errors=True)
        return 0
    except Exception as exc:
        payload = {
            "run_dir": str(base_dir),
            "error": str(exc),
        }
        print(json.dumps(payload, indent=2, sort_keys=True), file=sys.stderr)
        raise


if __name__ == "__main__":
    raise SystemExit(main())
