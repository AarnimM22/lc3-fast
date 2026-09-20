# USB PCM timing benchmark

This is a USB Audio Class 2 source-to-radio benchmark on the nRF52840 DK,
serial `000683088082`. Connect both its debugger USB and its **nRF USB** port.
The nRF5340 DK, serial `001050038165`, receives on its network core and now
decodes and drives I2S on its application core; see the
[receiver playback and clock recovery implementation](benchmark/app_core/README.md).
The desktop player selects only the endpoint named `nRF52840 PCM Bench`;
it never selects the default audio output.

The endpoint advertises stereo 48 kHz packed 24-bit PCM. It uses the installed
Zephyr UAC2 class and Nordic USB controller driver. USB SOF is the sample-clock
reference. Receiver-side audio PLL adjustment follows this source rate, without
USB feedback or additional ESB synchronization packets. This does not yet test
analog output or constitute a complete USB audio product.

The codec is the previous coarse-SNS, approximate-VBR custom format with TNS
and LTPF disabled, at a 320 or 400 kb/s stereo ceiling. Codec frames contain
120 stereo samples (2.5 ms). USB packets normally contain 48 stereo samples
(1 ms), so two/three-packet frame completion intervals are expected.

## Build and run

From the workspace root, using the existing PowerShell execution policy:

```powershell
& .\tools\audio-bench.ps1 -Action build -Target tx -Codec google-custom-lto -PcmBits 24 -UsbAudio
& .\tools\audio-bench.ps1 -Action flash -Target tx -Codec google-custom-lto -UsbAudio
& .\tools\build-usb-pcm-host.ps1
```

The USB variant uses `.bench/usb-tx`, leaving the synthetic benchmark's image
in its separate build directory. `-UsbAudio` is required for build, flash and
RTT so the correct ELF and target image are selected. The host player requires
Visual C++ and the Windows WASAPI APIs; playback uses exclusive mode, with no
sample-rate conversion or shared mixer requested. Its host buffer is 10 ms;
the device still receives 1 ms USB transactions. Host-buffer latency is not
part of the device timing measurements.

Start TX and RX capture in separate terminals:

```powershell
& .\tools\audio-bench.ps1 -Action rtt -Target tx -Codec google-custom-lto -UsbAudio -Seconds 220 -StopOn 'USB_DONE id=912'
& .\tools\audio-bench.ps1 -Action rtt -Target rx -Codec etsi -Seconds 220 -StopOn 'RX_RESULT run=912'
```

Then stream 20,000 codec frames (50 seconds) at each bitrate:

```powershell
.\.bench\usb-pcm-host\usb-pcm-host.exe .\firmware\benchmark\24_48k_PerfectTest.wav 911 320000 20000
.\.bench\usb-pcm-host\usb-pcm-host.exe .\firmware\benchmark\24_48k_PerfectTest.wav 912 400000 20000
```

Arguments are WAV path, run ID, stereo codec bitrate and measured frame count.
The player loops the WAV as necessary. It requires two channels, 48 kHz and
three bytes/sample; it rejects other formats. It sends one second of startup
silence, the test marker/control header, 100 warm-up codec frames (250 ms), the
measured WAV data, and one second of drain silence. Only the measured data is
sent as codec frames to the nRF5340. These extra intervals do not contribute
silence to the reported codec bitrate.

Normal audio playback without the marker does not initiate a test. This is
deliberately a controlled benchmark endpoint, not finished dongle firmware.

## Optimization experiments

`audio-bench.ps1 -Action build` accepts `-Optimizations <mask>`. Zero keeps the
previous codec/transport paths for comparisons. Add these independent bits:

| Bit | Change |
|---|---|
| 1 | Omit whole-PCM fingerprint and codec stage profiling; retain frame, packet CRC, timing and loss accounting |
| 2 | Direct 44-to-16 coarse SNS energy aggregation, preserving the old four-addend expressions |
| 4 | Reuse the first spectral-cost result when unchanged quantization provably fits; retain the original capped-frame fallback |
| 8 | Owned radio packet slab, pointer queue, direct encoding into its payload, and byte-table IEEE CRC32 |
| 16 | Specialize the 60-point FFT with the existing radix order and coefficient tables |
| 32 | Experimental RAM placement of `lc3_encode` and `lc3_mdct_forward`; measured regression, leave disabled |

