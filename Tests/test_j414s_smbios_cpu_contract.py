#!/usr/bin/env python3
"""Fail-closed tests for the SMBIOS Type 4 processor record this firmware publishes.

WHY THIS FILE EXISTS
--------------------
Windows on ARM64 does not invent a processor name.  In the pinned 26200.8037
kernel (`ntoskrnl.exe` SHA-256 e149ebcd...548a748, kept in the driver repo at
`build/m2-pro-readiness/crash-0x109-20260727/`) the routine at RVA 0x75b000
asks the SMBIOS walker for a **type 4** structure and then, byte for byte:

    ldrb w8,  [x21, #0x05]   ; Type 4 +0x05 = ProcessorType
    cmp  w8,  #3             ; must be CentralProcessor, or the whole record
    b.ne skip                ;   is ignored
    ldrb w1,  [x21, #0x07]   ; +0x07 = ProcessorManufacturer string index
    ldrb w20, [x21, #0x10]   ; +0x10 = ProcessorVersion string index

and then enumerates **every** subkey of
`HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor` and writes
`ProcessorNameString` (from ProcessorVersion) and `VendorIdentifier` (from
ProcessorManufacturer) into each one.  That is why a placeholder
ProcessorVersion showed up as ten cores all called "Not Specified": one bad
Type 4 string is copied onto all ten processors.

What this file does NOT claim
-----------------------------
Task Manager's *Base speed* does not come from Type 4 MaxSpeed on this kernel.
`HalpTimerSaveProcessorFrequency` (RVA 0x488a20) takes KPRCB.MHz from the
HAL timer with KnownType **10** -- the PMU cycle counter -- and
`PopProcessorInformation` (RVA 0x8c4350) prefers the PPM perf domain's
NominalFrequency over it.  Neither reads SMBIOS.  That chain is pinned
separately by `tests/test_windows_cpu_frequency_source.py` in the driver
repo.  MaxSpeed/CurrentSpeed are still asserted here because they are what
every *other* SMBIOS consumer reports, and because a regression to 0 (the
AppleSiliconPkg.dec default) is silent.

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SMBIOS_C = (
    REPO / "Silicon" / "Apple" / "AppleSiliconPkg" / "Drivers" / "SmbiosInfoDxe"
    / "SmbiosInfoDxe.c"
)
SMBIOS_INF = (
    REPO / "Silicon" / "Apple" / "AppleSiliconPkg" / "Drivers" / "SmbiosInfoDxe"
    / "SmbiosInfoDxe.inf"
)
FAMILY_DSC_INC = (
    REPO / "Silicon" / "Apple" / "T602XFamilyPkg" / "T602XFamilyPkg.dsc.inc"
)
TOPOLOGY_H = (
    REPO / "Silicon" / "Apple" / "T602XFamilyPkg" / "AcpiTables"
    / "T6020J414sTopology.h"
)
PPTT_ASLC = REPO / "Silicon" / "Apple" / "T602XFamilyPkg" / "AcpiTables" / "PPTT.aslc"

# The 10-core J414s ADT exposes cpu0..cpu6 and cpu8..cpu10 (no cpu7).  The
# highest slot that must be probed is therefore 10, so the probe bound has to
# be strictly greater than 10.
J414S_HIGHEST_ADT_CPU_INDEX = 10
J414S_CORE_COUNT = 10


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _pcd_value(text: str, name: str) -> str:
    match = re.search(
        rf"^\s*gAppleSiliconPkgTokenSpaceGuid\.{re.escape(name)}\|([^\s|#]+)",
        text,
        re.MULTILINE,
    )
    assert match is not None, f"{name} is not assigned"
    return match.group(1)


def _type4_template(text: str) -> str:
    """The SMBIOS_TABLE_TYPE4 initializer, as source text."""
    start = text.index("SMBIOS_TABLE_TYPE4 mProcessorInfoType4 = {")
    end = text.index("CHAR8 *mProcessorInfoType4Strings", start)
    return text[start:end]


def _type4_update_body(text: str) -> str:
    start = text.index("VOID ProcessorInfoUpdateSmbiosType4(")
    end = text.index("VOID CacheInfoUpdateSmbiosType7(", start)
    return text[start:end]


class Type4StaticRecord(unittest.TestCase):
    """Fields the kernel reads by offset, plus the ones that render as strings."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = _read(SMBIOS_C)
        cls.template = _type4_template(cls.text)

    def test_processor_type_is_central_processor(self) -> None:
        # `cmp w8, #3` at +0x05: anything else and Windows ignores the record
        # entirely, leaving ProcessorNameString unwritten on all processors.
        self.assertRegex(self.template, r"\bCentralProcessor,\s*//\s*ProcessorType")

    def test_family_is_indicated_via_family2_armv8(self) -> None:
        self.assertIn("ProcessorFamilyIndicatorFamily2", self.template)
        self.assertIn("ProcessorFamilyARMv8", self.template)

    def test_socket_manufacturer_version_and_part_number_have_string_indices(self) -> None:
        # A zero string index is SMBIOS for "no string", which every consumer
        # renders as "Not Specified".
        for field, index in (
            ("Socket", 1),
            ("ProcessorManufacture", 2),
            ("ProcessorVersion", 3),
            ("PartNumber", 4),
        ):
            with self.subTest(field=field):
                self.assertRegex(
                    self.template,
                    re.compile(rf"^\s*{index},\s*//\s*{field}", re.MULTILINE),
                )

    def test_status_is_populated_and_enabled(self) -> None:
        self.assertRegex(self.template, r"0x41,\s*//\s*Status")

    def test_characteristics_claim_64_bit_and_multi_core(self) -> None:
        match = re.search(r"(0x[0-9A-Fa-f]+),\s*//\s*ProcessorCharacteristics", self.template)
        self.assertIsNotNone(match)
        characteristics = int(match.group(1), 16)
        self.assertTrue(characteristics & (1 << 2), "64-bit Capable must be set")
        self.assertTrue(characteristics & (1 << 3), "Multi-Core must be set")
        self.assertFalse(
            characteristics & (1 << 4),
            "Hardware Thread must be clear: Apple cores are single-threaded",
        )

    def test_external_clock_is_the_soc_reference(self) -> None:
        self.assertRegex(self.template, r"\b24,\s*//\s*ExternalClock")

    def test_wide_core_counts_exist_in_the_static_record(self) -> None:
        # sizeof(SMBIOS_TABLE_TYPE4) covers the SMBIOS 3.0/3.6 fields, so the
        # published Length advertises them whether or not they are initialised.
        for field in ("CoreCount2", "EnabledCoreCount2", "ThreadCount2", "ThreadEnabled"):
            with self.subTest(field=field):
                self.assertIn(field, self.template)

    def test_no_type4_string_is_the_literal_placeholder(self) -> None:
        strings = re.search(
            r"CHAR8 \*mProcessorInfoType4Strings\[\] = \{(.*?)\};",
            self.text,
            re.DOTALL,
        )
        self.assertIsNotNone(strings)
        # Slots 2 (ProcessorVersion) and 3 (PartNumber) are both replaced at
        # runtime; assert the code actually does that rather than trusting the
        # static initialiser, which is only a fallback for a missing ADT.
        body = _type4_update_body(self.text)
        self.assertIn("mProcessorInfoType4Strings[2] = mProcessorVersionString;", body)
        self.assertIn("mProcessorInfoType4Strings[3] = mProcessorPartNumberString;", body)
        self.assertIn(
            "mProcessorInfoType4Strings[3] = (CHAR8 *)PcdGetPtr(PcdSmbiosCpuIdentifier);",
            body,
            "with no ADT chip-id the part number must fall back to the family "
            "identifier PCD, not to the literal string 'Not Specified'",
        )


