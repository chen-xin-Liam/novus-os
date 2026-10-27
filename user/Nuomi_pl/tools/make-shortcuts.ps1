# SPDX-License-Identifier: GPL-2.0
# tools/make-shortcuts.ps1 -- create Windows shortcuts (.lnk) that carry the Nuomi icon.
# Usage (repo root):  powershell -ExecutionPolicy Bypass -File tools\make-shortcuts.ps1
# Creates: Nuomi - Games / Nuomi - Guess / Nuomi - REPL  (double-click to run with nuomi.ico icon)
# Note: TCC-built nuomi.exe can't embed an icon (no windres / no PE header slack), so we
#       put the Nuomi icon on the shortcuts instead. Shortcuts pin to this repo's absolute path;
#       re-run this script if you move the repo. (Shortcuts (*.lnk) are machine-local, git-ignored.)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $repo "nuomi.exe"
$ico  = Join-Path $repo "nuomi.ico"
if (-not (Test-Path $exe)) { Write-Error "nuomi.exe not found in $repo - build first (build.bat)"; exit 1 }
if (-not (Test-Path $ico)) { Write-Error "nuomi.ico not found in $repo - generate: python tools\gen-icon.py"; exit 1 }

$ws = New-Object -ComObject WScript.Shell
$links = @(
  @{ Name = "Nuomi - Games"; Args = "samples\games.nuomi";      Desc = "Nuomi games: auto battle + number guess" },
  @{ Name = "Nuomi - Guess"; Args = "samples\guess-play.nuomi"; Desc = "Nuomi number-guessing game (you play)" },
  @{ Name = "Nuomi - REPL";  Args = "";                          Desc = "Nuomi language REPL" }
)
foreach ($l in $links) {
  $path = Join-Path $repo ($l.Name + ".lnk")
  $sc = $ws.CreateShortcut($path)
  $sc.TargetPath       = $exe
  $sc.Arguments        = $l.Args
  $sc.WorkingDirectory = $repo
  $sc.IconLocation     = ($ico + ",0")
  $sc.Description      = $l.Desc
  $sc.Save()
  Write-Host ("created: {0}" -f $path)
}
