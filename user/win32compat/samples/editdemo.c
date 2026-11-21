/**
 * Novus OS - M9 Win32 sample: typing into an edit control.
 *
 * A dialog gets its text from an edit; that means the caret, not just a
 * buffer. This sample puts a label, an edit and a Read button on the window
 * and stops. The serial log proves the calls; the picture proves the rest:
 * click the edit, type, use the arrow keys to move inside the text, press
 * Tab to walk the controls, then press Read - the button reads the buffer
 * with GetWindowTextA and prints exactly what the layer stored.
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

/* ---- the window -------------------------------------------------------- */

static u64 g_edit;
static u64 g_label;

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
    TextOutA(hdc, 16, 8, "edit: type, arrows move the caret, Tab walks", 44);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_COMMAND: {
    u32 id = (u32)((wp >> 16) & 0xFFFFu);
    u32 code = (u32)(wp & 0xFFFFu);

    if (id == 2u && code == BN_CLICKED) {
      /* Read: exactly what the control holds, straight from GetWindowTextA
       * and into the label, so both the serial log and the screen show it. */
      char buf[128];
      i32  got;

      buf[0] = '\0';
      got = GetWindowTextA(g_edit, buf, (i32)sizeof(buf));
      say("[EDIT] read len=");
      say_hex((u64)(u32)got);
      say(" text=");
      say(buf);
      say("\n");
      SetWindowTextA(g_label, buf[0] != '\0' ? buf : "(empty)");
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
  u64        button;
  char       text[40];
  i32        got;
  MSG        msg;

  say("[EDIT] typing into an edit control\n");
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
  wc.lpszClassName = (u64)&"EditDemo";
  wc.hIconSm = 0u;

  expect("RegisterClassExA", RegisterClassExA(&wc) != 0u, 1u);
  hwnd = CreateWindowExA(0u, "EditDemo", "编辑框演示", 0u, 60, 60, 520, 300,
                         0u, 0u, 0u, 0);
  expect_true("CreateWindowExA", hwnd != 0u);

  g_edit = CreateWindowExA(0u, "EDIT", "abc", 0u, 16, 44, 300, 28,
                           hwnd, 0u, 0u, 0);
  button = CreateWindowExA(0u, "BUTTON", "读取", 0u, 16, 84, 120, 32,
                           hwnd, 0u, 0u, 0);
  g_label = CreateWindowExA(0u, "STATIC", "(未读取)", 0u, 16, 128, 300, 20,
                            hwnd, 0u, 0u, 0);

  expect_true("EDIT handle", g_edit != 0u);
  expect_true("BUTTON handle", button != 0u);
  expect("GetDlgItem(parent, 1)", GetDlgItem(hwnd, 1), g_edit);

  /* The seed text is what the sample starts with, so a fresh read matches
   * the .c even before anyone types. */
  text[0] = '\0';
  got = GetWindowTextA(g_edit, text, (i32)sizeof(text));
  expect("GetWindowTextA on the seeded edit", (u64)(u32)got, 3u);
  expect_true("the seed text matches", text[0] == 'a' && text[1] == 'b' &&
              text[2] == 'c');

  say("[EDIT] FAIL count = ");
  say_hex(g_fail);
  say("\n");

  /* Stay up: typing is the visual half of this sample. */
  while (GetMessageA(0u, &msg, 0u, 0u)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  say(g_fail == 0 ? "[EDIT] PASS\n" : "[EDIT] FAILED\n");
  ExitProcess(g_fail == 0 ? 0u : 1u);
}
