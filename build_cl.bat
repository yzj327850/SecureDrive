@echo off
setlocal EnableDelayedExpansion

REM ============================================================
REM SecureDrive 直接编译（cl.exe + rc.exe，无 CMake）
REM 先设置 MSVC 环境，然后编译所有源文件
REM ============================================================

call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvarsall.bat failed
    pause & exit /b 1
)

echo [ENV] PATH: %PATH:~0,100%...
echo.

set "SRCDIR=%~dp0"
set "OUTDIR=%SRCDIR%build"

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

echo ============================================================
echo [1/4] Compiling crypto modules...
echo ============================================================
cl.exe /nologo /utf-8 /O2 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SRCDIR%src" /I"%SRCDIR%src\crypto" /I"%SRCDIR%src\disk" /I"%SRCDIR%src\ntfs" /I"%SRCDIR%src\security" /I"%SRCDIR%src\ui" /I"%SRCDIR%src\vfs" /I"%SRCDIR%src\volume" ^
   /c ^
   "%SRCDIR%src\crypto\aes.cpp" ^
   "%SRCDIR%src\crypto\aes_xts.cpp" ^
   "%SRCDIR%src\crypto\argon2id.cpp" ^
   "%SRCDIR%src\crypto\blake2b.cpp" ^
   "%SRCDIR%src\crypto\random.cpp" ^
   "%SRCDIR%src\crypto\sha256.cpp" ^
   "%SRCDIR%src\disk\disk.cpp" ^
   "%SRCDIR%src\ntfs\ntfs_reader.cpp" ^
   "%SRCDIR%src\security\lockout.cpp" ^
   "%SRCDIR%src\ui\app.cpp" ^
   "%SRCDIR%src\ui\init_wizard.cpp" ^
   "%SRCDIR%src\vfs\vfs.cpp" ^
   "%SRCDIR%src\volume\volume.cpp" ^
   /Fo"%OUT_DIR%\" ^
   2>&1

if errorlevel 1 (
    echo.
    echo *** COMPILE FAILED ***
    pause & exit /b 1
)

echo.
echo ============================================================
echo [2/4] Compiling main.cpp...
echo ============================================================
cl.exe /nologo /utf-8 /O2 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DSDRV_CONSOLE=1 ^
   /I"%SRCDIR%src" /I"%SRCDIR%src\crypto" /I"%SRCDIR%src\disk" /I"%SRCDIR%src\ntfs" /I"%SRCDIR%src\security" /I"%SRCDIR%src\ui" /I"%SRCDIR%src\vfs" /I"%SRCDIR%src\volume" ^
   /c ^
   "%SRCDIR%src\main.cpp" ^
   /Fo"%OUT_DIR%\main.obj" ^
   2>&1

if errorlevel 1 (
    echo.
    echo *** MAIN COMPILE FAILED ***
    pause & exit /b 1
)

echo.
echo ============================================================
echo [3/4] Compiling manifest.rc (icon resource)...
echo ============================================================
rc.exe /fo "%OUT_DIR%\manifest.res" "%SRCDIR%platform\windows\manifest.rc" 2>&1
if errorlevel 1 (
    echo [WARN] RC failed, trying with full path...
    "%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\rc.exe" /fo "%OUT_DIR%\manifest.res" "%SRCDIR%platform\windows\manifest.rc" 2>&1
)

echo.
echo ============================================================
echo [4/4] Linking SecureDrive.exe...
echo ============================================================
link.exe /NOLOGO /MACHINE:x64 /SUBSYSTEM:CONSOLE ^
   "%OUT_DIR%\aes.obj" ^
   "%OUT_DIR%\aes_xts.obj" ^
   "%OUT_DIR%\argon2id.obj" ^
   "%OUT_DIR%\blake2b.obj" ^
   "%OUT_DIR%\random.obj" ^
   "%OUT_DIR%\sha256.obj" ^
   "%OUT_DIR%\disk.obj" ^
   "%OUT_DIR%\ntfs_reader.obj" ^
   "%OUT_DIR%\lockout.obj" ^
   "%OUT_DIR%\app.obj" ^
   "%OUT_DIR%\init_wizard.obj" ^
   "%OUT_DIR%\vfs.obj" ^
   "%OUT_DIR%\volume.obj" ^
   "%OUT_DIR%\main.obj" ^
   "%OUT_DIR%\manifest.res" ^
   wldap32.lib Advapi32.lib User32.lib Gdi32.lib Shell32.lib Comdlg32.lib ComCtl32.lib OpenGL32.lib Glfw3dll.lib ^
   /OUT:"%OUT_DIR%\SecureDrive.exe" ^
   2>&1

if errorlevel 1 (
    echo.
    echo *** LINK FAILED ***
    pause & exit /b 1
)

echo.
echo ============================================================
echo BUILD SUCCESS!
echo ============================================================
if exist "%OUT_DIR%\SecureDrive.exe" (
    echo Output: %OUT_DIR%\SecureDrive.exe
    dir "%OUT_DIR%\SecureDrive.exe" | find ".exe"
)
echo.
pause
