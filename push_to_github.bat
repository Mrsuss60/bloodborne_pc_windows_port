@echo off
setlocal
cd /d "%~dp0"

echo ===================================================
echo Pushing Bloodborne Windows Port to GitHub Fork
echo Repository: https://github.com/Mrsuss60/bloodborne_pc_windows_port
echo ===================================================
echo.
git push -u origin master
if errorlevel 1 (
    echo.
    echo [ERROR] Push failed.
    echo 1. Make sure you clicked 'Create fork' on GitHub so the repository exists.
    echo 2. Make sure you are logged in to GitHub with credentials for Mrsuss60.
    echo.
    pause
    exit /b 1
)
echo.
echo [SUCCESS] Windows port successfully pushed to https://github.com/Mrsuss60/bloodborne_pc_windows_port!
echo.
pause
