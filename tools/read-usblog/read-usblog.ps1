# Read the boot log Novus OS leaves behind, without a serial port.
#
# Two places the kernel can write it (kernel/usblog.c):
#   * the stick it booted from - header at LBA 51200, up to 255 sectors of
#     text after it (USBLOG_HEADER_LBA / USBLOG_DATA_LBA), inside the 64 MiB
#     live image, so it survives a reboot;
#   * the machine's own disk's GPT gap - header at LBA 64, text from LBA 65
#     (DISKLOG_HEADER_LBA / DISKLOG_DATA_LBA). This is the one that works when
#     USB never comes up at all, which is exactly the state this laptop is in.
#
# Usage:
#   .\read-usblog.ps1 -Image C:\novus\out\novus-desktop.img
#   .\read-usblog.ps1 -Disk 4                 # stick, needs admin
#   .\read-usblog.ps1 -Disk 0 -Layout disk    # internal NVMe gap, needs admin
#
[CmdletBinding(DefaultParameterSetName = 'image')]
param(
    [Parameter(ParameterSetName = 'image', Mandatory = $true)]
    [string] $Image,

    [Parameter(ParameterSetName = 'disk', Mandatory = $true)]
    [int] $Disk,

    # 'usb' = the stick band (LBA 51200), 'disk' = the GPT-gap band (LBA 64).
    [ValidateSet('usb', 'disk')]
    [string] $Layout = 'usb',

    [string] $OutFile
)

$sector = 512
$sectors = 255
if ($Layout -eq 'disk') {
    $headerLba = 64
    $dataLba = 65
} else {
    $headerLba = 51200
    $dataLba = 51201
}

$path = if ($PSCmdlet.ParameterSetName -eq 'image') {
    (Resolve-Path -LiteralPath $Image).Path
} else {
    "\\.\PhysicalDrive$Disk"
}

$fs = $null
try {
    # ReadWrite sharing: Windows otherwise refuses to open a disk a browser or
    # the shell has open.
    $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')

    $hdr = New-Object byte[] $sector
    $fs.Seek($headerLba * $sector, [System.IO.SeekOrigin]::Begin) | Out-Null
    $hread = $fs.Read($hdr, 0, $sector)
    if ($hread -lt $sector -or
        [System.Text.Encoding]::ASCII.GetString($hdr) -notmatch 'NOVUS boot log') {
        Write-Host "no Novus boot log at LBA $headerLba ($Layout band; this medium was never written by the kernel)"
        exit 1
    }
    $hdrText = [System.Text.Encoding]::ASCII.GetString($hdr)
    $hdrText = ($hdrText -split "`0")[0]

    $buf = New-Object byte[] ($sectors * $sector)
    $fs.Seek($dataLba * $sector, [System.IO.SeekOrigin]::Begin) | Out-Null
    $read = $fs.Read($buf, 0, $buf.Length)
    if ($read -le 0) {
        throw "nothing readable at LBA $dataLba"
    }

    $log = [System.Text.Encoding]::ASCII.GetString($buf, 0, $read)
    # An all-zero sector ends the log (the kernel zero-pads the tail).
    $cut = [regex]::Match($log, '(\x00){512}')
    if ($cut.Success) { $log = $log.Substring(0, $cut.Index) }
    $log = $log.TrimEnd([char]0)

    $text = "HEADER(LBA $headerLba): $hdrText`r`n`r`n" + $log

    if ($OutFile) {
        Set-Content -LiteralPath $OutFile -Value $text -Encoding utf8
        Write-Host "boot log -> $OutFile ($($log.Length) bytes of text)"
    } else {
        $text
    }
}
finally {
    if ($fs) { $fs.Dispose() }
}
