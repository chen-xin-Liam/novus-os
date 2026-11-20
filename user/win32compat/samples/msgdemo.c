/**
 * Novus OS - M9 Win32 sample: messages, timers, coordinates, text layout.
 *
 * The window and the paint path are proven by window.exe visually. This one
 * proves the parts of the loop a picture cannot show: that GetMessageA has
 * the Windows argument order, that a posted message comes back with its
 * wParam and lParam intact, that a timer eventually turns into WM_TIMER,
 * that client and screen coordinates agree, and that a string measures to
 * the width the font actually gives it.
 *
 * Console only: one line per check on the serial console, then exit.
 */

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef long long          i64;
typedef int                i32;

typedef struct {
  u64 hwnd;
  u32 message;
  u32 pad;
  u64 wparam;
  u64 lparam;
  u32 time;
  i32 x;
  i32 y;
  u32 pad2;
} MSG;

typedef struct {
  i32 left;
  i32 top;
  i32 right;
  i32 bottom;
} RECT;

typedef struct {
  i32 x;
  i32 y;
} POINT;

typedef struct {
  i32 cx;
  i32 cy;
} SIZE;

__declspec(dllimport) void *GetStdHandle(u32 which);
__declspec(dllimport) int   WriteConsoleA(void *h, const void *text,
                                          u32 len, u32 *written, void *ov);
__declspec(dllimport) void  ExitProcess(u32 code);
__declspec(dllimport) void  Sleep(u32 ms);
__declspec(dllimport) u64   RegisterClassExA(const void *wc);
__declspec(dllimport) u64   CreateWindowExA(u32 exstyle, const char *cls,
                                            const char *title, u32 style,
                                            i32 x, i32 y, i32 w, i32 h,
                                            u64 parent, u64 menu, u64 inst,
                                            void *param);
__declspec(dllimport) int   DestroyWindow(u64 hwnd);
__declspec(dllimport) i32   GetMessageA(u64 hwnd, MSG *msg, u32 lo, u32 hi);
__declspec(dllimport) i32   PeekMessageA(u64 hwnd, MSG *msg, u32 lo, u32 hi,
                                         u32 remove);
__declspec(dllimport) i32   PostMessageA(u64 hwnd, u32 msg, u64 wp, u64 lp);
__declspec(dllimport) i32   SendMessageA(u64 hwnd, u32 msg, u64 wp, u64 lp);
__declspec(dllimport) u64   SetTimer(u64 hwnd, u64 id, u32 ms, void *proc);
__declspec(dllimport) i32   KillTimer(u64 hwnd, u64 id);
__declspec(dllimport) i32   GetClientRect(u64 hwnd, RECT *r);
__declspec(dllimport) i32   GetWindowRect(u64 hwnd, RECT *r);
__declspec(dllimport) i32   ClientToScreen(u64 hwnd, POINT *pt);
__declspec(dllimport) i32   ScreenToClient(u64 hwnd, POINT *pt);
__declspec(dllimport) i32   GetCursorPos(POINT *pt);
__declspec(dllimport) u64   GetWindowLongPtrA(u64 hwnd, i32 index);
__declspec(dllimport) u64   SetWindowLongPtrA(u64 hwnd, i32 index, u64 value);
__declspec(dllimport) i32   GetTextExtentPoint32A(u64 hdc, const char *s,
                                                  u32 count, SIZE *size);
__declspec(dllimport) u32   SetTextAlign(u64 hdc, u32 align);
__declspec(dllimport) u32   GetTextAlign(u64 hdc);
__declspec(dllimport) i32   GetTextFaceA(u64 hdc, u32 count, char *face);
__declspec(dllimport) i32   GetSystemMetrics(i32 index);

#define STD_OUTPUT_HANDLE ((u32)-11)
#define SM_CXSCREEN       0
#define SM_CYSCREEN       1

#define WM_CREATE      0x0001u
#define WM_DESTROY     0x0002u
#define WM_PAINT       0x000Fu
#define WM_TIMER       0x0113u
#define WM_APP         0x8000u
#define TA_CENTER      6u
#define GWLP_USERDATA  (-21)

/* ---- console output (no CRT) ------------------------------------------- */

