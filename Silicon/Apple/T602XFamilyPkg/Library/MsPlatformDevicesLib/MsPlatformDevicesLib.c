/** @file
 *MsPlatformDevicesLib  - Device specific library.

Copyright (C) Microsoft Corporation. All rights reserved.
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Protocol/DevicePath.h>

#include <Guid/SerialPortLibVendor.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MsPlatformDevicesLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

typedef struct {
  VENDOR_DEVICE_PATH       DisplayDevicePath;
  EFI_DEVICE_PATH_PROTOCOL EndDevicePath;
} EFI_DISPLAY_DEVICE_PATH;

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  UART_DEVICE_PATH            Uart;
  VENDOR_DEVICE_PATH          TerminalType;
  EFI_DEVICE_PATH_PROTOCOL    End;
} PLATFORM_SERIAL_CONSOLE_DEVICE_PATH;

EFI_DISPLAY_DEVICE_PATH DisplayDevicePath =
{
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(END_DEVICE_PATH_LENGTH),
      (UINT8)((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};
//
// The Apple UART, as published by MdeModulePkg SerialDxe over
// AppleUartSerialPortLib, with a TTYTERM terminal on top.  The UART node
// values must byte-match the device path SerialDxe installs, which it fills
// from PcdUartDefault{BaudRate,DataBits,Parity,StopBits} (115200/8/1/1 on
// this platform); a mismatched node would make the ConIn/ConOut variable
// entry connect nothing.  The terminal-type node selects TTYTERM
// (gEfiTtyTermGuid), matching PcdDefaultTerminalType|4.
//
// This is the same wire the m1n1 launcher already captures as "Mu's
// secondary UART": input typed into that session becomes SerialPortRead()
// data, which AppleUartSerialPortLib already implements.  RX is unused by
// the serial DEBUG/status-code sink, so console input does not collide
// with log output.
//
PLATFORM_SERIAL_CONSOLE_DEVICE_PATH SerialConsoleDevicePath =
{
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    EDKII_SERIAL_PORT_LIB_VENDOR_GUID
  },
  {
    {
      MESSAGING_DEVICE_PATH,
      MSG_UART_DP,
      {
        (UINT8)(sizeof(UART_DEVICE_PATH)),
        (UINT8)((sizeof(UART_DEVICE_PATH)) >> 8)
      }
    },
    0,        // Reserved
    115200,   // BaudRate  == PcdUartDefaultBaudRate
    8,        // DataBits  == PcdUartDefaultDataBits
    1,        // Parity    == PcdUartDefaultParity (NoParity)
    1         // StopBits  == PcdUartDefaultStopBits (OneStopBit)
  },
  {
    {
      MESSAGING_DEVICE_PATH,
      MSG_VENDOR_DP,
      {
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    // gEfiTtyTermGuid: TTYTERM terminal type for TerminalDxe
    { 0x7d916d80, 0x5bb1, 0x458c, { 0xa4, 0x8f, 0xe2, 0x5f, 0xdd, 0x51, 0xef, 0x94 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(END_DEVICE_PATH_LENGTH),
      (UINT8)((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};

//
// Predefined platform default console device path
//
BDS_CONSOLE_CONNECT_ENTRY gPlatformConsoles[] =
{
  {
    (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath,
    CONSOLE_OUT | STD_ERROR
  },
  {
    // Serial terminal: today the only input Mu can drive on J414s.  The
    // internal keyboard is MTP/DockChannel (no Mu driver yet) and USB HID
    // only helps when a keyboard is physically attached.
    (EFI_DEVICE_PATH_PROTOCOL *)&SerialConsoleDevicePath,
    CONSOLE_IN | CONSOLE_OUT
  },
  {
    NULL,
    0
  }
};

EFI_DEVICE_PATH_PROTOCOL *gPlatformConInDeviceList[] = {NULL};

/**
Library function used to provide the platform SD Card device path
**/
EFI_DEVICE_PATH_PROTOCOL *
EFIAPI
GetSdCardDevicePath (
  VOID
  )
{
  return NULL;
}

