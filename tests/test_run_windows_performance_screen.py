from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "run_windows_performance_screen.py"
SPEC = importlib.util.spec_from_file_location("windows_performance_screen", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
screen = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(screen)


def replay_log(*, measurement_cycles: int = 1500, end_steps: int = 1500,
               end_cycles: int = 3500, timeline_steps: int = 1500,
               timeline_cycles: int = 3500, end_vblanks: int = 12,
               end_audio_frames: int = 48, end_monotonic_ns: int = 600) -> str:
    return f"""\
Guest-work measurement: completed scope=EE instruction-boundary fixed guest work normal-window-completion=yes requested-steps=500 actual={{steps=500 ee-cycles={measurement_cycles} vblank-starts=2 audio-frames=8 monotonic-ns=500}}
  Guest-work start:    steps=1000 ee-cycles=2000 vblank-starts=10 audio-frames=40 audio-fnv1a64-cumulative=0x0000000000000001 monotonic-ns=100
  Guest-work end:      steps={end_steps} ee-cycles={end_cycles} vblank-starts={end_vblanks} audio-frames={end_audio_frames} audio-fnv1a64-cumulative=0x0000000000000002 monotonic-ns={end_monotonic_ns}
Guest input timeline: replay validated polls=3 steps={timeline_steps} cycles={timeline_cycles}
  EE instructions:     1500
  DMAC callbacks:      12
  VBlank starts:       12
  emulated EE cycles:  {end_cycles}
  stop reason:         guest-work measurement exact step budget reached (normal window completion)
  next PC:             0x00000200
    spu2-render={{frames={end_audio_frames} non-silent=20 output=disabled queued=0 no-device-discard=0 underruns=0 overflow-resyncs=0 overflow-dropped=0 fnv=0x2}}
"""


class WindowsPerformanceScreenTests(unittest.TestCase):
    def test_replay_command_is_headless_and_uses_runtime_pipe(self) -> None:
        command, raw_path = screen.build_command(
            Path("candidate.exe"), Path("SLUS_210.50"), Path("run"), 500,
            Path("garage.b3ir"), r"\\.\pipe\b3-perf-test")

        self.assertIn("--window", command)
        self.assertIn("--parallel-gs-only", command)
        self.assertIn("--parallel-gs-live", command)
        self.assertIn(r"\\.\pipe\b3-perf-test", command)
        self.assertIn("--headless-movie-eos", command)
        self.assertIn("--guest-work-measurement-steps", command)
        self.assertIn("--save-data-root", command)
        self.assertNotIn("--parallel-gs-stream", command)
        self.assertEqual(raw_path, Path("run") / "gs.raw")

    def test_fixed_work_command_uses_raw_file_without_window(self) -> None:
        command, raw_path = screen.build_command(
            Path("baseline.exe"), Path("SLUS_210.50"), Path("run"), 500, None,
            r"\\.\pipe\unused")

        self.assertIn("500", command)
        self.assertIn("--parallel-gs-stream", command)
        self.assertNotIn("--window", command)
        self.assertNotIn("--parallel-gs-live", command)
        self.assertEqual(raw_path, Path("run") / "gs.raw")

    def test_environment_is_isolated_dummy_and_excludes_windowsapps_and_home(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_directory = Path(directory) / "arm"
            fake_path = (
                "C:\\Windows\\System32;"
                "C:\\Users\\sample\\AppData\\Local\\Microsoft\\WindowsApps"
            )
            with patch.dict("os.environ", {
                "PATH": fake_path,
                "SYSTEMDRIVE": "C:",
                "PROGRAMDATA": "C:\\ProgramData",
            }, clear=True):
                environment = screen.build_environment(run_directory)
                self.assertEqual(environment["PATH"], "C:\\Windows\\System32")
                self.assertEqual(environment["SYSTEMDRIVE"], "C:")
                self.assertEqual(environment["PROGRAMDATA"], "C:\\ProgramData")
                self.assertEqual(environment["SDL_VIDEODRIVER"], "dummy")
                self.assertEqual(environment["SDL_AUDIODRIVER"], "dummy")
                self.assertEqual(environment["TAKEDOWN_AUDIO_MASTER_CLOCK"], "0")
                self.assertEqual(environment["TAKEDOWN_AUDIO_TIME_STRETCH"], "0")
                self.assertNotIn("HOME", environment)
                self.assertTrue(environment["APPDATA"].endswith("Roaming"))
                self.assertTrue(Path(environment["LOCALAPPDATA"]).is_dir())
                self.assertTrue(Path(environment["XDG_CONFIG_HOME"]).is_dir())

    def test_replay_projection_requires_exact_snapshot_and_timeline_deltas(self) -> None:
        parsed = screen.parse_run_log(replay_log(), work_steps=500, replay_mode=True)

        projection = parsed["guest_projection"]
        self.assertEqual(projection["validated_replay_polls"], 3)
        self.assertEqual(parsed["measurement_ns"], 500)
        self.assertEqual(projection["measurement"]["ee_cycles"], 1500)
        self.assertEqual(projection["start"]["audio_fnv1a64_cumulative"],
                         "0x0000000000000001")
        self.assertEqual(projection["end"]["audio_fnv1a64_cumulative"],
                         "0x0000000000000002")
        self.assertEqual(projection["next_pc"], "0x00000200")
        self.assertEqual(projection["modeled_audio"]["fnv1a64"], "0x2")

        malformed = (
            replay_log(measurement_cycles=1499),
            replay_log(timeline_steps=1499),
            replay_log(timeline_cycles=3499),
            replay_log(end_steps=1499),
            replay_log(end_cycles=3499),
            replay_log(end_vblanks=11),
            replay_log(end_audio_frames=47),
            replay_log(end_monotonic_ns=599),
        )
        for log in malformed:
            with self.subTest(log=log):
                with self.assertRaises(ValueError):
                    screen.parse_run_log(log, work_steps=500, replay_mode=True)

    def test_replay_requires_completed_measurement_and_validated_timeline(self) -> None:
        for incomplete in (
            replay_log().replace("Guest-work measurement: completed", "Guest-work measurement: incomplete"),
            replay_log().replace("Guest input timeline: replay validated", "Guest input timeline: replay failed"),
        ):
            with self.subTest(incomplete=incomplete):
                with self.assertRaises(ValueError):
                    screen.parse_run_log(incomplete, work_steps=500, replay_mode=True)

    def test_boot_projection_requires_exact_limit_and_includes_audio(self) -> None:
        text = """\
  EE instructions:     500
  DMAC callbacks:      7
  VBlank starts:       2
  emulated EE cycles:  12345
  stop reason:         configured step limit reached (normal completion)
  next PC:             0x00000200
    spu2-render={frames=48 non-silent=20 output=disabled queued=0 no-device-discard=0 underruns=0 overflow-resyncs=0 overflow-dropped=0 fnv=0x1}
"""
        parsed = screen.parse_run_log(text, work_steps=500, replay_mode=False)
        projection = parsed["guest_projection"]
        self.assertEqual(projection["steps"], 500)
        self.assertEqual(projection["dmac_callbacks"], 7)
        self.assertEqual(projection["modeled_audio"]["frames"], 48)
        self.assertEqual(projection["modeled_audio"]["non_silent_frames"], 20)
        self.assertEqual(projection["modeled_audio"]["fnv1a64"], "0x1")

        with self.assertRaises(ValueError):
            screen.parse_run_log(
                text.replace("configured step limit reached (normal completion)",
                             "outside mapped guest memory"),
                work_steps=500, replay_mode=False)

    def test_pair_order_alternates(self) -> None:
        self.assertEqual(screen._pair_order(0), ["baseline", "candidate"])
        self.assertEqual(screen._pair_order(1), ["candidate", "baseline"])
        self.assertEqual(screen._pair_order(2), ["baseline", "candidate"])

    def test_timing_summary_uses_only_measured_pairs_and_reports_mad(self) -> None:
        warmups = [{"timing": {"candidate_reduction_percent": 99.0}}]
        measured = [
            {"parity": {"passed": True},
             "timing": {"candidate_reduction_percent": value}}
            for value in (10.0, 12.0, 14.0)
        ]

        summary = screen._summarize_report(
            {"warmups": warmups, "pairs": measured}, requested_pairs=3,
            timing_basis="process-launch-to-exit-ns")

        self.assertEqual(summary["completed_pairs"], 3)
        self.assertTrue(summary["all_pairs_completed"])
        self.assertTrue(summary["all_parity_passed"])
        self.assertEqual(summary["median_candidate_reduction_percent"], 12.0)
        self.assertEqual(summary["candidate_reduction_mad_percentage_points"], 2.0)

        partial = screen._summarize_pairs(
            measured[:2], requested_pairs=3, timing_basis="process-launch-to-exit-ns")
        self.assertFalse(partial["all_pairs_completed"])
        self.assertFalse(partial["all_parity_passed"])


if __name__ == "__main__":
    unittest.main()
