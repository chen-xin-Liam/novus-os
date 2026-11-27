## @file
#  Minimal platform DSC for building NovusSplash.efi.
#
#  Copy this file to the root of an EDK II workspace and place the
#  NovusSplash/ directory next to it, then:
#    build -a X64 -t GCC5 -b RELEASE -p NovusSplashPkg.dsc
#  The produced image is Build/NovusSplash/RELEASE_GCC5/X64/NovusLoader.efi.
#
#  The application is also usable as [Components] entry in an existing
#  platform DSC (for example OvmfPkg/OvmfPkgX64.dsc) without this file.
##
[Defines]
  PLATFORM_NAME                  = NovusSplashPkg
  PLATFORM_GUID                  = 2C9E47A8-0B3D-4F61-A5E7-9D8C1B4F6A20
  PLATFORM_VERSION               = 0.10
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/NovusSplash
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf

[Components]
  NovusSplash/NovusSplash.inf
