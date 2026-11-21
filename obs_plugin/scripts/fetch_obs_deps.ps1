# 获取 OBS 30.x 编译依赖：libobs 头文件 + obs.lib 导入库（由 obs.dll 导出表生成）
# 以及（可选）OBS 便携版，用于本地验证插件加载。
#
# 用法：  powershell -ExecutionPolicy Bypass -File scripts\fetch_obs_deps.ps1 [-Version 30.2.3] [-NoPortable]
# 产物：  deps\libobs\include\...   头文件（obs-module.h 等）
#         deps\libobs\lib\obs.lib   导入库
#         deps\obs-portable\        OBS 便携版（bin\64bit\obs64.exe）
param(
    [string]$Version = "30.2.3",
    [switch]$NoPortable
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root "deps"
$dl   = Join-Path $deps "_download"
$tar  = Join-Path $env:SystemRoot "System32\tar.exe"   # 系统自带 bsdtar（可解 zip），避免误用 Git 的 GNU tar
New-Item -ItemType Directory -Force $dl | Out-Null

function Get-File($url, $out) {
    if (Test-Path $out) { Write-Host "已存在 $out"; return }
    Write-Host "下载 $url"
    & curl.exe -fL --retry 3 -o $out $url
    if ($LASTEXITCODE -ne 0) { throw "下载失败: $url" }
}

# 1) 源码 → 头文件
$src = Join-Path $dl "obs-studio-$Version.tar.gz"
Get-File "https://codeload.github.com/obsproject/obs-studio/tar.gz/refs/tags/$Version" $src
$inc = Join-Path $deps "libobs\include"
if (-not (Test-Path (Join-Path $inc "obs-module.h"))) {
    $tmp = Join-Path $dl "src"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    & $tar -xzf $src -C $tmp "obs-studio-$Version/libobs"
    if ($LASTEXITCODE -ne 0) { throw "解压源码失败" }
    $libobs = Join-Path $tmp "obs-studio-$Version\libobs"
    Get-ChildItem $libobs -Recurse -Include *.h,*.hpp | ForEach-Object {
        $rel = $_.FullName.Substring($libobs.Length + 1)
        $dst = Join-Path $inc $rel
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item $_.FullName $dst -Force
    }
    Remove-Item -Recurse -Force $tmp
}

# 2) 便携版 zip（含 obs.dll）
$zip = Join-Path $dl "OBS-Studio-$Version-Windows.zip"
Get-File "https://cdn-fastly.obsproject.com/downloads/OBS-Studio-$Version-Windows.zip" $zip
$portable = Join-Path $deps "obs-portable"
if (-not (Test-Path (Join-Path $portable "bin\64bit\obs.dll"))) {
    New-Item -ItemType Directory -Force $portable | Out-Null
    & $tar -xf $zip -C $portable
    if ($LASTEXITCODE -ne 0) { throw "解压便携版失败" }
    # 便携模式：obs64.exe 同级/上级存在 portable_mode.txt 时配置写入 config\ 而非 %APPDATA%
    New-Item -ItemType File -Force (Join-Path $portable "portable_mode.txt") | Out-Null
}

# 3) obs.dll → obs.def → obs.lib
$libdir = Join-Path $deps "libobs\lib"
New-Item -ItemType Directory -Force $libdir | Out-Null
$lib = Join-Path $libdir "obs.lib"
if (-not (Test-Path $lib)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $msvc = Get-ChildItem (Join-Path $vs "VC\Tools\MSVC") | Sort-Object Name -Descending | Select-Object -First 1
    $bin = Join-Path $msvc.FullName "bin\Hostx64\x64"
    $dll = Join-Path $portable "bin\64bit\obs.dll"
    $exports = & (Join-Path $bin "dumpbin.exe") /nologo /exports $dll
    $names = foreach ($line in $exports) {
        # 形如:  ordinal hint RVA      name
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $Matches[1] }
    }
    if ($names.Count -lt 100) { throw "obs.dll 导出符号过少（$($names.Count)），解析失败" }
    $def = Join-Path $libdir "obs.def"
    (@("LIBRARY obs.dll", "EXPORTS") + ($names | ForEach-Object { "    $_" })) | Set-Content -Encoding ascii $def
    & (Join-Path $bin "lib.exe") /nologo /machine:x64 /def:$def /out:$lib
    if ($LASTEXITCODE -ne 0) { throw "lib.exe 生成导入库失败" }
    Write-Host "已生成 $lib （$($names.Count) 个导出）"
}
Write-Host "依赖就绪：$deps"
