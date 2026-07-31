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

`NTASI_MU_PROFILE` accepts `baseline`, `ans`, `ans-noacpi`, `gpu`,
`gpu-noacpi`, `ans-gpu`, `wireless`, `gpu-wireless`, `ans-gpu-wireless`,
`media`, or `media-gpu` -- the same set `Tools/j414s_mu_profile_manifest.py`
seals, pinned key-for-key by `Tests/test_j414s_gpu_acpi_contract.py`.
ANS, GPU, and wireless publication can each be enabled
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

## The `media` profile: MCA0, AOPA and ISP0, with zero interrupts

`media` publishes the three ACPI devices the J414s media drivers bind to:

| Device | `_HID` | `_CRS` windows | What binds to it |
|---|---|---|---|
| `MCA0` | `NTAS0080` | 9 | `AppleMcaAudio` -- MCA I2S/TDM complex, ADMAC, NCO, six TAS2764 amps |
| `AOPA` | `NTAS0081` | 4 | `AppleAopAudio` -- AOP internal PDM microphone array |
| `ISP0` | `NTAS0090` | 8 | `AppleIsp` -- FaceTime camera coprocessor |

It changes exactly one thing a static build can prove: the
`NTASI_ENABLE_MEDIA_PUBLICATION` compiler define. Like `gpu` and `wireless` it
adds **no FFS module** -- `NtasiInstallMediaTables()` in `AcpiPlatformDxe`
builds all three SSDTs with `AmlLib` at DXE runtime, the same technique `ANS0`
and `DRT0` use -- so its `expected_ffs_count` equals baseline's and the
94-image count is unchanged. `profile_abi` is
`ntasi.j414s.windows.media-publication.v1`.

**Default OFF means byte-identical, not merely equivalent.** The entire
generator is inside `#if NTASI_ENABLE_MEDIA_PUBLICATION`, so a profile without
the flag compiles the same bytes it compiled before this feature existed. A
static ASL table was deliberately *not* used: it would land in the firmware
volume of every profile including the baseline that boots.

**Seven published GSIVs, and the media profile's own CSRT.** Each device's
interrupt list is appended *after* its memory windows, so the positional memory
contract is untouched:

| Device | Published | Physical | Translated by CSRT? |
|---|---|---|---|
| `MCA0` | 40, 41, 42, 43, 45 | 1218, 1211, 1213, 1221, 1231 | **yes** -- all above the 1019 limit |
| `AOPA` | 631 | 631 | no -- identity mapped |
| `ISP0` | 569 | 569 | no -- identity mapped |

Because MCA0's real lines are above Windows' GIC arbiter limit of 32..1019, the
media profile builds the **`m2-pro-media`** CSRT (8 aliases, 296 bytes,
sha256 `a082eb6c…`) instead of the ordinary **`m2-pro`** (3 aliases, 256 bytes,
sha256 `cdee0da8…`). `CSRT.aslc` selects it on the same
`NTASI_ENABLE_MEDIA_PUBLICATION` flag, so table and `_CRS` cannot get out of
step. **Every non-media profile's CSRT is byte-for-byte unchanged**, and the
media table is a strict *superset* -- the boot USB controller's `37 -> 1274`
alias is bit-identical in both.

**Never 44.** AIC 44 belongs to `/arm-io/i2c0/hpmBusManager` in the live ADT. An
early draft proposed `44 -> 1231`; it was withdrawn. `run-m2-pro-mu.sh` refuses
any manifest publishing it, by name.

**`media` and `gpu` are mutually exclusive** and `CSRT.aslc` `#error`s if both
are set: published GSIV 40 is the AGX mailbox (`40 -> 1146`) in the GPU table
and `admac-sio` (`40 -> 1218`) here. No shipped profile selects both.

**Measured caveat — six of the seven vectors are inert today.** Only `AppleIsp`
consumes an interrupt resource. `AppleMcaAudio` and `AppleAopAudio` contain no
`CmResourceTypeInterrupt` handling and no `WdfInterrupt` object at all
(`AppleMcaMapResources()` skips every non-memory descriptor), so their
bring-up remains wholly polled and those descriptors do nothing for the shipped
binaries. They are published so the streaming path's resources are already
arbitrated and so a grant now is evidence of a grant later — at the cost that
every one of them must be satisfiable for its devnode to start.

