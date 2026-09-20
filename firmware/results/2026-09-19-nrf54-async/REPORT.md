# Concurrent LC3plus encoding and radio on nRF54LM20A

Measured 2026-09-19. **The Arm core sustains 48 kHz, stereo, 24-bit-input Google LC3plus at both 256 and 320 kb/s with 2.5 ms frames when transmission overlaps encoding.** All 82000 frames passed independent nRF5340 reception/CRC checks. No RISC-V offloading was used.

## Results

| Run | kb/s | Frames | Encode + enqueue avg/max ms | Source late/skipped | PCM ready to ACK avg/max ms | ACK >5 ms | Pending peak | RX good |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 101 | 256 | 1000 | 2.003/2.208 | 0/0 | 2.878/3.088 | 0 | 1 | 1000 |
| 102 | 320 | 1000 | 2.040/2.272 | 0/0 | 2.985/3.196 | 0 | 1 | 1000 |
| 103 | 256 | 40000 | 1.997/2.208 | 0/0 | 2.867/3.096 | 0 | 1 | 40000 |
| 104 | 320 | 40000 | 2.037/2.272 | 0/0 | 2.982/3.208 | 0 | 1 | 40000 |

The two 40000-frame cases represent 100 seconds of audio each. Measured TX durations, including the final ACK, were 100.000544 and 100.000768 seconds. Each maintained 400 frames/s with zero source deadline misses, skipped source intervals, queue submission failures, codec errors, ACK failures, receiver losses, bad CRCs, overflow or timing cross-check errors. Retries across this suite: 0. Peak outstanding packets stayed at 1: there was no accumulating radio backlog.

At 320 kb/s, average encoding-plus-enqueue work was 2.037 ms (81.48% of each 2.5 ms period), and the observed maximum was 2.272 ms. The minimum observed remaining interval was 228 us; this includes PCM generation and radio-worker preemption but excludes USB input and other future application duties. It is an observed margin, not a universal execution-time bound.

The corresponding 256/320 kb/s API-only average/max timings under concurrent load were 1.825/2.065 ms and 1.859/2.094 ms. Unlike uninterrupted local API measurements, these include radio-thread/ISR preemption while the API call is active.

## Why this works

The previous sequential benchmark waited for an ACK before it could encode the next frame. Its 40000-frame cases averaged 2.712 and 2.823 ms of serial work per 2.5 ms frame; that measured scheduling constraint did not establish a hardware throughput limit. The [baseline report](../2026-09-19-nrf54/REPORT.md) and its original images remain preserved.

The main thread now encodes and copies each encoded packet to a four-entry queue. A higher-priority worker owns the existing blocking ESB send/ACK operation. It starts the radio and blocks on a semaphore, allowing the encoder to run while RADIO/EasyDMA transmit and receive the ACK. Both software threads run on the same Arm CPU; hardware radio activity supplies the concurrency. Queue storage owns each packet, so the next encoder invocation cannot overwrite in-flight data.

Mode 3 uses absolute 2.5 ms source releases, never rephases the source after lateness and never drops an input interval to conceal overload. It records enqueue completion against the next source release. Pending work is bounded to four queued packets plus one being transmitted, and drained before END/control traffic. The queue would return an error rather than grow without bound.

## Latency interpretation

`ready_to_ack` starts when a complete PCM frame is scheduled to become available. It includes encoding, queueing, radio transfer and ACK handling. The separate delivery target is 5 ms, chosen as two frame intervals; all packets met it in this run. A target is not an inserted delay: average measured completion was about 2.87/2.98 ms. Radio service alone averaged 842 / 921 us, including ACK wait.

This is not total playback latency. PCM collection, codec lookahead, USB scheduling, receiver decoding, jitter buffering and DAC output are outside that measurement. A pipeline can produce one frame every 2.5 ms even when an individual frame takes roughly 3 ms from PCM availability to its ACK.

## RISC-V option

FLPR is not needed for this measured encode-and-radio workload and was not programmed. The installed Nordic device tree declares its ISA as `rv32emc`, without the floating-point extension, whereas this Google encoder uses floating point on the M33 FPU. Moving one channel to FLPR is therefore not an automatic way to double encoder throughput. Integer PCM preparation or packet bookkeeping are possible candidates for a separate shared-memory/IPC implementation, whose benefit would need measurement.

Nordic lists nRF54LM20A FLPR kernel support as experimental. Its documentation also warns that FLPR activity can increase RAM_01 access latency. See [Nordic FLPR documentation](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/app_dev/device_guides/nrf54l/vpr_flpr.html); the installed NCS 3.2.1 `nrf/doc/nrf/app_dev/device_guides/nrf54l/vpr_flpr.rst` has the same support status and memory warning. The ISA declaration is in `zephyr/dts/vendor/nordic/nrf54lm20a.dtsi`.

## Setup, scope and retained evidence

- TX: nRF54LM20 DK serial 001051883190, Arm M33 at 128 MHz, FPU context sharing enabled. Linked nonvolatile memory 193860 bytes, RAM 82456 bytes.
- RX: nRF5340 DK serial 001050038165, same network-core frame/CRC receiver as the baseline.
- Mouse DK 001051802784 remained assigned to its restored mouse firmware and was not accessed during this experiment.
- Google liblc3 revision `8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07`, standard 48 kHz mode, 24-bit signed PCM in 32-bit containers, independent stereo encoders, deterministic tones/noise/silence/transients. `-O3`, LTO and `-ffast-math`; codec arithmetic unchanged. No HR mode or 1.25 ms mode.
- ESB 2 Mbit/s, fixed 2440 MHz, 0 dBm, acknowledged packets, up to three retries at 600 us spacing. At these frame sizes each stereo frame uses one packet.
- DWT timing cross-checked against 32 us GRTC-backed uptime. No logging occurs per frame. Average/max cover every frame; P99 covers at most the first 1024. Probe capture stays attached through the suite.
- This fixed-channel desk test does not measure RF interference robustness, range, USB Audio, nRF5340 decoding, DAC output, clock drift or perceptual quality. No retry/loss stress was deliberately injected.

```powershell
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx54 -Codec google-lto -Frames 1000 -SoakFrames 40000 -PcmBits 24 -AsyncOnly
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action flash -Target tx54 -Codec google-lto
```

Start RX capture before flashing TX; attach TX within its 10-second startup delay. Stop RX on `RX_RESULT run=104 ` and TX on `SUITE_DONE`; allow 300 seconds. The receiver capture includes old buffered messages before run 101; only matching run IDs 101-104 are joined to this TX suite.

[TX log](tx.log), [RX log](rx.log), [JSON results](results.json), [tables](tables.md), [build log](build.log), [source/image hashes](manifest.json). Matching source snapshots, firmware image, ELF, devicetree, Kconfig and compile commands are retained alongside this report. The RX image is retained with the baseline report.
