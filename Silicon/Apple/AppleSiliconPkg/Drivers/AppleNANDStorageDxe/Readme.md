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

## Hardware variants

- T8015-class ANS uses conventional submission queues, 16-entry admin and I/O
  queues, and 128-byte I/O command slots.
- T8103 and the current ANS2/ANS3 compatible families use Apple's linear
  submission queue and NVMMU TCBs, with a 2-entry admin queue and 64-entry I/O
  queue.
- SART v0, v2, and v3 are selected from ADT and existing bootloader-owned
  entries are preserved.

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
