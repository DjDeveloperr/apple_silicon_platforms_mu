/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple MTP/DockChannel HID protocol core.
 *
 * Provenance: m1n1 proxyclient/m1n1/fw/mtp.py (MIT) and Asahi Linux
 * drivers/hid/dockchannel-hid/dockchannel-hid.c, commit
 * e8efe09d4f378992c890d181d65e2ed8d8cb1194 (GPL-2.0 OR MIT).
 *
 * This module intentionally implements no firmware loading, DMA allocation,
 * GPIO operation, or device policy. Unknown commands and events fail closed.
 */

#include "AppleMtpProtocol.h"

#include <limits.h>
#include <string.h>

static uint16_t
AppleMtpReadLe16(
    const uint8_t *Data
    )
{
    return (uint16_t)((uint16_t)Data[0] | ((uint16_t)Data[1] << 8));
}

static uint32_t
AppleMtpReadLe32(
    const uint8_t *Data
    )
{
    return (uint32_t)Data[0] |
           ((uint32_t)Data[1] << 8) |
           ((uint32_t)Data[2] << 16) |
           ((uint32_t)Data[3] << 24);
}

static void
AppleMtpWriteLe16(
    uint8_t *Data,
    uint16_t Value
    )
{
    Data[0] = (uint8_t)Value;
    Data[1] = (uint8_t)(Value >> 8);
}

static void
AppleMtpWriteLe32(
    uint8_t *Data,
    uint32_t Value
    )
{
    Data[0] = (uint8_t)Value;
    Data[1] = (uint8_t)(Value >> 8);
    Data[2] = (uint8_t)(Value >> 16);
    Data[3] = (uint8_t)(Value >> 24);
}

static void
AppleMtpWriteLe64(
    uint8_t *Data,
    uint64_t Value
    )
{
    AppleMtpWriteLe32(Data, (uint32_t)Value);
    AppleMtpWriteLe32(Data + 4U, (uint32_t)(Value >> 32));
}

static int
AppleMtpIsValidReportType(
    APPLE_MTP_REPORT_TYPE ReportType
    )
{
    return ReportType == AppleMtpInputReport ||
           ReportType == AppleMtpOutputReport ||
           ReportType == AppleMtpFeatureReport;
}

static int
AppleMtpIsValidRequestType(
    APPLE_MTP_REQUEST_TYPE RequestType
    )
{
    return RequestType == AppleMtpSetReport ||
           RequestType == AppleMtpGetReport;
}

static APPLE_MTP_STATUS
AppleMtpMapDockStatus(
    APPLE_DOCKCHANNEL_STATUS Status
    )
{
    switch (Status) {
    case AppleDockChannelSuccess:
        return AppleMtpSuccess;
    case AppleDockChannelBufferTooSmall:
        return AppleMtpBufferTooSmall;
    case AppleDockChannelInvalidChannel:
        return AppleMtpInvalidChannel;
    case AppleDockChannelInvalidInterface:
        return AppleMtpInvalidInterface;
    case AppleDockChannelInvalidArgument:
        return AppleMtpInvalidArgument;
    default:
        return AppleMtpInvalidLength;
    }
}

static APPLE_MTP_STATUS
AppleMtpPaddedStringLength(
    const uint8_t *Data,
    size_t Length,
    size_t *StringLength
    )
{
    size_t index;
    size_t end;

    if (Data == NULL || StringLength == NULL || Length == 0U)
        return AppleMtpInvalidArgument;
    for (end = 0U; end < Length && Data[end] != 0U; end++) {
        /* Find the required terminator without reading beyond Length. */
    }
    if (end == Length)
        return AppleMtpMalformedMessage;
    for (index = end + 1U; index < Length; index++) {
        if (Data[index] != 0U)
            return AppleMtpInvalidPadding;
    }
    *StringLength = end;
    return AppleMtpSuccess;
}

