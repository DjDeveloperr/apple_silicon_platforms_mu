/** Portable C memory operations for the shared ANS cores in EDK II. */
#ifndef APPLE_ANS_EDK_STRING_H
#define APPLE_ANS_EDK_STRING_H

#include <Library/BaseMemoryLib.h>

#define memcpy(Destination, Source, Length) \
  CopyMem ((Destination), (Source), (Length))
#define memset(Buffer, Value, Length) \
  SetMem ((Buffer), (Length), (Value))

#endif
