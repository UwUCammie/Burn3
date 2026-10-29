"""Evidence admission and cleanup checks; no game, GPU or benchmark required."""
import argparse
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import json
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "windows_performance", Path(__file__).resolve().parents[1] / "tools/run_windows_performance.py")
subject = importlib.util.module_from_spec(spec)
spec.loader.exec_module(subject)


def boot_log():
    return """EE instructions: 290
kernel calls handled: 10
VBlank starts: 2
emulated EE cycles: 500
paraLLEl-GS completion: state=terminal healthy=1 failure=none hello=1 complete=2/2 pending=0 error=0 eos=1 clean=1 session=123 last-sequence=3 last-raw-vsync=2 eos-raw-vsync=2 next-raw-vsync=3 shutdown-timeout=0 protocol-failures=0 bytes=256
paraLLEl-GS live: VBlanks=2 FIFO="pipe" software=disabled
spu2-render={frames=100 non-silent=80 output=queued fnv=0xabcd}
stop reason: configured step limit reached (normal completion)
"""


def replay_log():
    return boot_log().replace("configured step limit reached (normal completion)",
                             "guest-work measurement exact step budget reached (normal window completion)") + """Guest input timeline: replay validated polls=7 steps=300 cycles=500
Guest-work measurement: completed scope=EE instruction-boundary fixed guest work normal-window-completion=yes requested-steps=100 actual={steps=100 ee-cycles=150 vblank-starts=1 audio-frames=40 monotonic-ns=2000000000}
  Guest-work start: steps=200 ee-cycles=350 vblank-starts=1 audio-frames=60 audio-fnv1a64-cumulative=0x1234 monotonic-ns=10000000000
  Guest-work end: steps=300 ee-cycles=500 vblank-starts=2 audio-frames=100 audio-fnv1a64-cumulative=0xabcd monotonic-ns=12000000000
"""


TIMELINE = {"start": [200, 350], "end": [7, 300, 500], "steps": 100, "path": "route.b3ir"}


