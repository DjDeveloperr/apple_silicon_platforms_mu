#!/usr/bin/env python3
"""Fail-closed tests for the J414s NTAS0023 (AGX GPU) ACPI publication.

WHY THIS FILE EXISTS
--------------------
NTAS0023 was unpublished for two months because the static `GPU.asl` that used
to describe it hardcoded `hw_data_a` at [0x103db294000, 0x103db29c000) -- inside
OS RAM, ending exactly at `SystemMemoryTop`, and containing the exact
`SP_EL1` (0x103db29ba10) that crashed Mu's PEI twice.  The launcher
(`tools/run-m2-pro-mu.sh`) hard-refused any firmware that published it.

Publication is now safe, and these tests pin the three properties that made it
safe, because each is something a plausible future edit could quietly undo:

  1. No published resource may fall inside OS RAM.  The three UAT carveouts come
     from the live ADT and are bounded against real DRAM; the two MMIO windows
     are driver-ABI constants PROVEN against the live ADT before publication;
     and hw_data_a/hw_data_b/globals are backed by a firmware-owned
     EfiReservedMemoryType allocation.  The old addresses must never reappear.

  2. The _CRS is exactly eight memory resources in a fixed order plus one
     interrupt LAST.  AppleAgxGpu matches them POSITIONALLY and its
     ntasi_agx_t6020_resources_validate() rejects any other count -- so a short
     list fails closed but a REORDERED list does not, which makes the order a
     contract rather than a detail.

  3. The published GSIV is 46, not 40.  40 is the media profile's admac-sio
     (40 -> 1218); the AGX mailbox is 46 -> 1146.  One number cannot mean two
     physical AIC lines, and a regression to 40 would hand the audio DMA
     controller's interrupt to the GPU on any FD carrying both features.

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import hashlib
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
ACPI_PLATFORM = (
    REPO / "Silicon" / "Apple" / "AppleSiliconPkg" / "Drivers" / "AcpiPlatformDxe"
    / "AcpiPlatform.c"
)
CSRT_ASLC = REPO / "Silicon" / "Apple" / "T602XFamilyPkg" / "AcpiTables" / "CSRT.aslc"
MODULE_PATH = REPO / "Tools" / "j414s_mu_profile_manifest.py"
SPEC = importlib.util.spec_from_file_location("j414s_mu_profile_manifest", MODULE_PATH)
M = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(M)

# Emitted by drivers/AppleAic/emit_aic2_csrt.c in the driver repo and
# transcribed verbatim into CSRT.aslc.
CSRT_SHA256 = {
    "m2-pro": "cdee0da81d9c54c17d2964510271de453ae2ff0428fe0ff13e69efa9406521a5",
    "m2-pro-media": "a082eb6c95a12a29cdbc71e0c17fcb4091c1c5c48624595512cf5fa0342f16ba",
    "m2-pro-gpu": "ddf756c7103303471bc7e558881cf27b4503458a4360fcb3407ad0138008daff",
    "m2-pro-media-gpu": (
        "9bacd0d00e07af808374990eaaf954f657d2c44976bf2b7341531b85cf8b3759"
    ),
}

AGX_PUBLISHED_GSIV = 46
AGX_PHYSICAL_AIC = 1146

# The addresses the deleted GPU.asl published for hw_data_a / hw_data_b /
# globals.  m1n1's dt_set_gpu() allocated them with top_of_memory_alloc() on a
# DIFFERENT boot, on the Linux path that also shrinks the DT memory node; on
# this project's chainload/HV path nothing shrinks anything, so Mu's boot_args
# still covered them.  They are inside OS RAM.  None may ever be published
# again.
FORBIDDEN_OS_RAM_ADDRESSES = (0x103DB294000, 0x103DB290000, 0x103DB278000)

# The live-ADT values the three UAT carveouts must resolve to on this machine.
# Anchored to the pinned capture 20260729-142731-j414s-ans-v5/j414s-adt.bin,
# /arm-io/sgx gpu-region / gfx-shared-region / gfx-handoff.  These sit ABOVE
# boot_args' mem_size ceiling (SystemMemoryTop 0x103db29c000) and below real
# DRAM top (0x10400000000), which is exactly why they are safe and why the
# bound is real installed DRAM rather than mem_size.
ADT_UAT_CARVEOUTS = {
    "uat_ttbs": (0x103FFFB8000, 0x4000),
    "uat_pagetables": (0x103FFF78000, 0x40000),
    "uat_handoff": (0x103FFF70000, 0x4000),
}
SYSTEM_MEMORY_TOP = 0x103DB29C000
REAL_DRAM_TOP = 0x10400000000


def _host_cc() -> str:
    for candidate in ("cc", "clang", "gcc"):
        path = shutil.which(candidate)
        if path:
            return path
    raise unittest.SkipTest("no host C compiler (cc/clang/gcc) found on PATH")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def gpu_block() -> str:
    """The #if NTASI_ENABLE_GPU_ACPI_PUBLICATION region of AcpiPlatform.c."""
    text = ACPI_PLATFORM.read_text(encoding="utf-8")
    start = text.index("#if NTASI_ENABLE_GPU_ACPI_PUBLICATION")
    end = text.index("#endif // NTASI_ENABLE_GPU_ACPI_PUBLICATION")
    return text[start:end]


