/** @file
  J414s internal display adapter ACPI device for the AppleDisplay WDDM
  display driver (rungs (a) and (b) of docs/j414s-display-driver.md).

  The inherited framebuffer is deliberately NOT an ACPI resource.  The WDDM
  display-only path takes its boot-specific physical address, geometry, and
  format from dxgkrnl at StartDevice via
  DxgkCbAcquirePostDisplayOwnership (the same POST-display buffer Mu's
  SimpleFbDxe published as the one-mode GOP). Embedding that framebuffer base
  here would be wrong twice over: the /vram carveout base is
  boot-specific (m1n1 rewrites boot_args.video.base each boot,
  src/display.c:586-606), and it would inject the scanout into Windows PnP
  arbitration for no benefit.

  The fixed _CRS ranges below are instead the exact six non-overlapping DCP
  and DART MMIO apertures required by rung (b). REG3 contains both the ASC
  CPU block at +0x400000 and mailbox at +0x408000, so those subranges are not
  published a second time. Keeping these resources on the display adapter
  avoids a second PnP driver and an undocumented cross-device lifetime
  protocol: DxgkDdiStartDevice receives the translated resource list and
  validates it before mapping anything.

  The _DSD properties are the pinned J414s geometry, provided ONLY as a
  cross-check for the driver and for offline verification tooling; none is a
  resource and the driver treats DxgkCbAcquirePostDisplayOwnership as
  authoritative. Interrupts are deliberately absent for the first DCP
  bring-up: physical AIC lines 932-935/911 have not yet been proven as usable
  Windows GSIVs. The driver therefore selects bounded polling. A later ACPI
  revision must publish the complete five-line set atomically; the driver
  rejects partial or unexpected interrupt inventories.

  Provenance:
    Pinned scanout geometry 3024x1964 BGRA32 stride 12096: WIP.md display
      note (commit 474037b), tools/m1n1-windows-debug.py win_capture_framebuffer.
    Panel 302 x 196 mm: Asahi linux-asahi t6020-j414s.dts panel node; also
      the argument Mu SimpleFbDxe passes to its synthesized EDID
      (SimpleFbDxe.c:369).
    _HID NTAS0070: next free NTASP ACPI id (drivers/ grep NTAS0001..0067).

  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("DISP.aml", "SSDT", 0x02, "Apple", "J414DSP", 0x00000002)
{
    Scope (\_SB)
    {
        Device (DISP)
        {
            Name (_HID, "NTAS0070")
            Name (_UID, Zero)
            Name (_CCA, One)

            Name (_CRS, ResourceTemplate ()
            {
                // DCP disp-0 register aperture.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000388000000,
                    0x000000038861BFFF,
                    0x0000000000000000,
                    0x000000000061C000
                    )
                // DCP firmware DART (stream 5).
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x000000038930C000,
                    0x000000038930FFFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // DISP0 scanout DART (stream 0; piodma stream 4).
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000389304000,
                    0x0000000389307FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // DCP disp-1 register aperture.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000389320000,
                    0x0000000389323FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // DCP disp-2 register aperture.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000389344000,
                    0x0000000389347FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // DCP disp-3 aperture, including ASC CPU and mailbox.
                QWordMemory (
                    ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x0000000000000000,
                    0x0000000389800000,
                    0x0000000389FFFFFF,
                    0x0000000000000000,
                    0x0000000000800000
                    )
            })

            //
            // Geometry cross-check only. Keep in lockstep
            // with drivers/AppleDisplay/AppleDisplayModeCore.h and the
            // firmware EDID Mu already publishes.
            //
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "ntasp,fb-width", 3024 },
                    Package () { "ntasp,fb-height", 1964 },
                    Package () { "ntasp,fb-stride", 12096 },
                    Package () { "ntasp,fb-format", "BGRA8888" },
                    Package () { "ntasp,panel-width-mm", 302 },
                    Package () { "ntasp,panel-height-mm", 196 },
                    Package () { "ntasp,preboot-owner", "m1n1-dcp-then-quiesced" },
                    Package () { "ntasp,preboot-handoff-required", Zero }
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}
