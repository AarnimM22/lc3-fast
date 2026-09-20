# nRF52840 LC3plus encoding and RF feasibility

Measured on the user's two DKs on 2026-09-19.

**Decision:** do not select the nRF52840 as the stereo LC3plus encoder for
320 kb/s at 2.5 ms or 1.25 ms based on these implementations. Encoding alone
exceeded the real-time budget. The radio carried 320 kb/s payloads, but the
longer tests also exposed occasional transport deadline misses.

This is a measurement of the tested software on the hardware, not a proof that
every possible commercial or hand-optimized encoder must fail.

## Test conditions

- TX nRF52840 DK `000683088082`, 64 MHz, instruction cache enabled.
- RX nRF5340 DK `001050038165`, network-core ESB receiver with frame CRC checks.
- Stereo 48 kHz, signed 16-bit PCM, **320 kb/s total across both channels**.
- Generated tones, noise, silence and transient mixtures; 200 frames per codec
  case. PCM generation is included in encoder timings.
- Official ETSI TS 103 634 v1.7.1 attachment: fixed-point software 1.9.2,
  revision `78d6b913cb5`. HR, lossless and FEC modes disabled.
- Google liblc3 revision `8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07` as an
  additional optimized implementation comparison. It lacks 1.25 ms support.
- NCS 3.2.1, GCC 12.2.0, codec `-O3`. Both ordinary and LTO builds tested;
  Google LTO also uses upstream's `-ffast-math` release option.
- ESB 2 Mbit/s, fixed 2440 MHz, 0 dBm, ACKs, three retries, 600 us retry delay.
  Frames have sequence, length and CRC32, with fragmentation above 224 bytes.

## Encoder results

Best tested build for each implementation: LTO, measured without RF traffic.
Times are per **stereo frame**, in milliseconds. Average / maximum over 200
frames; the frame duration is also the processing deadline.

| Audio frame / deadline | ETSI reference avg / max | Google comparison avg / max |
|---:|---:|---:|
| 10 ms | 49.223 / 61.387 | 10.479 / 12.179 |
| 5 ms | 28.415 / 34.203 | 5.606 / 6.321 |
| 2.5 ms | **18.240 / 21.304** | **3.597 / 3.992** |
| 1.25 ms | **13.487 / 16.347** | Unsupported |

At 2.5 ms the faster implementation consumes 144% of one CPU's available time
on average and 160% at the measured maximum, before USB input and transport.
Overlapping radio work with encoding cannot resolve an encoder that already
needs more than the entire frame interval. The ETSI reference is much further
from the target: about 7.30 times the 2.5 ms budget and 10.79 times the 1.25 ms
budget on average.

LTO made a material difference, so the conclusion is not based solely on an
unoptimized build. Without LTO, average 2.5 ms encode times were 26.244 ms for
ETSI and 4.066 ms for Google; ETSI's 1.25 ms average was 19.057 ms.

## Encoding and transmitting together

These runs encode, calculate CRC, send all fragments, and await ACKs
sequentially. Work time includes retransmissions and scheduler overhead.

| Implementation, frame | Work avg / max (ms) | Late frames / tested | Additional source intervals skipped | RX verified |
|---|---:|---:|---:|---:|
| ETSI LTO, 2.5 ms | 19.262 / 23.346 | 200 / 200 | 1,341 | 200 / 200 |
| ETSI LTO, 1.25 ms | 14.265 / 18.097 | 200 / 200 | 2,083 | 200 / 200 |
| Google LTO, 2.5 ms | 4.665 / 5.890 | 200 / 200 | 173 | 200 / 200 |

Every combined codec case at 320 kb/s missed every frame deadline, including
the longer 5 and 10 ms modes. The 160 and 240 kb/s checks at 5 ms also missed
every deadline with both implementations. Reducing bitrate alone did not
solve the CPU limit in these checks.

The receiver received all transmitted frames in these runs with no CRC errors
or queue overflows. That does **not** mean continuous audio was sustained:
frames were deliberately transmitted late to expose both CPU and RF behavior.

## Longer transport-only tests

The following use synthetic payloads matching the encoded sizes and cadence,
with no codec execution. There are 100 payload bytes per 2.5 ms frame and 50
per 1.25 ms frame at 320 kb/s. Each case sends 30,000 frames.

| Frame interval | RX verified | Late frames | Skipped intervals | Work avg / max | Retries | TX elapsed |
|---:|---:|---:|---:|---:|---:|---:|
| 2.5 ms | 30,000 / 30,000 | 8 (0.027%) | 0 | 1.043 / 3.327 ms | 154 | 75.000061 s |
| 1.25 ms | 30,000 / 30,000 | 160 (0.533%) | 2 | 0.826 / 2.716 ms | 83 | 37.502563 s |

There were zero CRC errors, receiver queue overflows, exhausted-ACK failures,
or send errors in either run. The two skipped intervals at 1.25 ms are source
cadence slips, not missing transmitted sequence numbers. The effective payload
rate was approximately 320 kb/s in both cases, but neither case passed a strict
zero-late-frame criterion. Retries and recovery to the absolute schedule can
affect more than one consecutive deadline.

These results distinguish data integrity from timeliness. Buffering can absorb
some late deliveries at the cost of latency. A fixed-channel desk test cannot
establish reliable operation around interference, at range, or in an earcup.

## Implications for the design

- The tested nRF52840 encoder configurations do not meet the requested target.
  More compute or a substantially faster encoder is required.
- Keeping the nRF52840 for transport is plausible, but the protocol still needs
  a defined playout budget and interference testing. The current ACK-per-frame
  test is not a finished low-latency audio transport.
- Encoding in host software is another architecture, but it requires host
  support; this test did not implement that path or a plug-and-play USB dongle.
- If considering the nRF5340 application core or another processor for encoding,
  benchmark the actual encoder there before selecting it. Its encoding or
  decoding capacity was not measured in this experiment.

The benchmark does not implement USB Audio Class, LC3plus decoding, I2S/CS43131
output, playout buffering, clock synchronization, hopping, encryption or audio
quality tests. CRC checks establish payload integrity, not codec conformance.
Codec frame duration is not total capture-to-playback latency.

## Reproduction and evidence

[Firmware and commands](../../README.md) describe the port adaptations, timing
method, build/flash steps and exact probe selection. Commands use the machine's
existing execution policy. The final TX image is `google-lto` with the 30,000
frame transport cases; RX remains the network-core receiver. Both finish their
tests and remain idle/listening. Resetting TX repeats its suite.

Matched raw captures and machine-readable summaries:

- [ETSI ordinary TX](etsi-tx.log), [RX](etsi-rx.log), [JSON](etsi.json).
- [ETSI LTO TX](etsi-lto-tx.log), [RX](etsi-lto-rx.log), [JSON](etsi-lto.json).
- [Google ordinary TX](google-tx.log), [RX](google-rx.log), [JSON](google.json).
- [Google LTO TX](google-lto-tx.log), [RX](google-lto-rx.log), [JSON](google-lto.json).
- [Image and source hashes](manifest.json) and [retained build configurations](configs/).

The raw logs retain pre-capture buffered lines where present. Only the matched
completed run IDs are used. The initial Google LTO attempt with stale RTT
descriptors is excluded; the final image initializes RTT on every boot.

Primary source packages: [official ETSI attachment](https://www.etsi.org/deliver/etsi_ts/103600_103699/103634/01.07.01_60/ts_103634v010701p0.zip)
and [pinned Google source](https://github.com/google/liblc3/tree/8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07).
