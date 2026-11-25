@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist logs mkdir logs

echo ============================================================================
echo  PROJECT DRAGON — PC DEBUG BUILD + AUTOMATED UNIT TESTS
echo ============================================================================

where cmake >nul 2>&1
if %errorlevel%==0 (
    echo [INFO] Configuring and building PC Debug targets on Windows...
    cmake -S . -B build-pc -DCMAKE_BUILD_TYPE=Release > logs\build_pc_debug.log 2>&1
    if !errorlevel!==0 (
        cmake --build build-pc --config Release >> logs\build_pc_debug.log 2>&1
        if !errorlevel!==0 (
            if exist build-pc\Release\dragon_tests.exe (
                build-pc\Release\dragon_tests.exe
            ) else if exist build-pc\dragon_tests.exe (
                build-pc\dragon_tests.exe
            )
            echo [OK] PC Debug build and tests complete. See logs\build_pc_debug.log
            exit /b !errorlevel!
        )
    )
)

where wsl >nul 2>&1
if %errorlevel%==0 (
    echo [INFO] Delegating PC Debug build to WSL scripts/build_pc_debug.sh...
    wsl bash -lc "cd \"$(wslpath '%~dp0')\" && ./scripts/build_pc_debug.sh"
    exit /b !errorlevel!
)

echo [ERROR] PC Debug build failed. See logs\build_pc_debug.log
exit /b 1
endlocal
