@echo off
setlocal

set "CURRENT_DIR=%~dp0"
if "%CURRENT_DIR:~-1%"=="\" set "CURRENT_DIR=%CURRENT_DIR:~0,-1%"

if not defined W64DEVKIT_DIR if exist "D:\DEV_CPP\w64devkit" set "W64DEVKIT_DIR=D:\DEV_CPP\w64devkit"
if not defined SDL3_DIR if exist "D:\DEV_CPP\SDL3-3.4.12\x86_64-w64-mingw32" set "SDL3_DIR=D:\DEV_CPP\SDL3-3.4.12\x86_64-w64-mingw32"
if not defined VULKAN_SDK if exist "D:\DEV_CPP\Vulkan-Headers" set "VULKAN_SDK=D:\DEV_CPP\Vulkan-Headers"

rem Check if directory path is deep and could cause MAX_PATH errors (> 70 chars)
rem If running directly from a deep path without a virtual root drive, mount and re-exec from X:
if not "%BB_SUBST_ACTIVE%"=="1" (
    set "BB_PATH_CHECK=%CURRENT_DIR%"
    if defined CURRENT_DIR (
        if "%CURRENT_DIR:~70,1%" neq "" (
            subst X: /d >nul 2>nul
            subst X: "%CURRENT_DIR%"
            if not errorlevel 1 (
                echo [INFO] Path length exceeds safe MAX_PATH threshold. Switching to virtual drive X:\
                set "BB_SUBST_ACTIVE=1"
                cd /d X:\
                call X:\build.bat %*
                set "BUILD_EXIT_CODE=%ERRORLEVEL%"
                cd /d "%CURRENT_DIR%"
                subst X: /d >nul 2>nul
                exit /b %BUILD_EXIT_CODE%
            )
        )
    )
)

cd /d "%~dp0"

if not exist "out" mkdir out
if not exist "out\gpu" mkdir out\gpu

if not defined W64DEVKIT_DIR if exist "D:\DEV_CPP\w64devkit" set "W64DEVKIT_DIR=D:\DEV_CPP\w64devkit"
if not defined SDL3_DIR if exist "D:\DEV_CPP\SDL3-3.4.12\x86_64-w64-mingw32" set "SDL3_DIR=D:\DEV_CPP\SDL3-3.4.12\x86_64-w64-mingw32"
if not defined VULKAN_SDK if exist "D:\DEV_CPP\Vulkan-Headers" set "VULKAN_SDK=D:\DEV_CPP\Vulkan-Headers"

if defined W64DEVKIT_DIR (
    set "PATH=%W64DEVKIT_DIR%\bin;%PATH%"
)
if defined MINGW_DIR (
    set "PATH=%MINGW_DIR%\bin;%PATH%"
) else if exist "C:\msys64\mingw64\bin" (
    set "PATH=C:\msys64\mingw64\bin;%PATH%"
)

if exist "C:\Program Files\CMake\bin" (
    set "PATH=C:\Program Files\CMake\bin;%PATH%"
)

rem Ensure Git handles long paths on Windows without failing on FidelityFX headers
git config --local core.longpaths true >nul 2>nul

rem Automatically initialize and update submodules if missing
if not exist "gpu\third_party\fsr-vulkan\CMakeLists.txt" (
    echo Initializing submodules...
    git submodule update --init --recursive
    if errorlevel 1 (
        echo Failed to update submodules. Please run: git submodule update --init --recursive
        exit /b 1
    )
)

if not exist "out\libatrac9.a" (
    echo Building LibAtrac9
    if not exist "out\atrac9" mkdir out\atrac9
    for %%f in (third_party\LibAtrac9\C\src\*.c) do (
        gcc -std=c99 -O2 -g -w -c "%%f" -o "out\atrac9\%%~nf.o"
        if errorlevel 1 (
            echo Failed to compile LibAtrac9 source: %%f
            exit /b 1
        )
    )
    pushd out\atrac9
    del /f /q ..\libatrac9.a 2>nul
    set "OBJS="
    for %%o in (*.o) do call set "OBJS=%%OBJS%% %%o"
    ar rcs ..\libatrac9.a %OBJS%
    popd
    if errorlevel 1 (
        echo Failed to create out\libatrac9.a
        exit /b 1
    )
    echo Built out\libatrac9.a
)

