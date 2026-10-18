/**
 * Novus OS - pinyin input method (M9).
 *
 * Compose -> look up -> commit. The dictionary is a sorted syllable table
 * (generated), the composing buffer is 15 letters, and the candidate
 * window is drawn as part of the shell's frame so it needs no window of
 * its own.
 */

#include "kernel.h"
#include "nd_internal.h"
#include "nd_ime.h"
#include "nd_font_cjk.h"
#include "nvgui.h"

#if CONFIG_NOVUS_GUI

typedef struct {
  int       active;
  char      py[16];
  UINT32    cand[ND_IME_CANDS];
  UINT8     n;   /* candidates found */
  UINT8     sel; /* highlighted one */
  nd_rect_t box; /* where the popup was drawn last */
  int       drawn;
} nd_ime_t;

static nd_ime_t g_ime;

static int str_equal(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int str_less(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return (UINT8)*a < (UINT8)*b;
}

/* Syllable -> candidates. Binary search over the generated table. */
static UINT32 lookup(const char *py, UINT32 *out) {
  UINT32 lo = 0;
  UINT32 hi = g_ime_count;

  while (lo < hi) {
    UINT32      mid = (lo + hi) / 2u;
    const char *s = g_ime_pool + g_ime_off[mid];

    if (str_equal(s, py)) {
      UINT32 n = g_ime_len[mid];
      UINT32 i;

      for (i = 0; i < n && i < ND_IME_CANDS; i++) {
        out[i] = g_ime_cand[g_ime_start[mid] + i];
      }
      return n;
    }
    if (str_less(s, py)) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return 0;
}

/* True when at least one syllable starts with `py`: while composing, a
 * letter that would break every syllable is refused, so the buffer is
 * always a pinyin prefix and the candidate list is never empty. */
static int syllable_prefix(const char *py) {
  UINT32 lo = 0;
  UINT32 hi = g_ime_count;
  UINT32 len = 0;

  while (py[len] != '\0') {
    len++;
  }
  while (lo < hi) {
    UINT32      mid = (lo + hi) / 2u;
    const char *s = g_ime_pool + g_ime_off[mid];
    UINT32      i;
    int         cmp = 0;

    for (i = 0; i < len; i++) {
      if (s[i] == '\0') {
        cmp = -1; /* syllable shorter than the prefix */
        break;
      }
      if ((UINT8)s[i] != (UINT8)py[i]) {
        cmp = ((UINT8)s[i] < (UINT8)py[i]) ? -1 : 1;
        break;
      }
    }
    if (cmp == 0) {
      return 1; /* this syllable starts with the prefix (or is equal) */
    }
    if (cmp < 0) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return 0;
}

static void refresh(void) {
  if (g_ime.py[0] == '\0') {
    g_ime.active = 0;
    g_ime.n = 0;
    return;
  }
  g_ime.n = (UINT8)lookup(g_ime.py, g_ime.cand);
  g_ime.sel = 0;
}

static void damage_popup(void) {
  if (g_ime.drawn) {
    ng_damage(g_ime.box);
  }
}

static void commit(ng_widget_t *edit) {
  if (edit != 0 && g_ime.n != 0u && g_ime.sel < g_ime.n) {
    (void)ng_edit_insert_cp(edit, g_ime.cand[g_ime.sel]);
  }
  damage_popup();
  g_ime.active = 0;
  g_ime.n = 0;
  g_ime.py[0] = '\0';
  ng_damage(nd_rect(0, 0, 0, 0)); /* force the caret to repaint */
}

void nd_ime_init(void) {
  memset(&g_ime, 0, sizeof(g_ime));
  kputs("[IME] pinyin up: ");
  kputs_hex(g_ime_count);
  kputs(" syllables, ");
  kputs_hex(g_ime_start[g_ime_count - 1u] + g_ime_len[g_ime_count - 1u]);
  kputs(" characters\n");
}

int nd_ime_active(void) { return g_ime.active; }

void nd_ime_bounds(INT32 *x, INT32 *y, INT32 *w, INT32 *h) {
  if (x != 0) {
    *x = g_ime.box.x;
  }
  if (y != 0) {
    *y = g_ime.box.y;
  }
  if (w != 0) {
    *w = g_ime.box.w;
  }
  if (h != 0) {
    *h = g_ime.box.h;
  }
}

int nd_ime_key(void *editw, UINT32 key) {
  ng_widget_t *edit = (ng_widget_t *)editw;
  UINT32       len;

  if (edit == 0 || edit->type != NG_TEXTBOX) {
    return 0;
  }
  len = 0;
  while (g_ime.py[len] != '\0') {
    len++;
  }

  if (!g_ime.active) {
    if (key >= 'a' && key <= 'z' && len + 1u < sizeof(g_ime.py)) {
      g_ime.py[len] = (char)key;
      g_ime.py[len + 1u] = '\0';
      if (!syllable_prefix(g_ime.py)) {
        g_ime.py[len] = '\0'; /* not the start of any syllable */
        return 1;
      }
      g_ime.active = 1;
      refresh();
      damage_popup();
      return 1;
    }
    return 0;
  }

  /* Composing. */
  if (key >= 'a' && key <= 'z') {
    if (len + 1u < sizeof(g_ime.py)) {
      g_ime.py[len] = (char)key;
      g_ime.py[len + 1u] = '\0';
      if (!syllable_prefix(g_ime.py)) {
        g_ime.py[len] = '\0'; /* keep the last valid prefix */
        return 1;
      }
      damage_popup();
      refresh();
    }
    return 1;
  }
  if (key >= '1' && key <= '6') {
    UINT32 idx = key - '1';

    if (idx < g_ime.n) {
      g_ime.sel = (UINT8)idx;
      commit(edit);
    }
    return 1;
  }
  switch (key) {
  case ' ':
  case ND_KEY_ENTER:
    if (g_ime.n != 0u) {
      commit(edit);
    } else {
      damage_popup();
      g_ime.active = 0;
      g_ime.py[0] = '\0';
    }
    return 1;
  case ND_KEY_BACK:
    damage_popup();
    if (len > 0u) {
      g_ime.py[len - 1u] = '\0';
      refresh();
      if (g_ime.n == 0u) {
        g_ime.active = (g_ime.py[0] != '\0') ? 1 : 0;
      }
    } else {
      g_ime.active = 0;
    }
    return 1;
  case ND_KEY_LEFT:
    if (g_ime.n != 0u) {
      damage_popup();
      g_ime.sel = (UINT8)((g_ime.sel + g_ime.n - 1u) % g_ime.n);
    }
    return 1;
  case ND_KEY_RIGHT:
    if (g_ime.n != 0u) {
      damage_popup();
      g_ime.sel = (UINT8)((g_ime.sel + 1u) % g_ime.n);
    }
    return 1;
  case ND_KEY_ESC:
    damage_popup();
    g_ime.active = 0;
    g_ime.py[0] = '\0';
    g_ime.n = 0;
    return 1;
  default:
    return 1; /* composing owns the keyboard until it ends */
  }
}

/* ---- drawing ---------------------------------------------------------- */

void nd_ime_draw(nd_surface_t *s) {
  const ng_theme_t *t = &ng_ctx_get()->theme;
  nd_rect_t         caret;
  INT32             bx;
  INT32             by;
  INT32             bw = 236;
  INT32             bh = 52;
  UINT32            i;

  if (!g_ime.active || s == 0) {
    return;
  }
  if (!ng_focus_caret(&caret)) {
    return;
  }
  bx = caret.x - 6;
  by = caret.y + 24;
  if (bx + bw > (INT32)s->w - 8) {
    bx = (INT32)s->w - bw - 8;
  }
  if (bx < 8) {
    bx = 8;
  }
  if (by + bh > (INT32)s->h - 8) {
    by = caret.y - bh - 8; /* flip above the caret near the bottom */
  }
  g_ime.box = nd_rect(bx, by, bw, bh);

  nd_shadow(s, g_ime.box, 8, 60);
  nd_round_rect(s, g_ime.box, 8, t->win_bg);
  nd_round_frame(s, g_ime.box, 8, 1, t->win_border);

  /* Composing text. */
  nd_text(s, bx + 10, by + 9, g_ime.py, t->text_dim, 1);

  /* Candidates: digit + glyph, highlighted cell for the selected one. */
  for (i = 0; i < g_ime.n; i++) {
    INT32         cx = bx + 8 + (INT32)i * 36;
    char          num[2];
    const UINT16 *rows = nd_cjk_find(g_ime.cand[i]);

    if (i == g_ime.sel) {
      nd_round_rect(s, nd_rect(cx - 2, by + 24, 34, 22), 4, t->accent);
    }
    num[0] = (char)('1' + i);
    num[1] = '\0';
    nd_text(s, cx, by + 30, num,
            (i == g_ime.sel) ? t->accent_text : t->text_dim, 1);
    if (rows != 0) {
      /* Reuse the text path so the glyph is drawn exactly like a label. */
      char utf8[4];
      UINT32 cp = g_ime.cand[i];

      utf8[0] = (char)(0xE0u | ((cp >> 12) & 0x0Fu));
      utf8[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
      utf8[2] = (char)(0x80u | (cp & 0x3Fu));
      utf8[3] = '\0';
      nd_text(s, cx + 12, by + 24,
              utf8, (i == g_ime.sel) ? t->accent_text : t->text, 1);
    }
  }
  g_ime.drawn = 1;
}

#else /* !CONFIG_NOVUS_GUI */

void nd_ime_init(void) {}
int  nd_ime_key(void *edit, UINT32 key) {
  (void)edit;
  (void)key;
  return 0;
}
void nd_ime_draw(nd_surface_t *s) { (void)s; }
int  nd_ime_active(void) { return 0; }
void nd_ime_bounds(INT32 *x, INT32 *y, INT32 *w, INT32 *h) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}

#endif
