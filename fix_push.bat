@echo off
chcp 65001 >nul
title Fix Git Push Conflict
echo ==========================================
echo   Fix GitHub Push Conflict
echo ==========================================
echo.

echo [Step 1/2] Fetching remote changes and merging...
git pull origin main --rebase
if %errorlevel% neq 0 (
    echo.
    echo WARNING: Pull failed. Trying standard merge instead...
    git pull origin main
    if %errorlevel% neq 0 (
        echo.
        echo ERROR: Unable to merge remote changes.
        echo Please check the error message above.
        pause
        exit /b 1
    )
)
echo   OK
echo.

echo [Step 2/2] Pushing to GitHub...
git push -u origin main
if %errorlevel% equ 0 (
    echo.
    echo ==========================================
    echo   Push Successful!
    echo ==========================================
    echo.
    echo Repository: https://github.com/yzj327850/SecureDrive
    echo.
) else (
    echo.
    echo ==========================================
    echo   Push Failed
    echo ==========================================
    echo.
)

pause
