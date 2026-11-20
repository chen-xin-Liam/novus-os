#ifndef NOVUS_WINDOWS_H
#define NOVUS_WINDOWS_H
typedef void* HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned short WORD;
typedef unsigned char BYTE;
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_ERROR_HANDLE  ((DWORD)-12)
#define FOREGROUND_BLUE      0x0001u
#define FOREGROUND_GREEN     0x0002u
#define FOREGROUND_RED       0x0004u
#define FOREGROUND_INTENSITY 0x0008u
#define BACKGROUND_BLUE      0x0010u
#define BACKGROUND_GREEN     0x0020u
#define BACKGROUND_RED       0x0040u
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
extern "C" {
  HANDLE __stdcall GetStdHandle(DWORD);
  BOOL   __stdcall SetConsoleTextAttribute(HANDLE, WORD);
  DWORD  __stdcall WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
}
#endif
