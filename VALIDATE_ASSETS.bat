@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist logs mkdir logs

echo ============================================================================
echo  PROJECT DRAGON — COD ONLINE 1.1.1.13 AUTOMATED ASSET VALIDATION
echo ============================================================================

set "GAME_DIR=%~1"
if "%GAME_DIR%"=="" if defined CODOL_GAME_DIR set "GAME_DIR=%CODOL_GAME_DIR%"
if "%GAME_DIR%"=="" (
    echo Usage: VALIDATE_ASSETS.bat "C:\Path\To\Call of Duty Online 1.1.1.13" [--all]
    exit /b 1
)

if exist build-pc\Release\dragon_validate.exe (
    build-pc\Release\dragon_validate.exe --game "%GAME_DIR%" --out docs\COMPATIBILITY_REPORT.md %2 %3 %4
    exit /b !errorlevel!
)
if exist build-pc\dragon_validate.exe (
    build-pc\dragon_validate.exe --game "%GAME_DIR%" --out docs\COMPATIBILITY_REPORT.md %2 %3 %4
    exit /b !errorlevel!
)

where wsl >nul 2>&1
if %errorlevel%==0 (
    wsl bash -lc "cd \"$(wslpath '%~dp0')\" && ./scripts/validate_assets.sh \"$(wslpath '%GAME_DIR%')\" %2 %3 %4"
    exit /b !errorlevel!
)

echo [ERROR] dragon_validate not built yet. Run BUILD_PC_DEBUG.bat first.
exit /b 1
endlocal
