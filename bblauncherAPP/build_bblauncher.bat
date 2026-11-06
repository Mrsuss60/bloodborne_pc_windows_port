@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Bloodborne Launcher - Build

rem Build script for BloodborneLauncher.exe
rem Place this file next to bblauncher_app.py. The exe is written to the parent folder.

cd /d "%~dp0"

echo ============================================
echo   Bloodborne PC Launcher - Build
echo ============================================
echo.

rem ---------- 1. Source files ----------
echo [1/4] Checking source files...
for %%F in (bblauncher_app.py bblauncher_UI.py bblauncher_Utils.py) do (
    if not exist "%%F" (
        echo   [FAIL]    Missing %%F next to this script
        goto :fail
    )
)
echo   [OK]      bblauncher_app.py, bblauncher_UI.py, bblauncher_Utils.py

set "ICON_ARGS="
if exist "icon.ico" (
    echo   [OK]      icon.ico found - will embed into executable
    set "ICON_ARGS=--icon "icon.ico" --add-data "icon.ico;.""
) else (
    echo   [INFO]    icon.ico not found - building with default icon
)

set "DATA_ARGS="
if exist "resources" (
    echo   [OK]      resources folder found - will embed into executable
    set "DATA_ARGS=--add-data "resources;resources""
)

if not exist "..\scripts\bb_guard.py" echo   [WARN]    ..\scripts\bb_guard.py not found - the exe will build without it
if not exist "..\scripts\game_check.py" echo   [WARN]    ..\scripts\game_check.py not found - the exe will build without it
echo.

rem ---------- 2. Python ----------
echo [2/4] Checking Python...
set "PY="
python -c "import sys" >nul 2>&1 && set "PY=python"
if not defined PY (
    py -3 -c "import sys" >nul 2>&1 && set "PY=py -3"
)
if not defined PY (
    echo   [FAIL]    Python 3 was not found on PATH.
    echo             Install Python 3.9 or newer from https://www.python.org/downloads/
    echo             and tick "Add python.exe to PATH" in the installer.
    goto :fail
)

%PY% -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)" >nul 2>&1
if errorlevel 1 (
    echo   [FAIL]    Python 3.9 or newer is required.
    %PY% --version
    goto :fail
)
for /f "delims=" %%V in ('%PY% --version 2^>^&1') do set "PYVER=%%V"
echo   [OK]      %PYVER%  ^(%PY%^)

%PY% -m pip --version >nul 2>&1
if errorlevel 1 (
    echo   [MISSING] pip - trying to bootstrap it...
    %PY% -m ensurepip --upgrade
    %PY% -m pip --version >nul 2>&1
    if errorlevel 1 (
        echo   [FAIL]    pip is not available for this Python installation.
        goto :fail
    )
)
echo   [OK]      pip
echo.

rem ---------- 3. Libraries ----------
rem Usage: call :ensure IMPORT_NAME PIP_PACKAGE
rem Add more lines here if scripts\bb_guard.py or game_check.py need extra packages.
echo [3/4] Checking required libraries...
call :ensure PySide6 PySide6
if errorlevel 1 goto :fail
call :ensure PyInstaller pyinstaller
if errorlevel 1 goto :fail
echo.

rem ---------- 4. Build ----------
echo [4/4] Building BloodborneLauncher.exe...
tasklist /FI "IMAGENAME eq BloodborneLauncher.exe" 2>nul | find /i "BloodborneLauncher.exe" >nul
if not errorlevel 1 (
    echo   [FAIL]    BloodborneLauncher.exe is currently running. Close it and run this script again.
    goto :fail
)
echo.

%PY% -m PyInstaller --noconfirm --onefile --windowed ^
    --name "BloodborneLauncher" ^
    %ICON_ARGS% ^
    %DATA_ARGS% ^
    --distpath ".." ^
    --paths "." ^
    --paths "..\scripts" ^
    --hidden-import "bb_guard" ^
    --hidden-import "game_check" ^
    --exclude-module "PySide6.QtWebEngineCore" ^
    --exclude-module "PySide6.QtWebEngineWidgets" ^
    --exclude-module "PySide6.QtQml" ^
    --exclude-module "PySide6.QtQuick" ^
    --exclude-module "PySide6.Qt3DCore" ^
    --exclude-module "PySide6.QtNetwork" ^
    --exclude-module "PySide6.QtMultimedia" ^
    --exclude-module "PySide6.QtVirtualKeyboard" ^
    --exclude-module "PySide6.QtPdf" ^
    --exclude-module "PySide6.QtPdfWidgets" ^
    --exclude-module "PySide6.QtSvg" ^
    --exclude-module "PySide6.QtOpenGL" ^
    --exclude-module "PySide6.QtOpenGLWidgets" ^
    --exclude-module "PySide6.QtSql" ^
    --exclude-module "PySide6.QtTest" ^
    --exclude-module "PySide6.QtXml" ^
    --exclude-module "PySide6.QtPrintSupport" ^
    --exclude-module "PySide6.QtDesigner" ^
    --exclude-module "PySide6.QtHelp" ^
    --exclude-module "PySide6.QtUiTools" ^
    --exclude-module "tkinter" ^
    --exclude-module "unittest" ^
    --exclude-module "pydoc" ^
    --exclude-module "doctest" ^
    --exclude-module "email" ^
    --exclude-module "http" ^
    bblauncher_app.py
if errorlevel 1 (
    echo.
    echo   [FAIL]    PyInstaller reported an error. See the output above.
    goto :fail
)

for %%I in ("%~dp0..") do set "OUT=%%~fI"
echo.
echo ============================================
echo   Build succeeded
echo   %OUT%\BloodborneLauncher.exe
echo ============================================
set "RC=0"
goto :end

:fail
echo.
echo Build failed.
set "RC=1"

:end
if /i not "%~1"=="/nopause" pause
exit /b %RC%


rem ---------- subroutine: install a package if its module cannot be imported ----------
:ensure
%PY% -c "import %~1" >nul 2>&1
if not errorlevel 1 (
    echo   [OK]      %~2
    exit /b 0
)
echo   [MISSING] %~2 - installing...
%PY% -m pip install %~2
if errorlevel 1 (
    echo   [FAIL]    Could not install %~2
    exit /b 1
)
%PY% -c "import %~1" >nul 2>&1
if errorlevel 1 (
    echo   [FAIL]    %~2 was installed but cannot be imported
    exit /b 1
)
echo   [OK]      %~2 installed
exit /b 0