**Nothing can make a sound.** `ntasp,mca-allow-render` is absent from every
table. The MCA render path needs that ACPI property *and* an
`AllowSpeakerRender` registry value *and* a compile-time constant in
`AppleMcaAudio` that is `0`; all three stay shut, and the manifest records
`media_speaker_render_enabled: false`, which the launcher also enforces. The
internal speakers have no thermal protection on Windows and the amplifiers
power on at maximum analog gain.

**The `_CRS` order is a contract.** All three drivers match memory descriptors
positionally and fail closed only on a *short* list; a *reordered* list is not
detected, and for `AppleIsp` it would put a DART TTBR write into a coprocessor
control register. `MCA0` publishes nine windows because
`AppleMcaMapResources()` accepts exactly 8, 9 or 12 and refuses anything
between -- the three capture windows (`i2c2`, `pinctrl_nub`, `sio_dart`) are
all-or-nothing and are withheld for first light because they arm code that
mutates hardware. `ISP0` window 4 is `0x4034` long *exactly*, not page-rounded.
`Platform/MacBookProEarly2023Pkg/AcpiTables/Media/{MCA,AOPA,ISP}.asl` is the
readable specification (the build does not compile it) and
`Tests/test_j414s_media_acpi_contract.py` pins it against the C tables.

**Known, unfixed-in-firmware: a pmgr_east resource overlap.** `MCA0` window 4
is `[0x290280000, 0x290280FFF]` and `ISP0` window 4 is
`[0x290280000, 0x290284033]`. `KBL0` (`NTAS0051`) already claims that page
exclusively in `KBL.asl`. Same defect class as the NTAS2003-vs-KBL0 collision
fixed on 2026-07-30.

Splitting the window to straddle KBL0's page was considered and **rejected as
impossible without a driver change**, for two independent reasons:

1. Both drivers need registers *inside* KBL0's page — ISP0 needs `ps_isp_sys`
   at `+0x1c8`, and every one of MCA0's ten power words (`ps_sio` `0x1C0`
   through `ps_mca3` `0x3B0`) is below `+0x1000`. There is no split that avoids
   the overlap; only one that reduces it.
2. A split turns one descriptor into two, changing ISP0's count from 8 to 9 and
   shifting `DARTLLT`/`DARTBULK`/`DARTRT` from indices 5/6/7 to 6/7/8 — exactly
   the reorder that would put a DART TTBR write into a coprocessor register.

So it stays, documented. **Expect `CM_PROB_NORMAL_CONFLICT` (Code 12) on `MCA0`
and `ISP0`**: `KBL0` is enumerated from a table installed before the runtime
SSDTs and is the likely winner of the page. `AOPA` has no pmgr window and no
overlap, so it is the one new device expected to start. The real fix is
driver-side and is the one ANS already took: read the pmgr_east base from
`_DSD` and drop the window.

## `NTAS0023` (GPU) is published, and how it was made safe

**Changed 2026-07-31.** This section used to record a decision *not* to publish.
It now records how publishing was made safe, because the reasons it was unsafe
were addressable and have been addressed.

### What was wrong

`GPU.asl`'s `_CRS` hardcoded `hw_data_a` at `[0x103db294000, 0x103db29c000)` --
inside OS RAM, ending exactly at `SystemMemoryTop`, and containing the exact
`SP_EL1` (`0x103db29ba10`) that crashed Mu's PEI twice. Those addresses were
real, but they came from m1n1's `dt_set_gpu()` calling `top_of_memory_alloc()`
on a *different* boot, on the Linux path -- which also shrinks the DT memory
node so Linux never owns them. This project's chainload/HV path never calls
`dt_set_gpu()` and never shrinks anything, so Mu's `boot_args` still covered
them. The table was also never installed: its FFS GUID was not one of the four
`Pcd*AcpiTableStorageFile` GUIDs `AcpiPlatformDxe` reads.

`GPU.asl` and `GpuAcpiTables.inf` remain deleted, and the manifest still fails
if `GPU.aml` is ever rebuilt. The device is now generated at DXE runtime with
`AmlLib` by `NtasiInstallGpuTable()`, exactly like `ANS0` and `DRT0` -- which is
what stops any address being baked into a build artifact again.

### The eight resources, and where each now comes from

`AppleAgxGpu` matches `_CRS` **positionally** and
`ntasi_agx_t6020_resources_validate()` rejects any count other than eight, so
the order is a contract and a short list cannot be improvised.

