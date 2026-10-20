@echo off
rem chunithm-streamkit test runner (Zig, x86)
setlocal
cd /d "%~dp0.."

if defined ZIG (
    set "ZIGEXE=%ZIG%"
) else (
    where zig >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] zig not found. Set ZIG or add zig to PATH.
        exit /b 1
    )
    set "ZIGEXE=zig"
)

set "CFLAGS=-target x86-windows-gnu -O2 -std=c++17 -Isrc -Ithird_party\brotli\include -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -DNOMINMAX"
if not exist tests\bin mkdir tests\bin

echo == parse_test ==
call :zigbuild tests\bin\parse_test.exe tests\parse_test.cpp || exit /b 1
tests\bin\parse_test.exe || exit /b 1

echo.
echo == songdb_test ==
call :zigbuild tests\bin\songdb_test.exe tests\songdb_test.cpp || exit /b 1
tests\bin\songdb_test.exe || exit /b 1

echo.
echo == queue_test ==
call :zigbuild tests\bin\queue_test.exe tests\queue_test.cpp src\app.cpp src\hook.cpp || exit /b 1
tests\bin\queue_test.exe || exit /b 1

echo.
echo == bili_auth_test (network) ==
call :zigbuild tests\bin\bili_auth_test.exe tests\bili_auth_test.cpp || exit /b 1
tests\bin\bili_auth_test.exe || exit /b 1

echo.
echo all tests passed.
echo For live danmaku: run build_zig.cmd, then tests\bin\bili_ws_test.exe
exit /b 0

:zigbuild
"%ZIGEXE%" c++ %CFLAGS% -o %*
if errorlevel 1 (
    echo [retry] zig spawn failed, retrying...
    "%ZIGEXE%" c++ %CFLAGS% -o %*
)
exit /b %errorlevel%
