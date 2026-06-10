#!/bin/bash
echo "============================================"
echo " SecureDrive macOS 构建脚本"
echo " 需要：CMake + Xcode Command Line Tools"
echo "============================================"

mkdir -p build_mac
cd build_mac
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(sysctl -n hw.ncpu)
echo ""
echo "构建完成！可执行文件位于 build_mac/SecureDrive.app/Contents/MacOS/SecureDrive"
echo "运行方式：sudo ./build_mac/SecureDrive"
cd ..
