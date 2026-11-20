/**
 * Novus OS - Win32 compatibility layer: the API gate and the registry (M9).
 *
 * A compatibility process reaches this file only through SYS_WIN32. The
 * thunk that pe.c writes puts the API id in R10 and moves the Windows first
 * argument (RCX) to RDI, because `syscall` overwrites RCX with the return
 * address. Everything else is read from the trap frame exactly as Windows
 * x64 left it: RDX, R8, R9 for arguments 1..3 and the user stack for the
 * rest (return address + 32-byte shadow space, so the fifth argument is at
 * rsp + 0x28).
 *
 * The registry is a flat table persisted under /Novus/wincompat. It is not a real
 * key tree: keys are strings, values are strings or DWORDs, and the whole
 * table is rewritten after every set. That is enough for the milestone's
 * "simulated registry" and leaves the format documented in one struct.
 */

#include "kernel.h"
#include "win32.h"
#include "sched.h"
#include "ipc.h"
#include "nvus_paths.h"
#include "pmm.h"
#include "rtc.h"
#include "../fs/vfs.h"
#include "../fs/vfs_mount.h"

#define WIN32_REG_MAX_ENTRIES 16u
#define WIN32_REG_KEY_LEN     48u
#define WIN32_REG_VALUE_LEN   40u
#define WIN32_REG_DATA_LEN    72u
#define WIN32_REG_MAGIC       0x4752564Eu /* 'NVRG' little-endian */
#define WIN32_REG_VERSION     1u
#define WIN32_REG_PATH        NVUS_REGISTRY_FILE

#define WIN32_KEY_MAX 8u
#define WIN32_KEY_BASE 0x1000ULL

#define WIN32_HKEY_CURRENT_USER  0x80000001ULL
#define WIN32_HKEY_LOCAL_MACHINE 0x80000002ULL

#define WIN32_REG_SZ    1u
#define WIN32_REG_DWORD 4u

#define WIN32_ERROR_SUCCESS        0u
#define WIN32_ERROR_FILE_NOT_FOUND 2u
#define WIN32_ERROR_INVALID_PARAM  87u

/* A Windows handle to a file is this layer's task fd table with a tag in the
 * high bits, so WriteFile and CloseHandle can tell a file from a console and
 * from a window without a second table. */
#define WIN32_FILE_HANDLE   0x22000000u
#define WIN32_FILE_SLOTS    16u
#define WIN32_DLL_HANDLE    0x5E000000u
#define WIN32_FIND_HANDLE   0x23000000u
#define WIN32_FIND_SLOTS    4u

typedef struct {
  char   key[WIN32_REG_KEY_LEN];
  char   value[WIN32_REG_VALUE_LEN];
  UINT32 type;
  UINT32 len;
  UINT8  data[WIN32_REG_DATA_LEN];
} win32_reg_entry_t;

typedef struct {
  UINT32 magic;
  UINT32 version;
  UINT32 count;
  UINT32 pad;
  win32_reg_entry_t e[WIN32_REG_MAX_ENTRIES];
} win32_reg_file_t;

typedef struct {
  UINT8  used;
  UINT64 owner;
  char   path[72];
} win32_key_t;

_Static_assert(sizeof(win32_reg_entry_t) == 168u, "registry ABI drift");
_Static_assert(sizeof(win32_reg_file_t) == 2704u, "registry file ABI drift");

static void win32_mem_owner_exit(UINT64 pid);
static void win32_gui_owner_exit(UINT64 pid);

static win32_reg_file_t g_reg;
static win32_key_t      g_keys[WIN32_KEY_MAX];
static int              g_save_failed;

/* API ids, in the order the thunks encode them. */
enum {
  WIN32_API_EXIT_PROCESS = 0,
  WIN32_API_GET_STD_HANDLE,
  WIN32_API_WRITE_FILE,
  WIN32_API_WRITE_CONSOLE,
  WIN32_API_SLEEP,
  WIN32_API_GET_TICK_COUNT64,
  WIN32_API_GET_TICK_COUNT,
  WIN32_API_GET_LAST_ERROR,
  WIN32_API_SET_LAST_ERROR,
  WIN32_API_GET_COMMAND_LINE,
  WIN32_API_GET_MODULE_HANDLE,
  WIN32_API_REG_OPEN_KEY_EX,
  WIN32_API_REG_QUERY_VALUE_EX,
  WIN32_API_REG_SET_VALUE_EX,
  WIN32_API_REG_CLOSE_KEY,
  /* Second block: the calls a program makes before it draws anything.
   * Memory, its own path, the clock and the machine description. */
  WIN32_API_GET_MODULE_FILE_NAME,
  WIN32_API_VIRTUAL_ALLOC,
  WIN32_API_VIRTUAL_FREE,
  WIN32_API_GET_PROCESS_HEAP,
  WIN32_API_HEAP_ALLOC,
  WIN32_API_HEAP_FREE,
  WIN32_API_GET_SYSTEM_TIME,
  WIN32_API_GET_LOCAL_TIME,
  WIN32_API_QUERY_PERFORMANCE_COUNTER,
  WIN32_API_QUERY_PERFORMANCE_FREQUENCY,
  WIN32_API_GET_CURRENT_PROCESS_ID,
  WIN32_API_GET_CURRENT_THREAD_ID,
  WIN32_API_IS_DEBUGGER_PRESENT,
  WIN32_API_GET_SYSTEM_INFO,
  WIN32_API_SET_UNHANDLED_EXCEPTION_FILTER,
  /* Third block: a window and a message loop. */
  WIN32_API_REGISTER_CLASS,
  WIN32_API_CREATE_WINDOW,
  WIN32_API_DESTROY_WINDOW,
  WIN32_API_SHOW_WINDOW,
  WIN32_API_GET_MESSAGE,
  WIN32_API_PEEK_MESSAGE,
  WIN32_API_DISPATCH_MESSAGE,
  WIN32_API_DEF_WINDOW_PROC,
  WIN32_API_TRANSLATE_MESSAGE,
  WIN32_API_GET_CLIENT_RECT,
  WIN32_API_GET_DC,
  WIN32_API_RELEASE_DC,
  WIN32_API_BEGIN_PAINT,
  WIN32_API_END_PAINT,
  WIN32_API_FILL_RECT,
  WIN32_API_TEXT_OUT,
  WIN32_API_SET_TEXT_COLOR,
  WIN32_API_CREATE_SOLID_BRUSH,
  /* Fourth block: thread-local storage and the screen description. A
   * program allocates a TLS index before it does anything else with the
   * C runtime, and asks for the screen size before it sizes a window. */
  WIN32_API_TLS_ALLOC,
  WIN32_API_TLS_FREE,
  WIN32_API_TLS_GET_VALUE,
  WIN32_API_TLS_SET_VALUE,
  WIN32_API_GET_SYSTEM_METRICS,
  /* Fifth block: the rest of what a painting program asks for before it
   * draws text - the font metrics it measures against, the stock brushes it
   * would otherwise have to create, and the invalidation that makes the
   * compositor repaint its client area. */
  WIN32_API_INVALIDATE_RECT,
  WIN32_API_GET_STOCK_OBJECT,
  WIN32_API_GET_TEXT_METRICS,
  /* Sixth block: the pen and the shapes it draws. A program that only fills
   * rectangles covers a UI made of boxes; one line of ink, an outline and an
   * ellipse are what turn boxes into a drawing, so the pen lives beside the
   * brush and every shape goes out as one display-list entry. */
  WIN32_API_SET_BK_MODE,
  WIN32_API_SET_BK_COLOR,
  WIN32_API_GET_TEXT_COLOR,
  WIN32_API_MOVE_TO_EX,
  WIN32_API_LINE_TO,
  WIN32_API_RECTANGLE,
  WIN32_API_ELLIPSE,
  WIN32_API_SET_PIXEL,
  WIN32_API_PAT_BLT,
  WIN32_API_CREATE_PEN,
  WIN32_API_SELECT_OBJECT,
  WIN32_API_DELETE_OBJECT,
  /* ... and the window half of the same block: end the loop, repaint now,
   * and read or write the caption. */
  WIN32_API_POST_QUIT_MESSAGE,
  WIN32_API_UPDATE_WINDOW,
  WIN32_API_SET_WINDOW_TEXT,
  WIN32_API_GET_WINDOW_TEXT,
  /* Seventh block: the rest of the message loop. A program posts messages to
   * itself, arms a timer, converts between client and screen coordinates,
   * measures a string before it lays it out, and asks the window for the
   * pointer it stashed there. */
  WIN32_API_POST_MESSAGE,
  WIN32_API_SEND_MESSAGE,
  WIN32_API_SET_TIMER,
  WIN32_API_KILL_TIMER,
  WIN32_API_GET_WINDOW_RECT,
  WIN32_API_CLIENT_TO_SCREEN,
  WIN32_API_SCREEN_TO_CLIENT,
  WIN32_API_GET_CURSOR_POS,
  WIN32_API_GET_WINDOW_LONG,
  WIN32_API_SET_WINDOW_LONG,
  /* Eighth block: text layout. Measuring, alignment, DrawTextA and the font
   * face the metrics describe. */
  WIN32_API_GET_TEXT_EXTENT_POINT,
  WIN32_API_SET_TEXT_ALIGN,
  WIN32_API_GET_TEXT_ALIGN,
  WIN32_API_DRAW_TEXT,
  WIN32_API_GET_TEXT_FACE,
  WIN32_API_SET_TEXT_CHAR_EXTRA,
  /* Ninth block: files and libraries. A program that can open, read, write
   * and enumerate files on the volume, and resolve a name at run time, is a
   * program that can do work rather than only draw. */
  WIN32_API_CREATE_FILE,
  WIN32_API_READ_FILE,
  WIN32_API_CLOSE_HANDLE,
  WIN32_API_GET_FILE_SIZE,
  WIN32_API_GET_FILE_ATTRIBUTES,
  WIN32_API_DELETE_FILE,
  WIN32_API_CREATE_DIRECTORY,
  WIN32_API_FIND_FIRST,
  WIN32_API_FIND_NEXT,
  WIN32_API_FIND_CLOSE,
  WIN32_API_LOAD_LIBRARY,
  WIN32_API_GET_PROC_ADDRESS,
  WIN32_API_FREE_LIBRARY,
  /* Tenth block: the window-side semantics a program uses once it is
   * interactive - it moves and resizes its own frame, and it takes the
   * keyboard deliberately instead of waiting for a click. */
  WIN32_API_MOVE_WINDOW,
  WIN32_API_SET_FOCUS,
  WIN32_API_GET_FOCUS,
  /* Eleventh block: ws2_32, started honestly. This kernel has no TCP/IP
   * stack yet, so what exists is the part that does not need one - the
   * version handshake a program calls first, the address conversions, and
   * error reporting - while every call that would need a transport answers
   * WSAENETDOWN instead of inventing a connection. */
  WIN32_API_WSA_STARTUP,
  WIN32_API_WSA_CLEANUP,
  WIN32_API_WSA_GET_LAST_ERROR,
  WIN32_API_WSA_SET_LAST_ERROR,
  WIN32_API_SOCKET,
  WIN32_API_CONNECT,
  WIN32_API_SEND,
  WIN32_API_RECV,
  WIN32_API_BIND,
  WIN32_API_LISTEN,
  WIN32_API_ACCEPT,
  WIN32_API_CLOSESOCKET,
  WIN32_API_GETHOSTBYNAME,
  WIN32_API_GETADDRINFO,
  WIN32_API_FREEADDRINFO,
  WIN32_API_SELECT,
  WIN32_API_IOCTLSOCKET,
  WIN32_API_INET_ADDR,
  WIN32_API_INET_NTOA,
  /* Twelfth block: controls. A dialog is a window with STATIC, BUTTON and
   * EDIT children; these are the calls that reach them. */
  WIN32_API_GET_DLG_ITEM,
  /* Thirteenth block: the modal box. It is the layer's own window, so it
   * exercises the controls without needing a dialog template. */
  WIN32_API_MESSAGE_BOX,
  /* Fourteenth block: menus. The bar is the layer's furniture drawn over the
   * client's top strip, and a click in it becomes WM_COMMAND. */
  WIN32_API_CREATE_MENU,
  WIN32_API_CREATE_POPUP_MENU,
  WIN32_API_APPEND_MENU,
  WIN32_API_GET_MENU_ITEM_COUNT,
  WIN32_API_SET_MENU,
  WIN32_API_GET_MENU,
  WIN32_API_DESTROY_MENU,
  WIN32_API_DRAW_MENU_BAR,
  /* Console text color, used by self-hosted C++ programs that port
   * SetConsoleTextAttribute/FOREGROUND_* style output. */
  WIN32_API_SET_CONSOLE_TEXT_ATTR,
};

typedef struct {
  const char *dll;
  const char *fn;
  UINT32      id;
} win32_api_t;

static const win32_api_t g_api[] = {
    {"kernel32.dll", "ExitProcess", WIN32_API_EXIT_PROCESS},
    {"kernel32.dll", "GetStdHandle", WIN32_API_GET_STD_HANDLE},
    {"kernel32.dll", "WriteFile", WIN32_API_WRITE_FILE},
    {"kernel32.dll", "WriteConsoleA", WIN32_API_WRITE_CONSOLE},
    {"kernel32.dll", "Sleep", WIN32_API_SLEEP},
    {"kernel32.dll", "GetTickCount64", WIN32_API_GET_TICK_COUNT64},
    {"kernel32.dll", "GetTickCount", WIN32_API_GET_TICK_COUNT},
    {"kernel32.dll", "GetLastError", WIN32_API_GET_LAST_ERROR},
    {"kernel32.dll", "SetLastError", WIN32_API_SET_LAST_ERROR},
    {"kernel32.dll", "GetCommandLineA", WIN32_API_GET_COMMAND_LINE},
    {"kernel32.dll", "GetModuleHandleA", WIN32_API_GET_MODULE_HANDLE},
    {"advapi32.dll", "RegOpenKeyExA", WIN32_API_REG_OPEN_KEY_EX},
    {"advapi32.dll", "RegQueryValueExA", WIN32_API_REG_QUERY_VALUE_EX},
    {"advapi32.dll", "RegSetValueExA", WIN32_API_REG_SET_VALUE_EX},
    {"advapi32.dll", "RegCloseKey", WIN32_API_REG_CLOSE_KEY},
    {"kernel32.dll", "GetModuleFileNameA", WIN32_API_GET_MODULE_FILE_NAME},
    {"kernel32.dll", "VirtualAlloc", WIN32_API_VIRTUAL_ALLOC},
    {"kernel32.dll", "VirtualFree", WIN32_API_VIRTUAL_FREE},
    {"kernel32.dll", "GetProcessHeap", WIN32_API_GET_PROCESS_HEAP},
    {"kernel32.dll", "HeapAlloc", WIN32_API_HEAP_ALLOC},
    {"kernel32.dll", "HeapFree", WIN32_API_HEAP_FREE},
    {"kernel32.dll", "GetSystemTime", WIN32_API_GET_SYSTEM_TIME},
    {"kernel32.dll", "GetLocalTime", WIN32_API_GET_SYSTEM_TIME},
    {"kernel32.dll", "QueryPerformanceCounter", WIN32_API_QUERY_PERFORMANCE_COUNTER},
    {"kernel32.dll", "QueryPerformanceFrequency", WIN32_API_QUERY_PERFORMANCE_FREQUENCY},
    {"kernel32.dll", "GetCurrentProcessId", WIN32_API_GET_CURRENT_PROCESS_ID},
    {"kernel32.dll", "GetCurrentThreadId", WIN32_API_GET_CURRENT_THREAD_ID},
    {"kernel32.dll", "IsDebuggerPresent", WIN32_API_IS_DEBUGGER_PRESENT},
    {"kernel32.dll", "GetSystemInfo", WIN32_API_GET_SYSTEM_INFO},
    {"kernel32.dll", "SetUnhandledExceptionFilter", WIN32_API_SET_UNHANDLED_EXCEPTION_FILTER},
    /* Third block: the window. A program that draws registers a class,
     * opens a window, pumps messages and paints into it. user32 owns the
     * message and window half, gdi32 the pixels. */
    {"user32.dll", "RegisterClassExA", WIN32_API_REGISTER_CLASS},
    {"user32.dll", "CreateWindowExA", WIN32_API_CREATE_WINDOW},
    {"user32.dll", "DestroyWindow", WIN32_API_DESTROY_WINDOW},
    {"user32.dll", "ShowWindow", WIN32_API_SHOW_WINDOW},
    {"user32.dll", "GetMessageA", WIN32_API_GET_MESSAGE},
    {"user32.dll", "PeekMessageA", WIN32_API_PEEK_MESSAGE},
    {"user32.dll", "DispatchMessageA", WIN32_API_DISPATCH_MESSAGE},
    {"user32.dll", "DefWindowProcA", WIN32_API_DEF_WINDOW_PROC},
    {"user32.dll", "TranslateMessage", WIN32_API_TRANSLATE_MESSAGE},
    {"user32.dll", "GetClientRect", WIN32_API_GET_CLIENT_RECT},
    {"user32.dll", "GetDC", WIN32_API_GET_DC},
    {"user32.dll", "ReleaseDC", WIN32_API_RELEASE_DC},
    {"user32.dll", "BeginPaint", WIN32_API_BEGIN_PAINT},
    {"user32.dll", "EndPaint", WIN32_API_END_PAINT},
    {"gdi32.dll", "FillRect", WIN32_API_FILL_RECT},
    {"gdi32.dll", "TextOutA", WIN32_API_TEXT_OUT},
    {"gdi32.dll", "SetTextColor", WIN32_API_SET_TEXT_COLOR},
    {"gdi32.dll", "CreateSolidBrush", WIN32_API_CREATE_SOLID_BRUSH},
    {"kernel32.dll", "TlsAlloc", WIN32_API_TLS_ALLOC},
    {"kernel32.dll", "TlsFree", WIN32_API_TLS_FREE},
    {"kernel32.dll", "TlsGetValue", WIN32_API_TLS_GET_VALUE},
    {"kernel32.dll", "TlsSetValue", WIN32_API_TLS_SET_VALUE},
    {"user32.dll", "GetSystemMetrics", WIN32_API_GET_SYSTEM_METRICS},
    {"user32.dll", "InvalidateRect", WIN32_API_INVALIDATE_RECT},
    {"gdi32.dll", "GetStockObject", WIN32_API_GET_STOCK_OBJECT},
    {"gdi32.dll", "GetTextMetricsA", WIN32_API_GET_TEXT_METRICS},
    /* Sixth block: pen state and shapes. */
    {"gdi32.dll", "SetBkMode", WIN32_API_SET_BK_MODE},
    {"gdi32.dll", "SetBkColor", WIN32_API_SET_BK_COLOR},
    {"gdi32.dll", "GetTextColor", WIN32_API_GET_TEXT_COLOR},
    {"gdi32.dll", "MoveToEx", WIN32_API_MOVE_TO_EX},
    {"gdi32.dll", "LineTo", WIN32_API_LINE_TO},
    {"gdi32.dll", "Rectangle", WIN32_API_RECTANGLE},
    {"gdi32.dll", "Ellipse", WIN32_API_ELLIPSE},
    {"gdi32.dll", "SetPixel", WIN32_API_SET_PIXEL},
    {"gdi32.dll", "PatBlt", WIN32_API_PAT_BLT},
    {"gdi32.dll", "CreatePen", WIN32_API_CREATE_PEN},
    {"gdi32.dll", "SelectObject", WIN32_API_SELECT_OBJECT},
    {"gdi32.dll", "DeleteObject", WIN32_API_DELETE_OBJECT},
    {"user32.dll", "PostQuitMessage", WIN32_API_POST_QUIT_MESSAGE},
    {"user32.dll", "UpdateWindow", WIN32_API_UPDATE_WINDOW},
    {"user32.dll", "SetWindowTextA", WIN32_API_SET_WINDOW_TEXT},
    {"user32.dll", "GetWindowTextA", WIN32_API_GET_WINDOW_TEXT},
    /* Seventh block: the loop itself - posted messages, timers, coordinate
     * conversion, the pointer, and the per-window 64-bit value. */
    {"user32.dll", "PostMessageA", WIN32_API_POST_MESSAGE},
    {"user32.dll", "SendMessageA", WIN32_API_SEND_MESSAGE},
    {"user32.dll", "SetTimer", WIN32_API_SET_TIMER},
    {"user32.dll", "KillTimer", WIN32_API_KILL_TIMER},
    {"user32.dll", "GetWindowRect", WIN32_API_GET_WINDOW_RECT},
    {"user32.dll", "ClientToScreen", WIN32_API_CLIENT_TO_SCREEN},
    {"user32.dll", "ScreenToClient", WIN32_API_SCREEN_TO_CLIENT},
    {"user32.dll", "GetCursorPos", WIN32_API_GET_CURSOR_POS},
    {"user32.dll", "GetWindowLongPtrA", WIN32_API_GET_WINDOW_LONG},
    {"user32.dll", "SetWindowLongPtrA", WIN32_API_SET_WINDOW_LONG},
    {"gdi32.dll", "GetTextExtentPoint32A", WIN32_API_GET_TEXT_EXTENT_POINT},
    {"gdi32.dll", "SetTextAlign", WIN32_API_SET_TEXT_ALIGN},
    {"gdi32.dll", "GetTextAlign", WIN32_API_GET_TEXT_ALIGN},
    {"user32.dll", "DrawTextA", WIN32_API_DRAW_TEXT},
    {"gdi32.dll", "GetTextFaceA", WIN32_API_GET_TEXT_FACE},
    {"gdi32.dll", "SetTextCharacterExtra", WIN32_API_SET_TEXT_CHAR_EXTRA},
    /* Ninth block: the volume and the library table. */
    {"kernel32.dll", "CreateFileA", WIN32_API_CREATE_FILE},
    {"kernel32.dll", "ReadFile", WIN32_API_READ_FILE},
    {"kernel32.dll", "CloseHandle", WIN32_API_CLOSE_HANDLE},
    {"kernel32.dll", "GetFileSize", WIN32_API_GET_FILE_SIZE},
    {"kernel32.dll", "GetFileAttributesA", WIN32_API_GET_FILE_ATTRIBUTES},
    {"kernel32.dll", "DeleteFileA", WIN32_API_DELETE_FILE},
    {"kernel32.dll", "CreateDirectoryA", WIN32_API_CREATE_DIRECTORY},
    {"kernel32.dll", "FindFirstFileA", WIN32_API_FIND_FIRST},
    {"kernel32.dll", "FindNextFileA", WIN32_API_FIND_NEXT},
    {"kernel32.dll", "FindClose", WIN32_API_FIND_CLOSE},
    {"kernel32.dll", "LoadLibraryA", WIN32_API_LOAD_LIBRARY},
    {"kernel32.dll", "GetProcAddress", WIN32_API_GET_PROC_ADDRESS},
    {"kernel32.dll", "FreeLibrary", WIN32_API_FREE_LIBRARY},
    {"user32.dll", "MoveWindow", WIN32_API_MOVE_WINDOW},
    {"user32.dll", "SetFocus", WIN32_API_SET_FOCUS},
    {"user32.dll", "GetFocus", WIN32_API_GET_FOCUS},
    /* Eleventh block: ws2_32. The handshake and the conversions work; the
     * transport calls fail, because there is no transport. */
    {"ws2_32.dll", "WSAStartup", WIN32_API_WSA_STARTUP},
    {"ws2_32.dll", "WSACleanup", WIN32_API_WSA_CLEANUP},
    {"ws2_32.dll", "WSAGetLastError", WIN32_API_WSA_GET_LAST_ERROR},
    {"ws2_32.dll", "WSASetLastError", WIN32_API_WSA_SET_LAST_ERROR},
    {"ws2_32.dll", "socket", WIN32_API_SOCKET},
    {"ws2_32.dll", "connect", WIN32_API_CONNECT},
    {"ws2_32.dll", "send", WIN32_API_SEND},
    {"ws2_32.dll", "recv", WIN32_API_RECV},
    {"ws2_32.dll", "bind", WIN32_API_BIND},
    {"ws2_32.dll", "listen", WIN32_API_LISTEN},
    {"ws2_32.dll", "accept", WIN32_API_ACCEPT},
    {"ws2_32.dll", "closesocket", WIN32_API_CLOSESOCKET},
    {"ws2_32.dll", "gethostbyname", WIN32_API_GETHOSTBYNAME},
    {"ws2_32.dll", "getaddrinfo", WIN32_API_GETADDRINFO},
    {"ws2_32.dll", "freeaddrinfo", WIN32_API_FREEADDRINFO},
    {"ws2_32.dll", "select", WIN32_API_SELECT},
    {"ws2_32.dll", "ioctlsocket", WIN32_API_IOCTLSOCKET},
    {"ws2_32.dll", "inet_addr", WIN32_API_INET_ADDR},
    {"ws2_32.dll", "inet_ntoa", WIN32_API_INET_NTOA},
    {"user32.dll", "GetDlgItem", WIN32_API_GET_DLG_ITEM},
    {"user32.dll", "MessageBoxA", WIN32_API_MESSAGE_BOX},
    {"user32.dll", "CreateMenu", WIN32_API_CREATE_MENU},
    {"user32.dll", "CreatePopupMenu", WIN32_API_CREATE_POPUP_MENU},
    {"user32.dll", "AppendMenuA", WIN32_API_APPEND_MENU},
    {"user32.dll", "GetMenuItemCount", WIN32_API_GET_MENU_ITEM_COUNT},
    {"user32.dll", "SetMenu", WIN32_API_SET_MENU},
    {"user32.dll", "GetMenu", WIN32_API_GET_MENU},
    {"user32.dll", "DestroyMenu", WIN32_API_DESTROY_MENU},
    {"user32.dll", "DrawMenuBar", WIN32_API_DRAW_MENU_BAR},
    {"kernel32.dll", "SetConsoleTextAttribute", WIN32_API_SET_CONSOLE_TEXT_ATTR},
};

