/** @file
  Minimal freestanding <string.h> shim for the vendored, host-tested MTP
  protocol cores (AppleDockChannel.c / AppleMtpProtocol.c / AppleMtpStream.c),
  which are kept byte-identical to drivers/AppleMtpHid in the
  apple_silicon_nt_drivers repository so that its host test suite remains
  the authority on their behavior.

  The firmware toolchain is freestanding: the compiler provides
  <stdint.h>/<stddef.h>/<limits.h>, but not <string.h>.  This shim declares
  only the three functions the cores use; the symbols themselves come from
  the toolchain intrinsics the platform already links
  (HAS_MEMCPY_INTRINSICS is set in the platform DSC BuildOptions).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef APPLE_MTP_INPUT_DXE_STRING_H_
#define APPLE_MTP_INPUT_DXE_STRING_H_

#include <stddef.h>

void  *memcpy (void *Dest, const void *Source, size_t Count);
void  *memset (void *Dest, int Value, size_t Count);
int   memcmp (const void *Left, const void *Right, size_t Count);

#endif
