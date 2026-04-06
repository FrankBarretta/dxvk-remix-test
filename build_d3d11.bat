@echo off
setlocal

set "BUILD_DIR=%~1"
if "%BUILD_DIR%"=="" set "BUILD_DIR=_Comp64DebugOptimized"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_d3d11.ps1" -BuildDir "%BUILD_DIR%"

exit /b %ERRORLEVEL%