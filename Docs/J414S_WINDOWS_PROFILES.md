# J414s Windows Mu profiles

All J414s Windows firmware is built from the single standalone checkout on
`feature/j414s-windows-unified`. The default configuration is the shared,
hardware-usable baseline. Experimental firmware publication is selected at
compile time and remains in the same source tree.

The baseline includes PCI0/MCFG, USB/xHCI/storage, GPIO/input, AIC/DART,
display/SimpleFB, CPU/reset, and WinPE RAM-disk support. It does not publish
ANS, GPU resources, or a wireless DART handoff, and it does not enable the
wireless SID-1 DART page-table handoff.

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
Tools/build-j414s-windows-profile.sh wireless
Tools/build-j414s-windows-profile.sh ans-gpu-wireless
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

`NTASI_MU_PROFILE` accepts `baseline`, `ans`, `gpu`, `ans-gpu`, `wireless`, or
`ans-gpu-wireless`. ANS, GPU, and wireless publication can each be enabled
independently, or combined -- `ans-gpu` sets both
`PcdAppleAnsPublishAcpiDevice` and `NTASI_J414S_GPU_RESOURCE_PROFILE` and
therefore publishes `NTAS1000`/`NTAS2002`/`NTAS2003` (whichever the live ADT
selects) alongside `NTAS0023` GPU resources in the same FD; `ans-gpu-wireless`
additionally sets `NTASI_ENABLE_WIRELESS_DART_HANDOFF`, publishing ANS, GPU,
and the wireless DART handoff together. It carries its own `profile_abi`
(`ntasi.j414s.windows.ans-gpu-wireless-combined.v1`) and FFS count (baseline +
the `AppleNANDStorageDxe` driver + the `GpuAcpiTables` SSDT; wireless adds no
FFS module of its own -- see below).

CORRECTED 2026-07-30: wireless used to be mutually exclusive with the other
profiles, took a second argument (a same-instance `ntasi.j414s.wireless-handoff.v2`
manifest sealed by m1n1's authoritative verifier, capturing one specific
hardware-observed reservation address), and baked that exact base/size/limit
into `PcdAppleWirelessDartPageTableBase/Size/Limit` at build time. That was
the coordinator's own hand-picked test address (`0x103e0000000`), sealed after
the fact -- and the end user directly asked "wouldn't that be hard coding it?"
They were right. `MemoryInitPeiLib.c` now derives the reservation at PEI
runtime from that boot's own `boot_args` (`mem_size_actual`), the same way it
already derives `SystemMemoryTop`, and publishes the result through the
`PatchableInModule` PCDs `PcdAppleWirelessDartPageTableBase/Size` for
`AcpiPlatformDxe`'s `NtasiInstallWirelessDartTable()` to read back and publish
as a dynamically generated `DRT0`/`NTAS0011` SSDT (the same AmlLib technique
`AcpiPlatformInstallAppleAnsTable()` already uses for ANS -- no more static
`WDRT.asl`/`WirelessDartAcpiTables.inf`, both deleted). `wireless` therefore
takes no manifest, needs no m1n1-side evidence at build time, and adds no new
FFS module: a Docker (non-hardware) build of any profile always reports
`PcdAppleWirelessDartPageTableBase/Size == 0` in its manifest, because only a
real boot's PEI phase, processing that boot's actual `boot_args`, ever
computes a nonzero value. `PcdAppleWirelessDartPageTableLimit` no longer
exists as a PCD at all.

`NtasiDeriveWirelessReservation()` places the reservation at the top of Mu's
own `mem_size_actual`-derived window, working downward, rather than counting
up from `guest_top + 16KiB` -- this is what keeps it clear of the TrustZone
carveouts and iBoot `GUAT`/`CARV` ranges m1n1 unmaps near physical top, per
the coordinator's guidance. Mu has no ADT/boot_args-visible way to learn the
TZ0/TZ1 carveout bounds themselves (they live in privileged MCC registers,
per `m1n1/src/mcc.c`), so this derivation satisfies m1n1's own numeric
`wlan_validate_reservation()` formula by construction, but safety in practice
still depends on both m1n1 and Mu computing that same formula from the same
inputs -- see the comment above `NtasiDeriveWirelessReservation()` in
`MemoryInitPeiLib.c` for the full caveat.
