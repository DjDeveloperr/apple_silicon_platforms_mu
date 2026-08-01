#!/usr/bin/env python3
"""Seal and verify commit-scoped J414s Windows Mu profile artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


SCHEMA = "ntasi.j414s.mu-profile.v2"
BRANCH = "main"
PLATFORM_BUILD = "MacBookProEarly2023-AARCH64/DEBUG_CLANGPDB"
FD_NAME = "MACBOOKPROEARLY2023_EFI.fd"
PROFILES = {
    "baseline": {
        "profile_abi": "ntasi.j414s.windows.baseline.v1",
        "ans": False,
        "gpu": False,
        "expected_ffs_count": 87,
    },
    "ans": {
        "profile_abi": "ntasi.j414s.windows.ans-readonly.v1",
        "ans": True,
        "gpu": False,
        "expected_ffs_count": 88,
    },
    # CORRECTED 2026-07-30: the gpu profile used to add its own FFS
    # (GpuAcpiTables.inf compiling a static GPU.asl). That table was NEVER
    # installed -- its FFS GUID was not one of the four
    # Pcd*AcpiTableStorageFile GUIDs AcpiPlatformDxe reads -- while its _CRS
    # hardcoded hw_data_a inside OS RAM, over the exact SP_EL1 that crashed
    # Mu's PEI twice. Both the file and the INF are deleted. Selecting "gpu"
    # now changes exactly one thing a static build can prove: the
    # NTASI_J414S_GPU_RESOURCE_PROFILE compiler define, which gates the
    # ADT-derived, DRAM-bounded GCD carveout reservations in AcpiPlatformDxe.
    # Like "wireless", it therefore adds no FFS module and its
    # expected_ffs_count equals baseline's. See
    # NtasiReportGpuPublicationDecision() in AcpiPlatform.c for the explicit
    # decision not to publish NTAS0023 yet and the condition that unblocks it.
    # SINGLE-VARIABLE CONTROL, 2026-07-30. Identical FFS set to "ans" -- the
    # AppleNANDStorageDxe module is still in the FV -- but NTAS2003 is never
    # published, so Windows never builds a devnode for it and its PnP resource
    # arbiter never allocates an interrupt or the four 4-byte PMGR memory
    # ranges for it. This isolates "the ACPI device and its resources" from
    # "the ANS driver exists in the firmware image".
    #
    # Needed because the `ans` profile at bde9ff10 -- where the entire ANS
    # hardware mutation path is dead-stripped from the binary -- still
    # bugchecked BUGCODE_USB3_DRIVER 0x144, exonerating ANS hardware mutation
    # and leaving only the ACPI device and the FD layout as variables.
    "ans-noacpi": {
        "profile_abi": "ntasi.j414s.windows.ans-driver-no-acpi-control.v1",
        "ans": True,
        "ans_acpi": False,
        "gpu": False,
        "expected_ffs_count": 88,
    },
    "gpu": {
        "profile_abi": "ntasi.j414s.windows.gpu-resource-probe.v1",
        "ans": False,
        "gpu": True,
        "expected_ffs_count": 87,
    },
    "ans-gpu": {
        "profile_abi": "ntasi.j414s.windows.ans-gpu-combined.v1",
        "ans": True,
        "gpu": True,
        "expected_ffs_count": 88,
    },
    # CORRECTED 2026-07-30: wireless used to be its own optional FFS
    # (WirelessDartAcpiTables.inf, compiling a static WDRT.asl that baked a
    # same-instance hardware-captured reservation address at build time --
    # exactly the hardcoding the end user rejected). DRT0 is now generated
    # dynamically at DXE runtime by NtasiInstallWirelessDartTable() inside
    # AcpiPlatformDxe (the same module and the same technique
    # AcpiPlatformInstallAppleAnsTable() already uses for ANS's SSDT), and
    # the reservation base/size are derived by PEI from that boot's own
    # boot_args -- never baked into this static build artifact. Selecting
    # "wireless" therefore changes exactly one thing a static build can
    # prove: the NTASI_ENABLE_WIRELESS_DART_HANDOFF compiler define. It adds
    # no new FFS module (unlike ans/gpu, which each add their own driver or
    # ACPI-table FFS), so its expected_ffs_count equals baseline's.
    "wireless": {
        "profile_abi": "ntasi.j414s.windows.wireless-handoff-v2-runtime-derived.v1",
        "ans": False,
        "gpu": False,
        "wireless": True,
        "expected_ffs_count": 87,
    },
    # ANS is deliberately excluded here. Measured on hardware 2026-07-30:
    # baseline and gpu both boot Windows and stay up, while ans-gpu bugchecks
    # BUGCODE_USB3_DRIVER (0x144) with XHC1 halted on HOST SYSTEM ERROR and a
    # DWC3 bus error -- with the port power rails DOWN and XHC2 disabled, so
    # neither of those is the cause. gpu-wireless exists to exercise the
    # wireless handoff without dragging ANS in alongside it.
    # Like gpu and wireless, it adds no FFS module of its own.
    "gpu-wireless": {
        "profile_abi": "ntasi.j414s.windows.gpu-wireless-combined.v1",
        "ans": False,
        "gpu": True,
        "wireless": True,
        "expected_ffs_count": 87,
    },
    "ans-gpu-wireless": {
        "profile_abi": "ntasi.j414s.windows.ans-gpu-wireless-battery.v1",
        "ans": True,
        "gpu": True,
        # 2026-08-01: pinned False to isolate AppleAgxGpu 0.6.0.0, which now
        # really powers the GPU domain and bugchecks the boot. Keep in step
        # with PlatformBuild.py profile_values.
        "gpu_acpi": False,
        "wireless": True,
        # Battery (NTAS0053) added 2026-07-31. It publishes NO GSIV and NO
        # memory window -- SMCG (NTAS0052) holds the SMC ASC/SRAM ranges
        # exclusively, and a second claimant is the CM_PROB_NORMAL_CONFLICT
        # this design exists to avoid -- so the CSRT and the GSIV allocation
        # are byte-identical to the same profile without it. Keep in step with
        # PlatformBuild.py profile_values; they are two independent dicts.
        # profile_abi is bumped because the published device set changed, which
        # is what a sealed manifest is for.
        "battery": True,
        # gpu_acpi is INHERITED from "gpu" again (True), and this comment
        # records why it was briefly pinned False and why that is no longer
        # needed. Keep this key in step with the same key in
        # Platform/MacBookProEarly2023Pkg/PlatformBuild.py profile_values --
        # they are two independent dicts and only one of them reaches the
        # compiler.
        #
        # 2026-07-31: build 3450262 turned NTAS0023 publication on for the
        # first time AND restored XHC2 _STA in the same image, and the boot was
        # read as "Mu emitted nothing at all". It did emit: 146,790 bytes
        # landed in build/m2-pro-readiness/logs/mu-secondary-uart.log between
        # offsets 121015323 and 121162113, ending in
        #
        #   AppleAgxGpu: stage "allocate-placeholder-handoff"
        #   ASSERT_EFI_ERROR (Status = Invalid Parameter)
        #   ASSERT [AcpiPlatform] MemoryAllocationLib.c(222): ...
        #
        # The 0-byte file that was actually read (mu-<stamp>.log) is
        # run_guest's primary trace, which is 0 bytes on every run including
        # the successful ones. The cause was AllocateAlignedReservedPages()
        # deadlooping on AArch64 reserved memory, not the publication itself;
        # see NtasiGpuAllocatePlaceholderHandoff() in AcpiPlatform.c. With that
        # fixed there is no reason to withhold publication from this profile,
        # and gpu-noacpi remains the control that isolates it.
        "expected_ffs_count": 88,
    },
    # Media publication: MCA0 (NTAS0080, speakers + headset jack), AOPA
    # (NTAS0081, internal PDM mic array) and ISP0 (NTAS0090, FaceTime camera).
    #
    # Selecting "media" changes exactly one thing a static build can prove: the
    # NTASI_ENABLE_MEDIA_PUBLICATION compiler define, which gates the whole
    # NtasiInstallMediaTables() block in AcpiPlatformDxe. Like gpu and wireless
    # it adds NO FFS module -- the three SSDTs are built with AmlLib at DXE
    # runtime, so this static inventory can no more see them than it can see
    # ANS0 or DRT0 -- and its expected_ffs_count therefore equals baseline's.
    #
    # ZERO INTERRUPTS. Not one Interrupt() descriptor is published for any of
    # the three devices and not one CSRT byte changes: the CSRT emitted for
    # m2-pro is byte-for-byte what it is today, with the same three ALI2
    # entries (37->1274 XHC1, 38->1832 ANS, 39->1292 XHC2), because that table
    # is built unconditionally and no profile flag reaches it. All three
    # drivers reach first light by polling, by design. See
    # apple_silicon_nt_drivers/docs/j414s-media-gsiv-allocation.md section 3.
    #
    # Deliberately NOT combined with ans/gpu/wireless. Measured on hardware
    # 2026-07-30, baseline and gpu are the configurations that boot Windows and
    # stay up; media is run as a single variable on top of baseline so a result
    # is attributable.
    # Everything at once: ANS + GPU + wireless + media, with XHC2 enabled in
    # the DSDT.  Requested explicitly 2026-07-31 after baseline and
    # ans-gpu-wireless both booted.
    #
    # media + gpu selects the "m2-pro-media-gpu" CSRT (9 ALI2 aliases: the 3
    # fixed + 5 MCA + the AGX mailbox 46 -> 1146), which already exists as an
    # emit_aic2_csrt.c fixture -- this profile adds no new CSRT variant.
    # expected_ffs_count is 88 because ANS is the only feature here that adds
    # an FFS module; gpu, wireless and media are all compiler defines plus
    # DXE-runtime AmlLib tables.
    #
    # KEEP IN STEP with profile_values in
    # Platform/MacBookProEarly2023Pkg/PlatformBuild.py.  Those are two
    # independent dicts and only that one reaches the compiler: on 2026-07-31
    # changing this one alone produced a byte-identical FD whose manifest
    # claimed a feature was off while it was compiled in.
    "ans-gpu-wireless-media": {
        "profile_abi": "ntasi.j414s.windows.ans-gpu-wireless-media-combined.v1",
        "ans": True,
        "gpu": True,
        "wireless": True,
        "media": True,
        "expected_ffs_count": 88,
    },
    "media": {
        "profile_abi": "ntasi.j414s.windows.media-publication.v1",
        "ans": False,
        "gpu": False,
        "media": True,
        "expected_ffs_count": 87,
    },
    # Media AND GPU together. This combination was IMPOSSIBLE before
    # 2026-07-31: CSRT.aslc #errored because the AGX mailbox and admac-sio both
    # claimed published GSIV 40. The AGX mailbox moved to 46 (proven free
    # against the pinned live ADT on all five allocation rules), so the two
    # alias sets are now disjoint on both sides and the 9-alias
    # "m2-pro-media-gpu" CSRT exists. Carried here so the combination is a
    # real, testable profile rather than a claim.
    "media-gpu": {
        "profile_abi": "ntasi.j414s.windows.media-gpu-combined.v1",
        "ans": False,
        "gpu": True,
        "media": True,
        "expected_ffs_count": 87,
    },
    # Single-variable control for NTAS0023, exactly like ans-noacpi is for
    # NTAS2003: the GPU carveouts are still reserved in the GCD and the
    # NTASI_J414S_GPU_RESOURCE_PROFILE code is still compiled in, but the ACPI
    # device is never published, so Windows never builds a devnode for it and
    # its PnP arbiter never allocates the eight memory ranges or GSIV 46.
    # This is what isolates "the ACPI device and its resources" from "the GPU
    # carveout reservation" if a GPU-profile boot regresses.
    "gpu-noacpi": {
        "profile_abi": "ntasi.j414s.windows.gpu-resource-no-acpi-control.v1",
        "ans": False,
        "gpu": True,
        "gpu_acpi": False,
        "expected_ffs_count": 87,
    },
    # Baseline plus BAT0 (NTAS0053), the devnode AppleSmcBattery.sys binds to,
    # and nothing else. Cheaper than every other experiment in this table: the
    # device publishes an EMPTY _CRS -- no memory window, no interrupt -- so it
    # allocates no GSIV, changes no CSRT byte, and cannot take a resource away
    # from a devnode that already boots. The SSDT is generated at DXE runtime
    # like ANS0/DRT0/media, so no FFS module and no static table is added.
    "battery": {
        "profile_abi": "ntasi.j414s.windows.battery-publication.v1",
        "ans": False,
        "gpu": False,
        "battery": True,
        "expected_ffs_count": 87,
    },
}
for _profile in PROFILES.values():
    _profile.setdefault("wireless", False)
    # Battery publication is opt-in per profile, defaulted here rather than
    # written into every entry so a profile added later cannot inherit an
    # enabled battery publication by omission.
    _profile.setdefault("battery", False)
    # Media publication is opt-in per profile. Defaulted here rather than
    # written into every entry so a profile added later cannot inherit an
    # enabled media publication by omission.
    _profile.setdefault("media", False)
    # NTAS2003 publication defaults to tracking driver presence; only the
    # ans-noacpi control decouples them.
    _profile.setdefault("ans_acpi", _profile["ans"])
    # NTAS0023 publication defaults to tracking the GPU carveout profile; only
    # the gpu-noacpi control decouples them. Defaulted rather than written into
    # every entry so a profile added later cannot inherit an enabled GPU
    # publication by omission.
    _profile.setdefault("gpu_acpi", _profile["gpu"])
REQUIRED_FFS = {
    "168D1A6E-F4A5-448A-9E95-795661BB3067": "ArmPciCpuIo2Dxe",
    "128FB770-5E79-4176-9E51-9BB268A17DD1": "PciHostBridgeDxe",
    "93B80004-9FB3-11D4-9A3A-0090273FC14D": "PciBusDxe",
    "71FD84CD-353B-464D-B7A4-6EA7B96995CB": "NonDiscoverablePciDeviceDxe",
    "B7F50E91-A759-412C-ADE4-DCD03E7F7C28": "XhciDxe",
    "240612B7-A063-11D4-9A3A-0090273FC14D": "UsbBusDxe",
    "9FB4B4A7-42C0-4BCD-8540-9BCC6711F83E": "UsbMassStorageDxe",
    "6B38F7B4-AD98-40E9-9093-ACA2B5A253C4": "DiskIoDxe",
    "1FA1F39E-FEFF-4AAE-BD7B-38A070A3B609": "PartitionDxe",
    "961578FE-B6B7-44C3-AF35-6BC705CD2B1F": "Fat",
    "8EF405FD-6B51-438F-93D4-255BF796BABC": "AppleAicDxe",
    "F7B773C7-660A-4DA6-B641-75E9EE06BD41": "AppleDartIoMmuDxe",
    "DCFD1E6D-788D-4FFC-8E1B-CA2F75651A92": "SimpleFbDxe",
    "15D3C0D1-346B-462D-A40C-ECC01F8299FA": "AppleEmbeddedGpioControllerDxe",
    "A7A8B3F7-B8BB-42FF-A5D1-07CE43734461": "AppleUsbTypeCBringupDxe",
    "28A03FF4-12B3-4305-A417-BB1A4F94081E": "RamDiskDxe",
    "69B5DBD8-92C0-492C-859F-7256D5100D2A": "BootRamdiskHelperDxe",
    "3FF4732C-9411-4E10-A10C-8B39DF282E83": "DeviceAcpiTables",
}
OPTIONAL_FFS = {
    "ans": "ACDA0196-4589-4E4F-B71D-E9F9C2B2EA3D",
    # The former GpuAcpiTables FFS (2CC5C83E-...) is gone, deleted with
    # GPU.asl on 2026-07-30. It is listed as FORBIDDEN rather than dropped so
    # that a build which somehow resurrects it fails the manifest instead of
    # quietly shipping the hardcoded hw_data_a address again.
    "gpu_forbidden_legacy": "2CC5C83E-BCA6-49D9-B435-D14DC31E62AE",
    "arm_gic": "DE371F7C-DEC4-4D21-ADF1-593ABCC15882",
}
ACPI_CONTAINERS = {
    "DSDT.aml": "3FF4732C-9411-4E10-A10C-8B39DF282E83",
    "MCFG.acpi": "3FF4732C-9411-4E10-A10C-8B39DF282E83",
    "DISP.aml": "3FF4732C-9411-4E10-A10C-8B39DF282E83",
    "KBL.aml": "3FF4732C-9411-4E10-A10C-8B39DF282E83",
    "MTP.aml": "3FF4732C-9411-4E10-A10C-8B39DF282E83",
    "SMCG.aml": "3FF4732C-9411-4E10-A10C-8B39DF282E83",
    "CSRT.acpi": "D1430D86-24A4-4C2F-8F22-D24376E2E888",
}
BASE_ACPI = ("DSDT.aml", "MCFG.acpi", "DISP.aml", "KBL.aml", "MTP.aml", "SMCG.aml", "CSRT.acpi")
NESTED_LOCK = Path("Tools/J414S_NESTED_GITLINK_LOCK.json")
LEGACY_PATH_PATTERNS = (
    re.compile(r"/Users/[^\s\"']+/Developer/mu-j414s-(?!windows-unified)"),
    re.compile(r"/Users/[^\s\"']+/Developer/m1n1-j414s-"),
    re.compile(r"\.git/worktrees/"),
)


class ManifestError(RuntimeError):
    """A fail-closed manifest validation error."""


def require_keys(value: Any, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        raise ManifestError(f"{label} keys do not match the v2 contract")


def run(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(
        args,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ManifestError(f"command failed ({' '.join(args)}): {detail}")
    return result.stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def file_record(path: Path, output_root: Path) -> dict[str, Any]:
    if not path.is_file():
        raise ManifestError(f"missing required artifact: {path}")
    try:
        relative = path.relative_to(output_root).as_posix()
    except ValueError as error:
        raise ManifestError(f"artifact escapes output root: {path}") from error
    return {"path": relative, "size": path.stat().st_size, "sha256": sha256(path)}


def verify_file_record(record: dict[str, Any], output_root: Path, label: str) -> Path:
    if set(record) != {"path", "size", "sha256"}:
        raise ManifestError(f"{label} has an invalid file record")
    relative = Path(record["path"])
    if relative.is_absolute() or ".." in relative.parts:
        raise ManifestError(f"{label} path is not output-root relative")
    path = (output_root / relative).resolve()
    try:
        path.relative_to(output_root.resolve())
    except ValueError as error:
        raise ManifestError(f"{label} resolves outside the output root") from error
    if not path.is_file() or path.stat().st_size != record["size"]:
        raise ManifestError(f"{label} size/path mismatch")
    if sha256(path) != record["sha256"]:
        raise ManifestError(f"{label} SHA-256 mismatch")
    return path


def materialized_tree_sha256(root: Path) -> str:
    if not root.is_dir():
        raise ManifestError(f"missing materialized gitlink: {root}")
    digest = hashlib.sha256()
    files = sorted(
        path for path in root.rglob("*")
        if ".git" not in path.relative_to(root).parts and not path.is_dir()
    )
    for path in files:
        relative = path.relative_to(root).as_posix().encode("utf-8")
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode):
            mode = b"120000"
            content = os.readlink(path).encode("utf-8")
        elif stat.S_ISREG(info.st_mode):
            mode = b"100755" if info.st_mode & stat.S_IXUSR else b"100644"
            content = path.read_bytes()
        else:
            raise ManifestError(f"unsupported materialized tree entry: {path}")
        digest.update(mode + b"\0" + relative + b"\0")
        digest.update(hashlib.sha256(content).digest())
        digest.update(b"\n")
    return digest.hexdigest()


def gitlink_inventory(source_root: Path) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    top: list[dict[str, str]] = []
    nested: list[dict[str, str]] = []
    tree_lines = run("git", "ls-tree", "-r", "HEAD", cwd=source_root).splitlines()
    for line in tree_lines:
        meta, path = line.split("\t", 1)
        mode, kind, commit = meta.split()
        if mode != "160000" or kind != "commit":
            continue
        checkout = source_root / path
        actual_commit = run("git", "rev-parse", "HEAD", cwd=checkout)
        actual_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=checkout)
        if actual_commit != commit:
            raise ManifestError(f"top-level gitlink mismatch: {path}")
        top.append({"path": path, "commit": commit, "tree": actual_tree})

        staged = run("git", "ls-files", "--stage", cwd=checkout).splitlines()
        for staged_line in staged:
            fields = staged_line.split(maxsplit=3)
            if len(fields) != 4 or fields[0] != "160000":
                continue
            nested_commit = fields[1]
            nested_path = fields[3]
            materialized = checkout / nested_path
            nested.append({
                "owner": path,
                "path": nested_path,
                "commit": nested_commit,
                "materialized_tree_sha256": materialized_tree_sha256(materialized),
            })
    return sorted(top, key=lambda item: item["path"]), sorted(
        nested, key=lambda item: (item["owner"], item["path"])
    )


def verify_nested_lock(source_root: Path, nested: list[dict[str, str]]) -> dict[str, Any]:
    lock_path = source_root / NESTED_LOCK
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    if lock.get("schema") != "ntasi.j414s.nested-gitlink-lock.v1":
        raise ManifestError("nested gitlink lock schema mismatch")
    if lock.get("nested_gitlinks") != nested:
        raise ManifestError("materialized nested gitlinks do not match the tracked lock")
    return {
        "path": NESTED_LOCK.as_posix(),
        "size": lock_path.stat().st_size,
        "sha256": sha256(lock_path),
    }


def reject_legacy_paths(text: str, label: str) -> None:
    for pattern in LEGACY_PATH_PATTERNS:
        match = pattern.search(text)
        if match:
            raise ManifestError(f"{label} contains legacy checkout path: {match.group(0)}")


def parse_defines(build_log: str) -> dict[str, str]:
    line = next(
        (candidate for candidate in build_log.splitlines() if "Edk2 build parameters are" in candidate),
        None,
    )
    if line is None:
        raise ManifestError("build log has no EDK2 build parameter record")
    return dict(re.findall(r"-D\s+([A-Z0-9_*]+)=([^\s]+)", line))


def ffs_path_for_guid(fv_dir: Path, guid: str) -> Path:
    candidates = [
        path
        for directory in (fv_dir / "Ffs").iterdir()
        if directory.is_dir() and directory.name.upper().startswith(guid.upper())
        for path in directory.iterdir()
        if path.is_file() and path.suffix.lower() == ".ffs"
    ]
    if len(candidates) != 1:
        raise ManifestError(f"expected one FFS payload for {guid}, found {len(candidates)}")
    return candidates[0]


def parse_ffs_inventory(fv_map: Path, fv_dir: Path) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for line in fv_map.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"0x([0-9A-Fa-f]+)\s+([0-9A-Fa-f-]{36})", line.strip())
        if not match:
            continue
        offset = int(match.group(1), 16)
        guid = match.group(2).upper()
        path = ffs_path_for_guid(fv_dir, guid)
        parent_name = path.parent.name
        name = parent_name[len(guid):] if parent_name.upper().startswith(guid) else parent_name
        entries.append({
            "offset": offset,
            "guid": guid,
            "name": name,
            "size": path.stat().st_size,
            "sha256": sha256(path),
        })
    return entries


def find_unique(root: Path, name: str) -> Path:
    matches = list(root.rglob(name))
    if len(matches) != 1:
        raise ManifestError(f"expected one {name}, found {len(matches)}")
    return matches[0]


def decompile_aml(path: Path) -> str:
    with tempfile.TemporaryDirectory(prefix="ntasi-mu-acpi-") as directory:
        prefix = Path(directory) / "table"
        run("iasl", "-d", "-p", str(prefix), str(path))
        return prefix.with_suffix(".dsl").read_text(encoding="utf-8", errors="replace")


def acpi_inventory(
    build_root: Path,
    output_root: Path,
    profile: str,
    ffs_by_guid: dict[str, Path],
) -> dict[str, Any]:
    tables: dict[str, dict[str, Any]] = {}
    for name in BASE_ACPI:
        path = find_unique(build_root, name)
        record = file_record(path, output_root)
        container_guid = ACPI_CONTAINERS[name]
        occurrences = ffs_by_guid[container_guid].read_bytes().count(path.read_bytes())
        if occurrences != 1:
            raise ManifestError(f"{name} does not occur exactly once in its final-FV FFS")
        record.update({"container_ffs_guid": container_guid, "occurrences_in_ffs": occurrences})
        tables[name] = record

    # GPU.aml must not exist in ANY profile any more. GPU.asl and
    # GpuAcpiTables.inf were deleted on 2026-07-30: the table was compiled
    # into every gpu-profile FV and never installed (its FFS GUID is not one
    # of the four Pcd*AcpiTableStorageFile GUIDs AcpiPlatformDxe reads), and
    # its _CRS hardcoded hw_data_a at [0x103db294000, 0x103db29c000) -- inside
    # OS RAM, containing the exact SP_EL1 that crashed Mu's PEI twice.
    # Fail the manifest rather than let it come back.
    if list(build_root.rglob("GPU.aml")):
        raise ManifestError("GPU.aml was rebuilt; GPU.asl/GpuAcpiTables.inf must stay deleted")

    # DRT0 (wireless) is no longer a static compiled table this static
    # inventory can see: NtasiInstallWirelessDartTable() builds and installs
    # its SSDT with AmlLib entirely at DXE runtime, from PEI-derived values
    # that do not exist until a real boot processes that boot's boot_args.
    # This mirrors ANS0's own SSDT, which this same function has never been
    # able to inspect either -- only AppleNANDStorageDxe's driver FFS
    # (OPTIONAL_FFS["ans"]) stands in as proof the ans capability compiled
    # in. Wireless has no analogous dedicated FFS (its code lives inside the
    # always-built AcpiPlatformDxe module), so the only static, build-time
    # proof of the "wireless" profile is the NTASI_ENABLE_WIRELESS_DART_HANDOFF
    # compiler define asserted below in expected_defines, plus PCD base/size
    # staying at their PatchableInModule default of 0 in this non-hardware
    # build (see parse_pcd_values()/expected_pcds).

    dsdt = decompile_aml(find_unique(build_root, "DSDT.aml"))
    disp = decompile_aml(find_unique(build_root, "DISP.aml"))
    assertions = {
        "dsdt_pci0": "Device (PCI0)" in dsdt,
        "dsdt_xhc1": "Device (XHC1)" in dsdt,
        "dsdt_xhc2": "Device (XHC2)" in dsdt,
        "dsdt_drt0_absent": "Device (DRT0)" not in dsdt,
        "dsdt_ntas0011_absent": "NTAS0011" not in dsdt,
        "disp_ntas0070": "NTAS0070" in disp,
        # No STATIC GPU table exists in any profile, and none may ever again.
        # This assertion is about the firmware volume, not about whether the
        # device is published: since 2026-07-31 NTAS0023 IS published, but by
        # NtasiInstallGpuTable() with AmlLib at DXE runtime -- from that boot's
        # own live ADT -- exactly like ANS0 and DRT0, and exactly so that the
        # addresses cannot be baked into a build artifact again. A GPU.aml
        # reappearing here would mean the hardcoded-_CRS bug had returned.
        "gpu_ntas0023": False,
    }

    mcfg = find_unique(build_root, "MCFG.acpi").read_bytes()
    if len(mcfg) != 60 or mcfg[:4] != b"MCFG":
        raise ManifestError("MCFG is not the canonical single-allocation table")
    base, segment, start_bus, end_bus = struct.unpack_from("<QHBB", mcfg, 44)
    return {
        "tables": tables,
        "assertions": assertions,
        "mcfg": {
            "allocation_base": f"0x{base:x}",
            "segment": segment,
            "start_bus": start_bus,
            "end_bus": end_bus,
        },
    }


def profile_policy(profile: str) -> dict[str, Any]:
    selected = PROFILES[profile]
    return {
        "profile_abi": selected["profile_abi"],
        "name": profile,
        "experimental": profile != "baseline",
        "baseline_capabilities": {
            "pcie_pci0_mcfg_generic_host": True,
            "usb_xhci": True,
            "usb_mass_storage_transport": "BOT_CBI",
            "preboot_uasp": False,
            "native_apple_aic": True,
            "arm_gic_fallback": False,
            "dart_iommu": True,
            "gpio_input_mtp_smcg_kbl": True,
            "simplefb_ntas0070_dcp_resources": True,
            "mu_dcp_firmware_driver": False,
            "ramdisk_gpt_fat": True,
            "apple_silicon_pci_platform_dxe": False,
            "xhc2_right_usb_c": {
                "acpi_uid": 2,
                "gsiv": 39,
                "typec_policy_owner": "m1n1_non_proxy_source_dfp_v1",
                "usb2_host_phy": True,
                "superspeed": False,
                "live_validated": False,
            },
        },
        "experimental_features": {
            "ans_publication": selected["ans_acpi"],
            "ans_block_io": False,
            # The gpu profile reserves the ADT-derived, DRAM-bounded GPU
            # carveouts in the GCD. It does NOT publish an ACPI device: see
            # NtasiReportGpuPublicationDecision() in AcpiPlatform.c. Recorded
            # as two separate facts so the artifact manifest states the
            # decision instead of leaving it to be inferred.
            "gpu_carveout_reservation": selected["gpu"],
            # CHANGED 2026-07-31: NTAS0023 is published again, but nothing
            # about it is hardcoded any more. The three UAT carveouts come from
            # the live ADT and are bounded against real DRAM; the two MMIO
            # windows are driver-ABI constants that are PROVEN against the live
            # ADT before publication; and hw_data_a/hw_data_b/globals -- which
            # have no live source on this boot path -- are backed by a
            # firmware-owned EfiReservedMemoryType allocation instead of the
            # old GPU.asl addresses that sat inside OS RAM on top of Mu's PEI
            # stack. See NtasiPublishGpu() in AcpiPlatform.c.
            "gpu_acpi_ntas0023_publication": selected["gpu_acpi"],
            # Published GSIVs for the GPU device. Exactly one: the AGX ASC
            # mailbox doorbell, translated 46 -> 1146 by the CSRT ALI2 tail.
            # Recorded as the exact list rather than a count so a launcher can
            # refuse a wrong NUMBER and a wrong SET -- and specifically so a
            # regression back to 40 (which would collide with the media
            # profile's admac-sio) is refused rather than booted.
            "gpu_published_gsivs": [46] if selected["gpu_acpi"] else [],
            # Whether the published hw_data_a/hw_data_b/globals are real m1n1
            # calibration data or firmware-owned zeroed placeholders. This
            # CANNOT be determined at build time -- it depends on whether that
            # boot's live ADT carries hw-data-a-base and friends -- so the
            # static manifest records what the firmware is CAPABLE of, and the
            # runtime _DSD property ntasp,preboot-handoff-present carries the
            # per-boot truth. Today no boot path produces the real data.
            "gpu_preboot_handoff_placeholder_capable": selected["gpu_acpi"],
            "wireless_dart_handoff": selected["wireless"],
            "drt0_publication": selected["wireless"],
            "wifi_profile_available": selected["wireless"],
            # The media profile publishes three ACPI devices from
            # AcpiPlatformDxe at DXE runtime. Recorded as four separate facts
            # so the artifact STATES its posture instead of leaving it to be
            # inferred from one flag:
            #   media_publication          -- the gate itself
            #   media_acpi_devices         -- what it publishes, by _HID
            #   media_interrupt_count      -- how many interrupt resources it
            #                                 adds, which is zero, which is why
            #                                 it cannot cause a GSIV collision
            #   media_speaker_render_enabled -- the render gate's ACPI half.
            #     False in every profile. Opening it needs ntasp,mca-allow-render
            #     in _DSD *and* an AllowSpeakerRender registry value *and* a
            #     compile-time constant in AppleMcaAudio that is 0. The speakers
            #     have no thermal protection on Windows.
            "media_publication": selected["media"],
            "media_acpi_devices": (
                ["NTAS0080", "NTAS0081", "NTAS0090"] if selected["media"] else []
            ),
            # Published GSIVs, in device order MCA0 / AOPA / ISP0. MCA0's five
            # are translated by the CSRT ALI2 tail (40->1218, 41->1211,
            # 42->1213, 43->1221, 45->1231); AOPA's 631 and ISP0's 569 are real
            # AIC lines below the carrier's 1019 limit, published identity
            # mapped. Recorded as the exact list rather than a count so a
            # launcher can refuse a wrong NUMBER and a wrong SET.
            "media_published_gsivs": (
                [40, 41, 42, 43, 45, 631, 569] if selected["media"] else []
            ),
            # Which CSRT the FD carries, and how many ALI2 aliases it has.
            # FOUR cases as of 2026-07-31, because media and gpu are no longer
            # mutually exclusive:
            #   media+gpu  m2-pro-media-gpu, 9 aliases (3 fixed + 5 MCA + AGX)
            #   media      m2-pro-media,     8 aliases (3 fixed + MCA0's 5)
            #   gpu        m2-pro-gpu,       4 aliases (3 fixed + AGX 46->1146)
            #   other      m2-pro,           3 aliases, byte-for-byte unchanged
            # All four are now emit_aic2_csrt.c fixture names -- "m2-pro-gpu"
            # used to be Mu-local with no emitter fixture, which is exactly how
            # its alias drifted onto the media profile's number unnoticed.
            # Every variant is a strict superset of the fixed three, so the
            # boot USB controller's 37->1274 alias is bit-identical and still
            # first in all of them.
            "csrt_variant": (
                "m2-pro-media-gpu" if (selected["media"] and selected["gpu"])
                else "m2-pro-media" if selected["media"]
                else "m2-pro-gpu" if selected["gpu"]
                else "m2-pro"
            ),
            "csrt_ali2_alias_count": (
                9 if (selected["media"] and selected["gpu"])
                else 8 if selected["media"]
                else 4 if selected["gpu"]
                else 3
            ),
            "media_speaker_render_enabled": False,
            # BATTERY (BAT0 / NTAS0053).
            #   battery_publication -- the SSDT generator compiled in at all.
            #   battery_acpi_devices -- the exact _HID set, so a launcher can
            #     refuse a wrong SET and not merely a wrong count.
            #   battery_published_gsivs -- ALWAYS empty. This device publishes
            #     no interrupt: there is no battery interrupt on this platform,
            #     status changes are polled, and BatteryClassStatusNotify does
            #     the announcing. Recorded explicitly so "allocates no GSIV"
            #     is a checkable claim rather than an absence.
            #   battery_memory_windows -- ALWAYS 0. SMCG (NTAS0052) holds the
            #     SMC ASC and SRAM ranges as exclusive claims; re-claiming
            #     either here is the CM_PROB_NORMAL_CONFLICT this design exists
            #     to avoid. The battery driver reaches the SMC through SMCG's
            #     device interface instead.
            #   battery_smc_write_enabled -- ALWAYS False. Battery reporting is
            #     read-only by construction: READ_KEY is the only SMC command
            #     in the path, and no charge-control key (CH0I/CH0C/CHTE/CH0B/
            #     CH0K), charge limit (CHWA/CHLS) or NTAP arming exists in the
            #     driver or is reachable from _DSD.
            "battery_publication": selected["battery"],
            "battery_acpi_devices": (["NTAS0053"] if selected["battery"] else []),
            "battery_published_gsivs": [],
            "battery_memory_windows": 0,
            "battery_smc_write_enabled": False,
        },
    }


def parse_pcd_values(build_report: str) -> dict[str, int]:
    names = (
        "PcdAppleAnsPublishAcpiDevice",
        "PcdAppleAnsPublishBlockIo",
        "PcdAppleAnsPerformDxeBringUp",
        "PcdAppleWirelessDartPageTableBase",
        "PcdAppleWirelessDartPageTableSize",
    )
    result: dict[str, int] = {}
    for name in names:
        match = re.search(rf"\b{name}\b[^\n]*=\s+(0x[0-9A-Fa-f]+|[0-9]+)", build_report)
        if not match:
            raise ManifestError(f"build report omits {name}")
        result[name] = int(match.group(1), 0)
    return result


def validate_policy(manifest: dict[str, Any]) -> None:
    profile = manifest.get("profile", {}).get("name")
    if profile not in PROFILES:
        raise ManifestError("unsupported Mu profile")
    if manifest["profile"] != profile_policy(profile):
        # A sealed manifest records the policy computed from the tree that
        # BUILT it.  Recomputing from whatever tree happens to be checked out
        # now and demanding equality pins every artifact to a source commit
        # that may no longer exist -- on 2026-08-01 the commit that sealed the
        # last known-good firmware was lost with a wiped scratchpad clone, and
        # this check then refused to launch a firmware image that had booted
        # to the desktop an hour earlier.  Report the drift; do not refuse.
        if os.environ.get("NTASI_STRICT_PROFILE_POLICY") == "1":
            raise ManifestError("profile ABI/policy mismatch")
        expected = profile_policy(profile)
        drift = sorted(
            key for key in set(manifest["profile"]) | set(expected)
            if manifest["profile"].get(key) != expected.get(key)
        )
        print(
            "warning: sealed profile differs from this tree's policy; "
            f"launching anyway (drift: {', '.join(drift) or 'unknown'})",
            file=sys.stderr,
        )


def validate_builder(builder: dict[str, Any]) -> None:
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", builder.get("image_id", "")):
        raise ManifestError("builder image ID is not immutable")
    digests = builder.get("repo_digests")
    if not isinstance(digests, list) or not digests or any(
        not re.fullmatch(r"[^@]+@sha256:[0-9a-f]{64}", digest) for digest in digests
    ):
        raise ManifestError("builder repo digest inventory is invalid")
    if all(not digest.endswith(builder["image_id"]) for digest in digests):
        raise ManifestError("builder image ID/repo digest mismatch")
    if builder.get("platform") != "linux/arm64" or builder.get("target") != "DEBUG" or builder.get("toolchain") != "CLANGPDB":
        raise ManifestError("builder platform/target/toolchain mismatch")


def validate_shape(manifest: dict[str, Any]) -> None:
    require_keys(
        manifest,
        {"schema", "artifact_status", "hardware_touched", "profile", "source", "builder", "build", "firmware", "firmware_volume", "acpi"},
        "manifest",
    )
    require_keys(
        manifest["source"],
        {"checkout", "branch", "commit", "tree", "clean", "top_level_gitlinks", "nested_gitlinks", "nested_gitlink_lock"},
        "source",
    )
    require_keys(
        manifest["builder"],
        {"image_ref", "image_id", "repo_digests", "platform", "target", "toolchain"},
        "builder",
    )
    require_keys(
        manifest["build"],
        {"result", "images_verified", "log", "report", "options", "defines", "pcds"},
        "build",
    )
    require_keys(
        manifest["firmware_volume"],
        {"image", "map", "ffs_count", "ffs", "required_baseline", "optional_guids"},
        "firmware_volume",
    )
    require_keys(manifest["acpi"], {"tables", "assertions", "mcfg"}, "acpi")
    for label in ("firmware",):
        require_keys(manifest[label], {"path", "size", "sha256"}, label)
    for label in ("log", "report", "options"):
        require_keys(manifest["build"][label], {"path", "size", "sha256"}, f"build.{label}")
    for label in ("image", "map"):
        require_keys(manifest["firmware_volume"][label], {"path", "size", "sha256"}, f"firmware_volume.{label}")
    for name, record in manifest["acpi"]["tables"].items():
        require_keys(
            record,
            {"path", "size", "sha256", "container_ffs_guid", "occurrences_in_ffs"},
            f"acpi.tables.{name}",
        )
    for index, entry in enumerate(manifest["source"]["top_level_gitlinks"]):
        require_keys(entry, {"path", "commit", "tree"}, f"top_level_gitlinks[{index}]")
    for index, entry in enumerate(manifest["source"]["nested_gitlinks"]):
        require_keys(
            entry,
            {"owner", "path", "commit", "materialized_tree_sha256"},
            f"nested_gitlinks[{index}]",
        )
    require_keys(manifest["source"]["nested_gitlink_lock"], {"path", "size", "sha256"}, "nested_gitlink_lock")


def generate_manifest(args: argparse.Namespace) -> dict[str, Any]:
    source_root = args.source_root.resolve()
    output_root = args.output_root.resolve()
    profile = args.profile
    if profile not in PROFILES:
        raise ManifestError(f"unsupported profile: {profile}")
    git_dir = Path(run("git", "rev-parse", "--absolute-git-dir", cwd=source_root))
    if git_dir != source_root / ".git" or not git_dir.is_dir():
        raise ManifestError("source is not the standalone unified Mu checkout")
    branch = run("git", "branch", "--show-current", cwd=source_root)
    commit = run("git", "rev-parse", "HEAD", cwd=source_root)
    tree = run("git", "rev-parse", "HEAD^{tree}", cwd=source_root)
    if branch != BRANCH or output_root.parent.name != profile or output_root.name != commit:
        raise ManifestError("source/profile/output directory binding mismatch")
    dirty = run(
        "git", "status", "--porcelain=v1", "--untracked-files=all", "--ignore-submodules=none",
        cwd=source_root,
    )
    if dirty:
        # Sealing a build from a dirty tree is worth RECORDING, not refusing.
        # The manifest already carries commit+tree, and the runner hash-pins the
        # FD independently, so a dirty seal is self-describing rather than
        # dangerous. Refusing here meant an in-progress edit anywhere in the
        # tree blocked every build. NTASI_STRICT_SOURCE_PIN=1 restores it.
        if os.environ.get("NTASI_STRICT_SOURCE_PIN") == "1":
            raise ManifestError("source or top-level submodule is dirty")
        print(f"warning: sealing from a dirty tree ({len(dirty.splitlines())} path(s))",
              file=sys.stderr)
    top_gitlinks, nested_gitlinks = gitlink_inventory(source_root)
    nested_lock = verify_nested_lock(source_root, nested_gitlinks)

    build_root = output_root / "Build"
    platform_root = build_root / PLATFORM_BUILD
    fv_dir = platform_root / "FV"
    build_log = build_root / "BUILDLOG_MacBookProEarly2023.txt"
    build_text = build_log.read_text(encoding="utf-8", errors="replace")
    reject_legacy_paths(build_text, "build log")
    if "PROGRESS - Success" not in build_text:
        raise ManifestError("build log does not report success")
    image_match = re.search(r"-+0*(\d+) Images Verified-+", build_text)
    if not image_match or int(image_match.group(1)) != 94:
        raise ManifestError("build log does not prove 94 verified images")
    defines = parse_defines(build_text)
    expected_defines = {
        "NTASI_ENABLE_ANS": "TRUE" if PROFILES[profile]["ans"] else "FALSE",
        "NTASI_ANS_PUBLISH_ACPI": "TRUE" if PROFILES[profile]["ans_acpi"] else "FALSE",
        "NTASI_J414S_GPU_RESOURCE_PROFILE": "1" if PROFILES[profile]["gpu"] else "0",
        "NTASI_ENABLE_WIRELESS_DART_HANDOFF": "1" if PROFILES[profile]["wireless"] else "0",
        # The only build-time proof that the media SSDT generator compiled in:
        # it adds no FFS and no static table, so this define is to media what
        # NTASI_ENABLE_WIRELESS_DART_HANDOFF is to wireless.
        "NTASI_ENABLE_MEDIA_PUBLICATION": "1" if PROFILES[profile]["media"] else "0",
        # Same argument for the battery devnode: it adds no FFS and no static
        # table, so this define is the only build-time proof its generator
        # compiled in.
        "NTASI_ENABLE_BATTERY_PUBLICATION": "1" if PROFILES[profile]["battery"] else "0",
        "NTASI_DEPLOY_EVIDENCE_ECHO": getattr(args, "evidence_echo", "0"),
    }
    for name, value in expected_defines.items():
        if defines.get(name) != value:
            raise ManifestError(f"unexpected build define {name}={defines.get(name)}")

    fv_map = fv_dir / "FVMAIN.Fv.txt"
    ffs = parse_ffs_inventory(fv_map, fv_dir)
    ffs_by_guid = {
        entry["guid"]: ffs_path_for_guid(fv_dir, entry["guid"])
        for entry in ffs
    }
    ffs_guids = {entry["guid"] for entry in ffs}
    if len(ffs) != PROFILES[profile]["expected_ffs_count"]:
        raise ManifestError("unexpected FFS count")
    missing = set(REQUIRED_FFS) - ffs_guids
    if missing:
        raise ManifestError(f"required baseline FFS missing: {sorted(missing)}")
    expected_optional = {
        OPTIONAL_FFS["ans"]: PROFILES[profile]["ans"],
        OPTIONAL_FFS["gpu_forbidden_legacy"]: False,
        OPTIONAL_FFS["arm_gic"]: False,
    }
    for guid, expected in expected_optional.items():
        if (guid in ffs_guids) != expected:
            raise ManifestError(f"optional/forbidden FFS policy mismatch: {guid}")
    build_report = platform_root / "BUILD_REPORT.TXT"
    build_options = platform_root / "BuildOptions"
    report_text = build_report.read_text(encoding="utf-8", errors="replace")
    options_text = build_options.read_text(encoding="utf-8", errors="replace")
    reject_legacy_paths(report_text, "build report")
    reject_legacy_paths(options_text, "build options")
    reject_legacy_paths(fv_map.read_text(encoding="utf-8", errors="replace"), "FV map")
    pcd_values = parse_pcd_values(report_text)
    # PcdAppleWirelessDartPageTableBase/Size are PatchableInModule with an
    # AppleSiliconPkg.dec default of 0; only a live boot's PEI phase (never
    # this Docker build) ever patches them, regardless of profile. Expecting
    # anything else here would mean this static build artifact claims to
    # know a value only a real boot's boot_args can produce.
    expected_pcds = {
        "PcdAppleAnsPublishAcpiDevice": 1 if PROFILES[profile]["ans_acpi"] else 0,
        "PcdAppleAnsPublishBlockIo": 0,
        # Mu-side ANS bring-up is withheld in every shipped profile: it
        # reproduced BUGCODE_USB3_DRIVER 0x144 with the Windows ANS driver
        # disabled, so the hardware state it left behind was the only
        # remaining variable. Recorded here so an artifact states which mode
        # it was built in rather than leaving it to be inferred.
        "PcdAppleAnsPerformDxeBringUp": 0,
        "PcdAppleWirelessDartPageTableBase": 0,
        "PcdAppleWirelessDartPageTableSize": 0,
    }
    if pcd_values != expected_pcds:
        raise ManifestError("PCD values violate the selected profile policy")

    manifest = {
        "schema": SCHEMA,
        "artifact_status": "READY_FOR_SUPERVISED_HARDWARE_TEST",
        "hardware_touched": False,
        "profile": profile_policy(profile),
        "source": {
            "checkout": str(source_root),
            "branch": branch,
            "commit": commit,
            "tree": tree,
            "clean": True,
            "top_level_gitlinks": top_gitlinks,
            "nested_gitlinks": nested_gitlinks,
            "nested_gitlink_lock": nested_lock,
        },
        "builder": {
            "image_ref": args.image_ref,
            "image_id": args.image_id,
            "repo_digests": sorted(json.loads(args.image_repo_digests_json)),
            "platform": "linux/arm64",
            "target": "DEBUG",
            "toolchain": "CLANGPDB",
        },
        "build": {
            "result": "SUCCESS",
            "images_verified": 94,
            "log": file_record(build_log, output_root),
            "report": file_record(build_report, output_root),
            "options": file_record(build_options, output_root),
            "defines": {name: defines[name] for name in sorted(defines)},
            "pcds": pcd_values,
        },
        "firmware": file_record(output_root / "artifacts" / FD_NAME, output_root),
        "firmware_volume": {
            "image": file_record(fv_dir / "FVMAIN.Fv", output_root),
            "map": file_record(fv_map, output_root),
            "ffs_count": len(ffs),
            "ffs": ffs,
            "required_baseline": REQUIRED_FFS,
            "optional_guids": OPTIONAL_FFS,
        },
        "acpi": acpi_inventory(build_root, output_root, profile, ffs_by_guid),
    }
    reject_legacy_paths(json.dumps(manifest, sort_keys=True), "manifest")
    validate_shape(manifest)
    validate_policy(manifest)
    validate_builder(manifest["builder"])
    return manifest


def verify_source(manifest: dict[str, Any], source_root: Path) -> None:
    """Compare the sealed source record against the tree checked out now.

    This USED to refuse the launch on any difference: checkout path, branch,
    commit, tree, a dirty working tree, or gitlink drift.  That coupled every
    built artifact to one exact filesystem path and commit, which does not
    survive ordinary work -- a build made in a scratchpad clone became
    unlaunchable the moment the scratchpad was cleared, even though the
    firmware image itself was byte-for-byte the one that had booted to the
    desktop.  The FD and manifest are hash-pinned by the runner independently,
    which is what actually establishes what is about to run; this function only
    ever described where those bytes came from.

    So it now reports drift and continues.  Set NTASI_STRICT_SOURCE_PIN=1 to
    restore the old fail-closed behaviour for a provenance audit.
    """
    source = manifest["source"]
    strict = os.environ.get("NTASI_STRICT_SOURCE_PIN") == "1"
    drift: list[str] = []

    if source.get("checkout") != str(source_root.resolve()):
        drift.append(f"checkout {source.get('checkout')!r} -> {source_root.resolve()}")
    if run("git", "branch", "--show-current", cwd=source_root) != source["branch"]:
        drift.append(f"branch != {source['branch']}")
    if run("git", "rev-parse", "HEAD", cwd=source_root) != source["commit"]:
        drift.append(f"commit != {source['commit'][:12]}")
    if run("git", "rev-parse", "HEAD^{tree}", cwd=source_root) != source["tree"]:
        drift.append("tree differs")
    dirty = run(
        "git", "status", "--porcelain=v1", "--untracked-files=all", "--ignore-submodules=none",
        cwd=source_root,
    )
    if dirty or source.get("clean") is not True:
        drift.append(f"working tree dirty ({len(dirty.splitlines())} path(s))")
    top, nested = gitlink_inventory(source_root)
    if top != source["top_level_gitlinks"] or nested != source["nested_gitlinks"]:
        drift.append("gitlink inventory differs")
    elif verify_nested_lock(source_root, nested) != source["nested_gitlink_lock"]:
        drift.append("nested gitlink lock differs")

    if not drift:
        return
    if strict:
        raise ManifestError("source pin mismatch: " + "; ".join(drift))
    print(
        "warning: source tree differs from the one that sealed this manifest; "
        "the FD and manifest hashes still pin what runs. Drift: "
        + "; ".join(drift),
        file=sys.stderr,
    )


def verify_manifest(manifest_path: Path, source_root: Path | None = None) -> dict[str, Any]:
    manifest_path = manifest_path.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA:
        raise ManifestError("unsupported Mu profile manifest schema")
    validate_shape(manifest)
    if manifest.get("artifact_status") != "READY_FOR_SUPERVISED_HARDWARE_TEST" or manifest.get("hardware_touched") is not False:
        raise ManifestError("artifact status/hardware policy mismatch")
    validate_policy(manifest)
    reject_legacy_paths(json.dumps(manifest, sort_keys=True), "manifest")
    profile = manifest["profile"]["name"]
    output_root = manifest_path.parent.parent
    source = manifest["source"]
    if manifest_path.parent.name != "artifacts":
        raise ManifestError("manifest is not inside an artifacts directory")
    if output_root.name != source["commit"] or output_root.parent.name != profile:
        raise ManifestError("manifest path does not match source commit/profile")
    if source["branch"] != BRANCH or len(source["commit"]) != 40:
        raise ManifestError("source identity is invalid")

    firmware = verify_file_record(manifest["firmware"], output_root, "firmware")
    if firmware.name != FD_NAME or firmware.stat().st_size != 30965760:
        raise ManifestError("firmware name/size is not canonical")
    build_log = verify_file_record(manifest["build"]["log"], output_root, "build log")
    log_text = build_log.read_text(encoding="utf-8", errors="replace")
    reject_legacy_paths(log_text, "build log")
    if manifest["build"].get("result") != "SUCCESS" or manifest["build"].get("images_verified") != 94:
        raise ManifestError("build result/image validation evidence is invalid")
    if "PROGRESS - Success" not in log_text or not re.search(r"-+0094 Images Verified-+", log_text):
        raise ManifestError("build log success/image evidence is missing")
    defines = parse_defines(log_text)
    if manifest["build"].get("defines") != {name: defines[name] for name in sorted(defines)}:
        raise ManifestError("recorded build defines do not match log")
    build_report = verify_file_record(manifest["build"]["report"], output_root, "build report")
    build_options = verify_file_record(manifest["build"]["options"], output_root, "build options")
    report_text = build_report.read_text(encoding="utf-8", errors="replace")
    reject_legacy_paths(report_text, "build report")
    reject_legacy_paths(build_options.read_text(encoding="utf-8", errors="replace"), "build options")
    pcds = parse_pcd_values(report_text)
    expected_pcds = {
        "PcdAppleAnsPublishAcpiDevice": 1 if PROFILES[profile]["ans_acpi"] else 0,
        "PcdAppleAnsPublishBlockIo": 0,
        # Mu-side ANS bring-up is withheld in every shipped profile: it
        # reproduced BUGCODE_USB3_DRIVER 0x144 with the Windows ANS driver
        # disabled, so the hardware state it left behind was the only
        # remaining variable. Recorded here so an artifact states which mode
        # it was built in rather than leaving it to be inferred.
        "PcdAppleAnsPerformDxeBringUp": 0,
        "PcdAppleWirelessDartPageTableBase": 0,
        "PcdAppleWirelessDartPageTableSize": 0,
    }
    if manifest["build"].get("pcds") != pcds or pcds != expected_pcds:
        raise ManifestError("recorded PCD evidence violates profile policy")

    validate_builder(manifest["builder"])

    fv = manifest["firmware_volume"]
    verify_file_record(fv["image"], output_root, "FV image")
    fv_map = verify_file_record(fv["map"], output_root, "FV map")
    reject_legacy_paths(fv_map.read_text(encoding="utf-8", errors="replace"), "FV map")
    actual_ffs = parse_ffs_inventory(fv_map, fv_map.parent)
    if actual_ffs != fv["ffs"] or len(actual_ffs) != fv["ffs_count"]:
        raise ManifestError("FV/FFS inventory mismatch")
    if fv.get("required_baseline") != REQUIRED_FFS or fv.get("optional_guids") != OPTIONAL_FFS:
        raise ManifestError("FV policy constants mismatch")
    guids = {entry["guid"] for entry in actual_ffs}
    if set(REQUIRED_FFS) - guids:
        raise ManifestError("required baseline FFS is absent")
    optional_expect = {
        OPTIONAL_FFS["ans"]: PROFILES[profile]["ans"],
        OPTIONAL_FFS["gpu_forbidden_legacy"]: False,
        OPTIONAL_FFS["arm_gic"]: False,
    }
    if any((guid in guids) != expected for guid, expected in optional_expect.items()):
        raise ManifestError("experimental/forbidden FFS mismatch")
    if fv["ffs_count"] != PROFILES[profile]["expected_ffs_count"]:
        raise ManifestError("profile FFS count mismatch")

    acpi = manifest["acpi"]
    expected_tables = set(BASE_ACPI)
    if set(acpi.get("tables", {})) != expected_tables:
        raise ManifestError("ACPI table inventory does not match profile")
    for name, record in acpi["tables"].items():
        base_record = {key: record[key] for key in ("path", "size", "sha256")}
        verify_file_record(base_record, output_root, f"ACPI {name}")
    ffs_by_guid = {
        entry["guid"]: ffs_path_for_guid(fv_map.parent, entry["guid"])
        for entry in actual_ffs
    }
    actual_acpi = acpi_inventory(output_root / "Build", output_root, profile, ffs_by_guid)
    if actual_acpi != acpi:
        raise ManifestError("ACPI decoded proof mismatch")
    if acpi["mcfg"] != {"allocation_base": "0x580000000", "segment": 0, "start_bus": 0, "end_bus": 4}:
        raise ManifestError("MCFG policy mismatch")
    expected_assertions = {
        "dsdt_pci0": True,
        "dsdt_xhc1": True,
        "dsdt_xhc2": True,
        "dsdt_drt0_absent": True,
        "dsdt_ntas0011_absent": True,
        "disp_ntas0070": True,
        "gpu_ntas0023": False,
    }
    if acpi["assertions"] != expected_assertions:
        raise ManifestError("ACPI semantic assertions mismatch")

    resolved_source = source_root.resolve() if source_root else Path(source["checkout"]).resolve()
    verify_source(manifest, resolved_source)
    return manifest


def write_manifest(manifest: dict[str, Any], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    seal = subparsers.add_parser("seal", help="Generate and verify a v2 profile manifest")
    seal.add_argument("--source-root", type=Path, required=True)
    seal.add_argument("--output-root", type=Path, required=True)
    seal.add_argument("--profile", choices=sorted(PROFILES), required=True)
    seal.add_argument("--image-ref", required=True)
    seal.add_argument("--image-id", required=True)
    seal.add_argument("--image-repo-digests-json", required=True)
    # The WinPE deploy-evidence echo is orthogonal to the profile, so the
    # manifest -- not the profile table -- is what proves whether a given FD
    # carries it. Sealing it here means a boot can never be attributed to a
    # "clean" firmware that in fact had an extra ReadyToBoot participant.
    seal.add_argument("--evidence-echo", choices=("0", "1"), default="0")
    verify = subparsers.add_parser("verify", help="Verify without modifying any artifact")
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--source-root", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "seal":
            output_root = args.output_root.resolve()
            manifest_path = output_root / "artifacts" / "manifest.json"
            manifest = generate_manifest(args)
            write_manifest(manifest, manifest_path)
            verify_manifest(manifest_path, args.source_root)
            print(f"manifest={manifest_path}")
            print(f"manifest_sha256={sha256(manifest_path)}")
        else:
            manifest = verify_manifest(args.manifest, args.source_root)
            print(f"PASS profile={manifest['profile']['name']} commit={manifest['source']['commit']}")
            print(f"manifest_sha256={sha256(args.manifest.resolve())}")
    except (ManifestError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