static APPLE_MTP_STATUS
AppleMtpValidateInitBlocks(
    const APPLE_MTP_INIT_EVENT_VIEW *Event
    )
{
    APPLE_MTP_INIT_BLOCK_ITERATOR iterator;
    APPLE_MTP_INIT_BLOCK_VIEW block;
    APPLE_MTP_STATUS status;

    status = AppleMtpInitBlockIteratorInitialize(Event, &iterator);
    if (status != AppleMtpSuccess)
        return status;
    for (;;) {
        status = AppleMtpInitBlockIteratorNext(&iterator, &block);
        if (status == AppleMtpEndOfBlocks)
            return AppleMtpSuccess;
        if (status != AppleMtpSuccess)
            return status;
    }
}

APPLE_MTP_STATUS
AppleMtpParseMessage(
    const APPLE_DOCKCHANNEL_PACKET_VIEW *Packet,
    APPLE_MTP_MESSAGE_VIEW *View
    )
{
    uint8_t flags;
    unsigned int group;
    unsigned int request;
    uint16_t payloadLength;
    size_t paddedLength;
    size_t requiredLength;
    size_t index;

    if (Packet == NULL || View == NULL || Packet->Body == NULL)
        return AppleMtpInvalidArgument;
    memset(View, 0, sizeof(*View));
    if (Packet->BodyLength < APPLE_MTP_SUBHEADER_SIZE)
        return AppleMtpInvalidLength;

    flags = Packet->Body[0];
    group = (unsigned int)(flags >> 6);
    request = (unsigned int)(flags & 0x3fU);
    if (group > (unsigned int)AppleMtpFeatureReport ||
        request > (unsigned int)AppleMtpGetReport)
        return AppleMtpInvalidFlags;
    if (Packet->Body[1] != 0U)
        return AppleMtpInvalidPadding;

    payloadLength = AppleMtpReadLe16(Packet->Body + 2);
    paddedLength = ((size_t)payloadLength + 3U) & ~(size_t)3U;
    requiredLength = APPLE_MTP_SUBHEADER_SIZE + paddedLength;
    if (requiredLength != Packet->BodyLength)
        return AppleMtpInvalidLength;
    /*
     * The bytes between the declared payload and the 4-byte alignment are not
     * required to be zero.  Captured on J414s hardware 2026-07-29: the MTP
     * firmware acknowledges ENABLE_INTERFACE for the keyboard with
     *
     *     subheader 80 00 01 00 00 00 00 00   payload b4 02 00 00
     *
     * declaring a one-byte payload while still writing the interface number
     * into the following byte -- the driver's own request for the same command
     * declares two.  Rejecting that as malformed dropped every bootstrap ACK,
     * so the endpoint never left WaitEnable, VhfCreate was never reached, and
     * no HID child was ever created even though the device had announced the
     * keyboard with a valid 189-byte report descriptor.
     *
     * Length and alignment are still enforced above, and every consumer reads
     * only PayloadLength bytes, so trailing alignment bytes cannot be mistaken
     * for payload.  (VALIDATED: hardware capture, run-mtp-trace log.)
     */
    (void)index;

    if (Packet->Channel == APPLE_DOCKCHANNEL_CHANNEL_REPORT) {
        if (group != (unsigned int)AppleMtpInputReport ||
            request != (unsigned int)AppleMtpSetReport)
            return AppleMtpInvalidFlags;
        if (AppleMtpReadLe32(Packet->Body + 4) != 0U)
            return AppleMtpInvalidResponse;
    } else if (Packet->Channel != APPLE_DOCKCHANNEL_CHANNEL_COMMAND) {
        return AppleMtpInvalidChannel;
    }

    View->Packet = Packet;
    View->Flags = flags;
    View->Unknown = 0U;
    View->ReportType = (APPLE_MTP_REPORT_TYPE)group;
    View->RequestType = (APPLE_MTP_REQUEST_TYPE)request;
    View->PayloadLength = payloadLength;
    View->ReturnCode = AppleMtpReadLe32(Packet->Body + 4);
    View->Payload = Packet->Body + APPLE_MTP_SUBHEADER_SIZE;
    View->PaddedPayloadLength = paddedLength;
    return AppleMtpSuccess;
}

