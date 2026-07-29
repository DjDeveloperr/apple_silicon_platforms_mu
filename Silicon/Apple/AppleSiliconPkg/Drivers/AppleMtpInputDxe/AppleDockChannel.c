/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple DockChannel portable core.
 *
 * Provenance: m1n1 proxyclient/m1n1/hw/dockchannel.py (MIT) and Asahi
 * Linux drivers/soc/apple/dockchannel.c (GPL-2.0-only OR MIT).
 */

#include "AppleDockChannel.h"

#include <string.h>

static uint16_t
AppleDockChannelReadLe16(
    const uint8_t *Data
    )
{
    return (uint16_t)((uint16_t)Data[0] | ((uint16_t)Data[1] << 8));
}

static uint32_t
AppleDockChannelReadLe32(
    const uint8_t *Data
    )
{
    return (uint32_t)Data[0] |
           ((uint32_t)Data[1] << 8) |
           ((uint32_t)Data[2] << 16) |
           ((uint32_t)Data[3] << 24);
}

static void
AppleDockChannelWriteLe16(
    uint8_t *Data,
    uint16_t Value
    )
{
    Data[0] = (uint8_t)Value;
    Data[1] = (uint8_t)(Value >> 8);
}

static void
AppleDockChannelWriteLe32(
    uint8_t *Data,
    uint32_t Value
    )
{
    Data[0] = (uint8_t)Value;
    Data[1] = (uint8_t)(Value >> 8);
    Data[2] = (uint8_t)(Value >> 16);
    Data[3] = (uint8_t)(Value >> 24);
}

static int
AppleDockChannelIsKnownChannel(
    uint8_t Channel
    )
{
    return Channel == APPLE_DOCKCHANNEL_CHANNEL_COMMAND ||
           Channel == APPLE_DOCKCHANNEL_CHANNEL_REPORT;
}

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelIrqBits(
    unsigned int Index,
    uint32_t *TxBit,
    uint32_t *RxBit
    )
{
    unsigned int shift;

    if (TxBit == NULL || RxBit == NULL)
        return AppleDockChannelInvalidArgument;
    if (Index >= 16U)
        return AppleDockChannelInvalidArgument;

    shift = Index * 2U;
    *TxBit = UINT32_C(1) << shift;
    *RxBit = UINT32_C(2) << shift;
    return AppleDockChannelSuccess;
}

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelUpdateIrqMask(
    uint32_t CurrentMask,
    unsigned int Index,
    int EnableTx,
    int EnableRx,
    uint32_t *NewMask
    )
{
    uint32_t txBit;
    uint32_t rxBit;
    APPLE_DOCKCHANNEL_STATUS status;

    if (NewMask == NULL)
        return AppleDockChannelInvalidArgument;
    status = AppleDockChannelIrqBits(Index, &txBit, &rxBit);
    if (status != AppleDockChannelSuccess)
        return status;

    CurrentMask = EnableTx ? (CurrentMask | txBit)
                           : (CurrentMask & ~txBit);
    CurrentMask = EnableRx ? (CurrentMask | rxBit)
                           : (CurrentMask & ~rxBit);
    *NewMask = CurrentMask;
    return AppleDockChannelSuccess;
}

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelDecodeIrqFlags(
    uint32_t Flags,
    unsigned int Index,
    int *TxPending,
    int *RxPending
    )
{
    uint32_t txBit;
    uint32_t rxBit;
    APPLE_DOCKCHANNEL_STATUS status;

    if (TxPending == NULL || RxPending == NULL)
        return AppleDockChannelInvalidArgument;
    status = AppleDockChannelIrqBits(Index, &txBit, &rxBit);
    if (status != AppleDockChannelSuccess)
        return status;

    *TxPending = (Flags & txBit) != 0U;
    *RxPending = (Flags & rxBit) != 0U;
    return AppleDockChannelSuccess;
}

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelGetTxRegister(
    unsigned int Width,
    uint32_t *Offset
    )
{
    static const uint32_t offsets[4] = {
        APPLE_DOCKCHANNEL_DATA_TX8,
        APPLE_DOCKCHANNEL_DATA_TX16,
        APPLE_DOCKCHANNEL_DATA_TX24,
        APPLE_DOCKCHANNEL_DATA_TX32
    };

    if (Offset == NULL)
        return AppleDockChannelInvalidArgument;
    if (Width == 0U || Width > 4U)
        return AppleDockChannelInvalidArgument;
    *Offset = offsets[Width - 1U];
    return AppleDockChannelSuccess;
}

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelGetRxRegister(
    unsigned int Width,
    uint32_t *Offset
    )
{
    static const uint32_t offsets[4] = {
        APPLE_DOCKCHANNEL_DATA_RX8,
        APPLE_DOCKCHANNEL_DATA_RX16,
        APPLE_DOCKCHANNEL_DATA_RX24,
        APPLE_DOCKCHANNEL_DATA_RX32
    };

    if (Offset == NULL)
        return AppleDockChannelInvalidArgument;
    if (Width == 0U || Width > 4U)
        return AppleDockChannelInvalidArgument;
    *Offset = offsets[Width - 1U];
    return AppleDockChannelSuccess;
}

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelDecodeNarrowRx(
    uint32_t RegisterValue,
    unsigned int Width,
    uint8_t *Data,
    size_t Capacity,
    uint8_t *CountField
    )
{
    unsigned int index;

    if (Data == NULL || CountField == NULL)
        return AppleDockChannelInvalidArgument;
    if (Width == 0U || Width > 3U)
        return AppleDockChannelInvalidArgument;
    if (Capacity < Width)
        return AppleDockChannelBufferTooSmall;

    *CountField = (uint8_t)RegisterValue;
    RegisterValue >>= 8;
    for (index = 0; index < Width; index++) {
        Data[index] = (uint8_t)RegisterValue;
        RegisterValue >>= 8;
    }
    return AppleDockChannelSuccess;
}

