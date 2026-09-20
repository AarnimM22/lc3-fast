# LC3plus encoder simplification on nRF52840

Measured 2026-09-19. Seven encoder variants were built, flashed and tested at 48 kHz stereo, 24-bit input, 2.5 ms frames and 256/320 kb/s. Encoder-only simplifications retain the Google decoder and its loss-concealment path, but **none of the tested variants sustained the 2.5 ms source cadence with concurrent radio**. No perceptual-transparency claim is established.

The user's subsequent variable-rate proposal is valid: a custom ESB codec need not optimize every frame to exactly 80 or 100 stereo bytes. That approach is discussed below; the implemented variants in this report still have fixed frame lengths. No closed-form/VBR encoder was implemented or measured in this experiment.

## Implementation and hardware

- nRF52840 DK PCA10056, serial `000683088082`, Cortex-M4F at 64 MHz: encoder and ESB transmitter.
- nRF5340 DK PCA10095, serial `001050038165`: existing network-core packet reassembly and CRC receiver. No audio decoding or DAC output on that board.
- Google liblc3 revision `8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07`, LC3plus 2.5 ms extension, standard 48 kHz mode, not HR mode. Stereo comprises two independent mono encoders, 40/50 bytes per channel.
- NCS 3.2.1, GCC 12.2, `-O3`, `-ffast-math`, LTO, hard-float. The build includes stage profiling; reported timings include its small overhead.
- Existing deterministic tone/noise/silence/transient source, changing every 25 frames, with nonzero low input bits. 1000 frames per case, fresh encoder for each case.
- ESB 2 Mbit/s, fixed 2440 MHz, 0 dBm, ACK and up to three retries. Bounded queue and separate radio worker allow encoding to overlap the previous packet's transmission. Source release times remain fixed even during overload.
- Neither connected mouse dongle nor any nRF54 DK was flashed.

`tools/prepare-lc3-lite.py` creates modified copies of three upstream translation units, with exact-match checks. The upstream tree remains untouched. `google-lite-lto` is an explicitly experimental build selection. The stock `google-lto` algorithm remains unchanged.

Flags are independent:

| Flag | Change | What remains |
|---:|---|---|
| 1 | Call Google's existing `lc3_encoder_disable_ltpf()` API | Decoder still accepts ordinary pitch-absent frames |
| 2 | TNS analysis returns zero filter orders | Correct filter-count/weighting metadata and ordinary inactive-TNS syntax |
| 4 | Set the second gain adjustment to zero | Initial gain search, quantization, bit estimation and final bit-budget fitting |

The sweep tests flags 0, 1, 2, 3, 4, 5 and 7, locally and with overlapping radio, at both rates: 28 cases and 28000 encoded stereo frames. Flag 4 is **not** a removal of the initial gain search.

## Measured performance

Local API times below include both channel encodes, excluding test PCM generation and radio. RF work includes PCM generation, encoding and enqueue/preemption cost; it does not wait for the final ACK before encoding the next frame. Figures are mean / observed maximum milliseconds.

| Variant | 256 kb/s local API | 320 kb/s local API | 256 kb/s RF work | 320 kb/s RF work |
|---|---:|---:|---:|---:|
| Baseline | 3.316 / 3.737 | 3.377 / 3.815 | 3.722 / 4.120 | 3.797 / 4.212 |
| No LTPF analysis | 2.458 / 2.856 | 2.517 / 2.936 | 2.866 / 3.357 | 2.937 / 3.418 |
| No TNS | 3.145 / 3.458 | 3.206 / 3.522 | 3.550 / 3.937 | 3.626 / 3.998 |
| No LTPF or TNS | 2.286 / 2.509 | 2.345 / 2.569 | 2.693 / 2.991 | 2.767 / 3.052 |
| No gain refinement | 3.272 / 3.695 | 3.335 / 3.737 | 3.676 / 4.059 | 3.754 / 4.120 |
| All three disabled | 2.243 / 2.455 | 2.304 / 2.500 | 2.650 / 2.930 | 2.725 / 2.991 |

All 14 RF cases reported 1000/1000 missed source deadlines. In the fastest 320 kb/s case, 1000 frames representing 2.5 seconds took 2.738434 seconds through the last ACK. Queue depth peaked at one packet: producer speed, not a growing radio queue, was the primary limit. Local mode has no pacing, so its `late=0` field is not a deadline-pass result.

The receiver verified all 14000 RF frames, with zero missing frames, bad CRCs or overflow. TX reported two retries total, zero exhausted ACKs, zero codec/send errors and zero timing cross-check errors. Eventual delivery under an overloaded source is not successful real-time playback. No Wi-Fi/Bluetooth coexistence stress or long soak was justified by these cadence failures.

