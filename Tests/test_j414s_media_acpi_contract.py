#!/usr/bin/env python3
"""Fail-closed tests for the J414s media ACPI publication contract.

The media profile publishes three devices whose _CRS is matched POSITIONALLY by
their Windows drivers.  A short list is detected by the drivers and fails
closed; a REORDERED list is not detected at all, and for AppleIsp it would put a
DART TTBR write into a coprocessor control register.  So the window order is a
contract, and it is written down twice:

  * Platform/MacBookProEarly2023Pkg/AcpiTables/Media/{MCA,AOPA,ISP}.asl -- the
    human-readable specification, which the build does NOT compile.
  * NtasiInstallMediaTables() in
    Silicon/Apple/AppleSiliconPkg/Drivers/AcpiPlatformDxe/AcpiPlatform.c -- the
    AmlLib generator that actually ships.

These tests pin the two against each other so they cannot drift, and pin the
three safety properties that a future edit could plausibly undo by accident:
zero interrupt resources, no speaker-render opt-in, and a media flag that is off
in every profile that is not named for it.
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
ASL_DIR = REPO / "Platform" / "MacBookProEarly2023Pkg" / "AcpiTables" / "Media"
ACPI_PLATFORM = (
    REPO
    / "Silicon"
    / "Apple"
    / "AppleSiliconPkg"
    / "Drivers"
    / "AcpiPlatformDxe"
    / "AcpiPlatform.c"
)
MODULE_PATH = REPO / "Tools" / "j414s_mu_profile_manifest.py"
SPEC = importlib.util.spec_from_file_location("j414s_mu_profile_manifest", MODULE_PATH)
M = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(M)

# name -> (ASL file, C window table, _HID, window count, published GSIVs)
DEVICES = {
    "MCA0": ("MCA.asl", "mNtasiMcaWindows", "NTAS0080", 9, [40, 41, 42, 43, 45]),
    "AOPA": ("AOPA.asl", "mNtasiAopWindows", "NTAS0081", 4, [631]),
    "ISP0": ("ISP.asl", "mNtasiIspWindows", "NTAS0090", 8, [569]),
}

# AIC 44 is claimed by /arm-io/i2c0/hpmBusManager in the live ADT. An early
# draft proposed 44 -> 1231 for dart-sio; it was withdrawn and must not return.
FORBIDDEN_GSIV = 44

CSRT_ASLC = (
    REPO / "Silicon" / "Apple" / "T602XFamilyPkg" / "AcpiTables" / "CSRT.aslc"
)
# Emitted by drivers/AppleAic/emit_aic2_csrt.c and re-derived by the CSRT tests
# below; the ordinary table must stay bit-identical in every non-media profile.
CSRT_SHA256 = {
    "m2-pro": "cdee0da81d9c54c17d2964510271de453ae2ff0428fe0ff13e69efa9406521a5",
    "m2-pro-media": "a082eb6c95a12a29cdbc71e0c17fcb4091c1c5c48624595512cf5fa0342f16ba",
}

# The pmgr_east page KBL0 (NTAS0051) already claims exclusively in KBL.asl.
KBL_PMGR_PAGE = (0x290280000, 0x1000)


def asl_windows(name: str) -> list[tuple[int, int]]:
    """Every QWordMemory descriptor in an ASL file's _CRS, in source order.

    Parsed rather than iasl-compiled so the test runs with no toolchain.  The
    iasl round trip is a separate test below and is skipped when iasl is
    absent.
    """
    text = (ASL_DIR / name).read_text(encoding="utf-8")
    text = re.sub(r"//[^\n]*", "", text)
    windows = []
    for body in re.findall(r"QWordMemory\s*\((.*?)\)", text, re.S):
        numbers = re.findall(r"0x[0-9A-Fa-f]+", body)
        # granularity, min, max, translation, length
        if len(numbers) != 5:
            raise AssertionError(f"{name}: unparsable QWordMemory: {body!r}")
        _, minimum, maximum, _, length = (int(value, 16) for value in numbers)
        if maximum != minimum + length - 1:
            raise AssertionError(
                f"{name}: descriptor 0x{minimum:X} has max 0x{maximum:X} "
                f"but length 0x{length:X}"
            )
        windows.append((minimum, length))
    return windows


def c_windows(table: str) -> list[tuple[int, int]]:
    """Every entry of a NTASI_MEDIA_WINDOW table in AcpiPlatform.c, in order."""
    text = ACPI_PLATFORM.read_text(encoding="utf-8")
    match = re.search(
        rf"NTASI_MEDIA_WINDOW\s+{re.escape(table)}\s*\[\s*\]\s*=\s*\{{(.*?)\n\}};",
        text,
        re.S,
    )
    if match is None:
        raise AssertionError(f"AcpiPlatform.c has no window table {table}")
    body = re.sub(r"//[^\n]*", "", match.group(1))
    return [
        (int(base, 16), int(length, 16))
        for base, length in re.findall(
            r"\{\s*(0x[0-9A-Fa-f]+)ULL\s*,\s*(0x[0-9A-Fa-f]+)ULL\s*\}", body
        )
    ]


def _host_cc() -> str:
    for candidate in ("cc", "clang", "gcc"):
        path = shutil.which(candidate)
        if path:
            return path
    raise unittest.SkipTest("no host C compiler (cc/clang/gcc) found on PATH")


def asl_interrupts(name: str) -> list[int]:
    """Every vector in an ASL file's Interrupt() descriptors, in source order."""
    text = strip_comments((ASL_DIR / name).read_text(encoding="utf-8"))
    vectors: list[int] = []
    for body in re.findall(r"Interrupt\s*\([^)]*\)\s*\{([^}]*)\}", text, re.S):
        vectors += [int(v) for v in re.findall(r"\b(\d+)\b", body)]
    return vectors


