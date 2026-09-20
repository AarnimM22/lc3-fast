"""Generate research codec from pinned upstream plus the measured lite patches."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
for f in a.source.iterdir():
    if f.suffix in ('.c','.h'): shutil.copyfile(f,a.output/f.name)
subprocess.run([sys.executable,str(Path(__file__).with_name('prepare-lc3-lite.py')),
    '--source',str(a.source),'--output',str(a.output)],check=True)

def replace(s,old,new):
    assert s.count(old)==1,(old,s.count(old))
    return s.replace(old,new)

s=(a.output/'sns.h').read_text()
s=replace(s,'    int lfcb, hfcb;','    uint8_t coarse[16];\n    int lfcb, hfcb;')
(a.output/'sns.h').write_text(s)

s=(a.output/'sns.c').read_text()
table='static const float custom_shape_gain[449] = {\n'+',\n'.join(
    '    '+', '.join(f'{2**(i*3/64):.10e}f' for i in range(j,min(j+8,225)))
    for j in range(-224,225,8))+'\n};\n'
s=replace(s,'LC3_HOT static void spectral_shaping(',table+'\nLC3_HOT static void spectral_shaping(')
# Algebraically collapse upstream's 16->64 interpolation and 64->44 merging.
# Weights sum to 16; gains lie exactly on the 3/64-octave table grid.
def weights(position):
    if position<2:return {0:16}
    if position>=62:return {14:-(2 if position==62 else 6),15:(18 if position==62 else 22)}
    i=(position-2)//4; w=2+4*((position-2)%4)
    return {i:16-w,i+1:w}
rows=[]
for band in range(44):
    w={}
    for pos in ((2*band,2*band+1) if band<20 else (band+20,)):
        for k,v in weights(pos).items():w[k]=w.get(k,0)+v
    if band<20:
        assert all(v%2==0 for v in w.values());w={k:v//2 for k,v in w.items()}
    assert len(w)<=2 and sum(w.values())==16
    if len(w)==1:w[next(iter(w))+1]=0
    (i,wa),(j,wb)=w.items()
    rows.append(f'    {{{i},{j},{wa},{wb}}}')
mapping='static const int8_t custom_band_weights[44][4] = {\n'+',\n'.join(rows)+'\n};\n'
helper=Path(__file__).resolve().parents[1]/'firmware/benchmark/src/custom_sns.inc'
tilt='static const float custom_tilt[16] = {\n    '+', '.join(
    f'{10**((4*i+1.5)/21):.10e}f' for i in range(16))+'\n};\n'
s=replace(s,'void lc3_sns_analyze(',tilt+'\n'+mapping+'\nvoid lc3_sns_analyze(')
s=replace(s,'void lc3_sns_analyze(',helper.read_text()+'\nvoid lc3_sns_analyze(')
s=replace(s,'    float scf[16], cn[4][16];', '''    if (custom_sns) {
        custom_sns_analyze(dt,sr,nbytes,eb,att,data,x,y);
        return;
    }
    float scf[16], cn[4][16];''')
s=replace(s,'    float scf[16], cn[16];','''    if (custom_sns) {
        if (custom_sns == CUSTOM_SNS_UNITY) {
            if (x != y) memcpy(y,x,lc3_ne(dt,sr)*sizeof(*y));
        } else {
            custom_shape_apply(data,true,x,y);
        }
        return;
    }
    float scf[16], cn[16];''')
s=replace(s,'    return 38;','    return custom_sns == CUSTOM_SNS_UNITY ? 0 : custom_sns ? 64 : 38;')
s=replace(s,'void lc3_sns_put_data(lc3_bits_t *bits, const struct lc3_sns_data *data)\n{',
'''void lc3_sns_put_data(lc3_bits_t *bits, const struct lc3_sns_data *data)
{
    if (custom_sns) {
        if (custom_sns != CUSTOM_SNS_UNITY)
            for (int i=0;i<16;i++) lc3_put_bits(bits,data->coarse[i],4);
        return;
    }
''')
s=replace(s,'    *data = (struct lc3_sns_data){','''    if (custom_sns) {
        if (custom_sns != CUSTOM_SNS_UNITY)
            for (int i=0;i<16;i++) data->coarse[i]=lc3_get_bits(bits,4);
        return 0;
    }
    *data = (struct lc3_sns_data){''')
(a.output/'sns.c').write_text('#include "bench_opts.h"\n#include "lc3_custom.h"\n'+s)

s=(a.output/'lc3.c').read_text()
s=replace(s,'LITE_MEASURE(PROF_BITS, encode(encoder, &side, nbytes, out););',
    'LITE_MEASURE(PROF_BITS, encode(encoder, &side, custom_nbytes, out));')
s=replace(s,'int lc3_encode(', 'CUSTOM_RAM int lc3_encode(')
(a.output/'lc3.c').write_text('#include "bench_opts.h"\n#include "lc3_custom.h"\n'+s)

s=(a.output/'spec.c').read_text()
s=replace(s,'void lc3_spec_analyze(',
    (helper.parent/'custom_spec.inc').read_text()+'\nvoid lc3_spec_analyze(')
s=replace(s,'    bool reset_off;','''    if (custom_rate) {
        custom_spec_analyze(dt,sr,nbytes,pitch,tns,spec,x,side);
        return;
    }
    bool reset_off;''')
(a.output/'spec.c').write_text('#include "bench_opts.h"\n#include "lc3_custom.h"\n'+s)

s=(a.output/'mdct.c').read_text()
s=replace(s,'    struct lc3_complex *y[2] = { y1, y0 };', '''#if OPT_MDCT
    /* Exact radix order and scratch-buffer alternation of the generic FFT. */
    if (n==60) {
        fft_5(x,y1,12);
        fft_bf3(lc3_fft_twiddles_bf3[0],y1,y0,4);
        fft_bf2(lc3_fft_twiddles_bf2[0][1],y0,y1,2);
        fft_bf2(lc3_fft_twiddles_bf2[1][1],y1,y0,1);
        return y0;
    }
#endif
    struct lc3_complex *y[2] = { y1, y0 };''')
s=replace(s,'void lc3_mdct_forward(', 'CUSTOM_RAM void lc3_mdct_forward(')
(a.output/'mdct.c').write_text('#include "bench_opts.h"\n'+s)
print('Generated custom SNS/VBR encoder and matching decoder')
