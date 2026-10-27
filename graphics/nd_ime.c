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
#include "nd_font_uni.h"
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
static int      g_en; /* 0 = Chinese (pinyin), 1 = Latin passthrough */

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

/* The dictionary's sorted syllables make prefix existence searchable
 * without building a second trie. Rejecting a non-prefix keystroke leaves
 * the composition buffer valid, even when the exact syllable has no result. */
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

/* Finish the composition and hand back the selected codepoint (0 when the
 * candidate list was empty). A selection is a Unicode codepoint, not a byte
 * from the candidate table. */
static UINT32 take_commit(void) {
  UINT32 cp = 0;

  if (g_ime.n != 0u && g_ime.sel < g_ime.n) {
    cp = g_ime.cand[g_ime.sel];
  }
  damage_popup();
  g_ime.active = 0;
  g_ime.n = 0;
  g_ime.py[0] = '\0';
  return cp;
}

void nd_ime_init(void) {
  memset(&g_ime, 0, sizeof(g_ime));
  g_en = 0;
  kputs("[IME] pinyin up: ");
  kputs_hex(g_ime_count);
  kputs(" syllables, ");
  kputs_hex(g_ime_start[g_ime_count - 1u] + g_ime_len[g_ime_count - 1u]);
  kputs(" characters\n");
}

void nd_ime_set_en(int en) {
  if (g_en == (en != 0)) {
    return;
  }
  /* Switching away from Chinese ends any open composition; the text box
   * keeps whatever was already committed. */
  if (g_ime.active) {
    damage_popup();
    g_ime.active = 0;
    g_ime.n = 0;
    g_ime.py[0] = '\0';
  }
  g_en = (en != 0);
}

int nd_ime_en(void) { return g_en; }

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

/* One keystroke through the composition state machine. Returns 1 when the
 * IME consumed the key, and stores a codepoint in *commit when that key
 * completed a composition. The caller decides where the text goes: a text
 * box (nd_ime_key) or a window over the WM protocol (nd_ime_key_ex). */
static int ime_step(UINT32 key, UINT32 *commit) {
  UINT32 len = 0;

  while (g_ime.py[len] != '\0') {
    len++;
  }

  if (!g_ime.active) {
    if (!g_en && key >= 'a' && key <= 'z' && len + 1u < sizeof(g_ime.py)) {
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
    return 0; /* Latin mode, or a non-letter key: hand it to the text box */
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
      *commit = take_commit();
    }
    return 1;
  }
  switch (key) {
  case ' ':
  case ND_KEY_ENTER:
    if (g_ime.n != 0u) {
      *commit = take_commit();
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

int nd_ime_key(void *editw, UINT32 key) {
  ng_widget_t *edit = (ng_widget_t *)editw;
  UINT32       cp = 0;
  int          consumed;

  if (edit == 0 || edit->type != NG_TEXTBOX) {
    return 0;
  }
  consumed = ime_step(key, &cp);
  if (consumed && cp != 0u) {
    (void)ng_edit_insert_cp(edit, cp);
  }
  if (consumed) {
    ng_damage(nd_rect(0, 0, 0, 0)); /* force the caret to repaint */
  }
  return consumed;
}

int nd_ime_key_ex(UINT32 key, UINT32 *commit) {
  UINT32 cp = 0;
  int    consumed = ime_step(key, &cp);

  if (commit != 0) {
    *commit = cp;
  }
  return consumed;
}

/* ---- drawing ---------------------------------------------------------- */

static void draw_popup(nd_surface_t *s, INT32 bx, INT32 by) {
  const ng_theme_t *t = &ng_ctx_get()->theme;
  INT32             bw = 236;
  INT32             bh = 52;
  UINT32            i;

  g_ime.box = nd_rect(bx, by, bw, bh);

  nd_shadow(s, g_ime.box, 8, 60);
  nd_round_rect(s, g_ime.box, 8, t->win_bg);
  nd_round_frame(s, g_ime.box, 8, 1, t->win_border);

  /* Composing text. */
  nd_text(s, bx + 10, by + 9, g_ime.py, t->text_dim, 1);

  /* Candidates: digit + glyph, highlighted cell for the selected one. */
  for (i = 0; i < g_ime.n; i++) {
    INT32  cx = bx + 8 + (INT32)i * 36;
    char   num[2];
    char   utf8[5];
    UINT32 len = nd_utf8_encode(g_ime.cand[i], utf8);

    if (i == g_ime.sel) {
      nd_round_rect(s, nd_rect(cx - 2, by + 24, 34, 22), 4, t->accent);
    }
    num[0] = (char)('1' + i);
    num[1] = '\0';
    nd_text(s, cx, by + 30, num,
            (i == g_ime.sel) ? t->accent_text : t->text_dim, 1);
    if (len != 0u) {
      /* Reuse the text path so the glyph is drawn exactly like a label. */
      utf8[len] = '\0';
      nd_text(s, cx + 12, by + 24,
              utf8, (i == g_ime.sel) ? t->accent_text : t->text, 1);
    }
  }
  g_ime.drawn = 1;
}

static void clamp_popup(nd_surface_t *s, INT32 *bx, INT32 *by) {
  if (*bx + 236 > (INT32)s->w - 8) {
    *bx = (INT32)s->w - 236 - 8;
  }
  if (*bx < 8) {
    *bx = 8;
  }
  if (*by + 52 > (INT32)s->h - 8) {
    *by = (INT32)s->h - 52 - 8;
  }
  if (*by < 8) {
    *by = 8;
  }
}

void nd_ime_draw(nd_surface_t *s) {
  nd_rect_t caret;
  INT32     bx;
  INT32     by;

  if (!g_ime.active || s == 0) {
    return;
  }
  if (!ng_focus_caret(&caret)) {
    return;
  }
  bx = caret.x - 6;
  by = caret.y + 24;
  if (by + 52 > (INT32)s->h - 8) {
    by = caret.y - 52 - 8; /* flip above the caret near the bottom */
  }
  clamp_popup(s, &bx, &by);
  draw_popup(s, bx, by);
}

void nd_ime_draw_at(nd_surface_t *s, INT32 x, INT32 y) {
  if (!g_ime.active || s == 0) {
    return;
  }
  clamp_popup(s, &x, &y);
  draw_popup(s, x, y);
}

#else /* !CONFIG_NOVUS_GUI */

void nd_ime_init(void) {}
int  nd_ime_key(void *edit, UINT32 key) {
  (void)edit;
  (void)key;
  return 0;
}
void nd_ime_draw(nd_surface_t *s) { (void)s; }
void nd_ime_draw_at(nd_surface_t *s, INT32 x, INT32 y) {
  (void)s;
  (void)x;
  (void)y;
}
int nd_ime_key_ex(UINT32 key, UINT32 *commit) {
  (void)key;
  if (commit != 0) {
    *commit = 0;
  }
  return 0;
}
int  nd_ime_active(void) { return 0; }
void nd_ime_bounds(INT32 *x, INT32 *y, INT32 *w, INT32 *h) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}
void nd_ime_set_en(int en) { (void)en; }
int  nd_ime_en(void) { return 1; } /* no IME: everything passes through */

#endif
