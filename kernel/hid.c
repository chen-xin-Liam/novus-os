/**
 * Novus OS - generic HID report descriptor parser.
 *
 * One parser for every pointing device: USB mice and, through the I2C-HID
 * path, laptop touchpads. The descriptor is a small stack language (global
 * items set state, local items describe the next main item); this reads it
 * the way the HID spec defines it instead of assuming a fixed layout, so a
 * device that reports relative deltas, absolute coordinates, extra buttons
 * or a wheel all land on the same decoder.
 *
 * Item prefix layout (HID 1.11, 6.2.2.2): bits 7..4 tag, bits 3..2 type,
 * bits 1..0 size, where type 0 is a main item (Input/Output/Collection),
 * type 1 is global and type 2 is local. Getting that mapping backwards is
 * the single mistake that makes every descriptor parse as empty.
 */

#include "hid.h"
#include "kernel.h"
#include <stddef.h>

/* Usage pages and usages this file cares about. */
#define HID_PAGE_GENERIC 0x01u
#define HID_USAGE_POINTER 0x01u
#define HID_USAGE_MOUSE   0x02u
#define HID_USAGE_KEYBOARD 0x06u
#define HID_USAGE_X       0x30u
#define HID_USAGE_Y       0x31u
#define HID_USAGE_WHEEL   0x38u
#define HID_PAGE_BUTTON   0x09u

#define HID_MAX_USAGES 8

typedef struct {
  UINT32 usage_page;
  INT32  logical_min;
  INT32  logical_max;
  UINT32 report_size;
  UINT32 report_count;
  UINT8  report_id;
} hid_global_t;

static UINT32 item_value(const UINT8 *data, UINT32 size) {
  UINT32 value = 0;

  if (size > 4u) {
    size = 4u;
  }
  memcpy(&value, data, size);
  return value;
}

static INT32 item_signed(const UINT8 *data, UINT32 size) {
  UINT32 value = item_value(data, size);
  INT32  signed_value = (INT32)value;

  if (size == 1u && (value & 0x80u) != 0u) {
    signed_value = (INT32)(value | 0xFFFFFF00u);
  } else if (size == 2u && (value & 0x8000u) != 0u) {
    signed_value = (INT32)(value | 0xFFFF0000u);
  }
  return signed_value;
}

static void add_field(hid_layout_t *out, UINT32 bit, UINT32 size, INT32 min,
                      INT32 max, UINT8 page, UINT8 usage, UINT8 relative) {
  hid_field_t *f;

  if (out->fields_used >= HID_MAX_FIELDS) {
    return;
  }
  f = &out->fields[out->fields_used++];
  f->bit = (UINT16)bit;
  f->size = (UINT16)size;
  f->min = min;
  f->max = max;
  f->page = page;
  f->usage = usage;
  f->relative = relative;
}

