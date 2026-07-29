# J414s Windows Mu profiles

All J414s Windows firmware is built from the single standalone checkout on
`feature/j414s-windows-unified`. The default configuration is the shared,
hardware-usable baseline. Experimental firmware publication is selected at
compile time and remains in the same source tree.

The baseline includes PCI0/MCFG, USB/xHCI/storage, GPIO/input, AIC/DART,
display/SimpleFB, CPU/reset, and WinPE RAM-disk support. It does not publish
ANS, GPU resources, or a wireless DART handoff.

Use the profile builder from the root of the unified checkout:

```sh
Tools/build-j414s-windows-profile.sh baseline
Tools/build-j414s-windows-profile.sh ans
Tools/build-j414s-windows-profile.sh gpu
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

`NTASI_MU_PROFILE` accepts only `baseline`, `ans`, or `gpu`. ANS and GPU are
enabled one at a time. The wireless DART/DRT0 experiment intentionally has no
build profile yet: its firmware reservation and ACPI publication must remain
off until the matching m1n1 handoff contract has been measured and sealed.
