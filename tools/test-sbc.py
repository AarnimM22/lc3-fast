"""Host-side SBC round trips and independent FFmpeg decoder checks."""
import argparse
import ctypes as C
import json
from pathlib import Path
import subprocess
import numpy as np

class Frame(C.Structure):
    _fields_ = [('msbc', C.c_bool), ('freq', C.c_int), ('mode', C.c_int),
                ('bam', C.c_int), ('nblocks', C.c_int), ('nsubbands', C.c_int), ('bitpool', C.c_int)]

p = argparse.ArgumentParser()
p.add_argument('--dll', type=Path, required=True)
p.add_argument('--bits', type=int, choices=[16, 24], required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--ffmpeg', default='C:/Program Files/Krita (x64)/bin/ffmpeg.exe')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
lib = C.CDLL(str(a.dll.resolve()))
enc = getattr(lib, 'sbc24_encode' if a.bits == 24 else 'sbc_encode')
dec = getattr(lib, 'sbc24_decode' if a.bits == 24 else 'sbc_decode')
enc.argtypes = [C.c_void_p,C.c_void_p,C.c_int,C.c_void_p,C.c_int,C.c_void_p,C.c_void_p,C.c_uint]
dec.argtypes = [C.c_void_p,C.c_void_p,C.c_uint,C.c_void_p,C.c_void_p,C.c_int,C.c_void_p,C.c_int]
enc.restype = dec.restype = C.c_int
dtype = np.int32 if a.bits == 24 else np.int16
fullscale = float(2**(a.bits-1))
n = 128*300
t = np.arange(n)/48000
signals = {f'tone_{db}dB': np.column_stack([np.sin(2*np.pi*997*t), np.sin(2*np.pi*1703*t)])*10**(db/20)
           for db in [-3, -60, -100, -110]}
signals['silence'] = np.zeros((n,2))
signals['noise'] = np.random.default_rng(123).uniform(-.9,.9,(n,2))
signals['rails'] = np.tile(np.array([[1-1/fullscale,-1],[-1,1-1/fullscale]]),(n//2,1))
results = {}
for label, original in signals.items():
    pcm = np.clip(np.rint(original*fullscale), -fullscale, fullscale-1).astype(dtype)
    output = np.zeros_like(pcm)
    estate, dstate = C.create_string_buffer(8192), C.create_string_buffer(8192)
    packet = C.create_string_buffer(512)
    stream = bytearray()
    frame = Frame(False,3,1,0,16,8,28)
    for i in range(300):
        frame.bitpool = 27 if i%3==0 else 28
        size = 12+4*frame.bitpool
        ip = pcm.ctypes.data+i*128*2*pcm.itemsize
        op = output.ctypes.data+i*128*2*pcm.itemsize
        assert enc(estate,ip,2,ip+pcm.itemsize,2,C.byref(frame),packet,512)==0, (label,i,'encode')
        stream.extend(packet.raw[:size])
        parsed = Frame()
        assert dec(dstate,packet,size,C.byref(parsed),op,2,op+pcm.itemsize,2)==0, (label,i,'decode')
        assert parsed.freq==3 and parsed.mode==1 and parsed.bitpool==frame.bitpool
    stream_path = a.out / f'{label}.sbc'
    stream_path.write_bytes(stream)
    output.astype(dtype).tofile(a.out / f'{label}-decoded-s{a.bits}.pcm')
    pcm.tofile(a.out / f'{label}-input-s{a.bits}.pcm')
    ff = subprocess.run([a.ffmpeg,'-v','error','-f','sbc','-i',str(stream_path),'-f','f32le','-acodec','pcm_f32le','-'],capture_output=True,check=True)
    assert not ff.stderr, ff.stderr.decode(errors='replace')
    reference = np.frombuffer(ff.stdout,dtype='<f4').reshape(-1,2)
    assert reference.shape==output.shape
    decoded = output.astype(np.float64)/fullscale
    assert np.isfinite(decoded).all()
    # SBC synthesis delay, find alignment using non-periodic noise below.
    if label == 'noise':
        errors=[np.mean((decoded[1024+lag:,0]-original[1024:n-lag,0])**2) for lag in range(160)]
        delay=int(np.argmin(errors))
    # Tones can be assessed without assuming delay via sine/cosine fitting.
    tone_metrics=[]
    if label.startswith('tone'):
        for ch,freq in enumerate([997,1703]):
            ref=np.column_stack([np.sin(2*np.pi*freq*t[1024:]),np.cos(2*np.pi*freq*t[1024:]),np.ones(n-1024)])
            coeff=np.linalg.lstsq(ref,decoded[1024:,ch],rcond=None)[0]
            residual=decoded[1024:,ch]-ref@coeff
            amp=np.hypot(coeff[0],coeff[1])
            noise=np.sqrt(np.mean(residual**2))
            tone_metrics.append({'amplitude_dbfs':float(20*np.log10(max(amp,1e-30))),
                                 'residual_dbfs':float(20*np.log10(max(noise,1e-30))),
                                 'snr_db':float(20*np.log10(max(amp/np.sqrt(2),1e-30)/max(noise,1e-30)))})
    mismatch=float(np.sqrt(np.mean((decoded-reference.astype(np.float64))**2)))
    results[label]={'bytes':len(stream),'decoded_samples_per_channel':n,
                    'input_nonzero':int(np.count_nonzero(pcm)), 'output_nonzero':int(np.count_nonzero(output)),
                    'ffmpeg_rms_difference':mismatch,'tones':tone_metrics}
    if label == 'noise': results[label]['delay_samples']=delay
    print(label, json.dumps(results[label]))
    assert mismatch < .01, (label,'independent decoder mismatch',mismatch)
    if label=='tone_-3dB':
        assert all(abs(x['amplitude_dbfs']+3)<.2 and x['snr_db']>35 for x in tone_metrics)
    if label=='silence': assert not np.count_nonzero(output)
    if a.bits==24 and label=='tone_-110dB':
        assert all(abs(x['amplitude_dbfs']+110)<3 for x in tone_metrics), 'Low input bits lost'
(a.out/'validation.json').write_text(json.dumps(results,indent=2)+'\n')
