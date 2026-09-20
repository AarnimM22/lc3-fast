# Opus CELT benchmark

This extends the [LC3plus bench](README.md) on the same nRF52840 TX and nRF5340
RX. Settings are 48 kHz stereo, 320 kb/s total, constant bitrate, full bandwidth,
CELT-only `OPUS_APPLICATION_RESTRICTED_LOWDELAY`, no FEC, no DTX, and complexity
0, 5 and 10. Standard Opus supports 2.5, 5 and 10 ms frames in this matrix;
1.25 ms is not an Opus frame duration. The encoder reports 120 samples (2.5 ms)
of lookahead at 48 kHz, in addition to collecting a frame.

`-Bitrate 256000` selects the updated 256 kb/s target. The default remains
320000 for reproducing earlier runs. Opus always sweeps both 16- and 24-bit input;
`-PcmBits 24` selects the input depth for the separate LC3plus implementations.
See the [256 kb/s comparison](results/2026-09-19-256k/REPORT.md).

## Source and builds

The unmodified official [libopus 1.6.1 release](https://opus-codec.org/downloads/)
is downloaded by `tools/fetch-opus.py`. Its published archive SHA256 is
`6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`.

Three variants use `-O3`, LTO and the SDK's Cortex-M4F ABI:

- `opus-float-lto`: single-precision floating point, upstream `FLOAT_APPROX`.
- `opus-fixed-lto`: fixed point with `ENABLE_RES24`, preserving 24-bit input.
- `opus-fixed16-lto`: standard fixed-point internal precision, tested with
  16-bit input only. The 24-bit cases are explicitly rejected, because merely
  calling `opus_encode24` on this build would downconvert to 16 bits.

The fixed-point builds use upstream ARM EDSP/MEDIA inline instructions supported
by Cortex-M4. NEON and desktop runtime CPU detection are not enabled. Upstream
hardening is enabled; neural extensions and custom modes are disabled. Fast-math
is not used. Upstream source lists are read from their make fragments rather
than changed. The compiler emits upstream double-promotion and theoretical
large-loop warnings; there are no codec source patches.

## Measurements

Each variant tests local encoding and encoding followed by acknowledged RF
transmission, using the existing ESB receiver and packet CRC checks. Codec cases
run for 200 frames each. Complexity and PCM depth are in each `RESULT` row.

- `api_avg_us` / `api_max_us`: only the `opus_encode` or `opus_encode24` call.
- `enc_*`: also includes PCM preparation and packet metadata validation, for
  comparison with the earlier LC3plus measurements.
- `work_*`: adds CRC, fragmentation, ACK waiting and retransmissions.
- `timing_errors`: compares DWT elapsed time with the independent RTC clock.
  A nonzero value invalidates a timing case. DWT is re-enabled before each frame
  because J-Link can clear trace enable when disconnecting.

Use a continuous capture long enough to include `SUITE_DONE`. A capture that
ends during encoding may invalidate that frame; do not splice such a run into
the final timing data. The first interrupted floating-point attempt is excluded
from retained results.
The RTT reader now reads control-block words atomically. Earlier byte accesses
could observe a torn producer offset and insert stale/duplicate text at a ring
wrap. The affected fixed RES24 capture is repeated and excluded. Retained suites
must contain exactly one complete result per expected case and a completion mark.

Input follows the earlier tones/noise/silence/transient pattern. RNG state resets
for every case. The 24-bit signal has nonzero low eight bits except in silence;
the 16-bit signal is its quantized counterpart. Both input buffers are prepared
in both tests, and the API-only timer isolates the actual encoder difference.
24-bit values are signed values in 32-bit containers, passed to the native
`opus_encode24` API, with `OPUS_SET_LSB_DEPTH(24)`.

Every encoded packet is checked for expected constant byte count, CELT ToC,
stereo, full bandwidth and sample count. RX verifies reassembled packet CRCs.
Neither this metadata validation nor CRC checking establishes audio quality or
bitstream conformance; decoding and listening tests are outside this bench.

## Run

From the repository root, using the existing PowerShell execution policy:

```powershell
python .\tools\fetch-opus.py
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx -Codec opus-float-lto -Frames 200
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action flash -Target tx -Codec opus-float-lto
```

Capture immediately after flashing, in two terminals:

```powershell
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action rtt -Target tx -Codec opus-float-lto -Seconds 240 -StopOn SUITE_DONE
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action rtt -Target rx -Codec etsi -Seconds 240 -StopOn 'RX_RESULT run=36 '
```

Use the matching variant for TX build/flash/RTT. RX retains the earlier receiver
image and is codec-agnostic. End capture before flashing or resetting its probe.
For `opus-fixed16-lto`, use RX stop text `'RX_RESULT run=30 '` because 24-bit
cases are unsupported. The timeout remains a cap if the expected text is absent.
`-SoakFrames N` adds complexity-0 encode-and-transmit tests at 2.5 ms for both
input depths; it differs from the earlier LC3plus radio-only soak option.

24-bit PCM increases packed raw input bandwidth from 1.536 to 2.304 Mb/s at
48 kHz stereo. The 320 kb/s compressed stream stays the same size. CPU cost
depends on arithmetic, conversion, content and complexity rather than being
fixed by output bitrate. Lossy 320 kb/s encoding does not preserve 24-bit PCM
losslessly. USB input and its packing/conversion costs are not measured here.
