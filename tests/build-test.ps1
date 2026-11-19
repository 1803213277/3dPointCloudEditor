# 编译并运行自检程序（PLY/PCD 解析、相机与选择几何）
#   在 Developer PowerShell 里运行：
#   powershell -ExecutionPolicy Bypass -File .\tests\build-test.ps1
$ErrorActionPreference = 'Stop'

$tests = $PSScriptRoot
$root = Split-Path $tests -Parent
$app = Join-Path $root '3dPointCloudEditor'
$samples = Join-Path $root 'samples'

Push-Location $tests
try {
    cl /nologo /std:c++20 /EHsc /MD /utf-8 /O2 /I"$app" `
        "PlyTest.cpp" "$app\PlyLoader.cpp" `
        /Fe:"PlyTest.exe" /link /SUBSYSTEM:CONSOLE
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    cl /nologo /std:c++20 /EHsc /MD /utf-8 /O2 /I"$app" `
        "CameraTest.cpp" `
        /Fe:"CameraTest.exe" /link /SUBSYSTEM:CONSOLE
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== PLY / PCD parser tests ==="
    & ".\PlyTest.exe" --out-dir (Join-Path $tests 'scratch') `
        (Join-Path $samples 'sphere_ascii.ply') `
        (Join-Path $samples 'wave_binary_le.ply') `
        (Join-Path $samples 'helix_binary_be.ply')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "=== camera / selection tests ==="
    & ".\CameraTest.exe"
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
