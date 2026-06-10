#!/bin/bash
echo "============================================"
echo " SecureDrive Linux 构建脚本"
echo " 需要：CMake + GCC/Clang + libGL-dev + xorg-dev"
echo "============================================"

# 安装依赖（Debian/Ubuntu）
if command -v apt-get &> /dev/null; then
    echo "检测到 apt，安装依赖..."
    sudo apt-get install -y cmake g++ libgl1-mesa-dev xorg-dev libblkid-dev
fi

mkdir -p build_linux
cd build_linux
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
echo ""
echo "构建完成！可执行文件位于 build_linux/SecureDrive"
echo "运行方式：sudo ./build_linux/SecureDrive"
cd ..
