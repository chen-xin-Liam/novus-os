/**
 * Novus OS - built-in application table (M8).
 *
 * The blobs are generated at build time by xxd from the Rust flat
 * binaries (see "M8 application images" in tools/tools.mk), so this file
 * is only the table: no parsing, no filesystem, no allocator.
 */

#include "apps.h"
#include "apps_blobs.h"
#include "nuc.h"
#include "sched.h"
#include "kernel.h"
#include "session.h"

typedef struct {
  const char          *name;
  const unsigned char *data;
  const unsigned int  *size;
} app_t;

static const app_t g_apps[] = {
    {"fileman", k_app_fileman, &k_app_fileman_len},
    {"taskman", k_app_taskman, &k_app_taskman_len},
    {"settings", k_app_settings, &k_app_settings_len},
    {"netman", k_app_netman, &k_app_netman_len},
    {"session", k_app_session, &k_app_session_len},
};

/* These positions are also the IDs sent by sysinfo's spawn request and the
 * ordering shown in /apps. Keep the table order aligned with those clients;
 * names, image bytes, and generated lengths must remain one row per image. */
UINT32 apps_count(void) { return (UINT32)(sizeof(g_apps) / sizeof(g_apps[0])); }

const char *apps_name(UINT32 i) {
  if (i >= apps_count()) {
    return 0;
  }
  return g_apps[i].name;
}

UINT32 apps_size(UINT32 i) {
  if (i >= apps_count()) {
    return 0;
  }
  return *g_apps[i].size;
}

const UINT8 *apps_data(UINT32 i) {
  if (i >= apps_count()) {
    return 0;
  }
  return g_apps[i].data;
}

int apps_find(const char *name) {
  UINT32 i;

  for (i = 0; i < apps_count(); i++) {
    const char *a = g_apps[i].name;
    const char *b = name;

    while (*a != '\0' && *a == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') {
      return (int)i;
    }
  }
  return -1;
}

int apps_launch(UINT32 i) {
  int rc;

  if (i >= apps_count()) {
    return -1;
  }
  /* The session manager is the bootstrap path; every other built-in app
   * requires an authenticated session and a single live instance by name.
   * This exception keeps the login prompt available before authentication. */
  if (i != 4u && !session_logged_in()) {
    return APPS_EAUTH;
  }
  if (sched_find_name(g_apps[i].name) != 0) {
    return APPS_ERUNNING;
  }
  kputs("[APPS] launching ");
  kputs(g_apps[i].name);
  kputs(" (");
  kputs_hex(*g_apps[i].size);
  kputs(" bytes)\n");
  /* The blobs are linked at 0x400000, which is where nuc_spawn_blob maps
   * them; entry offset 0 is `_start`, forced by the linker script. */
  rc = nuc_spawn_blob(g_apps[i].name, g_apps[i].data, *g_apps[i].size, 0);
  if (rc != 0) {
    kputs("[APPS] spawn failed: ");
    kputs_hex((UINT64)(INT64)rc);
    kputs("\n");
  }
  return rc;
}
