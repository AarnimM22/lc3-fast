# Custom SNS and variable-rate LC3-derived codec on nRF52840

Implemented, flashed and tested on 2026-09-20. The fastest shaped configuration is now a plausible **CPU and transport feasibility candidate at a 320 kb/s average-rate ceiling**, with a conditional path to 400 kb/s using buffering. These experiments do not establish perceptual transparency, a guaranteed production deadline, or interference robustness.

The selected configuration retains the MDCT and spectral noise shaping, replaces SNS vector quantization with a coarse scalar envelope, uses approximate gain selection and bounded variable payloads, and disables LTPF and TNS. It is a **custom format requiring the matching decoder**, not a standards-compatible LC3plus bitstream.

## Hardware and measurement scope

- Encoder/transmitter: nRF52840 DK, serial `000683088082`, 64 MHz Cortex-M4F.
- Packet receiver: nRF5340 DK network core, serial `001050038165`.
- NCS 3.2.1, toolchain `66cdf9b75e`, GCC 12.2; codec `-O3`, LTO and fast math.
- Google liblc3 revision [`8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07`](https://github.com/google/liblc3/tree/8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07).
- Stereo 48 kHz, signed 24-bit PCM, 120 input samples/channel per 2.5 ms frame. This is the normal-bandwidth mode: 100 coded MDCT coefficients/channel, approximately 20 kHz bandwidth. It is not LC3plus HR.
- ESB 2 Mbit/s, fixed channel 40 / 2440 MHz, ACK, three retries and 600 us retry delay. Encoding and radio run concurrently on the same M4F; RADIO/EasyDMA run while the worker blocks for completion.
- Synthetic PCM alternates tones, noise, silence and a transient mixture every 25 frames. One quarter is silence. Both channels contain nonzero low-order PCM bits outside silence.
- The nRF5340 checks sequence, length and CRC. Decoding was tested on the host, including samples encoded on the DK. There is no on-device receiver decoding, USB Audio input, I2S/DAC output, playout buffering or clock-drift correction in this experiment.

`api_*` times both channel encoder calls. `enc_*` also includes source generation. `work_*` includes generation, encoding, CRC and queue submission, with radio preemption, but not waiting for that packet's ACK. DWT timings are cross-checked against uptime and rearmed each frame; uptime has roughly 30.5 us resolution. Stage times are stereo averages; gain time is contained in spectral-analysis time, so it must not be added twice.

Two deadlines are reported separately: `late` means producer completion after the next absolute 2.5 ms release, while `radio_late` means ACK completion more than 5 ms after the scheduled availability of a complete PCM frame. This existing 5 ms transport budget is not total audio latency. Late frames remain visible; source phase is never moved to hide overload.

## Algorithm changes

The proposed plan correctly identified SNS as a major cost, but Google's SNS is not a 16th-order Levinson-Durbin LPC analysis. It estimates scale factors from band energies and performs split vector quantization plus a DCT/PVQ search. The 48 kHz/2.5 ms energy stage has 44 bands, subsequently reduced to 16 SNS scale factors. See the pinned [SNS implementation](https://github.com/google/liblc3/blob/8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07/src/sns.c).

Four SNS modes were built into the experiment:

| Mode | Algorithm | SNS time at 320 kb/s, local sweep |
|---|---|---:|
| 0 | Original Google envelope estimation and VQ/PVQ | 918 us |
| 1 | Coarse 16-value envelope and four-bit scalar indices | 195 us |
| 2 | Unity envelope, no SNS analysis or side data | 1 us |
| 3 | Original envelope estimator, replacing VQ/PVQ with scalar indices | 295 us |

Mode 1 reduces energy to 16 groups, applies a precomputed frequency tilt and energy floor, takes log energies, removes the mean and applies 0.85 shaping strength. Each value is quantized to one of 16 steps spaced 0.75 octaves apart. The receiver reconstructs the same envelope and applies its inverse. Mode 3 preserves the upstream smoothing and attack handling before scalar quantization; it costs approximately 100 us more per stereo frame than mode 1.

Interpolation from 16 scale factors through the upstream 64 positions to 44 energy bands is collapsed into integer weights. All resulting gains lie on a 3/64-octave grid, so a lookup table replaces runtime exponentiation and temporary envelope passes. The host probes produced identical waveform-error and bitrate results before and after this final integer-weight change. These are algorithm changes plus implementation optimizations; no new assembly kernel was needed.

The custom envelope costs 64 bits/channel instead of the original 38: **20.8 kb/s extra stereo side information** at 2.5 ms. Per-channel format headers add another 12.8 kb/s. Both are included in reported custom payload rates. Unity SNS saves side information but changes noise allocation substantially.

Three rate modes were compared:

1. Original constant-size gain search, refinement and packing.
2. Original initial gain estimate followed by the custom variable-size packing path.
3. Approximate gain from grouped spectral energies, an algebraic initial estimate and two fixed slope corrections, followed by variable-size packing.

The approximation replaces the eight-step initial gain search. It is not a claim of exact closed-form psychoacoustic allocation. At 320 kb/s, the original gain stage took 159 us, versus 91 us for the approximation. With coarse SNS and TNS enabled, total spectral-analysis time fell from 443 to 332 us. Keeping the original initial search in VBR mode took 399 us, which separates part of the search saving from the refinement changes.

Payload selection estimates entropy demand, reserves residual bits and a termination margin, then selects the actual byte length before writing. This is necessary because LC3 packing uses both ends of the buffer; the result cannot be obtained by chopping bytes off a completed fixed-length frame. A byte-credit reservoir constrains cumulative average rate. Each channel can borrow at most eight extra bytes for a frame, within a two-frame credit reservoir. When demand exceeds the cap, there is one bounded coarsening pass followed by inherited hard-budget spectral truncation. There is no unbounded encode/re-encode loop.

The fallback is material: it ran on 12,931 of 40,000 channel frames in the 320 kb/s soak and 15,013 at 400 kb/s. Its cost is included in timings. Each soak also recorded 9,600 envelope index clips out of 640,000 scale-factor values; the coarse range is a quality tradeoff, not an exact envelope representation.

MDCT arithmetic, window and tables are retained. The final build enables compiler loop unrolling for `mdct.c`; local MDCT time was approximately 544 us versus about 555 us in the preceding image. Other changes and code layout differ between these images, so this is not an isolated proof of the loop-unrolling benefit. Disabling TNS saved approximately 170 us in the local 320 kb/s comparison. LTPF is disabled throughout the custom matrix.

## Hardware results

The final 96-case sweep compares 12 combinations at 256, 320, 400 and 480 kb/s, with 500 frames each in local and concurrent-radio modes. At 320 kb/s, local encoder API averages were:

| Configuration, LTPF disabled throughout | Encoder API average / maximum |
|---|---:|
| Original SNS, original CBR, TNS on | 2.616 / 3.026 ms |
| Coarse SNS, original CBR, TNS on | 1.880 / 2.264 ms |
| Original envelope + scalar SNS, original CBR, TNS on | 1.980 / 2.408 ms |
| Unity SNS, original CBR, TNS on | 1.652 / 2.059 ms |
| Coarse SNS, approximate VBR, TNS on | 1.792 / 2.293 ms |
| Coarse SNS, approximate VBR, TNS off | 1.622 / 1.945 ms |

The best shaped configuration was then built as a dedicated endurance image and run for **20,000 stereo frames / 50 seconds at each target**:

| Dedicated soak, coarse SNS + approximate VBR, TNS/LTPF off | 320 kb/s target | 400 kb/s target |
|---|---:|---:|
| Actual codec payload rate, including custom headers | 282.206 kb/s | 344.156 kb/s |
| Encoder API average / maximum | 1.661 / 1.988 ms | 1.761 / 2.104 ms |
| Producer work average / maximum | 1.974 / 2.380 ms | 2.087 / 2.503 ms |
| Producer completions after 2.5 ms deadline | 0 / 20,000 | 1,751 / 20,000 |
| PCM-ready to ACK average / maximum | 2.888 / 3.394 ms | 3.073 / 3.644 ms |
| ACK completions after 5 ms budget | 0 / 20,000 | 0 / 20,000 |
| Maximum lateness starting a scheduled source frame | 129 us | 182 us |
| Delivered frames, CRC correct | 20,000 / 20,000 | 20,000 / 20,000 |
| Retries / failed ACKs / queue errors | 0 / 0 / 0 | 0 / 0 / 0 |
| Maximum stereo codec packet | 116 bytes | 141 bytes |
| Maximum queued + in-flight packets | 1 | 1 |

Actual VBR rates are below the target because this timing source includes 25% silence. Continuous non-silent host probes often approached the rate ceiling; these soaks must not be described as continuous 320/400 kb/s encoded payload. The 400 kb/s configuration had bounded producer jitter and did not accumulate backlog over these 50 seconds. It would still require input/playout buffering in a product.

The earlier full-suite image is a useful limitation on the result: the same algorithm at 320 kb/s had 24 producer misses in 500 frames, while all ACKs arrived within 3.486 ms. At 400 kb/s it had 135 producer misses, with maximum ACK latency 4.335 ms. Build layout, scheduling and run conditions differ; the dedicated soak's zero producer misses at 320 kb/s are not a guaranteed worst-case bound. No specific cause for the difference is established.

Keeping TNS with coarse SNS and approximate VBR did not meet the 5 ms budget consistently in the final sweep: 9/500 late ACKs at 320 kb/s and 316/500 at 400 kb/s. Unity SNS with approximate VBR and TNS retained had 0/500 late ACKs at 320 kb/s and 7/500 at 400 kb/s, but has different quality risks. The original envelope with scalar quantization was slower; at 400 kb/s, approximate VBR with TNS disabled still had 144/500 late ACKs.

At 480 kb/s, even the selected coarse/VBR/TNS-off configuration missed the 5 ms budget on 298/500 frames and reached 7.479 ms ACK latency. Increasing bitrate indefinitely is therefore not a free way to recover quality on this MCU.

Across all three optimization sweeps and the dedicated soak, **168,000 stereo frames were encoded and 104,000 were transmitted and CRC-verified**, with no codec errors, send errors, failed ACKs, missing frames, CRC failures or receiver overflow. There were 266 retries across the sweeps and zero during the soak. Many configurations nevertheless missed their timing budgets. One final-sweep case, ID 772, recorded one DWT/uptime cross-check error and is excluded from CPU timing conclusions. Both dedicated-soak cases had zero timing errors.

The soak TX log was read from the retained RTT buffer after execution; RX was captured throughout. Per-frame DWT rearming and zero timing cross-check errors support those timing measurements. The complete final sweep had live TX/RX capture. Raw logs are retained for both.

## Decode, loss recovery and quality probes

The matching custom host decoder was exercised on **416 signal/configuration/rate probes**: eight synthetic signals, four rates, 12 custom combinations plus the unmodified no-LTPF reference. All intact frames decoded successfully with finite output. The original-SNS custom control matched 15,360 unmodified channel payloads byte for byte at the same core payload budget, excluding custom headers.

There were **1,536 loss scenarios** covering isolated erasures, bursts of four or twenty frames, and periodic 5% erasures during the middle of the stream. The decoder returned concealment status exactly on requested erasures and successful decode on subsequent intact packets. Every final 20-frame window converged to its corresponding no-loss output within 1e-6 full scale. These tests establish eventual recovery on the probes, not inaudible concealment, immediate state recovery or a guaranteed recovery time.

Additionally, **392 stereo packets encoded on the nRF52840** (384 from the final sweep and eight from the soak) were decoded on the host: 784 successful channel decodes. These sparse vectors validate framing and decoder acceptance, not continuous on-board playback or ARM/host bit identity.

MDCT synthesis overlap and the original Google PLC still have state. Gain, SNS values and channel lengths are signaled per frame, and entropy state starts anew, so loss does not create an indefinite bitstream dependency. Preserving the PLC code does not guarantee unchanged concealment quality, because the reconstructed history has changed. Receiver software must detect sequence gaps and call concealment; the current hardware receiver only counts/verifies frames.

The proposed fidelity table was a hypothesis, not an established property of this codec. Measured waveform SNR for the changing-harmonics probe was:

| Total target kb/s | Unmodified Google, LTPF off | Coarse SNS + approximate VBR, TNS off | Unity SNS + original CBR, TNS on |
|---|---:|---:|---:|
| 256 | 44.94 dB | 40.58 dB | 42.07 dB |
| 320 | 51.64 dB | 50.11 dB | 46.44 dB |
| 400 | 57.32 dB | 55.96 dB | 41.85 dB |
| 480 | 60.61 dB | 59.42 dB | 39.62 dB |

On this probe, raising the coarse configuration from 320 to 400 kb/s improved waveform SNR by 5.85 dB. Its 400 kb/s RMS reconstruction error was -69.48 dBFS, and at 480 kb/s it was -72.93 dBFS. Unity SNS was not reliably improved by additional bitrate in this probe. Thus a general -90/-96 dBFS reconstruction claim is not supported. These values are signal-dependent waveform errors, **not an audible noise-floor measurement, DAC THD+N, effective bit depth or a perceptual quality ranking**. Other signals produce different rankings; for example unity SNS measures well on the chirp.

A lossy perceptual codec can be transparent despite appreciable waveform error, and excellent error figures on a tone do not establish transparency on music. No ABX, music corpus, headphones or analog DAC measurements were performed. Likewise, 24-bit input/output support does not establish 24-bit reconstruction accuracy. The normal-mode 20 kHz bandwidth also limits the wideband-noise probe, independently of quantization.

## Format and implementation

Each mono channel packet is `[0xd0 | sns_mode, core_length, core_payload...]`. Core length is restricted to 20..100 bytes. Two mono packets concatenate into a stereo frame, for a hard maximum of 204 bytes, fitting the transport's 224-byte codec capacity without fragmentation. The format is research version 1 and fixed to normal 48 kHz/2.5 ms mode. Gain and TNS parameters use the inherited per-frame fields. SNS modes 1/3 transmit 16 four-bit values; mode 2 transmits none.

Key sources:

- `firmware/benchmark/src/custom_sns.inc`: coarse and original-envelope scalar paths, unity bypass and lookup-based shaping.
- `firmware/benchmark/src/custom_spec.inc`: approximate gain, variable length selection, byte reservoir and bounded cap fallback.
- `firmware/benchmark/src/lc3_custom.c` and `.h`: format wrapper, matching decoder and controls.
- `tools/prepare-lc3-custom.py`: reproducible source generation; third-party sources remain untouched.
- `tools/test-lc3-custom.py` and `tools/check-lc3-custom-vectors.py`: host signal/loss and ARM packet tests.

The experiment uses global codec controls and is not reentrant. Production integration would require per-instance configuration, transport validation, buffering, and the decoder running on the nRF5340 application core. The selected soak image uses 196,188 bytes of flash and 86,528 bytes of RAM; both encoder states together are 5,032 bytes. These totals include the benchmark/Zephyr/radio instrumentation, not a finished audio product.

## Reproduction and retained evidence

Run from the workspace root using the existing execution policy. Restore the pinned third-party source with `tools/fetch-codecs.py --google` if needed.

```powershell
# Complete 96-case matrix (last run ID 796).
& .\tools\audio-bench.ps1 -Action build -Target tx -Codec google-custom-lto -Frames 500 -PcmBits 24

# Selected two-case endurance image (run IDs 801 and 802).
& .\tools\audio-bench.ps1 -Action build -Target tx -Codec google-custom-lto -Frames 500 -PcmBits 24 -AsyncOnly -SoakFrames 20000
```

Use one build or the other, then start RX capture, flash TX and attach TX capture within the 10-second startup delay. Do not reset/flash a probe while its RTT capture owns it. The existing RX CRC-verifier image is `-Target rx -Codec etsi`.

```powershell
# RX terminal, before flashing TX; use run=796 for the matrix.
& .\tools\audio-bench.ps1 -Action rtt -Target rx -Codec etsi -Seconds 170 -StopOn 'RX_RESULT run=802'

# TX terminal.
& .\tools\audio-bench.ps1 -Action flash -Target tx -Codec google-custom-lto
& .\tools\audio-bench.ps1 -Action rtt -Target tx -Codec google-custom-lto -Seconds 150 -StopOn SUITE_DONE

# Native host decoder and probes; requires Visual C++ and NumPy.
& .\tools\build-lc3-lite-host.ps1
& .\tools\build-lc3-custom-host.ps1
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' .\tools\test-lc3-custom.py --output .bench\custom-host-probes-repeat
```

`initial-sweep`, `revised-sweep`, `final-sweep` and `soak` each retain the exact flashed TX ELF/HEX, configuration, source snapshot, generated patched units, TX/RX logs, parsed measurements and a SHA256 manifest. `final-sweep` and `soak` contain ARM packet validation. The final host validation is copied into both. The initial VBR path's capped-output feedback bug was corrected before the revised/final tests; its earlier data is retained as development evidence, not the selected implementation. `summary.json` totals the evidence and `sha256.json` covers this result directory.

The boards are left with the completed custom endurance image on the nRF52840 and the existing packet verifier on the nRF5340. No mouse firmware or KiCad files were changed.
