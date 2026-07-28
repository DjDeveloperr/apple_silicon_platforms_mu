/* SimpleFbDxe: Simple FrameBuffer */
#include <PiDxe.h>
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/AppleDTLib.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/EdidActive.h>
#include <Protocol/EdidDiscovered.h>

/// Defines
/*
 * Convert enum video_log2_bpp to bytes and bits. Note we omit the outer
 * brackets to allow multiplication by fractional pixels.
 */
#define VNBYTES(bpix) (1 << (bpix)) / 8
#define VNBITS(bpix) (1 << (bpix))

#define POS_TO_FB(posX, posY)                                                  \
  ((UINT8                                                                      \
        *)((UINTN)This->Mode->FrameBufferBase + (posY)*This->Mode->Info->PixelsPerScanLine * FB_BYTES_PER_PIXEL + (posX)*FB_BYTES_PER_PIXEL))

#define FB_BITS_PER_PIXEL (32)
#define FB_BYTES_PER_PIXEL (FB_BITS_PER_PIXEL / 8)
#define DISPLAYDXE_PHYSICALADDRESS32(_x_) (UINTN)((_x_)&0xFFFFFFFF)

#define APPLE_FRAMEBUFFER_DEPTH_MASK  0xFF

#define X2R10G10B10_RED_MASK       0x3FF00000
#define X2R10G10B10_GREEN_MASK     0x000FFC00
#define X2R10G10B10_BLUE_MASK      0x000003FF
#define X2R10G10B10_RESERVED_MASK  0xC0000000

/*
 * Bits per pixel selector. Each value n is such that the bits-per-pixel is
 * 2 ^ n
 */
enum video_log2_bpp {
  VIDEO_BPP1 = 0,
  VIDEO_BPP2,
  VIDEO_BPP4,
  VIDEO_BPP8,
  VIDEO_BPP16,
  VIDEO_BPP32,
};

typedef struct {
  VENDOR_DEVICE_PATH DisplayDevicePath;
  EFI_DEVICE_PATH    EndDevicePath;
} DISPLAY_DEVICE_PATH;

DISPLAY_DEVICE_PATH mDisplayDevicePath = {
    {{HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
          (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
          (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
      }},
     EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID},
    {END_DEVICE_PATH_TYPE,
     END_ENTIRE_DEVICE_PATH_SUBTYPE,
     {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}};

/// Declares

STATIC FRAME_BUFFER_CONFIGURE *mFrameBufferBltLibConfigure;
STATIC UINTN mFrameBufferBltLibConfigureSize;

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber);

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL);

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mDisplay = {
    DisplayQueryMode, DisplaySetMode, DisplayBlt, NULL};

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;
  Status = gBS->AllocatePool(
      EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
      (VOID **)Info);

  ASSERT_EFI_ERROR(Status);
  if (EFI_ERROR(Status))
    return Status;

  ZeroMem(*Info, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));
  *SizeOfInfo                   = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  (*Info)->Version              = This->Mode->Info->Version;
  (*Info)->HorizontalResolution = This->Mode->Info->HorizontalResolution;
  (*Info)->VerticalResolution   = This->Mode->Info->VerticalResolution;
  (*Info)->PixelFormat          = This->Mode->Info->PixelFormat;
  (*Info)->PixelInformation     = This->Mode->Info->PixelInformation;
  (*Info)->PixelsPerScanLine    = This->Mode->Info->PixelsPerScanLine;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber)
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL)
{
  RETURN_STATUS Status;
  EFI_TPL       Tpl;
  //
  // We have to raise to TPL_NOTIFY, so we make an atomic write to the frame
  // buffer. We would not want a timer based event (Cursor, ...) to come in
  // while we are doing this operation.
  //
  Tpl    = gBS->RaiseTPL(TPL_NOTIFY);
  Status = FrameBufferBlt(
      mFrameBufferBltLibConfigure, BltBuffer, BltOperation, SourceX, SourceY,
      DestinationX, DestinationY, Width, Height, Delta);
  gBS->RestoreTPL(Tpl);

  return RETURN_ERROR(Status) ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}


/*
 * Windows has no other source of monitor geometry on this platform.  Without an
 * EDID, BasicDisplay's monitor has neither an aspect ratio nor a physical size,
 * so Windows composes the desktop at 4:3 and pillarboxes it into the real
 * framebuffer -- measured on J414s as 2618 px of content centred in 3024 px,
 * with 203 px black bars either side, because 1964 * 4/3 = 2618.67.  The same
 * gap pins DPI at 96, which is why nothing scales for a HiDPI panel.  Publish a
 * synthesized EDID describing the actual scanout and the panel's real size.
 */
#define EDID_BLOCK_SIZE 128

STATIC UINT8 mEdid[EDID_BLOCK_SIZE];
STATIC EFI_EDID_DISCOVERED_PROTOCOL mEdidDiscovered;
STATIC EFI_EDID_ACTIVE_PROTOCOL     mEdidActive;