APPLE_MTP_STATUS
AppleMtpBuildMessagePacket(
    uint8_t Channel,
    uint8_t Sequence,
    uint8_t Interface,
    APPLE_MTP_REPORT_TYPE ReportType,
    APPLE_MTP_REQUEST_TYPE RequestType,
    uint32_t ReturnCode,
    const uint8_t *Payload,
    size_t PayloadLength,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    )
{
    size_t paddedLength;
    size_t bodyLength;
    size_t requiredLength;
    uint8_t *body;
    APPLE_DOCKCHANNEL_STATUS dockStatus;

    if (Output == NULL || OutputLength == NULL ||
        (Payload == NULL && PayloadLength != 0U))
        return AppleMtpInvalidArgument;
    *OutputLength = 0U;
    if (!AppleMtpIsValidReportType(ReportType) ||
        !AppleMtpIsValidRequestType(RequestType))
        return AppleMtpInvalidFlags;
    if (Interface >= APPLE_MTP_MAX_INTERFACES)
        return AppleMtpInvalidInterface;
    if (PayloadLength > UINT16_MAX)
        return AppleMtpInvalidLength;

    paddedLength = (PayloadLength + 3U) & ~(size_t)3U;
    bodyLength = APPLE_MTP_SUBHEADER_SIZE + paddedLength;
    if (bodyLength > APPLE_DOCKCHANNEL_PACKET_MAX_BODY)
        return AppleMtpInvalidLength;
    requiredLength = APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE + bodyLength +
                     APPLE_DOCKCHANNEL_PACKET_CHECKSUM_SIZE;
    if (OutputCapacity < requiredLength)
        return AppleMtpBufferTooSmall;

    if (Channel == APPLE_DOCKCHANNEL_CHANNEL_REPORT) {
        if (ReportType != AppleMtpInputReport ||
            RequestType != AppleMtpSetReport || ReturnCode != 0U)
            return AppleMtpInvalidFlags;
    } else if (Channel == APPLE_DOCKCHANNEL_CHANNEL_COMMAND) {
        if (ReportType == AppleMtpInputReport &&
            RequestType == AppleMtpSetReport)
            return AppleMtpUnsupportedMessage;
    } else {
        return AppleMtpInvalidChannel;
    }

    body = Output + APPLE_DOCKCHANNEL_PACKET_HEADER_SIZE;
    body[0] = (uint8_t)(((unsigned int)ReportType << 6) |
                        (unsigned int)RequestType);
    body[1] = 0U;
    AppleMtpWriteLe16(body + 2, (uint16_t)PayloadLength);
    AppleMtpWriteLe32(body + 4, ReturnCode);
    if (PayloadLength != 0U)
        memcpy(body + APPLE_MTP_SUBHEADER_SIZE, Payload, PayloadLength);
    if (paddedLength > PayloadLength) {
        memset(body + APPLE_MTP_SUBHEADER_SIZE + PayloadLength, 0,
               paddedLength - PayloadLength);
    }

    dockStatus = AppleDockChannelBuildPacket(
        Channel, Sequence, Interface, body, bodyLength, Output,
        OutputCapacity, OutputLength);
    return AppleMtpMapDockStatus(dockStatus);
}

APPLE_MTP_STATUS
AppleMtpBuildGetReportPacket(
    uint8_t Sequence,
    uint8_t Interface,
    APPLE_MTP_REPORT_TYPE ReportType,
    uint8_t ReportId,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    )
{
    return AppleMtpBuildMessagePacket(
        APPLE_DOCKCHANNEL_CHANNEL_COMMAND, Sequence, Interface, ReportType,
        AppleMtpGetReport, 0U, &ReportId, 1U, Output, OutputCapacity,
        OutputLength);
}

