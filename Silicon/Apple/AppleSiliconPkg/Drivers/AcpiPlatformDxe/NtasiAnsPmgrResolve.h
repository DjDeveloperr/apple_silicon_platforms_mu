/** @file
  Pure, freestanding, host-testable arithmetic for resolving Apple PMGR
  power-state register addresses from the live ADT's "/arm-io/pmgr" node,
  by exact device name -- mirrors m1n1's pmgr_find_device() +
  pmgr_device_get_addr() (src/pmgr.c) byte for byte.

  WHY THIS EXISTS: on 2026-07-30, Mu's PcdAppleAnsPmgr*Base PCDs were
  hardcoded against the wrong "/arm-io/pmgr" register block. The node's
  own "ps-regs" property describes *multiple* physical register blocks
  (each a {reg_idx, reg_offset} pair indexing the node's own multi-tuple
  "reg" property) -- ANS2/APCIE_ST/APCIE_ST_SYS/APCIE_ST1_SYS live in the
  "pmgr_east" block, not the main "pmgr" block the hardcoded constants
  pointed into. At the exact same low offsets in the wrong block sit
  DCS_09/DCS_10 -- DRAM controller power domains -- so the wrong constants
  passed every alignment/distinctness sanity check while pointing at
  completely different, far more dangerous hardware.

  This header has ZERO dependencies -- no EDK2 headers, no <stdint.h>, no
  <stdbool.h> -- for the same reason NtasiGpuReservationGuard.h does not:
  EDK2 PEI/DXE builds are typically -nostdinc, so the exact same text
  compiles unmodified both into AcpiPlatform.c (under the EDK2/Clang
  AArch64 toolchain) and into Tests/test_ans_pmgr_resolve.c (compiled
  directly with the host cc, no EDK2, no hardware, no ADT involved).

  The device-table byte layout below is modeled as raw offsets into a byte
  buffer, not a C struct, on purpose: it stays includable from a
  freestanding host test with no struct-packing/ABI assumptions about the
  including compiler, and it is the literal 48-byte layout of
  `struct pmgr_device` in m1n1's src/pmgr.c (PACKED, verified field by
  field against that source: flags=1, unk1=2, id1=1, parent-union=4,
  unk3=2, addr_offset=1 @10, psreg_idx=1 @11, unk4=14, id2=2, unk5=4,
  name=16 @32 -- 48 bytes total).

  THE GUARD THIS BUYS: NtasiPmgrFindDomainAddress() can only ever return
  an address it found attached to the exact device name it was asked to
  look for, read fresh from the live ADT on this boot. There is no numeric
  fallback anywhere in this header -- a caller that only ever asks for
  "ANS2"/"APCIE_ST"/"APCIE_ST_SYS"/"APCIE_ST1_SYS" cannot be handed back a
  DCS address by construction, the way a hardcoded constant could be wrong
  without anything noticing.

  SPDX-License-Identifier: MIT
**/

#ifndef NTASI_ANS_PMGR_RESOLVE_H_
#define NTASI_ANS_PMGR_RESOLVE_H_

typedef unsigned long long  NTASI_PMGR_U64;
typedef unsigned int        NTASI_PMGR_U32;
typedef unsigned char       NTASI_PMGR_U8;
typedef int                 NTASI_PMGR_BOOL;

#define NTASI_PMGR_TRUE   1
#define NTASI_PMGR_FALSE  0

/* Byte layout of one "/arm-io/pmgr" "devices" entry (48 bytes, packed). */
#define NTASI_PMGR_DEVICE_SIZE          48u
#define NTASI_PMGR_DEVICE_ADDR_OFFSET   10u /* u8 addr_offset */
#define NTASI_PMGR_DEVICE_PSREG_IDX     11u /* u8 psreg_idx */
#define NTASI_PMGR_DEVICE_NAME_OFFSET   32u /* char name[16] */
#define NTASI_PMGR_DEVICE_NAME_LEN      16u

/* One PS register group, addr = RegTupleBases[reg_idx] + reg_offset. */
#define NTASI_PMGR_PSREG_STRIDE  3u /* {reg_idx, reg_offset, unused} u32 triples */

/**
  TRUE if device `Index`'s 16-byte name field exactly equals `Name`
  (NUL-terminated, at most 16 bytes including the terminator). Never reads
  past `Name`'s own terminator, and never reads past the 16-byte name
  field, regardless of `Name`'s length.
**/
static inline NTASI_PMGR_BOOL
NtasiPmgrDeviceNameEquals (
  const unsigned char  *Devices,
  NTASI_PMGR_U32        Index,
  const char            *Name
  )
{
  const unsigned char  *Field;
  NTASI_PMGR_U32         I;
  unsigned char           Expected;

  Field = Devices + (NTASI_PMGR_U64)Index * NTASI_PMGR_DEVICE_SIZE + NTASI_PMGR_DEVICE_NAME_OFFSET;
  for (I = 0; I < NTASI_PMGR_DEVICE_NAME_LEN; I++) {
    Expected = (unsigned char)Name[I];
    if (Field[I] != Expected) {
      return NTASI_PMGR_FALSE;
    }
    if (Expected == 0) {
      return NTASI_PMGR_TRUE;
    }
  }
  /* Name field is exactly 16 bytes with no terminator seen: only a match
   * if Name is also exactly 16 (non-NUL-terminated within the field) --
   * none of the domains this file cares about are that long, so treat as
   * no match rather than reading Name[16] (which may not be valid). */
  return NTASI_PMGR_FALSE;
}

