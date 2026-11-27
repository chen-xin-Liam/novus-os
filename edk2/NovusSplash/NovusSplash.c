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

#define NOVUS_SPINNER_FRAMES   24u
#define NOVUS_SPINNER_SAMPLES  64u
#define NOVUS_SPINNER_RADIUS   36
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
  INT16 X;
  INT16 Y;
  UINT8 Alpha;
} NOVUS_SPINNER_POINT;

typedef struct {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Background;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Frame;
  UINTN X;
  UINTN Y;
  UINTN Size;
  UINTN Center;
  UINTN FrameIndex;
} NOVUS_SPINNER;

STATIC NOVUS_SPINNER_POINT mSpinnerPoints[NOVUS_SPINNER_FRAMES][NOVUS_SPINNER_SAMPLES];
STATIC UINTN                mSpinnerCount[NOVUS_SPINNER_FRAMES];

// sin(0..90 degrees) scaled by 256, one entry every 5.625 degrees.
STATIC CONST INT16 mSinQuarter[17] = {
  0, 25, 50, 74, 98, 121, 142, 161, 181,
  198, 213, 226, 237, 245, 250, 254, 256
};

STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSpinnerColor = {
  0x2C, 0x93, 0xFF, 0xFF   // Blue, Green, Red, Reserved (=alpha for our source)
};

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

/**
  A 3x3 dab keeps the ring continuous on high-resolution panels.
**/
STATIC
VOID
BlendDot (
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL       *Buffer,
  IN     UINTN                                Width,
  IN     UINTN                                Height,
  IN     INTN                                 CenterX,
  IN     INTN                                 CenterY,
  IN     UINT8                               Alpha,
  IN     CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color
  )
{
  INTN  Dx;
  INTN  Dy;
  UINT8 EdgeAlpha;

  EdgeAlpha = (UINT8)(Alpha / 3u);
  for (Dy = -1; Dy <= 1; Dy++) {
    for (Dx = -1; Dx <= 1; Dx++) {
      BlendPixel (Buffer, Width, Height, CenterX + Dx, CenterY + Dy,
                  (Dx == 0 && Dy == 0) ? Alpha : EdgeAlpha, Color);
    }
  }
}

STATIC
VOID
CirclePoint (
  IN  UINTN Index,
  OUT INT16 *X,
  OUT INT16 *Y
  )
{
  UINTN Quadrant;
  UINTN Step;
  INT16 S;
  INT16 C;

  Quadrant = Index >> 4;
  Step = Index & 15u;
  S = mSinQuarter[Step];
  C = mSinQuarter[16u - Step];
  switch (Quadrant) {
  case 0:
    *X = C;  *Y = S;  break;
  case 1:
    *X = (INT16)-S;  *Y = C;  break;
  case 2:
    *X = (INT16)-C;  *Y = (INT16)-S;  break;
  default:
    *X = S;  *Y = (INT16)-C;  break;
  }
}

