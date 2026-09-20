$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.bench\wavpack-host'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$vs = & 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual C++ compiler not found' }
$src = Join-Path $root 'firmware\third_party\wavpack-stream'
$units = 'common_utils','decorr_tables','decorr_utils','entropy_utils','extra1','extra2','pack','pack_dns','pack_floats','pack_utils','write_words','open_utils','open_raw','read_words','unpack','unpack_floats','unpack_utils'
$files = ($units | ForEach-Object { '"' + (Join-Path $src "src\$_.c") + '"' }) -join ' '
$exports = 'OpenFileOutput','SetConfiguration64','PackInit','PackSamples','FlushSamples','CloseFile','OpenRawDecoder','UnpackSamples','GetNumErrors','GetBitsPerSample','GetSampleRate','GetNumChannels'
$link = ($exports | ForEach-Object { '/EXPORT:WavpackStream' + $_ }) -join ' '
$batch = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /LD /D_CRT_SECURE_NO_WARNINGS /I"$src\include" $files /link /OUT:wavpack-stream.dll $link
"@
$path = Join-Path $out 'build.cmd'
Set-Content -LiteralPath $path -Value $batch -Encoding ascii
Push-Location $out
try {
    & $env:ComSpec /d /c $path
    if ($LASTEXITCODE -ne 0) { throw "Native library build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
