#!/usr/bin/env python3
import csv
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("rdma_cxl_parallel_experiment.py")


def load_module():
    spec = importlib.util.spec_from_file_location("rdma_cxl_parallel",
                                                  SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RDMACXLParallelExperimentTest(unittest.TestCase):
    maxDiff = None

    def setUp(self):
        self.mod = load_module()

    def test_parse_args_defaults_to_converged_postcopy_dirty_rdma(self):
        args = self.mod.parse_args([
            "--pressure", "remap_xlarge_random_rw",
            "--mode", "hybrid_parallel_rdma_cxl",
        ])

        self.assertEqual(args.pressure, "remap_xlarge_random_rw")
        self.assertEqual(args.mode, "hybrid_parallel_rdma_cxl")
        self.assertTrue(args.postcopy_dirty_rdma)
        self.assertEqual(args.postcopy_dirty_rdma_min_bytes, 64 * 1024)
        self.assertEqual(args.x_cxl_rdma_sidecar_max_inflight_regions, 0)

    def test_parse_args_can_disable_postcopy_dirty_rdma(self):
        args = self.mod.parse_args([
            "--pressure", "remap_xlarge_random_rw",
            "--mode", "hybrid_parallel_rdma_cxl",
            "--no-postcopy-dirty-rdma",
        ])

        self.assertFalse(args.postcopy_dirty_rdma)

    def test_build_migration_parameters_enable_rdma_cxl_lanes(self):
        args = self.mod.parse_args([
            "--pressure", "remap_xlarge_random_rw",
            "--mode", "hybrid_parallel_rdma_cxl",
            "--rdma-host", "192.0.2.10",
            "--rdma-port", "7474",
            "--postcopy-dirty-rdma-min-bytes", "131072",
            "--x-cxl-rdma-sidecar-max-inflight-regions", "0",
            "--x-cxl-rdma-cxl-priority-threshold-bytes", "262144",
        ])

        params = self.mod.build_migration_parameters(
            args, cxl_path="/tmp/cxl.img", run_index=2)

        self.assertTrue(params["x-cxl-rdma-sidecar"])
        self.assertEqual(params["x-cxl-rdma-sidecar-address"], {
            "transport": "rdma",
            "host": "192.0.2.10",
            "port": "7476",
        })
        self.assertEqual(
            params["x-cxl-rdma-sidecar-max-inflight-regions"], 0)
        self.assertTrue(params["x-cxl-rdma-sidecar-postcopy-dirty"])
        self.assertEqual(
            params["x-cxl-rdma-sidecar-postcopy-dirty-min-bytes"], 131072)
        self.assertEqual(
            params["x-cxl-rdma-cxl-priority-threshold-bytes"], 262144)
        self.assertEqual(params["cxl-path"], "/tmp/cxl.img")
        self.assertNotIn("x-cxl-rdma-sidecar-max-" "cover-percent", params)

    def test_extract_summary_from_query_migrate_samples(self):
        samples = [{
            "src-query-migrate": {
                "status": "completed",
                "total-time": 1200,
                "setup-time": 30,
                "downtime": 11,
                "x-cxl": {
                    "page-state-cxl-worker-bytes": 4096,
                    "page-state-cxl-worker-time-ns": 1000,
                    "page-state-cxl-worker-precopy-bytes": 1024,
                    "page-state-cxl-worker-precopy-time-ns": 300,
                    "page-state-cxl-worker-postcopy-bytes": 3072,
                    "page-state-cxl-worker-postcopy-time-ns": 700,
                    "page-state-rdma-completed-bytes": 8192,
                    "page-state-rdma-completed-time-ns": 2000,
                    "page-state-rdma-precopy-completed-bytes": 4096,
                    "page-state-rdma-precopy-completed-time-ns": 1100,
                    "page-state-rdma-precopy-active-time-ns": 1200,
                    "page-state-rdma-precopy-transport-completed-time-ns": 900,
                    "page-state-rdma-precopy-transport-active-time-ns": 950,
                    "page-state-rdma-precopy-publish-time-ns": 200,
                    "page-state-rdma-postcopy-dirty-completed-bytes": 4096,
                    "page-state-rdma-postcopy-dirty-completed-time-ns": 2100,
                    "page-state-rdma-postcopy-dirty-active-time-ns": 2200,
                    "page-state-rdma-postcopy-dirty-transport-completed-time-ns": 1800,
                    "page-state-rdma-postcopy-dirty-transport-active-time-ns": 1850,
                    "page-state-rdma-postcopy-dirty-publish-time-ns": 300,
                    "rdma-sidecar-dynamic-window-regions": 8,
                    "rdma-sidecar-sq-capacity-regions": 16,
                    "rdma-sidecar-bdp-estimate-regions": 4,
                    "rdma-sidecar-admission-accepted-regions": 5,
                    "rdma-sidecar-admission-overflow-cxl-regions": 1,
                    "rdma-sidecar-admission-closed-events": 0,
                    "rdma-sidecar-admission-goodput-drop-events": 2,
                    "rdma-sidecar-postcopy-dirty-completed-spans": 3,
                    "rdma-sidecar-postcopy-dirty-completed-bytes": 4096,
                    "rdma-sidecar-postcopy-dirty-stale-pages": 0,
                    "rdma-sidecar-postcopy-dirty-overflow-cxl-spans": 1,
                    "rdma-sidecar-postcopy-dirty-min-span-cxl-spans": 2,
                },
            },
            "dst-query-status": {
                "status": "running",
                "running": True,
            },
        }]

        row = self.mod.extract_summary(
            samples, run_dir=Path("/tmp/run"), mode="hybrid_parallel_rdma_cxl",
            pressure="remap_xlarge_random_rw", run_index=1,
            stderr_summary={"error_count": 0, "matches": []})

        self.assertEqual(row["final_status"], "completed")
        self.assertTrue(row["dst_running"])
        self.assertEqual(row["dst_status"], "running")
        self.assertEqual(row["total_time_ms"], 1200)
        self.assertEqual(row["cxl_worker_bytes"], 4096)
        self.assertEqual(row["cxl_worker_precopy_bytes"], 1024)
        self.assertEqual(row["cxl_worker_precopy_time_ns"], 300)
        self.assertEqual(row["cxl_worker_postcopy_bytes"], 3072)
        self.assertEqual(row["cxl_worker_postcopy_time_ns"], 700)
        self.assertEqual(row["rdma_completed_bytes"], 8192)
        self.assertEqual(row["rdma_precopy_completed_bytes"], 4096)
        self.assertEqual(row["rdma_precopy_completed_time_ns"], 1100)
        self.assertEqual(row["rdma_precopy_active_time_ns"], 1200)
        self.assertEqual(
            row["rdma_precopy_transport_completed_time_ns"], 900)
        self.assertEqual(row["rdma_precopy_transport_active_time_ns"], 950)
        self.assertEqual(row["rdma_precopy_publish_time_ns"], 200)
        self.assertEqual(row["rdma_postcopy_dirty_completed_bytes"], 4096)
        self.assertEqual(
            row["rdma_postcopy_dirty_completed_time_ns"], 2100)
        self.assertEqual(row["rdma_postcopy_dirty_active_time_ns"], 2200)
        self.assertEqual(
            row["rdma_postcopy_dirty_transport_completed_time_ns"], 1800)
        self.assertEqual(
            row["rdma_postcopy_dirty_transport_active_time_ns"], 1850)
        self.assertEqual(row["rdma_postcopy_dirty_publish_time_ns"], 300)
        self.assertEqual(row["rdma_dynamic_window_regions"], 8)
        self.assertEqual(row["rdma_admission_accepted_regions"], 5)
        self.assertEqual(row["stderr_error_count"], 0)

    def test_classify_stderr_reports_error_lines(self):
        summary = self.mod.classify_stderr({
            "src": "normal\nUFFD copy failed\n",
            "dst": "Traceback: bad\ncleanup warning\n",
        })

        self.assertEqual(summary["error_count"], 3)
        self.assertEqual(
            [item["source"] for item in summary["matches"]],
            ["src", "dst", "dst"])

    def test_timeline_summary_uses_current_trace_stage_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / "src-trace.txt"
            trace.write_text(
                "\n".join([
                    "migration_precopy_timeline estimate now_ns=1000",
                    "migration_precopy_timeline iterate-begin now_ns=2000",
                    "migration_precopy_timeline request-postcopy now_ns=7000",
                    "migration_postcopy_timeline state-postcopy-active now_ns=9000",
                    "migration_postcopy_timeline completed now_ns=14000",
                ]) + "\n",
                encoding="ascii")

            summary = self.mod.timeline_summary(trace)

        self.assertEqual(summary["precopy_time_ms"], 0.005)
        self.assertEqual(summary["postcopy_time_ms"], 0.005)

    def test_write_summary_files_outputs_json_and_csv(self):
        row = {
            "run_dir": "/tmp/run",
            "mode": "hybrid_parallel_rdma_cxl",
            "pressure": "remap_xlarge_random_rw",
            "run_index": 1,
            "final_status": "completed",
            "dst_running": True,
            "dst_status": "running",
            "total_time_ms": 100,
            "precopy_time_ms": None,
            "postcopy_time_ms": None,
            "cxl_worker_bytes": 1,
            "cxl_worker_time_ns": 2,
            "cxl_worker_precopy_bytes": 3,
            "cxl_worker_precopy_time_ns": 4,
            "cxl_worker_postcopy_bytes": 5,
            "cxl_worker_postcopy_time_ns": 6,
            "rdma_completed_bytes": 7,
            "rdma_completed_time_ns": 8,
            "rdma_precopy_completed_bytes": 9,
            "rdma_precopy_completed_time_ns": 10,
            "rdma_precopy_active_time_ns": 11,
            "rdma_precopy_transport_completed_time_ns": 12,
            "rdma_precopy_transport_active_time_ns": 13,
            "rdma_precopy_publish_time_ns": 14,
            "rdma_postcopy_dirty_completed_bytes": 15,
            "rdma_postcopy_dirty_completed_time_ns": 16,
            "rdma_postcopy_dirty_active_time_ns": 17,
            "rdma_postcopy_dirty_transport_completed_time_ns": 18,
            "rdma_postcopy_dirty_transport_active_time_ns": 19,
            "rdma_postcopy_dirty_publish_time_ns": 20,
            "rdma_postcopy_dirty_completed_spans": 21,
            "rdma_postcopy_dirty_stale_pages": 0,
            "rdma_dynamic_window_regions": 22,
            "rdma_sq_capacity_regions": 23,
            "rdma_bdp_estimate_regions": 24,
            "rdma_admission_accepted_regions": 25,
            "rdma_admission_overflow_cxl_regions": 26,
            "rdma_admission_closed_events": 0,
            "rdma_admission_goodput_drop_events": 1,
            "rdma_postcopy_dirty_overflow_cxl_spans": 2,
            "rdma_postcopy_dirty_min_span_cxl_spans": 3,
            "stderr_error_count": 0,
            "stderr_error_summary": "",
        }

        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp)
            paths = self.mod.write_summary_files(out_dir, [row])

            payload = json.loads(paths["json"].read_text())
            self.assertEqual(payload["runs"][0]["final_status"], "completed")
            with paths["csv"].open(newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(rows[0]["mode"], "hybrid_parallel_rdma_cxl")
            self.assertEqual(rows[0]["cxl_worker_precopy_time_ns"], "4")
            self.assertEqual(rows[0]["rdma_completed_bytes"], "7")
            self.assertEqual(rows[0]["rdma_precopy_active_time_ns"], "11")
            self.assertEqual(
                rows[0]["rdma_postcopy_dirty_active_time_ns"], "17")


if __name__ == "__main__":
    unittest.main()