APPLE_MTP_STATUS
AppleMtpBuildSetReportPacket(
    uint8_t Sequence,
    uint8_t Interface,
    APPLE_MTP_REPORT_TYPE ReportType,
    const uint8_t *Report,
    size_t ReportLength,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    )
{
    if (Report == NULL || ReportLength == 0U)
        return AppleMtpInvalidArgument;
    if (ReportType != AppleMtpOutputReport &&
        ReportType != AppleMtpFeatureReport)
        return AppleMtpUnsupportedMessage;
    return AppleMtpBuildMessagePacket(
        APPLE_DOCKCHANNEL_CHANNEL_COMMAND, Sequence, Interface, ReportType,
        AppleMtpSetReport, 0U, Report, ReportLength, Output, OutputCapacity,
        OutputLength);
}

APPLE_MTP_STATUS
AppleMtpBuildEnableInterfacePacket(
    uint8_t Sequence,
    uint8_t TargetInterface,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    )
{
    uint8_t payload[2];

    if (TargetInterface == APPLE_MTP_INTERFACE_COMM ||
        TargetInterface >= APPLE_MTP_MAX_INTERFACES)
        return AppleMtpInvalidInterface;
    payload[0] = APPLE_MTP_COMMAND_ENABLE_INTERFACE;
    payload[1] = TargetInterface;
    return AppleMtpBuildMessagePacket(
        APPLE_DOCKCHANNEL_CHANNEL_COMMAND, Sequence,
        APPLE_MTP_INTERFACE_COMM, AppleMtpFeatureReport,
        AppleMtpSetReport, 0U, payload, sizeof(payload), Output,
        OutputCapacity, OutputLength);
}

APPLE_MTP_STATUS
AppleMtpBuildResetInterfacePacket(
    uint8_t Sequence,
    uint8_t TargetInterface,
    uint8_t State,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    )
{
    uint8_t payload[4];

    if (TargetInterface == APPLE_MTP_INTERFACE_COMM ||
        TargetInterface >= APPLE_MTP_MAX_INTERFACES)
        return AppleMtpInvalidInterface;
    if (State != 0U && State != 2U)
        return AppleMtpUnsupportedMessage;
    payload[0] = APPLE_MTP_COMMAND_RESET_INTERFACE;
    payload[1] = 1U;
    payload[2] = TargetInterface;
    payload[3] = State;
    return AppleMtpBuildMessagePacket(
        APPLE_DOCKCHANNEL_CHANNEL_COMMAND, Sequence,
        APPLE_MTP_INTERFACE_COMM, AppleMtpFeatureReport,
        AppleMtpSetReport, 0U, payload, sizeof(payload), Output,
        OutputCapacity, OutputLength);
}

APPLE_MTP_STATUS
AppleMtpBuildSendFirmwarePacket(
    uint8_t Sequence,
    uint8_t TargetInterface,
    uint64_t DeviceAddress,
    uint32_t FirmwareSize,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    )
{
    uint8_t payload[16];

    if (TargetInterface == APPLE_MTP_INTERFACE_COMM ||
        TargetInterface >= APPLE_MTP_MAX_INTERFACES)
        return AppleMtpInvalidInterface;
    if (DeviceAddress == 0U || FirmwareSize == 0U)
        return AppleMtpInvalidArgument;

    payload[0] = APPLE_MTP_COMMAND_SEND_FIRMWARE;
    payload[1] = 2U;
    payload[2] = 0U;
    payload[3] = TargetInterface;
    AppleMtpWriteLe64(payload + 4U, DeviceAddress);
    AppleMtpWriteLe32(payload + 12U, FirmwareSize);
    return AppleMtpBuildMessagePacket(
        APPLE_DOCKCHANNEL_CHANNEL_COMMAND, Sequence,
        APPLE_MTP_INTERFACE_COMM, AppleMtpFeatureReport,
        AppleMtpSetReport, 0U, payload, sizeof(payload), Output,
        OutputCapacity, OutputLength);
}

