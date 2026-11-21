<#
.SYNOPSIS
    用户态单元测试：在桌面程序中编译 src\aivcloopback.cpp（内核 API 用 tests\loopback\sysvad.h 打桩），
    模拟渲染/采集按时间推进，检查格式转换、44.1k<->48k 重采样、预缓冲延迟、掉音、断点跳变、静音与多读者。
#>
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$src  = Join-Path $here '..\..\src' | Resolve-Path
$out  = Join-Path $env:TEMP 'aivc_looptest'
New-Item -ItemType Directory -Force $out | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = & $vswhere -latest -prerelease -products * -property installationPath
$vcvars  = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && cl /nologo /EHsc /W4 /O2 /utf-8 /I`"$here`" /I`"$src`" `"$here\test.cpp`" `"$src\aivcloopback.cpp`" /Fo`"$out\\`" /Fe`"$out\looptest.exe`""
if ($LASTEXITCODE) { throw "编译失败" }
& "$out\looptest.exe"
exit $LASTEXITCODE
