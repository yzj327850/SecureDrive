@echo off
chcp 65001 >nul 2>&1
echo ============================================
echo  SecureDrive Windows 构建脚本
echo  需要：CMake + Visual Studio (含 C++ 工作负载)
echo ============================================

if not exist build_win mkdir build_win
cd build_win
cmake ..
if %errorlevel% neq 0 (
    echo.
    echo CMake 配置失败！请确保已安装 Visual Studio 并勾选 C++ 工作负载。
    cd ..
    pause
    exit /b 1
)
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo.
    echo 编译失败！
    cd ..
    pause
    exit /b 1
)
echo.
echo 构建完成！可执行文件位于 build_win\Release\SecureDrive.exe
echo 注意：运行需要管理员权限（程序会自动通过 UAC 请求）
cd ..
pause
