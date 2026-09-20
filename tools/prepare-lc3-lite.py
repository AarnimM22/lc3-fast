"""Generate auditable encoder-only experiments from pinned Google liblc3.

No upstream file is overwritten. Exact-match checks reject source drift.
Flags: 1 uses the existing disable-LTPF API in the caller; 2 disables TNS;
4 skips gain refinement, retaining gain estimation and hard bit-budget fitting.
"""
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--source", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)

def replace(text, old, new):
    assert text.count(old) == 1, (old, text.count(old))
    return text.replace(old, new)

def measure(text, old, slot, statement=None):
    return replace(text, old, f"LITE_MEASURE(PROF_{slot}, {statement or old});")

s = (a.source / "lc3.c").read_text()
s = replace(s, "bool att = lc3_attdet_run(dt, sr_pcm, nbytes, &encoder->attdet, xt);",
    "bool att; LITE_MEASURE(PROF_ATTACK, att = lc3_attdet_run(dt, sr_pcm, nbytes, &encoder->attdet, xt));")
for old, slot in [
    ("side->pitch_present = !encoder->ltpf_bypass &&\n        lc3_ltpf_analyse(dt, sr_pcm, &encoder->ltpf, xt, &side->ltpf);", "LTPF"),
    ("lc3_mdct_forward(dt, sr_pcm, sr, xs, xd, xf);", "MDCT"),
    ("side->bw = lc3_bwdet_run(dt, sr, e);", "BW"),
    ("lc3_sns_analyze(dt, sr, nbytes, e, att, &side->sns, xf, xf);", "SNS"),
    ("lc3_tns_analyze(dt, side->bw, nn_flag, nbytes, &side->tns, xf);", "TNS"),
    ("lc3_spec_analyze(dt, sr,\n        nbytes, side->pitch_present, &side->tns,\n        &encoder->spec, xf, &side->spec);", "SPEC"),
    ("encode(encoder, &side, nbytes, out);", "BITS"),
]:
    s = measure(s, old, slot)
s = replace(s, "bool nn_flag = lc3_energy_compute(dt, sr, xf, e);",
    "bool nn_flag; LITE_MEASURE(PROF_ENERGY, nn_flag = lc3_energy_compute(dt, sr, xf, e));")
(a.output / "lc3.c").write_text('#include "lc3_lite.h"\n' + s)

s = (a.source / "tns.c").read_text()
s = replace(s, "    compute_lpc_coeffs(dt, bw, maxorder, x, pred_gain, a);", """    if (lc3_lite_flags & LITE_NO_TNS) {
        data->rc_order[0] = data->rc_order[1] = 0;
        return;
    }
    compute_lpc_coeffs(dt, bw, maxorder, x, pred_gain, a);""")
(a.output / "tns.c").write_text('#include "lc3_lite.h"\n' + s)

s = (a.source / "spec.c").read_text()
s = replace(s, "int g_min, g_int = estimate_gain(dt, sr,\n        x, nbytes, nbits_budget, nbits_off, g_off, &reset_off, &g_min);",
    "int g_min, g_int; LITE_MEASURE(PROF_GAIN, g_int = estimate_gain(dt, sr,\n        x, nbytes, nbits_budget, nbits_off, g_off, &reset_off, &g_min));")
s = replace(s, "int g_adj = adjust_gain(dt, sr,\n        g_off + g_int, nbits, nbits_budget, g_off + g_min);",
    "int g_adj = (lc3_lite_flags & LITE_NO_GAIN_REFINE) ? 0 : adjust_gain(dt, sr,\n        g_off + g_int, nbits, nbits_budget, g_off + g_min);")
(a.output / "spec.c").write_text('#include "lc3_lite.h"\n' + s)
print(f"Generated 3 LC3 encoder experiment units in {a.output}")