| # | Resource | Source | Safe because |
|---|---|---|---|
| 0 | ASC `0x406400000 +0x40000` | driver-ABI constant, **proven** against live ADT | MMIO; a strict subset of `/arm-io/gfx-asc` `reg[0]` (`+0x6C000`) |
| 1 | SGX `0x404000000 +0x1000000` | driver-ABI constant, **proven** against live ADT | MMIO; **contains** `sgx` `reg[0]` and `reg[1]`, and the GPU PMGR page at `+0xE80000` the driver derives |
| 2 | `uat_ttbs` `0x103fffb8000 +0x4000` | live ADT `gpu-region` | above `SystemMemoryTop`, below real DRAM top |
| 3 | `uat_pagetables` `0x103fff78000 +0x40000` | live ADT `gfx-shared-region` | ditto |
| 4 | `uat_handoff` `0x103fff70000 +0x4000` | live ADT `gfx-handoff` | ditto |
| 5 | `hw_data_a` `+0x8000` | **firmware-allocated** | `EfiReservedMemoryType`, so Windows never owns it |
| 6 | `hw_data_b` `+0x4000` | **firmware-allocated** | ditto |
| 7 | `globals` `+0x18000` | **firmware-allocated** | ditto |

Resources 0 and 1 cannot be *derived*: the driver validates them against exact
constants and returns `ERR_FIXED` for anything else, so publishing the ADT's own
window lengths would be more truthful and would be **refused**. They are
therefore hardcoded and then checked -- the difference between this and
`GPU.asl` is a constant that is proven against the machine rather than trusted.

Resources 2-4 are bounded against **real installed DRAM**
(`ALIGN_DOWN(phys_base, 4GiB) + mem_size_actual`), not against `boot_args`'
`mem_size`. They legitimately live *above* the `mem_size` ceiling, in the pool
iBoot and m1n1 reserve for themselves; that is exactly why the naive bound was
wrong and why `NtasiGpuReservationGuard.h` exists.

### Resources 5-7: backed, not guessed and not omitted

There is no live source for them -- a full probe of `/arm-io/sgx` finds only
`gpu-region`, `gfx-shared-region`, `gfx-handoff`, `ttbat-phys-addr-base` and
`rtkit-private-vm-region-*` -- and deriving them from Mu's own window is what
crashed the machine. So firmware **allocates** one 16 KiB-aligned
`EfiReservedMemoryType` block and carves the three pinned sizes out of it,
zero-filled.

An earlier version of this document claimed firmware-allocated regions would be
refused by "the driver's carveout gate", so publication "buys nothing". That was
factually wrong. `AgxkmdVerifyCarveoutsNotOsOwned()` walks
`MmGetPhysicalMemoryRanges()` and refuses ranges Windows *owns*; reserved pages
are excluded from that list and are precisely the `RESERVED` verdict it accepts.
The refusal happens later, in the calibration blob validator, which detects an
all-zero blob **deliberately** -- because the DT reserves zero placeholders
before m1n1 fills them. So publication buys the machine reaching the one gate
that *names* the missing thing, instead of the device never existing.

Nothing is claimed falsely: `_DSD` carries `ntasp,preboot-handoff-present = 0`
whenever the blobs are placeholders, and the payload sizes and CRC32s the
deleted `GPU.asl` asserted are **not** republished -- they described data
captured on a different boot.

### The interrupt: GSIV **46**, not 40

One vector: the AGX ASC mailbox doorbell, physical AIC line **1146**
(`/arm-io/gfx-asc` `interrupts[2]`), above the carrier's 1019 limit and so
translated by the CSRT `ALI2` tail as `46 -> 1146`.

It was 40 until 2026-07-31 -- which is also the media profile's `admac-sio`
(`40 -> 1218`). `CSRT.aslc` carried a compile-time `#error` making media and GPU
mutually exclusive rather than fixing the clash. The **GPU** alias moved because
media's 40..45 block is a shipped, documented allocation with pinned CSRT bytes
and a driver-side `_DSD`, while the GPU alias had never been booted and is not
read by `AppleAgxGpu` at all (it takes its GSIV from the `_CRS` descriptor).

