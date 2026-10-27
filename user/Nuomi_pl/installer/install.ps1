# ============================================================================
#  Nuomi 便携安装脚本（零依赖，无需 Inno Setup）
#
#  用法：
#    安装：   右键 install.cmd -> 以管理员身份非必须；直接双击 install.cmd 即可
#    卸载：   powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
#
#  行为：
#    1. 把 nuomi.exe / nucutil.exe / README.md / LICENSE 复制到
#       %LOCALAPPDATA%\Programs\Nuomi（用户级，无需管理员）
#    2. 把该目录加入「用户 PATH」（注册表 HKCU\Environment）
#    3. 创建「开始菜单」快捷方式
#    4. 卸载时反过来
# ============================================================================

[CmdletBinding()]
param(
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\Nuomi'
$ThisDir    = Split-Path -Parent $MyInvocation.MyCommand.Definition

function Write-Step($msg) { Write-Host ">> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "   $msg" -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "   $msg" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# PATH 操作（用户级，HKCU\Environment）
# ---------------------------------------------------------------------------
function Add-UserPath($dir) {
    $p = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ($p -and ($p -split ';') -contains $dir) { Write-Ok "PATH 已包含 $dir，跳过"; return }
    $new = if ($p) { $p.TrimEnd(';') + ';' + $dir } else { $dir }
    [Environment]::SetEnvironmentVariable('Path', $new, 'User')
    Write-Ok "已加入用户 PATH: $dir"
}

function Remove-UserPath($dir) {
    $p = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $p) { return }
    $parts = $p -split ';' | Where-Object { $_ -and $_ -ne $dir }
    $new = $parts -join ';'
    [Environment]::SetEnvironmentVariable('Path', $new, 'User')
    Write-Ok "已从用户 PATH 移除: $dir"
}

# ---------------------------------------------------------------------------
# 广播 WM_SETTINGCHANGE，让已打开的资源管理器/新终端立即生效
# ---------------------------------------------------------------------------
function Send-EnvRefresh {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public class EnvRefresh {
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern IntPtr SendMessageTimeout(
        IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
        uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
}
'@
    $null = [EnvRefresh]::SendMessageTimeout(
        [IntPtr]0xFFFF, 0x001A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref][UIntPtr]::Zero)
}

# ---------------------------------------------------------------------------
# 开始菜单快捷方式
# ---------------------------------------------------------------------------
function New-StartMenuShortcut {
    $linkPath = Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs\Nuomi.lnk'
    $ws = New-Object -ComObject WScript.Shell
    $lnk = $ws.CreateShortcut($linkPath)
    $lnk.TargetPath = Join-Path $InstallDir 'nuomi.exe'
    $lnk.WorkingDirectory = $InstallDir
    $lnk.Description = 'Nuomi 脚本解释器'
    $lnk.Save()
    Write-Ok "开始菜单快捷方式: $linkPath"
}

function Remove-StartMenuShortcut {
    $linkPath = Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs\Nuomi.lnk'
    if (Test-Path $linkPath) { Remove-Item $linkPath -Force; Write-Ok "已删除开始菜单快捷方式" }
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
if ($Uninstall) {
    Write-Step "卸载 Nuomi"
    if (Test-Path $InstallDir) {
        Remove-Item $InstallDir -Recurse -Force
        Write-Ok "已删除目录: $InstallDir"
    }
    Remove-UserPath $InstallDir
    Remove-StartMenuShortcut
    Send-EnvRefresh
    Write-Host "`nNuomi 已卸载。" -ForegroundColor Green
    exit 0
}

Write-Step "安装 Nuomi 到 $InstallDir"
if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory -Path $InstallDir | Out-Null }

$files = @('nuomi.exe', 'nucutil.exe', 'README.md', 'LICENSE')
foreach ($f in $files) {
    $src = Join-Path $ThisDir $f
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $InstallDir $f) -Force
        Write-Ok "复制 $f"
    } else {
        Write-Warn "跳过（未找到）: $f"
    }
}

Add-UserPath $InstallDir
New-StartMenuShortcut
Send-EnvRefresh

Write-Host "`n安装完成！新开一个 cmd / 终端，直接输入:" -ForegroundColor Green
Write-Host "    nuomi 你的脚本.nuomi" -ForegroundColor White
Write-Host "即可运行。" -ForegroundColor Green
