/**
 * Novus OS - a ring-3 client of the window manager (M6).
 *
 * The program below is hand-assembled machine code, in the same spirit as
 * the idle/spin/selftest programs in kernel/nuc.c: M6 has no C compiler
 * for user space yet (that is M7), so the demonstration client is written
 * out byte by byte. It is a real ring-3 process with its own address
 * space; the only way it reaches the WM is the syscall gate plus the
 * protocol in nwm_proto.h.
 *
 * What it does, all over IPC:
 *
 *   getpid()                  -> its own inbox port id
 *   ipc_lookup("nwm")         -> the WM port
 *   ipc_send(CREATE, ...)     -> ask for 280x150 at (560,140)
 *   ipc_recv(block)           -> block until the WM answers with the id
 *   ipc_send(DRAW, 3 cmds)    -> a title-ish bar, a colour bar, a border
 *   loop:
 *     ipc_send(MOVE, x += 24) -> walk the window left to right, then wrap
 *     ipc_recv(nowait)        -> any input event the WM forwarded?
 *     if yes: cycle the colour bar and re-send DRAW
 *     yield                   -> pace the loop at the scheduler's slice
 *
 * The data block lives at a fixed address at the start of the segment and
 * the code after it, so every immediate in the instruction stream is a
 * constant known here (see the layout table).
 */

#include "kernel.h"
#include "nuc.h"
#include "nd.h"
#include "nwm.h"
#include "nwm_proto.h"

#if CONFIG_NOVUS_WM

/* Segment layout: virtual address 0x400000, the address mochi links at.
 *
 *   0x00  "nwm"                  port name for ipc_lookup
 *   0x08  nwm_create_t  (48 B)   CREATE payload, title at +24
 *   0x38  nwm_move_t    (12 B)   win patched at runtime, x patched
 *   0x44  nwm_draw_t    (56 B)   win patched, 3 commands
 *   0x7C  buffer        (160 B)  recv: header + payload
 *   0x140 code                   entry point (offset 320)
 */
#define CLIENT_BASE    0x400000u
#define CLIENT_NAME    0x400000u
#define CLIENT_CREATE  0x400008u
#define CLIENT_MOVE    0x400038u
#define CLIENT_DRAW    0x400044u
#define CLIENT_BUF     0x40007Cu
#define CLIENT_BUF_WIN (CLIENT_BUF + 32u) /* win id: first payload field */
#define CLIENT_CODE    0x400140u

/* The program. Every byte is deliberate; the comments give the offsets
 * the jumps were assembled against. Syscall ABI (README part 8):
 * rax = number, rdi / rsi / rdx / r10 = arguments, rax = result. */
