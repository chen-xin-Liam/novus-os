/**
 * Novus OS - self-developed minimal UEFI definitions (no external dependencies)
 *
 * Hand-written against the UEFI specification. No EDK2 / gnu-efi / host
 * headers are included. Layout targets the x86_64 UEFI environment
 * (Microsoft x64 calling convention, native to the mingw target).
 */

#ifndef NOVUS_UEFI_H
#define NOVUS_UEFI_H

/* ------------------------------------------------------------------ */
/* Basic fixed-width types (self-defined, freestanding)                */
/* ------------------------------------------------------------------ */

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;

typedef signed char        INT8;
typedef short              INT16;
typedef int                INT32;
typedef long long          INT64;

/* x86_64 UEFI: pointer-sized natural types. On the Windows ABI a plain
 * "long" is 32 bits, so "long long" is the 64-bit integer of choice. */
typedef UINT64 UINTN;
typedef INT64  INTN;

typedef char           CHAR8;
typedef unsigned short CHAR16; /* matches -fshort-wchar string literals */

typedef UINT8 BOOLEAN;
#define TRUE  1
#define FALSE 0

#define VOID void

/* The mingw target's default ABI already is Microsoft x64; the attribute
 * is kept explicit so every UEFI entry point is unambiguous. */
#define EFIAPI __attribute__((ms_abi))

/* ------------------------------------------------------------------ */
/* Core UEFI types                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
  UINT32 Data1;
  UINT16 Data2;
  UINT16 Data3;
  UINT8  Data4[8];
} EFI_GUID;

typedef UINTN  EFI_STATUS;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;

typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;
typedef UINTN EFI_TPL;

/* UEFI status encoding: errors have bit 63 set (0x8000...), warnings have
 * bit 62 set, success is 0. Error constants below follow the spec exactly so
 * comparisons against firmware-returned statuses are valid. */
#define EFI_ERROR(Status) ((INT64)(Status) < 0)

#define EFIERR(a)             (0x8000000000000000ULL | (UINT64)(a))

#define EFI_SUCCESS           0
#define EFI_LOAD_ERROR        EFIERR(1)
#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_UNSUPPORTED       EFIERR(3)
#define EFI_BAD_BUFFER_SIZE   EFIERR(4)
#define EFI_BUFFER_TOO_SMALL  EFIERR(5)
#define EFI_NOT_READY         EFIERR(6)
#define EFI_NOT_FOUND         EFIERR(14)

typedef enum {
  EfiReservedMemoryType      = 0,
  EfiLoaderCode              = 1,
  EfiLoaderData              = 2,
  EfiBootServicesCode        = 3,
  EfiBootServicesData        = 4,
  EfiRuntimeServicesCode     = 5,
  EfiRuntimeServicesData     = 6,
  EfiConventionalMemory      = 7,
  EfiUnusableMemory          = 8,
  EfiACPIReclaimMemory       = 9,
  EfiACPIMemoryNVS           = 10,
  EfiMemoryMappedIO          = 11,
  EfiMemoryMappedIOPortSpace = 12,
  EfiPalCode                 = 13,
  EfiPersistentMemory        = 14,
  EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
  AllocateAnyPages   = 0,
  AllocateMaxAddress = 1,
  AllocateAddress    = 2,
  MaxAllocateType    = 3
} EFI_ALLOCATE_TYPE;

typedef enum {
  AllHandles       = 0,
  ByRegisterNotify = 1,
  ByProtocol       = 2
} EFI_LOCATE_SEARCH_TYPE;

typedef enum {
  EFI_NATIVE_INTERFACE = 0
} EFI_INTERFACE_TYPE;

typedef enum {
  TimerCancel   = 0,
  TimerPeriodic = 1,
  TimerRelative = 2,
  TimerTypeMax  = 3
} EFI_TIMER_DELAY;

/* Memory map descriptor. The explicit Pad field keeps PhysicalStart on
 * the 8-byte boundary required by the UEFI-specified x86_64 layout. */
