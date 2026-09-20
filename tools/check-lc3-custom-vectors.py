"""Decode ARM-produced packets on the matching host decoder, checking framing."""
import argparse, ctypes as C, importlib.util, json, re
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser();p.add_argument('--log',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
spec=importlib.util.spec_from_file_location('custom_test',Path(__file__).with_name('test-lc3-custom.py'))
t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
rows=[]
for line in a.log.read_text(errors='replace').splitlines():
    m=re.fullmatch(r'VECTOR id=(\d+) seq=(\d+) bytes=(\d+) data=([0-9a-f]+)',line)
    if not m:continue
    run,seq,length=map(int,m.groups()[:3]); data=bytes.fromhex(m[4]);assert len(data)==length
    offset=0
    for ch in range(2):
        size=data[offset+1]+2; packet=data[offset:offset+size]
        assert len(packet)==size and 22<=size<=102
        mem,dec=t.state(t.lib,'decoder');pcm=np.empty(t.N,np.float32)
        buf=C.create_string_buffer(packet)
        rc=t.lib.lc3_custom_decode(dec,buf,size,3,pcm.ctypes.data,1)
        assert rc==0 and np.isfinite(pcm).all(),(run,seq,ch,rc)
        offset+=size
    assert offset==length
    rows.append(dict(run=run,sequence=seq,bytes=length))
assert rows
result=dict(stereo_packets=len(rows),channel_packets=2*len(rows),runs=len({r['run'] for r in rows}),vectors=rows,
    limitation='Fresh decoder validates packet syntax; sparse samples do not establish continuous on-board decoding or waveform quality.')
a.output.write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS {len(rows)} ARM stereo packets, {2*len(rows)} channel decodes')
