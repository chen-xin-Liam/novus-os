#include "session.h"
#include "kernel.h"
#include "sched.h"
#include "apps.h"
#include "nuc.h"
#include "shared_mem.h"
#include "../fs/novusfs/novusfs.h"
#include "../lib/novuslib_rs.h"
#include <stddef.h>

#define SESSION_MAGIC 0x5553564Eu
#define SESSION_VERSION 1u
#define SESSION_HASH_ROUNDS 1024u
#define SESSION_LOCK_TICKS 3000u
#define SESSION_MAX_FAILURES 5u

typedef struct __attribute__((packed)) {
  UINT32 magic;
  UINT32 version;
  char username[SESSION_NAME_MAX + 1u];
  UINT8 salt[16];
  UINT8 digest[32];
} session_record_t;

static session_record_t g_record;
static UINTN g_record_ready;
static UINTN g_logged_in;
static UINTN g_failures;
static UINT64 g_lock_until;
static UINT64 g_salt_counter;
static int text_equal(const char *a, const char *b);

static void desktop_start(void) {
  UINT32 i;
  shared_mem_t *shm = shared_mem_get();
  for (i = 0; i < 3u; i++) {
    if (apps_launch(i) != 0) {
      kputs("[SESSION] desktop app launch failed\n");
    }
  }
  if (shm->app_img_base != 0 && shm->app_img_size != 0 &&
      nuc_load((const void *)(UINTN)shm->app_img_base, shm->app_img_size, "demo") != 0) {
    kputs("[SESSION] staged app refused\n");
  }
}

static int desktop_app(const char *name) {
  return text_equal(name, "fileman") || text_equal(name, "taskman") ||
      text_equal(name, "settings") || text_equal(name, "netman") ||
      text_equal(name, "demo");
}

static void desktop_stop(void) {
  UINT32 i;
  for (i = 0; i < sched_task_count(); i++) {
    sched_info_t task;
    if (sched_task_info(i, &task) == 0 && task.state != TASK_EXITED &&
        desktop_app(task.name)) {
      (void)sched_kill(task.pid);
    }
  }
}

static int text_equal(const char *a, const char *b) {
  UINTN i = 0;
  while (a[i] != '\0' && a[i] == b[i]) i++;
  return a[i] == b[i];
}

static void derive(const UINT8 salt[16], const char *username,
                   const char *password, UINT8 out[32]) {
  UINT8 first[16 + SESSION_NAME_MAX + SESSION_PASS_MAX + 2u];
  UINTN n = 0;
  UINT32 i;
  /* Bind the initial digest to both the salt and username, then repeatedly
   * hash the prior digest with that same salt for the configured rounds.
   * Setup and login must use the same stored salt to derive matching digests. */
  memcpy(first + n, salt, 16); n += 16;
  while (*username != '\0') first[n++] = (UINT8)*username++;
  first[n++] = 0;
  while (*password != '\0') first[n++] = (UINT8)*password++;
  novus_rs_sha256(first, n, out);
  for (i = 0; i < SESSION_HASH_ROUNDS; i++) {
    UINT8 round[48];
    memcpy(round, out, 32);
    memcpy(round + 32, salt, 16);
    novus_rs_sha256(round, sizeof(round), out);
  }
  memset(first, 0, sizeof(first));
}

static void make_salt(UINT64 pid, UINT8 salt[16]) {
  UINT64 tsc;
  UINT32 lo;
  UINT32 hi;
  UINT8 seed[32];
  UINT8 digest[32];
  /* Mix the local clock, caller, counter, and scheduler ticks into a
   * 16-byte salt derived from SHA-256. These inputs add variation but do
   * not provide a hardware-backed entropy guarantee. */
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  tsc = ((UINT64)hi << 32) | lo;
  memset(seed, 0, sizeof(seed));
  memcpy(seed, &tsc, sizeof(tsc));
  memcpy(seed + 8, &pid, sizeof(pid));
  memcpy(seed + 16, &g_salt_counter, sizeof(g_salt_counter));
  {
    UINT64 ticks = sched_ticks();
    memcpy(seed + 24, &ticks, sizeof(ticks));
  }
  g_salt_counter++;
  novus_rs_sha256(seed, sizeof(seed), digest);
  memcpy(salt, digest, 16);
  memset(seed, 0, sizeof(seed));
  memset(digest, 0, sizeof(digest));
}

static int load_record(void) {
  novusfs_fs_t *fs = novusfs_root();
  UINT32 ino;
  if (fs == NULL) return -1;
  ino = novusfs_lookup_path(fs, fs->sb.root_inode, "/users.db");
  if (ino == 0) return 0;
  if (novusfs_read(fs, ino, 0, &g_record, sizeof(g_record)) != sizeof(g_record) ||
      g_record.magic != SESSION_MAGIC || g_record.version != SESSION_VERSION ||
      g_record.username[0] == '\0') return -1;
  g_record_ready = 1;
  return 1;
}

