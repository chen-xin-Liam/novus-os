/**
 * Novus OS - M9 Win32 sample: menus.
 *
 * A menu bar is a menu whose items open drop-downs (MF_POPUP). This sample
 * builds one, attaches it with SetMenu, then leaves the window on screen:
 * clicking the bar opens its drop-down and clicking an item arrives as
 * WM_COMMAND with the item's id. Novus-WM has no menu strip in the frame yet,
 * so the layer draws the bar over the top rows of the client area - see
 * docs/M9, section 15.
 */

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef long long          i64;
typedef int                i32;
typedef char               i8;

typedef struct {
  i32 left;
  i32 top;
  i32 right;
  i32 bottom;
} RECT;

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
  u32 cbSize;
  u32 style;
  u64 lpfnWndProc;
  i32 cbClsExtra;
  i32 cbWndExtra;
  u64 hInstance;
  u64 hIcon;
  u64 hCursor;
  u64 hbrBackground;
  u64 lpszMenuName;
  u64 lpszClassName;
  u64 hIconSm;
} WNDCLASSEX;

typedef struct {
  u64 hdc;
  u32 fErase;
  RECT rcPaint;
  u32 fRestore;
  u32 fIncUpdate;
  i8  reserved[32];
} PAINTSTRUCT;

__declspec(dllimport) void *GetStdHandle(u32 which);
__declspec(dllimport) int   WriteConsoleA(void *h, const void *text,
                                           u32 len, u32 *written, void *ov);
__declspec(dllimport) void  ExitProcess(u32 code);
__declspec(dllimport) u64   RegisterClassExA(const WNDCLASSEX *wc);
__declspec(dllimport) u64   CreateWindowExA(u32 ex_style, const char *cls,
                                            const char *title, u32 style,
                                            i32 x, i32 y, i32 w, i32 h,
                                            u64 parent, u64 menu, u64 inst,
                                            void *param);
__declspec(dllimport) int   GetMessageA(u64 hwnd, MSG *msg, u32 lo, u32 hi);
__declspec(dllimport) int   TranslateMessage(const MSG *msg);
__declspec(dllimport) i64   DispatchMessageA(const MSG *msg);
__declspec(dllimport) i64   DefWindowProcA(u64 hwnd, u32 msg, u64 wp, i64 lp);
__declspec(dllimport) u64   BeginPaint(u64 hwnd, PAINTSTRUCT *ps);
__declspec(dllimport) int   EndPaint(u64 hwnd, const PAINTSTRUCT *ps);
__declspec(dllimport) u64   CreateSolidBrush(u32 colorref);
__declspec(dllimport) int   FillRect(u64 hdc, const RECT *r, u64 brush);
__declspec(dllimport) u64   SetTextColor(u64 hdc, u32 colorref);
__declspec(dllimport) int   TextOutA(u64 hdc, i32 x, i32 y, const char *s,
                                     i32 count);
__declspec(dllimport) u64   CreateMenu(void);
__declspec(dllimport) u64   CreatePopupMenu(void);
__declspec(dllimport) int   AppendMenuA(u64 menu, u32 flags, u64 id,
                                        const char *text);
__declspec(dllimport) int   GetMenuItemCount(u64 menu);
__declspec(dllimport) int   SetMenu(u64 hwnd, u64 menu);
__declspec(dllimport) u64   GetMenu(u64 hwnd);
__declspec(dllimport) int   DrawMenuBar(u64 hwnd);
__declspec(dllimport) i32   TrackPopupMenu(u64 menu, u32 flags, i32 x, i32 y,
                                           i32 reserved, u64 hwnd,
                                           const RECT *rect);
__declspec(dllimport) int   SetWindowTextA(u64 hwnd, const char *text);

#define WM_PAINT    0x000Fu
#define WM_CLOSE    0x0010u
#define WM_DESTROY  0x0002u
#define WM_COMMAND  0x0111u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u
#define WM_CONTEXTMENU 0x007Bu

#define TPM_RIGHTALIGN 0x0008u
#define TPM_BOTTOMALIGN 0x0020u
#define TPM_RETURNCMD  0x0100u

#define MF_STRING    0x00000000u
#define MF_POPUP     0x00000010u
#define MF_SEPARATOR 0x00000800u
#define MF_CHECKED   0x00000008u

#define STD_OUTPUT_HANDLE ((u32)-11)

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

static void expect_true(const char *what, int ok) {
  say("  ");
  say(what);
  if (ok != 0) {
    say("  ok\n");
  } else {
    say("  FAIL\n");
    g_fail++;
  }
}

static u64 g_hwnd;
static u64 g_ctx;
static u32 g_cmds;
static u32 g_rdown;
static u32 g_rup;

