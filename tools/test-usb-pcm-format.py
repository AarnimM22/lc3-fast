"""Check packed/interleaved USB PCM against the prior planar 24-bit input path."""
import ctypes as C
import importlib.util
from pathlib import Path
import wave
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('custom',Path(__file__).with_name('test-lc3-custom.py'))
t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
with wave.open(str(ROOT/'firmware/benchmark/24_48k_PerfectTest.wav')) as w:
    assert (w.getnchannels(),w.getsampwidth(),w.getframerate())==(2,3,48000)
    packed=w.readframes(w.getnframes())
frames=len(packed)//720
packed=packed[:frames*720]
b=np.frombuffer(packed,np.uint8).astype(np.int32).reshape(-1,3)
x=((b[:,0]|b[:,1]<<8|b[:,2]<<16)<<8)>>8
x=x.reshape(-1,2)
checked=0
for rate in (320000,400000):
    reference,_=t.encode(x,rate,(1,2,0))
    t.lib.lc3_custom_configure(1,2,0)
    states=[t.state(t.lib,'encoder') for _ in range(2)]
    data=C.create_string_buffer(packed)
    for seq in range(frames):
        for ch in range(2):
            block=seq+ch
            target=(block+1)*rate//6400-block*rate//6400
            out=C.create_string_buffer(102)
            n=t.lib.lc3_custom_encode(states[ch][1],2,C.addressof(data)+seq*720+ch*3,2,target,out,102)
            assert n>0 and out.raw[:n]==reference[seq][ch],(rate,seq,ch,n)
            checked+=1
    print(f'PASS {rate}: {frames} stereo WAV frames, packed/interleaved input produces identical packets',flush=True)
print(f'PASS {checked} channel packets')
