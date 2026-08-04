#!/usr/bin/env python3
"""Contract for the names Windows displays for the J414s CPU devices."""

from __future__ import annotations

import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
DSDT = REPO / "Platform" / "MacBookProEarly2023Pkg" / "AcpiTables" / "DSDT.asl"

CORE_NAMES = {
    0: "Apple M2 Pro Efficiency Core 0",
    1: "Apple M2 Pro Efficiency Core 1",
    2: "Apple M2 Pro Efficiency Core 2",
    3: "Apple M2 Pro Efficiency Core 3",
    4: "Apple M2 Pro Performance Core 0",
    5: "Apple M2 Pro Performance Core 1",
    6: "Apple M2 Pro Performance Core 2",
    7: "Apple M2 Pro Performance Core 3",
    8: "Apple M2 Pro Performance Core 4",
    9: "Apple M2 Pro Performance Core 5",
}


def _device_block(text: str, cpu: int) -> str:
    marker = f"Device(CPU{cpu})"
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    raise AssertionError(f"unterminated {marker}")


class J414sAcpiCpuNames(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = DSDT.read_text(encoding="utf-8")

    def test_every_acpi0007_cpu_has_a_friendly_name(self) -> None:
        for cpu, name in CORE_NAMES.items():
            with self.subTest(cpu=cpu):
                block = _device_block(self.text, cpu)
                self.assertIn('Name(_HID, "ACPI0007")', block)
                self.assertIn(f'Name(_STR, Unicode ("{name}"))', block)
                self.assertIn(f'Name(_DDN, "{name}")', block)
                self.assertNotIn("Not Specified", block)

    def test_topology_still_publishes_exactly_ten_cpu_devices(self) -> None:
        self.assertEqual(
            sum(
                'Name(_HID, "ACPI0007")' in _device_block(self.text, cpu)
                for cpu in CORE_NAMES
            ),
            10,
        )


if __name__ == "__main__":
    unittest.main()
