#!/usr/bin/env python3
"""Compile and run the host-side Apple RTKit buffer-request regression test.

This wraps Tests/test_rtkit_buffer_request.c -- a standalone, EDK2-free C
program that links the real
Silicon/Apple/AppleSiliconPkg/Drivers/AppleNANDStorageDxe/Shared/
Apple{Asc,Rtkit,RtkitRuntime}Core.c translation units against a fake ASC
mailbox -- so it runs through the same `python3 -m unittest` entry point as
the rest of Tests/, with no EDK2 build and no hardware involved.

It pins the 2026-07-30 J414s/T6020 hardware failure in which ANS bring-up
completed successfully and the ExitBootServices handoff then logged
"AppleANS: RTKit handoff failed: -25" (NTASI_RTKIT_RUNTIME_ERR_BUFFER),
caused by rejecting buffer requests that carry a non-zero IOVA. It also pins
the current Asahi 44-bit IOVA field and the cold-versus-wake ownership rules.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / "Tests" / "test_rtkit_buffer_request.c"
SHARED = (
    REPO
    / "Silicon"
    / "Apple"
    / "AppleSiliconPkg"
    / "Drivers"
    / "AppleNANDStorageDxe"
    / "Shared"
)
CORES = (
    SHARED / "AppleAscCore.c",
    SHARED / "AppleRtkitCore.c",
    SHARED / "AppleRtkitRuntimeCore.c",
)
RUNTIME_HEADER = SHARED / "AppleRtkitRuntimeCore.h"

# Assertions that must appear by name in the output. A future edit that
# narrows the test would drop these, and the wrapper fails rather than
# quietly passing a weaker suite.
REQUIRED_ASSERTIONS = (
    "PASS: 2026-07-30 regression: a pre-allocated (non-zero IOVA) buffer request must NOT return -25",
    "PASS: a pre-allocated buffer request sends NO reply (matches m1n1)",
    "PASS: bit 42 is an IOVA bit: the request is pre-allocated",
    "PASS: bit 43 is the top IOVA bit: the request is pre-allocated",
    "PASS: WAKE never writes a live coprocessor's CPU_CONTROL",
    "PASS: COLD sends nothing before the coprocessor's HELLO",
    "PASS: release_shared runs for the AP-allocated buffer only",
    "PASS: the coprocessor run bit is cleared even when the quiesce failed",
    "PASS: a genuine allocation failure is still reported as -25",
)


def _find_host_cc() -> str:
    for candidate in ("cc", "clang", "gcc"):
        path = shutil.which(candidate)
        if path:
            return path
    raise unittest.SkipTest("no host C compiler (cc/clang/gcc) found on PATH")


class TestRtkitBufferRequest(unittest.TestCase):
    def test_source_files_present(self) -> None:
        self.assertTrue(SOURCE.is_file(), f"missing {SOURCE}")
        self.assertTrue(RUNTIME_HEADER.is_file(), f"missing {RUNTIME_HEADER}")
        for core in CORES:
            self.assertTrue(core.is_file(), f"missing {core}")

    def test_host_build_and_run_passes(self) -> None:
        compiler = _find_host_cc()
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test_rtkit_buffer_request"
            compile_result = subprocess.run(
                [
                    compiler,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-o",
                    str(binary),
                    str(SOURCE),
                    *[str(core) for core in CORES],
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
                f"RTKit buffer-request regression test failed:\n{run_result.stdout}\n{run_result.stderr}",
            )
            self.assertIn("all assertions passed", run_result.stdout)
            self.assertNotIn("FAIL:", run_result.stdout)
            for assertion in REQUIRED_ASSERTIONS:
                self.assertIn(assertion, run_result.stdout)


if __name__ == "__main__":
    unittest.main()
