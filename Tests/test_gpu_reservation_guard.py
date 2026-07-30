#!/usr/bin/env python3
"""Compile and run the host-side GPU reservation guard regression test.

This wraps Tests/test_gpu_reservation_guard.c -- a standalone, EDK2-free C
program that exercises the exact safety-invariant logic
(NtasiRangeContainsPoint / NtasiRangesOverlap) compiled into
Silicon/Apple/AppleSiliconPkg/Drivers/AcpiPlatformDxe/AcpiPlatform.c (moved
there from PEI's MemoryInitPeiLib.c on 2026-07-30) -- so it runs through
the same `python3 -m unittest` entry point as the rest of Tests/, with no
EDK2 build and no hardware involved.

It specifically reproduces the 2026-07-30 hardware crash (a GPU
"hw_data_a" preboot reservation computed from Mu's own SystemMemoryTop
that landed on the live PEI stack pointer) and fails if the guard would
ever again fail to flag that computation.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / "Tests" / "test_gpu_reservation_guard.c"
GUARD_HEADER = (
    REPO
    / "Silicon"
    / "Apple"
    / "AppleSiliconPkg"
    / "Drivers"
    / "AcpiPlatformDxe"
    / "NtasiGpuReservationGuard.h"
)


def _find_host_cc() -> str:
    for candidate in ("cc", "clang", "gcc"):
        path = shutil.which(candidate)
        if path:
            return path
    raise unittest.SkipTest("no host C compiler (cc/clang/gcc) found on PATH")


class TestGpuReservationGuard(unittest.TestCase):
    def test_source_files_present(self) -> None:
        self.assertTrue(SOURCE.is_file(), f"missing {SOURCE}")
        self.assertTrue(GUARD_HEADER.is_file(), f"missing {GUARD_HEADER}")

    def test_host_build_and_run_passes(self) -> None:
        compiler = _find_host_cc()
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test_gpu_reservation_guard"
            compile_result = subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-o", str(binary), str(SOURCE)],
                cwd=REPO,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                f"host compile failed:\nstdout={compile_result.stdout}\nstderr={compile_result.stderr}",
            )

            run_result = subprocess.run(
                [str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                run_result.returncode,
                0,
                f"guard regression test failed:\n{run_result.stdout}\n{run_result.stderr}",
            )
            self.assertIn("all assertions passed", run_result.stdout)
            self.assertNotIn("FAIL:", run_result.stdout)
            # The specific 2026-07-30 stack-overlap regression must be an
            # explicit, named assertion in the output, not merely absent
            # from a FAIL line -- this guards against the test itself being
            # silently narrowed in a future edit.
            self.assertIn(
                "PASS: 2026-07-30 regression: the crashing hw_data_a range must be flagged as containing the live PEI stack pointer",
                run_result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
