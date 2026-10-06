@echo off
setlocal

cd /d "%~dp0"

if not exist "out" mkdir out
if not exist "out\gpu" mkdir out\gpu

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
    ar rcs out\libatrac9.a out\atrac9\*.o
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
    set "SDL3_INC="
    if defined SDL3_DIR (
        set "SDL3_INC=-I%SDL3_DIR%/include"
    ) else if exist "C:\msys64\mingw64\include\SDL3" (
        set "SDL3_INC=-IC:/msys64/mingw64/include"
    )
    gcc -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Isrc %SDL3_INC% tests/test_pad.c out/SDL3.dll -o out/pad-test.exe
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

    cmake --build out/gpu --target motion-history-test motion-shader-test ui-composition-test upscaler-support-test
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
