[CmdletBinding()]
param(
    [int[]]$Rates = @(336000,352000,368000,384000),
    [ValidateRange(1000,100000)][int]$Frames = 20000,
    [int]$RunBase = 9300,
    [string]$OutputDirectory = 'firmware\results\2026-09-20-usb-rate-sweep',
    [string]$WavPath = 'firmware\benchmark\24_48k_PerfectTest.wav'
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root $OutputDirectory
if(Test-Path -LiteralPath $out) { throw "Refusing to overwrite existing sweep: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$ops=Join-Path $PSScriptRoot 'audio-bench.ps1'
$player=Join-Path $root '.bench\usb-pcm-host\usb-pcm-host.exe'
$wav=if([IO.Path]::IsPathRooted($WavPath)) { $WavPath } else { Join-Path $root $WavPath }
if(-not (Test-Path -LiteralPath $player)) { throw 'USB host player is missing' }
$last=$RunBase+$Rates.Count-1
$seconds=[int][Math]::Ceiling($Rates.Count*$Frames*0.0025+120)
$readers=@()
try {
    # Quote marker values because they contain spaces; otherwise PowerShell
    # binds the second word to the next parameter (Frames).
    $txArgs=@('-NoProfile','-File',$ops,'-Action','rtt','-Target','tx','-Codec','google-custom-lto',
        '-Seconds',$seconds,'-StopOn',('"USB_DONE id='+$last+'"'),'-UsbAudio')
    $rxArgs=@('-NoProfile','-File',$ops,'-Action','rtt','-Target','rx','-Codec','etsi',
        '-Seconds',$seconds,'-StopOn',('"RX_RESULT run='+$last+'"'))
    $readers += Start-Process -FilePath powershell.exe -ArgumentList $txArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $out 'tx-capture.log') -RedirectStandardError (Join-Path $out 'tx-capture-errors.log')
    $readers += Start-Process -FilePath powershell.exe -ArgumentList $rxArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $out 'rx-capture.log') -RedirectStandardError (Join-Path $out 'rx-capture-errors.log')
    Start-Sleep -Seconds 3
    for($i=0;$i -lt $Rates.Count;$i++) {
        $rate=$Rates[$i]; $run=$RunBase+$i
        $log=Join-Path $out ("host-{0}.log" -f ($rate/1000))
        Write-Host "Running $($rate/1000) kb/s, run $run, $Frames frames"
        & $player $wav $run $rate $Frames 2>&1 | Tee-Object -FilePath $log
        if($LASTEXITCODE -ne 0) { throw "USB host player failed at $rate" }
        $deadline=[DateTime]::UtcNow.AddSeconds(20)
        while(-not (Select-String -LiteralPath (Join-Path $out 'tx-capture.log') -SimpleMatch "USB_DONE id=$run" -Quiet)) {
            if([DateTime]::UtcNow -gt $deadline) { throw "TX did not finish run $run" }
            Start-Sleep -Milliseconds 100
        }
    }
    foreach($reader in $readers) {
        if(-not $reader.WaitForExit(30000)) { throw 'RTT reader did not observe final result' }
    }
    if(-not (Select-String -LiteralPath (Join-Path $out 'rx-capture.log') -SimpleMatch "RX_RESULT run=$last " -Quiet)) { throw 'Missing receiver final result' }
    foreach($name in @('tx-capture-errors.log','rx-capture-errors.log')) {
        if((Get-Item -LiteralPath (Join-Path $out $name)).Length) { throw "RTT stderr in $name" }
    }
    $summary=@{
        rates_kbps=@($Rates | ForEach-Object { $_/1000 })
        frames=$Frames
        run_base=$RunBase
        tx_log='tx-capture.log'
        rx_log='rx-capture.log'
        host_logs=@($Rates | ForEach-Object { "host-$($_/1000).log" })
        firmware='nRF52840 USB TX google-custom-lto, optimization mask 31, 2.5ms frames, USB frame pool 18, radio pool 5'
        source=$WavPath
    }
    ($summary | ConvertTo-Json -Depth 5) | Set-Content -LiteralPath (Join-Path $out 'sweep.json') -Encoding utf8
    Write-Host "Saved sweep to $out"
} finally {
    foreach($reader in $readers) {
        $reader.Refresh()
        if(-not $reader.HasExited) { Stop-Process -Id $reader.Id }
    }
}