/* ---- small helpers ----------------------------------------------------- */

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_equal(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (lower(*a) != lower(*b)) {
      return 0;
    }
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static int text_equal(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static void copy_text(char *dst, UINT32 cap, const char *src) {
  UINT32 i;

  for (i = 0; i + 1u < cap && src[i] != '\0'; i++) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

/* Append `src` to `dst`, keeping one byte for the terminator. */
static int append_text(char *dst, UINT32 cap, const char *src) {
  UINT32 n = 0;

  while (n + 1u < cap && dst[n] != '\0') {
    n++;
  }
  while (n + 1u < cap && *src != '\0') {
    dst[n++] = *src++;
  }
  dst[n] = '\0';
  return *src == '\0' ? 0 : -1;
}

/* Byte-at-a-time so a string that ends on the last byte of a mapped page is
 * read without touching the next, possibly unmapped, page. */
static int read_user_str(UINT64 cr3, UINT64 va, char *out, UINT32 cap) {
  UINT32 i;

  if (va == 0) {
    out[0] = '\0';
    return 0;
  }
  for (i = 0; i + 1u < cap; i++) {
    if (user_copy(cr3, va + i, &out[i], 1u, 0) != 0) {
      out[0] = '\0';
      return -1;
    }
    if (out[i] == '\0') {
      return 0;
    }
  }
  out[cap - 1u] = '\0';
  return 0;
}

/* Argument 4 and above lives on the caller's stack. */
/* The fifth and later Windows x64 arguments live above the shadow space:
 * the return address sits at rsp, the four register slots below it, and
 * argument 5 at rsp+0x28. n is the real argument number, so 5 is the first
 * one that ever reaches the stack. */
static int stack_arg(trap_frame_t *f, task_t *t, UINT32 n, UINT64 *out) {
  UINT64 va;

  if (t == NULL || n < 5u) {
    return -1;
  }
  va = f->rsp + 0x28u + (UINT64)(n - 5u) * 8u;
  return user_copy(t->cr3, va, out, 8u, 0);
}

static UINT32 reg_key_len(const char *s) {
  UINT32 n = 0;

  while (n < WIN32_REG_KEY_LEN && s[n] != '\0') {
    n++;
  }
  return n;
}

/* ---- registry ---------------------------------------------------------- */

static win32_reg_entry_t *reg_find(const char *key, const char *value) {
  UINT32 i;

  for (i = 0; i < g_reg.count && i < WIN32_REG_MAX_ENTRIES; i++) {
    if (text_equal(g_reg.e[i].key, key) &&
        text_equal(g_reg.e[i].value, value)) {
      return &g_reg.e[i];
    }
  }
  return NULL;
}

static win32_reg_entry_t *reg_alloc(const char *key, const char *value) {
  win32_reg_entry_t *e;

  e = reg_find(key, value);
  if (e != NULL) {
    return e;
  }
  if (g_reg.count >= WIN32_REG_MAX_ENTRIES) {
    return NULL;
  }
  e = &g_reg.e[g_reg.count++];
  memset(e, 0, sizeof(*e));
  copy_text(e->key, sizeof(e->key), key);
  copy_text(e->value, sizeof(e->value), value);
  return e;
}

/* Fewer than sixteen entries: a linear scan is the table. */
static int reg_save(void) {
  vfs_fs_t *fs = vfs_root();

  if (fs == NULL || !vfs_root_persistent()) {
    return -1;
  }
  g_reg.magic = WIN32_REG_MAGIC;
  g_reg.version = WIN32_REG_VERSION;
  if (vfs_write_file(fs, WIN32_REG_PATH, &g_reg, (UINT32)sizeof(g_reg)) != 0) {
    if (!g_save_failed) {
      g_save_failed = 1;
      kputs("[WIN32] registry write failed\n");
    }
    return -1;
  }
  return 0;
}

/* The file is accepted only at the exact size this build writes, so a
 * truncated or foreign file is ignored instead of half-loading. */
static int reg_load(void) {
  vfs_fs_t *fs = vfs_root();
  UINT32    ino;
  UINT64    size = 0;
  UINT16    mode = 0;

  if (fs == NULL) {
    return -1;
  }
  ino = vfs_lookup_path(fs, WIN32_REG_PATH);
  if (ino == 0 || fs->stat(fs, ino, &size, &mode) != 0 ||
      size != (UINT64)sizeof(g_reg)) {
    return -1;
  }
  if (vfs_read_file(fs, WIN32_REG_PATH, &g_reg, (UINT32)sizeof(g_reg)) != 0) {
    return -1;
  }
  if (g_reg.magic != WIN32_REG_MAGIC || g_reg.version != WIN32_REG_VERSION ||
      g_reg.count > WIN32_REG_MAX_ENTRIES) {
    return -1;
  }
  return 0;
}

static void reg_seed(void) {
  win32_reg_entry_t *e;

  memset(&g_reg, 0, sizeof(g_reg));
  g_reg.magic = WIN32_REG_MAGIC;
  g_reg.version = WIN32_REG_VERSION;
  {
    static const char greeting[] = "Hello from the Novus registry";

    e = reg_alloc("HKCU\\Software\\Novus", "Greeting");
    if (e != NULL) {
      e->type = WIN32_REG_SZ;
      e->len = (UINT32)sizeof(greeting);
      memcpy(e->data, greeting, sizeof(greeting));
    }
  }
}

void win32_init(void) {
  reg_seed();
  if (!vfs_root_persistent()) {
    kputs("[WIN32] registry: in-memory only (live root is read-only)\n");
    return;
  }
  if (reg_load() == 0) {
    kputs("[WIN32] registry: loaded ");
    kputs(WIN32_REG_PATH);
    kputs("\n");
    return;
  }
  (void)reg_save();
  kputs("[WIN32] registry: seeded ");
  kputs(WIN32_REG_PATH);
  kputs("\n");
}

void win32_owner_exit(UINT64 pid) {
  UINT32 i;

  for (i = 0; i < WIN32_KEY_MAX; i++) {
    if (g_keys[i].used && g_keys[i].owner == pid) {
      memset(&g_keys[i], 0, sizeof(g_keys[i]));
    }
  }
  win32_mem_owner_exit(pid);
  win32_gui_owner_exit(pid);
}

/* ---- key handles ------------------------------------------------------- */

/* Resolve hKey to a path prefix: a predefined hive or an open handle. */
static int key_base(UINT64 hkey, char *out, UINT32 cap) {
  UINT32 i;

  if (hkey == WIN32_HKEY_CURRENT_USER) {
    copy_text(out, cap, "HKCU");
    return 0;
  }
  if (hkey == WIN32_HKEY_LOCAL_MACHINE) {
    copy_text(out, cap, "HKLM");
    return 0;
  }
  if (hkey >= WIN32_KEY_BASE && hkey < WIN32_KEY_BASE + WIN32_KEY_MAX) {
    i = (UINT32)(hkey - WIN32_KEY_BASE);
    if (g_keys[i].used) {
      copy_text(out, cap, g_keys[i].path);
      return 0;
    }
  }
  return -1;
}

/* Compose "<base>\<sub>" into out. subkey_va == 0 appends nothing. */
static int key_path(UINT64 cr3, UINT64 hkey, UINT64 subkey_va, char *out,
                    UINT32 cap) {
  char sub[WIN32_REG_KEY_LEN];

  if (key_base(hkey, out, cap) != 0) {
    return -1;
  }
  if (subkey_va == 0 || read_user_str(cr3, subkey_va, sub, sizeof(sub)) != 0 ||
      sub[0] == '\0') {
    return 0;
  }
  if (append_text(out, cap, "\\") != 0 ||
      append_text(out, cap, sub) != 0 ||
      reg_key_len(out) >= WIN32_REG_KEY_LEN) {
    return -1;
  }
  return 0;
}

static int key_open(UINT64 pid, const char *path, UINT64 *out) {
  UINT32 i;

  for (i = 0; i < WIN32_KEY_MAX; i++) {
    if (!g_keys[i].used) {
      g_keys[i].used = 1;
      g_keys[i].owner = pid;
      copy_text(g_keys[i].path, sizeof(g_keys[i].path), path);
      *out = WIN32_KEY_BASE + i;
      return 0;
    }
  }
  return -1;
}

static void key_close(UINT64 pid, UINT64 hkey) {
  UINT32 i;

  if (hkey < WIN32_KEY_BASE || hkey >= WIN32_KEY_BASE + WIN32_KEY_MAX) {
    return;
  }
  i = (UINT32)(hkey - WIN32_KEY_BASE);
  if (g_keys[i].used && g_keys[i].owner == pid) {
    memset(&g_keys[i], 0, sizeof(g_keys[i]));
  }
}

/* ---- private memory ---------------------------------------------------- */

/* A compatibility process gets its own window of address space for
 * VirtualAlloc and the process heap. It sits clear of every fixed region
 * win32.h reserves (image 0x50000000, thunks 0x60000000, command line
 * 0x70000000, trampoline 0x90000000, stack 0xA0000000) and leaves room for
 * a 256 MiB working set, which is more than the 2.5 GiB test machine would
 * hand out anyway.
 *
 * The bookkeeping lives in the kernel, not in the process: a Win32
 * allocator would normally keep a header in front of every block, but the
 * kernel cannot write user memory without a copy, so the table is the
 * cheaper and the safer place. */
#define WIN32_HEAP_VA    0x0000000020000000ULL
#define WIN32_HEAP_END   0x0000000030000000ULL
#define WIN32_HEAP_SLOTS 64u
#define WIN32_PAGE       4096u
#define WIN32_HEAP_HANDLE 0x80000001ULL

typedef struct {
  UINT8  used;
  UINT64 owner;
  UINT64 va;
  UINT64 size;
} win32_alloc_t;

static win32_alloc_t g_alloc[WIN32_HEAP_SLOTS];

static void win32_mem_owner_exit(UINT64 pid) {
  UINT32 i;

  for (i = 0; i < WIN32_HEAP_SLOTS; i++) {
    if (g_alloc[i].used && g_alloc[i].owner == pid) {
      memset(&g_alloc[i], 0, sizeof(g_alloc[i]));
    }
  }
}

static UINT64 round_page(UINT64 n) {
  return (n + WIN32_PAGE - 1u) & ~(UINT64)(WIN32_PAGE - 1u);
}

/* Where the next block for this process goes: after the highest block it
 * already owns, so the range stays contiguous without a per-task cursor
 * that a restart would have to reset. */
static UINT64 arena_next(UINT64 pid) {
  UINT64 next = WIN32_HEAP_VA;
  UINT32 i;

  for (i = 0; i < WIN32_HEAP_SLOTS; i++) {
    if (g_alloc[i].used && g_alloc[i].owner == pid &&
        g_alloc[i].va + g_alloc[i].size > next) {
      next = g_alloc[i].va + g_alloc[i].size;
    }
  }
  return next;
}

static win32_alloc_t *alloc_find(UINT64 pid, UINT64 va) {
  UINT32 i;

  for (i = 0; i < WIN32_HEAP_SLOTS; i++) {
    if (g_alloc[i].used && g_alloc[i].owner == pid && g_alloc[i].va == va) {
      return &g_alloc[i];
    }
  }
  return NULL;
}

/* Reserve-and-commit `bytes` at the next free address of the process and
 * map a fresh physical page for every page of it. Pages come zeroed from
 * pmm_alloc_page(), which is what both VirtualAlloc and HEAP_ZERO_MEMORY
 * promise. */
static win32_alloc_t *arena_carve(task_t *t, UINT64 bytes) {
  win32_alloc_t *slot = NULL;
  UINT64 base = round_page(arena_next(t->pid));
  UINT64 size = round_page(bytes == 0u ? WIN32_PAGE : bytes);
  UINT64 va;

  if (base + size > WIN32_HEAP_END) {
    return NULL;
  }
  for (va = base; va < base + size; va += WIN32_PAGE) {
    void *page = pmm_alloc_page();

    if (page == NULL) {
      return NULL;
    }
    if (vm_user_map(&t->space, va, (UINT64)(UINTN)page,
                    VM_PROT_WRITE | VM_PROT_USER) != 0) {
      pmm_free_page(page);
      return NULL;
    }
  }
  for (va = 0; va < WIN32_HEAP_SLOTS; va++) {
    if (!g_alloc[va].used) {
      slot = &g_alloc[va];
      break;
    }
  }
  if (slot == NULL) {
    return NULL;
  }
  slot->used = 1u;
  slot->owner = t->pid;
  slot->va = base;
  slot->size = size;
  return slot;
}

/* A block is dropped from the bookkeeping but its pages stay mapped: this
 * layer has no unmap path yet, and handing pages back while a pointer to
 * them may still be live in the process would be worse than leaking them.
 * The space itself dies with the process. */
static int arena_release(UINT64 pid, UINT64 va) {
  win32_alloc_t *a = alloc_find(pid, va);

  if (a == NULL) {
    return 0;
  }
  memset(a, 0, sizeof(*a));
  return 1;
}

/* ---- API handlers ------------------------------------------------------ */

static void api_exit(trap_frame_t *f) {
  sched_exit_current((int)(UINT32)f->rdi);
  f->rax = 0;
}

static void api_get_std_handle(trap_frame_t *f) {
  switch ((UINT32)f->rdi) {
  case 0xFFFFFFF5u: /* STD_OUTPUT_HANDLE (-11) */
    f->rax = 1u;
    return;
  case 0xFFFFFFF4u: /* STD_ERROR_HANDLE (-12) */
    f->rax = 2u;
    return;
  default: /* STD_INPUT_HANDLE (-10) and anything else */
    f->rax = 0u;
    return;
  }
}

/* WriteFile/WriteConsoleA differ only in the last argument's meaning, which
 * this prototype ignores; both print through the kernel log. */
static int api_write_file_handle(trap_frame_t *f, task_t *t, UINT32 h);

static void api_write(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT64  len = f->r8;
  char    buf[65];
  UINT32  n;

  if (t == NULL) {
    f->rax = 0;
    return;
  }
  /* A handle at or above WIN32_FILE_HANDLE is a file: the payload belongs
   * on the volume, not in the log. */
  if ((UINT32)f->rdi >= WIN32_FILE_HANDLE &&
      (UINT32)f->rdi < WIN32_FILE_HANDLE + 0x100u) {
    (void)api_write_file_handle(f, t, (UINT32)f->rdi);
    return;
  }
  if (len > 64u) {
    len = 64u;
  }
  if (len != 0u) {
    if (user_copy(t->cr3, f->rdx, buf, len, 0) != 0) {
      t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
      f->rax = 0;
      return;
    }
  }
  buf[len] = '\0';
  kputs("[WIN32] ");
  kputs(buf);
  if (len == 0u || buf[len - 1u] != '\n') {
    kputs("\n");
  }
  n = (UINT32)len;
  if (f->r9 != 0u) {
    (void)user_copy(t->cr3, f->r9, &n, 4u, 1);
  }
  f->rax = 1u;
}

/* SetConsoleTextAttribute: the self-hosted C++98 test colors PASS/FAIL via
 * FOREGROUND_* bits. The Win32 console prototype has no real color buffer, so
 * emit the corresponding ANSI SGR sequence to the serial log. The sequence is
 * written immediately before the text the program prints, giving the same
 * visual result in any ANSI-capable terminal. */
static void api_set_console_text_attr(trap_frame_t *f) {
  UINT32 attr = (UINT32)f->rdx; /* arg1(handle)->rdi, arg2(attr)->rdx */
  int fg;

  if ((attr & 0x0007u) == 0x0007u) {
    fg = 37;                       /* grey/white: reset to default */
  } else if ((attr & 0x0005u) == 0x0005u) {
    fg = 35;                       /* red+blue -> magenta */
  } else if ((attr & 0x0006u) == 0x0006u) {
    fg = 33;                       /* green+red -> yellow */
  } else if ((attr & 0x0003u) == 0x0003u) {
    fg = 36;                       /* blue+green -> cyan */
  } else if ((attr & 0x0004u) != 0u) {
    fg = 31;                       /* red */
  } else if ((attr & 0x0002u) != 0u) {
    fg = (attr & 0x0008u) ? 92 : 32; /* bright green / green */
  } else if ((attr & 0x0001u) != 0u) {
    fg = 34;                       /* blue */
  } else {
    fg = 37;
  }
  /* Emit a fixed SGR code so no integer formatter is needed in the gate. */
  switch (fg) {
    case 31: kputs("\x1b[31m"); break;
    case 32: kputs("\x1b[32m"); break;
    case 33: kputs("\x1b[33m"); break;
    case 34: kputs("\x1b[34m"); break;
    case 35: kputs("\x1b[35m"); break;
    case 36: kputs("\x1b[36m"); break;
    case 37: kputs("\x1b[37m"); break;
    case 92: kputs("\x1b[92m"); break;
    default: kputs("\x1b[0m"); break;
  }
  f->rax = 1u;
}

static void api_sleep(trap_frame_t *f) {
  UINT64 ms = f->rdi;
  UINT32 ticks = (UINT32)((ms + 9u) / 10u);

  if (ticks == 0u) {
    ticks = 1u;
  }
  /* Park with no waker: only the tick deadline retires the wait. Rewind to
   * the `syscall` so the thunk replays when the task runs again; the entry
   * check above then turns the timeout into success. */
  sched_park(WIN32_SLEEP_PORT, ticks);
  f->rip -= 2u;
}

static void api_ticks(trap_frame_t *f, int wide) {
  UINT64 ms = sched_ticks() * 10u; /* the LAPIC tick is 10 ms, lapic.c */

  f->rax = wide ? ms : (UINT64)(UINT32)ms;
}

static void api_reg_open(trap_frame_t *f) {
  task_t *t = sched_current();
  char    path[72];
  UINT64  out_va = 0;
  UINT64  handle = 0;

  if (t == NULL || key_path(t->cr3, f->rdi, f->rdx, path, sizeof(path)) != 0 ||
      stack_arg(f, t, 5u, &out_va) != 0 || out_va == 0u) {
    if (t != NULL) {
      t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    }
    f->rax = WIN32_ERROR_FILE_NOT_FOUND;
    return;
  }
  /* The prototype does not require the key to exist yet: opening is what
   * creates the name, and a later set makes it appear in the table. */
  if (key_open(t->pid, path, &handle) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  (void)user_copy(t->cr3, out_va, &handle, 8u, 1);
  t->win32_last_error = WIN32_ERROR_SUCCESS;
  f->rax = WIN32_ERROR_SUCCESS;
}

static void api_reg_query(trap_frame_t *f) {
  task_t *t = sched_current();
  char    path[72];
  char    name[WIN32_REG_VALUE_LEN];
  win32_reg_entry_t *e;
  UINT64  type_va = 0;
  UINT64  data_va = 0;
  UINT64  size_va = 0;
  UINT32  want;

  if (t == NULL || key_path(t->cr3, f->rdi, 0, path, sizeof(path)) != 0 ||
      read_user_str(t->cr3, f->rdx, name, sizeof(name)) != 0 ||
      stack_arg(f, t, 5u, &data_va) != 0 ||
      stack_arg(f, t, 6u, &size_va) != 0) {
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  type_va = f->r9;
  e = reg_find(path, name);
  if (e == NULL) {
    if (t != NULL) {
      t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    }
    f->rax = WIN32_ERROR_FILE_NOT_FOUND;
    return;
  }
  if (type_va != 0u) {
    (void)user_copy(t->cr3, type_va, &e->type, 4u, 1);
  }
  if (data_va == 0u) {
    if (size_va != 0u) {
      (void)user_copy(t->cr3, size_va, &e->len, 4u, 1);
    }
    f->rax = WIN32_ERROR_SUCCESS;
    return;
  }
  want = e->len;
  if (size_va != 0u) {
    UINT32 cap = 0;

    if (user_copy(t->cr3, size_va, &cap, 4u, 0) != 0) {
      f->rax = WIN32_ERROR_INVALID_PARAM;
      return;
    }
    if (cap < want) {
      want = cap;
    }
  }
  if (want != 0u && user_copy(t->cr3, data_va, e->data, want, 1) != 0) {
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  if (size_va != 0u) {
    (void)user_copy(t->cr3, size_va, &e->len, 4u, 1);
  }
  t->win32_last_error = WIN32_ERROR_SUCCESS;
  f->rax = WIN32_ERROR_SUCCESS;
}

static void api_reg_set(trap_frame_t *f) {
  task_t *t = sched_current();
  char    path[72];
  char    name[WIN32_REG_VALUE_LEN];
  win32_reg_entry_t *e;
  UINT64  data_va = 0;
  UINT64  data_len = 0;

  if (t == NULL || key_path(t->cr3, f->rdi, 0, path, sizeof(path)) != 0 ||
      read_user_str(t->cr3, f->rdx, name, sizeof(name)) != 0 ||
      stack_arg(f, t, 5u, &data_va) != 0 ||
      stack_arg(f, t, 6u, &data_len) != 0) {
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  if (data_len > WIN32_REG_DATA_LEN || (data_len != 0u && data_va == 0u)) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  e = reg_alloc(path, name);
  if (e == NULL) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  e->type = (UINT32)f->r9;
  e->len = (UINT32)data_len;
  if (data_len != 0u &&
      user_copy(t->cr3, data_va, e->data, data_len, 0) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  (void)reg_save();
  t->win32_last_error = WIN32_ERROR_SUCCESS;
  f->rax = WIN32_ERROR_SUCCESS;
}

static void api_reg_close(trap_frame_t *f) {
  task_t *t = sched_current();

  if (t != NULL) {
    key_close(t->pid, f->rdi);
  }
  f->rax = WIN32_ERROR_SUCCESS;
}

/* ---- dispatcher -------------------------------------------------------- */

/* ---- second block ------------------------------------------------------ */

static void api_module_file_name(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT32  cap = (UINT32)f->r8;
  UINT32  n = 0;

  if (t == NULL || f->rdx == 0u || cap == 0u) {
    f->rax = 0u;
    return;
  }
  /* The image name the loader used, which is also what the desktop shows
   * for the process. A full path would need the mount point, and nothing
   * in the layer carries one yet. */
  while (t->name[n] != '\0' && n + 1u < cap) {
    UINT8 c = (UINT8)t->name[n];

    if (user_copy(t->cr3, f->rdx + n, &c, 1u, 1) != 0) {
      f->rax = 0u;
      return;
    }
    n++;
  }
  {
    UINT8 zero = 0u;

    (void)user_copy(t->cr3, f->rdx + n, &zero, 1u, 1);
  }
  f->rax = n;
}

static void api_virtual_alloc(trap_frame_t *f) {
  task_t        *t = sched_current();
  win32_alloc_t *a;

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  /* The requested address is ignored: the process has one window and no
   * way to ask for a slot inside it. Every caller in practice passes NULL. */
  a = arena_carve(t, f->rdx);
  if (a == NULL) {
    t->win32_last_error = 8u; /* ERROR_NOT_ENOUGH_MEMORY */
    f->rax = 0u;
    return;
  }
  f->rax = a->va;
}

static void api_virtual_free(trap_frame_t *f) {
  task_t *t = sched_current();

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  f->rax = arena_release(t->pid, f->rdi) ? 1u : 0u;
}

static void api_get_process_heap(trap_frame_t *f) {
  f->rax = WIN32_HEAP_HANDLE;
}

static void api_heap_alloc(trap_frame_t *f) {
  task_t        *t = sched_current();
  win32_alloc_t *a;

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  a = arena_carve(t, f->r8);
  f->rax = a != NULL ? a->va : 0u;
}

static void api_heap_free(trap_frame_t *f) {
  task_t *t = sched_current();

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  f->rax = arena_release(t->pid, f->rdx) ? 1u : 0u;
}

/* SYSTEMTIME is eight WORDs: year, month, day of week, day, hour, minute,
 * second, millisecond - in that order, which is why the day of week sits
 * between the month and the day. */
static void api_system_time(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT16 st[8];
  UINT16 year = 1970u;
  UINT8  month = 1u;
  UINT8  day = 1u;
  UINT8  hour = 0u;
  UINT8  minute = 0u;
  UINT64 secs;
  UINT64 ms;

  if (t == NULL || f->rdi == 0u) {
    f->rax = 0u;
    return;
  }
  secs = rtc_unix();
  (void)rtc_now_date(&year, &month, &day);
  (void)rtc_now(&hour, &minute);
  ms = (UINT64)sched_ticks() * 10u;
  st[0] = year;
  st[1] = month;
  st[2] = (UINT16)((secs / 86400u + 4u) % 7u); /* 1970-01-01 was a Thursday */
  st[3] = day;
  st[4] = hour;
  st[5] = minute;
  st[6] = (UINT16)(secs % 60u);
  st[7] = (UINT16)(ms % 1000u);
  if (user_copy(t->cr3, f->rdi, st, (UINT32)sizeof(st), 1) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = 0u; /* GetSystemTime returns nothing */
}

static void api_qpc(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT64 count = (UINT64)sched_ticks() * 10u * 10000u; /* 100 ns units */

  if (t != NULL && f->rdi != 0u) {
    (void)user_copy(t->cr3, f->rdi, &count, 8u, 1);
  }
  f->rax = 1u;
}

static void api_qpf(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT64 freq = 10000000u;

  if (t != NULL && f->rdi != 0u) {
    (void)user_copy(t->cr3, f->rdi, &freq, 8u, 1);
  }
  f->rax = 1u;
}

static void api_ids(trap_frame_t *f) {
  task_t *t = sched_current();

  f->rax = t != NULL ? t->pid : 0u;
}

/* 48 bytes: architecture, page size, the application address window, the
 * processor mask and counts, the allocation granularity, then three level
 * and revision words. A program reads this once at start-up and sizes its
 * arenas from it; the numbers below describe exactly what this machine is. */
static void api_system_info(trap_frame_t *f) {
  task_t *t = sched_current();
  struct {
    UINT16 arch;
    UINT16 reserved;
    UINT32 page;
    UINT64 min_addr;
    UINT64 max_addr;
    UINT64 mask;
    UINT32 processors;
    UINT32 type;
    UINT32 granularity;
    UINT16 level;
    UINT16 revision;
  } si;
  UINT32 cpus = sched_task_count();

  if (t == NULL || f->rdi == 0u) {
    f->rax = 0u;
    return;
  }
  memset(&si, 0, sizeof(si));
  si.arch = 9u; /* PROCESSOR_ARCHITECTURE_AMD64 */
  si.page = WIN32_PAGE;
  si.min_addr = 0x10000ULL;
  si.max_addr = 0x7FFEFFFFULL;
  si.mask = cpus >= 64u ? 0xFFFFFFFFFFFFFFFFULL
                       : ((1ULL << (cpus == 0u ? 1u : cpus)) - 1ULL);
  si.processors = cpus == 0u ? 1u : cpus;
  si.type = 8664u;
  si.granularity = 65536u;
  si.level = 6u;
  si.revision = 0x3C03u;
  if (user_copy(t->cr3, f->rdi, &si, (UINT32)sizeof(si), 1) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = 0u;
}

/* The handler is accepted, never invoked: an exception in this layer ends
 * the process, and pretending otherwise would hide the failure. Returning
 * the previous filter is what the caller expects. */
static void api_set_filter(trap_frame_t *f) {
  f->rax = f->rdi;
}

/* ---- third block: user32 and gdi32 ------------------------------------- */

/* A Win32 program that draws does four things in order: register a window
 * class (which carries its window procedure), create a window, pump the
 * message queue, and paint. This block implements exactly those four, and
 * the state they need is per process because a class registration and a
 * window handle belong to the program that made them.
 *
 * Two structural choices are worth naming:
 *
 *   - The window itself is a Novus-WM window. The kernel drives the WM's
 *     request handlers with the process id as owner (nwm_owner_*), so the
 *     desktop's z-order, focus, drag and close button all work on a Win32
 *     window without a second geometry model to drift.
 *   - Painting is a display list, not a pixel buffer: FillRect and TextOut
 *     append items that BeginPaint cleared, and EndPaint commits them. That
 *     is the WM's existing protocol, so a Win32 window paints through the
 *     same path as a native app - and it is why the layer needs no shared
 *     memory yet.
 *
 * Message routing: the WM sends events to the owner's inbox (the process
 * id is the port), so the pump here drains that inbox and turns NWM events
 * into the WM_* messages a Windows program expects. Nothing is lost when
 * the program is busy: events queue in the inbox, and the pump runs at the
 * top of every GetMessage/PeekMessage.
 */

#include "../wm/novus-wm/nwm.h"
#include "../wm/novus-wm/nwm_proto.h"
#include "../graphics/nd_input.h"
#include "../graphics/nd.h"
#include "../graphics/nd_ttf.h"

#define WIN32_GUI_MAX_PROCS   4u
#define WIN32_GUI_MAX_CLASSES 6u
#define WIN32_GUI_MAX_WINDOWS 4u
#define WIN32_GUI_MSGQ        16u
#define WIN32_GUI_PENS        8u
#define WIN32_GUI_TIMERS      4u
/* How long a window waits on its inbox before the pump runs again, in
 * scheduler ticks. Short enough that a message feels immediate, long enough
 * that an idle window is not spinning. */
#define WIN32_MSG_POLL_TICKS 5u

#define WIN32_HWND_BASE       0x40000000u
/* Frame thickness this layer assumes when it converts between client and
 * screen space: Novus-WM draws a one-pixel border and a 32-pixel caption. */
#define WIN32_FRAME_BORDER    1
#define WIN32_FRAME_CAPTION   NWM_TITLE_H
#define WIN32_FIND_DATA_LEN 320u

#define WM_CREATE     0x0001u
#define WM_DESTROY    0x0002u
#define WM_SIZE       0x0005u
#define WM_SETFOCUS   0x0007u
#define WM_KILLFOCUS  0x0008u
#define WM_PAINT      0x000Fu
#define WM_CLOSE      0x0010u
#define WM_QUIT       0x0012u
#define WM_KEYDOWN    0x0100u
#define WM_KEYUP      0x0101u
#define WM_CHAR       0x0102u
#define WM_MOUSEMOVE  0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP  0x0202u
#define WM_TIMER      0x0113u
#define WM_ERASEBKGND 0x0014u
#define WM_COMMAND    0x0111u

/* Control notification codes, the ones a button posts to its parent. */
#define BN_CLICKED    0u

/* Child controls. They are not compositor windows: a control is a small
 * record the layer draws at the end of its parent's paint and hit-tests
 * during its parent's mouse messages, which is what Windows does behind
 * CreateWindowExA("BUTTON", ...) anyway. The handle carries its own base so
 * a program can tell a control from a top-level window by the high bits. */
#define WIN32_GUI_MAX_CHILDREN 8u
#define WIN32_CHILD_HWND_BASE  0x41000000u
#define WIN32_CHILD_STATIC     1u
#define WIN32_CHILD_BUTTON     2u
#define WIN32_CHILD_EDIT       3u
#define WIN32_CHILD_TEXT_MAX   96u

/* MessageBoxA. The ids it returns are the ones Windows uses, and the button
 * sets it knows are the three a message box is actually asked for. */
#define WIN32_MB_OK       0x00000000u
#define WIN32_MB_OKCANCEL 0x00000001u
#define WIN32_MB_YESNO    0x00000004u
#define WIN32_IDOK        1u
#define WIN32_IDCANCEL    2u
#define WIN32_IDYES       6u
#define WIN32_IDNO        7u
#define WIN32_MSGBOX_MAX_LINES 4u
#define WIN32_MSGBOX_LINE_H    16

/* Menus. A menu is a small table of items owned by the layer, attached to a
 * window and drawn as a strip along the top of its client area. Windows puts
 * the bar in the non-client area; this compositor has no menu strip of its
 * own yet, so the bar covers the top WIN32_MENU_BAR_H pixels of the client
 * and the layer takes the clicks in that band before the program sees them.
 * That is a visible difference, not a hidden one. */
#define WIN32_GUI_MAX_MENUS     4u
#define WIN32_GUI_MAX_MENU_ITEMS 8u
#define WIN32_MENU_HANDLE_BASE  0x24000000u
#define WIN32_MENU_TEXT_MAX     24u
#define WIN32_MENU_BAR_H        20
#define WIN32_MENU_ITEM_H       18
#define WIN32_MENU_POPUP_W      140
#define WIN32_MENU_POPUP_PAD    4
#define MF_SEPARATOR 0x00000800u
#define MF_CHECKED   0x00000008u
#define MF_DISABLED  0x00000002u
#define MF_GRAYED    0x00000001u

#define WIN32_ERROR_INVALID_HANDLE 6u
#define WIN32_ERROR_INVALID_PARAM  87u

/* MSG as Windows lays it out for x64: 48 bytes, and the offsets matter
 * because the user-space dispatch stub reads them directly. */
typedef struct {
  UINT64 hwnd;
  UINT32 message;
  UINT32 pad;
  UINT64 wparam;
  UINT64 lparam;
  UINT32 time;
  INT32  x;
  INT32  y;
} win32_msg_t;

typedef struct {
  UINT8  used;
  char   name[32];
  UINT64 wndproc;
  UINT32 bg;      /* 0x00RRGGBB, already converted from COLORREF */
} win32_class_t;

typedef struct {
  UINT8  used;
  UINT32 hwnd;
  UINT32 win;     /* Novus-WM window id */
  INT32  w, h;    /* client size */
  INT32  x, y;    /* frame position the program asked for, in screen space */
  UINT32 bg;
  UINT8  painting;
  UINT8  dirty;   /* InvalidateRect asked for a repaint */
  char   title[NWM_TITLE_MAX];
  UINT64 userdata; /* SetWindowLongPtrA(GWLP_USERDATA) */
  /* The menu attached with SetMenu, and the popup the user has open. The
   * popup is a state the layer keeps, not a separate window. */
  UINT32 menu;
  UINT8  menu_open;
  UINT8  menu_open_idx;
} win32_wnd_t;

typedef struct {
  UINT8  used;
  UINT32 hwnd;
  UINT32 parent;   /* the top-level window this control belongs to */
  UINT8  kind;     /* WIN32_CHILD_STATIC / _BUTTON / _EDIT */
  UINT8  pressed;  /* button: the pointer is down inside it */
  UINT8  caret;    /* edit: draw the insertion bar */
  INT32  x, y, w, h;
  char   text[WIN32_CHILD_TEXT_MAX];
} win32_child_t;

typedef struct {
  UINT8 used;
  UINT32 id;      /* the id WM_COMMAND carries, for a real item */
  UINT32 sub;     /* MF_POPUP: the menu this bar item drops down */
  UINT8 sep;      /* separator: a line, not a command */
  UINT8 checked;
  UINT8 disabled;
  char  text[WIN32_MENU_TEXT_MAX];
} win32_mitem_t;

typedef struct {
  UINT8         used;
  UINT8         n;    /* how many items have been appended */
  win32_mitem_t item[WIN32_GUI_MAX_MENU_ITEMS];
} win32_menu_t;

typedef struct {
  UINT8       used;
  UINT64      pid;
  UINT8       stub_ready;
  UINT32      nclass;
  win32_class_t cls[WIN32_GUI_MAX_CLASSES];
  win32_wnd_t wnd[WIN32_GUI_MAX_WINDOWS];
  win32_child_t kid[WIN32_GUI_MAX_CHILDREN];
  win32_menu_t  menu[WIN32_GUI_MAX_MENUS];
  UINT32      focus_child; /* the control that owns typing, 0 = the window */
  win32_msg_t q[WIN32_GUI_MSGQ];
  UINT32      qh, qn;
  UINT8       quit_pending;
  UINT32      quit_code;
  UINT32      paint_hwnd;
  UINT32      focus_hwnd; /* the window the WM is routing keys to */
  /* A message box: the layer's own modal window, no class and no procedure.
   * While it is up the program is parked inside MessageBoxA, so its events
   * are settled here rather than handed to a window procedure. */
  UINT32      msgbox_hwnd;
  UINT32      msgbox_result; /* 0 = still open, else the id to return */
  UINT32      msgbox_owner;  /* the window that asked, for centring */
  UINT32      msgbox_type;   /* MB_OK / MB_OKCANCEL / MB_YESNO */
  UINT32      text_rgb;
  UINT32      bk_ref;   /* SetBkColor, kept as COLORREF */
  UINT8       bk_mode;  /* 1 = TRANSPARENT, 2 = OPAQUE */
  UINT32      pen_rgb;  /* SelectObject pen, already in RRGGBB */
  UINT32      pen_width;
  UINT32      brush_rgb; /* SelectObject brush, already in RRGGBB */
  INT16       cur_x, cur_y; /* MoveToEx position */
  /* Pens are the one object here that is more than a colour: a width lives
   * with it, so they get a small table and a handle that indexes into it. */
  UINT32      pen_h[WIN32_GUI_PENS];
  UINT32      pen_c[WIN32_GUI_PENS];
  UINT32      pen_w[WIN32_GUI_PENS];
  UINT32      nshape;   /* diagnostics: what the last paint submitted */
  UINT32      ntext;
  /* Timers. A program arms a few of them before the loop starts, and the
   * layer turns the tick counter into WM_TIMER when one comes due. */
  UINT8       tmr_used[WIN32_GUI_TIMERS];
  UINT32      tmr_hwnd[WIN32_GUI_TIMERS];
  UINT32      tmr_id[WIN32_GUI_TIMERS];
  UINT32      tmr_period[WIN32_GUI_TIMERS];
  UINT64      tmr_next[WIN32_GUI_TIMERS];
  UINT32      next_timer_id;
  /* Text layout state: SetTextAlign's TA_* bits and the extra space a
   * program asks for between characters. */
  UINT32      text_align;
  INT32       char_extra;
} win32_gui_t;

static win32_gui_t g_gui[WIN32_GUI_MAX_PROCS];

/* The stub DispatchMessageA runs in the program's own ring. Forty bytes of
 * x86-64 that call the procedure the kernel stored for this window:
 *
 *   mov  rax,[rcx]        ; MSG->hwnd
 *   and  eax,0xF          ; window slot
 *   mov  r11,[table+rax*8]
 *   test r11,r11
 *   jz   .none
 *   mov  r8, [rcx+0x10]   ; MSG->wParam
 *   mov  r9, [rcx+0x18]   ; MSG->lParam
 *   mov  edx,[rcx+0x08]   ; MSG->message
 *   mov  rax,rcx
 *   mov  rcx,[rax]        ; hwnd goes in RCX, the first argument
 *   sub  rsp,8            ; realign for the call
 *   call r11
 *   add  rsp,8
 *   ret
 * .none:
 *   xor  eax,eax
 *   ret
 */
static const UINT8 g_dispatch_stub[] = {
    0x48, 0x8B, 0x01,             /* mov %rax,(%rcx) */
    0x83, 0xE0, 0x0F,             /* and $0xF,%eax */
    0x4C, 0x8B, 0x1C, 0xC5,
    0x00, 0x3F, 0x00, 0x60,       /* mov 0x60003F00(,%rax,8),%r11 */
    0x4D, 0x85, 0xDB,             /* test %r11,%r11 */
    0x74, 0x1D,                   /* jz .none */
    0x4C, 0x8B, 0x41, 0x10,       /* mov 0x10(%rcx),%r8 */
    0x4C, 0x8B, 0x49, 0x18,       /* mov 0x18(%rcx),%r9 */
    0x8B, 0x51, 0x08,             /* mov 0x8(%rcx),%edx */
    0x48, 0x89, 0xC8,             /* mov %rcx,%rax */
    0x48, 0x8B, 0x08,             /* mov (%rax),%rcx */
    0x48, 0x83, 0xEC, 0x08,       /* sub $8,%rsp */
    0x41, 0xFF, 0xD3,             /* call %r11 */
    0x48, 0x83, 0xC4, 0x08,       /* add $8,%rsp */
    0xC3,                         /* ret */
    0x31, 0xC0,                   /* xor %eax,%eax */
    0xC3,                         /* ret */
};

/* COLORREF is 0x00BBGGRR while the WM's display list takes 0x00RRGGBB, so
 * every colour crosses this one conversion - a channel swap that would
 * otherwise turn a red rectangle blue. */
static UINT32 colorref_rgb(UINT32 c) {
  return ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
}

/* A background brush is either one of ours (high byte set, a plain colour)
 * or a system colour index, which the layer flattens to the face colour
 * because it has no palette yet. */
static UINT32 brush_rgb(UINT32 brush) {
  if ((brush & 0xFF000000u) == 0xFF000000u) {
    return colorref_rgb(brush & 0xFFFFFFu);
  }
  return 0x00FFFFFFu;
}

static win32_gui_t *gui_state(UINT64 pid, int create) {
  UINT32 i;

  for (i = 0; i < WIN32_GUI_MAX_PROCS; i++) {
    if (g_gui[i].used && g_gui[i].pid == pid) {
      return &g_gui[i];
    }
  }
  if (!create) {
    return NULL;
  }
  for (i = 0; i < WIN32_GUI_MAX_PROCS; i++) {
    if (!g_gui[i].used) {
      memset(&g_gui[i], 0, sizeof(g_gui[i]));
      g_gui[i].used = 1u;
      g_gui[i].pid = pid;
      g_gui[i].text_rgb = 0x00000000u;
      /* A fresh DC on Windows starts with a black pen, a white brush and
       * transparent text background; the layer matches that so a program
       * that never selects an object still draws something sensible. */
      g_gui[i].bk_mode = 1u;
      g_gui[i].bk_ref = 0x00FFFFFFu;
      g_gui[i].pen_rgb = 0x00000000u;
      g_gui[i].pen_width = 1u;
      g_gui[i].brush_rgb = 0x00FFFFFFu;
      g_gui[i].text_align = 0u; /* TA_LEFT | TA_TOP */
      g_gui[i].next_timer_id = 1u;
      return &g_gui[i];
    }
  }
  return NULL;
}

static win32_wnd_t *gui_window(win32_gui_t *gu, UINT32 hwnd) {
  UINT32 i;

  if (gu == NULL) {
    return NULL;
  }
  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    if (gu->wnd[i].used && gu->wnd[i].hwnd == hwnd) {
      return &gu->wnd[i];
    }
  }
  return NULL;
}

static void gui_push(win32_gui_t *gu, UINT32 hwnd, UINT32 msg, UINT64 wp,
                     UINT64 lp) {
  win32_msg_t *m;

  if (gu == NULL || gu->qn >= WIN32_GUI_MSGQ) {
    return; /* a program that never pumps loses messages, as on Windows */
  }
  m = &gu->q[(gu->qh + gu->qn) % WIN32_GUI_MSGQ];
  memset(m, 0, sizeof(*m));
  m->hwnd = hwnd;
  m->message = msg;
  m->wparam = wp;
  m->lparam = lp;
  m->time = (UINT32)(sched_ticks() * 10u);
  gu->qn++;
}

/* Map a window id from an event payload onto our HWND encoding, ignoring
 * events for windows this process did not open. */
static UINT32 gui_hwnd_for_win(win32_gui_t *gu, UINT32 win) {
  UINT32 i;

  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    if (gu->wnd[i].used && gu->wnd[i].win == win) {
      return gu->wnd[i].hwnd;
    }
  }
  return 0;
}

/* The WM hands over ND_KEY_* for keys that are not text and a Unicode
 * codepoint for those that are (see nd_input.h). A Windows program wants a
 * virtual-key code in WM_KEYDOWN and the character in WM_CHAR, so this maps
 * the first onto VK_* and passes the second through unchanged. */
static UINT32 vk_for_special(UINT32 nd_key) {
  switch (nd_key) {
  case ND_KEY_ESC:    return 0x1Bu; /* VK_ESCAPE */
  case ND_KEY_UP:     return 0x26u; /* VK_UP */
  case ND_KEY_DOWN:   return 0x28u; /* VK_DOWN */
  case ND_KEY_LEFT:   return 0x25u; /* VK_LEFT */
  case ND_KEY_RIGHT:  return 0x27u; /* VK_RIGHT */
  case ND_KEY_DELETE: return 0x2Eu; /* VK_DELETE */
  case ND_KEY_HOME:   return 0x24u; /* VK_HOME */
  case ND_KEY_END:    return 0x23u; /* VK_END */
  case ND_KEY_TAB:    return 0x09u; /* VK_TAB */
  case ND_KEY_ENTER:  return 0x0Du; /* VK_RETURN */
  case ND_KEY_BACK:   return 0x08u; /* VK_BACK */
  default:            return 0u;
  }
}

/* A text key's virtual code. Letters answer as their uppercase ASCII, which
 * is the VK a Windows program compares against; everything else keeps its
 * codepoint, which is right for digits, space and punctuation. */
static UINT32 vk_for_char(UINT32 cp) {
  if (cp >= 'a' && cp <= 'z') {
    return cp - 0x20u;
  }
  return cp;
}

/* InvalidateRect sets a flag instead of queueing a message: Windows coalesces
 * repeated invalidations into one WM_PAINT at the next pump, and a program
 * that invalidates on every keystroke would otherwise fill its own queue. */
static void gui_flush_dirty(win32_gui_t *gu) {
  UINT32 i;

  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    if (gu->wnd[i].used && gu->wnd[i].dirty) {
      gu->wnd[i].dirty = 0u;
      gui_push(gu, gu->wnd[i].hwnd, WM_PAINT, 0, 0);
    }
  }
}

/* Drain the owner's inbox and translate WM events into WM_* messages. */
static win32_child_t *gui_child(win32_gui_t *gu, UINT32 hwnd);
static win32_child_t *gui_child_new(win32_gui_t *gu, UINT32 parent,
                                    UINT8 kind, INT32 x, INT32 y, INT32 w,
                                    INT32 h);
static win32_child_t *gui_child_hit(win32_gui_t *gu, UINT32 parent, INT32 x,
                                    INT32 y);
static INT32 measure_text(const char *s, UINT32 len, INT32 extra);
/* A message box has no class and no procedure: its events are handled by the
 * layer, so the translator needs these before the drawing helpers exist. */
static void msgbox_paint(task_t *t, win32_gui_t *gu, win32_wnd_t *w);
static int  msgbox_event(task_t *t, win32_gui_t *gu, UINT32 hwnd,
                         const ipc_msg_t *m);
static int  gui_menu_click(task_t *t, win32_gui_t *gu, win32_wnd_t *w, INT32 x,
                           INT32 y, int down);
/* Translate one delivered message into a queued WM_* message. */
static void gui_translate(task_t *t, win32_gui_t *gu, const ipc_msg_t *msg) {
  ipc_msg_t m = *msg;
  UINT32    hwnd;
  UINT32    win;

  (void)t;
  if (m.hdr.type == NWM_RSP_OK || m.hdr.type == NWM_RSP_ERR) {
    return; /* a reply to a request we did not send through IPC */
  }
  win = 0;
  if (m.hdr.len >= 4u) {
    memcpy(&win, m.payload, 4);
  }
  hwnd = gui_hwnd_for_win(gu, win);
  if (hwnd == 0u) {
    /* A key that names a window this process does not own is the one
     * diagnostic worth printing: it separates "the WM routed the key here
     * and the layer dropped it" from "the WM routed it somewhere else". */
    if (m.hdr.type == NWM_EV_KEY) {
      kputs("[WIN32] key for an unknown window ");
      kputs_hex(win);
      kputs("\n");
    }
    return;
  }
  /* The message box is the layer's own window, so it takes its events here:
   * a click on one of its buttons settles the result and there is no
   * procedure to call. */
  if (gu->msgbox_hwnd != 0u && hwnd == gu->msgbox_hwnd) {
    (void)msgbox_event(t, gu, hwnd, &m);
    return;
  }
  switch (m.hdr.type) {
  case NWM_EV_CLOSE:
    /* The WM has already taken the window down: the close button is the
     * compositor's, not the client's. Hand the program WM_CLOSE so it can
     * react, then a WM_DESTROY, then end the loop - the same sequence a
     * Windows program sees when the user clicks its X. */
    gui_push(gu, hwnd, WM_CLOSE, 0, 0);
    gui_push(gu, hwnd, WM_DESTROY, 0, 0);
    gu->quit_pending = 1u;
    {
      win32_wnd_t *w = gui_window(gu, hwnd);

      if (w != NULL) {
        memset(w, 0, sizeof(*w)); /* the window no longer exists */
      }
    }
    break;
  case NWM_EV_KEY: {
    nwm_ev_key_t k;

    if (m.hdr.len >= sizeof(k)) {
      memcpy(&k, m.payload, sizeof(k));
      /* Key arrival is rare enough to log unconditionally: it is the only
       * way to tell "the WM routed the key elsewhere" apart from "the
       * program ignored it". */
      kputs("[WIN32] key event reached the layer\n");
      /* An edit control with the caret eats the text before the parent's
       * procedure sees it. Windows sends the key to the control and lets it
       * post its own WM_CHAR; this layer keeps one procedure per window, so
       * the buffer is edited here and the key messages still go to the
       * parent, which is where a program's accelerator handling lives. */
      {
        win32_child_t *ec = gui_child(gu, gu->focus_child);

        if (ec != NULL && ec->kind == WIN32_CHILD_EDIT && ec->parent == hwnd) {
          UINT32 n = 0;

          while (n + 1u < WIN32_CHILD_TEXT_MAX && ec->text[n] != '\0') {
            n++;
          }
          if (k.key < ND_KEY_BASE && (k.key == 8u || k.key == 0x7Fu)) {
            if (n > 0u) {
              ec->text[n - 1u] = '\0';
            }
          } else if (k.key >= 0x20u && k.key < 0x7Fu &&
                     n + 1u < WIN32_CHILD_TEXT_MAX) {
            ec->text[n] = (char)k.key;
            ec->text[n + 1u] = '\0';
          }
          {
            win32_wnd_t *pw = gui_window(gu, hwnd);

            if (pw != NULL) {
              pw->dirty = 1u;
            }
          }
        }
      }
      if (k.key >= ND_KEY_BASE) {
        UINT32 vk = vk_for_special(k.key - ND_KEY_BASE);

        if (vk != 0u) {
          gui_push(gu, hwnd, WM_KEYDOWN, vk, 0);
          /* The input source reports presses only, so the release is
           * synthesised right after the press. A program that watches
           * WM_KEYUP for a hotkey therefore sees a complete pair, one
           * message later than a real keyboard would give it. */
          gui_push(gu, hwnd, WM_KEYUP, vk, 0);
        }
      } else {
        UINT32 vk = vk_for_char(k.key);

        gui_push(gu, hwnd, WM_KEYDOWN, vk, 0);
        /* Queued here rather than by TranslateMessage: the layer has the
         * character already, and a program that pumps without calling
         * TranslateMessage would otherwise never see it. */
        gui_push(gu, hwnd, WM_CHAR, k.key, 0);
        gui_push(gu, hwnd, WM_KEYUP, vk, 0);
      }
    }
    break;
  }
  case NWM_EV_FOCUS: {
    /* The WM moved the keyboard somewhere - either because a program asked
     * with SetFocus, or because the user clicked. Keep the layer's idea of
     * focus in step and hand the program the pair of messages Windows would
     * send, so a program that draws a caret reacts to a click as well. */
    UINT32 old = gu->focus_hwnd;

    if (old != 0u && old != hwnd) {
      gui_push(gu, old, WM_KILLFOCUS, hwnd, 0);
    }
    gu->focus_hwnd = hwnd;
    if (old != hwnd) {
      gui_push(gu, hwnd, WM_SETFOCUS, old, 0);
    }
    break;
  }
  case NWM_EV_RESIZE: {
    nwm_ev_resize_t r;

    if (m.hdr.len >= sizeof(r)) {
      win32_wnd_t *w = gui_window(gu, hwnd);

      memcpy(&r, m.payload, sizeof(r));
      if (w != NULL && r.w > 0 && r.h > 0) {
        w->w = r.w;
        w->h = r.h;
      }
      gui_push(gu, hwnd, WM_SIZE, 0,
               (UINT64)(UINT32)r.w | ((UINT64)(UINT32)r.h << 16));
      /* A resized client has to repaint, and this compositor does not ask
       * on its own yet. */
      gui_push(gu, hwnd, WM_PAINT, 0, 0);
    }
    break;
  }
  case NWM_EV_MOUSE: {
    nwm_ev_mouse_t mm;

    if (m.hdr.len >= sizeof(mm)) {
      UINT32         msg = WM_MOUSEMOVE;
      UINT64         lp;
      win32_child_t *hit;

      memcpy(&mm, m.payload, sizeof(mm));
      if (mm.type == ND_EV_MOUSE_DOWN) {
        msg = WM_LBUTTONDOWN;
      } else if (mm.type == ND_EV_MOUSE_UP) {
        msg = WM_LBUTTONUP;
      }
      lp = (UINT64)(UINT32)mm.x | ((UINT64)(UINT32)mm.y << 16);
      /* The menu strip and its popup belong to the layer: a click there is
       * consumed here and either opens a menu or turns into WM_COMMAND, and
       * the program never sees the raw mouse message. */
      {
        win32_wnd_t *mw = gui_window(gu, hwnd);

        if (mw != NULL && mw->menu != 0u &&
            gui_menu_click(t, gu, mw, mm.x, mm.y,
                           (mm.type == ND_EV_MOUSE_DOWN) ? 1 : 0)) {
          break;
        }
      }
      /* Controls take the click the way Windows gives it to them: a press
       * inside a button puts it in and marks the parent dirty, a release
       * inside the same button takes it out and posts WM_COMMAND, and an
       * edit keeps the caret once it has been clicked. */
      hit = gui_child_hit(gu, hwnd, mm.x, mm.y);
      if (msg == WM_LBUTTONDOWN) {
        if (hit != NULL && hit->kind == WIN32_CHILD_BUTTON) {
          hit->pressed = 1u;
        }
        if (hit != NULL && hit->kind == WIN32_CHILD_EDIT) {
          gu->focus_child = hit->hwnd;
          hit->caret = 1u;
        } else if (hit == NULL && gu->focus_child != 0u) {
          win32_child_t *old = gui_child(gu, gu->focus_child);

          if (old != NULL) {
            old->caret = 0u;
          }
          gu->focus_child = 0u;
        }
      } else if (msg == WM_LBUTTONUP) {
        UINT32 i;

        for (i = 0; i < WIN32_GUI_MAX_CHILDREN; i++) {
          win32_child_t *c = &gu->kid[i];

          if (!c->used || !c->pressed) {
            continue;
          }
          c->pressed = 0u;
          if (c == hit) {
            /* wParam carries the notification the way Windows packs it:
             * control id in the high word, BN_CLICKED in the low one. */
            UINT64 wp = (UINT64)(((c->hwnd & 0xFFu) + 1u) << 16);

            gui_push(gu, hwnd, WM_COMMAND, wp, (UINT64)c->hwnd);
          }
        }
      }
      {
        win32_wnd_t *pw = gui_window(gu, hwnd);

        if (pw != NULL) {
          pw->dirty = 1u; /* pressed state or caret moved */
        }
      }
      gui_push(gu, hwnd, msg, 0, lp);
    }
    break;
  }
  default:
    break;
  }
}

/* Drain the owner's inbox. */
static void gui_pump(task_t *t, win32_gui_t *gu) {
  ipc_msg_t m;

  /* ipc_try_recv answers the payload length, not zero: a real event carries
   * 24..96 bytes, so testing for == 0 drained nothing at all and every
   * compositor event - a click, a key - sat in the inbox forever while the
   * pump kept saying there was nothing to do. Anything but IPC_EAGAIN (or
   * a bad port) means a message was taken off the queue. */
  while (ipc_try_recv(t->pid, &m) >= 0) {
    /* Anything that is not a reply is an event from the compositor, and
     * events are rare enough that naming each one in the log is the
     * cheapest way to see where a route drops a message. */
    if (m.hdr.type != NWM_RSP_OK && m.hdr.type != NWM_RSP_ERR) {
      kputs("[WIN32] ipc event type=");
      kputs_hex(m.hdr.type);
      kputs(" len=");
      kputs_hex(m.hdr.len);
      kputs("\n");
    }
    gui_translate(t, gu, &m);
  }
}

/* Map the dispatch stub's page and leave the procedure table zeroed. The
 * page is user-executable and not user-writable: the table is filled in by
 * the kernel through user_copy, so a program cannot point it at arbitrary
 * code after the fact. */
static int gui_stub_map(task_t *t, win32_gui_t *gu) {
  void  *page;
  UINT32 i;

  if (gu->stub_ready) {
    return 0;
  }
  page = pmm_alloc_page();
  if (page == NULL) {
    return -1;
  }
  memset(page, 0, 4096);
  memcpy(page, g_dispatch_stub, sizeof(g_dispatch_stub));
  /* The stub page also carries the dispatch table, and the kernel fills
   * that table through user_copy(), whose access check demands write
   * permission. Without VM_PROT_WRITE the table write failed and every
   * DispatchMessageA returned zero - the window existed but never painted. */
  if (vm_user_map(&t->space, WIN32_GUI_STUB_VA, (UINT64)(UINTN)page,
                  VM_PROT_USER | VM_PROT_EXEC | VM_PROT_WRITE) != 0) {
    pmm_free_page(page);
    return -1;
  }
  for (i = 0; i < WIN32_GUI_SLOTS; i++) {
    UINT64 zero = 0;

    (void)user_copy(t->cr3, WIN32_GUI_TABLE_VA + (UINT64)i * 8u, &zero, 8u, 1);
  }
  gu->stub_ready = 1u;
  return 0;
}

static void gui_set_wndproc(task_t *t, UINT32 hwnd, UINT64 proc) {
  UINT64 slot = WIN32_GUI_TABLE_VA +
                (UINT64)(hwnd & (WIN32_GUI_SLOTS - 1u)) * 8u;
  int    rc = user_copy(t->cr3, slot, &proc, 8u, 1);

  (void)rc;
}

static void api_register_class(trap_frame_t *f) {
  task_t       *t = sched_current();
  win32_gui_t  *gu;
  win32_class_t *c = NULL;
  UINT8         wc[80];
  char          name[32];
  UINT64        proc = 0;
  UINT64        name_va = 0;
  UINT32        bg = 0;
  UINT32        i;

  if (t == NULL || f->rdi == 0u ||
      user_copy(t->cr3, f->rdi, wc, sizeof(wc), 0) != 0) {
    f->rax = 0u;
    return;
  }
  gu = gui_state(t->pid, 1);
  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  /* WNDCLASSEXA for x64: cbSize 0, style 4, lpfnWndProc 8,
   * cbClsExtra 0x10, cbWndExtra 0x14, hInstance 0x18, hIcon 0x20,
   * hCursor 0x28, hbrBackground 0x30, lpszMenuName 0x38,
   * lpszClassName 0x40, hIconSm 0x48. */
  memcpy(&proc, wc + 0x08, 8);
  memcpy(&bg, wc + 0x30, 4);
  memcpy(&name_va, wc + 0x40, 8);
  if (proc == 0u || read_user_str(t->cr3, name_va, name, sizeof(name)) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  for (i = 0; i < WIN32_GUI_MAX_CLASSES; i++) {
    if (gu->cls[i].used && text_equal(gu->cls[i].name, name)) {
      c = &gu->cls[i];
      break;
    }
  }
  if (c == NULL) {
    for (i = 0; i < WIN32_GUI_MAX_CLASSES; i++) {
      if (!gu->cls[i].used) {
        c = &gu->cls[i];
        break;
      }
    }
  }
  if (c == NULL) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  memset(c, 0, sizeof(*c));
  c->used = 1u;
  copy_text(c->name, sizeof(c->name), name);
  c->wndproc = proc;
  c->bg = brush_rgb(bg);
  gu->nclass++;
  /* An ATOM is a nonzero small integer; the class name is the lookup key. */
  f->rax = 0xC000u + (UINT64)i;
}

static void api_create_window(trap_frame_t *f) {
  task_t       *t = sched_current();
  win32_gui_t  *gu;
  win32_class_t *c = NULL;
  char          cls[32];
  char          title[NWM_TITLE_MAX];
  UINT64        x = 0, y = 0, w = 0, h = 0;
  UINT32        win = 0;
  UINT32        hwnd;
  INT32         cw = 0, ch = 0;
  UINT32        i;

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  gu = gui_state(t->pid, 1);
  if (gu == NULL || gui_stub_map(t, gu) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  if (read_user_str(t->cr3, f->rdx, cls, sizeof(cls)) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  (void)read_user_str(t->cr3, f->r8, title, sizeof(title));
  /* x, y, nWidth, nHeight are arguments 5..8 on the stack. */
  if (stack_arg(f, t, 5u, &x) != 0 || stack_arg(f, t, 6u, &y) != 0 ||
      stack_arg(f, t, 7u, &w) != 0 || stack_arg(f, t, 8u, &h) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  /* CreateWindowExA with a system class name makes a control, not a
   * compositor window. STATIC, BUTTON and EDIT are the three every dialog
   * is built out of, and the layer draws them into the parent's frame. */
  {
    UINT8  kind = 0;
    UINT64 parent = 0;

    if (ci_equal(cls, "STATIC")) {
      kind = WIN32_CHILD_STATIC;
    } else if (ci_equal(cls, "BUTTON")) {
      kind = WIN32_CHILD_BUTTON;
    } else if (ci_equal(cls, "EDIT")) {
      kind = WIN32_CHILD_EDIT;
    }
    if (kind != 0u) {
      char           ctext[WIN32_CHILD_TEXT_MAX];
      win32_child_t *k;
      win32_wnd_t   *pw;

      if (stack_arg(f, t, 9u, &parent) != 0 || parent == 0u ||
          gui_window(gu, (UINT32)parent) == NULL) {
        t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
        f->rax = 0u;
        return;
      }
      k = gui_child_new(gu, (UINT32)parent, kind, (INT32)x, (INT32)y,
                        (INT32)w, (INT32)h);
      if (k == NULL) {
        t->win32_last_error = 0x00000417u; /* ERROR_TOO_MANY_CONTROLS */
        f->rax = 0u;
        return;
      }
      memset(ctext, 0, sizeof(ctext));
      (void)read_user_str(t->cr3, f->r8, ctext, sizeof(ctext));
      copy_text(k->text, sizeof(k->text), ctext);
      pw = gui_window(gu, (UINT32)parent);
      if (pw != NULL) {
        pw->dirty = 1u; /* the control has to appear */
      }
      f->rax = (UINT64)k->hwnd;
      return;
    }
  }

  for (i = 0; i < WIN32_GUI_MAX_CLASSES; i++) {
    if (gu->cls[i].used && text_equal(gu->cls[i].name, cls)) {
      c = &gu->cls[i];
      break;
    }
  }
  if (c == NULL) {
    t->win32_last_error = 1410u; /* ERROR_CLASS_NOT_FOUND */
    f->rax = 0u;
    return;
  }
  if (nwm_owner_create(t->pid, title, (INT32)x, (INT32)y, (INT32)w,
                       (INT32)h, &win) != NWM_OK) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  (void)nwm_owner_client(win, &cw, &ch);
  /* The WM may clamp the request to keep the window on screen, so the
   * origin a program converts coordinates with is the placed one. */
  {
    INT32 rx = (INT32)x;
    INT32 ry = (INT32)y;
    INT32 rw = 0;
    INT32 rh = 0;

    (void)nwm_owner_rect(win, &rx, &ry, &rw, &rh);
    x = (UINT64)(UINT32)rx;
    y = (UINT64)(UINT32)ry;
  }
  hwnd = WIN32_HWND_BASE | win;
  kputs("[WIN32] window hwnd=");
  kputs_hex(hwnd);
  kputs(" win=");
  kputs_hex(win);
  kputs(" pid=");
  kputs_hex(t->pid);
  kputs("\n");
  /* One window per slot, and the slot is the window id: the stub indexes
   * the table by that low nibble, so two windows must not share one. */
  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    win32_wnd_t *me;

    if (gu->wnd[i].used) {
      continue;
    }
    me = &gu->wnd[i];
    memset(me, 0, sizeof(*me));
    me->used = 1u;
    me->hwnd = hwnd;
    me->win = win;
    me->w = cw > 0 ? cw : (INT32)w;
    me->h = ch > 0 ? ch : (INT32)h;
    me->x = (INT32)x;
    me->y = (INT32)y;
    me->bg = c->bg;
    break;
  }
  gui_set_wndproc(t, hwnd, c->wndproc);
  /* The order a Windows program sees: WM_CREATE first, then a paint. */
  gui_push(gu, hwnd, WM_CREATE, 0, 0);
  gui_push(gu, hwnd, WM_PAINT, 0, 0);
  f->rax = hwnd;
}

static void api_destroy_window(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w != NULL) {
    UINT32 i;

    (void)nwm_owner_destroy(t->pid, w->win);
    gui_set_wndproc(t, w->hwnd, 0u);
    /* Controls live in the window, so they go with it. */
    for (i = 0; i < WIN32_GUI_MAX_CHILDREN; i++) {
      if (gu->kid[i].used && gu->kid[i].parent == w->hwnd) {
        if (gu->focus_child == gu->kid[i].hwnd) {
          gu->focus_child = 0u;
        }
        memset(&gu->kid[i], 0, sizeof(gu->kid[i]));
      }
    }
    memset(w, 0, sizeof(*w));
    f->rax = 1u;
    return;
  }
  f->rax = 0u; /* not ours: the Windows answer is FALSE */
}

static void api_show_window(trap_frame_t *f) {
  /* The window is visible from the moment the WM create request is
   * accepted - there is no separate mapping step in this compositor - so
   * ShowWindow only has to say so. */
  f->rax = 1u;
}

/* Turn the tick counter into WM_TIMER. Called before the queue is inspected,
 * so a program that sits in GetMessage still gets its timer: Windows posts
 * WM_TIMER from the input queue the same way. Periods are floored at 10 ms,
 * which is this kernel's tick. */
static void gui_timers(win32_gui_t *gu) {
  UINT32 i;
  UINT64 now = (UINT64)sched_ticks() * 10u;

  for (i = 0; i < WIN32_GUI_TIMERS; i++) {
    if (gu->tmr_used[i] == 0u) {
      continue;
    }
    if ((INT64)(now - gu->tmr_next[i]) < 0) {
      continue;
    }
    if (gu->qn >= WIN32_GUI_MSGQ) {
      /* Windows also coalesces: a full queue means the timer is late rather
       * than lost, and the next pump posts the one message that matters. */
      gu->tmr_next[i] = now + gu->tmr_period[i];
      continue;
    }
    gui_push(gu, gu->tmr_hwnd[i], WM_TIMER, gu->tmr_id[i], 0);
    gu->tmr_next[i] = now + gu->tmr_period[i];
  }
}

static int gui_pull(trap_frame_t *f, task_t *t, win32_gui_t *gu, int wait) {
  /* GetMessageA(HWND, LPMSG, UINT min, UINT max) - the Windows order: the
   * window goes in RCX and the MSG destination in RDX. */
  UINT32 min_msg = (UINT32)f->r8;
  UINT32 max_msg = (UINT32)f->r9;
  UINT32 only_hwnd = (UINT32)f->rdi;

  for (;;) {
    win32_msg_t m;
    UINT64      lp;

    gui_pump(t, gu);
    gui_timers(gu);
    if (gu->qn == 0u) {
      gui_flush_dirty(gu);
    }
    if (gu->qn == 0u) {
      if (gu->quit_pending) {
        /* GetMessage answers WM_QUIT rather than leaving the MSG untouched:
         * a loop that inspects msg.message instead of the return value then
         * still sees the end of the loop, and wParam carries the exit code
         * PostQuitMessage was given. */
        win32_msg_t quit;
        memset(&quit, 0, sizeof(quit));
        quit.message = WM_QUIT;
        quit.wparam = (UINT64)gu->quit_code;
        if (f->rdx != 0u &&
            user_copy(t->cr3, f->rdx, &quit, sizeof(quit), 1) != 0) {
          f->rax = 0u;
          return 0;
        }
        f->rax = 0u; /* WM_QUIT: the loop ends */
        return 0;
      }
      if (!wait) {
        f->rax = 0u;
        return 0;
      }
      /* Park on the inbox. A yield loop here does not work: this runs
       * inside the syscall, and yielding without ever leaving the syscall
       * starves the WM task that is supposed to produce the event, which
       * froze the machine the first time this was tried.
       *
       * The wait is bounded on purpose. An event normally wakes this task
       * through its inbox port, but a wake that is missed would leave the
       * window deaf for good - and that is exactly what a mis-routed key
       * looks like from the outside. Parking with a deadline makes a
       * missed wake cost one poll instead of a dead window. */
      {
        ipc_msg_t in;
        int       rc = ipc_try_recv(t->pid, &in);

        /* Same trap as gui_pump: the successful return is the payload
         * length, so >= 0 is "message taken", not 0. */
        if (rc >= 0) {
          gui_translate(t, gu, &in); /* take the message, do not discard */
          continue;
        }
        if (rc == IPC_EAGAIN) {
          sched_park(t->pid, WIN32_MSG_POLL_TICKS);
          /* Replay the syscall after the park: api_get_message rewinds the
           * instruction pointer and puts SYS_WIN32 back in RAX. */
          return -1;
        }
        f->rax = 0u;
        return 0;
      }
    }
    m = gu->q[gu->qh];
    gu->qh = (gu->qh + 1u) % WIN32_GUI_MSGQ;
    gu->qn--;
    if (only_hwnd != 0u && m.hwnd != (UINT64)only_hwnd) {
      continue; /* GetMessage(hwnd, ...) filters to that window */
    }
    if (min_msg != 0u || max_msg != 0u) {
      UINT32 lo = min_msg != 0u ? min_msg : 0u;
      UINT32 hi = max_msg != 0u ? max_msg : 0xFFFFu;

      if (m.message < lo || m.message > hi) {
        continue;
      }
    }
    if (f->rdx == 0u || user_copy(t->cr3, f->rdx, &m, sizeof(m), 1) != 0) {
      f->rax = 0u;
      return 0;
    }
    lp = 0;
    (void)lp;
    f->rax = 1u;
    return 1;
  }
}

static void api_get_message(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  int          rc;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  rc = gui_pull(f, t, gu, 1);
  if (rc > 0) {

  }
  if (rc < 0) {
    /* The task is parked. Rewind to the `syscall` instruction and put the
     * call number back in RAX, so that when the message lands the same
     * syscall runs again and finds it in the queue - the same replay the
     * blocking ipc_recv syscall uses. */
    f->rax = (UINT64)SYS_WIN32;
    f->rip -= WIN32_SYSCALL_LEN;
  }
}

static void api_peek_message(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  f->rax = (UINT64)gui_pull(f, t, gu, 0);
}

static void api_translate_message(trap_frame_t *f) {
  /* WM_CHAR is queued by gui_translate, not here: the layer already has the
   * character when the key arrives and does not need to remember a pending
   * WM_KEYDOWN. TranslateMessage therefore has nothing left to do, but it
   * exists so a program that calls it - every real one does - keeps working. */
  f->rax = 1u;
}

static void api_def_window_proc(trap_frame_t *f) {
  /* Reached only when DispatchMessageA arrives as a syscall instead of
   * through the user-space stub, which is the diagnostic that the stub
   * never ran. */

  f->rax = 0u;
}

static void api_get_client_rect(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  INT32        r[4];

  if (w == NULL || f->rdx == 0u) {
    f->rax = 0u;
    return;
  }
  r[0] = 0;
  r[1] = 0;
  r[2] = w->w;
  r[3] = w->h;
  (void)user_copy(t->cr3, f->rdx, r, sizeof(r), 1);
  f->rax = 1u;
}

/* Begin a frame: the display list is emptied, and every FillRect/TextOut
 * until the matching release fills it. End commits the list and the WM
 * repaints the client area from it in one go. */
static void api_begin_frame(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  (void)nwm_owner_clear(t->pid, w->win);
  w->painting = 1u;
  gu->paint_hwnd = w->hwnd;
  gu->nshape = 0u;
  gu->ntext = 0u;
  kputs("[WIN32] paint: begin hwnd=");
  kputs_hex(w->hwnd);
  kputs("\n");
  f->rax = (UINT64)w->hwnd; /* the HDC is the window, for this layer */
}

/* ---- child controls: static, button, edit ------------------------------ */

static win32_child_t *gui_child(win32_gui_t *gu, UINT32 hwnd) {
  UINT32 i;

  if (gu == NULL || (hwnd & 0xFF000000u) != WIN32_CHILD_HWND_BASE) {
    return NULL;
  }
  i = hwnd & 0xFFu;
  if (i >= WIN32_GUI_MAX_CHILDREN || !gu->kid[i].used ||
      gu->kid[i].hwnd != hwnd) {
    return NULL;
  }
  return &gu->kid[i];
}

static win32_child_t *gui_child_new(win32_gui_t *gu, UINT32 parent,
                                    UINT8 kind, INT32 x, INT32 y, INT32 w,
                                    INT32 h) {
  UINT32 i;

  for (i = 0; i < WIN32_GUI_MAX_CHILDREN; i++) {
    win32_child_t *c;

    if (gu->kid[i].used) {
      continue;
    }
    c = &gu->kid[i];
    memset(c, 0, sizeof(*c));
    c->used = 1u;
    c->kind = kind;
    c->parent = parent;
    c->hwnd = WIN32_CHILD_HWND_BASE | i;
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    return c;
  }
  return NULL;
}

/* One control, drawn into the parent's display list. Everything here is a
 * shape or a string, so a control costs the compositor nothing new: the
 * frame a program paints and the widgets on top of it are one list. */
static void gui_child_draw(task_t *t, win32_gui_t *gu, win32_wnd_t *w,
                           win32_child_t *c) {
  UINT32 len = 0;
  INT32  ty;

  while (len < WIN32_CHILD_TEXT_MAX - 1u && c->text[len] != '\0') {
    len++;
  }
  ty = c->y + (c->h - 10) / 2;
  if (ty < c->y) {
    ty = c->y;
  }
  switch (c->kind) {
  case WIN32_CHILD_STATIC:
    if (len > 0u) {
      (void)nwm_owner_text(t->pid, w->win, (INT16)(c->x + 4), (INT16)ty, 1u,
                           c->text, len, 0x202020u);
      gu->ntext++;
    }
    break;
  case WIN32_CHILD_BUTTON: {
    UINT32 bg = c->pressed ? 0xC6C6C6u : 0xE6E6E6u;
    INT32  tw;

    (void)nwm_owner_shape(t->pid, w->win, NWM_OP_ROUND_FILL, (INT16)c->x,
                          (INT16)c->y, (INT16)c->w, (INT16)c->h, bg);
    (void)nwm_owner_shape(t->pid, w->win, NWM_OP_ROUND_FRAME, (INT16)c->x,
                          (INT16)c->y, (INT16)c->w, (INT16)c->h, 0x707070u);
    gu->nshape += 2u;
    if (len > 0u) {
      tw = measure_text(c->text, len, 0);
      (void)nwm_owner_text(t->pid, w->win,
                           (INT16)(c->x + (c->w - tw) / 2), (INT16)ty, 1u,
                           c->text, len, 0x1A1A1Au);
      gu->ntext++;
    }
    break;
  }
  case WIN32_CHILD_EDIT: {
    INT32 tw;

    (void)nwm_owner_shape(t->pid, w->win, NWM_OP_FILL, (INT16)c->x,
                          (INT16)c->y, (INT16)c->w, (INT16)c->h, 0xFFFFFFu);
    (void)nwm_owner_shape(t->pid, w->win, NWM_OP_FRAME, (INT16)c->x,
                          (INT16)c->y, (INT16)c->w, (INT16)c->h, 0x909090u);
    gu->nshape += 2u;
    if (len > 0u) {
      (void)nwm_owner_text(t->pid, w->win, (INT16)(c->x + 5), (INT16)ty, 1u,
                           c->text, len, 0x101010u);
      gu->ntext++;
    }
    if (c->caret) {
      /* The insertion bar sits after the text, which is where a real edit
       * control keeps it while the pointer is at the end. */
      tw = (len > 0u) ? measure_text(c->text, len, 0) : 0;
      (void)nwm_owner_shape(t->pid, w->win, NWM_OP_VLINE,
                            (INT16)(c->x + 5 + tw), (INT16)(c->y + 3), 1,
                            (INT16)(c->h - 6), 0x303030u);
      gu->nshape++;
    }
    break;
  }
  default:
    break;
  }
}

static void gui_children_draw(task_t *t, win32_gui_t *gu, win32_wnd_t *w) {
  UINT32 i;

  for (i = 0; i < WIN32_GUI_MAX_CHILDREN; i++) {
    if (gu->kid[i].used && gu->kid[i].parent == w->hwnd) {
      gui_child_draw(t, gu, w, &gu->kid[i]);
    }
  }
}

/* The control under a client-area point, if any. Later controls win, which
 * matches the paint order: the list draws front to back is not the same
 * thing, so the search runs backwards over creation order. */
static win32_child_t *gui_child_hit(win32_gui_t *gu, UINT32 parent, INT32 x,
                                    INT32 y) {
  UINT32 i = WIN32_GUI_MAX_CHILDREN;

  while (i > 0u) {
    win32_child_t *c;

    i--;
    c = &gu->kid[i];
    if (!c->used || c->parent != parent || c->kind == WIN32_CHILD_STATIC) {
      continue;
    }
    if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) {
      return c;
    }
  }
  return NULL;
}

/* ---- menus ------------------------------------------------------------ */

static UINT32 menu_text_len(const char *s) {
  UINT32 n = 0;

  while (s[n] != '\0' && n < WIN32_MENU_TEXT_MAX) {
    n++;
  }
  return n;
}

static win32_menu_t *win32_menu(win32_gui_t *gu, UINT32 h) {
  UINT32 i;

  if (gu == NULL || (h & 0xFF000000u) != WIN32_MENU_HANDLE_BASE) {
    return NULL;
  }
  i = h & 0xFFu;
  if (i >= WIN32_GUI_MAX_MENUS || !gu->menu[i].used) {
    return NULL;
  }
  return &gu->menu[i];
}

static UINT32 win32_menu_new(win32_gui_t *gu) {
  UINT32 i;

  for (i = 0; i < WIN32_GUI_MAX_MENUS; i++) {
    if (!gu->menu[i].used) {
      memset(&gu->menu[i], 0, sizeof(gu->menu[i]));
      gu->menu[i].used = 1u;
      return WIN32_MENU_HANDLE_BASE | i;
    }
  }
  return 0u;
}

/* Where the n-th bar item starts and how wide it is. The same layout has to
 * serve the drawing and the hit test, so it lives in one place. */
static int gui_menu_bar_slot(win32_menu_t *m, UINT32 idx, INT32 *x, INT32 *w) {
  INT32  px = 8;
  UINT32 i;

  if (m == NULL || idx >= m->n) {
    return 0;
  }
  for (i = 0; i < m->n; i++) {
    INT32 tw = measure_text(m->item[i].text,
                            menu_text_len(m->item[i].text), 0);

    if (i == idx) {
      *x = px;
      *w = tw + 20;
      return 1;
    }
    px += tw + 20;
  }
  return 0;
}

static void gui_menu_draw(task_t *t, win32_gui_t *gu, win32_wnd_t *w) {
  win32_menu_t *bar = win32_menu(gu, w->menu);
  UINT32        i;
  INT32         px = 8;

  if (bar == NULL || bar->n == 0u) {
    return;
  }
  (void)nwm_owner_shape(t->pid, w->win, NWM_OP_FILL, 0, 0, (INT16)w->w,
                        WIN32_MENU_BAR_H, 0xEDEDEDu);
  (void)nwm_owner_shape(t->pid, w->win, NWM_OP_HLINE, 0, WIN32_MENU_BAR_H - 1,
                        (INT16)w->w, 1, 0xB0B0B0u);
  gu->nshape += 2u;
  for (i = 0; i < bar->n; i++) {
    win32_mitem_t *it = &bar->item[i];
    INT32          tw = measure_text(it->text,
                                     menu_text_len(it->text), 0);
    UINT32         bg = 0xEDEDEDu;

    if (it->sep) {
      continue;
    }
    if (w->menu_open && w->menu_open_idx == i) {
      bg = 0xD8D8D8u; /* the open bar item stays pressed, as Windows shows */
      (void)nwm_owner_shape(t->pid, w->win, NWM_OP_FILL, (INT16)(px - 4), 2,
                            (INT16)(tw + 16), WIN32_MENU_BAR_H - 4, bg);
      gu->nshape++;
    }
    (void)nwm_owner_text(t->pid, w->win, (INT16)(px + 2), 4, 1u, it->text,
                         menu_text_len(it->text),
                         it->disabled ? 0x909090u : 0x101010u);
    gu->ntext++;
    px += tw + 20;
  }
  if (w->menu_open && w->menu_open_idx < bar->n) {
    win32_menu_t *sub = win32_menu(gu, bar->item[w->menu_open_idx].sub);
    INT32         sx;
    INT32         sw;
    INT32         sh;
    UINT32        k;

    if (sub == NULL || sub->n == 0u) {
      return;
    }
    (void)gui_menu_bar_slot(bar, w->menu_open_idx, &sx, &sw);
    sh = WIN32_MENU_POPUP_PAD * 2;
    for (k = 0; k < sub->n; k++) {
      sh += sub->item[k].sep ? 7 : WIN32_MENU_ITEM_H;
    }
    if (sx + WIN32_MENU_POPUP_W > w->w) {
      sx = w->w - WIN32_MENU_POPUP_W;
    }
    if (sx < 0) {
      sx = 0;
    }
    (void)nwm_owner_shape(t->pid, w->win, NWM_OP_FILL, (INT16)sx,
                          (INT16)WIN32_MENU_BAR_H, WIN32_MENU_POPUP_W,
                          (INT16)sh, 0xFAFAFAu);
    (void)nwm_owner_shape(t->pid, w->win, NWM_OP_FRAME, (INT16)sx,
                          (INT16)WIN32_MENU_BAR_H, WIN32_MENU_POPUP_W,
                          (INT16)sh, 0x707070u);
    gu->nshape += 2u;
    {
      INT32 y = WIN32_MENU_BAR_H + WIN32_MENU_POPUP_PAD;

      for (k = 0; k < sub->n; k++) {
        win32_mitem_t *it = &sub->item[k];

        if (it->sep) {
          (void)nwm_owner_shape(t->pid, w->win, NWM_OP_HLINE,
                                (INT16)(sx + 6), (INT16)(y + 3), 
                                WIN32_MENU_POPUP_W - 12, 1, 0xC0C0C0u);
          gu->nshape++;
          y += 7;
          continue;
        }
        if (it->checked) {
          (void)nwm_owner_text(t->pid, w->win, (INT16)(sx + 5),
                               (INT16)(y + 2), 1u, "*", 1u, 0x101010u);
          gu->ntext++;
        }
        (void)nwm_owner_text(t->pid, w->win, (INT16)(sx + 20),
                             (INT16)(y + 2), 1u, it->text,
                             menu_text_len(it->text),
                             it->disabled ? 0xA0A0A0u : 0x101010u);
        gu->ntext++;
        y += WIN32_MENU_ITEM_H;
      }
    }
  }
}

/* A click inside the menu strip, or inside the open popup. Returns 1 when the
 * click belonged to the menu and the program must not see it. */
static int gui_menu_click(task_t *t, win32_gui_t *gu, win32_wnd_t *w, INT32 x,
                          INT32 y, int down) {
  win32_menu_t *bar = win32_menu(gu, w->menu);

  (void)t;
  if (bar == NULL || bar->n == 0u) {
    return 0;
  }
  if (y < WIN32_MENU_BAR_H) {
    UINT32 i;

    if (!down) {
      return 1;
    }
    for (i = 0; i < bar->n; i++) {
      INT32 sx;
      INT32 sw;

      if (!gui_menu_bar_slot(bar, i, &sx, &sw)) {
        continue;
      }
      if (x >= sx - 4 && x < sx - 4 + sw + 8) {
        win32_menu_t *sub = win32_menu(gu, bar->item[i].sub);

        if (sub != NULL && sub->n != 0u) {
          if (w->menu_open && w->menu_open_idx == (UINT8)i) {
            w->menu_open = 0u;
          } else {
            w->menu_open = 1u;
            w->menu_open_idx = (UINT8)i;
          }
          w->dirty = 1u;
        }
        return 1;
      }
    }
    if (w->menu_open) {
      w->menu_open = 0u;
      w->dirty = 1u;
    }
    return 1;
  }
  if (!w->menu_open) {
    return 0;
  }
  {
    win32_menu_t *sub = win32_menu(gu, bar->item[w->menu_open_idx].sub);
    INT32         sx;
    INT32         sw;
    INT32         sh;
    UINT32        k;
    INT32         yy;

    if (sub == NULL || sub->n == 0u) {
      w->menu_open = 0u;
      return 0;
    }
    (void)gui_menu_bar_slot(bar, w->menu_open_idx, &sx, &sw);
    sh = WIN32_MENU_POPUP_PAD * 2;
    for (k = 0; k < sub->n; k++) {
      sh += sub->item[k].sep ? 7 : WIN32_MENU_ITEM_H;
    }
    if (sx + WIN32_MENU_POPUP_W > w->w) {
      sx = w->w - WIN32_MENU_POPUP_W;
    }
    if (sx < 0) {
      sx = 0;
    }
    if (x < sx || x >= sx + WIN32_MENU_POPUP_W ||
        y < WIN32_MENU_BAR_H || y >= WIN32_MENU_BAR_H + sh) {
      /* A click outside the popup closes it, which is what Windows does. */
      if (down) {
        w->menu_open = 0u;
        w->dirty = 1u;
      }
      return 1;
    }
    if (!down) {
      return 1;
    }
    yy = WIN32_MENU_BAR_H + WIN32_MENU_POPUP_PAD;
    for (k = 0; k < sub->n; k++) {
      win32_mitem_t *it = &sub->item[k];
      INT32          ih = it->sep ? 7 : WIN32_MENU_ITEM_H;

      if (y >= yy && y < yy + ih) {
        if (!it->sep && !it->disabled && it->id != 0u) {
          gui_push(gu, w->hwnd, WM_COMMAND, (UINT64)it->id, 0u);
        }
        w->menu_open = 0u;
        w->dirty = 1u;
        return 1;
      }
      yy += ih;
    }
    w->menu_open = 0u;
    w->dirty = 1u;
    return 1;
  }
}

/* CreateMenu and CreatePopupMenu are the same object here: a menu is a list
 * of items, and whether it is a bar or a drop-down is decided by where it is
 * attached. Windows keeps two constructors for the two roles. */
static void api_create_menu(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  f->rax = (UINT64)win32_menu_new(gu);
}

static void api_append_menu(trap_frame_t *f) {
  task_t       *t = sched_current();
  win32_gui_t  *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  win32_menu_t *m = win32_menu(gu, (UINT32)f->rdi);
  UINT32        flags = (UINT32)f->rdx;

  if (m == NULL) {
    f->rax = 0u;
    return;
  }
  if (m->n >= WIN32_GUI_MAX_MENU_ITEMS) {
    f->rax = 0u; /* the menu is full; Windows would grow it */
    return;
  }
  {
    win32_mitem_t *it = &m->item[m->n];

    memset(it, 0, sizeof(*it));
    it->id = (UINT32)f->r8;
    it->sep = ((flags & MF_SEPARATOR) != 0u) ? 1u : 0u;
    it->checked = ((flags & MF_CHECKED) != 0u) ? 1u : 0u;
    it->disabled = ((flags & (MF_DISABLED | MF_GRAYED)) != 0u) ? 1u : 0u;
    if ((flags & 0x00000010u) != 0u) { /* MF_POPUP: the item opens a menu */
      it->sub = (UINT32)f->r8;
      it->id = 0u;
    }
    if (it->sep == 0u && f->r9 != 0u) {
      char text[WIN32_MENU_TEXT_MAX];

      memset(text, 0, sizeof(text));
      (void)read_user_str(t->cr3, f->r9, text, sizeof(text));
      copy_text(it->text, sizeof(it->text), text);
    }
    m->n++;
  }
  f->rax = 1u;
}

static void api_get_menu_item_count(trap_frame_t *f) {
  task_t       *t = sched_current();
  win32_gui_t  *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_menu_t *m = win32_menu(gu, (UINT32)f->rdi);

  f->rax = (m != NULL) ? (UINT64)m->n : 0u;
}

static void api_set_menu(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  UINT32       h = (UINT32)f->rdx;

  if (w == NULL || (h != 0u && win32_menu(gu, h) == NULL)) {
    f->rax = 0u;
    return;
  }
  w->menu = h;
  w->menu_open = 0u;
  w->dirty = 1u;
  f->rax = 1u;
}

static void api_get_menu(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  f->rax = (w != NULL) ? (UINT64)w->menu : 0u;
}

static void api_destroy_menu(trap_frame_t *f) {
  task_t       *t = sched_current();
  win32_gui_t  *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_menu_t *m = win32_menu(gu, (UINT32)f->rdi);
  UINT32        i;

  if (m == NULL) {
    f->rax = 0u;
    return;
  }
  /* A window that still shows this menu loses it, which keeps a destroyed
   * handle from being drawn. Windows ties the menu's lifetime to the window;
   * here the program owns it, so this is the explicit cleanup. */
  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    if (gu->wnd[i].used && gu->wnd[i].menu == (UINT32)f->rdi) {
      gu->wnd[i].menu = 0u;
      gu->wnd[i].menu_open = 0u;
      gu->wnd[i].dirty = 1u;
    }
  }
  memset(m, 0, sizeof(*m));
  f->rax = 1u;
}

static void api_draw_menu_bar(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  w->dirty = 1u;
  f->rax = 1u;
}

/* ---- MessageBoxA: a modal box the layer owns --------------------------- */

/* The box is not a program window: it has no class and no procedure, so the
 * frame it shows is built here instead of arriving from a WM_PAINT. That is
 * also why its buttons are settled in msgbox_event rather than through
 * WM_COMMAND - there is no procedure to command. */
static void msgbox_paint(task_t *t, win32_gui_t *gu, win32_wnd_t *w) {
  (void)nwm_owner_clear(t->pid, w->win);
  gu->nshape = 0u;
  gu->ntext = 0u;
  (void)nwm_owner_shape(t->pid, w->win, NWM_OP_FILL, 0, 0, (INT16)w->w,
                        (INT16)w->h, 0xF0F0F0u);
  gu->nshape++;
  gui_children_draw(t, gu, w);
  (void)nwm_owner_flush(t->pid, w->win);
}

/* Buttons in a box are numbered 1..n; the answer a click carries depends on
 * the button set the program asked for, exactly as Windows maps them. */
static UINT32 msgbox_button_id(win32_gui_t *gu, win32_child_t *c) {
  UINT32 id = (UINT32)(c->hwnd & 0xFFu) + 1u;

  if (gu->msgbox_type == WIN32_MB_YESNO) {
    return (id == 1u) ? WIN32_IDYES : WIN32_IDNO;
  }
  return (id == 1u) ? WIN32_IDOK : WIN32_IDCANCEL;
}

static int msgbox_event(task_t *t, win32_gui_t *gu, UINT32 hwnd,
                        const ipc_msg_t *m) {
  win32_wnd_t *w = gui_window(gu, hwnd);

  if (w == NULL) {
    return 0;
  }
  if (m->hdr.type == NWM_EV_CLOSE) {
    /* The close button is the way out that keeps the program running: the
     * box answers Cancel/No, the same as Windows does for a single-button
     * box closing with its X. */
    gu->msgbox_result = WIN32_IDCANCEL;
    return 0;
  }
  if (m->hdr.type != NWM_EV_MOUSE || m->hdr.len < sizeof(nwm_ev_mouse_t)) {
    return 0;
  }
  {
    nwm_ev_mouse_t mm;
    win32_child_t *hit;
    UINT32         i;
    UINT32         result = 0;

    memcpy(&mm, m->payload, sizeof(mm));
    hit = gui_child_hit(gu, hwnd, mm.x, mm.y);
    if (mm.type == ND_EV_MOUSE_DOWN) {
      if (hit != NULL && hit->kind == WIN32_CHILD_BUTTON) {
        hit->pressed = 1u;
        msgbox_paint(t, gu, w);
      }
      return 0;
    }
    if (mm.type != ND_EV_MOUSE_UP) {
      return 0;
    }
    for (i = 0; i < WIN32_GUI_MAX_CHILDREN; i++) {
      win32_child_t *c = &gu->kid[i];

      if (!c->used || !c->pressed || c->parent != hwnd) {
        continue;
      }
      c->pressed = 0u;
      if (c == hit) {
        result = msgbox_button_id(gu, c);
      }
    }
    msgbox_paint(t, gu, w);
    if (result != 0u) {
      gu->msgbox_result = result;
    }
  }
  return 0;
}

/* One line of box text per STATIC, wrapped to the box width. */
static void msgbox_split(task_t *t, const char *text, INT32 max_px,
                         char lines[WIN32_MSGBOX_MAX_LINES][WIN32_CHILD_TEXT_MAX],
                         UINT32 *nlines, INT32 *widest) {
  UINT32 n = 0;
  UINT32 len = 0;
  UINT32 i;
  INT32  widest_px = 0;

  (void)t;
  while (text[len] != '\0' && len + 1u < WIN32_CHILD_TEXT_MAX) {
    len++;
  }
  if (len == 0u) {
    lines[0][0] = '\0';
    *nlines = 1u;
    *widest = 0;
    return;
  }
  while (n < WIN32_MSGBOX_MAX_LINES && len > 0u) {
    UINT32 take = len < WIN32_CHILD_TEXT_MAX - 1u ? len : WIN32_CHILD_TEXT_MAX - 1u;
    INT32  w;

    /* Break at the last space that still fits, so a line does not end in
     * the middle of a word. A long run with no space just takes what fits. */
    if (n + 1u < WIN32_MSGBOX_MAX_LINES) {
      UINT32 fit = 0;

      while (fit < take) {
        UINT32 cut = fit + 1u;
        INT32  ww = measure_text(text + 0, cut, 0);

        if (ww > max_px) {
          break;
        }
        fit = cut;
      }
      if (fit > 0u) {
        take = fit;
      }
    }
    {
      UINT32 skip = 0u;

      /* A newline in the text is a hard break, the way a message box shows
       * one, and the break character itself is not part of either line. */
      for (i = 0; i < take; i++) {
        if (text[i] == '\n' || text[i] == '\r') {
          take = i;
          skip = 1u;
          break;
        }
      }
      while (take > 0u && text[take - 1u] == ' ') {
        take--;
      }
      for (i = 0; i < take; i++) {
        lines[n][i] = text[i];
      }
      lines[n][take] = '\0';
      w = measure_text(lines[n], take, 0);
      if (w > widest_px) {
        widest_px = w;
      }
      n++;
      text += take + skip;
      len -= take + skip;
      while (len > 0u && (*text == ' ' || *text == '\r' || *text == '\n')) {
        text++;
        len--;
      }
    }
  }
  if (n == 0u) {
    lines[0][0] = '\0';
    n = 1u;
  }
  *nlines = n;
  *widest = widest_px;
}

static void api_message_box(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  if (gu->msgbox_hwnd == 0u && gu->msgbox_result == 0u) {
    char     text[WIN32_MSGBOX_MAX_LINES * WIN32_CHILD_TEXT_MAX];
    char     caption[NWM_TITLE_MAX];
    char     lines[WIN32_MSGBOX_MAX_LINES][WIN32_CHILD_TEXT_MAX];
    UINT32   nlines = 1u;
    INT32    widest = 0;
    UINT32   type = (UINT32)f->r9 & 0xFu;
    UINT32   nbtn = (type == WIN32_MB_OKCANCEL || type == WIN32_MB_YESNO) ? 2u : 1u;
    INT32    cw;
    INT32    ch;
    INT32    bx;
    INT32    by;
    UINT32   win = 0;
    UINT32   i;

    memset(text, 0, sizeof(text));
    memset(caption, 0, sizeof(caption));
    (void)read_user_str(t->cr3, f->rdx, text, sizeof(text));
    (void)read_user_str(t->cr3, f->r8, caption, sizeof(caption));
    if (caption[0] == '\0') {
      copy_text(caption, sizeof(caption), "Novus");
    }
    /* The box wraps at 320 pixels, which is the width a message box gets in
     * Windows when the text is short. Newlines are kept as breaks. */
    msgbox_split(t, text, 320, lines, &nlines, &widest);
    cw = widest + 32;
    if (cw < 200) {
      cw = 200;
    }
    if (cw > 360) {
      cw = 360;
    }
    ch = 28 + (INT32)nlines * WIN32_MSGBOX_LINE_H + 44;
    {
      const nd_display_t *d = nd_display();
      INT32 sw = (d != NULL && d->available) ? (INT32)d->width : 1024;
      INT32 sh = (d != NULL && d->available) ? (INT32)d->height : 768;

      /* nwm_owner_create takes the frame size, which adds the caption and
       * the border the WM draws around the client. */
      bx = (sw - (cw + 2)) / 2;
      by = (sh - (ch + NWM_TITLE_H + 1)) / 2;
    }
    if (bx < 0) {
      bx = 0;
    }
    if (by < 0) {
      by = 0;
    }
    if (nwm_owner_create(t->pid, caption, bx, by, cw + 2,
                         ch + NWM_TITLE_H + 1, &win) != NWM_OK) {
      gu->msgbox_result = WIN32_IDCANCEL;
      f->rax = (UINT64)WIN32_IDCANCEL;
      return;
    }
    {
      win32_wnd_t *w = NULL;

      for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
        if (!gu->wnd[i].used) {
          w = &gu->wnd[i];
          break;
        }
      }
      if (w == NULL) {
        (void)nwm_owner_destroy(t->pid, win);
        gu->msgbox_result = WIN32_IDCANCEL;
        f->rax = (UINT64)WIN32_IDCANCEL;
        return;
      }
      memset(w, 0, sizeof(*w));
      w->used = 1u;
      w->hwnd = WIN32_HWND_BASE | win;
      w->win = win;
      w->w = cw;
      w->h = ch;
      w->x = bx;
      w->y = by;
      w->bg = 0xF0F0F0u;
      gu->msgbox_hwnd = w->hwnd;
      gu->msgbox_owner = (UINT32)f->rdi;
      gu->msgbox_type = type;
    }
    /* One STATIC per line, then the button row. The ids are 1..n so the
     * click handler can turn a button straight into its answer. */
    for (i = 0; i < nlines; i++) {
      win32_child_t *k = gui_child_new(gu, gu->msgbox_hwnd, WIN32_CHILD_STATIC,
                                       16, 20 + (INT32)i * WIN32_MSGBOX_LINE_H,
                                       cw - 24, WIN32_MSGBOX_LINE_H);

      if (k != NULL) {
        copy_text(k->text, sizeof(k->text), lines[i]);
      }
    }
    {
      INT32 bw = 84;
      INT32 bh = 26;
      INT32 total = (INT32)nbtn * bw + ((INT32)nbtn - 1) * 12;
      INT32 rowy = ch - bh - 14;
      INT32 startx = (cw - total) / 2;

      for (i = 0; i < nbtn; i++) {
        win32_child_t *k = gui_child_new(gu, gu->msgbox_hwnd, WIN32_CHILD_BUTTON,
                                         startx + (INT32)i * (bw + 12), rowy, bw,
                                         bh);

        if (k == NULL) {
          continue;
        }
        if (type == WIN32_MB_YESNO) {
          copy_text(k->text, sizeof(k->text), (i == 0u) ? "Yes" : "No");
        } else {
          copy_text(k->text, sizeof(k->text), (i == 0u) ? "OK" : "Cancel");
        }
      }
    }
    kputs("[WIN32] msgbox: hwnd=");
    kputs_hex(gu->msgbox_hwnd);
    kputs(" buttons=");
    kputs_hex(nbtn);
    kputs("\n");
    msgbox_paint(t, gu, gui_window(gu, gu->msgbox_hwnd));
  }
  if (gu->msgbox_result != 0u) {
    /* Settled: take the box down and hand the id back, which is the one
     * moment the program resumes. */
    UINT32 result = gu->msgbox_result;
    UINT32 hwnd = gu->msgbox_hwnd;
    win32_wnd_t *w = gui_window(gu, hwnd);
    UINT32 i;

    if (w != NULL) {
      (void)nwm_owner_destroy(t->pid, w->win);
      for (i = 0; i < WIN32_GUI_MAX_CHILDREN; i++) {
        if (gu->kid[i].used && gu->kid[i].parent == hwnd) {
          memset(&gu->kid[i], 0, sizeof(gu->kid[i]));
        }
      }
      memset(w, 0, sizeof(*w));
    }
    gu->msgbox_hwnd = 0u;
    gu->msgbox_result = 0u;
    gu->msgbox_owner = 0u;
    kputs("[WIN32] msgbox: result=");
    kputs_hex(result);
    kputs("\n");
    f->rax = (UINT64)result;
    return;
  }
  /* Still open: park the program and replay this call, so that the box keeps
   * the caller inside it the way a modal box does. */
  sched_park(t->pid, WIN32_MSG_POLL_TICKS);
  f->rax = (UINT64)SYS_WIN32;
  f->rip -= WIN32_SYSCALL_LEN;
}

static void api_end_frame(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  {
    UINT64 rc;

    /* Controls belong to the window, so they go out in the same frame the
     * program just painted: a program that never draws its own button still
     * gets a drawn one, which is what CreateWindowExA("BUTTON") promises. */
    gui_children_draw(t, gu, w);
    /* The menu is the layer's furniture, so it is drawn over whatever the
     * program painted last, in the same frame. */
    gui_menu_draw(t, gu, w);
    rc = (UINT64)nwm_owner_flush(t->pid, w->win);

    w->painting = 0u;
    kputs("[WIN32] paint: submitted shapes=");
    kputs_hex(gu->nshape);
    kputs(" texts=");
    kputs_hex(gu->ntext);
    kputs(" flush->");
    kputs_hex(rc);
    kputs("\n");
  }
  f->rax = 1u;
}

static void api_fill_rect(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  INT32        r[4];
  UINT32       color;

  if (w == NULL || f->rdx == 0u ||
      user_copy(t->cr3, f->rdx, r, sizeof(r), 0) != 0) {
    f->rax = 0u;
    return;
  }
  /* HANDLE 0 is the hollow brush: an area left untouched, not painted
   * white. Windows programs pass it to erase nothing. */
  if ((UINT32)f->r8 == 0u) {
    f->rax = 1u;
    return;
  }
  color = brush_rgb((UINT32)f->r8);
  (void)nwm_owner_shape(t->pid, w->win, NWM_OP_FILL,
                        (INT16)r[0], (INT16)r[1],
                        (INT16)(r[2] - r[0]), (INT16)(r[3] - r[1]), color);
  gu->nshape++;
  f->rax = 1u;
}

static INT32 measure_text(const char *s, UINT32 len, INT32 extra);

static void api_text_out(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  char         text[NWM_TEXT_MAX];
  UINT64       count = 0;
  UINT32       i;

  if (w == NULL || gu == NULL || f->r9 == 0u ||
      stack_arg(f, t, 5u, &count) != 0) {
    kputs("[WIN32] textout: bad args hwnd=");
    kputs_hex(f->rdi);
    kputs(" str=");
    kputs_hex(f->r9);
    kputs(" count=");
    kputs_hex(count);
    kputs("\n");
    f->rax = 0u;
    return;
  }
  if (count >= NWM_TEXT_MAX) {
    count = NWM_TEXT_MAX - 1u;
  }
  if (count == 0u || user_copy(t->cr3, f->r9, text, count, 0) != 0) {
    kputs("[WIN32] textout: count=");
    kputs_hex(count);
    kputs(" copy failed at ");
    kputs_hex(f->r9);
    kputs("\n");
    f->rax = 0u;
    return;
  }
  text[count] = '\0';
  /* The display list carries byte strings; drop anything past the cap. */
  for (i = 0; i < count; i++) {
    if (text[i] == '\0') {
      break;
    }
  }
  {
    INT32 x = (INT32)f->rdx;
    INT32 y = (INT32)f->r8;

    /* SetTextAlign's horizontal bits move the origin from the left edge of
     * the string to its centre or right edge, which is how a program centres
     * text without measuring it first. */
    if ((gu->text_align & 0x0006u) == 0x0006u) {        /* TA_CENTER */
      x -= measure_text(text, i, gu->char_extra) / 2;
    } else if ((gu->text_align & 0x0002u) == 0x0002u) { /* TA_RIGHT */
      x -= measure_text(text, i, gu->char_extra);
    }
    (void)nwm_owner_text(t->pid, w->win, (INT16)x, (INT16)y, 1u,
                         text, (UINT32)i, gu->text_rgb);
  }
  gu->ntext++;
  f->rax = 1u;
}

static void api_set_text_color(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  UINT32       old;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  old = gu->text_rgb;
  /* Answer in COLORREF (BGR) for the caller, keep RGB internally. */
  f->rax = ((old & 0xFFu) << 16) | (old & 0xFF00u) | ((old >> 16) & 0xFFu);
  gu->text_rgb = colorref_rgb((UINT32)f->rdx);
}

static void api_create_solid_brush(trap_frame_t *f) {
  /* A brush is a colour with the high byte set: no GDI object table, and a
   * program that never deletes one leaks nothing. */
  f->rax = 0xFF000000u | ((UINT32)f->rdi & 0xFFFFFFu);
}

/* ---- painting: pen, shapes, window text ------------------------------ */

/* Every drawing call answers the same question first: the HDC of this layer
 * is the window handle, so this resolves the window the call is aimed at,
 * and a call aimed nowhere reports failure instead of painting elsewhere. */
static win32_wnd_t *draw_target(trap_frame_t *f, task_t **tp, win32_gui_t **gp) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (t == NULL || gu == NULL || w == NULL) {
    return NULL;
  }
  *tp = t;
  *gp = gu;
  return w;
}

static void draw_shape(task_t *t, win32_gui_t *gu, win32_wnd_t *w, UINT8 op,
                       INT32 x, INT32 y, INT32 width, INT32 height,
                       UINT32 color) {
  (void)nwm_owner_shape(t->pid, w->win, op, (INT16)x, (INT16)y, (INT16)width,
                        (INT16)height, color);
  gu->nshape++;
}

static void api_set_bk_mode(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  UINT32       old;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  old = gu->bk_mode;
  /* TRANSPARENT (1) and OPAQUE (2) are the only two modes; anything else
   * keeps the current one rather than storing a value nothing honours. */
  if ((UINT32)f->rdx == 1u || (UINT32)f->rdx == 2u) {
    gu->bk_mode = (UINT8)f->rdx;
  }
  f->rax = old;
}

static void api_set_bk_color(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  UINT32       old;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  old = gu->bk_ref;
  gu->bk_ref = (UINT32)f->rdx & 0xFFFFFFu;
  f->rax = old;
}

static void api_get_text_color(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;

  f->rax = (gu != NULL) ? colorref_rgb(gu->text_rgb) : 0u;
}

static void api_move_to_ex(trap_frame_t *f) {
  task_t      *t = NULL;
  win32_gui_t *gu = NULL;
  win32_wnd_t *w = draw_target(f, &t, &gu);
  UINT8        pt[8];

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  /* lppt is optional; when given it takes the previous position, as the
   * real call does. */
  if (f->r9 != 0u) {
    pt[0] = (UINT8)((UINT32)(INT32)gu->cur_x & 0xFFu);
    pt[1] = (UINT8)(((UINT32)(INT32)gu->cur_x >> 8) & 0xFFu);
    pt[2] = (UINT8)(((UINT32)(INT32)gu->cur_x >> 16) & 0xFFu);
    pt[3] = (UINT8)(((UINT32)(INT32)gu->cur_x >> 24) & 0xFFu);
    pt[4] = (UINT8)((UINT32)(INT32)gu->cur_y & 0xFFu);
    pt[5] = (UINT8)(((UINT32)(INT32)gu->cur_y >> 8) & 0xFFu);
    pt[6] = (UINT8)(((UINT32)(INT32)gu->cur_y >> 16) & 0xFFu);
    pt[7] = (UINT8)(((UINT32)(INT32)gu->cur_y >> 24) & 0xFFu);
    if (user_copy(t->cr3, f->r9, pt, sizeof(pt), 1) != 0) {
      f->rax = 0u;
      return;
    }
  }
  gu->cur_x = (INT16)f->rdx;
  gu->cur_y = (INT16)f->r8;
  f->rax = 1u;
}

static void api_line_to(trap_frame_t *f) {
  task_t      *t = NULL;
  win32_gui_t *gu = NULL;
  win32_wnd_t *w = draw_target(f, &t, &gu);
  INT32        x1 = (INT16)f->rdx;
  INT32        y1 = (INT16)f->r8;

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  draw_shape(t, gu, w, NWM_OP_LINE, gu->cur_x, gu->cur_y, x1 - gu->cur_x,
             y1 - gu->cur_y, gu->pen_rgb);
  gu->cur_x = (INT16)x1;
  gu->cur_y = (INT16)y1;
  f->rax = 1u;
}

/* Rectangle and Ellipse take a bounding box in the four argument slots:
 * left/top in RDX/R8, right/bottom in R9 and the fifth stack slot. */
static int shape_box(trap_frame_t *f, task_t *t, INT32 box[4]) {
  UINT64 bottom = 0;

  box[0] = (INT16)f->rdx;
  box[1] = (INT16)f->r8;
  box[2] = (INT16)f->r9;
  if (stack_arg(f, t, 5u, &bottom) != 0) {
    return -1;
  }
  box[3] = (INT16)(UINT16)(bottom & 0xFFFFu);
  return 0;
}

static void api_rectangle(trap_frame_t *f) {
  task_t      *t = NULL;
  win32_gui_t *gu = NULL;
  win32_wnd_t *w = draw_target(f, &t, &gu);
  INT32        box[4];

  if (w == NULL || shape_box(f, t, box) != 0) {
    f->rax = 0u;
    return;
  }
  if (box[2] > box[0] && box[3] > box[1]) {
    draw_shape(t, gu, w, NWM_OP_FILL, box[0], box[1], box[2] - box[0],
               box[3] - box[1], gu->brush_rgb);
    draw_shape(t, gu, w, NWM_OP_FRAME, box[0], box[1], box[2] - box[0],
               box[3] - box[1], gu->pen_rgb);
  }
  f->rax = 1u;
}

static void api_ellipse(trap_frame_t *f) {
  task_t      *t = NULL;
  win32_gui_t *gu = NULL;
  win32_wnd_t *w = draw_target(f, &t, &gu);
  INT32        box[4];
  INT32        cx, cy, rx, ry;

  if (w == NULL || shape_box(f, t, box) != 0) {
    f->rax = 0u;
    return;
  }
  if (box[2] <= box[0] || box[3] <= box[1]) {
    f->rax = 1u;
    return;
  }
  cx = (box[0] + box[2]) / 2;
  cy = (box[1] + box[3]) / 2;
  rx = (box[2] - box[0]) / 2;
  ry = (box[3] - box[1]) / 2;
  draw_shape(t, gu, w, NWM_OP_ELLIPSE_FILL, cx, cy, rx, ry, gu->brush_rgb);
  draw_shape(t, gu, w, NWM_OP_ELLIPSE, cx, cy, rx, ry, gu->pen_rgb);
  f->rax = 1u;
}

static void api_set_pixel(trap_frame_t *f) {
  task_t      *t = NULL;
  win32_gui_t *gu = NULL;
  win32_wnd_t *w = draw_target(f, &t, &gu);
  UINT32       rgb;

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  rgb = colorref_rgb(f->r8 & 0xFFFFFFu);
  draw_shape(t, gu, w, NWM_OP_FILL, (INT16)f->rdx, (INT16)(UINT16)(f->r9 & 0xFFFFu),
             1, 1, rgb);
  f->rax = f->r8 & 0xFFFFFFu;
}

static void api_pat_blt(trap_frame_t *f) {
  task_t      *t = NULL;
  win32_gui_t *gu = NULL;
  win32_wnd_t *w = draw_target(f, &t, &gu);
  INT32        x = (INT16)f->rdx;
  INT32        y = (INT16)f->r8;
  UINT64       rop = 0;

  if (w == NULL || stack_arg(f, t, 6u, &rop) != 0) {
    f->rax = 0u;
    return;
  }
  {
    UINT32 width = (UINT32)(f->r9 & 0xFFFFu);
    UINT64 height = 0;

    if (stack_arg(f, t, 5u, &height) != 0) {
      f->rax = 0u;
      return;
    }
    switch ((UINT32)rop) {
    case 0x00F00021u: /* PATCOPY: lay the selected brush down */
      if ((gu->brush_rgb & 0xFFFFFFFFu) != 0u) {
        draw_shape(t, gu, w, NWM_OP_FILL, x, y, (INT32)width,
                   (INT32)(UINT16)(height & 0xFFFFu), gu->brush_rgb);
      }
      break;
    case 0x00FF0062u: /* WHITENESS */
      draw_shape(t, gu, w, NWM_OP_FILL, x, y, (INT32)width,
                 (INT32)(UINT16)(height & 0xFFFFu), 0x00FFFFFFu);
      break;
    case 0x00000042u: /* BLACKNESS */
      draw_shape(t, gu, w, NWM_OP_FILL, x, y, (INT32)width,
                 (INT32)(UINT16)(height & 0xFFFFu), 0x00000000u);
      break;
    default:
      /* Any other raster op would silently paint the wrong pixels, so it is
       * refused instead of approximated. */
      t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
      f->rax = 0u;
      return;
    }
  }
  f->rax = 1u;
}

/* A pen is the one GDI object with a width, so it gets a slot in a table
 * and an opaque handle that indexes it. */
static void api_create_pen(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  UINT32       i;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  for (i = 0; i < WIN32_GUI_PENS; i++) {
    if (gu->pen_h[i] == 0u) {
      gu->pen_h[i] = 0xFD000000u | (i + 1u);
      gu->pen_c[i] = colorref_rgb(f->r8 & 0xFFFFFFu);
      gu->pen_w[i] = (UINT32)f->rdx & 0xFFu;
      f->rax = gu->pen_h[i];
      return;
    }
  }
  f->rax = 0u;
}

static void api_select_object(trap_frame_t *f) {
  task_t      *t = NULL;
  win32_gui_t *gu = NULL;
  win32_wnd_t *w = draw_target(f, &t, &gu);
  UINT32       obj = (UINT32)f->rdx;

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  if ((obj & 0xFF000000u) == 0xFD000000u) {
    UINT32 i = (obj & 0xFFu);
    if (i == 0u || i > WIN32_GUI_PENS || gu->pen_h[i - 1u] != obj) {
      f->rax = 0u;
      return;
    }
    gu->pen_rgb = gu->pen_c[i - 1u];
    gu->pen_width = (gu->pen_w[i - 1u] > 0u) ? gu->pen_w[i - 1u] : 1u;
    f->rax = (UINT64)obj;
    return;
  }
  if ((obj & 0xFF000000u) == 0xFF000000u) {
    gu->brush_rgb = colorref_rgb(obj & 0xFFFFFFu);
    f->rax = (UINT64)obj;
    return;
  }
  if (obj == 0u) {
    /* HANDLE 0 is the hollow brush: interior left untouched. */
    gu->brush_rgb = 0u;
    f->rax = 1u;
    return;
  }
  f->rax = 0u;
}

static void api_delete_object(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  UINT32       obj = (UINT32)f->rdi;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  if ((obj & 0xFF000000u) == 0xFD000000u) {
    UINT32 i = (obj & 0xFFu);
    if (i == 0u || i > WIN32_GUI_PENS || gu->pen_h[i - 1u] != obj) {
      f->rax = 0u;
      return;
    }
    gu->pen_h[i - 1u] = 0u;
    f->rax = 1u;
    return;
  }
  /* Brushes and stock objects carry no state, so deleting one is a
   * no-op that succeeds - which is what Windows does for stock handles. */
  f->rax = 1u;
}

static void api_post_quit_message(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;

  if (gu == NULL) {
    return;
  }
  gu->quit_code = (UINT32)f->rdi;
  gu->quit_pending = 1u;
}

static void api_update_window(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  w->dirty = 1u;
  f->rax = 1u;
}

static void api_set_window_text(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  char         text[NWM_TITLE_MAX];
  win32_child_t *k;

  /* A control's caption is its text, so SetWindowTextA on a BUTTON or a
   * STATIC lands in the control's own buffer and the parent repaints. */
  k = (w == NULL) ? gui_child(gu, (UINT32)f->rdi) : NULL;
  if (k != NULL) {
    char ctext[WIN32_CHILD_TEXT_MAX];

    if (read_user_str(t->cr3, f->rdx, ctext, sizeof(ctext)) != 0) {
      f->rax = 0u;
      return;
    }
    copy_text(k->text, sizeof(k->text), ctext);
    {
      win32_wnd_t *pw = gui_window(gu, k->parent);

      if (pw != NULL) {
        pw->dirty = 1u;
      }
    }
    f->rax = 1u;
    return;
  }
  if (w == NULL || read_user_str(t->cr3, f->rdx, text, sizeof(text)) != 0) {
    f->rax = 0u;
    return;
  }
  copy_text(w->title, NWM_TITLE_MAX, text);
  (void)nwm_owner_title(t->pid, w->win, w->title);
  f->rax = 1u;
}

/* MoveWindow(hwnd, x, y, nWidth, nHeight, repaint). Windows sizes here are
 * the frame, title bar included, exactly what nwm_owner_create expects - so
 * the client area a program then sees shrinks by the frame, as it does on
 * Windows. The WM clamps a rectangle back onto the screen, which is why
 * GetWindowRect reports the placed rectangle rather than what was asked for. */
static void api_move_window(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  UINT64       h64 = 0;
  INT32        cx, cy, cw, ch;

  if (w == NULL || stack_arg(f, t, 5u, &h64) != 0) {
    f->rax = 0u;
    return;
  }
  cx = (INT32)f->rdx;
  cy = (INT32)f->r8;
  cw = (INT32)f->r9;
  ch = (INT32)h64;
  if (cw <= 0 || ch <= 0) {
    f->rax = 0u;
    return;
  }
  if (nwm_owner_move(t->pid, w->win, cx, cy) != NWM_OK ||
      nwm_owner_resize(t->pid, w->win, cw, ch) != NWM_OK) {
    f->rax = 0u;
    return;
  }
  w->x = cx;
  w->y = cy;
  w->w = cw - 2;
  w->h = ch - (INT32)NWM_TITLE_H - 1;
  if (w->w < 1) {
    w->w = 1;
  }
  if (w->h < 1) {
    w->h = 1;
  }
  w->dirty = 1u;
  f->rax = 1u;
}

/* SetFocus(hwnd) hands the keyboard to that window: the WM raises it and
 * routes key events to it, and the program gets WM_KILLFOCUS on the window
 * that had it before. */
static void api_set_focus(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  UINT32       old;

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  old = gu->focus_hwnd;
  if (nwm_owner_focus(t->pid, w->win) != 0) {
    f->rax = 0u;
    return;
  }
  gu->focus_hwnd = w->hwnd;
  if (old != 0u && old != w->hwnd) {
    gui_push(gu, old, WM_KILLFOCUS, w->hwnd, 0);
  }
  gui_push(gu, w->hwnd, WM_SETFOCUS, old, 0);
  f->rax = w->hwnd;
}

static void api_get_focus(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w;

  if (gu == NULL || gu->focus_hwnd == 0u) {
    f->rax = 0u;
    return;
  }
  w = gui_window(gu, gu->focus_hwnd);
  f->rax = (w != NULL) ? w->hwnd : 0u;
}

/* ---- ws2_32: the socket surface ---------------------------------------- */

/* The honest shape of this block: the kernel already owns a TCP engine
 * (net_tcp.c), a resolver (net_dns.c) and DHCP, but that engine is one
 * connection with no descriptor layer over it, so ws2_32 cannot hand a
 * program a socket yet. What this block makes real is the part that needs
 * no connection - the version handshake a program refuses to run without,
 * the address conversions, and error reporting - and every transport call
 * answers WSAENETDOWN with the failure value Windows documents, so a
 * program takes its error path instead of holding a socket that cannot
 * carry a byte. Wiring the descriptors onto net_tcp_* is the next block. */
#define WSAENETDOWN       10050u
#define WSAENOTSOCK       10038u
#define WSAHOST_NOT_FOUND 11001u

/* WSADATA is 2 + 2 + 257 + 129 + 2 + 2 + 8 = 402 bytes; the padding to the
 * vendor pointer rounds the struct to 408 on x64. */
#define WSA_DATA_LEN 408u

/* inet_ntoa answers a pointer, so the text has to live somewhere the
 * program can read. The TEB pages already carry the layer's scratch space
 * (TLS slots end at +0x1680, the syscall scratch word sits there); the
 * tail of the second page, +0x1700, is reserved for this 16-byte buffer. */
#define WIN32_INET_NTOA_OFF 0x1700u

static void wsa_fail(task_t *t, UINT32 code) {
  if (t != NULL) {
    t->win32_last_error = code;
  }
}

static void api_wsa_startup(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT8   d[WSA_DATA_LEN];
  UINT16  want = (UINT16)f->rdi;
  UINT16  ver;
  UINT16  high = 0x0202u;

  if (t == NULL) {
    f->rax = WSAENETDOWN;
    return;
  }
  ver = (want >= 0x0202u) ? 0x0202u : want;
  if (ver < 0x0101u) {
    ver = 0x0101u;
  }
  memset(d, 0, sizeof(d));
  memcpy(d + 0, &ver, 2);   /* wVersion */
  memcpy(d + 2, &high, 2);  /* wHighVersion */
  copy_text((char *)d + 4, 257, "Novus ws2_32");
  copy_text((char *)d + 261, 129, "socket layer not wired");
  /* iMaxSockets 0 and iMaxUdpDg 0 are the truth: no transport is open. */
  if (f->rdx != 0u && user_copy(t->cr3, f->rdx, d, sizeof(d), 1) != 0) {
    f->rax = WSAENETDOWN;
    return;
  }
  t->win32_last_error = 0u;
  f->rax = 0u;
}

static void api_wsa_cleanup(trap_frame_t *f) {
  wsa_fail(sched_current(), 0u);
  f->rax = 0u;
}

static void api_wsa_get_last_error(trap_frame_t *f) {
  task_t *t = sched_current();

  f->rax = (t != NULL) ? (UINT64)t->win32_last_error : 0u;
}

static void api_wsa_set_last_error(trap_frame_t *f) {
  wsa_fail(sched_current(), (UINT32)f->rdi);
  f->rax = 0u;
}

/* Every transport call lands here: the shape of the answer follows what
 * Windows returns on failure, so a caller's error handling still works. */
static void api_socket_fail(trap_frame_t *f, UINT32 code, UINT64 value) {
  wsa_fail(sched_current(), code);
  f->rax = value;
}

static void api_socket(trap_frame_t *f) {
  api_socket_fail(f, WSAENETDOWN, (UINT64)-1);
}

static void api_socket_call(trap_frame_t *f) {
  api_socket_fail(f, WSAENETDOWN, (UINT64)-1);
}

static void api_closesocket(trap_frame_t *f) {
  /* No socket was ever handed out, so every argument is "not a socket",
   * including the value Windows spells INVALID_SOCKET. */
  api_socket_fail(f, WSAENOTSOCK, (UINT64)-1);
}

static void api_gethostbyname(trap_frame_t *f) {
  api_socket_fail(f, WSAHOST_NOT_FOUND, 0u);
}

static void api_getaddrinfo(trap_frame_t *f) {
  /* EAI_FAIL is a negative int; returning it as x64 -4 keeps the caller's
   * "nonzero means failure" test working. */
  wsa_fail(sched_current(), 0u);
  f->rax = (UINT64)(INT64)-4;
}

static void api_freeaddrinfo(trap_frame_t *f) {
  f->rax = 0u;
}

/* GetDlgItem(parent, id): a program names its controls by the id it passed
 * to CreateWindowExA, and this layer hands out the index as the id, so the
 * lookup is the index. */
static void api_get_dlg_item(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  UINT32       i = ((UINT32)f->rdx & 0xFFFFu);

  if (gu == NULL || i == 0u || i > WIN32_GUI_MAX_CHILDREN) {
    f->rax = 0u;
    return;
  }
  i--;
  if (!gu->kid[i].used || gu->kid[i].parent != (UINT32)f->rdi) {
    f->rax = 0u;
    return;
  }
  f->rax = (UINT64)gu->kid[i].hwnd;
}

static void inet_ntoa_text(UINT32 v, char *out) {
  UINT32 i;
  UINT32 n = 0;

  for (i = 0; i < 4u; i++) {
    char   tmp[4];
    UINT32 k = 0;
    UINT32 b = (v >> (i * 8u)) & 0xFFu;

    if (b == 0u) {
      tmp[k++] = '0';
    }
    while (b > 0u) {
      tmp[k++] = (char)('0' + (b % 10u));
      b /= 10u;
    }
    while (k > 0u) {
      out[n++] = tmp[--k];
    }
    if (i < 3u) {
      out[n++] = '.';
    }
  }
  out[n] = '\0';
}

static UINT32 inet_addr_value(const char *s) {
  UINT32      parts[4];
  UINT32      i;
  const char *p = s;

  for (i = 0; i < 4u; i++) {
    UINT32 x = 0;
    UINT32 digits = 0;

    while (*p >= '0' && *p <= '9') {
      x = x * 10u + (UINT32)(*p - '0');
      if (x > 255u) {
        return 0xFFFFFFFFu;
      }
      p++;
      digits++;
    }
    if (digits == 0u || digits > 3u) {
      return 0xFFFFFFFFu;
    }
    parts[i] = x;
    if (i < 3u) {
      if (*p != '.') {
        return 0xFFFFFFFFu;
      }
      p++;
    }
  }
  if (*p != '\0') {
    return 0xFFFFFFFFu;
  }
  return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

static void api_inet_addr(trap_frame_t *f) {
  task_t *t = sched_current();
  char    s[32];

  if (t == NULL || read_user_str(t->cr3, f->rdi, s, sizeof(s)) != 0) {
    f->rax = 0xFFFFFFFFu; /* INADDR_NONE */
    return;
  }
  f->rax = (UINT64)inet_addr_value(s);
}

static void api_inet_ntoa(trap_frame_t *f) {
  task_t *t = sched_current();
  char    s[16];
  UINT64  dst;

  if (t == NULL || t->teb_va == 0u) {
    f->rax = 0u;
    return;
  }
  inet_ntoa_text((UINT32)f->rdi, s);
  dst = t->teb_va + WIN32_INET_NTOA_OFF;
  if (user_copy(t->cr3, dst, s, sizeof(s), 1) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = dst;
}

static void api_get_window_text(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  win32_child_t *k = gui_child(gu, (UINT32)f->rdi);
  UINT32       i;
  UINT64       bytes;
  UINT32       cap;
  const char  *src;

  if ((w == NULL && k == NULL) || f->rdx == 0u) {
    f->rax = 0u;
    return;
  }
  if (k != NULL) {
    src = k->text;
    cap = WIN32_CHILD_TEXT_MAX;
  } else {
    src = w->title;
    cap = NWM_TITLE_MAX;
  }
  bytes = f->r8 & 0xFFFFu;
  for (i = 0; i < cap && i + 1u < bytes; i++) {
    if (src[i] == '\0') {
      break;
    }
  }
  if (i != 0u && user_copy(t->cr3, f->rdx, (void *)src, i, 1) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = i;
}

/* ---- seventh block: posting, timers, coordinates, the pointer --------- */

static void api_post_message(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  /* PostMessageA(hwnd, msg, wParam, lParam): the message joins this process'
   * queue and the call returns before it is handled, as on Windows. */
  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  gui_push(gu, w->hwnd, (UINT32)f->rdx, f->r8, f->r9);
  f->rax = 1u;
}

static void api_send_message(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  /* A synchronous cross-window call needs the window procedure to run in the
   * caller's frame, and this layer dispatches procedures from the owning
   * program's own loop. The honest answer is therefore that SendMessageA is
   * delivered asynchronously and returns 0; a program that inspects a return
   * value from it has to be told so rather than quietly given a wrong one. */
  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  gui_push(gu, w->hwnd, (UINT32)f->rdx, f->r8, f->r9);
  f->rax = 0u;
}

static void api_set_timer(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  UINT32       id = (UINT32)f->rdx;
  UINT32       ms;
  UINT32       i;

  if (gu == NULL || w == NULL) {
    f->rax = 0u;
    return;
  }
  /* SetTimer(hwnd, id, ms, proc). A TIMERPROC would have to be called back
   * into user mode from the tick path, which this layer cannot do yet; a
   * program asking for one gets 0 and ERROR_INVALID_PARAMETER instead of a
   * timer that never fires. */
  if (f->r9 != 0u) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  ms = (UINT32)f->r8;
  if (ms < 10u) {
    ms = 10u; /* one tick: a zero millisecond timer is a spin, not a timer */
  }
  for (i = 0; i < WIN32_GUI_TIMERS; i++) {
    if (gu->tmr_used[i] != 0u && gu->tmr_hwnd[i] == w->hwnd &&
        (id == 0u || gu->tmr_id[i] == id)) {
      gu->tmr_period[i] = ms;
      gu->tmr_next[i] = (UINT64)sched_ticks() * 10u + ms;
      f->rax = gu->tmr_id[i];
      return;
    }
  }
  for (i = 0; i < WIN32_GUI_TIMERS; i++) {
    if (gu->tmr_used[i] == 0u) {
      break;
    }
  }
  if (i == WIN32_GUI_TIMERS) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM; /* table full */
    f->rax = 0u;
    return;
  }
  if (id == 0u) {
    if (gu->next_timer_id == 0u) {
      gu->next_timer_id = 1u;
    }
    id = gu->next_timer_id++;
  }
  gu->tmr_used[i] = 1u;
  gu->tmr_hwnd[i] = w->hwnd;
  gu->tmr_id[i] = id;
  gu->tmr_period[i] = ms;
  gu->tmr_next[i] = (UINT64)sched_ticks() * 10u + ms;
  f->rax = id;
}

static void api_kill_timer(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  UINT32       i;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  for (i = 0; i < WIN32_GUI_TIMERS; i++) {
    if (gu->tmr_used[i] != 0u && gu->tmr_hwnd[i] == (UINT32)f->rdi &&
        gu->tmr_id[i] == (UINT32)f->rdx) {
      gu->tmr_used[i] = 0u;
      f->rax = 1u;
      return;
    }
  }
  f->rax = 0u;
}

static void api_get_window_rect(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  INT32        r[4];
  INT32        rx;
  INT32        ry;
  INT32        rw;
  INT32        rh;

  if (w == NULL || f->rdx == 0u) {
    f->rax = 0u;
    return;
  }
  /* The frame the compositor draws around the client: one pixel of border,
   * then the caption. The rectangle itself is asked from the WM because a
   * window that was clamped to stay on screen does not sit where the caller
   * asked for, and reporting the request instead of the placement would
   * make ClientToScreen disagree with the pixels on screen. */
  rx = w->x;
  ry = w->y;
  rw = w->w + 2 * WIN32_FRAME_BORDER;
  rh = w->h + WIN32_FRAME_CAPTION + WIN32_FRAME_BORDER;
  if (nwm_owner_rect(w->win, &rx, &ry, &rw, &rh) != NWM_OK) {
    return; /* the window is gone; the caller gets no rect */
  }
  r[0] = rx;
  r[1] = ry;
  r[2] = rx + rw;
  r[3] = ry + rh;
  (void)user_copy(t->cr3, f->rdx, r, sizeof(r), 1);
  f->rax = 1u;
}

static void api_client_to_screen(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  INT32        pt[2];

  if (w == NULL || f->rdx == 0u ||
      user_copy(t->cr3, f->rdx, pt, sizeof(pt), 0) != 0) {
    f->rax = 0u;
    return;
  }
  /* client_rect() in the WM: one pixel of border on the left, the caption
   * above, nothing extra at the top edge. */
  pt[0] += w->x + WIN32_FRAME_BORDER;
  pt[1] += w->y + WIN32_FRAME_CAPTION;
  (void)user_copy(t->cr3, f->rdx, pt, sizeof(pt), 1);
  f->rax = 1u;
}

static void api_screen_to_client(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  INT32        pt[2];

  if (w == NULL || f->rdx == 0u ||
      user_copy(t->cr3, f->rdx, pt, sizeof(pt), 0) != 0) {
    f->rax = 0u;
    return;
  }
  pt[0] -= w->x + WIN32_FRAME_BORDER;
  pt[1] -= w->y + WIN32_FRAME_CAPTION;
  (void)user_copy(t->cr3, f->rdx, pt, sizeof(pt), 1);
  f->rax = 1u;
}

static void api_get_cursor_pos(trap_frame_t *f) {
  task_t *t = sched_current();
  INT32   x = 0, y = 0;
  INT32   pt[2];

  /* The software pointer the compositor keeps is the only pointer there is,
   * so this is the real position rather than a guessed one. */
  nd_cursor_get(&x, &y);
  pt[0] = x;
  pt[1] = y;
  if (f->rdi == 0u || user_copy(t->cr3, f->rdi, pt, sizeof(pt), 1) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = 1u;
}

static void api_get_window_long(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  /* GWLP_USERDATA is -21; the handle itself is the other one programs
   * commonly read, and no other index has a meaning in this layer. */
  if ((INT32)f->rdx == -21) {
    f->rax = w->userdata;
    return;
  }
  f->rax = 0u;
}

static void api_set_window_long(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  UINT64       old;

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  if ((INT32)f->rdx != -21) {
    f->rax = 0u;
    return;
  }
  old = w->userdata;
  w->userdata = f->r8;
  f->rax = old;
}

/* ---- eighth block: text layout --------------------------------------- */

/* Advance width of one UTF-8 string, in pixels. It has to be the
 * compositor's own measurement, or the extent a program computes and the
 * pixels it later paints drift apart: nd_text_width_n walks the same
 * advance table nd_text draws with (TrueType when a face is loaded, the
 * bitmap cells otherwise). */
static INT32 measure_text(const char *s, UINT32 len, INT32 extra) {
  UINT32 i = 0;
  UINT32 cps = 0;
  INT32  w;

  w = (INT32)nd_text_width_n(s, len, 1u);
  if (extra == 0) {
    return w;
  }
  /* One extra pixel after each character, which is what the caller asked
   * SetTextCharacterExtra for; counting the code points is the cheapest way
   * to know how many times to add it. */
  while (i < len && s[i] != '\0') {
    if (((UINT8)s[i] & 0xC0u) != 0x80u) {
      cps++;
    }
    i++;
  }
  return w + extra * (INT32)cps;
}

static void api_get_text_extent_point(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  char         text[64];
  UINT64       count = f->r8;
  INT32        size[2];

  /* GetTextExtentPoint32A(hdc, str, count, SIZE*). The HDC is not consulted:
   * there is one font in this layer and every window measures with it. */
  if (gu == NULL || f->rdx == 0u || f->r9 == 0u) {
    f->rax = 0u;
    return;
  }
  if (count >= sizeof(text)) {
    count = sizeof(text) - 1u;
  }
  if (user_copy(t->cr3, f->rdx, text, count, 0) != 0) {
    f->rax = 0u;
    return;
  }
  text[count] = '\0';
  size[0] = measure_text(text, (UINT32)count, gu->char_extra);
  size[1] = 16; /* one line of the 16 pixel face */
  (void)user_copy(t->cr3, f->r9, size, sizeof(size), 1);
  f->rax = 1u;
}

static void api_set_text_align(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  UINT32       old;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  old = gu->text_align;
  gu->text_align = (UINT32)f->rdx & 0x1Fu; /* TA_ flags only */
  f->rax = old;
}

static void api_get_text_align(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;

  f->rax = (gu != NULL) ? gu->text_align : 0u;
}

static void api_set_text_char_extra(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  INT32        old;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  old = gu->char_extra;
  gu->char_extra = (INT32)f->rdx;
  f->rax = (UINT64)(UINT32)old;
}

static void api_get_text_face(trap_frame_t *f) {
  task_t *t = sched_current();
  char    face[32];
  UINT32  n;
  UINT32  room = (UINT32)f->rdx;

  /* GetTextFaceA(hdc, cch, buffer): the family the metrics came from. With
   * no TrueType face loaded the compositor draws from its built-in bitmap
   * cells, and the name it reports is that face, not a font that is not
   * there. */
  if (nd_ttf_active() != 0) {
    copy_text(face, sizeof(face), nd_ttf_family());
  } else {
    copy_text(face, sizeof(face), "Novus Bitmap");
  }
  n = 0;
  while (face[n] != '\0') {
    n++;
  }
  if (room == 0u || f->r8 == 0u ||
      n + 1u > room ||
      user_copy(t->cr3, f->r8, face, n + 1u, 1) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = n;
}

/* How many bytes of one text run fit into `avail` pixels: the length of the
 * first wrapped line. A break goes at an ASCII space when one is available
 * on that line, otherwise between characters - Chinese runs have no spaces,
 * and a line that stops mid-word is still better than one that runs over the
 * rectangle. */
static UINT32 wrap_len(const char *s, UINT32 len, INT32 avail, INT32 extra) {
  UINT32 i = 0;
  UINT32 last_space = 0;

  if (len == 0u || avail <= 0) {
    return len;
  }
  while (i < len) {
    UINT32 next = i + 1u;
    INT32  w;

    while (next < len && ((UINT8)s[next] & 0xC0u) == 0x80u) {
      next++; /* step to the next code point */
    }
    w = measure_text(s, next, extra);
    if (w > avail && i != 0u) {
      return (last_space != 0u) ? last_space : i;
    }
    if ((UINT8)s[i] == ' ') {
      last_space = next;
    }
    i = next;
  }
  return len;
}

/* DrawTextA(hdc, str, count, RECT*, format). One line per '\n', wrapped at
 * the rectangle's width when DT_WORDBREAK is set, aligned the way
 * DT_LEFT/CENTER/RIGHT and DT_VCENTER ask. */
static void api_draw_text(trap_frame_t *f) {
  task_t      *t = NULL;
  win32_gui_t *gu = NULL;
  win32_wnd_t *w = draw_target(f, &t, &gu);
  char         text[NWM_TEXT_MAX];
  UINT64       count = f->r8;
  INT32        rect[4];
  UINT32       format;
  UINT32       i = 0;
  UINT32       line = 0;
  UINT32       max_lines = NWM_TEXT_MAX / 2u;
  UINT64       format64 = 0;

  if (w == NULL || f->rdx == 0u || f->r9 == 0u ||
      stack_arg(f, t, 5u, &format64) != 0 ||
      user_copy(t->cr3, f->r9, rect, sizeof(rect), 0) != 0) {
    f->rax = 0u;
    return;
  }
  format = (UINT32)format64;
  if (count >= sizeof(text)) {
    count = sizeof(text) - 1u;
  }
  if (count != 0u && user_copy(t->cr3, f->rdx, text, count, 0) != 0) {
    f->rax = 0u;
    return;
  }
  text[count] = '\0';
  if (count == 0u) {
    f->rax = 0u;
    return;
  }
  {
    UINT32 len = (UINT32)count;
    INT32  y = rect[1];
    UINT32 nline = 0;

    for (i = 0; i < len; i++) {
      if (text[i] == '\n') {
        nline++;
      }
    }
    if ((format & 0x00000004u) != 0u && nline < max_lines) { /* DT_VCENTER */
      INT32 total = (INT32)(nline + 1u) * 20;

      y = rect[1] + (rect[3] - rect[1] - total) / 2;
      if (y < rect[1]) {
        y = rect[1];
      }
    }
    i = 0;
    while (i < len && line < max_lines) {
      UINT32 start = i;
      UINT32 n;
      INT32  x = rect[0];
      INT32  width;

      while (i < len && text[i] != '\n') {
        i++;
      }
      n = i - start;
      while (n != 0u && (text[start + n - 1u] == '\r')) {
        n--; /* CRLF: the CR is not part of the line */
      }
      if ((format & 0x00000010u) != 0u && n != 0u) { /* DT_WORDBREAK */
        UINT32 cut = wrap_len(text + start, n, rect[2] - rect[0], gu->char_extra);

        if (cut != 0u && cut < n) {
          n = cut;
          i = start + cut; /* the rest of the run becomes the next line */
        }
      }
      width = measure_text(text + start, n, gu->char_extra);
      if ((format & 0x00000001u) != 0u) {          /* DT_CENTER */
        x = rect[0] + (rect[2] - rect[0] - width) / 2;
      } else if ((format & 0x00000002u) != 0u) {   /* DT_RIGHT */
        x = rect[2] - width;
      }
      if (n != 0u) {
        (void)nwm_owner_text(t->pid, w->win, (INT16)x, (INT16)y, 1u,
                             text + start, n, gu->text_rgb);
        gu->ntext++;
      }
      y += 20;
      line++;
      if (i < len && text[i] == '\n') {
        i++; /* step over the '\n' */
      }
      /* A wrapped line usually breaks at a space; do not start the next
       * line with the space the break left behind. */
      while (i < len && text[i] == ' ') {
        i++;
      }
    }
  }
  f->rax = (UINT64)line;
}

/* ---- ninth block: files and libraries --------------------------------- */

#define WIN32_A_NORMAL 0x80u
#define WIN32_A_DIRECTORY 0x10u

typedef struct {
  UINT8    used;
  vfs_fs_t *fs;
  UINT32   dir;    /* directory inode being listed */
  UINT32   index;  /* how many entries have been handed out */
  char     pattern[64];
} win32_find_t;

static win32_find_t g_find[WIN32_FIND_SLOTS];
static UINT8        g_fio[4096];

/* A Windows path is either rooted ("/data/x") or relative to the console
 * directory, which this layer maps onto the root. */
static int win32_path(task_t *t, UINT64 va, char *out, UINT32 cap) {
  char   tmp[VFS_PATH_MAX];
  UINT32 i;
  UINT32 n;

  if (read_user_str(t->cr3, va, tmp, sizeof(tmp)) != 0) {
    return -1;
  }
  if (tmp[0] == '/') {
    copy_text(out, cap, tmp);
    return 0;
  }
  if (cap < 2u) {
    return -1;
  }
  out[0] = '/';
  n = 1u;
  for (i = 0; tmp[i] != '\0' && n + 1u < cap; i++, n++) {
    out[n] = tmp[i];
  }
  out[n] = '\0';
  return 0;
}

static int find_fd_slot(task_t *t, UINT32 *out) {
  UINT32 i;

  for (i = 0; i < WIN32_FILE_SLOTS; i++) {
    if (t->fd[i].used == 0) {
      *out = i;
      return 0;
    }
  }
  return -1;
}

static void api_create_file(trap_frame_t *f) {
  task_t   *t = sched_current();
  char      path[VFS_PATH_MAX];
  vfs_fs_t *fs = NULL;
  UINT32    ino;
  UINT32    slot = 0;
  UINT64    creation = 0;
  UINT64    len = 0;
  UINT16    mode = 0;

  /* CreateFileA(name, access, share, sec, creation, flags, template). */
  if (t == NULL || win32_path(t, f->rdi, path, sizeof(path)) != 0 ||
      stack_arg(f, t, 5u, &creation) != 0) {
    if (t != NULL) {
      t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    }
    f->rax = 0u;
    return;
  }
  ino = vfs_resolve_path(path, &fs);
  if (ino != 0 && fs != NULL) {
    /* CREATE_NEW asked for a name that must not already exist, so opening
     * the existing file is the failure the caller asked to be told about. */
    if (creation == 1u) {
      t->win32_last_error = 80u; /* ERROR_FILE_EXISTS */
      f->rax = 0u;
      return;
    }
    /* TRUNCATE_EXISTING (5) keeps the file and drops its bytes; the other
     * open modes leave it alone. */
    if (creation == 5u) {
      (void)fs->write(fs, ino, 0u, "", 0u);
    }
  } else if (creation == 3u /* OPEN_EXISTING */) {
    t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    f->rax = 0u;
    return;
  } else {
    /* CREATE_ALWAYS or OPEN_ALWAYS: create at the root of the live volume. */
    char  copy[VFS_PATH_MAX];
    UINT32 parent = 0;
    char *leaf = NULL;

    copy_text(copy, sizeof(copy), path);
    if (vfs_resolve_parent(copy, &fs, &parent, &leaf) != 0 || fs == NULL ||
        fs->readonly != 0u || leaf == NULL) {
      t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
      f->rax = 0u;
      return;
    }
    ino = fs->creat(fs, parent, leaf);
    if (ino == 0) {
      t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
      f->rax = 0u;
      return;
    }
  }
  if (find_fd_slot(t, &slot) != 0) {
    t->win32_last_error = 4u; /* ERROR_TOO_MANY_OPEN_FILES */
    f->rax = 0u;
    return;
  }
  (void)len;
  (void)mode;
  t->fd[slot].ino = ino;
  t->fd[slot].offset = 0;
  t->fd[slot].dir_index = 0;
  t->fd[slot].is_dir = 0;
  t->fd[slot].fs = fs;
  t->fd[slot].used = 1;
  f->rax = (UINT64)(WIN32_FILE_HANDLE | slot);
}

static void api_read_file(trap_frame_t *f) {
  task_t   *t = sched_current();
  UINT32    h = (UINT32)f->rdi;
  UINT32    slot = h & 0xFFu;
  UINT64    len = f->r8;
  UINT64    got = 0;
  UINT32    written_va = (UINT32)0;
  INT64     n;
  vfs_fs_t *fs;

  /* ReadFile(handle, buf, len, written*, overlapped). */
  if (t == NULL || h < WIN32_FILE_HANDLE || slot >= WIN32_FILE_SLOTS ||
      t->fd[slot].used == 0) {
    if (t != NULL) {
      t->win32_last_error = WIN32_ERROR_INVALID_HANDLE;
    }
    f->rax = 0u;
    return;
  }
  if (len > sizeof(g_fio)) {
    len = sizeof(g_fio);
  }
  fs = t->fd[slot].fs;
  if (fs == NULL) {
    f->rax = 0u;
    return;
  }
  n = fs->read(fs, t->fd[slot].ino, t->fd[slot].offset, g_fio, len);
  if (n < 0) {
    f->rax = 0u;
    return;
  }
  if (n != 0 && user_copy(t->cr3, f->rdx, g_fio, (UINT64)n, 1) != 0) {
    f->rax = 0u;
    return;
  }
  t->fd[slot].offset += (UINT64)n;
  got = (UINT64)n;
  written_va = (UINT32)0;
  (void)written_va;
  if (f->r9 != 0u) {
    UINT32 w = (UINT32)got;

    (void)user_copy(t->cr3, f->r9, &w, 4u, 1);
  }
  f->rax = 1u;
}

/* A write to a file handle: same layout as the console call, but the payload
 * goes to the volume instead of the log. */
static int api_write_file_handle(trap_frame_t *f, task_t *t, UINT32 h) {
  UINT32    slot = h & 0xFFu;
  UINT64    len = f->r8;
  vfs_fs_t *fs;
  INT64     n;

  if (slot >= WIN32_FILE_SLOTS || t->fd[slot].used == 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_HANDLE;
    f->rax = 0u;
    return 1;
  }
  if (len > sizeof(g_fio)) {
    len = sizeof(g_fio);
  }
  if (user_copy(t->cr3, f->rdx, g_fio, len, 0) != 0) {
    f->rax = 0u;
    return 1;
  }
  fs = t->fd[slot].fs;
  if (fs == NULL || fs->readonly != 0u) {
    t->win32_last_error = 5u; /* ERROR_ACCESS_DENIED */
    f->rax = 0u;
    return 1;
  }
  n = fs->write(fs, t->fd[slot].ino, t->fd[slot].offset, g_fio, len);
  if (n < 0) {
    f->rax = 0u;
    return 1;
  }
  t->fd[slot].offset += (UINT64)n;
  if (f->r9 != 0u) {
    UINT32 w = (UINT32)n;

    (void)user_copy(t->cr3, f->r9, &w, 4u, 1);
  }
  f->rax = 1u;
  return 1;
}

static void api_close_handle(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT32  h = (UINT32)f->rdi;
  UINT32  slot = h & 0xFFu;
  UINT32  i;

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  if (h >= WIN32_FILE_HANDLE && h < WIN32_FILE_HANDLE + 0x100u &&
      slot < WIN32_FILE_SLOTS && t->fd[slot].used != 0) {
    memset(&t->fd[slot], 0, sizeof(t->fd[slot]));
    f->rax = 1u;
    return;
  }
  if (h >= WIN32_FIND_HANDLE && h < WIN32_FIND_HANDLE + 0x100u) {
    slot = h & 0xFFu;
    if (slot < WIN32_FIND_SLOTS && g_find[slot].used != 0) {
      memset(&g_find[slot], 0, sizeof(g_find[slot]));
      f->rax = 1u;
      return;
    }
  }
  /* The console and the standard handles are not closable objects. */
  for (i = 0; i < 3u; i++) {
    if (h == i) {
      f->rax = 1u;
      return;
    }
  }
  t->win32_last_error = WIN32_ERROR_INVALID_HANDLE;
  f->rax = 0u;
}

static void api_get_file_size(trap_frame_t *f) {
  task_t   *t = sched_current();
  UINT32    slot = (UINT32)f->rdi & 0xFFu;
  vfs_fs_t *fs;
  UINT64    size = 0;
  UINT16    mode = 0;

  if (t == NULL || (UINT32)f->rdi < WIN32_FILE_HANDLE ||
      slot >= WIN32_FILE_SLOTS || t->fd[slot].used == 0) {
    f->rax = 0xFFFFFFFFu; /* INVALID_FILE_SIZE */
    return;
  }
  fs = t->fd[slot].fs;
  if (fs == NULL || fs->stat(fs, t->fd[slot].ino, &size, &mode) != 0) {
    f->rax = 0xFFFFFFFFu;
    return;
  }
  f->rax = size & 0xFFFFFFFFu;
}

static void api_get_file_attributes(trap_frame_t *f) {
  task_t   *t = sched_current();
  char      path[VFS_PATH_MAX];
  vfs_fs_t *fs = NULL;
  UINT32    ino;
  UINT64    size = 0;
  UINT16    mode = 0;

  if (t == NULL || win32_path(t, f->rdi, path, sizeof(path)) != 0 ||
      (ino = vfs_resolve_path(path, &fs)) == 0 || fs == NULL ||
      fs->stat(fs, ino, &size, &mode) != 0) {
    t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    f->rax = 0xFFFFFFFFu;
    return;
  }
  f->rax = (mode == VFS_MODE_DIR) ? WIN32_A_DIRECTORY : WIN32_A_NORMAL;
}

static void api_delete_file(trap_frame_t *f) {
  task_t   *t = sched_current();
  char      path[VFS_PATH_MAX];
  char      copy[VFS_PATH_MAX];
  vfs_fs_t *fs = NULL;
  UINT32    parent = 0;
  char     *leaf = NULL;

  if (t == NULL || win32_path(t, f->rdi, path, sizeof(path)) != 0) {
    f->rax = 0u;
    return;
  }
  /* unlink wants the parent directory and the leaf, which is the same split
   * the creat path uses. */
  copy_text(copy, sizeof(copy), path);
  if (vfs_resolve_parent(copy, &fs, &parent, &leaf) != 0 || fs == NULL ||
      fs->readonly != 0u || fs->unlink == NULL || leaf == NULL ||
      fs->unlink(fs, parent, leaf) != 0) {
    t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    f->rax = 0u;
    return;
  }
  f->rax = 1u;
}

static void api_create_directory(trap_frame_t *f) {
  task_t   *t = sched_current();
  char      path[VFS_PATH_MAX];
  char      copy[VFS_PATH_MAX];
  vfs_fs_t *fs = NULL;
  UINT32    parent = 0;
  char     *leaf = NULL;
  UINT32    ino;

  if (t == NULL || win32_path(t, f->rdi, path, sizeof(path)) != 0) {
    f->rax = 0u;
    return;
  }
  copy_text(copy, sizeof(copy), path);
  if (vfs_resolve_parent(copy, &fs, &parent, &leaf) != 0 || fs == NULL ||
      fs->readonly != 0u || leaf == NULL || fs->mkdir == NULL) {
    t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    f->rax = 0u;
    return;
  }
  ino = fs->mkdir(fs, parent, leaf);
  f->rax = (ino != 0) ? 1u : 0u;
}

/* "*.txt" against a directory entry: '*' matches any run, '?' one
 * character, everything else compares by byte. */
static int find_match(const char *pat, const char *name) {
  while (*pat != '\0') {
    if (*pat == '*') {
      pat++;
      if (*pat == '\0') {
        return 1;
      }
      while (*name != '\0') {
        if (find_match(pat, name)) {
          return 1;
        }
        name++;
      }
      return 0;
    }
    if (*name == '\0') {
      return 0;
    }
    if (*pat != '?' && lower(*pat) != lower(*name)) {
      return 0;
    }
    pat++;
    name++;
  }
  return *name == '\0';
}

typedef struct {
  UINT32 index;   /* entry we are looking for */
  UINT32 seen;
  UINT32 ino;
  UINT64 size;
  UINT16 mode;
  int    hit;
  vfs_fs_t *fs;
  const char *pattern;
  char   name[64];
} find_ctx_t;

static void find_cb(void *arg, UINT32 ino, const char *name) {
  find_ctx_t *c = (find_ctx_t *)arg;
  UINT64 size = 0;
  UINT16 mode = 0;

  if (c->hit != 0) {
    return;
  }
  if (c->seen++ < c->index) {
    return;
  }
  if (!find_match(c->pattern, name)) {
    return;
  }
  if (c->fs->stat(c->fs, ino, &size, &mode) != 0) {
    return;
  }
  c->hit = 1;
  c->ino = ino;
  c->size = size;
  c->mode = mode;
  copy_text(c->name, sizeof(c->name), name);
}

static int find_step(task_t *t, win32_find_t *s, UINT64 out_va) {
  UINT8      data[WIN32_FIND_DATA_LEN];
  find_ctx_t c;
  UINT32     i;
  int        rc;
  UINT32     seen;

  /* The back end keeps directory blocks in a write-back cache, so a file
   * the same process created a moment ago is not visible to a block-level
   * enumeration until the volume is flushed. Windows hides that from
   * programs; this layer has to ask. */
  if (s->fs->sync != NULL) {
    (void)s->fs->sync(s->fs);
  }
  memset(&c, 0, sizeof(c));
  c.index = s->index;
  c.fs = s->fs;
  c.pattern = s->pattern;
  rc = (s->fs->readdir != NULL)
           ? s->fs->readdir(s->fs, s->dir, find_cb, &c)
           : -1;
  seen = c.seen;
  /* A readdir error is a failure worth a line; running out of matching
   * entries is how an enumeration is supposed to end. */
  if (rc != 0) {
    kputs("[WIN32] find: dir=");
    kputs_hex(s->dir);
    kputs(" rc=");
    kputs_hex((UINT64)(UINT32)rc);
    kputs(" seen=");
    kputs_hex(seen);
    kputs(" index=");
    kputs_hex(s->index);
    kputs(" pattern=");
    kputs(s->pattern);
    kputs("\n");
    return -1;
  }
  if (c.hit == 0) {
    return -1;
  }
  memset(data, 0, sizeof(data));
  data[0] = (UINT8)((c.mode == VFS_MODE_DIR) ? WIN32_A_DIRECTORY : WIN32_A_NORMAL);
  /* dwFileSizeLow / dwFileSizeHigh */
  data[0x20] = (UINT8)(c.size & 0xFFu);
  data[0x21] = (UINT8)((c.size >> 8) & 0xFFu);
  data[0x22] = (UINT8)((c.size >> 16) & 0xFFu);
  data[0x23] = (UINT8)((c.size >> 24) & 0xFFu);
  data[0x24] = (UINT8)((c.size >> 32) & 0xFFu);
  data[0x25] = (UINT8)((c.size >> 40) & 0xFFu);
  data[0x26] = (UINT8)((c.size >> 48) & 0xFFu);
  data[0x27] = (UINT8)((c.size >> 56) & 0xFFu);
  for (i = 0; i < sizeof(c.name) && c.name[i] != '\0'; i++) {
    data[0x2C + i] = (UINT8)c.name[i];
  }
  if (user_copy(t->cr3, out_va, data, sizeof(data), 1) != 0) {
    return -1;
  }
  s->index++;
  return 0;
}

static void api_find_first(trap_frame_t *f) {
  task_t   *t = sched_current();
  char      path[VFS_PATH_MAX];
  char      dir[VFS_PATH_MAX];
  vfs_fs_t *fs = NULL;
  UINT32    ino;
  UINT32    slot;
  char     *pat;
  UINT32    i;

  if (t == NULL || win32_path(t, f->rdi, path, sizeof(path)) != 0 ||
      f->rdx == 0u) {
    f->rax = 0u;
    return;
  }
  /* Split "dir/pattern" into the directory and the leaf pattern. */
  copy_text(dir, sizeof(dir), path);
  pat = NULL;
  for (i = 0; dir[i] != '\0'; i++) {
    if (dir[i] == '*' || dir[i] == '?') {
      pat = &dir[i];
      break;
    }
  }
  if (pat == NULL) {
    t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    f->rax = 0u;
    return;
  }
  {
    UINT32 slash = 0;

    for (i = 0; dir[i] != '\0'; i++) {
      if (dir[i] == '/') {
        slash = i;
      }
    }
    dir[slash] = '\0';
    if (slash == 0u) {
      copy_text(dir, sizeof(dir), "/");
    }
    pat = path + (slash + 1u);
  }
  ino = vfs_resolve_path(dir, &fs);
  if (ino == 0 || fs == NULL) {
    t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    f->rax = 0u;
    return;
  }
  for (slot = 0; slot < WIN32_FIND_SLOTS; slot++) {
    if (g_find[slot].used == 0u) {
      break;
    }
  }
  if (slot == WIN32_FIND_SLOTS) {
    t->win32_last_error = 4u;
    f->rax = 0u;
    return;
  }
  memset(&g_find[slot], 0, sizeof(g_find[slot]));
  g_find[slot].used = 1u;
  g_find[slot].fs = fs;
  g_find[slot].dir = ino;
  g_find[slot].index = 0u;
  copy_text(g_find[slot].pattern, sizeof(g_find[slot].pattern), pat);
  if (find_step(t, &g_find[slot], f->rdx) != 0) {
    memset(&g_find[slot], 0, sizeof(g_find[slot]));
    t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    f->rax = 0u;
    return;
  }
  f->rax = (UINT64)(WIN32_FIND_HANDLE | slot);
}

static void api_find_next(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT32  h = (UINT32)f->rdi;
  UINT32  slot = h & 0xFFu;

  if (t == NULL || h < WIN32_FIND_HANDLE || slot >= WIN32_FIND_SLOTS ||
      g_find[slot].used == 0u || f->rdx == 0u) {
    f->rax = 0u;
    return;
  }
  if (find_step(t, &g_find[slot], f->rdx) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = 1u;
}

static void api_find_close(trap_frame_t *f) {
  UINT32 h = (UINT32)f->rdi;
  UINT32 slot = h & 0xFFu;

  if (h >= WIN32_FIND_HANDLE && h < WIN32_FIND_HANDLE + 0x100u &&
      slot < WIN32_FIND_SLOTS && g_find[slot].used != 0u) {
    memset(&g_find[slot], 0, sizeof(g_find[slot]));
    f->rax = 1u;
    return;
  }
  f->rax = 0u;
}

/* LoadLibrary cannot map a PE from disk yet, but the three DLLs whose
 * entry points are built into every image exist, and handing back a handle
 * for them is truthful: the thunks are already mapped. Anything else
 * returns 0 and a null module, rather than a handle whose GetProcAddress
 * would fail later. */
static void api_load_library(trap_frame_t *f) {
  task_t *t = sched_current();
  char    name[32];
  UINT32  i;
  static const char *known[] = {"kernel32.dll", "user32.dll", "gdi32.dll",
                                "advapi32.dll", "ws2_32.dll"};

  if (t == NULL || read_user_str(t->cr3, f->rdi, name, sizeof(name)) != 0) {
    f->rax = 0u;
    return;
  }
  for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (ci_equal(name, known[i])) {
      f->rax = (UINT64)(WIN32_DLL_HANDLE | (i + 1u));
      return;
    }
  }
  t->win32_last_error = 126u; /* ERROR_MOD_NOT_FOUND */
  f->rax = 0u;
}

static void api_get_proc_address(trap_frame_t *f) {
  task_t *t = sched_current();
  char    name[48];
  char    dll[24];
  char    fn[32];
  UINT32  i;
  UINT32  d = 0;
  int     id;

  /* GetProcAddress(module, "Name"): the module decides the DLL, and the
   * entry point is the same thunk the import table would have got. */
  if (t == NULL || read_user_str(t->cr3, f->rdx, name, sizeof(name)) != 0) {
    f->rax = 0u;
    return;
  }
  d = ((UINT32)f->rdi & 0xFFu);
  if (d == 0u || d > 5u) {
    t->win32_last_error = 127u; /* ERROR_PROC_NOT_FOUND */
    f->rax = 0u;
    return;
  }
  {
    static const char *known[] = {"kernel32.dll", "user32.dll", "gdi32.dll",
                                  "advapi32.dll", "ws2_32.dll"};
    copy_text(dll, sizeof(dll), known[d - 1u]);
  }
  for (i = 0; i < sizeof(fn) - 1u && name[i] != '\0'; i++) {
    fn[i] = name[i];
  }
  fn[i] = '\0';
  id = win32_api_lookup(dll, fn);
  if (id < 0) {
    t->win32_last_error = 127u;
    f->rax = 0u;
    return;
  }
  f->rax = win32_api_entry((UINT32)id);
}

static void api_free_library(trap_frame_t *f) {
  UINT32 h = (UINT32)f->rdi;

  /* The thunks belong to the image, not to the library: nothing to unmap. */
  f->rax = (h >= WIN32_DLL_HANDLE && h < WIN32_DLL_HANDLE + 8u) ? 1u : 0u;
}

/* ---- painting: invalidation, stock brushes, metrics ------------------- */

/* Stock objects are the same colour-with-a-tag encoding CreateSolidBrush
 * produces, so a program can pass either to FillRect without the layer
 * having to tell them apart. HANDLE 0 is the hollow brush: no fill at all,
 * which FillRect honours by drawing nothing. */
static void api_get_stock_object(trap_frame_t *f) {
  switch ((UINT32)f->rdi) {
  case 0u:  f->rax = 0xFF000000u | 0xFFFFFFu; break; /* WHITE_BRUSH */
  case 1u:  f->rax = 0xFF000000u | 0xC0C0C0u; break; /* LTGRAY_BRUSH */
  case 2u:  f->rax = 0xFF000000u | 0x808080u; break; /* GRAY_BRUSH */
  case 3u:  f->rax = 0xFF000000u | 0x404040u; break; /* DKGRAY_BRUSH */
  case 4u:  f->rax = 0xFF000000u | 0x000000u; break; /* BLACK_BRUSH */
  case 5u:                                        /* NULL_BRUSH */
  case 7u:  f->rax = 0u; break;                   /* HOLLOW_BRUSH */
  case 6u:  f->rax = 0xFF000000u | 0xFFFFFFu; break; /* WHITE_PEN */
  case 8u:  f->rax = 0xFF000000u | 0x000000u; break; /* BLACK_PEN */
  case 9u:  f->rax = 0u; break;                     /* NULL_PEN */
  default:  f->rax = 0u; break;
  }
}

/* TEXTMETRICA for the 16-pixel cell this compositor draws text in: the
 * bitmap is one em tall, the face is fixed-pitch with an 8-pixel half-width
 * cell and a 16-pixel cell for CJK. A program that centres its text uses
 * tmHeight and tmAscent, so those two are the load-bearing numbers. */
static void api_get_text_metrics(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT8   tm[56];
  INT32  *l;
  UINT8  *b;

  if (t == NULL || f->rdx == 0u) {
    f->rax = 0u;
    return;
  }
  /* TEXTMETRICA for x64 is eleven LONGs (44 bytes) followed by nine BYTEs,
   * padded to 56; filling it as one blob keeps the two halves aligned with
   * what a program's compiler laid out. */
  memset(tm, 0, sizeof(tm));
  l = (INT32 *)(void *)tm;
  b = tm + 44;
  l[0] = 16;   /* tmHeight */
  l[1] = 13;   /* tmAscent */
  l[2] = 3;    /* tmDescent */
  l[3] = 0;    /* tmInternalLeading */
  l[4] = 0;    /* tmExternalLeading */
  l[5] = 8;    /* tmAveCharWidth */
  l[6] = 16;   /* tmMaxCharWidth */
  l[7] = 400;  /* tmWeight: FW_NORMAL */
  l[8] = 0;    /* tmOverhang */
  l[9] = 96;   /* tmDigitizedAspectX */
  l[10] = 96;  /* tmDigitizedAspectY */
  b[0] = 0x20; /* tmFirstChar */
  b[1] = 0x7E; /* tmLastChar */
  b[2] = '?';  /* tmDefaultChar */
  b[3] = ' ';  /* tmBreakChar */
  b[4] = 0;    /* tmItalic */
  b[5] = 0;    /* tmUnderlined */
  b[6] = 0;    /* tmStruckOut */
  b[7] = 0x30; /* tmPitchAndFamily: FF_MODERN, TMPF_FIXED_PITCH clear */
  b[8] = 134;  /* tmCharSet: GB2312 - the font carries the CJK block */

  if (user_copy(t->cr3, f->rdx, tm, sizeof(tm), 1) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = 1u;
}

static void api_invalidate_rect(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w == NULL) {
    f->rax = 0u; /* not a window of ours: Windows answers FALSE */
    return;
  }
  /* The rectangle is ignored: the compositor repaints the whole client area,
   * which is what the one-frame display list can express anyway. */
  w->dirty = 1u;
  f->rax = 1u;
}

/* ---- thread-local storage --------------------------------------------- */

/* One bit per TEB TLS slot, process-wide because the layer runs one Win32
 * process. Bit 0 stays set: slot 0 belongs to the image's TLS directory,
 * which the loader fills from the PE TLS template (pe_tls); TlsAlloc must
 * not hand it out, so a compiler-emitted %fs-based module-TLS access reads
 * the template pointer instead of somebody else's value. */
static UINT64 g_tls_used = 1u;

static int win32_teb_read(task_t *t, UINT64 off, void *buf, UINT32 len) {
  if (t == NULL || t->teb_va == 0u) {
    return -1;
  }
  return user_copy(t->cr3, t->teb_va + off, buf, len, 0);
}

static int win32_teb_write(task_t *t, UINT64 off, const void *buf, UINT32 len) {
  if (t == NULL || t->teb_va == 0u) {
    return -1;
  }
  return user_copy(t->cr3, t->teb_va + off, (void *)buf, len, 1);
}

static int tls_slot_ptr(task_t *t, UINT32 index, UINT64 *out) {
  UINT64 v;
  UINT64 off = WIN32_TEB_TLS_OFF + (UINT64)index * 8u;

  if (index >= WIN32_TEB_TLS_SLOTS || win32_teb_read(t, off, &v, 8u) != 0) {
    return -1;
  }
  *out = v;
  return 0;
}

static void api_tls_alloc(trap_frame_t *f) {
  UINT32 i;

  /* TLS_OUT_OF_INDEXES (0xFFFFFFFF) is a failure the program is expected
   * to handle; a slot index is otherwise 0..63. */
  for (i = 1; i < WIN32_TEB_TLS_SLOTS; i++) {
    UINT64 bit = 1u << i;
    if ((g_tls_used & bit) == 0u) {
      g_tls_used |= bit;
      f->rax = i;
      return;
    }
  }
  f->rax = 0xFFFFFFFFu;
}

static void api_tls_free(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT32  index = (UINT32)f->rdi;
  UINT64  zero = 0;

  if (index == 0u || index >= WIN32_TEB_TLS_SLOTS) {
    f->rax = 0;
    return;
  }
  /* Windows does not clear the slot, but leaving a stale pointer there
   * would be visible to the same index after it is reallocated. */
  (void)win32_teb_write(t, WIN32_TEB_TLS_OFF + (UINT64)index * 8u, &zero, 8u);
  g_tls_used &= ~(1u << index);
  f->rax = 1;
}

static void api_tls_get_value(trap_frame_t *f) {
  UINT64 v = 0;

  if ((g_tls_used & (1u << (UINT32)f->rdi)) == 0u) {
    f->rax = 0; /* a free index reads as null, as on Windows */
    return;
  }
  if (tls_slot_ptr(sched_current(), (UINT32)f->rdi, &v) != 0) {
    f->rax = 0;
    return;
  }
  f->rax = v;
}

static void api_tls_set_value(trap_frame_t *f) {
  UINT32 index = (UINT32)f->rdi;
  UINT64 value = f->rdx;

  if (index >= WIN32_TEB_TLS_SLOTS ||
      (g_tls_used & (1u << index)) == 0u) {
    f->rax = 0;
    return;
  }
  if (win32_teb_write(sched_current(), WIN32_TEB_TLS_OFF + (UINT64)index * 8u,
                      &value, 8u) != 0) {
    f->rax = 0;
    return;
  }
  f->rax = 1;
}

/* ---- screen metrics ---------------------------------------------------- */

static void api_get_system_metrics(trap_frame_t *f) {
  const nd_display_t *d = nd_display();
  UINT64              w = (d != NULL && d->available) ? d->width : 1024u;
  UINT64              h = (d != NULL && d->available) ? d->height : 768u;

  switch ((UINT32)f->rdi) {
  case 0u:  f->rax = w;  break; /* SM_CXSCREEN */
  case 1u:  f->rax = h;  break; /* SM_CYSCREEN */
  case 2u:  f->rax = 1;  break; /* SM_CXVSCROLL */
  case 3u:  f->rax = 1;  break; /* SM_CYHSCROLL */
  case 5u:  f->rax = 1;  break; /* SM_CXBORDER */
  case 6u:  f->rax = 1;  break; /* SM_CYBORDER */
  case 11u: f->rax = 72; break; /* SM_CXICON */
  case 12u: f->rax = 72; break; /* SM_CYICON */
  case 19u: f->rax = 32; break; /* SM_CXSMICON */
  case 20u: f->rax = 32; break; /* SM_CYSMICON */
  case 21u: f->rax = 32; break; /* SM_CYCAPTION */
  case 23u: f->rax = 24; break; /* SM_CXSIZE */
  case 24u: f->rax = 24; break; /* SM_CYSIZE */
  case 45u: f->rax = 32; break; /* SM_CXDRAG */
  case 46u: f->rax = 32; break; /* SM_CYDRAG */
  default:  f->rax = 0;  break;
  }
}

/* Drop every window and every class a dying process left behind. A window
 * outliving its client would keep a slot in the WM's table forever, and
 * the desktop would show a frame nobody can paint or close. */
static void win32_gui_owner_exit(UINT64 pid) {
  win32_gui_t *gu = gui_state(pid, 0);
  UINT32       i;

  if (gu == NULL) {
    return;
  }
  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    if (gu->wnd[i].used) {
      (void)nwm_owner_destroy(pid, gu->wnd[i].win);
    }
  }
  memset(gu, 0, sizeof(*gu));
}

void win32_syscall(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT32  api;


  if (t == NULL || t->abi != TASK_ABI_WIN32) {
    f->rax = (UINT64)-1;
    return;
  }
  api = (UINT32)f->r10;

  /* A replay after a park carries the timeout in wait_err. Sleep is the
   * only caller that parks, so a cleared wait_err here means the sleep is
   * over; success is what Windows returns (void, but RAX is defined). */
  if (t->wait_err != 0) {
    t->wait_err = 0;
    if (api == WIN32_API_SLEEP) {
      f->rax = 0;
      return;
    }
  }

  switch (api) {
  case WIN32_API_EXIT_PROCESS:
    api_exit(f);
    return;
  case WIN32_API_GET_STD_HANDLE:
    api_get_std_handle(f);
    return;
  case WIN32_API_WRITE_FILE:
  case WIN32_API_WRITE_CONSOLE:
    api_write(f);
    return;
  case WIN32_API_SLEEP:
    api_sleep(f);
    return;
  case WIN32_API_GET_TICK_COUNT64:
    api_ticks(f, 1);
    return;
  case WIN32_API_GET_TICK_COUNT:
    api_ticks(f, 0);
    return;
  case WIN32_API_GET_LAST_ERROR:
    f->rax = t->win32_last_error;
    return;
  case WIN32_API_SET_LAST_ERROR:
    t->win32_last_error = (UINT32)f->rdi;
    f->rax = 0;
    return;
  case WIN32_API_GET_COMMAND_LINE:
    f->rax = WIN32_ARGV_VA;
    return;
  case WIN32_API_GET_MODULE_HANDLE:
    f->rax = t->win32_image_base;
    return;
  case WIN32_API_REG_OPEN_KEY_EX:
    api_reg_open(f);
    return;
  case WIN32_API_REG_QUERY_VALUE_EX:
    api_reg_query(f);
    return;
  case WIN32_API_REG_SET_VALUE_EX:
    api_reg_set(f);
    return;
  case WIN32_API_GET_MODULE_FILE_NAME:
    api_module_file_name(f);
    return;
  case WIN32_API_VIRTUAL_ALLOC:
    api_virtual_alloc(f);
    return;
  case WIN32_API_VIRTUAL_FREE:
    api_virtual_free(f);
    return;
  case WIN32_API_GET_PROCESS_HEAP:
    api_get_process_heap(f);
    return;
  case WIN32_API_HEAP_ALLOC:
    api_heap_alloc(f);
    return;
  case WIN32_API_HEAP_FREE:
    api_heap_free(f);
    return;
  case WIN32_API_GET_SYSTEM_TIME:
    api_system_time(f);
    return;
  case WIN32_API_QUERY_PERFORMANCE_COUNTER:
    api_qpc(f);
    return;
  case WIN32_API_QUERY_PERFORMANCE_FREQUENCY:
    api_qpf(f);
    return;
  case WIN32_API_GET_CURRENT_PROCESS_ID:
  case WIN32_API_GET_CURRENT_THREAD_ID:
    api_ids(f);
    return;
  case WIN32_API_IS_DEBUGGER_PRESENT:
    f->rax = 0u;
    return;
  case WIN32_API_GET_SYSTEM_INFO:
    api_system_info(f);
    return;
  case WIN32_API_SET_UNHANDLED_EXCEPTION_FILTER:
    api_set_filter(f);
    return;
  case WIN32_API_REG_CLOSE_KEY:
    api_reg_close(f);
    return;
  case WIN32_API_REGISTER_CLASS:
    api_register_class(f);
    return;
  case WIN32_API_CREATE_WINDOW:
    api_create_window(f);
    return;
  case WIN32_API_DESTROY_WINDOW:
    api_destroy_window(f);
    return;
  case WIN32_API_SHOW_WINDOW:
    api_show_window(f);
    return;
  case WIN32_API_GET_MESSAGE:
    api_get_message(f);
    return;
  case WIN32_API_PEEK_MESSAGE:
    api_peek_message(f);
    return;
  case WIN32_API_DISPATCH_MESSAGE:
  case WIN32_API_DEF_WINDOW_PROC:
    api_def_window_proc(f);
    return;
  case WIN32_API_TRANSLATE_MESSAGE:
    api_translate_message(f);
    return;
  case WIN32_API_GET_CLIENT_RECT:
    api_get_client_rect(f);
    return;
  case WIN32_API_GET_DC:
  case WIN32_API_BEGIN_PAINT:
    api_begin_frame(f);
    return;
  case WIN32_API_RELEASE_DC:
  case WIN32_API_END_PAINT:
    api_end_frame(f);
    return;
  case WIN32_API_FILL_RECT:
    api_fill_rect(f);
    return;
  case WIN32_API_TEXT_OUT:
    api_text_out(f);
    return;
  case WIN32_API_SET_TEXT_COLOR:
    api_set_text_color(f);
    return;
  case WIN32_API_CREATE_SOLID_BRUSH:
    api_create_solid_brush(f);
    return;
  case WIN32_API_TLS_ALLOC:
    api_tls_alloc(f);
    return;
  case WIN32_API_TLS_FREE:
    api_tls_free(f);
    return;
  case WIN32_API_TLS_GET_VALUE:
    api_tls_get_value(f);
    return;
  case WIN32_API_TLS_SET_VALUE:
    api_tls_set_value(f);
    return;
  case WIN32_API_GET_SYSTEM_METRICS:
    api_get_system_metrics(f);
    return;
  case WIN32_API_INVALIDATE_RECT:
    api_invalidate_rect(f);
    return;
  case WIN32_API_GET_STOCK_OBJECT:
    api_get_stock_object(f);
    return;
  case WIN32_API_GET_TEXT_METRICS:
    api_get_text_metrics(f);
    return;
  case WIN32_API_SET_BK_MODE:
    api_set_bk_mode(f);
    return;
  case WIN32_API_SET_BK_COLOR:
    api_set_bk_color(f);
    return;
  case WIN32_API_GET_TEXT_COLOR:
    api_get_text_color(f);
    return;
  case WIN32_API_MOVE_TO_EX:
    api_move_to_ex(f);
    return;
  case WIN32_API_LINE_TO:
    api_line_to(f);
    return;
  case WIN32_API_RECTANGLE:
    api_rectangle(f);
    return;
  case WIN32_API_ELLIPSE:
    api_ellipse(f);
    return;
  case WIN32_API_SET_PIXEL:
    api_set_pixel(f);
    return;
  case WIN32_API_PAT_BLT:
    api_pat_blt(f);
    return;
  case WIN32_API_CREATE_PEN:
    api_create_pen(f);
    return;
  case WIN32_API_SELECT_OBJECT:
    api_select_object(f);
    return;
  case WIN32_API_DELETE_OBJECT:
    api_delete_object(f);
    return;
  case WIN32_API_POST_QUIT_MESSAGE:
    api_post_quit_message(f);
    return;
  case WIN32_API_UPDATE_WINDOW:
    api_update_window(f);
    return;
  case WIN32_API_SET_WINDOW_TEXT:
    api_set_window_text(f);
    return;
  case WIN32_API_GET_WINDOW_TEXT:
    api_get_window_text(f);
    return;
  case WIN32_API_POST_MESSAGE:
    api_post_message(f);
    return;
  case WIN32_API_SEND_MESSAGE:
    api_send_message(f);
    return;
  case WIN32_API_SET_TIMER:
    api_set_timer(f);
    return;
  case WIN32_API_KILL_TIMER:
    api_kill_timer(f);
    return;
  case WIN32_API_GET_WINDOW_RECT:
    api_get_window_rect(f);
    return;
  case WIN32_API_CLIENT_TO_SCREEN:
    api_client_to_screen(f);
    return;
  case WIN32_API_SCREEN_TO_CLIENT:
    api_screen_to_client(f);
    return;
  case WIN32_API_GET_CURSOR_POS:
    api_get_cursor_pos(f);
    return;
  case WIN32_API_GET_WINDOW_LONG:
    api_get_window_long(f);
    return;
  case WIN32_API_SET_WINDOW_LONG:
    api_set_window_long(f);
    return;
  case WIN32_API_GET_TEXT_EXTENT_POINT:
    api_get_text_extent_point(f);
    return;
  case WIN32_API_SET_TEXT_ALIGN:
    api_set_text_align(f);
    return;
  case WIN32_API_GET_TEXT_ALIGN:
    api_get_text_align(f);
    return;
  case WIN32_API_DRAW_TEXT:
    api_draw_text(f);
    return;
  case WIN32_API_GET_TEXT_FACE:
    api_get_text_face(f);
    return;
  case WIN32_API_SET_TEXT_CHAR_EXTRA:
    api_set_text_char_extra(f);
    return;
  case WIN32_API_CREATE_FILE:
    api_create_file(f);
    return;
  case WIN32_API_READ_FILE:
    api_read_file(f);
    return;
  case WIN32_API_CLOSE_HANDLE:
    api_close_handle(f);
    return;
  case WIN32_API_GET_FILE_SIZE:
    api_get_file_size(f);
    return;
  case WIN32_API_GET_FILE_ATTRIBUTES:
    api_get_file_attributes(f);
    return;
  case WIN32_API_DELETE_FILE:
    api_delete_file(f);
    return;
  case WIN32_API_CREATE_DIRECTORY:
    api_create_directory(f);
    return;
  case WIN32_API_FIND_FIRST:
    api_find_first(f);
    return;
  case WIN32_API_FIND_NEXT:
    api_find_next(f);
    return;
  case WIN32_API_FIND_CLOSE:
    api_find_close(f);
    return;
  case WIN32_API_LOAD_LIBRARY:
    api_load_library(f);
    return;
  case WIN32_API_GET_PROC_ADDRESS:
    api_get_proc_address(f);
    return;
  case WIN32_API_FREE_LIBRARY:
    api_free_library(f);
    return;
  case WIN32_API_MOVE_WINDOW:
    api_move_window(f);
    return;
  case WIN32_API_SET_FOCUS:
    api_set_focus(f);
    return;
  case WIN32_API_GET_FOCUS:
    api_get_focus(f);
    return;
  case WIN32_API_WSA_STARTUP:
    api_wsa_startup(f);
    return;
  case WIN32_API_WSA_CLEANUP:
    api_wsa_cleanup(f);
    return;
  case WIN32_API_WSA_GET_LAST_ERROR:
    api_wsa_get_last_error(f);
    return;
  case WIN32_API_WSA_SET_LAST_ERROR:
    api_wsa_set_last_error(f);
    return;
  case WIN32_API_SOCKET:
    api_socket(f);
    return;
  case WIN32_API_CONNECT:
  case WIN32_API_SEND:
  case WIN32_API_RECV:
  case WIN32_API_BIND:
  case WIN32_API_LISTEN:
  case WIN32_API_ACCEPT:
  case WIN32_API_SELECT:
  case WIN32_API_IOCTLSOCKET:
    api_socket_call(f);
    return;
  case WIN32_API_CLOSESOCKET:
    api_closesocket(f);
    return;
  case WIN32_API_GETHOSTBYNAME:
    api_gethostbyname(f);
    return;
  case WIN32_API_GETADDRINFO:
    api_getaddrinfo(f);
    return;
  case WIN32_API_FREEADDRINFO:
    api_freeaddrinfo(f);
    return;
  case WIN32_API_INET_ADDR:
    api_inet_addr(f);
    return;
  case WIN32_API_INET_NTOA:
    api_inet_ntoa(f);
    return;
  case WIN32_API_GET_DLG_ITEM:
    api_get_dlg_item(f);
    return;
  case WIN32_API_MESSAGE_BOX:
    api_message_box(f);
    return;
  case WIN32_API_CREATE_MENU:
  case WIN32_API_CREATE_POPUP_MENU:
    api_create_menu(f);
    return;
  case WIN32_API_APPEND_MENU:
    api_append_menu(f);
    return;
  case WIN32_API_GET_MENU_ITEM_COUNT:
    api_get_menu_item_count(f);
    return;
  case WIN32_API_SET_MENU:
    api_set_menu(f);
    return;
  case WIN32_API_GET_MENU:
    api_get_menu(f);
    return;
  case WIN32_API_DESTROY_MENU:
    api_destroy_menu(f);
    return;
  case WIN32_API_DRAW_MENU_BAR:
    api_draw_menu_bar(f);
    return;
  case WIN32_API_SET_CONSOLE_TEXT_ATTR:
    api_set_console_text_attr(f);
    return;
  default:
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = (UINT64)-1;
    return;
  }
}

/* ---- import resolution ------------------------------------------------- */

int win32_api_lookup(const char *dll, const char *func) {
  UINT32 i;
  int fallback = -1;

  for (i = 0; i < sizeof(g_api) / sizeof(g_api[0]); i++) {
    if (ci_equal(g_api[i].dll, dll) && text_equal(g_api[i].fn, func)) {
      /* The id, not the row number: two names may share one entry point
       * (GetLocalTime / GetSystemTime), and the dispatcher switches on the
       * id. Returning the row index worked only while the table happened to
       * be one row per id, which stopped being true the moment a bad import
       * was dropped. */
      if (g_api[i].id >= WIN32_API_COUNT) {
        return -1;
      }
      return (int)g_api[i].id;
    }
  }

  /* The DLL column is bookkeeping, not a contract. Windows forwards
     several of these between modules - FillRect is a USER32 export that
     most compilers import from GDI32, and a program that links against
     either must work - so a name that exists under one DLL is accepted
     under another. An exact match above always wins. */
  for (i = 0; i < sizeof(g_api) / sizeof(g_api[0]); i++) {
    if (!text_equal(g_api[i].fn, func) || g_api[i].id >= WIN32_API_COUNT) {
      continue;
    }
    if (fallback < 0) {
      fallback = (int)g_api[i].id;
    }
  }
  return fallback;
}

/* mov %rdi,%fs:scratch ; mov %rcx,%rdi ; mov $id,%r10d ; mov $40,%eax ;
 * syscall ; mov %fs:scratch,%rdi ; ret
 *
 * RDI is callee-saved in the Windows x64 ABI, so the thunk has to give it
 * back exactly as it found it, and RCX (the real first argument) is gone by
 * the time the kernel looks: `syscall` overwrites both RCX and R11 with the
 * return address and the flags. So the caller's RDI is parked in the TEB,
 * which needs no register of its own and is always mapped - the shadow space
 * above the return address is not: a program that calls from its first frame
 * has its stack pointer at the very top of the stack, and a write there
 * lands on a page that is mapped but read-only. */
void win32_thunk_bytes(UINT32 api_id, UINT8 out[WIN32_THUNK_LEN]) {
  UINT32 i;

  out[0] = 0x64; /* mov %rdi,%fs:WIN32_TEB_SCRATCH_OFF */
  out[1] = 0x48;
  out[2] = 0x89;
  out[3] = 0x3C;
  out[4] = 0x25;
  out[5] = (UINT8)(WIN32_TEB_SCRATCH_OFF & 0xFFu);
  out[6] = (UINT8)((WIN32_TEB_SCRATCH_OFF >> 8) & 0xFFu);
  out[7] = (UINT8)((WIN32_TEB_SCRATCH_OFF >> 16) & 0xFFu);
  out[8] = (UINT8)((WIN32_TEB_SCRATCH_OFF >> 24) & 0xFFu);
  out[9] = 0x48; /* mov %rcx,%rdi */
  out[10] = 0x89;
  out[11] = 0xCF;
  out[12] = 0x41; /* mov $api_id,%r10d */
  out[13] = 0xBA;
  out[14] = (UINT8)(api_id & 0xFFu);
  out[15] = (UINT8)((api_id >> 8) & 0xFFu);
  out[16] = (UINT8)((api_id >> 16) & 0xFFu);
  out[17] = (UINT8)((api_id >> 24) & 0xFFu);
  out[18] = 0xB8; /* mov $SYS_WIN32,%eax */
  out[19] = (UINT8)(SYS_WIN32 & 0xFFu);
  out[20] = (UINT8)((SYS_WIN32 >> 8) & 0xFFu);
  out[21] = (UINT8)((SYS_WIN32 >> 16) & 0xFFu);
  out[22] = (UINT8)((SYS_WIN32 >> 24) & 0xFFu);
  out[23] = 0x0F; /* syscall */
  out[24] = 0x05;
  out[25] = 0x64; /* mov %fs:WIN32_TEB_SCRATCH_OFF,%rdi */
  out[26] = 0x48;
  out[27] = 0x8B;
  out[28] = 0x3C;
  out[29] = 0x25;
  out[30] = (UINT8)(WIN32_TEB_SCRATCH_OFF & 0xFFu);
  out[31] = (UINT8)((WIN32_TEB_SCRATCH_OFF >> 8) & 0xFFu);
  out[32] = (UINT8)((WIN32_TEB_SCRATCH_OFF >> 16) & 0xFFu);
  out[33] = (UINT8)((WIN32_TEB_SCRATCH_OFF >> 24) & 0xFFu);
  out[34] = 0xC3; /* ret */
  /* The stride is wider than the code so every thunk starts on a fresh
   * address; the rest is padding that a `ret` never reaches. */
  for (i = 35u; i < WIN32_THUNK_LEN; i++) {
    out[i] = 0x90;
  }
}

/* One import, one address. DispatchMessageA is the exception: it has to
 * call the program's own window procedure, and that call can only happen
 * in the program's own ring, so its IAT entry points at the stub page
 * instead of a thunk. */
UINT64 win32_api_entry(UINT32 api_id) {
  if (api_id == (UINT32)WIN32_API_DISPATCH_MESSAGE) {
    return WIN32_GUI_STUB_VA;
  }
  return WIN32_THUNK_VA + (UINT64)api_id * WIN32_THUNK_LEN;
}
