#ifndef NOVUS_HID_H
#define NOVUS_HID_H

#include "uefi.h"

/* A HID report descriptor parser, small enough to live in the kernel and
 * generic enough that one implementation serves every pointing device the
 * system can meet: a USB mouse on the xHCI bus, and - once the I2C-HID
 * controller is up - a laptop touchpad, which speaks exactly the same
 * descriptor language. Nothing here is specific to a vendor or a model,
 * which is the point: the boot protocol is only a fallback for devices that
 * happen to implement it. */

#define HID_MAX_FIELDS  24
#define HID_MAX_REPORTS 8

typedef struct {
  UINT16 bit;          /* bit offset inside the report payload */
  UINT16 size;         /* width in bits */
  INT32  min;
  INT32  max;
  UINT8  usage;        /* usage id of the first field of this group */
  UINT8  page;         /* usage page */
  UINT8  relative;     /* input item requests relative values */
} hid_field_t;

typedef struct {
  UINT8       report_id;      /* 0 when the descriptor uses no report ids */
  UINT32      bits;           /* total payload bits of this report */
  UINT8       app_usage;      /* usage of the top-level application collection */
  UINT8       app_page;
  UINT8       mouse;          /* a Generic Desktop pointer, not a keyboard */
  UINT8       absolute;       /* X/Y are absolute rather than deltas */
  UINT32      x_range;        /* logical maximum of X when absolute */
  UINT32      y_range;        /* logical maximum of Y when absolute */
  UINT16      btn_bit;
  UINT8       btn_count;
  UINT16      x_bit;
  UINT16      x_size;
  INT32       x_min;
  INT32       x_max;
  UINT16      y_bit;
  UINT16      y_size;
  INT32       y_min;
  INT32       y_max;
  UINT16      wheel_bit;
  UINT16      wheel_size;
  INT32       wheel_min;
  INT32       wheel_max;
  UINT8       has_x;
  UINT8       has_y;
  UINT8       has_wheel;
  UINTN       fields_used;
  hid_field_t fields[HID_MAX_FIELDS];
} hid_layout_t;

/* Parse one report descriptor. Returns 1 when a pointer (mouse, touchpad,
 * pointing stick, touchscreen) was identified, 0 when the descriptor parsed
 * but describes something else, -1 when it could not be parsed at all. */
int hid_parse_report(const UINT8 *desc, UINT32 len, hid_layout_t *out);

/* Report length in bytes, including the report-id byte when present. */
UINT32 hid_report_bytes(const hid_layout_t *l);

/* Decode one pointer report into deltas (or absolute coordinates, see
 * l->absolute). Returns 0 when the report carried no pointer fields. */
int hid_decode_pointer(const hid_layout_t *l, const UINT8 *rep, UINT32 len,
                       INT32 *dx, INT32 *dy, INT32 *wheel, UINT8 *buttons);

#endif /* NOVUS_HID_H */
