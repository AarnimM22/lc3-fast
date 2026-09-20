"""Validate measured successes and retained failures without erasing either."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import wave
import zlib

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument('--root',type=Path,default=ROOT/'firmware/results/2026-09-20-usb-optimizations')
a=p.parse_args()
with wave.open(str(ROOT/'firmware/benchmark/24_48k_PerfectTest.wav')) as w:
    pcm=w.readframes(w.getnframes())
expected={}
summary=[]
fingerprints={}
for folder in sorted(a.root.iterdir()):
    if not (folder/'hardware.json').is_file(): continue
    d=json.loads((folder/'hardware.json').read_text())
    assert len(d['results'])==len(d['input'])==len(d['payload'])==2,folder
    for r in d['results']:
        source=next(s for s in d['host'] if s['id']==r['id'])
        usb=next(s for s in d['input'] if s['id']==r['id'])
        rx=next(s for s in d['receiver'] if s['run']==r['id'])
        packet=next(s for s in d['payload'] if s['id']==r['id'])
        n=r['expected']
        if n not in expected:
            count=n*720
            stream=(pcm*((count+len(pcm)-1)//len(pcm)))[:count]
            h=14695981039346656037
            for (word,) in struct.iter_unpack('<I',stream):
                h=((h^word)*1099511628211)&0xffffffffffffffff
            expected[n]=(h,zlib.crc32(stream))
        assert source['frames']==rx['expected']==n
        assert source['pcm_hash']==expected[n][0] and source['pcm_crc']==expected[n][1]
        assert r['frames']==r['radio_completed']==rx['good']
        assert usb['overflows']==rx['missing']==n-r['frames']
        assert all(r[k]==0 for k in ('codec_errors','send_errors','ack_fail'))
        assert all(usb[k]==0 for k in ('zero_packets','malformed','buffer_fail','disconnects'))
        assert rx['bad']==rx['overflow']==0
        integrity=None if not packet['pcm_hash_enabled'] else r['pcm_hash']==expected[n][0]
        assert r['pcm_integrity_matches_host'] is integrity
        complete=r['frames']==n
        if complete:
            assert r['sequence_errors']==0
            key=(n,r['bitrate'])
            old=fingerprints.setdefault(key,packet['packet_hash'])
            assert old==packet['packet_hash'],(folder,r['id'],'ARM encoded stream differs')
        summary.append(dict(experiment=folder.name,id=r['id'],optimizations=packet['optimizations'],
            rate=r['bitrate'],offered=n,received=r['frames'],input_drops=usb['overflows'],
            pcm_integrity=integrity,packet_hash=f"{packet['packet_hash']:016x}",
            work_avg_us=r['work_avg_us'],work_max_us=r['work_max_us'],
            queue_peak=usb['queue_peak'],ack_max_us=r['ready_to_ack_max_us'],
            delivery_deadline_misses=r['radio_late'],timing_crosscheck_errors=r['timing_errors'],
            throughput_pass=complete,delivery_deadline_pass=complete and r['radio_late']==0))
    # The initial archive error is explicitly documented, never silently
    # promoted into valid executable provenance.
    if folder.name!='all-profiled':
        assert (folder/'tx-zephyr.elf').is_file() and (folder/'tx-zephyr.hex').is_file()
    hashes={f.relative_to(folder).as_posix():hashlib.sha256(f.read_bytes()).hexdigest()
        for f in sorted(folder.rglob('*')) if f.is_file() and f.name!='sha256.json'}
    (folder/'sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
equivalence=json.loads((a.root/'host-equivalence.json').read_text())
for name,key in (('baseline.dll','baseline_sha256'),('optimized.dll','candidate_sha256')):
    assert hashlib.sha256((a.root/'equivalence'/name).read_bytes()).hexdigest()==equivalence[key]
if (a.root/'lean-soak/hardware.json').is_file():
    for name in ('tx-zephyr.elf','tx-zephyr.hex','tx-merged.hex'):
        assert (a.root/'lean'/name).read_bytes()==(a.root/'lean-soak'/name).read_bytes()
    assert (a.root/'lean-soak/tx-zephyr.hex').read_bytes()==(ROOT/'.bench/usb-tx/benchmark/zephyr/zephyr.hex').read_bytes()
result=dict(results=summary,host_equivalence=equivalence['identical_channel_packets'],
    limitation='Packet fingerprints are non-cryptographic. No receiver DAC playback, perceptual test or controlled RF interference test.')
(a.root/'validation.json').write_text(json.dumps(result,indent=2)+'\n')
hashes={f.relative_to(a.root).as_posix():hashlib.sha256(f.read_bytes()).hexdigest()
    for f in sorted(a.root.rglob('*')) if f.is_file() and f!=a.root/'sha256.json'}
(a.root/'sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
for row in summary: print(row)
