# WavPack-stream: nRF52840 timing and packet-loss recovery

Measured 2026-09-19. **The tested portable WavPack-stream encoder does not meet the low-latency, 48 kHz stereo, 24-bit-input deadlines on nRF52840, including fast mode.** Independent block decoding and recovery after dropped blocks passed host tests. That recovery does not reconstruct or conceal the missing audio.

## Hardware and codec configuration

- TX: nRF52840 DK PCA10056, serial `000683088082`, Cortex-M4F at 64 MHz.
- RX: nRF5340 DK PCA10095, serial `001050038165`, network-core ESB reassembly/CRC receiver. No decoder or DAC ran on RX.
- nRF54 boards were not flashed or accessed in this experiment.
- Official [dbry/wavpack-stream](https://github.com/dbry/wavpack-stream/tree/79ec9e178bd12c1445a09bec096db28707f1c55b), version 0.2.0, revision `79ec9e178bd12c1445a09bec096db28707f1c55b`. This is the streaming fork, not standard WavPack files.
- NCS 3.2.1, GCC 12.2, portable C, codec `-O3`, LTO, hard-float M4 ABI. No codec arithmetic edits or assembly optimizations. Default and `CONFIG_FAST_FLAG` modes both tested, upstream automatic/dynamic noise shaping retained, no extra processing mode.
- Hybrid/lossy output with no correction stream: targets 256/320/384 kb/s; fixed 60/120/240 samples per channel (1.25/2.5/5 ms); 48 kHz stereo; signed 24-bit input in int32 containers. Encoder context persists across blocks. No per-frame reinitialization, no hidden lookahead buffer or flush requirement.
- Deterministic tones, noise, silence and transient mixtures, changing every 25 blocks, with nonzero input low bits. Each case resets the encoder and source generator.
- ESB 2 Mbit/s, fixed 2440 MHz, 0 dBm, ACKs and up to three retries at 600 us spacing. No hopping, FEC or concealment.

## Encoding results

All figures below are local API-only average / observed maximum milliseconds over 1000 stereo blocks, excluding PCM generation and RF. The maximum includes startup and content transitions. Full results are in [tables.md](tables.md) and [results.json](results.json).

| Mode | Target kb/s | 5 ms blocks | 2.5 ms blocks | 1.25 ms blocks |
|---|---:|---:|---:|---:|
| Default | 256 | 5.861 / 16.700 | 3.162 / 11.887 | 1.827 / 9.282 |
| Default | 320 | 5.978 / 16.744 | 3.226 / 11.932 | 1.853 / 9.316 |
| Default | 384 | 6.105 / 16.833 | 3.286 / 11.957 | 1.876 / 9.340 |
| Fast | 256 | 4.600 / 13.844 | 2.519 / 10.411 | 1.478 / 8.543 |
| Fast | 320 | 4.727 / 13.979 | 2.583 / 10.481 | 1.500 / 8.575 |
| Fast | 384 | 4.847 / 14.070 | 2.641 / 10.539 | 1.528 / 8.605 |

At 320 kb/s target and 2.5 ms, fast mode's full encoding wrapper averaged 2.662 ms, with P99 5.311 ms. The miss is not solely a single cold-start maximum. With radio overlap, encoding plus enqueue averaged 3.088 ms and 1000 blocks representing 2.5 seconds took 3.101348 seconds through the final ACK. Source latency accumulated because the source schedule was not moved forward to disguise overload.

Even the fast 5 ms cases, whose local average API cost is below the frame interval, failed with radio: average encoding/enqueue work was 5.210 / 5.517 / 5.685 ms at the three target rates, with much larger individual spikes. No tested configuration passed the source-cadence test. No long soak was justified after these deadline failures. This is a result for this implementation, flags and synthetic source, not proof that every possible WavPack implementation or larger-frame/16-bit configuration must fail.

## Actual bitrate and bursts

The configured hybrid bitrate is not a strict compressed-block size or radio bitrate cap. Values below include every codec byte over the represented source duration; RF headers, ACKs and retries are additional. Content and block duration affect the result.

| Fast mode, 2.5 ms | Configured target | Actual encoded payload | Largest block | Fragmented blocks / 1000 |
|---|---:|---:|---:|---:|
| 24-bit stereo | 256 kb/s | 414.323 kb/s | 1300 bytes | 51 |
| 24-bit stereo | 320 kb/s | 475.168 kb/s | 1316 bytes | 80 |
| 24-bit stereo | 384 kb/s | 532.845 kb/s | 1332 bytes | 88 |

Each ESB data packet carries at most 224 codec bytes plus a 24-byte application header. Thus a 1316-byte block needs six RF packets. At the 320 kb/s target, fast mode used 1140 packets for 1000 2.5 ms blocks. The corresponding payload rate was 669.274 kb/s at 1.25 ms and 367.910 kb/s at 5 ms. Short blocks make both independent-block metadata and variable coding bursts more significant. These figures are from the measured synthetic content, not a universal rate for music.

Input precision should not be confused with retained fidelity: accepting and decoding 24-bit samples does not mean lossy output has 24 effective bits or is perceptually transparent. No listening/ABX or full acoustic-quality evaluation was performed, and LC3plus/SBC bitrate-quality claims cannot simply be transferred to this codec or its fast mode.

## Radio results and timing interpretation

The final sweep encoded 36000 blocks: 18000 local and 18000 with radio. All 18000 RF blocks passed independent reassembly and CRC32 verification, across 23030 packets. There were 179 retries, zero exhausted-ACK failures, zero queue submission errors, zero codec errors, zero timing cross-check errors, zero receiver losses, zero bad CRCs and zero RX overflow.

All 18 RF cases missed their source cadence. Receiving every block eventually does not establish timely playback. The radio was supplied by an encoder slower than the intended source and therefore did not demonstrate full-rate coexistence robustness.

The previously tested concurrent-radio worker is reused on the nRF52840. Encoding occurs while RADIO/EasyDMA and ACK handling proceed. Fixed absolute source release times are retained; the test does not discard source intervals or change phase after overload. The packet queue is bounded to 16 entries plus one in flight for WavPack, accommodating large fragmented blocks. The measured peak was nine. The queue is drained before END.

For fragmented frames, `radio_completed`, `radio_late` and `ready_to_ack_*` refer to individual RF packets. Their deadline is two source intervals after scheduled PCM availability. Those latency fields include accumulated source lateness and are not an estimate of intrinsic codec delay. The per-case expected RF packet count is `data_packets`.

DWT measurements use the actual 64 MHz clock and are cross-checked against RTC-backed uptime with a 200 us tolerance. Uptime resolution is approximately 30.5 us. All 1000 samples contribute to averages/maxima and P99 in this sweep. Radio-mode codec timings include interrupts and worker preemption. Capture stayed attached throughout.

## What packet-loss recovery actually guarantees

Upstream explicitly designs WavPack-stream blocks to be independently decodable. Our host tests verify this for the pinned implementation and tested settings:

1. Eighteen configurations: both compression modes, all three rates, all three block lengths; 400 blocks per case, 7200 blocks total.
2. Decode the complete stream continuously, then decode every block with a fresh decoder. All resulting PCM samples match the corresponding continuous baseline exactly.
3. Delete isolated blocks (1%), four-block bursts, and a twenty-block burst. Decode the surviving stream with a persistent decoder. All surviving PCM samples match the no-loss baseline exactly, including the first intact block after each gap. There is no additional predictor recovery tail in these tests.
4. Test stereo input with amplitude only 24 in a signed 24-bit integer scale, which would disappear under simple 16-bit truncation. The lossless round trip is exact. Fast hybrid mode at target 320 kb/s produces nonzero output with maximum absolute error two integer units on this low-level tone.

These are host decoder tests, not RF erasure injection. The host signal generator uses continuous 1000/1700 Hz tones and a separate deterministic RNG, so its bitrate figures are not expected to equal the embedded generator's figures. Raw outcomes are in [host-validation.json](host-validation.json).

Missing blocks are absent from the decoded stream. The codec does not know to preserve their place on a playback timeline or synthesize the missing waveform; the application must use sequence numbers/sample timestamps and provide silence, interpolation, repetition/crossfade or another explicit concealment policy. The tests compare the surviving samples after removing gaps; they do not count missing samples as recovered. No PLC or FEC was implemented.

Upstream's internal block/audio checksums are disabled by default in `wavpack_local.h`, and the raw-memory decoder also requests `OPEN_NO_CHECKSUM`. Therefore corrupted/incomplete payloads must be rejected by the transport rather than treated as usable blocks. This harness uses ESB CRC plus an application CRC32 over the reassembled codec frame. Loss of one required fragment means the entire codec block is unusable unless it is recovered in time.

## Wi-Fi / Bluetooth coexistence

Independent blocks prevent persistent decoder damage after an erasure; they do not prevent RF collisions or make gaps inaudible. There is no basis here for a general assurance that WavPack-stream alone is sufficient around Wi-Fi and Bluetooth.

At 2.5 ms there are 400 blocks per second. A residual whole-block loss rate of 1% after recovery is four gaps per second; 0.1% is one every 2.5 seconds; 0.01% is one every 25 seconds on average. Burst losses are also important. Whether a gap is audible depends on content, duration and concealment. These are arithmetic examples, not measured RF rates or perceptual thresholds.

A practical proprietary link needs channel selection/hopping, bounded retries with spare airtime, CRC/sequence checks, a small playout buffer, and concealment for unrecovered blocks; FEC or selective redundancy may also be useful. These add airtime, memory, CPU and/or latency. Bluetooth itself uses adaptive frequency hopping to avoid interference; this fixed-channel ESB benchmark does not inherit that behavior. See [Bluetooth SIG's explanation](https://www.bluetooth.com/blog/how-bluetooth-technology-uses-adaptive-frequency-hopping-to-overcome-packet-interference/) and [Nordic's ESB documentation](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/libraries/others/esb.html).

No controlled interferer, saturated Wi-Fi traffic, range sweep, antenna/body-obstruction test or final earcup hardware was used. The bench RF run therefore tests delivery integrity under the conditions present, not general coexistence qualification. The already demonstrated nRF54LM20A + Google LC3plus path remains the stronger measured candidate for the intended short-frame dongle.

## Port corrections, memory and reproduction

An initial 200-frame sweep exposed occasional blocks larger than the old 1250-byte shared frame buffer. The output callback correctly rejected those blocks, so that preliminary run had codec-wrapper errors and is not used for the final result. The shared TX/RX frame cap is now 4096 bytes; all final calls emit exactly one complete block per input block, with no errors. Largest observed final block: 1874 bytes. The codec source itself remains unchanged.

Linked TX usage is 116896 bytes flash, 78144 bytes static RAM; RX is 46984 bytes flash, 57448 bytes RAM. WavPack also dynamically allocates contexts, metadata and temporary buffers from the newlib heap, so linked static RAM is not the total runtime memory footprint. No runtime heap high-water measurement was made.

```powershell
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools/fetch-wavpack-stream.py
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target rx -Codec etsi
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx -Codec wavpack-lto -Frames 1000 -PcmBits 24
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action flash -Target rx -Codec etsi
```

Start RX capture before flashing TX, then attach TX within its 10-second startup delay. Stop RX on `RX_RESULT run=236 ` and TX on `SUITE_DONE`, allowing 300 seconds. Hardware roles are pinned in `tools/audio-bench.ps1`.

Host tests: run `powershell.exe -NoProfile -File .\tools\build-wavpack-host.ps1`, then `tools/test-wavpack-stream.py` with Python. Visual C++ 2022 builds the same unmodified portable encoder/decoder sources as a native DLL. WavPack-stream is not compatible with the ordinary FFmpeg WavPack decoder; the upstream decoder was used, not an independent implementation.

[TX capture](tx.log), [RX capture](rx.log), [full tables](tables.md), [JSON results](results.json), [host recovery tests](host-validation.json), [source/image hashes](manifest.json), [upstream license](wavpack-license.txt). Matching source snapshots, firmware images, ELF files, build logs, Kconfig, devicetrees and compile commands are retained alongside this report.