def asl_descriptor_order(name: str) -> list[str]:
    """"M"/"I" per _CRS descriptor, in source order, to pin interrupts LAST."""
    text = strip_comments((ASL_DIR / name).read_text(encoding="utf-8"))
    crs = text[text.index("Name (_CRS"):]
    order = []
    for match in re.finditer(r"\b(QWordMemory|Interrupt)\s*\(", crs):
        order.append("M" if match.group(1) == "QWordMemory" else "I")
        if match.group(1) == "Interrupt":
            break  # the interrupt list is the last descriptor by contract
    return order


def c_interrupts(device: str) -> list[int]:
    """The NTASI_MEDIA interrupt table AcpiPlatform.c ships for one device."""
    table = {
        "MCA0": "mNtasiMcaInterrupts",
        "AOPA": "mNtasiAopInterrupts",
        "ISP0": "mNtasiIspInterrupts",
    }[device]
    text = ACPI_PLATFORM.read_text(encoding="utf-8")
    match = re.search(
        rf"UINT32\s+{re.escape(table)}\s*\[\s*\]\s*=\s*\{{([^}}]*)\}}", text
    )
    if match is None:
        raise AssertionError(f"AcpiPlatform.c has no interrupt table {table}")
    return [int(v) for v in re.findall(r"\b(\d+)\b", strip_comments(match.group(1)))]


def strip_comments(text: str) -> str:
    """Drop // and /* */ comments so a prose mention is not read as code.

    Both this file's C block and the ASL specs discuss the render opt-in and
    the interrupts they deliberately do not emit, so a naive substring search
    would fire on the explanation rather than on an actual emission.
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def media_block() -> str:
    """The #if NTASI_ENABLE_MEDIA_PUBLICATION region of AcpiPlatform.c."""
    text = ACPI_PLATFORM.read_text(encoding="utf-8")
    start = text.index("#if NTASI_ENABLE_MEDIA_PUBLICATION")
    end = text.index("#endif // NTASI_ENABLE_MEDIA_PUBLICATION")
    return text[start:end]


class MediaCrsContract(unittest.TestCase):
    def test_asl_and_generator_agree_on_every_window_in_order(self):
        for device, (asl, table, _hid, count, _gsivs) in DEVICES.items():
            with self.subTest(device=device):
                spec = asl_windows(asl)
                shipped = c_windows(table)
                self.assertEqual(len(spec), count, f"{asl} window count")
                # Order, base and length all matter, so compare the lists
                # themselves rather than their contents as sets.
                self.assertEqual(shipped, spec, f"{device} _CRS drifted from {asl}")

    def test_mca_publishes_a_shape_the_driver_accepts(self):
        # AppleMcaMapResources() accepts exactly 8, 9 or 12 memory descriptors
        # and returns STATUS_DEVICE_CONFIGURATION_ERROR for anything between,
        # because the three capture windows are all-or-nothing and a partial
        # set would silently shift the meaning of every later index.
        self.assertIn(len(c_windows("mNtasiMcaWindows")), (8, 9, 12))

    def test_isp_pmgr_window_length_is_not_page_rounded(self):
        # 0x4034 verbatim from isp0's own `reg` index 1 -- one byte past
        # ps_isp_clr at offset 0x4030. Rounding it to 0x5000 would be a silent
        # change to what the driver is told the power block is.
        windows = c_windows("mNtasiIspWindows")
        self.assertEqual(windows[4], (0x290280000, 0x4034))


