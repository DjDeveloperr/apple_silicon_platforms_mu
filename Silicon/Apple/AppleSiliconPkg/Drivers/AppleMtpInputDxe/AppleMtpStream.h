/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Bounded stream reassembly for DockChannel packets. */

#pragma once

#include "AppleDockChannel.h"

#include <stddef.h>
#include <stdint.h>

#define APPLE_MTP_STREAM_MAX_PACKET_SIZE 8192U

typedef struct _APPLE_MTP_STREAM {
    uint8_t Buffer[APPLE_MTP_STREAM_MAX_PACKET_SIZE];
    size_t Length;
    size_t ExpectedLength;
} APPLE_MTP_STREAM;

void
AppleMtpStreamReset(
    APPLE_MTP_STREAM *Stream
    );

/*
 * Adds one byte. PacketReady is set only after a complete, checksum-valid
 * packet has been assembled. The returned view remains valid until Consume
 * or Reset. Callers must consume a ready packet before pushing another byte.
 */
APPLE_DOCKCHANNEL_STATUS
AppleMtpStreamPushByte(
    APPLE_MTP_STREAM *Stream,
    uint8_t Byte,
    APPLE_DOCKCHANNEL_PACKET_VIEW *Packet,
    int *PacketReady
    );

void
AppleMtpStreamConsume(
    APPLE_MTP_STREAM *Stream
    );