static i64 WndProc(u64 hwnd, u32 msg, u64 wp, i64 lp) {
  (void)hwnd;
  (void)wp;
  (void)lp;

  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps;
    u64         hdc;

    hdc = BeginPaint(hwnd, &ps);
    if (hdc == 0u) {
      return 0;
    }
    {
      RECT r;

      r.left = 0; r.top = 0; r.right = 420; r.bottom = 260;
      FillRect(hdc, &r, CreateSolidBrush(0x00F7F7F7u));
    }
    SetTextColor(hdc, 0x00303030u);
    TextOutA(hdc, 16, 32, "menus: File / Edit", 18);
    TextOutA(hdc, 16, 54, "click the bar, then an item", 27);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_RBUTTONDOWN:
    g_rdown++;
    return 0;
  case WM_RBUTTONUP:
    g_rup++;
    return 0;
  case WM_CONTEXTMENU: {
    i32 x = (i32)(lp & 0xFFFF);
    i32 y = (i32)((lp >> 16) & 0xFFFF);
    i32 id;

    say("[MENU] WM_CONTEXTMENU\n");
    id = TrackPopupMenu(g_ctx, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                        x, y, 0, hwnd, 0);
    say("[MENU] TrackPopupMenu id=");
    say_hex((u64)(i64)id);
    say("\n");
    expect_true("TrackPopupMenu result", id == 201 || id == 0);
    return 0;
  }
  case WM_COMMAND: {
    u32 id = (u32)(wp & 0xFFFFu);

    g_cmds++;
    say("[MENU] WM_COMMAND id=");
    say_hex(id);
    say("\n");
    if (id == 101u) {
      SetWindowTextA(g_hwnd, "menu: new");
    } else if (id == 103u) {
      SetWindowTextA(g_hwnd, "menu: undo");
    }
    return 0;
  }
  case WM_CLOSE:
  case WM_DESTROY:
    DefWindowProcA(hwnd, msg, wp, lp);
    return 0;
  default:
    break;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The PE entry point the loader calls; it never returns. */
void novus_win32_main(void) {
  WNDCLASSEX wc;
  u64        bar;
  u64        file;
  u64        edit;
  u64        hwnd;
  MSG        msg;

  say("[MENU] menu bar\n");
  wc.cbSize = 0u;
  wc.style = 0u;
  wc.lpfnWndProc = (u64)&WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = 0u;
  wc.hIcon = 0u;
  wc.hCursor = 0u;
  wc.hbrBackground = 0u;
  wc.lpszMenuName = 0u;
  wc.lpszClassName = (u64)&"MenuDemo";
  wc.hIconSm = 0u;

  expect("RegisterClassExA", RegisterClassExA(&wc) != 0u, 1u);
  hwnd = CreateWindowExA(0u, "MenuDemo", "menu demo", 0u, 120, 80, 420, 260,
                         0u, 0u, 0u, 0);
  expect_true("CreateWindowExA", hwnd != 0u);
  g_hwnd = hwnd;

  bar = CreateMenu();
  file = CreatePopupMenu();
  edit = CreatePopupMenu();
  expect_true("CreateMenu", bar != 0u);
  expect_true("CreatePopupMenu x2", file != 0u && edit != 0u);

  expect("AppendMenuA(bar, File)", AppendMenuA(bar, MF_POPUP | MF_STRING, file,
                                               "File"), 1u);
  expect("AppendMenuA(bar, Edit)", AppendMenuA(bar, MF_POPUP | MF_STRING, edit,
                                               "Edit"), 1u);
  expect("AppendMenuA(File, New)", AppendMenuA(file, MF_STRING, 101u, "New"),
         1u);
  expect("AppendMenuA(File, separator)", AppendMenuA(file, MF_SEPARATOR, 0u, ""),
         1u);
  expect("AppendMenuA(File, Exit)", AppendMenuA(file, MF_STRING, 102u, "Exit"),
         1u);
  expect("AppendMenuA(Edit, Undo)",
         AppendMenuA(edit, MF_STRING | MF_CHECKED, 103u, "Undo"), 1u);
  expect("GetMenuItemCount(bar)", (u64)(u32)GetMenuItemCount(bar), 2u);
  expect("GetMenuItemCount(file)", (u64)(u32)GetMenuItemCount(file), 3u);
  expect("GetMenuItemCount(edit)", (u64)(u32)GetMenuItemCount(edit), 1u);

  g_ctx = CreatePopupMenu();
  expect_true("CreatePopupMenu(context)", g_ctx != 0u);
  expect("AppendMenuA(context, refresh)",
         AppendMenuA(g_ctx, MF_STRING, 201u, "刷新"), 1u);
  expect("AppendMenuA(context, properties)",
         AppendMenuA(g_ctx, MF_STRING, 202u, "属性"), 1u);

  expect("SetMenu", (u64)(u32)SetMenu(hwnd, bar), 1u);
  expect("GetMenu", GetMenu(hwnd), bar);
  expect("DrawMenuBar", (u64)(u32)DrawMenuBar(hwnd), 1u);

  say("[MENU] FAIL count = ");
  say_hex(g_fail);
  say("\n");

  while (GetMessageA(0u, &msg, 0u, 0u)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  say("[MENU] WM_RBUTTONDOWN=");
  say_hex(g_rdown);
  say(" WM_RBUTTONUP=");
  say_hex(g_rup);
  say("\n");
  say(g_fail == 0 ? "[MENU] PASS\n" : "[MENU] FAILED\n");
  ExitProcess(g_fail == 0 ? 0u : 1u);
}
