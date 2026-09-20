"""Retain USB benchmark images, source snapshots, logs and parsed failures too."""
import argparse, hashlib, json, re, shutil, wave
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument('--output',type=Path,required=True)
p.add_argument('--tx',type=Path,required=True)
p.add_argument('--rx',type=Path,nargs='+',required=True)
p.add_argument('--host',type=Path,nargs='+',required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
def read(path):
    raw=path.read_bytes()
    return raw.decode('utf-16' if raw.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig',errors='replace')
def rows(text,prefix):
    result=[]
    for line in text.splitlines():
        if not line.startswith(prefix+' '):continue
        r={k:int(v,16 if k in ('pcm_crc','pcm_hash','packet_hash') else 10) for k,v in re.findall(r'(\w+)=([0-9a-f]+)(?= |$)',line)}
        result.append(r)
    return result
tx=read(a.tx);rx=''.join(read(f) for f in a.rx);host=''.join(read(f) for f in a.host)
for name,content in (('tx.log',tx),('rx.log',rx),('host.log',host)):
    (a.output/name).write_text(content,encoding='utf-8')
data={key:rows(content,prefix) for key,content,prefix in (
    ('results',tx,'USB_RESULT'),('input',tx,'USB_INPUT'),('profiles',tx,'USB_PROFILE'),('overhead',tx,'USB_OVERHEAD'),
    ('receiver',rx,'RX_RESULT'),('host',host,'PCM_SOURCE'),('payload',tx,'USB_PAYLOAD'))}
ids={r['id'] for r in data['results']}
data['receiver']=[r for r in data['receiver'] if r['run'] in ids]
for r in data['results']:
    source=next((h for h in data['host'] if h['id']==r['id']),None)
    field='pcm_hash' if 'pcm_hash' in r else 'pcm_crc'
    packet=next((s for s in data['payload'] if s['id']==r['id']),{})
    r['pcm_integrity_matches_host']=bool(source and source[field]==r[field]) if packet.get('pcm_hash_enabled',1) else None
    r['actual_payload_bitrate']=r['payload_bytes']*8/(r['frames']*.0025) if r['frames'] else 0
wav=ROOT/'firmware/benchmark/24_48k_PerfectTest.wav'
with wave.open(str(wav)) as w:
    data['wav']=dict(file=wav.name,sha256=hashlib.sha256(wav.read_bytes()).hexdigest(),
        channels=w.getnchannels(),sample_rate=w.getframerate(),bits=8*w.getsampwidth(),frames=w.getnframes())
(a.output/'hardware.json').write_text(json.dumps(data,indent=2)+'\n')
b=ROOT/'.bench/usb-tx'
cache=(b/'CMakeCache.txt').read_text()
mask=re.search(r'^benchmark_BENCH_OPTIMIZATIONS:[^=]+=([0-9]+)$',cache,re.M)
if data['payload'] and mask:
    assert all(r['optimizations']==int(mask[1]) for r in data['payload']), 'Refusing to archive another build variant'
shutil.copyfile(b/'CMakeCache.txt',a.output/'tx-CMakeCache.txt')
for f in ('zephyr.elf','zephyr.hex','.config','zephyr.dts','zephyr.map'):
    shutil.copyfile(b/'benchmark/zephyr'/f,a.output/('tx-'+f))
shutil.copyfile(b/'merged.hex',a.output/'tx-merged.hex')
for unit in ('lc3.c','sns.c','sns.h','spec.c','mdct.c'):
    shutil.copyfile(b/'benchmark/lc3-custom'/unit,a.output/('generated-'+unit))
sources=[*list((ROOT/'firmware/benchmark/src').glob('*')),ROOT/'firmware/benchmark/CMakeLists.txt',
    ROOT/'firmware/benchmark/usb_audio.conf',ROOT/'firmware/benchmark/usb_audio.overlay',
    *list((ROOT/'tools').glob('*usb*')),*list((ROOT/'tools').glob('*lc3*')),
    ROOT/'tools/audio-bench.ps1',ROOT/'tools/rtt-memory.py']
for f in sources:
    if not f.is_file():continue
    dest=a.output/'source'/f.relative_to(ROOT);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,dest)
shutil.copyfile(ROOT/'.bench/usb-pcm-host/usb-pcm-host.exe',a.output/'usb-pcm-host.exe')
manifest={str(f.relative_to(a.output)):hashlib.sha256(f.read_bytes()).hexdigest()
    for f in a.output.rglob('*') if f.is_file() and f.name!='sha256.json'}
(a.output/'sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
for r in data['results']:
    print('Run',r['id'],'frames',r['frames'],'/',r['expected'],'PCM integrity match',r['pcm_integrity_matches_host'],
        'work avg/max',r['work_avg_us'],r['work_max_us'],'ACK late',r['radio_late'])
