@echo off
setlocal

cd /d "%~dp0"

REM Find Python 3
if "%PYTHON%"=="" (
    for %%P in (
        "C:\Program Files\Python313\python.exe"
        "C:\Program Files\Python312\python.exe"
        "C:\Program Files\Python311\python.exe"
        "C:\Program Files\Python310\python.exe"
        "%LocalAppData%\Programs\Python\Python313\python.exe"
        "%LocalAppData%\Programs\Python\Python312\python.exe"
        "%LocalAppData%\Programs\Python\Python311\python.exe"
        "%LocalAppData%\Programs\Python\Python310\python.exe"
    ) do (
        if not defined PYTHON if exist %%P set "PYTHON=%%~P"
    )
)
if "%PYTHON%"=="" (
    for /f "delims=" %%i in ('where python.exe 2^>nul') do (
        set "pycand=%%i"
        if not "%%~dpi"=="%LocalAppData%\Microsoft\WindowsApps\" (
            if "%PYTHON%"=="" set "PYTHON=%%i"
        )
    )
)
if "%PYTHON%"=="" (
    for /f "tokens=*" %%i in ('where py.exe 2^>nul') do (
        if "%PYTHON%"=="" set "PYTHON=%%i"
    )
)
if "%PYTHON%"=="" set "PYTHON=python"

REM Writable directory for generated files (out/), saves (user/) and bbport.ini
if "%BB_DATA_DIR%"=="" set "BB_DATA_DIR=."
set "out=%BB_DATA_DIR%\out"
if not exist "%out%" mkdir "%out%"
if "%BB_CONFIG%"=="" set "BB_CONFIG=%BB_DATA_DIR%\bbport.ini"

REM Game directory containing eboot.bin
set "EXTRA_ARGS="
:arg_loop
if "%~1"=="" goto arg_done
if exist "%~1\eboot.bin" (
    set "BB_GAME_DIR=%~1"
    shift
    goto arg_loop
)
if "%~nx1"=="eboot.bin" if exist "%~1" (
    set "BB_GAME_DIR=%~dp1"
    shift
    goto arg_loop
)
set EXTRA_ARGS=%EXTRA_ARGS% %1
shift
goto arg_loop
:arg_done

if "%BB_GAME_DIR%"=="" set "BB_GAME_DIR=..\CUSA03173"
set "game=%BB_GAME_DIR%"
REM Strip trailing backslash if present
if "%game:~-1%"=="\" set "game=%game:~0,-1%"
if not exist "%game%\eboot.bin" (
    echo No eboot.bin in %game% ^(set BB_GAME_DIR or pass path to run.bat^). >&2
    exit /b 1
)

set "mods_dir=%BB_MODS_DIR%"
if "%mods_dir%"=="" set "mods_dir=%BB_DATA_DIR%\mods"
set "mods_config=%BB_MODS_CONFIG%"
if "%mods_config%"=="" set "mods_config=%BB_DATA_DIR%\mods.json"
set "mods_enabled=%BB_MODS_ENABLED%"
if "%mods_enabled%"=="" set "mods_enabled=1"

REM NOTE: for /f ('"exe" args "more args"') trips cmd's quote-stripping rule and prints
REM "The filename, directory name, or volume label syntax is incorrect." So write the
REM output to a temp file and read that instead (last line wins, same as before).
set "mods_result=%out%\mods_game.txt"
if exist "%mods_result%" del "%mods_result%"
"%PYTHON%" scripts\mods.py "%game%" --out "%out%" --mods-dir "%mods_dir%" --config "%mods_config%" --enabled "%mods_enabled%" > "%mods_result%"
if exist "%mods_result%" for /f "usebackq delims=" %%g in ("%mods_result%") do set "game=%%g"

"%PYTHON%" scripts\prepare.py "%game%" --out "%out%"
if errorlevel 1 exit /b 1

"%PYTHON%" scripts\link_libc.py "%game%" --out "%out%"
if errorlevel 1 exit /b 1

"%PYTHON%" scripts\link_modules.py "%game%" --out "%out%"
if errorlevel 1 exit /b 1

