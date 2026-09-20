# Receiver audio-clock recovery, 2026-09-20

The nRF5340 now regulates its I2S playback clock to follow the USB-sourced
audio rate. The final implementation passed a 250-second full-track run and
a 250-second demanding-excerpt run with deliberately injected clock offsets.
Every audio frame completed through I2S DMA, with no underruns. The three-frame
startup prefill and eight-block allocation pool were retained. Normal firmware
with disturbance injection disabled is left flashed on the receiver.

## Clocking in plain language

USB determines how quickly samples enter this system. ESB transports them with
variable delay, and I2S clocks them out to a DAC. A PLL is a circuit that generates
the audio clock; changing its setting changes how quickly I2S consumes samples.
I2C is a separate interface for DAC configuration and volume registers, and does
not carry the audio samples or determine this stream's sample timing.

The nRF5340 audio oscillator can be adjusted while in use, in approximately
3.311 ppm steps near 12.288 MHz. This is documented by
[Nordic's audio oscillator specification](https://docs.nordicsemi.com/r/bundle/ps_nrf5340/page/chapters/clock/doc/clock.html-concepthfclkaudio).
The local NCS 3.2.1 nRF5340 Audio application also adjusts HFCLKAUDIO during
playback. The previous suggestion that this setup necessarily required
resampling or sample insertion/deletion was therefore incorrect.

The USB host remains the source timing master. The nRF52840 relays encoded
sample-numbered frames without additional processing or synchronization packets.
The nRF5340 follows the source's average rate, while remaining the physical
I2S clock master on its output pins. Its HFXO, CPU and radio clocks are not retuned.
The controller does not modify, duplicate, discard or resample PCM samples.

## Final implementation

At frame arrival, the application core atomically snapshots local time and
I2S DMA position. Source sequence numbers provide sample position. Fractional
progress since the last DMA release avoids whole-frame quantization of the
estimated source-to-playback lead. A fixed pipeline offset is absorbed in the
startup target; the lead is not an end-to-end latency measurement.

After ignoring the first second, five one-second averages establish the startup
target. Subsequent one-second averages feed a PI controller:

- proportional gain 1/10 s; integral gain 1/400 s squared;
- 50 us deadband, consistent with RTC resolution and averaged arrival jitter;
- integral bounded to +/-150 ppm; output bounded to approximately +/-200 ppm;
- maximum slew of four PLL register steps per update, approximately 13.25 ppm/s;
- hold the last frequency and discard a partial measurement window on sequence
  discontinuities or arrival gaps of at least 100 ms;
- reset to nominal and recalibrate on each new stream.

The measured nominal register value is 39845. The controller owns
HFCLKAUDIO.FREQUENCY during streaming. This is slow audio-rate recovery based
on buffer position, not a microsecond radio-phase synchronization protocol.
Transport delay changes affect the observed lead, so variable codec work can
produce larger residual phase variation than a repeated excerpt.

Implementation and pin details: [application-core README](../../benchmark/app_core/README.md).

## Hardware results

TX: nRF52840 DK 000683088082, existing USB custom LC3-fast encoder, optimization
mask 31, packed 24-bit stereo at 48 kHz, 2.5 ms frames. RX: nRF5340 DK
001050038165, network-core ESB and application-core decoding at 64 MHz.
I2S carries 24 significant bits in 32-bit slots.

| Measurement | Previous uncorrected run 9490 | Final normal run 9604 | Final disturbance run 9603 |
|---|---:|---:|---:|
| Duration | 250 s | 250 s | 250 s |
| Source | Full WAV loop | Full WAV loop | First 5 s loop |
| Configured codec rate | 384 kb/s | 384 kb/s | 384 kb/s |
| Actual mean compressed audio payload | 270.596 kb/s | 270.596 kb/s | 381.383 kb/s |
| Received / decoded / DMA completed | 100,000 each | 100,000 each | 100,000 each |
| Missing audio / I2S underruns | 0 / 0 | 0 / 0 | 0 / 0 |
| PCM ownership minimum / maximum | 3 / 7 | 2 / 5 | 2 / 5 |
| PCM ownership early / final | 4 / 7 | 4 / 4 | 4 / 4 |
| Final averaged lead error | Not measured | +623 us | +110 us |
| Peak absolute averaged lead error | Not measured | 1,280 us | 1,552 us |
| Arrival span minus DMA span | -7,659 us | -945 us | -153 us |
| Decode plus volume mean / maximum | 1.333 / 1.740 ms | 1.326 / 1.770 ms | 1.543 / 1.740 ms |
| Volume updates applied | 1,000 | 1,000 | 1,000 |
| Radio retries / terminal ACK failures | 15 / 0 | 341 / 0 | 1,351 / 0 |

The configured bitrate is a variable-payload ceiling/target, not the measured
average radio payload rate. Payload rates exclude radio framing, ACKs, retries
and volume commands. The demanding excerpt sustains nearly 384 kb/s.

Both final runs passed every check in `validation.json`: all frames were decoded,
queued, DMA-released and drained; no USB overflow or disconnect, IPC drop,
codec error, radio send failure or I2S error occurred. Each performed 244 servo
updates with zero rejected observations. Normal firmware reported `stress=0`
and `bias=0`. Final correction was +15 steps; this instantaneous setting includes
phase correction and is not an estimate of crystal error by itself.

The stress firmware adds +30 PLL steps (about +99.3 ppm) around 50 seconds,
then switches to -30 steps around 125 seconds, a roughly -198.7 ppm transition.
The estimator receives no feed-forward knowledge of those offsets. Application
RTT confirms both signs were applied. Final correction was +39 steps with a
-30-step test bias, for a net +9 steps. It recovered while using the same buffers.

Each final run also had about 248,000 synthetic sensor interrupts, 240,000
sensor-work executions, 24,976 synthetic DAC interrupts, and 7,250 real I2C
controller attempts. All I2C attempts failed as expected because no target is
attached. Radio retry counts reflect the actual environment, not controlled
interference injection.

Slab ownership includes software-queued, current and next DMA buffers. The
three-frame startup prefill is not a three-block total ownership limit or a
measured end-to-end latency. Arrival/DMA span differences include endpoint
jitter and RTC timestamp quantization; they must not be reported as calibrated
oscillator accuracy. The full-track endpoint retains more jitter than the
repeated excerpt. Stable bounded buffer use is the primary drift result.

## Evidence and reproduction

This directory contains final normal-run captures, `validation.json`, complete
application-core clock trace, build log, configurations, firmware hashes and
source snapshot. The passing final disturbance test is in
[`../2026-09-20-clock-servo-tuned-stress`](../2026-09-20-clock-servo-tuned-stress).

From the workspace root, with both nRF52840 USB connectors attached:

```powershell
# Build/flash the normal receiver; existing USB transmitter stays in place.
.\tools\audio-bench.ps1 -Action build -Target rx -Codec etsi -Frames 200
.\tools\audio-bench.ps1 -Action flash -Target rx -Codec etsi
.\tools\run-usb-rate-sweep.ps1 -Rates 384000 -Frames 100000 -RunBase 9700 -OutputDirectory firmware\results\new-clock-normal
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\summarize-rx-playback.py firmware\results\new-clock-normal --run 9700 --require-servo

# Deliberate disturbances; use the original WAV's first 240000 stereo samples.
.\tools\audio-bench.ps1 -Action build -Target rx -Codec etsi -Frames 200 -ClockStress
.\tools\audio-bench.ps1 -Action flash -Target rx -Codec etsi
.\tools\run-usb-rate-sweep.ps1 -Rates 384000 -Frames 100000 -RunBase 9701 -OutputDirectory firmware\results\new-clock-stress -WavPath .bench\playback-heavy-5s.wav
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\summarize-rx-playback.py firmware\results\new-clock-stress --run 9701 --require-stress

# Restore normal firmware after a stress test.
.\tools\audio-bench.ps1 -Action build -Target rx -Codec etsi -Frames 200
.\tools\audio-bench.ps1 -Action flash -Target rx -Codec etsi
```

Choose new output directories and IDs for each run. The heavy WAV is a lossless
copy of the first five seconds of `firmware/benchmark/24_48k_PerfectTest.wav`;
it has not been resampled or reduced in precision.

## Earlier development captures and limitations

- Run 9600 kept playback stable but failed measurement checks because a DMA
  interrupt could occur between time and position reads. The final code takes
  the snapshot atomically under an interrupt lock.
- Run 9601 passed a heavy normal-stream test. Despite its directory name
  `clock-servo-stress`, the build option had not propagated to the child image.
  It is not disturbance evidence. The CMake propagation was fixed and the
  validator now checks that the injected bias actually appears.
- Run 9602 completed all playback but its conservative slew limit allowed
  3.775 ms peak lead error under injected shifts, failing the same less-than-one-
  frame acceptance limit later passed by 9603. The final gain and slew settings
  above reduced that peak to 1.552 ms. Original captures are retained.

These are finite DK tests. There is still no physical DAC/accelerometer attached,
no analog listening or clock-jitter measurement, and no validation of successful
CS43131 I2C transactions or physical GPIO interrupt behavior. Extended outages,
temperature extremes, hours-long runs and different USB hosts remain outside
this validation. The +/-200 ppm controller bound is an explicit operating limit,
not a guarantee for arbitrary source clocks. The benchmark's existing loss
concealment and stream recovery behavior have not been redesigned here.