STATIC VOID SimpleFbBuildEdid(
    IN UINT32 Width, IN UINT32 Height, IN UINT32 WidthMm, IN UINT32 HeightMm)
{
  //
  // Blanking is not read back from DCP; these are ordinary reduced-blanking
  // values.  Nothing here drives a mode set -- BasicDisplay never reprograms
  // the scanout -- so only the active geometry and the physical size matter.
  //
  UINT32 HBlank = 80;
  UINT32 VBlank = 24;
  UINT32 Clock10Khz = ((Width + HBlank) * (Height + VBlank) * 60) / 10000;
  UINT8  *Dtd;
  UINT32 Index;
  UINT8  Checksum = 0;

  SetMem(mEdid, sizeof(mEdid), 0);
  SetMem(mEdid + 1, 6, 0xFF);                 /* header 00 FF*6 00 */

  mEdid[8]  = 0x06;                           /* "APP" */
  mEdid[9]  = 0x10;
  mEdid[10] = 0x14;                           /* product code */
  mEdid[11] = 0x14;
  mEdid[16] = 0;                              /* week unspecified */
  mEdid[17] = 33;                             /* 2023 */
  mEdid[18] = 1;                              /* EDID 1.4 */
  mEdid[19] = 4;
  mEdid[20] = 0x95;                           /* digital, 8 bpc, DisplayPort */
  mEdid[21] = (UINT8)((WidthMm + 5) / 10);    /* cm */
  mEdid[22] = (UINT8)((HeightMm + 5) / 10);
  mEdid[23] = 120;                            /* gamma 2.2 */
  mEdid[24] = 0x02;                           /* preferred timing is native */

  /* sRGB chromaticity */
  mEdid[25] = 0xEE; mEdid[26] = 0x91; mEdid[27] = 0xA3; mEdid[28] = 0x54;
  mEdid[29] = 0x4C; mEdid[30] = 0x99; mEdid[31] = 0x26; mEdid[32] = 0x0F;
  mEdid[33] = 0x50; mEdid[34] = 0x54;

  /* No established or standard timings: the panel has exactly one mode. */
  for (Index = 38; Index < 54; Index += 2) {
    mEdid[Index]     = 0x01;
    mEdid[Index + 1] = 0x01;
  }

  Dtd = &mEdid[54];
  Dtd[0]  = (UINT8)(Clock10Khz & 0xFF);
  Dtd[1]  = (UINT8)(Clock10Khz >> 8);
  Dtd[2]  = (UINT8)(Width & 0xFF);
  Dtd[3]  = (UINT8)(HBlank & 0xFF);
  Dtd[4]  = (UINT8)(((Width >> 8) << 4) | ((HBlank >> 8) & 0xF));
  Dtd[5]  = (UINT8)(Height & 0xFF);
  Dtd[6]  = (UINT8)(VBlank & 0xFF);
  Dtd[7]  = (UINT8)(((Height >> 8) << 4) | ((VBlank >> 8) & 0xF));
  Dtd[8]  = 24;                               /* hsync front porch */
  Dtd[9]  = 32;                               /* hsync width */
  Dtd[10] = (3 << 4) | 6;                     /* vsync porch/width */
  Dtd[11] = 0;
  Dtd[12] = (UINT8)(WidthMm & 0xFF);
  Dtd[13] = (UINT8)(HeightMm & 0xFF);
  Dtd[14] = (UINT8)(((WidthMm >> 8) << 4) | ((HeightMm >> 8) & 0xF));
  Dtd[17] = 0x1E;                             /* digital separate, +h +v */

  /* Descriptor 2: monitor name.  The text field is exactly 13 bytes: a name
     shorter than that is terminated with 0x0A and padded with spaces. */
  mEdid[72 + 3] = 0xFC;
  SetMem(&mEdid[72 + 5], 13, 0x20);
  CopyMem(&mEdid[72 + 5], "Apple Panel\n", 12);

  /* Descriptors 3 and 4: unused. */
  mEdid[90 + 3]  = 0x10;
  mEdid[108 + 3] = 0x10;

  mEdid[126] = 0;                             /* no extension blocks */
  for (Index = 0; Index < EDID_BLOCK_SIZE - 1; Index++) {
    Checksum = (UINT8)(Checksum + mEdid[Index]);
  }
  mEdid[127] = (UINT8)(0x100 - Checksum);
}

