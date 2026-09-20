# Actual USB PCM streaming on nRF52840

Tested on 2026-09-20 using `firmware/benchmark/24_48k_PerfectTest.wav`.

**The earlier synthetic benchmark's 2.380 ms maximum did not establish enough
margin for the complete USB pipeline.** With the same coarse-SNS / approximate-VBR
codec, the optimized USB implementation transmitted all 20,000 frames at a
320 kb/s target, but 8,852 exceeded the 5 ms delivery budget. At a 400 kb/s target,
the input queue overflowed and discarded 578 of 20,000 input frames. This is a
failure of the original low-latency target, not a claim that all nRF52840 USB
implementations or codecs are infeasible.

## Test setup

- nRF52840 DK `000683088082`, 64 MHz Cortex-M4F, with debugger USB and native
  nRF USB connected. The native endpoint enumerates as **nRF52840 PCM Bench**.
- nRF5340 DK `001050038165`, existing network-core ESB packet/CRC verifier.
- Zephyr USB Audio Class 2, stereo 48 kHz, packed 24-bit PCM, USB SOF-synchronized
  clock. USB uses the normal Nordic controller driver and EasyDMA.
- Windows WASAPI **exclusive mode**, requesting the exact PCM format and
  selecting only the named benchmark endpoint. The player does not choose the
  default output or request shared-mixer sample conversion.
- The supplied WAV has 637,685 stereo sample frames, lasting 13.285104 s. The
  player loops it continuously to offer 20,000 codec frames / 50 s at each rate.
  It preserves the final five sample frames when wrapping the file.
- Source SHA256:
  `310240ee7b63d7c4f46d7f59bf9633c0b008c9c1085930cd6d20e83d6c565456`.
- Codec unchanged from the selected prior experiment: coarse 16-value scalar
  SNS, approximate gain/VBR, TNS and LTPF disabled, 2.5 ms, two mono packets per
  stereo frame. Normal-bandwidth mode, not HR or standards-compatible LC3plus.
- Concurrent ESB: 2 Mbit/s, fixed 2440 MHz, ACK and three allowed retries.
  There was no controlled interference stress test.
- NCS 3.2.1 / GCC 12.2, `-O3`, LTO and fast math for the codec. The final USB
  build uses full newlib instead of its size-optimized nano variant.

Each playback has one second of startup silence, an in-band test marker/header,
100 warm-up codec frames (250 ms), the measured WAV stream, and one second of
drain silence. Startup/warm-up/drain silence is excluded from the measured
codec frame count and payload bitrate. USB callback statistics include warm-up.
The WASAPI host buffer is 10 ms; USB transactions still arrive at 1 ms intervals.
This host buffer is excluded from the reported device latencies.

## Final 50-second results

Both rates use the same final firmware image, with the bitrate supplied by the
test header. DWT/uptime cross-check errors were zero in both cases.

| Metric | 320 kb/s ceiling, run 911 | 400 kb/s ceiling, run 912 |
|---|---:|---:|
| Offered PCM codec frames | 20,000 | 20,000 |
| Encoded and CRC-verified at receiver | **20,000** | **19,422** |
| Input frames dropped by full queue | **0** | **578** |
| Whole-stream PCM fingerprint matches source | **Yes** | **No: input frames were dropped** |
| Encoder calls, average / maximum | 1.874 / 2.626 ms | 1.974 / 2.757 ms |
| Producer work, average / maximum | 2.078 / 2.808 ms | 2.180 / 2.961 ms |
| Individual work durations above 2.5 ms | 4,317 | 9,866 |
| Submissions more than 2.5 ms after USB PCM ready | 10,295 | 11,191 |
| Maximum input-queue wait before processing | 6.622 ms | 42.908 ms |
| Maximum completed-frame input queue occupancy | 3 of 16 | 16 of 16 |
| USB PCM ready to ACK, average / maximum | 4.609 / **10.223 ms** | 24.273 / **48.798 ms** |
| ACKs later than 5 ms | **8,852 / 20,000** | **11,117 / 19,422** |
| Failed ACKs / radio send errors | 0 / 0 | 0 / 0 |
| ESB retries | 11 | 21 |
| Bad RF CRC / receiver overflow | 0 / 0 | 0 / 0 |
| USB zero-length / malformed packets | 0 / 0 | 0 / 0 |
| USB receive-buffer allocation failures | 0 | 0 |
| Maximum queued + in-flight radio packets | 1 | 2 |
| Actual encoded bitrate per encoded audio duration | 241.298 kb/s | 282.977 kb/s |
| Maximum codec payload size | 116 bytes | 141 bytes |

At 400 kb/s, the final 2 s input timeout occurs because the source has ended
after 20,000 offered frames, while the consumer has processed only 19,422.
The receiver independently reports exactly 578 missing frames. The 400 kb/s
actual payload rate over the **offered** 50 s is 274.799 kb/s; the larger
282.977 kb/s table value is normalized to the 19,422 encoded frames. Neither
should be presented as a successful continuous 400 kb/s stream.

The 320 kb/s run delivered every frame without input overflow, so it demonstrated
throughput feasibility for this repeated file with buffering. It did not meet
the 5 ms device delivery target, and a playback buffer would need to accommodate
the observed 10.223 ms maximum plus receiver decode/output and an appropriate
margin. This measurement is not a guaranteed bound on other material or RF
conditions. At 400 kb/s the 40 ms-capacity input queue filled; merely observing
an average work time below 2.5 ms is insufficient when demanding passages
persist long enough to exhaust it.

