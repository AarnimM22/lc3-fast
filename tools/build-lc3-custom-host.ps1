param([ValidateRange(0,63)][int]$Optimizations=0, [string]$OutputDirectory='.bench\lc3-custom-host')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root $OutputDirectory
$src = Join-Path $root 'firmware\third_party\liblc3'
$support = Join-Path $root 'firmware\benchmark\src'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' "$PSScriptRoot\prepare-lc3-custom.py" --source "$src\src" --output "$out\generated"
if ($LASTEXITCODE -ne 0) { throw 'Custom source generation failed' }
$vs = & 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual C++ compiler not found' }
$files = @((Get-ChildItem -LiteralPath "$out\generated" -Filter '*.c').FullName)
$files += "$support\lc3_custom.c", "$support\lc3_lite.c"
$files = ($files | ForEach-Object { '"'+$_+'"' }) -join ' '
$exports = 'encoder_size','setup_encoder','encoder_disable_ltpf','encode','decoder_size','setup_decoder','decode','delay_samples','custom_configure','custom_encode','custom_decode'
$link = ($exports | ForEach-Object { '/EXPORT:lc3_'+$_ }) -join ' '
$batch = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /O2 /fp:fast /MT /LD /D_CRT_SECURE_NO_WARNINGS /DBENCH_OPTIMIZATIONS=$Optimizations /DLC3_PLUS=1 /DLC3_PLUS_HR=1 /I"$src\include" /I"$out\generated" /I"$support" $files /link /OUT:lc3-custom.dll $link /EXPORT:custom_cap_frames,DATA /EXPORT:custom_sns_clips,DATA
"@
$path = Join-Path $out 'build.cmd'
Set-Content -LiteralPath $path -Value $batch -Encoding ascii
Push-Location $out
try {
    & $env:ComSpec /d /c $path
    if ($LASTEXITCODE -ne 0) { throw "Native custom build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
