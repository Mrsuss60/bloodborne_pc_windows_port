@echo off
setlocal

cd /d "%~dp0"

REM Find Python 3 (prefer bundled embedded Python if present)
if defined PYTHON goto found_python
if exist "%~dp0python-embed\python.exe" set "PYTHON=%~dp0python-embed\python.exe" & goto found_python
if exist "%~dp0python\python.exe" set "PYTHON=%~dp0python\python.exe" & goto found_python
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

REM Writable directory for generated files (out/), saves (user/) and bbport.ini
if "%BB_DATA_DIR%"=="" set "BB_DATA_DIR=."
set "out=%BB_DATA_DIR%\out"
if not exist "%out%" mkdir "%out%"
if "%BB_CONFIG%"=="" set "BB_CONFIG=%BB_DATA_DIR%\bbport.ini"

REM An in-game restart or a crash can leave the old bbport.exe alive with gigabytes of RAM and
REM VRAM. Give it a few seconds to exit, then terminate it (the guard never fails the launch).
if exist scripts\bb_guard.py "%PYTHON%" scripts\bb_guard.py --exe-dir out --wait 10

REM Game directory containing eboot.bin (no parenthesized blocks so paths with '(' or ')' never break cmd.exe)
set "EXTRA_ARGS="
:arg_loop
if "%~1"=="" goto arg_done
if exist "%~1\eboot.bin" goto arg_game_dir
if /i "%~nx1"=="eboot.bin" if exist "%~1" goto arg_eboot_file
set EXTRA_ARGS=%EXTRA_ARGS% %1
shift
goto arg_loop
:arg_game_dir
set "BB_GAME_DIR=%~1"
shift
goto arg_loop
:arg_eboot_file
set "BB_GAME_DIR=%~dp1"
shift
goto arg_loop
:arg_done

if "%BB_GAME_DIR%"=="" set "BB_GAME_DIR=..\CUSA03173"
set "game=%BB_GAME_DIR%"
REM Strip trailing backslash if present
if "%game:~-1%"=="\" set "game=%game:~0,-1%"
if exist "%game%\eboot.bin" goto found_eboot
echo No eboot.bin in "%game%" ^(set BB_GAME_DIR or pass path to run.bat^). >&2
exit /b 1
:found_eboot

set "mods_dir=%BB_MODS_DIR%"
if "%mods_dir%"=="" set "mods_dir=%BB_DATA_DIR%\mods"
set "mods_config=%BB_MODS_CONFIG%"
if "%mods_config%"=="" set "mods_config=%BB_DATA_DIR%\mods.json"
set "mods_enabled=%BB_MODS_ENABLED%"
if "%mods_enabled%"=="" set "mods_enabled=1"

REM NOTE: for /f ('"exe" args "more args"') trips cmd's quote-stripping rule and prints
REM "The filename, directory name, or volume label syntax is incorrect." Also, cmd's for /f
REM in ("path") parser breaks if the path itself contains ')'. Write to a temp file in %out%
REM and read it via pushd using a plain filename so ')' in paths cannot break the loop.
set "mods_result=%out%\mods_game.txt"
if exist "%mods_result%" del "%mods_result%"
"%PYTHON%" scripts\mods.py "%game%" --out "%out%" --mods-dir "%mods_dir%" --config "%mods_config%" --enabled "%mods_enabled%" > "%mods_result%"
pushd "%out%"
if exist "mods_game.txt" for /f "usebackq delims=" %%g in ("mods_game.txt") do set "game=%%g"
popd

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

if "%BB_FPS%"=="30" set "BB_LIVE_FPS=0"
if "%BB_LIVE_FPS%"=="1" (
    set "fps=live"
    if "%BB_VBLANK_HZ%"=="" set "BB_VBLANK_HZ=0"
) else (
    if "%BB_FPS%"=="" set "BB_FPS=uncap"
    set "fps=%BB_FPS%"
)

