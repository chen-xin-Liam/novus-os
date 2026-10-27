// SPDX-License-Identifier: GPL-2.0
/*
 * novus_syscall.h — novus-os 系统调用号表与调用约定
 *
 * ====================== 权威来源（2026-10-24 扶正） ======================
 * 旧版基于 novus-live2.iso **逆向推测**，号表/入口/寄存器几乎全猜错
 * （曾误判为 int 0x80 中断门、号从 0 连排、exit=7）。
 * 现以 liam 官方《Novus-WM 与 IPC 协议规范 V1.0》+ DEMO.NUC 实证为准：
 *   · 规范：https://github.com/chen-xin-Liam/novus-os/blob/main/docs/
 *            Novus-WM%20%E4%B8%8E%20IPC%20%E5%8D%8F%E8%AE%AE%E8%A7%84%E8%8C%83%20V1.0.md
 *   · DEMO.NUC 实证：mov eax,2; syscall (write) / mov eax,60; syscall (exit)
 *
 * 入口：x86_64 `syscall` 指令（MSR_LSTAR → syscall_entry），**不是** int 0x80。
 * 参数：rax=调用号，rdi / rsi / rdx / r10（第 4 参是 r10，因 syscall 占 rcx/r11）。
 * 返回：rax，负值=错误码。
 *
 * 编译器所有 syscall 相关代码只依赖本文件的宏，不出现裸数字；内核改号只改这里。
 */

/* 号表已被官方规范 + DEMO.NUC 实证确认：1 = 已确认 */
#define NUOMI_SYSCALL_SPEC_CONFIRMED 1

/* ---------- 系统调用号 ---------- */
/* 规范 §2 表格：已实现并运行（1-7 + 60） */
#define NOVUS_SYS_YIELD      1u   /* 让出 CPU */
#define NOVUS_SYS_WRITE      2u   /* (fd, buf, len) -> written */
#define NOVUS_SYS_GETPID     3u   /* () -> pid（= 自己的收件端口号） */
#define NOVUS_SYS_IPC_SEND   4u   /* (port, type|flags<<16, payload, len) */
#define NOVUS_SYS_IPC_RECV   5u   /* (port, buf, cap, flags) */
#define NOVUS_SYS_IPC_LOOKUP 6u   /* (name) -> port */
#define NOVUS_SYS_IPC_STATS  7u   /* (rsv, buf, cap) */
#define NOVUS_SYS_EXIT       60u  /* (code)，不返回 */

/* liam 提供、**不在 M6 规范 §2 表格内**——用前须与 liam 确认内核已实现，
 * 否则 syscall 分发器会报 "unknown syscall" */
#define NOVUS_SYS_OPEN       8u   /* 待确认 */
#define NOVUS_SYS_READ       9u   /* 待确认 */
#define NOVUS_SYS_FWRITE     10u  /* 待确认 */
#define NOVUS_SYS_CLOSE      11u  /* 待确认 */
#define NOVUS_SYS_STAT       12u  /* 待确认 */
#define NOVUS_SYS_OPENDIR    13u  /* 待确认 */
#define NOVUS_SYS_READDIR    14u  /* 待确认 */
#define NOVUS_SYS_MKDIR      15u  /* 待确认 */
#define NOVUS_SYS_POWEROFF   16u  /* 待确认 */
#define NOVUS_SYS_REBOOT     17u  /* 待确认 */
#define NOVUS_SYS_SESSION    18u  /* 待确认 */
#define NOVUS_SYS_SETUP      19u  /* 待确认 */

/* 名字表里没有的号 → 内核会报 "unknown syscall" */
#define NOVUS_SYS_UNKNOWN    (-1)

/* ---------- 调用约定 ---------- */
#define NOVUS_SYSCALL_REG_NUM     "rax"                 /* rax = 调用号 */
#define NOVUS_SYSCALL_ARG_REGS    "rdi, rsi, rdx, r10"  /* 第 4 参是 r10！ */
#define NOVUS_SYSCALL_INSTR       "syscall"             /* 入口指令（非 int 0x80） */

/* ---------- 各服务参数个数（供生成调用点） ---------- */
#define NOVUS_ARGC_YIELD      0
#define NOVUS_ARGC_WRITE      3   /* (fd, buf, len) */
#define NOVUS_ARGC_GETPID     0
#define NOVUS_ARGC_IPC_SEND   4   /* (port, type|flags<<16, payload, len) */
#define NOVUS_ARGC_IPC_RECV   4   /* (port, buf, cap, flags) */
#define NOVUS_ARGC_IPC_LOOKUP 1   /* (name) */
#define NOVUS_ARGC_IPC_STATS  3   /* (rsv, buf, cap) */
#define NOVUS_ARGC_EXIT       1   /* (code) */

/* 写终端用的标准描述符 */
#define NOVUS_FD_STDOUT       1
#define NOVUS_FD_STDERR       2

/* ---------- IPC 常量（规范 §3，供将来编译 IPC 程序参考） ---------- */
#define NOVUS_IPC_MAGIC        0x4E495043u  /* "NIPC" */
#define NOVUS_IPC_PAYLOAD_MAX  128u          /* 载荷上限（字节） */
#define NOVUS_IPC_QUEUE_LEN    8u            /* 每端口队列深度 */
#define NOVUS_IPC_PORT_BASE    0x1000u       /* 命名端口号段起点 */
#define NOVUS_WM_PORT_NAME     "nwm"         /* Novus-WM 端口名 */
#define NOVUS_WM_PROTO_VER     0x00010000u   /* WM 协议版本 1.0 */
