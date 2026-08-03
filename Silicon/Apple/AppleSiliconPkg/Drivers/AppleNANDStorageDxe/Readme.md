# Apple NAND Storage DXE Driver

## About

This DXE driver exposes the internal Apple NAND Storage controller through
UEFI Block I/O and Device Path protocols. DiskIoDxe and PartitionDxe can then
discover filesystems and boot loaders on the internal SSD.

The implementation includes the ASC mailbox transport, RTKit endpoint and
power-state negotiation, SART DMA-window management, Apple NVMe controller
lifecycle, namespace identification, and polling read/write/flush paths.

## Why is this driver necessary?

In Apple devices, the boot drives are not standard PCIe NVMe devices. The
internal flash connects to a SoC coprocessor called Apple NAND Storage (ANS).
The AP starts that coprocessor over an ASC mailbox and negotiates RTKit before
using Apple's modified NVMe register and queue interface.

The standard UEFI PCIe NVMe driver therefore cannot drive this hardware.

## Ownership and Windows boot

ANS is already owned by iBoot when m1n1 enters, and it may still be running
when Mu starts. Mu must choose its RTKit path from that observed state:

- a running coprocessor is attached with `WAKE`; Mu does not write its run bit;
- a stopped coprocessor is reset, released with a cold `RUN` write, and is
  allowed to send RTKit `HELLO` before Mu transmits anything;
- a preserve-for-OS build allocates every queue, TCB, bounce area, and RTKit
  shared buffer as `EfiReservedMemoryType`; ordinary quiesce/reset builds may
  use `EfiBootServicesData` for the controller-only allocations;
- at ExitBootServices, an internal-storage boot deletes Mu's I/O queues and
  disables only the NVMe controller. It deliberately keeps the RTKit/ASC
  coprocessor running and preserves all shared buffers and SART grants. The
  Windows AppleNvme driver then takes its warm `WAKE` path and programs wholly
  new queues. If the bounded controller shutdown fails, reserved queue memory
  keeps the still-live DMA master from targeting pages Windows can reclaim.

The `internal-storage` J414s build profile is the only profile that enables
DXE bring-up, read-only Block I/O, and live OS handoff together. Other profiles
retain their existing quiesce/reset policy.

Mu includes DiskIoDxe, PartitionDxe, and the FAT filesystem driver. It does
not include an NTFS filesystem driver. Therefore an NTFS Windows partition by
itself is not a firmware boot volume. The internal GPT must also contain a
FAT32 EFI System Partition with `\EFI\Microsoft\Boot\bootmgfw.efi` and its BCD
store; `bootmgfw.efi` loads Windows from the NTFS partition after firmware has
handed over the block device. Adding an NTFS parser to Mu is neither required
nor part of the ANS ownership solution.

## Hardware variants

- T8015-class ANS uses conventional submission queues, 16-entry admin and I/O
  queues, and 128-byte I/O command slots.
- T8103 and the current ANS2/ANS3 compatible families use Apple's linear
  submission queue and NVMMU TCBs, with a 2-entry admin queue and 64-entry I/O
  queue.
- SART v0, v2, and v3 are selected from ADT and existing bootloader-owned
  entries are preserved while the inherited coprocessor may still DMA.

These classes follow the current Asahi Linux compatibility data rather than a
hard-coded board list. The Mu platform packages for M1, M1 Pro/Max/Ultra, and
M2 Pro/Max include the driver in their firmware volumes.

## Why this hardware design?

There are several benefits to this design, including integration with SoC data
protection and AES hardware.

## Validation status

The portable controller, block, ASC/RTKit, and SART cores are host-tested in
the Windows driver repository. This DXE driver builds and links into the M1 Mu
firmware images with CLANGPDB and passes Mu's PE/COFF image validator.

Real ANS firmware, flash media, timing, and DMA coherency still require
on-device validation. A successful emulated or firmware build is not evidence
that writes are safe on a physical internal SSD; use expendable media and a
recoverable backup for initial hardware testing.
