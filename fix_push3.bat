@echo off
chcp 65001 >nul
title Fix Git Push - Final Attempt
echo ==========================================
echo   Fix GitHub Push Conflict
echo ==========================================
echo.

echo [Step 1/3] Committing local helper scripts first...
git add push_to_github.bat fix_push.bat fix_push2.bat fix_push3.bat
git commit -m "Add helper scripts for GitHub push"
echo   OK
echo.

echo [Step 2/3] Pulling remote changes with unrelated histories...
git pull origin main --allow-unrelated-histories
if %errorlevel% neq 0 (
    echo.
    echo Pull failed. Trying again...
    git pull origin main --allow-unrelated-histories
    if %errorlevel% neq 0 (
        echo.
        echo ERROR: Unable to merge.
        echo Please check network connection and retry.
        pause
        exit /b 1
    )
)
echo   OK
echo.

echo [Step 3/3] Pushing to GitHub...
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
    echo   Push Failed - Retrying...
    echo ==========================================
    echo.
    git push -u origin main
    if %errorlevel% equ 0 (
        echo Retry successful!
        echo Repository: https://github.com/yzj327850/SecureDrive
    ) else (
        echo Still failed. Please check network.
    )
)

pause
