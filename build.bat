@echo off
setlocal

set "CURRENT_DIR=%~dp0"
if "%CURRENT_DIR:~-1%"=="\" set "CURRENT_DIR=%CURRENT_DIR:~0,-1%"

if not defined W64DEVKIT_DIR if exist "D:\DEV_CPP\w64devkit" set "W64DEVKIT_DIR=D:\DEV_CPP\w64devkit"
if not defined SDL3_DIR if exist "D:\DEV_CPP\SDL3-3.4.12\x86_64-w64-mingw32" set "SDL3_DIR=D:\DEV_CPP\SDL3-3.4.12\x86_64-w64-mingw32"
if not defined VULKAN_SDK if exist "D:\DEV_CPP\Vulkan-Headers" set "VULKAN_SDK=D:\DEV_CPP\Vulkan-Headers"

rem Check if directory path is deep and could cause MAX_PATH errors (> 70 chars).
rem Uses goto labels instead of nested ( ) blocks so '(' or ')' in paths never breaks cmd.exe.
if "%BB_SUBST_ACTIVE%"=="1" goto subst_done
if not defined CURRENT_DIR goto subst_done
if "%CURRENT_DIR:~70,1%"=="" goto subst_done
subst X: /d >nul 2>nul
subst X: "%CURRENT_DIR%"
if errorlevel 1 goto subst_done
echo [INFO] Path length exceeds safe MAX_PATH threshold. Switching to virtual drive X:\
set "BB_SUBST_ACTIVE=1"
cd /d X:\
call X:\build.bat %*
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
cd /d "%CURRENT_DIR%"
subst X: /d >nul 2>nul
exit /b %BUILD_EXIT_CODE%
:subst_done

cd /d "%~dp0"

if not exist "out" mkdir out
if not exist "out\gpu" mkdir out\gpu

if defined W64DEVKIT_DIR set "PATH=%W64DEVKIT_DIR%\bin;%PATH%"
if defined MINGW_DIR goto mingw_path_set
if exist "C:\msys64\mingw64\bin" set "PATH=C:\msys64\mingw64\bin;%PATH%"
goto mingw_path_done
:mingw_path_set
set "PATH=%MINGW_DIR%\bin;%PATH%"
:mingw_path_done

if exist "C:\Program Files\CMake\bin" set "PATH=C:\Program Files\CMake\bin;%PATH%"

rem Determine parallel job count (defaults to logical CPU count; override with BB_BUILD_JOBS)
if not defined BB_BUILD_JOBS (
    if defined NUMBER_OF_PROCESSORS (
        set "BB_BUILD_JOBS=%NUMBER_OF_PROCESSORS%"
    ) else (
        set "BB_BUILD_JOBS=4"
    )
)

rem Find Python 3.10+ for staging and test scripts
if defined PYTHON goto found_python
for %%P in (
    "C:\Program Files\Python314\python.exe"
    "C:\Program Files\Python313\python.exe"
    "C:\Program Files\Python312\python.exe"
    "C:\Program Files\Python311\python.exe"
    "C:\Program Files\Python310\python.exe"
    "%LocalAppData%\Programs\Python\Python314\python.exe"
    "%LocalAppData%\Programs\Python\Python313\python.exe"
    "%LocalAppData%\Programs\Python\Python312\python.exe"
    "%LocalAppData%\Programs\Python\Python311\python.exe"
    "%LocalAppData%\Programs\Python\Python310\python.exe"
) do if not defined PYTHON if exist %%P set "PYTHON=%%~P"
if defined PYTHON goto found_python
for /f "delims=" %%i in ('where python.exe 2^>nul') do if not "%%~dpi"=="%LocalAppData%\Microsoft\WindowsApps\" if not defined PYTHON set "PYTHON=%%i"
if defined PYTHON goto found_python
for /f "tokens=*" %%i in ('where py.exe 2^>nul') do if not defined PYTHON set "PYTHON=%%i"
if not defined PYTHON set "PYTHON=python"
:found_python

rem Automatically initialize submodules if missing
if exist "gpu\third_party\fsr-vulkan\CMakeLists.txt" goto submodules_ok
git config --local core.longpaths true >nul 2>nul
echo Initializing submodules...
git submodule update --init --recursive
if errorlevel 1 (
    echo Failed to update submodules. Please run: git submodule update --init --recursive
    exit /b 1
)
:submodules_ok