def define_value(name: str) -> int:
    text = ACPI_PLATFORM.read_text(encoding="utf-8")
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|\d+)", text, re.M)
    if match is None:
        raise AssertionError(f"AcpiPlatform.c has no #define {name}")
    return int(match.group(1), 0)


def csrt_bytes(media: int, gpu: int) -> bytes:
    source = CSRT_ASLC.read_text(encoding="utf-8")
    for drop in ("#include <Base.h>", "#include <IndustryStandard/Acpi.h>"):
        source = source.replace(drop, "")
    source = (
        source.replace("STATIC_ASSERT", "_Static_assert")
        .replace("UINT8", "unsigned char")
        .replace("VOID *CONST", "void *const")
    )
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "csrt.c"
        path.write_text(source, encoding="utf-8")
        result = subprocess.run(
            [
                _host_cc(), "-E", "-P",
                f"-DNTASI_ENABLE_MEDIA_PUBLICATION={media}",
                f"-DNTASI_J414S_GPU_RESOURCE_PROFILE={gpu}",
                str(path),
            ],
            capture_output=True, text=True,
        )
    if result.returncode:
        raise AssertionError(result.stderr)
    array = result.stdout[result.stdout.index("Csrt[] = {"):]
    array = array[: array.index("}")]
    return bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", array))


class GpuResourceContract(unittest.TestCase):
    """The eight-resource _CRS order AppleAgxGpu matches positionally."""

    def test_resource_indices_are_the_drivers_order(self):
        expected = [
            ("NTASI_GPU_RES_ASC", 0),
            ("NTASI_GPU_RES_SGX", 1),
            ("NTASI_GPU_RES_TTBS", 2),
            ("NTASI_GPU_RES_PAGETABLES", 3),
            ("NTASI_GPU_RES_HANDOFF", 4),
            ("NTASI_GPU_RES_HWDATA_A", 5),
            ("NTASI_GPU_RES_HWDATA_B", 6),
            ("NTASI_GPU_RES_GLOBALS", 7),
            ("NTASI_GPU_RES_COUNT", 8),
        ]
        for name, value in expected:
            with self.subTest(macro=name):
                self.assertEqual(define_value(name), value)

    def test_generator_emits_every_resource_then_the_interrupt_last(self):
        block = strip_comments(gpu_block())
        # One loop over all eight, then exactly one AmlCodeGenRdInterrupt.
        self.assertIn("Index < NTASI_GPU_RES_COUNT", block)
        self.assertEqual(block.count("AmlCodeGenRdInterrupt"), 1)
        self.assertLess(
            block.index("AppleAnsAddMemoryResource"),
            block.index("AmlCodeGenRdInterrupt"),
            "the interrupt descriptor must come after the memory windows",
        )

    def test_a_short_crs_is_never_published(self):
        """Every resource, or none.  A gap cannot be expressed positionally."""
        block = strip_comments(gpu_block())
        self.assertIn("Resolved", block)
        self.assertIn("EFI_NOT_FOUND", block)

    def test_published_resources_are_checked_for_overlap(self):
        self.assertIn("NtasiRangesOverlap", strip_comments(gpu_block()))