Baseline local stage averages, microseconds per stereo frame:

| Stage | 256 kb/s | 320 kb/s |
|---|---:|---:|
| Attack detector | 11 | 11 |
| Pitch/LTPF analysis | 867 | 866 |
| MDCT | 516 | 517 |
| Band energies | 73 | 73 |
| Bandwidth detector | 10 | 10 |
| Spectral noise shaping (SNS) | 877 | 877 |
| Temporal noise shaping (TNS) | 173 | 173 |
| Spectrum analysis/quantization/rate control | 410 | 435 |
| Bitstream encoding | 253 | 289 |
| Initial gain estimate, nested inside spectrum stage | 144 | 147 |

Do not add the last row to the spectrum row. In radio mode the stage counters include worker/interrupt preemption. The local figures best show relative algorithm costs. DWT uses the actual CPU frequency and is cross-checked against RTC time with a 200 us tolerance. RTT stayed attached throughout.

## Decoder and loss validation

`tools/build-lc3-lite-host.ps1` builds two MSVC C11 DLLs: completely unmodified upstream and the experimental encoder sources. Every encoded stream is decoded by the separately built **unmodified** DLL. The modified DLL's decoder is not used for validation.

Eight synthetic stereo sources (tones, multitone, chirp, quiet 24-bit tones, noise, percussion, impulses, changing harmonics), 240 frames each, two rates and seven flag combinations produce 112 signal probes. All packets were accepted without accidental decoder concealment. The flags-zero encoder matched all 7680 baseline channel packets byte for byte on this host compiler.

For every signal/rate/variant, tests drop one frame, four consecutive frames, twenty consecutive frames, and periodically drop 1% or 5% during the middle of the stream: 560 scenarios. The decoder returned its PLC status exactly on requested erasures, returned successful decode on intact frames, and produced finite samples. Each test returned to its corresponding no-loss decoded waveform within the final 20-frame comparison window at a maximum absolute difference below 1e-6 full scale. Across all probes, differences above that threshold persisted as far as 25 good frames (62.5 ms) beyond the final erasure. This is observed convergence on these probes, not a guaranteed recovery bound.

Preserving the PLC code does not preserve the perceptual quality of concealment automatically: its decoded history changes when encoder decisions change. Recovery need not be bit-identical on the first intact frame, because synthesis overlap, postfiltering and concealment have state. This experiment validates Google's current PLC, not every concealment method in the ETSI reference implementation.

These tests do not compare host and ARM encoded samples bit for bit, certify standard conformance, or qualify acoustic quality. Hardware RX verified packets, while host tests exercised decoding.

## Audio-quality implications

The waveform-error metrics in `tables.md` are diagnostic, delay aligned by the library's reported 120-sample codec delay. They are not perceptual scores or effective-bit-depth estimates. No music corpus, listening/ABX or analog DAC measurement was performed.

There is clear evidence against assuming these shortcuts are free. On the 320 kb/s chirp, baseline waveform SNR was 60.30 dB, no TNS was 46.96 dB, and no gain refinement was 33.13 dB. A tone can also improve when LTPF is disabled; the upstream API explicitly describes its poor behavior on some synthetic signals. Such results must not be generalized into a ranking of music quality.

At 48 kHz/2.5 ms, Google's decoder formula makes LTPF synthesis inactive at 50 bytes/channel (320 kb/s stereo), but it can still be active at 40 bytes/channel (256 kb/s stereo). Disabling the encoder analysis at 320 kb/s remains capable of changing the bitstream and reconstructed samples because it also changes pitch signaling and the spectral bit budget. It is not a bit-exact optimization.

