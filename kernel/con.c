/**
 * Novus OS - kernel text console (M11).
 *
 * A WM-free framebuffer terminal. It renders text with the same NovusDisplay
 * primitives the boot splash and desktop use, and reads the keyboard from the
 * shared input queue (PS/2 or USB HID). nsh reaches it through SYS_CONSOLE, so
 * the shell engine does not change between the serial and the on-screen path.
 *
 * The model is a classic scrolling terminal: a ring of logical rows, a cursor
 * that always sits at the end of the current row, and a handful of control
 * codes (\n, \r, \b). Backspace is handled by dropping the previous UTF-8
 * character, which keeps CJK input correct under nsh's "\b \b" echo.
 */
#include "kernel.h"
#include "nd.h"
#include "nd_input.h"
#include "serial.h"
#include "con.h"

#define CON_MAX_ROWS 300u
#define CON_ROW_BYTES 256u
#define CON_IN_CAP 128u

static nd_surface_t *g_s;
static char          g_rows[CON_MAX_ROWS][CON_ROW_BYTES];
static int           g_nrows;
static int           g_cell_w = 6;
static int           g_cell_h = 16;
static int           g_rows_vis = 1;

/* ANSI escape state, so nsh's clear screen and the welcome banner do not print
 * raw escape bytes. ESC [ 2 J clears; other sequences are skipped. */
static int g_esc;
static int g_esc_param;

static UINT8 g_in[CON_IN_CAP];
static UINT32 g_in_n;

static UINT32 con_strlen(const char *p) {
  UINT32 n = 0u;
  while (p[n] != '\0') {
    n++;
  }
  return n;
}

static void con_memmove(UINT8 *d, const UINT8 *s, UINT32 n) {
  if (d == s || n == 0u) {
    return;
  }
  if (d < s) {
    for (UINT32 i = 0u; i < n; i++) {
      d[i] = s[i];
    }
  } else {
    for (UINT32 i = n; i > 0u; i--) {
      d[i - 1u] = s[i - 1u];
    }
  }
}

void con_init(nd_surface_t *s) {
  g_s = s;
  g_nrows = 1;
  g_rows[0][0] = '\0';
  g_in_n = 0u;
  if (s != 0) {
    g_cell_w = (int)nd_text_width("A", 1u);
    if (g_cell_w < 1) {
      g_cell_w = 6;
    }
    g_cell_h = 16;
    g_rows_vis = (int)(s->h / g_cell_h);
    if (g_rows_vis < 1) {
      g_rows_vis = 1;
    }
    nd_clear(s, ND_RGB(0, 0, 0));
  }
}

int con_ready(void) {
  return (g_s != 0) ? 1 : 0;
}

static void append_byte(UINT8 b) {
  char  *row = g_rows[g_nrows - 1];
  UINT32 l = con_strlen(row);
  if (l + 1u < CON_ROW_BYTES) {
    row[l] = (char)b;
    row[l + 1u] = '\0';
  }
}

static void newline(void) {
  if ((UINT32)g_nrows < CON_MAX_ROWS) {
    g_nrows++;
    g_rows[g_nrows - 1][0] = '\0';
  } else {
    con_memmove((UINT8 *)g_rows[0], (const UINT8 *)g_rows[1],
                (CON_MAX_ROWS - 1u) * CON_ROW_BYTES);
    g_rows[CON_MAX_ROWS - 1u][0] = '\0';
  }
}

/* Drop the previous whole UTF-8 character from the current row. */
static void pop_last(void) {
  char  *row = g_rows[g_nrows - 1];
  UINT32 l = con_strlen(row);
  UINT32 i;
  if (l == 0u) {
    return;
  }
  i = l - 1u;
  while (i > 0u && (((UINT8)row[i]) & 0xC0u) == 0x80u) {
    i--;
  }
  row[i] = '\0';
}

static void repaint(void) {
  int start = 0;
  int r;

  nd_clear(g_s, ND_RGB(0, 0, 0));
  if (g_nrows > g_rows_vis) {
    start = g_nrows - g_rows_vis;
  }
  for (r = start; r < g_nrows; r++) {
    int y = (r - start) * g_cell_h;
    nd_text(g_s, 0, y, g_rows[r], ND_RGB(0xD8, 0xD8, 0xD8), 1u);
  }
  {
    int cy = (g_nrows - 1 - start) * g_cell_h;
    int cx = (int)nd_text_width(g_rows[g_nrows - 1], 1u);
    nd_fill_rect(g_s, nd_rect(cx, cy, g_cell_w, g_cell_h),
                 ND_RGB(0xD8, 0xD8, 0xD8));
  }
}

