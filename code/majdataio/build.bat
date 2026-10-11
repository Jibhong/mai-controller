@echo off
setlocal
cd /d "%~dp0"

where gcc >nul 2>&1
if %errorlevel%==0 (
    echo [build] using gcc
    gcc -Wall -Wextra -O2 -o majdataio.exe majdataio.c -lsetupapi -ladvapi32 -luser32
    goto :done
)

where cl >nul 2>&1
if %errorlevel%==0 (
    echo [build] using MSVC cl
    cl /nologo /O2 majdataio.c /Fe:majdataio.exe /link setupapi.lib advapi32.lib user32.lib
    goto :done
)

echo [build] no compiler found.
echo         Install MinGW-w64 / MSYS2 (gcc), or run this from "x64 Native Tools Command Prompt for VS".
exit /b 1

:done
if exist majdataio.exe (
    echo [build] OK: majdataio.exe
    exit /b 0
)
echo [build] FAILED
exit /b 1
