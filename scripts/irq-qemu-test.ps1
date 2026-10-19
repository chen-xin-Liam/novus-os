param(
    [Parameter(Mandatory)][string]$Qemu,
    [ValidateSet('bios', 'uefi')][string]$Mode = 'bios',
    [string]$Firmware,
    [string]$FirmwareVars,
    [ValidateSet('pc', 'q35')][string]$Machine = 'pc',
    [int]$TimeoutSeconds = 90,
    [switch]$Trace,
    [string]$BiosImage = 'out/bios.img',
    [string]$LogSuffix = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'out'
$log = Join-Path $out "irq-$Mode-$Machine$LogSuffix.log"
$shot = Join-Path $out "irq-$Mode-$Machine.ppm"
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$serialText = ''
$start = [Diagnostics.ProcessStartInfo]::new($Qemu)
$start.WorkingDirectory = $root
$start.UseShellExecute = $false
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.Environment['TEMP'] = $out
$start.Environment['TMP'] = $out
$arguments = @('-machine', $Machine, '-smp', '2', '-m', '4G',
    '-display', 'none', '-no-reboot', '-qmp', 'stdio',
    '-chardev', "socket,id=serial0,host=127.0.0.1,port=$port", '-serial', 'chardev:serial0',
    '-netdev', 'user,id=net0',
    '-device', 'e1000,netdev=net0')
$share = Join-Path (Split-Path $Qemu -Parent) 'share'
if (Test-Path $share) { $arguments += @('-L', $share) }
if ($Trace) { $arguments += @('-d', 'int,cpu_reset', '-D', "out/irq-$Mode-$Machine$LogSuffix-trace.log") }
if ($Mode -eq 'bios') {
    $arguments += @('-drive', "file=$BiosImage,format=raw")
} else {
    if (-not $Firmware) { throw 'UEFI requires -Firmware (OVMF/edk2 code image)' }
    $esp = Join-Path $out 'irq-esp/EFI/BOOT'
    New-Item -ItemType Directory -Force -Path $esp | Out-Null
    Copy-Item (Join-Path $out 'NOVUS.EFI') (Join-Path $esp 'BOOTX64.EFI') -Force
    if (-not $FirmwareVars) {
        $FirmwareVars = Join-Path (Split-Path $Firmware -Parent) 'edk2-i386-vars.fd'
    }
    $vars = Join-Path $out "irq-$Machine-vars.fd"
    Copy-Item $FirmwareVars $vars -Force
    $arguments += @('-drive', "if=pflash,format=raw,readonly=on,file=$Firmware",
        '-drive', "if=pflash,format=raw,file=$vars",
        '-drive', 'format=raw,file=fat:rw:out/irq-esp')
}
foreach ($argument in $arguments) { $start.ArgumentList.Add($argument) }
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

function Read-Qmp {
    $remaining = [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds
    if ($remaining -le 0) { throw 'QEMU test deadline exceeded' }
    $task = $process.StandardOutput.ReadLineAsync()
    if (-not $task.Wait($remaining)) { throw 'QMP response timeout' }
    if ($null -eq $task.Result) { throw "QEMU closed QMP: $($errors.GetAwaiter().GetResult())" }
    return $task.Result | ConvertFrom-Json -AsHashtable
}

function Send-Qmp([string]$Command, [hashtable]$Arguments = @{}) {
    $request = @{execute = $Command; arguments = $Arguments}
    $process.StandardInput.WriteLine(($request | ConvertTo-Json -Depth 10 -Compress))
    $process.StandardInput.Flush()
    do { $response = Read-Qmp } while ($response.ContainsKey('event'))
    if ($response.ContainsKey('error')) { throw ($response.error | ConvertTo-Json) }
    return $response['return']
}

function Wait-Log([string]$Pattern, [int]$After = 0) {
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($script:serialText.Substring($After) -match $Pattern) { return $script:serialText }
        if ($script:serialText -match '\[TRAP\] kernel fault|selftest(?:\s*:\s*|\s+)FAIL(?:ED)?\b|0x0*[1-9A-F][0-9A-F]* failed') {
            throw "Kernel failure, see $log"
        }
        $remaining = [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds
        $task = $serialReader.ReadLineAsync()
        if (-not $task.Wait([Math]::Max(1, $remaining))) { break }
        if ($null -eq $task.Result) { throw 'QEMU serial connection closed' }
        $script:serialText += $task.Result + "`n"
        $logWriter.WriteLine($task.Result)
    }
    throw "Missing log pattern '$Pattern', see $log"
}

function Click-Guest([int]$X, [int]$Y) {
    $null = Send-Qmp 'input-send-event' @{events = @(
        @{type = 'rel'; data = @{axis = 'x'; value = $X - $script:pointerX}},
        @{type = 'rel'; data = @{axis = 'y'; value = $Y - $script:pointerY}})}
    $script:pointerX = $X
    $script:pointerY = $Y
    $null = Send-Qmp 'input-send-event' @{events = @(
        @{type = 'btn'; data = @{button = 'left'; down = $true}})}
    $null = Send-Qmp 'input-send-event' @{events = @(
        @{type = 'btn'; data = @{button = 'left'; down = $false}})}
    $null = Wait-Log '\[SCHED\] ticks=' $script:serialText.Length
}

try {
    $null = $process.Start()
    $errors = $process.StandardError.ReadToEndAsync()
    $null = Read-Qmp
    $null = Send-Qmp 'qmp_capabilities'
    $connection = $listener.AcceptTcpClientAsync()
    if (-not $connection.Wait(10000)) { throw 'QEMU serial connection timeout' }
    $serialClient = $connection.Result
    $serialReader = [IO.StreamReader]::new($serialClient.GetStream())
    $logWriter = [IO.StreamWriter]::new($log, $false)
    $logWriter.AutoFlush = $true
    $null = Wait-Log '\[SCHED\] ticks='
    $null = Send-Qmp 'human-monitor-command' @{'command-line' = 'sendkey a'}
    $null = Send-Qmp 'input-send-event' @{events = @(
        @{type = 'rel'; data = @{axis = 'x'; value = 20}},
        @{type = 'rel'; data = @{axis = 'y'; value = 10}})}
    $null = Wait-Log '\[ND\] keyboard byte from IRQ queue'
    $null = Wait-Log '\[ND\] mouse byte from IRQ queue'
    $null = Wait-Log '\[NET\] e1000 RX from IRQ queue'
    $text = Wait-Log '\[NET\] ping 0x[0-9A-F]+ rtt='
    $script:pointerX = 660
    $script:pointerY = 276
    Click-Guest 390 764
    Click-Guest 450 550
    Click-Guest 650 144
    foreach ($key in @('1', '2', '3')) {
        $null = Send-Qmp 'human-monitor-command' @{'command-line' = "sendkey $key 1"}
        $text = Wait-Log '\[SCHED\] ticks=' $script:serialText.Length
    }
    $null = Send-Qmp 'screendump' @{filename = $shot}
    foreach ($pattern in @('\[PMM\][^\r\n]*PASS', '\[MM\][^\r\n]*PASS',
        '\[HEAP\][^\r\n]*PASS', '\[ND\] selftest: 0x[0-9A-F]+ checks passed, 0x0+ failed',
        '\[IPC\] selftest: PASS', '\[NOVS\][^\r\n]*PASSED',
        '\[NWM\] selftest: PASS',
        '\[SCHED\] task selftest exited with code 0x0*8E', '\[SCHED\] foreground selftest: PASS')) {
        if ($text -notmatch $pattern) { throw "Missing selftest '$pattern', see $log" }
    }
    Write-Output "$Mode/${Machine}: boot, selftests, PS/2 IRQ queues, e1000 IRQ RX + ping PASS"
    Write-Output "Serial: $log"
    Write-Output "Screen: $shot"
} finally {
    if (-not $process.HasExited) {
        try { $null = Send-Qmp 'quit' } catch { $process.Kill() }
        $process.WaitForExit()
    }
    if ($logWriter) { $logWriter.Dispose() }
    if ($serialReader) { $serialReader.Dispose() }
    if ($serialClient) { $serialClient.Dispose() }
    $listener.Stop()
    $process.Dispose()
}