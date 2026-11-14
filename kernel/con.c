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
 *
 * ANSI SGR colour escapes (ESC[...m) are parsed so the shell can emit coloured
 * output on both the framebuffer and the serial mirror; ESC[2J clears.
 */
#include "kernel.h"
#include "nd.h"
#include "nd_input.h"
#include "serial.h"
#include "con.h"

#define CON_MAX_ROWS 300u
#define CON_ROW_BYTES 256u
#define CON_IN_CAP 128u

/* Palette indexed by the foreground colour attribute stored per cell.
 * 0 = default; 1..8 basic 30..37; 9..16 bright 90..97. */
#define CON_PAL_N 17u
static const nd_color_t g_pal[CON_PAL_N] = {
  ND_RGB(0xD8, 0xD8, 0xD8), /* 0 default light gray */
  ND_RGB(0x20, 0x20, 0x20), /* 1 black   */
  ND_RGB(0xE5, 0x48, 0x4D), /* 2 red     */
  ND_RGB(0x4C, 0xC2, 0x6A), /* 3 green   */
  ND_RGB(0xE2, 0xC0, 0x4A), /* 4 yellow  */
  ND_RGB(0x4C, 0x9B, 0xE2), /* 5 blue    */
  ND_RGB(0xC2, 0x6A, 0xE5), /* 6 magenta */
  ND_RGB(0x4C, 0xC2, 0xE5), /* 7 cyan    */
  ND_RGB(0xE8, 0xEE, 0xF8), /* 8 white   */
  ND_RGB(0x70, 0x78, 0x88), /* 9 bright black / gray */
  ND_RGB(0xFF, 0x6B, 0x6F), /* 10 bright red     */
  ND_RGB(0x6B, 0xE5, 0x8A), /* 11 bright green   */
  ND_RGB(0xFF, 0xE5, 0x6B), /* 12 bright yellow  */
  ND_RGB(0x6B, 0xB8, 0xFF), /* 13 bright blue    */
  ND_RGB(0xE5, 0x8A, 0xFF), /* 14 bright magenta */
  ND_RGB(0x6B, 0xE5, 0xFF), /* 15 bright cyan    */
  ND_RGB(0xFF, 0xFF, 0xFF), /* 16 bright white   */
};

static nd_surface_t *g_s;
static char          g_rows[CON_MAX_ROWS][CON_ROW_BYTES];
static UINT8         g_row_col[CON_MAX_ROWS][CON_ROW_BYTES];
static int           g_nrows;
static int           g_cell_w = 6;
static int           g_cell_h = 16;
static int           g_rows_vis = 1;

/* ANSI escape state. g_esc: 0 none, 1 after ESC, 2 after ESC[. */
static int g_esc;
static int g_esc_num;
static int g_esc_params[8];
static int g_esc_nparams;
static int g_fg; /* current foreground palette index */

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
  g_row_col[0][0] = 0;
  g_fg = 0;
  g_esc = 0;
  g_esc_nparams = 0;
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
  UINT8 *col = g_row_col[g_nrows - 1];
  UINT32 l = con_strlen(row);
  if (l + 1u < CON_ROW_BYTES) {
    row[l] = (char)b;
    row[l + 1u] = '\0';
    col[l] = (UINT8)g_fg;
  }
}

static void newline(void) {
  if ((UINT32)g_nrows < CON_MAX_ROWS) {
    g_nrows++;
    g_rows[g_nrows - 1][0] = '\0';
    g_row_col[g_nrows - 1][0] = 0;
  } else {
    con_memmove((UINT8 *)g_rows[0], (const UINT8 *)g_rows[1],
                (CON_MAX_ROWS - 1u) * CON_ROW_BYTES);
    con_memmove(g_row_col[0], g_row_col[1],
                (CON_MAX_ROWS - 1u) * CON_ROW_BYTES);
    g_rows[CON_MAX_ROWS - 1u][0] = '\0';
    g_row_col[CON_MAX_ROWS - 1u][0] = 0;
  }
}

/* Drop the previous whole UTF-8 character from the current row. The parallel
 * colour array is addressed by the same byte offset, so truncating the text at
 * i leaves the stale trailing colours unused. */
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
    const char *row = g_rows[r];
    int         y = (r - start) * g_cell_h;
    UINT32      x = 0u;
    UINT32      i = 0u;
    UINT32      run_start = 0u;
    UINT8       run_col = g_row_col[r][0];
    char        tmp[CON_ROW_BYTES];

    while (row[i] != '\0') {
      if (g_row_col[r][i] != run_col) {
        UINT32 k;
        for (k = run_start; k < i; k++) {
          tmp[k - run_start] = row[k];
        }
        tmp[i - run_start] = '\0';
        nd_text(g_s, (int)x, y, tmp, g_pal[run_col], 1u);
        x += nd_text_width(tmp, 1u);
        run_start = i;
        run_col = g_row_col[r][i];
      }
      i++;
    }
    if (i > run_start) {
      UINT32 k;
      for (k = run_start; k < i; k++) {
        tmp[k - run_start] = row[k];
      }
      tmp[i - run_start] = '\0';
      nd_text(g_s, (int)x, y, tmp, g_pal[run_col], 1u);
    }
  }
  {
    int cy = (g_nrows - 1 - start) * g_cell_h;
    int cx = (int)nd_text_width(g_rows[g_nrows - 1], 1u);
    nd_fill_rect(g_s, nd_rect(cx, cy, g_cell_w, g_cell_h),
                 ND_RGB(0xD8, 0xD8, 0xD8));
  }
}

