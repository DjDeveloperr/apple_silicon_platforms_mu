"""Build and run the host-only Apple USB4 ACIO/NHI sequencing core."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Tests/test_apple_usb4_core.c"
CORE = ROOT / (
    "Silicon/Apple/AppleSiliconPkg/Drivers/AppleUsb4BringupDxe/"
    "AppleUsb4Core.c"
)
ROUTER_CORE = ROOT / (
    "Silicon/Apple/AppleSiliconPkg/Drivers/AppleUsb4BringupDxe/"
    "AppleUsb4RouterCore.c"
)
TUNNEL_CORE = ROOT / (
    "Silicon/Apple/AppleSiliconPkg/Drivers/AppleUsb4BringupDxe/"
    "AppleUsb4TunnelCore.c"
)


class AppleUsb4CoreTests(unittest.TestCase):
    def test_host_core(self) -> None:
        compiler = next(
            (path for name in ("cc", "clang", "gcc") if (path := shutil.which(name))),
            None,
        )
        if compiler is None:
            self.skipTest("no host C compiler")

        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "apple-usb4-core-test"
            build = subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(SOURCE),
                    str(CORE),
                    str(ROUTER_CORE),
                    str(TUNNEL_CORE),
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("apple usb4 core tests passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
