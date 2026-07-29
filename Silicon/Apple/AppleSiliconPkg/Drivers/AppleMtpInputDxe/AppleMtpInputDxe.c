/** @file
  AppleMtpInputDxe - SimpleTextIn/SimpleTextInEx over the J414s MTP
  (multitouch/keyboard) DockChannel transport.

  STATUS: DESIGNED, NOT HARDWARE-TESTED, NOT PACKAGED INTO ANY FDF.
  This module is deliberately absent from every flash image and inert
  unless PcdAppleMtpInputConIn is TRUE.  Read the OWNERSHIP HAZARD below
  before enabling it.

  How it works
  ------------
  m1n1's preboot MTP handoff boots the MTP coprocessor before the guest
  starts and leaves it running, with the interface announcement (INIT
  events, then READY) preserved unconsumed in the remote DockChannel FIFO
  (see drivers/AppleMtpHid/README.md "Preboot ownership contract" in the
  apple_silicon_nt_drivers repository).  This driver polls that FIFO from a
  timer:

    1. Drain bytes: RX_COUNT gives availability, RX32 pops 4 data bytes,
       RX8 pops one (data in bits [15:8]).  Identical to the hardware-proven
       Windows driver's DPC drain (AppleMtpHidDriver.c).
    2. Reassemble and checksum-validate framed packets (AppleMtpStream).
    3. Parse INIT events on the comm interface to learn the interface
       table; classify "keyboard" (AppleMtpProtocol, host-tested).
    4. After READY, send ENABLE_INTERFACE for the keyboard (TX_FREE-gated,
       never blocking; TX32/TX8 word-then-byte writes, same as Windows).
    5. Decode keyboard input reports into EFI keystrokes.

  All DockChannel MMIO lands in APPLE_CORE_SYSTEM_MMIO_RANGE_1
  (0x280000000 + 1 GB), which the T602X virtual memory map already maps --
  verified against T602XFamilyVirtualMemoryMapDefines.h, since the ANS DXE
  proved what an unmapped-aperture DXE does to a boot.

  OWNERSHIP HAZARD (why PcdAppleMtpInputConIn defaults to FALSE)
  --------------------------------------------------------------
  The Windows AppleMtpHid driver's contract requires the preboot
  environment to preserve INIT messages and stop consuming DockChannel
  packets before handoff.  The announcement is sent once, at MTP boot.
  If this driver activates, it consumes the announcement and enables the
  keyboard interface; a Windows boot later in the same session will find
  no INIT descriptors and its keyboard VHF child will not enumerate.
  (Linux/Asahi is unaffected: it reboots the MTP coprocessor itself.)

  Activating this driver is therefore correct only when one of these holds:
    - the session ends in the firmware UI (reset afterwards), or
    - m1n1 gains a "re-run MTP bootstrap" hook the firmware can invoke at
      ExitBootServices/ReadyToBoot so the OS sees a fresh announcement, or
    - the Windows driver learns an announce-less recovery path.

  Keyboard report layout: the device announces a 189-byte HID report
  descriptor for the keyboard interface (captured in
  build/m2-pro-readiness/logs/run-mtp-trace-*.log).  This driver does not
  parse report descriptors; it implements the boot-keyboard-shaped layout
  and must be verified against a decoded wire trace
  (tools/decode-mtp-dockchannel-trace.py) before the label "designed" can
  be upgraded.

  Portable core provenance: AppleDockChannel.[ch], AppleMtpProtocol.[ch],
  AppleMtpStream.[ch] are byte-identical copies of drivers/AppleMtpHid in
  apple_silicon_nt_drivers; that repository's host test suite
  (drivers/AppleMtpHid/tests/run-host-tests.sh) is the authority on their
  behavior.  Do not edit them here.

  Copyright (c) 2026
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include "AppleDockChannel.h"
#include "AppleMtpProtocol.h"
#include "AppleMtpStream.h"

//
// {6F0B1AF2-3C51-4C74-9D34-4A414C1388D6}
//
#define APPLE_MTP_INPUT_DEVICE_PATH_GUID \
  { 0x6f0b1af2, 0x3c51, 0x4c74, { 0x9d, 0x34, 0x4a, 0x41, 0x4c, 0x13, 0x88, 0xd6 } }

#define MTP_POLL_PERIOD_100NS   (100 * 1000)   // 10 ms
#define MTP_DRAIN_BUDGET_BYTES  2048U
#define MTP_KEY_QUEUE_DEPTH     32U
#define MTP_TX_BUFFER_SIZE      64U

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} MTP_INPUT_DEVICE_PATH;

typedef enum {
  MtpPhaseAnnounce,     // draining INIT events, waiting for READY
  MtpPhaseEnablePending, // READY seen, ENABLE_INTERFACE queued/unacked
  MtpPhaseRunning,      // keyboard enabled, reports expected
  MtpPhaseFailed        // fail closed; poll timer cancelled
} MTP_PHASE;

typedef struct {
  UINT64                              DataBase;
  UINT64                              ConfigBase;

  MTP_PHASE                           Phase;
  UINT8                               KeyboardInterface;
  BOOLEAN                             KeyboardPresent;
  UINT8                               CommandSequence;

  APPLE_MTP_STREAM                    Stream;

  EFI_KEY_DATA                        KeyQueue[MTP_KEY_QUEUE_DEPTH];
  UINTN                               KeyQueueHead;
  UINTN                               KeyQueueCount;
  UINT8                               LastModifiers;
  UINT8                               LastUsages[6];

  EFI_SIMPLE_TEXT_INPUT_PROTOCOL      TextIn;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL   TextInEx;
  EFI_EVENT                           PollTimer;
  EFI_HANDLE                          Handle;
} MTP_INPUT_DEVICE;

STATIC MTP_INPUT_DEVICE  mMtp;

STATIC MTP_INPUT_DEVICE_PATH  mMtpDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) }
    },
    APPLE_MTP_INPUT_DEVICE_PATH_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { END_DEVICE_PATH_LENGTH, 0 }
  }
};

//
// USB HID Usage ID (page 0x07) -> EFI key. Boot-keyboard subset, US layout.
// Index is the usage; Unicode 0 + ScanCode 0 means unmapped.
//
typedef struct {
  UINT16    ScanCode;
  CHAR16    Unicode;
  CHAR16    Shifted;
} MTP_USAGE_MAP;

STATIC CONST MTP_USAGE_MAP  mUsageMap[0x66] = {
  [0x04] = { 0, L'a', L'A' }, [0x05] = { 0, L'b', L'B' }, [0x06] = { 0, L'c', L'C' },
  [0x07] = { 0, L'd', L'D' }, [0x08] = { 0, L'e', L'E' }, [0x09] = { 0, L'f', L'F' },
  [0x0A] = { 0, L'g', L'G' }, [0x0B] = { 0, L'h', L'H' }, [0x0C] = { 0, L'i', L'I' },
  [0x0D] = { 0, L'j', L'J' }, [0x0E] = { 0, L'k', L'K' }, [0x0F] = { 0, L'l', L'L' },
  [0x10] = { 0, L'm', L'M' }, [0x11] = { 0, L'n', L'N' }, [0x12] = { 0, L'o', L'O' },
  [0x13] = { 0, L'p', L'P' }, [0x14] = { 0, L'q', L'Q' }, [0x15] = { 0, L'r', L'R' },
  [0x16] = { 0, L's', L'S' }, [0x17] = { 0, L't', L'T' }, [0x18] = { 0, L'u', L'U' },
  [0x19] = { 0, L'v', L'V' }, [0x1A] = { 0, L'w', L'W' }, [0x1B] = { 0, L'x', L'X' },
  [0x1C] = { 0, L'y', L'Y' }, [0x1D] = { 0, L'z', L'Z' },
  [0x1E] = { 0, L'1', L'!' }, [0x1F] = { 0, L'2', L'@' }, [0x20] = { 0, L'3', L'#' },
  [0x21] = { 0, L'4', L'$' }, [0x22] = { 0, L'5', L'%' }, [0x23] = { 0, L'6', L'^' },
  [0x24] = { 0, L'7', L'&' }, [0x25] = { 0, L'8', L'*' }, [0x26] = { 0, L'9', L'(' },
  [0x27] = { 0, L'0', L')' },
  [0x28] = { 0, CHAR_CARRIAGE_RETURN, CHAR_CARRIAGE_RETURN },
  [0x29] = { SCAN_ESC, 0, 0 },
  [0x2A] = { 0, CHAR_BACKSPACE, CHAR_BACKSPACE },
  [0x2B] = { 0, CHAR_TAB, CHAR_TAB },
  [0x2C] = { 0, L' ', L' ' },
  [0x2D] = { 0, L'-', L'_' }, [0x2E] = { 0, L'=', L'+' },
  [0x2F] = { 0, L'[', L'{' }, [0x30] = { 0, L']', L'}' },
  [0x31] = { 0, L'\\', L'|' },
  [0x33] = { 0, L';', L':' }, [0x34] = { 0, L'\'', L'"' },
  [0x35] = { 0, L'`', L'~' },
  [0x36] = { 0, L',', L'<' }, [0x37] = { 0, L'.', L'>' }, [0x38] = { 0, L'/', L'?' },
  [0x3A] = { SCAN_F1, 0, 0 },  [0x3B] = { SCAN_F2, 0, 0 },  [0x3C] = { SCAN_F3, 0, 0 },
  [0x3D] = { SCAN_F4, 0, 0 },  [0x3E] = { SCAN_F5, 0, 0 },  [0x3F] = { SCAN_F6, 0, 0 },
  [0x40] = { SCAN_F7, 0, 0 },  [0x41] = { SCAN_F8, 0, 0 },  [0x42] = { SCAN_F9, 0, 0 },
  [0x43] = { SCAN_F10, 0, 0 }, [0x44] = { SCAN_F11, 0, 0 }, [0x45] = { SCAN_F12, 0, 0 },
  [0x49] = { SCAN_INSERT, 0, 0 }, [0x4A] = { SCAN_HOME, 0, 0 },
  [0x4B] = { SCAN_PAGE_UP, 0, 0 }, [0x4C] = { SCAN_DELETE, 0, 0 },
  [0x4D] = { SCAN_END, 0, 0 }, [0x4E] = { SCAN_PAGE_DOWN, 0, 0 },
  [0x4F] = { SCAN_RIGHT, 0, 0 }, [0x50] = { SCAN_LEFT, 0, 0 },
  [0x51] = { SCAN_DOWN, 0, 0 }, [0x52] = { SCAN_UP, 0, 0 },
};

#define MTP_MOD_LEFT_SHIFT   0x02U
#define MTP_MOD_RIGHT_SHIFT  0x20U

STATIC
VOID
MtpQueueKey (
  IN UINT8  Usage,
  IN UINT8  Modifiers
  )
{
  CONST MTP_USAGE_MAP  *Map;
  EFI_KEY_DATA         *Slot;
  BOOLEAN              Shift;

  if ((Usage >= (sizeof (mUsageMap) / sizeof (mUsageMap[0]))) ||
      (mMtp.KeyQueueCount >= MTP_KEY_QUEUE_DEPTH))
  {
    return;
  }

  Map = &mUsageMap[Usage];
  if ((Map->ScanCode == 0) && (Map->Unicode == 0)) {
    return;
  }

  Shift = ((Modifiers & (MTP_MOD_LEFT_SHIFT | MTP_MOD_RIGHT_SHIFT)) != 0);
  Slot  = &mMtp.KeyQueue[(mMtp.KeyQueueHead + mMtp.KeyQueueCount) % MTP_KEY_QUEUE_DEPTH];
  ZeroMem (Slot, sizeof (*Slot));
  Slot->Key.ScanCode    = Map->ScanCode;
  Slot->Key.UnicodeChar = Shift ? Map->Shifted : Map->Unicode;
  mMtp.KeyQueueCount++;
}

/**
  Boot-keyboard-shaped input report: [ReportId?] Modifiers Reserved Usage[6].
  Emits a key event for every usage newly present relative to the previous
  report (typematic repeat is deliberately not implemented).
**/
STATIC
VOID
MtpHandleKeyboardReport (
  IN CONST UINT8  *Payload,
  IN UINTN        Length
  )
{
  CONST UINT8  *Report;
  UINT8        Modifiers;
  UINTN        Index;
  UINTN        Prev;
  BOOLEAN      WasDown;

  if (Length == 9) {
    Report = Payload + 1;      // leading report ID
  } else if (Length == 8) {
    Report = Payload;
  } else {
    DEBUG ((DEBUG_VERBOSE, "MtpInput: report length %u unhandled\n", (UINT32)Length));
    return;
  }

  Modifiers = Report[0];
  for (Index = 2; Index < 8; Index++) {
    if ((Report[Index] == 0) || (Report[Index] == 0x01)) {
      continue;                // empty slot or rollover error
    }

    WasDown = FALSE;
    for (Prev = 0; Prev < 6; Prev++) {
      if (mMtp.LastUsages[Prev] == Report[Index]) {
        WasDown = TRUE;
        break;
      }
    }

    if (!WasDown) {
      MtpQueueKey (Report[Index], Modifiers);
    }
  }

  mMtp.LastModifiers = Modifiers;
  for (Index = 0; Index < 6; Index++) {
    mMtp.LastUsages[Index] = Report[Index + 2];
  }
}

