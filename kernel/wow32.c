/**
 * Novus OS - WOW32 API gate and stdcall thunks (M13).
 *
 * The 32-bit side is deliberately small: the trap frame is still the
 * kernel's 64-bit one, but EAX selects an API and arguments are read from
 * the 32-bit user stack at [ESP+4], [ESP+8], ... The `ret imm16` in each
 * thunk restores the caller's stdcall stack.
 */

#include <stddef.h>
#include "kernel.h"
#include "sched.h"
#include "mm.h"
#include "vm.h"
#include "wow32.h"
#include "win32.h"

/* Indices are the row positions in g_api[]. Keep the first block in sync
 * with the switch in wow32_syscall(); everything from WOW_FORWARD_FIRST on
 * is handed to the existing Win64 handler with a translated frame. */
enum {
  WOW_EXIT_PROCESS = 0,
  WOW_GET_STD_HANDLE,
  WOW_WRITE_FILE,
  WOW_GET_COMMAND_LINE_A,
  WOW_GET_COMMAND_LINE_W,
  WOW_GET_LAST_ERROR,
  WOW_SET_LAST_ERROR,
  WOW_SLEEP,
  WOW_GET_TICK_COUNT,
  WOW_GET_TICK_COUNT64,
  WOW_GET_MODULE_HANDLE_A,
  WOW_GET_MODULE_HANDLE_W,
  WOW_GET_CURRENT_PROCESS_ID,
  WOW_GET_CURRENT_THREAD_ID,
  WOW_IS_WOW64_PROCESS,
  WOW_MESSAGE_BOX_A,
  WOW_MESSAGE_BOX_W,
  WOW_FORWARD_FIRST
};

/* Indices of rows added for the two installers' full import lists. These are
 * table positions, not Win32 API ids; keep the list in sync with g_api[]. */
enum {
  WOW_ENV_STRINGS = 86,
  WOW_FREE_ENV_STRINGS = 87,
  WOW_GET_FILE_TYPE = 89,
  WOW_RAISE_EXCEPTION = 90,
  WOW_LC_MAP_STRING = 91,
  WOW_HEAP_SIZE = 92,
  WOW_HEAP_REALLOC = 93,
  WOW_CONSOLE_OUTPUT_CP = 95,
  WOW_GET_CONSOLE_MODE = 96,
  WOW_DECODE_POINTER = 99,
  WOW_VIRTUAL_PROTECT = 100,
  WOW_ENCODE_POINTER = 101,
  WOW_LOAD_LIBRARY_EX = 102,
  WOW_TLS_FREE = 104,
  WOW_TLS_SET = 105,
  WOW_TLS_GET = 106,
  WOW_TLS_ALLOC = 107,
  WOW_INIT_CRIT_SECTION = 108,
  WOW_LEAVE_CRIT_SECTION = 110,
  WOW_ENTER_CRIT_SECTION = 111,
  WOW_PROCESSOR_FEATURE = 113,
  WOW_GET_STARTUP_INFO = 114,
  WOW_UNHANDLED_EXCEPTION = 115,
  WOW_RTL_UNWIND = 116,
  WOW_FILETIME = 118,
  WOW_STRING_TYPE = 119,
  WOW_FORMAT_MESSAGE = 123,
  WOW_FILE_ATTR_EX = 125,
  WOW_DISK_FREE_SPACE = 126,
  WOW_TEMP_FILE_NAME = 135,
  WOW_WAIT_SINGLE_OBJECT = 138,
  WOW_CREATE_THREAD = 142,
  WOW_VIRTUAL_QUERY = 146,
  WOW_CREATE_PROCESS = 147
};

typedef struct {
  const char *dll;
  const char *fn;
  UINT8       nargs;
  UINT8       direct;
} wow_api_t;

/* The stdcall byte count is part of the ABI: a wrong value corrupts the
 * caller's stack after the call, so each row states it explicitly. */
