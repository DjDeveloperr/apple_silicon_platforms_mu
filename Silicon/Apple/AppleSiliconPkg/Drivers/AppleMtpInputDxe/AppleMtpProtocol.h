/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Portable parser/builder API for Apple's MTP HID transport.
 *
 * Wire semantics are based on m1n1 proxyclient/m1n1/fw/mtp.py (MIT) and
 * Asahi Linux drivers/hid/dockchannel-hid/dockchannel-hid.c at commit
 * e8efe09d4f378992c890d181d65e2ed8d8cb1194 (GPL-2.0 OR MIT).
 */

#pragma once

#include "AppleDockChannel.h"

#include <stddef.h>
#include <stdint.h>

#define APPLE_MTP_SUBHEADER_SIZE               8U
#define APPLE_MTP_MAX_INTERFACES               16U
#define APPLE_MTP_INTERFACE_COMM               0U
#define APPLE_MTP_INIT_HEADER_SIZE             22U
#define APPLE_MTP_INIT_BLOCK_HEADER_SIZE       4U
#define APPLE_MTP_INTERFACE_NAME_SIZE          16U
#define APPLE_MTP_GPIO_NAME_SIZE               32U
#define APPLE_MTP_GPIO_REQUEST_SIZE            36U
#define APPLE_MTP_MAX_HID_DESCRIPTOR_SIZE      4096U
#define APPLE_MTP_MAX_PRODUCT_NAME_SIZE        256U
#define APPLE_MTP_MAX_TERMINATOR_DATA_SIZE     64U

#define APPLE_MTP_EVENT_GPIO_COMMAND           0xa0U
#define APPLE_MTP_EVENT_INIT                   0xf0U
#define APPLE_MTP_EVENT_READY                  0xf1U

#define APPLE_MTP_INIT_HID_DESCRIPTOR          0U
#define APPLE_MTP_INIT_GPIO_REQUEST            1U
#define APPLE_MTP_INIT_TERMINATOR              2U
#define APPLE_MTP_INIT_PRODUCT_NAME            7U

#define APPLE_MTP_COMMAND_RESET_INTERFACE      0x40U
#define APPLE_MTP_COMMAND_SEND_FIRMWARE        0x95U
#define APPLE_MTP_COMMAND_ENABLE_INTERFACE     0xb4U
#define APPLE_MTP_COMMAND_ACK_GPIO             0xa1U

typedef enum _APPLE_MTP_STATUS {
    AppleMtpSuccess = 0,
    AppleMtpInvalidArgument,
    AppleMtpBufferTooSmall,
    AppleMtpInvalidLength,
    AppleMtpInvalidPadding,
    AppleMtpInvalidFlags,
    AppleMtpInvalidChannel,
    AppleMtpInvalidInterface,
    AppleMtpInvalidSequence,
    AppleMtpInvalidResponse,
    AppleMtpMalformedMessage,
    AppleMtpUnsupportedMessage,
    AppleMtpEndOfBlocks
} APPLE_MTP_STATUS;

typedef enum _APPLE_MTP_REPORT_TYPE {
    AppleMtpInputReport = 0,
    AppleMtpOutputReport = 1,
    AppleMtpFeatureReport = 2
} APPLE_MTP_REPORT_TYPE;

typedef enum _APPLE_MTP_REQUEST_TYPE {
    AppleMtpSetReport = 0,
    AppleMtpGetReport = 1
} APPLE_MTP_REQUEST_TYPE;

typedef enum _APPLE_MTP_INTERFACE_KIND {
    AppleMtpInterfaceUnknown = 0,
    AppleMtpInterfaceComm,
    AppleMtpInterfaceMultiTouch,
    AppleMtpInterfaceKeyboard,
    AppleMtpInterfaceStm,
    AppleMtpInterfaceActuator,
    AppleMtpInterfaceTrackpadAccelerator
} APPLE_MTP_INTERFACE_KIND;

typedef struct _APPLE_MTP_MESSAGE_VIEW {
    const APPLE_DOCKCHANNEL_PACKET_VIEW *Packet;
    uint8_t Flags;
    uint8_t Unknown;
    APPLE_MTP_REPORT_TYPE ReportType;
    APPLE_MTP_REQUEST_TYPE RequestType;
    uint16_t PayloadLength;
    uint32_t ReturnCode;
    const uint8_t *Payload;
    size_t PaddedPayloadLength;
} APPLE_MTP_MESSAGE_VIEW;

typedef enum _APPLE_MTP_CONTROL_COMMAND_KIND {
    AppleMtpControlCommandEnable = 1,
    AppleMtpControlCommandReset,
    AppleMtpControlCommandFirmware,
    AppleMtpControlCommandGpioAck
} APPLE_MTP_CONTROL_COMMAND_KIND;

typedef struct _APPLE_MTP_CONTROL_COMMAND_VIEW {
    APPLE_MTP_CONTROL_COMMAND_KIND Kind;
    uint8_t TargetInterface;
    uint8_t ResetState;
    uint64_t FirmwareAddress;
    uint32_t FirmwareSize;
    uint32_t GpioReturnCode;
    const uint8_t *EmbeddedGpioCommand;
    size_t EmbeddedGpioCommandLength;
} APPLE_MTP_CONTROL_COMMAND_VIEW;

