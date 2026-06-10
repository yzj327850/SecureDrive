@echo off
chcp 65001 >nul
title Push to GitHub
echo ==========================================
echo   SecureDrive GitHub Push Tool
echo ==========================================
echo.

echo [Step 1/3] Checking project directory...
if not exist "CMakeLists.txt" (
    echo ERROR: CMakeLists.txt not found.
    echo Please run this batch file inside the project folder.
    pause
    exit /b 1
)
echo   OK
echo.

echo [Step 2/3] Configuring remote repository...
git remote get-url origin >nul 2>&1
if %errorlevel% neq 0 (
    git remote add origin https://github.com/yzj327850/SecureDrive.git
    echo   Remote added: https://github.com/yzj327850/SecureDrive.git
) else (
    echo   Remote already exists
)
echo.

echo [Step 3/3] Pushing to GitHub...
echo   Uploading code, please wait...
echo   If asked for credentials, use your GitHub Personal Access Token as password.
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
    echo   2. Click the Actions tab to watch build progress
    echo   3. Wait 5-10 minutes for binaries
    echo.
    echo To release a version, run:
    echo   git tag v1.0.0
    echo   git push origin v1.0.0
    echo.
) else (
    echo.
    echo ==========================================
    echo   Push Failed
    echo ==========================================
    echo.
    echo Possible reasons:
    echo   - Network issues
    echo   - Wrong GitHub credentials
    echo   - Repository conflicts
    echo.
    echo Please check the error message and retry.
    echo.
)

pause