class Type4RuntimePatching(unittest.TestCase):
    """The ADT-derived and PCD-derived values, and the loop that finds them."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = _read(SMBIOS_C)
        cls.body = _type4_update_body(cls.text)
        cls.inf = _read(SMBIOS_INF)
        cls.dsc_inc = _read(FAMILY_DSC_INC)

    def test_core_count_comes_from_the_live_adt(self) -> None:
        self.assertIn('"/cpus/cpu%u"', self.body)
        self.assertIn('dt_node_prop(CpuNode, "cluster-type"', self.body)

    def test_the_probe_bound_reaches_the_highest_j414s_cpu_slot(self) -> None:
        self.assertIn("for (Index = 0; Index < PcdGet32(PcdCoreCount); Index++)", self.body)
        bound = int(_pcd_value(self.dsc_inc, "PcdCoreCount").split("|")[0])
        self.assertGreater(
            bound,
            J414S_HIGHEST_ADT_CPU_INDEX,
            "PcdCoreCount bounds the sparse ADT probe; the J414s has cpu10, so "
            "a bound of 10 would silently publish 9 cores",
        )

    def test_all_four_count_fields_are_patched_together(self) -> None:
        for field in (
            "CoreCount",
            "EnabledCoreCount",
            "ThreadCount",
            "CoreCount2",
            "EnabledCoreCount2",
            "ThreadCount2",
            "ThreadEnabled",
        ):
            with self.subTest(field=field):
                self.assertRegex(
                    self.body,
                    rf"mProcessorInfoType4\.{field}\s*=\s*\(UINT(?:8|16)\)TotalCoreCount;",
                )

    def test_speeds_are_taken_from_the_family_pcds(self) -> None:
        self.assertIn("mProcessorInfoType4.MaxSpeed = (UINT16)PCoreMaxMhz;", self.body)
        self.assertIn("mProcessorInfoType4.CurrentSpeed = (UINT16)BootFreqMhz;", self.body)
        self.assertIn("FixedPcdGet32(PcdSmbiosPCoreMaxFreqMhz)", self.body)
        self.assertIn("FixedPcdGet32(PcdSmbiosBootFreqMhz)", self.body)

    def test_frequency_pcds_are_non_zero_and_pinned(self) -> None:
        # 0 is the AppleSiliconPkg.dec default and means "unknown"; every
        # SMBIOS consumer then reports a 0 MHz processor.
        expected = {
            "PcdSmbiosECoreMaxFreqMhz": 2424,
            "PcdSmbiosPCoreMaxFreqMhz": 3504,
            "PcdSmbiosBootFreqMhz": 1968,
        }
        for name, value in expected.items():
            with self.subTest(pcd=name):
                actual = int(_pcd_value(self.dsc_inc, name))
                self.assertNotEqual(actual, 0, f"{name} fell back to the .dec default")
                self.assertEqual(actual, value)

    def test_the_frequency_provenance_stays_in_the_dsc(self) -> None:
        # These three numbers are constants, not probed p-state tables; the
        # comment naming where each came from is the only thing that keeps
        # them checkable.
        self.assertIn("t602x-common.dtsi", self.dsc_inc)
        self.assertIn("blizzard_opp", self.dsc_inc)
        self.assertIn("avalanche_opp", self.dsc_inc)
        self.assertIn("cpufreq.c", self.dsc_inc)

    def test_the_chip_name_map_resolves_t6020(self) -> None:
        self.assertRegex(self.text, r"\{\s*0x6020,\s*\"Apple M2 Pro\"\s*\}")
        self.assertIn('dt_node_prop(ChosenNode, "chip-id"', self.text)

    def test_every_consumed_pcd_is_declared_by_the_inf(self) -> None:
        for name in (
            "PcdCoreCount",
            "PcdSmbiosCpuModel",
            "PcdSmbiosCpuIdentifier",
            "PcdSmbiosECoreMaxFreqMhz",
            "PcdSmbiosPCoreMaxFreqMhz",
            "PcdSmbiosBootFreqMhz",
        ):
            with self.subTest(pcd=name):
                self.assertIn(f"gAppleSiliconPkgTokenSpaceGuid.{name}", self.inf)


class SmbiosAgreesWithAcpiTopology(unittest.TestCase):
    """One machine, one core count: SMBIOS and PPTT must not disagree."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.topology = _read(TOPOLOGY_H)
        cls.pptt = _read(PPTT_ASLC)

    def _define(self, name: str) -> int:
        match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)", self.topology, re.MULTILINE)
        assert match is not None, f"{name} is not defined"
        return int(match.group(1))

    def test_topology_header_still_describes_ten_cores(self) -> None:
        self.assertEqual(self._define("T6020_J414S_CPU_COUNT"), J414S_CORE_COUNT)

    def test_the_clusters_sum_to_the_core_count(self) -> None:
        total = (
            self._define("T6020_J414S_E_CORE_COUNT")
            + self._define("T6020_J414S_P0_CORE_COUNT")
            + self._define("T6020_J414S_P1_CORE_COUNT")
        )
        self.assertEqual(total, J414S_CORE_COUNT)
        self.assertEqual(self._define("T6020_J414S_CLUSTER_COUNT"), 3)

    def test_pptt_exists_and_describes_one_package_of_three_clusters(self) -> None:
        self.assertIn("PROCESSOR_PROPERTIES_TOPOLOGY_TABLE", self.pptt)
        self.assertIn("#define DIE_COUNT               1", self.pptt)
        self.assertIn("CLUSTER_4_INIT(DieId, Blizzard", self.pptt)
        self.assertIn("CLUSTER_3_INIT(DieId, Avalanche0", self.pptt)
        self.assertIn("CLUSTER_3_INIT(DieId, Avalanche1", self.pptt)

    def test_pptt_shares_the_single_topology_header(self) -> None:
        # A second, divergent core count is exactly how SMBIOS and ACPI drift.
        self.assertIn('#include "T6020J414sTopology.h"', self.pptt)


if __name__ == "__main__":
    unittest.main()