static const UINT8 client_code[] = {
    /* 0x00 */ 0xB8, 0x03, 0x00, 0x00, 0x00, /* mov eax, 3 (getpid) */
    /* 0x05 */ 0x0F, 0x05,                   /* syscall */
    /* 0x07 */ 0x49, 0x89, 0xC3,             /* mov r13, rax: inbox */
    /* 0x0A */ 0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, 6 (lookup) */
    /* 0x0F */ 0x48, 0xBF,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, /* rdi = "nwm" */
    /* 0x19 */ 0x0F, 0x05,                   /* syscall */
    /* 0x1B */ 0x49, 0x89, 0xC7,             /* mov r15, rax: WM port */

    /* CREATE */
    /* 0x1E */ 0xB8, 0x04, 0x00, 0x00, 0x00, /* mov eax, 4 (send) */
    /* 0x23 */ 0x4C, 0x89, 0xFF,             /* mov rdi, r15 */
    /* 0x26 */ 0xBE, 0x01, 0x01, 0x00, 0x00, /* mov esi, NWM_REQ_CREATE */
    /* 0x2B */ 0x48, 0xBA,
    0x08, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, /* rdx = payload */
    /* 0x35 */ 0x41, 0xBA, 0x30, 0x00, 0x00, 0x00, /* r10d = 48 */
    /* 0x3B */ 0x0F, 0x05,                   /* syscall */

    /* wait for the reply that carries the window id */
    /* 0x3D */ 0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, 5 (recv) */
    /* 0x42 */ 0x4C, 0x89, 0xEF,             /* mov rdi, r13 */
    /* 0x45 */ 0x48, 0xBE,
    0x7C, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, /* rsi = buffer */
    /* 0x4F */ 0xBA, 0xA0, 0x00, 0x00, 0x00, /* mov edx, 160 */
    /* 0x54 */ 0x41, 0xBA, 0x04, 0x00, 0x00, 0x00, /* r10d = IPC_F_BLOCK */
    /* 0x5A */ 0x0F, 0x05,                   /* syscall */

    /* win id sits 32 bytes into the buffer: patch both payloads */
    /* 0x5C */ 0x8B, 0x04, 0x25,
    0x9C, 0x00, 0x40, 0x00,                  /* mov eax, [0x40009c] */
    /* 0x63 */ 0x89, 0x04, 0x25,
    0x38, 0x00, 0x40, 0x00,                  /* mov [0x400038], eax */
    /* 0x6A */ 0x89, 0x04, 0x25,
    0x44, 0x00, 0x40, 0x00,                  /* mov [0x400044], eax */
    /* 0x71 */ 0xBB, 0x30, 0x02, 0x00, 0x00, /* mov ebx, 560 (x) */

    /* first DRAW */
    /* 0x76 */ 0xB8, 0x04, 0x00, 0x00, 0x00, /* mov eax, 4 */
    /* 0x7B */ 0x4C, 0x89, 0xFF,             /* mov rdi, r15 */
    /* 0x7E */ 0xBE, 0x0A, 0x01, 0x00, 0x00, /* mov esi, NWM_REQ_DRAW */
    /* 0x83 */ 0x48, 0xBA,
    0x44, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, /* rdx = draw */
    /* 0x8D */ 0x41, 0xBA, 0x38, 0x00, 0x00, 0x00, /* r10d = 56 */
    /* 0x93 */ 0x0F, 0x05,                   /* syscall */

    /* loop: */
    /* 0x95 */ 0x89, 0x1C, 0x25,
    0x3C, 0x00, 0x40, 0x00,                  /* mov [0x40003c], ebx: x */
    /* 0x9C */ 0xB8, 0x04, 0x00, 0x00, 0x00, /* mov eax, 4 */
    /* 0xA1 */ 0x4C, 0x89, 0xFF,             /* mov rdi, r15 */
    /* 0xA4 */ 0xBE, 0x03, 0x01, 0x00, 0x00, /* mov esi, NWM_REQ_MOVE */
    /* 0xA9 */ 0x48, 0xBA,
    0x38, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, /* rdx = move */
    /* 0xB3 */ 0x41, 0xBA, 0x0C, 0x00, 0x00, 0x00, /* r10d = 12 */
    /* 0xB9 */ 0x0F, 0x05,                   /* syscall */

    /* poll the inbox for input the WM forwarded */
    /* 0xBB */ 0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, 5 */
    /* 0xC0 */ 0x4C, 0x89, 0xEF,             /* mov rdi, r13 */
    /* 0xC3 */ 0x48, 0xBE,
    0x7C, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, /* rsi = buffer */
    /* 0xCD */ 0xBA, 0xA0, 0x00, 0x00, 0x00, /* mov edx, 160 */
    /* 0xD2 */ 0x45, 0x31, 0xD2,             /* xor r10d, r10d: no block */
    /* 0xD5 */ 0x0F, 0x05,                   /* syscall */
    /* 0xD7 */ 0x48, 0x85, 0xC0,             /* test rax, rax */
    /* 0xDA */ 0x7E, 0x26,                   /* jle 0x102 (no event) */
    /* 0xDC */ 0xFE, 0x04, 0x25,
    0x68, 0x00, 0x40, 0x00,                  /* inc byte [0x400068] */
    /* 0xE3 */ 0xB8, 0x04, 0x00, 0x00, 0x00, /* mov eax, 4 */
    /* 0xE8 */ 0x4C, 0x89, 0xFF,             /* mov rdi, r15 */
    /* 0xEB */ 0xBE, 0x0A, 0x01, 0x00, 0x00, /* mov esi, NWM_REQ_DRAW */
    /* 0xF0 */ 0x48, 0xBA,
    0x44, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, /* rdx = draw */
    /* 0xFA */ 0x41, 0xBA, 0x38, 0x00, 0x00, 0x00, /* r10d = 56 */
    /* 0x100 */ 0x0F, 0x05,                  /* syscall */

    /* 0x102: x += 24; if x > 936 wrap to 560 */
    /* 0x102 */ 0x83, 0xC3, 0x18,           /* add ebx, 24 */
    /* 0x105 */ 0x81, 0xFB, 0xA8, 0x03, 0x00, 0x00, /* cmp ebx, 936 */
    /* 0x10B */ 0x7E, 0x05,                 /* jle 0x112 */
    /* 0x10D */ 0xBB, 0x30, 0x02, 0x00, 0x00, /* mov ebx, 560 */

    /* 0x112: yield, so the loop runs at the scheduler's pace */
    /* 0x112 */ 0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, 1 (yield) */
    /* 0x117 */ 0x0F, 0x05,                  /* syscall */
    /* 0x119 */ 0xE9, 0x77, 0xFF, 0xFF, 0xFF, /* jmp 0x95 (loop) */
};

