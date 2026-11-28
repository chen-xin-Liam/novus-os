/** @file
  Novus BGRT-aware splash and kernel loader.

  Boot flow targeted by this application:

    SEC -> PEI -> DXE (Quiet Boot on: firmware blits the logo and fills BGRT)
      -> BDS loads NovusLoader.efi
        -> locate GOP, keep the current mode (never SetMode)
        -> read ACPI BGRT for the firmware logo X/Y and bitmap dimensions
        -> place the spinner just below that logo
        -> run the spinner cooperatively while the kernel image is loaded
        -> stop the spinner and hand over to Novus.

  The handover deliberately uses gBS->LoadImage()/StartImage() on NOVUS.EFI.
  Novus is a PE32+ UEFI application whose efi_main() owns the payload loading,
  memory-map capture, ExitBootServices() and the jump to kernel_entry(); see
  arch/x86_64/boot/main.c. Calling ExitBootServices() in this loader would
  make that entry unable to use boot services, and jumping to kernel_entry()
  from a different image would break its RIP-relative .bss/stack setup. So the
  loader stops its animation and starts the already-loaded image; the existing
  Novus boot stage then performs the final ExitBootServices() and jump.

  Spinner transparency is done in memory: GOP Blt() has no alpha operation.
  A small rectangle under the logo is read with EfiBltVideoToBltBuffer, the
  ring is alpha-composited into that snapshot, and only the rectangle is
  written back with EfiBltBufferToVideo. The saved pixels are restored when
  the spinner stops, so the firmware logo and background stay intact.

  This application never writes flash and does not disable Secure Boot.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#include "LogoPixels.h"

#pragma pack(1)
/* Sparse RGBA sprite pixel. Pixel.Reserved carries the source alpha; the
 * loader clears it when writing the blended result back to the GOP. */
typedef struct {
  UINT8 X;
  UINT8 Y;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel;
} NOVUS_SPRITE_PIXEL;

typedef struct {
  CONST NOVUS_SPRITE_PIXEL *Pixels;
  UINT16                   Count;
} NOVUS_SPRITE_FRAME;
#pragma pack()

#include "SpinnerFrames.h"

#define NOVUS_MIN_SPINS        2u
#define NOVUS_FRAME_STALL_US   16000u   // ~62.5 FPS
#define NOVUS_LOGO_GAP         24u
#define NOVUS_RSDP_GUID        { 0x8868E871, 0xE4F1, 0x11D3, \
                                 { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } }

#pragma pack(1)
typedef struct {
  UINT8  Signature[8];
  UINT8  Checksum;
  UINT8  OemId[6];
  UINT8  Revision;
  UINT32 RsdtAddress;
  UINT32 Length;
  UINT64 XsdtAddress;
  UINT8  ExtendedChecksum;
  UINT8  Reserved[3];
} NOVUS_RSDP;

typedef struct {
  UINT8  Signature[4];
  UINT32 Length;
  UINT8  Revision;
  UINT8  Checksum;
  UINT8  OemId[6];
  UINT8  OemTableId[8];
  UINT32 OemRevision;
  UINT32 CreatorId;
  UINT32 CreatorRevision;
} NOVUS_ACPI_HEADER;

typedef struct {
  NOVUS_ACPI_HEADER Header;
  UINT16 Version;
  UINT8  Status;
  UINT8  Reserved1;
  UINT8  ImageType;
  UINT8  Reserved2[3];
  UINT64 ImageAddress;
  UINT32 XOffset;
  UINT32 YOffset;
} NOVUS_BGRT;
#pragma pack()

typedef struct {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Background;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Frame;
  UINTN X;
  UINTN Y;
  UINTN Size;
  UINTN FrameIndex;
} NOVUS_SPINNER;

STATIC CONST EFI_GUID mAcpiRsdpGuid = NOVUS_RSDP_GUID;

// The standard ESP already carries the kernel as BOOTX64.EFI. Copying a
// second 3.4 MiB NOVUS.EFI into the 16 MiB install template exhausts FAT16
// clusters, so the loader uses the existing copy. IsSelfPath() skips the
// fallback when this application itself was launched from BOOTX64.EFI.
STATIC CONST CHAR16 *CONST mKernelPaths[] = {
  L"\\EFI\\BOOT\\NOVUS.EFI",
  L"\\NOVUS.EFI",
  L"\\EFI\\BOOT\\BOOTX64.EFI"
};

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  );

