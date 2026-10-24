/**
 * Novus OS - design token tables (generated).
 */

#include "kernel.h"
#include "nd_theme.h"
#include "rtc.h"

static const nd_tokens_t g_light = {
    0xFFEEF2F8u, /* canvas_top */
    0xFFE3EAF3u, /* canvas_bottom */
    0xFFFFFFFFu, /* surface */
    0xFFFBFBFDu, /* card */
    0xFFF0F2F5u, /* subtle */
    0xFFE1E5EAu, /* stroke */
    0xFF1B1B1Bu, /* text */
    0xFF5F6368u, /* dim */
    0xFF0067C0u, /* accent */
    0xFFFFFFFFu, /* on_accent */
    0xFF107C10u, /* success */
    0xFFB45309u, /* warn */
    0xFFC42B1Cu, /* danger */
    0xFFE5F1FBu, /* selection */
    0xFF000000u, /* shadow */
};

static const nd_tokens_t g_dark = {
    0xFF1A1E24u, /* canvas_top */
    0xFF11151Au, /* canvas_bottom */
    0xFF202020u, /* surface */
    0xFF272727u, /* card */
    0xFF2E2E2Eu, /* subtle */
    0xFF3A3A3Au, /* stroke */
    0xFFF5F5F5u, /* text */
    0xFFB8BCC2u, /* dim */
    0xFF4CC2FFu, /* accent */
    0xFF00243Du, /* on_accent */
    0xFF6CCB5Fu, /* success */
    0xFFFCE100u, /* warn */
    0xFFFF99A4u, /* danger */
    0xFF1F3A4Du, /* selection */
    0xFF000000u, /* shadow */
};

static const nd_metrics_t g_metrics = {
    6, /* radius_control */
    10, /* radius_card */
    18, /* radius_pill */
    4, /* space_xs */
    8, /* space_s */
    12, /* space_m */
    16, /* space_l */
    24, /* space_xl */
    32, /* control_h */
    36, /* button_h */
    28, /* row_h */
    32, /* title_h */
    14, /* shadow_spread */
};

static const UINT32 g_light_papers[][2] = {
    {0xFF9E8AB5u, 0xFF0279A4u},
    {0xFFBFE3C1u, 0xFF25657Du},
    {0xFF3499E4u, 0xFF6FA5CEu},
    {0xFF5B6D80u, 0xFF2B4308u},
    {0xFF813562u, 0xFF7A1D74u},
    {0xFFCC7505u, 0xFF30221Du},
    {0xFF733641u, 0xFF7E475Fu},
};

static const UINT32 g_dark_papers[][2] = {
    {0xFF3A3145u, 0xFF023A4Eu},
    {0xFF3E5240u, 0xFF12313Du},
    {0xFF1A4C77u, 0xFF37566Bu},
    {0xFF2D3740u, 0xFF152208u},
    {0xFF401B31u, 0xFF3D0E3Au},
    {0xFF663A02u, 0xFF181110u},
    {0xFF391B20u, 0xFF3E2330u},
};

static nd_theme_mode_t g_mode = ND_THEME_AUTO;
static int             g_is_dark;
static UINT32          g_version = 1u;
static UINT32          g_accent;    /* 0 = palette accent */
static UINT32          g_wallpaper;
static nd_tokens_t     g_current;
static int             g_built;   /* g_current holds a resolved palette */

static void theme_build(void) {
  const nd_tokens_t *base = g_is_dark ? &g_dark : &g_light;
  const UINT32 (*papers)[2] = g_is_dark ? g_dark_papers : g_light_papers;
  UINT32 count = (UINT32)(sizeof(g_light_papers) / sizeof(g_light_papers[0]));
  UINT32 idx = (g_wallpaper < count) ? g_wallpaper : 0u;

  g_current = *base;
  g_current.canvas_top = papers[idx][0];
  g_current.canvas_bottom = papers[idx][1];
  if (g_accent != 0u) {
    /* Readable caption on a user accent: luminance picks the ink. */
    UINT32 r = (g_accent >> 16) & 0xFFu;
    UINT32 g = (g_accent >> 8) & 0xFFu;
    UINT32 b = g_accent & 0xFFu;
    UINT32 luma = (r * 299u + g * 587u + b * 114u) / 1000u;

    g_current.accent = g_accent;
    g_current.on_accent = (luma >= 150u) ? 0x00002436u : 0x00FFFFFFu;
  }
}

static void theme_apply(void);

const nd_metrics_t *nd_metrics(void) { return &g_metrics; }
const nd_tokens_t *nd_tokens_for(int dark) { return dark ? &g_dark : &g_light; }
const nd_tokens_t *nd_tokens(void) {
  /* First use may precede any mode change (light mode at boot used
   * to leave the table zeroed, which painted a black desktop). */
  if (!g_built) {
    theme_apply();
  }
  return &g_current;
}
nd_theme_mode_t nd_theme_mode(void) { return g_mode; }
int nd_theme_is_dark(void) { return g_is_dark; }
UINT32 nd_theme_version(void) { return g_version; }

static void theme_apply(void) {
  theme_build();
  g_built = 1;
  g_version++;
}

int nd_theme_resolve(void) {
  UINT8 hour = 0;
  UINT8 minute = 0;
  int want;

  if (g_mode != ND_THEME_AUTO) {
    return 0;
  }
  if (!rtc_now(&hour, &minute)) {
    return 0; /* no RTC: keep the last resolution */
  }
  want = (hour < 7u || hour >= 19u) ? 1 : 0;
  if (want == g_is_dark) {
    return 0;
  }
  g_is_dark = want;
  theme_apply();
  return 1;
}

void nd_theme_set_mode(nd_theme_mode_t mode) {
  if (mode > ND_THEME_AUTO) {
    return;
  }
  g_mode = mode;
  if (mode == ND_THEME_AUTO) {
    UINT8 hour = 0;
    UINT8 minute = 0;

    g_is_dark = rtc_now(&hour, &minute) ? ((hour < 7u || hour >= 19u) ? 1 : 0) : g_is_dark;
  } else {
    g_is_dark = (mode == ND_THEME_DARK) ? 1 : 0;
  }
  theme_apply();
}

UINT32 nd_theme_accent(void) { return g_current.accent; }
UINT32 nd_theme_paper_top(UINT32 index) {
  const UINT32 (*papers)[2] = g_is_dark ? g_dark_papers : g_light_papers;
  UINT32 count = (UINT32)(sizeof(g_light_papers) / sizeof(g_light_papers[0]));

  return (index < count) ? papers[index][0] : papers[0][0];
}
void nd_theme_set_accent(UINT32 rgb) {
  g_accent = rgb & 0x00FFFFFFu;
  theme_apply();
}
UINT32 nd_theme_wallpaper(void) { return g_wallpaper; }
void nd_theme_set_wallpaper(UINT32 index) {
  if (index >= (UINT32)(sizeof(g_light_papers) / sizeof(g_light_papers[0]))) {
    return;
  }
  g_wallpaper = index;
  theme_apply();
}
int nd_theme_wallpaper_count(void) {
  return (int)(sizeof(g_light_papers) / sizeof(g_light_papers[0]));
}