void session_init(void) {
  g_record_ready = 0;
  g_logged_in = 0;
  g_failures = 0;
  g_lock_until = 0;
  if (load_record() < 0) kputs("[SESSION] account record unavailable or invalid\n");
  else kputs(g_record_ready ? "[SESSION] account loaded; login required\n"
                            : "[SESSION] first boot; account setup required\n");
}

int session_logged_in(void) { return g_logged_in != 0; }

static int write_record(void) {
  novusfs_fs_t *fs = novusfs_root();
  UINT32 ino;
  if (fs == NULL) return -1;
  ino = novusfs_lookup_path(fs, fs->sb.root_inode, "/users.db");
  if (ino == 0) ino = novusfs_creat(fs, fs->sb.root_inode, "users.db");
  if (ino == 0) return -1;
  return novusfs_write(fs, ino, 0, &g_record, sizeof(g_record)) == sizeof(g_record) ? 0 : -1;
}

static int username_valid(const session_req_t *req) {
  UINTN i;
  if (req->name_len == 0 || req->name_len > SESSION_NAME_MAX ||
      req->username[req->name_len] != '\0') return 0;
  for (i = 0; i < req->name_len; i++) {
    char c = req->username[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 1;
}

static int password_valid(const session_req_t *req) {
  UINTN i;
  if (req->pass_len < 4 || req->pass_len > SESSION_PASS_MAX ||
      req->password[req->pass_len] != '\0') return 0;
  for (i = 0; i < req->pass_len; i++) if ((UINT8)req->password[i] < 0x20u) return 0;
  return 1;
}

int session_handle(const session_req_t *req, UINT64 caller_pid) {
  UINT8 digest[32];
  task_t *manager;
  if (req == NULL) return -1;
  if (req->op == SESSION_OP_STATUS) return g_record_ready ? 1 : 0;
  /* Mutating session operations are accepted only from the named session
   * manager task; STATUS is intentionally handled before this gate.
   * That leaves status reads available without granting state changes. */
  manager = sched_find_name("session");
  if (manager == NULL || manager->pid != caller_pid) return -5;
  if (req->op == SESSION_OP_LOGOUT) {
    g_logged_in = 0;
    g_failures = 0;
    desktop_stop();
    kputs("[SESSION] user logged out\n");
    return 0;
  }
  if (!username_valid(req) || !password_valid(req)) return -1;
  if (req->op == SESSION_OP_SETUP) {
    if (g_record_ready) return -2;
    if (!novusfs_root_persistent()) return -4;
    memset(&g_record, 0, sizeof(g_record));
    g_record.magic = SESSION_MAGIC;
    g_record.version = SESSION_VERSION;
    memcpy(g_record.username, req->username, req->name_len + 1u);
    make_salt(caller_pid, g_record.salt);
    derive(g_record.salt, req->username, req->password, g_record.digest);
    if (write_record() != 0) {
      memset(&g_record, 0, sizeof(g_record));
      return -1;
    }
    g_record_ready = 1;
    g_logged_in = 1;
    g_failures = 0;
    desktop_start();
    kputs("[SESSION] account created; password stored as salted hash\n");
    return 0;
  }
  if (req->op != SESSION_OP_LOGIN || !g_record_ready) return -2;
  if (sched_ticks() < g_lock_until) return -3;
  if (!text_equal(req->username, g_record.username)) goto failed;
  derive(g_record.salt, req->username, req->password, digest);
  {
    /* Accumulate every byte difference before branching, avoiding an
     * early-exit comparison of the stored and freshly derived digests.
     * The mismatch decision is made only after the full digest is examined. */
    UINT8 diff = 0;
    UINTN i;
    for (i = 0; i < sizeof(digest); i++) diff |= digest[i] ^ g_record.digest[i];
    if (diff != 0) goto failed;
  }
  memset(digest, 0, sizeof(digest));
  g_logged_in = 1;
  g_failures = 0;
  g_lock_until = 0;
  desktop_start();
  kputs("[SESSION] login accepted\n");
  return 0;
failed:
  memset(digest, 0, sizeof(digest));
  g_failures++;
  kputs("[SESSION] login rejected\n");
  if (g_failures >= SESSION_MAX_FAILURES) {
    g_lock_until = sched_ticks() + SESSION_LOCK_TICKS;
    g_failures = 0;
    kputs("[SESSION] too many attempts; locked for 30 seconds\n");
  }
  return -1;
}

int session_power_allowed(UINT64 caller_pid) {
  task_t *manager = sched_find_name("session");
  return g_logged_in || (manager != NULL && manager->pid == caller_pid);
}