/**
  Blend one source pixel over the snapshot. Dst->Reserved is the GOP's
  unused byte; we clear it so the value written back is a plain video pixel.
**/
STATIC
VOID
BlendPixel (
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL       *Buffer,
  IN     UINTN                                Width,
  IN     UINTN                                Height,
  IN     INTN                                 X,
  IN     INTN                                 Y,
  IN     UINT8                               Alpha,
  IN     CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Source
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Dst;
  UINT16                        InvAlpha;

  if (Alpha == 0 || X < 0 || Y < 0 ||
      (UINTN)X >= Width || (UINTN)Y >= Height) {
    return;
  }
  Dst = &Buffer[(UINTN)Y * Width + (UINTN)X];
  if (Alpha == 255) {
    Dst->Blue = Source->Blue;
    Dst->Green = Source->Green;
    Dst->Red = Source->Red;
    Dst->Reserved = 0;
    return;
  }
  InvAlpha = (UINT16)(255u - Alpha);
  Dst->Blue = (UINT8)(((UINT16)Source->Blue * Alpha +
                       (UINT16)Dst->Blue * InvAlpha) / 255u);
  Dst->Green = (UINT8)(((UINT16)Source->Green * Alpha +
                        (UINT16)Dst->Green * InvAlpha) / 255u);
  Dst->Red = (UINT8)(((UINT16)Source->Red * Alpha +
                      (UINT16)Dst->Red * InvAlpha) / 255u);
  Dst->Reserved = 0;
}

STATIC
UINT32
ReadLe32 (
  IN CONST UINT8 *Bytes
  )
{
  return (UINT32)Bytes[0] | ((UINT32)Bytes[1] << 8) |
         ((UINT32)Bytes[2] << 16) | ((UINT32)Bytes[3] << 24);
}

STATIC
BOOLEAN
TableChecksumOk (
  IN CONST NOVUS_ACPI_HEADER *Header
  )
{
  CONST UINT8 *Bytes;
  UINTN        Index;
  UINT8        Sum;

  if (Header->Length < sizeof (NOVUS_ACPI_HEADER)) {
    return FALSE;
  }
  Bytes = (CONST UINT8 *)Header;
  Sum = 0;
  for (Index = 0; Index < Header->Length; Index++) {
    Sum = (UINT8)(Sum + Bytes[Index]);
  }
  return Sum == 0;
}