typedef enum _APPLE_MTP_CONTROL_EVENT_KIND {
    AppleMtpControlEventInit = 1,
    AppleMtpControlEventReady,
    AppleMtpControlEventGpio
} APPLE_MTP_CONTROL_EVENT_KIND;

typedef struct _APPLE_MTP_INIT_EVENT_VIEW {
    uint8_t Interface;
    APPLE_MTP_INTERFACE_KIND InterfaceKind;
    const uint8_t *InterfaceName;
    size_t InterfaceNameLength;
    int MorePackets;
    const uint8_t *Blocks;
    size_t BlocksLength;
} APPLE_MTP_INIT_EVENT_VIEW;

typedef struct _APPLE_MTP_READY_EVENT_VIEW {
    uint8_t Interface;
    uint16_t Unknown;
} APPLE_MTP_READY_EVENT_VIEW;

typedef struct _APPLE_MTP_GPIO_EVENT_VIEW {
    uint8_t Interface;
    uint8_t Gpio;
    uint8_t Command;
    const uint8_t *WireData;
    size_t WireLength;
} APPLE_MTP_GPIO_EVENT_VIEW;

typedef struct _APPLE_MTP_CONTROL_EVENT_VIEW {
    APPLE_MTP_CONTROL_EVENT_KIND Kind;
    APPLE_MTP_INIT_EVENT_VIEW Init;
    APPLE_MTP_READY_EVENT_VIEW Ready;
    APPLE_MTP_GPIO_EVENT_VIEW Gpio;
} APPLE_MTP_CONTROL_EVENT_VIEW;

typedef struct _APPLE_MTP_INIT_BLOCK_ITERATOR {
    const uint8_t *Cursor;
    size_t Remaining;
    int Finished;
} APPLE_MTP_INIT_BLOCK_ITERATOR;

typedef struct _APPLE_MTP_INIT_BLOCK_VIEW {
    uint16_t Type;
    const uint8_t *Data;
    size_t Length;
    uint16_t GpioUnknown;
    uint16_t GpioId;
    const uint8_t *String;
    size_t StringLength;
} APPLE_MTP_INIT_BLOCK_VIEW;

typedef struct _APPLE_MTP_ACK_VIEW {
    uint8_t ReportId;
    uint32_t ReturnCode;
    const uint8_t *Response;
    size_t ResponseLength;
} APPLE_MTP_ACK_VIEW;

APPLE_MTP_STATUS
AppleMtpParseMessage(
    const APPLE_DOCKCHANNEL_PACKET_VIEW *Packet,
    APPLE_MTP_MESSAGE_VIEW *View
    );

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
    );

APPLE_MTP_STATUS
AppleMtpBuildGetReportPacket(
    uint8_t Sequence,
    uint8_t Interface,
    APPLE_MTP_REPORT_TYPE ReportType,
    uint8_t ReportId,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    );

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
    );

APPLE_MTP_STATUS
AppleMtpBuildEnableInterfacePacket(
    uint8_t Sequence,
    uint8_t TargetInterface,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    );

APPLE_MTP_STATUS
AppleMtpBuildResetInterfacePacket(
    uint8_t Sequence,
    uint8_t TargetInterface,
    uint8_t State,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    );

APPLE_MTP_STATUS
AppleMtpBuildSendFirmwarePacket(
    uint8_t Sequence,
    uint8_t TargetInterface,
    uint64_t DeviceAddress,
    uint32_t FirmwareSize,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    );

APPLE_MTP_STATUS
AppleMtpBuildGpioAckPacket(
    uint8_t Sequence,
    const APPLE_MTP_GPIO_EVENT_VIEW *Event,
    uint32_t ReturnCode,
    uint8_t *Output,
    size_t OutputCapacity,
    size_t *OutputLength
    );

APPLE_MTP_STATUS
AppleMtpParseControlCommand(
    const APPLE_MTP_MESSAGE_VIEW *Message,
    APPLE_MTP_CONTROL_COMMAND_VIEW *View
    );

APPLE_MTP_STATUS
AppleMtpParseControlEvent(
    const APPLE_MTP_MESSAGE_VIEW *Message,
    APPLE_MTP_CONTROL_EVENT_VIEW *View
    );

APPLE_MTP_STATUS
AppleMtpInitBlockIteratorInitialize(
    const APPLE_MTP_INIT_EVENT_VIEW *Event,
    APPLE_MTP_INIT_BLOCK_ITERATOR *Iterator
    );

APPLE_MTP_STATUS
AppleMtpInitBlockIteratorNext(
    APPLE_MTP_INIT_BLOCK_ITERATOR *Iterator,
    APPLE_MTP_INIT_BLOCK_VIEW *View
    );

APPLE_MTP_STATUS
AppleMtpValidateAck(
    const APPLE_MTP_MESSAGE_VIEW *Message,
    uint8_t ExpectedSequence,
    uint8_t ExpectedInterface,
    APPLE_MTP_REPORT_TYPE ExpectedReportType,
    APPLE_MTP_REQUEST_TYPE ExpectedRequestType,
    uint8_t ExpectedReportId,
    APPLE_MTP_ACK_VIEW *View
    );

APPLE_MTP_STATUS
AppleMtpClassifyInterfaceName(
    const uint8_t *Name,
    size_t NameLength,
    APPLE_MTP_INTERFACE_KIND *Kind
    );