/**
  Pre-generate all spinner frames before the animation loop starts.
**/
STATIC
VOID
GenerateSpinner (
  VOID
  )
{
  UINTN Frame;
  UINTN Sample;
  INTN  Phase;
  UINT8 Alpha;
  INT16 UnitX;
  INT16 UnitY;

  for (Frame = 0; Frame < NOVUS_SPINNER_FRAMES; Frame++) {
    mSpinnerCount[Frame] = 0;
    for (Sample = 0; Sample < NOVUS_SPINNER_SAMPLES; Sample++) {
      Phase = (INTN)((Sample * 256u) / NOVUS_SPINNER_SAMPLES) -
              (INTN)((Frame * 256u) / NOVUS_SPINNER_FRAMES);
      while (Phase < 0) {
        Phase += 256;
      }
      if (Phase > 112) {
        continue;
      }
      Alpha = (UINT8)(255u - (UINTN)(Phase * 255) / 112u);
      if (Alpha == 0) {
        continue;
      }
      CirclePoint (Sample, &UnitX, &UnitY);
      mSpinnerPoints[Frame][mSpinnerCount[Frame]].X =
        (INT16)((UnitX * NOVUS_SPINNER_RADIUS) / 256);
      mSpinnerPoints[Frame][mSpinnerCount[Frame]].Y =
        (INT16)((UnitY * NOVUS_SPINNER_RADIUS) / 256);
      mSpinnerPoints[Frame][mSpinnerCount[Frame]].Alpha = Alpha;
      mSpinnerCount[Frame]++;
    }
  }
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
  Spin->Size = (UINTN)(NOVUS_SPINNER_RADIUS + 3) * 2u + 1u;
  Spin->Center = (UINTN)(NOVUS_SPINNER_RADIUS + 3);
  Spin->X = (CenterX > Spin->Center) ? CenterX - Spin->Center : 0;
  Spin->Y = (CenterY > Spin->Center) ? CenterY - Spin->Center : 0;
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
  UINTN Frame;
  UINTN Point;

  if (Spin == NULL || Spin->Background == NULL || Spin->Frame == NULL) {
    return;
  }
  CopyMem (Spin->Frame, Spin->Background,
           Spin->Size * Spin->Size * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  Frame = Spin->FrameIndex % NOVUS_SPINNER_FRAMES;
  for (Point = 0; Point < mSpinnerCount[Frame]; Point++) {
    BlendDot (Spin->Frame, Spin->Size, Spin->Size,
              (INTN)Spin->Center + mSpinnerPoints[Frame][Point].X,
              (INTN)Spin->Center + mSpinnerPoints[Frame][Point].Y,
              mSpinnerPoints[Frame][Point].Alpha, &mSpinnerColor);
  }
  Spin->Gop->Blt (Spin->Gop, Spin->Frame, EfiBltBufferToVideo,
                  0, 0, Spin->X, Spin->Y, Spin->Size, Spin->Size, 0);
  Spin->FrameIndex++;
  gBS->Stall (NOVUS_FRAME_STALL_US);
}

/**
  Draw the baked logo only when the firmware did not report one through BGRT.
**/
STATIC
VOID
DrawFallbackLogo (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels;
  UINTN ScreenW;
  UINTN ScreenH;
  UINTN Width;
  UINTN Height;
  UINTN X;
  UINTN Y;
  UINTN Sx;
  UINTN Sy;
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Source;

  ScreenW = Gop->Mode->Info->HorizontalResolution;
  ScreenH = Gop->Mode->Info->VerticalResolution;
  Width = ScreenW / 3;
  if (Width > 480) {
    Width = 480;
  }
  Height = (Width * NOVUS_EFI_LOGO_HEIGHT) / NOVUS_EFI_LOGO_WIDTH;
  if (Height > ScreenH / 2) {
    Height = ScreenH / 2;
    Width = (Height * NOVUS_EFI_LOGO_WIDTH) / NOVUS_EFI_LOGO_HEIGHT;
  }
  X = (ScreenW - Width) / 2;
  Y = ScreenH / 6;
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
  UINTN                         LogoX;
  UINTN                         LogoY;
  UINTN                         LogoW;
  UINTN                         LogoH;
  UINTN                         SpinX;
  UINTN                         SpinY;
  UINTN                         Tick;

  GenerateSpinner ();
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

  Bgrt = FindBgrt (SystemTable);
  if (Bgrt != NULL && BgrtLogoRect (Bgrt, &LogoX, &LogoY, &LogoW, &LogoH)) {
    SpinX = LogoX + LogoW / 2u;
    SpinY = LogoY + LogoH + NOVUS_LOGO_GAP;
  } else {
    DrawFallbackLogo (Gop);
    LogoW = Gop->Mode->Info->HorizontalResolution / 3;
    if (LogoW > 480) {
      LogoW = 480;
    }
    LogoH = (LogoW * NOVUS_EFI_LOGO_HEIGHT) / NOVUS_EFI_LOGO_WIDTH;
    SpinX = (Gop->Mode->Info->HorizontalResolution - LogoW) / 2u;
    SpinY = Gop->Mode->Info->VerticalResolution / 6u + LogoH +
            NOVUS_LOGO_GAP;
  }
  Status = SpinnerStart (&Spin, SpinX, SpinY);
  if (EFI_ERROR (Status)) {
    return LoadNovusKernel (ImageHandle, &Spin);
  }
  for (Tick = 0; Tick < NOVUS_MIN_SPINS; Tick++) {
    SpinnerTick (&Spin);
  }
  return LoadNovusKernel (ImageHandle, &Spin);
}
