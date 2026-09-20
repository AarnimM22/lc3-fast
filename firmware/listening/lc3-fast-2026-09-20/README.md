# Custom lc3-fast music listening set

24 WAV files: three music excerpts, each with one reference and seven decoded versions.
All listening WAVs are stereo, 48,000 Hz, signed 24-bit PCM, with matching lengths and
the codec's 120-sample delay removed. Start with the files in the track folders below.

| Folder / playlist | Music | Source interval | Suggested listening focus |
| --- | --- | --- | --- |
| [01-bee-moved](01-bee-moved/all-rates.m3u8) | Blue Monday FM — Bee Moved | 0:00–0:39.875 | Chimes, handclaps, cymbals, rim clicks and stereo effects |
| [02-jazzy](02-jazzy/all-rates.m3u8) | 3delite — 40 - 20 - Jazzy | 0:05–1:05 | Rhythmic detail, pitched notes, ambience and decays |
| [03-techno-goa-industrial](03-techno-goa-industrial/all-rates.m3u8) | 3delite — 22 - 62 - Techno - goa - industrial | 0:30–1:30 | Dense synth textures, percussion, bass against treble detail |

Each folder contains:

- `reference-48k24.wav`: the uncompressed comparison reference, with the same gain as the codec inputs.
- `lc3-fast-400k.wav`, `lc3-fast-320k.wav`, `lc3-fast-256k.wav`, `lc3-fast-224k.wav`,
  `lc3-fast-192k.wav`, `lc3-fast-160k.wav`, `lc3-fast-140.8k.wav`.
- `all-rates.m3u8`: reference followed by the descending bitrate ladder.
- `manifest.json`: provenance, measured bitrate, processing details and file hashes.
- `encoded/`: the actual compressed channel packets, including pre-roll and flush frames.

The bitrate labels mean **total stereo codec payload**, including the custom two-byte
header per channel. They exclude ESB/radio headers, acknowledgements and retransmissions.
WAVs are decoded PCM, so all versions of an excerpt have the same WAV size.

## How to find your threshold

1. Start with `02-jazzy/reference-48k24.wav` against `02-jazzy/lc3-fast-140.8k.wav`.
   This is the most strongly degraded example by waveform error in this set. Find a
   short passage where you can describe a repeatable difference. Also try Bee Moved.
   I have not established a listener's detection threshold; audibility is not guaranteed.
2. Compare the reference against 160, 192, 224, 256, 320 and 400 kb/s, moving upward.
   Loop the same 5–10 seconds and keep the player volume fixed. Switch quickly;
   remembering a minute-long passage makes subtle comparisons difficult.
