# nRF52840 SBC dual-channel encoding at 368 kb/s

Measured on 2026-09-19. TX nRF52840 DK 000683088082, 64 MHz; RX nRF5340 DK 001050038165, network-core ESB receiver.

**The encoder fits comfortably. The current transport does not yet demonstrate consistently meeting every deadline.** Both 100-second runs delivered all 37,500 encoded frames; the repeat had 656 late frames and three skipped source intervals. No receiver decoding, USB audio, DAC output, or listening test was performed.

## Configuration

- Google libsbc, pinned revision 6e505650145c9973d08a0bdd5e5f5e1914305e40, Apache-2.0. Source obtained by tools/fetch-sbc.py with archive SHA256 verification.
- 48 kHz stereo, signed 16-bit input, dual channel, loudness allocation, eight subbands, 16 blocks. This is an XQ-style dual-channel configuration, not a claim that 368 kb/s defines every SBC-XQ preset.
- 128 samples/channel per frame: exactly 8000/3 microseconds. The integer frame_us=2667 field is a display/configuration tag; the scheduler uses the rational period.
- Bitpool 27 gives 120 bytes (360 kb/s); bitpool 28 gives 124 bytes (372 kb/s). Repeating 27,28,28 gives exactly 368 kb/s average. A fixed 372 kb/s comparison is also included.
- Codec compiled with -O3 and LTO; instruction cache enabled. API timing excludes PCM generation and packet validation. Combined work includes those plus sequential acknowledged RF transmission.
- ESB 2 Mbit/s, fixed 2440 MHz, 0 dBm, three retries and 600 us retry delay. No channel hopping, FEC, or audio playout buffer.
- Deterministic tones, noise, silence and transients, with RNG reset per case; 600 frames in each short case and 37,500 frames in each soak.

## Upstream correction

The upstream encoder's compute_scale_factors() already processes both dual-channel channels. encode_frame() then calls it again with pointers offset by one channel, accessing past the two-channel arrays. sbc.cmake verifies and removes that redundant call in a generated copy, sbc_port.c. The downloaded source is unchanged. Independent FFmpeg decoding was used to check generated streams. This is a narrowly corrected implementation, not an unmodified upstream benchmark.

## Results

Times below are microseconds. Run IDs: 1 local 368, 2 local 372, 3 RF 368, 4 RF 372, 5 RF 368 soak.

| Case | API avg / max | Work avg / max | Late / skipped | Retries | RX good |
|---|---:|---:|---:|---:|---:|
| Repeat, local 368, 600 frames | 724 / 776 | 817 / 885 | 0 / 0 | 0 | N/A |
| Repeat, RF 368, 600 frames | 722 / 735 | 1949 / 1984 | 0 / 0 | 0 | 600 / 600 |
| Repeat, RF 372, 600 frames | 723 / 735 | 1959 / 1984 | 0 / 0 | 0 | 600 / 600 |
| First soak, 368, 37,500 frames | 722 / 735 | 1948 / 1984 | 0 / 0 | 0 | 37,500 / 37,500 |
| Repeat soak, 368, 37,500 frames | 722 / 735 | 1968 / 5676 | 656 / 3 | 585 | 37,500 / 37,500 |

The API consumes about 27.1% of the frame budget in the RF soak. Worst measured encoding including input preparation is 839 us. All cases have zero codec, send, and DWT-versus-RTC timing errors. All retained complete receiver results have zero missing frames, bad CRCs, and queue overflows. Retry counts and late-frame counts measure different events and need not match. The logs do not isolate the cause of each late frame.

The first RX capture began with a full stale idle buffer: its run 3 result is absent and its run 4 result has a damaged line prefix. These short cases are not treated as complete paired evidence. Run 5 is intact. The repeat capture contains all three expected RX_RESULT lines. Both original logs are retained without repairs.

## Host codec checks

tools/test-sbc.py exercised the same corrected C codec through a native Clang DLL. Seven 300-frame stereo streams (tones at -3, -60, -100 and -110 dBFS; silence; noise; alternating rails) were encoded, decoded by the library, and independently decoded by FFmpeg. Each stream contains 38,400 samples/channel and 36,800 encoded bytes, exactly 368 kb/s.

The -3 dBFS tones measured about 64.3 and 68.1 dB signal-to-residual ratio at 997 and 1703 Hz. These are whole-codec results for this implementation and bit allocation, not isolated filterbank measurements, THD-only measurements, or a universal equivalent-bit-depth limit. FFmpeg and the library output are not bit-identical; their RMS differences are recorded, including 0.00388 full scale for the noise test. This functional check is not a codec conformance or perceptual-transparency result.

The -100 and -110 dBFS tests round to zero in the undithered 16-bit source generator, so they cannot assess codec preservation of below-LSB signals. This is not a claim that properly dithered 16-bit PCM has an absolute audibility cutoff at those levels.

## Status and reproduction

Only the 16-bit SBC implementation has been completed. Work on a proposed 24-bit extension was deferred when the discussion turned to filterbank limits and alternative codecs. No 24-bit SBC firmware or measured result is claimed. TX remains flashed with this 16-bit SBC suite; RX remains the codec-agnostic receiver.

```powershell
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\fetch-sbc.py
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx -Codec sbc-lto -Frames 600 -SoakFrames 37500 -Bitrate 368000 -PcmBits 16
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action flash -Target tx -Codec sbc-lto
```

Start/drain RX RTT before resetting TX. Capture RX through `RX_RESULT run=5 ` and TX through `SUITE_DONE`, without detaching either reader during the measured suite. Build and RTT tooling intentionally use no execution-policy override.

Raw logs, parsed JSON, host-validation metrics, generated port source and image/source hashes are retained alongside this report. Source files remain in firmware/benchmark and tools.
