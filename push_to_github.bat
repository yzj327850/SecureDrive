@echo off
chcp 65001 >nul
echo.
git push -u origin main

if %errorlevel% equ 0 (
    echo.
    echo ==========================================
    echo   git tag v1.0.0
    echo   git push origin v1.0.0
    echo.
) else (
    echo.
    echo ==========================================
    echo.
)

pause
