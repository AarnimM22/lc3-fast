[CmdletBinding()]
param(
    [ValidatePattern('^[a-z0-9-]+$')][string]$Label,
    [ValidateRange(1000,10000000)][int]$RunId,
    [ValidateRange(100,100000)][int]$Frames=20000
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$folder=Join-Path $root "firmware\results\2026-09-20-usb-optimizations\$Label"
if(Test-Path -LiteralPath $folder) { throw "Refusing to overwrite existing experiment $folder" }
New-Item -ItemType Directory -Path $folder | Out-Null
$ops=Join-Path $PSScriptRoot 'audio-bench.ps1'
$seconds=[int][Math]::Ceiling($Frames*0.005+90)
$last=$RunId+1
$readers=@()
try {
    foreach($target in @('tx','rx')) {
        $codec=if($target -eq 'tx'){'google-custom-lto'}else{'etsi'}
        $stop=if($target -eq 'tx'){"USB_DONE id=$last"}else{"RX_RESULT run=$last "}
        $args=@('-NoProfile','-File',('"'+$ops+'"'),'-Action','rtt','-Target',$target,
            '-Codec',$codec,'-Seconds',$seconds,'-StopOn',('"'+$stop+'"'))
        if($target -eq 'tx'){$args+='-UsbAudio'}
        $readers+=Start-Process -FilePath powershell.exe -ArgumentList $args -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput "$folder\$target-capture.log" -RedirectStandardError "$folder\$target-capture-errors.log"
    }
    # Readers attach without halting the running targets; startup silence also
    # gives them time to drain older diagnostic messages before the new run.
    $player=Join-Path $root '.bench\usb-pcm-host\usb-pcm-host.exe'
    $wav=Join-Path $root 'firmware\benchmark\24_48k_PerfectTest.wav'
    & $player $wav $RunId 320000 $Frames | Tee-Object -FilePath "$folder\host-320.log"
    if($LASTEXITCODE -ne 0){throw '320 kb/s player failed'}
    # A deliberately overloaded image may need its 2 s input timeout after
    # the last PCM byte. Wait for completion before offering the next marker.
    $drainDeadline=[DateTime]::UtcNow.AddSeconds(10)
    while(-not (Select-String -LiteralPath "$folder\tx-capture.log" -SimpleMatch "USB_DONE id=$RunId" -Quiet)) {
        if([DateTime]::UtcNow -gt $drainDeadline){throw 'First USB run did not finish'}
        Start-Sleep -Milliseconds 100
    }
    & $player $wav $last 400000 $Frames | Tee-Object -FilePath "$folder\host-400.log"
    if($LASTEXITCODE -ne 0){throw '400 kb/s player failed'}
    foreach($reader in $readers) {
        if(-not $reader.WaitForExit(30000)){throw 'RTT reader did not observe its final result'}
        $reader.Refresh()
        if($null -ne $reader.ExitCode -and $reader.ExitCode -ne 0){throw 'RTT reader failed'}
    }
    # Windows PowerShell can return a null ExitCode after Refresh(). Require
    # complete end markers and empty stderr independently of that property.
    if(-not (Select-String -LiteralPath "$folder\tx-capture.log" -SimpleMatch "USB_DONE id=$last" -Quiet)) {throw 'Missing final TX result'}
    if(-not (Select-String -LiteralPath "$folder\rx-capture.log" -SimpleMatch "RX_RESULT run=$last " -Quiet)) {throw 'Missing final RX result'}
    foreach($target in @('tx','rx')) {
        if((Get-Item -LiteralPath "$folder\$target-capture-errors.log").Length){throw "RTT stderr on $target"}
    }
    & 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' "$PSScriptRoot\archive-usb-bench.py" `
        --output $folder --tx "$folder\tx-capture.log" --rx "$folder\rx-capture.log" `
        --host "$folder\host-320.log" "$folder\host-400.log"
    if($LASTEXITCODE -ne 0){throw 'USB experiment archive failed'}
} finally {
    foreach($reader in $readers) {
        $reader.Refresh()
        if(-not $reader.HasExited){Stop-Process -Id $reader.Id}
    }
}
