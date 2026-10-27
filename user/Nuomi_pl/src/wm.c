// SPDX-License-Identifier: GPL-2.0
/*
 * wm.c — Nuomi 窗口原语（Novus-WM / IPC 客户端侧）
 *
 * 定位：Nuomi 为 novus 上的用户级 GUI 应用语言，窗口能力最终走 Novus-WM 协议
 *       （include/novus_wm.h + 规范 V1.0）。
 *
 * 三套后端：
 *   ① Windows（#ifdef _WIN32）：**Win32 真窗口**——CreateWindowExA 建窗、GDI 双缓冲
 *      绘制（fill/frame/circle/round/text）、消息泵嵌进 window-event、方向键映射符号。
 *      文本走 A 版 API（ANSI）：ASCII 完美；中文需 UTF-8→GBK/Wide 转换，TCC 的
 *      winnls.h 不全，暂不支持（待换带完整 SDK 的编译器再补）。
 *   ② 其它宿主（#else）：内存"假 WM"，消息摘要打 stderr，event 恒 nil。
 *   ③ 真机 novus（⬜ 待接）：内核 ipc_lookup("nwm") + ipc_send/ipc_recv，NIPC 头由
 *      内核填，客户端只构造 type+载荷。需 codegen/nucutil 发射 ipc syscall。
 */
#include "nuomi.h"
#include "novus_wm.h"
#include <string.h>
#include <stdlib.h>

static int g_wm_trace = 0;

#ifdef _WIN32
/* ====================== Win32 真窗口后端（A 版 API） ====================== */
#include <windows.h>

#define W32_MAX   16
#define W32_KQ    256
#define W32_DL0   64

typedef struct {
    int op, x, y, w, h; uint32_t color; int radius;
    int is_text; int tx, ty, scale; char text[96];
} W32Cmd;

typedef struct {
    int used; uint32_t id; HWND hwnd; int w, h;
    W32Cmd* dl; int dn, dcap;
    int keyq[W32_KQ]; int kqh, kqt;
    int want_close;
} W32Win;

static W32Win   g_w32[W32_MAX];
static uint32_t g_w32_next = 1;
static uint32_t g_w32_cur  = 0;
static int      g_w32_class = 0;

static void w32_pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}
static W32Win* w32_by_hwnd(HWND h) { for (int i=0;i<W32_MAX;i++) if (g_w32[i].used && g_w32[i].hwnd==h) return &g_w32[i]; return NULL; }
static W32Win* w32_by_id(uint32_t id){ for (int i=0;i<W32_MAX;i++) if (g_w32[i].used && g_w32[i].id==id) return &g_w32[i]; return NULL; }
static void w32_pushkey(W32Win* W, int vk){ int n=(W->kqt+1)%W32_KQ; if(n!=W->kqh){ W->keyq[W->kqt]=vk; W->kqt=n; } }
static int  w32_popkey(W32Win* W){ if(W->kqh==W->kqt) return 0; int v=W->keyq[W->kqh]; W->kqh=(W->kqh+1)%W32_KQ; return v; }