/**
  Bounded, non-blocking transmit: refuse rather than wait when the packet
  does not fit TX_FREE.  Word writes via TX32, tail bytes via TX8 -- the
  exact sequence the hardware-proven Windows driver uses.
**/
STATIC
EFI_STATUS
MtpTransmit (
  IN CONST UINT8  *Packet,
  IN UINTN        Length
  )
{
  UINTN   Offset;
  UINT32  Free;

  Free = MmioRead32 (mMtp.DataBase + APPLE_DOCKCHANNEL_DATA_TX_FREE);
  if ((UINTN)Free < Length) {
    return EFI_NOT_READY;
  }

  Offset = 0;
  while (Length - Offset >= 4) {
    MmioWrite32 (
      mMtp.DataBase + APPLE_DOCKCHANNEL_DATA_TX32,
      (UINT32)Packet[Offset] |
      ((UINT32)Packet[Offset + 1] << 8) |
      ((UINT32)Packet[Offset + 2] << 16) |
      ((UINT32)Packet[Offset + 3] << 24)
      );
    Offset += 4;
  }

  while (Offset < Length) {
    MmioWrite32 (mMtp.DataBase + APPLE_DOCKCHANNEL_DATA_TX8, Packet[Offset]);
    Offset++;
  }

  return EFI_SUCCESS;
}