/**
  Library function used to determine if the DevicePath is a valid bootable 'USB' device.
  USB here indicates the port connection type not the device protocol.
  With TBT or USB4 support PCIe storage devices are valid 'USB' boot options.

  This must not return TRUE unconditionally.  MsBootPolicy's "USB Storage"
  option filters candidate filesystems with
  IsDevicePathUSB() = has-MSG_USB_DP-node || MsBootPolicyLibIsDevicePathUsb(),
  and the second term lands here.  When this stub returned TRUE for every
  device path, the ANS-published internal SSD passed the USB-only filter,
  and "USB Storage" booted the internal disk's Asahi/Fedora ESP
  (shim -> fallback -> GRUB) in preference to the Windows loader on the
  actual USB stick -- measured on J414s 2026-07-29, mu-secondary-uart.log:
  MsBootPolicy connected the 4Kn LastBlock=0x747AE14 ANS disk and launched
  HD(3,GPT,A16EE7E0-...,0x6726506,0x1F400)\EFI\BOOT\fbaa64.efi.  It also
  made FilterNoUSB() ("Internal Storage") unable to match anything, ever.

  A USB-connected device always carries a Usb() node on this platform's
  XHCI paths, so the extra platform-specific cases (TBT/USB4 tunneled PCIe
  storage) are the only thing this hook may add.  T6020 J414s publishes no
  such tunneled storage to UEFI today, so: USB class/WWID nodes only.
**/
BOOLEAN
EFIAPI
PlatformIsDevicePathUsb (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  if (DevicePath == NULL) {
    return FALSE;
  }

  for (Node = DevicePath; !IsDevicePathEndType (Node); Node = NextDevicePathNode (Node)) {
    if (DevicePathType (Node) == MESSAGING_DEVICE_PATH) {
      switch (DevicePathSubType (Node)) {
        case MSG_USB_DP:
        case MSG_USB_CLASS_DP:
        case MSG_USB_WWID_DP:
          return TRUE;
        default:
          break;
      }
    }
  }

  return FALSE;
}

/**
Library function used to provide the list of platform devices that MUST be
connected at the beginning of BDS
**/
EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
GetPlatformConnectList (
  VOID
  )
{
  return NULL;
}

/**
 * Library function used to provide the list of platform console devices.
 */
BDS_CONSOLE_CONNECT_ENTRY *
EFIAPI
GetPlatformConsoleList (
  VOID
  )
{
  return (BDS_CONSOLE_CONNECT_ENTRY *)&gPlatformConsoles;
}

/**
Library function used to provide the list of platform devices that MUST be connected
to support ConsoleIn activity.  This call occurs on the ConIn connect event, and
allows platforms to do enable specific devices ConsoleIn support.
**/
EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
GetPlatformConnectOnConInList (
  VOID
  )
{
  return NULL;
}

/**
Library function used to provide the console type.  For ConType == DisplayPath,
device path is filled in to the exact controller to use.  For other ConTypes, DisplayPath
must NULL. The device path must NOT be freed.
**/
EFI_HANDLE
EFIAPI
GetPlatformPreferredConsole (
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                Handle = NULL;
  EFI_DEVICE_PATH_PROTOCOL *TempDevicePath;

  TempDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath;

  Status = gBS->LocateDevicePath(
      &gEfiGraphicsOutputProtocolGuid, &TempDevicePath, &Handle);
  if (!EFI_ERROR(Status) && IsDevicePathEnd(TempDevicePath)) {
  }
  else {
    DEBUG(
        (DEBUG_ERROR,
         "%a - Unable to locate platform preferred console. Code=%r\n",
         __FUNCTION__, Status));
    Status = EFI_DEVICE_ERROR;
  }

  if (Handle != NULL) {
    //
    // Connect the GOP driver
    //
    gBS->ConnectController(Handle, NULL, NULL, TRUE);

    //
    // Get the GOP device path
    // NOTE: We may get a device path that contains Controller node in it.
    //
    TempDevicePath = EfiBootManagerGetGopDevicePath(Handle);
    *DevicePath    = TempDevicePath;
  }

  return Handle;
}