3. Around the point where the difference becomes uncertain, use a blind comparison.
   [foobar2000's ABX Comparator](https://www.foobar2000.org/components/view/foo_abx)
   supports double-blind tests between two tracks. Select the reference and one decoded
   WAV. Disable ReplayGain, normalization, crossfade, EQ and other player DSP for the test.
   Use a 48 kHz playback path if practical.
4. Practice first, then choose a fixed number of trials (for example 16) before starting
   a scored run. Save the result and your passage timestamps. Don't stop a run the first
   time its score looks favorable. Recheck apparent differences in a separate run.
5. Compare adjacent candidates too: 256 vs 320, then 320 vs 400. A failure to distinguish
   two encoded versions does not by itself establish that either matches the reference.
   Repeat on all three excerpts before choosing a provisional working bitrate.

The useful result is your lowest consistently acceptable rate on these passages, with
some margin for other music. A successful test on three excerpts is not proof of
transparency for every recording. Keep notes in [listening-notes.csv](listening-notes.csv).

## Sources and limits

- **Bee Moved:** [Sony's official downloadable comparison sample](https://helpguide.sony.net/high-res/sample1/v1/en/index.html),
  supplied as 96 kHz / 24-bit FLAC. The full 39.876-second download is retained; the
  listening files omit its final millisecond to end on a codec-frame boundary.
- **Jazzy and Techno - goa - industrial:** [3delite's official free FLAC downloads](https://www.3delite.hu/MP4%20Stream%20Editor/music.html).
  The artist identifies the synthesizers and final mixes as 96 kHz / 24-bit, but says
  some sampled instruments originate at 44.1 kHz / 16-bit. These are real 24-bit music
  mixes, not an exclusively native-hires acoustic-recording corpus.

All three downloaded files were checked to be two-channel 96 kHz PCM_24 FLACs, with
nonzero low-order sample bytes. That checks the file representation, not the effective
resolution of every instrument or recording stage. Original downloads and source-page
snapshots are in `sources/`. Copyright remains with the respective rightsholders.
3delite permits non-commercial use and requires contact for commercial use. Sony offers
its sample for downloading and listening. This folder is for local evaluation; no public
redistribution license is asserted. No purchased music or streaming-service captures were used.

## Exact codec and audio processing

- Current custom lc3-fast research format v1, host build of the project sources with
  optimization mask **31**. It is not a standard LC3/LC3plus bitstream.
- **2.5 ms**, 120 samples/channel, 48 kHz, separate channel states, signed 24-bit encoder
  input, coarse SNS, analytic VBR gain selection, TNS off, LTPF off.
- No dropped packets. This evaluates codec audio quality, not radio reliability or PLC.
- Entire source downsampled in float64 using **libsoxr VHQ** before selecting excerpts.
  Fixed **−3 dB gain before encoding**, applied to every source to provide headroom.
  The saved reference receives the same gain. No independent loudness normalization.
- Deterministic, unshaped TPDF dither when quantizing the resampled encoder input to
  24 bits and when writing the final 24-bit WAVs. The latter is below the lossy-codec
  error by a very large margin. No truncation to 16 bits.
- One second of actual preceding music initializes state for the two excerpts cut from
  longer tracks. Bee Moved starts at its original beginning with zero-initialized state.
  Enough following audio/zero flush is encoded to recover the complete delayed ending.
- Delay API reports **120 samples (2.5 ms)**; a stereo impulse round trip independently
  measured the same delay. All decoded files are trimmed accordingly.
- Additional output attenuation would be applied equally to every variant and reference
  if any version needed it. **None was needed here**; every output peak is below −1 dBFS.
- All 24 WAVs were read back and checked against the intended integer samples. All
  encoded frames decoded successfully. The independent verification also checks hashes,
  packet framing, sample counts, channel order and alignment on the music itself:
  **906,626 archived channel packets decoded again, zero errors, zero residual sample
  shift on either channel of every variant**. The reconstructed samples match the saved
  WAVs within the expected 24-bit dither/rounding tolerance. See `verification.json`.

Minimum bitrate is **140.8 kb/s stereo**: the current encoder requires at least 22 bytes
per channel per 2.5 ms frame. A 128 or 96 kb/s version would require changing the codec
format/configuration. The lowest-rate examples therefore stop at 140.8; they aren't
artificially damaged to create an easy listening anchor.

Actual average payload rates are very close to their targets on this music:

| Target kb/s | Bee Moved | Jazzy | Techno / goa / industrial |
| ---: | ---: | ---: | ---: |
| 400 | 399.996 | 399.998 | 399.995 |
| 320 | 319.991 | 319.994 | 319.985 |
| 256 | 255.806 | 255.918 | 255.887 |
| 224 | 223.979 | 223.995 | 223.636 |
| 192 | 191.999 | 192.000 | 191.998 |
| 160 | 160.000 | 160.000 | 160.000 |
| 140.8 | 140.800 | 140.800 | 140.800 |

Unlike the earlier test signal, these excerpts use nearly all the configured budget.
Measured rates cover the excerpt's input frame interval; pre-roll/flush traffic is excluded.
Payload size still varies from frame to frame. SNS saturation counts are zero; gain
coarsening when a provisional frame exceeds its byte allowance is retained as in the
current firmware, and its counts are recorded in the manifests.

Waveform SNR improves from **13.54 to 32.30 dB** on Bee Moved, **10.60 to 27.03 dB** on
Jazzy and **23.49 to 38.97 dB** on the electronic excerpt between 140.8 and 400 kb/s.
These are alignment/error diagnostics, not perceptual scores, effective DAC bit depths,
or evidence that any particular bitrate is transparent. A 24-bit output container also
does not make a lossy codec lossless or give it 24-bit waveform accuracy.

## Reproduce

From the repository root, with the existing Nordic toolchain and Visual C++ installed:

```powershell
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' -m pip install --target .bench\listening-python soundfile==0.14.0 soxr==1.1.0 numpy==2.5.3
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\fetch-listening-sources.py --manifest tools\listening-sources.json
powershell.exe -NoProfile -File .\tools\build-lc3-custom-host.ps1 -Optimizations 31 -OutputDirectory .bench\lc3-listening-host
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\make-lc3-listening-set.py
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\verify-lc3-listening-set.py
```

`manifest.json` records the codec DLL SHA-256, generated codec source hashes, dependency
versions, source/WAV hashes and all measured data. `reproducibility/lc3-custom.dll` preserves
the exact Windows host codec used here. The generator accepts `--dll` to reuse that binary
even if the working codec sources subsequently change. This is a host build of the current
firmware codec; it does not establish bit-for-bit identity with every ARM compiler build.

The `.lc3fast` files are an archival container, not an ESB capture: eight-byte `LC3FAST1`
magic, six little-endian uint32 values (sample rate, frame microseconds, channels, target
bitrate, stereo frame count, configuration `0x201`), then records with two uint8 channel
lengths followed by left and right custom payloads. No extra compression or codec is used.
