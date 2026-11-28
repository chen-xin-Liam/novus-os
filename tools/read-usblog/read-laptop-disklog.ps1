# Read the Novus boot log off the machine that actually ran it.
#
# Run this ON THE LAPTOP (the HP EliteBook), in Windows, as Administrator:
#   powershell -ExecutionPolicy Bypass -File read-laptop-disklog.ps1
#
# Novus writes its kernel log into the GPT gap of the machine's own disk:
# header at LBA 64, text from LBA 65 (kernel/usblog.c, DISKLOG_*). When USB
# never enumerates that is the only copy that exists, so this is the way to get
# it back without photographing the screen. It also looks at the stick band
# (LBA 51200) in case the stick was written after all.
#
# Everything it prints is worth keeping: if no band is found, the "probe" lines
# below say which drives existed at all.

$sector = 512
$bands = @(
    @{ Name = 'disk-gap';   Header = 64;    Data = 65;    Limit = 255 },
    @{ Name = 'stick';      Header = 51200; Data = 51201; Limit = 255 },
    # An earlier build put the disk log at LBA 41; if that write ever survived
    # on this machine it is still the best evidence there is, from the build
    # whose diagnostic page photo we already have.
    @{ Name = 'legacy-41';  Header = 41;    Data = 42;    Limit = 255 }
)

function Show-Bytes {
    param([byte[]] $b, [int] $n)
    $max = [Math]::Min($n, $b.Length)
    $hex = for ($i = 0; $i -lt $max; $i++) { '{0:x2}' -f $b[$i] }
    return ($hex -join ' ')
}

Write-Host ('- ' + [Environment]::MachineName + ' / ' + (Get-Date).ToString('s'))

for ($d = 0; $d -le 6; $d++) {
    $path = "\\.\PhysicalDrive$d"

    $fs = $null
    try {
        $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    }
    catch {
        # Test-Path does not work on device paths, so "no such drive" and
        # "not an administrator" both land here; say which one it looks like.
        $msg = $_.Exception.Message
        if ($msg -match 'find file|does not exist') { continue }
        Write-Host "drive $d : cannot open (rerun as Administrator): $msg"
        continue
    }

    try {
        $disk = Get-Disk -Number $d -ErrorAction SilentlyContinue
        $label = if ($disk) { "$($disk.FriendlyName) [$($disk.BusType)]" } else { 'unknown' }

        foreach ($band in $bands) {
            $hdr = New-Object byte[] $sector
            $fs.Seek([int64]$band.Header * $sector, [System.IO.SeekOrigin]::Begin) | Out-Null
            $n = $fs.Read($hdr, 0, $sector)
            if ($n -lt $sector) {
                Write-Host "drive $d $label : $($band.Name) band read short"
                continue
            }
            $txt = [System.Text.Encoding]::ASCII.GetString($hdr)
            if ($txt -notmatch 'NOVUS') {
                Write-Host ("drive $d {0} : {1} band empty (LBA {2}: {3})" -f `
                    $label, $band.Name, $band.Header, (Show-Bytes $hdr 16))
                continue
            }

            $headLine = (($txt -split "`0")[0]).Trim()
            $fs.Seek([int64]$band.Data * $sector, [System.IO.SeekOrigin]::Begin) | Out-Null
            $buf = New-Object byte[] ([int64]$band.Limit * $sector)
            $rd = $fs.Read($buf, 0, $buf.Length)
            $body = [System.Text.Encoding]::ASCII.GetString($buf, 0, [Math]::Max($rd, 0))
            $cut = [regex]::Match($body, '(\x00){128}')
            if ($cut.Success) { $body = $body.Substring(0, $cut.Index) }
            $body = $body.TrimEnd([char]0)

            Write-Host ""
            Write-Host "===== drive $d $label : $($band.Name) band ====="
            Write-Host "HEADER: $headLine"
            Write-Host "----- log text -----"
            Write-Host $body
            Write-Host "----- end of log ($($body.Length) chars) -----"
            Write-Host ""
        }
    }
    finally {
        if ($fs) { $fs.Dispose() }
    }
}

Write-Host 'done.'