static LRESULT CALLBACK w32_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        LPCREATESTRUCT cs = (LPCREATESTRUCT)lp;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_KEYDOWN: {
        W32Win* W = w32_by_hwnd(hwnd);
        if (W) w32_pushkey(W, (int)wp);
        return 0;
    }
    case WM_CLOSE: {
        W32Win* W = w32_by_hwnd(hwnd);
        if (W) W->want_close = 1;
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        return 0;
    case WM_PAINT: {
        W32Win* W = w32_by_hwnd(hwnd);
        if (W && W->hwnd) {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, W->w, W->h);
            HGDIOBJ oldbmp = SelectObject(mem, bmp);
            RECT rc = {0, 0, W->w, W->h};
            HBRUSH bg = CreateSolidBrush(RGB(16,16,20));
            FillRect(mem, &rc, bg); DeleteObject(bg);
            for (int i = 0; i < W->dn; i++) {
                W32Cmd* c = &W->dl[i];
                COLORREF col = RGB((c->color>>16)&0xFF, (c->color>>8)&0xFF, c->color&0xFF);
                if (c->is_text) {
                    SetBkMode(mem, TRANSPARENT);
                    SetTextColor(mem, col);
                    int fs = (c->scale >= 2) ? 22 : 13;
                    HFONT f = CreateFontA(fs,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
                                          OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,
                                          FIXED_PITCH|FF_DONTCARE,"Consolas");
                    HGDIOBJ of = SelectObject(mem, f);
                    TextOutA(mem, c->tx, c->ty, c->text, (int)strlen(c->text));
                    SelectObject(mem, of); DeleteObject(f);
                } else {
                    RECT r = { c->x, c->y, c->x + c->w, c->y + c->h };
                    switch (c->op) {
                    case 1: { HBRUSH b=CreateSolidBrush(col); FillRect(mem,&r,b); DeleteObject(b);} break;
                    case 2: { HBRUSH b=CreateSolidBrush(col); FrameRect(mem,&r,b); DeleteObject(b);} break;
                    case 5: {
                        HPEN p=CreatePen(PS_SOLID,1,col); HGDIOBJ op=SelectObject(mem,p);
                        HGDIOBJ ob=SelectObject(mem,GetStockObject(NULL_BRUSH));
                        int cx=c->x+c->w/2, cy=c->y+c->h/2;
                        Ellipse(mem, cx-c->w/2, cy-c->h/2, cx+c->w/2, cy+c->h/2);
                        SelectObject(mem,ob); SelectObject(mem,op); DeleteObject(p);
                    } break;
                    case 6: case 7: {
                        int rad = c->radius < 0 ? 0 : c->radius;
                        HPEN p = CreatePen(PS_SOLID, (c->op==7)?1:0, col); HGDIOBJ op=SelectObject(mem,p);
                        HBRUSH b = (c->op==6) ? CreateSolidBrush(col) : (HBRUSH)GetStockObject(NULL_BRUSH);
                        HGDIOBJ ob = SelectObject(mem, b);
                        RoundRect(mem, r.left, r.top, r.right, r.bottom, rad, rad);
                        SelectObject(mem,ob); SelectObject(mem,op); DeleteObject(p);
                        if (c->op==6) DeleteObject(b);
                    } break;
                    default: break;
                    }
                }
            }
            BitBlt(hdc, 0, 0, W->w, W->h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldbmp); DeleteObject(bmp); DeleteObject(mem);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static W32Win* w32_alloc(void) {
    for (int i=0;i<W32_MAX;i++) if (!g_w32[i].used) {
        W32Win* W = &g_w32[i];
        W->used = 1; W->id = g_w32_next++; W->hwnd = NULL; W->want_close = 0;
        W->dcap = W32_DL0; W->dn = 0; W->dl = (W32Cmd*)malloc(sizeof(W32Cmd)*W->dcap);
        W->kqh = W->kqt = 0;
        return W;
    }
    return NULL;
}
static void w32_dl_push(W32Win* W, W32Cmd c) {
    if (W->dn >= W->dcap) { W->dcap *= 2; W->dl = (W32Cmd*)realloc(W->dl, sizeof(W32Cmd)*W->dcap); }
    if (W->dl) W->dl[W->dn++] = c;
}

/* (window-create x y w h [title]) -> 窗口号 */
static Value w32Create(int c, Value* a, Env* e) {
    (void)e;
    if (c < 4 || !IS_NUM(a[0])||!IS_NUM(a[1])||!IS_NUM(a[2])||!IS_NUM(a[3])) {
        runtimeError("window-create expects (x y w h [title])"); return NIL_VAL;
    }
    W32Win* W = w32_alloc();
    if (!W) { runtimeError("window-create: too many windows"); return NIL_VAL; }
    W->w = (int)AS_NUM(a[2]); W->h = (int)AS_NUM(a[3]);
    char title[128] = "Nuomi";
    if (c >= 5 && IS_OBJ(a[4])) { char* t = valueToCString(a[4]); if (t){ strncpy(title,t,127); title[127]=0; free(t);} }
    if (!g_w32_class) {
        WNDCLASSEXA wc; memset(&wc,0,sizeof wc);
        wc.cbSize = sizeof wc; wc.lpfnWndProc = w32_wndproc;
        wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "NuomiWMClass";
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassExA(&wc); g_w32_class = 1;
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX; /* 固定大小 */
    RECT rc = {0, 0, W->w, W->h};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    HWND hwnd = CreateWindowExA(0, "NuomiWMClass", title, style,
                                (int)AS_NUM(a[0]), (int)AS_NUM(a[1]), ww, wh,
                                NULL, NULL, GetModuleHandleA(NULL), W);
    if (!hwnd) { W->used = 0; free(W->dl); W->dl = NULL; runtimeError("window-create: CreateWindow failed"); return NIL_VAL; }
    W->hwnd = hwnd; g_w32_cur = W->id;
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); w32_pump();
    if (g_wm_trace) fprintf(stderr, "[wm-win] CREATE id=%u %dx%d@(%d,%d)\n", W->id, W->w, W->h, (int)AS_NUM(a[0]),(int)AS_NUM(a[1]));
    return NUM_VAL((double)W->id);
}

static W32Win* w32_win(int c, Value* a) {
    if (c < 1 || !IS_NUM(a[0])) return NULL;
    W32Win* W = w32_by_id((uint32_t)AS_NUM(a[0]));
    return W ? W : (g_w32_cur ? w32_by_id(g_w32_cur) : NULL);
}

static Value w32Shape(int c, Value* a, Env* e) {
    (void)e;
    if (c < 7 || !IS_NUM(a[0]) || !IS_NUM(a[1])) { runtimeError("window-shape expects (win op x y w h color [radius])"); return NIL_VAL; }
    W32Win* W = w32_win(c, a);
    if (!W) { runtimeError("window-shape: unknown window"); return NIL_VAL; }
    W32Cmd cmd; memset(&cmd, 0, sizeof cmd);
    cmd.op = (int)AS_NUM(a[1]);
    cmd.x = IS_NUM(a[2])?(int)AS_NUM(a[2]):0;
    cmd.y = IS_NUM(a[3])?(int)AS_NUM(a[3]):0;
    cmd.w = IS_NUM(a[4])?(int)AS_NUM(a[4]):0;
    cmd.h = IS_NUM(a[5])?(int)AS_NUM(a[5]):0;
    cmd.color = IS_NUM(a[6])?(uint32_t)AS_NUM(a[6]):0;
    cmd.radius = (c>=8 && IS_NUM(a[7]))?(int)AS_NUM(a[7]):0;
    cmd.is_text = 0;
    w32_dl_push(W, cmd);
    return NUM_VAL((double)W->id);
}

static Value w32Fill(int c, Value* a, Env* e) {
    if (c < 5 || !IS_NUM(a[0])) { runtimeError("window-fill expects (win x y w h color)"); return NIL_VAL; }
    Value args[7];
    args[0]=a[0]; args[1]=NUM_VAL(NWM_OP_FILL);
    args[2]=a[1]; args[3]=a[2]; args[4]=a[3];
    args[5]=(c>=5)?a[4]:NUM_VAL(0);
    args[6]=(c>=6 && IS_NUM(a[5]))?a[5]:NUM_VAL(0);
    return w32Shape(7, args, e);
}

static Value w32Text(int c, Value* a, Env* e) {
    (void)e;
    if (c < 5 || !IS_NUM(a[0]) || !IS_OBJ(a[4])) { runtimeError("window-text expects (win x y scale \"text\")"); return NIL_VAL; }
    W32Win* W = w32_win(c, a);
    if (!W) { runtimeError("window-text: unknown window"); return NIL_VAL; }
    char* s = valueToCString(a[4]);
    if (!s) { runtimeError("window-text: bad string"); return NIL_VAL; }
    W32Cmd cmd; memset(&cmd, 0, sizeof cmd);
    cmd.is_text = 1;
    cmd.tx = IS_NUM(a[1])?(int)AS_NUM(a[1]):0;
    cmd.ty = IS_NUM(a[2])?(int)AS_NUM(a[2]):0;
    cmd.scale = IS_NUM(a[3])?(int)AS_NUM(a[3]):1;
    cmd.color = (c >= 6 && IS_NUM(a[5])) ? (uint32_t)AS_NUM(a[5]) : 0xFFFFFFu; /* 第6参颜色，默认白 */
    strncpy(cmd.text, s, sizeof cmd.text - 1);
    free(s);
    w32_dl_push(W, cmd);
    return NUM_VAL((double)W->id);
}

static Value w32Clear(int c, Value* a, Env* e) {
    (void)e;
    W32Win* W = w32_win(c, a);
    if (!W) { runtimeError("window-clear: unknown window"); return NIL_VAL; }
    W->dn = 0;
    return NIL_VAL;
}

static Value w32Invalidate(int c, Value* a, Env* e) {
    (void)e;
    W32Win* W = w32_win(c, a);
    if (!W) { runtimeError("window-invalidate: unknown window"); return NIL_VAL; }
    InvalidateRect(W->hwnd, NULL, FALSE);
    UpdateWindow(W->hwnd);
    return NIL_VAL;
}

static Value w32Move(int c, Value* a, Env* e) {
    (void)e;
    if (c < 3 || !IS_NUM(a[0])) { runtimeError("window-move expects (win x y)"); return NIL_VAL; }
    W32Win* W = w32_win(c, a);
    if (!W) { runtimeError("window-move: unknown window"); return NIL_VAL; }
    SetWindowPos(W->hwnd, NULL, (int)AS_NUM(a[1]), (int)AS_NUM(a[2]), 0, 0, SWP_NOSIZE|SWP_NOZORDER);
    return NIL_VAL;
}

static Value w32Title(int c, Value* a, Env* e) {
    (void)e;
    if (c < 2 || !IS_NUM(a[0]) || !IS_OBJ(a[1])) { runtimeError("window-title expects (win \"title\")"); return NIL_VAL; }
    W32Win* W = w32_win(c, a);
    if (!W) { runtimeError("window-title: unknown window"); return NIL_VAL; }
    char* t = valueToCString(a[1]);
    if (t) { SetWindowTextA(W->hwnd, t); free(t); }
    return NIL_VAL;
}

static Value w32Close(int c, Value* a, Env* e) {
    (void)e;
    W32Win* W = w32_win(c, a);
    if (!W) { runtimeError("window-close: unknown window"); return NIL_VAL; }
    if (W->hwnd) DestroyWindow(W->hwnd);
    W->hwnd = NULL; W->used = 0; free(W->dl); W->dl = NULL;
    w32_pump();
    return NIL_VAL;
}

static Value w32_key_sym(int vk) {
    const char* s = NULL; int len = 0;
    switch (vk) {
        case VK_UP: s="up"; len=2; break;
        case VK_DOWN: s="down"; len=4; break;
        case VK_LEFT: s="left"; len=4; break;
        case VK_RIGHT: s="right"; len=5; break;
        case VK_ESCAPE: s="esc"; len=3; break;
        case VK_SPACE: s="space"; len=5; break;
        case VK_RETURN: s="enter"; len=5; break;
        default: break;
    }
    if (s) {
        ObjString* o = internString(s, len, true);
        retain(OBJ_VAL(o));
        return OBJ_VAL(o);
    }
    /* 字母键 A-Z → 单字符小写字符串（供 key-dir 的 "w"/"a"/"s"/"d" 与 wait-restart 的 "r" 比较） */
    if (vk >= 'A' && vk <= 'Z') {
        char ch = (char)('a' + (vk - 'A'));
        ObjString* o = internString(&ch, 1, false);
        retain(OBJ_VAL(o));
        return OBJ_VAL(o);
    }
    return NIL_VAL;
}

/* (window-event [block]) -> 按键符号或 nil；同时泵消息让窗口保持响应 */
static Value w32Event(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    w32_pump();
    if (!g_w32_cur) return NIL_VAL;
    W32Win* W = w32_by_id(g_w32_cur);
    if (!W) return NIL_VAL;
    int vk = w32_popkey(W);
    return vk ? w32_key_sym(vk) : NIL_VAL;
}

static Value w32Trace(int c, Value* a, Env* e) {
    (void)e;
    if (c>=1 && IS_NUM(a[0])) g_wm_trace = AS_NUM(a[0]) != 0;
    return NIL_VAL;
}

void registerWmNatives(Env* g) {
    defNative(g, "window-create",     w32Create);
    defNative(g, "window-shape",      w32Shape);
    defNative(g, "window-fill",       w32Fill);
    defNative(g, "window-text",       w32Text);
    defNative(g, "window-clear",      w32Clear);
    defNative(g, "window-invalidate", w32Invalidate);
    defNative(g, "window-move",       w32Move);
    defNative(g, "window-title",      w32Title);
    defNative(g, "window-close",      w32Close);
    defNative(g, "window-event",      w32Event);
    defNative(g, "window-trace",      w32Trace);
}

#else
/* ====================== 非 Windows：宿主假 WM ====================== */
#define HOST_WM_MAX_WIN 16
typedef struct { int used; uint32_t win; int x, y, w, h; char title[64]; } HostWin;
static HostWin   g_host_wm[HOST_WM_MAX_WIN];
static uint32_t  g_host_wm_next = 1;

static HostWin* hostwm_find(uint32_t win) {
    for (int i = 0; i < HOST_WM_MAX_WIN; i++)
        if (g_host_wm[i].used && g_host_wm[i].win == win) return &g_host_wm[i];
    return NULL;
}
static int argInt(Value v, int dflt) { return IS_NUM(v) ? (int)AS_NUM(v) : dflt; }

static Value natWinCreate(int c, Value* a, Env* e) {
    (void)e;
    if (c < 4 || !IS_NUM(a[0])||!IS_NUM(a[1])||!IS_NUM(a[2])||!IS_NUM(a[3])) { runtimeError("window-create expects (x y w h [title])"); return NIL_VAL; }
    int slot=-1; for (int i=0;i<HOST_WM_MAX_WIN;i++) if(!g_host_wm[i].used){slot=i;break;}
    if (slot<0){ runtimeError("window-create: too many windows"); return NIL_VAL; }
    uint32_t win=g_host_wm_next++; HostWin* hw=&g_host_wm[slot];
    hw->used=1; hw->win=win; hw->x=(int)AS_NUM(a[0]); hw->y=(int)AS_NUM(a[1]); hw->w=(int)AS_NUM(a[2]); hw->h=(int)AS_NUM(a[3]); hw->title[0]=0;
    if (c>=5 && IS_OBJ(a[4])){ char* t=valueToCString(a[4]); if(t){ strncpy(hw->title,t,sizeof hw->title-1); hw->title[sizeof hw->title-1]=0; free(t);} }
    if (g_wm_trace) fprintf(stderr,"[wm-host] CREATE win=%u %dx%d@(%d,%d) title=\"%s\"\n", win, hw->w, hw->h, hw->x, hw->y, hw->title);
    return NUM_VAL((double)win);
}
static Value natWinShape(int c, Value* a, Env* e) {
    (void)e;
    if (c<7 || !IS_NUM(a[0])||!IS_NUM(a[1])){ runtimeError("window-shape expects (win op x y w h color [radius])"); return NIL_VAL; }
    if (!hostwm_find((uint32_t)AS_NUM(a[0]))){ runtimeError("window-shape: unknown window"); return NIL_VAL; }
    if (g_wm_trace) fprintf(stderr,"[wm-host] SHAPE win=%u op=%d %dx%d@(%d,%d) color=#%06X radius=%d\n",
        (uint32_t)AS_NUM(a[0]),(int)AS_NUM(a[1]),argInt(a[4],0),argInt(a[5],0),argInt(a[2],0),argInt(a[3],0),
        IS_NUM(a[6])?(uint32_t)AS_NUM(a[6]):0, (c>=8&&IS_NUM(a[7]))?(int)AS_NUM(a[7]):0);
    return NUM_VAL(AS_NUM(a[0]));
}
static Value natWinFill(int c, Value* a, Env* e) {
    if (c<5||!IS_NUM(a[0])){ runtimeError("window-fill expects (win x y w h color)"); return NIL_VAL; }
    Value args[7]; args[0]=a[0]; args[1]=NUM_VAL(NWM_OP_FILL); args[2]=a[1]; args[3]=a[2]; args[4]=a[3];
    args[5]=(c>=5)?a[4]:NUM_VAL(0); args[6]=(c>=6&&IS_NUM(a[5]))?a[5]:NUM_VAL(0);
    return natWinShape(7, args, e);
}
static Value natWinText(int c, Value* a, Env* e) {
    (void)e;
    if (c<5||!IS_NUM(a[0])||!IS_OBJ(a[4])){ runtimeError("window-text expects (win x y scale \"text\")"); return NIL_VAL; }
    if (!hostwm_find((uint32_t)AS_NUM(a[0]))){ runtimeError("window-text: unknown window"); return NIL_VAL; }
    char* s=valueToCString(a[4]); if(!s){ runtimeError("window-text: bad string"); return NIL_VAL; }
    if (g_wm_trace) fprintf(stderr,"[wm-host] TEXT win=%u @(%d,%d) scale=%d \"%s\"\n",(uint32_t)AS_NUM(a[0]),(int)AS_NUM(a[1]),(int)AS_NUM(a[2]),(int)AS_NUM(a[3]),s);
    free(s); return NUM_VAL(AS_NUM(a[0]));
}
static Value natWinClear(int c, Value* a, Env* e){ (void)e; if(c<1||!IS_NUM(a[0])||!hostwm_find((uint32_t)AS_NUM(a[0]))){runtimeError("window-clear: unknown window");return NIL_VAL;} if(g_wm_trace)fprintf(stderr,"[wm-host] CLEAR win=%u\n",(uint32_t)AS_NUM(a[0])); return NIL_VAL; }
static Value natWinInvalidate(int c, Value* a, Env* e){ (void)e; if(c<1||!IS_NUM(a[0])||!hostwm_find((uint32_t)AS_NUM(a[0]))){runtimeError("window-invalidate: unknown window");return NIL_VAL;} if(g_wm_trace)fprintf(stderr,"[wm-host] INVALIDATE win=%u\n",(uint32_t)AS_NUM(a[0])); return NIL_VAL; }
static Value natWinMove(int c, Value* a, Env* e){ (void)e; if(c<3||!IS_NUM(a[0])||!hostwm_find((uint32_t)AS_NUM(a[0]))){runtimeError("window-move: unknown window");return NIL_VAL;} return NIL_VAL; }
static Value natWinTitle(int c, Value* a, Env* e){ (void)e; if(c<2||!IS_NUM(a[0])||!IS_OBJ(a[1])||!hostwm_find((uint32_t)AS_NUM(a[0]))){runtimeError("window-title: unknown window");return NIL_VAL;} return NIL_VAL; }
static Value natWinClose(int c, Value* a, Env* e){ (void)e; if(c<1||!IS_NUM(a[0])){runtimeError("window-close: unknown window");return NIL_VAL;} HostWin* hw=hostwm_find((uint32_t)AS_NUM(a[0])); if(hw)hw->used=0; return NIL_VAL; }
static Value natWinEvent(int c, Value* a, Env* e){ (void)c;(void)a;(void)e; return NIL_VAL; }
static Value natWinTrace(int c, Value* a, Env* e){ (void)e; if(c>=1&&IS_NUM(a[0])) g_wm_trace=AS_NUM(a[0])!=0; return NIL_VAL; }

void registerWmNatives(Env* g) {
    defNative(g, "window-create",     natWinCreate);
    defNative(g, "window-shape",      natWinShape);
    defNative(g, "window-fill",       natWinFill);
    defNative(g, "window-text",       natWinText);
    defNative(g, "window-clear",      natWinClear);
    defNative(g, "window-invalidate", natWinInvalidate);
    defNative(g, "window-move",       natWinMove);
    defNative(g, "window-title",      natWinTitle);
    defNative(g, "window-close",      natWinClose);
    defNative(g, "window-event",      natWinEvent);
    defNative(g, "window-trace",      natWinTrace);
}
#endif
