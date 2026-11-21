<#
.SYNOPSIS
    卸载 AI 变声器虚拟声卡驱动。

.DESCRIPTION
      1. 删除所有硬件 ID 为 ROOT\AIVoiceChanger 的设备节点（等同 devcon remove）
      2. 从驱动商店删除对应的 oemNN.inf 驱动包（pnputil /delete-driver /uninstall /force）
      3. 可选：-RemoveCertificate 删除 install_test.ps1 导入的测试证书
      4. 可选：-DisableTestSigning 执行 bcdedit /set testsigning off（需重启）

.EXAMPLE
    # 以管理员身份：
    .\uninstall.ps1 -RemoveCertificate
#>
[CmdletBinding()]
param(
    [switch]$RemoveCertificate,
    [switch]$DisableTestSigning
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts\AivcSetupApi.ps1')

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }

if (-not (Test-AivcAdmin)) {
    Write-Host "请右键“以管理员身份运行” PowerShell 后再执行本脚本。" -ForegroundColor Red
    exit 1
}

Step "删除设备节点 $AivcHardwareId"
$n = [AivcSetupApi]::RemoveDevices($AivcHardwareId)
Write-Host "已删除 $n 个设备节点"

Step "从驱动商店删除驱动包"
$oems = @(Get-AivcOemInf)
if ($oems.Count -eq 0) {
    Write-Host "驱动商店中未找到本驱动包"
}
foreach ($oem in $oems) {
    & pnputil /delete-driver $oem /uninstall /force
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) {
        Write-Host "删除 $oem 失败（退出码 $LASTEXITCODE）" -ForegroundColor Yellow
    } else {
        Write-Host "已删除 $oem"
    }
}

if ($RemoveCertificate) {
    Step "删除测试证书"
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $AivcCertSubject } | ForEach-Object {
            Remove-Item $_.PSPath
            Write-Host "已从 LocalMachine\$store 删除 $($_.Thumbprint)"
        }
    }
}

if ($DisableTestSigning) {
    Step "关闭测试签名模式"
    & bcdedit /set testsigning off
    Write-Host "已关闭，重启后生效。" -ForegroundColor Yellow
} else {
    Write-Host "`n提示：如不再需要测试签名模式，可执行  bcdedit /set testsigning off  并重启。"
}

Write-Host "`n卸载完成。" -ForegroundColor Green