/* Apply one SGR parameter to the current foreground colour. */
static void con_sgr_param(int p) {
  if (p == 0 || p == 39 || p == 49) {
    g_fg = 0;
  } else if (p >= 30 && p <= 37) {
    g_fg = p - 30 + 1;
  } else if (p >= 90 && p <= 97) {
    g_fg = p - 90 + 9;
  }
  /* 1 (bold), 4 (underline), 38/48 extended, ... are accepted but ignored. */
}

static void con_esc_end(int letter) {
  int i;
  if (letter == 'm') {
    if (g_esc_nparams == 0) {
      con_sgr_param(0);
    } else {
      for (i = 0; i < g_esc_nparams; i++) {
        con_sgr_param(g_esc_params[i]);
      }
    }
  } else if (letter == 'J') {
    /* ESC[2J / ESC[J / ESC[0J -> full clear (we only clear the whole screen). */
    for (UINT32 rr = 0u; rr < CON_MAX_ROWS; rr++) {
      g_rows[rr][0] = '\0';
      g_row_col[rr][0] = 0;
    }
    g_nrows = 1;
  }
  g_esc = 0;
  g_esc_num = 0;
  g_esc_nparams = 0;
}

void con_out(const UINT8 *buf, UINT32 len) {
  UINT32 i;
  for (i = 0u; i < len; i++) {
    UINT8 b = buf[i];

    if (g_esc != 0) {
      if (g_esc == 1) {
        if (b == '[') {
          g_esc = 2;
          g_esc_num = 0;
          g_esc_nparams = 0;
        } else {
          g_esc = 0;
        }
        continue;
      }
      /* g_esc == 2: inside ESC[ ... */
      if (b >= '0' && b <= '9') {
        g_esc_num = g_esc_num * 10 + (b - '0');
        if (g_esc_num > 9999) {
          g_esc_num = 9999;
        }
      } else if (b == ';') {
        if (g_esc_nparams < 8) {
          g_esc_params[g_esc_nparams++] = g_esc_num;
        }
        g_esc_num = 0;
      } else if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')) {
        if (g_esc_nparams < 8) {
          g_esc_params[g_esc_nparams++] = g_esc_num;
        }
        con_esc_end((int)b);
      } else {
        g_esc = 0;
        g_esc_num = 0;
        g_esc_nparams = 0;
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
  /* Mirror to the serial console so the session can be captured/debugged;
   * ANSI escapes pass through untouched, so a serial terminal shows colour. */
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
