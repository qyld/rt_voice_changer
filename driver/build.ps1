<#
.SYNOPSIS
    编译 AI 变声器虚拟声卡驱动，生成 .sys/.inf/.cat 并用测试证书签名。

.DESCRIPTION
    步骤：
      1. nuget restore（WDK/SDK 以 NuGet 包形式放在 driver\packages，无需安装 WDK）
      2. MSBuild 编译 src\AIVirtualAudio\AIVirtualAudio.vcxproj
      3. 复制 .sys/.inf 到 driver\package\
      4. InfVerif 校验 INF
      5. Inf2Cat 生成 .cat
      6. 创建/复用测试代码签名证书（CurrentUser\My），用 signtool 签名 .sys 与 .cat，
         并导出公钥证书 package\AIVoiceChangerTest.cer（给 install_test.ps1 导入用）

    只在开发机运行；不修改 bcdedit、不安装驱动。

.PARAMETER Configuration
    Release（默认）或 Debug。
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$CertSubject = 'CN=AIVoiceChanger Test Signing (DO NOT TRUST)',
    [switch]$SkipSign
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$WdkVer = '10.0.28000.2526'
$Pkgs = Join-Path $Root 'packages'
$OutPackage = Join-Path $Root 'package'

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }

# ---------------------------------------------------------------- 1. NuGet
Step "还原 NuGet 包（WDK $WdkVer）"
if (-not (Test-Path (Join-Path $Pkgs "Microsoft.Windows.WDK.x64.$WdkVer"))) {
    $nuget = (Get-Command nuget -ErrorAction SilentlyContinue).Source
    if (-not $nuget) {
        $nuget = Join-Path $env:TEMP 'nuget.exe'
        if (-not (Test-Path $nuget)) {
            Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nuget
        }
    }
    & $nuget restore (Join-Path $Root 'packages.config') -PackagesDirectory $Pkgs
    if ($LASTEXITCODE) { throw "nuget restore 失败" }
} else {
    Write-Host "已存在，跳过"
}

$WdkBin   = Join-Path $Pkgs "Microsoft.Windows.WDK.x64.$WdkVer\c\bin\10.0.28000.0"
$WdkTools = Join-Path $Pkgs "Microsoft.Windows.WDK.x64.$WdkVer\c\tools\10.0.28000.0\x64"
$SdkBin   = Join-Path $Pkgs "Microsoft.Windows.SDK.CPP.$WdkVer\c\bin\10.0.28000.0\x64"
$InfVerif = Join-Path $WdkTools 'infverif.exe'
$Inf2Cat  = Join-Path $WdkBin  'x86\Inf2Cat.exe'
$SignTool = Join-Path $SdkBin  'signtool.exe'

# ---------------------------------------------------------------- 2. MSBuild
Step "编译驱动（$Configuration|x64）"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw "未找到 MSBuild（需要 Visual Studio 2022/2026 + C++ 桌面开发 + Spectre 缓解库可选）" }
& $msbuild (Join-Path $Root 'src\AIVirtualAudio\AIVirtualAudio.vcxproj') `
    /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /v:m /t:Rebuild
if ($LASTEXITCODE) { throw "编译失败" }

$BuildOut = Join-Path $Root "build\x64\$Configuration"
$BuildInf = Join-Path $Root "build\obj\x64\$Configuration\AIVirtualAudio.inf"

# ---------------------------------------------------------------- 3. 打包
Step "复制到 $OutPackage"
New-Item -ItemType Directory -Force $OutPackage | Out-Null
Remove-Item (Join-Path $OutPackage 'AIVirtualAudio.*') -ErrorAction SilentlyContinue
Copy-Item (Join-Path $BuildOut 'AIVirtualAudio.sys') $OutPackage
Copy-Item (Join-Path $BuildOut 'AIVirtualAudio.pdb') $OutPackage -ErrorAction SilentlyContinue
Copy-Item $BuildInf $OutPackage

# ---------------------------------------------------------------- 4. InfVerif
Step "InfVerif 校验"
& $InfVerif /w /v (Join-Path $OutPackage 'AIVirtualAudio.inf')
if ($LASTEXITCODE) { throw "InfVerif /w 失败" }
& $InfVerif /h (Join-Path $OutPackage 'AIVirtualAudio.inf')
if ($LASTEXITCODE) { throw "InfVerif /h 失败" }

if ($SkipSign) { Write-Host "`n已跳过签名。"; return }

# ---------------------------------------------------------------- 5. 签名 .sys
Step "准备测试证书"
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date).AddDays(30) } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
        -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 `
        -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
    Write-Host "已创建新测试证书：$($cert.Thumbprint)"
} else {
    Write-Host "复用测试证书：$($cert.Thumbprint)"
}
Export-Certificate -Cert $cert -FilePath (Join-Path $OutPackage 'AIVoiceChangerTest.cer') -Type CERT | Out-Null

function Sign-File($path) {
    & $SignTool sign /v /fd sha256 /sha1 $cert.Thumbprint /s My $path
    if ($LASTEXITCODE) { throw "signtool 签名失败：$path" }
}

Step "签名 AIVirtualAudio.sys"
Sign-File (Join-Path $OutPackage 'AIVirtualAudio.sys')

# ---------------------------------------------------------------- 6. Inf2Cat + 签名 .cat
Step "Inf2Cat 生成 AIVirtualAudio.cat"
& $Inf2Cat /driver:$OutPackage /os:10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64,ServerFE_X64 /uselocaltime
if ($LASTEXITCODE) { throw "Inf2Cat 失败" }
# Inf2Cat 输出小写文件名，统一成与 INF 中 CatalogFile 一致的大小写
$catLower = Join-Path $OutPackage 'aivirtualaudio.cat'
if (Test-Path $catLower) { Rename-Item $catLower 'AIVirtualAudio.cat.tmp'; Rename-Item (Join-Path $OutPackage 'AIVirtualAudio.cat.tmp') 'AIVirtualAudio.cat' }

Step "签名 AIVirtualAudio.cat"
Sign-File (Join-Path $OutPackage 'AIVirtualAudio.cat')

Step "检查签名"
# 本机未信任测试根证书，因此 signtool verify /pa 会以“根证书不受信任”结束，这是预期的；
# 这里只确认：.sys 已在 .cat 中登记，且两者签名者都是本测试证书。
$verifyOut = & $SignTool verify /v /pa /c (Join-Path $OutPackage 'AIVirtualAudio.cat') (Join-Path $OutPackage 'AIVirtualAudio.sys') 2>&1
$global:LASTEXITCODE = 0
if (-not ($verifyOut -match 'File is signed in catalog')) { throw ".sys 未在 .cat 中找到对应哈希" }
Write-Host ".sys 哈希已登记在 AIVirtualAudio.cat 中"
foreach ($f in 'AIVirtualAudio.sys', 'AIVirtualAudio.cat') {
    $sig = Get-AuthenticodeSignature (Join-Path $OutPackage $f)
    if ($sig.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw "$f 签名者不正确" }
    Write-Host ("{0}: 签名者 {1}（状态 {2}，测试根未受信任时为 UnknownError 属正常）" -f $f, $sig.SignerCertificate.Subject, $sig.Status)
}

Write-Host "`n完成。驱动包位于：$OutPackage" -ForegroundColor Green
Get-ChildItem $OutPackage | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Host