typedef struct {
  UINT32 Type;
  UINT32 Pad;
  UINT64 PhysicalStart;
  UINT64 VirtualStart;
  UINT64 Pages;
  UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ------------------------------------------------------------------ */
/* Common table header                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
  UINT64 Signature;
  UINT32 Revision;
  UINT32 HeaderSize;
  UINT32 CRC32;
  UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ------------------------------------------------------------------ */
/* Simple Text Output protocol                                         */
/* ------------------------------------------------------------------ */

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
  INT32   MaxMode;
  INT32   Mode;
  INT32   Attribute;
  INT32   CursorColumn;
  INT32   CursorRow;
  BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_TEST_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber, UINTN *Columns,
    UINTN *Rows);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_MODE)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);

/* Member order follows the UEFI spec exactly: Reset, OutputString,
 * TestString, QueryMode, SetMode, SetAttribute, ClearScreen,
 * SetCursorPosition, EnableCursor, Mode. */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
  EFI_TEXT_RESET               Reset;
  EFI_TEXT_STRING              OutputString;
  EFI_TEXT_TEST_STRING         TestString;
  EFI_TEXT_QUERY_MODE          QueryMode;
  EFI_TEXT_SET_MODE            SetMode;
  EFI_TEXT_SET_ATTRIBUTE       SetAttribute;
  EFI_TEXT_CLEAR_SCREEN        ClearScreen;
  EFI_TEXT_SET_CURSOR_POSITION SetCursorPosition;
  EFI_TEXT_ENABLE_CURSOR       EnableCursor;
  EFI_SIMPLE_TEXT_OUTPUT_MODE *Mode;
};

/* ------------------------------------------------------------------ */
/* Boot Services table                                                 */
/* ------------------------------------------------------------------ */

typedef struct EFI_DEVICE_PATH_PROTOCOL_ EFI_DEVICE_PATH_PROTOCOL;
typedef struct EFI_OPEN_PROTOCOL_INFORMATION_ENTRY_
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;

typedef VOID (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, VOID *Context);

typedef EFI_TPL (EFIAPI *EFI_RAISE_TPL)(EFI_TPL NewTpl);
typedef VOID (EFIAPI *EFI_RESTORE_TPL)(EFI_TPL OldTpl);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(EFI_ALLOCATE_TYPE Type,
                                                EFI_MEMORY_TYPE MemoryType,
                                                UINTN Pages,
                                                EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory,
                                            UINTN Pages);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
    UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(EFI_MEMORY_TYPE PoolType,
                                               UINTN Size, VOID **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_CREATE_EVENT)(UINT32 Type, EFI_TPL NotifyTpl,
                                              EFI_EVENT_NOTIFY NotifyFunction,
                                              VOID *NotifyContext,
                                              EFI_EVENT *Event);
typedef EFI_STATUS (EFIAPI *EFI_SET_TIMER)(EFI_EVENT Event,
                                           EFI_TIMER_DELAY Type,
                                           UINT64 TriggerTime);
typedef EFI_STATUS (EFIAPI *EFI_WAIT_FOR_EVENT)(UINTN NumberOfEvents,
                                                EFI_EVENT *Event, UINTN *Index);
typedef EFI_STATUS (EFIAPI *EFI_SIGNAL_EVENT)(EFI_EVENT Event);
typedef EFI_STATUS (EFIAPI *EFI_CLOSE_EVENT)(EFI_EVENT Event);
typedef EFI_STATUS (EFIAPI *EFI_CHECK_EVENT)(EFI_EVENT Event);
typedef EFI_STATUS (EFIAPI *EFI_INSTALL_PROTOCOL_INTERFACE)(
    EFI_HANDLE *Handle, EFI_GUID *Protocol, EFI_INTERFACE_TYPE InterfaceType,
    VOID *Interface);
typedef EFI_STATUS (EFIAPI *EFI_REINSTALL_PROTOCOL_INTERFACE)(
    EFI_HANDLE Handle, EFI_GUID *Protocol, VOID *OldInterface,
    VOID *NewInterface);