Mask 30 keeps input integrity/profiling enabled while applying all four
implementation changes. Mask 31 additionally removes those test costs.
The decoder, quantization choices, rate control and wire format are unchanged.
The `USB_PAYLOAD` line records the mask and explicitly states whether the PCM
hash and stage profiling are enabled. Disabled PCM validation is recorded as
`null`, never as a successful integrity check.

Each encoded packet also updates a cheap 64-bit fingerprint: XOR the IEEE
CRC32 in the low 32 bits and packet length in the high 32 bits, then multiply
by 1099511628211 modulo 2^64, starting from 14695981039346656037. This allows
on-device output comparison without sending entire streams through RTT. It is
an additional non-cryptographic check, not a substitute for packet equivalence
tests or the whole-PCM hash in validation builds.

After building/flashing, the paired capture/player/archive helper runs both
rates with unique adjacent run IDs (use a new label for each experiment):

```powershell
& .\tools\audio-bench.ps1 -Action build -Target tx -Codec google-custom-lto -PcmBits 24 -UsbAudio -Optimizations 30
& .\tools\audio-bench.ps1 -Action flash -Target tx -Codec google-custom-lto -UsbAudio
& .\tools\run-usb-optimization.ps1 -Label example-profiled -RunId 2001 -Frames 20000
```

Results and source/image snapshots are in
`firmware/results/2026-09-20-usb-optimizations/<label>`. The helper refuses to
overwrite a label. Do not rebuild until capture and archival have completed.

## Input and buffer integrity

USB DMA owns receive buffers until its completion callback. The callback
assembles PCM with block copies into an 18-buffer slab; a 16-entry queue passes
ownership by pointer. One buffer may be under construction and another under
encoding. The main thread releases a buffer only after encoding has consumed
it. Queue overflow drops and counts a complete codec input frame, retains its
sequence gap and causes integrity validation to fail; it never silently
slows the host source to conceal CPU overload.

The codec consumes packed interleaved PCM directly through
`LC3_PCM_FORMAT_S24_3LE`, stride 2, with right-channel offset 3 bytes. A host
comparison on the supplied WAV verified 21,256 channel packets identical to
the previous planar signed-24-bit input path at 320/400 kb/s.

The final test calculates a 64-bit whole-stream fingerprint before encoding:
start with 14695981039346656037; for each little-endian 32-bit PCM word, XOR
then multiply by 1099511628211 modulo 2^64. This is a **word-wise FNV-derived
test fingerprint**, not the standard byte-wise FNV algorithm or a cryptographic
proof. The host computes the expected value. Its cost remains inside measured
producer work. The earlier prototype used a slower software PCM CRC32; its
failed tests are retained separately. Radio packets continue to use CRC32.

The marker is 48 bytes: `NRFPCM24nrf52840` repeated three times. It is followed
by eight little-endian 24-bit words: run ID, bitrate, measured frame count,
`0x544553`, then the 24-bit complement of those four words. This ensures that
format conversion, channel swapping or loss of low-order bits cannot silently
start an apparently valid test. There is no custom USB host driver or bulk/CDC
data path involved.

## Interpreting results

- `work_*`: integrity check, codec call, radio CRC/queue submission and buffer
  release, including USB/radio preemption during that interval. It excludes
  waiting for input and for radio ACK.
- `api_*`: both channel encoder calls, including preemption.
- `work_over_period`: work duration above 2.5 ms.
- `producer_late`: submission more than 2.5 ms after the USB callback that
  supplied that frame's final PCM bytes. Queue waiting is included here.
- `queue_age_max_us`: longest wait from that callback to processing start.
- `ready_to_ack_*` / `radio_late`: delivery after that callback; the budget is
  5 ms. This is not PC-to-DAC latency and excludes host buffering and earlier
  USB/codec frame accumulation.
- `overflows`, sequence errors, timeouts and fingerprint mismatch expose lost
  input. Receiver missing/CRC/overflow fields independently check RF delivery.
- Callback statistics include the warm-up interval. They measure application
  receive callbacks, not the whole USB driver's CPU time, and can overlap the
  encoder timing when preemption occurs; do not add them twice.

The firmware uses the full newlib library because this toolchain's nano
library selects a small byte-copy implementation. It reports failures and
remains ready for another test; there is no automatic sample dropping to
maintain a misleadingly constant latency.

See [the retained USB results](results/2026-09-20-usb-pcm/REPORT.md) for the
measured outcome and the distinction between throughput and latency budgets.
