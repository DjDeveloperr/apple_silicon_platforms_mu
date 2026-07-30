/*
 * Minimal host stand-in for EDK II's <Base.h>, used ONLY by Tests/ so that
 * headers under Silicon/Apple/AppleSiliconPkg/Include can be compiled
 * verbatim by a plain host C compiler.
 *
 * This exists so the wireless handoff validator that PEI (MemoryInitPeiLib.c)
 * and DXE (AcpiPlatform.c) now share can be regression-tested without an EDK2
 * build, a cross toolchain, or hardware -- the same principle as
 * NtasiGpuReservationGuard.h and NtasiAnsPmgrResolve.h, which avoid EDK2
 * headers entirely. J414sWirelessHandoff.h cannot do that: it is a wire-ABI
 * header that must use EDK II's exact types and packing in firmware.
 *
 * Only the constructs that header actually uses are defined. It is not a
 * general EDK2 emulation and must not grow into one.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NTASI_TEST_EDK2_BASE_H_
#define NTASI_TEST_EDK2_BASE_H_

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int32_t   INT32;
typedef uintptr_t UINTN;
typedef unsigned char BOOLEAN;
typedef char      CHAR8;
typedef void      VOID;

#define TRUE   ((BOOLEAN)(1 == 1))
#define FALSE  ((BOOLEAN)(0 == 1))

#define MAX_UINT32  ((UINT32)0xFFFFFFFFU)
#define MAX_UINT64  ((UINT64)0xFFFFFFFFFFFFFFFFULL)

#define BIT0  0x00000001

#define STATIC        static
#define CONST         const
#define IN
#define OUT

#define SIGNATURE_16(A, B)        ((A) | ((B) << 8))
#define SIGNATURE_32(A, B, C, D)  (SIGNATURE_16 (A, B) | ((UINT32)(SIGNATURE_16 (C, D)) << 16))

#define STATIC_ASSERT  _Static_assert

#endif /* NTASI_TEST_EDK2_BASE_H_ */
