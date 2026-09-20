[CmdletBinding()]
param(
    [ValidateSet('list','build','flash','reset','rtt')][string]$Action = 'list',
    [ValidateSet('tx','tx54','rx')][string]$Target = 'tx',
    [ValidateSet('etsi','etsi-lto','google','google-lto','google-lite-lto','google-custom-lto','opus-float-lto','opus-fixed-lto','opus-fixed16-lto','sbc-lto','wavpack-lto')][string]$Codec = 'etsi',
    [int]$Seconds = 45,
    [int]$Frames = 200,
    [int]$SoakFrames = 0,
    [ValidateRange(16000,512000)][int]$Bitrate = 320000,
    [ValidateSet(16,24)][int]$PcmBits = 16,
    [string]$StopOn = '',
    [switch]$RecoverReceiver,
    [switch]$AsyncOnly,
    [switch]$UsbAudio,
    [switch]$ClockStress,
    [ValidateRange(0,63)][int]$Optimizations = 0,
    [switch]$Pristine
)
$ErrorActionPreference = 'Stop'
if ($RecoverReceiver -and ($Target -ne 'rx' -or $Action -ne 'flash')) {
    throw '-RecoverReceiver is restricted to flashing the nRF5340 benchmark receiver'
}
$root = Split-Path -Parent $PSScriptRoot
$tc = 'C:\ncs\toolchains\66cdf9b75e'
# Environment setup and explicit-probe RTT pattern adapted from the user's
# mouse-main/tools/nrf-mouse-ops.ps1. No mouse project manifests are imported.
$savedPath = $env:PATH
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('PATH', $savedPath, 'Process')
$e = Get-Content "$tc\environment.json" -Raw | ConvertFrom-Json
foreach ($v in $e.env_vars) {
    if ($v.type -eq 'relative_paths') {
        $value = (@($v.values | ForEach-Object { Join-Path $tc $_ }) -join ';')
        if ($v.existing_value_treatment -eq 'prepend_to') { $value += ';' + [Environment]::GetEnvironmentVariable($v.key, 'Process') }
    } else { $value = $v.value }
    [Environment]::SetEnvironmentVariable($v.key, $value, 'Process')
}
$env:ZEPHYR_BASE = 'C:\ncs\v3.2.1\zephyr'
$env:PATH = "$env:SystemRoot\System32;$env:PATH"
$env:GIT_CONFIG_COUNT = '1'
$env:GIT_CONFIG_KEY_0 = 'safe.directory'
$env:GIT_CONFIG_VALUE_0 = 'C:/ncs/v3.2.1/*'
$nrfutil = "$tc\nrfutil\bin\nrfutil.exe"
$west = "$tc\opt\bin\Scripts\west.exe"
$build = Join-Path $root ".bench\build-$Target-$Codec"
if ($UsbAudio) {
    if ($Target -ne 'tx' -or $Codec -ne 'google-custom-lto') { throw 'USB audio requires tx/google-custom-lto' }
    $build = Join-Path $root '.bench\usb-tx'
}
$logs = Join-Path $root '.bench\logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$serial = if ($Target -eq 'tx54') { '1051883190' } elseif ($Target -eq 'tx') { '683088082' } else { '1050038165' }
$device = if ($Target -eq 'tx54') { 'nRF54LM20A_M33' } elseif ($Target -eq 'tx') { 'NRF52840_XXAA' } else { 'NRF5340_XXAA_NET' }
$board = if ($Target -eq 'tx54') { 'nrf54lm20dk/nrf54lm20a/cpuapp' } elseif ($Target -eq 'tx') { 'nrf52840dk/nrf52840' } else { 'nrf5340dk/nrf5340/cpunet' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
switch ($Action) {
    list { & $nrfutil device list }
    build {
        $a = @('build','--sysbuild','-b',$board,'-d',$build,(Join-Path $root 'firmware\benchmark'))
        if ($Pristine) { $a += @('-p','always') }
        $a += @('--',"-Dbenchmark_BENCH_ROLE=$Target","-Dbenchmark_BENCH_CODEC=$Codec","-Dbenchmark_BENCH_FRAMES=$Frames","-Dbenchmark_BENCH_SOAK_FRAMES=$SoakFrames","-Dbenchmark_BENCH_BITRATE=$Bitrate","-Dbenchmark_BENCH_PCM_BITS=$PcmBits")
        $a += "-Dbenchmark_BENCH_ASYNC_ONLY=$([int]$AsyncOnly.IsPresent)"
        $a += "-Dbenchmark_BENCH_USB=$([int]$UsbAudio.IsPresent)"
        $a += "-Dbenchmark_BENCH_OPTIMIZATIONS=$Optimizations"
        if ($Target -eq 'rx') { $a += "-Drx_app_core_AUDIO_CLOCK_STRESS=$([int]$ClockStress.IsPresent)" }
        if ($UsbAudio) {
            $usbRoot = $root.Replace('\','/')
            $a += "-Dbenchmark_EXTRA_CONF_FILE=$usbRoot/firmware/benchmark/usb_audio.conf"
            $a += "-Dbenchmark_DTC_OVERLAY_FILE=$usbRoot/firmware/benchmark/usb_audio.overlay"
        }
        $ErrorActionPreference = 'Continue'
        & $west @a 2>&1 | ForEach-Object { $_.ToString() } | Tee-Object -FilePath "$logs\build-$Target-$Codec-$stamp.log"
        $ErrorActionPreference = 'Stop'
    }
    flash {
        $flashArgs = @('flash','-d',$build,'--dev-id',$serial,'--runner','nrfutil')
        # Match Nordic's RRAM update path: erase only ranges present in this
        # image, preserving unrelated settings and provisioned key storage.
        if ($Target -eq 'tx54') { $flashArgs += @('--erase-mode','ranges') }
        if ($RecoverReceiver) { $flashArgs += '--recover' }
        & $west @flashArgs
    }
    reset { & $nrfutil device reset --serial-number $serial }
    rtt {
        $elf = Join-Path $build 'benchmark\zephyr\zephyr.elf'
        $nm = "$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin\arm-zephyr-eabi-nm.exe"
        $symbol = & $nm $elf | Select-String ' _SEGGER_RTT$' | Select-Object -First 1
        if (-not $symbol) { throw 'RTT symbol not found in firmware' }
        $addr = '0x' + ($symbol.ToString().Split(' ')[0])
        $path = "$logs\rtt-$Target-$Codec-$stamp.log"
        $rttArgs = @((Join-Path $PSScriptRoot 'rtt-memory.py'), '--serial', $serial, '--device', $device, '--address', $addr, '--seconds', $Seconds, '--output', $path)
        if ($StopOn) { $rttArgs += @('--stop-on', $StopOn) }
        & "$tc\opt\bin\python.exe" @rttArgs
        if ($LASTEXITCODE -ne 0) { throw 'RTT memory reader failed' }
    }
}
if ($Action -ne 'rtt' -and $LASTEXITCODE -ne 0) { throw "$Action failed ($LASTEXITCODE)" }
