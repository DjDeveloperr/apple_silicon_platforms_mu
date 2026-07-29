#!/usr/bin/env python3
"""Fail-closed tests for the J414s Mu profile manifest contract."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import stat
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
MODULE_PATH = REPO / "Tools" / "j414s_mu_profile_manifest.py"
SPEC = importlib.util.spec_from_file_location("j414s_mu_profile_manifest", MODULE_PATH)
M = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(M)


def record(path: str = "artifacts/file") -> dict[str, object]:
    return {"path": path, "size": 1, "sha256": "0" * 64}


def valid_shape(profile: str = "baseline") -> dict[str, object]:
    image_id = "sha256:" + "1" * 64
    tables = {
        name: {**record(f"Build/{name}"), "container_ffs_guid": M.ACPI_CONTAINERS[name], "occurrences_in_ffs": 1}
        for name in M.BASE_ACPI
    }
    if profile == "gpu":
        tables["GPU.aml"] = {
            **record("Build/GPU.aml"),
            "container_ffs_guid": M.ACPI_CONTAINERS["GPU.aml"],
            "occurrences_in_ffs": 1,
        }
    return {
        "schema": M.SCHEMA,
        "artifact_status": "READY_FOR_SUPERVISED_HARDWARE_TEST",
        "hardware_touched": False,
        "profile": M.profile_policy(profile),
        "source": {
            "checkout": str(REPO),
            "branch": M.BRANCH,
            "commit": "2" * 40,
            "tree": "3" * 40,
            "clean": True,
            "top_level_gitlinks": [],
            "nested_gitlinks": [],
            "nested_gitlink_lock": record("Tools/J414S_NESTED_GITLINK_LOCK.json"),
        },
        "builder": {
            "image_ref": "builder:tag",
            "image_id": image_id,
            "repo_digests": ["builder@" + image_id],
            "platform": "linux/arm64",
            "target": "DEBUG",
            "toolchain": "CLANGPDB",
        },
        "build": {
            "result": "SUCCESS",
            "images_verified": 94,
            "log": record("Build/log"),
            "report": record("Build/report"),
            "options": record("Build/options"),
            "defines": {},
            "pcds": {},
        },
        "firmware": record(f"artifacts/{M.FD_NAME}"),
        "firmware_volume": {
            "image": record("Build/FVMAIN.Fv"),
            "map": record("Build/FVMAIN.Fv.txt"),
            "ffs_count": M.PROFILES[profile]["expected_ffs_count"],
            "ffs": [],
            "required_baseline": M.REQUIRED_FFS,
            "optional_guids": M.OPTIONAL_FFS,
        },
        "acpi": {"tables": tables, "assertions": {}, "mcfg": {}},
    }


class ContractShapeTests(unittest.TestCase):
    def test_all_profile_abis_are_distinct_and_wireless_is_unavailable(self):
        abis = set()
        for profile in M.PROFILES:
            manifest = valid_shape(profile)
            M.validate_shape(manifest)
            M.validate_policy(manifest)
            M.validate_builder(manifest["builder"])
            abis.add(manifest["profile"]["profile_abi"])
            self.assertFalse(manifest["profile"]["experimental_features"]["wireless_dart_handoff"])
            self.assertFalse(manifest["profile"]["experimental_features"]["wifi_profile_available"])
        self.assertEqual(len(abis), len(M.PROFILES))

    def test_unknown_field_is_rejected(self):
        manifest = valid_shape()
        manifest["untrusted"] = True
        with self.assertRaises(M.ManifestError):
            M.validate_shape(manifest)

    def test_profile_claim_mutations_are_rejected(self):
        mutations = (
            ("profile_abi", "foreign.v1"),
            ("name", "wifi"),
            ("experimental", True),
        )
        for key, value in mutations:
            with self.subTest(key=key):
                manifest = valid_shape()
                manifest["profile"][key] = value
                with self.assertRaises(M.ManifestError):
                    M.validate_policy(manifest)
        manifest = valid_shape()
        manifest["profile"]["experimental_features"]["wireless_dart_handoff"] = True
        with self.assertRaises(M.ManifestError):
            M.validate_policy(manifest)

    def test_mutable_or_mismatched_builder_identity_is_rejected(self):
        manifest = valid_shape()
        for bad in ("builder:tag", "sha256:1234", ""):
            with self.subTest(image_id=bad):
                builder = copy.deepcopy(manifest["builder"])
                builder["image_id"] = bad
                with self.assertRaises(M.ManifestError):
                    M.validate_builder(builder)
        builder = copy.deepcopy(manifest["builder"])
        builder["repo_digests"] = ["builder@sha256:" + "4" * 64]
        with self.assertRaises(M.ManifestError):
            M.validate_builder(builder)

    def test_legacy_checkout_paths_are_rejected(self):
        for value in (
            "/Users/dj/Developer/mu-j414s-display-full/file",
            "/Users/dj/Developer/m1n1-j414s-old/file",
            "/repo/.git/worktrees/lane/file",
        ):
            with self.subTest(value=value), self.assertRaises(M.ManifestError):
                M.reject_legacy_paths(value, "fixture")
        M.reject_legacy_paths("/Users/dj/Developer/mu-j414s-windows-unified/file", "fixture")


class FileAndTreeTests(unittest.TestCase):
    def test_materialized_tree_digest_covers_content_mode_symlink_and_extra_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "a"
            source.write_bytes(b"one")
            link = root / "link"
            link.symlink_to("a")
            original = M.materialized_tree_sha256(root)
            source.write_bytes(b"two")
            self.assertNotEqual(original, M.materialized_tree_sha256(root))
            source.write_bytes(b"one")
            source.chmod(source.stat().st_mode | stat.S_IXUSR)
            self.assertNotEqual(original, M.materialized_tree_sha256(root))
            source.chmod(source.stat().st_mode & ~stat.S_IXUSR)
            link.unlink()
            link.symlink_to("missing")
            self.assertNotEqual(original, M.materialized_tree_sha256(root))
            link.unlink()
            link.symlink_to("a")
            (root / "extra").write_bytes(b"x")
            self.assertNotEqual(original, M.materialized_tree_sha256(root))

    def test_file_records_fail_on_hash_size_traversal_and_symlink_escape(self):
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory() as outside:
            root = Path(directory)
            path = root / "artifact"
            path.write_bytes(b"x")
            good = M.file_record(path, root)
            self.assertEqual(M.verify_file_record(good, root, "fixture"), path.resolve())
            for key, value in (("size", 2), ("sha256", "f" * 64), ("path", "../escape")):
                with self.subTest(key=key):
                    bad = copy.deepcopy(good)
                    bad[key] = value
                    with self.assertRaises(M.ManifestError):
                        M.verify_file_record(bad, root, "fixture")
            target = Path(outside) / "target"
            target.write_bytes(b"x")
            link = root / "link"
            link.symlink_to(target)
            bad = {"path": "link", "size": 1, "sha256": M.sha256(target)}
            with self.assertRaises(M.ManifestError):
                M.verify_file_record(bad, root, "fixture")

    def test_tracked_nested_lock_authenticates_current_materialization(self):
        _, nested = M.gitlink_inventory(REPO)
        lock = M.verify_nested_lock(REPO, nested)
        self.assertEqual(lock["path"], M.NESTED_LOCK.as_posix())
        tampered = copy.deepcopy(nested)
        tampered[0]["materialized_tree_sha256"] = "0" * 64
        with self.assertRaises(M.ManifestError):
            M.verify_nested_lock(REPO, tampered)


class EvidenceParserTests(unittest.TestCase):
    def test_build_evidence_parsers_fail_closed(self):
        log = "Edk2 build parameters are -D NTASI_ENABLE_ANS=FALSE -D NTASI_ENABLE_WIRELESS_DART_HANDOFF=0\n"
        self.assertEqual(M.parse_defines(log)["NTASI_ENABLE_ANS"], "FALSE")
        with self.assertRaises(M.ManifestError):
            M.parse_defines("no build command")
        report = "\n".join(
            f"{name} : FIXED (UINT64) = {value}"
            for name, value in (
                ("PcdAppleAnsPublishAcpiDevice", "0"),
                ("PcdAppleAnsPublishBlockIo", "0"),
                ("PcdAppleWirelessDartPageTableBase", "0x0"),
                ("PcdAppleWirelessDartPageTableSize", "0x0"),
            )
        )
        self.assertEqual(set(M.parse_pcd_values(report)), {
            "PcdAppleAnsPublishAcpiDevice",
            "PcdAppleAnsPublishBlockIo",
            "PcdAppleWirelessDartPageTableBase",
            "PcdAppleWirelessDartPageTableSize",
        })
        with self.assertRaises(M.ManifestError):
            M.parse_pcd_values(report.replace("PcdAppleAnsPublishBlockIo", "missing"))


if __name__ == "__main__":
    unittest.main()