size_t
AppleDockChannelClampThreshold(
    size_t FifoSize,
    size_t Requested
    )
{
    return Requested < FifoSize ? Requested : FifoSize;
}

unsigned int
AppleDockChannelTransferWidth(
    size_t Available,
    size_t Remaining
    )
{
    if (Available == 0U || Remaining == 0U)
        return 0U;
    if (Available >= 4U && Remaining >= 4U)
        return 4U;
    return 1U;
}

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelComputeChecksum(
    const uint8_t *Data,
    size_t Length,
    uint32_t *Checksum
    )
{
    uint32_t sum = 0U;
    size_t offset;

    if (Checksum == NULL || (Data == NULL && Length != 0U))
        return AppleDockChannelInvalidArgument;
    if ((Length & 3U) != 0U)
        return AppleDockChannelInvalidAlignment;

    for (offset = 0; offset < Length; offset += 4U)
        sum += AppleDockChannelReadLe32(Data + offset);
    *Checksum = UINT32_MAX - sum;
    return AppleDockChannelSuccess;
}

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
    )
{
    size_t required;
    uint32_t checksum;
    APPLE_DOCKCHANNEL_STATUS status;

    if (Output == NULL || OutputLength == NULL ||
        (Body == NULL && BodyLength != 0U))
        return AppleDockChannelInvalidArgument;
    *OutputLength = 0U;
    if (!AppleDockChannelIsKnownChannel(Channel))
        return AppleDockChannelInvalidChannel;
    if (Interface > APPLE_DOCKCHANNEL_PACKET_MAX_INTERFACE)
        return AppleDockChannelInvalidInterface;
    if (BodyLength > APPLE_DOCKCHANNEL_PACKET_MAX_BODY)
        return AppleDockChannelInvalidLength;
    if ((BodyLength & 3U) != 0U)
        return AppleDockChannelInvalidAlignment;

    required = APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE + BodyLength +
               APPLE_DOCKCHANNEL_PACKET_CHECKSUM_SIZE;
    if (OutputCapacity < required)
        return AppleDockChannelBufferTooSmall;

    Output[0] = APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE;
    Output[1] = Channel;
    AppleDockChannelWriteLe16(Output + 2, (uint16_t)BodyLength);
    Output[4] = Sequence;
    Output[5] = Interface;
    AppleDockChannelWriteLe16(Output + 6, 0U);
    if (BodyLength != 0U && Body != Output + APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE)
        memcpy(Output + APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE, Body, BodyLength);

    status = AppleDockChannelComputeChecksum(
        Output, APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE + BodyLength, &checksum);
    if (status != AppleDockChannelSuccess)
        return status;
    AppleDockChannelWriteLe32(
        Output + APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE + BodyLength,
        checksum);
    *OutputLength = required;
    return AppleDockChannelSuccess;
}

APPLE_DOCKCHANNEL_STATUS
AppleDockChannelParsePacket(
    const uint8_t *Packet,
    size_t PacketLength,
    APPLE_DOCKCHANNEL_PACKET_VIEW *View
    )
{
    uint16_t bodyLength;
    size_t expectedLength;
    uint32_t expectedChecksum;
    uint32_t suppliedChecksum;
    APPLE_DOCKCHANNEL_STATUS status;

    if (Packet == NULL || View == NULL)
        return AppleDockChannelInvalidArgument;
    memset(View, 0, sizeof(*View));
    if (PacketLength < APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE +
                       APPLE_DOCKCHANNEL_PACKET_CHECKSUM_SIZE)
        return AppleDockChannelIncompletePacket;
    if (Packet[0] != APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE)
        return AppleDockChannelInvalidHeader;
    if (!AppleDockChannelIsKnownChannel(Packet[1]))
        return AppleDockChannelInvalidChannel;
    if (Packet[5] > APPLE_DOCKCHANNEL_PACKET_MAX_INTERFACE)
        return AppleDockChannelInvalidInterface;
    if (AppleDockChannelReadLe16(Packet + 6) != 0U)
        return AppleDockChannelInvalidHeader;

    bodyLength = AppleDockChannelReadLe16(Packet + 2);
    if (bodyLength > APPLE_DOCKCHANNEL_PACKET_MAX_BODY)
        return AppleDockChannelInvalidLength;
    if ((bodyLength & 3U) != 0U)
        return AppleDockChannelInvalidAlignment;
    expectedLength = APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE +
                     (size_t)bodyLength +
                     APPLE_DOCKCHANNEL_PACKET_CHECKSUM_SIZE;
    if (PacketLength < expectedLength)
        return AppleDockChannelIncompletePacket;
    if (PacketLength > expectedLength)
        return AppleDockChannelTrailingData;

    status = AppleDockChannelComputeChecksum(
        Packet, APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE + (size_t)bodyLength,
        &expectedChecksum);
    if (status != AppleDockChannelSuccess)
        return status;
    suppliedChecksum = AppleDockChannelReadLe32(
        Packet + APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE + (size_t)bodyLength);
    if (suppliedChecksum != expectedChecksum)
        return AppleDockChannelInvalidChecksum;

    View->HeaderLength = Packet[0];
    View->Channel = Packet[1];
    View->BodyLength = bodyLength;
    View->Sequence = Packet[4];
    View->Interface = Packet[5];
    View->Padding = 0U;
    View->Body = Packet + APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE;
    View->Checksum = suppliedChecksum;
    View->PacketLength = expectedLength;
    return AppleDockChannelSuccess;
}
