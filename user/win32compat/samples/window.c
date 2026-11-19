/**
 * Novus OS - M9 Win32 sample: a window, a message loop, and a paint.
 *
 * This is the smallest program that proves the GUI half of the layer is
 * real: it registers a window class, opens a window on the Novus desktop,
 * pumps messages until the user closes it, and paints its client area
 * through FillRect and TextOutA.
 *
 * It is linked like every other sample - -nostdlib, no CRT, entry point
 * novus_win32_main - so the only code that runs is the code below and the
 * layer behind each import.
 */

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef long long          i64;
typedef long               i32;
typedef short              i16;
typedef unsigned short     u16;
typedef char               i8;

typedef struct {
  i32 left, top, right, bottom;
} RECT;

typedef struct {
  u64 hwnd;
  u32 message;
  u32 pad;
  u64 wparam;
  i64 lparam;
  u32 time;
  i32 x, y;
} MSG;

/* WNDCLASSEXA (x64). The kernel reads lpfnWndProc at 0x08, hbrBackground
 * at 0x30 and lpszClassName at 0x40, so the fields before them must be
 * exactly this wide. */
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

/* PAINTSTRUCT: the layer only needs the caller to hand it something big
 * enough to hold the real thing (64 bytes). */
typedef struct {
  u64 hdc;
  u32 fErase;
  RECT rcPaint;
  u32 fRestore;
  u32 fIncUpdate;
  i8  reserved[32];
} PAINTSTRUCT;

#define WM_CREATE   0x0001u
#define WM_DESTROY  0x0002u
#define WM_PAINT    0x000Fu
#define WM_CLOSE    0x0010u

#define STD_OUTPUT_HANDLE ((u32)-11)

__declspec(dllimport) void *GetStdHandle(u32 which);
__declspec(dllimport) int   WriteConsoleA(void *h, const void *text,
                                          u32 len, u32 *written, void *ov);
__declspec(dllimport) u64   RegisterClassExA(const WNDCLASSEX *wc);
__declspec(dllimport) u64   CreateWindowExA(u32 ex_style, const char *cls,
                                            const char *title, u32 style,
                                            i32 x, i32 y, i32 w, i32 h,
                                            u64 parent, u64 menu, u64 inst,
                                            void *param);
__declspec(dllimport) int   DestroyWindow(u64 hwnd);
__declspec(dllimport) int   ShowWindow(u64 hwnd, i32 cmd);
__declspec(dllimport) int   GetMessageA(MSG *msg, u64 hwnd, u32 lo, u32 hi);
__declspec(dllimport) int   TranslateMessage(const MSG *msg);
__declspec(dllimport) i64   DispatchMessageA(const MSG *msg);
__declspec(dllimport) i64   DefWindowProcA(u64 hwnd, u32 msg, u64 wp, i64 lp);
__declspec(dllimport) int   GetClientRect(u64 hwnd, RECT *out);
__declspec(dllimport) u64   BeginPaint(u64 hwnd, PAINTSTRUCT *ps);
__declspec(dllimport) int   EndPaint(u64 hwnd, const PAINTSTRUCT *ps);
__declspec(dllimport) u64   CreateSolidBrush(u32 colorref);
__declspec(dllimport) int   FillRect(u64 hdc, const RECT *r, u64 brush);
__declspec(dllimport) u64   SetTextColor(u64 hdc, u32 colorref);
__declspec(dllimport) int   TextOutA(u64 hdc, i32 x, i32 y, const char *s,
                                     i32 count);
__declspec(dllimport) void  ExitProcess(u32 code);

/* ---- tiny string helpers (no CRT) -------------------------------------- */

static void mem_zero(void *p, u32 n) {
  i8 *b = (i8 *)p;
  u32 i;

  for (i = 0; i < n; i++) {
    b[i] = 0;
  }
}

static u32 str_len(const char *s) {
  u32 n = 0;

  while (s[n] != 0) {
    n++;
  }
  return n;
}

static void console(const char *s) {
  void *h = GetStdHandle(STD_OUTPUT_HANDLE);
  u32   written = 0;

  WriteConsoleA(h, s, str_len(s), &written, 0);
}

/* ---- colours ---------------------------------------------------------- */

/* COLORREF is 0x00BBGGRR. These are stated in the same order Windows
 * tools would write them. */
#define RGB_RED   0x000000FFu
#define RGB_WHITE 0x00FFFFFFu
#define RGB_INK   0x00141414u
#define RGB_MUTED 0x00707070u
#define RGB_LINE  0x00D8D8D8u