REM The scaled sizes go through a file, like mods.py above: read with for /f ('"%PYTHON%" ...
REM "%BB_CONFIG%"') the command lost its quotes and printed nothing, so an output other than
REM 1080p never got its startup patch (or its direct memory) and always ran live.
set "scaled_render="
set "scaled_output="
set "scaled_result=%out%\scaled_sizes.txt"
if exist "%scaled_result%" del "%scaled_result%"
if "%BB_RENDER_RES%"=="" "%PYTHON%" scripts\patches.py --print-scaled --settings "%BB_CONFIG%" > "%scaled_result%" 2>nul
pushd "%out%"
if exist "scaled_sizes.txt" for /f "usebackq tokens=1,2" %%a in ("scaled_sizes.txt") do (
    set "scaled_render=%%a"
    set "scaled_output=%%b"
)
popd

REM One statement per line: inside a ( ) block %live% keeps the value it had before the block,
REM which turned live_resolution=1 and auto from bbport.ini back into 0.
set "live=0"
if "%scaled_output%"=="" goto live_chosen
if not "%BB_LIVE_RES%"=="" set "live=%BB_LIVE_RES%"
if not "%live%"=="0" goto check_live_auto
if not exist "%BB_CONFIG%" goto check_live_auto
copy /y "%BB_CONFIG%" "%out%\bbport_ini.tmp" >nul 2>nul
pushd "%out%"
if exist "bbport_ini.tmp" for /f "usebackq tokens=1,2 delims==" %%a in ("bbport_ini.tmp") do if "%%a"=="live_resolution" set "live=%%b"
if exist "bbport_ini.tmp" del /f /q "bbport_ini.tmp" >nul 2>nul
popd
:check_live_auto
if "%live%"=="auto" for /f "delims=" %%c in ('out\bb-gpu-capabilities.exe --live-resolution 2^>nul') do set "live=%%c"
if not "%live%"=="1" set "live=0"
:live_chosen

if "%live%"=="1" (
    echo Output %scaled_output%: live resolution changes ^(live_resolution=0: startup patch^)
) else if not "%scaled_output%"=="" (
    set "BB_RENDER_RES=%scaled_render%"
    set "BB_OUTPUT_RES=%scaled_output%"
    set "BB_AUTO_RENDER_RES=1"
    if "%BB_DMEM_MB%"=="" set "BB_DMEM_MB=9152"
)
REM Outside the block, so the message shows the memory just set.
if defined BB_AUTO_RENDER_RES echo Output %scaled_output%: scene %scaled_render%, direct memory %BB_DMEM_MB% MiB ^(live_resolution=1: live changes^)

REM Explicit launcher resolutions skip scaled_output above, but patches.py still adds
REM Increased Graphics Heap Sizes above 1080p. Match that patch's direct-memory budget.
set "render_pixels=0"
if defined BB_RENDER_RES for /f "tokens=1,2 delims=xX" %%a in ("%BB_RENDER_RES%") do set /a "render_pixels=%%a*%%b" >nul 2>nul
if %render_pixels% GTR 2073600 if not defined BB_DMEM_MB set "BB_DMEM_MB=9152"
if %render_pixels% GTR 2073600 echo Render %BB_RENDER_RES%: direct memory %BB_DMEM_MB% MiB

set "patches_dir=%BB_PATCHES_DIR%"
if "%patches_dir%"=="" set "patches_dir=%BB_DATA_DIR%\patches"
set "patches_config=%BB_PATCHES_CONFIG%"
if "%patches_config%"=="" set "patches_config=%BB_DATA_DIR%\patches.json"

"%PYTHON%" scripts\patches.py --out "%out%" --fps "%fps%" --extra "%BB_PATCHES%" --settings "%BB_CONFIG%" --game-dir "%game%" --render-res "%BB_RENDER_RES%" --output-res "%BB_OUTPUT_RES%" --patches-dir "%patches_dir%" --patches-config "%patches_config%"
if errorlevel 1 exit /b 1

if "%BB_VBLANK_HZ%"=="" (
    if "%fps%"=="live" (
        set "BB_VBLANK_HZ=0"
    ) else if "%fps%"=="uncap" (
        set "BB_VBLANK_HZ=0"
    ) else if "%fps%"=="90" (
        set "BB_VBLANK_HZ=90"
    ) else (
        set "BB_VBLANK_HZ=60"
    )
)

if exist "out\bbport.exe" goto found_probe
if exist "build.bat" (
    call build.bat
    if errorlevel 1 exit /b 1
    goto found_probe
)
echo [ERROR] out\bbport.exe not found. >&2
exit /b 1
:found_probe

set "probe=out\bbport.exe"
set "user_dir=%BB_USER_DIR%"
if "%user_dir%"=="" set "user_dir=%BB_DATA_DIR%\user"
set "timeout=%BB_TIMEOUT%"
if "%timeout%"=="" set "timeout=0"
if not defined BB_SHADER_RECIPES if exist "%~dp0shader_recipes.bin" set "BB_SHADER_RECIPES=%~dp0shader_recipes.bin"
if not defined BB_SHADER_GAME_DIR set "BB_SHADER_GAME_DIR=%game%"

echo Launching Bloodborne PC (%probe%)...
"%probe%" "%out%\boot-linked.bin" --content-profile "%out%\content.bin" --patches "%out%\patches.bin" --app0 "%game%" --user "%user_dir%" --timeout "%timeout%" %EXTRA_ARGS%
exit /b %ERRORLEVEL%
