"""Archive a specific custom-codec image, sources and paired hardware results."""
import argparse, hashlib, json, re, shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument('--tx',type=Path,required=True);p.add_argument('--rx',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--host',type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
def records(path,prefix):
    return [dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',l))
        for l in path.read_text(errors='replace').splitlines() if l.startswith(prefix+' ')]
tx=records(a.tx,'RESULT'); profiles=records(a.tx,'PROFILE')
ids={r['id'] for r in tx if r['mode']==3}
rx=[r for r in records(a.rx,'RX_RESULT') if r['run'] in ids]
assert len(tx)==len(profiles) and len(rx)==len(ids) and tx
assert len({r['id'] for r in tx})==len(tx)
for r in tx:
    assert all(r[k]==0 for k in ('codec_errors','send_errors','ack_fail','skipped'))
    assert r['payload_max']<=204 and r['fragmented_frames']==0
    if r['mode']==3: assert r['radio_completed']==r['frames']
for r in rx: assert r['good']==r['expected'] and r['missing']==r['bad']==r['overflow']==0
(a.output/'hardware.json').write_text(json.dumps(dict(results=tx,profiles=profiles,receiver=rx),indent=2)+'\n')
for path,name in ((a.tx,'tx.log'),(a.rx,'rx.log')): shutil.copyfile(path,a.output/name)
b=ROOT/'.bench/build-tx-google-custom-lto'
for name in ('zephyr.elf','zephyr.hex','.config'): shutil.copyfile(b/'benchmark/zephyr'/name,a.output/('tx-'+name))
shutil.copyfile(b/'merged.hex',a.output/'tx-merged.hex')
for unit in ('lc3.c','spec.c','sns.c','sns.h'):
    shutil.copyfile(b/'benchmark/lc3-custom'/unit,a.output/('generated-'+unit))
sources=[*list((ROOT/'firmware/benchmark/src').glob('*')),
    ROOT/'firmware/benchmark/CMakeLists.txt',*list((ROOT/'tools').glob('*lc3*')),
    ROOT/'tools/audio-bench.ps1']
for f in sources:
    if not f.is_file():continue
    dest=a.output/'source'/f.relative_to(ROOT);dest.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(f,dest)
if a.host:
    shutil.copyfile(a.host/'host-validation.json',a.output/'host-validation.json')
lines=['| SNS | Rate mode | TNS | Target kb/s | Mode | API avg/max us | Work avg/max us | Late | ACK late | Actual kb/s | Max bytes |',
       '|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
for r in tx:
    rate=r['payload_bytes']*8/(r['frames']*.0025)/1000
    lines.append(f"| {r['sns_mode']} | {r['rate_mode']} | {r['tns']} | {r['bitrate']//1000} | {r['mode']} | {r['api_avg_us']}/{r['api_max_us']} | {r['work_avg_us']}/{r['work_max_us']} | {r['late']} | {r.get('radio_late',0)} | {rate:.3f} | {r['payload_max']} |")
(a.output/'tables.md').write_text('\n'.join(lines)+'\n')
manifest={str(f.relative_to(a.output)):hashlib.sha256(f.read_bytes()).hexdigest()
    for f in a.output.rglob('*') if f.is_file() and f.name!='sha256.json'}
(a.output/'sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Encodes:',sum(r['frames'] for r in tx),'RF frames:',sum(r['good'] for r in rx),
    'Retries:',sum(r['retries'] for r in tx),'RF cases without late releases:',sum(r['late']==0 for r in tx if r['mode']==3))
print('Cases with invalid timing samples:',[r['id'] for r in tx if r['timing_errors']])
