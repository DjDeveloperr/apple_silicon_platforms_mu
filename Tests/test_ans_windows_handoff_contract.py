"""Host-side safety contract for Mu -> Windows Apple ANS ownership."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / (
    "Silicon/Apple/AppleSiliconPkg/Drivers/AppleNANDStorageDxe/"
    "AppleNANDStorageDxe.c"
)


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"function {name} not found")
    start = source.index("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : index]
    raise AssertionError(f"unterminated function {name}")


class AnsWindowsHandoffContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.driver = DRIVER.read_text(encoding="utf-8")

    def test_preserve_profile_uses_reserved_controller_dma_memory(self) -> None:
        allocate = function_body(self.driver, "AllocateQueueMemory")
        self.assertRegex(
            allocate,
            r"MemoryType\s*=\s*FixedPcdGetBool\s*\(PcdAppleAnsPreserveForOs\)"
            r"\s*\?\s*EfiReservedMemoryType\s*:\s*EfiBootServicesData",
        )
        self.assertIn("MemoryType,", allocate)

    def test_ebs_quiesces_nvme_but_preserves_live_rtkit(self) -> None:
        handoff = function_body(self.driver, "AnsExitBootServices")
        preserve_start = handoff.index(
            "if (FixedPcdGetBool (PcdAppleAnsPreserveForOs)"
        )
        preserve_end = handoff.index("return;", preserve_start)
        preserve = handoff[preserve_start:preserve_end]

        self.assertIn("ntasi_ans_controller_stop", preserve)
        self.assertIn("ArmDataSynchronizationBarrier", preserve)
        self.assertNotIn("ntasi_rtkit_runtime_handoff", preserve)
        self.assertNotIn("ntasi_rtkit_runtime_release_buffers", preserve)
        self.assertNotIn("ntasi_sart_runtime_clear_owned", preserve)
        self.assertNotIn("ntasi_asc_cpu_stop", preserve)

    def test_live_handoff_reserves_inherited_sart_dma_memory(self) -> None:
        reserve = function_body(self.driver, "AnsReserveInheritedSartMemory")
        self.assertIn("Device->Sart.protected_entries", reserve)
        self.assertIn("ntasi_sart_runtime_read", reserve)
        self.assertIn("RUNTIME_PAGE_ALLOCATION_GRANULARITY", reserve)
        self.assertIn("Descriptor->Type != EfiConventionalMemory", reserve)
        self.assertIn("outside the UEFI RAM map", reserve)
        self.assertIn("already unavailable to Windows", reserve)
        self.assertIn("Reservations[ReservationCount]", reserve)
        self.assertIn("AllocateAddress", reserve)
        self.assertIn("EfiReservedMemoryType", reserve)
        self.assertIn("EFI_ACCESS_DENIED", reserve)
        self.assertNotIn("ntasi_sart_runtime_clear_owned", reserve)
        self.assertNotIn("ntasi_sart_runtime_close_all", reserve)

        entry = function_body(self.driver, "AppleNANDStorageDxeInitialize")
        reserve_call = entry.index("AnsReserveInheritedSartMemory (Device)")
        rtkit_boot = entry.index("ntasi_rtkit_runtime_boot")
        self.assertLess(reserve_call, rtkit_boot)

        handoff = function_body(self.driver, "AnsExitBootServices")
        preserve_start = handoff.index(
            "if (FixedPcdGetBool (PcdAppleAnsPreserveForOs)"
        )
        preserve_end = handoff.index("return;", preserve_start)
        preserve_guard = handoff[preserve_start:preserve_end]
        self.assertIn("Device->InheritedSartMemoryReserved", preserve_guard)
        self.assertIn("refusing live RTKit handoff", handoff)
        self.assertIn("ntasi_sart_runtime_close_all", handoff)

    def test_ready_to_boot_diagnostic_is_bounded_and_read_only(self) -> None:
        ready = function_body(self.driver, "AnsReadyToBoot")
        self.assertIn("APPLE_ANS_MAX_GPT_PARTITIONS", ready)
        self.assertIn("BlockIo->ReadBlocks", ready)
        self.assertNotIn("WriteBlocks", ready)
        self.assertNotIn("SetVariable", ready)
        self.assertIn("Device->ReadAttributionArmed = TRUE", ready)
        self.assertIn("Event == Device->PartitionInfoEvent", ready)
        self.assertIn("Event == Device->SimpleFileSystemEvent", ready)
        self.assertIn("BcdAudited && WindowsPartitionPresent", ready)

        entry = function_body(self.driver, "AppleNANDStorageDxeInitialize")
        self.assertIn("gEfiEventReadyToBootGuid", entry)
        self.assertIn("gEfiPartitionInfoProtocolGuid", entry)
        self.assertIn("gEfiSimpleFileSystemProtocolGuid", entry)
        self.assertEqual(entry.count("RegisterProtocolNotify"), 2)

        bcd = function_body(self.driver, "AnsAuditBcdReferences")
        self.assertIn("APPLE_ANS_MAX_BCD_BYTES + 1u", bcd)
        self.assertIn("EFI_FILE_MODE_READ", bcd)
        self.assertIn("\\\\EFI\\\\Microsoft\\\\Boot\\\\BCD", bcd)
        self.assertIn("UniquePartitionGuid", bcd)
        self.assertIn("winload.efi", bcd)
        self.assertIn("AnsFindGuidReferences", bcd)
        self.assertIn("disk-guid-binary", bcd)
        self.assertIn("disk-guid-text", bcd)
        self.assertNotIn("Write", bcd)
        self.assertNotIn("SetVariable", bcd)

        guid_scan = function_body(self.driver, "AnsFindGuidReferences")
        self.assertIn("AnsBufferContains", guid_scan)
        self.assertIn("AnsBufferContainsUtf16Ascii", guid_scan)
        self.assertIn('AsciiSPrint (GuidText, sizeof (GuidText), "%g", Guid)', guid_scan)


if __name__ == "__main__":
    unittest.main()
