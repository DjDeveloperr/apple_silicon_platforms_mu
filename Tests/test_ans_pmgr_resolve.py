#!/usr/bin/env python3
"""Compile and run the host-side ANS PMGR domain resolution regression test.

This wraps Tests/test_ans_pmgr_resolve.c -- a standalone, EDK2-free C
program that exercises the exact name-based PMGR resolution arithmetic
(NtasiPmgrFindDomainAddress / NtasiPmgrResolveAddress /
NtasiPmgrDeviceNameEquals) compiled into
Silicon/Apple/AppleSiliconPkg/Drivers/AcpiPlatformDxe/AcpiPlatform.c --
so it runs through the same `python3 -m unittest` entry point as the rest
of Tests/, with no EDK2 build, no ADT, and no hardware involved.

It specifically reproduces the 2026-07-30 hardware finding (the DSC's
hardcoded PcdAppleAnsPmgr*Base constants resolved to DCS_09/DCS_10 --
DRAM controller power domains -- instead of ANS2/APCIE_ST/APCIE_ST_SYS/
APCIE_ST1_SYS, because they were computed against the wrong "/arm-io/pmgr"
register block) and fails if name-based resolution would ever again
confuse the two.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / "Tests" / "test_ans_pmgr_resolve.c"
RESOLVE_HEADER = (
    REPO
    / "Silicon"
    / "Apple"
    / "AppleSiliconPkg"
    / "Drivers"
    / "AcpiPlatformDxe"
    / "NtasiAnsPmgrResolve.h"
)


def _find_host_cc() -> str:
    for candidate in ("cc", "clang", "gcc"):
        path = shutil.which(candidate)
        if path:
            return path
    raise unittest.SkipTest("no host C compiler (cc/clang/gcc) found on PATH")


class TestAnsPmgrResolve(unittest.TestCase):
    def test_source_files_present(self) -> None:
        self.assertTrue(SOURCE.is_file(), f"missing {SOURCE}")
        self.assertTrue(RESOLVE_HEADER.is_file(), f"missing {RESOLVE_HEADER}")

    def test_host_build_and_run_passes(self) -> None:
        compiler = _find_host_cc()
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test_ans_pmgr_resolve"
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
                f"resolution regression test failed:\n{run_result.stdout}\n{run_result.stderr}",
            )
            self.assertIn("all assertions passed", run_result.stdout)
            self.assertNotIn("FAIL:", run_result.stdout)
            # The specific 2026-07-30 DCS misresolution regression must be
            # an explicit, named assertion in the output, not merely
            # absent from a FAIL line -- this guards against the test
            # itself being silently narrowed in a future edit.
            self.assertIn(
                "PASS: regression: resolving \"ANS2\" by name never returns DCS_10's address",
                run_result.stdout,
            )
            self.assertIn(
                "PASS: regression: resolving \"APCIE_ST\" by name never returns DCS_09's address",
                run_result.stdout,
            )
            self.assertIn(
                "PASS: ANS2 resolves to the hardware-confirmed 0x2902801A8, not the old 0x28E0801A8",
                run_result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