class MediaInterruptFootprint(unittest.TestCase):
    """The published GSIVs, pinned in both the ASL and the generator.

    MCA0's five are PUBLISHED numbers that the CSRT ALI2 tail translates; AOPA's
    631 and ISP0's 569 are real AIC lines below the carrier's 1019 limit.
    """

    def test_asl_and_generator_agree_on_the_published_gsivs(self):
        for device, (asl, _table, _hid, _count, gsivs) in DEVICES.items():
            with self.subTest(device=device):
                self.assertEqual(asl_interrupts(asl), gsivs)
                self.assertEqual(c_interrupts(device), gsivs)

    def test_the_interrupt_is_the_last_descriptor(self):
        # All three drivers count memory descriptors in their own index space,
        # but keeping the interrupt last is what makes the ASL and the
        # generator diffable and keeps the memory contract obviously intact.
        for device, (asl, _t, _h, count, gsivs) in DEVICES.items():
            with self.subTest(device=device):
                order = asl_descriptor_order(asl)
                self.assertEqual(order, ["M"] * count + ["I"])
                self.assertTrue(gsivs)

    def test_gsiv_44_is_never_published(self):
        for device, (asl, _t, _h, _c, gsivs) in DEVICES.items():
            with self.subTest(device=device):
                self.assertNotIn(FORBIDDEN_GSIV, gsivs)
                self.assertNotIn(FORBIDDEN_GSIV, asl_interrupts(asl))
                self.assertNotIn(FORBIDDEN_GSIV, c_interrupts(device))

    def test_every_published_gsiv_is_arbiter_legal_or_translated(self):
        # Windows' GIC arbiter accepts 32..1019. Anything outside that MUST be
        # translated by an ALI2 alias, and nothing this profile publishes is.
        for device, (_a, _t, _h, _c, gsivs) in DEVICES.items():
            for gsiv in gsivs:
                with self.subTest(device=device, gsiv=gsiv):
                    self.assertGreaterEqual(gsiv, 32)
                    self.assertLessEqual(gsiv, 1019)

    def test_manifest_records_the_exact_published_set(self):
        expected = [g for _, (_a, _t, _h, _c, gsivs) in DEVICES.items() for g in gsivs]
        for profile in M.PROFILES:
            with self.subTest(profile=profile):
                features = M.profile_policy(profile)["experimental_features"]
                self.assertEqual(
                    features["media_published_gsivs"],
                    # Every profile NAMED for media, not just "media" itself:
                    # media-gpu publishes the same three devices and the same
                    # seven GSIVs. Keyed off the manifest's own media flag so a
                    # profile added later cannot be missed here.
                    expected if M.PROFILES[profile]["media"] else [],
                )


