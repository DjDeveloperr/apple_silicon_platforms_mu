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

## `DRT0` publication: fixed (was a PEI -> DXE plumbing bug)

`PcdAppleWirelessDartPageTableBase/Size` are `PcdsPatchableInModule`. A
`PatchableInModule` PCD is a **per-module copy**: `MemoryInitPeiLib`'s
`PatchPcdSet64/32` wrote the copy linked into `PrePi`, and `AcpiPlatformDxe`'s
`PcdGet64/32` read their own never-patched copies, which are always the DEC
default of zero. `NtasiInstallWirelessDartTable()` therefore took its "no
reservation published by PEI this boot" path on **every** boot regardless of
what PEI derived and authenticated, and `DRT0` was never installed. The log
line blamed the wrong phase.

The cross-phase channel is now a GUID HOB
(`NTASI_WIRELESS_DART_RESERVATION_HOB_GUID`), the same mechanism
`MemoryInitPeiLib.c` already used for the appended ramdisk. PEI publishes the
reservation only after it authenticates the ABI v2 descriptor; DXE reads the
HOB and then **re-authenticates the descriptor itself** before publishing
`DRT0`. The re-check is not a formality -- it proves the reservation survived
all of PEI and DXE dispatch byte-intact, and publishing a DART page-table base
to Windows on the strength of a HOB alone would mean trusting a structure
nothing re-checked.

The validator (`NtasiValidateWirelessHandoffV2` + its CRC-32) now lives once,
in `Include/IndustryStandard/J414sWirelessHandoff.h`, and both phases call it.
`Tests/test_wireless_handoff_validate.py` compiles that header verbatim on the
host and pins every field, both page-table CRCs, and the caller-supplied
bounds -- 37 assertions, every one of them fail-closed.

The PCDs are still patched, because `BuildVirtualMemoryMap()` consumes them
from inside `PrePi` where the patch **is** visible. They are simply no longer
the cross-phase channel. Failure is still fail-closed at every step: no HOB, a
malformed HOB, or a descriptor that fails re-authentication all withhold
`DRT0` loudly, and a zero is never published.

