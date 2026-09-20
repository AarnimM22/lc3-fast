# Google LC3plus encoding on nRF54LM20A

Measured on 2026-09-19 with nRF Connect SDK 3.2.1 and GCC 12.2.0.

**The 128 MHz Arm Cortex-M33 meets the tested stereo encoding deadlines, including 2.5 ms at both 256 and 320 kb/s with 24-bit input. RISC-V offloading was not required to meet the encoding budget and was not implemented.** The existing sequential encode-and-wait-for-ACK transport misses the 2.5 ms cadence; encoder feasibility does not establish a complete real-time audio system.

This report preserves the sequential baseline. A subsequent [concurrent-radio experiment](../2026-09-19-nrf54-async/REPORT.md) implements and measures overlapping encoding and transmission.

## Hardware and firmware

| Probe | Board | Final role |
|---|---|---|
| 001051883190 | nRF54LM20 DK, PCA10184 | Google LC3plus encoding benchmark on Arm; user reconfirmed this board for audio after identifying its former mouse role |
| 001050038165 | nRF5340 DK | ESB frame/CRC receiver on network core |
| 001051802784 | nRF54LM20 DK | Restored mouse receiver firmware, explicitly requested by user |

The nRF52840 DK was disconnected and was not accessed. Its previous results are historical comparisons, not concurrent reruns.

- Unmodified Google liblc3 revision `8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07`, using its LC3plus short-frame support.
- 48 kHz stereo, signed 24-bit PCM in 32-bit containers, two independent channel encoders. The source includes nonzero low bits and deterministic tones, noise, silence and transient mixtures. RNG and codec state reset per case.
- Standard 48 kHz mode, not LC3plus HR. The compiled library supports HR, but the benchmark uses `lc3_setup_encoder`, not its HR setup API. No FEC or lossless mode.
- 256000 and 320000 bits/s total stereo, with 10, 5 and 2.5 ms frames. Google liblc3 does not offer 1.25 ms frames.
- `-O3`, LTO, upstream-style `-ffast-math`, Cortex-M33 `fpv5-sp-d16` hard-float ABI, instruction cache enabled, 128 MHz core. No codec arithmetic changes or RISC-V work.
- Linked TX usage: 192880 bytes nonvolatile memory, 78008 bytes RAM. The two encoder states require 5032 bytes for 2.5 ms, 6952 for 5 ms and 10792 for 10 ms. Benchmark RAM also includes large stacks, RTT and radio buffers.
- Same ESB 2 Mbit/s, 2440 MHz fixed channel, 0 dBm, three retries, 600 us retransmit delay. No hopping, playout queue, USB Audio, receiver decoding or DAC output.

## Encoding results

API-only average / maximum milliseconds, excluding input preparation and transmission. Each table cell is a 1000-frame local case.

| Stereo bitrate | 10 ms frames | 5 ms frames | 2.5 ms frames | Average 2.5 ms CPU load |
|---|---:|---:|---:|---:|
| 256 kb/s | 4.776 / 5.643 | 2.669 / 2.977 | 1.794 / 2.013 | 71.76% |
| 320 kb/s | 4.875 / 5.721 | 2.711 / 3.065 | 1.821 / 2.049 | 72.84% |

At 2.5 ms the maximum including PCM preparation is 2.034 ms at 256 kb/s and 2.066 ms at 320 kb/s. These synthetic measurements leave about 0.43-0.47 ms at the observed worst case for additional per-frame CPU work. They are not a worst-case execution-time proof for every audio signal or for a complete USB/radio application.

The prior nRF52840 256 kb/s, 24-bit, 2.5 ms API result was 3.336 / 3.645 ms. The nRF54 average here is about 1.86 times faster. The same library, input generator, codec flags and settings were retained; the CPU, memory system and board differ. Previous runs used 200 frames per case, this test uses 1000. See [the prior report](../2026-09-19-256k/REPORT.md).

## Encoding plus radio

The existing mode 1 benchmark encodes a frame, sends it, and waits for its ACK before advancing. It does not overlap radio transfer with the next frame's encoding.

| 1000-frame case | Combined work avg / max ms | Late frames | Skipped source intervals | RX verified |
|---|---:|---:|---:|---:|
| 256 kb/s, 10 ms | 7.358 / 9.440 | 0 | 0 | 1000 |
| 256 kb/s, 5 ms | 3.976 / 5.568 | 9 | 0 | 1000 |
| 256 kb/s, 2.5 ms | 2.714 / 3.936 | 947 | 89 | 1000 |
| 320 kb/s, 10 ms | 7.794 / 9.888 | 0 | 0 | 1000 |
| 320 kb/s, 5 ms | 4.214 / 7.328 | 51 | 0 | 1000 |
| 320 kb/s, 2.5 ms | 2.826 / 4.096 | 1000 | 132 | 1000 |