void con_out(const UINT8 *buf, UINT32 len) {
  UINT32 i;
  for (i = 0u; i < len; i++) {
    UINT8 b = buf[i];

    if (g_esc != 0) {
      if (g_esc == 1) {
        if (b == '[') {
          g_esc = 2;
        } else {
          g_esc = 0;
          g_esc_param = 0;
        }
        continue;
      }
      /* g_esc == 2: inside ESC [ ... */
      if (b >= '0' && b <= '9') {
        if (b == '2') {
          g_esc_param = 1;
        }
      } else if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')) {
        if (g_esc_param != 0 && b == 'J') {
          for (UINT32 r = 0u; r < CON_MAX_ROWS; r++) {
            g_rows[r][0] = '\0';
          }
          g_nrows = 1;
        }
        g_esc = 0;
        g_esc_param = 0;
      } else {
        g_esc = 0;
        g_esc_param = 0;
      }
      continue;
    }
    if (b == 0x1Bu) {
      g_esc = 1;
      continue;
    }
    if (b == '\n') {
      newline();
    } else if (b == '\r') {
      /* carriage return with no column tracking: ignore */
    } else if (b == '\b' || b == 0x7Fu) {
      pop_last();
    } else if (b == '\t') {
      int t;
      for (t = 0; t < 4; t++) {
        append_byte((UINT8)' ');
      }
    } else if (b >= 0x20u) {
      append_byte(b);
    }
  }
  if (g_s != 0) {
    repaint();
  }
  /* Mirror to the serial console so the session can be captured/debugged. */
  for (i = 0u; i < len; i++) {
    serial_putc((char)buf[i]);
  }
}

static void push_in(UINT8 b) {
  if (g_in_n < CON_IN_CAP) {
    g_in[g_in_n++] = b;
  }
}

static void push_utf8(UINT32 cp) {
  if (cp < 0x80u) {
    push_in((UINT8)cp);
  } else if (cp < 0x800u) {
    push_in((UINT8)(0xC0u | (cp >> 6u)));
    push_in((UINT8)(0x80u | (cp & 0x3Fu)));
  } else if (cp < 0x10000u) {
    push_in((UINT8)(0xE0u | (cp >> 12u)));
    push_in((UINT8)(0x80u | ((cp >> 6u) & 0x3Fu)));
    push_in((UINT8)(0x80u | (cp & 0x3Fu)));
  } else {
    push_in((UINT8)(0xF0u | (cp >> 18u)));
    push_in((UINT8)(0x80u | ((cp >> 12u) & 0x3Fu)));
    push_in((UINT8)(0x80u | ((cp >> 6u) & 0x3Fu)));
    push_in((UINT8)(0x80u | (cp & 0x3Fu)));
  }
}

/* Translate a keyboard event into the shell's byte stream. */
static void con_feed_event(const nd_event_t *ev) {
  UINT32 k = ev->key;

  if (ev->type != ND_EV_KEY) {
    return;
  }
  if (k == ND_KEY_ENTER) {
    push_in(0x0du);
    return;
  }
  if (k == ND_KEY_BACK || k == ND_KEY_DELETE) {
    push_in(0x08u);
    return;
  }
  if (k >= ND_KEY_BASE) {
    return; /* arrows, function keys, ... are not text */
  }
  if ((ev->mods & ND_MOD_CTRL) != 0u) {
    UINT32 c = k;
    if (c >= (UINT32)'a' && c <= (UINT32)'z') {
      push_in((UINT8)(c - (UINT32)'a' + 1u));
      return;
    }
    if (c >= (UINT32)'A' && c <= (UINT32)'Z') {
      push_in((UINT8)(c - (UINT32)'A' + 1u));
      return;
    }
    return; /* other Ctrl combinations: ignored */
  }
  if (k == (UINT32)'\n' || k == (UINT32)'\r') {
    push_in(0x0du);
    return;
  }
  if (k >= 0x20u) {
    push_utf8(k);
  }
}

UINT32 con_in_read(UINT8 *buf, UINT32 len) {
  if (g_in_n == 0u) {
    nd_event_t ev;
    nd_input_poll();
    while (nd_input_pop(&ev)) {
      con_feed_event(&ev);
      if (g_in_n >= CON_IN_CAP) {
        break;
      }
    }
  }
  {
    UINT32 n = (g_in_n < len) ? g_in_n : len;
    UINT32 i;
    for (i = 0u; i < n; i++) {
      buf[i] = g_in[i];
    }
    if (n < g_in_n) {
      con_memmove(g_in, g_in + n, g_in_n - n);
    }
    g_in_n -= n;
    return n;
  }
}