static u64 g_hwnd;
static char g_status[64];

/* An array of string literals is a table of absolute addresses, and that is
 * what keeps the image relocatable: the linker only emits a .reloc section
 * when the program actually holds absolute values. A program without one
 * cannot be moved off 0x400000, so the loader refuses it - correctly. These
 * lines are painted, so nothing here exists only to satisfy the linker. */
static const char *const k_body[] = {
  "这个窗口由 user32.dll 的调用创建，",
  "内容经 FillRect / TextOutA 提交给",
  "Novus-WM 的显示列表。",
};

static const char k_title[] = "Novus OS · Win32 兼容层";

static const char *const k_info[] = {
  "Class   NovusWin32",
  "Message WM_PAINT -> BeginPaint -> EndPaint",
};

static i64 WndProc(u64 hwnd, u32 msg, u64 wp, i64 lp) {
  (void)wp;
  (void)lp;

  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps;
    u64         hdc;
    RECT        r;
    u32         w;
    u32         h;
    u32         i;

    mem_zero(&ps, (u32)sizeof(ps));
    hdc = BeginPaint(hwnd, &ps);
    if (hdc == 0u) {
      return 0;
    }
    if (!GetClientRect(hwnd, &r)) {
      r.left = 0;
      r.top = 0;
      r.right = 460;
      r.bottom = 260;
    }
    w = (u32)(r.right - r.left);
    h = (u32)(r.bottom - r.top);

    FillRect(hdc, &r, CreateSolidBrush(RGB_WHITE));

    /* A 2 px accent bar on the left, then the text block. */
    r.left = 0;
    r.top = 0;
    r.right = 4;
    r.bottom = (i32)h;
    FillRect(hdc, &r, CreateSolidBrush(RGB_RED));

    SetTextColor(hdc, RGB_INK);
    TextOutA(hdc, 24, 24, k_title, (i32)str_len(k_title));
    for (i = 0; i < (u32)(sizeof(k_body) / sizeof(k_body[0])); i++) {
      TextOutA(hdc, 24, 48 + (i32)i * 20, k_body[i], (i32)str_len(k_body[i]));
    }

    SetTextColor(hdc, RGB_MUTED);
    for (i = 0; i < (u32)(sizeof(k_info) / sizeof(k_info[0])); i++) {
      TextOutA(hdc, 24, 124 + (i32)i * 20, k_info[i], (i32)str_len(k_info[i]));
    }
    if (g_status[0] != 0) {
      TextOutA(hdc, 24, 164, g_status, (i32)str_len(g_status));
    }

    SetTextColor(hdc, RGB_LINE);
    TextOutA(hdc, 24, (i32)h - 32, "Close the window to end the process.", 36);

    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_CLOSE:
    /* Windows destroys the window here; the layer's WM has already taken
     * it down, so DestroyWindow just drops our bookkeeping. */
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    return 0;
  default:
    break;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

void novus_win32_main(void) {
  WNDCLASSEX wc;
  MSG        msg;

  console("[WIN32] window: register class\n");
  mem_zero(&wc, (u32)sizeof(wc));
  wc.cbSize = (u32)sizeof(wc);
  wc.style = 0;
  wc.lpfnWndProc = (u64)&WndProc;
  wc.hbrBackground = 0xFF000000u | RGB_WHITE;
  wc.lpszClassName = (u64)"NovusWin32";
  if (RegisterClassExA(&wc) == 0u) {
    console("[WIN32] window: RegisterClassExA FAILED\n");
    ExitProcess(1);
  }

  g_hwnd = CreateWindowExA(0, "NovusWin32", "Win32 窗口 · Novus OS",
                           0, 260, 180, 480, 300, 0, 0, 0, 0);
  if (g_hwnd == 0u) {
    console("[WIN32] window: CreateWindowExA FAILED\n");
    ExitProcess(2);
  }
  console("[WIN32] window: created\n");
  ShowWindow(g_hwnd, 1);

  g_status[0] = 'c';
  g_status[1] = 'l';
  g_status[2] = 'i';
  g_status[3] = 'e';
  g_status[4] = 'n';
  g_status[5] = 't';
  g_status[6] = ' ';
  g_status[7] = 'o';
  g_status[8] = 'k';
  g_status[9] = 0;

  mem_zero(&msg, (u32)sizeof(msg));
  while (GetMessageA(&msg, 0, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  console("[WIN32] window: message loop ended\n");
  ExitProcess(0);
}
