@echo off
setlocal

REM === SecureDrive Build Script (Run as Administrator) ===
echo ============================================================
echo   SecureDrive Build Script
echo ============================================================

REM Setup MSVC environment
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo ERROR: Failed to setup MSVC environment
    pause
    exit /b 1
)

cd /d "C:\Users\yzj32\WorkBuddy\20260529083812\SecureDrive"

REM Clean old build
if exist build rmdir /s /q build

echo.
echo === CMake Configure (NMake Makefiles) ===
cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo ERROR: CMake configure failed!
    echo.
    pause
    exit /b 1
)

echo.
echo === Building SecureDrive ===
cmake --build build
if errorlevel 1 (
    echo.
    echo ERROR: Build failed!
    echo.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo   Build successful!
echo   Output: build\SecureDrive.exe
echo ============================================================
echo.
pause