APPLE_MTP_STATUS
AppleMtpBuildGpioAckPacket(
    uint8_t Sequence,
    const APPLE_MTP_GPIO_EVENT_VIEW *Event,
    uint32_t ReturnCode,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    )
{
    uint8_t payload[10];

    if (Event == NULL || Event->WireData == NULL || Event->WireLength != 5U)
        return AppleMtpInvalidArgument;
    if (Event->WireData[0] != APPLE_MTP_EVENT_GPIO_COMMAND ||
        Event->WireData[1] != Event->Interface ||
        Event->WireData[2] != Event->Gpio || Event->WireData[3] != 0U ||
        Event->WireData[4] != Event->Command || Event->Command != 3U)
        return AppleMtpMalformedMessage;

    payload[0] = APPLE_MTP_COMMAND_ACK_GPIO;
    AppleMtpWriteLe32(payload + 1, ReturnCode);
    memcpy(payload + 5, Event->WireData, Event->WireLength);
    return AppleMtpBuildMessagePacket(
        APPLE_DOCKCHANNEL_CHANNEL_COMMAND, Sequence,
        APPLE_MTP_INTERFACE_COMM, AppleMtpFeatureReport,
        AppleMtpSetReport, 0U, payload, sizeof(payload), Output,
        OutputCapacity, OutputLength);
}

APPLE_MTP_STATUS
AppleMtpParseControlCommand(
    const APPLE_MTP_MESSAGE_VIEW *Message,
    APPLE_MTP_CONTROL_COMMAND_VIEW *View
    )
{
    const uint8_t *payload;

    if (Message == NULL || View == NULL || Message->Packet == NULL ||
        Message->Payload == NULL)
        return AppleMtpInvalidArgument;
    memset(View, 0, sizeof(*View));
    if (Message->Packet->Channel != APPLE_DOCKCHANNEL_CHANNEL_COMMAND)
        return AppleMtpInvalidChannel;
    if (Message->Packet->Interface != APPLE_MTP_INTERFACE_COMM)
        return AppleMtpInvalidInterface;
    if (Message->ReportType != AppleMtpFeatureReport ||
        Message->RequestType != AppleMtpSetReport ||
        Message->ReturnCode != 0U)
        return AppleMtpInvalidFlags;
    if (Message->PayloadLength == 0U)
        return AppleMtpInvalidLength;

    payload = Message->Payload;
    switch (payload[0]) {
    case APPLE_MTP_COMMAND_ENABLE_INTERFACE:
        if (Message->PayloadLength != 2U)
            return AppleMtpInvalidLength;
        if (payload[1] == APPLE_MTP_INTERFACE_COMM ||
            payload[1] >= APPLE_MTP_MAX_INTERFACES)
            return AppleMtpInvalidInterface;
        View->Kind = AppleMtpControlCommandEnable;
        View->TargetInterface = payload[1];
        return AppleMtpSuccess;

    case APPLE_MTP_COMMAND_RESET_INTERFACE:
        if (Message->PayloadLength != 4U || payload[1] != 1U)
            return AppleMtpMalformedMessage;
        if (payload[2] == APPLE_MTP_INTERFACE_COMM ||
            payload[2] >= APPLE_MTP_MAX_INTERFACES)
            return AppleMtpInvalidInterface;
        if (payload[3] != 0U && payload[3] != 2U)
            return AppleMtpUnsupportedMessage;
        View->Kind = AppleMtpControlCommandReset;
        View->TargetInterface = payload[2];
        View->ResetState = payload[3];
        return AppleMtpSuccess;

    case APPLE_MTP_COMMAND_ACK_GPIO:
        if (Message->PayloadLength != 10U ||
            payload[5] != APPLE_MTP_EVENT_GPIO_COMMAND ||
            payload[6] >= APPLE_MTP_MAX_INTERFACES ||
            payload[8] != 0U || payload[9] != 3U)
            return AppleMtpMalformedMessage;
        View->Kind = AppleMtpControlCommandGpioAck;
        View->TargetInterface = payload[6];
        View->GpioReturnCode = AppleMtpReadLe32(payload + 1);
        View->EmbeddedGpioCommand = payload + 5;
        View->EmbeddedGpioCommandLength = 5U;
        return AppleMtpSuccess;

    case APPLE_MTP_COMMAND_SEND_FIRMWARE:
        if (Message->PayloadLength != 16U || payload[1] != 2U ||
            payload[2] != 0U)
            return AppleMtpMalformedMessage;
        if (payload[3] == APPLE_MTP_INTERFACE_COMM ||
            payload[3] >= APPLE_MTP_MAX_INTERFACES)
            return AppleMtpInvalidInterface;
        View->FirmwareAddress =
            (uint64_t)AppleMtpReadLe32(payload + 4U) |
            ((uint64_t)AppleMtpReadLe32(payload + 8U) << 32);
        View->FirmwareSize = AppleMtpReadLe32(payload + 12U);
        if (View->FirmwareAddress == 0U || View->FirmwareSize == 0U)
            return AppleMtpMalformedMessage;
        View->Kind = AppleMtpControlCommandFirmware;
        View->TargetInterface = payload[3];
        return AppleMtpSuccess;

    default:
        return AppleMtpUnsupportedMessage;
    }
}

