@echo off
chcp 65001 >nul
cd /d "%~dp0"
nuomi.exe samples\games.nuomi
echo.
pause
