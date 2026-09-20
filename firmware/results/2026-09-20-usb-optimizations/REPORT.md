# nRF52840 USB encoder optimizations

Measured on 2026-09-20 using nRF52840 DK **000683088082** and the existing
nRF5340 packet verifier **001050038165**. The supplied
`24_48k_PerfectTest.wav` is streamed through Windows WASAPI exclusive mode into
the real stereo 48 kHz, packed 24-bit USB Audio endpoint. The custom codec uses
2.5 ms frames, coarse SNS, approximate VBR gain, and disabled TNS/LTPF. ESB uses
the previous 2 Mbit/s fixed-channel configuration with acknowledgements and
three permitted retries. Encoding and radio continue to overlap.

**The selected mask-31 build preserves the existing codec format and improves
buffered throughput at both 320 and 400 kb/s settings. It does not establish a
hard 2.5 ms processing or 5 ms delivery bound.** The 400 kb/s input drops seen
in the previous image are absent in the selected 50-second tests. The receiver
still verifies packets; these tests do not include receiver decoding or DAC
playback, controlled RF interference, clock-drift correction, or listening.

## Implementation

The build script accepts `-Optimizations <mask>`, default zero for reproduction.
The independent bits are:

| Bit | Implemented change |
|---|---|
| 1 | Omit test-only whole-PCM hashing and codec stage profiling, retaining overall timing, packet CRC, sequence and loss accounting |
| 2 | Aggregate the 44 energy bands directly into 16 SNS values; preserve the previous four-addend expressions, tilt, floor, scalar quantizer and shaping |
| 4 | Reuse the first spectral cost/endpoint when the packet already fits; keep the original second budget pass for capped or requantized frames |
| 8 | Encode directly into an owned radio packet, queue its pointer, retain ownership through ACK/failure, and use a 256-entry IEEE CRC32 table |
| 16 | Specialize the 60-point FFT using exactly the existing 5/3/2/2 radix sequence and coefficient tables |
| 32 | Experimental RAM placement of the encoder and forward transform; rejected due to worse measured performance |

The spectral fast path checks that both payload sizes select the same entropy
context and that neither enables LSB mode. The first estimate already reserves
the full spectral cost, residual bits and termination margin. Consequently the
second walk cannot truncate coefficients on this path; its endpoint is reused.
The original fallback remains intact when that reasoning does not apply.

The radio pool contains five packets: four can wait while one belongs to the
worker. Failed submissions release ownership. Packet headers, radio CRC,
quantization, SNS values, entropy format, decoder and rate control are unchanged.
This experiment does not replace arithmetic coding with Rice coding or add
adaptive bitrate; the measured improvements do not require those quality/format
tradeoffs.

## Matched 50-second cases

Each row offers 20,000 frames, with the same 100-frame warm-up and exactly the
same source waveform. Work includes applicable PCM validation, encoding,
submission and USB/radio preemption; it excludes waiting for PCM or ACK.

| Image | Setting | Frames received | Input drops | Work avg / max (ms) | Input queue peak | USB-ready to ACK max (ms) | ACKs beyond 5 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| Baseline, mask 0, run 1041 | 320 kb/s | 19,993 | 7 | 2.119 / 2.869 | 16 | 44.404 | 11,425 |
| Baseline, mask 0, run 1042 | 400 kb/s | 19,332 | 668 | 2.204 / 2.991 | 16 | 47.607 | 11,046 |
| SNS/spec/transport, mask 14, run 1031 | 320 kb/s | 20,000 | 0 | 1.976 / 2.808 | 3 | 8.423 | 622 |
| SNS/spec/transport, mask 14, run 1032 | 400 kb/s | 20,000 | 0 | 2.093 / 2.930 | 5 | 15.564 | 11,259 |
| Add specialized FFT, mask 30, run 1021* | 320 kb/s | 20,000 | 0 | 1.807 / 2.656 | 1 | 5.554 | 9 |
| Add specialized FFT, mask 30, run 1022* | 400 kb/s | 20,000 | 0 | 1.884 / 2.838 | 2 | 7.874 | 425 |
| **Selected lean build, mask 31, run 1061** | **320 kb/s** | **20,000** | **0** | **1.748 / 2.624** | **1** | **5.310** | **1** |
| **Selected lean build, mask 31, run 1062** | **400 kb/s** | **20,000** | **0** | **1.849 / 2.808** | **2** | **6.439** | **265** |

Compared with this session's baseline, selected average work falls by 17.5% at
320 and 16.1% at 400. Compared with the previous retained USB image's 2.078 /
2.180 ms averages, the reductions are 15.9% / 15.2%. The baseline remains close
enough to overload that minor overhead/layout and RF variations matter: the
earlier 320 run had zero drops; this fresh baseline has seven. Neither result
is erased. New baseline instrumentation includes a cheap encoded-packet
fingerprint, so it is not byte-identical to the earlier firmware image.

The selected encoder API averages/maxima are 1.574/2.314 ms at 320 and
1.730/2.664 ms at 400. Whole work exceeds 2.5 ms on 14 and 272 frames,
respectively. There are zero sequence, codec, send, ACK-failure, USB allocation,
USB malformed-packet, receiver CRC/overflow and timing-cross-check errors in
both selected 50-second cases. ESB retries are 12 and 3. The selected image
uses **231,904 bytes flash and 98,472 bytes RAM**.