APPLE_MTP_STATUS
AppleMtpClassifyInterfaceName(
    const uint8_t *Name,
    size_t NameLength,
    APPLE_MTP_INTERFACE_KIND *Kind
    )
{
    struct APPLE_MTP_KNOWN_NAME {
        const char *Name;
        size_t Length;
        APPLE_MTP_INTERFACE_KIND Kind;
    };
    static const struct APPLE_MTP_KNOWN_NAME names[] = {
        { "comm", 4U, AppleMtpInterfaceComm },
        { "multi-touch", 11U, AppleMtpInterfaceMultiTouch },
        { "keyboard", 8U, AppleMtpInterfaceKeyboard },
        { "stm", 3U, AppleMtpInterfaceStm },
        { "actuator", 8U, AppleMtpInterfaceActuator },
        { "tp_accel", 8U, AppleMtpInterfaceTrackpadAccelerator }
    };
    size_t index;

    if (Name == NULL || Kind == NULL)
        return AppleMtpInvalidArgument;
    *Kind = AppleMtpInterfaceUnknown;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); index++) {
        if (NameLength == names[index].Length &&
            memcmp(Name, names[index].Name, NameLength) == 0) {
            *Kind = names[index].Kind;
            return AppleMtpSuccess;
        }
    }
    return AppleMtpUnsupportedMessage;
}

