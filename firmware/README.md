# LC3plus transmitter feasibility bench

The [receiver clock-recovery test](results/2026-09-20-clock-servo-final/REPORT.md)
adds application-core decoding, continuously clocked I2S, concurrent peripheral
load and a receiver audio PLL servo. At 384 kb/s it passed both normal playback
and deliberately injected clock offsets without increasing the startup prefill.

The [USB PCM streaming test](USB_PCM.md) adds Windows USB Audio playback of
the supplied 24-bit/48 kHz WAV, concurrent encoding and radio transmission.
Its [measured results](results/2026-09-20-usb-pcm/REPORT.md) distinguish input
integrity, throughput and latency; USB is enabled only with `-UsbAudio`.

The [subsequent implementation optimizations](results/2026-09-20-usb-optimizations/REPORT.md)
add equivalent coarse-SNS aggregation, spectral-cost reuse, an owned radio
packet queue and a specialized transform. `-Optimizations 31` selects the lean
flash-resident candidate; `30` retains full PCM validation and stage profiling.

The [custom SNS and variable-rate codec experiment](results/2026-09-20-lc3-custom/REPORT.md)
adds coarse/scalar SNS, unity bypass and approximate gain selection on nRF52840,
with a matching custom host decoder and 24-bit/48 kHz/2.5 ms tests. The
`google-custom-lto` variant requires `-PcmBits 24`; its report contains the matrix
and endurance commands, quality limitations and retained hardware evidence.

The subsequent [Opus CELT and 16/24-bit PCM benchmark](OPUS.md) uses the same
boards and transport.

The [256 kb/s, 24-bit comparison](results/2026-09-19-256k/REPORT.md) adds matching
PCM and encoder-call-only timing for ETSI, Google and Opus. Build with
`-Bitrate 256000 -PcmBits 24` to select this LC3plus configuration. Defaults are
320000 and 16; Opus sweeps both bit depths regardless of `-PcmBits`.
For a non-default bitrate, the LC3plus suite tests the selected rate at all four
frame durations without the historical extra 160/240 kb/s cases. Its last run
ID is 12, including radio-only runs (Google skips the 1.25 ms codec cases).
The current LC3plus wrapper resets its test-signal RNG for each case to match
Opus; earlier reports retain their original source hashes and measurements.

Tests whether a 64 MHz nRF52840 can encode stereo PCM and send LC3plus to an
nRF5340 DK. The KiCad files and outdated hardware design notes are not inputs to
this benchmark and have not been edited.

## Targets and software

- TX: nRF52840 DK, J-Link serial **000683088082** / `683088082`.
- RX: nRF5340 DK, J-Link serial **001050038165** / `1050038165`.
- NCS `C:\ncs\v3.2.1`, bundled toolchain `66cdf9b75e`, GCC 12.2.0.
- ESB 2 Mbit/s, channel 40 (2440 MHz), 0 dBm, acknowledgments, three retries,
  600 microsecond retransmit delay, maximum payload 252 bytes.
- The nRF5340 network core receives and verifies frames. Its application core
  runs Nordic's `empty_app_core` startup image, which enables the network core.