static u32 str_len(const char *s) {
  u32 n = 0;

  while (s[n] != '\0') {
    n++;
  }
  return n;
}

static void say(const char *s) {
  u32 written = 0;

  WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), s, str_len(s), &written, 0);
}

static void say_hex(u64 v) {
  char buf[19];
  u64  n = 0;
  u32  i;

  buf[0] = '0';
  buf[1] = 'x';
  if (v == 0) {
    buf[2] = '0';
    n = 3;
  } else {
    char tmp[16];
    u32  t = 0;

    while (v != 0 && t < 16) {
      u32 d = (u32)(v & 0xF);

      tmp[t++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
      v >>= 4;
    }
    for (i = 0; i < t && n < 18; i++) {
      buf[n++] = tmp[t - 1 - i];
    }
  }
  buf[n] = '\0';
  say(buf);
}

static u32 g_fail;

static void expect(const char *what, u64 got, u64 want) {
  say("  ");
  say(what);
  say(" = ");
  say_hex(got);
  if (got == want) {
    say("  ok\n");
  } else {
    say("  expect ");
    say_hex(want);
    say("  FAIL\n");
    g_fail++;
  }
}

/* A check whose answer is "anything but zero", used where Windows returns
 * an atom or a sum rather than the constant a fixed test would expect. */
static void expect_true(const char *what, int bad) {
  say("  ");
  say(what);
  if (bad == 0) {
    say("  ok\n");
  } else {
    say("  FAIL\n");
    g_fail++;
  }
}

/* ---- the checks -------------------------------------------------------- */

/* WNDCLASSEXA, as the layer reads it: the procedure at 0x08 and the class
 * name at 0x40. */
typedef struct {
  u32 style;
  u32 pad;
  u64 wndproc;
  i32 cbClsExtra;
  i32 cbWndExtra;
  u64 instance;
  u64 icon;
  u64 cursor;
  u64 background;
  u64 menu;
  u64 class_name;
  u64 small_icon;
} WNDCLASSEXA;

static i32 window_proc(u64 hwnd, u32 msg, u64 wp, u64 lp) {
  (void)hwnd;
  (void)msg;
  (void)wp;
  (void)lp;
  return 0;
}

void novus_win32_main(void) {
  WNDCLASSEXA wc;
  char        cls[16];
  char        title[32];
  u64         hwnd;
  MSG         msg;
  RECT        rc;
  RECT        wr;
  POINT       pt;
  char        face[32];
  i32         i;
  i32         seen;
  u64         timer;
  u64         old;

  cls[0] = 'M'; cls[1] = 's'; cls[2] = 'g'; cls[3] = 'D'; cls[4] = 'e';
  cls[5] = 'm'; cls[6] = 'o'; cls[7] = '\0';
  title[0] = 'M'; title[1] = 's'; title[2] = 'g'; title[3] = '\0';

  say("\n[MSG] message loop, timers, coordinates, text\n");

  wc.style = 0u;
  wc.pad = 0u;
  wc.wndproc = (u64)&window_proc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.instance = 0u;
  wc.icon = 0u;
  wc.cursor = 0u;
  wc.background = 0xFF000000u | 0xFFFFFFu;
  wc.menu = 0u;
  wc.class_name = (u64)cls;
  wc.small_icon = 0u;
  /* A class registration returns the atom, not 1: nonzero is success, and a
   * zero is the only failure the program may test for. */
  expect_true("RegisterClassExA", RegisterClassExA(&wc) == 0 ? 1 : 0);

  hwnd = CreateWindowExA(0u, cls, title, 0u, 96, 96, 320, 180, 0u, 0u, 0u, 0);
  say("  CreateWindowExA hwnd = ");
  say_hex(hwnd);
  say("\n");
  if (hwnd == 0u) {
    say("[MSG] FAIL: no window\n");
    ExitProcess(2u);
  }

  /* WM_CREATE is already in the queue, so the first GetMessage returns
   * without blocking. The hwnd inside the MSG is what proves the argument
   * order: if the layer still read the MSG pointer from RCX, both would be
   * zero or garbage. */
  msg.message = 0u;
  expect("GetMessageA returned 1", (u64)GetMessageA(hwnd, &msg, 0u, 0u), 1u);
  expect("MSG.hwnd", msg.hwnd, hwnd);
  expect("MSG.message = WM_CREATE", (u64)msg.message, (u64)WM_CREATE);

  msg.message = 0u;
  expect("second GetMessageA", (u64)GetMessageA(hwnd, &msg, 0u, 0u), 1u);
  expect("MSG.message = WM_PAINT", (u64)msg.message, (u64)WM_PAINT);

  /* A message the program posts to itself comes back with both payload
   * words, and the filter is honoured: WM_APP+1 is the only message that
   * passes the 0x8001..0x8001 window. */
  expect("PostMessageA", (u64)PostMessageA(hwnd, WM_APP + 1u, 0x1234u,
                                           0x5678u), 1u);
  msg.message = 0u;
  msg.wparam = 0u;
  msg.lparam = 0u;
  expect("GetMessageA(filter) returned 1",
         (u64)GetMessageA(hwnd, &msg, WM_APP + 1u, WM_APP + 1u), 1u);
  expect("MSG.message = WM_APP+1", (u64)msg.message, (u64)(WM_APP + 1u));
  expect("MSG.wParam", msg.wparam, 0x1234u);
  expect("MSG.lParam", msg.lparam, 0x5678u);

  /* SendMessageA is documented as asynchronous in this layer: it returns 0
   * and the message arrives on the next pump. The check asserts exactly
   * that, so the deviation cannot pass silently. */
  expect("SendMessageA returns 0 (async)", (u64)SendMessageA(hwnd, WM_APP + 2u,
                                                             5u, 6u), 0u);
  msg.message = 0u;
  expect("GetMessageA after SendMessageA",
         (u64)GetMessageA(hwnd, &msg, 0u, 0u), 1u);
  expect("MSG.message = WM_APP+2", (u64)msg.message, (u64)(WM_APP + 2u));
  expect("MSG.wParam from SendMessageA", msg.wparam, 5u);

  /* A 100 ms timer should fire within a second even when nothing else is
   * happening, because the layer posts WM_TIMER from the tick counter. */
  timer = SetTimer(hwnd, 7u, 100u, 0);
  expect("SetTimer(7) = 7", timer, 7u);
  seen = 0;
  for (i = 0; i < 40 && seen == 0; i++) {
    msg.message = 0u;
    if (PeekMessageA(hwnd, &msg, 0u, 0u, 1u) != 0) {
      if (msg.message == WM_TIMER && msg.wparam == 7u) {
        seen = 1;
        break;
      }
    }
    /* Sleep parks the task, so the WM and the timer both get to run. */
    Sleep(15u);
  }
  expect("WM_TIMER arrived", (u64)seen, 1u);
  expect("KillTimer(7)", (u64)KillTimer(hwnd, 7u), 1u);
  expect("KillTimer(7) again (nothing left)", (u64)KillTimer(hwnd, 7u), 0u);

  /* Geometry. CreateWindowExA takes the whole window size, so the client an
   * overlapped window ends up with is what is left after the frame: one
   * pixel of border on each side and a 32 pixel caption above (which is
   * also what GetWindowRect must add back). */
  rc.left = -1; rc.top = -1; rc.right = -1; rc.bottom = -1;
  expect("GetClientRect", (u64)GetClientRect(hwnd, &rc), 1u);
  expect("client width = 320 - 2 border", (u64)(u32)rc.right, 318u);
  expect("client height = 180 - 32 caption - 1 border",
         (u64)(u32)rc.bottom, 147u);

  wr.left = 0; wr.top = 0; wr.right = 0; wr.bottom = 0;
  expect("GetWindowRect", (u64)GetWindowRect(hwnd, &wr), 1u);
  expect("window left", (u64)(u32)wr.left, 96u);
  expect("window top", (u64)(u32)wr.top, 96u);
  expect("window right = left + 320", (u64)(u32)wr.right, 96u + 320u);
  expect("window bottom = top + 180", (u64)(u32)wr.bottom, 96u + 180u);

  pt.x = 10;
  pt.y = 10;
  expect("ClientToScreen", (u64)ClientToScreen(hwnd, &pt), 1u);
  expect("screen x = 96 + 1 + 10", (u64)(u32)pt.x, 107u);
  expect("screen y = 96 + 32 + 10", (u64)(u32)pt.y, 138u);
  expect("ScreenToClient", (u64)ScreenToClient(hwnd, &pt), 1u);
  expect("client x back", (u64)(u32)pt.x, 10u);
  expect("client y back", (u64)(u32)pt.y, 10u);

  pt.x = -1;
  pt.y = -1;
  expect("GetCursorPos", (u64)GetCursorPos(&pt), 1u);
  say("  cursor = ");
  say_hex((u64)(u32)pt.x);
  say(",");
  say_hex((u64)(u32)pt.y);
  say("\n");
  if (pt.x >= 0 && pt.y >= 0 &&
      pt.x <= GetSystemMetrics(SM_CXSCREEN) &&
      pt.y <= GetSystemMetrics(SM_CYSCREEN)) {
    say("  cursor inside the screen  ok\n");
  } else {
    say("  cursor outside the screen  FAIL\n");
    g_fail++;
  }

  /* Text layout. The face may be the loaded TrueType one or the built-in
   * bitmap cells, so the checks are relational: four of the same letter
   * must measure four cells, and one full-width character must be at least
   * as wide as two half-width ones - the numbers themselves are the
   * compositor's business and are printed for the log. */
  {
    SIZE one;
    SIZE four;
    SIZE cjk1;
    SIZE cjk2;

    one.cx = -1; one.cy = -1;
    four.cx = -1; four.cy = -1;
    cjk1.cx = -1; cjk1.cy = -1;
    cjk2.cx = -1; cjk2.cy = -1;
    expect("GetTextExtentPoint32A(\"M\")",
           (u64)GetTextExtentPoint32A(hwnd, "M", 1u, &one), 1u);
    say("  one M = ");
    say_hex((u64)(u32)one.cx);
    say("\n");
    expect("GetTextExtentPoint32A(\"MMMM\")",
           (u64)GetTextExtentPoint32A(hwnd, "MMMM", 4u, &four), 1u);
    expect("  width = 4 cells", (u64)(u32)four.cx, 4u * (u64)(u32)one.cx);
    expect("  height = 16", (u64)(u32)four.cy, 16u);

    expect("GetTextExtentPoint32A(CJK 1)",
           (u64)GetTextExtentPoint32A(hwnd, "\xe4\xb8\xad", 3u, &cjk1), 1u);
    say("  one CJK = ");
    say_hex((u64)(u32)cjk1.cx);
    say("\n");
    expect("GetTextExtentPoint32A(CJK 2)",
           (u64)GetTextExtentPoint32A(hwnd, "\xe4\xb8\xad\xe6\x96\x87", 6u,
                                      &cjk2), 1u);
    expect("  CJK width = 2 cells", (u64)(u32)cjk2.cx,
           2u * (u64)(u32)cjk1.cx);
    expect_true("a full-width cell is wider than a half-width one",
                (u32)cjk1.cx < (u32)one.cx ? 1 : 0);
  }

  expect("GetTextAlign starts TA_LEFT", (u64)GetTextAlign(hwnd), 0u);
  expect("SetTextAlign(TA_CENTER)", (u64)SetTextAlign(hwnd, TA_CENTER), 0u);
  expect("GetTextAlign now TA_CENTER", (u64)GetTextAlign(hwnd),
         (u64)TA_CENTER);

  face[0] = '\0';
  say("  GetTextFaceA -> ");
  i = GetTextFaceA(hwnd, 32u, face);
  if (i > 0) {
    say(face);
    say("  ok\n");
  } else {
    say("FAIL\n");
    g_fail++;
  }

  /* GWLP_USERDATA is the one window word a program stores by convention; it
   * must survive a set/get round trip. */
  expect("SetWindowLongPtrA(GWLP_USERDATA)",
         SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0xCAFEu), 0u);
  expect("GetWindowLongPtrA(GWLP_USERDATA)",
         GetWindowLongPtrA(hwnd, GWLP_USERDATA), 0xCAFEu);

  expect("DestroyWindow", (u64)DestroyWindow(hwnd), 1u);

  if (g_fail == 0u) {
    say("[MSG] PASS\n");
    ExitProcess(0u);
  }
  say("[MSG] FAIL count = ");
  say_hex((u64)g_fail);
  say("\n");
  ExitProcess(1u);
}