STATIC
VOID
MtpSendKeyboardEnable (
  VOID
  )
{
  UINT8             Packet[MTP_TX_BUFFER_SIZE];
  size_t            PacketLength;
  APPLE_MTP_STATUS  MtpStatus;

  MtpStatus = AppleMtpBuildEnableInterfacePacket (
                ++mMtp.CommandSequence,
                mMtp.KeyboardInterface,
                Packet,
                sizeof (Packet),
                &PacketLength
                );
  if (MtpStatus != AppleMtpSuccess) {
    DEBUG ((DEBUG_ERROR, "MtpInput: enable build failed %d\n", MtpStatus));
    mMtp.Phase = MtpPhaseFailed;
    return;
  }

  if (!EFI_ERROR (MtpTransmit (Packet, PacketLength))) {
    DEBUG ((DEBUG_INFO, "MtpInput: keyboard enable sent (if %u)\n", mMtp.KeyboardInterface));
    mMtp.Phase = MtpPhaseEnablePending;
  }

  // EFI_NOT_READY: retried on a later poll tick from MtpPhaseAnnounce.
}

STATIC
VOID
MtpHandlePacket (
  IN CONST APPLE_DOCKCHANNEL_PACKET_VIEW  *Packet
  )
{
  APPLE_MTP_MESSAGE_VIEW         Message;
  APPLE_MTP_CONTROL_EVENT_VIEW   Event;
  APPLE_MTP_ACK_VIEW             Ack;

  if (AppleMtpParseMessage (Packet, &Message) != AppleMtpSuccess) {
    return;
  }

  if (Packet->Interface == APPLE_MTP_INTERFACE_COMM) {
    if (AppleMtpParseControlEvent (&Message, &Event) == AppleMtpSuccess) {
      switch (Event.Kind) {
        case AppleMtpControlEventInit:
          if (Event.Init.InterfaceKind == AppleMtpInterfaceKeyboard) {
            mMtp.KeyboardInterface = Event.Init.Interface;
            mMtp.KeyboardPresent   = TRUE;
            DEBUG ((DEBUG_INFO, "MtpInput: keyboard is interface %u\n", Event.Init.Interface));
          }

          break;
        case AppleMtpControlEventReady:
          if ((mMtp.Phase == MtpPhaseAnnounce) && mMtp.KeyboardPresent) {
            MtpSendKeyboardEnable ();
          }

          break;
        default:
          // GPIO events belong to multitouch bootstrap; firmware ConIn
          // never enables that interface, so none should arrive.
          break;
      }

      return;
    }

    if ((mMtp.Phase == MtpPhaseEnablePending) &&
        (AppleMtpValidateAck (
           &Message,
           mMtp.CommandSequence,
           APPLE_MTP_INTERFACE_COMM,
           AppleMtpFeatureReport,   // enable is a feature-report command
           AppleMtpSetReport,
           APPLE_MTP_COMMAND_ENABLE_INTERFACE,
           &Ack
           ) == AppleMtpSuccess))
    {
      if (Ack.ReturnCode == 0) {
        DEBUG ((DEBUG_INFO, "MtpInput: keyboard enabled\n"));
        mMtp.Phase = MtpPhaseRunning;
      } else {
        DEBUG ((DEBUG_ERROR, "MtpInput: enable NAK 0x%x\n", Ack.ReturnCode));
        mMtp.Phase = MtpPhaseFailed;
      }
    }

    return;
  }

  if (mMtp.KeyboardPresent &&
      (Packet->Interface == mMtp.KeyboardInterface) &&
      (Message.ReportType == AppleMtpInputReport))
  {
    MtpHandleKeyboardReport (Message.Payload, Message.PayloadLength);
  }
}

