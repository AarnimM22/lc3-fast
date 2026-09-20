# nRF52840 Opus CELT: stereo 320 kb/s and 16/24-bit PCM

Measured on 2026-09-19 using the same nRF52840 DK and nRF5340 receiver as the LC3plus experiment.

## Result

**None of the tested Opus builds sustained the requested real-time stereo encoding.** Even complexity 0 exceeded the frame budget before radio transmission. 24-bit input had a small effect within the same implementation; it did not resolve the CPU limitation.

This applies to the official libopus builds tested here, not every possible hand-optimized implementation.

## Conditions

- TX: nRF52840 DK `000683088082`, 64 MHz, instruction cache and FPU enabled.
- RX: nRF5340 DK `001050038165`, unchanged ESB receiver on its network core.
- Official libopus 1.6.1, unmodified source; `-O3`, LTO, Cortex-M4F ABI. Fixed-point builds use ARM EDSP/MEDIA instructions. Floating point uses `FLOAT_APPROX`, without fast-math.
- 48 kHz stereo, 320 kb/s total CBR, full bandwidth, restricted-low-delay CELT, no FEC or DTX.
- Complexities 0, 5 and 10; frame durations 10, 5 and 2.5 ms; 200 frames per case.
- Locally generated tones, noise, silence and transients. The 24-bit samples include nonzero low bits; 16-bit input is their quantized counterpart. RNG resets for each case.
- Same acknowledged ESB transport: 2 Mbit/s, fixed 2440 MHz, 0 dBm, three retries, 600 us retry delay.

Opus has no standard 1.25 ms frame. The measured encoder reports 120 samples (2.5 ms) of lookahead, making the minimum nominal codec delay 5 ms with a 2.5 ms frame, before transport and system buffers.

## Encoding at minimum complexity

API-only average / maximum milliseconds for a **stereo** frame, without RF. These exclude PCM preparation and packet validation.

| Implementation | PCM bits | 10 ms frame | 5 ms frame | 2.5 ms frame |
|---|---:|---:|---:|---:|
| Fixed point, normal internal precision | 16 | 15.705 / 16.542 | 9.891 / 10.377 | 6.744 / 7.002 |
| Fixed point, RES24 | 16 | 15.600 / 16.605 | 9.725 / 10.101 | 6.478 / 6.688 |
| Fixed point, RES24 | 24 | 15.466 / 16.440 | 9.650 / 10.058 | 6.435 / 6.656 |
| Floating point | 16 | 22.559 / 24.033 | 13.872 / 14.710 | 9.389 / 9.663 |
| Floating point | 24 | 22.618 / 24.080 | 13.889 / 14.711 | 9.395 / 9.666 |

## Complexity at the shortest frame

API-only average / maximum milliseconds at 2.5 ms. Complexity 0 is the fastest setting, with a quality/computation tradeoff; these tests do not assess listening quality.

| Implementation | PCM bits | Complexity 0 | Complexity 5 | Complexity 10 |
|---|---:|---:|---:|---:|
| Fixed point, normal internal precision | 16 | 6.744 / 7.002 | 10.479 / 11.146 | 14.679 / 25.395 |
| Fixed point, RES24 | 16 | 6.478 / 6.688 | 10.254 / 10.913 | 14.331 / 25.447 |
| Fixed point, RES24 | 24 | 6.435 / 6.656 | 10.212 / 10.906 | 14.277 / 24.519 |
| Floating point | 16 | 9.389 / 9.663 | 13.426 / 13.918 | 21.060 / 31.580 |
| Floating point | 24 | 9.395 / 9.666 | 13.413 / 13.864 | 21.055 / 31.562 |

## Effect of 24-bit input

- float: average API-time changes ranged from -0.10% to +0.60% across matched frame-duration/complexity cases.
- fixed24: average API-time changes ranged from -0.86% to +0.03% across matched frame-duration/complexity cases.

These are small differences from finite synthetic-input runs, not a claim of identical cycles or a universally faster input format. Bitrate fixes the output byte budget; it does not fix the operation count. Input conversion, precision, content and complexity also matter.

