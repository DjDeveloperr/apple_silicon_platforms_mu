#!/usr/bin/env python3
"""Compile and run the host-side wireless DART handoff validator test.

This wraps Tests/test_wireless_handoff_validate.c, which compiles the real
Silicon/Apple/AppleSiliconPkg/Include/IndustryStandard/J414sWirelessHandoff.h
verbatim against the minimal Tests/edk2stub/Base.h.

It pins the validator that MemoryInitPeiLib.c (PEI) and AcpiPlatform.c (DXE)
now SHARE. Before 2026-07-30 the reservation crossed from PEI to DXE through
PcdAppleWirelessDartPageTableBase/Size, which are [PcdsPatchableInModule] --
per-module copies -- so DXE always read zero and DRT0 was withheld on every
boot regardless of what PEI derived. That channel is now a GUID HOB and both
phases authenticate the descriptor with this one copy of the validator.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / "Tests" / "test_wireless_handoff_validate.c"
STUB_DIR = REPO / "Tests" / "edk2stub"
HANDOFF_HEADER = (
    REPO
    / "Silicon"
    / "Apple"
    / "AppleSiliconPkg"
    / "Include"
    / "IndustryStandard"
    / "J414sWirelessHandoff.h"
)

# Assertions that must appear by name. A future edit that narrows the suite
# drops these and the wrapper fails rather than quietly passing less.
REQUIRED_ASSERTIONS = (
    "PASS: a fully valid ABI v2 handoff is accepted",
    "PASS: a single flipped bit in the L1 page table is rejected",
    "PASS: a single flipped bit in the MSI L2 page table is rejected",
    "PASS: a wholly zeroed reservation is rejected",
    "PASS: a zero descriptor CRC is rejected",
    "PASS: a mismatched guest memory top is rejected",
    "PASS: PEI->DXE reservation HOB is 32 bytes",
)


def _find_host_cc() -> str:
    for candidate in ("cc", "clang", "gcc"):
        path = shutil.which(candidate)
        if path:
            return path
    raise unittest.SkipTest("no host C compiler (cc/clang/gcc) found on PATH")


class TestWirelessHandoffValidate(unittest.TestCase):
    def test_source_files_present(self) -> None:
        self.assertTrue(SOURCE.is_file(), f"missing {SOURCE}")
        self.assertTrue(HANDOFF_HEADER.is_file(), f"missing {HANDOFF_HEADER}")
        self.assertTrue((STUB_DIR / "Base.h").is_file(), f"missing {STUB_DIR / 'Base.h'}")

    def test_validator_is_shared_by_both_phases(self) -> None:
        """The whole point of the fix: one copy, two consumers."""
        pei = (
            REPO
            / "Silicon"
            / "Apple"
            / "T602XFamilyPkg"
            / "Library"
            / "MemoryInitPeiLib"
            / "MemoryInitPeiLib.c"
        ).read_text(encoding="utf-8")
        dxe = (
            REPO
            / "Silicon"
            / "Apple"
            / "AppleSiliconPkg"
            / "Drivers"
            / "AcpiPlatformDxe"
            / "AcpiPlatform.c"
        ).read_text(encoding="utf-8")

        self.assertIn("NtasiValidateWirelessHandoffV2", pei)
        self.assertIn("NtasiValidateWirelessHandoffV2", dxe)
        # Neither may carry its own copy of the implementation any more.
        for name, text in (("PEI", pei), ("DXE", dxe)):
            self.assertNotIn(
                "Crc = (Crc >> 1) ^ (0xedb88320U",
                text,
                f"{name} re-implements the handoff CRC32 instead of sharing the header",
            )

    def test_dxe_reads_the_hob_not_the_patchable_pcds(self) -> None:
        """Regression: the PatchableInModule PCDs never crossed PEI -> DXE."""
        dxe = (
            REPO
            / "Silicon"
            / "Apple"
            / "AppleSiliconPkg"
            / "Drivers"
            / "AcpiPlatformDxe"
            / "AcpiPlatform.c"
        ).read_text(encoding="utf-8")
        self.assertIn("NTASI_WIRELESS_DART_RESERVATION_HOB_GUID", dxe)
        self.assertIn("GetFirstGuidHob", dxe)
        self.assertNotIn("PcdGet64 (PcdAppleWirelessDartPageTableBase)", dxe)
        self.assertNotIn("PcdGet32 (PcdAppleWirelessDartPageTableSize)", dxe)

    def test_host_build_and_run_passes(self) -> None:
        compiler = _find_host_cc()
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test_wireless_handoff_validate"
            compile_result = subprocess.run(
                [
                    compiler,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(STUB_DIR),
                    "-o",
                    str(binary),
                    str(SOURCE),
                ],
                cwd=REPO,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                f"host compile failed:\nstdout={compile_result.stdout}\nstderr={compile_result.stderr}",
            )

            run_result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(
                run_result.returncode,
                0,
                f"wireless handoff validator test failed:\n{run_result.stdout}\n{run_result.stderr}",
            )
            self.assertIn("all assertions passed", run_result.stdout)
            self.assertNotIn("FAIL:", run_result.stdout)
            for assertion in REQUIRED_ASSERTIONS:
                self.assertIn(assertion, run_result.stdout)


if __name__ == "__main__":
    unittest.main()
