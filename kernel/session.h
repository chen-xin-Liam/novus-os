#ifndef NOVUS_SESSION_H
#define NOVUS_SESSION_H

#include "uefi.h"

#define SESSION_OP_STATUS 0u
#define SESSION_OP_SETUP  1u
#define SESSION_OP_LOGIN  2u
#define SESSION_OP_LOGOUT 3u

#define SESSION_NAME_MAX 31u
#define SESSION_PASS_MAX 63u

typedef struct {
  UINT8 op;
  UINT8 name_len;
  UINT8 pass_len;
  UINT8 pad;
  char username[SESSION_NAME_MAX + 1u];
  char password[SESSION_PASS_MAX + 1u];
} session_req_t;

_Static_assert(sizeof(session_req_t) == 100u, "session request ABI drift");

void session_init(void);
int session_handle(const session_req_t *req, UINT64 caller_pid);
int session_logged_in(void);
int session_power_allowed(UINT64 caller_pid);

#endif
