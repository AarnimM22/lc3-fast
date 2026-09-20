"""Research-format validation and waveform-error probes; not listening tests."""
import argparse
import ctypes as C
import json
from pathlib import Path
import wave
import importlib.util
import numpy as np
_spec=importlib.util.spec_from_file_location('lite_test',Path(__file__).with_name('test-lc3-lite.py'))
lite_test=importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lite_test)
base,state,signals,quality=lite_test.base,lite_test.state,lite_test.signals,lite_test.quality
N,FS,FRAMES,ROOT=lite_test.N,lite_test.FS,lite_test.FRAMES,lite_test.ROOT

CONFIGS=((0,0,1),(1,0,1),(2,0,1),(3,0,1),(1,1,1),(1,2,1),(2,2,1),(3,2,1),
         (1,0,0),(1,2,0),(3,0,0),(3,2,0))
lib=C.CDLL(str(ROOT/'.bench/lc3-custom-host/lc3-custom.dll'))
for kind in ('encoder','decoder'):
    f=getattr(lib,f'lc3_{kind}_size'); f.argtypes=[C.c_int]*2; f.restype=C.c_uint
    f=getattr(lib,f'lc3_setup_{kind}'); f.argtypes=[C.c_int]*3+[C.c_void_p]; f.restype=C.c_void_p
lib.lc3_custom_configure.argtypes=[C.c_uint]*3
lib.lc3_custom_configure.restype=None
lib.lc3_custom_encode.argtypes=[C.c_void_p,C.c_int,C.c_void_p,C.c_int,C.c_int,C.c_void_p,C.c_int]
lib.lc3_custom_decode.argtypes=[C.c_void_p,C.c_void_p,C.c_int,C.c_int,C.c_void_p,C.c_int]
lib.lc3_custom_encode.restype=lib.lc3_custom_decode.restype=C.c_int

def encode(x,rate,config):
    lib.lc3_custom_configure(*config)
    states=[state(lib,'encoder') for _ in range(2)]
    packets=[]
    for seq,frame in enumerate(x.reshape(-1,N,2)):
        pair=[]
        for ch in range(2):
            block=seq+ch
            target=(block+1)*rate//6400-block*rate//6400
            pcm=np.ascontiguousarray(frame[:,ch],dtype=np.int32)
            out=C.create_string_buffer(102)
            size=lib.lc3_custom_encode(states[ch][1],1,pcm.ctypes.data,1,target,out,102)
            assert 22<=size<=102,(seq,ch,config,rate,size)
            pair.append(out.raw[:size])
        packets.append(pair)
    stats={name:C.c_uint.in_dll(lib,name).value for name in ('custom_cap_frames','custom_sns_clips')}
    sizes=np.array([sum(map(len,p)) for p in packets])
    stats.update(actual_bitrate=float(sizes.sum()*8/(len(packets)*.0025)),
        min_bytes=int(sizes.min()),max_bytes=int(sizes.max()),
        max_20_frame_bitrate=float(np.convolve(sizes,np.ones(20),mode='valid').max()*8/.05))
    assert sizes.max()<=204
    assert stats['actual_bitrate']<=rate+1e-6
    return packets,stats

def decode(packets,drops=()):
    states=[state(lib,'decoder') for _ in range(2)]
    y=np.empty((len(packets)*N,2),dtype=np.float32)
    for seq,pair in enumerate(packets):
        for ch,data in enumerate(pair):
            pcm=np.empty(N,dtype=np.float32)
            lost=seq in drops
            buf=None if lost else C.create_string_buffer(data)
            rc=lib.lc3_custom_decode(states[ch][1],buf,len(data),3,pcm.ctypes.data,1)
            assert rc==int(lost),(seq,ch,rc,data.hex())
            assert np.isfinite(pcm).all()
            y[seq*N:(seq+1)*N,ch]=pcm
    return y.astype(np.float64)

