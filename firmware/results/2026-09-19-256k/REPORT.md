# 256 kb/s stereo encoding: 24-bit, 48 kHz PCM

Measured on the connected nRF52840 DK and nRF5340 DK on 2026-09-19.

**The tested encoders do not sustain 2.5 ms stereo encoding on the 64 MHz nRF52840.** Opus complexity 0 is substantially faster than the ETSI reference, but slower than Google liblc3. These results rank the tested implementations and settings, not the inherent complexity of the codec standards.

## Matching conditions

- 48 kHz, two channels, 256000 bits/s total, 24-bit signed PCM in 32-bit containers.
- 200 frames per case; identical deterministic tones/noise/silence/transient PCM with RNG reset per case.
- Opus 1.6.1 fixed point with ENABLE_RES24, CELT restricted low delay, CBR, full bandwidth; complexity 0, 5 and 10, both 16- and 24-bit input.
- ETSI TS 103 634 v1.7.1 reference software 1.9.2 fixed point, native lc3plus_enc24 API; HR, lossless and FEC disabled. Accepting 24-bit PCM does not enable HR mode.
- Google liblc3 revision 8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07, LC3plus short frame modes, native LC3_PCM_FORMAT_S24. Two independently encoded mono channels.
- Codec -O3, LTO, instruction cache enabled; Google additionally uses upstream fast-math.
- Same acknowledged ESB 2 Mbit/s transport, fixed 2440 MHz, 0 dBm. Radio/USB/decoder/DAC costs are excluded from API times. Combined work includes sequential RF and ACK waiting.

## Encoder-only time

Average / maximum milliseconds per stereo frame; input generation excluded for every implementation. Opus rows below use its fastest complexity 0.

| Implementation | 10 ms frame | 5 ms frame | 2.5 ms frame | 1.25 ms frame |
|---|---:|---:|---:|---:|
| ETSI LC3plus reference | 54.094 / 66.826 | 31.207 / 36.484 | 20.363 / 22.933 | 14.828 / 17.542 |
| Opus CELT, complexity 0 | 15.005 / 16.137 | 9.427 / 9.797 | 6.267 / 6.460 | Unsupported |
| Google LC3plus | 9.923 / 11.566 | 5.261 / 5.873 | 3.336 / 3.645 | Unsupported |

Google's 10 ms API average is just below the deadline, but its measured maximum is above it. All combined encoding/RF cases miss all 200 deadlines. No sustained encode-and-send case passes.

## Opus bitrate and complexity

At 2.5 ms and complexity 0, the prior 320 kb/s RES24 run averaged 6.435 ms, versus 6.267 ms here. The observed reduction is about 2.6%, despite a 20% reduction in payload size (100 to 80 bytes). Reducing bitrate does not proportionally reduce transforms, band processing or per-frame work. The earlier 320 kb/s run has its own retained image/source hashes.

| Opus complexity, 24-bit PCM | 2.5 ms API avg / max |
|---|---:|
| 0 | 6.267 / 6.460 |
| 5 | 10.122 / 10.623 |
| 10 | 14.054 / 24.266 |

## Encoding plus radio at 2.5 ms

| Implementation | Work avg / max ms | Late frames | RX good |
|---|---:|---:|---:|
| opus | 7.288 / 8.514 | 200/200 | 200/200 |
| google | 4.359 / 4.761 | 200/200 | 200/200 |
| etsi | 21.376 / 23.957 | 200/200 | 200/200 |

All 5000 transmitted encoded frames and 1600 synthetic radio-only frames arrived without CRC failures, missing frames or RX queue overflows. All transmitted encoded frames were late. These short tests establish integrity for the frames actually sent, not continuous real-time audio reliability. All retained timing cases pass the DWT-versus-RTC validity check.

## Optimization audit

- The prior Opus build already had -O3, LTO, Cortex-M4F hard-float ABI, fixed-point RES24, OPUS_ARM_INLINE_EDSP and OPUS_ARM_INLINE_MEDIA. The generated image contains smulwb/smlabb DSP instructions.
- There is no evidence of an accidentally unoptimized build or disabled instruction cache. The upstream portable routines and inline DSP operations are not a complete Cortex-M4-specific optimization. No CMSIS-DSP FFT substitution or bespoke M4 vector kernels have been implemented or benchmarked.
- Upstream NEON routines cannot be used on Cortex-M4. The separate upstream pitch assembly was not enabled; the complexity-0 encoder skips the expensive pitch_search branch inside run_prefilter (gated on complexity >= 5), so optimizing that branch cannot fix the fastest case.
- Opus hardening remains enabled; no quality-reducing changes beyond the documented complexity control were made. The source is the unmodified official release.
- At the original 320 kb/s, comparing wrapper timings gives roughly 6.5 ms Opus versus 18.2 ms ETSI and 3.6 ms Google. A general claim that one codec must be cheaper than another does not identify the implementation, CPU, frame duration or quality setting.

## What 256 kb/s says about transparency

256 kb/s is a candidate operating point, not a verified transparency threshold for this firmware. The timing test contains no decoded listening comparison.

[ETSI TR 103 633 v1.1.1, section 8.1, p.27](https://www.etsi.org/deliver/etsi_tr/103600_103699/103633/01.01.01_60/tr_103633v010101p.pdf) estimates near transparency from PEAQ on 12 critical 48 kHz mono music items at 128 kb/s for 10 ms, 144 kb/s for 5 ms and 160 kb/s for 2.5 ms. For independent stereo channels, these correspond to 256, 288 and 320 kb/s total. These are objective estimates, not guarantees for every listener or signal; the cited result does not establish a 1.25 ms threshold.

[Xiph's Opus guidance](https://wiki.xiph.org/Opus_Recommended_Settings) discusses transparency for VBR music storage and recommends the default 20 ms frame and complexity 10. [RFC 6716 sections 2.1.4, 2.1.5 and 2.1.8](https://www.rfc-editor.org/rfc/rfc6716.html#section-2.1.4) describe the frame-length, complexity and VBR tradeoffs. Those recommendations do not validate 2.5 ms CBR at complexity 0. A controlled, level-matched blind comparison at the actual settings is needed.

24-bit input is independent of perceptual transparency: the API can accept all input bits while lossy compression discards information. Standard LC3plus accepting 24-bit input is also distinct from LC3plus HR mode.

## Reproduction and retained evidence

Build each of `opus-fixed-lto`, `google-lto`, `etsi-lto` using:

```powershell
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx -Codec opus-fixed-lto -Frames 200 -Bitrate 256000 -PcmBits 24
```

Then flash and capture using [the existing instructions](../../OPUS.md). Use TX stop text SUITE_DONE and RX run 36 for Opus, run 12 for ETSI/Google. The final nRF52840 image is etsi-lto at 256 kb/s, 24-bit input; the RX image remains unchanged.

Raw TX/RX logs, complete JSON tables, selected compile commands, Kconfig snapshots and image/source hashes are retained beside this report. The RX logs may begin with buffered idle lines from the previous suite; only matching RX_RESULT records are joined.

- opus: [TX](opus-tx.log), [RX](opus-rx.log), [JSON](opus.json).
- google: [TX](google-tx.log), [RX](google-rx.log), [JSON](google.json).
- etsi: [TX](etsi-tx.log), [RX](etsi-rx.log), [JSON](etsi.json).
- [Manifest](manifest.json), [configurations](configs/).