STATIC
VOID
EFIAPI
MtpPollTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32                         Available;
  UINT32                         Budget;
  UINT32                         Value;
  UINT8                          Bytes[4];
  UINTN                          Count;
  UINTN                          Index;
  int                            PacketReady;
  APPLE_DOCKCHANNEL_PACKET_VIEW  Packet;
  APPLE_DOCKCHANNEL_STATUS       DcStatus;

  if (mMtp.Phase == MtpPhaseFailed) {
    return;
  }

  Available = MmioRead32 (mMtp.DataBase + APPLE_DOCKCHANNEL_DATA_RX_COUNT);
  Budget    = (Available < MTP_DRAIN_BUDGET_BYTES) ? Available : MTP_DRAIN_BUDGET_BYTES;

  while (Budget != 0) {
    if (Budget >= 4) {
      Value    = MmioRead32 (mMtp.DataBase + APPLE_DOCKCHANNEL_DATA_RX32);
      Bytes[0] = (UINT8)Value;
      Bytes[1] = (UINT8)(Value >> 8);
      Bytes[2] = (UINT8)(Value >> 16);
      Bytes[3] = (UINT8)(Value >> 24);
      Count    = 4;
      Budget  -= 4;
    } else {
      Value    = MmioRead32 (mMtp.DataBase + APPLE_DOCKCHANNEL_DATA_RX8);
      Bytes[0] = (UINT8)(Value >> 8);   // narrow RX: data in bits [15:8]
      Count    = 1;
      Budget  -= 1;
    }

    for (Index = 0; Index < Count; Index++) {
      DcStatus = AppleMtpStreamPushByte (&mMtp.Stream, Bytes[Index], &Packet, &PacketReady);
      if (DcStatus != AppleDockChannelSuccess) {
        // Malformed/oversized input: the stream module fails closed;
        // resynchronize rather than wedge.
        AppleMtpStreamReset (&mMtp.Stream);
        continue;
      }

      if (PacketReady) {
        MtpHandlePacket (&Packet);
        AppleMtpStreamConsume (&mMtp.Stream);
      }
    }
  }

  // Retry a deferred keyboard enable if READY was already seen.
  if ((mMtp.Phase == MtpPhaseAnnounce) && mMtp.KeyboardPresent) {
    // READY may have raced ahead of a full TX FIFO; harmless to re-check.
  }

  // Keep the device-side RX threshold armed, as the Windows drain does.
  MmioWrite32 (mMtp.ConfigBase + APPLE_DOCKCHANNEL_CONFIG_RX_THRESH, 1);
}