These results identify a transport/scheduling limit after the encoder became fast enough. A streaming implementation should overlap radio activity and encoding, then measure bounded buffering and late/lost-packet behavior. That overlap has not been implemented or validated by this experiment. Moving floating-point codec work to the RISC-V core was not needed to pass the requested encoding test, and no claim is made about RISC-V codec performance.

Both 40000-frame radio runs received every frame with zero codec errors, send errors, timing errors, bad CRCs or RX overflow. At 256 kb/s, API average/max was 1.792/2.023 ms, sequential work 2.712/4.992 ms, with 37620 late frames and 3572 skipped source intervals. At 320 kb/s, API average/max was 1.819/2.049 ms, sequential work 2.823/4.128 ms, with 39997 late frames and 5246 skipped source intervals. Totals across all cases: 92000 frames encoded and 86000 radio frames verified. Full results are in [the generated tables](tables.md) and [machine-readable data](results.json).

## Measurement and port details

DWT measures both the codec API and the wrapper, with cycle conversion based on `SystemCoreClock` instead of the old fixed divide-by-64. Each frame's wrapper timing is cross-checked against RTC/GRTC-backed uptime with the existing 200 us tolerance. The nRF54 kernel uptime resolution is 32 us. Average and maximum cover every frame; the reported P99 uses at most the first 1024 frames. Capture remains attached for the complete retained suite.

The radio port follows the SDK's nRF54LM20A PLL-start workaround (MLTPAN-39). ESB's `tx_output_power` parameter takes signed dBm, so it is set to integer zero; the old `ESB_TX_POWER_0DBM` register encoding differs on this SoC and caused repeated conversion warnings before correction.

One initial nRF54 flash failed on RRAM write protection. Erasing only image ranges resolved this without chip recovery or key reprovisioning. The nRF5340 network core became inaccessible after reset; its benchmark application/network images were recovered and reloaded before the retained paired run. Captures with a silent receiver or transmit-power warnings are excluded from paired transport evidence; their successful local encoding measurements are preliminary only. The reported table comes from the subsequent complete matched run.

This is a computational and transport-integrity test with synthetic PCM, not a listening, conformance, USB Audio or CS43131 output test. A 2.5 ms frame is not total codec or end-to-end playback latency.

## Mouse dongle restoration

At the user's request, serial `001051802784` received the exact saved `radio-deadline` mouse receiver image previously recorded for `001051883190`. Its SHA256 is `3e781d92bb3c50b7c4ac11d93d9b83ac2dd69e324eeee9f3f4c21dc2a49dfcf6`. Both probes' recorded provisioning keyfile hashes matched. Range-only programming and readback verification completed successfully; there was no chip erase or key reprovisioning.

The boot log confirms stored pairing loaded, AES-CCM known-answer test passed, and radio receiver READY. The DK was attached through its debugger only, so USB enumeration and actual mouse movement were not tested. The nRF USB port must also be connected for HID input to reach the PC.

Restoration used the pinned image rather than rebuilding from newer mouse sources. The mouse project's advisory bench-state file was not changed; [mouse-restoration.json](mouse-restoration.json) and [the boot log](mouse-restoration-boot.log) record the operation here. The helper `tools/restore-mouse-dk.ps1` is restricted to the saved image hash and the explicitly requested second probe.

## Reproduction and evidence

```powershell
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx54 -Codec google-lto -Frames 1000 -SoakFrames 40000 -PcmBits 24
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action flash -Target tx54 -Codec google-lto
```

Capture the receiver before restarting the transmitter; use RX stop text `RX_RESULT run=14 ` and TX stop text `SUITE_DONE`. Allow 420 seconds for the full suite. RX is selected explicitly with `-Target rx -Codec etsi`.

- [TX capture](tx.log), [RX capture](rx.log).
- [Complete tables](tables.md), [JSON results](results.json), [image/source hashes](manifest.json).
- Matching images, ELF files, Kconfig, devicetrees and compile commands are retained beside this report.

Primary hardware sources: [nRF54LM20A product specifications](https://www.nordicsemi.com/Products/nRF54LM20A) and the installed SDK board definitions under `zephyr/boards/nordic/nrf54lm20dk`.