typedef EFI_STATUS (EFIAPI *EFI_UNINSTALL_PROTOCOL_INTERFACE)(
    EFI_HANDLE Handle, EFI_GUID *Protocol, VOID *Interface);
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE Handle,
                                                 EFI_GUID *Protocol,
                                                 VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_REGISTER_PROTOCOL_NOTIFY)(EFI_GUID *Protocol,
                                                          EFI_EVENT Event,
                                                          VOID **Registration);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE)(EFI_LOCATE_SEARCH_TYPE SearchType,
                                               EFI_GUID *Protocol,
                                               VOID *SearchKey,
                                               UINTN *BufferSize,
                                               EFI_HANDLE *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_DEVICE_PATH)(
    EFI_GUID *Protocol, EFI_DEVICE_PATH_PROTOCOL **DevicePath,
    EFI_HANDLE *Device);
typedef EFI_STATUS (EFIAPI *EFI_INSTALL_CONFIGURATION_TABLE)(EFI_GUID *Guid,
                                                             VOID *Table);
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_LOAD)(BOOLEAN BootPolicy,
                                            EFI_HANDLE ParentImageHandle,
                                            EFI_DEVICE_PATH_PROTOCOL *DevicePath,
                                            VOID *SourceBuffer,
                                            UINTN SourceSize,
                                            EFI_HANDLE *ImageHandle);
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_START)(EFI_HANDLE ImageHandle,
                                             UINTN *ExitDataSize,
                                             CHAR16 **ExitData);
typedef EFI_STATUS (EFIAPI *EFI_EXIT)(EFI_HANDLE ImageHandle,
                                      EFI_STATUS ExitStatus,
                                      UINTN ExitDataSize, CHAR16 *ExitData);
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_UNLOAD)(EFI_HANDLE ImageHandle);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle,
                                                    UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_MONOTONIC_COUNT)(UINT64 *Count);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);
typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(UINTN Timeout,
                                                    UINT64 WatchdogCode,
                                                    UINTN DataSize,
                                                    CHAR16 *WatchdogData);
typedef EFI_STATUS (EFIAPI *EFI_CONNECT_CONTROLLER)(
    EFI_HANDLE ControllerHandle, EFI_HANDLE *DriverImageHandle,
    EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath, BOOLEAN Recursive);
typedef EFI_STATUS (EFIAPI *EFI_DISCONNECT_CONTROLLER)(
    EFI_HANDLE ControllerHandle, EFI_HANDLE DriverImageHandle,
    EFI_HANDLE ChildHandle);
typedef EFI_STATUS (EFIAPI *EFI_OPEN_PROTOCOL)(
    EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface,
    EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle, UINT32 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_CLOSE_PROTOCOL)(EFI_HANDLE Handle,
                                                EFI_GUID *Protocol,
                                                EFI_HANDLE AgentHandle,
                                                EFI_HANDLE ControllerHandle);
typedef EFI_STATUS (EFIAPI *EFI_OPEN_PROTOCOL_INFORMATION)(
    EFI_HANDLE Handle, EFI_GUID *Protocol,
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **EntryBuffer, UINTN *EntryCount);
typedef EFI_STATUS (EFIAPI *EFI_PROTOCOLS_PER_HANDLE)(
    EFI_HANDLE Handle, EFI_GUID ***ProtocolBuffer,
    UINTN *ProtocolBufferCount);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol, VOID *SearchKey,
    UINTN *NumberHandles, EFI_HANDLE **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(EFI_GUID *Protocol,
                                                 VOID *Registration,
                                                 VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES)(
    EFI_HANDLE *Handle, ...);
typedef EFI_STATUS (EFIAPI *EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES)(
    EFI_HANDLE Handle, ...);
typedef EFI_STATUS (EFIAPI *EFI_CALCULATE_CRC32)(VOID *Data, UINTN DataSize,
                                                 UINT32 *Crc32);
typedef VOID (EFIAPI *EFI_COPY_MEM)(VOID *Destination, VOID *Source,
                                    UINTN Length);
typedef VOID (EFIAPI *EFI_SET_MEM)(VOID *Buffer, UINTN Size, UINT8 Value);
typedef EFI_STATUS (EFIAPI *EFI_CREATE_EVENT_EX)(UINT32 Type,
                                                 EFI_TPL NotifyTpl,
                                                 EFI_EVENT_NOTIFY NotifyFunction,
                                                 const VOID *NotifyContext,
                                                 EFI_GUID *EventGroup,
                                                 EFI_EVENT *Event);

