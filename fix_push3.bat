@echo off
chcp 65001 >nul
title Push to GitHub
echo ==========================================
echo   Push SecureDrive to GitHub
echo ==========================================
echo.
echo [Step 1/1] Pushing to GitHub...
echo   Make sure your network is stable.
echo   If you have VPN/proxy, turn it on now.
echo.

git push -u origin main
if %errorlevel% equ 0 (
    echo.
    echo ==========================================
    echo   Push Successful!
    echo ==========================================
    echo.
    echo Repository: https://github.com/yzj327850/SecureDrive
    echo.
    echo Next steps:
    echo   1. Open the link above to view your repo
    echo   2. Click Actions tab to watch CI builds
    echo   3. Tag a release: git tag v1.0.0 ^&^& git push origin v1.0.0
    echo.
) else (
    echo.
    echo ==========================================
    echo   Push Failed - Retrying in 3 seconds...
    echo ==========================================
    echo.
    timeout /t 3 /nobreak >nul
    git push -u origin main
    if %errorlevel% equ 0 (
        echo.
        echo Retry successful!
        echo Repository: https://github.com/yzj327850/SecureDrive
        echo.
    ) else (
        echo.
        echo Still failed. Please:
        echo   1. Check your internet connection
        echo   2. Enable VPN/proxy if available
        echo   3. Wait a minute and run this script again
        echo.
    )
)

pause