static inline NTASI_PMGR_U8
NtasiPmgrDeviceAddrOffset (
  const unsigned char  *Devices,
  NTASI_PMGR_U32        Index
  )
{
  return Devices[(NTASI_PMGR_U64)Index * NTASI_PMGR_DEVICE_SIZE + NTASI_PMGR_DEVICE_ADDR_OFFSET];
}

static inline NTASI_PMGR_U8
NtasiPmgrDevicePsRegIdx (
  const unsigned char  *Devices,
  NTASI_PMGR_U32        Index
  )
{
  return Devices[(NTASI_PMGR_U64)Index * NTASI_PMGR_DEVICE_SIZE + NTASI_PMGR_DEVICE_PSREG_IDX];
}

/**
  Compute one device's PMGR PS register address from its (PsRegIdx,
  AddrOffset) and the node-wide ps-regs/reg-tuple tables. Matches m1n1's
  pmgr_get_psreg() + pmgr_device_get_addr() (no per-die offset -- callers
  on a multi-die SoC must add PMGR_DIE_OFFSET*die themselves; J414s/T6020
  is single-die).

  RegTupleBases[reg_idx] must already hold the resolved base address of
  the pmgr ADT node's reg_idx'th own "reg" tuple (i.e. the result of
  dt_node_reg(PmgrNode, reg_idx, ...) on the EDK2 side); this header does
  not parse #address-cells/#size-cells "reg" tuples itself, so it stays
  includable from a plain host test with a synthetic table.
**/
static inline NTASI_PMGR_BOOL
NtasiPmgrResolveAddress (
  const NTASI_PMGR_U64  *RegTupleBases,
  NTASI_PMGR_U32          RegTupleCount,
  const NTASI_PMGR_U32  *PsRegs,
  NTASI_PMGR_U32          PsRegsCount,
  NTASI_PMGR_U8           PsRegIdx,
  NTASI_PMGR_U8           AddrOffset,
  NTASI_PMGR_U64         *Address
  )
{
  NTASI_PMGR_U32  RegIdx;
  NTASI_PMGR_U32  RegOffset;

  if (((NTASI_PMGR_U32)PsRegIdx + 1u) * NTASI_PMGR_PSREG_STRIDE > PsRegsCount) {
    return NTASI_PMGR_FALSE;
  }

  RegIdx    = PsRegs[NTASI_PMGR_PSREG_STRIDE * PsRegIdx];
  RegOffset = PsRegs[NTASI_PMGR_PSREG_STRIDE * PsRegIdx + 1u];

  if (RegIdx >= RegTupleCount) {
    return NTASI_PMGR_FALSE;
  }

  *Address = RegTupleBases[RegIdx] + (NTASI_PMGR_U64)RegOffset + ((NTASI_PMGR_U64)AddrOffset << 3);
  return NTASI_PMGR_TRUE;
}

/**
  Find the unique "/arm-io/pmgr" device named exactly `Name` and resolve
  its PS register address. NTASI_PMGR_FALSE (and *Address left untouched)
  if zero or more than one device matches, or if its psreg_idx/reg_idx
  cannot be resolved against the supplied tables -- ambiguity is refused,
  never guessed at. This is the only entry point callers should use; it is
  the one place the "never return an address for the wrong name" guarantee
  is enforced.
**/
static inline NTASI_PMGR_BOOL
NtasiPmgrFindDomainAddress (
  const unsigned char   *Devices,
  NTASI_PMGR_U32          DeviceCount,
  const NTASI_PMGR_U64  *RegTupleBases,
  NTASI_PMGR_U32          RegTupleCount,
  const NTASI_PMGR_U32  *PsRegs,
  NTASI_PMGR_U32          PsRegsCount,
  const char             *Name,
  NTASI_PMGR_U64         *Address
  )
{
  NTASI_PMGR_U32  Index;
  NTASI_PMGR_U32  MatchCount;
  NTASI_PMGR_U64  Resolved;
  NTASI_PMGR_BOOL Ok;

  MatchCount = 0;
  Resolved   = 0;
  Ok         = NTASI_PMGR_TRUE;

  for (Index = 0; Index < DeviceCount; Index++) {
    if (!NtasiPmgrDeviceNameEquals (Devices, Index, Name)) {
      continue;
    }

    MatchCount++;
    if (MatchCount > 1) {
      continue; /* keep counting to report ambiguity accurately */
    }

    Ok = NtasiPmgrResolveAddress (
           RegTupleBases,
           RegTupleCount,
           PsRegs,
           PsRegsCount,
           NtasiPmgrDevicePsRegIdx (Devices, Index),
           NtasiPmgrDeviceAddrOffset (Devices, Index),
           &Resolved
           );
  }

  if ((MatchCount != 1) || !Ok) {
    return NTASI_PMGR_FALSE;
  }

  *Address = Resolved;
  return NTASI_PMGR_TRUE;
}

#endif // NTASI_ANS_PMGR_RESOLVE_H_