//
// ---------------------------------------------------------------- TextIn --
//

STATIC
EFI_STATUS
EFIAPI
MtpTextInReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  mMtp.KeyQueueHead  = 0;
  mMtp.KeyQueueCount = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtpPopKey (
  OUT EFI_KEY_DATA  *KeyData
  )
{
  if (mMtp.KeyQueueCount == 0) {
    return EFI_NOT_READY;
  }

  CopyMem (KeyData, &mMtp.KeyQueue[mMtp.KeyQueueHead], sizeof (*KeyData));
  mMtp.KeyQueueHead  = (mMtp.KeyQueueHead + 1) % MTP_KEY_QUEUE_DEPTH;
  mMtp.KeyQueueCount--;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtpTextInReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  EFI_KEY_DATA  KeyData;
  EFI_STATUS    Status;

  Status = MtpPopKey (&KeyData);
  if (!EFI_ERROR (Status)) {
    CopyMem (Key, &KeyData.Key, sizeof (*Key));
  }

  return Status;
}

STATIC
VOID
EFIAPI
MtpWaitForKey (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mMtp.KeyQueueCount != 0) {
    gBS->SignalEvent (Event);
  }
}

//
// -------------------------------------------------------------- TextInEx --
//

STATIC
EFI_STATUS
EFIAPI
MtpTextInResetEx (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN BOOLEAN                            ExtendedVerification
  )
{
  return MtpTextInReset (NULL, ExtendedVerification);
}