SNS and TNS serve distinct purposes. SNS allocates quantization noise across frequency; TNS shapes it in time. Skipping either can affect masking or transients even when decoder syntax remains valid. See [ETSI TS 103 634, sections 5.3.7, 5.3.9 and 5.3.11](https://www.etsi.org/deliver/etsi_ts/103600_103699/103634/01.07.01_60/ts_103634v010701p.pdf).

## Closed-form gain with variable ESB payloads

The user's proposal removes an unnecessary transport restriction **if 256/320 kb/s are average targets rather than strict per-frame budgets**. A candidate architecture is:

1. Keep MDCT and spectral shaping. Estimate quantizer scale from shaped band energies and a measure of spectral flatness/tonality, rather than total energy alone.
2. Quantize once and entropy-code into a bounded scratch packet; send the actual produced length. An oversized scratch allocation does not require transmitting its unused capacity.
3. Signal the gain, shaping parameters and channel lengths explicitly in each frame. Reset entropy-coder state at frame boundaries, so the next packet remains parseable after a loss.
4. Limit peak packet size and rolling average bitrate. Use a bounded coarsening/fallback path when a frame exceeds the limit; measure that path's deadline and quality, too.

A uniform-quantization model can provide an initial closed-form scale from a desired quantization-error energy, but its high-rate/uniform-error assumptions are not an audibility guarantee for sparse spectra or transients. Equal total energy can describe a single tone or broadband noise with quite different masking and encoded sizes. A fixed quality setting therefore produces variable bitrate; an average bitrate target still needs rate feedback, though it need not use an exact-size search on every frame.

LC3's existing packing cannot simply be shortened after encoding: arithmetic data grow from one end, side/residual bits grow from the other, and frame length influences gain offset and some coding decisions. The stock writer also uses available capacity for residual data. A compact custom format needs explicit stream boundaries, or an encoder that selects the actual frame length consistently before writing. Merely dropping trailing bytes is invalid.

Current codec payloads are exactly 80/100 stereo bytes. The existing ESB application allows 224 codec bytes in a single packet; 224 bytes every 2.5 ms would be 716.8 kb/s of codec payload before transport overhead. That is a size bound, not measured coexistence capacity or a recommendation to consume all headroom. Variable bursts consume airtime and retry margin, and larger packets can increase loss exposure.

The initial gain estimator accounts for only 0.144-0.147 ms of the present stereo encode. A closed-form replacement still costs something; even a hypothetical free replacement would leave essentially no average 256 kb/s margin in the fastest tested RF variant, with remaining worst-case overruns. VBR could additionally simplify bit counting/refinement, so these measurements do not rule out a more extensive redesign. They do show why gain estimation alone is not the dominant optimization target.

## Other simplifications worth distinguishing

| Stage | Assessment |
|---|---|
| LTPF analysis | Largest measured removable stage; use the existing API as the first experiment. Validate tonal/speech quality, especially at 256 kb/s. |
| SNS | Largest remaining analysis stage. Profile its codebook search and vector operations; optimize the same mathematics first. Pruned searches or simpler scalefactor coding require quality evaluation. Removing shaping entirely is a poor default for transparency. |
| MDCT | Essential to this codec architecture. Optimized kernels can preserve the transform; deleting it creates a fundamentally different codec. |
| TNS | About 0.17 ms available here, with a measurable signal-error tradeoff. An adaptive bypass requires criteria that do not miss problematic transients. |
| Gain estimation/rate control | VBR and a better estimate are plausible, but profile savings against peak rate and quality. The fixed-budget refinement bypass tested here saved only about 0.04 ms and harmed some probes substantially. |
| Bandwidth/attack detection | Only about 0.02 ms combined; low priority. Forcing full bandwidth need not improve coding efficiency. |
| Entropy coder | Optimize its implementation first. Simpler Rice/Huffman-style coding may reduce CPU but changes compression efficiency and packet bursts. |
| Decoder PLC/overlap | Keep this path; encoder simplifications need not remove concealment. Do not introduce unsignaled inter-frame decoder dependencies. |

No tested configuration is ready to qualify the nRF52840 dongle for transparent 2.5 ms audio. A custom VBR design remains a plausible next experiment, especially alongside SNS/kernel and transport-overhead optimization, but neither timing nor unchanged perceptual quality has been demonstrated for it.

## Reproduction and artifacts

```powershell
& .\tools\audio-bench.ps1 -Action build -Target tx -Codec google-lite-lto -Frames 1000 -PcmBits 24
& .\tools\audio-bench.ps1 -Action rtt -Target rx -Codec etsi -Seconds 180 -StopOn 'RX_RESULT run=328'
# Start RX capture before flashing TX; use separate processes.
& .\tools\audio-bench.ps1 -Action flash -Target tx -Codec google-lite-lto
& .\tools\audio-bench.ps1 -Action rtt -Target tx -Codec google-lite-lto -Seconds 150 -StopOn SUITE_DONE
& .\tools\build-lc3-lite-host.ps1
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' .\tools\test-lc3-lite.py --output firmware/results/2026-09-19-lc3-lite/host-validation.json
```

The directory includes raw TX/RX logs, numerical results, tables, the measured transmitter ELF/HEX/configuration, generated experimental source, source snapshots and a SHA-256 manifest. The measured transmitter is left running its completed-suite idle loop; the receiver remains the existing CRC benchmark image.
