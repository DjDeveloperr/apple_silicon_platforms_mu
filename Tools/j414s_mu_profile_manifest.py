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
BRANCH = "feature/j414s-windows-unified"
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
    "gpu": {
        "profile_abi": "ntasi.j414s.windows.gpu-resource-probe.v1",
        "ans": False,
        "gpu": True,
        "expected_ffs_count": 88,
    },
    "wireless": {
        "profile_abi": "ntasi.j414s.windows.wireless-handoff-v2.v1",
        "ans": False,
        "gpu": False,
        "wireless": True,
        "expected_ffs_count": 88,
    },
}
for _profile in PROFILES.values():
    _profile.setdefault("wireless", False)
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
    "gpu": "2CC5C83E-BCA6-49D9-B435-D14DC31E62AE",
    "wireless": "58ED2876-3BD4-470A-A18F-CE5347BE9C13",
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
    "GPU.aml": OPTIONAL_FFS["gpu"],
    "WDRT.aml": OPTIONAL_FFS["wireless"],
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

    gpu_matches = list(build_root.rglob("GPU.aml"))
    wdrt_matches = list(build_root.rglob("WDRT.aml"))
    if bool(gpu_matches) != PROFILES[profile]["gpu"] or len(gpu_matches) > 1:
        raise ManifestError("GPU ACPI output does not match profile")
    if bool(wdrt_matches) != PROFILES[profile]["wireless"] or len(wdrt_matches) > 1:
        raise ManifestError("wireless DART ACPI output does not match profile")
    if gpu_matches:
        record = file_record(gpu_matches[0], output_root)
        container_guid = ACPI_CONTAINERS["GPU.aml"]
        occurrences = ffs_by_guid[container_guid].read_bytes().count(gpu_matches[0].read_bytes())
        if occurrences != 1:
            raise ManifestError("GPU.aml does not occur exactly once in its final-FV FFS")
        record.update({"container_ffs_guid": container_guid, "occurrences_in_ffs": occurrences})
        tables["GPU.aml"] = record
    if wdrt_matches:
        record = file_record(wdrt_matches[0], output_root)
        container_guid = ACPI_CONTAINERS["WDRT.aml"]
        occurrences = ffs_by_guid[container_guid].read_bytes().count(wdrt_matches[0].read_bytes())
        if occurrences != 1:
            raise ManifestError("WDRT.aml does not occur exactly once in its final-FV FFS")
        record.update({"container_ffs_guid": container_guid, "occurrences_in_ffs": occurrences})
        tables["WDRT.aml"] = record

    dsdt = decompile_aml(find_unique(build_root, "DSDT.aml"))
    disp = decompile_aml(find_unique(build_root, "DISP.aml"))
    assertions = {
        "dsdt_pci0": "Device (PCI0)" in dsdt,
        "dsdt_xhc1": "Device (XHC1)" in dsdt,
        "dsdt_xhc2": "Device (XHC2)" in dsdt,
        "dsdt_drt0_absent": "Device (DRT0)" not in dsdt,
        "dsdt_ntas0011_absent": "NTAS0011" not in dsdt,
        "disp_ntas0070": "NTAS0070" in disp,
        "gpu_ntas0023": False,
    }
    if gpu_matches:
        assertions["gpu_ntas0023"] = "NTAS0023" in decompile_aml(gpu_matches[0])
    if wdrt_matches:
        wdrt = decompile_aml(wdrt_matches[0])
        assertions.update({
            "wdrt_ntas0011": "NTAS0011" in wdrt,
            "wdrt_dynamic_abi_v2": (
                "ntasi,wireless-handoff-abi" in wdrt and
                "ntasi,wireless-handoff-descriptor-offset" in wdrt
            ),
        })

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
        },
        "experimental_features": {
            "ans_publication": selected["ans"],
            "ans_block_io": False,
            "gpu_resource_publication": selected["gpu"],
            "wireless_dart_handoff": selected["wireless"],
            "drt0_publication": selected["wireless"],
            "wifi_profile_available": selected["wireless"],
        },
    }


