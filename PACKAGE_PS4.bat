@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist dist mkdir dist

echo ============================================================================
echo  PROJECT DRAGON — PACKAGE STANDALONE PS4 PKG (LibOrbisPkg / PkgTool.Core)
echo ============================================================================

if not exist build-ps4\eboot.bin (
    echo [INFO] build-ps4\eboot.bin missing — running BUILD_PS4.bat first...
    call BUILD_PS4.bat
    if !errorlevel! neq 0 exit /b 1
)

where wsl >nul 2>&1
if %errorlevel%==0 (
    echo [INFO] Packaging standalone PS4 PKG via WSL scripts/package_ps4.sh...
    wsl bash -lc "cd \"$(wslpath '%~dp0')\" && ./scripts/package_ps4.sh"
    if !errorlevel! neq 0 (
        echo [ERROR] PKG packaging failed. See logs\package_ps4.log
        exit /b 1
    )
    goto :done
)

if not defined OO_PS4_TOOLCHAIN (
    set "OO_PS4_TOOLCHAIN=%~dp0toolchains\openorbis\OpenOrbis\PS4Toolchain"
)
set "TOOLS=%OO_PS4_TOOLCHAIN%\bin\windows"
set "STAGE=build-ps4\pkg_stage"
set "TITLE_ID=DRGN00113"
set "CONTENT_ID=IV0000-DRGN00113_00-PROJECTDRAGON000"

if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\sce_sys\about" "%STAGE%\sce_module"
copy /y build-ps4\eboot.bin "%STAGE%\eboot.bin" >nul
copy /y ps4\sce_sys\icon0.png "%STAGE%\sce_sys\icon0.png" >nul
copy /y "%OO_PS4_TOOLCHAIN%\samples\piglet\sce_sys\about\right.sprx" "%STAGE%\sce_sys\about\right.sprx" >nul
copy /y "%OO_PS4_TOOLCHAIN%\samples\piglet\sce_module\*.prx" "%STAGE%\sce_module\" >nul

"%TOOLS%\PkgTool.Core.exe" sfo_new "%STAGE%\sce_sys\param.sfo" > logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" APP_TYPE --type Integer --maxsize 4 --value 1 >> logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" APP_VER --type Utf8 --maxsize 8 --value "01.00" >> logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" ATTRIBUTE --type Integer --maxsize 4 --value 2 >> logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" CATEGORY --type Utf8 --maxsize 4 --value "gd" >> logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" CONTENT_ID --type Utf8 --maxsize 48 --value "%CONTENT_ID%" >> logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0 >> logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" SYSTEM_VER --type Integer --maxsize 4 --value 0 >> logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" TITLE --type Utf8 --maxsize 128 --value "Project Dragon - COD Online Native" >> logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" TITLE_ID --type Utf8 --maxsize 12 --value "%TITLE_ID%" >> logs\package_ps4.log 2>&1
"%TOOLS%\PkgTool.Core.exe" sfo_setentry "%STAGE%\sce_sys\param.sfo" VERSION --type Utf8 --maxsize 8 --value "01.00" >> logs\package_ps4.log 2>&1

pushd "%STAGE%"
"%TOOLS%\create-gp4.exe" -out pkg.gp4 --content-id="%CONTENT_ID%" --files "eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx" >> "%~dp0logs\package_ps4.log" 2>&1
"%TOOLS%\PkgTool.Core.exe" pkg_build pkg.gp4 "%~dp0dist" >> "%~dp0logs\package_ps4.log" 2>&1
popd

:done
if not exist "dist\IV0000-DRGN00113_00-PROJECTDRAGON000.pkg" (
    echo [ERROR] PKG file was not created! See logs\package_ps4.log
    exit /b 1
)
echo [OK] Standalone PS4 PKG built and verified:
dir dist\*.pkg
endlocal