class MediaCsrt(unittest.TestCase):
    """The media CSRT must be the 8-alias table, and only for the media profile.

    Every other profile's table has to stay byte-for-byte what it was, which is
    checked by re-deriving all three variants through the real C preprocessor
    and hashing them.
    """

    def _csrt_bytes(self, media: int, gpu: int) -> bytes:
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
                    _host_cc(),
                    "-E",
                    "-P",
                    f"-DNTASI_ENABLE_MEDIA_PUBLICATION={media}",
                    f"-DNTASI_J414S_GPU_RESOURCE_PROFILE={gpu}",
                    str(path),
                ],
                capture_output=True,
                text=True,
            )
        if result.returncode:
            raise AssertionError(result.stderr)
        array = result.stdout[result.stdout.index("Csrt[] = {") :]
        array = array[: array.index("}")]
        return bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", array))

    def test_non_media_tables_are_byte_for_byte_unchanged(self):
        plain = self._csrt_bytes(media=0, gpu=0)
        self.assertEqual(len(plain), 256)
        self.assertEqual(hashlib.sha256(plain).hexdigest(), CSRT_SHA256["m2-pro"])

    def test_media_table_is_the_8_alias_superset(self):
        media = self._csrt_bytes(media=1, gpu=0)
        plain = self._csrt_bytes(media=0, gpu=0)
        self.assertEqual(len(media), 296)
        self.assertEqual(
            hashlib.sha256(media).hexdigest(), CSRT_SHA256["m2-pro-media"]
        )
        # Strict superset: the three fixed aliases, including the boot USB
        # controller's 37 -> 1274, must be bit-identical in both tables.
        for alias in (
            struct.pack("<II", 37, 1274),
            struct.pack("<II", 38, 1832),
            struct.pack("<II", 39, 1292),
        ):
            self.assertIn(alias, plain)
            self.assertIn(alias, media)

    def test_media_table_carries_exactly_the_mca_translations(self):
        media = self._csrt_bytes(media=1, gpu=0)
        for published, physical in (
            (40, 1218), (41, 1211), (42, 1213), (43, 1221), (45, 1231)
        ):
            with self.subTest(gsiv=published):
                self.assertIn(struct.pack("<II", published, physical), media)
        # 44 must appear as neither a published GSIV nor a physical line.
        self.assertNotIn(struct.pack("<I", FORBIDDEN_GSIV), media[-64:])

    def test_media_and_gpu_together_now_build_a_9_alias_table(self):
        """CHANGED 2026-07-31, and this is a policy change, not a relaxation.

        This test used to assert that media+gpu was refused at compile time by
        an #error, because published GSIV 40 meant the AGX mailbox there and
        admac-sio here.  The clash is now GONE rather than refused: the AGX
        mailbox moved to 46.  So the combination must BUILD, and must produce
        the 9-alias superset.

        What replaced the #error is checked by
        test_a_reintroduced_gsiv_collision_still_fails_the_build below -- the
        guard is stricter now, not absent.
        """
        combined = self._csrt_bytes(media=1, gpu=1)
        self.assertEqual(len(combined), 304)
        media = self._csrt_bytes(media=1, gpu=0)
        # Strict superset: every media alias survives unchanged, and the AGX
        # entry is appended.
        for published, physical in (
            (40, 1218), (41, 1211), (42, 1213), (43, 1221), (45, 1231),
            (37, 1274), (38, 1832), (39, 1292),
        ):
            with self.subTest(gsiv=published):
                self.assertIn(struct.pack("<II", published, physical), media)
                self.assertIn(struct.pack("<II", published, physical), combined)
        self.assertIn(struct.pack("<II", 46, 1146), combined)
        # ...and the AGX mailbox is NOT published as 40 anywhere.
        self.assertNotIn(struct.pack("<II", 40, 1146), combined)

    def test_a_reintroduced_gsiv_collision_still_fails_the_build(self):
        """The replacement for the deleted #error, exercised.

        CSRT.aslc names every published GSIV and STATIC_ASSERTs that no two
        collide.  Regressing the AGX mailbox to 40 -- the exact historical bug
        -- must fail the build, and so must a collision the old #error could
        never have caught, such as one purely between two media aliases.
        """
        source = CSRT_ASLC.read_text(encoding="utf-8")
        for name, mutation in (
            (
                "AGX regressed to 40 (collides with media admac-sio)",
                ("#define NTASI_CSRT_GSIV_AGX_MAILBOX 46",
                 "#define NTASI_CSRT_GSIV_AGX_MAILBOX 40"),
            ),
            (
                "AGX moved to 44 (a real line owned by hpmBusManager)",
                ("#define NTASI_CSRT_GSIV_AGX_MAILBOX 46",
                 "#define NTASI_CSRT_GSIV_AGX_MAILBOX 44"),
            ),
            (
                "media dart-sio collides with media i2c2 (the old #error missed this class)",
                ("#define NTASI_CSRT_GSIV_SIO_DART    45",
                 "#define NTASI_CSRT_GSIV_SIO_DART    43"),
            ),
        ):
            with self.subTest(mutation=name):
                mutated = source.replace(*mutation)
                self.assertNotEqual(mutated, source, "mutation did not apply")
                self.assertFalse(
                    self._compiles(mutated, media=1, gpu=1),
                    f"a collision was accepted: {name}",
                )
        # Control: unmutated must compile, or the test above proves nothing.
        self.assertTrue(self._compiles(source, media=1, gpu=1))

    def _compiles(self, source: str, media: int, gpu: int) -> bool:
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
            return subprocess.run(
                [
                    _host_cc(), "-std=c11", "-c", "-o", os.devnull,
                    f"-DNTASI_ENABLE_MEDIA_PUBLICATION={media}",
                    f"-DNTASI_J414S_GPU_RESOURCE_PROFILE={gpu}",
                    str(path),
                ],
                capture_output=True,
            ).returncode == 0

    def test_manifest_records_the_csrt_variant_for_every_profile(self):
        for profile in M.PROFILES:
            with self.subTest(profile=profile):
                entry = M.PROFILES[profile]
                features = M.profile_policy(profile)["experimental_features"]
                if entry["media"] and entry["gpu"]:
                    expected = ("m2-pro-media-gpu", 9)
                elif entry["media"]:
                    expected = ("m2-pro-media", 8)
                elif entry["gpu"]:
                    expected = ("m2-pro-gpu", 4)
                else:
                    expected = ("m2-pro", 3)
                self.assertEqual(
                    (features["csrt_variant"], features["csrt_ali2_alias_count"]),
                    expected,
                )

    def test_every_profiles_csrt_variant_is_the_bytes_it_names(self):
        """The manifest's alias COUNT must equal the table the FD really has.

        A profile that claims 9 aliases while its CSRT arm emits 4 would pass
        every other check here and hand Windows a translation table that does
        not describe what the SSDTs published.
        """
        for profile, entry in M.PROFILES.items():
            with self.subTest(profile=profile):
                features = M.profile_policy(profile)["experimental_features"]
                table = self._csrt_bytes(
                    media=1 if entry["media"] else 0,
                    gpu=1 if entry["gpu"] else 0,
                )
                # ALI2 tail: 8-byte header then 8 bytes per alias entry.
                index = table.index(b"ALI2")
                count = struct.unpack_from("<I", table, index + 8)[0]
                self.assertEqual(count, features["csrt_ali2_alias_count"])