static const wow_api_t g_api[] = {
  {"kernel32.dll", "ExitProcess",             1, 1},
  {"kernel32.dll", "GetStdHandle",            1, 1},
  {"kernel32.dll", "WriteFile",               5, 1},
  {"kernel32.dll", "GetCommandLineA",         0, 1},
  {"kernel32.dll", "GetCommandLineW",         0, 1},
  {"kernel32.dll", "GetLastError",            0, 1},
  {"kernel32.dll", "SetLastError",            1, 1},
  {"kernel32.dll", "Sleep",                   1, 1},
  {"kernel32.dll", "GetTickCount",            0, 1},
  {"kernel32.dll", "GetTickCount64",          0, 1},
  {"kernel32.dll", "GetModuleHandleA",        1, 1},
  {"kernel32.dll", "GetModuleHandleW",        1, 1},
  {"kernel32.dll", "GetCurrentProcessId",     0, 1},
  {"kernel32.dll", "GetCurrentThreadId",      0, 1},
  {"kernel32.dll", "IsWow64Process",          2, 1},
  {"user32.dll",   "MessageBoxA",             4, 1},
  {"user32.dll",   "MessageBoxW",             4, 1},
  /* Forwarded to the existing Win32 layer (simple scalar arguments only). */
  {"kernel32.dll", "WriteConsoleA",           5, 0},
  {"kernel32.dll", "ReadFile",                5, 0},
  {"kernel32.dll", "CloseHandle",             1, 0},
  {"kernel32.dll", "CreateFileA",             7, 0},
  {"kernel32.dll", "CreateFileW",             7, 0},
  {"kernel32.dll", "GetFileSize",             2, 0},
  {"kernel32.dll", "GetFileAttributesA",      1, 0},
  {"kernel32.dll", "GetFileAttributesW",      1, 0},
  {"kernel32.dll", "DeleteFileA",             1, 0},
  {"kernel32.dll", "DeleteFileW",             1, 0},
  {"kernel32.dll", "CreateDirectoryA",        2, 0},
  {"kernel32.dll", "CreateDirectoryW",        2, 0},
  {"kernel32.dll", "FindFirstFileA",          2, 0},
  {"kernel32.dll", "FindFirstFileW",          2, 0},
  {"kernel32.dll", "FindNextFileA",           2, 0},
  {"kernel32.dll", "FindNextFileW",           2, 0},
  {"kernel32.dll", "FindClose",               1, 0},
  {"kernel32.dll", "GetTempPathA",            2, 0},
  {"kernel32.dll", "GetTempPathW",            2, 0},
  {"kernel32.dll", "GetLongPathNameA",        3, 0},
  {"kernel32.dll", "GetLongPathNameW",        3, 0},
  {"kernel32.dll", "MultiByteToWideChar",     6, 0},
  {"kernel32.dll", "WideCharToMultiByte",     6, 0},
  {"kernel32.dll", "GetACP",                  0, 0},
  {"kernel32.dll", "GetOEMCP",                0, 0},
  {"kernel32.dll", "IsValidCodePage",         1, 0},
  {"kernel32.dll", "GetCPInfo",               2, 0},
  {"kernel32.dll", "GetModuleFileNameA",      3, 0},
  {"kernel32.dll", "GetModuleFileNameW",      3, 0},
  {"kernel32.dll", "LoadLibraryA",            1, 1},
  {"kernel32.dll", "LoadLibraryW",            1, 1},
  {"kernel32.dll", "GetProcAddress",          2, 1},
  {"kernel32.dll", "FreeLibrary",             1, 0},
  {"kernel32.dll", "VirtualAlloc",            4, 0},
  {"kernel32.dll", "VirtualFree",             3, 0},
  {"kernel32.dll", "GetProcessHeap",          0, 0},
  {"kernel32.dll", "HeapAlloc",               3, 0},
  {"kernel32.dll", "HeapFree",                3, 0},
  {"kernel32.dll", "GetSystemTime",           1, 0},
  {"kernel32.dll", "GetLocalTime",            1, 0},
  {"kernel32.dll", "QueryPerformanceCounter", 1, 0},
  {"kernel32.dll", "QueryPerformanceFrequency", 1, 0},
  {"kernel32.dll", "GetSystemInfo",           1, 0},
  {"kernel32.dll", "IsDebuggerPresent",       0, 0},
  {"kernel32.dll", "GetVersion",              0, 0},
  {"kernel32.dll", "GetVersionExA",           1, 0},
  {"kernel32.dll", "OutputDebugStringA",      1, 0},
  {"kernel32.dll", "OutputDebugStringW",      1, 0},
  {"advapi32.dll", "RegOpenKeyExA",           5, 0},
  {"advapi32.dll", "RegOpenKeyExW",           5, 0},
  {"advapi32.dll", "RegQueryValueExA",        6, 0},
  {"advapi32.dll", "RegQueryValueExW",        6, 0},
  {"advapi32.dll", "RegSetValueExA",          6, 0},
  {"advapi32.dll", "RegSetValueExW",          6, 0},
  {"advapi32.dll", "RegCloseKey",             1, 0},
  {"shell32.dll",  "SHGetFolderPathA",        5, 0},
  {"shell32.dll",  "SHGetFolderPathW",        5, 0},
  {"shell32.dll",  "SHGetSpecialFolderPathA", 4, 0},
  {"shell32.dll",  "SHGetSpecialFolderPathW", 4, 0},
  {"shell32.dll",  "SHCreateDirectoryExW",    3, 0},
  {"shell32.dll",  "SHFileOperationA",        1, 0},
  {"shell32.dll",  "SHFileOperationW",        1, 0},
  {"shell32.dll",  "CommandLineToArgvW",      2, 0},
  {"ole32.dll",    "CoInitializeEx",          2, 0},
  {"ole32.dll",    "CoUninitialize",          0, 0},
  {"ole32.dll",    "CoTaskMemFree",           1, 0},
  {"user32.dll",   "CharLowerBuffW",          2, 0},
  /* The full import surface of the two bundled installers. Entries the
   * executor cannot implement yet still need the right stdcall byte count:
   * a wrong `ret imm16` corrupts the caller even if the function is a stub. */
  {"kernel32.dll", "GetModuleHandleExW",       3, 0},
  {"kernel32.dll", "FindFirstFileExW",         6, 0},
  {"kernel32.dll", "GetEnvironmentStringsW",   0, 0},
  {"kernel32.dll", "FreeEnvironmentStringsW",  1, 0},
  {"kernel32.dll", "SetStdHandle",             2, 0},
  {"kernel32.dll", "GetFileType",              1, 0},
  {"kernel32.dll", "RaiseException",           4, 0},
  {"kernel32.dll", "LCMapStringW",             6, 0},
  {"kernel32.dll", "HeapSize",                 3, 0},
  {"kernel32.dll", "HeapReAlloc",              4, 0},
  {"kernel32.dll", "FlushFileBuffers",         1, 0},
  {"kernel32.dll", "GetConsoleOutputCP",       0, 0},
  {"kernel32.dll", "GetConsoleMode",           2, 0},
  {"kernel32.dll", "SetFilePointerEx",         4, 0},
  {"kernel32.dll", "WriteConsoleW",            5, 0},
  {"kernel32.dll", "DecodePointer",            1, 0},
  {"kernel32.dll", "VirtualProtect",           4, 0},
  {"kernel32.dll", "EncodePointer",            1, 0},
  {"kernel32.dll", "LoadLibraryExW",           3, 0},
  {"kernel32.dll", "LoadLibraryExA",           3, 0},
  {"kernel32.dll", "TlsFree",                  1, 0},
  {"kernel32.dll", "TlsSetValue",              2, 0},
  {"kernel32.dll", "TlsGetValue",              1, 0},
  {"kernel32.dll", "TlsAlloc",                 0, 0},
  {"kernel32.dll", "InitializeCriticalSectionAndSpinCount", 2, 0},
  {"kernel32.dll", "DeleteCriticalSection",    1, 0},
  {"kernel32.dll", "LeaveCriticalSection",     1, 0},
  {"kernel32.dll", "EnterCriticalSection",     1, 0},
  {"kernel32.dll", "TerminateProcess",         2, 0},
  {"kernel32.dll", "IsProcessorFeaturePresent", 1, 0},
  {"kernel32.dll", "GetStartupInfoW",          1, 0},
  {"kernel32.dll", "UnhandledExceptionFilter", 1, 0},
  {"kernel32.dll", "RtlUnwind",                4, 0},
  {"kernel32.dll", "InitializeSListHead",      1, 0},
  {"kernel32.dll", "GetSystemTimeAsFileTime",  1, 0},
  {"kernel32.dll", "GetStringTypeW",           3, 0},
  {"kernel32.dll", "SizeofResource",           2, 0},
  {"kernel32.dll", "Wow64DisableWow64FsRedirection", 1, 0},
  {"kernel32.dll", "RemoveDirectoryW",         1, 0},
  {"kernel32.dll", "FormatMessageW",           5, 0},
  {"kernel32.dll", "Wow64RevertWow64FsRedirection", 1, 0},
  {"kernel32.dll", "GetFileAttributesExW",     3, 0},
  {"kernel32.dll", "GetDiskFreeSpaceExW",      4, 0},
  {"kernel32.dll", "FindResourceExW",          4, 0},
  {"kernel32.dll", "LoadResource",             2, 0},
  {"kernel32.dll", "FindResourceW",            3, 0},
  {"kernel32.dll", "HeapDestroy",              1, 0},
  {"kernel32.dll", "LocalFree",                1, 0},
  {"kernel32.dll", "VerSetConditionMask",      3, 0},
  {"kernel32.dll", "CopyFileW",                3, 0},
  {"kernel32.dll", "VerifyVersionInfoW",       4, 0},
  {"kernel32.dll", "GetTempFileNameW",         4, 0},
  {"kernel32.dll", "lstrcmpiW",                2, 0},
  {"kernel32.dll", "CreateMutexW",             3, 0},
  {"kernel32.dll", "WaitForSingleObject",      2, 0},
  {"kernel32.dll", "ReleaseMutex",             1, 0},
  {"kernel32.dll", "CreateEventW",             4, 0},
  {"kernel32.dll", "SetEvent",                 1, 0},
  {"kernel32.dll", "CreateThread",             6, 0},
  {"kernel32.dll", "UnmapViewOfFile",          1, 0},
  {"kernel32.dll", "CreateFileMappingW",       6, 0},
  {"kernel32.dll", "MapViewOfFile",            4, 0},
  {"kernel32.dll", "VirtualQuery",             3, 0},
  {"kernel32.dll", "CreateProcessW",          10, 0},
  {"kernel32.dll", "GetExitCodeProcess",       2, 0},
  {"kernel32.dll", "ResetEvent",               1, 0},
  {"kernel32.dll", "WaitForSingleObjectEx",    3, 0},
  {"kernel32.dll", "GetFileSizeEx",            2, 0},
  {"kernel32.dll", "FreeResource",             1, 0},
  {"advapi32.dll", "SetSecurityDescriptorDacl", 4, 0},
  {"advapi32.dll", "GetAclInformation",        4, 0},
  {"advapi32.dll", "SetSecurityDescriptorOwner", 2, 0},
  {"advapi32.dll", "GetSidSubAuthority",       2, 0},
  {"advapi32.dll", "GetSidLengthRequired",     1, 0},
  {"advapi32.dll", "CopySid",                  3, 0},
  {"advapi32.dll", "InitializeSid",            3, 0},
  {"advapi32.dll", "IsValidSid",               1, 0},
  {"advapi32.dll", "AddAce",                   5, 0},
  {"advapi32.dll", "InitializeSecurityDescriptor", 2, 0},
  {"advapi32.dll", "InitializeAcl",            3, 0},
  {"advapi32.dll", "GetLengthSid",             1, 0},
  {"advapi32.dll", "GetSecurityDescriptorLength", 1, 0},
  {"advapi32.dll", "MakeSelfRelativeSD",       3, 0},
  {"advapi32.dll", "MakeAbsoluteSD",           9, 0},
  {"advapi32.dll", "SetSecurityDescriptorGroup", 2, 0},
  {"advapi32.dll", "GetSecurityDescriptorSacl", 4, 0},
  {"advapi32.dll", "GetSecurityDescriptorDacl", 4, 0},
  {"advapi32.dll", "GetSecurityDescriptorGroup", 3, 0},
  {"advapi32.dll", "GetSecurityDescriptorControl", 3, 0},
  {"advapi32.dll", "GetSecurityDescriptorOwner", 3, 0},
  {"advapi32.dll", "RegCreateKeyExA",          8, 0},
  {"advapi32.dll", "RegDeleteValueA",          2, 0},
  {"shell32.dll",  "SHGetKnownFolderPath",     4, 0},
  {"kernel32.dll", "GetCurrentProcess",        0, 1},
  {"kernel32.dll", "SetUnhandledExceptionFilter", 1, 1},
  {"kernel32.dll", "LockResource",             1, 1},
  {"kernel32.dll", "InitializeCriticalSectionEx", 3, 1},
};

