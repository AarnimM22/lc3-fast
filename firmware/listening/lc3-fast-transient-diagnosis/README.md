# Bee Moved transient diagnosis

The user's ABX results were 16/16 at 256 kb/s, 13/16 at 320 kb/s and 10/16 at
400 kb/s against the reference. The noticeable 320 kb/s differences were concentrated
in rapid transients in the first three seconds. In a fixed 16-trial fair-guessing test,
the one-sided probabilities of at least those scores are 0.00153%, 1.064% and 22.725%.
These probabilities are not the probability that the listener guessed or that a codec
is transparent. Repeated tests/optional stopping affect their interpretation.

The strongest result from the diagnostic comparison is that **restoring the original
gain-and-budget policy improves waveform accuracy substantially at the same 320 kb/s**.
Merely enabling TNS does not materially improve these aggregate error measurements.
This points to the custom quantizer/rate-control path as the first optimization target.
It does not prove which particular artifact the listener detected.

## Controlled comparison

The original archived codec DLL is used unchanged, selecting existing runtime options.
All variants encode identical samples from the prepared Bee Moved reference. This input
includes the original listening reference's final <=1.5 LSB TPDF/rounding difference,
so these are controlled diagnostic re-encodes, not claimed bit-identical recreations
of the original listening files. No gain normalization, radio loss or firmware changes.
Outputs are eight seconds, 48 kHz / 24-bit stereo, with 120 samples of delay removed.
Final diagnostic WAV quantization uses rounding without additional dither.

Metrics below cover 0:00–0:03. Waveform SNR is an error diagnostic, not a perceptual score.

| Variant | Actual kb/s | Waveform SNR, dB | Change vs current 320 |
| --- | ---: | ---: | ---: |
| Current 320 | 319.899 | 21.769 | — |
| Current 320 with existing TNS enabled | 319.899 | 21.769 | essentially none |
| Current 320 with original initial gain search | 319.925 | 21.782 | +0.013 dB |
| Current 320 with original envelope analysis, still scalar-coded SNS | 319.885 | 21.973 | +0.203 dB |
| Original CBR gain/budget/refinement path, same coarse SNS and TNS off | 320.000 | 25.850 | +4.081 dB |
| Current 400 | 399.904 | 27.926 | +6.157 dB |

The CBR comparison changes feedback, frame-length policy and final gain adjustment
together. It does **not** isolate the final adjustment as the sole source of improvement.
The more accurate initial search alone retaining the custom VBR controller is nearly
unchanged, which argues against reinstating the initial search as the main solution.

A secondary diagnostic measures error in the 0.5 ms before 20 heuristic onsets selected
from the reference alone. The full CBR path reduces aggregate error there by 4.69 dB;
400 kb/s reduces it by 7.69 dB. TNS makes effectively no change. These windows may
contain existing music and do not establish audible pre-echo or exact attack boundaries.
Full parameters and per-event measurements are in `diagnosis.json`.

## Code findings

- `firmware/benchmark/src/custom_spec.inc:61` reserves `side->nq` residual bits plus a
  termination margin. `nq` is a spectral endpoint; it includes zero coefficients within
  that span. Upstream `put_residual()` only emits refinement bits for coefficients that
  survive quantization. The estimate can therefore over-reserve residual space.
- The custom controller feeds this conservative requested length back into its gain
  estimate, and only performs a final correction toward coarser quantization when the
  request exceeds the cap. It lacks the original final correction toward finer gain
  when measured bit use permits it. Together these are plausible inefficiencies;
  the ablation establishes the policy-level effect, not each line's contribution.
- The 320 kb/s custom path invokes cap correction on 61/2400 channel frames in the first
  three seconds. Cap correction is not synonymous with truncation. It cannot by itself
  explain the entire error difference from the CBR path.
- The inherited attack detector returns false for 2.5 ms mode, and coarse SNS does not
  use its attack argument. Rate allocation is not explicitly transient-aware.
- Upstream TNS at 2.5 ms uses one filter with maximum order four, not a 16th-order LPC
  search. It selects activity by prediction gain and near-Nyquist flag. Turning it on
  here does not make a persuasive aggregate improvement; gated/reduced-order TNS is
  consequently not the leading proposal for this passage.

Quantization error from a transform can be spread around a sharp onset, where it may
be easier to hear. TNS is designed to reshape that error in time; see the
[Fraunhofer analysis of low-latency audio coding](https://www.iis.fraunhofer.de/content/dam/iis/de/doc/ame/conference/AES-119-Convention_StructuralAnalysisofLowLatencyAudioCoding_AES6601.pdf).
That general mechanism is consistent with the listening report, but these measurements
do not identify audible pre-echo specifically.

## Fast changes worth testing next

1. Keep the analytic initial gain estimate. Use the already-computed bit count for one
   bounded final gain correction, allowing a finer gain as well as a coarser one.
   Count actual residual-eligible coefficients, preferably in an existing scan, instead
   of reserving one bit for every bin up to the endpoint. Revisit feedback with that
   corrected demand estimate. Retain exact final budget fitting. These are encoder-only
   changes using existing gain/length fields; they need no new decoder inter-frame state.
2. If artifacts remain, add a cheap attack detector using short-block energy rise or
   first-difference energy, and prioritize bits for the affected transform frame and
   necessary neighbor frames. Spending a 400 kb/s-equivalent budget on 10% of frames
   while leaving the others at 320 gives 328 kb/s average. To hold a strict 320 average,
   the other 90% must average about 311.1 kb/s. Dense percussion may trigger much more
   often, so both average and peak limits must remain explicit.
3. The current burst cap is eight extra bytes per channel, allowing at most 371.2 kb/s
   instantaneous payload from a 320 kb/s nominal frame with enough saved budget.
   A 400-equivalent transient frame needs roughly 12–13 extra bytes per channel, so
   simply detecting an attack is insufficient: the cap/reservoir policy also needs work.

No new MCU timing claims are made. An older hardware sweep measured the original
coarse-SNS CBR path at 1.747 ms average versus 1.664 ms for the custom analytic path
at 320 kb/s, encoder-only. That roughly 83 us difference is evidence a bounded refinement
may be affordable, not a timing prediction for a new implementation or the USB music run.
The previous successful USB/RF soak used less than the nominal payload rate; this music
nearly fills the configured rate. Sustained music still needs its own hardware timing test.

## Listen

Use [comparisons.m3u8](comparisons.m3u8), or select these files for ABX:

- `reference-8s.wav`
- `320-current.wav`
- `320-cbr-gain-refinement.wav` — the most informative next listening comparison
- `400-current.wav`

The other three diagnostic variants are also saved. None is asserted to be perceptually
better based on SNR alone. The codec production sources and flashed firmware were not
changed by this investigation. Regenerate with `tools/probe-lc3-transients.py`.
