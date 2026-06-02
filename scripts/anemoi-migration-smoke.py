#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Minimal two-QEMU smoke test for the Anemoi P2 migration path.

The test uses the in-process memory backend and a paused TCG VM.  It is a
plumbing check for the Anemoi migration subsection and incoming attach path,
not a shared-AnemoiM data-integrity test.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import shutil
from pathlib import Path


class QMPError(RuntimeError):
    pass


class QMPClient:
    def __init__(self, path: Path, timeout: float = 10.0):
        self.path = path
        self.timeout = timeout
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.file = None
        self.next_id = 1

    def connect(self):
        deadline = time.monotonic() + self.timeout
        while True:
            try:
                self.sock.connect(str(self.path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)

        self.file = self.sock.makefile("rwb", buffering=0)
        greeting = self._read_json()
        if "QMP" not in greeting:
            raise QMPError(f"missing QMP greeting on {self.path}: {greeting}")
        self.command("qmp_capabilities")

    def close(self):
        if self.file:
            self.file.close()
            self.file = None
        self.sock.close()

    def _read_json(self):
        line = self.file.readline()
        if not line:
            raise QMPError(f"QMP socket closed: {self.path}")
        return json.loads(line.decode("utf-8"))

    def command(self, execute: str, arguments=None):
        cmd_id = self.next_id
        self.next_id += 1
        payload = {"execute": execute, "id": cmd_id}
        if arguments is not None:
            payload["arguments"] = arguments
        self.file.write(json.dumps(payload).encode("utf-8") + b"\r\n")

        while True:
            response = self._read_json()
            if response.get("id") != cmd_id:
                continue
            if "error" in response:
                err = response["error"]
                raise QMPError(f"{execute} failed: {err.get('desc', err)}")
            return response.get("return")


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def qemu_binary(default_repo: Path, override: str | None) -> Path:
    if override:
        return Path(override)
    env_qemu = os.environ.get("QEMU_SYSTEM_X86_64")
    if env_qemu:
        return Path(env_qemu)
    return default_repo / "build" / "qemu-system-x86_64"


def start_qemu(qemu: Path, qmp: Path, log, memory: str,
               incoming_uri: str | None = None) -> subprocess.Popen:
    argv = [
        str(qemu),
        "-nodefaults",
        "-display", "none",
        "-machine", "pc,accel=tcg",
        "-m", memory,
        "-S",
        "-qmp", f"unix:{qmp},server=on,wait=off",
    ]
    if incoming_uri:
        argv.extend(["-incoming", incoming_uri])
    return subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)


def ensure_running(proc: subprocess.Popen, name: str, log_path: Path):
    if proc.poll() is not None:
        raise RuntimeError(
            f"{name} exited with status {proc.returncode}; see {log_path}"
        )


def wait_migration_complete(qmp: QMPClient, timeout: float):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = qmp.command("query-migrate")
        status = last.get("status")
        if status == "completed":
            return last
        if status in {"failed", "cancelled"}:
            raise RuntimeError(f"migration ended with status {status}: {last}")
        time.sleep(0.1)
    raise TimeoutError(f"migration did not complete; last status: {last}")


def qmp_quit(qmp: QMPClient):
    try:
        qmp.command("quit")
    except (BrokenPipeError, QMPError):
        pass


def dump_log(path: Path):
    if not path.exists():
        return
    print(f"--- {path} ---", file=sys.stderr)
    data = path.read_text(errors="replace")
    print(data[-8192:], file=sys.stderr, end="" if data.endswith("\n") else "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    repo = Path(__file__).resolve().parents[1]
    parser.add_argument("--qemu", help="path to qemu-system-x86_64")
    parser.add_argument("--memory", default="16M", help="guest memory size")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--keep-logs", action="store_true",
                        help="keep temporary QEMU logs after the run")
    args = parser.parse_args()

    qemu = qemu_binary(repo, args.qemu)
    if not qemu.exists():
        raise SystemExit(f"QEMU binary not found: {qemu}")

    tmp = tempfile.mkdtemp(prefix="anemoi-mig-")
    tmpdir = Path(tmp)
    keep_logs = args.keep_logs
    try:
        src_qmp_path = tmpdir / "src.qmp"
        dst_qmp_path = tmpdir / "dst.qmp"
        src_log_path = tmpdir / "src.log"
        dst_log_path = tmpdir / "dst.log"
        mig_port = free_tcp_port()
        mig_uri = f"tcp:127.0.0.1:{mig_port}"

        src_log = src_log_path.open("wb")
        dst_log = dst_log_path.open("wb")
        src_proc = dst_proc = None
        src_qmp = dst_qmp = None
        try:
            dst_proc = start_qemu(qemu, dst_qmp_path, dst_log, args.memory,
                                  incoming_uri=mig_uri)
            src_proc = start_qemu(qemu, src_qmp_path, src_log, args.memory)
            ensure_running(dst_proc, "destination QEMU", dst_log_path)
            ensure_running(src_proc, "source QEMU", src_log_path)

            dst_qmp = QMPClient(dst_qmp_path, timeout=args.timeout)
            src_qmp = QMPClient(src_qmp_path, timeout=args.timeout)
            dst_qmp.connect()
            src_qmp.connect()

            common = {
                "backend": "memory",
                "auto-pause": False,
                "auto-resume": False,
            }
            dst_qmp.command("x-anemoi-prepare-incoming", common)
            src_qmp.command("x-anemoi-start", common)

            dst_before = dst_qmp.command("query-anemoi")
            src_before = src_qmp.command("query-anemoi")
            if dst_before.get("boot-mode") != "destination-attach":
                raise RuntimeError(f"bad destination boot mode: {dst_before}")
            if src_before.get("boot-mode") != "source-seed":
                raise RuntimeError(f"bad source boot mode: {src_before}")

            src_qmp.command("migrate", {"uri": mig_uri})
            wait_migration_complete(src_qmp, args.timeout)

            src_after = src_qmp.command("query-anemoi")
            dst_after = dst_qmp.command("query-anemoi")
            if not src_after.get("fault-service-quiesced"):
                raise RuntimeError(f"source did not quiesce: {src_after}")
            if dst_after.get("boot-mode") != "destination-attach":
                raise RuntimeError(f"destination runtime changed: {dst_after}")
            if dst_after.get("fault-service-failed"):
                raise RuntimeError(f"destination fault service failed: {dst_after}")

            print("Anemoi migration smoke passed")
            print(json.dumps({"source": src_after, "destination": dst_after},
                             sort_keys=True))
        except Exception:
            keep_logs = True
            dump_log(src_log_path)
            dump_log(dst_log_path)
            raise
        finally:
            for qmp in (src_qmp, dst_qmp):
                if qmp:
                    qmp_quit(qmp)
                    qmp.close()
            for proc in (src_proc, dst_proc):
                if proc and proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=5)
            src_log.close()
            dst_log.close()
    finally:
        if keep_logs:
            print(f"Anemoi migration smoke logs kept at {tmpdir}", file=sys.stderr)
        else:
            shutil.rmtree(tmpdir)

    return 0


if __name__ == "__main__":
    sys.exit(main())
