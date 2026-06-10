@echo off & setlocal EnableDelayedExpansion
REM ===========================================================
REM SecureDrive 编译脚本（cmd 直接调用 nmake）
REM 用法：右键"以管理员身份运行" 或在 VS 开发人员命令提示符中运行
REM ===========================================================

call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvarsall.bat 失败，请确保安装了 VS 2026
    pause & exit /b 1
)

echo [ENV] PATH=%PATH:~0,120%...
echo [ENV] INCLUDE=%INCLUDE:~0,120%...
echo.

set "SRC=C:\Users\yzj32\WorkBuddy\20260529083812\SecureDrive"
set "BLD=%SRC%\build"

echo ============================================================
echo [1/3] Cleaning build directory...
echo ============================================================
if exist "%BLD%" rmdir /s /q "%BLD%" 2>nul
mkdir "%BLD%" 2>nul

echo.
echo ============================================================
echo [2/3] CMake Configure (NMake Makefiles)...
echo ============================================================
cd /d "%BLD%"
cmake -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=cl ^
  -DCMAKE_CXX_COMPILER=cl ^
  -DSDRV_CONSOLE=ON ^
  "%SRC%" 2>&1
if errorlevel 1 (
    echo.
    echo *** CMAKE CONFIGURE FAILED ***
    pause & exit /b 1
)

echo.
echo ============================================================
echo [3/3] Building...
echo ============================================================
cmake --build . --config Release 2>&1
if errorlevel 1 (
    echo.
    echo *** BUILD FAILED ***
    pause & exit /b 1
)

echo.
echo ============================================================
echo BUILD SUCCESS
echo ============================================================
if exist "%BLD%\SecureDrive.exe" (
    echo Output: %BLD%\SecureDrive.exe
    dir "%BLD%\SecureDrive.exe" | find ".exe"
)
echo.
pause
