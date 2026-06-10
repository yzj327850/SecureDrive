$ErrorActionPreference = "Stop"

$msvcRoot  = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$sdkRoot   = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer    = "10.0.26100.0"

# 设置 MSVC 编译环境
$env:PATH    = "$msvcRoot\bin\Hostx64\x64;" + $env:PATH
$env:INCLUDE = "$msvcRoot\include;$sdkRoot\Include\$sdkVer\ucrt;$sdkRoot\Include\$sdkVer\um;$sdkRoot\Include\$sdkVer\shared"
$env:LIB     = "$msvcRoot\lib\x64;$sdkRoot\Lib\$sdkVer\ucrt\x64;$sdkRoot\Lib\$sdkVer\um\x64"
$env:VCTargetsPath = "$msvcRoot\Targets"

$buildDir = "C:\Users\yzj32\WorkBuddy\20260529083812\SecureDrive\build"
$srcDir    = "C:\Users\yzj32\WorkBuddy\20260529083812\SecureDrive"

Write-Host "=== 环境检查 ==="
& "$msvcRoot\bin\Hostx64\x64\cl.exe" /? 2>&1 | Select-Object -First 2 | Write-Host
& "$msvcRoot\bin\Hostx64\x64\nmake.exe" /? 2>&1 | Select-Object -First 1 | Write-Host

Write-Host ""
Write-Host "=== CMake Configure (NMake Makefiles) ==="
if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
& "C:\Program Files\CMake\bin\cmake.exe" -B $buildDir -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$msvcRoot\bin\Hostx64\x64\cl.exe" -DCMAKE_CXX_COMPILER="$msvcRoot\bin\Hostx64\x64\cl.exe" -DSDRV_CONSOLE=ON 2>&1 | Write-Host

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMAKE CONFIGURE FAILED (exit $LASTEXITCODE)"
    exit 1
}

Write-Host ""
Write-Host "=== Build ==="
& "C:\Program Files\CMake\bin\cmake.exe" --build $buildDir --config Release 2>&1 | Write-Host

if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED (exit $LASTEXITCODE)"
    exit 1
}

Write-Host ""
Write-Host "=== BUILD SUCCESS ==="
if (Test-Path "$buildDir\SecureDrive.exe") {
    $file = Get-Item "$buildDir\SecureDrive.exe"
    Write-Host "输出: $($file.FullName)  ($([math]::Round($file.Length/1KB)) KB)"
}