int hid_parse_report(const UINT8 *desc, UINT32 len, hid_layout_t *out) {
  hid_global_t g;
  UINT32 report_bits[HID_MAX_REPORTS];
  UINT8  usage_list[HID_MAX_USAGES];
  UINT32 usage_count = 0;
  UINT8  usage_min = 0;
  UINT8  usage_min_set = 0;
  UINT8  app_page = 0;
  UINT8  app_usage = 0;
  UINT8  pointer_report = 0;
  UINT32 pos = 0;
  UINT32 i;

  if (desc == NULL || out == NULL || len < 4u) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  memset(&g, 0, sizeof(g));
  for (i = 0; i < HID_MAX_REPORTS; i++) {
    report_bits[i] = 0;
  }

  while (pos < len) {
    UINT8        prefix = desc[pos];
    UINT32       size;
    UINT32       type;
    UINT32       tag;
    const UINT8 *data;

    if (prefix == 0xFEu) {          /* long item: skip it whole */
      if (pos + 3u > len) {
        break;
      }
      size = desc[pos + 1u];
      pos += 3u + size;
      continue;
    }
    size = (UINT32)(prefix & 0x03u);
    if (size == 3u) {
      size = 4u;
    }
    type = (prefix >> 2) & 0x03u;
    tag = prefix >> 4;
    if (pos + 1u + size > len) {
      break;
    }
    data = desc + pos + 1u;
    pos += 1u + size;

    if (type == 1u) {               /* global */
      switch (tag) {
      case 0u: g.usage_page = item_value(data, size); break;
      case 1u: g.logical_min = item_signed(data, size); break;
      case 2u: g.logical_max = item_signed(data, size); break;
      case 7u: g.report_size = item_value(data, size); break;
      case 8u: g.report_id = (UINT8)item_value(data, size); break;
      case 9u: g.report_count = item_value(data, size); break;
      default: break;
      }
    } else if (type == 2u) {        /* local: usages for the next main item */
      if (tag == 0u) {              /* usage */
        if (usage_count < HID_MAX_USAGES) {
          usage_list[usage_count++] = (UINT8)(item_value(data, size) & 0xFFu);
        }
      } else if (tag == 1u) {       /* usage minimum */
        usage_min = (UINT8)(item_value(data, size) & 0xFFu);
        usage_min_set = 1u;
      }
      /* usage maximum (tag 2) is implied by the report count. */
    } else {                        /* main */
      if (tag == 8u) {              /* input */
        UINT32 count = g.report_count;
        UINT32 rsize = g.report_size;
        UINT32 flags = item_value(data, size);
        UINT32 index = (g.report_id < HID_MAX_REPORTS) ? g.report_id : 0u;
        UINT32 j;

        if (rsize != 0u && rsize <= 32u) {
          for (j = 0; j < count; j++) {
            UINT32 bit = report_bits[index];
            UINT8  usage;
            UINT8  relative = (UINT8)((flags >> 2) & 1u);

            if (j < usage_count) {
              usage = usage_list[j];
            } else if (usage_min_set != 0u) {
              usage = (UINT8)(usage_min + j);
            } else if (usage_count != 0u) {
              usage = usage_list[usage_count - 1u];
            } else {
              usage = 0u;
            }

            if (g.usage_page == HID_PAGE_BUTTON && out->btn_count == 0u) {
              out->btn_bit = (UINT16)bit;
              out->btn_count = (UINT8)(count > 8u ? 8u : count);
            }
            if (g.usage_page == HID_PAGE_GENERIC) {
              if (usage == HID_USAGE_X && out->has_x == 0u) {
                out->x_bit = (UINT16)bit;
                out->x_size = (UINT16)rsize;
                out->x_min = g.logical_min;
                out->x_max = g.logical_max;
                out->has_x = 1;
              } else if (usage == HID_USAGE_Y && out->has_y == 0u) {
                out->y_bit = (UINT16)bit;
                out->y_size = (UINT16)rsize;
                out->y_min = g.logical_min;
                out->y_max = g.logical_max;
                out->has_y = 1;
              } else if (usage == HID_USAGE_WHEEL && out->has_wheel == 0u) {
                out->wheel_bit = (UINT16)bit;
                out->wheel_size = (UINT16)rsize;
                out->wheel_min = g.logical_min;
                out->wheel_max = g.logical_max;
                out->has_wheel = 1;
              }
            }
            add_field(out, bit, rsize, g.logical_min, g.logical_max,
                      (UINT8)g.usage_page, usage, relative);
            report_bits[index] += rsize;
          }
        }
        usage_count = 0;
        usage_min_set = 0;
      } else if (tag == 10u) {      /* collection */
        UINT32 kind = item_value(data, size);
        UINT8  primary;

        if (usage_count != 0u) {
          primary = usage_list[0];
        } else if (usage_min_set != 0u) {
          primary = usage_min;
        } else {
          primary = 0u;
        }
        if ((kind & 0xFFu) == 1u && app_usage == 0u) {
          app_usage = primary;
          app_page = (UINT8)g.usage_page;
          pointer_report = g.report_id;
        }
        usage_count = 0;
        usage_min_set = 0;
      } else {
        usage_count = 0;
        usage_min_set = 0;
      }
    }
  }

  out->app_usage = app_usage;
  out->app_page = app_page;
  out->report_id = pointer_report;
  if (out->report_id < HID_MAX_REPORTS) {
    out->bits = report_bits[out->report_id];
  }

  if (out->app_page == HID_PAGE_GENERIC &&
      (out->app_usage == HID_USAGE_MOUSE ||
       out->app_usage == HID_USAGE_POINTER)) {
    out->mouse = 1;
  }
  if (out->has_x != 0u && out->has_y != 0u) {
    UINT32 k;

    out->mouse = 1;
    out->x_range = (out->x_max > out->x_min)
                       ? (UINT32)(out->x_max - out->x_min) : 0u;
    out->y_range = (out->y_max > out->y_min)
                       ? (UINT32)(out->y_max - out->y_min) : 0u;
    out->absolute = 1;
    for (k = 0; k < out->fields_used; k++) {
      if (out->fields[k].page == HID_PAGE_GENERIC &&
          (out->fields[k].usage == HID_USAGE_X ||
           out->fields[k].usage == HID_USAGE_Y) &&
          out->fields[k].relative != 0u) {
        out->absolute = 0;
        break;
      }
    }
  }
  return out->mouse ? 1 : 0;
}

