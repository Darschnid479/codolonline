@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist dist\all-in-one mkdir dist\all-in-one

echo ============================================================================
echo  PROJECT DRAGON — ALL-IN-ONE PERSONAL PS4 PKG PACKAGER
echo  Bundles native PS4 eboot.bin + your local COD Online 1.1.1.13 game files
echo  into /app0/codol inside a single PS4 .pkg (PERSONAL USE ONLY).
echo ============================================================================

set "GAME_DIR=%~1"
if "%GAME_DIR%"=="" if defined CODOL_GAME_DIR set "GAME_DIR=%CODOL_GAME_DIR%"
if "%GAME_DIR%"=="" (
    echo Usage: PACKAGE_PS4_ALL_IN_ONE.bat "C:\Path\To\Call of Duty Online 1.1.1.13" [--no-videos]
    exit /b 1
)

if not exist build-ps4\eboot.bin (
    echo [INFO] build-ps4\eboot.bin missing — running BUILD_PS4.bat first...
    call BUILD_PS4.bat
    if !errorlevel! neq 0 exit /b 1
)

where wsl >nul 2>&1
if %errorlevel%==0 (
    echo [INFO] Packaging All-in-One PS4 PKG via WSL scripts/package_ps4_all_in_one.sh...
    wsl bash -lc "cd \"$(wslpath '%~dp0')\" && chmod +x scripts/package_ps4_all_in_one.sh && ./scripts/package_ps4_all_in_one.sh \"$(wslpath '%GAME_DIR%')\" %2"
    if !errorlevel! neq 0 (
        echo [ERROR] All-in-One PKG build failed. See logs\package_ps4_all_in_one.log
        exit /b 1
    )
    echo [OK] All-in-One PS4 PKG ready:
    dir dist\all-in-one\*.pkg
    exit /b 0
)

echo [ERROR] WSL is required on Windows to run the All-in-One GP4/LibOrbisPkg packager.
exit /b 1
endlocal