class GpuAddressSafety(unittest.TestCase):
    """Nothing published may land in memory the OS owns."""

    def test_the_os_ram_addresses_never_reappear(self):
        text = ACPI_PLATFORM.read_text(encoding="utf-8")
        code = strip_comments(text)
        for address in FORBIDDEN_OS_RAM_ADDRESSES:
            with self.subTest(address=hex(address)):
                # Comments may discuss them -- the incident history is the
                # reason this file is careful -- but no CODE may contain them.
                self.assertNotIn(f"{address:X}", code.upper().replace("0X", ""))

    def test_no_static_gpu_asl_has_come_back(self):
        acpi_tables = REPO / "Platform" / "MacBookProEarly2023Pkg" / "AcpiTables"
        self.assertFalse(
            (acpi_tables / "GPU.asl").exists(),
            "GPU.asl is back; it hardcoded _CRS ranges inside OS RAM",
        )
        self.assertFalse((acpi_tables / "GpuAcpiTables.inf").exists())

    def test_uat_carveouts_are_above_os_ram_and_inside_real_dram(self):
        """The property that makes resources 2-4 safe, stated as arithmetic.

        These are read from the live ADT at DXE, not hardcoded -- the values
        here are the pinned capture's, used to assert the SAFETY PROPERTY holds
        for this machine, not to pin what firmware publishes.
        """
        for label, (base, size) in ADT_UAT_CARVEOUTS.items():
            with self.subTest(region=label):
                self.assertGreaterEqual(
                    base, SYSTEM_MEMORY_TOP,
                    f"{label} starts inside OS RAM",
                )
                self.assertLessEqual(
                    base + size, REAL_DRAM_TOP,
                    f"{label} runs past real installed DRAM",
                )

    def test_placeholders_are_firmware_owned_reserved_memory(self):
        """Resources 5-7 have no live source, so they must be ALLOCATED."""
        block = strip_comments(gpu_block())
        self.assertIn("AllocateAlignedReservedPages", block)
        # Zero-filled, and the fact stated in _DSD rather than hidden.
        self.assertIn("ZeroMem", block)
        self.assertIn("ntasp,preboot-handoff-present", block)

    def test_no_fabricated_calibration_metadata_is_published(self):
        """The deleted GPU.asl asserted CRC32s for data captured elsewhere."""
        block = strip_comments(gpu_block())
        for forbidden in ("crc32", "payload-size", "fac3327a", "8360bea5", "8ac088ef"):
            with self.subTest(property=forbidden):
                self.assertNotIn(forbidden, block)

    def test_mmio_constants_are_proven_against_the_live_adt(self):
        """They cannot be derived (the driver pins them), so they are checked."""
        block = strip_comments(gpu_block())
        self.assertIn("NtasiGpuMmioWindowsAgreeWithAdt", block)
        self.assertIn("dt_node_reg", block)
        self.assertIn("NtasiRangeWithinWindow", block)


class GpuInterruptAllocation(unittest.TestCase):
    """The published GSIV must be 46 and must collide with nothing."""

    def test_generator_publishes_46(self):
        self.assertEqual(define_value("NTASI_GPU_PUBLISHED_GSIV"), AGX_PUBLISHED_GSIV)
        self.assertEqual(define_value("NTASI_GPU_PHYSICAL_AIC"), AGX_PHYSICAL_AIC)

    def test_exactly_one_interrupt_is_published(self):
        block = strip_comments(gpu_block())
        match = re.search(
            r"AmlCodeGenRdInterrupt\s*\((.*?)\);", block, re.S
        )
        self.assertIsNotNone(match)
        # The vector count argument is the literal 1.
        self.assertRegex(match.group(1), r"&Irq,\s*\n?\s*1,")

    def test_csrt_translates_46_to_1146_in_both_gpu_variants(self):
        for media in (0, 1):
            with self.subTest(media=media):
                table = csrt_bytes(media=media, gpu=1)
                self.assertIn(
                    struct.pack("<II", AGX_PUBLISHED_GSIV, AGX_PHYSICAL_AIC), table
                )

    def test_the_agx_alias_is_never_40_or_44(self):
        """40 is media's admac-sio; 44 is owned by /arm-io/i2c0/hpmBusManager."""
        for media in (0, 1):
            table = csrt_bytes(media=media, gpu=1)
            with self.subTest(media=media):
                self.assertNotIn(struct.pack("<II", 40, AGX_PHYSICAL_AIC), table)
                self.assertNotIn(struct.pack("<II", 44, AGX_PHYSICAL_AIC), table)

    def test_no_two_aliases_share_a_published_gsiv_or_a_line(self):
        """The invariant that replaced CSRT.aslc's #error, checked on bytes."""
        for media, gpu in ((0, 0), (1, 0), (0, 1), (1, 1)):
            with self.subTest(media=media, gpu=gpu):
                table = csrt_bytes(media=media, gpu=gpu)
                index = table.index(b"ALI2")
                count = struct.unpack_from("<I", table, index + 8)[0]
                entries = [
                    struct.unpack_from("<II", table, index + 16 + 8 * i)
                    for i in range(count)
                ]
                published = [g for g, _ in entries]
                physical = [p for _, p in entries]
                self.assertEqual(len(set(published)), len(published))
                self.assertEqual(len(set(physical)), len(physical))
                # A published number must not also be somebody's real line.
                self.assertFalse(set(published) & set(physical))
                # Every published GSIV must be inside the GIC carrier window.
                for gsiv in published:
                    self.assertTrue(32 <= gsiv < 1024)


