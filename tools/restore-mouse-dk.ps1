[CmdletBinding()]
param([ValidateSet('flash','rtt')][string]$Action = 'flash')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tc = 'C:\ncs\toolchains\66cdf9b75e'
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
$image = Join-Path $root '.bench\mouse-dk-restoration\radio-nrf54-dk-receiver.hex'
$expected = '3e781d92bb3c50b7c4ac11d93d9b83ac2dd69e324eeee9f3f4c21dc2a49dfcf6'
if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'Saved mouse image does not match its recorded last-flashed SHA256'
}
# Restore the exact retained image, without rebuilding it from newer sources.
# User explicitly assigned this second DK to mouse duty on 2026-09-19.
$serial = '1051802784'
if ($Action -eq 'flash') {
    & "$tc\nrfutil\bin\nrfutil.exe" device program --serial-number $serial --firmware $image --options 'chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,verify=VERIFY_READ,reset=RESET_PIN'
} else {
    $elf = 'C:\ncs\v3.2.1\nrf\applications\mouse-main\.build\l\n\ru\mouse-main\zephyr\zephyr.elf'
    $symbol = & "$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin\arm-zephyr-eabi-nm.exe" $elf | Select-String ' _SEGGER_RTT$' | Select-Object -First 1
    if (-not $symbol) { throw 'Saved mouse image has no RTT symbol' }
    $addr = '0x' + ($symbol.ToString().Split(' ')[0])
    & "$tc\opt\bin\python.exe" (Join-Path $PSScriptRoot 'rtt-memory.py') --serial $serial --device nRF54LM20A_M33 --address $addr --seconds 15 --output (Join-Path $root '.bench\mouse-dk-restoration\boot.log')
}
if ($LASTEXITCODE -ne 0) { throw "Mouse DK $Action failed ($LASTEXITCODE)" }