The codec target is an average-rate ceiling. The WAV's content and variable-rate
encoder produce lower actual rates. The measured portion contains exactly the
requested source waveform, including its own quiet/silent passages, without the
synthetic benchmark's deliberately inserted quarter-stream silence.

## What the timings include

`api_*` covers both channel encoder calls and interruptions while those calls
run. `work_*` starts after acquiring an input-frame pointer and covers the
whole-stream integrity fingerprint, encoding, radio CRC/queue submission and
input-buffer release, including intervening USB/radio work. It excludes blocking
for input and waiting for that frame's ACK.

The ready timestamp is taken at entry to the USB completion callback that
provides the codec frame's final PCM bytes. `producer_late` and `ready_to_ack_*`
include queue waiting after that timestamp. They do not include USB work before
the callback, host buffering, earlier PCM accumulation, receiver decoding or DAC
playout. They are **not PC-to-headphone latency**. USB normally completes codec
frames on alternating 2/3 ms packet intervals; the input queue absorbs this
packetization mismatch. Source timing is never deliberately slowed when the
encoder is busy.

The receive callback averaged 15 us/USB packet at 320 kb/s and 14 us at 400 kb/s.
These figures are not the complete USB stack cost and must not be added to
encoder timings when the same callback already preempted the encoder. The final
PCM fingerprint cost averaged 46/53 us per codec frame; radio submission and
buffer release averaged 150/145 us. The integrity check is test instrumentation
and its cost is conservatively included. Removing it might improve timing, but
that unmeasured change cannot be treated as a demonstrated pass.

The earlier 2.380 ms synthetic result and this USB result use different PCM
sources and integration builds. They therefore do not isolate a single fixed
"USB overhead" value. Input content, per-millisecond driver activity, queue
handling and instruction/cache layout can all affect observed wall time.

## Implementation and verification

The receive callback copies USB packet fragments into an owned PCM frame. A
queue passes pointers instead of copying 720-byte frames into and out of a
message queue. Eighteen slab buffers cover sixteen queued frames, one under
construction and one being encoded. The encoder consumes packed interleaved
24-bit samples directly through the library's existing `S24_3LE` input mode.
The consumer releases each buffer after encoding. Overflow and sequence gaps
are counted explicitly; there is no silent rate adaptation or audio concealment
to hide overloaded cases.

A host test compared all 5,314 complete codec frames in the supplied WAV at
both rates. **21,256 channel packets were byte-identical** between packed,
interleaved input and the prior planar 24-bit path. This is a host equivalence
check, not an ARM-versus-host bit-exact certification.

The final firmware fingerprints every measured PCM byte before encoding, using
a documented 64-bit word-wise FNV-derived accumulator. The expected fingerprint
for each 20,000-frame source is `7b4c85c4caed4b6d`, computed by the C++ player and
independently checked in Python. Run 911 matched exactly. Run 912 returned
`7c2b9a933f4c4f7c`, consistent with its missing input frames. This is a
non-cryptographic integrity check, alongside frame accounting and hardware USB
error handling. Radio payloads retain the existing CRC32.

The first implementations failed more severely. They are retained so their
measurements are not confused with the final image:

| Short 320 kb/s test | Received / offered frames | Input drops | Average callback | Average / max work |
|---|---:|---:|---:|---:|
| Initial byte-by-byte assembly, run 901 | 3,059 / 4,000 | 941 | 137 us | 3.175 / 3.875 ms |
| Block assembly, run 902 | 3,327 / 4,000 | 673 | 59 us | 2.918 / 3.663 ms |
| Pointer ownership, packed input, full newlib, run 903 | 3,851 / 4,000 | 149 | 14 us | 2.386 / 2.960 ms |

Those prototypes used a full software PCM CRC32 in the producer. Its remaining
155 us average cost in run 903 motivated the cheaper whole-stream fingerprint.
The encoder's algorithm and TNS/SNS configuration were not changed to obtain the
final USB results. Run 901 had three timing cross-check errors, so its cycle
figures are diagnostic only; runs 902, 903, 911 and 912 had none. USB/radio/frame
failure counts remain retained for all runs.

RTT collection was also fixed to wait for the newline after a stop marker, so a
`printk` result split across reads cannot lose its final fields. Run 901's RX
line was completed using a second capture; its raw parts were concatenated in
order in the archive. Later captures include complete result lines directly.

The final image uses 228,968 bytes of flash and 98,184 bytes of RAM. It is left
running on the nRF52840, waiting for another marker-controlled test. The nRF5340
packet verifier remains unchanged. The benchmark does not decode audio on the
nRF5340, drive the DAC, implement clock-drift correction, or test acoustic
quality. No mouse firmware or KiCad data was changed.

## Reproduction and evidence

See [USB_PCM.md](../../USB_PCM.md) for build, flash, capture and exclusive-mode
playback commands. `-UsbAudio` selects the separate `.bench/usb-tx` image.

`initial`, `block-copy`, `owned-buffers` and `final` retain exact ELF/HEX images,
configuration, device tree, source snapshots, host executable, raw host/TX/RX
logs, parsed `hardware.json` and SHA256 manifests. `final` contains both 50-second
runs. The top-level `usb-packed-format-validation.log` preserves the input-format
equivalence result. `validation.json` records the final cross-checks, including
the intentional distinction between successful input integrity at 320 kb/s and
the measured failure at 400 kb/s.
