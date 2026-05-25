#!/usr/bin/env python3
import argparse
import importlib.util
import itertools
import re
import struct
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("cxl-hybrid-warm-experiment.py")
REPO_ROOT = Path(__file__).resolve().parents[1]


def load_module():
    spec = importlib.util.spec_from_file_location("cxl_hybrid_warm_experiment",
                                                  SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WarmExperimentScriptTest(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_cxl_source_visibility_producers_drive_shared_bitmap(self):
        source = (REPO_ROOT / "migration" / "cxl.c").read_text()

        publish_start = source.index("int cxl_hybrid_publish_page_to_cxl(")
        publish_end = source.index("static void cxl_hybrid_publish_fault_burst(")
        publish_body = source[publish_start:publish_end]
        self.assertIn("cxl_hybrid_ctrl_set_page_visible(page_idx, generation)",
                      publish_body)
        self.assertIn("cxl_hybrid_mark_page_cxl_visible(page_idx)",
                      publish_body)

        invalidate_start = source.index(
            "static void cxl_hybrid_invalidate_published_page(")
        invalidate_end = source.index(
            "static int cxl_hybrid_copy_page_to_stable_cxl(")
        invalidate_body = source[invalidate_start:invalidate_end]
        self.assertIn("cxl_hybrid_ctrl_clear_page_visible(page_idx)",
                      invalidate_body)
        self.assertIn("clear_bit_atomic(page_idx, cxl_state.cxl_visible_bmap)",
                      invalidate_body)
        self.assertIn("set_bit_atomic(page_idx, cxl_state.remaining_bmap)",
                      invalidate_body)

        remap_start = source.index("static void cxl_mark_pages_remapped(")
        remap_end = source.index(
            "static bool cxl_refresh_region_backing(")
        remap_body = source[remap_start:remap_end]
        self.assertIn("cxl_hybrid_ctrl_set_page_visible(page_idx, generation)",
                      remap_body)
        self.assertIn("set_bit_atomic(page_idx, cxl_state.cxl_visible_bmap)",
                      remap_body)
        self.assertIn("clear_bit_atomic(page_idx, cxl_state.remaining_bmap)",
                      remap_body)

    def test_ctrl_set_page_visible_uses_source_header_without_exact_generation_gate(self):
        source = (REPO_ROOT / "migration" / "cxl-hybrid-control.c").read_text()

        fn_start = source.index("void cxl_hybrid_ctrl_set_page_visible(")
        fn_end = source.index("void cxl_hybrid_ctrl_clear_page_visible(")
        fn_body = source[fn_start:fn_end]

        self.assertIn("if (!state->hdr || !state->visible_bitmap)", fn_body)
        self.assertNotIn("state->hdr->generation != generation", fn_body)
        self.assertIn("cxl_hybrid_control_mark_page_visible(state->hdr,",
                      fn_body)

    def test_summarize_guest_heartbeats_reports_handoff_gap_and_stall(self):
        events = [
            {"ts": 0.010, "side": "src"},
            {"ts": 0.020, "side": "src"},
            {"ts": 0.030, "side": "src"},
            {"ts": 0.040, "side": "src"},
            {"ts": 0.080, "side": "dst"},
            {"ts": 0.090, "side": "dst"},
        ]

        report = self.mod.summarize_guest_heartbeats(
            events,
            migration_start_ts=0.045,
            migration_complete_ts=0.085,
        )

        self.assertEqual(report["events_total"], 6)
        self.assertEqual(report["events_src"], 4)
        self.assertEqual(report["events_dst"], 2)
        self.assertAlmostEqual(report["baseline_gap_ms"], 10.0)
        self.assertAlmostEqual(report["handoff_gap_ms"], 40.0)
        self.assertAlmostEqual(report["handoff_stall_ms"], 30.0)
        self.assertAlmostEqual(report["max_gap_during_migration_ms"], 40.0)
        self.assertAlmostEqual(report["max_gap_stall_ms"], 30.0)

    def test_summarize_guest_heartbeats_clips_total_stall_to_completion(self):
        events = [
            {"ts": 0.010, "side": "src"},
            {"ts": 0.020, "side": "src"},
            {"ts": 0.030, "side": "src"},
            {"ts": 0.040, "side": "src"},
            {"ts": 0.080, "side": "dst"},
            {"ts": 0.090, "side": "dst"},
        ]

        report = self.mod.summarize_guest_heartbeats(
            events,
            migration_start_ts=0.045,
            migration_complete_ts=0.065,
        )

        self.assertAlmostEqual(report["baseline_gap_ms"], 10.0)
        self.assertAlmostEqual(report["max_gap_during_migration_ms"], 40.0)
        self.assertAlmostEqual(report["total_gap_during_migration_ms"], 20.0)
        self.assertAlmostEqual(report["total_stall_during_migration_ms"], 10.0)

    def test_summarize_guest_heartbeats_reports_corrected_window(self):
        events = [
            {"ts": 0.000, "side": "src"},
            {"ts": 0.010, "side": "src"},
            {"ts": 0.020, "side": "src"},
            {"ts": 0.040, "side": "dst"},
            {"ts": 0.050, "side": "dst"},
            {"ts": 0.080, "side": "dst"},
            {"ts": 0.090, "side": "dst"},
        ]

        report = self.mod.summarize_guest_heartbeats(
            events,
            migration_start_ts=0.000,
            migration_complete_ts=0.090,
            corrected_start_ts=0.020,
            corrected_complete_ts=0.045,
        )

        self.assertAlmostEqual(report["total_gap_during_migration_ms"], 90.0)
        self.assertAlmostEqual(report["total_stall_during_migration_ms"], 30.0)
        corrected = report["corrected_window"]
        self.assertAlmostEqual(corrected["total_gap_during_migration_ms"], 25.0)
        self.assertAlmostEqual(corrected["total_stall_during_migration_ms"], 10.0)
        self.assertAlmostEqual(corrected["first_destination_gap_stall_ms"], 10.0)
        self.assertAlmostEqual(corrected["destination_rest_stall_ms"], 0.0)

    def test_corrected_window_counts_handoff_overlap_in_total_stall(self):
        events = [
            {"ts": 0.000, "side": "src"},
            {"ts": 0.010, "side": "src"},
            {"ts": 0.020, "side": "src"},
            {"ts": 0.040, "side": "dst"},
            {"ts": 0.050, "side": "dst"},
        ]

        report = self.mod.summarize_guest_heartbeats(
            events,
            migration_start_ts=0.000,
            migration_complete_ts=0.050,
            corrected_start_ts=0.015,
            corrected_complete_ts=0.035,
        )

        corrected = report["corrected_window"]
        self.assertAlmostEqual(corrected["handoff_stall_ms"], 10.0)
        self.assertAlmostEqual(corrected["total_gap_during_migration_ms"], 20.0)
        self.assertAlmostEqual(corrected["total_stall_during_migration_ms"], 5.0)
        self.assertAlmostEqual(corrected["first_destination_gap_stall_ms"], 5.0)
        self.assertAlmostEqual(corrected["destination_rest_stall_ms"], 0.0)

    def test_summarize_guest_heartbeats_reports_edge_gap_breakdown(self):
        events = [
            {"ts": 0.000, "side": "src"},
            {"ts": 0.010, "side": "src"},
            {"ts": 0.021, "side": "src"},
            {"ts": 0.033, "side": "src"},
            {"ts": 0.045, "side": "src"},
            {"ts": 0.060, "side": "src"},
            {"ts": 0.090, "side": "dst"},
            {"ts": 0.112, "side": "dst"},
            {"ts": 0.124, "side": "dst"},
        ]

        report = self.mod.summarize_guest_heartbeats(events)

        self.assertAlmostEqual(report["src_tail_gap_mean_ms"], 12.0)
        self.assertAlmostEqual(report["src_tail_gap_max_ms"], 15.0)
        self.assertAlmostEqual(report["dst_initial_gap_1_ms"], 22.0)
        self.assertAlmostEqual(report["dst_initial_gap_2_ms"], 12.0)
        self.assertAlmostEqual(report["dst_initial_excess_stall_ms"], 10.0)

    def test_collect_heartbeat_events_with_grace_waits_for_delayed_destination(self):
        class FakeHeartbeatCollector:
            def __init__(self):
                self.calls = 0

            def snapshot(self):
                self.calls += 1
                events = [
                    {"ts": 1.000, "side": "src"},
                    {"ts": 1.010, "side": "src"},
                ]
                if self.calls >= 2:
                    events.append({"ts": 1.025, "side": "dst"})
                    events.append({"ts": 1.030, "side": "dst"})
                return events

        sleeps = []
        original_sleep = self.mod.time.sleep
        try:
            self.mod.time.sleep = lambda secs: sleeps.append(secs)

            events = self.mod.collect_heartbeat_events_with_grace(
                FakeHeartbeatCollector()
            )
        finally:
            self.mod.time.sleep = original_sleep

        self.assertEqual(sleeps, [self.mod.HEARTBEAT_POST_COMPLETE_GRACE_SECS])
        self.assertEqual(events[-1]["side"], "dst")

    def test_collect_heartbeat_events_with_grace_polls_until_destination_seen(self):
        class FakeHeartbeatCollector:
            def __init__(self):
                self.calls = 0

            def snapshot(self):
                self.calls += 1
                events = [
                    {"ts": 1.000, "side": "src"},
                    {"ts": 1.010, "side": "src"},
                ]
                if self.calls >= 4:
                    events.append({"ts": 1.080, "side": "dst"})
                    events.append({"ts": 1.090, "side": "dst"})
                return events

        sleeps = []
        original_sleep = self.mod.time.sleep
        try:
            self.mod.time.sleep = lambda secs: sleeps.append(secs)

            events = self.mod.collect_heartbeat_events_with_grace(
                FakeHeartbeatCollector()
            )
        finally:
            self.mod.time.sleep = original_sleep

        self.assertEqual(sleeps, [
            self.mod.HEARTBEAT_POST_COMPLETE_GRACE_SECS,
            self.mod.HEARTBEAT_POST_COMPLETE_GRACE_SECS,
            self.mod.HEARTBEAT_POST_COMPLETE_GRACE_SECS,
        ])
        self.assertEqual(events[-1]["side"], "dst")

    def test_collect_heartbeat_events_with_grace_waits_for_min_destination_events(self):
        class FakeHeartbeatCollector:
            def __init__(self):
                self.calls = 0

            def snapshot(self):
                self.calls += 1
                events = [
                    {"ts": 1.000, "side": "src"},
                    {"ts": 1.010, "side": "src"},
                    {"ts": 1.030, "side": "dst"},
                ]
                if self.calls >= 2:
                    events.append({"ts": 1.040, "side": "dst"})
                return events

        sleeps = []
        original_sleep = self.mod.time.sleep
        try:
            self.mod.time.sleep = lambda secs: sleeps.append(secs)

            events = self.mod.collect_heartbeat_events_with_grace(
                FakeHeartbeatCollector()
            )
        finally:
            self.mod.time.sleep = original_sleep

        self.assertEqual(sleeps, [self.mod.HEARTBEAT_POST_COMPLETE_GRACE_SECS])
        self.assertEqual(
            sum(1 for item in events if item["side"] == "dst"),
            self.mod.HEARTBEAT_POST_COMPLETE_MIN_DST_EVENTS,
        )

    def test_corrected_window_from_total_time_anchors_at_migrate_start(self):
        start, complete = self.mod.corrected_window_from_total_time(
            migration_start_ts=9.927,
            migration_complete_ts=10.0,
            final_info={"total-time": 47},
        )

        self.assertAlmostEqual(start, 9.927)
        self.assertAlmostEqual(complete, 9.974)

    def test_summarize_single_result_reports_guest_total_stall(self):
        row = self.mod.summarize_single_result(
            "heavy",
            "hybrid_postcopy_auto",
            self.mod.resolve_threshold_profile("balanced"),
            1,
            {
                "summary": {},
                "trace": {"combined": {}},
                "latency": {
                    "total_time_ms": 42,
                    "downtime_ms": 1,
                    "observed_migration_window_ms": 58.5,
                },
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "total_gap_during_migration_ms": 30.0,
                    "total_stall_during_migration_ms": 18.0,
                },
                "final_status": "completed",
            },
        )

        self.assertEqual(row["guest_total_gap_during_migration_ms"], 30.0)
        self.assertEqual(row["guest_total_stall_during_migration_ms"], 18.0)
        self.assertEqual(row["observed_migration_window_ms"], 58.5)

    def test_summarize_single_result_reports_handoff_breakdown_fields(self):
        row = self.mod.summarize_single_result(
            "heavy",
            "hybrid_postcopy_auto",
            self.mod.resolve_threshold_profile("balanced"),
            1,
            {
                "summary": {},
                "trace": {"combined": {}},
                "latency": {},
                "guest_latency": {},
                "handoff_breakdown": {
                    "control_src_start_to_dst_vm_started_ms": 2.5,
                    "control_dst_run_to_vm_started_ms": 0.2,
                    "guest_handoff_stall_ms": 11.0,
                    "dst_initial_gap_1_ms": 8.0,
                    "src_tail_gap_max_ms": 3.0,
                    "postcopy_fault_request": 9,
                    "trace_dst_region_remap": 4,
                    "region_publish_pages": 128,
                    "ram_stream_publish_span_pages": 64,
                    "ram_stream_publish_span_written_pages": 16,
                    "ram_stream_publish_span_source_remapped_pages": 32,
                    "ram_stream_publish_span_destination_owned_pages": 16,
                },
                "final_status": "completed",
            },
        )

        self.assertEqual(row["handoff_control_src_start_to_dst_vm_started_ms"], 2.5)
        self.assertEqual(row["handoff_control_dst_run_to_vm_started_ms"], 0.2)
        self.assertEqual(row["handoff_guest_handoff_stall_ms"], 11.0)
        self.assertEqual(row["handoff_dst_initial_gap_1_ms"], 8.0)
        self.assertEqual(row["handoff_src_tail_gap_max_ms"], 3.0)
        self.assertEqual(row["handoff_postcopy_fault_request"], 9)
        self.assertEqual(row["handoff_trace_dst_region_remap"], 4)
        self.assertEqual(row["handoff_region_publish_pages"], 128)
        self.assertEqual(row["handoff_ram_stream_publish_span_pages"], 64)
        self.assertEqual(row["handoff_ram_stream_publish_span_written_pages"], 16)
        self.assertEqual(
            row["handoff_ram_stream_publish_span_source_remapped_pages"], 32)
        self.assertEqual(
            row["handoff_ram_stream_publish_span_destination_owned_pages"], 16)

    def test_parse_trace_log_classifies_ram_stream_publish_spans(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            trace_path = Path(tmpdir) / "trace.log"
            trace_path.write_text(
                "\n".join([
                    "cxl_hybrid_ram_stream_publish_span pc.ram/0x1000 "
                    "len=0x2000 gen=2 kind=0 reason=0",
                    "cxl_hybrid_ram_stream_publish_span pc.ram/0x3000 "
                    "len=0x1000 gen=2 kind=1 reason=1",
                    "cxl_hybrid_ram_stream_publish_span pc.ram/0x4000 "
                    "len=0x3000 gen=2 kind=1 reason=2",
                ]),
                encoding="utf-8",
            )

            counts = self.mod.parse_trace_log(trace_path)

        self.assertEqual(counts["ram_stream_publish_span"], 3)
        self.assertEqual(counts["ram_stream_publish_span_pages"], 6)
        self.assertEqual(counts["ram_stream_publish_span_written"], 1)
        self.assertEqual(counts["ram_stream_publish_span_written_pages"], 2)
        self.assertEqual(
            counts["ram_stream_publish_span_source_remapped"], 1)
        self.assertEqual(
            counts["ram_stream_publish_span_source_remapped_pages"], 1)
        self.assertEqual(
            counts["ram_stream_publish_span_destination_owned"], 1)
        self.assertEqual(
            counts["ram_stream_publish_span_destination_owned_pages"], 3)

    def test_set_caps_hybrid_postcopy_auto_enables_postcopy_and_hybrid(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_caps(object(), "hybrid_postcopy_auto")

        self.assertEqual(len(calls), 1)
        cmd, args = calls[0]
        self.assertEqual(cmd, "migrate-set-capabilities")
        caps = {
            item["capability"]: item["state"]
            for item in args["capabilities"]
        }
        self.assertTrue(caps["mapped-ram"])
        self.assertTrue(caps["postcopy-ram"])
        self.assertTrue(caps["x-cxl-hybrid"])
        self.assertTrue(caps["multifd"])

    def test_set_caps_hybrid_postcopy_manual_enables_postcopy_and_hybrid(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_caps(object(), "hybrid_postcopy_manual")

        _cmd, args = calls[0]
        caps = {
            item["capability"]: item["state"]
            for item in args["capabilities"]
        }
        self.assertTrue(caps["mapped-ram"])
        self.assertTrue(caps["postcopy-ram"])
        self.assertTrue(caps["x-cxl-hybrid"])
        self.assertTrue(caps["multifd"])

    def test_set_caps_pure_precopy_disables_postcopy_and_hybrid(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_caps(object(), "pure_precopy")

        _cmd, args = calls[0]
        caps = {
            item["capability"]: item["state"]
            for item in args["capabilities"]
        }
        self.assertTrue(caps["mapped-ram"])
        self.assertFalse(caps["postcopy-ram"])
        self.assertFalse(caps["x-cxl-hybrid"])
        self.assertTrue(caps["multifd"])

    def test_set_caps_native_postcopy_stream_enables_postcopy_only(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_caps(object(), "native_postcopy_stream")

        _cmd, args = calls[0]
        caps = {
            item["capability"]: item["state"]
            for item in args["capabilities"]
        }
        self.assertFalse(caps["mapped-ram"])
        self.assertTrue(caps["postcopy-ram"])
        self.assertFalse(caps["x-cxl-hybrid"])
        self.assertTrue(caps["multifd"])

    def test_set_caps_native_rdma_precopy_disables_incompatible_caps(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_caps(object(), "native_rdma_precopy")

        _cmd, args = calls[0]
        caps = {
            item["capability"]: item["state"]
            for item in args["capabilities"]
        }
        self.assertFalse(caps["mapped-ram"])
        self.assertFalse(caps["postcopy-ram"])
        self.assertFalse(caps["x-cxl-hybrid"])
        self.assertFalse(caps["multifd"])
        self.assertFalse(caps["rdma-pin-all"])

    def test_set_caps_native_rdma_precopy_can_pin_all(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_caps(object(), "native_rdma_precopy", rdma_pin_all=True)

        _cmd, args = calls[0]
        caps = {
            item["capability"]: item["state"]
            for item in args["capabilities"]
        }
        self.assertTrue(caps["rdma-pin-all"])

    def test_set_params_hybrid_auto_defaults_prefetch_rate_to_zero(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_params(object(), "/tmp/cxl.img",
                            "hybrid_postcopy_auto", "heavy",
                            shared_backing=True)

        self.assertEqual(len(calls), 1)
        cmd, args = calls[0]
        self.assertEqual(cmd, "migrate-set-parameters")
        self.assertEqual(args["cxl-path"], "/tmp/cxl.img")
        self.assertEqual(args["multifd-channels"], 2)
        self.assertEqual(args["max-bandwidth"], 8 * 1024 * 1024)
        self.assertEqual(args["x-cxl-switch-dirty-threshold"], 1)
        self.assertEqual(args["x-cxl-switch-max-iters"], 20)
        self.assertEqual(args["x-cxl-switch-max-precopy-ms"], 0)
        self.assertEqual(args["x-cxl-switch-min-remaining"], 8 * 1024 * 1024)
        self.assertEqual(args["x-cxl-brake-remap-granule"], 2 * 1024 * 1024)
        self.assertEqual(args["x-cxl-prefetch-batch-pages"], 128)
        self.assertEqual(args["x-cxl-prefetch-rate"], 0)
        self.assertTrue(args["x-cxl-brake-enable"])
        self.assertTrue(args["x-cxl-shared-backing"])

    def test_build_migration_parameters_accepts_remap_granule_override(self):
        args = argparse.Namespace(
            pressure="heavy",
            threshold_profile="balanced",
            x_cxl_switch_dirty_threshold=None,
            x_cxl_switch_max_iters=None,
            x_cxl_switch_max_precopy_ms=None,
            x_cxl_switch_min_remaining=None,
            x_cxl_prefetch_rate=None,
            x_cxl_dst_install_policy=None,
            x_cxl_fault_control_plane=None,
            x_cxl_fault_resolve_mode=None,
            x_cxl_brake_remap_granule=1024 * 1024,
        )

        params = self.mod.build_migration_parameters(
            args,
            "hybrid_postcopy_manual",
            cxl_path="/tmp/cxl.img",
            shared_backing=True,
        )

        self.assertEqual(params["x-cxl-brake-remap-granule"], 1024 * 1024)

    def test_build_migration_parameters_can_disable_hybrid_brake(self):
        args = argparse.Namespace(
            pressure="heavy",
            threshold_profile="balanced",
            x_cxl_switch_dirty_threshold=None,
            x_cxl_switch_max_iters=None,
            x_cxl_switch_max_precopy_ms=None,
            x_cxl_switch_min_remaining=None,
            x_cxl_switch_remap_coverage=None,
            x_cxl_prefetch_rate=None,
            x_cxl_dst_install_policy=None,
            x_cxl_fault_control_plane=None,
            x_cxl_fault_resolve_mode=None,
            x_cxl_brake_remap_granule=2 * 1024 * 1024,
            x_cxl_brake_enable=False,
        )

        params = self.mod.build_migration_parameters(
            args,
            "hybrid_postcopy_auto",
            cxl_path="/tmp/cxl.img",
            shared_backing=True,
        )

        self.assertFalse(params["x-cxl-brake-enable"])

    def test_build_migration_parameters_accepts_postcopy_bandwidth_override(self):
        args = argparse.Namespace(
            pressure="heavy",
            threshold_profile="balanced",
            x_cxl_switch_dirty_threshold=None,
            x_cxl_switch_max_iters=None,
            x_cxl_switch_max_precopy_ms=None,
            x_cxl_switch_min_remaining=None,
            x_cxl_prefetch_rate=None,
            x_cxl_dst_install_policy=None,
            x_cxl_fault_control_plane=None,
            x_cxl_fault_resolve_mode=None,
            x_cxl_brake_remap_granule=2 * 1024 * 1024,
            max_postcopy_bandwidth=1024 * 1024,
        )

        params = self.mod.build_migration_parameters(
            args,
            "hybrid_postcopy_manual",
            cxl_path="/tmp/cxl.img",
            shared_backing=True,
        )

        self.assertEqual(params["max-postcopy-bandwidth"], 1024 * 1024)

    def test_build_migration_parameters_accepts_cxl_backing_rate_override(self):
        args = argparse.Namespace(
            pressure="heavy",
            threshold_profile="balanced",
            x_cxl_switch_dirty_threshold=None,
            x_cxl_switch_max_iters=None,
            x_cxl_switch_max_precopy_ms=None,
            x_cxl_switch_min_remaining=None,
            x_cxl_switch_remap_coverage=None,
            x_cxl_prefetch_rate=None,
            x_cxl_dst_install_policy=None,
            x_cxl_fault_control_plane=None,
            x_cxl_fault_resolve_mode=None,
            x_cxl_brake_remap_granule=2 * 1024 * 1024,
            max_postcopy_bandwidth=None,
            x_cxl_backing_rate=512 * 1024 * 1024,
        )

        params = self.mod.build_migration_parameters(
            args,
            "hybrid_postcopy_manual",
            cxl_path="/tmp/cxl.img",
            shared_backing=True,
        )

        self.assertEqual(params["x-cxl-backing-rate"], 512 * 1024 * 1024)

    def test_clean_remap_args_set_qmp_parameters(self):
        chw = self.mod
        args = chw.parse_args([
            "--pressure", "remap_xlarge_random_rw",
            "--mode", "hybrid_postcopy_auto",
            "--x-cxl-clean-remap-enable",
            "--x-cxl-clean-remap-copy-budget", "8388608",
            "--x-cxl-clean-remap-throttle-us", "200",
            "--x-cxl-clean-remap-prefault-mode", "madvise",
        ])

        params = chw.build_migration_parameters(
            args,
            "hybrid_postcopy_auto",
        )

        self.assertEqual(params["x-cxl-clean-remap-enable"], True)
        self.assertEqual(params["x-cxl-clean-remap-copy-budget"], 8388608)
        self.assertEqual(params["x-cxl-clean-remap-throttle-us"], 200)
        self.assertEqual(params["x-cxl-clean-remap-prefault-mode"], "madvise")

    def test_disable_brake_cli_sets_brake_enable_false(self):
        default_args = self.mod.parse_args([])
        disabled_args = self.mod.parse_args([
            "--pressure", "remap_xlarge_random_rw",
            "--mode", "hybrid_postcopy_auto",
            "--x-cxl-disable-brake",
        ])

        self.assertTrue(default_args.x_cxl_brake_enable)
        self.assertFalse(disabled_args.x_cxl_brake_enable)

    def test_clean_remap_debug_mode_sets_source_env_only(self):
        src_env = self.mod.build_qemu_env(
            "hybrid_postcopy_auto", is_source=True,
            clean_remap_debug_mode="scan-only",
        )
        dst_env = self.mod.build_qemu_env(
            "hybrid_postcopy_auto", is_source=False,
            clean_remap_debug_mode="scan-only",
        )
        default_env = self.mod.build_qemu_env(
            "hybrid_postcopy_auto", is_source=True,
        )

        self.assertEqual(src_env["QEMU_CXL_CLEAN_REMAP_DEBUG"], "scan-only")
        self.assertNotIn("QEMU_CXL_CLEAN_REMAP_DEBUG", dst_env)
        self.assertNotIn("QEMU_CXL_CLEAN_REMAP_DEBUG", default_env)

    def test_clean_remap_debug_modes_include_copy_decomposition(self):
        for mode in ("read-only", "write-only"):
            args = self.mod.parse_args([
                "--pressure", "remap_xlarge_random_rw",
                "--mode", "hybrid_postcopy_auto",
                "--x-cxl-clean-remap-debug-mode", mode,
            ])
            src_env = self.mod.build_qemu_env(
                "hybrid_postcopy_auto", is_source=True,
                clean_remap_debug_mode=args.x_cxl_clean_remap_debug_mode,
            )

            self.assertEqual(args.x_cxl_clean_remap_debug_mode, mode)
            self.assertEqual(src_env["QEMU_CXL_CLEAN_REMAP_DEBUG"], mode)

    def test_clean_remap_copy_decomposition_uses_memcpy_paths(self):
        cxl_text = (REPO_ROOT / "migration" / "cxl.c").read_text(
            encoding="utf-8")
        copy_start = cxl_text.index(
            "static int cxl_clean_remap_copy_region(")
        copy_end = cxl_text.index(
            "static void cxl_clean_remap_publish_and_remap(", copy_start)
        copy_region = cxl_text[copy_start:copy_end]

        drain_start = cxl_text.index("int cxl_hybrid_clean_remap_drain(")
        drain_end = cxl_text.index(
            "void cxl_hybrid_clean_remap_finalize_deferred(", drain_start)
        drain = cxl_text[drain_start:drain_end]

        self.assertIn("memcpy(scratch, src, region_len);", copy_region)
        self.assertIn("memcpy(dst, scratch, region_len);", copy_region)
        self.assertIn("cxl_clean_remap_prefault_region(dst, region_len",
                      copy_region)
        self.assertIn("cxl_clean_remap_debug_no_publish()", drain)

    def test_build_common_args_accepts_kvm_accel(self):
        args = self.mod.build_common_args(Path("/tmp/boot.img"), accel="kvm")

        machine_index = args.index("-machine")
        self.assertEqual(args[machine_index + 1], "pc,accel=kvm")

    def test_clean_remap_drain_uses_short_bql_windows(self):
        ram_text = (REPO_ROOT / "migration" / "ram.c").read_text(
            encoding="utf-8")
        cxl_text = (REPO_ROOT / "migration" / "cxl.c").read_text(
            encoding="utf-8")
        migration_text = (REPO_ROOT / "migration" / "migration.c").read_text(
            encoding="utf-8")

        iterate_start = ram_text.index("static int ram_save_iterate(")
        iterate_end = ram_text.index("/**\n * ram_save_complete:", iterate_start)
        iterate = ram_text[iterate_start:iterate_end]

        clean_drain_start = cxl_text.index(
            "int cxl_hybrid_clean_remap_drain(")
        clean_drain_end = cxl_text.index(
            "static bool cxl_pending_remaps_have_clean_region",
            clean_drain_start)
        clean_drain = cxl_text[clean_drain_start:clean_drain_end]

        clean_finalize_start = cxl_text.index(
            "static void cxl_clean_remap_finalize_inflight_regions(")
        clean_finalize_end = cxl_text.index(
            "int cxl_hybrid_clean_remap_drain(", clean_finalize_start)
        clean_finalize = cxl_text[clean_finalize_start:clean_finalize_end]

        source_drain_start = cxl_text.index(
            "void cxl_hybrid_drain_source_remaps(")
        source_drain_end = cxl_text.index("static void cxl_try_remap_range(",
                                          source_drain_start)
        source_drain = cxl_text[source_drain_start:source_drain_end]

        lookup_start = cxl_text.index(
            "static bool cxl_clean_remap_lookup_region(")
        lookup_end = cxl_text.index(
            "static bool cxl_clean_remap_region_dirty_now", lookup_start)
        lookup = cxl_text[lookup_start:lookup_end]

        run_start = migration_text.index(
            "static MigIterateState migration_iteration_run(")
        run_end = migration_text.index(
            "static void migration_iteration_finish(", run_start)
        run = migration_text[run_start:run_end]

        self.assertIn("ram_cxl_hybrid_clean_remap_scan(rs)", iterate)
        self.assertNotIn("cxl_hybrid_clean_remap_drain(", iterate)
        self.assertIn("cxl_hybrid_clean_remap_drain(", source_drain)
        short_bql_drain_pattern = re.compile(
            r"if \(cxl_hybrid_clean_remap_enabled\(\)\) \{\s*"
            r"cxl_hybrid_drain_source_remaps\(\);\s*"
            r"\} else \{\s*"
            r"bql_lock\(\);\s*"
            r"cxl_hybrid_drain_source_remaps\(\);\s*"
            r"bql_unlock\(\);\s*"
            r"\}")
        self.assertEqual(len(short_bql_drain_pattern.findall(run)), 2)
        self.assertLess(
            clean_drain.index("ret = cxl_clean_remap_copy_region("),
            clean_drain.index("cxl_clean_remap_finalize_inflight_regions();"))
        self.assertLess(
            clean_drain.index("while ((region_idx = find_first_bit("),
            clean_drain.index("if (used) {"))
        self.assertLess(
            clean_drain.index("if (used) {"),
            clean_drain.index("cxl_clean_remap_finalize_inflight_regions();"))
        self.assertIn("BQL_LOCK_GUARD();", clean_finalize)
        self.assertIn("pause_all_vcpus();", clean_finalize)
        self.assertEqual(clean_finalize.count("pause_all_vcpus();"), 1)
        self.assertIn("ram_cxl_hybrid_sync_dirty_bitmap();", clean_finalize)
        self.assertIn("resume_all_vcpus();", clean_finalize)
        self.assertIn("RCU_READ_LOCK_GUARD();", lookup)

        scan_start = ram_text.index(
            "static void ram_cxl_hybrid_clean_remap_scan(")
        scan_end = ram_text.index("void ram_release_page(", scan_start)
        scan = ram_text[scan_start:scan_end]
        self.assertIn("BQL_LOCK_GUARD();\n"
                      "        ram_cxl_hybrid_sync_dirty_bitmap();", scan)

    def test_clean_remap_postcopy_handoff_skips_remapped_staged_pages(self):
        cxl_text = (REPO_ROOT / "migration" / "cxl.c").read_text(
            encoding="utf-8")

        run_start = cxl_text.index(
            "static int cxl_hybrid_publish_staged_run_for_postcopy(")
        run_end = cxl_text.index(
            "int cxl_hybrid_publish_staged_pages_for_postcopy(",
            run_start)
        run = cxl_text[run_start:run_end]

        scan_start = cxl_text.index(
            "int cxl_hybrid_publish_staged_pages_for_postcopy(")
        scan_end = cxl_text.index(
            "int cxl_hybrid_begin_source_run_with_precopy_remaps(",
            scan_start)
        scan = cxl_text[scan_start:scan_end]

        self.assertIn("cxl_state.remapped_pages_bmap", scan)
        self.assertIn("find_next_bit(cxl_state.remapped_pages_bmap", scan)
        self.assertIn("find_next_zero_bit(cxl_state.remapped_pages_bmap", scan)
        self.assertNotIn("source_remapped = test_bit(page_idx, "
                         "cxl_state.remapped_pages_bmap)", run)

    def test_clean_remap_ramblock_write_skips_already_visible_remaps(self):
        cxl_text = (REPO_ROOT / "migration" / "cxl.c").read_text(
            encoding="utf-8")

        write_start = cxl_text.index("static int cxl_ramblock_write_pages(")
        write_end = cxl_text.index("int cxl_write_ramblock_iov(",
                                   write_start)
        write_pages = cxl_text[write_start:write_end]
        skip_start = write_pages.index(
            "if (action == MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_VISIBLE")
        skip_end = write_pages.index("if (!out_niov)", skip_start)
        skip_block = write_pages[skip_start:skip_end]

        self.assertIn("MIGRATION_POSTCOPY_CXL_RAM_STREAM_SKIP_ALREADY_VISIBLE",
                      skip_block)
        self.assertIn("trace_cxl_hybrid_ram_stream_skip_visible", skip_block)
        self.assertIn("continue;", skip_block)

    def test_destination_owned_visible_ram_stream_skip_does_not_publish(self):
        ram_text = (REPO_ROOT / "migration" / "ram.c").read_text(
            encoding="utf-8")

        source_branch = re.search(
            r"RAM_CXL_HYBRID_REGION_WRITE_SKIP_SOURCE_REMAPPED\).*?"
            r"RAM_CXL_HYBRID_REGION_WRITE_SKIP_DESTINATION_OWNED\)",
            ram_text,
            re.S)
        self.assertIsNotNone(source_branch)
        self.assertIn("ram_cxl_hybrid_publish_visible_page_batched",
                      source_branch.group(0))

        for match in re.finditer(
                r"RAM_CXL_HYBRID_REGION_WRITE_SKIP_DESTINATION_OWNED\).*?"
                r"return 1;",
                ram_text,
                re.S):
            self.assertNotIn("ram_cxl_hybrid_publish_visible_page_batched",
                             match.group(0))

    def test_set_params_hybrid_auto_accepts_clean_remap_overrides(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_params(
            object(), "/tmp/cxl.img", "hybrid_postcopy_auto", "heavy",
            shared_backing=True,
            clean_remap_enable=True,
            clean_remap_copy_budget=8388608,
            clean_remap_throttle_us=200,
            clean_remap_prefault_mode="touch",
        )

        _cmd, args = calls[0]
        self.assertEqual(args["x-cxl-clean-remap-enable"], True)
        self.assertEqual(args["x-cxl-clean-remap-copy-budget"], 8388608)
        self.assertEqual(args["x-cxl-clean-remap-throttle-us"], 200)
        self.assertEqual(args["x-cxl-clean-remap-prefault-mode"], "touch")

    def test_set_params_hybrid_manual_uses_hybrid_transport(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_params(object(), "/tmp/cxl.img",
                            "hybrid_postcopy_manual", "heavy",
                            shared_backing=True)

        _cmd, args = calls[0]
        self.assertEqual(args["cxl-path"], "/tmp/cxl.img")
        self.assertTrue(args["x-cxl-shared-backing"])
        self.assertEqual(args["x-cxl-warm-transport"], "cxl-offset")
        self.assertEqual(args["x-cxl-dst-install-policy"], "on-demand")

    def test_resolve_threshold_profile_balanced(self):
        profile = self.mod.resolve_threshold_profile("balanced")

        self.assertEqual(profile["name"], "balanced")
        self.assertEqual(profile["x-cxl-switch-dirty-threshold"], 1)
        self.assertEqual(profile["x-cxl-switch-max-iters"], 20)
        self.assertEqual(profile["x-cxl-switch-max-precopy-ms"], 0)
        self.assertEqual(profile["x-cxl-switch-min-remaining"], 8 * 1024 * 1024)

    def test_resolve_threshold_profile_with_overrides(self):
        profile = self.mod.resolve_threshold_profile(
            "balanced",
            dirty_threshold=1,
            max_iters=8,
            max_precopy_ms=420,
            min_remaining=16 * 1024 * 1024,
        )

        self.assertEqual(profile["name"], "custom")
        self.assertEqual(profile["x-cxl-switch-max-iters"], 8)
        self.assertEqual(profile["x-cxl-switch-max-precopy-ms"], 420)
        self.assertEqual(profile["x-cxl-switch-min-remaining"], 16 * 1024 * 1024)

    def test_set_params_hybrid_auto_uses_threshold_profile_values(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        thresholds = {
            "name": "aggressive",
            "x-cxl-switch-dirty-threshold": 1,
            "x-cxl-switch-max-iters": 8,
            "x-cxl-switch-max-precopy-ms": 420,
            "x-cxl-switch-min-remaining": 16 * 1024 * 1024,
        }

        self.mod.set_params(
            object(), "/tmp/cxl.img", "hybrid_postcopy_auto", "heavy",
            shared_backing=True, thresholds=thresholds
        )

        _cmd, args = calls[0]
        self.assertEqual(args["x-cxl-switch-max-iters"], 8)
        self.assertEqual(args["x-cxl-switch-max-precopy-ms"], 420)
        self.assertEqual(args["x-cxl-switch-min-remaining"], 16 * 1024 * 1024)

    def test_set_params_hybrid_auto_accepts_prefetch_and_install_overrides(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_params(
            object(), "/tmp/cxl.img", "hybrid_postcopy_auto", "heavy",
            shared_backing=True,
            prefetch_rate=16 * 1024 * 1024,
            dst_install_policy="eager",
        )

        _cmd, args = calls[0]
        self.assertEqual(args["x-cxl-prefetch-rate"], 16 * 1024 * 1024)
        self.assertEqual(args["x-cxl-dst-install-policy"], "eager")

    def test_set_params_hybrid_auto_accepts_fault_control_plane_override(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_params(
            object(), "/tmp/cxl.img", "hybrid_postcopy_auto", "heavy",
            shared_backing=True,
            fault_control_plane="cxl",
        )

        _cmd, args = calls[0]
        self.assertEqual(args["x-cxl-fault-control-plane"], "cxl")

    def test_fault_resolve_mode_arg_sets_qmp_parameter(self):
        chw = self.mod
        args = chw.parse_args([
            "--pressure", "remap_heavy",
            "--mode", "hybrid_postcopy_manual",
            "--x-cxl-fault-resolve-mode", "region-remap",
        ])
        params = chw.build_migration_parameters(args,
                                                "hybrid_postcopy_manual")
        self.assertEqual(params["x-cxl-fault-resolve-mode"], "region-remap")

    def test_fault_resolve_mode_qapi_and_qom_property_exist(self):
        qapi_text = (REPO_ROOT / "qapi" / "migration.json").read_text()
        options_text = (REPO_ROOT / "migration" / "options.c").read_text()
        qdev_header_text = (
            REPO_ROOT / "include" / "hw" / "core" /
            "qdev-properties-system.h"
        ).read_text()
        qdev_text = (
            REPO_ROOT / "hw" / "core" / "qdev-properties-system.c"
        ).read_text()

        self.assertIn("'enum': 'CXLHybridFaultResolveMode'", qapi_text)
        self.assertIn("'*x-cxl-fault-resolve-mode': {", qapi_text)
        self.assertIn("extern const PropertyInfo qdev_prop_cxl_hybrid_fault_resolve_mode;",
                      qdev_header_text)
        self.assertIn("#define DEFINE_PROP_CXL_HYBRID_FAULT_RESOLVE_MODE(",
                      qdev_header_text)
        self.assertIn("QEMU_BUILD_BUG_ON(sizeof(CXLHybridFaultResolveMode) != sizeof(int));",
                      qdev_text)
        self.assertIn("const PropertyInfo qdev_prop_cxl_hybrid_fault_resolve_mode = {",
                      qdev_text)
        self.assertIn("&CXLHybridFaultResolveMode_lookup", qdev_text)
        self.assertIn("DEFINE_PROP_CXL_HYBRID_FAULT_RESOLVE_MODE",
                      options_text)
        self.assertNotIn("qdev_prop_cxl_hybrid_fault_resolve_mode = {",
                         options_text)
        self.assertNotIn("#include \"hw/core/qdev-prop-internal.h\"",
                         options_text)

    def test_fault_resolve_mode_cli_passes_override_to_run_matrix(self):
        calls = []

        def fake_run_pressure_matrix(base, pressures, modes, threshold_profile=None,
                                    repeat=1, migration_timeout=60.0,
                                    prefetch_rate=None, dst_install_policy=None,
                                    fault_control_plane=None,
                                    fault_resolve_mode=None,
                                    cxl_path_override=None, **kwargs):
            calls.append({
                "base": str(base),
                "pressures": list(pressures),
                "modes": list(modes),
                "threshold_profile": threshold_profile,
                "repeat": repeat,
                "migration_timeout": migration_timeout,
                "prefetch_rate": prefetch_rate,
                "dst_install_policy": dst_install_policy,
                "fault_control_plane": fault_control_plane,
                "fault_resolve_mode": fault_resolve_mode,
                "cxl_path_override": cxl_path_override,
                "brake_enable": kwargs.get("brake_enable"),
            })
            return {
                "pressures": list(pressures),
                "modes": list(modes),
                "results": {},
                "summary": [],
                "summary_grouped": [],
            }

        argv = [
            "cxl-hybrid-warm-experiment.py",
            "--pressure", "remap_heavy",
            "--mode", "hybrid_postcopy_manual",
            "--x-cxl-fault-resolve-mode", "region-remap",
        ]
        original_maybe_reexec = self.mod.maybe_reexec_with_sudo
        original_qemu = self.mod.QEMU
        original_trace_events = self.mod.TRACE_EVENTS
        original_boot_asm = self.mod.BOOT_ASM
        original_mkdtemp = self.mod.tempfile.mkdtemp
        original_rmtree = self.mod.shutil.rmtree
        had_print = hasattr(self.mod, "print")
        original_print = getattr(self.mod, "print", None)
        original_run_pressure_matrix = self.mod.run_pressure_matrix
        original_argv = self.mod.sys.argv
        try:
            self.mod.maybe_reexec_with_sudo = lambda: None
            self.mod.QEMU = SCRIPT_PATH
            self.mod.TRACE_EVENTS = SCRIPT_PATH
            self.mod.BOOT_ASM = SCRIPT_PATH
            self.mod.tempfile.mkdtemp = (
                lambda *args, **kwargs: "/tmp/cxl-exp-test"
            )
            self.mod.shutil.rmtree = lambda _path: None
            self.mod.print = lambda *_args, **_kwargs: None
            self.mod.run_pressure_matrix = fake_run_pressure_matrix
            self.mod.sys.argv = argv
            self.mod.main()
        finally:
            self.mod.maybe_reexec_with_sudo = original_maybe_reexec
            self.mod.QEMU = original_qemu
            self.mod.TRACE_EVENTS = original_trace_events
            self.mod.BOOT_ASM = original_boot_asm
            self.mod.tempfile.mkdtemp = original_mkdtemp
            self.mod.shutil.rmtree = original_rmtree
            if had_print:
                self.mod.print = original_print
            else:
                delattr(self.mod, "print")
            self.mod.run_pressure_matrix = original_run_pressure_matrix
            self.mod.sys.argv = original_argv

        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["fault_resolve_mode"], "region-remap")

    def test_region_remap_stats_are_exported_to_qapi_and_summary(self):
        qapi_text = (REPO_ROOT / "qapi" / "migration.json").read_text()
        cxl_text = (REPO_ROOT / "migration" / "cxl.c").read_text()

        for field in (
            "dst-region-map-successes",
            "dst-region-fallback-copies",
            "dst-region-wait-samples",
            "dst-region-wait-time-ns",
            "region-publish-requests",
        ):
            self.assertIn(f"'{field}'", qapi_text)

        self.assertIn("info->x_cxl->dst_region_map_successes", cxl_text)
        self.assertIn("info->x_cxl->dst_region_fallback_copies", cxl_text)
        self.assertIn("info->x_cxl->region_publish_requests", cxl_text)

        summary = self.mod.extract_summary([
            {
                "x-cxl": {
                    "dst-region-map-successes": 1,
                    "dst-region-wait-samples": 1,
                    "dst-region-wait-time-ns": 2000,
                    "region-publish-requests": 1,
                },
                "dst-query-migrate": {
                    "x-cxl": {
                        "dst-region-map-successes": 3,
                        "dst-region-fallback-copies": 2,
                        "dst-region-wait-samples": 4,
                        "dst-region-wait-time-ns": 8000,
                        "max-dst-region-wait-time-ns": 5000,
                        "region-publish-requests": 5,
                        "region-publish-pages": 512,
                        "region-publish-time-ns": 9000,
                    }
                },
            }
        ])

        self.assertEqual(summary["max_dst_region_map_successes"], 3)
        self.assertEqual(summary["max_dst_region_fallback_copies"], 2)
        self.assertEqual(summary["dst_region_wait_samples"], 4)
        self.assertEqual(summary["dst_region_wait_time_ns"], 8000)
        self.assertEqual(summary["max_dst_region_wait_time_ns"], 5000)
        self.assertEqual(summary["region_publish_requests"], 5)
        self.assertEqual(summary["region_publish_pages"], 512)
        self.assertEqual(summary["region_publish_time_ns"], 9000)

    def test_region_remap_trace_events_are_counted(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            trace = Path(tmpdir) / "trace.log"
            trace.write_text(
                "\n".join([
                    "cxl_hybrid_region_publish_complete first-page=0 pages=512 published=128 gen=1",
                    "cxl_hybrid_region_wait_begin first-page=0 pages=512 gen=1",
                    "cxl_hybrid_region_wait_complete first-page=0 pages=512 gen=1 ret=0",
                    "cxl_hybrid_dst_region_remap pc.ram/0x200000 len=2097152 cxl=0x400000 region=1",
                ]) + "\n",
                encoding="ascii",
            )

            counts = self.mod.parse_trace_log(trace)

        self.assertEqual(counts["region_publish_complete"], 1)
        self.assertEqual(counts["region_publish_pages"], 512)
        self.assertEqual(counts["region_publish_published_pages"], 128)
        self.assertEqual(counts["region_wait_begin"], 1)
        self.assertEqual(counts["region_wait_complete"], 1)
        self.assertEqual(counts["region_wait_complete_failures"], 0)
        self.assertEqual(counts["dst_region_remap"], 1)

    def test_fault_control_plane_option_is_declared(self):
        qapi_source = (SCRIPT_PATH.parent.parent / "qapi" /
                       "migration.json").resolve()
        options_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "options.c").resolve()
        options_header = (SCRIPT_PATH.parent.parent / "migration" /
                          "options.h").resolve()
        qdev_header = (SCRIPT_PATH.parent.parent / "include" / "hw" / "core" /
                       "qdev-properties-system.h").resolve()
        qdev_source = (SCRIPT_PATH.parent.parent / "hw" / "core" /
                       "qdev-properties-system.c").resolve()

        qapi_text = qapi_source.read_text(encoding="utf-8")
        options_text = options_source.read_text(encoding="utf-8")
        options_header_text = options_header.read_text(encoding="utf-8")
        qdev_header_text = qdev_header.read_text(encoding="utf-8")
        qdev_text = qdev_source.read_text(encoding="utf-8")

        self.assertIn("# @CXLHybridFaultControlPlane:", qapi_text)
        self.assertIn("{ 'enum': 'CXLHybridFaultControlPlane',", qapi_text)
        self.assertIn("{ 'name': 'stream', 'features': [ 'unstable' ] }",
                      qapi_text)
        self.assertIn("{ 'name': 'cxl', 'features': [ 'unstable' ] }",
                      qapi_text)
        self.assertIn("'*x-cxl-fault-control-plane': {", qapi_text)
        self.assertIn("'type': 'CXLHybridFaultControlPlane'", qapi_text)

        self.assertIn("extern const PropertyInfo qdev_prop_cxl_hybrid_fault_control_plane;",
                      qdev_header_text)
        self.assertIn("#define DEFINE_PROP_CXL_HYBRID_FAULT_CONTROL_PLANE(",
                      qdev_header_text)
        self.assertIn("QEMU_BUILD_BUG_ON(sizeof(CXLHybridFaultControlPlane) != sizeof(int));",
                      qdev_text)
        self.assertIn("const PropertyInfo qdev_prop_cxl_hybrid_fault_control_plane = {",
                      qdev_text)
        self.assertIn("&CXLHybridFaultControlPlane_lookup", qdev_text)

        self.assertIn("DEFINE_PROP_CXL_HYBRID_FAULT_CONTROL_PLANE(\"x-cxl-fault-control-plane\",",
                      options_text)
        self.assertIn("parameters.x_cxl_fault_control_plane,", options_text)
        self.assertIn("CXLHybridFaultControlPlane migrate_cxl_fault_control_plane(void)",
                      options_text)
        self.assertIn("return s->parameters.x_cxl_fault_control_plane;",
                      options_text)
        self.assertIn("bool migrate_cxl_fault_control_plane_cxl(void)",
                      options_text)
        self.assertIn("return migrate_cxl_fault_control_plane() ==\n           CXL_HYBRID_FAULT_CONTROL_PLANE_CXL;",
                      options_text)

        self.assertIn("CXLHybridFaultControlPlane migrate_cxl_fault_control_plane(void);",
                      options_header_text)
        self.assertIn("bool migrate_cxl_fault_control_plane_cxl(void);",
                      options_header_text)

    def test_cxl_fault_control_module_is_declared(self):
        meson_source = (SCRIPT_PATH.parent.parent / "migration" /
                        "meson.build").resolve()
        cxl_header = (SCRIPT_PATH.parent.parent / "migration" /
                      "cxl.h").resolve()
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()

        meson_text = meson_source.read_text(encoding="utf-8")
        cxl_header_text = cxl_header.read_text(encoding="utf-8")
        control_text = control_source.read_text(encoding="utf-8")

        self.assertIn("'cxl-hybrid-control.c',", meson_text)

        self.assertIn("#define CXL_HYBRID_CTRL_MAGIC", cxl_header_text)
        self.assertIn("typedef struct CXLHybridControlHeader {",
                      cxl_header_text)
        self.assertIn("typedef struct CXLHybridFaultRequestRecord {",
                      cxl_header_text)
        self.assertIn("typedef struct CXLHybridFaultReadyRecord {",
                      cxl_header_text)
        self.assertIn("int cxl_hybrid_control_init_source(Error **errp);",
                      cxl_header_text)
        self.assertIn("int cxl_hybrid_control_init_destination(Error **errp);",
                      cxl_header_text)
        self.assertIn("void cxl_hybrid_control_cleanup_source(void);",
                      cxl_header_text)
        self.assertIn("void cxl_hybrid_control_cleanup_destination(void);",
                      cxl_header_text)
        self.assertIn("int cxl_hybrid_ctrl_enqueue_fault_request(uint64_t page_index,",
                      cxl_header_text)

        self.assertIn("static void cxl_hybrid_ctrl_publish_request(",
                      control_text)
        self.assertIn("static void cxl_hybrid_ctrl_publish_ready(",
                      control_text)
        self.assertIn("static bool cxl_hybrid_ctrl_try_dequeue_request(",
                      control_text)
        self.assertIn("static bool cxl_hybrid_ctrl_try_dequeue_ready(",
                      control_text)

    def test_cxl_fault_control_workers_have_lifecycle_hooks(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()

        cxl_text = cxl_source.read_text(encoding="utf-8")
        control_text = control_source.read_text(encoding="utf-8")

        self.assertIn("static void *cxl_hybrid_ctrl_request_worker_thread(void *opaque)",
                      control_text)
        self.assertIn("static void *cxl_hybrid_ctrl_ready_poller_thread(void *opaque)",
                      control_text)
        self.assertIn("qemu_thread_create(&state->request_worker, \"cxl-ctrl-req\",",
                      control_text)
        self.assertIn("qemu_thread_create(&state->ready_poller, \"cxl-ctrl-ready\",",
                      control_text)
        self.assertIn("qemu_thread_join(&state->request_worker);", control_text)
        self.assertIn("qemu_thread_join(&state->ready_poller);", control_text)

        source_init_start = cxl_text.index("bool cxl_hybrid_init_source(void)")
        cleanup_start = cxl_text.index("void cxl_hybrid_cleanup_source(void)")
        source_init = cxl_text[source_init_start:cleanup_start]
        destination_init_start = cxl_text.index("bool cxl_hybrid_init_destination(Error **errp)")
        write_redirect_start = cxl_text.index("static bool cxl_write_redirect_enabled(void)")
        destination_init = cxl_text[destination_init_start:write_redirect_start]
        destination_cleanup_start = cxl_text.index("static void cxl_destination_staging_cleanup(void)")
        destination_init_again = cxl_text.index("bool cxl_hybrid_init_destination(Error **errp)")
        destination_cleanup = cxl_text[destination_cleanup_start:destination_init_again]
        source_cleanup = cxl_text[cleanup_start:cxl_text.index("bool cxl_hybrid_enabled(void)")]

        self.assertIn("migrate_cxl_fault_control_plane_cxl()", source_init)
        self.assertIn("cxl_hybrid_control_init_source(&local_err)", source_init)
        self.assertIn("migrate_cxl_fault_control_plane_cxl()", destination_init)
        self.assertIn("cxl_hybrid_control_init_destination(errp)", destination_init)
        self.assertIn("cxl_hybrid_control_cleanup_source();", source_cleanup)
        self.assertIn("cxl_hybrid_control_cleanup_destination();",
                      destination_cleanup)

    def test_cxl_fault_control_traces_are_declared(self):
        trace_events = (SCRIPT_PATH.parent.parent / "migration" /
                        "trace-events").resolve()
        text = trace_events.read_text(encoding="utf-8")

        self.assertIn("cxl_hybrid_ctrl_request_worker_start", text)
        self.assertIn("cxl_hybrid_ctrl_request_worker_stop", text)
        self.assertIn("cxl_hybrid_ctrl_ready_poller_start", text)
        self.assertIn("cxl_hybrid_ctrl_ready_poller_stop", text)

    def test_fault_publish_core_is_backend_neutral(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()

        cxl_text = cxl_source.read_text(encoding="utf-8")
        control_text = control_source.read_text(encoding="utf-8")

        self.assertIn("int cxl_hybrid_publish_fault_request_core(",
                      cxl_text)
        core_start = cxl_text.index("int cxl_hybrid_publish_fault_request_core(")
        handle_start = cxl_text.index("int cxl_hybrid_handle_publish_request(")
        core = cxl_text[core_start:cxl_text.index("int cxl_hybrid_handle_publish_quiesce(")]
        self.assertIn("CXLHybridFaultReadyRecord *primary_ready", cxl_text)
        self.assertIn("cxl_hybrid_publish_page_to_cxl(", core)
        self.assertIn("primary_ready->page_index =", core)
        self.assertIn("primary_ready->cxl_offset =", core)
        self.assertNotIn("qemu_savevm_send_cxl_hybrid_publish_ready(", core)

        self.assertIn("cxl_hybrid_publish_fault_request_core(", control_text)

    def test_cxl_request_worker_publishes_without_ready_ring(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        control_text = control_source.read_text(encoding="utf-8")

        worker_start = control_text.index(
            "static void *cxl_hybrid_ctrl_request_worker_thread(void *opaque)")
        ready_poller_start = control_text.index(
            "static void *cxl_hybrid_ctrl_ready_poller_thread(void *opaque)")
        worker = control_text[worker_start:ready_poller_start]

        self.assertIn("cxl_hybrid_ctrl_dequeue_fault_request(&record)", worker)
        self.assertIn("cxl_hybrid_lookup_global_page(record.page_index, &block,",
                      worker)
        self.assertIn("cxl_hybrid_publish_fault_request_core(", worker)
        self.assertNotIn("cxl_hybrid_ctrl_ring_ready_consumer", worker)
        self.assertNotIn("cxl_hybrid_ctrl_enqueue_fault_ready(", worker)

    def test_control_workers_filter_on_shared_header_generation(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        control_text = control_source.read_text(encoding="utf-8")

        worker_start = control_text.index(
            "static void *cxl_hybrid_ctrl_request_worker_thread(void *opaque)")
        ready_poller_start = control_text.index(
            "static void *cxl_hybrid_ctrl_ready_poller_thread(void *opaque)")
        start_helper = control_text.index(
            "static void cxl_hybrid_ctrl_start_request_worker(")
        worker = control_text[worker_start:ready_poller_start]
        poller = control_text[ready_poller_start:start_helper]

        self.assertIn("record.generation != state->hdr->generation", worker)
        self.assertIn("record.generation != state->hdr->generation", poller)
        self.assertNotIn("record.generation != cxl_hybrid_fault_publish_generation()",
                         worker)
        self.assertNotIn("record.generation != cxl_hybrid_fault_publish_generation()",
                         poller)

    def test_fault_path_uses_shared_bitmap_and_self_registration(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        wait_start = text.index("int cxl_hybrid_wait_and_resolve_fault")
        publish_ready_start = text.index("void cxl_hybrid_get_publish_stats")
        wait = text[wait_start:publish_ready_start]

        self.assertIn("if (migrate_cxl_fault_control_plane_cxl()) {", wait)
        self.assertIn("page_index = cxl_global_page_index(rb, offset);", wait)
        self.assertIn("cxl_hybrid_ctrl_page_visible(page_index, generation)", wait)
        self.assertIn("ret = cxl_hybrid_ctrl_enqueue_fault_request(page_index, generation,",
                      wait)
        self.assertIn("ret = cxl_hybrid_ctrl_wait_page_visible(page_index, generation, errp);",
                      wait)
        self.assertIn("cxl_hybrid_source_page_cxl_offset(ramblock, offset,", wait)
        self.assertIn("cxl_hybrid_dst_staging_register_external_page(", wait)
        self.assertIn("} else {", wait)
        self.assertIn("ret = migrate_send_rp_cxl_publish_req(", wait)
        cxl_branch = wait[wait.index("if (migrate_cxl_fault_control_plane_cxl()) {"):
                          wait.index("} else {")]
        self.assertNotIn("cxl_hybrid_dst_staging_wait_range_present(",
                         cxl_branch)
        self.assertNotIn("ready_recv_ns", cxl_branch)

    def test_ready_poller_registers_pages_into_staging(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        control_text = control_source.read_text(encoding="utf-8")

        poller_start = control_text.index(
            "static void *cxl_hybrid_ctrl_ready_poller_thread(void *opaque)")
        start_helper = control_text.index(
            "static void cxl_hybrid_ctrl_start_request_worker(")
        poller = control_text[poller_start:start_helper]

        self.assertIn("cxl_hybrid_ctrl_dequeue_fault_ready(&record)", poller)
        self.assertIn("if (record.generation != state->hdr->generation) {",
                      poller)
        self.assertIn("cxl_hybrid_handle_fault_ready_record(&record, &local_err);",
                      poller)

    def test_control_region_maps_shared_visible_bitmap(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        text = control_source.read_text(encoding="utf-8")

        self.assertIn("unsigned long *visible_bitmap;", text)
        self.assertIn("cxl_hybrid_control_region.visible_bitmap =", text)
        self.assertIn("state->visible_bitmap = cxl_hybrid_control_region.visible_bitmap;", text)
        self.assertIn("cxl_hybrid_control_visible_bitmap_bytes(total_pages)", text)

    def test_control_region_source_begin_resets_visible_bitmap(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        text = control_source.read_text(encoding="utf-8")

        begin_start = text.index("int cxl_hybrid_control_begin_source_run")
        activate_start = text.index("int cxl_hybrid_control_activate_destination")
        begin = text[begin_start:activate_start]

        self.assertIn("cxl_hybrid_control_reset_run_state(", begin)
        self.assertIn("cxl_hybrid_control_source.visible_bitmap", begin)

    def test_control_wrappers_expose_shared_visible_bitmap(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        cxl_header = (SCRIPT_PATH.parent.parent / "migration" / "cxl.h").resolve()

        control_text = control_source.read_text(encoding="utf-8")
        header_text = cxl_header.read_text(encoding="utf-8")

        self.assertIn("bool cxl_hybrid_ctrl_page_visible(uint64_t page_index,",
                      header_text)
        self.assertIn("void cxl_hybrid_ctrl_set_page_visible(uint64_t page_index,",
                      header_text)
        self.assertIn("void cxl_hybrid_ctrl_clear_page_visible(uint64_t page_index);",
                      header_text)
        self.assertIn("int cxl_hybrid_ctrl_wait_page_visible(uint64_t page_index,",
                      header_text)

        self.assertIn("bool cxl_hybrid_ctrl_page_visible(uint64_t page_index,",
                      control_text)
        self.assertIn("void cxl_hybrid_ctrl_set_page_visible(uint64_t page_index,",
                      control_text)
        self.assertIn("void cxl_hybrid_ctrl_clear_page_visible(uint64_t page_index)",
                      control_text)
        self.assertIn("int cxl_hybrid_ctrl_wait_page_visible(uint64_t page_index,",
                      control_text)

    def test_destination_activate_validates_shared_bitmap_geometry(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        text = control_source.read_text(encoding="utf-8")

        activate_start = text.index("int cxl_hybrid_control_activate_destination")
        cleanup_start = text.index("void cxl_hybrid_control_cleanup_source(void)")
        activate = text[activate_start:cleanup_start]

        self.assertIn("hdr->visible_page_words", activate)
        self.assertIn("expected_visible_page_words", activate)
        self.assertIn("header=%u expected=%zu", activate)

    def test_control_visible_bitmap_geometry_uses_migratable_ram_iteration(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        text = control_source.read_text(encoding="utf-8")

        helper_start = text.index("static uint64_t cxl_hybrid_ctrl_total_pages(void)")
        next_start = text.index("static uint32_t cxl_hybrid_ctrl_visible_page_words")
        helper = text[helper_start:next_start]

        self.assertIn("RAMBLOCK_FOREACH_MIGRATABLE(block)", helper)
        self.assertNotIn("RAMBLOCK_FOREACH_NOT_IGNORED(block)", helper)

    def test_control_clear_page_visible_uses_source_shared_state(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        text = control_source.read_text(encoding="utf-8")

        clear_start = text.index("void cxl_hybrid_ctrl_clear_page_visible(uint64_t page_index)")
        wait_start = text.index("int cxl_hybrid_ctrl_wait_page_visible(")
        clear = text[clear_start:wait_start]

        self.assertIn("CXLHybridControlState *state = &cxl_hybrid_control_source;", clear)
        self.assertNotIn("cxl_hybrid_control_destination", clear)

    def test_control_wait_page_visible_has_no_fixed_timeout(self):
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()
        text = control_source.read_text(encoding="utf-8")

        wait_start = text.index("int cxl_hybrid_ctrl_wait_page_visible(")
        enqueue_start = text.index("int cxl_hybrid_ctrl_enqueue_fault_request(")
        wait = text[wait_start:enqueue_start]

        self.assertIn("while (!cxl_hybrid_ctrl_page_visible(page_index, generation))",
                      wait)
        self.assertIn("cxl_hybrid_control_destination.hdr->generation != generation",
                      wait)
        self.assertIn("g_usleep(50);", wait)
        self.assertNotIn("retries", wait)
        self.assertNotIn("Timed out waiting", wait)
        self.assertNotIn("-ETIMEDOUT", wait)

    def test_default_migration_timeout_secs(self):
        self.assertEqual(self.mod.DEFAULT_MIGRATION_TIMEOUT_SECS, 60.0)

    def test_set_params_hybrid_payload_is_rejected_in_cxl_only_mode(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        with self.assertRaisesRegex(ValueError, "payload"):
            self.mod.set_params(object(), "/tmp/cxl.img",
                                "hybrid_postcopy_payload", "light")
        self.assertEqual(calls, [])

    def test_set_params_hybrid_cxl_offset_forces_descriptor_transport(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_params(object(), "/tmp/cxl.img",
                            "hybrid_postcopy_cxl_offset", "light",
                            shared_backing=True)

        _cmd, args = calls[0]
        self.assertTrue(args["x-cxl-shared-backing"])
        self.assertEqual(args["x-cxl-dst-install-policy"], "on-demand")
        self.assertEqual(args["x-cxl-warm-transport"], "cxl-offset")

    def test_default_modes_exclude_payload_hybrid(self):
        self.assertNotIn("hybrid_postcopy_payload",
                         self.mod.DEFAULT_MIGRATION_MODE_CSV.split(","))
        self.assertIn("hybrid_postcopy_auto",
                      self.mod.DEFAULT_MIGRATION_MODE_CSV.split(","))
        self.assertIn("hybrid_postcopy_manual", self.mod.MIGRATION_MODES)
        self.assertIn("native_postcopy_stream", self.mod.MIGRATION_MODES)
        self.assertNotIn("hybrid_postcopy_manual",
                         self.mod.DEFAULT_MIGRATION_MODE_CSV.split(","))
        self.assertNotIn("native_postcopy_stream",
                         self.mod.DEFAULT_MIGRATION_MODE_CSV.split(","))

    def test_set_params_hybrid_cxl_offset_requires_shared_backing(self):
        with self.assertRaisesRegex(ValueError, "shared_backing"):
            self.mod.set_params(object(), "/tmp/cxl.img",
                                "hybrid_postcopy_cxl_offset", "light")

    def test_main_passes_prefetch_install_fault_control_and_cxl_path_overrides_to_run_matrix(self):
        calls = []

        def fake_run_pressure_matrix(base, pressures, modes, threshold_profile=None,
                                    repeat=1, migration_timeout=60.0,
                                    prefetch_rate=None, dst_install_policy=None,
                                    fault_control_plane=None,
                                    fault_resolve_mode=None,
                                    cxl_path_override=None, **kwargs):
            calls.append({
                "base": str(base),
                "pressures": list(pressures),
                "modes": list(modes),
                "threshold_profile": threshold_profile,
                "repeat": repeat,
                "migration_timeout": migration_timeout,
                "prefetch_rate": prefetch_rate,
                "dst_install_policy": dst_install_policy,
                "fault_control_plane": fault_control_plane,
                "fault_resolve_mode": fault_resolve_mode,
                "cxl_path_override": cxl_path_override,
                "brake_enable": kwargs.get("brake_enable"),
            })
            return {
                "pressures": list(pressures),
                "modes": list(modes),
                "results": {},
                "summary": [],
                "summary_grouped": [],
            }

        self.mod.run_pressure_matrix = fake_run_pressure_matrix
        argv = [
            "cxl-hybrid-warm-experiment.py",
            "--pressure", "heavy",
            "--mode", "hybrid_postcopy_auto",
            "--threshold-profile", "aggressive",
            "--repeat", "2",
            "--migration-timeout", "120",
            "--x-cxl-prefetch-rate", str(16 * 1024 * 1024),
            "--x-cxl-dst-install-policy", "eager",
            "--x-cxl-fault-control-plane", "cxl",
            "--cxl-path", "/dev/dax0.0",
        ]
        original_maybe_reexec = self.mod.maybe_reexec_with_sudo
        original_qemu = self.mod.QEMU
        original_trace_events = self.mod.TRACE_EVENTS
        original_boot_asm = self.mod.BOOT_ASM
        original_mkdtemp = self.mod.tempfile.mkdtemp
        original_rmtree = self.mod.shutil.rmtree
        had_print = hasattr(self.mod, "print")
        original_print = getattr(self.mod, "print", None)
        original_run_pressure_matrix = self.mod.run_pressure_matrix
        original_argv = self.mod.sys.argv
        try:
            self.mod.maybe_reexec_with_sudo = lambda: None
            self.mod.QEMU = SCRIPT_PATH
            self.mod.TRACE_EVENTS = SCRIPT_PATH
            self.mod.BOOT_ASM = SCRIPT_PATH
            self.mod.tempfile.mkdtemp = (
                lambda *args, **kwargs: "/tmp/cxl-exp-test"
            )
            self.mod.shutil.rmtree = lambda _path: None
            self.mod.print = lambda *_args, **_kwargs: None
            self.mod.run_pressure_matrix = fake_run_pressure_matrix
            self.mod.sys.argv = argv
            self.mod.main()
        finally:
            self.mod.maybe_reexec_with_sudo = original_maybe_reexec
            self.mod.QEMU = original_qemu
            self.mod.TRACE_EVENTS = original_trace_events
            self.mod.BOOT_ASM = original_boot_asm
            self.mod.tempfile.mkdtemp = original_mkdtemp
            self.mod.shutil.rmtree = original_rmtree
            if had_print:
                self.mod.print = original_print
            else:
                delattr(self.mod, "print")
            self.mod.run_pressure_matrix = original_run_pressure_matrix
            self.mod.sys.argv = original_argv

        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["prefetch_rate"], 16 * 1024 * 1024)
        self.assertEqual(calls[0]["dst_install_policy"], "eager")
        self.assertEqual(calls[0]["fault_control_plane"], "cxl")
        self.assertEqual(calls[0]["cxl_path_override"], "/dev/dax0.0")
        self.assertEqual(calls[0]["repeat"], 2)

    def test_fault_hit_probe_uses_host_page_range(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        try_resolve_start = text.index("int cxl_hybrid_try_resolve_fault")
        wait_resolve_start = text.index("int cxl_hybrid_wait_and_resolve_fault")
        try_resolve = text[try_resolve_start:wait_resolve_start]

        self.assertIn("pagesize = qemu_ram_pagesize(rb);", try_resolve)
        self.assertIn("cxl_hybrid_dst_staging_range_present(ramblock, offset, pagesize)",
                      try_resolve)
        self.assertNotIn("cxl_hybrid_dst_staging_page_present(ramblock, offset)",
                         try_resolve)

    def test_staging_cleanup_keeps_sync_primitives_live(self):
        metadata_source = (
            SCRIPT_PATH.parent.parent / "migration" / "cxl-hybrid-metadata.c"
        ).resolve()
        text = metadata_source.read_text(encoding="utf-8")
        cleanup_start = text.index("void cxl_hybrid_dst_staging_cleanup")
        init_start = text.index("int cxl_hybrid_dst_staging_init_path_at")
        cleanup = text[cleanup_start:init_start]

        self.assertNotIn("qemu_cond_destroy(&cxl_dst_staging.cond)", cleanup)
        self.assertNotIn("qemu_mutex_destroy(&cxl_dst_staging.lock)", cleanup)
        self.assertIn("cxl_dst_staging.stopping = true;", cleanup)
        self.assertIn("qemu_cond_broadcast(&cxl_dst_staging.cond)", cleanup)

    def test_real_devdax_paths_use_aligned_reserved_regions_and_fixed_staging_fd(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        control_source = (
            SCRIPT_PATH.parent.parent / "migration" / "cxl-hybrid-control.c"
        ).resolve()
        metadata_source = (
            SCRIPT_PATH.parent.parent / "migration" / "cxl-hybrid-metadata.c"
        ).resolve()

        cxl_text = cxl_source.read_text(encoding="utf-8")
        control_text = control_source.read_text(encoding="utf-8")
        metadata_text = metadata_source.read_text(encoding="utf-8")

        self.assertIn("cxl_hybrid_align_mapping_bytes(",
                      metadata_text)
        self.assertIn("cxl_hybrid_reserved_region_bytes(",
                      cxl_text)
        self.assertIn("cxl_hybrid_dst_staging_init_fixed_fd(cioc->fd,",
                      cxl_text)
        self.assertIn(
            "cxl_hybrid_fault_control_region_allocation_bytes(cioc->align)",
            control_text,
        )

    def test_publish_request_rejects_zero_page_len(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        publish_start = text.index("int cxl_hybrid_publish_page_to_cxl")
        handle_start = text.index("int cxl_hybrid_handle_publish_request")
        publish = text[publish_start:handle_start]

        self.assertNotIn("page_len = TARGET_PAGE_SIZE", publish)
        self.assertIn("!page_len || !QEMU_IS_ALIGNED(page_len, TARGET_PAGE_SIZE)",
                      publish)

    def test_publish_request_queues_primary_ready_before_fault_burst(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        handle_start = text.index("int cxl_hybrid_handle_publish_request")
        quiesce_start = text.index("int cxl_hybrid_handle_publish_quiesce")
        handle = text[handle_start:quiesce_start]

        self.assertIn("cxl_hybrid_publish_fault_request_core(ramblock, guest_offset,", handle)
        self.assertIn("&primary_ready,", handle)
        self.assertIn("cxl_hybrid_stream_ready_consumer,", handle)
        self.assertIn("cxl_hybrid_publish_fault_burst(", handle)
        self.assertIn("generation, ready_consumer, errp);", handle)
        self.assertLess(
            handle.index("cxl_hybrid_stream_ready_consumer,"),
            handle.index("cxl_hybrid_publish_fault_burst(")
        )

    def test_fault_wait_records_are_non_cxl_only(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        wait_start = text.index("int cxl_hybrid_wait_and_resolve_fault")
        publish_ready_start = text.index("void cxl_hybrid_get_publish_stats")
        wait = text[wait_start:publish_ready_start]

        self.assertIn("wait_start_ns = cxl_now_ns();", wait)
        self.assertIn("cxl_hybrid_lookup_fault_wait_record_locked(", wait)
        self.assertIn("migrate_send_rp_cxl_publish_req(", wait)
        self.assertIn("cxl_ctrl_fault_path = migrate_cxl_fault_control_plane_cxl();",
                      wait)
        self.assertIn("if (!cxl_ctrl_fault_path && cxl_state.publish_mutex_ready) {",
                      wait)
        self.assertIn("if (!cxl_ctrl_fault_path) {", wait)
        non_cxl_branch = wait[wait.index("} else {"):
                              wait.index("if (publish_req_sent) {")]
        cxl_branch = wait[wait.index("if (migrate_cxl_fault_control_plane_cxl()) {"):
                          wait.index("} else {")]

        self.assertIn("cxl_hybrid_take_fault_wait_ready_recv_ns_locked(",
                      non_cxl_branch)
        self.assertLess(wait.index("cxl_hybrid_lookup_fault_wait_record_locked("),
                        wait.index("migrate_send_rp_cxl_publish_req("))
        self.assertNotIn("cxl_hybrid_lookup_fault_wait_record_locked(",
                         cxl_branch)
        self.assertNotIn("cxl_hybrid_take_fault_wait_ready_recv_ns_locked(",
                         cxl_branch)

    def test_fault_burst_helper_uses_fixed_forward_window(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        helper_start = text.index("static void cxl_hybrid_publish_fault_burst(")
        handle_start = text.index("int cxl_hybrid_handle_publish_request")
        helper = text[helper_start:handle_start]

        self.assertIn("CXL_HYBRID_FAULT_BURST_PAGES 4", text)
        self.assertIn("for (page = 1; page < CXL_HYBRID_FAULT_BURST_PAGES; page++)", helper)
        self.assertIn("neighbor_offset = guest_offset + page_len +", helper)
        self.assertIn("(uint64_t)(page - 1) * TARGET_PAGE_SIZE", helper)
        self.assertNotIn("page = 0", helper)

    def test_pending_publish_ready_revalidates_before_send(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        send_start = text.index("int cxl_hybrid_send_pending_publish_ready")
        handle_start = text.index("int cxl_hybrid_handle_publish_ready")
        send = text[send_start:handle_start]

        self.assertIn("cxl_hybrid_republish_pending_ready(ready, errp)", send)
        self.assertLess(send.index("cxl_hybrid_republish_pending_ready"),
                        send.index("cxl_hybrid_publish_notify_encoded_len"))

    def test_primary_publish_ready_queue_marks_urgent_migration_request(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        migration_header = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.h").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        header = migration_header.read_text(encoding="utf-8")
        queue_start = text.index("static void cxl_hybrid_queue_publish_ready(")
        republish_start = text.index("static int cxl_hybrid_republish_pending_ready(")
        queue = text[queue_start:republish_start]

        self.assertIn("void migration_mark_cxl_hybrid_ready_urgent(void);", header)
        self.assertIn("bool migration_cxl_hybrid_ready_urgent(void);", header)
        self.assertIn("void migration_clear_cxl_hybrid_ready_urgent(void);", header)
        self.assertIn("if (fault_primary) {", queue)
        self.assertIn("migration_mark_cxl_hybrid_ready_urgent();", queue)

    def test_ram_iterate_breaks_early_for_cxl_hybrid_ready_urgent(self):
        ram_source = (SCRIPT_PATH.parent.parent / "migration" / "ram.c").resolve()
        text = ram_source.read_text(encoding="utf-8")
        iterate_start = text.index("static int ram_save_iterate(QEMUFile *f, void *opaque)")
        complete_start = text.index("static int ram_save_complete(QEMUFile *f, void *opaque)")
        iterate = text[iterate_start:complete_start]

        self.assertIn("migration_cxl_hybrid_ready_urgent()", iterate)
        self.assertLess(iterate.index("migration_cxl_hybrid_ready_urgent()"),
                        iterate.index("pages = ram_find_and_save_block(rs);"))

    def test_pending_publish_ready_drain_clears_urgent_flag_before_send_loop(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        send_start = text.index("int cxl_hybrid_send_pending_publish_ready")
        handle_start = text.index("int cxl_hybrid_handle_publish_ready")
        send = text[send_start:handle_start]

        self.assertIn("if (migration_cxl_hybrid_ready_urgent()) {", send)
        self.assertIn("migration_clear_cxl_hybrid_ready_urgent();", send)
        self.assertLess(send.index("migration_clear_cxl_hybrid_ready_urgent();"),
                        send.index("while ((ready = cxl_hybrid_pop_publish_ready()) != NULL) {"))

    def test_cleanup_latches_final_x_cxl_stats_before_state_reset(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        cleanup_start = text.index("static void cxl_remap_state_cleanup(void)")
        outgoing_cleanup_start = text.index("void cxl_cleanup_outgoing_migration(void)")
        cleanup = text[cleanup_start:outgoing_cleanup_start]

        self.assertIn("cxl_hybrid_latch_cleanup_snapshot();", cleanup)
        self.assertLess(
            cleanup.index("cxl_hybrid_latch_cleanup_snapshot();"),
            cleanup.index("g_free(cxl_state.last_publish_request.ramblock);")
        )
        self.assertLess(
            cleanup.index("cxl_hybrid_latch_cleanup_snapshot();"),
            cleanup.index("memset(&cxl_state, 0, sizeof(cxl_state));")
        )

    def test_query_migrate_uses_latched_cleanup_snapshot_after_source_reset(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")
        populate_start = text.index("void cxl_populate_migration_info(MigrationInfo *info)")
        lookup_start = text.index("bool cxl_hybrid_lookup_global_page(", populate_start)
        populate = text[populate_start:lookup_start]

        self.assertIn("if (!cxl_state.active && cxl_cleanup_snapshot) {", populate)
        self.assertIn("info->x_cxl = QAPI_CLONE(CXLMigrationStats, cxl_cleanup_snapshot);",
                      populate)

    def test_postcopy_completion_prepares_cxl_before_eof(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        text = migration_source.read_text(encoding="utf-8")

        helper_start = text.rindex(
            "static int migration_completion_prepare_cxl_postcopy")
        postcopy_start = text.index("static void migration_completion_postcopy(MigrationState *s)")
        completion_start = text.index("static void migration_completion(MigrationState *s)")
        iterate_start = text.index("static MigIterateState migration_iteration_run")
        helper = text[helper_start:postcopy_start]
        completion = text[completion_start:iterate_start]

        self.assertIn("migration_completion_prepare_cxl_postcopy(s, &local_err)",
                      completion)
        self.assertLess(
            completion.index("migration_completion_prepare_cxl_postcopy"),
            completion.index("migration_completion_postcopy(s)")
        )

    def test_postcopy_start_initializes_cxl_fault_control_source_workers(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = migration_source.read_text(encoding="utf-8")
        cxl_text = cxl_source.read_text(encoding="utf-8")

        start = text.index("static int postcopy_start(MigrationState *ms, Error **errp)")
        trace_start = text.index("trace_postcopy_start();", start)
        body = text[start:trace_start]

        self.assertIn("if (migrate_cxl_hybrid()) {", body)
        self.assertIn("cxl_hybrid_init_source()", body)
        self.assertIn("cxl_hybrid_enter_phase(CXL_HYBRID_PHASE_SWITCHING,", body)
        self.assertLess(body.index("cxl_hybrid_init_source()"),
                        body.index("cxl_hybrid_enter_phase(CXL_HYBRID_PHASE_SWITCHING,"))
        self.assertIn("bool cxl_hybrid_init_source(void)", cxl_text)

    def test_pre_eof_cxl_prepare_publishes_remaining_pages_without_publish_ready_drain(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        text = migration_source.read_text(encoding="utf-8")

        helper_start = text.rindex(
            "static int migration_completion_prepare_cxl_postcopy")
        postcopy_start = text.index("static void migration_completion_postcopy(MigrationState *s)")
        helper = text[helper_start:postcopy_start]

        self.assertIn("cxl_hybrid_completion_publish_remaining_pages(ms, errp)",
                      helper)
        self.assertIn("if (!migrate_cxl_shared_bitmap()) {", helper)
        self.assertIn("cxl_hybrid_send_pending_publish_ready(ms->to_dst_file, errp)",
                      helper)
        self.assertNotIn("while (ms->rp_state.rp_thread_created", helper)
        self.assertNotIn("ms->rp_state.rp_thread_exited", helper)

    def test_pre_eof_cxl_prepare_quiesces_publish_requests_without_final_visibility_drain(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        savevm_source = (SCRIPT_PATH.parent.parent / "migration" /
                         "savevm.c").resolve()
        text = migration_source.read_text(encoding="utf-8")
        savevm = savevm_source.read_text(encoding="utf-8")

        helper_start = text.rindex(
            "static int migration_completion_prepare_cxl_postcopy")
        postcopy_start = text.index("static void migration_completion_postcopy(MigrationState *s)")
        helper = text[helper_start:postcopy_start]

        self.assertIn("qemu_savevm_send_cxl_hybrid_publish_quiesce(ms->to_dst_file)",
                      helper)
        self.assertIn("migration_completion_wait_cxl_publish_quiesce_ack(ms, errp)",
                      helper)
        self.assertLess(
            helper.index("qemu_savevm_send_cxl_hybrid_publish_quiesce"),
            helper.index("migration_completion_wait_cxl_publish_quiesce_ack")
        )
        self.assertIn("if (!migrate_cxl_shared_bitmap()) {", helper)
        self.assertIn("cxl_hybrid_send_pending_publish_ready(ms->to_dst_file, errp)",
                      helper)
        self.assertIn("MIG_CMD_CXL_HYBRID_PUBLISH_QUIESCE", savevm)

    def test_pre_eof_cxl_prepare_flushes_stream_after_visibility_updates(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        text = migration_source.read_text(encoding="utf-8")

        helper_start = text.rindex(
            "static int migration_completion_prepare_cxl_postcopy")
        postcopy_start = text.index("static void migration_completion_postcopy(MigrationState *s)")
        helper = text[helper_start:postcopy_start]

        self.assertIn("qemu_fflush(ms->to_dst_file)", helper)
        self.assertIn("if (ready_sent > 0) {", helper)
        self.assertIn("cxl_hybrid_mark_completion_publish_ready_flushed()", helper)

    def test_warm_push_publishes_directly_without_warm_descriptor_transport(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        send_start = text.index("static int cxl_hybrid_send_selected_page(")
        cleanup_start = text.index("static int cxl_hybrid_completion_publish_remaining_page(")
        send = text[send_start:cleanup_start]

        self.assertIn("cxl_hybrid_publish_page_to_cxl(", send)
        self.assertIn("if (!ret && !migrate_cxl_shared_bitmap()) {", send)
        self.assertIn("cxl_hybrid_send_warm_descriptor(", send)
        self.assertIn("set_bit_atomic(page_idx, cxl_state.warm_sent_bmap);", send)
        self.assertIn("clear_bit_atomic(page_idx, cxl_state.warm_dirty_bmap);", send)

    def test_completion_publish_remaining_pages_covers_all_unsent_pages(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        callback_start = text.index(
            "static int cxl_hybrid_completion_publish_remaining_page")
        helper_start = text.index(
            "int cxl_hybrid_completion_publish_remaining_pages")
        next_start = text.index("void cxl_account_dirty_sync_ns")
        callback = text[callback_start:helper_start]
        helper = text[helper_start:next_start]

        self.assertNotIn("ram_cxl_hybrid_walk_unsent", helper)
        self.assertIn("cxl_hybrid_publish_page_to_cxl", callback)
        self.assertNotIn("cxl_hybrid_build_warm_desc_range", callback)
        self.assertNotIn("cxl_hybrid_warm_desc_batch_builder_append", callback)
        self.assertNotIn("cxl_hybrid_warm_desc_batch_builder_flush", helper)
        self.assertIn("find_next_bit(cxl_state.remaining_bmap", helper)

    def test_completion_publish_remaining_regions_covers_region_fault_granularity(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        cxl_text = cxl_source.read_text(encoding="utf-8")
        migration_text = migration_source.read_text(encoding="utf-8")

        helper_start = cxl_text.index(
            "int cxl_hybrid_completion_publish_remaining_regions")
        next_start = cxl_text.index("void cxl_account_dirty_sync_ns")
        helper = cxl_text[helper_start:next_start]

        finish_start = migration_text.index(
            "static int migration_completion_finish_cxl_postcopy")
        wait_start = migration_text.index("static inline void", finish_start)
        finish = migration_text[finish_start:wait_start]

        self.assertIn("migrate_cxl_fault_resolve_uses_region()", finish)
        self.assertIn("cxl_hybrid_completion_publish_remaining_regions(errp)",
                      finish)
        self.assertLess(
            finish.index("cxl_hybrid_completion_publish_remaining_regions"),
            finish.index("cxl_hybrid_control_complete_source_run")
        )
        self.assertIn("RAMBLOCK_FOREACH_NOT_IGNORED(block)", helper)
        self.assertIn("block_first_page", helper)
        self.assertIn("block_last_page", helper)
        self.assertIn("cxl_hybrid_ctrl_region_bit_visible(", helper)
        self.assertIn("cxl_hybrid_publish_fault_region_request_core(", helper)

    def test_completion_publish_remaining_pages_uses_publish_only_callback(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        callback_start = text.index(
            "static int cxl_hybrid_completion_publish_remaining_page")
        helper_start = text.index(
            "int cxl_hybrid_completion_publish_remaining_pages")
        next_start = text.index("void cxl_account_dirty_sync_ns")
        callback = text[callback_start:helper_start]
        helper = text[helper_start:next_start]

        self.assertNotIn("CXLHybridWarmDescBatchBuilder", callback)
        self.assertNotIn("cxl_hybrid_warm_desc_batch_builder_append", callback)
        self.assertNotIn("cxl_hybrid_warm_desc_batch_builder_flush", helper)
        self.assertIn("if (migrate_cxl_shared_bitmap()) {", callback)
        self.assertIn("cxl_hybrid_send_warm_descriptor(s->to_dst_file", callback)

    def test_completion_publish_remaining_pages_filters_on_cxl_visibility(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        callback_start = text.index(
            "static int cxl_hybrid_completion_publish_remaining_page")
        callback_end = text.index("static size_t cxl_hybrid_pick_recent_miss_page")
        callback = text[callback_start:callback_end]

        self.assertIn("test_bit(page_idx, cxl_state.remaining_bmap)", callback)
        self.assertIn("test_bit(page_idx, cxl_state.cxl_visible_bmap)", callback)
        self.assertNotIn("cxl_state.dst_sent_bmap", callback)

    def test_completion_visible_state_uses_page_index_storage(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        self.assertIn("unsigned long *cxl_visible_bmap;", text)
        self.assertIn("unsigned long *remaining_bmap;", text)
        self.assertIn("CXLHybridPublishedPageEntry *published_page_state;", text)
        self.assertIn("cxl_state.cxl_visible_bmap = bitmap_new(cxl_state.total_pages);",
                      text)
        self.assertIn("cxl_state.remaining_bmap = bitmap_new(cxl_state.total_pages);",
                      text)
        self.assertIn("cxl_state.published_page_state = g_new0(CXLHybridPublishedPageEntry,",
                      text)
        self.assertNotIn("GHashTable *published_pages;", text)

    def test_completion_remaining_pages_iterates_remaining_bitmap(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        callback_start = text.index(
            "static int cxl_hybrid_completion_publish_remaining_page")
        helper_start = text.index(
            "int cxl_hybrid_completion_publish_remaining_pages")
        callback = text[callback_start:helper_start]
        next_start = text.index("void cxl_account_dirty_sync_ns")
        helper = text[helper_start:next_start]

        self.assertNotIn("ram_cxl_hybrid_walk_unsent", helper)
        self.assertIn("find_next_bit(cxl_state.remaining_bmap", helper)
        self.assertIn("cxl_hybrid_lookup_global_page(page_idx, &block, &block_offset)",
                      callback)
        self.assertIn("cxl_hybrid_completion_publish_remaining_page(s, page_idx",
                      helper)

    def test_iteration_run_skips_publish_ready_drain_in_shared_bitmap_mode(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        text = migration_source.read_text(encoding="utf-8")

        start = text.index("static MigIterateState migration_iteration_run(MigrationState *s)")
        end = text.index("static void migration_iteration_finish(MigrationState *s)")
        body = text[start:end]

        self.assertIn("!migrate_cxl_shared_bitmap() &&", body)
        self.assertIn("cxl_hybrid_send_pending_publish_ready(s->to_dst_file", body)
        self.assertIn("cxl_hybrid_warm_push_iteration(s, &local_err)", body)

    def test_publish_and_invalidate_maintain_remaining_bitmap(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        dirty_start = text.index("void cxl_hybrid_account_warm_dirty")
        dst_sent_start = text.index("void cxl_hybrid_account_dst_page_sent")
        visible_start = text.index("static void cxl_hybrid_mark_page_cxl_visible")
        remaining_start = text.index("static void cxl_hybrid_mark_page_remaining")
        publish_start = text.index("int cxl_hybrid_publish_page_to_cxl")
        handle_start = text.index("static void cxl_hybrid_publish_fault_burst")
        dirty = text[dirty_start:dst_sent_start]
        visible = text[visible_start:remaining_start]
        publish = text[publish_start:handle_start]

        self.assertIn("cxl_hybrid_mark_page_cxl_visible(page_idx);",
                      publish)
        self.assertIn("set_bit_atomic(page_idx, cxl_state.cxl_visible_bmap);",
                      visible)
        self.assertIn("clear_bit_atomic(page_idx, cxl_state.remaining_bmap);",
                      visible)
        self.assertIn("clear_bit_atomic(page_idx, cxl_state.cxl_visible_bmap);",
                      dirty)
        self.assertIn("set_bit_atomic(page_idx, cxl_state.remaining_bmap);",
                      dirty)

    def test_remap_marks_pages_visible_for_completion(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        mark_start = text.index("static void cxl_mark_pages_remapped")
        sender_begin = text.index("bool cxl_sender_access_begin(void)")
        helper = text[mark_start:sender_begin]

        self.assertIn("set_bit_atomic(page_idx, cxl_state.remapped_pages_bmap);",
                      helper)
        self.assertIn("set_bit_atomic(page_idx, cxl_state.cxl_visible_bmap);",
                      helper)
        self.assertIn("clear_bit_atomic(page_idx, cxl_state.remaining_bmap);",
                      helper)

    def test_warm_desc_batch_protocol_is_declared(self):
        cxl_header = (SCRIPT_PATH.parent.parent / "migration" / "cxl.h").resolve()
        savevm_source = (SCRIPT_PATH.parent.parent / "migration" / "savevm.c").resolve()
        savevm_header = (SCRIPT_PATH.parent.parent / "migration" / "savevm.h").resolve()
        cxl_text = cxl_header.read_text(encoding="utf-8")
        savevm_text = savevm_source.read_text(encoding="utf-8")
        savevm_header_text = savevm_header.read_text(encoding="utf-8")

        self.assertIn("typedef struct CXLHybridWarmDescRange {", cxl_text)
        self.assertIn("typedef struct CXLHybridWarmDescBatch {", cxl_text)
        self.assertIn("int cxl_hybrid_warm_desc_batch_encode(", cxl_text)
        self.assertIn("int cxl_hybrid_warm_desc_batch_decode(", cxl_text)
        self.assertIn("MIG_CMD_CXL_HYBRID_WARM_DESC_BATCH", savevm_text)
        self.assertIn("qemu_savevm_send_cxl_hybrid_warm_desc_batch", savevm_header_text)

    def test_warm_descriptor_send_marks_page_visible_on_destination(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        text = cxl_source.read_text(encoding="utf-8")

        send_start = text.index("int cxl_hybrid_send_warm_descriptor")
        next_start = text.index("int cxl_hybrid_send_warm_page")
        send = text[send_start:next_start]

        self.assertIn("cxl_hybrid_account_dst_page_sent(ramblock, guest_offset, TARGET_PAGE_SIZE)",
                      send)
        self.assertLess(
            send.index("qemu_savevm_send_cxl_hybrid_warm_desc"),
            send.index("cxl_hybrid_account_dst_page_sent")
        )

    def test_publish_request_send_observes_destination_quiesce_gate(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        migration_header = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.h").resolve()
        text = migration_source.read_text(encoding="utf-8")
        header = migration_header.read_text(encoding="utf-8")
        send_start = text.index("int migrate_send_rp_cxl_publish_req")
        next_start = text.index("int migrate_send_rp_req_pages")
        send = text[send_start:next_start]

        self.assertIn("bool cxl_publish_request_quiesce;", header)
        self.assertIn("bool *sentp", send)
        self.assertIn("mis->cxl_publish_request_quiesce", send)
        self.assertIn("QEMU_LOCK_GUARD(&mis->rp_mutex)", send)
        self.assertLess(
            send.index("mis->cxl_publish_request_quiesce"),
            send.index("migrate_send_rp_message_locked")
        )

    def test_publish_request_protocol_carries_send_timestamp(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        cxl_header = (SCRIPT_PATH.parent.parent / "migration" /
                      "cxl.h").resolve()
        text = migration_source.read_text(encoding="utf-8")
        cxl_header_text = cxl_header.read_text(encoding="utf-8")

        send_start = text.index("int migrate_send_rp_cxl_publish_req")
        next_start = text.index("int migrate_send_rp_req_pages")
        send = text[send_start:next_start]
        handler_start = text.index("static void *source_return_path_thread(void *opaque)")
        source_open_start = text.index("static void open_return_path_on_source(MigrationState *ms)")
        handler = text[handler_start:source_open_start]

        self.assertIn("size_t msglen = 24;", send)
        self.assertIn("uint64_t sent_at_ns;", send)
        self.assertIn("sent_at_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);", send)
        self.assertIn("stq_be_p(bufc + 16, sent_at_ns);", send)
        self.assertIn("void cxl_hybrid_record_publish_req_recv_time(uint64_t elapsed_ns);", cxl_header_text)
        self.assertIn("expected_len = 24 + 1;", handler)
        self.assertIn("publish_req_sent_at_ns = ldq_be_p(buf + 16);", handler)
        self.assertIn("cxl_hybrid_record_publish_req_recv_time(", handler)

    def test_publish_ready_command_carries_send_timestamp_and_primary_flag(self):
        savevm_source = (SCRIPT_PATH.parent.parent / "migration" /
                         "savevm.c").resolve()
        savevm_header = (SCRIPT_PATH.parent.parent / "migration" /
                         "savevm.h").resolve()
        cxl_header = (SCRIPT_PATH.parent.parent / "migration" /
                      "cxl.h").resolve()
        savevm_text = savevm_source.read_text(encoding="utf-8")
        savevm_header_text = savevm_header.read_text(encoding="utf-8")
        cxl_header_text = cxl_header.read_text(encoding="utf-8")

        send_start = savevm_text.index("void qemu_savevm_send_cxl_hybrid_publish_ready")
        quiesce_start = savevm_text.index("void qemu_savevm_send_cxl_hybrid_publish_quiesce")
        send = savevm_text[send_start:quiesce_start]
        switch_start = savevm_text.index("static int loadvm_process_command(QEMUFile *f, Error **errp)")
        cleanup_start = savevm_text.index("void qemu_loadvm_state_cleanup(MigrationIncomingState *mis)")
        switch = savevm_text[switch_start:cleanup_start]

        self.assertIn("bool fault_primary", savevm_header_text)
        self.assertIn("uint64_t sent_at_ns;", send)
        self.assertIn("size_t cmd_hdr_len = 8 + 1;", send)
        self.assertIn("sent_at_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);", send)
        self.assertIn("stq_be_p(cmd_buf, sent_at_ns);", send)
        self.assertIn("cmd_buf[8] = fault_primary ? CXL_HYBRID_PUBLISH_READY_FLAG_PRIMARY : 0;", send)
        self.assertIn("void cxl_hybrid_record_publish_ready_recv_time(uint64_t elapsed_ns);", cxl_header_text)
        self.assertIn("if (len < 9) {", switch)
        self.assertIn("publish_ready_sent_at_ns = ldq_be_p(buf);", switch)
        self.assertIn("fault_primary = buf[8] & CXL_HYBRID_PUBLISH_READY_FLAG_PRIMARY;", switch)
        self.assertIn("cxl_hybrid_record_publish_ready_recv_time(", switch)
        self.assertIn("cxl_hybrid_handle_publish_ready(&notify, fault_primary,", switch)

    def test_source_return_path_handles_publish_quiesce_ack(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        text = migration_source.read_text(encoding="utf-8")
        handler_start = text.index("static void *source_return_path_thread(void *opaque)")
        source_open_start = text.index("static void open_return_path_on_source(MigrationState *ms)")
        handler = text[handler_start:source_open_start]

        self.assertIn("MIG_RP_MSG_CXL_HYBRID_PUBLISH_QUIESCE_ACK", handler)
        self.assertIn("ms->cxl_publish_quiesce_acked = true;", handler)
        self.assertIn("migration_rp_kick(ms);", handler)

    def test_loadvm_handles_warm_desc_batch_command(self):
        savevm_source = (SCRIPT_PATH.parent.parent / "migration" /
                         "savevm.c").resolve()
        text = savevm_source.read_text(encoding="utf-8")

        switch_start = text.index("static int loadvm_process_command(QEMUFile *f, Error **errp)")
        cleanup_start = text.index("void qemu_loadvm_state_cleanup(MigrationIncomingState *mis)")
        switch = text[switch_start:cleanup_start]

        self.assertIn("case MIG_CMD_CXL_HYBRID_WARM_DESC_BATCH:", switch)
        self.assertIn("cxl_hybrid_warm_desc_batch_decode", switch)
        self.assertIn("cxl_hybrid_warm_desc_batch_store", switch)

    def test_destination_has_warm_desc_batch_apply_helper(self):
        metadata_source = (SCRIPT_PATH.parent.parent / "migration" /
                           "cxl-hybrid-metadata.c").resolve()
        text = metadata_source.read_text(encoding="utf-8")

        self.assertIn("int cxl_hybrid_warm_desc_batch_store", text)
        self.assertIn("CXLHybridWarmDescBatch *batch", text)
        self.assertIn("qemu_cond_broadcast(&cxl_dst_staging.cond)", text)

    def test_locked_external_page_registration_uses_locked_active_helper(self):
        metadata_source = (SCRIPT_PATH.parent.parent / "migration" /
                           "cxl-hybrid-metadata.c").resolve()
        text = metadata_source.read_text(encoding="utf-8")

        helper_start = text.index(
            "static int cxl_hybrid_dst_staging_register_external_page_locked(")
        wrapper_start = text.index(
            "int cxl_hybrid_dst_staging_register_external_page(")
        helper = text[helper_start:wrapper_start]

        self.assertIn("cxl_hybrid_dst_staging_is_active_locked()", helper)
        self.assertNotIn("cxl_hybrid_dst_staging_is_active()", helper)

    def test_destination_batch_apply_splits_ranges_across_slot_boundaries(self):
        metadata_source = (SCRIPT_PATH.parent.parent / "migration" /
                           "cxl-hybrid-metadata.c").resolve()
        text = metadata_source.read_text(encoding="utf-8")

        range_start = text.index(
            "static int cxl_hybrid_dst_staging_register_external_range_locked(")
        batch_start = text.index("int cxl_hybrid_warm_desc_batch_store(")
        range_helper = text[range_start:batch_start]
        batch_store = text[batch_start:]

        self.assertIn("cxl_hybrid_dst_staging_lookup", range_helper)
        self.assertIn("remaining", range_helper)
        self.assertIn("chunk_len", range_helper)
        self.assertIn("cxl_hybrid_dst_staging_register_external_range_locked", batch_store)
        self.assertNotIn("cxl_hybrid_dst_staging_register_external_page_locked(", batch_store)

    def test_trace_events_include_warm_desc_batch_traces(self):
        trace_events = (SCRIPT_PATH.parent.parent / "migration" /
                        "trace-events").resolve()
        text = trace_events.read_text(encoding="utf-8")

        self.assertIn("savevm_send_cxl_hybrid_warm_desc_batch", text)
        self.assertIn("cxl_hybrid_warm_desc_batch_send", text)
        self.assertIn("cxl_hybrid_warm_desc_batch_recv", text)

    def test_pre_eof_cxl_prepare_uses_publish_only_completion(self):
        migration_source = (SCRIPT_PATH.parent.parent / "migration" /
                            "migration.c").resolve()
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" /
                      "cxl.c").resolve()
        migration_text = migration_source.read_text(encoding="utf-8")
        cxl_text = cxl_source.read_text(encoding="utf-8")

        helper_start = migration_text.rindex(
            "static int migration_completion_prepare_cxl_postcopy")
        postcopy_start = migration_text.index("static void migration_completion_postcopy(MigrationState *s)")
        helper = migration_text[helper_start:postcopy_start]
        completion_start = cxl_text.index(
            "static int cxl_hybrid_completion_publish_remaining_page")
        next_start = cxl_text.index("void cxl_account_dirty_sync_ns")
        completion = cxl_text[completion_start:next_start]

        self.assertIn("cxl_hybrid_completion_publish_remaining_pages(ms, errp)", helper)
        self.assertNotIn("cxl_hybrid_warm_desc_batch_builder_flush", completion)
        self.assertIn("if (migrate_cxl_shared_bitmap()) {", completion)
        self.assertIn("cxl_hybrid_send_warm_descriptor(s->to_dst_file", completion)

    def test_loadvm_rejects_legacy_visibility_transport_commands_in_shared_bitmap_mode(self):
        savevm_source = (SCRIPT_PATH.parent.parent / "migration" /
                         "savevm.c").resolve()
        text = savevm_source.read_text(encoding="utf-8")

        switch_start = text.index("static int loadvm_process_command(QEMUFile *f, Error **errp)")
        cleanup_start = text.index("void qemu_loadvm_state_cleanup(MigrationIncomingState *mis)")
        switch = text[switch_start:cleanup_start]

        self.assertIn("case MIG_CMD_CXL_HYBRID_WARM_DESC:", switch)
        self.assertIn("case MIG_CMD_CXL_HYBRID_WARM_DESC_BATCH:", switch)
        self.assertIn("case MIG_CMD_CXL_HYBRID_PUBLISH_READY:", switch)
        self.assertIn("migrate_cxl_shared_bitmap()", switch)
        self.assertIn("shared-bitmap mode rejects legacy", switch)

    def test_qapi_includes_last_publish_event_diagnostics(self):
        qapi_source = (SCRIPT_PATH.parent.parent / "qapi" /
                       "migration.json").resolve()
        text = qapi_source.read_text(encoding="utf-8")

        self.assertIn("{ 'struct': 'CXLMigrationPublishRequestInfo'", text)
        self.assertIn("{ 'struct': 'CXLMigrationPublishReadyInfo'", text)
        self.assertIn("{ 'struct': 'CXLMigrationPublishWaitInfo'", text)
        self.assertIn("'*last-publish-request': {", text)
        self.assertIn("'type': 'CXLMigrationPublishRequestInfo'", text)
        self.assertIn("'*last-publish-ready': {", text)
        self.assertIn("'*last-completion-publish-ready': {", text)
        self.assertIn("'type': 'CXLMigrationPublishReadyInfo'", text)
        self.assertIn("'*last-publish-ready-recv': {", text)
        self.assertIn("'*last-publish-wait-begin': {", text)
        self.assertIn("'*last-publish-wait-complete': {", text)
        self.assertIn("'type': 'CXLMigrationPublishWaitInfo'", text)

    def test_qapi_includes_fault_wait_breakdown_fields(self):
        qapi_source = (SCRIPT_PATH.parent.parent / "qapi" /
                       "migration.json").resolve()
        text = qapi_source.read_text(encoding="utf-8")

        self.assertIn("'fault-publish-primary-samples': {", text)
        self.assertIn("'fault-publish-primary-time-ns': {", text)
        self.assertIn("'max-fault-publish-primary-time-ns': {", text)
        self.assertIn("'fault-publish-burst-samples': {", text)
        self.assertIn("'fault-publish-burst-time-ns': {", text)
        self.assertIn("'max-fault-publish-burst-time-ns': {", text)
        self.assertIn("'fault-primary-ready-send-samples': {", text)
        self.assertIn("'fault-primary-ready-send-time-ns': {", text)
        self.assertIn("'max-fault-primary-ready-send-time-ns': {", text)
        self.assertIn("'fault-publish-req-recv-samples': {", text)
        self.assertIn("'fault-publish-req-recv-time-ns': {", text)
        self.assertIn("'max-fault-publish-req-recv-time-ns': {", text)
        self.assertIn("'fault-publish-req-handle-samples': {", text)
        self.assertIn("'fault-publish-req-handle-time-ns': {", text)
        self.assertIn("'max-fault-publish-req-handle-time-ns': {", text)
        self.assertIn("'fault-primary-ready-drain-samples': {", text)
        self.assertIn("'fault-primary-ready-drain-time-ns': {", text)
        self.assertIn("'max-fault-primary-ready-drain-time-ns': {", text)
        self.assertIn("'fault-primary-ready-write-samples': {", text)
        self.assertIn("'fault-primary-ready-write-time-ns': {", text)
        self.assertIn("'max-fault-primary-ready-write-time-ns': {", text)
        self.assertIn("'fault-primary-ready-recv-samples': {", text)
        self.assertIn("'fault-primary-ready-recv-time-ns': {", text)
        self.assertIn("'max-fault-primary-ready-recv-time-ns': {", text)
        self.assertIn("'fault-primary-ready-handle-samples': {", text)
        self.assertIn("'fault-primary-ready-handle-time-ns': {", text)
        self.assertIn("'max-fault-primary-ready-handle-time-ns': {", text)
        self.assertIn("'fault-wait-ready-recv-samples': {", text)
        self.assertIn("'fault-wait-ready-recv-time-ns': {", text)
        self.assertIn("'max-fault-wait-ready-recv-time-ns': {", text)
        self.assertIn("'fault-wait-after-ready-recv-samples': {", text)
        self.assertIn("'fault-wait-after-ready-recv-time-ns': {", text)
        self.assertIn("'max-fault-wait-after-ready-recv-time-ns': {", text)

    def test_set_params_precopy_omits_hybrid_prefetch_settings(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_params(object(), "/tmp/cxl.img",
                            "pure_precopy", "light")

        _cmd, args = calls[0]
        self.assertEqual(args["cxl-path"], "/tmp/cxl.img")
        self.assertEqual(args["multifd-channels"], 2)
        self.assertNotIn("x-cxl-prefetch-batch-pages", args)
        self.assertNotIn("x-cxl-prefetch-rate", args)
        self.assertNotIn("x-cxl-switch-dirty-threshold", args)

    def test_set_params_native_postcopy_stream_omits_cxl_parameters(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_params(object(), "/tmp/cxl.img",
                            "native_postcopy_stream", "light")

        _cmd, args = calls[0]
        self.assertEqual(args["multifd-channels"], 2)
        self.assertEqual(args["max-bandwidth"], 8 * 1024 * 1024)
        self.assertNotIn("cxl-path", args)
        self.assertNotIn("x-cxl-brake-remap-granule", args)
        self.assertNotIn("x-cxl-prefetch-batch-pages", args)
        self.assertNotIn("x-cxl-prefetch-rate", args)
        self.assertNotIn("x-cxl-switch-dirty-threshold", args)

    def test_set_params_native_rdma_precopy_omits_cxl_and_multifd_parameters(self):
        calls = []

        def fake_qmp_ok(_f, cmd, args=None):
            calls.append((cmd, args))
            return {}

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.set_params(object(), "/tmp/cxl.img",
                            "native_rdma_precopy", "light",
                            max_bandwidth=0)

        _cmd, args = calls[0]
        self.assertEqual(args["max-bandwidth"], 0)
        self.assertNotIn("multifd-channels", args)
        self.assertNotIn("cxl-path", args)
        self.assertNotIn("x-cxl-brake-remap-granule", args)
        self.assertNotIn("x-cxl-prefetch-batch-pages", args)
        self.assertNotIn("x-cxl-prefetch-rate", args)
        self.assertNotIn("x-cxl-switch-dirty-threshold", args)

    def test_build_migration_uri_uses_rdma_endpoint_for_rdma_mode(self):
        uri = self.mod.build_migration_uri(
            "native_rdma_precopy", Path("/tmp/mig.sock"),
            rdma_host="10.0.0.2", rdma_port=7701)

        self.assertEqual(uri, "rdma:10.0.0.2:7701")

    def test_build_migration_uri_uses_unix_socket_for_non_rdma_mode(self):
        uri = self.mod.build_migration_uri(
            "native_postcopy_stream", Path("/tmp/mig.sock"),
            rdma_host="10.0.0.2", rdma_port=7701)

        self.assertEqual(uri, "unix:/tmp/mig.sock")

    def test_build_qemu_env_sets_mode_flags(self):
        pure_env = self.mod.build_qemu_env("pure_precopy", is_source=True)
        redirect_env = self.mod.build_qemu_env("redirect_precopy", is_source=True)
        hybrid_env = self.mod.build_qemu_env("hybrid_postcopy_auto", is_source=True)
        native_env = self.mod.build_qemu_env("native_postcopy_stream",
                                             is_source=True)
        rdma_env = self.mod.build_qemu_env("native_rdma_precopy",
                                           is_source=True)

        self.assertEqual(pure_env["QEMU_CXL_HYBRID_WARM_DISABLE"], "1")
        self.assertEqual(pure_env["QEMU_CXL_WRITE_REDIRECT"], "0")
        self.assertEqual(redirect_env["QEMU_CXL_HYBRID_WARM_DISABLE"], "1")
        self.assertEqual(redirect_env["QEMU_CXL_WRITE_REDIRECT"], "1")
        self.assertNotIn("QEMU_CXL_HYBRID_WARM_DISABLE", hybrid_env)
        self.assertEqual(hybrid_env["QEMU_CXL_WRITE_REDIRECT"], "1")
        self.assertEqual(native_env["QEMU_CXL_HYBRID_WARM_DISABLE"], "1")
        self.assertEqual(native_env["QEMU_CXL_WRITE_REDIRECT"], "0")
        self.assertEqual(rdma_env["QEMU_CXL_HYBRID_WARM_DISABLE"], "1")
        self.assertEqual(rdma_env["QEMU_CXL_WRITE_REDIRECT"], "0")

    def test_start_qemu_perf_attaches_to_started_qemu_pids(self):
        popen_calls = []

        class FakeQemuProc:
            def __init__(self, pid):
                self.pid = pid

        class FakePerfProc:
            def __init__(self):
                self.pid = 9001
                self.returncode = None
                self.signals = []
                self.waits = []

            def poll(self):
                return self.returncode

            def send_signal(self, sig):
                self.signals.append(sig)
                self.returncode = 0

            def wait(self, timeout=None):
                self.waits.append(timeout)
                return self.returncode

        def fake_popen(argv, **kwargs):
            popen_calls.append((argv, kwargs))
            return FakePerfProc()

        self.mod.subprocess.Popen = fake_popen

        with tempfile.TemporaryDirectory() as tmpdir:
            proc, info = self.mod.start_qemu_perf(
                Path(tmpdir), FakeQemuProc(111), FakeQemuProc(222),
                enabled=True, frequency=1234)
            self.mod.stop_qemu_perf(proc, info)

        argv, kwargs = popen_calls[0]
        self.assertEqual(argv[:8], [
            "perf", "record", "--clockid", "mono",
            "-T", "-F", "1234", "-g",
        ])
        self.assertIn("-p", argv)
        self.assertEqual(argv[argv.index("-p") + 1], "111,222")
        self.assertIn("--", argv)
        self.assertEqual(argv[argv.index("--") + 1:],
                         ["sleep", str(self.mod.PERF_RECORD_SECS)])
        self.assertTrue(info["qemu_perf_path"].endswith("qemu-perf.data"))
        self.assertEqual(info["qemu_perf_returncode"], 0)
        self.assertEqual(proc.signals, [self.mod.signal.SIGINT])
        self.assertIs(kwargs["stdout"], self.mod.subprocess.DEVNULL)

    def test_build_common_args_disable_vapic_and_start_paused(self):
        args = self.mod.build_common_args(Path("/tmp/warm.img"))

        self.assertEqual(args[:3], [
            str(self.mod.QEMU),
            "-machine",
            "pc,accel=tcg",
        ])
        self.assertIn("-S", args)
        self.assertIn("-global", args)
        vapic_idx = args.index("-global")
        self.assertEqual(args[vapic_idx + 1], "apic-common.vapic=false")
        self.assertIn("file=/tmp/warm.img,format=raw,if=floppy", args)

    def test_build_boot_image_passes_address_defines_to_nasm(self):
        calls = []

        def fake_run(argv, check):
            calls.append(argv)
            out = Path(argv[argv.index("-o") + 1])
            out.write_bytes(b"\0" * 512)
            return None

        self.mod.subprocess.run = fake_run

        with tempfile.TemporaryDirectory() as tmpdir:
            img = self.mod.build_boot_image(Path(tmpdir), "light")

        self.assertEqual(img.name, "warm-boot-light.img")
        self.assertEqual(calls[0][0], "nasm")
        self.assertIn("PRESSURE_START_ADDR=0x00020000", calls[0])
        self.assertIn("PRESSURE_END_ADDR=0x00040000", calls[0])
        self.assertIn("PRESSURE_WRITES_PER_PAGE=1", calls[0])
        self.assertIn("PRESSURE_OUTER_SPIN=8192", calls[0])

    def test_build_boot_image_passes_random_rw_defines_to_nasm(self):
        calls = []

        def fake_run(argv, check):
            calls.append(argv)
            out = Path(argv[argv.index("-o") + 1])
            out.write_bytes(b"\0" * 512)
            return None

        self.mod.subprocess.run = fake_run

        with tempfile.TemporaryDirectory() as tmpdir:
            img = self.mod.build_boot_image(Path(tmpdir), "remap_heavy_random_rw")

        self.assertEqual(img.name, "warm-boot-remap_heavy_random_rw.img")
        self.assertEqual(calls[0][0], "nasm")
        self.assertIn("PRESSURE_START_ADDR=0x00100000", calls[0])
        self.assertIn("PRESSURE_END_ADDR=0x00900000", calls[0])
        self.assertIn("PRESSURE_PAGE_ORDER_RANDOM=1", calls[0])
        self.assertIn("PRESSURE_ACCESS_PATTERN_RANDOM_RW=1", calls[0])
        self.assertIn("PRESSURE_RANDOM_PAGE_STRIDE=73", calls[0])
        self.assertIn("PRESSURE_RANDOM_EPOCH_SEED_STEP=1", calls[0])

    def test_build_boot_image_passes_in_memory_latency_defines_to_nasm(self):
        calls = []

        def fake_run(argv, check):
            calls.append(argv)
            out = Path(argv[argv.index("-o") + 1])
            out.write_bytes(b"\0" * 512)
            return None

        self.mod.subprocess.run = fake_run

        with tempfile.TemporaryDirectory() as tmpdir:
            self.mod.build_boot_image(
                Path(tmpdir), "remap_xlarge_random_rw",
                in_memory_guest_latency=True,
            )

        self.assertIn("PRESSURE_IN_MEMORY_LATENCY=1", calls[0])
        self.assertIn(
            f"PRESSURE_IN_MEMORY_LOG_ADDR=0x{self.mod.IN_MEMORY_LATENCY_ADDR:08x}",
            calls[0],
        )
        self.assertIn(
            f"PRESSURE_IN_MEMORY_LOG_RECORDS={self.mod.IN_MEMORY_LATENCY_RECORDS}",
            calls[0],
        )
        self.assertLessEqual(
            self.mod.IN_MEMORY_LATENCY_ADDR +
            self.mod.IN_MEMORY_LATENCY_DUMP_BYTES,
            self.mod.GUEST_RAM_BYTES,
        )
        self.assertIn(
            f"PRESSURE_IN_MEMORY_MARKER_ADDR=0x{self.mod.IN_MEMORY_MARKER_ADDR:08x}",
            calls[0],
        )
        self.assertLessEqual(
            self.mod.IN_MEMORY_MARKER_ADDR +
            self.mod.IN_MEMORY_MARKER_BYTES,
            self.mod.GUEST_RAM_BYTES,
        )

    def test_in_memory_marker_ring_does_not_overlap_latency_ring(self):
        latency_start = self.mod.IN_MEMORY_LATENCY_ADDR
        latency_end = (
            latency_start + self.mod.IN_MEMORY_LATENCY_HEADER_BYTES +
            self.mod.IN_MEMORY_LATENCY_RECORDS * 4
        )
        marker_start = self.mod.IN_MEMORY_MARKER_ADDR
        marker_end = (
            marker_start + self.mod.IN_MEMORY_MARKER_HEADER_BYTES +
            self.mod.IN_MEMORY_LATENCY_RECORDS * 4
        )

        self.assertLessEqual(latency_end, self.mod.GUEST_RAM_BYTES)
        self.assertLessEqual(marker_end, self.mod.GUEST_RAM_BYTES)
        self.assertTrue(
            latency_end <= marker_start or marker_end <= latency_start,
            (latency_start, latency_end, marker_start, marker_end),
        )

    def test_parse_in_memory_latency_payload_reports_excess_cycles(self):
        records = [100, 105, 95, 500, 100, 110]
        payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_LATENCY_MAGIC,
            1,
            64,
            16,
            len(records),
            0,
            0,
            0,
        ) + struct.pack(f"<{len(records)}I", *records)

        report = self.mod.parse_in_memory_latency_payload(
            payload,
            "remap_xlarge_random_rw",
            tsc_khz=1000,
        )

        self.assertTrue(report["valid"])
        self.assertEqual(report["sample_interval_pages"], 64)
        self.assertEqual(report["samples_total"], len(records))
        self.assertEqual(report["all"]["count"], len(records))
        self.assertEqual(report["all"]["median_cycles"], 102.5)
        self.assertEqual(report["all"]["max_cycles"], 500)
        self.assertAlmostEqual(report["all"]["max_excess_us"], 397.5)

    def test_parse_in_memory_latency_payload_reorders_ring_records(self):
        capacity = 8
        total = 12
        stored = [8, 9, 10, 11, 4, 5, 6, 7]
        payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_LATENCY_MAGIC,
            1,
            64,
            capacity,
            total,
            total - capacity,
            0,
            0,
        ) + struct.pack(f"<{capacity}I", *stored)

        report = self.mod.parse_in_memory_latency_payload(
            payload,
            "remap_xlarge_random_rw",
            tsc_khz=1000,
        )

        self.assertEqual(report["samples_read"], capacity)
        self.assertEqual(report["sample_index_start"], 4)
        self.assertEqual(report["sample_index_end"], 12)
        self.assertEqual(report["all"]["min_cycles"], 4)
        self.assertEqual(report["all"]["max_cycles"], 11)

    def test_parse_in_memory_latency_payload_uses_marker_window(self):
        records = [100, 110, 400, 100, 120, 100]
        markers = [0, 1, 2, 2, 3, 4]
        payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_LATENCY_MAGIC,
            self.mod.IN_MEMORY_LATENCY_VERSION,
            64,
            16,
            len(records),
            0,
            0,
            0,
        ) + struct.pack(f"<{len(records)}I", *records)
        marker_payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_MARKER_MAGIC,
            self.mod.IN_MEMORY_MARKER_VERSION,
            len(markers),
            16,
            0,
            0,
            0,
            0,
        ) + struct.pack(f"<{len(markers)}I", *markers)

        report = self.mod.parse_in_memory_latency_payload(
            payload,
            "remap_xlarge_random_rw",
            tsc_khz=1000,
            marker_payload=marker_payload,
            marker_start_event=2,
            marker_complete_event=3,
        )

        self.assertTrue(report["marker_valid"])
        self.assertEqual(report["marker_start_event"], 2)
        self.assertEqual(report["marker_complete_event"], 3)
        self.assertEqual(report["corrected_window_sample_start"], 2)
        self.assertEqual(report["corrected_window_sample_end"], 5)
        self.assertEqual(report["corrected_window"]["count"], 3)
        self.assertEqual(report["corrected_window"]["max_cycles"], 400)

    def test_parse_in_memory_latency_payload_reports_stall_windows(self):
        records = [100, 110, 120, 500, 130, 140, 900, 150, 160, 1000, 170, 180]
        markers = [1, 1, 7, 7, 8, 8, 6, 6, 10, 10, 5, 5]
        payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_LATENCY_MAGIC,
            self.mod.IN_MEMORY_LATENCY_VERSION,
            64,
            16,
            len(records),
            0,
            0,
            0,
        ) + struct.pack(f"<{len(records)}I", *records)
        marker_payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_MARKER_MAGIC,
            self.mod.IN_MEMORY_MARKER_VERSION,
            len(markers),
            16,
            0,
            0,
            0,
            0,
        ) + struct.pack(f"<{len(markers)}I", *markers)

        report = self.mod.parse_in_memory_latency_payload(
            payload,
            "remap_xlarge_random_rw",
            tsc_khz=1000,
            marker_payload=marker_payload,
            marker_start_event=1,
            marker_complete_event=6,
            window_specs=[
                ("precopy_bulk_window", 1, 7),
                ("precopy_brake_window", 7, 8),
                ("postcopy_handoff_window", 8, 6),
                ("postcopy_window", 6, 10),
                ("completion_tail_window", 10, 5),
            ],
        )

        self.assertTrue(report["marker_valid"])
        self.assertEqual(report["precopy_bulk_window_sample_start"], 0)
        self.assertEqual(report["precopy_brake_window_sample_start"], 2)
        self.assertEqual(report["postcopy_handoff_window_sample_start"], 4)
        self.assertEqual(report["postcopy_window_sample_start"], 6)
        self.assertEqual(report["completion_tail_window_sample_start"], 8)
        self.assertEqual(report["precopy_bulk_window"]["count"], 3)
        self.assertEqual(report["precopy_brake_window"]["count"], 3)
        self.assertEqual(report["postcopy_handoff_window"]["count"], 3)
        self.assertEqual(report["postcopy_window"]["count"], 3)
        self.assertEqual(report["completion_tail_window"]["count"], 3)
        self.assertEqual(report["postcopy_window"]["max_cycles"], 900)

    def test_summarize_single_result_exports_in_memory_stall_windows(self):
        result = {
            "guest_in_memory_latency": {
                "valid": True,
                "samples_read": 10,
                "precopy_bulk_window": {
                    "total_excess_ms": 1.25,
                    "max_excess_us": 500.0,
                },
                "precopy_brake_window": {
                    "total_excess_ms": 2.5,
                    "max_excess_us": 750.0,
                },
                "postcopy_handoff_window": {
                    "total_excess_ms": 3.75,
                    "max_excess_us": 900.0,
                },
                "postcopy_window": {
                    "total_excess_ms": 4.0,
                    "max_excess_us": 1000.0,
                },
                "completion_tail_window": {
                    "total_excess_ms": 0.5,
                    "max_excess_us": 100.0,
                },
            },
            "handoff_breakdown": {
                "control_src_start_to_enter_brake_ms": 10.0,
                "control_src_enter_brake_to_request_postcopy_ms": 20.0,
                "control_src_request_postcopy_to_postcopy_start_ms": 30.0,
                "control_src_completion_enter_to_completed_ms": 40.0,
            },
            "summary": {},
            "trace": {"combined": self.mod.trace_count_template()},
            "latency": {},
        }

        row = self.mod.summarize_single_result(
            "remap_xlarge_random_rw",
            "hybrid_postcopy_auto",
            self.mod.resolve_threshold_profile("balanced"),
            1,
            result,
        )

        self.assertEqual(row["guest_in_memory_precopy_bulk_total_excess_ms"],
                         1.25)
        self.assertEqual(row["guest_in_memory_precopy_brake_total_excess_ms"],
                         2.5)
        self.assertEqual(
            row["guest_in_memory_postcopy_handoff_total_excess_ms"], 3.75
        )
        self.assertEqual(row["guest_in_memory_postcopy_total_excess_ms"], 4.0)
        self.assertEqual(
            row["guest_in_memory_completion_tail_total_excess_ms"], 0.5
        )
        self.assertEqual(
            row["handoff_control_src_enter_brake_to_request_postcopy_ms"],
            20.0,
        )

    def test_dump_guest_physical_memory_quotes_hmp_filename(self):
        calls = []
        default_timeout = self.mod.QMP_TIMEOUT_SECS

        class FakeSock:
            def gettimeout(self):
                return default_timeout

            def settimeout(self, _timeout):
                pass

        class FakeConn:
            def __init__(self):
                self.sock = FakeSock()

        def fake_qmp_ok(_conn, cmd, args=None):
            calls.append((cmd, args))
            return ""

        self.mod.qmp_ok = fake_qmp_ok
        self.mod.dump_guest_physical_memory(FakeConn(), 0x2000000, 4096,
                                            Path("/tmp/a b/dump.bin"))

        self.assertEqual(calls[0][0], "human-monitor-command")
        self.assertEqual(
            calls[0][1]["command-line"],
            'pmemsave 0x2000000 4096 "/tmp/a b/dump.bin"',
        )

    def test_dump_guest_physical_memory_uses_long_timeout_temporarily(self):
        timeouts = []

        class FakeSock:
            def __init__(self):
                self.timeout = 1.0

            def gettimeout(self):
                return self.timeout

            def settimeout(self, timeout):
                timeouts.append(timeout)
                self.timeout = timeout

        class FakeConn:
            def __init__(self):
                self.sock = FakeSock()

        def fake_qmp_ok(conn, _cmd, _args=None):
            self.assertEqual(conn.sock.gettimeout(),
                             self.mod.QMP_DUMP_TIMEOUT_SECS)
            return ""

        self.mod.qmp_ok = fake_qmp_ok
        conn = FakeConn()

        self.mod.dump_guest_physical_memory(conn, 0x2000000, 4096,
                                            Path("/tmp/dump.bin"))

        self.assertEqual(timeouts,
                         [self.mod.QMP_DUMP_TIMEOUT_SECS,
                          self.mod.QMP_TIMEOUT_SECS])
        self.assertEqual(conn.sock.gettimeout(), self.mod.QMP_TIMEOUT_SECS)

    def test_run_case_in_memory_latency_omits_debugcon_heartbeat(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            started_args = []
            constructed_collectors = []

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    constructed_collectors.append(sockets)

                def start(self):
                    pass

                def snapshot(self):
                    return []

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure,
                                      in_memory_guest_latency=False):
                self.assertTrue(in_memory_guest_latency)
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path,
                              env=None):
                started_args.append(extra_args)
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_qmp_ok(_conn, cmd, _args=None):
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 10,
                    "setup-time": 1,
                    "downtime": 1,
                    "x-cxl": {"phase": "disabled"},
                }
                samples = [{
                    "ts": 1.0,
                    "status": "completed",
                    "src-query-migrate": last,
                    "dst-query-migrate": {"x-cxl": {}},
                    "src-query-status": {"status": "postmigrate"},
                    "dst-query-status": {"status": "running"},
                    "ram": {"postcopy-requests": 0},
                }]
                return last, samples, {"status": "postmigrate"}, {
                    "status": "running"
                }, "completed"

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = (
                lambda path, proc, procs, timeout=10.0: Path(path).touch()
            )
            self.mod.connect_qmp = lambda path: FakeQMP(path)
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.collect_in_memory_guest_latency = (
                lambda *_args, **_kwargs: {"valid": True}
            )
            self.mod.parse_trace_log = (
                lambda _path: self.mod.trace_count_template()
            )

            result = self.mod.run_case(
                base, "pure_precopy", "light",
                in_memory_guest_latency=True,
            )

        flattened = [item for args in started_args for item in args]
        self.assertFalse(any("isa-debugcon" in item for item in flattened))
        self.assertFalse(constructed_collectors)
        self.assertTrue(result["guest_in_memory_latency"]["valid"])

    def test_run_case_in_memory_latency_passes_source_fallback_dump(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            collect_kwargs = []

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure,
                                      in_memory_guest_latency=False):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(_common, _qmp_sock, _extra_args, stderr_path,
                              env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_qmp_ok(_conn, cmd, _args=None):
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 10,
                    "setup-time": 1,
                    "downtime": 1,
                    "x-cxl": {"phase": "disabled"},
                }
                samples = [{
                    "ts": 1.0,
                    "status": "completed",
                    "src-query-migrate": last,
                    "dst-query-migrate": {"x-cxl": {}},
                    "src-query-status": {"status": "postmigrate"},
                    "dst-query-status": {"status": "running"},
                    "ram": {"postcopy-requests": 0},
                }]
                return last, samples, {"status": "postmigrate"}, {
                    "status": "running"
                }, "completed"

            def fake_collect_in_memory(conn, path, pressure, **kwargs):
                collect_kwargs.append((conn.path, path, kwargs))
                return {"valid": True, "dump_source": "fallback"}

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = (
                lambda path, proc, procs, timeout=10.0: Path(path).touch()
            )
            self.mod.connect_qmp = lambda path: FakeQMP(path)
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.collect_in_memory_guest_latency = fake_collect_in_memory
            self.mod.parse_trace_log = (
                lambda _path: self.mod.trace_count_template()
            )

            result = self.mod.run_case(
                base, "pure_precopy", "light",
                in_memory_guest_latency=True,
            )

        self.assertTrue(result["guest_in_memory_latency"]["valid"])
        self.assertEqual(len(collect_kwargs), 1)
        _conn_path, path, kwargs = collect_kwargs[0]
        self.assertIn("fallback_conn", kwargs)
        self.assertIn("fallback_path", kwargs)
        self.assertIn("fallback_marker_path", kwargs)
        self.assertTrue(str(kwargs["fallback_path"]).endswith(
            "guest-in-memory-latency-src.bin"
        ))
        self.assertTrue(str(kwargs["fallback_marker_path"]).endswith(
            "guest-in-memory-marker-src.bin"
        ))

    def test_run_case_in_memory_latency_can_dump_source_first(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            collect_calls = []

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure,
                                      in_memory_guest_latency=False):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(_common, _qmp_sock, _extra_args, stderr_path,
                              env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_qmp_ok(_conn, cmd, _args=None):
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 10,
                    "setup-time": 1,
                    "downtime": 1,
                    "x-cxl": {"phase": "disabled"},
                }
                samples = [{
                    "ts": 1.0,
                    "status": "completed",
                    "src-query-migrate": last,
                    "dst-query-migrate": {"x-cxl": {}},
                    "src-query-status": {"status": "postmigrate"},
                    "dst-query-status": {"status": "running"},
                    "ram": {"postcopy-requests": 0},
                }]
                return last, samples, {"status": "postmigrate"}, {
                    "status": "running"
                }, "completed"

            def fake_collect_in_memory(conn, path, pressure, **kwargs):
                collect_calls.append((conn.path, path, kwargs))
                return {"valid": True, "dump_source": "source-first"}

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = (
                lambda path, proc, procs, timeout=10.0: Path(path).touch()
            )
            self.mod.connect_qmp = lambda path: FakeQMP(path)
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.collect_in_memory_guest_latency = fake_collect_in_memory
            self.mod.parse_trace_log = (
                lambda _path: self.mod.trace_count_template()
            )

            result = self.mod.run_case(
                base, "pure_precopy", "light",
                in_memory_guest_latency=True,
                in_memory_guest_latency_source_first=True,
            )

        self.assertTrue(result["guest_in_memory_latency"]["valid"])
        self.assertEqual(len(collect_calls), 1)
        conn_path, path, kwargs = collect_calls[0]
        self.assertIn("src.qmp", str(conn_path))
        self.assertTrue(str(path).endswith("guest-in-memory-latency-src.bin"))
        self.assertTrue(str(kwargs["marker_path"]).endswith(
            "guest-in-memory-marker-src.bin"
        ))
        self.assertEqual(kwargs["primary_dump_source"], "source-first")
        self.assertNotIn("fallback_conn", kwargs)

    def test_collect_in_memory_guest_latency_parses_partial_dump_after_timeout(self):
        records = [100, 105, 500, 95]
        payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_LATENCY_MAGIC,
            self.mod.IN_MEMORY_LATENCY_VERSION,
            64,
            16,
            len(records),
            0,
            0,
            0,
        ) + struct.pack("<4I", *records)

        def fake_dump(_conn, _addr, _size, path):
            path.write_bytes(payload)
            raise TimeoutError("pmemsave timed out")

        self.mod.dump_guest_physical_memory = fake_dump

        with tempfile.TemporaryDirectory() as tmpdir:
            report = self.mod.collect_in_memory_guest_latency(
                object(), Path(tmpdir) / "latency.bin",
                "remap_xlarge_random_rw",
                tsc_khz=1_000_000,
            )

        self.assertTrue(report["valid"])
        self.assertTrue(report["partial_dump"])
        self.assertIn("pmemsave timed out", report["dump_error"])
        self.assertEqual(report["samples_read"], len(records))

    def test_collect_in_memory_guest_latency_dumps_only_recorded_samples(self):
        records = [100, 105, 500, 95]
        payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_LATENCY_MAGIC,
            self.mod.IN_MEMORY_LATENCY_VERSION,
            64,
            16,
            len(records),
            0,
            0,
            0,
        ) + struct.pack("<4I", *records)
        marker_records = [0, 1, 1, 6]
        marker_payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_MARKER_MAGIC,
            self.mod.IN_MEMORY_MARKER_VERSION,
            len(marker_records),
            16,
            6,
            3,
            0,
            0,
        ) + struct.pack("<4I", *marker_records)
        calls = []

        def fake_dump(_conn, addr, size, path):
            calls.append((addr, size))
            if addr == self.mod.IN_MEMORY_LATENCY_ADDR:
                path.write_bytes(
                    payload[:self.mod.IN_MEMORY_LATENCY_HEADER_BYTES]
                    if size == self.mod.IN_MEMORY_LATENCY_HEADER_BYTES
                    else payload[:size]
                )
            elif addr == self.mod.IN_MEMORY_MARKER_ADDR:
                path.write_bytes(
                    marker_payload[:self.mod.IN_MEMORY_MARKER_HEADER_BYTES]
                    if size == self.mod.IN_MEMORY_MARKER_HEADER_BYTES
                    else marker_payload[:size]
                )
            else:
                raise AssertionError(addr)

        self.mod.dump_guest_physical_memory = fake_dump

        with tempfile.TemporaryDirectory() as tmpdir:
            report = self.mod.collect_in_memory_guest_latency(
                object(), Path(tmpdir) / "latency.bin",
                "remap_xlarge_random_rw",
                marker_path=Path(tmpdir) / "marker.bin",
                tsc_khz=1_000_000,
            )

        self.assertTrue(report["valid"])
        self.assertTrue(report["marker_valid"])
        expected_latency_size = (
            self.mod.IN_MEMORY_LATENCY_HEADER_BYTES + len(records) * 4
        )
        expected_marker_size = (
            self.mod.IN_MEMORY_MARKER_HEADER_BYTES + len(marker_records) * 4
        )
        self.assertIn(
            (self.mod.IN_MEMORY_LATENCY_ADDR,
             self.mod.IN_MEMORY_LATENCY_HEADER_BYTES),
            calls,
        )
        self.assertIn(
            (self.mod.IN_MEMORY_LATENCY_ADDR, expected_latency_size),
            calls,
        )
        self.assertIn(
            (self.mod.IN_MEMORY_MARKER_ADDR,
             self.mod.IN_MEMORY_MARKER_HEADER_BYTES),
            calls,
        )
        self.assertIn(
            (self.mod.IN_MEMORY_MARKER_ADDR, expected_marker_size),
            calls,
        )

    def test_collect_in_memory_guest_latency_collects_marker_after_latency_timeout(self):
        records = [100, 105, 500, 95]
        payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_LATENCY_MAGIC,
            self.mod.IN_MEMORY_LATENCY_VERSION,
            64,
            16,
            len(records),
            0,
            0,
            0,
        ) + struct.pack("<4I", *records)
        marker_records = [0, 1, 1, 6]
        marker_payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_MARKER_MAGIC,
            self.mod.IN_MEMORY_MARKER_VERSION,
            len(marker_records),
            16,
            6,
            3,
            0,
            0,
        ) + struct.pack("<4I", *marker_records)

        def fake_dump(_conn, addr, size, path):
            if addr == self.mod.IN_MEMORY_LATENCY_ADDR:
                if size == self.mod.IN_MEMORY_LATENCY_HEADER_BYTES:
                    path.write_bytes(
                        payload[:self.mod.IN_MEMORY_LATENCY_HEADER_BYTES]
                    )
                    return
                path.write_bytes(payload[:size])
                raise TimeoutError("latency dump timed out")
            if addr == self.mod.IN_MEMORY_MARKER_ADDR:
                if size == self.mod.IN_MEMORY_MARKER_HEADER_BYTES:
                    path.write_bytes(
                        marker_payload[:self.mod.IN_MEMORY_MARKER_HEADER_BYTES]
                    )
                else:
                    path.write_bytes(marker_payload[:size])
                return
            raise AssertionError(addr)

        self.mod.dump_guest_physical_memory = fake_dump

        with tempfile.TemporaryDirectory() as tmpdir:
            report = self.mod.collect_in_memory_guest_latency(
                object(), Path(tmpdir) / "latency.bin",
                "remap_xlarge_random_rw",
                marker_path=Path(tmpdir) / "marker.bin",
                tsc_khz=1_000_000,
                marker_start_event=1,
                marker_complete_event=6,
            )

        self.assertTrue(report["valid"])
        self.assertTrue(report["marker_valid"])
        self.assertIn("latency dump timed out", report["dump_error"])
        self.assertEqual(report["corrected_window_sample_start"], 1)
        self.assertEqual(report["corrected_window_sample_end"], 4)

    def test_collect_in_memory_guest_latency_falls_back_to_source_dump(self):
        records = [100, 105, 500, 95]
        latency_payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_LATENCY_MAGIC,
            self.mod.IN_MEMORY_LATENCY_VERSION,
            64,
            16,
            len(records),
            0,
            0,
            0,
        ) + struct.pack("<4I", *records)
        marker_records = [1, 7, 8, 8]
        marker_payload = struct.pack(
            "<8I",
            self.mod.IN_MEMORY_MARKER_MAGIC,
            self.mod.IN_MEMORY_MARKER_VERSION,
            len(marker_records),
            16,
            8,
            3,
            0,
            0,
        ) + struct.pack("<4I", *marker_records)
        calls = []

        class FakeConn:
            def __init__(self, name):
                self.name = name

        def fake_dump(conn, addr, size, path):
            calls.append((conn.name, addr, size, path.name))
            if conn.name == "dst":
                if addr == self.mod.IN_MEMORY_LATENCY_ADDR:
                    path.write_bytes(
                        latency_payload[:self.mod.IN_MEMORY_LATENCY_HEADER_BYTES]
                    )
                    raise TimeoutError("dst pmemsave timed out")
                raise AssertionError("dst marker should not be needed")

            if addr == self.mod.IN_MEMORY_LATENCY_ADDR:
                path.write_bytes(
                    latency_payload[:self.mod.IN_MEMORY_LATENCY_HEADER_BYTES]
                    if size == self.mod.IN_MEMORY_LATENCY_HEADER_BYTES
                    else latency_payload[:size]
                )
            elif addr == self.mod.IN_MEMORY_MARKER_ADDR:
                path.write_bytes(
                    marker_payload[:self.mod.IN_MEMORY_MARKER_HEADER_BYTES]
                    if size == self.mod.IN_MEMORY_MARKER_HEADER_BYTES
                    else marker_payload[:size]
                )
            else:
                raise AssertionError(addr)

        self.mod.dump_guest_physical_memory = fake_dump

        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            report = self.mod.collect_in_memory_guest_latency(
                FakeConn("dst"), base / "latency.bin",
                "remap_xlarge_random_rw",
                marker_path=base / "marker.bin",
                fallback_conn=FakeConn("src"),
                fallback_path=base / "latency-src.bin",
                fallback_marker_path=base / "marker-src.bin",
                tsc_khz=1_000_000,
                marker_start_event=1,
                marker_complete_event=8,
            )

        self.assertTrue(report["valid"])
        self.assertEqual(report["dump_source"], "fallback")
        self.assertIn("dst pmemsave timed out", report["primary_dump_error"])
        self.assertEqual(report["samples_read"], len(records))
        self.assertEqual(report["precopy_brake_window_sample_start"], 1)
        self.assertEqual(report["precopy_brake_window_sample_end"], 3)
        self.assertTrue(any(call[0] == "src" for call in calls))

    def test_dump_guest_physical_memory_chunked_appends_chunks(self):
        calls = []

        def fake_dump(_conn, addr, size, path):
            calls.append((addr, size, path.name))
            path.write_bytes(bytes([addr & 0xff]) * size)

        self.mod.dump_guest_physical_memory = fake_dump
        old_chunk = self.mod.IN_MEMORY_DUMP_CHUNK_BYTES
        self.mod.IN_MEMORY_DUMP_CHUNK_BYTES = 4
        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                path = Path(tmpdir) / "dump.bin"
                self.mod.dump_guest_physical_memory_chunked(
                    object(), 0x1000, 10, path,
                )
                data = path.read_bytes()
        finally:
            self.mod.IN_MEMORY_DUMP_CHUNK_BYTES = old_chunk

        self.assertEqual(
            calls,
            [
                (0x1000, 4, "dump.bin.chunk"),
                (0x1004, 4, "dump.bin.chunk"),
                (0x1008, 2, "dump.bin.chunk"),
            ],
        )
        self.assertEqual(data, b"\x00" * 4 + b"\x04" * 4 + b"\x08" * 2)

    def test_make_runtime_socket_paths_stay_within_unix_limit(self):
        paths = self.mod.make_runtime_socket_paths()
        self.addCleanup(
            lambda: self.mod.shutil.rmtree(paths["socket_dir"],
                                           ignore_errors=True)
        )

        self.assertTrue(paths["socket_dir"].exists())
        self.assertLess(len(str(paths["src_qmp"])), 108)
        self.assertLess(len(str(paths["dst_qmp"])), 108)
        self.assertLess(len(str(paths["mig_sock"])), 108)
        self.assertLess(len(str(paths["src_heartbeat_sock"])), 108)
        self.assertLess(len(str(paths["dst_heartbeat_sock"])), 108)

    def test_collect_until_complete_breaks_on_qmp_probe_timeout(self):
        written = []
        sleeps = []
        ticks = itertools.count()
        samples = iter([
            {
                "ts": 1.0,
                "status": "postcopy-active",
                "src-query-migrate": {"status": "postcopy-active"},
                "dst-query-migrate": {"status": "active"},
                "src-query-status": {"status": "running"},
                "dst-query-status": {"status": "inmigrate"},
                "errors": {},
            },
            {
                "ts": 1.1,
                "status": None,
                "src-query-migrate": None,
                "dst-query-migrate": None,
                "src-query-status": None,
                "dst-query-status": None,
                "errors": {"src-query-migrate": "timed out"},
            },
            {
                "ts": 1.2,
                "status": None,
                "src-query-migrate": None,
                "dst-query-migrate": None,
                "src-query-status": None,
                "dst-query-status": None,
                "errors": {"src-query-migrate": "timed out"},
            },
        ])

        def fake_poll(_src_qmp, _dst_qmp):
            return next(samples)

        def fake_write_json(path, payload):
            written.append((Path(path).name, payload))

        self.mod.poll_migration_sample = fake_poll
        self.mod.write_json = fake_write_json
        self.mod.time.sleep = lambda secs: sleeps.append(secs)
        self.mod.time.time = lambda: next(ticks) * 0.2

        last, seen, src_status, dst_status, stop_reason = (
            self.mod.collect_until_complete(object(), object(),
                                            Path("/tmp/samples.json"),
                                            timeout=1.0,
                                            max_probe_failures=2)
        )

        self.assertEqual(last["status"], "postcopy-active")
        self.assertEqual(len(seen), 3)
        self.assertEqual(src_status, {"status": "running"})
        self.assertEqual(dst_status, {"status": "inmigrate"})
        self.assertEqual(stop_reason, "qmp-unresponsive")
        self.assertEqual(seen[-1]["errors"]["src-query-migrate"], "timed out")
        self.assertTrue(any(name == "samples.json" for name, _ in written))
        self.assertEqual(sleeps, [
            self.mod.SAMPLE_INTERVAL_SECS,
            self.mod.SAMPLE_INTERVAL_SECS,
        ])

    def test_collect_until_complete_keeps_completed_tail_samples(self):
        written = []
        sleeps = []
        ticks = itertools.count()
        samples = iter([
            {
                "ts": 1.0,
                "status": "postcopy-active",
                "src-query-migrate": {
                    "status": "postcopy-active",
                    "x-cxl": {"fault-primary-ready-send-samples": 1},
                },
                "dst-query-migrate": {"status": "active"},
                "src-query-status": {"status": "running"},
                "dst-query-status": {"status": "inmigrate"},
                "errors": {},
            },
            {
                "ts": 1.1,
                "status": "completed",
                "src-query-migrate": {
                    "status": "completed",
                    "x-cxl": {"fault-primary-ready-send-samples": 2},
                },
                "dst-query-migrate": {"status": "running"},
                "src-query-status": {"status": "postmigrate"},
                "dst-query-status": {"status": "running"},
                "errors": {},
            },
            {
                "ts": 1.2,
                "status": "completed",
                "src-query-migrate": {
                    "status": "completed",
                    "x-cxl": {"fault-primary-ready-send-samples": 2},
                },
                "dst-query-migrate": {"status": "running"},
                "src-query-status": {"status": "postmigrate"},
                "dst-query-status": {"status": "running"},
                "errors": {},
            },
            {
                "ts": 1.3,
                "status": "completed",
                "src-query-migrate": {
                    "status": "completed",
                    "x-cxl": {"fault-primary-ready-send-samples": 3},
                },
                "dst-query-migrate": {"status": "running"},
                "src-query-status": {"status": "postmigrate"},
                "dst-query-status": {"status": "running"},
                "errors": {},
            },
        ])

        def fake_poll(_src_qmp, _dst_qmp):
            return next(samples)

        def fake_write_json(path, payload):
            written.append((Path(path).name, payload))

        self.mod.poll_migration_sample = fake_poll
        self.mod.write_json = fake_write_json
        self.mod.time.sleep = lambda secs: sleeps.append(secs)
        self.mod.time.time = lambda: next(ticks) * 0.2

        last, seen, src_status, dst_status, stop_reason = (
            self.mod.collect_until_complete(object(), object(),
                                            Path("/tmp/samples.json"),
                                            timeout=1.0,
                                            max_probe_failures=2)
        )

        self.assertEqual(last["status"], "completed")
        self.assertEqual(len(seen), 4)
        self.assertEqual(src_status, {"status": "postmigrate"})
        self.assertEqual(dst_status, {"status": "running"})
        self.assertEqual(stop_reason, "completed")
        self.assertEqual(
            seen[-1]["src-query-migrate"]["x-cxl"]["fault-primary-ready-send-samples"],
            3,
        )
        self.assertTrue(any(name == "samples.json" for name, _ in written))
        self.assertEqual(sleeps, [
            self.mod.SAMPLE_INTERVAL_SECS,
            self.mod.SAMPLE_INTERVAL_SECS,
            self.mod.SAMPLE_INTERVAL_SECS,
        ])

    def test_effectiveness_requires_stage_improvement(self):
        cold = {
            "summary": {
                "max_dst_stage_present_slots": 0,
                "max_dst_fault_hits": 0,
            },
            "trace": {
                "combined": {
                    "warm_send": 0,
                    "warm_recv": 0,
                    "fault_hit": 0,
                    "fault_miss": 1,
                },
            },
        }
        warm = {
            "summary": {
                "max_dst_stage_present_slots": 4,
                "max_dst_fault_hits": 1,
            },
            "trace": {
                "combined": {
                    "warm_send": 2,
                    "warm_recv": 2,
                    "fault_hit": 1,
                    "fault_miss": 0,
                },
            },
        }

        report = self.mod.effectiveness_report(cold, warm)

        self.assertTrue(report["warm_activity_seen"])
        self.assertTrue(report["stage_population_improved"])
        self.assertTrue(report["fault_hits_improved_or_equal"])
        self.assertTrue(report["effective"])

    def test_effectiveness_requires_destination_receive(self):
        cold = {
            "summary": {
                "max_dst_stage_present_slots": 0,
                "max_dst_fault_hits": 0,
            },
            "trace": {
                "combined": {
                    "warm_send": 0,
                    "warm_recv": 0,
                    "fault_hit": 0,
                    "fault_miss": 1,
                },
            },
        }
        warm = {
            "summary": {
                "max_dst_stage_present_slots": 4,
                "max_dst_fault_hits": 1,
            },
            "trace": {
                "combined": {
                    "warm_send": 2,
                    "warm_recv": 0,
                    "fault_hit": 1,
                    "fault_miss": 0,
                },
            },
        }

        report = self.mod.effectiveness_report(cold, warm)

        self.assertFalse(report["warm_activity_seen"])
        self.assertFalse(report["effective"])

    def test_effectiveness_uses_warm_recv_as_stage_population_fallback(self):
        cold = {
            "summary": {
                "max_dst_stage_present_slots": 0,
                "max_dst_fault_hits": 0,
            },
            "trace": {
                "combined": {
                    "warm_send": 0,
                    "warm_recv": 0,
                    "fault_hit": 0,
                    "fault_miss": 1,
                },
            },
        }
        warm = {
            "summary": {
                "max_dst_stage_present_slots": 0,
                "max_dst_fault_hits": 0,
            },
            "trace": {
                "combined": {
                    "warm_send": 2,
                    "warm_recv": 2,
                    "fault_hit": 0,
                    "fault_miss": 1,
                },
            },
        }

        report = self.mod.effectiveness_report(cold, warm)

        self.assertEqual(report["stage_population_observed_cold"], 0)
        self.assertEqual(report["stage_population_observed_warm"], 2)
        self.assertTrue(report["stage_population_improved"])
        self.assertTrue(report["effective"])

    def test_parse_trace_log_counts_text_backend(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            trace_file = Path(tmpdir) / "trace.log"
            trace_file.write_text(
                "\n".join([
                    "cxl_hybrid_phase_transition old=1 new=2 reason=0 iteration=1",
                    "cxl_hybrid_phase_transition old=3 new=4 reason=0 iteration=2",
                    "cxl_hybrid_warm_page_send pc.ram offset=0x1000 len=4096",
                    "cxl_hybrid_warm_page_recv pc.ram offset=0x1000 len=4096",
                    "cxl_hybrid_warm_page_queued pc.ram offset=0x1000",
                    "cxl_hybrid_fault_hit pc.ram offset=0x1000 len=4096 read_time_ns=100",
                    "cxl_hybrid_fault_hit pc.ram offset=0x2000 len=4096 read_time_ns=250",
                    "cxl_hybrid_fault_place pc.ram offset=0x2000 place_time_ns=300 ret=0",
                    "cxl_hybrid_fault_miss pc.ram offset=0x2000",
                    "cxl_hybrid_publish_request_send pc.ram guest=0x2000 generation=7",
                    "cxl_hybrid_publish_request_recv pc.ram guest=0x2000 generation=7",
                    "cxl_hybrid_publish_ready_send pc.ram guest=0x2000 cxl=0x3000 generation=7",
                    "cxl_hybrid_publish_ready_recv pc.ram guest=0x2000 cxl=0x3000 generation=7",
                    "cxl_hybrid_publish_ready_queue pc.ram guest=0x2000 cxl=0x3000 generation=7 pending=1",
                    "cxl_hybrid_publish_ready_pop pc.ram guest=0x2000 cxl=0x3000 generation=7 pending=0",
                    "cxl_hybrid_publish_ready_drain_begin pending=1",
                    "cxl_hybrid_publish_ready_drain_end sent=1 pending=0",
                    "cxl_hybrid_completion_prepare_begin pending=0",
                    "cxl_hybrid_completion_prepare_end remaining=0 ready=0 pending=0 ret=0",
                    "cxl_hybrid_publish_wait_begin pc.ram guest=0x2000 generation=7",
                    "cxl_hybrid_publish_wait_complete pc.ram guest=0x2000 generation=7 wait_time_ns=12 ret=0",
                ]) + "\n",
                encoding="utf-8",
            )

            counts = self.mod.parse_trace_log(trace_file)

        self.assertEqual(counts["warm_send"], 1)
        self.assertEqual(counts["warm_recv"], 1)
        self.assertEqual(counts["warm_queued"], 1)
        self.assertEqual(counts["fault_hit"], 2)
        self.assertEqual(counts["fault_hit_read_bytes"], 8192)
        self.assertEqual(counts["fault_hit_read_time_ns"], 350)
        self.assertEqual(counts["max_fault_hit_read_time_ns"], 250)
        self.assertEqual(counts["fault_place"], 1)
        self.assertEqual(counts["fault_place_time_ns"], 300)
        self.assertEqual(counts["max_fault_place_time_ns"], 300)
        self.assertEqual(counts["fault_miss"], 1)
        self.assertEqual(counts["publish_request_send"], 1)
        self.assertEqual(counts["publish_request_recv"], 1)
        self.assertEqual(counts["publish_ready_send"], 1)
        self.assertEqual(counts["publish_ready_recv"], 1)
        self.assertEqual(counts["publish_ready_queue"], 1)
        self.assertEqual(counts["publish_ready_pop"], 1)
        self.assertEqual(counts["publish_ready_drain_begin"], 1)
        self.assertEqual(counts["publish_ready_drain_end"], 1)
        self.assertEqual(counts["completion_prepare_begin"], 1)
        self.assertEqual(counts["completion_prepare_end"], 1)
        self.assertEqual(counts["publish_wait_begin"], 1)
        self.assertEqual(counts["publish_wait_complete"], 1)
        self.assertEqual(counts["phase_postcopy_warm"], 1)

    def test_parse_postcopy_timeline_extracts_control_breakdown(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            src_trace = Path(tmpdir) / "src-trace.bin"
            dst_trace = Path(tmpdir) / "dst-trace.bin"
            src_trace.write_text(
                "\n".join([
                    "migration_precopy_timeline estimate now_ns=500000 state=4 iteration=1 pending=200 must=120 can=80 phase=1 coverage=20 staged=10",
                    "migration_precopy_timeline enter-brake now_ns=700000 state=4 iteration=2 pending=180 must=100 can=80 phase=2 coverage=50 staged=10",
                    "migration_precopy_timeline request-postcopy now_ns=900000 state=4 iteration=2 pending=160 must=90 can=70 phase=2 coverage=50 staged=10",
                    "migration_postcopy_timeline start now_ns=1000000 state=4 iteration=2 pending=0 package_loaded=0",
                    "migration_postcopy_timeline downtime-end now_ns=1800000 state=15 iteration=2 pending=0 package_loaded=0",
                    "migration_postcopy_timeline state-postcopy-active now_ns=3300000 state=6 iteration=2 pending=0 package_loaded=1",
                    "migration_postcopy_timeline completion-enter now_ns=4200000 state=6 iteration=2 pending=0 package_loaded=1",
                    "migration_postcopy_timeline completion-prepare-done now_ns=4700000 state=6 iteration=2 pending=0 package_loaded=1",
                    "migration_postcopy_timeline completed now_ns=5000000 state=7 iteration=2 pending=0 package_loaded=1",
                ]) + "\n",
                encoding="utf-8",
            )
            dst_trace.write_text(
                "\n".join([
                    "migration_postcopy_timeline dst-packaged-received now_ns=2500000 state=4 iteration=0 pending=1 package_loaded=0",
                    "migration_postcopy_timeline dst-postcopy-run-cmd now_ns=2600000 state=5 iteration=0 pending=0 package_loaded=0",
                    "migration_postcopy_timeline dst-postcopy-bh-vm-started now_ns=3100000 state=6 iteration=0 pending=0 package_loaded=0",
                    "migration_postcopy_timeline dst-postcopy-bh-ack-sent now_ns=3200000 state=6 iteration=0 pending=0 package_loaded=0",
                ]) + "\n",
                encoding="utf-8",
            )

            breakdown = self.mod.summarize_handoff_breakdown(
                src_trace,
                dst_trace,
                guest_latency={
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 5.0,
                    "handoff_stall_ms": 4.0,
                    "src_tail_gap_mean_ms": 1.1,
                    "src_tail_gap_max_ms": 1.2,
                    "dst_initial_gap_1_ms": 2.0,
                    "dst_initial_gap_2_ms": 1.5,
                    "dst_initial_excess_stall_ms": 1.5,
                },
                trace_counts={"combined": {
                    "postcopy_fault_request": 7,
                    "fault_hit_read_time_ns": 800,
                    "fault_place_time_ns": 900,
                    "dst_region_remap": 3,
                    "ram_save_queue_pages": 5,
                    "get_queued_page": 6,
                    "region_publish_pages": 64,
                    "ram_stream_publish_span_pages": 32,
                }},
                summary={
                    "dst_region_wait_samples": 4,
                    "dst_region_wait_time_ns": 1000,
                    "max_fault_publish_wait_time_ns": 1100,
                    "region_publish_requests": 2,
                    "region_publish_pages": 32,
                    "max_dst_region_wait_time_ns": 600,
                    "max_dst_region_map_time_ns": 700,
                    "region_publish_time_ns": 2000,
                },
                heartbeat_events=[
                    {"side": "src", "ts": 1.0, "mono_ns": 2_000_000},
                    {"side": "dst", "ts": 1.1, "mono_ns": 3_600_000},
                    {"side": "dst", "ts": 1.2, "mono_ns": 4_000_000},
                ],
            )

        self.assertEqual(breakdown["control_src_start_to_dst_vm_started_ms"], 2.1)
        self.assertEqual(breakdown["control_src_start_to_dst_ack_ms"], 2.2)
        self.assertEqual(breakdown["control_src_start_to_downtime_end_ms"], 0.8)
        self.assertEqual(breakdown["control_src_downtime_end_to_dst_run_ms"], 0.8)
        self.assertEqual(breakdown["control_dst_run_to_vm_started_ms"], 0.5)
        self.assertEqual(breakdown["control_dst_vm_started_to_ack_us"], 100.0)
        self.assertEqual(breakdown["control_src_start_to_enter_brake_ms"], 0.2)
        self.assertEqual(
            breakdown["control_src_enter_brake_to_request_postcopy_ms"], 0.2
        )
        self.assertEqual(
            breakdown["control_src_request_postcopy_to_postcopy_start_ms"], 0.1
        )
        self.assertEqual(
            breakdown["control_src_completion_enter_to_completed_ms"], 0.8
        )
        self.assertEqual(breakdown["guest_handoff_stall_ms"], 4.0)
        self.assertEqual(breakdown["postcopy_fault_request"], 7)
        self.assertEqual(breakdown["postcopy_fault_read_time_ns"], 800)
        self.assertEqual(breakdown["postcopy_fault_place_time_ns"], 900)
        self.assertEqual(breakdown["postcopy_fault_publish_wait_time_ns"], 1100)
        self.assertEqual(breakdown["postcopy_fault_total_time_ns"], 2800)
        self.assertEqual(breakdown["trace_dst_region_remap"], 3)
        self.assertEqual(breakdown["postcopy_region_wait_samples"], 4)
        self.assertEqual(breakdown["postcopy_region_wait_time_ns"], 1000)
        self.assertEqual(breakdown["postcopy_region_publish_requests"], 2)
        self.assertEqual(breakdown["postcopy_region_publish_pages"], 32)
        self.assertEqual(breakdown["postcopy_region_publish_time_ns"], 2000)
        self.assertEqual(breakdown["region_publish_pages"], 32)
        self.assertTrue(breakdown["post_vm_start_wallclock_alignment_available"])
        self.assertEqual(
            breakdown["post_vm_start_to_first_dst_heartbeat_ms"], 0.5)

    def test_extract_summary_uses_destination_x_cxl_counters(self):
        samples = [
            {
                "x-cxl": {
                    "dst-stage-slots": 0,
                    "dst-stage-present-slots": 0,
                    "dst-fault-hits": 0,
                    "dst-fault-misses": 0,
                    "warm-publish-pages": 3,
                    "fault-publish-requests": 1,
                    "fault-publish-waits": 0,
                    "fault-publish-wait-time-ns": 100,
                    "pending-publish-ready": 1,
                    "completion-pending-publish-ready": 0,
                    "remap-attempts": 2,
                    "remap-successes": 1,
                    "remapped-regions": 1,
                    "staged-pages": 4,
                    "phase": "postcopy-warm",
                    "warm-payload-fallback-pages": 7,
                },
                "dst-query-migrate": {
                    "x-cxl": {
                        "dst-stage-slots": 10,
                        "dst-stage-present-slots": 32,
                        "dst-fault-hits": 0,
                        "dst-fault-misses": 1,
                        "dst-fault-read-bytes": 8192,
                        "dst-fault-read-time-ns": 333,
                        "dst-fault-place-successes": 2,
                        "dst-fault-place-failures": 1,
                        "warm-publish-pages": 9,
                        "fault-publish-requests": 4,
                        "fault-publish-waits": 2,
                        "fault-publish-wait-time-ns": 500,
                        "fault-publish-primary-samples": 2,
                        "fault-publish-primary-time-ns": 180,
                        "max-fault-publish-primary-time-ns": 110,
                        "fault-publish-burst-samples": 2,
                        "fault-publish-burst-time-ns": 90,
                        "max-fault-publish-burst-time-ns": 70,
                        "fault-primary-ready-send-samples": 2,
                        "fault-primary-ready-send-time-ns": 220,
                        "max-fault-primary-ready-send-time-ns": 140,
                        "fault-publish-req-recv-samples": 2,
                        "fault-publish-req-recv-time-ns": 120,
                        "max-fault-publish-req-recv-time-ns": 80,
                        "fault-publish-req-handle-samples": 2,
                        "fault-publish-req-handle-time-ns": 210,
                        "max-fault-publish-req-handle-time-ns": 130,
                        "fault-primary-ready-drain-samples": 2,
                        "fault-primary-ready-drain-time-ns": 180,
                        "max-fault-primary-ready-drain-time-ns": 120,
                        "fault-primary-ready-write-samples": 2,
                        "fault-primary-ready-write-time-ns": 40,
                        "max-fault-primary-ready-write-time-ns": 30,
                        "fault-primary-ready-recv-samples": 2,
                        "fault-primary-ready-recv-time-ns": 260,
                        "max-fault-primary-ready-recv-time-ns": 170,
                        "fault-primary-ready-handle-samples": 2,
                        "fault-primary-ready-handle-time-ns": 50,
                        "max-fault-primary-ready-handle-time-ns": 35,
                        "fault-wait-ready-recv-samples": 2,
                        "fault-wait-ready-recv-time-ns": 260,
                        "max-fault-wait-ready-recv-time-ns": 170,
                        "fault-wait-after-ready-recv-samples": 2,
                        "fault-wait-after-ready-recv-time-ns": 40,
                        "max-fault-wait-after-ready-recv-time-ns": 30,
                        "pending-publish-ready": 3,
                        "completion-pending-publish-ready": 2,
                        "last-publish-ready-recv": {
                            "count": 2,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x2000,
                            "generation": 7,
                            "cxl-offset": 0x3000,
                        },
                        "last-publish-wait-begin": {
                            "count": 3,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x2000,
                            "generation": 7,
                        },
                        "last-publish-wait-complete": {
                            "count": 2,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x2000,
                            "generation": 7,
                            "wait-time-ns": 123,
                            "ret": 0,
                        },
                    },
                },
                "ram": {
                    "postcopy-requests": 1,
                    "dirty-pages-rate": 123,
                },
            },
        ]

        summary = self.mod.extract_summary(samples)

        self.assertEqual(summary["max_dst_stage_slots"], 10)
        self.assertEqual(summary["max_dst_stage_present_slots"], 32)
        self.assertEqual(summary["max_dst_fault_misses"], 1)
        self.assertEqual(summary["max_dst_fault_read_bytes"], 8192)
        self.assertEqual(summary["max_dst_fault_read_time_ns"], 333)
        self.assertEqual(summary["max_dst_fault_place_successes"], 2)
        self.assertEqual(summary["max_dst_fault_place_failures"], 1)
        self.assertEqual(summary["max_staged_pages"], 4)
        self.assertEqual(summary["max_remap_attempts"], 2)
        self.assertEqual(summary["max_remap_successes"], 1)
        self.assertEqual(summary["max_remapped_regions"], 1)
        self.assertEqual(summary["max_dirty_pages_rate"], 123)
        self.assertEqual(summary["max_warm_publish_pages"], 9)
        self.assertEqual(summary["max_fault_publish_requests"], 4)
        self.assertEqual(summary["max_fault_publish_waits"], 2)
        self.assertEqual(summary["max_fault_publish_wait_time_ns"], 500)
        self.assertEqual(summary["fault_publish_primary_samples"], 2)
        self.assertEqual(summary["fault_publish_primary_time_ns"], 180)
        self.assertEqual(summary["max_fault_publish_primary_time_ns"], 110)
        self.assertEqual(summary["fault_publish_burst_samples"], 2)
        self.assertEqual(summary["fault_publish_burst_time_ns"], 90)
        self.assertEqual(summary["max_fault_publish_burst_time_ns"], 70)
        self.assertEqual(summary["fault_primary_ready_send_samples"], 2)
        self.assertEqual(summary["fault_primary_ready_send_time_ns"], 220)
        self.assertEqual(summary["max_fault_primary_ready_send_time_ns"], 140)
        self.assertEqual(summary["fault_publish_req_recv_samples"], 2)
        self.assertEqual(summary["fault_publish_req_recv_time_ns"], 120)
        self.assertEqual(summary["max_fault_publish_req_recv_time_ns"], 80)
        self.assertEqual(summary["fault_publish_req_handle_samples"], 2)
        self.assertEqual(summary["fault_publish_req_handle_time_ns"], 210)
        self.assertEqual(summary["max_fault_publish_req_handle_time_ns"], 130)
        self.assertEqual(summary["fault_primary_ready_drain_samples"], 2)
        self.assertEqual(summary["fault_primary_ready_drain_time_ns"], 180)
        self.assertEqual(summary["max_fault_primary_ready_drain_time_ns"], 120)
        self.assertEqual(summary["fault_primary_ready_write_samples"], 2)
        self.assertEqual(summary["fault_primary_ready_write_time_ns"], 40)
        self.assertEqual(summary["max_fault_primary_ready_write_time_ns"], 30)
        self.assertEqual(summary["fault_primary_ready_recv_samples"], 2)
        self.assertEqual(summary["fault_primary_ready_recv_time_ns"], 260)
        self.assertEqual(summary["max_fault_primary_ready_recv_time_ns"], 170)
        self.assertEqual(summary["fault_primary_ready_handle_samples"], 2)
        self.assertEqual(summary["fault_primary_ready_handle_time_ns"], 50)
        self.assertEqual(summary["max_fault_primary_ready_handle_time_ns"], 35)
        self.assertEqual(summary["fault_wait_ready_recv_samples"], 2)
        self.assertEqual(summary["fault_wait_ready_recv_time_ns"], 260)
        self.assertEqual(summary["max_fault_wait_ready_recv_time_ns"], 170)
        self.assertEqual(summary["fault_wait_after_ready_recv_samples"], 2)
        self.assertEqual(summary["fault_wait_after_ready_recv_time_ns"], 40)
        self.assertEqual(summary["max_fault_wait_after_ready_recv_time_ns"], 30)
        self.assertEqual(summary["max_pending_publish_ready"], 3)
        self.assertEqual(summary["max_completion_pending_publish_ready"], 2)
        self.assertEqual(summary["max_warm_payload_fallback_pages"], 7)
        self.assertEqual(summary["last_publish_ready_recv"]["count"], 2)
        self.assertEqual(summary["last_publish_wait_begin"]["count"], 3)
        self.assertEqual(summary["last_publish_wait_complete"]["count"], 2)
        self.assertEqual(summary["last_publish_wait_complete"]["wait-time-ns"], 123)
        self.assertTrue(summary["saw_postcopy_warm"])

    def test_extract_summary_keeps_highest_count_last_publish_events(self):
        samples = [
            {
                "x-cxl": {
                    "last-publish-request": {
                        "count": 2,
                        "ramblock": "pc.ram",
                        "guest-offset": 0x1000,
                        "generation": 3,
                    },
                    "last-publish-ready": {
                        "count": 1,
                        "ramblock": "pc.ram",
                        "guest-offset": 0x1000,
                        "generation": 3,
                        "cxl-offset": 0x2000,
                    },
                    "last-completion-publish-ready": {
                        "count": 1,
                        "ramblock": "pc.ram",
                        "guest-offset": 0x1000,
                        "generation": 3,
                        "cxl-offset": 0x2000,
                    },
                },
                "dst-query-migrate": {
                    "x-cxl": {
                        "last-publish-ready-recv": {
                            "count": 2,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x1000,
                            "generation": 3,
                            "cxl-offset": 0x2000,
                        },
                        "last-publish-wait-begin": {
                            "count": 1,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x1000,
                            "generation": 3,
                        },
                        "last-publish-wait-complete": {
                            "count": 1,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x1000,
                            "generation": 3,
                            "wait-time-ns": 11,
                            "ret": 0,
                        },
                    },
                },
            },
            {
                "x-cxl": {
                    "last-publish-request": {
                        "count": 4,
                        "ramblock": "pc.ram",
                        "guest-offset": 0x5f0000,
                        "generation": 7,
                    },
                    "last-publish-ready": {
                        "count": 3,
                        "ramblock": "pc.ram",
                        "guest-offset": 0x5ef000,
                        "generation": 7,
                        "cxl-offset": 0x600000,
                    },
                },
                "dst-query-migrate": {
                    "x-cxl": {
                        "last-publish-ready-recv": {
                            "count": 5,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x5ef000,
                            "generation": 7,
                            "cxl-offset": 0x600000,
                        },
                        "last-publish-wait-begin": {
                            "count": 6,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x5ef000,
                            "generation": 7,
                        },
                        "last-publish-wait-complete": {
                            "count": 4,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x5ef000,
                            "generation": 7,
                            "wait-time-ns": 33,
                            "ret": 0,
                        },
                    },
                },
            },
        ]

        summary = self.mod.extract_summary(samples)

        self.assertEqual(summary["last_publish_request"]["count"], 4)
        self.assertEqual(summary["last_publish_request"]["guest-offset"], 0x5f0000)
        self.assertEqual(summary["last_publish_request"]["generation"], 7)
        self.assertEqual(summary["last_publish_ready"]["count"], 3)
        self.assertEqual(summary["last_publish_ready"]["guest-offset"], 0x5ef000)
        self.assertEqual(summary["last_publish_ready"]["cxl-offset"], 0x600000)
        self.assertEqual(summary["last_completion_publish_ready"]["count"], 1)
        self.assertEqual(summary["last_completion_publish_ready"]["guest-offset"],
                         0x1000)
        self.assertEqual(summary["last_publish_ready_recv"]["count"], 5)
        self.assertEqual(summary["last_publish_ready_recv"]["guest-offset"], 0x5ef000)
        self.assertEqual(summary["last_publish_wait_begin"]["count"], 6)
        self.assertEqual(summary["last_publish_wait_complete"]["count"], 4)

    def test_cxl_ready_ring_poller_reuses_publish_ready_timing_path(self):
        cxl_source = (SCRIPT_PATH.parent.parent / "migration" / "cxl.c").resolve()
        control_source = (SCRIPT_PATH.parent.parent / "migration" /
                          "cxl-hybrid-control.c").resolve()

        cxl_text = cxl_source.read_text(encoding="utf-8")
        control_text = control_source.read_text(encoding="utf-8")

        self.assertIn("int cxl_hybrid_handle_fault_ready_record(", cxl_text)
        self.assertIn("cxl_hybrid_record_publish_ready_recv_time(", cxl_text)
        self.assertIn("cxl_hybrid_handle_publish_ready(", cxl_text)
        self.assertIn("cxl_hybrid_handle_fault_ready_record(&record, &local_err)", control_text)

        helper_start = cxl_text.index("int cxl_hybrid_handle_fault_ready_record(")
        warm_desc_start = cxl_text.index("int cxl_hybrid_send_warm_descriptor(")
        helper = cxl_text[helper_start:warm_desc_start]
        self.assertIn("fault_primary = record->flags & CXL_HYBRID_FAULT_READY_F_PRIMARY;", helper)
        self.assertIn("cxl_hybrid_record_publish_ready_recv_time(ready_recv_elapsed_ns);",
                      helper)
        self.assertIn("return cxl_hybrid_handle_publish_ready(&notify, fault_primary,",
                      helper)

    def test_destination_postcopy_timeline_instrumentation_present(self):
        savevm = (REPO_ROOT / "migration" / "savevm.c").read_text(
            encoding="utf-8")

        self.assertIn("loadvm_trace_postcopy_timeline(", savevm)
        for stage in (
            "dst-postcopy-advise-cxl-init-begin",
            "dst-postcopy-advise-cxl-init-done",
            "dst-packaged-begin",
            "dst-packaged-received",
            "dst-cxl-metadata-begin",
            "dst-cxl-metadata-done",
            "dst-postcopy-run-cmd",
            "dst-postcopy-bh-vm-started",
            "dst-postcopy-bh-ack-sent",
        ):
            self.assertIn(stage, savevm)

    def test_cxl_fault_place_time_instrumentation_present(self):
        trace_events = (REPO_ROOT / "migration" / "trace-events").read_text(
            encoding="utf-8")
        cxl_source = (REPO_ROOT / "migration" / "cxl.c").read_text(
            encoding="utf-8")

        self.assertIn("cxl_hybrid_fault_place(", trace_events)
        self.assertIn("trace_cxl_hybrid_fault_place(", cxl_source)

    def test_redirect_precopy_rows_keep_fault_latency_columns_zeroed(self):
        result = {
            "mode": "redirect_precopy",
            "pressure": "remap_heavy",
            "final_status": "completed",
            "latency": {
                "total_time_ms": 123,
                "downtime_ms": 4,
                "setup_time_ms": 2,
            },
            "summary": {
                "saw_postcopy_warm": False,
                "max_warm_payload_fallback_pages": 0,
                "max_ram_postcopy_requests": 0,
            },
            "trace": {
                "combined": self.mod.trace_count_template(),
            },
            "stage_population_observed": 0,
            "guest_latency": {
                "baseline_gap_ms": 1.0,
                "handoff_gap_ms": 2.0,
                "handoff_stall_ms": 1.0,
                "max_gap_ms": 2.0,
                "max_gap_during_migration_ms": 2.0,
                "max_gap_stall_ms": 1.0,
            },
        }

        row = self.mod.summarize_single_result(
            "remap_heavy", "redirect_precopy",
            self.mod.resolve_threshold_profile("balanced"), 1, result)

        self.assertEqual(row["mode"], "redirect_precopy")
        self.assertEqual(row["batch_pages"], 0)
        self.assertEqual(row["fault_publish_req_recv_mean_ns"], 0)
        self.assertEqual(row["fault_primary_ready_recv_mean_ns"], 0)
        self.assertEqual(row["fault_wait_ready_recv_mean_ns"], 0)

    def test_summary_row_exports_stop_to_start_time(self):
        result = {
            "mode": "hybrid_postcopy_manual",
            "pressure": "remap_heavy",
            "final_status": "completed",
            "latency": {
                "total_time_ms": 74,
                "setup_time_ms": 1,
                "downtime_ms": 1,
                "stop_to_start_time_ms": 37,
                "observed_migration_window_ms": 114.0,
            },
            "summary": {},
            "trace": {
                "combined": self.mod.trace_count_template(),
            },
            "guest_latency": {},
        }

        row = self.mod.summarize_single_result(
            "remap_heavy", "hybrid_postcopy_manual",
            self.mod.resolve_threshold_profile("balanced"), 1, result)

        self.assertEqual(row["stop_to_start_time_ms"], 37)

    def test_summarize_single_result_reports_clean_remap_stats(self):
        row = self.mod.summarize_single_result(
            "remap_xlarge_random_rw",
            "hybrid_postcopy_auto",
            self.mod.resolve_threshold_profile("balanced"),
            1,
            {
                "summary": {},
                "trace": {"combined": {}},
                "latency": {},
                "guest_latency": {},
                "final_x_cxl": {
                    "backing-write-bytes": 16 * 1024 * 1024,
                    "remap-coverage": 67,
                    "clean-remap-scan-calls": 3,
                    "clean-remap-candidate-regions": 5,
                    "clean-remap-copy-bytes": 8 * 1024 * 1024,
                    "clean-remap-copy-time-ns": 1234,
                    "clean-remap-abandoned-dirty": 2,
                    "clean-remap-budget-exhaustions": 1,
                    "clean-remap-pending-bytes": 4 * 1024 * 1024,
                    "clean-remap-coverage": 25,
                    "clean-remap-prefault-bytes": 6 * 1024 * 1024,
                    "clean-remap-prefault-time-ns": 5678,
                    "clean-remap-prefault-errors": 1,
                },
                "final_status": "completed",
            },
        )

        self.assertEqual(row["final_backing_write_bytes"], 16 * 1024 * 1024)
        self.assertEqual(row["final_remap_coverage"], 67)
        self.assertEqual(row["clean_remap_scan_calls"], 3)
        self.assertEqual(row["clean_remap_candidate_regions"], 5)
        self.assertEqual(row["clean_remap_copy_bytes"], 8 * 1024 * 1024)
        self.assertEqual(row["clean_remap_copy_time_ns"], 1234)
        self.assertEqual(row["clean_remap_abandoned_dirty"], 2)
        self.assertEqual(row["clean_remap_budget_exhaustions"], 1)
        self.assertEqual(row["clean_remap_pending_bytes"], 4 * 1024 * 1024)
        self.assertEqual(row["clean_remap_coverage"], 25)
        self.assertEqual(row["clean_remap_prefault_bytes"], 6 * 1024 * 1024)
        self.assertEqual(row["clean_remap_prefault_time_ns"], 5678)
        self.assertEqual(row["clean_remap_prefault_errors"], 1)

    def test_run_case_flushes_trace_files_before_parsing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            qmp_calls = []
            parsed = []
            terminated = []
            heartbeat_snapshot_calls = []

            class FakeProc:
                def __init__(self, label):
                    self.label = label

                def poll(self):
                    return None

                def terminate(self):
                    terminated.append(self.label)

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    terminated.append(f"{self.label}-kill")

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    self.sockets = sockets

                def start(self):
                    pass

                def snapshot(self):
                    heartbeat_snapshot_calls.append(len(heartbeat_snapshot_calls))
                    return [
                        {"ts": 1.00, "side": "src"},
                        {"ts": 1.01, "side": "src"},
                        {"ts": 1.03, "side": "dst"},
                    ]

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path, env=None):
                label = Path(qmp_sock).stem
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc(label)

            def fake_wait_sock(path, proc, procs, timeout=10.0):
                Path(path).touch()

            def fake_connect_qmp(path):
                return FakeQMP(path)

            def fake_qmp_ok(conn, cmd, args=None):
                qmp_calls.append((Path(conn.path).stem, cmd, args))
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 10,
                    "x-cxl": {"phase": "disabled"},
                }
                samples = [
                    {
                        "ts": self.mod.time.time() + 0.010,
                        "status": "completed",
                        "src-query-migrate": {"status": "completed"},
                        "x-cxl": {"phase": "postcopy-warm", "staged-pages": 1},
                        "dst-query-migrate": {"x-cxl": {}},
                        "ram": {"postcopy-requests": 0},
                    }
                ]
                src_status = {"status": "postmigrate"}
                dst_status = {"status": "running"}
                return last, samples, src_status, dst_status, "completed"

            def fake_parse_trace_log(path):
                parsed.append(path.name)
                return self.mod.trace_count_template()

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = fake_wait_sock
            self.mod.connect_qmp = fake_connect_qmp
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.parse_trace_log = fake_parse_trace_log
            heartbeat_summary_kwargs = []

            def fake_summarize_guest_heartbeats(*_args, **kwargs):
                heartbeat_summary_kwargs.append(kwargs)
                return {
                    "events_total": 3,
                    "events_src": 2,
                    "events_dst": 1,
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 20.0,
                    "handoff_stall_ms": 19.0,
                    "max_gap_ms": 20.0,
                    "max_gap_during_migration_ms": 20.0,
                    "max_gap_stall_ms": 19.0,
                }

            self.mod.summarize_guest_heartbeats = fake_summarize_guest_heartbeats

            result = self.mod.run_case(base, "hybrid_postcopy_auto", "medium")

        flush_calls = [
            (peer, cmd) for peer, cmd, _args in qmp_calls
            if cmd == "trace-file-flush"
        ]
        self.assertEqual(flush_calls, [
            ("src", "trace-file-flush"),
            ("dst", "trace-file-flush"),
        ])
        self.assertEqual(parsed, ["src-trace.bin", "dst-trace.bin"])
        self.assertEqual(len(heartbeat_snapshot_calls), 1)
        self.assertEqual(result["guest_latency"]["handoff_gap_ms"], 20.0)
        self.assertEqual(result["latency"]["qemu_total_time_window_ms"], 10)
        self.assertAlmostEqual(
            result["latency"]["qmp_poll_tail_ms"],
            max(result["latency"]["observed_migration_window_ms"] - 10, 0.0),
        )
        self.assertAlmostEqual(
            heartbeat_summary_kwargs[0]["corrected_complete_ts"],
            heartbeat_summary_kwargs[0]["migration_start_ts"] + 0.010)
        self.assertAlmostEqual(
            heartbeat_summary_kwargs[0]["corrected_start_ts"],
            heartbeat_summary_kwargs[0]["migration_start_ts"])

    def test_run_case_waits_for_heartbeat_grace_when_destination_missing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            sleeps = []
            snapshots = []

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    self.sockets = sockets

                def start(self):
                    pass

                def snapshot(self):
                    snapshots.append(len(snapshots))
                    events = [
                        {"ts": 1.00, "side": "src"},
                        {"ts": 1.01, "side": "src"},
                    ]
                    if len(snapshots) >= 2:
                        events.append({"ts": 1.03, "side": "dst"})
                        events.append({"ts": 1.04, "side": "dst"})
                    return events

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path, env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_qmp_ok(conn, cmd, args=None):
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                now = self.mod.time.time()
                return (
                    {
                        "status": "completed",
                        "total-time": 10,
                        "x-cxl": {"phase": "disabled"},
                    },
                    [{
                        "ts": now + 0.010,
                        "status": "completed",
                        "src-query-migrate": {"status": "completed"},
                        "x-cxl": {"phase": "postcopy-warm"},
                    }],
                    {"status": "postmigrate"},
                    {"status": "running"},
                    "completed",
                )

            original_sleep = self.mod.time.sleep
            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = lambda path, proc, procs, timeout=10.0: Path(path).touch()
            self.mod.connect_qmp = lambda path: FakeQMP(path)
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.parse_trace_log = lambda _path: self.mod.trace_count_template()
            self.mod.time.sleep = lambda secs: sleeps.append(secs)

            try:
                result = self.mod.run_case(base, "hybrid_postcopy_auto", "light")
            finally:
                self.mod.time.sleep = original_sleep

        self.assertIn(self.mod.HEARTBEAT_POST_COMPLETE_GRACE_SECS, sleeps)
        self.assertEqual(len(snapshots), 2)
        self.assertEqual(result["guest_latency"]["events_dst"], 2)

    def test_run_case_enables_descriptor_trace_events(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    self.sockets = sockets

                def start(self):
                    pass

                def snapshot(self):
                    return []

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path, env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_wait_sock(path, proc, procs, timeout=10.0):
                Path(path).touch()

            def fake_connect_qmp(path):
                return FakeQMP(path)

            def fake_qmp_ok(conn, cmd, args=None):
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 10,
                    "setup-time": 1,
                    "downtime": 1,
                    "x-cxl": {"phase": "postcopy-warm"},
                }
                samples = [
                    {
                        "status": "completed",
                        "x-cxl": {"phase": "postcopy-warm"},
                        "dst-query-migrate": {"x-cxl": {}},
                        "ram": {"postcopy-requests": 0},
                    }
                ]
                src_status = {"status": "postmigrate"}
                dst_status = {"status": "running"}
                return last, samples, src_status, dst_status, "completed"

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = fake_wait_sock
            self.mod.connect_qmp = fake_connect_qmp
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.parse_trace_log = lambda _path: self.mod.trace_count_template()
            self.mod.summarize_guest_heartbeats = (
                lambda *_args, **_kwargs: {
                    "events_total": 0,
                    "events_src": 0,
                    "events_dst": 0,
                    "baseline_gap_ms": 0.0,
                    "handoff_gap_ms": 0.0,
                    "handoff_stall_ms": 0.0,
                    "max_gap_ms": 0.0,
                    "max_gap_during_migration_ms": 0.0,
                    "max_gap_stall_ms": 0.0,
                }
            )

            result = self.mod.run_case(base, "hybrid_postcopy_auto", "light")

            trace_events = Path(result["case_dir"]) / "trace-events"
            contents = trace_events.read_text(encoding="ascii").splitlines()

        self.assertIn("cxl_hybrid_warm_desc_send", contents)
        self.assertIn("cxl_hybrid_warm_desc_recv", contents)
        self.assertIn("cxl_hybrid_publish_request_send", contents)
        self.assertIn("cxl_hybrid_publish_request_recv", contents)
        self.assertIn("cxl_hybrid_publish_ready_send", contents)
        self.assertIn("cxl_hybrid_publish_ready_recv", contents)
        self.assertIn("cxl_hybrid_publish_ready_queue", contents)
        self.assertIn("cxl_hybrid_publish_ready_pop", contents)
        self.assertIn("cxl_hybrid_publish_ready_drain_begin", contents)
        self.assertIn("cxl_hybrid_publish_ready_drain_end", contents)
        self.assertIn("cxl_hybrid_completion_prepare_begin", contents)
        self.assertIn("cxl_hybrid_completion_prepare_end", contents)
        self.assertIn("cxl_hybrid_publish_wait_begin", contents)
        self.assertIn("cxl_hybrid_publish_wait_complete", contents)

    def test_run_case_pure_precopy_does_not_require_postcopy_warm(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    self.sockets = sockets

                def start(self):
                    pass

                def snapshot(self):
                    return [
                        {"ts": 1.00, "side": "src"},
                        {"ts": 1.02, "side": "src"},
                        {"ts": 1.05, "side": "dst"},
                    ]

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path, env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_wait_sock(path, proc, procs, timeout=10.0):
                Path(path).touch()

            def fake_connect_qmp(path):
                return FakeQMP(path)

            def fake_qmp_ok(conn, cmd, args=None):
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 18,
                    "setup-time": 2,
                    "downtime": 2,
                    "x-cxl": {"phase": "disabled"},
                }
                samples = [
                    {
                        "status": "completed",
                        "x-cxl": {"phase": "disabled"},
                        "dst-query-migrate": {"x-cxl": {}},
                        "ram": {"postcopy-requests": 0},
                    }
                ]
                src_status = {"status": "postmigrate"}
                dst_status = {"status": "running"}
                return last, samples, src_status, dst_status, "completed"

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = fake_wait_sock
            self.mod.connect_qmp = fake_connect_qmp
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.parse_trace_log = lambda _path: self.mod.trace_count_template()
            self.mod.summarize_guest_heartbeats = (
                lambda *_args, **_kwargs: {
                    "events_total": 3,
                    "events_src": 2,
                    "events_dst": 1,
                    "baseline_gap_ms": 2.0,
                    "handoff_gap_ms": 30.0,
                    "handoff_stall_ms": 28.0,
                    "max_gap_ms": 30.0,
                    "max_gap_during_migration_ms": 30.0,
                    "max_gap_stall_ms": 28.0,
                }
            )

            result = self.mod.run_case(base, "pure_precopy", "light")

        self.assertEqual(result["mode"], "pure_precopy")
        self.assertEqual(result["final_status"], "completed")
        self.assertEqual(result["latency"]["total_time_ms"], 18)
        self.assertEqual(result["guest_latency"]["max_gap_stall_ms"], 28.0)

    def test_run_case_native_postcopy_stream_requests_postcopy_without_cxl_backing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            commands = []

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    self.sockets = sockets

                def start(self):
                    pass

                def snapshot(self):
                    return [
                        {"ts": 1.00, "side": "src"},
                        {"ts": 1.02, "side": "src"},
                        {"ts": 1.07, "side": "dst"},
                    ]

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path, env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_wait_sock(path, proc, procs, timeout=10.0):
                Path(path).touch()

            def fake_connect_qmp(path):
                return FakeQMP(path)

            def fake_qmp_ok(conn, cmd, args=None):
                commands.append((cmd, args))
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 22,
                    "setup-time": 3,
                    "downtime": 2,
                    "x-cxl": {"phase": "disabled"},
                }
                samples = [
                    {
                        "status": "completed",
                        "x-cxl": {"phase": "disabled"},
                        "dst-query-migrate": {"x-cxl": {}},
                        "ram": {"postcopy-requests": 7},
                    }
                ]
                src_status = {"status": "postmigrate"}
                dst_status = {"status": "running"}
                return last, samples, src_status, dst_status, "completed"

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = fake_wait_sock
            self.mod.connect_qmp = fake_connect_qmp
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.parse_trace_log = lambda _path: self.mod.trace_count_template()
            self.mod.summarize_guest_heartbeats = (
                lambda *_args, **_kwargs: {
                    "events_total": 3,
                    "events_src": 2,
                    "events_dst": 1,
                    "baseline_gap_ms": 2.0,
                    "handoff_gap_ms": 50.0,
                    "handoff_stall_ms": 48.0,
                    "max_gap_ms": 50.0,
                    "max_gap_during_migration_ms": 50.0,
                    "max_gap_stall_ms": 48.0,
                }
            )

            result = self.mod.run_case(base, "native_postcopy_stream", "light")

        cmd_names = [cmd for cmd, _args in commands]
        self.assertIn("migrate", cmd_names)
        self.assertIn("migrate-start-postcopy", cmd_names)
        self.assertLess(cmd_names.index("migrate"),
                        cmd_names.index("migrate-start-postcopy"))
        self.assertEqual(result["mode"], "native_postcopy_stream")
        self.assertEqual(result["final_status"], "completed")
        self.assertEqual(result["summary"]["max_ram_postcopy_requests"], 7)
        self.assertEqual(result["postcopy_control"]["request_mode"],
                         "manual-native")
        self.assertTrue(result["postcopy_control"]["requested"])
        self.assertFalse((Path(result["case_dir"]) / "cxl-backing.img").exists())

    def test_run_case_native_rdma_precopy_uses_rdma_uri_without_postcopy_or_cxl_backing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            commands = []

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    self.sockets = sockets

                def start(self):
                    pass

                def snapshot(self):
                    return [
                        {"ts": 1.00, "side": "src"},
                        {"ts": 1.02, "side": "src"},
                        {"ts": 1.06, "side": "dst"},
                    ]

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path, env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_wait_sock(path, proc, procs, timeout=10.0):
                Path(path).touch()

            def fake_connect_qmp(path):
                return FakeQMP(path)

            def fake_qmp_ok(conn, cmd, args=None):
                commands.append((cmd, args))
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 20,
                    "setup-time": 2,
                    "downtime": 2,
                    "x-cxl": {"phase": "disabled"},
                }
                samples = [
                    {
                        "status": "completed",
                        "x-cxl": {"phase": "disabled"},
                        "dst-query-migrate": {"x-cxl": {}},
                        "ram": {"postcopy-requests": 0},
                    }
                ]
                src_status = {"status": "postmigrate"}
                dst_status = {"status": "running"}
                return last, samples, src_status, dst_status, "completed"

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = fake_wait_sock
            self.mod.connect_qmp = fake_connect_qmp
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.parse_trace_log = lambda _path: self.mod.trace_count_template()
            self.mod.summarize_guest_heartbeats = (
                lambda *_args, **_kwargs: {
                    "events_total": 3,
                    "events_src": 2,
                    "events_dst": 1,
                    "baseline_gap_ms": 2.0,
                    "handoff_gap_ms": 40.0,
                    "handoff_stall_ms": 38.0,
                    "max_gap_ms": 40.0,
                    "max_gap_during_migration_ms": 40.0,
                    "max_gap_stall_ms": 38.0,
                }
            )

            result = self.mod.run_case(
                base, "native_rdma_precopy", "light",
                rdma_host="10.0.0.2", rdma_port=7701,
            )

        cmd_names = [cmd for cmd, _args in commands]
        self.assertIn("migrate-incoming", cmd_names)
        self.assertIn("migrate", cmd_names)
        self.assertNotIn("migrate-start-postcopy", cmd_names)
        migrate_incoming_args = [
            args for cmd, args in commands if cmd == "migrate-incoming"
        ][0]
        migrate_args = [args for cmd, args in commands if cmd == "migrate"][0]
        self.assertEqual(migrate_incoming_args["uri"], "rdma:10.0.0.2:7701")
        self.assertEqual(migrate_args["uri"], "rdma:10.0.0.2:7701")
        self.assertEqual(result["mode"], "native_rdma_precopy")
        self.assertEqual(result["final_status"], "completed")
        self.assertFalse((Path(result["case_dir"]) / "cxl-backing.img").exists())

    def test_run_case_hybrid_postcopy_manual_requests_postcopy_with_cxl_backing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            commands = []

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    self.sockets = sockets

                def start(self):
                    pass

                def snapshot(self):
                    return [
                        {"ts": 1.00, "side": "src"},
                        {"ts": 1.03, "side": "src"},
                        {"ts": 1.06, "side": "dst"},
                    ]

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path, env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_wait_sock(path, proc, procs, timeout=10.0):
                Path(path).touch()

            def fake_connect_qmp(path):
                return FakeQMP(path)

            def fake_qmp_ok(conn, cmd, args=None):
                commands.append((cmd, args))
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 40,
                    "setup-time": 3,
                    "downtime": 1,
                    "x-cxl": {"phase": "postcopy-warm"},
                }
                samples = [
                    {
                        "status": "completed",
                        "x-cxl": {"phase": "postcopy-warm"},
                        "dst-query-migrate": {"x-cxl": {}},
                        "ram": {"postcopy-requests": 0},
                    }
                ]
                src_status = {"status": "postmigrate"}
                dst_status = {"status": "running"}
                return last, samples, src_status, dst_status, "completed"

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = fake_wait_sock
            self.mod.connect_qmp = fake_connect_qmp
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.parse_trace_log = lambda _path: self.mod.trace_count_template()
            self.mod.summarize_guest_heartbeats = (
                lambda *_args, **_kwargs: {
                    "events_total": 3,
                    "events_src": 2,
                    "events_dst": 1,
                    "baseline_gap_ms": 2.0,
                    "handoff_gap_ms": 30.0,
                    "handoff_stall_ms": 28.0,
                    "max_gap_ms": 30.0,
                    "max_gap_during_migration_ms": 30.0,
                    "max_gap_stall_ms": 28.0,
                }
            )

            result = self.mod.run_case(base, "hybrid_postcopy_manual", "light")
            cxl_backing_exists = (
                Path(result["case_dir"]) / "cxl-backing.img"
            ).exists()

        cmd_names = [cmd for cmd, _args in commands]
        self.assertIn("migrate", cmd_names)
        self.assertIn("migrate-start-postcopy", cmd_names)
        self.assertLess(cmd_names.index("migrate"),
                        cmd_names.index("migrate-start-postcopy"))
        self.assertEqual(result["mode"], "hybrid_postcopy_manual")
        self.assertEqual(result["final_status"], "completed")
        self.assertEqual(result["summary"]["max_ram_postcopy_requests"], 0)
        self.assertEqual(result["postcopy_control"]["request_mode"],
                         "manual-hybrid")
        self.assertTrue(result["postcopy_control"]["requested"])
        self.assertTrue(cxl_backing_exists)

    def test_run_case_hybrid_postcopy_manual_uses_explicit_cxl_path_override(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            commands = []

            class FakeProc:
                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    self.sockets = sockets

                def start(self):
                    pass

                def snapshot(self):
                    return [
                        {"ts": 1.00, "side": "src"},
                        {"ts": 1.03, "side": "src"},
                        {"ts": 1.06, "side": "dst"},
                    ]

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path, env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc()

            def fake_wait_sock(path, proc, procs, timeout=10.0):
                Path(path).touch()

            def fake_connect_qmp(path):
                return FakeQMP(path)

            def fake_qmp_ok(conn, cmd, args=None):
                commands.append((cmd, args))
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **_kwargs):
                last = {
                    "status": "completed",
                    "total-time": 40,
                    "setup-time": 3,
                    "downtime": 1,
                    "x-cxl": {"phase": "postcopy-warm"},
                }
                samples = [
                    {
                        "status": "completed",
                        "x-cxl": {"phase": "postcopy-warm"},
                        "dst-query-migrate": {"x-cxl": {}},
                        "ram": {"postcopy-requests": 0},
                    }
                ]
                src_status = {"status": "postmigrate"}
                dst_status = {"status": "running"}
                return last, samples, src_status, dst_status, "completed"

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = fake_wait_sock
            self.mod.connect_qmp = fake_connect_qmp
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.parse_trace_log = lambda _path: self.mod.trace_count_template()
            self.mod.summarize_guest_heartbeats = (
                lambda *_args, **_kwargs: {
                    "events_total": 3,
                    "events_src": 2,
                    "events_dst": 1,
                    "baseline_gap_ms": 2.0,
                    "handoff_gap_ms": 30.0,
                    "handoff_stall_ms": 28.0,
                    "max_gap_ms": 30.0,
                    "max_gap_during_migration_ms": 30.0,
                    "max_gap_stall_ms": 28.0,
                }
            )

            result = self.mod.run_case(
                base,
                "hybrid_postcopy_manual",
                "light",
                cxl_path_override="/dev/dax0.0",
            )
            cxl_backing_path = Path(result["case_dir"]) / "cxl-backing.img"

        migrate_set_parameters = [
            args for cmd, args in commands
            if cmd == "migrate-set-parameters"
        ]
        self.assertEqual(len(migrate_set_parameters), 2)
        self.assertEqual(migrate_set_parameters[0]["cxl-path"], "/dev/dax0.0")
        self.assertEqual(migrate_set_parameters[1]["cxl-path"], "/dev/dax0.0")
        self.assertFalse(cxl_backing_path.exists())

    def test_run_pressure_matrix_aggregates_requested_modes_and_pressures(self):
        calls = []

        def fake_run_case(base, mode, pressure):
            calls.append((str(base), mode, pressure))
            return {
                "mode": mode,
                "pressure": pressure,
                "latency": {
                    "total_time_ms": 10 if mode == "hybrid_postcopy_auto" else 20,
                    "downtime_ms": 3 if mode == "hybrid_postcopy_auto" else 5,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots":
                        4 if mode == "hybrid_postcopy_auto" else 0,
                    "max_dst_fault_hits": 0,
                    "max_dst_fault_misses":
                        1 if mode == "hybrid_postcopy_auto" else 0,
                    "max_warm_publish_pages":
                        5 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_publish_requests":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_publish_waits":
                        1 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_publish_wait_time_ns":
                        200 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_publish_primary_samples":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_publish_primary_time_ns":
                        150 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_publish_primary_time_ns":
                        90 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_publish_burst_samples":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_publish_burst_time_ns":
                        70 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_publish_burst_time_ns":
                        50 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_send_samples":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_send_time_ns":
                        210 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_primary_ready_send_time_ns":
                        140 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_publish_req_recv_samples":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_publish_req_recv_time_ns":
                        120 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_publish_req_recv_time_ns":
                        80 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_publish_req_handle_samples":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_publish_req_handle_time_ns":
                        190 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_publish_req_handle_time_ns":
                        120 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_drain_samples":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_drain_time_ns":
                        170 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_primary_ready_drain_time_ns":
                        110 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_write_samples":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_write_time_ns":
                        40 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_primary_ready_write_time_ns":
                        30 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_recv_samples":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_recv_time_ns":
                        260 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_primary_ready_recv_time_ns":
                        170 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_handle_samples":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_primary_ready_handle_time_ns":
                        50 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_primary_ready_handle_time_ns":
                        35 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_wait_ready_recv_samples":
                        1 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_wait_ready_recv_time_ns":
                        160 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_wait_ready_recv_time_ns":
                        160 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_wait_after_ready_recv_samples":
                        1 if mode == "hybrid_postcopy_auto" else 0,
                    "fault_wait_after_ready_recv_time_ns":
                        40 if mode == "hybrid_postcopy_auto" else 0,
                    "max_fault_wait_after_ready_recv_time_ns":
                        40 if mode == "hybrid_postcopy_auto" else 0,
                    "max_pending_publish_ready":
                        3 if mode == "hybrid_postcopy_auto" else 0,
                    "max_completion_pending_publish_ready":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "last_publish_request":
                        {
                            "count": 4,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x5f0000,
                            "generation": 7,
                        } if mode == "hybrid_postcopy_auto" else None,
                    "last_publish_ready":
                        {
                            "count": 3,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x5ef000,
                            "generation": 7,
                            "cxl-offset": 0x600000,
                        } if mode == "hybrid_postcopy_auto" else None,
                    "last_completion_publish_ready":
                        {
                            "count": 2,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x5ef000,
                            "generation": 7,
                            "cxl-offset": 0x600000,
                        } if mode == "hybrid_postcopy_auto" else None,
                    "last_publish_ready_recv":
                        {
                            "count": 5,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x5ef000,
                            "generation": 7,
                            "cxl-offset": 0x600000,
                        } if mode == "hybrid_postcopy_auto" else None,
                    "last_publish_wait_begin":
                        {
                            "count": 6,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x5ef000,
                            "generation": 7,
                        } if mode == "hybrid_postcopy_auto" else None,
                    "last_publish_wait_complete":
                        {
                            "count": 4,
                            "ramblock": "pc.ram",
                            "guest-offset": 0x5ef000,
                            "generation": 7,
                            "wait-time-ns": 33,
                            "ret": 0,
                        } if mode == "hybrid_postcopy_auto" else None,
                    "max_ram_postcopy_requests":
                        1 if mode == "hybrid_postcopy_auto" else 0,
                    "max_remap_attempts":
                        2 if mode == "redirect_precopy" else 0,
                    "max_remap_successes":
                        2 if mode == "redirect_precopy" else 0,
                    "max_remapped_regions":
                        2 if mode == "redirect_precopy" else 0,
                    "max_warm_desc_sent_pages":
                        2 if mode == "hybrid_postcopy_auto" else 0,
                    "max_warm_payload_fallback_pages":
                        0,
                    "max_warm_desc_skip_unremapped":
                        0,
                    "max_dirty_pages_rate": 10 if pressure == "light" else 20,
                    "max_staged_pages": 11 if pressure == "light" else 22,
                },
                "trace": {
                    "combined": {
                        "warm_send": 0,
                        "warm_recv": 0,
                        "warm_desc_send":
                            2 if mode == "hybrid_postcopy_auto" else 0,
                        "warm_desc_recv":
                            2 if mode == "hybrid_postcopy_auto" else 0,
                        "fault_hit": 0,
                        "fault_miss": 1 if mode == "hybrid_postcopy_auto" else 0,
                    },
                },
                "stage_population_observed":
                    4 if mode == "hybrid_postcopy_auto" else 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0 if pressure == "light" else 2.0,
                    "handoff_gap_ms": 8.0 if mode == "hybrid_postcopy_auto" else 12.0,
                    "handoff_stall_ms": 7.0 if mode == "hybrid_postcopy_auto" else 10.0,
                    "max_gap_ms": 8.0 if mode == "hybrid_postcopy_auto" else 12.0,
                    "max_gap_during_migration_ms":
                        8.0 if mode == "hybrid_postcopy_auto" else 12.0,
                    "max_gap_stall_ms":
                        7.0 if mode == "hybrid_postcopy_auto" else 10.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(Path(tmpdir),
                                                  ["light", "heavy"],
                                                  ["hybrid_postcopy_auto",
                                                   "pure_precopy"])

        self.assertEqual(calls, [
            (str(Path(tmpdir) / "light"), "hybrid_postcopy_auto", "light"),
            (str(Path(tmpdir) / "light"), "pure_precopy", "light"),
            (str(Path(tmpdir) / "heavy"), "hybrid_postcopy_auto", "heavy"),
            (str(Path(tmpdir) / "heavy"), "pure_precopy", "heavy"),
        ])
        self.assertEqual(matrix["pressures"], ["light", "heavy"])
        self.assertEqual(matrix["modes"], ["hybrid_postcopy_auto", "pure_precopy"])
        self.assertIn("light", matrix["results"])
        self.assertIn("hybrid_postcopy_auto", matrix["results"]["light"])
        self.assertEqual(matrix["summary"][0]["pressure"], "light")
        self.assertEqual(matrix["summary"][0]["mode"], "hybrid_postcopy_auto")
        self.assertEqual(matrix["summary"][0]["stage_population_observed"], 4)
        self.assertEqual(matrix["summary"][0]["workset_pages"], 32)
        self.assertEqual(matrix["summary"][0]["writes_per_page"], 1)
        self.assertEqual(matrix["summary"][0]["batch_pages"], 32)
        self.assertEqual(matrix["summary"][0]["warm_recv"], 0)
        self.assertEqual(matrix["summary"][0]["warm_desc_send"], 2)
        self.assertEqual(matrix["summary"][0]["warm_desc_recv"], 2)
        self.assertEqual(matrix["summary"][0]["max_warm_desc_sent_pages"], 2)
        self.assertEqual(matrix["summary"][0]["max_warm_payload_fallback_pages"], 0)
        self.assertEqual(matrix["summary"][0]["max_warm_publish_pages"], 5)
        self.assertEqual(matrix["summary"][0]["max_fault_publish_requests"], 2)
        self.assertEqual(matrix["summary"][0]["max_fault_publish_waits"], 1)
        self.assertEqual(matrix["summary"][0]["max_fault_publish_wait_time_ns"], 200)
        self.assertEqual(matrix["summary"][0]["fault_publish_primary_samples"], 2)
        self.assertEqual(matrix["summary"][0]["fault_publish_primary_time_ns"], 150)
        self.assertEqual(matrix["summary"][0]["max_fault_publish_primary_time_ns"], 90)
        self.assertEqual(matrix["summary"][0]["fault_publish_primary_mean_ns"], 75)
        self.assertEqual(matrix["summary"][0]["fault_publish_burst_samples"], 2)
        self.assertEqual(matrix["summary"][0]["fault_publish_burst_time_ns"], 70)
        self.assertEqual(matrix["summary"][0]["max_fault_publish_burst_time_ns"], 50)
        self.assertEqual(matrix["summary"][0]["fault_publish_burst_mean_ns"], 35)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_send_samples"], 2)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_send_time_ns"], 210)
        self.assertEqual(matrix["summary"][0]["max_fault_primary_ready_send_time_ns"], 140)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_send_mean_ns"], 105)
        self.assertEqual(matrix["summary"][0]["fault_publish_req_recv_samples"], 2)
        self.assertEqual(matrix["summary"][0]["fault_publish_req_recv_time_ns"], 120)
        self.assertEqual(matrix["summary"][0]["max_fault_publish_req_recv_time_ns"], 80)
        self.assertEqual(matrix["summary"][0]["fault_publish_req_recv_mean_ns"], 60)
        self.assertEqual(matrix["summary"][0]["fault_publish_req_handle_samples"], 2)
        self.assertEqual(matrix["summary"][0]["fault_publish_req_handle_time_ns"], 190)
        self.assertEqual(matrix["summary"][0]["max_fault_publish_req_handle_time_ns"], 120)
        self.assertEqual(matrix["summary"][0]["fault_publish_req_handle_mean_ns"], 95)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_drain_samples"], 2)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_drain_time_ns"], 170)
        self.assertEqual(matrix["summary"][0]["max_fault_primary_ready_drain_time_ns"], 110)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_drain_mean_ns"], 85)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_write_samples"], 2)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_write_time_ns"], 40)
        self.assertEqual(matrix["summary"][0]["max_fault_primary_ready_write_time_ns"], 30)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_write_mean_ns"], 20)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_recv_samples"], 2)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_recv_time_ns"], 260)
        self.assertEqual(matrix["summary"][0]["max_fault_primary_ready_recv_time_ns"], 170)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_recv_mean_ns"], 130)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_handle_samples"], 2)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_handle_time_ns"], 50)
        self.assertEqual(matrix["summary"][0]["max_fault_primary_ready_handle_time_ns"], 35)
        self.assertEqual(matrix["summary"][0]["fault_primary_ready_handle_mean_ns"], 25)
        self.assertEqual(matrix["summary"][0]["fault_wait_ready_recv_samples"], 1)
        self.assertEqual(matrix["summary"][0]["fault_wait_ready_recv_time_ns"], 160)
        self.assertEqual(matrix["summary"][0]["max_fault_wait_ready_recv_time_ns"], 160)
        self.assertEqual(matrix["summary"][0]["fault_wait_ready_recv_mean_ns"], 160)
        self.assertEqual(matrix["summary"][0]["fault_wait_after_ready_recv_samples"], 1)
        self.assertEqual(matrix["summary"][0]["fault_wait_after_ready_recv_time_ns"], 40)
        self.assertEqual(matrix["summary"][0]["max_fault_wait_after_ready_recv_time_ns"], 40)
        self.assertEqual(matrix["summary"][0]["fault_wait_after_ready_recv_mean_ns"], 40)
        self.assertEqual(matrix["summary"][0]["max_pending_publish_ready"], 3)
        self.assertEqual(matrix["summary"][0]["max_completion_pending_publish_ready"], 2)
        self.assertEqual(matrix["summary"][0]["last_publish_request_count"], 4)
        self.assertEqual(matrix["summary"][0]["last_publish_ready_count"], 3)
        self.assertEqual(matrix["summary"][0]["last_completion_publish_ready_count"], 2)
        self.assertEqual(matrix["summary"][0]["last_publish_ready_recv_count"], 5)
        self.assertEqual(matrix["summary"][0]["last_publish_wait_begin_count"], 6)
        self.assertEqual(matrix["summary"][0]["last_publish_wait_complete_count"], 4)
        self.assertEqual(matrix["summary"][0]["max_staged_pages"], 11)
        self.assertEqual(matrix["summary"][0]["total_time_ms"], 10)
        self.assertEqual(matrix["summary"][0]["guest_handoff_gap_ms"], 8.0)
        self.assertEqual(matrix["summary"][0]["guest_max_gap_stall_ms"], 7.0)
        self.assertEqual(matrix["summary"][1]["mode"], "pure_precopy")
        self.assertEqual(matrix["summary"][2]["mode"], "hybrid_postcopy_auto")
        self.assertEqual(matrix["summary"][3]["mode"], "pure_precopy")
        self.assertEqual(matrix["summary"][2]["workset_pages"], 128)
        self.assertEqual(matrix["summary"][2]["writes_per_page"], 8)
        self.assertEqual(matrix["summary"][2]["batch_pages"], 128)
        self.assertEqual(matrix["summary"][3]["batch_pages"], 0)
        self.assertEqual(matrix["summary"][3]["downtime_ms"], 5)
        self.assertEqual(matrix["summary"][3]["guest_handoff_gap_ms"], 12.0)
        self.assertEqual(matrix["summary"][1]["max_remap_attempts"], 0)

    def test_run_pressure_matrix_exports_redirect_remap_peaks(self):
        def fake_run_case(_base, mode, pressure):
            return {
                "mode": mode,
                "pressure": pressure,
                "latency": {
                    "total_time_ms": 30,
                    "downtime_ms": 4,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots": 0,
                    "max_dst_fault_hits": 0,
                    "max_dst_fault_misses": 0,
                    "max_warm_publish_pages": 0,
                    "max_fault_publish_requests": 0,
                    "max_fault_publish_waits": 0,
                    "max_fault_publish_wait_time_ns": 0,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 3 if mode == "redirect_precopy" else 0,
                    "max_remap_successes": 2 if mode == "redirect_precopy" else 0,
                    "max_remapped_regions": 2 if mode == "redirect_precopy" else 0,
                    "max_warm_desc_sent_pages": 0,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_dirty_pages_rate": 0,
                    "max_staged_pages": 100,
                },
                "trace": {
                    "combined": {
                        "warm_send": 0,
                        "warm_recv": 0,
                        "warm_desc_send": 0,
                        "warm_desc_recv": 0,
                        "fault_hit": 0,
                        "fault_miss": 0,
                    },
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 5.0,
                    "handoff_stall_ms": 4.0,
                    "max_gap_ms": 5.0,
                    "max_gap_during_migration_ms": 5.0,
                    "max_gap_stall_ms": 4.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_heavy"],
                ["redirect_precopy"],
            )

        self.assertEqual(matrix["summary"][0]["max_remap_attempts"], 3)
        self.assertEqual(matrix["summary"][0]["max_remap_successes"], 2)
        self.assertEqual(matrix["summary"][0]["max_remapped_regions"], 2)

    def test_run_pressure_matrix_supports_remap_heavy_profile(self):
        def fake_run_case(_base, mode, pressure):
            return {
                "mode": mode,
                "pressure": pressure,
                "latency": {
                    "total_time_ms": 50,
                    "downtime_ms": 5,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots": 0,
                    "max_dst_fault_hits": 0,
                    "max_dst_fault_misses": 0,
                    "max_warm_publish_pages": 0,
                    "max_fault_publish_requests": 0,
                    "max_fault_publish_waits": 0,
                    "max_fault_publish_wait_time_ns": 0,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 0,
                    "max_remap_successes": 0,
                    "max_remapped_regions": 0,
                    "max_warm_desc_sent_pages": 0,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_dirty_pages_rate": 0,
                    "max_staged_pages": 0,
                },
                "trace": {
                    "combined": {
                        "warm_send": 0,
                        "warm_recv": 0,
                        "warm_desc_send": 0,
                        "warm_desc_recv": 0,
                        "fault_hit": 0,
                        "fault_miss": 0,
                    },
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 2.0,
                    "handoff_stall_ms": 1.0,
                    "max_gap_ms": 2.0,
                    "max_gap_during_migration_ms": 2.0,
                    "max_gap_stall_ms": 1.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_heavy"],
                ["redirect_precopy"],
            )

        self.assertEqual(matrix["summary"][0]["pressure"], "remap_heavy")
        self.assertEqual(matrix["summary"][0]["workset_pages"], 2048)
        self.assertEqual(matrix["summary"][0]["writes_per_page"], 8)

    def test_run_pressure_matrix_supports_remap_heavy_random_rw_profile(self):
        def fake_run_case(_base, mode, pressure):
            return {
                "mode": mode,
                "pressure": pressure,
                "latency": {
                    "total_time_ms": 45,
                    "downtime_ms": 2,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots": 0,
                    "max_dst_fault_hits": 0,
                    "max_dst_fault_misses": 0,
                    "max_warm_publish_pages": 0,
                    "max_fault_publish_requests": 0,
                    "max_fault_publish_waits": 0,
                    "max_fault_publish_wait_time_ns": 0,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 0,
                    "max_remap_successes": 0,
                    "max_remapped_regions": 0,
                    "max_warm_desc_sent_pages": 0,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_dirty_pages_rate": 0,
                    "max_staged_pages": 0,
                },
                "trace": {
                    "combined": {
                        "warm_send": 0,
                        "warm_recv": 0,
                        "warm_desc_send": 0,
                        "warm_desc_recv": 0,
                        "fault_hit": 0,
                        "fault_miss": 0,
                    },
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 2.0,
                    "handoff_stall_ms": 1.0,
                    "max_gap_ms": 2.0,
                    "max_gap_during_migration_ms": 2.0,
                    "max_gap_stall_ms": 1.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_heavy_random_rw"],
                ["hybrid_postcopy_auto"],
            )

        self.assertEqual(matrix["summary"][0]["pressure"],
                         "remap_heavy_random_rw")
        self.assertEqual(matrix["summary"][0]["workset_pages"], 2048)
        self.assertEqual(matrix["summary"][0]["writes_per_page"], 1)
        self.assertEqual(matrix["summary"][0]["batch_pages"], 512)

    def test_run_pressure_matrix_forwards_explicit_cxl_path_override(self):
        calls = []

        def fake_run_case(_base, mode, pressure, cxl_path_override=None):
            calls.append((mode, pressure, cxl_path_override))
            return {
                "mode": mode,
                "pressure": pressure,
                "latency": {
                    "total_time_ms": 45,
                    "downtime_ms": 2,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots": 0,
                    "max_dst_fault_hits": 0,
                    "max_dst_fault_misses": 0,
                    "max_warm_publish_pages": 0,
                    "max_fault_publish_requests": 0,
                    "max_fault_publish_waits": 0,
                    "max_fault_publish_wait_time_ns": 0,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 0,
                    "max_remap_successes": 0,
                    "max_remapped_regions": 0,
                    "max_warm_desc_sent_pages": 0,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_dirty_pages_rate": 0,
                    "max_staged_pages": 0,
                },
                "trace": {
                    "combined": {
                        "warm_send": 0,
                        "warm_recv": 0,
                        "warm_desc_send": 0,
                        "warm_desc_recv": 0,
                        "fault_hit": 0,
                        "fault_miss": 0,
                    },
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 2.0,
                    "handoff_stall_ms": 1.0,
                    "max_gap_ms": 2.0,
                    "max_gap_during_migration_ms": 2.0,
                    "max_gap_stall_ms": 1.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_heavy"],
                ["hybrid_postcopy_manual"],
                cxl_path_override="/dev/dax0.0",
            )

        self.assertEqual(calls, [
            ("hybrid_postcopy_manual", "remap_heavy", "/dev/dax0.0"),
        ])

    def test_run_pressure_matrix_forwards_clean_remap_overrides(self):
        calls = []

        def fake_run_case(_base, mode, pressure, **kwargs):
            calls.append((mode, pressure,
                          kwargs.get("clean_remap_enable"),
                          kwargs.get("clean_remap_copy_budget"),
                          kwargs.get("clean_remap_throttle_us"),
                          kwargs.get("clean_remap_debug_mode")))
            return {
                "mode": mode,
                "pressure": pressure,
                "final_status": "completed",
                "latency": {
                    "total_time_ms": 45,
                    "downtime_ms": 2,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots": 0,
                    "max_dst_fault_hits": 0,
                    "max_dst_fault_misses": 0,
                    "max_warm_publish_pages": 0,
                    "max_fault_publish_requests": 0,
                    "max_fault_publish_waits": 0,
                    "max_fault_publish_wait_time_ns": 0,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 0,
                    "max_remap_successes": 0,
                    "max_remapped_regions": 0,
                    "max_warm_desc_sent_pages": 0,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_dirty_pages_rate": 0,
                    "max_staged_pages": 0,
                },
                "trace": {
                    "combined": {
                        "warm_send": 0,
                        "warm_recv": 0,
                        "warm_desc_send": 0,
                        "warm_desc_recv": 0,
                        "fault_hit": 0,
                        "fault_miss": 0,
                    },
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 2.0,
                    "handoff_stall_ms": 1.0,
                    "max_gap_ms": 2.0,
                    "max_gap_during_migration_ms": 2.0,
                    "max_gap_stall_ms": 1.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_heavy"],
                ["hybrid_postcopy_auto"],
                clean_remap_enable=True,
                clean_remap_copy_budget=8388608,
                clean_remap_throttle_us=200,
                clean_remap_debug_mode="copy-only",
            )

        self.assertEqual(calls, [
            ("hybrid_postcopy_auto", "remap_heavy", True, 8388608, 200,
             "copy-only"),
        ])

    def test_run_pressure_matrix_forwards_brake_enable(self):
        calls = []

        def fake_run_case(_base, mode, pressure, **kwargs):
            calls.append((mode, pressure, kwargs.get("brake_enable")))
            return {
                "mode": mode,
                "pressure": pressure,
                "final_status": "completed",
                "latency": {
                    "total_time_ms": 45,
                    "downtime_ms": 2,
                    "setup_time_ms": 1,
                },
                "summary": {},
                "trace": {"combined": self.mod.trace_count_template()},
                "stage_population_observed": 0,
                "guest_latency": {},
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_heavy"],
                ["hybrid_postcopy_auto"],
                brake_enable=False,
            )

        self.assertEqual(calls, [
            ("hybrid_postcopy_auto", "remap_heavy", False),
        ])

    def test_run_pressure_matrix_forwards_qemu_perf_option(self):
        calls = []

        def fake_run_case(_base, mode, pressure, **kwargs):
            calls.append(kwargs.get("qemu_perf"))
            return {
                "mode": mode,
                "pressure": pressure,
                "final_status": "completed",
                "latency": {
                    "total_time_ms": 45,
                    "downtime_ms": 2,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots": 0,
                    "max_dst_fault_hits": 0,
                    "max_dst_fault_misses": 0,
                    "max_warm_publish_pages": 0,
                    "max_fault_publish_requests": 0,
                    "max_fault_publish_waits": 0,
                    "max_fault_publish_wait_time_ns": 0,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 0,
                    "max_remap_successes": 0,
                    "max_remapped_regions": 0,
                    "max_warm_desc_sent_pages": 0,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_dirty_pages_rate": 0,
                    "max_staged_pages": 0,
                },
                "trace": {
                    "combined": self.mod.trace_count_template(),
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 2.0,
                    "handoff_stall_ms": 1.0,
                    "max_gap_ms": 2.0,
                    "max_gap_during_migration_ms": 2.0,
                    "max_gap_stall_ms": 1.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_heavy"],
                ["hybrid_postcopy_auto"],
                qemu_perf=True,
            )

        self.assertEqual(calls, [True])

    def test_run_pressure_matrix_forwards_in_memory_latency_option(self):
        calls = []

        def fake_run_case(_base, mode, pressure, **kwargs):
            calls.append(kwargs.get("in_memory_guest_latency"))
            return {
                "mode": mode,
                "pressure": pressure,
                "final_status": "completed",
                "latency": {
                    "total_time_ms": 45,
                    "downtime_ms": 2,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots": 0,
                    "max_dst_fault_hits": 0,
                    "max_dst_fault_misses": 0,
                    "max_warm_publish_pages": 0,
                    "max_fault_publish_requests": 0,
                    "max_fault_publish_waits": 0,
                    "max_fault_publish_wait_time_ns": 0,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 0,
                    "max_remap_successes": 0,
                    "max_remapped_regions": 0,
                    "max_warm_desc_sent_pages": 0,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_dirty_pages_rate": 0,
                    "max_staged_pages": 0,
                },
                "trace": {
                    "combined": self.mod.trace_count_template(),
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 2.0,
                    "handoff_stall_ms": 1.0,
                    "max_gap_ms": 2.0,
                    "max_gap_during_migration_ms": 2.0,
                    "max_gap_stall_ms": 1.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_heavy"],
                ["hybrid_postcopy_auto"],
                in_memory_guest_latency=True,
            )

        self.assertEqual(calls, [True])

    def test_run_pressure_matrix_supports_remap_mid_profile(self):
        def fake_run_case(_base, mode, pressure):
            return {
                "mode": mode,
                "pressure": pressure,
                "latency": {
                    "total_time_ms": 40,
                    "downtime_ms": 4,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots": 0,
                    "max_dst_fault_hits": 0,
                    "max_dst_fault_misses": 0,
                    "max_warm_publish_pages": 0,
                    "max_fault_publish_requests": 0,
                    "max_fault_publish_waits": 0,
                    "max_fault_publish_wait_time_ns": 0,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 1,
                    "max_remap_successes": 1,
                    "max_remapped_regions": 1,
                    "max_warm_desc_sent_pages": 0,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_dirty_pages_rate": 0,
                    "max_staged_pages": 512,
                },
                "trace": {
                    "combined": {
                        "warm_send": 0,
                        "warm_recv": 0,
                        "warm_desc_send": 0,
                        "warm_desc_recv": 0,
                        "fault_hit": 0,
                        "fault_miss": 0,
                    },
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 4.0,
                    "handoff_stall_ms": 3.0,
                    "max_gap_ms": 4.0,
                    "max_gap_during_migration_ms": 4.0,
                    "max_gap_stall_ms": 3.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_mid"],
                ["redirect_precopy"],
            )

        self.assertEqual(matrix["summary"][0]["pressure"], "remap_mid")
        self.assertEqual(matrix["summary"][0]["workset_pages"], 512)
        self.assertEqual(matrix["summary"][0]["writes_per_page"], 8)
        self.assertEqual(matrix["summary"][0]["max_remap_successes"], 1)

    def test_summarize_run_matrix_records_profile_and_repeat_index(self):
        rows = self.mod.summarize_run_matrix(
            pressure="medium",
            mode="hybrid_postcopy_auto",
            threshold_profile={
                "name": "aggressive",
                "x-cxl-switch-dirty-threshold": 1,
                "x-cxl-switch-max-iters": 8,
                "x-cxl-switch-max-precopy-ms": 0,
                "x-cxl-switch-min-remaining": 16 * 1024 * 1024,
            },
            run_results=[
                {"run_index": 1, "result": {"latency": {"total_time_ms": 100}, "summary": {}, "trace": {"combined": self.mod.trace_count_template()}, "guest_latency": {}}},
                {"run_index": 2, "result": {"latency": {"total_time_ms": 110}, "summary": {}, "trace": {"combined": self.mod.trace_count_template()}, "guest_latency": {}}},
            ],
        )

        self.assertEqual(rows[0]["threshold_profile"], "aggressive")
        self.assertEqual(rows[0]["run_index"], 1)
        self.assertEqual(rows[1]["run_index"], 2)
        self.assertEqual(rows[0]["x-cxl-switch-max-iters"], 8)
        self.assertEqual(rows[0]["x-cxl-switch-min-remaining"], 16 * 1024 * 1024)

    def test_run_pressure_matrix_repeat_adds_grouped_aggregate_rows(self):
        calls = []
        profile = {
            "name": "balanced",
            "x-cxl-switch-dirty-threshold": 1,
            "x-cxl-switch-max-iters": 20,
            "x-cxl-switch-max-precopy-ms": 0,
            "x-cxl-switch-min-remaining": 8 * 1024 * 1024,
        }

        def fake_run_case(base, mode, pressure, threshold_profile=None, run_index=1):
            calls.append((str(base), mode, pressure, run_index, threshold_profile["name"]))
            return {
                "mode": mode,
                "pressure": pressure,
                "final_status": "completed",
                "latency": {
                    "total_time_ms": 90 + run_index * 10,
                    "downtime_ms": 4 + run_index,
                    "stop_to_start_time_ms": 28 + run_index * 4,
                    "setup_time_ms": 2,
                    "observed_migration_window_ms": 120 + run_index * 15,
                },
                "summary": {
                    "max_dst_stage_present_slots": 4 + run_index,
                    "max_dst_fault_hits": run_index,
                    "max_dst_fault_misses": 1,
                    "max_warm_desc_sent_pages": 2,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_warm_publish_pages": 5,
                    "max_fault_publish_requests": 2 + run_index,
                    "max_fault_publish_waits": run_index,
                    "max_fault_publish_wait_time_ns": run_index * 100,
                    "max_dst_fault_read_time_ns": run_index * 1000,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 0,
                    "max_remap_successes": 0,
                    "max_remapped_regions": 0,
                    "max_dirty_pages_rate": 12,
                    "max_staged_pages": 24,
                    "saw_postcopy_warm": True,
                },
                "trace": {
                    "combined": {
                        "warm_send": 3,
                        "warm_recv": 3,
                        "warm_desc_send": 2,
                        "warm_desc_recv": 2,
                        "fault_hit": run_index,
                        "fault_hit_read_bytes": run_index * 4096,
                        "fault_hit_read_time_ns": run_index * 2000,
                        "max_fault_hit_read_time_ns": run_index * 1200,
                        "fault_place": run_index,
                        "fault_place_time_ns": run_index * 3000,
                        "max_fault_place_time_ns": run_index * 1400,
                        "fault_miss": 1,
                        "phase_postcopy_warm": 1,
                    },
                },
                "stage_population_observed": 4 + run_index,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 6.0,
                    "handoff_stall_ms": 5.0,
                    "max_gap_ms": 8.0,
                    "max_gap_during_migration_ms": 8.0,
                    "max_gap_stall_ms": 6.0 + run_index,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["light"],
                ["hybrid_postcopy_auto"],
                threshold_profile=profile,
                repeat=2,
            )

        self.assertEqual(calls, [
            (str(Path(tmpdir) / "light"), "hybrid_postcopy_auto", "light", 1, "balanced"),
            (str(Path(tmpdir) / "light"), "hybrid_postcopy_auto", "light", 2, "balanced"),
        ])
        self.assertEqual(len(matrix["summary"]), 2)
        self.assertEqual(matrix["summary"][0]["run_index"], 1)
        self.assertEqual(matrix["summary"][1]["run_index"], 2)
        self.assertEqual(matrix["summary"][0]["threshold_profile"], "balanced")
        self.assertEqual(len(matrix["summary_grouped"]), 1)
        grouped = matrix["summary_grouped"][0]
        self.assertEqual(grouped["total_time_ms_mean"], 105)
        self.assertEqual(grouped["total_time_ms_min"], 100)
        self.assertEqual(grouped["total_time_ms_max"], 110)
        self.assertEqual(grouped["observed_migration_window_ms_mean"], 142.5)
        self.assertEqual(grouped["observed_migration_window_ms_min"], 135)
        self.assertEqual(grouped["observed_migration_window_ms_max"], 150)
        self.assertEqual(grouped["downtime_ms_mean"], 5.5)
        self.assertEqual(grouped["stop_to_start_time_ms_mean"], 34)
        self.assertEqual(grouped["stop_to_start_time_ms_min"], 32)
        self.assertEqual(grouped["stop_to_start_time_ms_max"], 36)
        self.assertEqual(grouped["guest_max_gap_stall_ms_mean"], 7.5)
        self.assertEqual(grouped["max_fault_publish_waits_mean"], 1.5)
        self.assertEqual(grouped["max_fault_publish_wait_time_ns_max"], 200)
        self.assertEqual(grouped["max_dst_fault_read_time_ns_mean"], 1500)
        self.assertEqual(grouped["fault_hit_read_time_ns_mean"], 3000)
        self.assertEqual(grouped["max_fault_hit_read_time_ns_max"], 2400)
        self.assertEqual(grouped["fault_place_time_ns_mean"], 4500)
        self.assertEqual(grouped["max_fault_place_time_ns_max"], 2800)
        self.assertTrue(grouped["guardrail_all_completed"])
        self.assertTrue(grouped["guardrail_zero_payload_fallback"])
        self.assertTrue(grouped["guardrail_zero_ram_postcopy_requests"])
        self.assertTrue(grouped["guardrail_postcopy_warm_seen"])

    def test_run_pressure_matrix_records_failure_and_continues(self):
        calls = []

        def fake_run_case(base, mode, pressure):
            calls.append((str(base), mode, pressure))
            if mode == "pure_precopy":
                raise self.mod.ExperimentFailure({
                    "case": mode,
                    "reason": "migration-not-completed",
                    "stop_reason": "timeout",
                    "last": {
                        "status": "active",
                        "total-time": 120000,
                        "setup-time": 2,
                        "downtime": 0,
                        "x-cxl": {"phase": "disabled"},
                    },
                    "src_status": {"status": "running"},
                    "dst_status": {"status": "inmigrate"},
                    "summary": {
                        "max_dst_stage_present_slots": 0,
                        "max_dst_fault_hits": 0,
                        "max_dst_fault_misses": 0,
                        "max_warm_desc_sent_pages": 0,
                        "max_warm_payload_fallback_pages": 0,
                        "max_warm_desc_skip_unremapped": 0,
                        "max_warm_publish_pages": 0,
                        "max_fault_publish_requests": 0,
                        "max_fault_publish_waits": 0,
                        "max_fault_publish_wait_time_ns": 0,
                        "max_ram_postcopy_requests": 0,
                        "max_remap_attempts": 0,
                        "max_remap_successes": 0,
                        "max_remapped_regions": 0,
                        "max_dirty_pages_rate": 4000,
                        "max_staged_pages": 0,
                        "saw_postcopy_warm": False,
                    },
                    "trace": {
                        "combined": self.mod.trace_count_template(),
                    },
                    "guest_latency": {
                        "baseline_gap_ms": 1.0,
                        "handoff_gap_ms": None,
                        "handoff_stall_ms": None,
                        "max_gap_ms": 33.0,
                        "max_gap_during_migration_ms": 33.0,
                        "max_gap_stall_ms": 33.0,
                    },
                    "case_dir": "/tmp/failure-case",
                    "diagnostics_path": "/tmp/failure-case/diagnostics.json",
                })

            return {
                "mode": mode,
                "pressure": pressure,
                "final_status": "completed",
                "latency": {
                    "total_time_ms": 90,
                    "downtime_ms": 5,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_dst_stage_present_slots": 4,
                    "max_dst_fault_hits": 1,
                    "max_dst_fault_misses": 1,
                    "max_warm_desc_sent_pages": 2,
                    "max_warm_payload_fallback_pages": 0,
                    "max_warm_desc_skip_unremapped": 0,
                    "max_warm_publish_pages": 3,
                    "max_fault_publish_requests": 1,
                    "max_fault_publish_waits": 1,
                    "max_fault_publish_wait_time_ns": 100,
                    "max_ram_postcopy_requests": 0,
                    "max_remap_attempts": 0,
                    "max_remap_successes": 0,
                    "max_remapped_regions": 0,
                    "max_dirty_pages_rate": 10,
                    "max_staged_pages": 5,
                    "saw_postcopy_warm": True,
                },
                "trace": {
                    "combined": {
                        "warm_send": 0,
                        "warm_recv": 0,
                        "warm_desc_send": 2,
                        "warm_desc_recv": 2,
                        "fault_hit": 1,
                        "fault_miss": 1,
                        "phase_postcopy_warm": 1,
                    },
                },
                "stage_population_observed": 4,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 8.0,
                    "handoff_stall_ms": 7.0,
                    "max_gap_ms": 8.0,
                    "max_gap_during_migration_ms": 8.0,
                    "max_gap_stall_ms": 7.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["remap_heavy"],
                ["pure_precopy", "hybrid_postcopy_auto"],
            )

        self.assertEqual(calls, [
            (str(Path(tmpdir) / "remap_heavy"), "pure_precopy", "remap_heavy"),
            (str(Path(tmpdir) / "remap_heavy"), "hybrid_postcopy_auto", "remap_heavy"),
        ])
        self.assertEqual(matrix["summary"][0]["mode"], "pure_precopy")
        self.assertEqual(matrix["summary"][0]["final_status"], "active")
        self.assertEqual(matrix["summary"][0]["error_reason"], "migration-not-completed")
        self.assertEqual(matrix["summary"][1]["mode"], "hybrid_postcopy_auto")
        self.assertEqual(matrix["results"]["remap_heavy"]["pure_precopy"]["error_reason"],
                         "migration-not-completed")
        self.assertTrue(matrix["results"]["remap_heavy"]["pure_precopy"]["failed"])
        self.assertFalse(matrix["summary_grouped"][0]["guardrail_all_completed"])
        self.assertTrue(matrix["summary_grouped"][1]["guardrail_all_completed"])

    def test_grouped_postcopy_warm_guardrail_is_not_applicable_to_baselines(self):
        profile = {
            "name": "balanced",
            "x-cxl-switch-dirty-threshold": 1,
            "x-cxl-switch-max-iters": 20,
            "x-cxl-switch-max-precopy-ms": 0,
            "x-cxl-switch-min-remaining": 8 * 1024 * 1024,
        }

        def fake_run_case(_base, mode, pressure, threshold_profile=None, run_index=1):
            self.assertEqual(mode, "redirect_precopy")
            self.assertEqual(pressure, "light")
            self.assertEqual(threshold_profile["name"], "balanced")
            self.assertEqual(run_index, 1)
            return {
                "mode": mode,
                "pressure": pressure,
                "final_status": "completed",
                "latency": {
                    "total_time_ms": 80,
                    "downtime_ms": 5,
                    "setup_time_ms": 1,
                },
                "summary": {
                    "max_warm_payload_fallback_pages": 0,
                    "max_ram_postcopy_requests": 0,
                    "saw_postcopy_warm": False,
                },
                "trace": {
                    "combined": self.mod.trace_count_template(),
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 2.0,
                    "handoff_stall_ms": 1.0,
                    "max_gap_ms": 2.0,
                    "max_gap_during_migration_ms": 2.0,
                    "max_gap_stall_ms": 1.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["light"],
                ["redirect_precopy"],
                threshold_profile=profile,
                repeat=1,
            )

        grouped = matrix["summary_grouped"][0]
        self.assertIsNone(grouped["guardrail_postcopy_warm_seen"])

    def test_native_postcopy_grouped_guardrails_track_manual_postcopy(self):
        profile = {
            "name": "balanced",
            "x-cxl-switch-dirty-threshold": 1,
            "x-cxl-switch-max-iters": 20,
            "x-cxl-switch-max-precopy-ms": 0,
            "x-cxl-switch-min-remaining": 8 * 1024 * 1024,
        }

        def fake_run_case(_base, mode, pressure, threshold_profile=None, run_index=1):
            self.assertEqual(mode, "native_postcopy_stream")
            self.assertEqual(pressure, "light")
            self.assertEqual(threshold_profile["name"], "balanced")
            self.assertEqual(run_index, 1)
            return {
                "mode": mode,
                "pressure": pressure,
                "final_status": "completed",
                "latency": {
                    "total_time_ms": 70,
                    "downtime_ms": 4,
                    "setup_time_ms": 2,
                    "observed_migration_window_ms": 82,
                },
                "summary": {
                    "max_warm_payload_fallback_pages": 0,
                    "max_ram_postcopy_requests": 9,
                    "saw_postcopy_warm": False,
                },
                "trace": {
                    "combined": self.mod.trace_count_template(),
                },
                "postcopy_control": {
                    "requested": True,
                    "request_mode": "manual-native",
                    "request_error": None,
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 3.0,
                    "handoff_stall_ms": 2.0,
                    "max_gap_ms": 3.0,
                    "max_gap_during_migration_ms": 3.0,
                    "max_gap_stall_ms": 2.0,
                    "total_gap_during_migration_ms": 3.0,
                    "total_stall_during_migration_ms": 2.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["light"],
                ["native_postcopy_stream"],
                threshold_profile=profile,
                repeat=1,
            )

        grouped = matrix["summary_grouped"][0]
        self.assertTrue(grouped["guardrail_all_completed"])
        self.assertIsNone(grouped["guardrail_zero_payload_fallback"])
        self.assertIsNone(grouped["guardrail_zero_ram_postcopy_requests"])
        self.assertIsNone(grouped["guardrail_postcopy_warm_seen"])
        self.assertTrue(grouped["guardrail_postcopy_request_issued"])
        self.assertTrue(grouped["guardrail_ram_postcopy_requests_observed"])

    def test_hybrid_manual_grouped_guardrails_keep_hybrid_checks_and_request_flag(self):
        profile = {
            "name": "balanced",
            "x-cxl-switch-dirty-threshold": 1,
            "x-cxl-switch-max-iters": 20,
            "x-cxl-switch-max-precopy-ms": 0,
            "x-cxl-switch-min-remaining": 8 * 1024 * 1024,
        }

        def fake_run_case(_base, mode, pressure, threshold_profile=None, run_index=1):
            self.assertEqual(mode, "hybrid_postcopy_manual")
            self.assertEqual(pressure, "light")
            self.assertEqual(threshold_profile["name"], "balanced")
            self.assertEqual(run_index, 1)
            return {
                "mode": mode,
                "pressure": pressure,
                "final_status": "completed",
                "latency": {
                    "total_time_ms": 60,
                    "downtime_ms": 1,
                    "setup_time_ms": 2,
                    "observed_migration_window_ms": 95,
                },
                "summary": {
                    "max_warm_payload_fallback_pages": 0,
                    "max_ram_postcopy_requests": 0,
                    "saw_postcopy_warm": True,
                },
                "trace": {
                    "combined": {
                        **self.mod.trace_count_template(),
                        "phase_postcopy_warm": 1,
                    },
                },
                "postcopy_control": {
                    "requested": True,
                    "request_mode": "manual-hybrid",
                    "request_error": None,
                },
                "stage_population_observed": 0,
                "guest_latency": {
                    "baseline_gap_ms": 1.0,
                    "handoff_gap_ms": 4.0,
                    "handoff_stall_ms": 3.0,
                    "max_gap_ms": 4.0,
                    "max_gap_during_migration_ms": 4.0,
                    "max_gap_stall_ms": 3.0,
                    "total_gap_during_migration_ms": 4.0,
                    "total_stall_during_migration_ms": 3.0,
                },
            }

        self.mod.run_case = fake_run_case

        with tempfile.TemporaryDirectory() as tmpdir:
            matrix = self.mod.run_pressure_matrix(
                Path(tmpdir),
                ["light"],
                ["hybrid_postcopy_manual"],
                threshold_profile=profile,
                repeat=1,
            )

        grouped = matrix["summary_grouped"][0]
        self.assertTrue(grouped["guardrail_all_completed"])
        self.assertTrue(grouped["guardrail_zero_payload_fallback"])
        self.assertTrue(grouped["guardrail_zero_ram_postcopy_requests"])
        self.assertTrue(grouped["guardrail_postcopy_warm_seen"])
        self.assertTrue(grouped["guardrail_postcopy_request_issued"])
        self.assertIsNone(grouped["guardrail_ram_postcopy_requests_observed"])

    def test_run_case_forwards_migration_timeout(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            base = Path(tmpdir)
            seen = {}

            class FakeProc:
                def __init__(self, label):
                    self.label = label

                def poll(self):
                    return None

                def terminate(self):
                    pass

                def wait(self, timeout=None):
                    return 0

                def kill(self):
                    pass

            class FakeQMP:
                def __init__(self, path):
                    self.path = path

                def close(self):
                    pass

            class FakeHeartbeatCollector:
                def __init__(self, sockets):
                    self.sockets = sockets

                def start(self):
                    pass

                def snapshot(self):
                    return []

                def close(self):
                    pass

            def fake_build_boot_image(case_dir, pressure):
                img = case_dir / f"warm-boot-{pressure}.img"
                img.write_bytes(b"\0" * 512)
                return img

            def fake_start_vm(common, qmp_sock, extra_args, stderr_path, env=None):
                Path(stderr_path).write_text("", encoding="utf-8")
                return FakeProc(Path(qmp_sock).stem)

            def fake_wait_sock(path, proc, procs, timeout=10.0):
                Path(path).touch()

            def fake_connect_qmp(path):
                return FakeQMP(path)

            def fake_qmp_ok(conn, cmd, args=None):
                if cmd == "query-status":
                    return {"status": "running"}
                return {}

            def fake_collect_until_complete(*_args, **kwargs):
                seen["timeout"] = kwargs["timeout"]
                last = {
                    "status": "completed",
                    "total-time": 10,
                    "setup-time": 1,
                    "downtime": 1,
                    "x-cxl": {"phase": "postcopy-warm"},
                }
                samples = [
                    {
                        "status": "completed",
                        "x-cxl": {"phase": "postcopy-warm"},
                        "dst-query-migrate": {"x-cxl": {}},
                        "ram": {"postcopy-requests": 0},
                    }
                ]
                src_status = {"status": "postmigrate"}
                dst_status = {"status": "running"}
                return last, samples, src_status, dst_status, "completed"

            self.mod.build_boot_image = fake_build_boot_image
            self.mod.start_vm = fake_start_vm
            self.mod.wait_sock = fake_wait_sock
            self.mod.connect_qmp = fake_connect_qmp
            self.mod.connect_stream_socket = lambda _path: object()
            self.mod.GuestHeartbeatCollector = FakeHeartbeatCollector
            self.mod.qmp_ok = fake_qmp_ok
            self.mod.collect_until_complete = fake_collect_until_complete
            self.mod.parse_trace_log = lambda _path: self.mod.trace_count_template()
            self.mod.summarize_guest_heartbeats = (
                lambda *_args, **_kwargs: {
                    "events_total": 0,
                    "events_src": 0,
                    "events_dst": 0,
                    "baseline_gap_ms": 0.0,
                    "handoff_gap_ms": 0.0,
                    "handoff_stall_ms": 0.0,
                    "max_gap_ms": 0.0,
                    "max_gap_during_migration_ms": 0.0,
                    "max_gap_stall_ms": 0.0,
                }
            )

            self.mod.run_case(base, "hybrid_postcopy_auto", "light",
                              migration_timeout=self.mod.DEFAULT_MIGRATION_TIMEOUT_SECS + 30)

        self.assertEqual(seen["timeout"], self.mod.DEFAULT_MIGRATION_TIMEOUT_SECS + 30)


if __name__ == "__main__":
    unittest.main()
