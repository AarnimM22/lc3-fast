$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.bench\usb-pcm-host'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$vs = & 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual C++ compiler not found' }
$batch = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W4 "$PSScriptRoot\usb-pcm-host.cpp" /Fe:usb-pcm-host.exe /link ole32.lib uuid.lib avrt.lib
"@
$path = Join-Path $out 'build.cmd'
Set-Content -LiteralPath $path -Value $batch -Encoding ascii
Push-Location $out
try {
    & $env:ComSpec /d /c $path
    if ($LASTEXITCODE -ne 0) { throw "USB host build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
