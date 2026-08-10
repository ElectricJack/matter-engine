#!/usr/bin/env python3
"""Focused regression tests for atmosphere presentation log association."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import atmosphere_presentation_metrics as metrics


class CaptureParsingTests(unittest.TestCase):
    def test_capture_state_does_not_reuse_prior_or_malformed_values(self):
        with tempfile.TemporaryDirectory() as root_text:
            root = Path(root_text)
            log = root / "viewer.log"
            first = root / "acceptance_raster_90.png"
            second = root / "acceptance_raster_5.png"
            log.write_text(
                "\n".join((
                    "get: render.lighting.exposure_ev = -2",
                    "get: render.lighting.sun_elevation_deg = 90",
                    f"shot_now: queued {first}",
                    "get: render.lighting.exposure_ev = malformed",
                    "get: render.lighting.sun_elevation_deg = 5",
                    f"shot_now: queued {second}",
                )) + "\n",
                encoding="utf-8",
            )

            captures, errors = metrics.parse_captures(log)

            self.assertTrue(any("expected float" in error for error in errors))
            self.assertEqual(
                captures[("raster", 90)]["render.lighting.exposure_ev"], -2.0)
            self.assertNotIn(
                "render.lighting.exposure_ev", captures[("raster", 5)],
                "a missing/malformed capture-local value reused the prior frame",
            )

    def test_logged_capture_must_resolve_to_capture_directory(self):
        with tempfile.TemporaryDirectory() as root_text:
            root = Path(root_text)
            capture_dir = root / "captures"
            wrong_dir = root / "other"
            capture_dir.mkdir()
            wrong_dir.mkdir()
            log = root / "viewer.log"
            log.write_text(
                f"shot_now: queued {wrong_dir / 'acceptance_raster_90.png'}\n",
                encoding="utf-8",
            )

            try:
                _captures, errors = metrics.parse_captures(log, capture_dir)
            except TypeError:
                self.fail("parse_captures does not validate the capture directory")

            self.assertTrue(
                any("does not match capture directory" in error for error in errors),
                "a matching basename in a different logged directory was accepted",
            )

    def test_logged_capture_requires_done_guard(self):
        with tempfile.TemporaryDirectory() as root_text:
            capture_dir = Path(root_text)
            png = capture_dir / "acceptance_raster_90.png"
            png.write_bytes(b"not decoded by the log parser")
            log = capture_dir / "viewer.log"
            log.write_text(f"shot_now: queued {png}\n", encoding="utf-8")

            try:
                _captures, errors = metrics.parse_captures(log, capture_dir)
            except TypeError:
                self.fail("parse_captures does not validate .done guards")

            self.assertTrue(
                any("missing .done guard" in error for error in errors),
                "standalone metrics accepted an unguarded screenshot",
            )


if __name__ == "__main__":
    unittest.main()
