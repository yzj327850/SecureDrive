@echo off
REM ============================================================
REM SecureDrive 直接编译脚本（不依赖 MSBuild/CMake）
REM 使用 cl.exe + nmake 或一次性编译
REM ============================================================

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

echo [1/3] 收集源文件...
set SRC=
set INCLUDES=-I".\src" -I".\src\crypto" -I".\src\disk" -I".\src\ntfs" -I".\src\security" -I".\src\ui" -I".\src\vfs" -I".\src\volume"

echo [2/3] 编译 SecureDrive.exe（工作目录版，带控制台）...
cl.exe /nologo /utf-8 /O2 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DSDRV_CONSOLE=1 %INCLUDES% ^
   .\src\main.cpp ^
   .\src\crypto\aes.cpp ^
   .\src\crypto\aes_xts.cpp ^
   .\src\crypto\argon2id.cpp ^
   .\src\crypto\blake2b.cpp ^
   .\src\crypto\random.cpp ^
   .\src\crypto\sha256.cpp ^
   .\src\disk\disk.cpp ^
   .\src\ntfs\ntfs_reader.cpp ^
   .\src\security\lockout.cpp ^
   .\src\ui\app.cpp ^
   .\src\ui\init_wizard.cpp ^
   .\src\vfs\vfs.cpp ^
   .\src\volume\volume.cpp ^
   .\platform\windows\manifest.rc ^
   /link /MACHINE:x64 /SUBSYSTEM:CONSOLE wldap32.lib Advapi32.lib User32.lib Gdi32.lib Shell32.lib Comdlg32.lib ComCtl32.lib OpenGL32.lib Glfw3dll.lib ^
   /OUT:.\build\SecureDrive.exe ^
   2>&1

if errorlevel 1 (
    echo.
    echo *** 编译失败！***
    pause
    exit /b 1
)

echo.
echo [3/3] 编译成功！
echo   工作目录版: .\build\SecureDrive.exe
dir /b ".\build\SecureDrive.exe" 2>nul && (
    echo.
    echo 文件大小:
    dir ".\build\SecureDrive.exe" | find ".exe"
)
echo.
pause
