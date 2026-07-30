/** @file
  Generated J414s/T6020 G14X GPU ACPI contract. DO NOT EDIT.

  Manifest SHA-256: a87b0b8119cb12572bd0fd2d3ac83ee524dcae66005e64fb0e182c263fcf2d06
  m1n1 commit: 5b54d17e3cf7c82a9af54512fc63dcd292abd72c
  m1n1 image SHA-256: d4d9da57bf154b99ae2d302118f79b586e506068412b7fa6c1b58cfa9e808775

  UNVERIFIED STALENESS NOTE (2026-07-30): MemoryInitPeiLib.c no longer
  hardcodes these six QWordMemory addresses -- it derives them at boot from
  the live "/arm-io/sgx" ADT node (uat_ttbs/uat_pagetables/uat_handoff) and
  from Mu's own observed SystemMemoryTop (hw_data_a/hw_data_b/globals), the
  way m1n1's kboot_gpu.c derives the same six regions for Linux. The three
  ADT-sourced addresses are fixed silicon carveouts and should never drift
  for this board. The three top-of-memory addresses below happen to match
  what PEI computes tonight (SystemMemoryTop = 0x103db29c000), but they are
  only a snapshot: if SystemMemoryTop ever moves (a different m1n1 build,
  a different Mu feature profile that carves more or less memory before
  this point), PEI's *behavior* stays safe (it degrades the GPU reservation
  and logs loudly rather than hanging), but this static _CRS will go stale
  again and the AppleAgxGpu Windows driver will simply fail its CRC check
  and not load -- not hang the boot. Treat GPU.asl regeneration as owed
  whenever the "ans"/"gpu" feature mix or m1n1 build changes, and re-run
  whatever produced NTASI-GPU-OVERLAY.json / GPU-HANDOFF.json to confirm.

  SPDX-License-Identifier: MIT
**/

DefinitionBlock ("GPU.aml", "SSDT", 0x02, "Apple", "J414GPU", 0x00000001)
{
    Scope (\_SB)
    {
        Device (GPU0)
        {
            Name (_HID, "NTAS0023")
            Name (_UID, Zero)
            Name (_CCA, One)

            // AppleAgxGpu requires this exact descriptor order.
            Name (_CRS, ResourceTemplate ()
            {
                // 0: ASC
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x0000000406400000,
                    0x000000040643FFFF,
                    0x0000000000000000,
                    0x0000000000040000
                    )
                // 1: SGX
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x0000000404000000,
                    0x0000000404FFFFFF,
                    0x0000000000000000,
                    0x0000000001000000
                    )
                // 2: uat_ttbs
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x00000103FFFB8000,
                    0x00000103FFFBBFFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // 3: uat_pagetables
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x00000103FFF78000,
                    0x00000103FFFB7FFF,
                    0x0000000000000000,
                    0x0000000000040000
                    )
                // 4: uat_handoff
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadWrite,
                    0x0000000000000000,
                    0x00000103FFF70000,
                    0x00000103FFF73FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // 5: hw_data_a
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadOnly,
                    0x0000000000000000,
                    0x00000103DB294000,
                    0x00000103DB29BFFF,
                    0x0000000000000000,
                    0x0000000000008000
                    )
                // 6: hw_data_b
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadOnly,
                    0x0000000000000000,
                    0x00000103DB290000,
                    0x00000103DB293FFF,
                    0x0000000000000000,
                    0x0000000000004000
                    )
                // 7: globals
                QWordMemory (
                    ResourceConsumer,
                    PosDecode,
                    MinFixed,
                    MaxFixed,
                    NonCacheable,
                    ReadOnly,
                    0x0000000000000000,
                    0x00000103DB278000,
                    0x00000103DB28FFFF,
                    0x0000000000000000,
                    0x0000000000018000
                    )
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                {
                    40
                }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "ntasp,gpu-chip-id", 0x6020 },
                    Package () { "ntasp,gpu-generation", 14 },
                    Package () { "ntasp,gpu-variant", "G14X" },
                    Package () { "ntasp,gpu-firmware-compat-major", 13 },
                    Package () { "ntasp,gpu-firmware-compat-minor", 5 },
                    Package () { "ntasp,gpu-max-frequency-khz", 1398000 },
                    Package () { "ntasp,gpu-mailbox-aic-line", 1146 },
                    Package () { "ntasp,gpu-mailbox-gsiv", 40 },
                    Package () { "ntasp,gpu-pmgr-gpx-offset", 0x0 },
                    Package () { "ntasp,gpu-pmgr-afr-offset", 0x100 },
                    Package () { "ntasp,gpu-pmgr-gfx-offset", 0x108 },
                    Package () { "ntasp,gpu-pmgr-afr-min-state", 4 },
                    Package () { "ntasp,gpu-pmgr-gpx-always-on", One },
                    Package () { "ntasp,hw-data-a-payload-size", 0x6C34 },
                    Package () { "ntasp,hw-data-b-payload-size", 0x1884 },
                    Package () { "ntasp,globals-payload-size", 0x1715C },
                    Package () { "ntasp,hw-data-a-crc32", "fac3327a" },
                    Package () { "ntasp,hw-data-b-crc32", "8360bea5" },
                    Package () { "ntasp,globals-crc32", "8ac088ef" },
                    Package () { "ntasp,preboot-owner", "m1n1" },
                    Package () { "ntasp,preboot-handoff-required", One }
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}
