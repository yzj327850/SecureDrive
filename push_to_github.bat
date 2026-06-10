@echo off
chcp 65001 >nul
<<<<<<< HEAD
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
=======
title Push SecureDrive to GitHub
echo ==========================================
echo   SecureDrive GitHub 推送工具
echo ==========================================
echo.

REM 检查是否在正确的目录
echo [1/3] 检查项目目录...
if not exist "CMakeLists.txt" (
    echo 错误：未找到 CMakeLists.txt，请确保在此批处理文件所在目录运行。
    pause
    exit /b 1
)
echo      OK
echo.

REM 添加远程仓库
echo [2/3] 配置远程仓库...
git remote get-url origin >nul 2>&1
if %errorlevel% neq 0 (
    git remote add origin https://github.com/yzj327850/SecureDrive.git
    echo      已添加远程仓库: https://github.com/yzj327850/SecureDrive.git
) else (
    echo      远程仓库已存在
)
echo.

REM 推送到 GitHub
echo [3/3] 推送到 GitHub...
echo      正在上传代码，请稍候...
echo      如果提示输入用户名密码，请输入你的 GitHub 凭据。
echo      建议使用 Personal Access Token 作为密码。
>>>>>>> 94669b0983ee94d61e25dec671a2d820f353fbe9
echo.
git push -u origin main

if %errorlevel% equ 0 (
    echo.
    echo ==========================================
<<<<<<< HEAD
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
=======
    echo   推送成功！
    echo ==========================================
    echo.
    echo 你的代码已上传到：
    echo   https://github.com/yzj327850/SecureDrive
    echo.
    echo 下一步：
    echo   1. 打开上面的链接查看仓库
    echo   2. 点击 Actions 标签查看自动构建进度
    echo   3. 等待约 5-10 分钟后下载各平台二进制文件
    echo.
    echo 要发布版本，请运行：
>>>>>>> 94669b0983ee94d61e25dec671a2d820f353fbe9
    echo   git tag v1.0.0
    echo   git push origin v1.0.0
    echo.
) else (
    echo.
    echo ==========================================
<<<<<<< HEAD
    echo   Push Failed
    echo ==========================================
    echo.
    echo Possible reasons:
    echo   - Network issues
    echo   - Wrong GitHub credentials
    echo   - Repository conflicts
    echo.
    echo Please check the error message and retry.
=======
    echo   推送失败
    echo ==========================================
    echo.
    echo 可能的原因：
    echo   - 网络连接问题
    echo   - GitHub 凭据错误
    echo   - 仓库已存在冲突
    echo.
    echo 请检查错误信息并重试。
>>>>>>> 94669b0983ee94d61e25dec671a2d820f353fbe9
    echo.
)

pause
