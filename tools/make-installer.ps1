# 构建 Setup.exe 安装包（Inno Setup）
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File .\tools\make-installer.ps1
#
# 前置条件：
#   - Visual Studio（msbuild）能构建 Release|x64
#   - Inno Setup 6/7 的命令行编译器 ISCC.exe 可用。脚本会依次查找：
#       PATH → %LOCALAPPDATA%\Programs\InnoSetupNuget\*\tools\ISCC.exe
#            → C:\Program Files (x86)\Inno Setup 6\ISCC.exe → C:\Program Files\Inno Setup 7\ISCC.exe
#     没有的话可以用 NuGet 包免安装获取：下载 tools.innosetup 的 .nupkg，改扩展名为 .zip
#     解压，ISCC.exe 就在解压目录的 tools\ 下（本项目就是这么装的，不写系统目录）。
#
# 产物：dist\3dPointCloudEditor-<版本>-setup.exe
param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$project = Join-Path $root '3dPointCloudEditor\3dPointCloudEditor.vcxproj'
$manifest = Join-Path $root '3dPointCloudEditor\Package.appxmanifest'
$iss = Join-Path $root 'tools\installer\Setup.iss'
$dist = Join-Path $root 'dist'

function Find-ISCC {
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @()
    $candidates += Get-ChildItem "${env:LOCALAPPDATA}\Programs\InnoSetupNuget\*\tools\ISCC.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -ExpandProperty FullName
    $candidates += 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
    $candidates += 'C:\Program Files\Inno Setup 6\ISCC.exe'
    $candidates += 'C:\Program Files\Inno Setup 7\ISCC.exe'
    foreach ($candidate in $candidates) { if (Test-Path $candidate) { return $candidate } }
    return $null
}

$msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
if ($msbuild) { $msbuild = $msbuild.Source }
else {
    foreach ($candidate in @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe')) {
        if (Test-Path $candidate) { $msbuild = $candidate; break }
    }
}
if (-not $msbuild) { throw 'MSBuild.exe 未找到，请从 Visual Studio Developer PowerShell 运行' }

$iscc = Find-ISCC
if (-not $iscc) {
    throw '没有找到 ISCC.exe（Inno Setup 命令行编译器）。请安装 Inno Setup，或按脚本开头的说明用 NuGet 包获取。'
}

# 版本号：Package.appxmanifest 的 1.0.0.0 -> 1.0.0
$version = '1.0.0'
$match = Select-String -Path $manifest -Pattern 'Version="([0-9]+\.[0-9]+\.[0-9]+)\.[0-9]+"'
if ($match) { $version = $match.Matches[0].Groups[1].Value }

Write-Host "构建应用（$Configuration|$Platform）…"
& $msbuild $project /m /nologo /v:minimal /p:Configuration=$Configuration /p:Platform=$Platform
if ($LASTEXITCODE -ne 0) { throw "构建失败，退出码 $LASTEXITCODE" }

$buildOut = Join-Path $root ("{0}\{1}" -f $Platform, $Configuration)
if (-not (Test-Path (Join-Path $buildOut '3dPointCloudEditor.exe'))) {
    throw "没有找到 $buildOut\3dPointCloudEditor.exe"
}

New-Item -ItemType Directory -Force -Path $dist | Out-Null
Write-Host "打包安装程序（版本 $version）…"
& $iscc $iss "/DAppVersion=$version" "/DBuildDir=$buildOut"
if ($LASTEXITCODE -ne 0) { throw "ISCC 编译失败，退出码 $LASTEXITCODE" }

$setup = Get-ChildItem $dist -Filter "3dPointCloudEditor-$version-setup.exe" | Select-Object -First 1
if (-not $setup) { throw '没有找到生成的 setup.exe' }

# 关键：把 exe 的完整性级别设为 Medium。
# 若文件带着 Low Mandatory Level（在沙箱/受限环境里构建时会留下），
# 从它启动的进程也是低完整性，无法创建 %TEMP%\is-*.tmp 和用户目录，
# 用户双击时会看到“安装程序无法创建目录 … 错误 5: 拒绝访问”。
& icacls $setup.FullName /setintegritylevel Medium | Out-Null

Write-Host ''
Write-Host ("完成：{0}（{1:N1} MB）" -f $setup.FullName, ($setup.Length / 1MB))
Write-Host '双击即可安装；默认装到 %LocalAppData%\Programs\3dPointCloudEditor，不需要管理员权限。'
