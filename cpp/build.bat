@echo off
rem Manual MSVC env (vcvars32.bat needs reg.exe which is blocked by policy)
set "MSVC=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKV=10.0.26100.0"

set "PATH=%MSVC%\bin\Hostx64\x86;%SDK%\bin\%SDKV%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\winrt"
set "LIB=%MSVC%\lib\x86;%SDK%\Lib\%SDKV%\ucrt\x86;%SDK%\Lib\%SDKV%\um\x86"

cd /d "%~dp0"

rc /nologo /c65001 /fo phonerecord.res phonerecord.rc
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /MT /O2 /GS /EHsc /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /D_WIN32_IE=0x0800 phonerecord.cpp phonerecord.res /Fe:PhoneRecord.exe /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /MANIFEST:EMBED /MANIFESTINPUT:phonerecord.manifest ws2_32.lib shell32.lib comctl32.lib user32.lib gdi32.lib crypt32.lib comdlg32.lib uxtheme.lib winmm.lib

if errorlevel 1 (
  echo BUILD_FAILED
  exit /b 1
)
echo BUILD_OK
