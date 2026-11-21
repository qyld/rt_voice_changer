<#
.SYNOPSIS
    【测试签名版】安装 AI 变声器虚拟声卡驱动。

.DESCRIPTION
    在“测试签名模式”下安装 build.ps1 生成的驱动包：
      1. 检查管理员权限、系统版本（Windows 10 2004 / 19041 及以上，x64）
      2. 检查测试签名模式（bcdedit testsigning）与安全启动状态；
         未开启时给出提示后退出（加 -EnableTestSigning 可由脚本代为执行 bcdedit，需重启）
      3. 把测试证书导入 本地计算机\受信任的根证书颁发机构 和 受信任的发布者
      4. pnputil /add-driver 把驱动加入驱动商店
      5. 创建 ROOT\AIVoiceChanger 设备节点并安装驱动（等同 devcon install）
      6. 显示安装结果与新出现的音频端点

    正式版（EV 证书 + 微软证明签名）不需要测试签名模式，见 SIGNING.md。

.PARAMETER PackageDir
    驱动包目录（含 AIVirtualAudio.inf/.sys/.cat 与 AIVoiceChangerTest.cer），默认 .\package

.PARAMETER EnableTestSigning
    若测试签名模式未开启，执行 bcdedit /set testsigning on（之后需要手动重启再运行本脚本）。

.EXAMPLE
    # 以管理员身份打开 PowerShell：
    Set-ExecutionPolicy -Scope Process Bypass
    .\install_test.ps1
#>
[CmdletBinding()]
param(
    [string]$PackageDir = (Join-Path $PSScriptRoot 'package'),
    [switch]$EnableTestSigning
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts\AivcSetupApi.ps1')

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "`n[失败] $msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- 1. 环境检查
Step "检查运行环境"
if (-not (Test-AivcAdmin)) { Fail "请右键“以管理员身份运行” PowerShell 后再执行本脚本。" }
if (-not [Environment]::Is64BitOperatingSystem) { Fail "仅支持 64 位 Windows。" }
$build = [Environment]::OSVersion.Version.Build
if ($build -lt 19041) { Fail "需要 Windows 10 2004 (19041) 或更高版本，当前内部版本 $build。" }
Write-Host "Windows 内部版本 $build，x64，管理员权限 OK"

$inf = Join-Path $PackageDir 'AIVirtualAudio.inf'
$cer = Join-Path $PackageDir 'AIVoiceChangerTest.cer'
foreach ($f in $inf, (Join-Path $PackageDir 'AIVirtualAudio.sys'), (Join-Path $PackageDir 'AIVirtualAudio.cat'), $cer) {
    if (-not (Test-Path $f)) { Fail "缺少文件：$f（请先在开发机运行 build.ps1，并把整个 package 目录拷过来）" }
}
$inf = (Resolve-Path $inf).Path

# ---------------------------------------------------------------- 2. 测试签名模式
Step "检查测试签名模式"
$secureBoot = $null
try { $secureBoot = Confirm-SecureBootUEFI } catch { $secureBoot = $null }   # 非 UEFI/不支持时为 $null
$bcd = (& bcdedit /enum '{current}') -join "`n"
$testSigningOn = $bcd -match '(?im)^\s*testsigning\s+(Yes|是)\s*$'

if (-not $testSigningOn) {
    Write-Host "当前未开启测试签名模式。" -ForegroundColor Yellow
    if ($secureBoot -eq $true) {
        Write-Host "检测到【安全启动 Secure Boot】已开启：开启测试签名前必须先在 BIOS/UEFI 设置里关闭 Secure Boot。" -ForegroundColor Yellow
    }
    if ($EnableTestSigning) {
        & bcdedit /set testsigning on
        if ($LASTEXITCODE) { Fail "bcdedit /set testsigning on 执行失败（Secure Boot 未关闭时会失败）。" }
        Write-Host "`n已开启测试签名模式。请【重启电脑】后再次运行本脚本。重启后桌面右下角会出现“测试模式”水印，属正常现象。" -ForegroundColor Green
        exit 0
    }
    Write-Host @"

请按以下步骤操作后重新运行本脚本：
  1. （如开启了 Secure Boot）进入 BIOS/UEFI 关闭 Secure Boot
  2. 以管理员身份执行： bcdedit /set testsigning on
     （或直接运行： .\install_test.ps1 -EnableTestSigning ）
  3. 重启电脑
"@
    exit 2
}
Write-Host "测试签名模式：已开启"

# ---------------------------------------------------------------- 3. 导入测试证书
Step "导入测试证书到 本地计算机\Root 与 TrustedPublisher"
foreach ($store in 'Root', 'TrustedPublisher') {
    Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    Write-Host "已导入到 LocalMachine\$store"
}

# ---------------------------------------------------------------- 4. 加入驱动商店
Step "pnputil /add-driver"
& pnputil /add-driver $inf
if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) { Fail "pnputil /add-driver 失败（退出码 $LASTEXITCODE）。" }

# ---------------------------------------------------------------- 5. 创建设备并安装
Step "创建虚拟设备 $AivcHardwareId 并安装驱动"
$existing = [AivcSetupApi]::FindDevices($AivcHardwareId)
if ($existing.Count -eq 0) {
    $id = [AivcSetupApi]::CreateRootDevice('MEDIA', $AivcMediaClassGuid, $AivcHardwareId, 'AI变声器 虚拟声卡')
    Write-Host "已创建设备节点：$id"
} else {
    Write-Host "设备节点已存在：$($existing -join ', ')，将更新驱动"
}
$reboot = [AivcSetupApi]::UpdateDriver($AivcHardwareId, $inf)

# ---------------------------------------------------------------- 6. 结果
Step "安装结果"
Start-Sleep -Seconds 2
Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue |
    Where-Object { $_.HardwareID -contains $AivcHardwareId } |
    Format-Table Status, FriendlyName, InstanceId -AutoSize | Out-String | Write-Host

Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -like '*AI变声器*' } |
    Format-Table Status, FriendlyName -AutoSize | Out-String | Write-Host

if ($reboot) { Write-Host "系统提示需要重启才能完成安装。" -ForegroundColor Yellow }
Write-Host @"
完成。请打开“声音设置”确认：
  播放设备：AI变声器 内部输出（变声器输出选它）
  录制设备：AI变声器 麦克风（抖音直播伴侣/OBS 的麦克风选它）
若设备状态为 Error，请在“设备管理器 → 声音、视频和游戏控制器 → AI变声器 虚拟声卡 → 属性”查看错误代码（52 = 签名不受信任）。
"@ -ForegroundColor Green
