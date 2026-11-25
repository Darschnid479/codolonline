@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist logs mkdir logs

echo ============================================================================
echo  PROJECT DRAGON — NATIVE PS4 BUILD (OrbisOS x86-64 ELF + eboot.bin FSELF)
echo ============================================================================

if not defined OO_PS4_TOOLCHAIN (
    if exist "%~dp0toolchains\openorbis\OpenOrbis\PS4Toolchain\lib\crt1.o" (
        set "OO_PS4_TOOLCHAIN=%~dp0toolchains\openorbis\OpenOrbis\PS4Toolchain"
    )
)

where clang++ >nul 2>&1
if %errorlevel%==0 (
    where ld.lld >nul 2>&1
    if !errorlevel!==0 (
        if defined OO_PS4_TOOLCHAIN (
            echo [INFO] Building native PS4 binary on Windows with LLVM + OpenOrbis...
            cmake -S . -B build-ps4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake\openorbis-ps4-toolchain.cmake -DCMAKE_BUILD_TYPE=Release > logs\build_ps4.log 2>&1
            if !errorlevel! neq 0 goto :try_wsl
            cmake --build build-ps4 >> logs\build_ps4.log 2>&1
            if !errorlevel! neq 0 (
                echo [ERROR] PS4 build failed. See logs\build_ps4.log
                type logs\build_ps4.log
                exit /b 1
            )
            goto :verify
        )
    )
)

:try_wsl
where wsl >nul 2>&1
if %errorlevel%==0 (
    echo [INFO] Delegating native PS4 build to WSL scripts/build_ps4.sh...
    wsl bash -lc "cd \"$(wslpath '%~dp0')\" && ./scripts/build_ps4.sh"
    if !errorlevel! neq 0 (
        echo [ERROR] PS4 build failed. See logs\build_ps4.log
        exit /b 1
    )
    goto :verify
)

echo [ERROR] Neither Windows LLVM+OpenOrbis nor WSL build environment was found. Run SETUP.bat first.
exit /b 1

:verify
if not exist build-ps4\eboot.bin (
    echo [ERROR] build-ps4\eboot.bin was not produced!
    exit /b 1
)
echo [OK] Produced native PS4 executable: build-ps4\eboot.bin
dir build-ps4\project_dragon.elf build-ps4\project_dragon.oelf build-ps4\eboot.bin
endlocal
