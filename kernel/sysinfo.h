/**
 * Novus OS - sysinfo service (M8).
 *
 * One port that answers questions about the machine: the scheduler's task
 * table, the built-in file tree, memory and IPC counters, the desktop
 * accent. The three M8 applications are pure ring-3 clients; keeping the
 * answers behind IPC means they need no special syscall and no header
 * from the kernel - only this protocol.
 *
 * Transport is the M6 IPC format (kernel/ipc.h): requests carry the
 * structs below as payload, replies come back on the caller's inbox with
 * one of the RSP_* types. Lists are paged (three records per reply) so
 * every answer fits in IPC_PAYLOAD_MAX without a second message format.
 */

#ifndef NOVUS_SYSINFO_H
#define NOVUS_SYSINFO_H

#include "uefi.h"

#define SYSINFO_PORT_NAME "sys"

/* Requests: client -> service. */
#define SYSINFO_REQ_TASKS  0x0301u
#define SYSINFO_REQ_FILES  0x0302u
#define SYSINFO_REQ_STATS  0x0303u
#define SYSINFO_REQ_ACCENT 0x0304u
#define SYSINFO_REQ_SPAWN  0x0305u

/* Replies. Distinct types per query so a client can match its answer
 * without a correlation id. */
#define SYSINFO_RSP_OK     0x0310u
#define SYSINFO_RSP_TASKS  0x0311u
#define SYSINFO_RSP_FILES  0x0312u
#define SYSINFO_RSP_STATS  0x0313u
#define SYSINFO_RSP_ACCENT 0x0314u
#define SYSINFO_RSP_ERR    0x03FFu

/* Directories the file manager can browse. */
#define SYSINFO_DIR_APPS 0u
#define SYSINFO_DIR_DEV  1u
#define SYSINFO_DIR_PROC 2u

/* Entry kinds. */
#define SYSINFO_KIND_APP  1u
#define SYSINFO_KIND_DEV  2u
#define SYSINFO_KIND_VIRT 3u

#define SYSINFO_RECS 3u

typedef struct {
  UINT16 total; /* entries in this directory */
  UINT8  n;     /* records in this reply */
  UINT8  pad;
} sysinfo_list_t;

typedef struct {
  UINT64 pid;
  UINT32 ticks;
  INT32  exit_code;
  UINT8  state;
  UINT8  slice;
  char   name[14];
} sysinfo_task_t;

typedef struct {
  sysinfo_list_t h;
  sysinfo_task_t rec[SYSINFO_RECS];
} sysinfo_tasks_rsp_t;

typedef struct {
  UINT32 size;
  UINT8  kind;
  UINT8  pad[3];
  char   name[24];
} sysinfo_file_t;

typedef struct {
  sysinfo_list_t h;
  sysinfo_file_t rec[SYSINFO_RECS];
} sysinfo_files_rsp_t;

typedef struct {
  UINT64 uptime;   /* ticks since boot */
  UINT64 free_kb;
  UINT32 tasks;    /* live tasks */
  UINT32 windows;  /* windows the WM holds */
  UINT32 ipc_sent;
  UINT32 ipc_drop;
  UINT32 accent;
  UINT32 pad;
} sysinfo_stats_t;

typedef struct {
  UINT16 index;
  UINT8  max;
  UINT8  dir;
  UINT32 pad;
} sysinfo_list_req_t;

typedef struct {
  UINT32 rgb;
  UINT8  set; /* 0 = query, 1 = apply */
  UINT8  pad[3];
} sysinfo_accent_t;

/* Create the port. Call once, after the scheduler owns a task table. */
void sysinfo_init(void);

/* Timer tick: answer queued requests (bounded work per tick). */
void sysinfo_tick(void);

#endif /* NOVUS_SYSINFO_H */
