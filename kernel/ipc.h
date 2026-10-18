/**
 * Novus OS - kernel-assisted IPC (M6, README part 10.2-10.3).
 *
 * Point-to-point messages between processes, carried by the kernel. A
 * "port" is the addressable end of a queue; every task owns exactly one
 * inbox port whose id equals its pid, which is what makes a reply
 * possible: a server sends to hdr.sender and lands in the caller's queue.
 *
 * Message = header + payload + checksum, as the README sketches it. The
 * kernel stamps sender, target, length and checksum on the copy it makes,
 * so a client cannot forge another process's identity, cannot claim a
 * length it did not send, and a corrupted queue entry is rejected by
 * ipc_recv() instead of being delivered.
 *
 * Queues are bounded (IPC_QUEUE_LEN) on purpose: a sender that outruns a
 * receiver gets IPC_EBUSY instead of eating kernel memory - the README's
 * "queue length limit against message floods". No allocation happens on
 * the send path at all; the port table is static.
 *
 * Blocking: ipc_recv(block=1) parks the calling task on the port and
 * returns IPC_ERETRY. The syscall layer rewrites the return frame's RIP to
 * point back at the `syscall` instruction, so the task re-executes the
 * call when a message wakes it. That keeps the blocking primitive out of
 * the scheduler: no continuation state, just a rewound frame.
 */

#ifndef NOVUS_IPC_H
#define NOVUS_IPC_H

#include "uefi.h"

#define IPC_MAGIC       0x4E495043u /* "NIPC" */
#define IPC_HDR_SIZE    32u
#define IPC_PAYLOAD_MAX 128u
#define IPC_QUEUE_LEN   8u
#define IPC_MAX_PORTS   16u
#define IPC_NAME_MAX    16u

/* Named ports start here so they can never collide with a task pid,
 * which is also an inbox port id. */
#define IPC_PORT_BASE 0x1000ull

/* Header flags. */
#define IPC_F_SYNC  0x0001u /* sender expects a reply (protocol level) */
#define IPC_F_REPLY 0x0002u /* this message answers an earlier one */
#define IPC_F_BLOCK 0x0004u /* ipc_recv: park until a message arrives */
#define IPC_F_NOWAIT 0x0000u

/* Return codes. Negative, so a payload length can be returned directly. */
#define IPC_OK         0
#define IPC_EBADF     (-1)
#define IPC_EBADMSG   (-2)
#define IPC_EMSGSIZE  (-3)
#define IPC_EBUSY     (-4)
#define IPC_ENOENT    (-5)
#define IPC_EAGAIN    (-6)
#define IPC_ERETRY    (-7) /* internal: task parked, call must be retried */
#define IPC_ETIMEDOUT (-8)

/* Wire header. Field order is the README's IpcMsgHeader. */
typedef struct {
  UINT32 magic;
  UINT16 type;
  UINT16 flags;
  UINT64 sender;
  UINT64 target;
  UINT32 len;
  UINT32 checksum;
} ipc_hdr_t;

typedef struct {
  ipc_hdr_t hdr;
  UINT8     payload[IPC_PAYLOAD_MAX];
} ipc_msg_t;

typedef struct {
  UINT32 sent;
  UINT32 delivered;
  UINT32 dropped;  /* receiver queue full */
  UINT32 bad;      /* rejected: bad magic, length or checksum */
  UINT32 blocked;  /* tasks parked by a blocking receive */
  UINT32 woke;     /* blocked tasks woken by a message */
} ipc_stats_t;

void ipc_init(void);

/* Create a port owned by `owner`. A non-empty name makes it findable by
 * ipc_port_lookup(); `id_hint` 0 asks the kernel to allocate one. */
int ipc_port_create(const char *name, UINT64 owner, UINT64 id_hint,
                    UINT64 *out_id);

/* Resolve a name ("nwm") to a port id. Returns IPC_ENOENT when unused. */
int ipc_port_lookup(const char *name, UINT64 *out_id);

/* Give a new task its inbox: a port whose id is the pid, so a server can
 * answer hdr.sender without knowing anything else about the caller. */
int ipc_task_attach(UINT64 pid);

/* Destroy every port owned by `pid`, and drop tasks parked on them. */
void ipc_owner_exit(UINT64 pid);

/* Queue one message. `payload` may be NULL when len is 0. Wakes a task
 * parked on the destination. Returns IPC_OK or a negative code. */
int ipc_send(UINT64 dst_port, UINT64 sender, UINT16 type, UINT16 flags,
             const void *payload, UINT32 len);

/* Pop one message. Returns the payload length (>= 0) or a negative code.
 * With `block` set, an empty queue parks the caller and returns
 * IPC_ERETRY; the caller (the syscall layer) must rewind and retry. */
int ipc_recv(UINT64 port, ipc_msg_t *out, int block);

/* Non-blocking convenience used by kernel servers: 0 when a message was
 * taken, IPC_EAGAIN when the queue is empty. */
int ipc_try_recv(UINT64 port, ipc_msg_t *out);

const ipc_stats_t *ipc_stats(void);
void ipc_selftest(void);

#endif /* NOVUS_IPC_H */
