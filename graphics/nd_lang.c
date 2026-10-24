/**
 * Novus OS - UI language state.
 *
 * Version starts at 1 so a client that records 0 on startup always sees
 * the first real value as a change; every switch bumps it once.
 */

#include "nd_lang.h"

static UINT32 g_lang;
static UINT32 g_version = 1u;

void nd_lang_init(UINT32 lang) {
  if (lang > ND_LANG_EN) {
    lang = ND_LANG_ZH;
  }
  g_lang = lang;
}

UINT32 nd_lang_get(void) { return g_lang; }

void nd_lang_set(UINT32 lang) {
  if (lang > ND_LANG_EN || lang == g_lang) {
    return;
  }
  g_lang = lang;
  g_version++;
}

UINT32 nd_lang_version(void) { return g_version; }

const char *nd_lang_pick(const char *zh, const char *en) {
  return (g_lang == ND_LANG_EN) ? en : zh;
}

const char *nd_lang_tr(const char *zh) {
  const char *en;

  if (g_lang != ND_LANG_EN || zh == 0) {
    return zh;
  }
  en = nd_lang_lookup(zh);
  return (en != 0) ? en : zh;
}
