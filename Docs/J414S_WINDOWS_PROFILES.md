# J414s Windows Mu profiles

All J414s Windows firmware is built from the single standalone checkout on
`feature/j414s-windows-unified`. The default configuration is the shared,
hardware-usable baseline. Experimental firmware publication is selected at
compile time and remains in the same source tree.

The baseline includes PCI0/MCFG, USB/xHCI/storage, GPIO/input, AIC/DART,
display/SimpleFB, CPU/reset, and WinPE RAM-disk support. It does not publish
ANS, GPU resources, or a wireless DART handoff.

The baseline also publishes right-side `XHC2` as ACPI UID 2 / GSIV 39 and
contains `AppleUsbTypeCBringupDxe`. Its external CD3217 policy is owned by the
paired m1n1 `m1n1_non_proxy_source_dfp_v1` handoff, not reconstructed in Mu.
The current contract enables the USB2 host PHY only; SuperSpeed and live
validation remain explicitly false in the artifact manifest.

Use the profile builder from the root of the unified checkout:

```sh
Tools/build-j414s-windows-profile.sh baseline
Tools/build-j414s-windows-profile.sh ans
Tools/build-j414s-windows-profile.sh gpu
Tools/build-j414s-windows-profile.sh ans-gpu
Tools/build-j414s-windows-profile.sh wireless /absolute/path/wireless-handoff.json
```

The builder refuses a linked worktree, the wrong branch, or dirty/untracked
source. It mounts the source read-only in the build container. Each profile
uses its own commit-keyed `Build`, `Conf`, `artifacts`, lock, and manifest
directory under:

```text
../apple_silicon_nt_drivers/build/m2-pro/<profile>/<source-commit>/
```

Set `NTASI_MU_OUTPUT_ROOT` to relocate the profile output root or
`NTASI_MU_BUILD_IMAGE` to select an already-installed compatible builder
image. Agents must deploy only the FD named by that profile's manifest; they
must never copy an FD out of a different profile directory.

Every successful build produces a `ntasi.j414s.mu-profile.v2` manifest. The
manifest binds the FD and build proof to the source commit/tree, the full
top-level and materialized nested gitlink ledger, the builder image's immutable
digest, the 94-image validation result, every FFS payload, and compiled ACPI
evidence. Verify it without modifying the artifacts:

```sh
Tools/verify-j414s-windows-profile.py verify \
  --manifest ../apple_silicon_nt_drivers/build/m2-pro/baseline/<commit>/artifacts/manifest.json \
  --source-root .
```

`NTASI_MU_PROFILE` accepts `baseline`, `ans`, `gpu`, `ans-gpu`, or `wireless`.
ANS and GPU publication can each be enabled independently, or together via the
combined `ans-gpu` profile, which sets both `PcdAppleAnsPublishAcpiDevice` and
`NTASI_J414S_GPU_RESOURCE_PROFILE` and therefore publishes `NTAS1000`/
`NTAS2002`/`NTAS2003` (whichever the live ADT selects) alongside `NTAS0023`
GPU resources in the same FD. It carries its own `profile_abi`
(`ntasi.j414s.windows.ans-gpu-combined.v1`) and FFS count (baseline + the
`AppleNANDStorageDxe` driver + the `GpuAcpiTables` SSDT). Wireless publication
remains mutually exclusive with the others: the wireless profile has no fixed
reservation and cannot be built from source flags alone. Its
second argument must be a same-instance `ntasi.j414s.wireless-handoff.v2`
manifest sealed by m1n1's authoritative verifier. The builder re-runs that
verifier, authenticates its referenced m1n1 manifest and full 64-KiB capture,
then pins the exact dynamic base, inclusive limit, and size into both PEI and
`DRT0` ACPI. Missing evidence, old versions, fixed-layout legacy policy, source
drift, range mismatch, or any descriptor/table CRC mismatch fails closed.

The next capture must use authoritative unified m1n1 commit
`3994baa8f3bb856f923481c71290b63a4ccb7e69`, manifest SHA-256
`ea409b4dd8c8ed0a90267fc3679eea9243fce4ae295e3a5f0e0a73b0c94140c3`,
and Mach-O SHA-256
`763e7721c9960239676c6401386f14f36c8b535c59ec3d5464838bc082a09aa3`.
Those pins do not authorize a build without a fresh live capture, and a reset
between capture and Mu boot invalidates the resulting profile.