Successful 50-second cases encode exactly 1,508,111 / 1,788,613 bytes, or
**241.298 / 286.178 kb/s actual codec payload**. The 320/400 labels are rate
ceilings, not constant payload rates. Radio headers/retries are additional.

## Longer selected-image verification

The same flashed mask-31 image subsequently ran for 150 seconds at each rate,
without rebuilding or resetting between the short and long tests:

| Metric | 320 kb/s, run 1071 | 400 kb/s, run 1072 |
|---|---:|---:|
| Offered / received frames | 60,000 / 60,000 | 60,000 / 60,000 |
| Input drops | 0 | 0 |
| Encoder API avg / max | 1.559 / 2.320 ms | 1.713 / 2.661 ms |
| Whole work avg / max | 1.732 / 2.594 ms | 1.831 / 3.112 ms |
| Work intervals above 2.5 ms | 23 | 777 |
| Input queue peak | 1 | 2 |
| USB-ready to ACK avg / max | 2.628 / 5.340 ms | 2.859 / 8.118 ms |
| ACKs beyond 5 ms | 1 | 832 |
| ESB retries | 58 | 288 |
| Actual codec payload rate | 237.661 kb/s | 280.913 kb/s |

Both runs have zero sequence, codec, send, ACK-failure, input-allocation,
malformed-USB, receiver CRC/overflow and timing-cross-check errors. The selected
image has therefore delivered all **160,000 frames across 400 seconds** of
short plus long tests. The 400 kb/s delivery maximum exceeds three 2.5 ms frame
periods even before receiver decoding/output, so a three-frame total latency
budget is not established. This remains successful buffered throughput on the
repeated supplied file, not a worst-case bound across all content and RF
environments.

## What helped, and what did not

The specialized transform is useful in the tested flash image: measured MDCT
averages fall from 697/736 us without it to 521/522 us with it. The other
changes also affect code layout and interrupt placement; individual stage
differences should not be treated as isolated causal savings. In particular,
bitstream-writing time varies even though its algorithm is unchanged. Its
byte-oriented stores do not support attributing that variation to output-buffer
alignment alone.

The RAM experiment (mask 62, runs 1051/1052) drops 27/808 input frames, with
2.107/2.198 ms average work and 3.022/3.357 ms maxima. MPU section alignment
increases RAM allocation to 147,368 bytes. Its hot functions demonstrably map
into RAM, but the tested placement is slower and is excluded from the selected
configuration. This result does not rule out every possible RAM layout.

## Integrity and timing validation

The native comparison checks **236,432 channel packets byte for byte** against
the previously validated encoder DLL: eight signal families, twelve codec
configurations, 256/320/400/480 kb/s, the complete WAV frames, and changing random
input. Every packet and reported rate/cap statistic matches. The source
generator preserves the pinned upstream files and produces modified copies.

Full-PCM hashing remains enabled in masks 14 and 30. All four complete cases
match the independently known input hash `7b4c85c4caed4b6d`. The lean image
explicitly reports `pcm_hash_enabled=0`; its absent whole-PCM check is recorded
as null, not a pass. Its encoded-stream fingerprints match both validated
variants at each rate: `d71b022c32527c0d` (320), `250025f38e59b76c` (400).
These combine per-packet IEEE CRC32 and lengths into a 64-bit accumulator and
are non-cryptographic checks, not an ARM byte-by-byte conformance certificate.

The original timer cross-check could compare different intervals if an IRQ
occurred between its wall-clock and DWT reads. The final implementation takes
each pair under a short IRQ lock; interrupt processing remains included in the
measured work between the paired marks. Runs 1022 and 1041 retain one old-style
cross-check discrepancy each. Selected runs use the corrected measurement.

`USB-ready` means the callback providing the final bytes of a codec frame.
Delivery measurements exclude host buffering (10 ms in this player), earlier
frame accumulation, receiver decoding and DAC output. An observed sender queue
peak is not a guaranteed receiver playout-buffer requirement. The 16-slot input
queue's capacity was not reduced to the observed peak.

*The first mask-30 archive attempt failed on Windows PowerShell UTF-16 logs.
The next build had already replaced the ELF/HEX before recovery. Its raw
measurements are retained with an explicit `all-profiled/ARCHIVE_NOTE.md`;
misassociated mask-14 binaries are clearly renamed. Subsequent archives finish
before rebuilding and check the selected sysbuild mask. They contain exact
images, generated source, configurations, host/TX/RX logs and SHA256 manifests.

## Reproduce

```powershell
& .\tools\audio-bench.ps1 -Action build -Target tx -Codec google-custom-lto -PcmBits 24 -UsbAudio -Optimizations 31
& .\tools\audio-bench.ps1 -Action flash -Target tx -Codec google-custom-lto -UsbAudio
& .\tools\run-usb-optimization.ps1 -Label new-lean-test -RunId 2001 -Frames 20000
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\validate-usb-optimizations.py
```

Use `-Optimizations 30` for full PCM integrity and stage profiling. See
`firmware/USB_PCM.md` for protocol, ownership and timing definitions. The normal
marker-controlled benchmark remains installed on the nRF52840; the nRF5340
packet verifier and mouse firmware are unchanged.
