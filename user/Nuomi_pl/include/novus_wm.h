// SPDX-License-Identifier: GPL-2.0
/*
 * novus_wm.h — Novus-WM 窗口协议常量与载荷结构（供 Nuomi 窗口原语使用）
 *
 * 依据 liam《Novus-WM 与 IPC 协议规范 V1.0》：
 *   https://github.com/chen-xin-Liam/novus-os/blob/main/docs/
 *   Novus-WM%20%E4%B8%8E%20IPC%20%E5%8D%8F%E8%AE%AE%E8%A7%84%E8%8C%83%20V1.0.md
 *
 * 客户端（Nuomi 窗口原语）经内核 IPC 与 WM 通信：
 *   ipc_lookup("nwm")                       -> WM 端口号
 *   ipc_send(port, type|flags<<16, payload, len)  -> 发请求
 *   ipc_recv(port, buf, cap, flags)         -> 收应答/事件
 *
 * NIPC 消息头（magic / type / flags / sender / target / len / checksum）由
 * **内核**填写（sender 不可伪造、交付前重算校验和），客户端只需给出 type 与
 * 载荷，无需手搓 FNV-1a 校验和——这比规范初看简单得多。
 *
 * ⚠️ 各载荷结构（nwm_create_t / nwm_shape_t / nwm_text_t …）规范只给了语义、
 *    未给字段偏移；下面按语义给一种 8 字节对齐的合理布局，统一标 [布局待确认]。
 *    真机联调前必须与 liam 核对字段偏移/宽度，否则载荷对不上。
 */
#ifndef NOVUS_WM_H
#define NOVUS_WM_H

#include <stdint.h>

/* ====================== IPC 常量（规范 §3） ====================== */
#define NIPC_MAGIC          0x4E495043u   /* "NIPC" */
#define NIPC_PAYLOAD_MAX    128u          /* 载荷上限（字节） */
#define NIPC_QUEUE_LEN      8u            /* 每端口队列深度 */
#define IPC_PORT_BASE       0x1000u       /* 命名端口号段起点 */
#define NOVUS_WM_PORT_NAME  "nwm"         /* Novus-WM 端口名 */
#define NOVUS_WM_PROTO_VER  0x00010000u   /* WM 协议版本 1.0 */

/* ipc_send 第 2 参 rsi = type | (flags << 16) */
#define IPC_MSG_TYPE(t)     ((uint32_t)(t) & 0xFFFFu)
#define IPC_MSG_FLAGS(f)    (((uint32_t)(f) & 0xFFFFu) << 16)
#define IPC_MSG_TF(t, f)    (IPC_MSG_TYPE(t) | IPC_MSG_FLAGS(f))

/* ====================== WM 请求类型（规范 §4.1） ====================== */
#define NWM_CREATE     0x0101
#define NWM_DESTROY    0x0102
#define NWM_MOVE       0x0103
#define NWM_RESIZE     0x0104
#define NWM_RAISE      0x0105
#define NWM_MINIMIZE   0x0106
#define NWM_MAXIMIZE   0x0107
#define NWM_RESTORE    0x0108
#define NWM_FOCUS      0x0109
#define NWM_DRAW       0x010A
#define NWM_TITLE      0x010B
#define NWM_EFFECT     0x010C
#define NWM_CLEAR      0x010D
#define NWM_SHAPE      0x010E
#define NWM_TEXT       0x010F
#define NWM_INVALIDATE 0x0110

/* ====================== 应答 / 事件（规范 §4.2 / §4.3） ====================== */
#define NWM_RSP_OK     0x0001
#define NWM_RSP_ERR    0x0002
#define NWM_EV_CREATED 0x0201
#define NWM_EV_MOUSE   0x0202
#define NWM_EV_KEY     0x0203
#define NWM_EV_FOCUS   0x0204
#define NWM_EV_RESIZE  0x0205
#define NWM_EV_CLOSE   0x0206

/* ====================== 图元 op（规范 §4.1 + v1.2 圆角） ====================== */
#define NWM_OP_FILL         1   /* 填充矩形 */
#define NWM_OP_FRAME        2   /* 边框矩形 */
#define NWM_OP_HLINE        3   /* 横线 */
#define NWM_OP_VLINE        4   /* 竖线 */
#define NWM_OP_CIRCLE       5   /* 圆 */
#define NWM_OP_ROUND_FILL   6   /* 圆角填充（半径在 pad） */
#define NWM_OP_ROUND_FRAME  7   /* 圆角描边（半径在 pad） */

/* ====================== 载荷结构（[布局待与 liam 确认]） ====================== */

/* 建窗：win=0 表示由 WM 分配；CREATE 总是回执窗口号 */
typedef struct {
    uint32_t win;       /* 0 = 请求 WM 分配 */
    uint32_t x, y;      /* 位置（客户区左上角） */
    uint32_t w, h;      /* 尺寸 */
} nwm_create_t;         /* 20 字节 [布局待确认] */

/* 仅窗口号：DESTROY/RAISE/MINIMIZE/MAXIMIZE/RESTORE/FOCUS/CLEAR/INVALIDATE */
typedef struct {
    uint32_t win;
} nwm_win_t;

/* 追加一个图元（SHAPE） */
typedef struct {
    uint32_t win;
    uint8_t  op;        /* NWM_OP_* */
    uint8_t  pad;       /* 圆角半径 / 描边厚度 */
    uint16_t rsvd;
    int32_t  x, y;      /* 相对客户区左上角 */
    uint32_t w, h;
    uint32_t color;
} nwm_shape_t;          /* [布局待确认] */

/* 追加一段文字（TEXT，≤96 字节，scale 1/2） */
typedef struct {
    uint32_t win;
    int32_t  x, y;
    uint32_t scale;     /* 1 或 2 */
    uint32_t len;       /* text 字节数 */
    char     text[96];
} nwm_text_t;           /* [布局待确认] */

/* 鼠标事件（EV_MOUSE，客户区坐标） */
typedef struct {
    uint32_t win;
    int32_t  x, y;
    uint32_t buttons;   /* 按下/抬起状态 [位定义待确认] */
} nwm_ev_mouse_t;

/* 按键事件（EV_KEY） */
typedef struct {
    uint32_t win;
    uint32_t key;       /* 键码 [编码待确认] */
} nwm_ev_key_t;

#endif /* NOVUS_WM_H */