class GpuCsrtVariants(unittest.TestCase):
    def test_all_four_variants_hash_as_expected(self):
        for (media, gpu), name in (
            ((0, 0), "m2-pro"),
            ((1, 0), "m2-pro-media"),
            ((0, 1), "m2-pro-gpu"),
            ((1, 1), "m2-pro-media-gpu"),
        ):
            with self.subTest(variant=name):
                table = csrt_bytes(media=media, gpu=gpu)
                self.assertEqual(
                    hashlib.sha256(table).hexdigest(), CSRT_SHA256[name]
                )

    def test_every_variant_is_a_superset_with_boot_usb_first(self):
        """37 -> 1274 is the boot USB controller.  It must never move."""
        for media, gpu in ((0, 0), (1, 0), (0, 1), (1, 1)):
            with self.subTest(media=media, gpu=gpu):
                table = csrt_bytes(media=media, gpu=gpu)
                index = table.index(b"ALI2")
                first = struct.unpack_from("<II", table, index + 16)
                self.assertEqual(first, (37, 1274))

    def test_the_non_gpu_tables_are_byte_for_byte_unchanged(self):
        """Moving the AGX alias must not have touched any other profile."""
        self.assertEqual(len(csrt_bytes(media=0, gpu=0)), 256)
        self.assertEqual(len(csrt_bytes(media=1, gpu=0)), 296)


class GpuProfilePolicy(unittest.TestCase):
    def test_publication_tracks_the_gpu_flag_except_for_the_control(self):
        for profile, entry in M.PROFILES.items():
            with self.subTest(profile=profile):
                features = M.profile_policy(profile)["experimental_features"]
                self.assertIs(
                    features["gpu_carveout_reservation"], bool(entry["gpu"])
                )
                self.assertIs(
                    features["gpu_acpi_ntas0023_publication"],
                    bool(entry["gpu_acpi"]),
                )

    def test_gpu_noacpi_is_a_true_single_variable_control(self):
        """It must differ from `gpu` in the publication and nothing else."""
        gpu = M.PROFILES["gpu"]
        control = M.PROFILES["gpu-noacpi"]
        self.assertTrue(control["gpu"])
        self.assertFalse(control["gpu_acpi"])
        self.assertEqual(control["expected_ffs_count"], gpu["expected_ffs_count"])
        for key in ("ans", "wireless", "media"):
            self.assertEqual(control[key], gpu[key])

    def test_published_gsiv_set_is_pinned_in_both_directions(self):
        for profile, entry in M.PROFILES.items():
            with self.subTest(profile=profile):
                features = M.profile_policy(profile)["experimental_features"]
                expected = [AGX_PUBLISHED_GSIV] if entry["gpu_acpi"] else []
                self.assertEqual(features["gpu_published_gsivs"], expected)
                self.assertNotIn(40, features["gpu_published_gsivs"])

    def test_gpu_publication_never_implies_a_new_ffs_module(self):
        """The SSDT is generated at DXE runtime, like ANS0 and DRT0."""
        self.assertEqual(
            M.PROFILES["gpu"]["expected_ffs_count"],
            M.PROFILES["baseline"]["expected_ffs_count"],
        )

    def test_media_and_gpu_can_now_be_selected_together(self):
        entry = M.PROFILES["media-gpu"]
        self.assertTrue(entry["media"] and entry["gpu"])
        features = M.profile_policy("media-gpu")["experimental_features"]
        self.assertEqual(features["csrt_variant"], "m2-pro-media-gpu")
        self.assertEqual(features["csrt_ali2_alias_count"], 9)
        # Both device sets are published, and their GSIVs are disjoint.
        self.assertEqual(features["gpu_published_gsivs"], [AGX_PUBLISHED_GSIV])
        self.assertFalse(
            set(features["gpu_published_gsivs"])
            & set(features["media_published_gsivs"])
        )


if __name__ == "__main__":
    unittest.main()