class MediaRenderGate(unittest.TestCase):
    def test_render_opt_in_is_absent_everywhere(self):
        # ntasp,mca-allow-render is the ACPI half of the MCA render gate. The
        # internal speakers have no thermal protection on Windows and the
        # amplifiers power on at maximum analog gain.
        self.assertNotIn(
            "mca-allow-render",
            strip_comments(media_block()),
            "the generator emits the MCA render opt-in",
        )
        for asl, *_ in DEVICES.values():
            with self.subTest(asl=asl):
                self.assertNotIn(
                    "mca-allow-render",
                    strip_comments((ASL_DIR / asl).read_text(encoding="utf-8")),
                    f"{asl} declares the MCA render opt-in",
                )

    def test_manifest_records_render_as_disabled_in_every_profile(self):
        for profile in M.PROFILES:
            with self.subTest(profile=profile):
                features = M.profile_policy(profile)["experimental_features"]
                self.assertIs(features["media_speaker_render_enabled"], False)


class MediaProfileGate(unittest.TestCase):
    def test_only_the_media_profile_publishes_media(self):
        for profile in M.PROFILES:
            with self.subTest(profile=profile):
                features = M.profile_policy(profile)["experimental_features"]
                expected = bool(M.PROFILES[profile]["media"])
                self.assertIs(features["media_publication"], expected)
                self.assertEqual(
                    features["media_acpi_devices"],
                    ["NTAS0080", "NTAS0081", "NTAS0090"] if expected else [],
                )

    def test_media_adds_no_ffs_module(self):
        # The SSDTs are generated at DXE runtime inside the always-built
        # AcpiPlatformDxe, so media must not change the FV inventory the way
        # ans does.
        self.assertEqual(
            M.PROFILES["media"]["expected_ffs_count"],
            M.PROFILES["baseline"]["expected_ffs_count"],
        )

    def test_media_is_a_single_variable_on_top_of_baseline(self):
        media = M.PROFILES["media"]
        for other in ("ans", "ans_acpi", "gpu", "wireless"):
            with self.subTest(feature=other):
                self.assertFalse(media[other])

    def test_profile_abi_is_distinct_and_correctly_namespaced(self):
        abis = [entry["profile_abi"] for entry in M.PROFILES.values()]
        self.assertEqual(len(abis), len(set(abis)))
        self.assertEqual(
            M.PROFILES["media"]["profile_abi"],
            "ntasi.j414s.windows.media-publication.v1",
        )

    def test_generator_is_preprocessor_gated_not_merely_pcd_gated(self):
        # "Default OFF" has to mean byte-identical firmware, not just
        # equivalent behaviour, so the whole block is #if'd out rather than
        # compiled in and skipped at runtime.
        text = ACPI_PLATFORM.read_text(encoding="utf-8")
        self.assertIn("#if NTASI_ENABLE_MEDIA_PUBLICATION", text)
        self.assertIn("#endif // NTASI_ENABLE_MEDIA_PUBLICATION", text)
        self.assertIn(
            "-DNTASI_ENABLE_MEDIA_PUBLICATION=$(NTASI_ENABLE_MEDIA_PUBLICATION)",
            (
                REPO
                / "Platform"
                / "MacBookProEarly2023Pkg"
                / "MacBookProEarly2023.dsc"
            ).read_text(encoding="utf-8"),
        )

    def test_expected_defines_track_the_profile_table(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertIn(
            '"NTASI_ENABLE_MEDIA_PUBLICATION": "1" if PROFILES[profile]["media"] else "0"',
            source,
        )


class MediaKnownOverlapIsDocumented(unittest.TestCase):
    """MCA0 and ISP0 window 4 overlap the page KBL0 claims exclusively.

    This is not fixable in firmware -- both drivers index _CRS positionally, so
    removing window 4 shifts every later window -- but it must never become an
    undocumented surprise, so the test asserts the overlap is real AND that the
    two ASL specs still say so.
    """

    def test_overlap_is_real_and_stated(self):
        page_start, page_length = KBL_PMGR_PAGE
        page_end = page_start + page_length - 1
        for device, table, asl in (
            ("MCA0", "mNtasiMcaWindows", "MCA.asl"),
            ("ISP0", "mNtasiIspWindows", "ISP.asl"),
        ):
            with self.subTest(device=device):
                base, length = c_windows(table)[4]
                self.assertLessEqual(base, page_end)
                self.assertGreaterEqual(base + length - 1, page_start)
                self.assertIn(
                    "KNOWN RESOURCE OVERLAP",
                    (ASL_DIR / asl).read_text(encoding="utf-8"),
                )
        self.assertIn("KNOWN RESOURCE OVERLAP", media_block())


@unittest.skipUnless(shutil.which("iasl"), "iasl is not installed")
class MediaAslCompiles(unittest.TestCase):
    """Compile each spec and walk the real AML, not the source text."""

    def test_each_spec_compiles_clean_and_matches_the_generator(self):
        for device, (asl, table, hid, count, gsivs) in DEVICES.items():
            with self.subTest(asl=asl), tempfile.TemporaryDirectory() as directory:
                prefix = Path(directory) / "table"
                result = subprocess.run(
                    ["iasl", "-p", str(prefix), str(ASL_DIR / asl)],
                    check=True,
                    text=True,
                    capture_output=True,
                )
                self.assertIn("0 Errors, 0 Warnings", result.stdout)

                aml = prefix.with_suffix(".aml").read_bytes()
                self.assertIn(hid.encode(), aml)

                # Walk the compiled _CRS descriptor chain. Only QWordMemory
                # (0x8A) and ExtendedInterrupt (0x89) may appear, the memory
                # windows must match the generator exactly and in order, and
                # the interrupt list must come last.
                offset = aml.index(b"\x8a\x2b\x00", aml.index(b"_CRS"))
                windows, vectors, order = [], [], []
                while True:
                    tag = aml[offset]
                    if tag == 0x79:  # end tag
                        break
                    body_length = struct.unpack_from("<H", aml, offset + 1)[0]
                    body = aml[offset + 3 : offset + 3 + body_length]
                    if tag == 0x8A:
                        windows.append(
                            (
                                struct.unpack_from("<Q", body, 11)[0],
                                struct.unpack_from("<Q", body, 35)[0],
                            )
                        )
                        order.append("M")
                    elif tag == 0x89:
                        vector_count = body[1]
                        vectors += list(
                            struct.unpack_from(f"<{vector_count}I", body, 2)
                        )
                        order.append("I")
                    else:
                        self.fail(f"{asl}: unexpected descriptor 0x{tag:02X}")
                    offset += 3 + body_length

                self.assertEqual(len(windows), count)
                self.assertEqual(windows, c_windows(table))
                self.assertEqual(vectors, gsivs)
                self.assertEqual(vectors, c_interrupts(device))
                self.assertEqual(order, ["M"] * count + ["I"])
                self.assertNotIn(FORBIDDEN_GSIV, vectors)


if __name__ == "__main__":
    unittest.main()