/**
  Find BGRT through the ACPI 2.0 RSDP configuration table. Returns NULL when
  the firmware did not report a boot graphics resource table.
**/
STATIC
CONST NOVUS_BGRT *
FindBgrt (
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  UINTN                       Index;
  CONST NOVUS_RSDP            *Rsdp;
  CONST NOVUS_ACPI_HEADER     *Table;
  UINTN                       EntryCount;
  UINTN                       EntryIndex;
  UINT64                      Address;
  CONST UINT8                 *Entries;

  if (SystemTable == NULL) {
    return NULL;
  }
  for (Index = 0; Index < SystemTable->NumberOfTableEntries; Index++) {
    if (CompareMem (&SystemTable->ConfigurationTable[Index].VendorGuid,
                    &mAcpiRsdpGuid, sizeof (EFI_GUID)) != 0) {
      continue;
    }
    Rsdp = (CONST NOVUS_RSDP *)SystemTable->ConfigurationTable[Index].VendorTable;
    if (Rsdp == NULL || Rsdp->Signature[0] != 'R' ||
        Rsdp->Signature[1] != 'S' || Rsdp->Signature[2] != 'D' ||
        Rsdp->Signature[3] != 'P') {
      continue;
    }
    if (Rsdp->Revision >= 2 && Rsdp->XsdtAddress != 0) {
      Table = (CONST NOVUS_ACPI_HEADER *)(UINTN)Rsdp->XsdtAddress;
      if (Table->Length <= sizeof (NOVUS_ACPI_HEADER)) {
        continue;
      }
      Entries = (CONST UINT8 *)Table + sizeof (NOVUS_ACPI_HEADER);
      EntryCount = (Table->Length - sizeof (NOVUS_ACPI_HEADER)) / 8u;
      for (EntryIndex = 0; EntryIndex < EntryCount; EntryIndex++) {
        Address = ReadLe32 (Entries + EntryIndex * 8u) |
                  ((UINT64)ReadLe32 (Entries + EntryIndex * 8u + 4u) << 32);
        Table = (CONST NOVUS_ACPI_HEADER *)(UINTN)Address;
        if (Table->Signature[0] == 'B' && Table->Signature[1] == 'G' &&
            Table->Signature[2] == 'R' && Table->Signature[3] == 'T' &&
            TableChecksumOk (Table)) {
          return (CONST NOVUS_BGRT *)Table;
        }
      }
    } else if (Rsdp->RsdtAddress != 0) {
      Table = (CONST NOVUS_ACPI_HEADER *)(UINTN)Rsdp->RsdtAddress;
      if (Table->Length <= sizeof (NOVUS_ACPI_HEADER)) {
        continue;
      }
      Entries = (CONST UINT8 *)Table + sizeof (NOVUS_ACPI_HEADER);
      EntryCount = (Table->Length - sizeof (NOVUS_ACPI_HEADER)) / 4u;
      for (EntryIndex = 0; EntryIndex < EntryCount; EntryIndex++) {
        Address = ReadLe32 (Entries + EntryIndex * 4u);
        Table = (CONST NOVUS_ACPI_HEADER *)(UINTN)Address;
        if (Table->Signature[0] == 'B' && Table->Signature[1] == 'G' &&
            Table->Signature[2] == 'R' && Table->Signature[3] == 'T' &&
            TableChecksumOk (Table)) {
          return (CONST NOVUS_BGRT *)Table;
        }
      }
    }
  }
  return NULL;
}

/**
  BGRT stores X/Y and the physical address of a BMP, not its dimensions.
  Read width/height from the BMP's BITMAPINFOHEADER.
**/
STATIC
BOOLEAN
BgrtLogoRect (
  IN  CONST NOVUS_BGRT *Bgrt,
  OUT UINTN            *X,
  OUT UINTN            *Y,
  OUT UINTN            *Width,
  OUT UINTN            *Height
  )
{
  CONST UINT8 *Bmp;
  UINT32       H;

  if (Bgrt == NULL || (Bgrt->Status & 1u) == 0 ||
      Bgrt->ImageAddress == 0 || Bgrt->Header.Length < sizeof (NOVUS_BGRT)) {
    return FALSE;
  }
  Bmp = (CONST UINT8 *)(UINTN)Bgrt->ImageAddress;
  if (Bmp[0] != 'B' || Bmp[1] != 'M') {
    return FALSE;
  }
  *Width = ReadLe32 (Bmp + 18);
  H = ReadLe32 (Bmp + 22);
  if (*Width == 0 || H == 0 || *Width > 4096 || H > 4096) {
    return FALSE;
  }
  *Height = (H & 0x80000000u) != 0 ? (UINT32)(0u - H) : H;
  *X = Bgrt->XOffset;
  *Y = Bgrt->YOffset;
  return TRUE;
}

STATIC
VOID
SpinnerStop (
  IN OUT NOVUS_SPINNER *Spin
  );

