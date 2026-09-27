@echo off
:: Launcher for build_and_generate_dxil.ps1
:: Double-click or run from any PowerShell/CMD prompt without worrying about execution policy.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_and_generate_dxil.ps1"
if %ERRORLEVEL% neq 0 (
    echo.
    echo FAILED with error %ERRORLEVEL%
    pause
)
