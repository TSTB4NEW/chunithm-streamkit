@echo off
rem ============================================================
rem 部署到游戏目录（ASI 必须放在 bin\ 根目录，插件是从那里被加载的）
rem 用法：deploy.cmd            （复制 asi + web）
rem       deploy.cmd web        （只更新 web 资源）
rem ============================================================
setlocal
cd /d "%~dp0"
set "DEST=X:\Game\Chunithm\SDHD - MATE\bin"

if /i "%~1"=="web" goto web

copy /y "build_zig\chunithm_streamkit.asi" "%DEST%\chunithm_streamkit.asi" || goto locked
copy /y "build_zig\chunithm_streamkit.asi" "%DEST%\chunithm_streamkit\chunithm_streamkit.asi" >nul 2>nul

:web
if exist web\index.html (
  xcopy /y /e /i "web" "%DEST%\chunithm_streamkit\web" >nul
  echo web  -> %DEST%\chunithm_streamkit\web
)
echo done
exit /b 0

:locked
echo [ERROR] chunithm_streamkit.asi is locked - close the game first
exit /b 1