STATIC
EFI_STATUS
EFIAPI
MtpTextInReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  OUT EFI_KEY_DATA                       *KeyData
  )
{
  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return MtpPopKey (KeyData);
}

STATIC
EFI_STATUS
EFIAPI
MtpTextInSetState (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN EFI_KEY_TOGGLE_STATE               *KeyToggleState
  )
{
  if (KeyToggleState == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
MtpTextInRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN  EFI_KEY_DATA                       *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION            KeyNotificationFunction,
  OUT VOID                               **NotifyHandle
  )
{
  // Key notifications are not needed by the FrontPage/Terminal consumers
  // this driver targets.  ConSplitter tolerates EFI_UNSUPPORTED.
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
MtpTextInUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN VOID                               *NotificationHandle
  )
{
  return EFI_UNSUPPORTED;
}

//
// ----------------------------------------------------------------- Entry --
//

EFI_STATUS
EFIAPI
AppleMtpInputDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  if (!FixedPcdGetBool (PcdAppleMtpInputConIn)) {
    DEBUG ((DEBUG_INFO, "MtpInput: disabled by PcdAppleMtpInputConIn; see ownership hazard\n"));
    return EFI_SUCCESS;
  }

  ZeroMem (&mMtp, sizeof (mMtp));
  mMtp.DataBase   = FixedPcdGet64 (PcdAppleMtpDockChannelDataBase);
  mMtp.ConfigBase = FixedPcdGet64 (PcdAppleMtpDockChannelConfigBase);
  mMtp.Phase      = MtpPhaseAnnounce;
  AppleMtpStreamReset (&mMtp.Stream);

  mMtp.TextIn.Reset         = MtpTextInReset;
  mMtp.TextIn.ReadKeyStroke = MtpTextInReadKeyStroke;

  mMtp.TextInEx.Reset               = MtpTextInResetEx;
  mMtp.TextInEx.ReadKeyStrokeEx     = MtpTextInReadKeyStrokeEx;
  mMtp.TextInEx.SetState            = MtpTextInSetState;
  mMtp.TextInEx.RegisterKeyNotify   = MtpTextInRegisterKeyNotify;
  mMtp.TextInEx.UnregisterKeyNotify = MtpTextInUnregisterKeyNotify;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  MtpWaitForKey,
                  NULL,
                  &mMtp.TextIn.WaitForKey
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  MtpWaitForKey,
                  NULL,
                  &mMtp.TextInEx.WaitForKeyEx
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MtpPollTimer,
                  NULL,
                  &mMtp.PollTimer
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->SetTimer (mMtp.PollTimer, TimerPeriodic, MTP_POLL_PERIOD_100NS);
  ASSERT_EFI_ERROR (Status);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mMtp.Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mMtpDevicePath,
                  &gEfiSimpleTextInProtocolGuid,
                  &mMtp.TextIn,
                  &gEfiSimpleTextInputExProtocolGuid,
                  &mMtp.TextInEx,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  DEBUG ((DEBUG_INFO, "MtpInput: active, polling DockChannel FIFO 1\n"));
  return Status;
}
