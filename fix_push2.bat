@echo off
chcp 65001 >nul
title Fix Git Push - Attempt 2
echo ==========================================
echo   Fix GitHub Push Conflict
echo ==========================================
echo.

echo [Step 1/2] Pulling remote with unrelated histories...
git pull origin main --rebase --allow-unrelated-histories
if %errorlevel% neq 0 (
    echo.
    echo Pull failed. Trying standard merge with unrelated histories...
    git pull origin main --allow-unrelated-histories
    if %errorlevel% neq 0 (
        echo.
        echo ERROR: Still unable to merge.
        echo Please try manually with Git Bash.
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