def parse_pcd_values(build_report: str) -> dict[str, int]:
    names = (
        "PcdAppleAnsPublishAcpiDevice",
        "PcdAppleAnsPublishBlockIo",
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


def wireless_handoff_record(output_root: Path, profile: str) -> dict[str, Any] | None:
    path = output_root / "wireless-handoff.json"
    if not PROFILES[profile]["wireless"]:
        if path.exists():
            raise ManifestError("non-wireless profile contains a handoff manifest")
        return None
    if not path.is_file():
        raise ManifestError("wireless profile omits the same-instance handoff manifest")
    evidence = json.loads(path.read_text(encoding="utf-8"))
    contract = evidence.get("contract", {})
    reservation = evidence.get("reservation", {})
    descriptor = evidence.get("descriptor", {})
    m1n1 = evidence.get("m1n1", {})
    if (evidence.get("schema") != "ntasi.j414s.wireless-handoff.v2" or
            evidence.get("artifact_status") != "READY_FOR_SAME_INSTANCE_MU_BUILD" or
            evidence.get("hardware_touched") is not True or
            contract != {
                "name": "dynamic_reserved_wireless_handoff_v2",
                "descriptor_version": 2,
                "descriptor_size": 96,
                "descriptor_offset": 0xc000,
            }):
        raise ManifestError("wireless handoff evidence ABI/state mismatch")
    base = reservation.get("base")
    size = reservation.get("size")
    if (not isinstance(base, int) or not isinstance(size, int) or
            size != 0x10000 or base & 0x3fff or
            descriptor.get("reservation_base") != base or
            descriptor.get("reservation_size") != size or
            descriptor.get("descriptor_physical") != base + 0xc000 or
            descriptor.get("descriptor_crc32", 0) == 0 or
            not re.fullmatch(r"[0-9a-f]{40}", m1n1.get("source_commit", "")) or
            not re.fullmatch(r"[0-9a-f]{64}", m1n1.get("manifest_sha256", ""))):
        raise ManifestError("wireless handoff evidence layout/provenance mismatch")
    return {
        "manifest": file_record(path, output_root),
        "schema": evidence["schema"],
        "contract": contract,
        "base": base,
        "size": size,
        "descriptor_crc32": descriptor["descriptor_crc32"],
        "m1n1_commit": m1n1["source_commit"],
        "m1n1_manifest_sha256": m1n1["manifest_sha256"],
    }


def validate_policy(manifest: dict[str, Any]) -> None:
    profile = manifest.get("profile", {}).get("name")
    if profile not in PROFILES:
        raise ManifestError("unsupported Mu profile")
    if manifest["profile"] != profile_policy(profile):
        raise ManifestError("profile ABI/policy mismatch")


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
        {"schema", "artifact_status", "hardware_touched", "profile", "source", "builder", "build", "firmware", "firmware_volume", "acpi", "wireless_handoff"},
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
        raise ManifestError("source or top-level submodule is dirty")
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
        "NTASI_J414S_GPU_RESOURCE_PROFILE": "1" if PROFILES[profile]["gpu"] else "0",
        "NTASI_ENABLE_WIRELESS_DART_HANDOFF": "1" if PROFILES[profile]["wireless"] else "0",
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
        OPTIONAL_FFS["gpu"]: PROFILES[profile]["gpu"],
        OPTIONAL_FFS["wireless"]: PROFILES[profile]["wireless"],
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
    wireless_handoff = wireless_handoff_record(output_root, profile)
    expected_pcds = {
        "PcdAppleAnsPublishAcpiDevice": 1 if PROFILES[profile]["ans"] else 0,
        "PcdAppleAnsPublishBlockIo": 0,
        "PcdAppleWirelessDartPageTableBase": wireless_handoff["base"] if wireless_handoff else 0,
        "PcdAppleWirelessDartPageTableSize": wireless_handoff["size"] if wireless_handoff else 0,
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
        "wireless_handoff": wireless_handoff,
    }
    reject_legacy_paths(json.dumps(manifest, sort_keys=True), "manifest")
    validate_shape(manifest)
    validate_policy(manifest)
    validate_builder(manifest["builder"])
    return manifest


def verify_source(manifest: dict[str, Any], source_root: Path) -> None:
    source = manifest["source"]
    if source.get("checkout") != str(source_root.resolve()):
        raise ManifestError("source checkout path mismatch")
    if run("git", "branch", "--show-current", cwd=source_root) != source["branch"]:
        raise ManifestError("source branch mismatch")
    if run("git", "rev-parse", "HEAD", cwd=source_root) != source["commit"]:
        raise ManifestError("source commit mismatch")
    if run("git", "rev-parse", "HEAD^{tree}", cwd=source_root) != source["tree"]:
        raise ManifestError("source tree mismatch")
    dirty = run(
        "git", "status", "--porcelain=v1", "--untracked-files=all", "--ignore-submodules=none",
        cwd=source_root,
    )
    if dirty or source.get("clean") is not True:
        raise ManifestError("source is dirty or was not sealed clean")
    top, nested = gitlink_inventory(source_root)
    if top != source["top_level_gitlinks"] or nested != source["nested_gitlinks"]:
        raise ManifestError("gitlink/materialized tree inventory mismatch")
    if verify_nested_lock(source_root, nested) != source["nested_gitlink_lock"]:
        raise ManifestError("nested gitlink lock record mismatch")


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
    wireless_handoff = wireless_handoff_record(output_root, profile)
    if manifest.get("wireless_handoff") != wireless_handoff:
        raise ManifestError("recorded wireless handoff evidence mismatch")
    expected_pcds = {
        "PcdAppleAnsPublishAcpiDevice": 1 if PROFILES[profile]["ans"] else 0,
        "PcdAppleAnsPublishBlockIo": 0,
        "PcdAppleWirelessDartPageTableBase": wireless_handoff["base"] if wireless_handoff else 0,
        "PcdAppleWirelessDartPageTableSize": wireless_handoff["size"] if wireless_handoff else 0,
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
        OPTIONAL_FFS["gpu"]: PROFILES[profile]["gpu"],
        OPTIONAL_FFS["wireless"]: PROFILES[profile]["wireless"],
        OPTIONAL_FFS["arm_gic"]: False,
    }
    if any((guid in guids) != expected for guid, expected in optional_expect.items()):
        raise ManifestError("experimental/forbidden FFS mismatch")
    if fv["ffs_count"] != PROFILES[profile]["expected_ffs_count"]:
        raise ManifestError("profile FFS count mismatch")

    acpi = manifest["acpi"]
    expected_tables = set(BASE_ACPI) | ({"GPU.aml"} if PROFILES[profile]["gpu"] else set()) | ({"WDRT.aml"} if PROFILES[profile]["wireless"] else set())
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
        "gpu_ntas0023": PROFILES[profile]["gpu"],
    }
    if PROFILES[profile]["wireless"]:
        expected_assertions.update({
            "wdrt_ntas0011": True,
            "wdrt_dynamic_abi_v2": True,
        })
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