APPLE_MTP_STATUS
AppleMtpParseControlEvent(
    const APPLE_MTP_MESSAGE_VIEW *Message,
    APPLE_MTP_CONTROL_EVENT_VIEW *View
    )
{
    const uint8_t *payload;
    size_t nameLength;
    APPLE_MTP_STATUS status;

    if (Message == NULL || View == NULL || Message->Packet == NULL ||
        Message->Payload == NULL)
        return AppleMtpInvalidArgument;
    memset(View, 0, sizeof(*View));
    if (Message->Packet->Channel != APPLE_DOCKCHANNEL_CHANNEL_REPORT)
        return AppleMtpInvalidChannel;
    if (Message->Packet->Interface != APPLE_MTP_INTERFACE_COMM)
        return AppleMtpInvalidInterface;
    if (Message->ReportType != AppleMtpInputReport ||
        Message->RequestType != AppleMtpSetReport ||
        Message->ReturnCode != 0U)
        return AppleMtpInvalidFlags;
    if (Message->PayloadLength == 0U)
        return AppleMtpInvalidLength;

    payload = Message->Payload;
    switch (payload[0]) {
    case APPLE_MTP_EVENT_INIT:
        if (Message->PayloadLength < APPLE_MTP_INIT_HEADER_SIZE)
            return AppleMtpInvalidLength;
        if (payload[1] != 1U || payload[2] != 0U ||
            payload[3] >= APPLE_MTP_MAX_INTERFACES ||
            payload[20] > 1U || payload[21] != 0U)
            return AppleMtpMalformedMessage;
        status = AppleMtpPaddedStringLength(
            payload + 4, APPLE_MTP_INTERFACE_NAME_SIZE, &nameLength);
        if (status != AppleMtpSuccess)
            return status;
        View->Kind = AppleMtpControlEventInit;
        View->Init.Interface = payload[3];
        View->Init.InterfaceName = payload + 4;
        View->Init.InterfaceNameLength = nameLength;
        View->Init.MorePackets = payload[20] != 0U;
        View->Init.Blocks = payload + APPLE_MTP_INIT_HEADER_SIZE;
        View->Init.BlocksLength =
            (size_t)Message->PayloadLength - APPLE_MTP_INIT_HEADER_SIZE;
        status = AppleMtpClassifyInterfaceName(
            View->Init.InterfaceName, View->Init.InterfaceNameLength,
            &View->Init.InterfaceKind);
        if (status != AppleMtpSuccess)
            return status;
        return AppleMtpValidateInitBlocks(&View->Init);

    case APPLE_MTP_EVENT_READY:
        if (Message->PayloadLength != 4U)
            return AppleMtpInvalidLength;
        if (payload[1] >= APPLE_MTP_MAX_INTERFACES)
            return AppleMtpInvalidInterface;
        View->Kind = AppleMtpControlEventReady;
        View->Ready.Interface = payload[1];
        View->Ready.Unknown = AppleMtpReadLe16(payload + 2);
        return AppleMtpSuccess;

    case APPLE_MTP_EVENT_GPIO_COMMAND:
        if (Message->PayloadLength != 5U)
            return AppleMtpInvalidLength;
        if (payload[1] >= APPLE_MTP_MAX_INTERFACES)
            return AppleMtpInvalidInterface;
        if (payload[3] != 0U)
            return AppleMtpMalformedMessage;
        if (payload[4] != 3U)
            return AppleMtpUnsupportedMessage;
        View->Kind = AppleMtpControlEventGpio;
        View->Gpio.Interface = payload[1];
        View->Gpio.Gpio = payload[2];
        View->Gpio.Command = payload[4];
        View->Gpio.WireData = payload;
        View->Gpio.WireLength = Message->PayloadLength;
        return AppleMtpSuccess;

    default:
        return AppleMtpUnsupportedMessage;
    }
}

APPLE_MTP_STATUS
AppleMtpInitBlockIteratorInitialize(
    const APPLE_MTP_INIT_EVENT_VIEW *Event,
    APPLE_MTP_INIT_BLOCK_ITERATOR *Iterator
    )
{
    if (Event == NULL || Iterator == NULL || Event->Blocks == NULL ||
        Event->BlocksLength == 0U)
        return AppleMtpInvalidArgument;
    Iterator->Cursor = Event->Blocks;
    Iterator->Remaining = Event->BlocksLength;
    Iterator->Finished = 0;
    return AppleMtpSuccess;
}