The normal fixed-point build converts 24-bit API input to 16-bit internal precision. We explicitly rejected its 24-bit cases and used `ENABLE_RES24` for genuine 24-bit processing. This changes internal state size and arithmetic, so compare the RES24 and normal builds separately.

Encoder initialization reported 43,836 bytes of state for normal fixed point, 45,756 bytes for fixed RES24, and 48,624 bytes for floating point. The instrumented firmware reserves a larger 98,304-byte state arena and a 49,152-byte main stack; total linked RAM is 200,512 bytes in each variant. These are benchmark allocations, not optimized product memory requirements.

Packed raw PCM bandwidth rises from 1.536 Mb/s to 2.304 Mb/s at 48 kHz stereo. The 24-bit API uses 32-bit sample containers, doubling its input-buffer size versus 16-bit. Both produce the same 320 kb/s compressed packet budget. USB input and packing/conversion are not measured. Opus at this bitrate is lossy; accepting 24-bit PCM does not preserve 24-bit precision losslessly.

## Encoding and acknowledged delivery

2.5 ms, complexity 0. Work includes PCM generation, encoding, validation, CRC, RF and ACKs.

| Implementation | PCM bits | Encode including input avg/max ms | Work avg/max ms | Late | Skipped intervals | RX good |
|---|---:|---:|---:|---:|---:|---:|
| Fixed point, normal internal precision | 16 | 6.821 / 6.933 | 7.861 / 7.966 | 200/200 | 429 | 200/200 |
| Fixed point, RES24 | 16 | 6.551 / 6.731 | 7.579 / 7.782 | 200/200 | 406 | 200/200 |
| Fixed point, RES24 | 24 | 6.507 / 6.695 | 7.534 / 7.721 | 200/200 | 403 | 200/200 |
| Floating point | 16 | 9.453 / 9.768 | 10.523 / 10.834 | 200/200 | 642 | 200/200 |
| Floating point | 24 | 9.458 / 9.772 | 10.527 / 10.865 | 200/200 | 642 | 200/200 |

Across 90 completed cases, the receiver verified 9,000 transmitted frames with zero missing frames, CRC errors or queue overflows. 9,000/9,000 transmitted frames were late. Sending every frame eventually is not continuous real-time playback.

No extended encode-and-send soak was necessary to establish the failure: the encoder alone already exceeded its budget in every configuration. The previous radio-only test is documented in the [LC3plus report](../2026-09-19/REPORT.md).

## Validation, limitations and reproduction

All retained cases have zero codec, packet-metadata, send and timing errors. Every packet was checked for CELT ToC, stereo, full bandwidth, duration and CBR length. DWT cycle timings were cross-checked against an independent RTC clock (200 us tolerance); the first floating-point run with a capture reconnect was discarded. An earlier fixed RES24 capture with stale/duplicate text at an RTT wrap was also excluded; the logger now uses atomic 32-bit reads for live ring offsets. The retained runs use continuous captures through suite completion.

This does not implement USB Audio Class, receiver decoding, DAC output, clock-drift control, playout buffering or listening tests. Packet checks are not a conformance test. Higher-compute hardware or a much faster implementation is needed for this target; the nRF5340 application core was not benchmarked as an encoder.

[Build and run instructions](../../OPUS.md). Final TX image: `opus-fixed-lto` (RES24), idle after completion. RX remains the earlier codec-agnostic receiver. Resetting TX repeats the suite.

- [Float TX](float-tx.log), [RX](float-rx.log), [JSON](float.json).
- [Fixed RES24 TX](fixed24-tx.log), [RX](fixed24-rx.log), [JSON](fixed24.json).
- [Fixed RES16 TX](fixed16-tx.log), [RX](fixed16-rx.log), [JSON](fixed16.json).
- [Manifest and SHA256 hashes](manifest.json); configurations are in `configs/`.

Primary sources: [official release and checksum](https://opus-codec.org/downloads/), [encoder API and frame sizes](https://opus-codec.org/docs/opus_api-1.6/group__opus__encoder.html), [encoder controls](https://opus-codec.org/docs/opus_api-1.6/group__opus__encoderctls.html).