set "sku=%BB_CONTENT_SKU%"
if "%sku%"=="" set "sku=full"
"%PYTHON%" scripts\content_profile.py "%game%" --out "%out%" --sku "%sku%"
if errorlevel 1 exit /b 1

if "%BB_AUTO_RENDER_RES%"=="1" (
    set "BB_RENDER_RES="
    set "BB_OUTPUT_RES="
    set "BB_AUTO_RENDER_RES="
)

if "%BB_FPS%"=="" set "BB_FPS=uncap"
set "fps=%BB_FPS%"

set "scaled_render="
set "scaled_output="
if "%BB_RENDER_RES%"=="" (
    for /f "tokens=1,2" %%a in ('"%PYTHON%" scripts\patches.py --print-scaled --settings "%BB_CONFIG%" 2^>nul') do (
        set "scaled_render=%%a"
        set "scaled_output=%%b"
    )
)

set "live=0"
if not "%scaled_output%"=="" (
    if not "%BB_LIVE_RES%"=="" set "live=%BB_LIVE_RES%"
    if "%live%"=="0" if exist "%BB_CONFIG%" (
        for /f "tokens=1,2 delims==" %%a in ('type "%BB_CONFIG%" 2^>nul') do (
            if "%%a"=="live_resolution" set "live=%%b"
        )
    )
    if "%live%"=="auto" (
        set "caps=out\bb-gpu-capabilities.exe"
        for /f "delims=" %%c in ('"%caps%" --live-resolution 2^>nul') do set "live=%%c"
    )
    if not "%live%"=="1" set "live=0"
)

if "%live%"=="1" (
    echo Output %scaled_output%: live resolution changes ^(live_resolution=0: startup patch^)
) else if not "%scaled_output%"=="" (
    set "BB_RENDER_RES=%scaled_render%"
    set "BB_OUTPUT_RES=%scaled_output%"
    set "BB_AUTO_RENDER_RES=1"
    if "%BB_DMEM_MB%"=="" set "BB_DMEM_MB=9152"
    echo Output %scaled_output%: scene %scaled_render%, direct memory %BB_DMEM_MB% MiB ^(live_resolution=1: live changes^)
)

set "patches_dir=%BB_PATCHES_DIR%"
if "%patches_dir%"=="" set "patches_dir=%BB_DATA_DIR%\patches"
set "patches_config=%BB_PATCHES_CONFIG%"
if "%patches_config%"=="" set "patches_config=%BB_DATA_DIR%\patches.json"

"%PYTHON%" scripts\patches.py --out "%out%" --fps "%fps%" --extra "%BB_PATCHES%" --settings "%BB_CONFIG%" --game-dir "%game%" --render-res "%BB_RENDER_RES%" --output-res "%BB_OUTPUT_RES%" --patches-dir "%patches_dir%" --patches-config "%patches_config%"
if errorlevel 1 exit /b 1

if "%BB_VBLANK_HZ%"=="" (
    if "%fps%"=="uncap" (
        set "BB_VBLANK_HZ=0"
    ) else if "%fps%"=="90" (
        set "BB_VBLANK_HZ=90"
    ) else (
        set "BB_VBLANK_HZ=60"
    )
)

if not exist "out\bbport.exe" (
    if exist "build.bat" (
        call build.bat
        if errorlevel 1 exit /b 1
    ) else (
        echo [ERROR] out\bbport.exe not found. >&2
        exit /b 1
    )
)

set "probe=out\bbport.exe"
set "user_dir=%BB_USER_DIR%"
if "%user_dir%"=="" set "user_dir=%BB_DATA_DIR%\user"
set "timeout=%BB_TIMEOUT%"
if "%timeout%"=="" set "timeout=0"

echo Launching Bloodborne PC (%probe%)...
"%probe%" "%out%\boot-linked.bin" --content-profile "%out%\content.bin" --patches "%out%\patches.bin" --app0 "%game%" --user "%user_dir%" --timeout "%timeout%" %EXTRA_ARGS%
exit /b %ERRORLEVEL%