/* Member order follows the UEFI Boot Services table exactly. */
typedef struct {
  EFI_TABLE_HEADER Hdr;

  EFI_RAISE_TPL   RaiseTPL;
  EFI_RESTORE_TPL RestoreTPL;

  EFI_ALLOCATE_PAGES AllocatePages;
  EFI_FREE_PAGES     FreePages;
  EFI_GET_MEMORY_MAP GetMemoryMap;
  EFI_ALLOCATE_POOL  AllocatePool;
  EFI_FREE_POOL      FreePool;

  EFI_CREATE_EVENT  CreateEvent;
  EFI_SET_TIMER     SetTimer;
  EFI_WAIT_FOR_EVENT WaitForEvent;
  EFI_SIGNAL_EVENT  SignalEvent;
  EFI_CLOSE_EVENT   CloseEvent;
  EFI_CHECK_EVENT   CheckEvent;

  EFI_INSTALL_PROTOCOL_INTERFACE    InstallProtocolInterface;
  EFI_REINSTALL_PROTOCOL_INTERFACE  ReinstallProtocolInterface;
  EFI_UNINSTALL_PROTOCOL_INTERFACE  UninstallProtocolInterface;
  EFI_HANDLE_PROTOCOL               HandleProtocol;
  VOID                             *Reserved;
  EFI_REGISTER_PROTOCOL_NOTIFY      RegisterProtocolNotify;
  EFI_LOCATE_HANDLE                 LocateHandle;
  EFI_LOCATE_DEVICE_PATH            LocateDevicePath;
  EFI_INSTALL_CONFIGURATION_TABLE   InstallConfigurationTable;

  EFI_IMAGE_LOAD         LoadImage;
  EFI_IMAGE_START        StartImage;
  EFI_EXIT               Exit;
  EFI_IMAGE_UNLOAD       UnloadImage;
  EFI_EXIT_BOOT_SERVICES ExitBootServices;

  EFI_GET_NEXT_MONOTONIC_COUNT GetNextMonotonicCount;
  EFI_STALL                    Stall;
  EFI_SET_WATCHDOG_TIMER       SetWatchdogTimer;

  EFI_CONNECT_CONTROLLER    ConnectController;
  EFI_DISCONNECT_CONTROLLER DisconnectController;

  EFI_OPEN_PROTOCOL            OpenProtocol;
  EFI_CLOSE_PROTOCOL           CloseProtocol;
  EFI_OPEN_PROTOCOL_INFORMATION OpenProtocolInformation;
  EFI_PROTOCOLS_PER_HANDLE      ProtocolsPerHandle;
  EFI_LOCATE_HANDLE_BUFFER      LocateHandleBuffer;
  EFI_LOCATE_PROTOCOL           LocateProtocol;
  EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES   InstallMultipleProtocolInterfaces;
  EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES UninstallMultipleProtocolInterfaces;

  EFI_CALCULATE_CRC32 CalculateCrc32;
  EFI_COPY_MEM        CopyMem;
  EFI_SET_MEM         SetMem;
  EFI_CREATE_EVENT_EX CreateEventEx;
} EFI_BOOT_SERVICES;

/* ------------------------------------------------------------------ */
/* System table                                                        */
/* ------------------------------------------------------------------ */

/* FirmwareRevision (UINT32) is followed by 8-byte-aligned pointers; the
 * implicit 4-byte tail padding matches the UEFI-specified x86_64 layout. */
typedef struct {
  EFI_TABLE_HEADER Hdr;
  CHAR16          *FirmwareVendor;
  UINT32           FirmwareRevision;
  /* UINT32 implicit padding here */

  EFI_HANDLE                       ConsoleInHandle;
  VOID                            *ConIn;
  EFI_HANDLE                       ConsoleOutHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  EFI_HANDLE                       StandardErrorHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;

  VOID              *RuntimeServices;
  EFI_BOOT_SERVICES *BootServices;

  UINTN NumberOfTableEntries;
  VOID *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* NOVUS_UEFI_H */
