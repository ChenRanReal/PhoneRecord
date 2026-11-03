@echo off
if not exist "%~dp0cpp\PhoneRecord.exe" (
  echo PhoneRecord.exe is missing. Run cpp\build.bat first.
  pause
  exit /b 1
)
start "" "%~dp0cpp\PhoneRecord.exe"