enum {
  WOW_LOAD_LIBRARY_A = 46,
  WOW_LOAD_LIBRARY_W = 47,
  WOW_GET_PROC_ADDRESS = 48,
  WOW_KNOWN_FOLDER_PATH = 176,
  WOW_GET_CURRENT_PROCESS = 177,
  WOW_SET_UNHANDLED_FILTER = 178,
  WOW_LOCK_RESOURCE = 179,
  WOW_INIT_CRIT_SECTION_EX = 180
};

/* Win32 API ids for the forwarded rows, filled when the import is resolved.
 * Direct rows leave this zero; they never reach win32_syscall(). */
static UINT32 g_ids[WOW32_MAX_APIS];

static int name_is(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) { a++; b++; }
  return *a == '\0' && *b == '\0';
}

/* DLL names are case-insensitive in Windows; the table uses lowercase. */
static int dll_is(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return 0;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

int wow32_api_lookup(const char *dll, const char *func, UINT32 *nargs,
                     UINT32 *win32_id, UINT8 *direct) {
  UINT32 i;

  for (i = 0; i < sizeof(g_api) / sizeof(g_api[0]); i++) {
    if (!dll_is(g_api[i].dll, dll) || !name_is(g_api[i].fn, func)) continue;
    if (nargs != NULL) *nargs = g_api[i].nargs;
    if (direct != NULL) *direct = g_api[i].direct;
    if (win32_id != NULL) {
      int id = win32_api_lookup(g_api[i].dll, g_api[i].fn);
      g_ids[i] = (id >= 0) ? (UINT32)id : 0xFFFFFFFFu;
      *win32_id = g_ids[i];
    }
    return (int)i;
  }
  return -1;
}

UINT64 wow32_api_entry(UINT32 api) {
  return WOW32_THUNK_VA + (UINT64)api * WOW32_THUNK_LEN;
}

void wow32_thunk_bytes(UINT32 api, UINT32 nargs, UINT8 out[WOW32_THUNK_LEN]) {
  UINT32 i;
  UINT16 bytes = (UINT16)(nargs * 4u);

  out[0] = 0xB8u; /* mov eax, imm32 */
  out[1] = (UINT8)(api & 0xFFu);
  out[2] = (UINT8)((api >> 8) & 0xFFu);
  out[3] = (UINT8)((api >> 16) & 0xFFu);
  out[4] = (UINT8)((api >> 24) & 0xFFu);
  out[5] = 0xCDu; /* int 0x80 */
  out[6] = 0x80u;
  out[7] = 0xC2u; /* ret imm16 */
  out[8] = (UINT8)(bytes & 0xFFu);
  out[9] = (UINT8)((bytes >> 8) & 0xFFu);
  for (i = 10; i < WOW32_THUNK_LEN; i++) out[i] = 0x90u; /* nop padding */
}

/* Read a DWORD from the 32-bit user stack through the current address space. */
static int wow_u32(task_t *t, UINT64 va, UINT32 *out) {
  UINT64 page = va & ~0xFFFULL;
  UINT64 off = va & 0xFFFULL;
  UINT64 phys;

  if (off + 4u > 4096u) return -1; /* a dword never straddles a stack slot */
  phys = vm_translate(&t->space, page);
  if (phys == 0) return -1;
  *out = *(UINT32 *)(void *)(phys_to_virt(phys) + off);
  return 0;
}

static int wow_u8(task_t *t, UINT64 va, UINT8 *out) {
  UINT64 page = va & ~0xFFFULL;
  UINT64 off = va & 0xFFFULL;
  UINT64 phys = vm_translate(&t->space, page);

  if (phys == 0) return -1;
  *out = *(UINT8 *)(void *)(phys_to_virt(phys) + off);
  return 0;
}

static void wow_put16(task_t *t, UINT32 va, UINT16 value) {
  UINT64 page = (UINT64)va & ~0xFFFULL;
  UINT64 off = (UINT64)va & 0xFFFULL;
  UINT64 phys = vm_translate(&t->space, page);

  if (phys != 0u && off + 2u <= 4096u) {
    *(UINT16 *)(void *)(phys_to_virt(phys) + off) = value;
  }
}

static void wow_zero(task_t *t, UINT32 va, UINT32 len) {
  UINT32 i;
  for (i = 0u; i < len; i++) wow_put16(t, va + i, 0u);
}

/* Argument 1 is at ESP+4: `int 0x80` does not push anything on the user
 * stack, but the `call thunk` that reached it pushed the return address. */
static int wow_arg(task_t *t, trap_frame_t *f, UINT32 n, UINT32 *out) {
  return wow_u32(t, (f->rsp & 0xFFFFFFFFULL) + 4u + n * 4u, out);
}

static void wow_copy_out(task_t *t, UINT32 va, UINT32 value) {
  UINT64 page = (UINT64)va & ~0xFFFULL;
  UINT64 off = (UINT64)va & 0xFFFULL;
  UINT64 phys = vm_translate(&t->space, page);

  if (phys != 0 && off + 4u <= 4096u) {
    *(UINT32 *)(void *)(phys_to_virt(phys) + off) = value;
  }
}

void wow32_syscall(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT32 api;
  UINT32 a[8];
  UINT32 i;

  if (t == NULL || t->abi != TASK_ABI_WOW32) {
    f->rax = (UINT64)-1;
    return;
  }
  api = (UINT32)f->rax;
  if (api >= WOW32_MAX_APIS || api >= sizeof(g_api) / sizeof(g_api[0])) {
    kputs("[WOW32] unknown API ");
    kputs_hex(api);
    kputs("\n");
    f->rax = (UINT64)(UINT32)-1;
    return;
  }
  for (i = 0; i < g_api[api].nargs && i < 8; i++) {
    if (wow_arg(t, f, i, &a[i]) != 0) {
      kputs("[WOW32] bad stack for API ");
      kputs_hex(api);
      kputs("\n");
      f->rax = (UINT64)(UINT32)-1;
      return;
    }
  }

  switch (api) {
  case WOW_EXIT_PROCESS:
    sched_exit_current((int)(INT32)a[0]);
    f->rax = 0;
    return;
  case WOW_GET_STD_HANDLE:
    f->rax = (UINT64)a[0]; /* STD_OUTPUT_HANDLE == (HANDLE)-11 */
    return;
  case WOW_WRITE_FILE: {
    static UINT8 text[513];
    UINT32 len = a[2];
    UINT32 n = (len < sizeof(text) - 1u) ? len : (UINT32)sizeof(text) - 1u;

    for (i = 0; i < n; i++) {
      if (wow_u8(t, (UINT64)a[1] + i, &text[i]) != 0) { n = i; break; }
    }
    text[n] = 0;
    if (n != 0u) (void)kputs((const char *)text);
    if (a[3] != 0u) wow_copy_out(t, a[3], n);
    f->rax = 1u;
    return;
  }
  case WOW_GET_COMMAND_LINE_A:
  case WOW_GET_COMMAND_LINE_W:
    f->rax = (UINT64)(UINT32)WOW32_ARGV_VA;
    return;
  case WOW_GET_LAST_ERROR:
    f->rax = (UINT64)t->win32_last_error;
    return;
  case WOW_SET_LAST_ERROR:
    t->win32_last_error = a[0];
    f->rax = 0;
    return;
  case WOW_SLEEP:
    f->rax = 0;
    return;
  case WOW_GET_TICK_COUNT:
  case WOW_GET_TICK_COUNT64:
    f->rax = (UINT64)t->ticks;
    return;
  case WOW_GET_MODULE_HANDLE_A:
  case WOW_GET_MODULE_HANDLE_W:
    f->rax = (a[0] == 0u) ? (UINT64)(UINT32)t->win32_image_base : 0u;
    return;
  case WOW_GET_CURRENT_PROCESS_ID:
    f->rax = (UINT64)t->pid;
    return;
  case WOW_GET_CURRENT_THREAD_ID:
    f->rax = (UINT64)t->pid;
    return;
  case WOW_IS_WOW64_PROCESS:
    if (a[0] != 0u) wow_copy_out(t, a[0], 1u); /* yes: 32-bit on x64 */
    f->rax = 1u;
    return;
  case WOW_MESSAGE_BOX_A:
  case WOW_MESSAGE_BOX_W:
    kputs("[WOW32] MessageBox: ");
    if (a[1] != 0u) {
      static UINT8 cap[129];
      UINT32 n = 0;
      while (n < sizeof(cap) - 1u && wow_u8(t, (UINT64)a[1] + n, &cap[n]) == 0 &&
             cap[n] != 0u) n++;
      cap[n] = 0;
      (void)kputs((const char *)cap);
    }
    kputs("\n");
    f->rax = 1u; /* IDOK */
    return;
  case WOW_ENV_STRINGS: {
    /* A double-NUL-terminated UTF-16 environment block in the argv page. */
    static const UINT16 env[] = {0x0050,0x0041,0x0054,0x0048,0x003D,0x0043,
                                 0x003A,0x005C,0x0000,0x0000};
    UINT32 base = (UINT32)WOW32_ARGV_VA + 0x100u, k;
    for (k = 0u; k < sizeof(env) / sizeof(env[0]); k++) {
      wow_put16(t, base + k * 2u, env[k]);
    }
    f->rax = base;
    return;
  }
  case WOW_FREE_ENV_STRINGS:
    f->rax = 1u;
    return;
  case WOW_GET_FILE_TYPE:
    f->rax = 1u; /* FILE_TYPE_DISK */
    return;
  case WOW_RAISE_EXCEPTION:
  case WOW_RTL_UNWIND:
  case WOW_UNHANDLED_EXCEPTION:
  case WOW_FORMAT_MESSAGE:
    f->rax = 0u;
    return;
  case WOW_LC_MAP_STRING: {
    UINT32 n = a[3], cap = a[5], k;
    if (n > cap) n = cap;
    for (k = 0u; k < n; k++) {
      UINT16 c = 0u;
      UINT64 page = ((UINT64)a[2] + k * 2u) & ~0xFFFULL;
      UINT64 off = ((UINT64)a[2] + k * 2u) & 0xFFFULL;
      UINT64 phys = vm_translate(&t->space, page);
      if (phys != 0u) c = *(UINT16 *)(void *)(phys_to_virt(phys) + off);
      wow_put16(t, a[4] + k * 2u, c);
    }
    f->rax = n;
    return;
  }
  case WOW_HEAP_SIZE:
    f->rax = a[2];
    return;
  case WOW_HEAP_REALLOC:
    f->rax = a[1]; /* keep the original block valid */
    return;
  case WOW_CONSOLE_OUTPUT_CP:
    f->rax = 65001u; /* UTF-8 */
    return;
  case WOW_GET_CONSOLE_MODE:
    if (a[1] != 0u) wow_copy_out(t, a[1], 0u);
    f->rax = 1u;
    return;
  case WOW_DECODE_POINTER:
  case WOW_ENCODE_POINTER:
    f->rax = a[0];
    return;
  case WOW_VIRTUAL_PROTECT:
    if (a[2] != 0u) wow_copy_out(t, a[2], 0x40u);
    f->rax = 1u;
    return;
  case WOW_LOAD_LIBRARY_EX:
    f->rax = (UINT32)t->win32_image_base;
    return;
  case WOW_TLS_ALLOC: {
    UINT32 k;
    for (k = 1u; k < 64u; k++) {
      if (t->wow_tls[k] == 0u) {
        t->wow_tls[k] = 1u; /* allocated marker */
        f->rax = k;
        return;
      }
    }
    f->rax = (UINT32)-1;
    return;
  }
  case WOW_TLS_SET:
    t->wow_tls[a[0] & 63u] = a[1];
    f->rax = 1u;
    return;
  case WOW_TLS_GET:
    f->rax = (UINT64)t->wow_tls[a[0] & 63u];
    return;
  case WOW_TLS_FREE:
    t->wow_tls[a[0] & 63u] = 0u;
    f->rax = 1u;
    return;
  case WOW_INIT_CRIT_SECTION:
    wow_zero(t, a[0], 8u);
    f->rax = 1u;
    return;
  case WOW_ENTER_CRIT_SECTION:
  case WOW_LEAVE_CRIT_SECTION:
    f->rax = 0u;
    return;
  case WOW_PROCESSOR_FEATURE:
    f->rax = 1u;
    return;
  case WOW_GET_STARTUP_INFO:
    wow_zero(t, a[0], 104u);
    f->rax = 0u;
    return;
  case WOW_FILETIME:
    if (a[0] != 0u) {
      UINT64 page = (UINT64)a[0] & ~0xFFFULL;
      UINT64 off = (UINT64)a[0] & 0xFFFULL;
      UINT64 phys = vm_translate(&t->space, page);
      UINT64 now = (UINT64)t->ticks * 10000u;
      if (phys != 0u && off + 8u <= 4096u) {
        *(UINT64 *)(void *)(phys_to_virt(phys) + off) = now;
      }
    }
    f->rax = 0u;
    return;
  case WOW_STRING_TYPE:
    if (a[2] != 0u) {
      UINT32 k;
      for (k = 0u; k < 4u; k++) wow_copy_out(t, a[2] + k * 4u, 1u);
    }
    f->rax = 1u;
    return;
  case WOW_FILE_ATTR_EX:
    f->rax = (UINT32)-1;
    return;
  case WOW_DISK_FREE_SPACE:
    if (a[1] != 0u) wow_copy_out(t, a[1], 0u);
    if (a[2] != 0u) wow_copy_out(t, a[2], 0u);
    if (a[3] != 0u) wow_copy_out(t, a[3], 0u);
    f->rax = 1u;
    return;
  case WOW_TEMP_FILE_NAME:
    if (a[0] != 0u) {
      static const UINT16 tmp[] = {0x0074,0x006D,0x0070,0x0000};
      UINT32 k;
      for (k = 0u; k < sizeof(tmp) / sizeof(tmp[0]); k++) {
        wow_put16(t, a[0] + k * 2u, tmp[k]);
      }
    }
    f->rax = 1u;
    return;
  case WOW_LOAD_LIBRARY_A:
  case WOW_LOAD_LIBRARY_W:
    f->rax = (UINT32)t->win32_image_base;
    return;
  case WOW_GET_PROC_ADDRESS: {
    char name[64];
    UINT32 k = 0u;
    int api = -1;

    if (a[1] != 0u) {
      while (k + 1u < sizeof(name)) {
        UINT8 c = 0u;
        UINT64 page = ((UINT64)a[1] + k) & ~0xFFFULL;
        UINT64 off = ((UINT64)a[1] + k) & 0xFFFULL;
        UINT64 phys = vm_translate(&t->space, page);
        if (phys == 0u) break;
        c = *(UINT8 *)(void *)(phys_to_virt(phys) + off);
        name[k] = (char)c;
        k++;
        if (c == 0u) break;
      }
      name[k] = '\0';
      api = wow32_api_lookup("kernel32.dll", name, NULL, NULL, NULL);
    }
    /* Never hand a null function pointer back: a caller that ignores the
     * failure would jump to address 0. Point it at the harmless stub. */
    f->rax = (api >= 0) ? (UINT32)wow32_api_entry((UINT32)api)
                        : (UINT32)wow32_api_entry(WOW32_MAX_APIS - 1u);
    return;
  }
  case WOW_KNOWN_FOLDER_PATH:
    if (a[3] != 0u) wow_copy_out(t, a[3], (UINT32)WOW32_ARGV_VA + 0x200u);
    f->rax = 0u;
    return;
  case WOW_GET_CURRENT_PROCESS:
    f->rax = (UINT32)-1; /* pseudo handle */
    return;
  case WOW_SET_UNHANDLED_FILTER:
    f->rax = 0u;
    return;
  case WOW_LOCK_RESOURCE:
    f->rax = a[0];
    return;
  case WOW_INIT_CRIT_SECTION_EX:
    wow_zero(t, a[0], 8u);
    f->rax = 1u;
    return;
  case WOW_WAIT_SINGLE_OBJECT:
    f->rax = 0u; /* WAIT_OBJECT_0 */
    return;
  case WOW_CREATE_THREAD:
  case WOW_CREATE_PROCESS:
  case WOW_VIRTUAL_QUERY:
    t->win32_last_error = 50u; /* ERROR_NOT_SUPPORTED */
    f->rax = 0u;
    return;
  default: {
    /* Translate the scalar stdcall arguments into a Win64-looking frame and
     * let the existing compatibility layer answer the call. */
    trap_frame_t tf;
    UINT32 id = g_ids[api];

    memset(&tf, 0, sizeof(tf));
    tf.rdi = (UINT64)a[0];
    tf.rsi = (UINT64)a[1];
    tf.rdx = (UINT64)a[2];
    tf.r8  = (UINT64)a[3];
    tf.r9  = (UINT64)a[4];
    tf.r10 = (UINT64)id;
    tf.rsp = f->rsp;
    win32_syscall(&tf);
    f->rax = (UINT64)(UINT32)tf.rax;
    return;
  }
  }
}