class WindowsPerformanceTests(unittest.TestCase):
    def test_native_rgba_requires_family_coverage_and_consistent_totals(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        base += "Native strip bridge: jobs=20 packets=15 quadwords=600\n"
        active = dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_RGBA_CASE="local.b3geo")
        row = "Native strip families: specular=12 supplied-alpha=8\n"
        self.assertEqual(subject.performance_telemetry(base + row, active)["native_strip_families"],
                         {"specular": 12, "supplied_alpha": 8, "two_sided": 0, "two_sided_specular": 0, "environment": 0})
        for invalid in ("", row + row, row.replace("alpha=8", "alpha=7"),
                        row.replace("specular=12 supplied-alpha=8", "specular=20 supplied-alpha=0")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE="local.b3geo"))

    def test_native_two_sided_requires_coverage_and_accounts_for_all_families(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        base += "Native strip bridge: jobs=20 packets=15 quadwords=600\n"
        active = dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_TWO_SIDED_CASE="local.b3geo")
        row = "Native strip families: specular=0 supplied-alpha=0 two-sided=20\n"
        self.assertEqual(subject.performance_telemetry(base + row, active)["native_strip_families"]["two_sided"], 20)
        for invalid in ("", row.replace("two-sided=20", "two-sided=19"),
                        row.replace("specular=0", "specular=20").replace("two-sided=20", "two-sided=0")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE="local.b3geo"))

    def test_native_two_sided_specular_requires_distinct_coverage(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        base += "Native strip bridge: jobs=20 packets=15 quadwords=600\n"
        active = dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_TWO_SIDED_SPECULAR_CASE="local.b3geo")
        row = "Native strip families: specular=0 supplied-alpha=0 two-sided=0 two-sided-specular=20\n"
        self.assertEqual(subject.performance_telemetry(base + row, active)["native_strip_families"],
                         {"specular": 0, "supplied_alpha": 0, "two_sided": 0, "two_sided_specular": 20, "environment": 0})
        for invalid in ("", row + row, row.replace("two-sided-specular=20", "two-sided-specular=19"),
                        row.replace("two-sided=0 two-sided-specular=20", "two-sided=20 two-sided-specular=0")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE="local.b3geo"))

    def test_idle_batch_resume_requires_activation(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        active = dict(subject.FLAGS, TAKEDOWN_JOIN_PUMP_RESUME_IDLE_BATCH="1")
        row = "Join-pump idle batch resume: on loops=120\n"
        self.assertEqual(subject.performance_telemetry(base + row, active)["idle_batch_resumed_loops"], 120)
        for invalid in ("", row + row, row.replace("120", "0"), row.replace("on", "off")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, subject.FLAGS)

    def test_native_join_wait_requires_activation(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        active = dict(subject.FLAGS, TAKEDOWN_NATIVE_JOIN_DRAIN_WAIT="1")
        row = "Native join-drain wait: on calls=3 instructions=63\n"
        self.assertEqual(subject.performance_telemetry(base + row, active)["native_join_drain_wait"],
                         {"calls": 3, "instructions": 63})
        for invalid in ("", row + row, row.replace("calls=3", "calls=0"),
                        row.replace("instructions=63", "instructions=0"), row.replace("on", "off")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, subject.FLAGS)

    def test_native_environment_requires_distinct_coverage(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        base += "Native strip bridge: jobs=20 packets=20 quadwords=600\n"
        active = dict(subject.FLAGS, TAKEDOWN_NATIVE_ENVIRONMENT_CASE="local.b3geo")
        row = "Native strip families: specular=0 supplied-alpha=0 two-sided=0 two-sided-specular=0 environment=20\n"
        self.assertEqual(subject.performance_telemetry(base + row, active)["native_strip_families"]["environment"], 20)
        for invalid in ("", row + row, row.replace("environment=20", "environment=19"),
                        row.replace("specular=0 supplied", "specular=20 supplied").replace("environment=20", "environment=0")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE="local.b3geo"))

    def test_native_execution_requires_every_admitted_job_and_no_reference_pairs(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        base += "Native strip bridge: jobs=20 packets=15 quadwords=600\n"
        active = dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE="local.b3geo", TAKEDOWN_NATIVE_STRIP_EXECUTE="1")
        row = "Native strip execution: jobs=20 reference-pairs=0\n"
        self.assertEqual(subject.performance_telemetry(base + row, active)["native_strip_execution_jobs"], 20)
        for invalid in ("", row + row, row.replace("jobs=20", "jobs=19"), row.replace("pairs=0", "pairs=1")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, dict(active, TAKEDOWN_NATIVE_STRIP_EXECUTE="0"))
        with tempfile.TemporaryDirectory() as temporary:
            args = subject.parse_args(["--output", temporary, "--route", "time-attack"])
            with mock.patch.object(subject.subprocess, "Popen") as launch, self.assertRaisesRegex(ValueError, "requires its fixture"):
                subject.run_arm(args, args.binary, Path(temporary) / "arm", TIMELINE, {"TAKEDOWN_NATIVE_STRIP_EXECUTE": "1"}, mock.Mock())
            launch.assert_not_called()

    def test_poll_diagnostic_cannot_authenticate_as_strict_replay(self):
        diagnostic = replay_log().replace("replay validated", "poll diagnostic recorded")
        subject.completed_identity(diagnostic, 100, TIMELINE, poll_diagnostic=True)
        with self.assertRaises(ValueError):
            subject.completed_identity(diagnostic, 100, TIMELINE)
        with self.assertRaises(ValueError):
            subject.completed_identity(replay_log(), 100, TIMELINE, poll_diagnostic=True)
        with self.assertRaises(ValueError):
            subject.completed_identity(diagnostic + "Guest input timeline: replay validated polls=7 steps=300 cycles=500\n",
                                       100, TIMELINE, poll_diagnostic=True)
        with self.assertRaises(ValueError):
            subject.completed_identity(diagnostic.replace("healthy=1", "healthy=0"), 100, TIMELINE, poll_diagnostic=True)

    def test_poll_diagnostic_preserves_inputs_and_marker_steps(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, actual = Path(temporary) / "original.b3ir", Path(temporary) / "retimed.b3ir"
            source.write_text("B3IR 2\nS 2 10 100 64 -12 0\nA 20 200\nS 3 30 300 0 0 0\nC 4 40 400\n")
            original = subject.timeline_identity(source)
            retimed = "B3IR 2\nS 2 11 110 64 -12 0\nS 3 19 190 0 0 0\nA 20 220\nC 5 40 440\n"
            actual.write_text(retimed)
            self.assertEqual(subject.retimed_timeline_identity(original, actual)["end"], [5, 40, 440])
            for malformed in (retimed.replace("S 2", "S 1"), retimed.replace("64 -12", "64 -13"),
                              retimed.replace("A 20", "A 21"), retimed.replace("C 5 40", "C 5 41"),
                              retimed.replace("S 2 11", "S 2 011"), retimed.replace("S 3 19 190 0 0 0\n", "")):
                actual.write_text(malformed)
                with self.assertRaises(ValueError):
                    subject.retimed_timeline_identity(original, actual)
            actual.write_text(retimed)
            source.write_text(retimed)
            with self.assertRaisesRegex(ValueError, "original replay input changed"):
                subject.retimed_timeline_identity(original, actual)

    def test_poll_diagnostic_requires_replay_and_new_output(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            subject.parse_args(["--output", "unused", "--route", "boot", "--poll-input-diagnostic"])
        args = subject.parse_args(["--output", "unused", "--route", "time-attack", "--poll-input-diagnostic"])
        args.steps = 100
        _, command = subject.commands(args, args.binary, Path("arm"), TIMELINE)
        self.assertEqual(command[command.index("--guest-input-poll-diagnostic") + 1], str(Path("arm/retimed.b3ir")))
        with mock.patch.object(subject.subprocess, "Popen") as launch, self.assertRaises(ValueError):
            subject.run_arm(args, args.binary, Path("arm"), None, {}, mock.Mock())
        launch.assert_not_called()

    def test_successor_capture_rejects_missing_native_fixture_before_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = subject.parse_args(["--output", temporary, "--route", "time-attack", "--geometry-successors"])
            with mock.patch.object(subject.subprocess, "Popen") as launch:
                with self.assertRaisesRegex(ValueError, "successor capture requires"):
                    subject.run_arm(args, args.binary, Path(temporary) / "arm", TIMELINE, {}, mock.Mock())
                launch.assert_not_called()

    def test_fallback_capture_rejects_invalid_or_conflicting_modes(self):
        for options in (["--geometry-fallback-entry", "-1"], ["--geometry-fallback-entry", "2048"],
                        ["--geometry-fallback-entry", "668", "--route", "boot"],
                        ["--geometry-fallback-entry", "668", "--geometry-successors"],
                        ["--geometry-fallback-entry", "668", "--geometry-capture", "--vu1-profile"]):
            with self.subTest(options=options), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                subject.parse_args(["--output", "unused", *options])
        args = subject.parse_args(["--output", "unused", "--geometry-fallback-entry", "0"])
        self.assertEqual(args.geometry_fallback_entry, 0)

    def test_fallback_capture_requires_native_execution_before_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = subject.parse_args(["--output", temporary, "--route", "time-attack", "--geometry-fallback-entry", "668"])
            with mock.patch.object(subject.subprocess, "Popen") as launch:
                with self.assertRaisesRegex(ValueError, "fallback capture requires"):
                    subject.run_arm(args, args.binary, Path(temporary) / "arm", TIMELINE, {}, mock.Mock())
                launch.assert_not_called()

    def test_native_strip_scratch_reset_requires_all_admitted_jobs(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        base += "Native strip bridge: jobs=20 packets=15 quadwords=600\n"
        active = dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE="local.b3geo",
                      TAKEDOWN_NATIVE_STRIP_RESET_SCRATCH="1")
        row = "Native strip scratch reset: jobs=20\n"
        self.assertEqual(subject.performance_telemetry(base + row, active)["native_strip_scratch_reset_jobs"], 20)
        for mode in subject.SCRATCH_DIAGNOSTIC_MODES - {"0"}:
            self.assertEqual(subject.override("TAKEDOWN_NATIVE_STRIP_RESET_SCRATCH=" + mode)[1], mode)
            selected = dict(active, TAKEDOWN_NATIVE_STRIP_RESET_SCRATCH=mode)
            self.assertEqual(subject.performance_telemetry(base + row, selected)["native_strip_scratch_reset_jobs"], 20)
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base, selected)
        with self.assertRaises(argparse.ArgumentTypeError):
            subject.override("TAKEDOWN_NATIVE_STRIP_RESET_SCRATCH=typo")
        for invalid in ("", row + row, row.replace("20", "0"), row.replace("20", "19")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, dict(active, TAKEDOWN_NATIVE_STRIP_RESET_SCRATCH="0"))

    def test_native_strip_bridge_requires_actual_submission(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        active = dict(subject.FLAGS, TAKEDOWN_NATIVE_STRIP_SUBMISSION_CASE="local.b3geo")
        row = "Native strip bridge: jobs=20 packets=15 quadwords=600 (reference state execution retained; diagnostic only)\n"
        self.assertEqual(subject.performance_telemetry(base + row, active)["native_strip_submission"]["jobs"], 20)
        for invalid in ("", row + row, row.replace("jobs=20", "jobs=0"), row.replace("packets=15", "packets=0")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, subject.FLAGS)

    def test_native_strip_fixture_is_explicit_and_isolated(self):
        for key in subject.NATIVE_STRIP_CASES:
            with tempfile.TemporaryDirectory() as directory:
                fixture = Path(directory) / "native.b3geo"
                fixture.write_bytes(b"local fixture placeholder")
                self.assertEqual(subject.override(key + "=" + str(fixture)), (key, str(fixture.resolve())))
                self.assertEqual(subject.file_identity(str(fixture)), subject.file_identity(fixture))
                self.assertNotIn(key, subject.isolated_environment({key: str(fixture)}, Path(directory), {}))
                self.assertEqual(subject.isolated_environment({}, Path(directory), {key: str(fixture)})[key], str(fixture))
                for invalid in (directory, str(fixture.with_suffix('.missing')), ""):
                    with self.assertRaises(argparse.ArgumentTypeError):
                        subject.override(key + "=" + invalid)

    def test_geometry_capture_requires_profiled_replay(self):
        with contextlib.redirect_stderr(io.StringIO()):
            for options in ([], ["--vu1-profile", "--route", "boot"]):
                with self.assertRaises(SystemExit):
                    subject.parse_args(["--output", "unused", "--geometry-capture", *options])
        args = subject.parse_args(["--output", "unused", "--geometry-capture", "--vu1-profile", "--route", "time-attack"])
        self.assertTrue(args.geometry_capture)

    def test_render_state_packet_requires_actual_calls(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        active = dict(subject.FLAGS, TAKEDOWN_NATIVE_RENDER_STATE_PACKET="1")
        row = "Native render-state packet: active=1 calls=20\n"
        subject.performance_telemetry(base + row, active)
        for invalid in ("", row + row, row.replace("calls=20", "calls=0"),
                        row.replace("active=1", "active=0")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, active)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, subject.FLAGS)

    def test_module_registration_uses_presence_not_zero(self):
        flag = "TAKEDOWN_DISABLE_R5900_BLOCK_MODULE_REGISTRATION"
        for overrides in ({}, {flag: "default"}):
            self.assertNotIn(flag, subject.isolated_environment({flag: "1"}, Path("arm"), overrides))
        self.assertEqual(subject.isolated_environment({}, Path("arm"), {flag: "1"})[flag], "1")
        with self.assertRaises(argparse.ArgumentTypeError):
            subject.override(flag + "=0")
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        disabled = dict(subject.FLAGS, **{flag: "1"})
        row = "Compiled C++ blocks: registered=0 dispatches=0 retired=0\n"
        self.assertEqual(subject.performance_telemetry(base + row, disabled)["compiled_blocks"]["registered"], 0)
        for invalid in ("", row + row, row.replace("registered=0", "registered=4")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, disabled)
        active = "Compiled C++ blocks: registered=4 dispatches=8 retired=16\n"
        self.assertEqual(subject.performance_telemetry(base + active, subject.FLAGS,
                         require_compiled_blocks=True)["compiled_blocks"]["retired"], 16)
        for invalid in ("", row, active.replace("dispatches=8", "dispatches=0")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, subject.FLAGS, require_compiled_blocks=True)

    def test_requested_dxgi_cannot_silently_fall_back(self):
        enabled = {"TAKEDOWN_GS_DXGI_PRESENT": "1"}
        line = "Initialized DXGI interop swapchain!\nDXGI vsync presentation: on."
        self.assertEqual(subject.presentation_backend_telemetry(line, enabled), "DXGI")
        self.assertEqual(subject.presentation_backend_telemetry("", {}), "Vulkan WSI")
        for text, env in (("", enabled), (line, {}),
                          (line.replace(": on.", ": off."), enabled),
                          ("Initialized DXGI interop swapchain!", enabled)):
            with self.assertRaises(ValueError):
                subject.presentation_backend_telemetry(text, env)

    def test_normal_presentation_retains_raw_fields_without_extra_presents(self):
        rows = lambda ids: [argparse.Namespace(draw_index=i+1, raw_vsync_index=value)
                            for i, value in enumerate(ids)]
        source, present = rows([1, 2, 3, 4]), rows([1, 3])
        subject.validate_frame_policy(source, present, present, "normal")
        for submitted, interactive, policy in ((present, present, "all fields"),
                (source, present, "normal"), (rows([5]), rows([5]), "normal")):
            with self.assertRaises(ValueError):
                subject.validate_frame_policy(source, submitted, interactive, policy)
        enabled = {"TAKEDOWN_GS_NORMAL_PRESENT_POLICY": "1"}
        self.assertEqual(subject.presentation_policy_telemetry(
            "Completion presentation policy: normal.", enabled), "normal")
        for text in ("", "Completion presentation policy: all fields."):
            with self.assertRaises(ValueError):
                subject.presentation_policy_telemetry(text, enabled)

    def test_display_request_restores_previous_state_on_failure(self):
        isolation = subject.WindowsIsolation.__new__(subject.WindowsIsolation)
        isolation.kernel = mock.Mock()
        isolation.kernel.SetThreadExecutionState.side_effect = [0x80000001, 0x80000002]
        with self.assertRaisesRegex(RuntimeError, "replay failed"):
            with isolation.display_awake() as request:
                self.assertEqual(request["previous_flags"], 0x80000001)
                raise RuntimeError("replay failed")
        self.assertEqual(isolation.kernel.SetThreadExecutionState.call_args_list,
                         [mock.call(0x80000002), mock.call(0x80000001)])

    def test_presentation_wait_requires_matching_renderer_readback(self):
        enabled = {"TAKEDOWN_GS_BOUNDED_PRESENT_WAIT": "1"}
        line = "[INFO]: Presentation latency wait bound: 2 ms.\n"
        self.assertEqual(subject.presentation_wait_telemetry(line, enabled)["maximum_wait_ns"], 2_000_000)
        self.assertIsNone(subject.presentation_wait_telemetry("", {}))
        for text, env in (("", enabled), (line, {}), (line + line, enabled),
                          ("Presentation latency wait bound: unlimited.", enabled)):
            with self.assertRaisesRegex(ValueError, "not confirmed"):
                subject.presentation_wait_telemetry(text, env)

    def test_three_second_cadence_expires_history_and_counts_silence(self):
        times = [i * 2_500_000 for i in range(1, 1201)] # 400 FPS for 3 seconds
        times += [3_000_000_000 + i * 200_000_000 for i in range(1, 16)]
        report = subject.three_second_cadence(times, 0, 6_000_000_000)
        self.assertEqual(report["samples"][0]["fps"], 400)
        self.assertEqual(report["last_fps"], 5)
        self.assertFalse(report["warmup_only"])
        self.assertEqual(subject.three_second_cadence(times, 0, 9_000_000_000)["last_fps"], 0)

    def test_three_second_cadence_warmup_excludes_events_outside_measurement(self):
        report = subject.three_second_cadence([0, 1_000_000_000, 1_500_000_000, 2_000_000_000],
                                             1_000_000_000, 1_500_000_000)
        self.assertEqual(report["last_fps"], 2)
        self.assertTrue(report["warmup_only"])
        with self.assertRaises(ValueError):
            subject.three_second_cadence([], 4, 4)

    def test_cli_validates_finite_deadline_and_toggle_allowlist(self):
        for options in (("--timeout", "nan"), ("--timeout", "inf"), ("--timeout", "0"),
                        ("--visible-renderer",),
                        ("--pairs", "0"), ("--steps", "0"),
                        ("--candidate-env", "TAKEDOWN_TEST_OFFSCREEN=0"),
                        ("--candidate-env", "TAKEDOWN_UNKNOWN=1"),
                        ("--baseline-env", "SDL_VIDEODRIVER=other"),
                        ("--route", "boot", "--timeline", "route.b3ir")):
            with self.subTest(options=options), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                subject.parse_args(["--output", "out", *options])
        args = subject.parse_args(["--output", "out", "--candidate-env", "TAKEDOWN_JOIN_PUMP_CALLBACK_REGION=1"])
        self.assertEqual(args.candidate_env, [("TAKEDOWN_JOIN_PUMP_CALLBACK_REGION", "1")])
        self.assertEqual(subject.FLAGS["TAKEDOWN_JOIN_PUMP_CALLBACK_REGION"], "0")
        self.assertEqual(subject.FLAGS["TAKEDOWN_JOIN_PUMP_CALLBACK_TAIL_REGION"], "0")

    def test_direct_arm_cannot_bypass_offscreen_policy(self):
        with mock.patch.object(subject.subprocess, "Popen") as launch:
            with self.assertRaisesRegex(ValueError, "off-screen muted"):
                subject.run_arm(argparse.Namespace(visible_renderer=True), Path("game.exe"),
                                Path("unused"), None, {}, mock.Mock())
            launch.assert_not_called()

    def test_native_sampler_requires_an_authenticated_window(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            subject.parse_args(["--output", "out", "--route", "boot", "--native-sampler", "sampler.exe"])
        args = subject.parse_args(["--output", "out", "--native-sampler", "sampler.exe"])
        self.assertEqual(args.native_sampler, Path("sampler.exe"))

    def test_simd_activation_must_match_the_requested_arm(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=2 blocked-ns=4000000 final-queued=1024}\n"
        enabled = dict(subject.FLAGS, TAKEDOWN_VU1_SIMD_ARITHMETIC="1")
        subject.performance_telemetry(base + "VU1 SIMD arithmetic: on\n", enabled)
        for log in (base, base + "VU1 SIMD arithmetic: off\n",
                    base + "VU1 SIMD arithmetic: on\nVU1 SIMD arithmetic: on\n"):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(log, enabled)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + "VU1 SIMD arithmetic: on\n", subject.FLAGS)

    def test_replay_command_has_no_conflicting_live_input_or_unbounded_loop(self):
        args = subject.parse_args(["--output", "out", "--steps", "100", "--profile", "--capture-stream"])
        renderer, game = subject.commands(args, Path("candidate.exe"), Path("out/run"), TIMELINE)
        self.assertNotIn("--parallel-gs-input", game)
        self.assertNotIn("--input-fifo", renderer)
        self.assertNotIn("--run-until-close", game)
        self.assertNotIn("--frame-timestamps", renderer)

        self.assertIn("--guest-input-replay", game)
        self.assertIn("--guest-work-measurement-steps", game)
        self.assertIn("--profile-guest-work-window", game)
        self.assertIn("--parallel-gs-live-capture", game)
        self.assertTrue(renderer[1].startswith("\\\\.\\pipe\\takedown-perf-"))
        self.assertEqual(game[game.index("--parallel-gs-live") + 1], renderer[1])
        self.assertEqual(game[game.index("--parallel-gs-completion") + 1], renderer[renderer.index("--completion-fifo") + 1])
        self.assertNotEqual(renderer[1], subject.commands(args, Path("candidate.exe"), Path("out/run"), TIMELINE)[0][1])

    def test_device_profile_requires_scoped_replay_and_emits_counter_option(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            subject.parse_args(["--output", "out", "--graphics-profile"])
        args = subject.parse_args(["--output", "out", "--steps", "100", "--profile", "--graphics-profile"])
        _, game = subject.commands(args, Path("candidate.exe"), Path("out/run"), TIMELINE)
        self.assertIn("--ee-graphics-service-profile", game)
        self.assertIn("--profile-guest-work-window", game)

    def test_boot_command_has_explicit_fixed_steps(self):
        args = subject.parse_args(["--output", "out", "--route", "boot", "--steps", "300"])
        renderer, game = subject.commands(args, Path("candidate.exe"), Path("out/run"), None)
        self.assertEqual(game[:4], ["candidate.exe", "boot-entry", str(args.elf), "300"])
        self.assertNotIn("--guest-input-replay", game)
        self.assertNotIn("--ee-pc-profile", game)

    def test_environment_removes_inherited_experiments_and_desktop_settings(self):
        env = subject.isolated_environment({"PATH": "tools", "TAKEDOWN_UNKNOWN": "1", "SDL_VIDEODRIVER": "x11",
                                           "SDL_AUDIO_DRIVER": "disk", "XDG_CONFIG_HOME": "personal", "DISPLAY": ":0",
                                           "WAYLAND_DISPLAY": "wayland-0"}, Path("arm"),
                                          {"TAKEDOWN_JOIN_PUMP_CALLBACK_REGION": "1"})
        self.assertNotIn("TAKEDOWN_UNKNOWN", env)
        self.assertNotIn("DISPLAY", env)
        self.assertNotIn("WAYLAND_DISPLAY", env)
        self.assertNotIn("SDL_AUDIO_DRIVER", env)
        self.assertEqual(env["TAKEDOWN_TEST_OFFSCREEN"], "1")
        self.assertEqual(env["SDL_VIDEODRIVER"], "windows")
        self.assertEqual(env["TAKEDOWN_JOIN_PUMP_CALLBACK_REGION"], "1")
        self.assertEqual(env["XDG_CONFIG_HOME"], str(Path("arm/config")))

    def test_runtime_default_check_really_removes_flags(self):
        flags={key: "default" for key in subject.DEFAULT_TEST_FLAGS}
        env=subject.isolated_environment({key: "0" for key in flags},Path("arm"),flags)
        for key in flags:
            self.assertNotIn(key,env)
            self.assertEqual(subject.override(key+"=default"),(key,"default"))
        self.assertEqual(env["TAKEDOWN_TEST_OFFSCREEN"],"1")
        log="Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        log+="audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        log+="VU1 AVX512 arithmetic: on\nminimal-execution-dispatches=50\n"
        subject.performance_telemetry(log,env)
        subject.performance_telemetry(log.replace("arithmetic: on","arithmetic: off"),env)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(log.replace("dispatches=50","dispatches=0"),env)

    def test_timeline_uses_local_input_not_historical_output_proof(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "route.b3ir"
            path.write_text("B3IR 2\nS 4 100 300 16 0 0\nA 200 350\nC 7 300 500\n")
            result = subject.timeline_identity(path)
            self.assertEqual(result["start"], TIMELINE["start"])
            self.assertEqual(result["end"], TIMELINE["end"])
            self.assertEqual(result["steps"], 100)
            self.assertEqual(result["sha256"], subject.sha256(path))
            for contents in ("B3IR 1\nA 200 350\nC 7 300 500\n", "B3IR 2\nA 200 350\nC 7 200 500\n",
                             "B3IR 2\nA 200 350\nA 201 351\nC 7 300 500\n"):
                path.write_text(contents)
                with self.assertRaises(ValueError):
                    subject.timeline_identity(path)

    def test_boot_admission_requires_exact_work_and_healthy_gpu_drain(self):
        identity, seconds = subject.completed_identity(boot_log(), 300)
        self.assertEqual(identity["retired"], 300)
        self.assertEqual(identity["audio"]["fnv"], "0xabcd")
        self.assertIsNone(seconds)
        for log in (boot_log().replace("complete=2/2", "complete=2/1"),
                    boot_log().replace("clean=1", "clean=0"),
                    boot_log().replace("bytes=256", "bytes=255"),
                    boot_log().replace("software=disabled", "software=enabled"),
                    boot_log().replace("healthy=1", "healthy=1 healthy=1"),
                    boot_log().replace("EE instructions: 290", "EE instructions: 291"),
                    boot_log().replace("configured step limit reached (normal completion)", "fault")):
            with self.subTest(log=log), self.assertRaises(ValueError):
                subject.completed_identity(log, 300)

    def test_replay_measures_only_a_to_c_and_compares_nonclock_identity(self):
        identity, seconds = subject.completed_identity(replay_log(), 100, TIMELINE)
        self.assertEqual(seconds, 2)
        self.assertNotIn("monotonic-ns", identity["window"]["start"])
        altered_clock = replay_log().replace("2000000000}", "3000000000}").replace("12000000000", "13000000000")
        second_identity, second_seconds = subject.completed_identity(altered_clock, 100, TIMELINE)
        self.assertEqual(second_seconds, 3)
        self.assertEqual(identity, second_identity)
        for log in (replay_log().replace("polls=7", "polls=8"),
                    replay_log().replace("requested-steps=100", "requested-steps=99"),
                    replay_log().replace("audio-frames=40", "audio-frames=41"),
                    replay_log().replace("Guest-work start: steps=200", "Guest-work start: steps=199"),
                    replay_log().replace("monotonic-ns=2000000000", "monotonic-ns=0"),
                    replay_log() + replay_log()):
            with self.subTest(log=log), self.assertRaises(ValueError):
                subject.completed_identity(log, 100, TIMELINE)

    def test_summary_matches_reversed_pair_order_and_reports_noise(self):
        rows = [{"pair": pair, "arm": arm, "seconds": seconds} for pair, arm, seconds in
                ((1, "baseline", 10), (1, "candidate", 8), (2, "candidate", 9), (2, "baseline", 10))]
        summary = subject.summarize(rows)
        self.assertEqual(summary["candidate_wins"], 2)
        self.assertAlmostEqual(summary["paired_percent_median"], -15)
        self.assertAlmostEqual(summary["paired_percent_mad"], 5)
        self.assertEqual(summary, subject.summarize(rows[::-1]))
        with self.assertRaises(ValueError):
            subject.summarize(rows[:1])

    def test_candidate_first_rerun_preserves_arm_labels_and_alternation(self):
        default = subject.parse_args(["--output", "out"])
        rerun = subject.parse_args(["--output", "out", "--first-arm", "candidate"])
        self.assertEqual([subject.pair_order(pair, default.first_arm) for pair in (1, 2, 3)],
                         [("baseline", "candidate"), ("candidate", "baseline"), ("baseline", "candidate")])
        self.assertEqual([subject.pair_order(pair, rerun.first_arm) for pair in (1, 2, 3)],
                         [("candidate", "baseline"), ("baseline", "candidate"), ("candidate", "baseline")])

    def test_telemetry_requires_requested_callback_to_execute(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        audio = "audio-master-clock={blocks=2 blocked-ns=4000000 final-queued=1024}\n"
        telemetry = subject.performance_telemetry(base + audio, subject.FLAGS)
        self.assertEqual(telemetry["audio_master_clock"]["blocked_ns"], 4000000)
        enabled = dict(subject.FLAGS, TAKEDOWN_JOIN_PUMP_CALLBACK_REGION="1",
                       TAKEDOWN_JOIN_PUMP_CALLBACK_TAIL_REGION="1")
        active = "Join-pump region: requested=yes status=active-callback-tail calls=5 inert-dmac=5 retired=100\n"
        self.assertEqual(subject.performance_telemetry(active + audio, enabled)["join_pump_region"]["retired"], 100)
        for text in (base + audio, active.replace("calls=5", "calls=0") + audio,
                     active.replace("active-callback-tail", "elf-callback-source-rejected") + audio,
                     active, active + audio + audio):
            with self.subTest(text=text), self.assertRaises(ValueError):
                subject.performance_telemetry(text, enabled)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(active + audio, subject.FLAGS)

    def test_timeout_cleans_both_owned_processes_and_retains_failure(self):
        args = subject.parse_args(["--output", "out", "--route", "boot", "--steps", "300", "--timeout", "1"])
        renderer, game = mock.Mock(), mock.Mock()
        for process in (renderer, game):
            process.poll.return_value = None
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(subject.subprocess, "Popen", side_effect=[renderer, game]), \
                mock.patch.object(subject.subprocess, "CREATE_NO_WINDOW", 0, create=True), \
                mock.patch.object(subject.time, "perf_counter", side_effect=[0, 2, 2]):
            directory = Path(temporary) / "arm"
            with self.assertRaises(TimeoutError):
                subject.run_arm(args, Path("candidate.exe"), directory, None, {}, mock.Mock())
            self.assertTrue((directory / "failure.json").is_file())
            settings = (directory / "config/takedown-recomp/settings.ini").read_text()
            self.assertIn("master_volume = 0.0", settings)
            self.assertIn("presentation_fps_limit = unlimited", settings)
            renderer.terminate.assert_called_once()
            game.terminate.assert_called_once()
            renderer.wait.assert_called_once_with(timeout=5)
            game.wait.assert_called_once_with(timeout=5)

    def test_sampler_uses_game_environment_and_early_failure_cleans_children(self):
        args = subject.parse_args(["--output", "out", "--route", "boot", "--steps", "300"])
        # Exercise process startup independently of replay input parsing.
        args.native_sampler = Path("sampler.exe")
        renderer, game, sampler = mock.Mock(), mock.Mock(), mock.Mock()
        renderer.poll.return_value = game.poll.return_value = None
        sampler.poll.return_value = sampler.returncode = 0xc0000135
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(subject.subprocess, "Popen", side_effect=[renderer, game, sampler]) as launch, \
                mock.patch.object(subject.subprocess, "CREATE_NO_WINDOW", 0, create=True), \
                mock.patch.object(subject.time, "perf_counter", side_effect=[0, .01]):
            directory = Path(temporary) / "arm"
            with self.assertRaisesRegex(ValueError, "native sampler exited before completing"):
                subject.run_arm(args, Path("candidate.exe"), directory, None, {}, mock.Mock())
            self.assertEqual(launch.call_args_list[2].kwargs["env"], launch.call_args_list[1].kwargs["env"])
            failure = json.loads((directory / "failure.json").read_text())
            self.assertEqual(failure["elapsed_seconds"], .01)
            renderer.terminate.assert_called_once()
            game.terminate.assert_called_once()
            sampler.terminate.assert_not_called()

    def test_window_host_accounting_is_separate_and_missing_is_not_zero(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=2 blocked-ns=4000000 final-queued=1024}\n"
        row = "  Guest-work host: thread-cpu-ns=120 process-cpu-ns=140 thread-cycles=unavailable audio-wait-ns=4 gs-write-ns=5\n"
        host = subject.performance_telemetry(base + row, subject.FLAGS)["guest_window_host"]
        self.assertEqual(host["thread-cpu-ns"], 120)
        self.assertIsNone(host["thread-cycles"])
        self.assertIsNone(subject.performance_telemetry(base, subject.FLAGS)["guest_window_host"])
        for invalid in (row + row, row.replace(" gs-write-ns=5", ""), row.replace("120", "-120")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, subject.FLAGS)

    def test_idle_code_cache_must_report_actual_reuse(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        env = dict(subject.FLAGS, TAKEDOWN_JOIN_PUMP_IDLE_CODE_CACHE="1")
        row = "Join-pump idle code cache: on hits=4 misses=1\n"
        self.assertEqual(subject.performance_telemetry(base + row, env)["idle_code_cache"]["hits"], 4)
        self.assertIsNone(subject.performance_telemetry(base, subject.FLAGS)["idle_code_cache"])
        for invalid in ("", row + row, row.replace("hits=4", "hits=0"), row.replace("on", "off")):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, env)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base + row, subject.FLAGS)

    def test_native_control_coverage_preserves_old_logs_and_rejects_overcount(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        row = "VU1 native schedule window: total-pairs=100 native-pairs=60 transfer-blocks=20"
        old = subject.performance_telemetry(base+row+"\n",subject.FLAGS)["native_schedule_window"]
        self.assertEqual(old["control_pairs"],0)
        new = subject.performance_telemetry(base+row+" control-pairs=30\n",subject.FLAGS)["native_schedule_window"]
        self.assertEqual(new["control_pairs"],30)
        fast = subject.performance_telemetry(base+row+" control-pairs=30 control-fast-pairs=20\n",subject.FLAGS)["native_schedule_window"]
        self.assertEqual(fast["control_fast_pairs"],20)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base+row+" control-pairs=30 control-fast-pairs=31\n",subject.FLAGS)
        with self.assertRaises(ValueError):
            subject.performance_telemetry(base+row+" control-pairs=41\n",subject.FLAGS)

    def test_experimental_paths_must_be_exercised(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        cases = (
            ("TAKEDOWN_NATIVE_STATIC_OBJECT", "Native static object: active=1 calls=50\n"),
            ("TAKEDOWN_VU1_NATIVE_SIGNED_BRANCHES", "VU1 native signed branch module: on.\n"),
            ("TAKEDOWN_VU1_INLINE_BLOCKS", "local-inline-block-pairs=50\n"),
            ("TAKEDOWN_SCALAR_COLD_OBSERVERS", "Scalar observer bypass: on steps=50 observers-configured=no\n"),
            ("TAKEDOWN_VU1_MINIMAL_COUNTERS", "minimal-execution-dispatches=50\n"),
            ("TAKEDOWN_VU1_NATIVE_SCHEDULE", "native-scheduled-pairs=50\n"),
            ("TAKEDOWN_VU1_NATIVE_TRANSFER_SCHEDULE", "native-transfer-blocks=50\n"),
            ("TAKEDOWN_VU1_NATIVE_FLAG_LIVENESS", "native-flag-liveness-blocks=50\n"),
            ("TAKEDOWN_VU1_WORKER_CONSUME", "vu1-worker-differential: observed=3 result_mismatches=0 state_mismatches=0 vumem_mismatches=0 deferred_admitted=2 consume_fences(next_launch=2)\n"),
        )
        for flag, row in cases:
            enabled = dict(subject.FLAGS, **{flag: "1"})
            subject.performance_telemetry(base + row, enabled)
            with self.subTest(flag=flag), self.assertRaises(ValueError):
                subject.performance_telemetry(base, enabled)
            with self.subTest(flag=flag), self.assertRaises(ValueError):
                subject.performance_telemetry(base + row, subject.FLAGS)

    def test_vu_profile_can_decline_native_schedules_only_in_scoped_window(self):
        log = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        log += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        log += "native-scheduled-pairs=50\n"
        log += "VU1 native schedule window: total-pairs=100 native-pairs=0 transfer-blocks=0\n"
        env = dict(subject.FLAGS, TAKEDOWN_VU1_NATIVE_SCHEDULE="1")
        with self.assertRaisesRegex(ValueError, "inside the measured window"):
            subject.performance_telemetry(log, env)
        subject.performance_telemetry(log, env, vu1_profile=True)

    def test_scalar_observer_bypass_must_preserve_configured_observers(self):
        base = "Join-pump region: requested=no status=disabled calls=0 inert-dmac=0 retired=0\n"
        base += "audio-master-clock={blocks=0 blocked-ns=0 final-queued=0}\n"
        env = dict(subject.FLAGS, TAKEDOWN_SCALAR_COLD_OBSERVERS="1")
        row = "Scalar observer bypass: on steps=0 observers-configured=yes\n"
        subject.performance_telemetry(base + row, env)
        for invalid in (row.replace("steps=0", "steps=1"), row.replace("=yes", "=no"), row + row):
            with self.assertRaises(ValueError):
                subject.performance_telemetry(base + invalid, env)


if __name__ == "__main__":
    unittest.main()