static void fill_data(UINT8 *seg) {
  nwm_create_t *c = (nwm_create_t *)(seg + (CLIENT_CREATE - CLIENT_BASE));
  nwm_move_t   *mv = (nwm_move_t *)(seg + (CLIENT_MOVE - CLIENT_BASE));
  nwm_draw_t   *dr = (nwm_draw_t *)(seg + (CLIENT_DRAW - CLIENT_BASE));

  memset(seg, 0, (UINT32)(CLIENT_CODE - CLIENT_BASE));
  memcpy(seg, NWM_PORT_NAME, sizeof(NWM_PORT_NAME));

  c->win = 0; /* 0 = let the WM assign the id */
  c->x = 560;
  c->y = 140;
  c->w = 280;
  c->h = 150;
  memcpy(c->title, "IPC client", 10);

  mv->win = 0; /* patched by the program once the id is known */
  mv->x = 560;
  mv->y = 140;

  dr->win = 0;
  dr->count = 3;
  /* A "content" that says which process drew it: a header bar, a colour
   * bar that cycles whenever the WM forwards an input event, and a
   * border just inside the client area. */
  dr->cmd[0].op = NWM_OP_FILL;
  dr->cmd[0].x = 6;
  dr->cmd[0].y = 6;
  dr->cmd[0].w = 120;
  dr->cmd[0].h = 22;
  dr->cmd[0].color = ND_RGB(0x2F, 0x5F, 0x8F);

  dr->cmd[1].op = NWM_OP_FILL;
  dr->cmd[1].x = 6;
  dr->cmd[1].y = 36;
  dr->cmd[1].w = 180;
  dr->cmd[1].h = 18;
  dr->cmd[1].color = ND_RGB(0x20, 0x50, 0x80);

  dr->cmd[2].op = NWM_OP_FRAME;
  dr->cmd[2].x = 0;
  dr->cmd[2].y = 0;
  dr->cmd[2].w = 276;
  dr->cmd[2].h = 123;
  dr->cmd[2].pad2 = 1;
  dr->cmd[2].color = ND_RGB(0x4C, 0x8D, 0xFF);
}

int nwm_spawn_client(void) {
  static UINT8 seg[1024];
  UINT32       total;

  if (sizeof(client_code) > sizeof(seg) - (CLIENT_CODE - CLIENT_BASE)) {
    return -1;
  }
  fill_data(seg);
  memcpy(seg + (CLIENT_CODE - CLIENT_BASE), client_code, sizeof(client_code));
  total = (UINT32)(CLIENT_CODE - CLIENT_BASE) + (UINT32)sizeof(client_code);
  if (nuc_spawn_blob("wmclient", seg, total,
                     (UINT32)(CLIENT_CODE - CLIENT_BASE)) != 0) {
    kputs("[NWM] could not start the ring-3 client\n");
    return -1;
  }
  kputs("[NWM] ring-3 client started, "
        "it will ask for a window over IPC\n");
  return 0;
}

#else /* !CONFIG_NOVUS_WM */

int nwm_spawn_client(void) { return -1; }

#endif
