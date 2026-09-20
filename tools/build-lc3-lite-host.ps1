$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.bench\lc3-lite-host'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$src = Join-Path $root 'firmware\third_party\liblc3'
$support = Join-Path $root 'firmware\benchmark\src'
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' "$PSScriptRoot\prepare-lc3-lite.py" --source "$src\src" --output "$out\generated"
if ($LASTEXITCODE -ne 0) { throw 'LC3 source generation failed' }
$vs = & 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual C++ compiler not found' }
$exports = 'encoder_size','setup_encoder','encoder_disable_ltpf','encode','decoder_size','setup_decoder','decode','delay_samples'
$link = ($exports | ForEach-Object { '/EXPORT:lc3_' + $_ }) -join ' '
$commands = @()
foreach ($variant in 'baseline','lite') {
    $files = @(Get-ChildItem -LiteralPath "$src\src" -Filter '*.c' | ForEach-Object {
        if ($variant -eq 'lite' -and $_.BaseName -in @('lc3','tns','spec')) { '"' + "$out\generated\$($_.Name)" + '"' }
        else { '"' + $_.FullName + '"' }
    })
    $extra = ''
    if ($variant -eq 'lite') {
        $files += '"' + "$support\lc3_lite.c" + '"'
        $extra = '/EXPORT:lc3_lite_set_flags'
    }
    $commands += "cl /nologo /std:c11 /O2 /fp:fast /MT /LD /D_CRT_SECURE_NO_WARNINGS /DLC3_PLUS=1 /DLC3_PLUS_HR=1 /I`"$src\include`" /I`"$src\src`" /I`"$support`" $($files -join ' ') /link /OUT:lc3-$variant.dll $link $extra"
    $commands += 'if errorlevel 1 exit /b 1'
}
$batch = "@echo off`r`ncall `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul`r`nif errorlevel 1 exit /b 1`r`n" + ($commands -join "`r`n")
$path = Join-Path $out 'build.cmd'
Set-Content -LiteralPath $path -Value $batch -Encoding ascii
Push-Location $out
try {
    & $env:ComSpec /d /c $path
    if ($LASTEXITCODE -ne 0) { throw "Native LC3 build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
