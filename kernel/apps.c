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
#include "install.h"
#if CONFIG_NOVUS_WIN32
#include "win32_blobs.h"
#include "pe.h"
#endif

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
    {"setup", k_app_setup, &k_app_setup_len},
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

static int name_is(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

/* Two applications are allowed before a session exists, and both are
 * bootstrap paths: the login window itself, and the installer (a machine
 * being installed has no account yet). Names, not table positions - the
 * setup app shifted every index once already. */
static int app_is_bootstrap(const char *name) {
  return name_is(name, "session") || name_is(name, "setup");
}

int apps_launch(UINT32 i) {
  int rc;

  if (i >= apps_count()) {
    return -1;
  }
  /* Every other built-in app requires an authenticated session and a
   * single live instance by name. */
  if (!app_is_bootstrap(g_apps[i].name) && !session_logged_in()) {
    return APPS_EAUTH;
  }
  if (name_is(g_apps[i].name, "setup") && !install_available()) {
    return -1;
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
    return rc;
  }
  /* Only the process the kernel started as the installer may drive the
   * destructive path; the pid is the capability. */
  if (name_is(g_apps[i].name, "setup")) {
    task_t *t = sched_find_name("setup");

    install_authorize((t != 0) ? t->pid : 0);
  }
  return 0;
}

int apps_launch_name(const char *name) {
  int i = apps_find(name);
  int w;

  if (i >= 0) {
    return apps_launch((UINT32)i);
  }
  w = apps_win32_find(name);
  if (w >= 0) {
    return apps_win32_launch((UINT32)w);
  }
  return -1;
}

#if CONFIG_NOVUS_WIN32

/* Compatibility images are a separate table: adding one must not shift a
 * NUC index, which the sysinfo /apps page and every spawn client rely on. */
static const app_t g_win32[] = {
    {"hello.exe", k_app_hello_exe, &k_app_hello_exe_len},
};

UINT32 apps_win32_count(void) {
  return (UINT32)(sizeof(g_win32) / sizeof(g_win32[0]));
}

const char *apps_win32_name(UINT32 i) {
  if (i >= apps_win32_count()) {
    return 0;
  }
  return g_win32[i].name;
}

UINT32 apps_win32_size(UINT32 i) {
  if (i >= apps_win32_count()) {
    return 0;
  }
  return *g_win32[i].size;
}

int apps_win32_find(const char *name) {
  UINT32 i;

  for (i = 0; i < apps_win32_count(); i++) {
    if (name_is(g_win32[i].name, name)) {
      return (int)i;
    }
  }
  return -1;
}

int apps_win32_launch(UINT32 i) {
  int rc;

  if (i >= apps_win32_count()) {
    return -1;
  }
  if (!session_logged_in()) {
    return APPS_EAUTH;
  }
  if (sched_find_name(g_win32[i].name) != 0) {
    return APPS_ERUNNING;
  }
  kputs("[APPS] launching ");
  kputs(g_win32[i].name);
  kputs(" (");
  kputs_hex(*g_win32[i].size);
  kputs(" bytes)\n");
  rc = pe_load(g_win32[i].name, g_win32[i].data, *g_win32[i].size);
  if (rc != 0) {
    kputs("[APPS] win32 spawn failed: ");
    kputs_hex((UINT64)(INT64)rc);
    kputs("\n");
  }
  return rc;
}

#else /* !CONFIG_NOVUS_WIN32 */

UINT32 apps_win32_count(void) { return 0; }
const char *apps_win32_name(UINT32 i) { (void)i; return 0; }
UINT32 apps_win32_size(UINT32 i) { (void)i; return 0; }
int apps_win32_find(const char *name) { (void)name; return -1; }
int apps_win32_launch(UINT32 i) { (void)i; return -1; }

#endif /* CONFIG_NOVUS_WIN32 */