The same trap was confirmed on hardware for `PcdSystemMemoryBase/Size`: every
GPU-profile boot printed the DSC default window `[0x10000000000,
0x10400000000)` rather than this machine's real `[0x10001E40000,
0x103DB29C000)`, which made the GPU carveout guard reject all three live ADT
carveouts. `AcpiPlatform.c` derives both windows from `boot_args` instead (see
`NtasiDeriveBootArgsWindows()`) and logs a `DEBUG_WARN` when the PCDs disagree.

## `NTAS0023` (GPU) is deliberately NOT published

This is a decision, recorded here and in the artifact manifest
(`experimental_features.gpu_acpi_ntas0023_publication: false`). It used to be
an accident, twice over:

* `GPU.asl`'s FFS GUID (`2CC5C83E-…`) was never one of the four
  `Pcd*AcpiTableStorageFile` GUIDs `AcpiPlatformDxe` reads, so the table was
  compiled into every `gpu`-profile FV and **never installed**.
* Its `_CRS` hardcoded `hw_data_a` at `[0x103db294000, 0x103db29c000)` --
  inside OS RAM, ending exactly at `SystemMemoryTop`, and containing the exact
  `SP_EL1` (`0x103db29ba10`) that crashed Mu's PEI twice.

`GPU.asl` and `GpuAcpiTables.inf` are deleted, and the manifest now **fails**
if `GPU.aml` is ever rebuilt, so those addresses cannot ship again. The `gpu`
profile's FFS count is consequently baseline's, exactly like `wireless`:
selecting it changes one thing a static build can prove, the
`NTASI_J414S_GPU_RESOURCE_PROFILE` define, which gates the ADT-derived,
DRAM-bounded GCD carveout reservations.

`AppleAgxGpu`'s `_CRS` needs eight resources. Resources 2-4
(`uat_ttbs`/`uat_pagetables`/`uat_handoff`) are real silicon carveouts and are
resolved live from `/arm-io/sgx`, bounded against real DRAM, and reserved.
Resources 5-7 (`hw_data_a`/`hw_data_b`/`globals`) have **no source on this boot
path**: no ADT property carries them, m1n1's `dt_set_gpu()` never runs on the
chainload/HV path, and computing them from Mu's own window crashed the machine
twice. Publishing them would either hand the driver addresses inside memory
Windows owns, or claim pre-computed init data exists when it does not. The
Windows `AppleAgxGpu` carveout gate correctly refuses either way, so
publication buys nothing and risks real harm.

**What unblocks it:** `NtasiResolveAndReserveGpuCarveouts()` already probes all
six regions with one naming convention. The moment `/arm-io/sgx` carries
`hw-data-a-base/-size`, `hw-data-b-base/-size` and `gpu-globals-base/-size` --
i.e. m1n1 publishes the preboot handoff its own `_DSD` contract
(`ntasp,preboot-owner` = `"m1n1"`, `ntasp,preboot-handoff-required` = `One`)
already promises -- all six resolve, the log says so in one line, and
generating `NTAS0023` is a mechanical follow-up on the same AmlLib path `ANS0`
and `DRT0` already use.

**Worth raising with the AGX workstream:** in Asahi these three are not preboot
carveouts at all. `HwDataA`/`HwDataB`/`Globals` are AGX *initdata* structures
the GPU driver builds itself at runtime from the ADT's power/perf tables; m1n1
only forwards those tables and never allocates a region for them. If
`AppleAgxGpu` built them the same way, resources 5-7 would not need to exist
and `NTAS0023` could be published today from resources 0-4 alone. That is a
driver-side ABI question, not one firmware can settle unilaterally, which is
why nothing here forces it. The deleted `GPU.asl`'s `_DSD` (chip id, perf
data, PMGR offsets, payload sizes and the three expected CRC32s) is recoverable
from git history if that ABI is revisited.

## The `ans-noacpi` control, and what the USB3 0x144 investigation has ruled out

`BUGCODE_USB3_DRIVER 0x144` (`USB3_BUGCODE_BOOT_DEVICE_FAILED`) reproduces on
every ANS-carrying profile and on no other: 6 boots / 0 failures for non-ANS
profiles versus 0 / 3 for ANS profiles, on the same cable. XHC1 is captured
halted with `USBSTS 0x1d [HCH|HSE|EINT|PCD]` and DWC3 `buserr_valid=1`. `HSE`
is Host System Error -- the host bus rejected the controller's DMA.

**Ruled out so far, each by measurement rather than argument:**

* **SART.** m1n1 instantiates exactly one, `sart_init("/arm-io/sart-ans")`
  (`src/nvme.c:334`, `:445`), consumed only by the ANS RTKit instance. In the
  device tree `apple,sart` is a property of the nvme node alone. It is also an
  *allow* list: an armed entry permits ANS DMA to a range and can never reject
  another master's transaction. Confirmed clean at handoff on hardware --
  `protected=0x002F` (all iBoot-owned), `owned=0x0000`.
* **The memory map.** Highest described physical address is `0x103DB29C000` in
  both `ans` and `baseline`. All ANS reserved allocations land inside valid RAM.
* **ANS hardware mutation.** The `ans` profile at `bde9ff10` has the entire ANS
  mutation path dead-stripped by LTO -- no `asc-init`, no `rtkit-boot`, no SART
  writes, no handoff -- and **still bugchecks**. Mu cannot touch ANS hardware in
  that build.
* **XHC1's IOMMU.** There isn't one in the path: `AppleDartIoMmuDxe` programs
  every USB DART stream to `TCR = BYPASS_DART|BYPASS_DAPF` and installs no
  `gEdkiiIoMmuProtocol`. Measured at the crash: `DARTTCR usb1 ['0x6' x8]`. XHC1
  DMAs to raw physical addresses.

**Still open, and what `ans-noacpi` exists to separate:**

1. `NTAS2003` is published, so Windows builds a devnode and its PnP arbiter
   allocates resources for it -- an interrupt (GSIV 38, aliased to physical AIC
   line 1832) and four 4-byte PMGR memory ranges -- even though
   `AppleNvmeSart3` is disabled (`Start=4`) and never loads.
2. The FD layout differs: 88 FFS versus 87.

`PcdAppleAnsPublishAcpiDevice` used to be wired to `$(NTASI_ENABLE_ANS)`, the
same define that gates the driver's FFS in the FDF, so the two could not be
varied independently. It now has its own `$(NTASI_ANS_PUBLISH_ACPI)`, and the
`ans-noacpi` profile sets `ans=TRUE` / `ans_acpi=FALSE`: the same 88-FFS module
set as `ans`, with no `NTAS2003` at all.

  * `ans-noacpi` boots -> the cause is the ACPI device and the resources
    Windows allocates for it.
  * `ans-noacpi` fails -> the cause is the FD layout / memory footprint, and
    one variable remains.

The FFS *count* and module set are identical between `ans` and `ans-noacpi`;
the FDs are not byte-identical, because `PcdAppleAnsPublishAcpiDevice` is
`FixedAtBuild` and LTO strips `AcpiPlatformInstallAppleAnsTable()` out of
`AcpiPlatformDxe` when it is FALSE. That is a size delta inside one existing
FFS, not a different set of modules, and it is verifiable: `NTAS2003` occurs
3 times in `ans`'s `AcpiPlatform.efi` and 0 times in `ans-noacpi`'s.

This matters beyond ANS. Device Manager reports **code 12 ("cannot find enough
free resources") on all four PCIe devices** on a wireless-enabled boot. If
Windows' resource arbiter is already failing on this platform, one more ACPI
device claiming an interrupt is a far more plausible route from ANS to an
unrelated USB controller than anything ANS does to its own hardware -- and the
two problems may share a root cause in how Mu describes resources.

## 2026-07-30: ANS handoff and GPU carveout bounds

Two hardware-confirmed corrections, both from
`build/m2-pro-readiness/logs/mu-secondary-uart.log` in the drivers repo.

ANS does **not** hang. The `ans` profile completes every bring-up stage and
boots Windows; the earlier `sart-init` exception was the unmapped
`/arm-io/ans` MMIO aperture, fixed by `4a2b071`. What remained was
`AppleANS: RTKit handoff failed: -25` from `AnsExitBootServices()`.
`-25` is `NTASI_RTKIT_RUNTIME_ERR_BUFFER`; the causes were a buffer request
carrying a non-zero IOVA being rejected (m1n1 adopts the coprocessor's own
address and sends no reply), a 44-bit IOVA mask where m1n1 uses
`GENMASK(41, 0)`, and unmodelled system messages aborting the receive where
m1n1 logs and continues. The handoff is now fail-safe: the ASC run bit is
driven low unconditionally, SART grants are revoked only once the coprocessor
is confirmed halted, and the RTKit shared buffers are 16 KiB-granular
`EfiReservedMemoryType` so they survive into the OS instead of being handed
back seconds before Windows reuses them. Pinned by
`Tests/test_rtkit_buffer_request.py`.

GPU carveouts are now bounded by the machine's real installed-DRAM window
(`ALIGN_DOWN(phys_base, 4GiB) + mem_size_actual`, m1n1's own
`top_of_memory_alloc()` formula) via `NtasiRangeWithinWindow()`. That is the
bound the original incident actually lacked: three of the six hardcoded
constants sat above `boot_args`' `mem_size` ceiling, and PEI's HOB-punching
reservation path could not represent them, so `MemoryInitPeiLib` returned a
fatal status with no console. Nothing is hardcoded and nothing is bounded by a
build-time default any more. Pinned by
`Tests/test_gpu_reservation_guard.py`.

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
