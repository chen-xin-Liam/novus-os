@echo off
rem Create Nuomi icon'd Windows shortcuts (Games / Guess / REPL) using nuomi.ico.
rem Double-click this file; it runs tools\make-shortcuts.ps1 (needs nuomi.exe + nuomi.ico present).
powershell -ExecutionPolicy Bypass -File "%~dp0tools\make-shortcuts.ps1"
pause
