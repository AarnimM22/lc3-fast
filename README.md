# lc3-fast

`lc3-fast` is an experimental, deliberately simplified LC3plus-derived audio
codec for small Nordic microcontrollers. Its main target is real-time stereo
audio on an nRF52840: 48 kHz, 24-bit PCM enters over USB, the nRF52840 encodes
and transmits it, and an nRF5340 receives, decodes, and drives I2S.

The codec starts from [Google's `liblc3`](https://github.com/google/liblc3), but
it is **not LC3 or LC3plus compatible**. It keeps the useful core of the codec
while removing or approximating expensive analysis stages. This spends more
radio bandwidth to save CPU time-a good trade for a dedicated short-range
wireless audio link, but the opposite of what a general-purpose codec normally
tries to do.

The current recommended operating point is **384 kb/s total stereo**. It gives
some timing headroom below the highest one-frame-buffer rate tested, while
listening quality is close to the approximately 400 kb/s range where the codec
has been indistinguishable from the original PCM in the author's tests.

> [!IMPORTANT]
> `lc3-fast` uses a custom bitstream and requires the matching decoder from this
> repository. It is not a drop-in replacement for an LC3/LC3plus encoder and
> will not interoperate with standard LC3/LC3plus products.

## At a glance

| Property | Current configuration |
|---|---|
| Input | Stereo, 48 kHz, signed 24-bit PCM |
| Frame size | 2.5 ms; 120 samples per channel |
| Transform bandwidth | 100 MDCT coefficients per channel; about 20 kHz audio bandwidth |
| Recommended bitrate | 384 kb/s total stereo |
| Tested bitrate range | 140.8–400 kb/s for listening; up to 400 kb/s on the embedded streaming path |
| Encoder | nRF52840, 64 MHz Cortex-M4F |
| Receiver/decoder | nRF5340 network core + application core |
| Transport used here | Nordic ESB at 2 Mbit/s, acknowledged packets |
| Output | 48 kHz I2S, 24 significant bits in 32-bit slots |
| Compatibility | Custom research format; matching decoder required |

Bitrates throughout this repository are for **both channels together**, and
include the codec's per-channel headers. Radio framing, acknowledgements, and
retransmissions are additional.

## Why this exists instead of using LC3plus directly

LC3plus is designed to preserve quality at relatively low bitrates while
supporting a broad set of operating modes. Finding the best way to represent
each short piece of audio takes computation. On a 64 MHz nRF52840, that work
competes with USB input, packet preparation, radio interrupts, and the strict
cadence of a low-latency audio stream.

In this project's measurements, the mostly stock Google encoder-already with
the long-term postfilter disabled-averaged about **2.62 ms** for a 2.5 ms stereo
frame at 320 kb/s. That is already slower than the rate at which frames arrive,
before allowing for the rest of the transmitter.

`lc3-fast` is based on a different assumption: this link can afford roughly
384–400 kb/s, so the encoder does not need to work as hard to save every bit.
It uses cheaper decisions, emits a less compact custom representation, and
recovers quality by assigning more bits to the audio. The result is much more
practical on the nRF52840, at the cost of compression efficiency, flexibility,
and standards compatibility.

This project is therefore aimed at a fixed embedded link, not at replacing
LC3plus in Bluetooth, files, phones, or general media applications.

## How the codec works

At a high level, each 2.5 ms PCM frame follows the familiar transform-codec
pipeline:

1. An MDCT converts the samples into frequency coefficients.
2. A coarse spectral envelope estimates which frequency regions need more or
   less precision.
3. The coefficients are quantized and entropy-coded within a bounded byte
   budget.
4. Two independently encoded channel packets are joined into one stereo radio
   payload.
5. The receiver reverses the process and overlap-adds successive transforms to
   reconstruct PCM.

The MDCT, spectral quantizer, entropy coding, synthesis transform, overlap, and
the underlying packet-loss concealment machinery are still derived from
Google's implementation. This is not raw PCM with a lightweight wrapper. Most
of the savings come from simplifying the analysis used to choose encoding
parameters and specializing the implementation for one fixed mode.

## Major codec shortcuts

These changes affect the encoded representation or its quality. Together they
are why the result is a custom codec rather than an optimized LC3plus encoder.

### One fixed operating mode

The custom API accepts only 48 kHz, 2.5 ms frames in the normal-bandwidth mode.
The system is built around stereo 24-bit PCM, with each channel encoded
independently. Supporting arbitrary sample rates, frame durations, high-
resolution mode, and the rest of LC3plus's configuration space is intentionally
out of scope.

Specializing for one mode makes loops, tables, buffer bounds, and packet sizes
known in advance. It also removes the flexibility expected from a standard
codec library.

### Coarse spectral noise shaping

Stock LC3plus derives an energy envelope and performs a relatively expensive
split vector-quantization/PVQ search. That stage tells the quantizer how to
distribute error across the spectrum.

`lc3-fast` instead reduces 44 energy bands directly to 16 values, applies a
fixed frequency tilt and energy floor, removes the mean, and scalar-quantizes
each value to one of 16 levels. The decoder reconstructs the same envelope from
16 four-bit indices. Precomputed integer interpolation weights and a gain lookup
table replace additional runtime passes and exponentiation.

On the original 320 kb/s benchmark, stereo SNS processing fell from roughly
**918 us to 195 us**. The tradeoff is a much coarser envelope and **64 bits per
channel** of side information instead of the stock 38 bits. It is faster, but
less bit-efficient and less precise.

### Approximate gain selection and bounded variable-rate packing

The original encoder searches for a quantizer gain and refines it to make the
best use of a fixed packet budget. `lc3-fast` estimates the gain from groups of
four spectral coefficients, uses an algebraic initial estimate, and applies two
fixed slope corrections instead of the original eight-step search.

Packet size is allowed to vary from frame to frame. A small byte-credit
reservoir carries unused budget forward; a channel may borrow at most eight
extra bytes and the reservoir is capped at two nominal frames. If a frame still
does not fit, one bounded coarsening pass runs before the inherited hard-budget
truncation. There is no unbounded encode/re-encode loop.

This shortcut saves CPU time, but it is also one of the clearest quality
tradeoffs. Follow-up transient testing found that restoring the original
constant-size gain/budget/refinement policy improved waveform accuracy much
more than merely restoring the initial search. In other words, the faster rate
control-not only its first estimate-is a meaningful source of the remaining
artifacts.

### TNS disabled

Temporal Noise Shaping helps keep quantization noise from spreading around
sharp attacks. It is useful for transient-heavy material, but its analysis cost
about **170 us per stereo frame** in the measured 320 kb/s configuration.
`lc3-fast` disables it. This is an intentional speed-versus-transient-quality
tradeoff.

### LTPF disabled

The Long-Term Postfilter analysis and side information are disabled. LTPF is
especially useful for periodic and speech-like signals at lower bitrates. At
the high music-oriented rates targeted here, removing it saves work and
simplifies the path, while giving up some coding efficiency.

### Custom framing

Each channel is stored as:

```text
[format/SNS byte] [core length byte] [core payload...]
```

The core payload is restricted to 20–100 bytes. A complete stereo frame is at
most 204 bytes and fits in the project's radio payload without codec
fragmentation. Gain, envelope, and channel lengths are independently signaled
per frame, so intact frames do not depend on an indefinitely chained entropy
state.

The matching decoder retains the inherited concealment path for missing frames,
and automated tests cover isolated losses and loss bursts. Those tests show
recovery of the decoder state; they do not claim that concealment is inaudible.

## Bitstream-preserving implementation optimizations

After selecting the custom algorithm, several additional changes reduced CPU
and transport overhead without changing its codec output:

- **Direct SNS energy aggregation:** the fixed 44-to-16 mapping is evaluated
  directly while preserving the previous arithmetic grouping.
- **Spectral-cost reuse:** when quantization and entropy context are unchanged
  and the provisional frame already fits, the first coefficient-cost walk is
  reused. Capped or requantized frames keep the original exact fallback.
- **Specialized 60-point FFT:** the MDCT's FFT uses the known 5/3/2/2 radix
  sequence directly with the existing coefficient tables. This reduced the
  measured MDCT average from roughly 700 us to 520 us in the USB build.
- **Transform loop unrolling, `-O3`, LTO, and fast math:** the fixed transform
  is compiled for speed on the Cortex-M4F.
- **Owned radio packet pool:** the encoder writes directly into a packet that
  remains owned through transmission, avoiding an extra payload copy and unsafe
  reuse while EasyDMA is active.
- **Pointer queue and table-driven CRC32:** packet handoff is small and bounded,
  and CRC calculation uses a 256-entry table.
- **Concurrent encoding and radio:** a higher-priority worker handles the
  acknowledged ESB transaction while the main path prepares subsequent audio.
- **Lean production-style measurement build:** expensive whole-stream PCM
  hashing and per-stage profiling can be removed while sequence numbers, packet
  CRCs, timing, loss accounting, and a cheap encoded-stream fingerprint remain.

Native validation compared **236,432 channel packets byte for byte** between
the optimized and pre-optimization custom encoders across multiple signal
families, configurations, and rates. The packet output matched. An experiment
that moved hot functions into RAM was slower and consumed substantially more
RAM, so it is deliberately not part of the selected configuration.

## Measured embedded performance

The current end-to-end test path is:

```text
Windows USB Audio (48 kHz / 24-bit)
        -> nRF52840 encode + ESB transmit
        -> nRF5340 network-core receive
        -> nRF5340 application-core decode
        -> clock-corrected I2S / EasyDMA
```

At 384 kb/s, a 250-second run delivered, decoded, queued, and DMA-released
**100,000/100,000 frames** with no missing audio or I2S underruns. A separate
50-second demanding excerpt also completed without loss while sustaining about
381 kb/s of actual codec payload. Receiver clock recovery adjusts the nRF5340
audio PLL to follow the USB source without resampling, duplicating, or dropping
samples.

The nRF52840 bitrate sweep found:

- **396 kb/s** was the highest tested rate whose USB input queue stayed at a
  one-frame peak, including a 150-second / 60,000-frame soak.
- **400 kb/s** also streamed without dropped or corrupt frames, but the longer
  run used more buffering and reached a higher observed delivery latency.
- **384 kb/s** is the recommended default because it leaves modest headroom
  below that boundary with little subjective quality loss relative to 400 kb/s.

"Stable" here means that every offered frame made it through the tested
buffered pipeline without USB overflow, codec error, terminal ACK failure,
receiver loss, decoder error, or I2S underrun. It does **not** mean every encode
finishes inside 2.5 ms, nor is it a worst-case RF or latency guarantee. Some
individual frames take longer than one frame period and are absorbed by the
bounded queues. Results are from a fixed-channel desk setup, not controlled
Wi-Fi/Bluetooth interference or a range test.

The variable-rate encoder may produce less than its configured target on easy
or silent material. Real music in the listening set uses nearly the full
budget; synthetic sources containing silence do not. Always distinguish the
configured target from measured codec payload and total radio airtime.

## Listening results

The project's subjective findings are straightforward:

| Total stereo bitrate | Observed quality |
|---:|---|
| Below about 200 kb/s | Not usable for the intended high-quality music link |
| 256 kb/s | Becomes usable, but artifacts can be found with careful listening |
| 320 kb/s | Much cleaner, although transient differences can still be detected on demanding excerpts |
| Near 400 kb/s | No reliable audible difference from the original PCM in the author's listening/ABX work |

One focused ABX session on the “Bee Moved” excerpt scored 16/16 at 256 kb/s,
13/16 at 320 kb/s, and 10/16 at 400 kb/s against the reference. The 400 kb/s
result is consistent with chance rather than reliable discrimination. The
detectable lower-rate differences were concentrated around rapid transients,
which agrees with the decision to disable TNS and simplify rate control.

These are practical engineering observations from one listener and a small
music set, not a universal transparency threshold. Hearing, equipment, content,
and test method all matter. Waveform SNR figures in the retained reports are
useful for debugging but are not perceptual quality scores. Similarly, 24-bit
input and output containers do not make this lossy codec “24-bit accurate.”

The reproducible listening set and methodology are documented in
[`firmware/listening/lc3-fast-2026-09-20`](firmware/listening/lc3-fast-2026-09-20/README.md),
with the transient follow-up in
[`firmware/listening/lc3-fast-transient-diagnosis`](firmware/listening/lc3-fast-transient-diagnosis/README.md).

## Repository guide

- [`firmware/benchmark/src`](firmware/benchmark/src) - codec wrapper, custom SNS
  and rate-control code, USB input, radio transport, and benchmark application.
- [`firmware/benchmark/app_core`](firmware/benchmark/app_core) - nRF5340
  application-core decoder, I2S playback, buffering, and clock recovery.
- [`tools/prepare-lc3-custom.py`](tools/prepare-lc3-custom.py) - reproducibly
  generates the custom codec units from the pinned Google source. It verifies
  exact source matches instead of modifying the downloaded tree in place.
- [`tools/audio-bench.ps1`](tools/audio-bench.ps1) - builds, flashes, and captures
  the Nordic test targets.
- [`tools/test-lc3-custom.py`](tools/test-lc3-custom.py) - host signal, decode,
  loss, and recovery tests.
- [`firmware/results`](firmware/results) - benchmark archive. Human-readable
  reports and compact result summaries are versioned; bulky raw logs, firmware
  images, generated source snapshots, and local captures are ignored.

The most useful detailed reports are:

- [Custom codec design and initial measurements](firmware/results/2026-09-20-lc3-custom/REPORT.md)
- [Bitstream-preserving USB/encoder optimizations](firmware/results/2026-09-20-usb-optimizations/REPORT.md)
- [320–400 kb/s transmitter sweep and 396 kb/s soak](firmware/results/2026-09-20-usb-rate-sweep-2/REPORT.md)
- [384 kb/s USB-to-I2S playback validation](firmware/results/2026-09-20-rx-appcore-384-100k/REPORT.md)
- [Receiver audio-clock recovery](firmware/results/2026-09-20-clock-servo-final/REPORT.md)

## Building and reproducing

The retained setup uses Windows, PowerShell, nRF Connect SDK 3.2.1, and its GCC
12.2 toolchain. The helper currently contains local SDK paths and development-
kit probe IDs, so a different setup will need those values adjusted.

Third-party codec sources are intentionally not committed. Fetch the pinned
Google revision (and the reference codec used by comparison tests) first:

```powershell
python .\tools\fetch-codecs.py --google
```

Build the optimized USB transmitter and matching nRF5340 receiver:

```powershell
& .\tools\audio-bench.ps1 -Action build -Target tx -Codec google-custom-lto `
    -PcmBits 24 -UsbAudio -Optimizations 31
& .\tools\audio-bench.ps1 -Action build -Target rx -Codec etsi
```

The receiver target's historical `etsi` build label is misleading: its
application-core child image builds the matching custom decoder. Detailed USB,
flash, capture, and playback commands are in
[`firmware/USB_PCM.md`](firmware/USB_PCM.md) and
[`firmware/benchmark/app_core/README.md`](firmware/benchmark/app_core/README.md).

For codec-only host validation:

```powershell
& .\tools\build-lc3-custom-host.ps1 -Optimizations 31
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' `
    .\tools\test-lc3-custom.py --output .bench\custom-host-probes
```

## Current scope and limitations

This is research firmware for a particular wireless DAC/headphone architecture,
not a finished reusable codec library or shipping radio protocol.

- The wire format is versioned only informally and may change.
- Codec controls are global and the implementation is not reentrant.
- Only the fixed 48 kHz / 2.5 ms mode is supported by the custom wrapper.
- The present link has no claim of LC3/LC3plus conformance or interoperability.
- RF testing is fixed-channel and does not establish range or coexistence
  robustness.
- The DK tests exercise I2S/EasyDMA but not the final external DAC, analog path,
  or complete product hardware.
- Packet-loss recovery is tested functionally, not proven inaudible.
- The current bitrate and buffer conclusions are measurements of the supplied
  sources and hardware, not universal worst-case bounds.

Within that scope, the central result is already demonstrated: a 64 MHz
nRF52840 can accept real 48 kHz/24-bit stereo USB audio, encode it with a
transform codec, and stream it reliably to an nRF5340 that decodes and clocks it
out over I2S-at a quality level that becomes effectively transparent near
400 kb/s.
