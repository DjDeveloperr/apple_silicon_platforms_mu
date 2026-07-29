/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Host-portable Apple DockChannel register and packet helpers.
 *
 * Register semantics are derived from m1n1's MIT-licensed
 * proxyclient/m1n1/hw/dockchannel.py and from the Asahi Linux DockChannel
 * driver (drivers/soc/apple/dockchannel.c, dual GPL-2.0/MIT).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define APPLE_DOCKCHANNEL_IRQ_MASK_OFFSET       UINT32_C(0x0000)
#define APPLE_DOCKCHANNEL_IRQ_FLAG_OFFSET       UINT32_C(0x0004)

#define APPLE_DOCKCHANNEL_CONFIG_TX_THRESH      UINT32_C(0x0000)
#define APPLE_DOCKCHANNEL_CONFIG_RX_THRESH      UINT32_C(0x0004)
#define APPLE_DOCKCHANNEL_DATA_REGION_OFFSET    UINT32_C(0x4000)

#define APPLE_DOCKCHANNEL_DATA_TX8              UINT32_C(0x0004)
#define APPLE_DOCKCHANNEL_DATA_TX16             UINT32_C(0x0008)
#define APPLE_DOCKCHANNEL_DATA_TX24             UINT32_C(0x000c)
#define APPLE_DOCKCHANNEL_DATA_TX32             UINT32_C(0x0010)
#define APPLE_DOCKCHANNEL_DATA_TX_FREE          UINT32_C(0x0014)
#define APPLE_DOCKCHANNEL_DATA_RX8              UINT32_C(0x001c)
#define APPLE_DOCKCHANNEL_DATA_RX16             UINT32_C(0x0020)
#define APPLE_DOCKCHANNEL_DATA_RX24             UINT32_C(0x0024)
#define APPLE_DOCKCHANNEL_DATA_RX32             UINT32_C(0x0028)
#define APPLE_DOCKCHANNEL_DATA_RX_COUNT          UINT32_C(0x002c)

#define APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE    8U
#define APPLE_DOCKCHANNEL_PACKET_CHECKSUM_SIZE  4U
#define APPLE_DOCKCHANNEL_PACKET_MAX_INTERFACE  15U
#define APPLE_DOCKCHANNEL_PACKET_MAX_BODY       65532U
#define APPLE_DOCKCHANNEL_PACKET_MAX_SIZE       \
    (APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE + \
     APPLE_DOCKCHANNEL_PACKET_MAX_BODY + \
     APPLE_DOCKCHANNEL_PACKET_CHECKSUM_SIZE)

#define APPLE_DOCKCHANNEL_CHANNEL_COMMAND       0x11U
#define APPLE_DOCKCHANNEL_CHANNEL_REPORT        0x12U

typedef enum _APPLE_DOCKCHANNEL_STATUS {
    AppleDockChannelSuccess = 0,
    AppleDockChannelInvalidArgument,
    AppleDockChannelInvalidLength,
    AppleDockChannelInvalidAlignment,
    AppleDockChannelInvalidHeader,
    AppleDockChannelInvalidChannel,
    AppleDockChannelInvalidInterface,
    AppleDockChannelInvalidChecksum,
    AppleDockChannelIncompletePacket,
    AppleDockChannelTrailingData,
    AppleDockChannelBufferTooSmall
} APPLE_DOCKCHANNEL_STATUS;

typedef struct _APPLE_DOCKCHANNEL_PACKET_VIEW {
    uint8_t HeaderLength;
    uint8_t Channel;
    uint16_t BodyLength;
    uint8_t Sequence;
    uint8_t Interface;
    uint16_t Padding;
    const uint8_t *Body;
    uint32_t Checksum;
    size_t PacketLength;
} APPLE_DOCKCHANNEL_PACKET_VIEW;

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelIrqBits(
    unsigned int Index,
    uint32_t *TxBit,
    uint32_t *RxBit
    );

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelUpdateIrqMask(
    uint32_t CurrentMask,
    unsigned int Index,
    int EnableTx,
    int EnableRx,
    uint32_t *NewMask
    );

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelDecodeIrqFlags(
    uint32_t Flags,
    unsigned int Index,
    int *TxPending,
    int *RxPending
    );

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelGetTxRegister(
    unsigned int Width,
    uint32_t *Offset
    );

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelGetRxRegister(
    unsigned int Width,
    uint32_t *Offset
    );

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelDecodeNarrowRx(
    uint32_t RegisterValue,
    unsigned int Width,
    uint8_t *Data,
    size_t Capacity,
    uint8_t *CountField
    );

size_t
AppleDockChannelClampThreshold(
    size_t FifoSize,
    size_t Requested
    );

unsigned int
AppleDockChannelTransferWidth(
    size_t Available,
    size_t Remaining
    );

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelComputeChecksum(
    const uint8_t *Data,
    size_t Length,
    uint32_t *Checksum
    );

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelBuildPacket(
    uint8_t Channel,
    uint8_t Sequence,
    uint8_t Interface,
    const uint8_t *Body,
    size_t BodyLength,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    );

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelParsePacket(
    const uint8_t *Packet,
    size_t PacketLength,
    APPLE_DOCKCHANNEL_PACKET_VIEW *View
    );
