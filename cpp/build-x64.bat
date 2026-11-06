@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\Build-Desktop.ps1" -Architecture x64 %*
exit /b %errorlevel%