rem Build LibAtrac9 static archive (rebuild if missing or lacking Atrac9 symbols)
if not exist "third_party\LibAtrac9\C\src\libatrac9.c" (
    echo LibAtrac9 sources missing. Run: git submodule update --init --recursive
    exit /b 1
)
if not exist "out\libatrac9.a" goto atrac9_build
nm out\libatrac9.a 2>nul | findstr /c:"Atrac9Decode" >nul
if not errorlevel 1 goto atrac9_done
echo out\libatrac9.a is stale or incomplete - rebuilding...
del /f /q out\libatrac9.a 2>nul
del /f /q out\atrac9\*.o 2>nul
:atrac9_build
echo Building LibAtrac9...
if not exist "out\atrac9" mkdir out\atrac9
set "AT9_OBJS="
for %%f in (third_party\LibAtrac9\C\src\*.c) do (
    if not exist "out\atrac9\%%~nf.o" (
        gcc -std=c99 -O2 -g -w -c "%%f" -o "out\atrac9\%%~nf.o"
        if errorlevel 1 (
            echo Failed to compile LibAtrac9 source: %%f
            exit /b 1
        )
    )
    call set "AT9_OBJS=%%AT9_OBJS%% out\atrac9\%%~nf.o"
)
ar rcs out\libatrac9.a %AT9_OBJS%
if errorlevel 1 (
    echo Failed to create out\libatrac9.a
    exit /b 1
)
echo Built out\libatrac9.a
:atrac9_done

rem Configure CMake (Ninja) only when build.ninja is not yet generated
if exist "out\gpu\build.ninja" goto cmake_configured
git config --local core.longpaths true >nul 2>nul
if exist "patches\fsr_vulkan_mingw.patch" if exist "gpu\third_party\fsr-vulkan\.git" (
    git -C gpu\third_party\fsr-vulkan apply --check "..\..\..\patches\fsr_vulkan_mingw.patch" >nul 2>nul
    if not errorlevel 1 (
        echo Applying MinGW compatibility patch to FSR-Vulkan submodule...
        git -C gpu\third_party\fsr-vulkan apply "..\..\..\patches\fsr_vulkan_mingw.patch"
    )
)

echo Configuring CMake (Ninja)...
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
:cmake_configured

echo Building bbgpu, bbport, and bb-gpu-capabilities (-j %BB_BUILD_JOBS%)...
cmake --build out/gpu --target bbgpu bbport bb-gpu-capabilities -- -j %BB_BUILD_JOBS%
if errorlevel 1 (
    echo Parallel build encountered an error; retrying with -j 1...
    cmake --build out/gpu --target bbgpu bbport bb-gpu-capabilities -- -j 1
    if errorlevel 1 (
        echo Build failed.
        exit /b 1
    )
)

if exist "out\bbport.exe" (
    rem Strip unneeded debug symbols and relocations to reduce binary size from ~260MB to ~12MB
    where strip.exe >nul 2>nul
    if not errorlevel 1 (
        echo Stripping debug symbols from out\bbport.exe...
        strip -s "out\bbport.exe"
    )
    copy /Y "out\bbport.exe" "out\bb-probe.exe" >nul
)

rem Stage runtime DLLs on first build, when SDL3.dll is missing, or when --stage-dlls is requested.
rem Skipping redundant objdump scans across 110+ DLLs saves 5-15s on every incremental build.
if /i "%~1"=="--stage-dlls" goto run_stage_dlls
if not exist "out\SDL3.dll" goto run_stage_dlls
if not exist "out\.dlls_staged" goto run_stage_dlls
goto stage_dlls_done
:run_stage_dlls
"%PYTHON%" scripts\stage_dlls.py
if not errorlevel 1 echo staged > "out\.dlls_staged"
:stage_dlls_done

rem Optional: the DLSS bridge (bbport_dlss.dll). NVIDIA's Windows NGX SDK is MSVC-only, so this
rem one DLL is built with MSVC while everything else stays MinGW-w64. Without Visual Studio or
rem the vendored SDK the step is skipped and DLSS falls back to FSR 3.1 at run time.
rem BB_DLSS_BRIDGE=0 skips it explicitly.
call :build_dlss_bridge

echo Build complete: out\bbport.exe

if not "%~1"=="--test" goto build_done
echo Running unit tests...
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc tests/test_win32_exception.c -o out/win32-exception-test.exe
if errorlevel 1 exit /b 1
out\win32-exception-test.exe
if errorlevel 1 exit /b 1

