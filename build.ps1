Push-Location 'C:\Users\yzj32\WorkBuddy\20260529083812\SecureDrive'

# Load VS environment
$vsPath = 'C:\Program Files\Microsoft Visual Studio\18\Community'
$vcvars = "$vsPath\VC\Auxiliary\Build\vcvarsall.bat"
$tempBat = [System.IO.Path]::GetTempFileName()
$envFile = [System.IO.Path]::GetTempFileName()
"@echo off`ncall `"$vcvars`" x64 >nul 2>&1`nset > `"$envFile`"" | Set-Content $tempBat -Encoding ASCII
cmd /c $tempBat
Get-Content $envFile | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
Remove-Item $tempBat, $envFile -ErrorAction SilentlyContinue

Write-Host "=== Configuring CMake ==="
& cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: cmake configure failed"
    Pop-Location
    exit 1
}

Write-Host "=== Building ==="
& cmake --build build
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: build failed"
    Pop-Location
    exit 1
}

Write-Host "=== Build successful ==="
Pop-Location