APPLE_MTP_STATUS
AppleMtpInitBlockIteratorNext(
    APPLE_MTP_INIT_BLOCK_ITERATOR *Iterator,
    APPLE_MTP_INIT_BLOCK_VIEW *View
    )
{
    uint16_t type;
    uint16_t length;
    size_t stringLength;
    APPLE_MTP_STATUS status;

    if (Iterator == NULL || View == NULL || Iterator->Cursor == NULL)
        return AppleMtpInvalidArgument;
    memset(View, 0, sizeof(*View));
    if (Iterator->Finished)
        return AppleMtpEndOfBlocks;
    if (Iterator->Remaining < APPLE_MTP_INIT_BLOCK_HEADER_SIZE)
        return AppleMtpMalformedMessage;

    type = AppleMtpReadLe16(Iterator->Cursor);
    length = AppleMtpReadLe16(Iterator->Cursor + 2);
    if ((size_t)length > Iterator->Remaining -
                         APPLE_MTP_INIT_BLOCK_HEADER_SIZE)
        return AppleMtpInvalidLength;

    View->Type = type;
    View->Data = Iterator->Cursor + APPLE_MTP_INIT_BLOCK_HEADER_SIZE;
    View->Length = length;
    Iterator->Cursor += APPLE_MTP_INIT_BLOCK_HEADER_SIZE + (size_t)length;
    Iterator->Remaining -= APPLE_MTP_INIT_BLOCK_HEADER_SIZE + (size_t)length;

    switch (type) {
    case APPLE_MTP_INIT_HID_DESCRIPTOR:
        if (length == 0U || length > APPLE_MTP_MAX_HID_DESCRIPTOR_SIZE)
            return AppleMtpInvalidLength;
        return AppleMtpSuccess;

    case APPLE_MTP_INIT_GPIO_REQUEST:
        if (length != APPLE_MTP_GPIO_REQUEST_SIZE)
            return AppleMtpInvalidLength;
        View->GpioUnknown = AppleMtpReadLe16(View->Data);
        View->GpioId = AppleMtpReadLe16(View->Data + 2);
        status = AppleMtpPaddedStringLength(
            View->Data + 4, APPLE_MTP_GPIO_NAME_SIZE, &stringLength);
        if (status != AppleMtpSuccess)
            return status;
        View->String = View->Data + 4;
        View->StringLength = stringLength;
        return AppleMtpSuccess;

    case APPLE_MTP_INIT_TERMINATOR:
        if (length > APPLE_MTP_MAX_TERMINATOR_DATA_SIZE ||
            Iterator->Remaining != 0U)
            return AppleMtpMalformedMessage;
        Iterator->Finished = 1;
        return AppleMtpSuccess;

    case APPLE_MTP_INIT_PRODUCT_NAME:
        if (length == 0U || length > APPLE_MTP_MAX_PRODUCT_NAME_SIZE)
            return AppleMtpInvalidLength;
        status = AppleMtpPaddedStringLength(
            View->Data, View->Length, &stringLength);
        if (status != AppleMtpSuccess)
            return status;
        View->String = View->Data;
        View->StringLength = stringLength;
        return AppleMtpSuccess;

    default:
        return AppleMtpUnsupportedMessage;
    }
}

APPLE_MTP_STATUS
AppleMtpValidateAck(
    const APPLE_MTP_MESSAGE_VIEW *Message,
    uint8_t ExpectedSequence,
    uint8_t ExpectedInterface,
    APPLE_MTP_REPORT_TYPE ExpectedReportType,
    APPLE_MTP_REQUEST_TYPE ExpectedRequestType,
    uint8_t ExpectedReportId,
    APPLE_MTP_ACK_VIEW *View
    )
{
    if (Message == NULL || View == NULL || Message->Packet == NULL ||
        Message->Payload == NULL)
        return AppleMtpInvalidArgument;
    memset(View, 0, sizeof(*View));
    if (Message->Packet->Channel != APPLE_DOCKCHANNEL_CHANNEL_COMMAND)
        return AppleMtpInvalidChannel;
    if (Message->Packet->Sequence != ExpectedSequence)
        return AppleMtpInvalidSequence;
    if (Message->Packet->Interface != ExpectedInterface)
        return AppleMtpInvalidInterface;
    if (Message->ReportType != ExpectedReportType ||
        Message->RequestType != ExpectedRequestType)
        return AppleMtpInvalidFlags;
    if (Message->PayloadLength < 1U ||
        Message->Payload[0] != ExpectedReportId)
        return AppleMtpInvalidResponse;

    View->ReportId = Message->Payload[0];
    View->ReturnCode = Message->ReturnCode;
    View->Response = Message->Payload + 1;
    View->ResponseLength = (size_t)Message->PayloadLength - 1U;
    return AppleMtpSuccess;
}