if exist "patches\fsr_vulkan_mingw.patch" if exist "gpu\third_party\fsr-vulkan\.git" (
    git -C gpu\third_party\fsr-vulkan apply --check "..\..\..\patches\fsr_vulkan_mingw.patch" >nul 2>nul
    if not errorlevel 1 (
        echo Applying MinGW compatibility patch to FSR-Vulkan submodule...
        git -C gpu\third_party\fsr-vulkan apply "..\..\..\patches\fsr_vulkan_mingw.patch"
    )
)

if not exist "out\gpu\build.ninja" (
    echo Configuring CMake (Ninja)
    set "CMAKE_OPTS=-S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
    where ninja.exe >nul 2>nul
    if errorlevel 1 (
        if defined W64DEVKIT_DIR if exist "%W64DEVKIT_DIR%\bin\ninja.exe" (
            set "CMAKE_OPTS=%CMAKE_OPTS% -DCMAKE_MAKE_PROGRAM=%W64DEVKIT_DIR%/bin/ninja.exe"
        )
    )
    set "PREFIX_PATHS="
    if defined SDL3_DIR set "PREFIX_PATHS=%SDL3_DIR%"
    if defined VULKAN_SDK (
        if defined PREFIX_PATHS (
            set "PREFIX_PATHS=%PREFIX_PATHS%;%VULKAN_SDK%"
        ) else (
            set "PREFIX_PATHS=%VULKAN_SDK%"
        )
    )
    if exist "C:\msys64\mingw64" (
        if defined PREFIX_PATHS (
            set "PREFIX_PATHS=%PREFIX_PATHS%;C:/msys64/mingw64"
        ) else (
            set "PREFIX_PATHS=C:/msys64/mingw64"
        )
    )
    if defined CMAKE_PREFIX_PATH (
        if defined PREFIX_PATHS (
            set "PREFIX_PATHS=%PREFIX_PATHS%;%CMAKE_PREFIX_PATH%"
        ) else (
            set "PREFIX_PATHS=%CMAKE_PREFIX_PATH%"
        )
    )
    cmake %CMAKE_OPTS% -DCMAKE_PREFIX_PATH="%PREFIX_PATHS%"
    if errorlevel 1 (
        echo CMake configuration failed.
        exit /b 1
    )
)

echo Building bbgpu, bbport, and bb-gpu-capabilities
cmake --build out/gpu --target bbgpu bbport bb-gpu-capabilities -- -j 1
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

if exist "out\bbport.exe" (
    copy /Y "out\bbport.exe" "out\bb-probe.exe" >nul
)

python scripts\stage_dlls.py

echo Build complete: out\bbport.exe

if "%~1"=="--test" (
    echo Running unit tests
    gcc -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc tests/test_win32_exception.c -o out/win32-exception-test.exe
    if errorlevel 1 exit /b 1
    out\win32-exception-test.exe
    if errorlevel 1 exit /b 1

    set "SDL3_INC=-ID:/DEV_CPP/SDL3-3.4.12/x86_64-w64-mingw32/include"
    gcc -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Isrc -ID:/DEV_CPP/SDL3-3.4.12/x86_64-w64-mingw32/include tests/test_pad.c out/SDL3.dll -o out/pad-test.exe
    if errorlevel 1 exit /b 1
    out\pad-test.exe
    if errorlevel 1 exit /b 1

    out\test_runtime.exe
    if errorlevel 1 exit /b 1

    gcc -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc tests/test_file_mods.c -o out/file-mods-test.exe
    if errorlevel 1 exit /b 1
    out\file-mods-test.exe
    if errorlevel 1 exit /b 1

    out\test_sema.exe
    if errorlevel 1 exit /b 1

    out\content-test.exe
    if errorlevel 1 exit /b 1

    cmake --build out/gpu --target shader-user-data-test motion-history-test motion-shader-test ui-composition-test upscaler-support-test
    if errorlevel 1 exit /b 1

    out\gpu\shader-user-data-test.exe
    if errorlevel 1 exit /b 1

    out\gpu\motion-history-test.exe
    if errorlevel 1 exit /b 1
    out\gpu\ui-composition-test.exe
    if errorlevel 1 exit /b 1
    out\gpu\upscaler-support-test.exe
    if errorlevel 1 exit /b 1
    out\gpu\motion-shader-test.exe
    if errorlevel 1 exit /b 1

    python -c "import glob, subprocess, sys; results = [(f, subprocess.run([sys.executable, f]).returncode) for f in sorted(glob.glob('tests/test_*.py'))]; [print(f, code) for f, code in results]; sys.exit(0 if all(code == 0 for f, code in results) else 1)"
    if errorlevel 1 exit /b 1

    echo ALL TESTS PASSED!
)
