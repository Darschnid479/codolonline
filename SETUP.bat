@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist toolchains mkdir toolchains

echo ============================================================================
echo  PROJECT DRAGON — SETUP (OpenOrbis PS4 Toolchain + LLVM + CMake)
echo ============================================================================
echo [%date% %time%] Starting SETUP.bat > logs\setup.log

where wsl >nul 2>&1
if %errorlevel%==0 (
    echo [INFO] WSL detected. Bootstrapping OpenOrbis v0.5.4 toolchain via scripts/setup.sh...
    wsl bash -lc "cd \"$(wslpath '%~dp0')\" && chmod +x scripts/*.sh && ./scripts/setup.sh" >> logs\setup.log 2>&1
    if !errorlevel!==0 (
        echo [OK] OpenOrbis PS4 Toolchain ready in WSL.
        goto :check_win_tools
    )
)

:check_win_tools
if not defined OO_PS4_TOOLCHAIN (
    if exist "%~dp0toolchains\openorbis\OpenOrbis\PS4Toolchain\lib\crt1.o" (
        set "OO_PS4_TOOLCHAIN=%~dp0toolchains\openorbis\OpenOrbis\PS4Toolchain"
    )
)

echo [INFO] Checking Windows host tools (cmake, clang++, ld.lld, python)...
for %%T in (cmake clang++ ld.lld python) do (
    where %%T >nul 2>&1
    if !errorlevel!==0 (
        echo   [FOUND] %%T
    ) else (
        echo   [MISS ] %%T ^(install LLVM 18 + CMake or use WSL^)
    )
)

if defined OO_PS4_TOOLCHAIN (
    echo [OK] OO_PS4_TOOLCHAIN = %OO_PS4_TOOLCHAIN%
) else (
    echo [INFO] Downloading OpenOrbis v0.5.4 toolchain-llvm-18.tar.gz...
    curl -fL "https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz" -o toolchains\toolchain-llvm-18.tar.gz >> logs\setup.log 2>&1
    if !errorlevel!==0 (
        if not exist toolchains\openorbis mkdir toolchains\openorbis
        tar -xzf toolchains\toolchain-llvm-18.tar.gz -C toolchains\openorbis >> logs\setup.log 2>&1
        echo [OK] Extracted OpenOrbis toolchain to toolchains\openorbis\OpenOrbis\PS4Toolchain
    ) else (
        echo [ERROR] Download failed. See logs\setup.log
        exit /b 1
    )
)

echo [DONE] SETUP complete. See logs\setup.log for details.
endlocal
