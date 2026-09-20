"""Compare optimized encoder packets to the previously validated native DLL."""
import argparse
import ctypes as C
import hashlib
import importlib.util
import json
from pathlib import Path
import wave
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument('--candidate',type=Path,default=ROOT/'.bench/lc3-custom-host-opt/lc3-custom.dll')
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
spec=importlib.util.spec_from_file_location('custom',Path(__file__).with_name('test-lc3-custom.py'))
t=importlib.util.module_from_spec(spec); spec.loader.exec_module(t)
baseline=t.lib
candidate=C.CDLL(str(a.candidate.resolve()))
for name in ('lc3_encoder_size','lc3_setup_encoder','lc3_decoder_size','lc3_setup_decoder',
             'lc3_custom_configure','lc3_custom_encode','lc3_custom_decode'):
    f=getattr(candidate,name); ref=getattr(baseline,name)
    f.argtypes=ref.argtypes; f.restype=ref.restype
checked=0; probes=[]

def compare(name,x,rates,configs):
    global checked
    for rate in rates:
        for config in configs:
            t.lib=baseline; expected,old_stats=t.encode(x,rate,config)
            t.lib=candidate; actual,new_stats=t.encode(x,rate,config)
            for seq,(old,new) in enumerate(zip(expected,actual)):
                for ch in range(2):
                    assert old[ch]==new[ch],(name,rate,config,seq,ch,old[ch].hex(),new[ch].hex())
                    checked+=1
            assert old_stats==new_stats,(name,rate,config,old_stats,new_stats)
            probes.append(dict(signal=name,rate=rate,config=config,frames=len(actual)))
        print(f'PASS {name} {rate}: {len(configs)} configurations',flush=True)

for name,x in t.signals():
    compare(name,x,(256000,320000,400000,480000),t.CONFIGS)
with wave.open(str(ROOT/'firmware/benchmark/24_48k_PerfectTest.wav')) as w:
    b=np.frombuffer(w.readframes(w.getnframes()),np.uint8).astype(np.int32).reshape(-1,3)
x=((b[:,0]|b[:,1]<<8|b[:,2]<<16)<<8)>>8
x=x[:len(x)//240*240].reshape(-1,2)
compare('WAV',x,(256000,320000,400000,480000),((1,2,0),))
# Longer changing, full-band input exercises cap/requantize fallback and fast fit.
rng=np.random.default_rng(52840)
x=rng.integers(-8388608,8388608,size=(120*1200,2),dtype=np.int32)
x[120*300:120*600] >>= 12
x[120*600:120*650]=0
compare('random',x,(256000,320000,400000,480000),((1,2,0),))
result=dict(identical_channel_packets=checked,probes=probes,
    baseline_sha256=hashlib.sha256((ROOT/'.bench/lc3-custom-host/lc3-custom.dll').read_bytes()).hexdigest(),
    candidate_sha256=hashlib.sha256(a.candidate.read_bytes()).hexdigest(),
    limitation='Native MSVC equivalence, not ARM bit-exact certification or listening validation.')
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS {checked} byte-identical channel packets',flush=True)
