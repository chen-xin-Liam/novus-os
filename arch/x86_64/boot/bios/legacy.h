#ifndef NOVUS_BIOS_LEGACY_H
#define NOVUS_BIOS_LEGACY_H

#include "boot_layout.h"
#include "uefi.h"

typedef struct {
  UINT64 base;
  UINT64 length;
  UINT32 type;
  UINT32 attributes;
} bios_e820_entry_t;

typedef struct {
  UINT32 magic;
  UINT32 version;
  UINT32 e820_count;
  UINT32 e820_entry_size;
  UINT64 e820_addr;
  UINT32 vbe_available;
  UINT32 width;
  UINT32 height;
  UINT32 pitch_bytes;
  UINT16 bpp;
  UINT8 red_size;
  UINT8 red_pos;
  UINT8 green_size;
  UINT8 green_pos;
  UINT8 blue_size;
  UINT8 blue_pos;
  UINT64 fb_base;
  UINT64 fb_size;
  UINT16 vbe_mode;
} bios_boot_info_t;

_Static_assert(__builtin_offsetof(bios_boot_info_t, e820_addr) ==
                   BIOS_BI_E820_ADDR,
               "BIOS boot info E820 pointer offset drift");
_Static_assert(__builtin_offsetof(bios_boot_info_t, fb_base) ==
                   BIOS_BI_FB_BASE,
               "BIOS boot info framebuffer offset drift");
_Static_assert(__builtin_offsetof(bios_boot_info_t, vbe_mode) ==
                   BIOS_BI_VBE_MODE,
               "BIOS boot info VBE mode offset drift");
_Static_assert(sizeof(bios_e820_entry_t) == 24,
               "BIOS E820 entry layout must be 24 bytes");

void legacy_main(void);

#endif