46 is free on all five allocation rules: inside `[32,1024)`, not another alias's
published GSIV, not any alias's physical line, outside the MSI bank, and --
the rule that matters -- **claimed by no node in the live ADT**, unlike 44
(`/arm-io/i2c0/hpmBusManager`).

The `#error` was replaced by something **stricter**, not deleted: `CSRT.aslc`
now names every published GSIV and `STATIC_ASSERT`s that no two collide,
pairwise, across every feature combination. That catches the original 40-vs-40
case, the 44 rule, *and* collisions purely between two media aliases -- a class
the `#error` could never have caught. `ntasi_aic2_t6020_aliases_are_wellformed()`
enforces the same invariant a second, independent time at emission.

### Consequences

* Media and GPU are no longer mutually exclusive. The `media-gpu` profile and
  the 9-alias `m2-pro-media-gpu` CSRT exist.
* `gpu-noacpi` is the single-variable control, the GPU analogue of
  `ans-noacpi`: carveouts reserved and the CSRT alias present, device not
  published.
* The launcher guard is no longer an unconditional refusal. It is a
  both-directions cross-check plus an exact published-GSIV set, so an FD whose
  publication flag disagrees with its profile name -- or which republishes 40 --
  is refused rather than launched.
* `AppleAgxGpu` is `StartType=3` (demand-start), `ErrorControl=1`, one service
  per binary, and explicitly **non-WDDM**. It is never loaded by `winload`,
  cannot participate in boot-device selection, and registers no display adapter,
  so it cannot blank the SimpleFb console. A devnode that fails to start costs
  the GPU and nothing else.

### The first boot of this code wedged Mu, and why (2026-07-31)

Mu `3450262` (FD `a9d1cae1`, profile `ans-gpu-wireless`) stopped dead inside
`AcpiPlatformDxe`. It was first reported as "Mu emitted nothing at all". **It
emitted 146,790 bytes.** They were in
`build/m2-pro-readiness/logs/mu-secondary-uart.log` between offsets `121015323`
and `121162113` -- the offsets the launcher itself printed -- and the tail is:

```
AppleAgxGpu: MMIO windows agree with the live ADT
AppleAgxGpu: stage "allocate-placeholder-handoff"
ASSERT_EFI_ERROR (Status = Invalid Parameter)
ASSERT [AcpiPlatform] MemoryAllocationLib.c(222): !(((RETURN_STATUS)(Status)) >= 0x8000000000000000ULL)
```

The 0-byte file that was read instead, `mu-<stamp>.log`, is `run_guest`'s
primary trace; it is 0 bytes on **every** run, including the ones that reach
Windows. All Mu firmware text lives only in `mu-secondary-uart.log`, which is
cumulative across runs and must be sliced by the printed offset.

The cause was `AllocateAlignedReservedPages (36 pages, 16 KiB)`. That function
implements alignment by over-allocating and freeing the remainder back
(`MemoryAllocationLib.c:189-223`), but the DXE core forces
`EfiReservedMemoryType` to `RUNTIME_PAGE_ALLOCATION_GRANULARITY` -- 64 KiB on
AArch64 -- when allocating (`Page.c:1649-1657`) **and** when freeing
(`Page.c:1928-1941`). The base therefore came back 64 KiB aligned and the head
free was skipped, but the tail free at `Base + 0x24000` was only 16 KiB aligned,
was refused with `EFI_INVALID_PARAMETER`, and `ASSERT_EFI_ERROR` turned that
into `CpuDeadLoop()` in a `DEBUG` build. Nothing dispatched behind
`AcpiPlatformDxe` ever ran.

The alignment wrapper was never needed: the core already returns reserved
memory 64 KiB aligned, which satisfies the driver's 16 KiB requirement. The fix
is a plain `AllocateReservedPages()` of a granularity-rounded page count, a
fail-closed alignment check on the returned base instead of an `ASSERT`, and no
free-back at all. Two `STATIC_ASSERT`s in `AcpiPlatform.c` and a ban on
`AllocateAligned{Reserved,Runtime}*` in `Tests/test_j414s_gpu_acpi_contract.py`
keep the class of bug out.

The general lesson, and it is not GPU-specific: **any MdePkg allocator wrapper
that ends in `ASSERT_EFI_ERROR` is a `CpuDeadLoop()` in the DEBUG builds this
project boots.** `FreePages()` is one of them. A DXE driver that promises "boot
unaffected" cannot call them on a path it expects to fail.

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
