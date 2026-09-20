"""Check independent blocks, loss recovery, bit depth and actual payload rates."""
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import random

root=Path(__file__).resolve().parents[1]
out=root/'.bench/wavpack-validation'
out.mkdir(parents=True,exist_ok=True)
lib=C.CDLL(str(root/'.bench/wavpack-host/wavpack-stream.dll'))
class Config(C.Structure):
    _fields_=[('bitrate',C.c_float),('shaping_weight',C.c_float)]+[
        (x,C.c_int) for x in ['bits_per_sample','bytes_per_sample','qmode','flags','xmode','num_channels','float_norm_exp','block_samples','block_bytes','extra_flags','sample_rate','channel_mask']]+[
        ('md5_checksum',C.c_ubyte*16),('md5_read',C.c_ubyte),('num_tag_strings',C.c_int),('tag_strings',C.c_void_p)]
callback=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_int32)
def bind(name,args,ret):
    fn=getattr(lib,'WavpackStream'+name); fn.argtypes=args; fn.restype=ret; return fn
openenc=bind('OpenFileOutput',[callback,C.c_void_p,C.c_void_p],C.c_void_p)
configure=bind('SetConfiguration64',[C.c_void_p,C.POINTER(Config),C.c_int64,C.c_void_p],C.c_int)
init=bind('PackInit',[C.c_void_p],C.c_int)
pack=bind('PackSamples',[C.c_void_p,C.POINTER(C.c_int32),C.c_uint32],C.c_int)
close=bind('CloseFile',[C.c_void_p],C.c_void_p)
opendec=bind('OpenRawDecoder',[C.c_void_p,C.c_int32,C.c_void_p,C.c_int32,C.c_int16,C.c_char_p,C.c_int,C.c_int],C.c_void_p)
unpack=bind('UnpackSamples',[C.c_void_p,C.POINTER(C.c_int32),C.c_uint32],C.c_uint32)
errors=bind('GetNumErrors',[C.c_void_p],C.c_int)
bits=bind('GetBitsPerSample',[C.c_void_p],C.c_int)

def decode(blob,n):
    storage=C.create_string_buffer(blob)
    error=C.create_string_buffer(256)
    ctx=opendec(storage,len(blob),None,0,0,error,0,0)
    assert ctx,error.value
    try:
        assert bits(ctx)==24
        pcm=(C.c_int32*(n*2))()
        count=unpack(ctx,pcm,n)
        assert count==n,(count,n,error.value)
        assert errors(ctx)==0
        return bytes(pcm)
    finally: close(ctx)

def encode(data,samples,rate,fast,lossless=False):
    blocks=[]
    @callback
    def write(_,buf,count):
        blocks.append(C.string_at(buf,count)); return 1
    ctx=openenc(write,None,None); assert ctx
    cfg=Config(); cfg.bitrate=rate; cfg.bits_per_sample=24; cfg.bytes_per_sample=3
    cfg.num_channels=2; cfg.channel_mask=3; cfg.sample_rate=48000; cfg.block_samples=samples
    cfg.flags=(0 if lossless else 8|0x2000)|(0x200 if fast else 0)
    try:
        assert configure(ctx,C.byref(cfg),-1,None) and init(ctx)
        for frame in data:
            pcm=(C.c_int32*(2*samples))(*frame)
            prior=len(blocks)
            assert pack(ctx,pcm,samples)
            assert len(blocks)==prior+1,(len(blocks),prior)
        return blocks
    finally: close(ctx)

results=[]
for samples in [60,120,240]:
    rng=random.Random(48125340)
    frames=[]
    for seq in range(400):
        phase=(seq//25)%4
        frame=[]
        for i in range(samples):
            for ch in range(2):
                tone=round(3072000*math.sin(2*math.pi*(1700 if ch else 1000)*(seq*samples+i)/48000))
                noise=rng.randrange(-4194304,4194304)
                frame.append(tone if phase==0 else noise if phase==1 else 0 if phase==2 else noise if (seq+i)%7==0 else tone)
        frames.append(frame)
    for fast in [False,True]:
        for rate in [256,320,384]:
            blocks=encode(frames,samples,rate,fast)
            baseline=decode(b''.join(blocks),samples*len(blocks))
            # Every block must reconstruct the same PCM in a fresh decoder.
            for i,block in enumerate(blocks):
                assert decode(block,samples)==baseline[i*samples*8:(i+1)*samples*8],(samples,fast,rate,i)
            masks={'isolated_1pct':{i for i in range(400) if i%100==17},
                   'burst4':{i for i in range(400) if 80<=i%200<84},
                   'burst20':set(range(171,191))}
            loss=[]
            for label,dropped in masks.items():
                # A persistent decoder reads the surviving stream with gaps.
                kept=[i for i in range(400) if i not in dropped]
                decoded=decode(b''.join(blocks[i] for i in kept),samples*len(kept))
                expected=b''.join(baseline[i*samples*8:(i+1)*samples*8] for i in kept)
                assert decoded==expected,(label,samples,fast,rate)
                loss.append({'pattern':label,'dropped':len(dropped),'following_blocks_match':True})
            row={'samples':samples,'frame_ms':samples/48,'fast':fast,'target_kbps':rate,
                 'frames':len(blocks),'payload_kbps':sum(map(len,blocks))*8*48/(samples*len(blocks)),
                 'min_bytes':min(map(len,blocks)),'max_bytes':max(map(len,blocks)),
                 'loss_tests':loss,'independent_blocks_match':True,
                 'stream_sha256':hashlib.sha256(b''.join(blocks)).hexdigest()}
            results.append(row)
            (out/f'{samples}-{rate}-{int(fast)}.wps').write_bytes(b''.join(blocks))
            print(json.dumps(row),flush=True)
# Nonzero samples that would disappear if input were truncated to 16 bits.
low=[[round(24*math.sin(2*math.pi*997*(f*120+i)/48000)) for i in range(120) for ch in range(2)] for f in range(20)]
blob=b''.join(encode(low,120,320,True,True))
decoded=decode(blob,2400)
expected=bytes((C.c_int32*4800)(*(x for f in low for x in f)))
assert decoded==expected and any(decoded)
hybrid=decode(b''.join(encode(low,120,320,True,False)),2400)
original=[x for frame in low for x in frame]
decoded_low=list((C.c_int32*4800).from_buffer_copy(hybrid))
low_metrics={'max_input':max(map(abs,original)), 'max_decoded':max(map(abs,decoded_low)),
             'nonzero_decoded':sum(x!=0 for x in decoded_low),
             'max_abs_error':max(abs(x-y) for x,y in zip(original,decoded_low))}
assert low_metrics['nonzero_decoded']>0 and low_metrics['max_abs_error']<10
(out/'validation.json').write_text(json.dumps({'cases':results,'low_level_24bit_lossless_roundtrip':True,
    'low_level_24bit_hybrid':low_metrics},indent=2)+'\n')
print('PASS: independent block decode, isolated/burst loss recovery, and low-level 24-bit preservation.')
