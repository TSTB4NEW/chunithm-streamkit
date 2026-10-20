@echo off
rem ============================================================
rem chunithm-streamkit build script (MSVC, Win32) - recommended
rem Requires: Visual Studio 2017/2019/2022 with C++ desktop
rem workload + CMake.
rem Output: build_win32\Release\chunithm_streamkit.asi
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere not found. Install Visual Studio C++ desktop workload.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo [ERROR] no Visual Studio with VC tools found
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul

cmake -S . -B build_win32 -A Win32 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build_win32 --config Release
if errorlevel 1 exit /b 1

echo.
echo done: build_win32\Release\chunithm_streamkit.asi
echo test host: build_win32\Release\test_host.exe