STATIC
EFI_STATUS
SpinnerStart (
  IN OUT NOVUS_SPINNER *Spin,
  IN     UINTN          CenterX,
  IN     UINTN          CenterY
  )
{
  UINTN ScreenW;
  UINTN ScreenH;

  if (Spin == NULL || Spin->Gop == NULL ||
      Spin->Gop->Mode == NULL || Spin->Gop->Mode->Info == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ScreenW = Spin->Gop->Mode->Info->HorizontalResolution;
  ScreenH = Spin->Gop->Mode->Info->VerticalResolution;
  Spin->Size = NOVUS_SPINNER_W;
  Spin->X = (CenterX > NOVUS_SPINNER_W / 2u)
                ? CenterX - NOVUS_SPINNER_W / 2u : 0;
  Spin->Y = (CenterY > NOVUS_SPINNER_H / 2u)
                ? CenterY - NOVUS_SPINNER_H / 2u : 0;
  if (Spin->X + Spin->Size > ScreenW) {
    Spin->X = (ScreenW > Spin->Size) ? ScreenW - Spin->Size : 0;
  }
  if (Spin->Y + Spin->Size > ScreenH) {
    Spin->Y = (ScreenH > Spin->Size) ? ScreenH - Spin->Size : 0;
  }
  Spin->Background = AllocatePool (Spin->Size * Spin->Size *
                                   sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  Spin->Frame = AllocatePool (Spin->Size * Spin->Size *
                              sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  if (Spin->Background == NULL || Spin->Frame == NULL) {
    SpinnerStop (Spin);
    return EFI_OUT_OF_RESOURCES;
  }
  return Spin->Gop->Blt (Spin->Gop, Spin->Background,
                         EfiBltVideoToBltBuffer, Spin->X, Spin->Y, 0, 0,
                         Spin->Size, Spin->Size,
                         Spin->Size * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
}

STATIC
VOID
SpinnerStop (
  IN OUT NOVUS_SPINNER *Spin
  )
{
  if (Spin == NULL) {
    return;
  }
  if (Spin->Background != NULL && Spin->Gop != NULL) {
    Spin->Gop->Blt (Spin->Gop, Spin->Background, EfiBltBufferToVideo,
                    0, 0, Spin->X, Spin->Y, Spin->Size, Spin->Size, 0);
    FreePool (Spin->Background);
    Spin->Background = NULL;
  }
  if (Spin->Frame != NULL) {
    FreePool (Spin->Frame);
    Spin->Frame = NULL;
  }
}

/**
  Draw one spinner frame and stall for a 16 ms tick. Called before each
  loader step so the animation keeps moving during cooperative init.
**/
STATIC
VOID
SpinnerTick (
  IN OUT NOVUS_SPINNER *Spin
  )
{
  UINTN                      Frame;
  UINTN                      Index;
  CONST NOVUS_SPRITE_PIXEL  *Pixel;

  if (Spin == NULL || Spin->Background == NULL || Spin->Frame == NULL) {
    return;
  }
  /* Restore the exact pixels captured at loader entry, then blend one
   * pre-rendered RGBA sprite over them. Nothing else touches this rect. */
  CopyMem (Spin->Frame, Spin->Background,
           Spin->Size * Spin->Size * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  Frame = Spin->FrameIndex % NOVUS_SPINNER_FRAMES;
  for (Index = 0; Index < mNovusSpinnerFrames[Frame].Count; Index++) {
    Pixel = &mNovusSpinnerFrames[Frame].Pixels[Index];
    BlendPixel (Spin->Frame, NOVUS_SPINNER_W, NOVUS_SPINNER_H,
                Pixel->X, Pixel->Y, Pixel->Pixel.Reserved, &Pixel->Pixel);
  }
  Spin->Gop->Blt (Spin->Gop, Spin->Frame, EfiBltBufferToVideo,
                  0, 0, Spin->X, Spin->Y, Spin->Size, Spin->Size, 0);
  Spin->FrameIndex++;
  gBS->Stall (NOVUS_FRAME_STALL_US);
}

/**
  Alpha-blend boot.png at an explicit rectangle. The logo is our resource,
  not the firmware's; BGRT is only used to choose where the firmware put its
  own mark, so our mark and the spinner feel like one boot animation.
**/
STATIC
VOID
DrawLogoAt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  IN UINTN                         X,
  IN UINTN                         Y,
  IN UINTN                         Width,
  IN UINTN                         Height
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels;
  UINTN Sx;
  UINTN Sy;
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Source;

  if (Width == 0 || Height == 0) {
    return;
  }
  Pixels = AllocatePool (Width * Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  if (Pixels == NULL) {
    return;
  }
  if (EFI_ERROR (Gop->Blt (Gop, Pixels, EfiBltVideoToBltBuffer, X, Y, 0, 0,
                           Width, Height,
                           Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)))) {
    FreePool (Pixels);
    return;
  }
  for (Sy = 0; Sy < Height; Sy++) {
    for (Sx = 0; Sx < Width; Sx++) {
      Source = &mNovusEfiLogo[((Sy * NOVUS_EFI_LOGO_HEIGHT) / Height) *
                              NOVUS_EFI_LOGO_WIDTH +
                              ((Sx * NOVUS_EFI_LOGO_WIDTH) / Width)];
      BlendPixel (Pixels, Width, Height, (INTN)Sx, (INTN)Sy,
                  Source->Reserved, Source);
    }
  }
  Gop->Blt (Gop, Pixels, EfiBltBufferToVideo, 0, 0, X, Y, Width, Height, 0);
  FreePool (Pixels);
}

/**
  Never chain-load ourselves: if the loader was launched from BOOTX64.EFI,
  the fallback path would otherwise create a reboot loop.
**/
STATIC
BOOLEAN
IsSelfPath (
  IN EFI_HANDLE                  ImageHandle,
  IN CONST EFI_DEVICE_PATH_PROTOCOL *Candidate
  )
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;

  if (Candidate == NULL ||
      gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid,
                           (VOID **)&Loaded) != EFI_SUCCESS ||
      Loaded->FilePath == NULL) {
    return FALSE;
  }
  return CompareDevicePath (Loaded->FilePath, Candidate) == 0;
}

/**
  Load NOVUS.EFI from the ESP and start it. Each filesystem probe and the
  image load yield a spinner frame; after StartImage() the Novus boot stage
  owns the screen, payload loading and ExitBootServices().
**/
STATIC
EFI_STATUS
LoadNovusKernel (
  IN EFI_HANDLE         ImageHandle,
  IN OUT NOVUS_SPINNER *Spin
  )
{
  EFI_HANDLE                      *Handles;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL               *Root;
  EFI_FILE_PROTOCOL               *File;
  EFI_DEVICE_PATH_PROTOCOL        *Path;
  EFI_HANDLE                      ChildImage;
  EFI_LOADED_IMAGE_PROTOCOL       *ChildLoaded;
  UINTN                           ExitDataSize;
  CHAR16                          *ExitData;
  EFI_STATUS                      Status;
  UINTN                           Count;
  UINTN                           HandleIndex;
  UINTN                           PathIndex;
  UINTN                           Tick;

  Status = gBS->LocateHandleBuffer (ByProtocol,
                                   &gEfiSimpleFileSystemProtocolGuid,
                                   NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  SpinnerTick (Spin);

  for (PathIndex = 0; PathIndex < sizeof (mKernelPaths) / sizeof (mKernelPaths[0]);
       PathIndex++) {
    for (HandleIndex = 0; HandleIndex < Count; HandleIndex++) {
      SpinnerTick (Spin);
      Status = gBS->HandleProtocol (Handles[HandleIndex],
                                    &gEfiSimpleFileSystemProtocolGuid,
                                    (VOID **)&Fs);
      if (EFI_ERROR (Status)) {
        continue;
      }
      Status = Fs->OpenVolume (Fs, &Root);
      if (EFI_ERROR (Status)) {
        continue;
      }
      Status = Root->Open (Root, &File, mKernelPaths[PathIndex],
                           EFI_FILE_MODE_READ, 0);
      if (EFI_ERROR (Status)) {
        Root->Close (Root);
        continue;
      }
      File->Close (File);
      Root->Close (Root);

      Path = FileDevicePath (Handles[HandleIndex], mKernelPaths[PathIndex]);
      if (Path == NULL) {
        FreePool (Handles);
        return EFI_OUT_OF_RESOURCES;
      }
      if (IsSelfPath (ImageHandle, Path)) {
        FreePool (Path);
        continue;
      }
      ChildImage = NULL;
      Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &ChildImage);
      FreePool (Path);
      if (EFI_ERROR (Status)) {
        FreePool (Handles);
        return Status;
      }
      /* Boot options may launch this application with FilePath == NULL, so
       * the device-path check above can miss a self-load. The entry point is
       * unambiguous: if the child is us, unload it instead of recursing. */
      ChildLoaded = NULL;
      if (gBS->HandleProtocol (ChildImage, &gEfiLoadedImageProtocolGuid,
                               (VOID **)&ChildLoaded) == EFI_SUCCESS &&
          ChildLoaded != NULL && ChildLoaded->EntryPoint == UefiMain) {
        gBS->UnloadImage (ChildImage);
        FreePool (Handles);
        return EFI_NOT_FOUND;
      }
      // The firmware's PE loader reads and relocates the kernel here.
      for (Tick = 0; Tick < NOVUS_MIN_SPINS; Tick++) {
        SpinnerTick (Spin);
      }
      SpinnerStop (Spin);
      FreePool (Handles);

      // NOVUS.EFI's efi_main() loads SYSTEM.IMG, captures the memory map,
      // calls ExitBootServices() and enters kernel_entry().
      ExitDataSize = 0;
      ExitData = NULL;
      return gBS->StartImage (ChildImage, &ExitDataSize, &ExitData);
    }
  }
  FreePool (Handles);
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  CONST NOVUS_BGRT             *Bgrt;
  NOVUS_SPINNER                 Spin;
  EFI_STATUS                    Status;
  UINTN                         ScreenW;
  UINTN                         ScreenH;
  UINTN                         LogoX;
  UINTN                         LogoY;
  UINTN                         LogoW;
  UINTN                         LogoH;
  UINTN                         SpinX;
  UINTN                         SpinY;
  UINTN                         Tick;

  ZeroMem (&Spin, sizeof (Spin));

  // Keep whatever mode the firmware/Quiet Boot selected. SetMode() would
  // change the panel timing and move the logo BGRT describes.
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                (VOID **)&Gop);
  if (EFI_ERROR (Status) || Gop == NULL || Gop->Mode == NULL ||
      Gop->Mode->Info == NULL) {
    return LoadNovusKernel (ImageHandle, &Spin);
  }
  Spin.Gop = Gop;

  ScreenW = Gop->Mode->Info->HorizontalResolution;
  ScreenH = Gop->Mode->Info->VerticalResolution;
  Bgrt = FindBgrt (SystemTable);
  if (Bgrt != NULL && BgrtLogoRect (Bgrt, &LogoX, &LogoY, &LogoW, &LogoH)) {
    /* Use the firmware's mark position so our logo replaces it in place. */
    if (LogoX >= ScreenW) {
      LogoX = 0;
    }
    if (LogoY >= ScreenH) {
      LogoY = ScreenH / 6u;
    }
    if (LogoW == 0 || LogoW > ScreenW - LogoX) {
      LogoW = ScreenW - LogoX;
    }
    if (LogoH == 0 || LogoH > ScreenH - LogoY) {
      LogoH = ScreenH - LogoY;
    }
  } else {
    LogoW = ScreenW / 3u;
    if (LogoW > 480) {
      LogoW = 480;
    }
    LogoH = (LogoW * NOVUS_EFI_LOGO_HEIGHT) / NOVUS_EFI_LOGO_WIDTH;
    if (LogoH > ScreenH / 2u) {
      LogoH = ScreenH / 2u;
      LogoW = (LogoH * NOVUS_EFI_LOGO_WIDTH) / NOVUS_EFI_LOGO_HEIGHT;
    }
    LogoX = (ScreenW - LogoW) / 2u;
    LogoY = ScreenH / 6u;
  }

  /* boot.png is our resource: alpha-blend it over whatever the firmware left
   * on the panel, then place the spinner directly below it. */
  DrawLogoAt (Gop, LogoX, LogoY, LogoW, LogoH);
  SpinX = LogoX + LogoW / 2u;
  SpinY = LogoY + LogoH + NOVUS_LOGO_GAP;

  Status = SpinnerStart (&Spin, SpinX, SpinY);
  if (EFI_ERROR (Status)) {
    return LoadNovusKernel (ImageHandle, &Spin);
  }
  for (Tick = 0; Tick < NOVUS_MIN_SPINS; Tick++) {
    SpinnerTick (&Spin);
  }
  return LoadNovusKernel (ImageHandle, &Spin);
}