set "SDL3_INC="
if defined SDL3_DIR set "SDL3_INC=-I%SDL3_DIR%/include"
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Isrc %SDL3_INC% tests/test_pad.c out/SDL3.dll -o out/pad-test.exe
if errorlevel 1 exit /b 1
out\pad-test.exe
if errorlevel 1 exit /b 1

if exist "out\test_runtime.exe" (
    out\test_runtime.exe
    if errorlevel 1 exit /b 1
)

gcc -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc tests/test_file_mods.c src/win32_compat.c -lws2_32 -o out/file-mods-test.exe
if errorlevel 1 exit /b 1
out\file-mods-test.exe
if errorlevel 1 exit /b 1

if exist "out\test_sema.exe" (
    out\test_sema.exe
    if errorlevel 1 exit /b 1
)

if exist "out\content-test.exe" (
    out\content-test.exe
    if errorlevel 1 exit /b 1
)

cmake --build out/gpu --target shader-user-data-test motion-history-test motion-shader-test ui-composition-test scene-proxy-release-test upscaler-support-test -- -j %BB_BUILD_JOBS%
if errorlevel 1 exit /b 1

out\gpu\shader-user-data-test.exe
if errorlevel 1 exit /b 1
out\gpu\motion-history-test.exe
if errorlevel 1 exit /b 1
out\gpu\ui-composition-test.exe
if errorlevel 1 exit /b 1
out\gpu\scene-proxy-release-test.exe
if errorlevel 1 exit /b 1
out\gpu\upscaler-support-test.exe
if errorlevel 1 exit /b 1
out\gpu\motion-shader-test.exe
if errorlevel 1 exit /b 1

"%PYTHON%" -c "import glob, subprocess, sys; results = [(f, subprocess.run([sys.executable, f]).returncode) for f in sorted(glob.glob('tests/test_*.py'))]; [print(f, code) for f, code in results]; sys.exit(0 if all(code == 0 for f, code in results) else 1)"
if errorlevel 1 exit /b 1

echo ALL TESTS PASSED!
:build_done
exit /b 0

rem ---------------------------------------------------------------------------------------------
rem The DLSS bridge: gpu/dlss_bridge -> out/bbport_dlss.dll (MSVC, optionally Ninja).
rem ---------------------------------------------------------------------------------------------
:build_dlss_bridge
if /i "%BB_DLSS_BRIDGE%"=="0" exit /b 0
if not exist "gpu\dlss_bridge\bbport_dlss_bridge.cpp" exit /b 0
if not exist "gpu\dlss_bridge\third_party\nvngx-sdk\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" (
    echo [INFO] No vendored NGX SDK: skipping the DLSS bridge; DLSS uses FSR 3.1.
    exit /b 0
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [INFO] Visual Studio not found: skipping the DLSS bridge; DLSS uses FSR 3.1.
    exit /b 0
)
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo [INFO] MSVC tools not found: skipping the DLSS bridge; DLSS uses FSR 3.1.
    exit /b 0
)

echo Building the DLSS bridge (bbport_dlss.dll, MSVC)...
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [WARN] vcvars64.bat failed: DLSS will use FSR 3.1.
    exit /b 0
)
set "DLSS_CMAKE_MAKE="
where ninja.exe >nul 2>nul
if errorlevel 1 if defined W64DEVKIT_DIR if exist "%W64DEVKIT_DIR%\bin\ninja.exe" (
    set "DLSS_CMAKE_MAKE=-DCMAKE_MAKE_PROGRAM=%W64DEVKIT_DIR%/bin/ninja.exe"
)
cmake -S gpu\dlss_bridge -B out\dlss-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CXX_COMPILER=cl %DLSS_CMAKE_MAKE% ^
      -DDLSS_SDK_ROOT="%CD%\gpu\dlss_bridge\third_party\nvngx-sdk" ^
      -DVULKAN_INCLUDE="%VULKAN_SDK%\include"
if errorlevel 1 (
    echo [WARN] DLSS bridge configuration failed: DLSS will use FSR 3.1.
    exit /b 0
)
cmake --build out\dlss-bridge
if errorlevel 1 (
    echo [WARN] DLSS bridge build failed: DLSS will use FSR 3.1.
    exit /b 0
)
copy /Y "out\dlss-bridge\bbport_dlss.dll" "out\bbport_dlss.dll" >nul
if errorlevel 1 (
    echo [WARN] Could not copy bbport_dlss.dll next to bbport.exe.
    exit /b 0
)
echo DLSS bridge: out\bbport_dlss.dll
exit /b 0
