/** Portable C integer types for the shared ANS cores in EDK II. */
#ifndef APPLE_ANS_EDK_STDINT_H
#define APPLE_ANS_EDK_STDINT_H

#include <Base.h>

typedef INT8   int8_t;
typedef UINT8  uint8_t;
typedef INT16  int16_t;
typedef UINT16 uint16_t;
typedef INT32  int32_t;
typedef UINT32 uint32_t;
typedef INT64  int64_t;
typedef UINT64 uint64_t;

#define UINT16_C(Value)  Value##U
#define UINT32_C(Value)  Value##U
#define UINT64_C(Value)  Value##ULL

#define UINT8_MAX   MAX_UINT8
#define UINT16_MAX  MAX_UINT16
#define UINT32_MAX  MAX_UINT32
#define UINT64_MAX  MAX_UINT64

#endif
