"""Cross-check retained USB measurements, including their deadline failures."""
import argparse, hashlib, json, shutil, struct, wave, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True)
a=p.parse_args();final=a.root/'final'
data=json.loads((final/'hardware.json').read_text())
assert {r['id'] for r in data['results']}=={911,912}
with wave.open(str(ROOT/'firmware/benchmark/24_48k_PerfectTest.wav')) as w:
    pcm=w.readframes(w.getnframes())
n=20000*720
stream=(pcm*((n+len(pcm)-1)//len(pcm)))[:n]
h=14695981039346656037
for (word,) in struct.iter_unpack('<I',stream):
    h=((h^word)*1099511628211)&0xffffffffffffffff
checks=[]
for r in data['results']:
    source=next(s for s in data['host'] if s['id']==r['id'])
    usb=next(s for s in data['input'] if s['id']==r['id'])
    rx=next(s for s in data['receiver'] if s['run']==r['id'])
    assert source['frames']==r['expected']==rx['expected']==20000
    assert source['pcm_hash']==h and source['pcm_crc']==zlib.crc32(stream)
    assert r['frames']==r['radio_completed']==rx['good']
    assert usb['overflows']==rx['missing']==20000-r['frames']
    assert all(r[k]==0 for k in ('codec_errors','send_errors','ack_fail','timing_errors'))
    assert all(usb[k]==0 for k in ('zero_packets','malformed','buffer_fail','disconnects'))
    assert rx['bad']==rx['overflow']==0
    checks.append(dict(id=r['id'],frames=r['frames'],offered=20000,
        lost_input=usb['overflows'],pcm_integrity_pass=r['pcm_hash']==h,
        throughput_pass=r['frames']==20000 and r['pcm_hash']==h,
        production_deadline_pass=r['producer_late']==0 and r['frames']==20000,
        delivery_deadline_pass=r['radio_late']==0 and r['frames']==20000))
assert checks[0]['throughput_pass'] and not checks[1]['throughput_pass']
assert all(not r['delivery_deadline_pass'] for r in checks)
matched=0
for source in (final/'source').rglob('*'):
    if source.is_file():
        original=ROOT/source.relative_to(final/'source')
        assert source.read_bytes()==original.read_bytes(),str(original)
        matched+=1
for name in ('prj.conf','sysbuild.cmake'):
    shutil.copyfile(ROOT/'firmware/benchmark'/name,final/'source/firmware/benchmark'/name)
for name in ('zephyr.elf','zephyr.hex'):
    assert (final/('tx-'+name)).read_bytes()==(ROOT/'.bench/usb-tx/benchmark/zephyr'/name).read_bytes()
shutil.copyfile(ROOT/'.bench/logs/usb-packed-format-validation.log',a.root/'usb-packed-format-validation.log')
summary=dict(expected_pcm_word_hash=f'{h:016x}',expected_pcm_crc32=f'{zlib.crc32(stream):08x}',
    frames_per_case=20000,source_snapshot_files_checked=matched,results=checks,
    note='Validation passed by correctly preserving both the 320 kb/s throughput success and the timing/400 kb/s failures.')
(a.root/'validation.json').write_text(json.dumps(summary,indent=2)+'\n')
for folder in (final,a.root):
    hashes={f.relative_to(folder).as_posix():hashlib.sha256(f.read_bytes()).hexdigest()
            for f in sorted(folder.rglob('*')) if f.is_file() and f!=folder/'sha256.json'}
    (folder/'sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
print(json.dumps(summary,indent=2))
