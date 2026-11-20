/**
 * Novus OS - M9 Win32 sample: dialog controls.
 *
 * A dialog on Windows is a window with STATIC, BUTTON and EDIT children.
 * This sample creates the three of them with CreateWindowExA, checks the
 * text round trip on a control, then leaves the window on screen: clicking
 * the button fills WM_COMMAND, and typing into the edit goes into its own
 * buffer. The console lines below it prove the calls; the picture proves
 * the rest.
 */

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef long long          i64;
typedef int                i32;
typedef long               i32s;
typedef char               i8;
typedef short              i16;
typedef unsigned short     u16;

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
__declspec(dllimport) u64   GetDlgItem(u64 parent, i32 id);
__declspec(dllimport) int   SetWindowTextA(u64 hwnd, const char *text);
__declspec(dllimport) int   GetWindowTextA(u64 hwnd, char *out, i32 max);
__declspec(dllimport) i32   MessageBoxA(u64 hwnd, const char *text,
                                        const char *caption, u32 type);

#define WM_CREATE   0x0001u
#define WM_DESTROY  0x0002u
#define WM_PAINT    0x000Fu
#define WM_CLOSE    0x0010u
#define WM_COMMAND  0x0111u

#define BN_CLICKED  0u

#define STD_OUTPUT_HANDLE ((u32)-11)

/* ---- console output ---------------------------------------------------- */

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

static int str_eq(const char *a, const char *b) {
  u32 i = 0;

  while (a[i] != '\0' || b[i] != '\0') {
    if (a[i] != b[i]) {
      return 0;
    }
    i++;
  }
  return 1;
}

/* ---- the window -------------------------------------------------------- */

static u64 g_label;
static u64 g_button;
static u64 g_edit;
static u32 g_clicks;

static i64 WndProc(u64 hwnd, u32 msg, u64 wp, i64 lp) {
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

      r.left = 0; r.top = 0; r.right = 520; r.bottom = 300;
      FillRect(hdc, &r, CreateSolidBrush(0x00F5F5F5u));
    }
    SetTextColor(hdc, 0x00202020u);
    TextOutA(hdc, 16, 8, "控件：标签 / 按钮 / 编辑框", 22);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_COMMAND: {
    u32 id = (u32)((wp >> 16) & 0xFFFFu);
    u32 code = (u32)(wp & 0xFFFFu);

    if (id == 2u && code == BN_CLICKED) {
      /* The button said so; write the count into the label and print it. */
      char buf[32];
      u32  n = 0;

      g_clicks++;
      buf[0] = 'b';
      buf[1] = 't';
      buf[2] = 'n';
      buf[3] = ' ';
      buf[4] = '#';
      n = 5;
      {
        char d = (char)('0' + (g_clicks % 10u));

        buf[n++] = d;
      }
      buf[n] = '\0';
      say("[CTRL] WM_COMMAND id=2 code=BN_CLICKED\n");
      SetWindowTextA(g_label, buf);
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
  u64        hwnd;
  char       text[40];
  i32        got;
  MSG        msg;

  say("[CTRL] dialog controls\n");
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
  wc.lpszClassName = (u64)&"CtrlDemo";
  wc.hIconSm = 0u;

  expect("RegisterClassExA", RegisterClassExA(&wc) != 0u, 1u);
  hwnd = CreateWindowExA(0u, "CtrlDemo", "控件演示", 0u, 60, 60, 520, 300,
                         0u, 0u, 0u, 0);
  expect_true("CreateWindowExA", hwnd != 0u);

  g_label = CreateWindowExA(0u, "STATIC", "等待按钮", 0u, 16, 40, 200, 20,
                            hwnd, 0u, 0u, 0);
  g_button = CreateWindowExA(0u, "BUTTON", "点我", 0u, 16, 76, 120, 32,
                             hwnd, 0u, 0u, 0);
  g_edit = CreateWindowExA(0u, "EDIT", "编辑框", 0u, 16, 124, 240, 28,
                           hwnd, 0u, 0u, 0);

  expect_true("STATIC handle", g_label != 0u);
  expect_true("BUTTON handle", g_button != 0u);
  expect_true("EDIT handle", g_edit != 0u);
  expect_true("controls have distinct handles",
              g_label != g_button && g_button != g_edit && g_label != g_edit);
  expect("GetDlgItem(parent, 2)", GetDlgItem(hwnd, 2), g_button);

  /* Text round trip on a control: SetWindowTextA stores it, GetWindowTextA
   * reads the same bytes back, which is what a dialog does at WM_INITDIALOG. */
  text[0] = '\0';
  expect("SetWindowTextA on the STATIC", SetWindowTextA(g_label,
         "\u5df2\u6539\u5199"), 1u);
  got = GetWindowTextA(g_label, text, (i32)sizeof(text));
  expect("GetWindowTextA returns the new text", (u64)(u32)got,
         (u64)str_len("\u5df2\u6539\u5199"));
  expect_true("the text matches", str_eq(text, "\u5df2\u6539\u5199"));
  /* Put the label back so the picture starts from a known state. */
  SetWindowTextA(g_label, "\u7b49\u5f85\u6309\u94ae");

  /* The modal box: the layer's own window, so it proves the controls work
   * even for a caller that never builds a dialog. MB_OKCANCEL answers 1 for
   * OK and 2 for Cancel. */
  say("[CTRL] MessageBoxA: opening the box\n");
  got = MessageBoxA(hwnd, "Message box from the control demo.", "CtrlDemo",
                    1u);
  expect_true("MessageBoxA answered IDOK or IDCANCEL", got == 1 || got == 2);

  say("[CTRL] FAIL count = ");
  say_hex(g_fail);
  say("\n");

  /* The window stays up: the click and the typing are the visual half of
   * this sample, and they need a message loop to run. */
  while (GetMessageA(0u, &msg, 0u, 0u)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  say(g_fail == 0 ? "[CTRL] PASS\n" : "[CTRL] FAILED\n");
  ExitProcess(g_fail == 0 ? 0u : 1u);
}