EFI_STATUS
EFIAPI
SimpleFbDxeInitialize(
    IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{

  EFI_STATUS Status             = EFI_SUCCESS;
  EFI_HANDLE hUEFIDisplayHandle = NULL;

  /* Retrieve simple frame buffer from pre-SEC bootloader */
  DEBUG(
      (EFI_D_INFO,
       "SimpleFbDxe: Getting framebuffer parameters from PCD and ADT\n"));

  
  
  struct boot_args *BootArgs = (struct boot_args *)FixedPcdGet64(PcdBootArgsPointer);
  UINT64 FramebufferAddr   = BootArgs->video.base;
  UINT32 FramebufferWidth  = BootArgs->video.width;
  UINT32 FramebufferHeight = BootArgs->video.height;
  UINT32 FramebufferStride = BootArgs->video.stride;
  UINT32 FramebufferDepth  = BootArgs->video.depth & APPLE_FRAMEBUFFER_DEPTH_MASK;

  DEBUG((EFI_D_INFO, "SimpleFbDxe: Framebuffer parameters, Base: 0x%llx, Width: %u, Height: %u, Stride: %u, Depth: %u\n", FramebufferAddr, FramebufferWidth, FramebufferHeight, FramebufferStride, FramebufferDepth));

  /* Sanity check */
  if (FramebufferAddr == 0 || FramebufferWidth == 0 ||
      FramebufferHeight == 0 || FramebufferStride < FramebufferWidth * FB_BYTES_PER_PIXEL ||
      (FramebufferStride % FB_BYTES_PER_PIXEL) != 0 ||
      (FramebufferDepth != 30 && FramebufferDepth != 32)) {
    DEBUG((EFI_D_ERROR, "SimpleFbDxe: Invalid framebuffer parameters\n"));
    return EFI_DEVICE_ERROR;
  }

  /* Prepare struct */
  if (mDisplay.Mode == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE),
        (VOID **)&mDisplay.Mode);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));
  }

  if (mDisplay.Mode->Info == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
        (VOID **)&mDisplay.Mode->Info);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode->Info, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));
  }

  /* Set information */
  mDisplay.Mode->MaxMode       = 1;
  mDisplay.Mode->Mode          = 0;
  mDisplay.Mode->Info->Version = 0;

  mDisplay.Mode->Info->HorizontalResolution = FramebufferWidth;
  mDisplay.Mode->Info->VerticalResolution   = FramebufferHeight;

  /* Apple framebuffers use four bytes per pixel for both 8:8:8 and 10:10:10. */
  UINT32               LineLength = FramebufferStride;
  UINT32               FrameBufferSize    = LineLength * FramebufferHeight;
  EFI_PHYSICAL_ADDRESS FrameBufferAddress = FramebufferAddr;

  mDisplay.Mode->Info->PixelsPerScanLine = FramebufferStride / FB_BYTES_PER_PIXEL;
  if (FramebufferDepth == 30) {
    mDisplay.Mode->Info->PixelFormat = PixelBitMask;
    mDisplay.Mode->Info->PixelInformation.RedMask      = X2R10G10B10_RED_MASK;
    mDisplay.Mode->Info->PixelInformation.GreenMask    = X2R10G10B10_GREEN_MASK;
    mDisplay.Mode->Info->PixelInformation.BlueMask     = X2R10G10B10_BLUE_MASK;
    mDisplay.Mode->Info->PixelInformation.ReservedMask = X2R10G10B10_RESERVED_MASK;
  } else {
    mDisplay.Mode->Info->PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
  }
  mDisplay.Mode->SizeOfInfo      = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  mDisplay.Mode->FrameBufferBase = FrameBufferAddress;
  mDisplay.Mode->FrameBufferSize = FrameBufferSize;

  /* Create the FrameBufferBltLib configuration. */
  Status = FrameBufferBltConfigure(
      (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
      mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);

  if (Status == RETURN_BUFFER_TOO_SMALL) {
    mFrameBufferBltLibConfigure = AllocatePool(mFrameBufferBltLibConfigureSize);
    if (mFrameBufferBltLibConfigure != NULL) {
      Status = FrameBufferBltConfigure(
          (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
          mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);
    }
  }

  ASSERT_EFI_ERROR(Status);

  //
  // The panel is the 14.2" J414s internal display: 302 x 196 mm, which is what
  // turns 3024 x 1964 into ~246 DPI for the guest instead of the 96 DPI Windows
  // assumes when no monitor geometry exists at all.
  //
  SimpleFbBuildEdid(FramebufferWidth, FramebufferHeight, 302, 196);
  mEdidDiscovered.SizeOfEdid = sizeof(mEdid);
  mEdidDiscovered.Edid       = mEdid;
  mEdidActive.SizeOfEdid     = sizeof(mEdid);
  mEdidActive.Edid           = mEdid;

  /* Register handle */
  Status = gBS->InstallMultipleProtocolInterfaces(
      &hUEFIDisplayHandle, &gEfiDevicePathProtocolGuid, &mDisplayDevicePath,
      &gEfiGraphicsOutputProtocolGuid, &mDisplay,
      &gEfiEdidDiscoveredProtocolGuid, &mEdidDiscovered,
      &gEfiEdidActiveProtocolGuid, &mEdidActive, NULL);

  ASSERT_EFI_ERROR(Status);

  return Status;
}