def stock(x,rate,subtract_header=False):
    states=[state(base,'encoder') for _ in range(2)]
    for _,ptr in states: base.lc3_encoder_disable_ltpf(ptr)
    packets=[]
    for seq,frame in enumerate(x.reshape(-1,N,2)):
        pair=[]
        for ch in range(2):
            block=seq+ch
            size=(block+1)*rate//6400-block*rate//6400-(2 if subtract_header else 0)
            pcm=np.ascontiguousarray(frame[:,ch],dtype=np.int32)
            out=C.create_string_buffer(size)
            assert base.lc3_encode(states[ch][1],1,pcm.ctypes.data,1,size,out)==0
            pair.append(out.raw)
        packets.append(pair)
    return packets,lite_test.decode(packets)

def save_wav(path,y):
    values=np.clip(np.rint(y*8388608),-8388608,8388607).astype(np.int32).reshape(-1)
    packed=np.column_stack((values&255,(values>>8)&255,(values>>16)&255)).astype(np.uint8).tobytes()
    with wave.open(str(path),'wb') as f:
        f.setnchannels(2); f.setsampwidth(3); f.setframerate(FS); f.writeframes(packed)

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    rows=[]; losses=[]; matched=0
    for name,x in signals():
        for rate in (256000,320000,400000,480000):
            ref_packets,ref=stock(x,rate)
            compat,_=stock(x,rate,True)
            rows.append(dict(signal=name,rate=rate,sns=-1,rate_mode=-1,tns=1,
                actual_bitrate=rate,**quality(x,ref)))
            for config in CONFIGS:
                packets,stats=encode(x,rate,config)
                y=decode(packets)
                if config==(0,0,1):
                    for expected,got in zip(compat,packets):
                        for e,g in zip(expected,got):
                            assert e==g[2:],'stock SNS payload changed'
                            matched+=1
                row=dict(signal=name,rate=rate,sns=config[0],rate_mode=config[1],tns=config[2],
                    **stats,**quality(x,y))
                rows.append(row)
                for label,drops in (('isolated',{60}),('burst4',set(range(60,64))),
                        ('burst20',set(range(60,80))),('periodic5pct',set(range(40,FRAMES-40,20)))):
                    z=decode(packets,drops)
                    final=float(np.max(np.abs(z[-20*N:]-y[-20*N:])))
                    assert final<1e-6,(name,rate,config,label,final)
                    losses.append(dict(signal=name,rate=rate,sns=config[0],rate_mode=config[1],tns=config[2],
                        pattern=label,final_difference=final))
                if name=='chirp' and rate==400000 and config in ((1,0,1),(1,2,1),(2,0,1)):
                    save_wav(a.output/f'chirp-sns{config[0]}-rate{config[1]}-400k.wav',y)
            print(f'PASS {name} {rate}: {len(CONFIGS)} variants and {4*len(CONFIGS)} loss cases',flush=True)
    # Malformed framing must fail before attempting entropy decode.
    mem,dec=state(lib,'decoder'); pcm=np.zeros(N,np.float32)
    for bad in (b'',b'\xd0\x14'+bytes(19),b'\xd4\x14'+bytes(20),b'\xd0\xff'+bytes(20)):
        buf=C.create_string_buffer(bad)
        assert lib.lc3_custom_decode(dec,buf,len(bad),3,pcm.ctypes.data,1)==-1
    result=dict(settings=dict(rate=48000,channels=2,pcm_bits=24,frame_us=2500,
        frames=FRAMES,configs=CONFIGS),baseline_identical_channel_packets=matched,
        quality=rows,loss_tests=losses,
        limitation='Synthetic waveform metrics, not ABX, perceptual transparency or ARM bit-exact validation.')
    (a.output/'host-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(f'PASS {len(rows)} quality probes, {len(losses)} loss cases, {matched} stock-equivalent packets')

if __name__=='__main__':main()
