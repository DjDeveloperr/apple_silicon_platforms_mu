/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#include "AppleMtpStream.h"

#include <string.h>

static uint16_t
AppleMtpStreamReadLe16(
    const uint8_t *Data
    )
{
    return (uint16_t)((uint16_t)Data[0] | ((uint16_t)Data[1] << 8));
}

void
AppleMtpStreamReset(
    APPLE_MTP_STREAM *Stream
    )
{
    if (Stream != NULL)
        memset(Stream, 0, sizeof(*Stream));
}

APPLE_DOCKCHANNEL_STATUS
AppleMtpStreamPushByte(
    APPLE_MTP_STREAM *Stream,
    uint8_t Byte,
    APPLE_DOCKCHANNEL_PACKET_VIEW *Packet,
    int *PacketReady
    )
{
    size_t bodyLength;
    APPLE_DOCKCHANNEL_STATUS status;

    if (Stream == NULL || Packet == NULL || PacketReady == NULL)
        return AppleDockChannelInvalidArgument;
    *PacketReady = 0;
    if (Stream->ExpectedLength != 0U &&
        Stream->Length == Stream->ExpectedLength)
        return AppleDockChannelTrailingData;
    if (Stream->Length >= sizeof(Stream->Buffer)) {
        AppleMtpStreamReset(Stream);
        return AppleDockChannelInvalidLength;
    }

    Stream->Buffer[Stream->Length++] = Byte;
    if (Stream->Length == APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE) {
        if (Stream->Buffer[0] != APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE ||
            (Stream->Buffer[1] != APPLE_DOCKCHANNEL_CHANNEL_COMMAND &&
             Stream->Buffer[1] != APPLE_DOCKCHANNEL_CHANNEL_REPORT) ||
            Stream->Buffer[5] > APPLE_DOCKCHANNEL_PACKET_MAX_INTERFACE ||
            Stream->Buffer[6] != 0U || Stream->Buffer[7] != 0U) {
            AppleMtpStreamReset(Stream);
            return AppleDockChannelInvalidHeader;
        }
        bodyLength = AppleMtpStreamReadLe16(Stream->Buffer + 2);
        if ((bodyLength & 3U) != 0U) {
            AppleMtpStreamReset(Stream);
            return AppleDockChannelInvalidAlignment;
        }
        Stream->ExpectedLength = APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE +
                                 bodyLength +
                                 APPLE_DOCKCHANNEL_PACKET_CHECKSUM_SIZE;
        if (Stream->ExpectedLength > sizeof(Stream->Buffer)) {
            AppleMtpStreamReset(Stream);
            return AppleDockChannelInvalidLength;
        }
    }

    if (Stream->ExpectedLength == 0U ||
        Stream->Length != Stream->ExpectedLength)
        return AppleDockChannelSuccess;

    status = AppleDockChannelParsePacket(
        Stream->Buffer, Stream->Length, Packet);
    if (status != AppleDockChannelSuccess) {
        AppleMtpStreamReset(Stream);
        return status;
    }
    *PacketReady = 1;
    return AppleDockChannelSuccess;
}

void
AppleMtpStreamConsume(
    APPLE_MTP_STREAM *Stream
    )
{
    AppleMtpStreamReset(Stream);
}