- Official [ETSI TS 103 634 v1.7.1 source attachment](https://www.etsi.org/deliver/etsi_ts/103600_103699/103634/01.07.01_60/ts_103634v010701p0.zip),
  codec software 1.9.2, source revision `78d6b913cb5`, June 15, 2026.
- Optional [Google liblc3 comparison](https://github.com/google/liblc3/tree/8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07).
  This implements LC3plus 2.5/5 ms modes; it does not implement 1.25 ms.

The ETSI archive's SHA256 is
`0b9de69247d2f24ae782ada4255e31c044a1ab4f14ec6798ccf6dd3651d77f7e`.
Downloaded third-party sources retain their license notices and are ignored by
Git. Restore them with `python .\tools\fetch-codecs.py --google`.

## Run

Run from the repository root. These commands use the existing execution policy.
The local script is intended for Windows PowerShell 5.1. Build and flash need
access to the installed SDK's cache and Nordic tool logs.

```powershell
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action list
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target rx -Codec etsi
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx -Codec etsi -Frames 200
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action flash -Target rx -Codec etsi
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action flash -Target tx -Codec etsi
```

Start captures in two terminals, one per probe:

```powershell
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action rtt -Target rx -Codec etsi -Seconds 150
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action rtt -Target tx -Codec etsi -Seconds 150
```

Each suite starts ten seconds after boot and runs once. To repeat it, end both
captures before `-Action reset -Target tx`. Never flash/reset a probe while its
capture owns it. `-Codec` accepts `etsi`, `etsi-lto`, `google`, `google-lto`.
`-SoakFrames 30000` adds 75-second and 37.5-second nominal radio-only runs at
320 kb/s with 2.5 ms and 1.25 ms frames respectively. Use a capture long enough
for both the codec matrix and any soak runs.

Builds and temporary logs are in `.bench/`. Retained evidence is in
`firmware/results/2026-09-19/`. Join matching captures with:

```powershell
python .\tools\summarize-bench.py TX.log RX.log result.json
```

## What is measured

Input is stereo 48 kHz, signed 16-bit PCM. The deterministic source alternates
tones, noise, silence and transient mixtures every 25 frames. Both channels are
encoded on the nRF52840. Bitrates are **total stereo bitrate**, not per channel.
High-resolution mode, FEC, and lossless mode are disabled in the reference tests.

- Mode 0: encoding without RF transmission, 320 kb/s at 10/5/2.5/1.25 ms.
- Mode 1: encoding followed by acknowledged RF delivery, the same matrix plus
  160 and 240 kb/s at 5 ms.
- Mode 2: synthetic payloads at the exact codec cadence, without encoding.

The CPU cycle counter measures encoding plus the small PCM-generation overhead.
Conversion uses `SystemCoreClock` (64 MHz on nRF52840, 128 MHz on nRF54LM20A).
Work time uses Zephyr's RTC/GRTC-backed uptime, with about 30.5 microsecond
resolution on nRF52840 and 32 microseconds in the current nRF54 build, and includes CRC, fragmentation, queueing,
acknowledgments and retransmissions. P99 is based on the first 1,024 frames at
most; average and maximum cover every frame. Mode 2's `enc_*` fields measure
synthetic payload generation, not encoding.

The scheduler advances absolute audio deadlines. `late` counts completed frames
that missed that deadline. `skipped` counts additional whole source intervals
lost when work cannot keep up. Late frames are sent for diagnostic visibility;
there is no claim that their arrival means timely audio playback. Source input
is generated locally, so no USB PCM acquisition cost is hidden in these fields.

Frames carry a run ID, sequence, length, fragment offset and CRC32. The receiver
reassembles and checks each frame independently. ESB suppresses link duplicates.
`ack_ok` includes the final control packet; receiver `packets` counts data
packets only. START/END control packets delimit an expected frame count.

## Reference code adaptations

1. `basop_util.h`: replace three `#ifdef WMOPS` guards with
   `#if defined(WMOPS) && WMOPS`, making upstream's `WMOPS=0` option compile.
2. At CMake configure time, move the 200 kB local initialization scratch array
   in `lc3plus.c` into the existing static scratch area. No algorithm change.
3. `src/ref_port.c` supplies the exact portable `Mpy_32_16_0_0` helper that upstream
   excludes on ARM without providing an ARM replacement.

Codec sources use `-O3`; the harness/kernel use Zephyr speed optimization.
`*-lto` enables Zephyr LTO with locally declared ISR tables. The `google-lto`
variant also uses `-ffast-math`, as upstream's Meson release build does.
RTT uses `tools/rtt-memory.py`, since JLinkRTTLogger 9.24a could not locate the
network-core RTT block. It reads the linked block address through the selected
J-Link core without halting or resetting either MCU; it updates only RTT's
consumer offset.
RTT initializes unconditionally at boot so flashing a different linker layout
cannot reuse stale descriptors retained in RAM. This was added before the final
Google LTO capture; earlier retained captures used the SDK's default strong-ID
check and had valid descriptors.

## Scope

This is a feasibility benchmark, not finished headphone firmware. It does not
implement USB Audio Class, LC3plus decoding on the nRF5340, I2S/DAC output,
playout buffering, clock-drift compensation, RF hopping, encryption, or listening
tests. It does not establish conformance of the embedded port. A fixed-channel
desk test does not establish range or interference robustness in an earcup.

## nRF54LM20 DK extension

`-Target tx54 -Codec google-lto -PcmBits 24` selects board
`nrf54lm20dk/nrf54lm20a/cpuapp`, probe `001051883190`. The user confirmed this
probe for audio testing after identifying its former use as a mouse receiver.
The second DK, `001051802784`, is assigned to mouse firmware and is never selected
by the audio build/flash targets.

The tx54 suite tests 256 and 320 kb/s stereo, each at 10, 5 and 2.5 ms, first
encoding only and then encoding with sequential acknowledged transmission.
`-SoakFrames` adds two 2.5 ms encoding-and-RF cases, one per bitrate. Google liblc3
does not provide a 1.25 ms frame mode. The codec arithmetic and PCM source are
unchanged from the nRF52840 Google benchmark. See the retained nRF54 report for
timings and the important distinction between CPU feasibility and transport
deadline compliance.

```powershell
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx54 -Codec google-lto -Frames 1000 -SoakFrames 40000 -PcmBits 24
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action flash -Target tx54 -Codec google-lto
```

The nRF54 flash uses erase-only-image-ranges and readback verification. The radio
port starts the PLL as in the SDK ESB sample (MLTPAN-39) and passes transmit power
as signed dBm; nRF54 radio register encodings are not interchangeable with this
API's dBm argument. The existing receiver remains `-Target rx -Codec etsi`.

Add `-AsyncOnly` to build the concurrent nRF54 test (mode 3). It runs 2.5 ms
frames at 256 and 320 kb/s, then two optional `-SoakFrames` cases. Run IDs are
101 through 104. A higher-priority worker owns acknowledged ESB transmission;
the main thread encodes against fixed absolute source times. A four-entry
packet queue plus one in-flight packet bounds memory and backlog. Submission
copies the packet so subsequent encoding cannot overwrite a DMA-owned buffer.
The worker blocks while RADIO/EasyDMA perform transmission and ACK handling,
allowing the encoder to run on the same Arm core. No FLPR firmware is involved.

Mode 3 `work_*` covers encoding and nonblocking enqueue, including preempting
radio work; it excludes waiting for that frame's ACK. `late` counts enqueue
completion after the next source release. It never shifts phase or skips input
to conceal overload. `ready_to_ack_*` measures from the scheduled availability
of a complete PCM frame to the ACK; `radio_late` counts completion past the
explicit 5 ms delivery budget. This budget is separate from the 2.5 ms source
period and is not total playback latency. `pending_peak` includes queued and
in-flight packets, and `radio_service_*` includes blocking ACK/retry time.
Encoding timings include interruptions by the radio worker/ISRs.
Drain completes before END so receiver counts include the final data packet.

```powershell
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx54 -Codec google-lto -Frames 1000 -SoakFrames 40000 -PcmBits 24 -AsyncOnly
```

Use RX stop text `RX_RESULT run=104 ` and TX stop text `SUITE_DONE` for this
suite. Start RX capture before flashing TX, then attach TX within its 10-second
startup delay; allow 300 seconds. See `results/2026-09-19-nrf54-async/REPORT.md`.

## WavPack-stream on nRF52840

`tools/fetch-wavpack-stream.py` restores upstream revision
`79ec9e178bd12c1445a09bec096db28707f1c55b` from a hash-checked archive.
`-Target tx -Codec wavpack-lto -PcmBits 24` selects the nRF52840 DK, serial
`000683088082`. This is WavPack-stream 0.2.0, not the standard WavPack file codec.
It tests default and fast lossy/hybrid modes without a correction stream,
at targets 256/320/384 kb/s and frame sizes 5/2.5/1.25 ms. Dynamic noise shaping
uses upstream defaults. Each fixed-size input block emits immediately, and the
encoder retains its context between calls. No codec arithmetic is modified.

Run IDs 201-236 cover 1000-frame local and concurrent-RF cases when built with
`-Frames 1000`. `-SoakFrames` optionally adds three fast-mode 2.5 ms radio cases.
The bitrate is a codec target; `encoded_payload_kbps` in summarized JSON uses
actual bytes over the represented audio duration. It includes all codec block
metadata, but excludes transport headers and retransmissions. `payload_max`
and `fragmented_frames` expose bursts hidden by the average bitrate.

Variable blocks require a 4096-byte frame buffer on both TX and RX. WavPack's
radio packet queue has 16 entries plus one in flight; the LC3plus queue remains
four entries. The worker still handles one RF packet at a time. For fragmented
frames, `radio_completed`, `radio_late` and `ready_to_ack_*` count/measure packets,
not complete codec frames. `data_packets` supplies the expected packet count.
Neither source overload nor queue errors are hidden by shifting source phase.

```powershell
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target rx -Codec etsi
powershell.exe -NoProfile -File .\tools\audio-bench.ps1 -Action build -Target tx -Codec wavpack-lto -Frames 1000 -PcmBits 24
```

Flash RX before starting capture. Stop RX on `RX_RESULT run=236 ` and TX on
`SUITE_DONE`; allow 300 seconds. Host validation uses Visual C++ through
`tools/build-wavpack-host.ps1`, then `tools/test-wavpack-stream.py`. It compares
continuous decoding against fresh independent blocks and streams with isolated
or burst losses, and checks that low-level 24-bit samples survive. This proves
recovery of intact blocks, not concealment or inaudibility of missing blocks.
See `results/2026-09-19-wavpack/REPORT.md` for hardware and recovery results.