UINT32 hid_report_bytes(const hid_layout_t *l) {
  UINT32 bytes;

  if (l == NULL || l->bits == 0u) {
    return 0u;
  }
  bytes = (l->bits + 7u) / 8u;
  if (l->report_id != 0u) {
    bytes++;
  }
  return bytes;
}

static UINT32 report_field(const UINT8 *rep, UINT32 len, UINT16 bit,
                           UINT16 size) {
  UINT32 value = 0;
  UINT16 i;

  for (i = 0; i < size; i++) {
    UINT16 at = (UINT16)(bit + i);
    UINT32 byte = at / 8u;
    UINT32 shift = at % 8u;

    if (byte >= len) {
      break;
    }
    if ((rep[byte] & (1u << shift)) != 0u) {
      value |= (1u << i);
    }
  }
  return value;
}

static INT32 report_signed(const UINT8 *rep, UINT32 len, UINT16 bit,
                           UINT16 size, INT32 min) {
  UINT32 raw = report_field(rep, len, bit, size);

  if (min < 0 && size != 0u && size < 32u &&
      (raw & (1u << (size - 1u))) != 0u) {
    raw |= ~((1u << size) - 1u);
  }
  return (INT32)raw;
}

int hid_decode_pointer(const hid_layout_t *l, const UINT8 *rep, UINT32 len,
                       INT32 *dx, INT32 *dy, INT32 *wheel, UINT8 *buttons) {
  UINT32 skip = 0;

  if (l == NULL || !l->mouse || rep == NULL) {
    return 0;
  }
  if (l->report_id != 0u) {
    if (len < 1u || rep[0] != l->report_id) {
      return 0;
    }
    skip = 1u;
  }
  if (len <= skip) {
    return 0;
  }
  rep += skip;
  len -= skip;

  if (dx != NULL && l->has_x != 0u) {
    *dx = report_signed(rep, len, l->x_bit, l->x_size, l->x_min);
  }
  if (dy != NULL && l->has_y != 0u) {
    *dy = report_signed(rep, len, l->y_bit, l->y_size, l->y_min);
  }
  if (wheel != NULL && l->has_wheel != 0u) {
    *wheel = report_signed(rep, len, l->wheel_bit, l->wheel_size,
                           l->wheel_min);
  }
  if (buttons != NULL) {
    UINT32 raw = report_field(rep, len, l->btn_bit, (UINT16)l->btn_count);
    UINT32 bits = (l->btn_count > 3u) ? 3u : l->btn_count;
    UINT32 mask = (bits == 0u) ? 0u : ((1u << bits) - 1u);

    *buttons = (UINT8)(raw & mask);
  }
  return 1;
}
