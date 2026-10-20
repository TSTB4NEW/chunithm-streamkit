@echo off
rem ============================================================
rem chunithm-streamkit build script (Zig, Win32) - no MSVC needed
rem Requires: zig 0.13+ on PATH, or set ZIG env var.
rem Output: build_zig\chunithm_streamkit.asi / test_host.exe
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

if defined ZIG (
    set "ZIGEXE=%ZIG%"
) else (
    where zig >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] zig not found. Install zig 0.13+ and add to PATH, or set ZIG.
        exit /b 1
    )
    set "ZIGEXE=zig"
)

if not exist build_zig mkdir build_zig
set "CFLAGS=-target x86-windows-gnu -O2 -std=c++17 -Isrc -Ithird_party\brotli\include -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -DNOMINMAX"

echo == building brotli decoder (C) ==
set "BRLI=third_party\brotli"
set "BRINC=-I%BRLI%\include -I%BRLI%"
set "BROBJ="
for %%f in (bit_reader decode huffman state) do (
    "%ZIGEXE%" cc -target x86-windows-gnu -O2 %BRINC% -c -o build_zig\br_%%f.obj %BRLI%\dec\%%f.c
    if errorlevel 1 exit /b 1
    set "BROBJ=!BROBJ! build_zig\br_%%f.obj"
)
for %%f in (constants context dictionary transform platform) do (
    "%ZIGEXE%" cc -target x86-windows-gnu -O2 %BRINC% -c -o build_zig\br_%%f.obj %BRLI%\common\%%f.c
    if errorlevel 1 exit /b 1
    set "BROBJ=!BROBJ! build_zig\br_%%f.obj"
)

echo == building ASI (x86) ==
"%ZIGEXE%" c++ %CFLAGS% -shared -s -o build_zig\chunithm_streamkit.asi src\main.cpp src\app.cpp src\httpd.cpp src\hook.cpp %BROBJ% -lws2_32
if errorlevel 1 exit /b 1

echo == building test host (pure C, no C++ runtime) ==
"%ZIGEXE%" cc -target x86-windows-gnu -O2 -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -o build_zig\test_host.exe tests\host\main.c -lws2_32
if errorlevel 1 exit /b 1

echo == copying runtime files ==
if not exist build_zig\web mkdir build_zig\web
xcopy /y /q /e web\* build_zig\web\ >nul
copy /y config\chunithm_streamkit.ini.example build_zig\chunithm_streamkit.ini.example >nul

echo.
echo done: build_zig\chunithm_streamkit.asi
