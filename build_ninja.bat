@echo off
:: ============================================================
:: SecureDrive Build Script (Ninja + MSVC)
:: Run as Administrator
:: ============================================================

title SecureDrive Build

:: Force command echo so we can see what's happening
@echo on

:: Switch to script directory
cd /d "%~dp0"

echo ========================================
echo  Step 1: Running vcvarsall.bat x64
echo ========================================
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
echo vcvarsall.bat exit code: %ERRORLEVEL%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] vcvarsall.bat failed!
    echo Check that VS 2026 is installed at the expected path.
    goto :end
)

echo.
echo ========================================
echo  Step 2: Verify cl.exe and rc.exe
echo ========================================
where cl.exe
if %ERRORLEVEL% neq 0 (
    echo [ERROR] cl.exe not found in PATH after vcvarsall!
    goto :end
)
where rc.exe
if %ERRORLEVEL% neq 0 (
    echo [WARN] rc.exe not in PATH, adding manually...
    set PATH=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64;%PATH%
    where rc.exe
)

echo.
echo ========================================
echo  Step 3: Add Ninja to PATH
echo ========================================
set NINJA_DIR=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja
if exist "%NINJA_DIR%\ninja.exe" (
    echo [OK] ninja.exe found
    set PATH=%NINJA_DIR%;%PATH%
) else (
    echo [ERROR] ninja.exe not found at %NINJA_DIR%
    goto :end
)

echo.
echo ========================================
echo  Step 4: CMake Configure (Ninja)
echo ========================================
if exist build rmdir /s /q build
mkdir build
cd build

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
echo CMake exit code: %ERRORLEVEL%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configure failed!
    goto :end
)

echo.
echo ========================================
echo  Step 5: Build with Ninja
echo ========================================
ninja -j8
echo Ninja exit code: %ERRORLEVEL%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed!
    goto :end
)

echo.
echo ========================================
echo  Build SUCCESS!
echo ========================================
echo Work dir version: %cd%\SecureDrive.exe

echo.
echo Creating desktop version (no console window)...
set EDITBIN_EXE=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\editbin.exe
if exist "%EDITBIN_EXE%" (
    "%EDITBIN_EXE%" /SUBSYSTEM:WINDOWS SecureDrive.exe /OUT:SecureDrive_NoConsole.exe
    if %ERRORLEVEL% equ 0 (
        echo Desktop version: %cd%\SecureDrive_NoConsole.exe
    ) else (
        echo [WARN] editbin failed
    )
) else (
    echo [WARN] editbin.exe not found
)

goto :end

:end
echo.
echo ========================================
echo  Script finished. Window will stay open.
echo  Press any key to close.
echo ========================================
pause
