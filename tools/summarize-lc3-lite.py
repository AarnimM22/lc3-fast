"""Validate and archive the LC3-lite hardware sweep and diagnostic host probes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--tx', type=Path, required=True)
p.add_argument('--rx', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)

def parse(path, prefix):
    return [{k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
            for line in path.read_text(errors='replace').splitlines() if line.startswith(prefix+' ')]

results, profiles, rx = parse(a.tx, 'RESULT'), parse(a.tx, 'PROFILE'), parse(a.rx, 'RX_RESULT')
rx = [r for r in rx if 301 <= r['run'] <= 328]
assert len(results) == len(profiles) == 28 and len(rx) == 14
assert {r['id'] for r in results} == set(range(301, 329))
assert all(r['frames'] == 1000 for r in results)
assert all(not r[k] for r in results for k in ('codec_errors','send_errors','ack_fail','timing_errors','skipped'))
assert all(not r[k] for r in rx for k in ('missing','bad','overflow'))
assert sum(r['good'] for r in rx) == 14000
assert all(r['radio_completed'] == 1000 for r in results if r['mode'] == 3)
assert all(r['late'] == r['frames'] for r in results if r['mode'] == 3)
assert {r['run'] for r in rx} == {r['id'] for r in results if r['mode'] == 3}
for r in results:
    assert r['payload_min'] == r['payload_max'] == r['bitrate']//3200
    assert r['payload_bytes'] == r['frames']*r['payload_min']

(a.output/'hardware-results.json').write_text(json.dumps({'results':results,'profiles':profiles,'receiver':rx},indent=2)+'\n')
shutil.copyfile(a.tx, a.output/'tx.log')
shutil.copyfile(a.rx, a.output/'rx.log')
build = ROOT/'.bench/build-tx-google-lite-lto'
for name in ('zephyr.elf','zephyr.hex','.config'):
    shutil.copyfile(build/'benchmark/zephyr'/name, a.output/('tx-'+name))
shutil.copyfile(build/'merged.hex', a.output/'tx-merged.hex')
for unit in ('lc3','tns','spec'):
    shutil.copyfile(build/'benchmark/lc3-lite'/f'{unit}.c',a.output/f'experimental-{unit}.c')

labels = {0:'Baseline',1:'No LTPF',2:'No TNS',3:'No LTPF/TNS',4:'No gain refinement',5:'No LTPF/gain refinement',7:'All three disabled'}
lines = ['# LC3-lite sweep','', '| Variant | kb/s | Local API avg/max, us | Local wrapper P99, us | Concurrent RF work avg/max, us | Late/1000 |',
         '|---|---:|---:|---:|---:|---:|']
for flags,label in labels.items():
    for rate in (256000,320000):
        local=next(r for r in results if r['lite_flags']==flags and r['bitrate']==rate and r['mode']==0)
        rf=next(r for r in results if r['lite_flags']==flags and r['bitrate']==rate and r['mode']==3)
        lines.append(f"| {label} | {rate//1000} | {local['api_avg_us']}/{local['api_max_us']} | {local['enc_p99_us']} | {rf['work_avg_us']}/{rf['work_max_us']} | {rf['late']} |")
host=json.loads((a.output/'host-validation.json').read_text())
lines += ['', '## Synthetic waveform SNR (diagnostic only)', '',
          'Delay aligned; not a perceptual score, listening result or effective-bit-depth estimate.', '',
          '| Signal | kb/s | Baseline | No LTPF | No TNS | No gain refinement | All three disabled |',
          '|---|---:|---:|---:|---:|---:|---:|']
for signal in dict.fromkeys(r['signal'] for r in host['quality']):
    for rate in (256000,320000):
        vals=[next(r['snr_db'] for r in host['quality'] if r['signal']==signal and r['bitrate']==rate and r['flags']==flags) for flags in (0,1,2,4,7)]
        lines.append(f'| {signal} | {rate//1000} | '+' | '.join(f'{v:.2f}' for v in vals)+' |')
(a.output/'tables.md').write_text('\n'.join(lines)+'\n')

sources=['firmware/benchmark/CMakeLists.txt','firmware/benchmark/src/main.c',
    'firmware/benchmark/src/codec.c','firmware/benchmark/src/lc3_lite.h',
    'firmware/benchmark/src/lc3_lite.c','firmware/benchmark/src/radio_async.c',
    'firmware/benchmark/src/radio.c','firmware/benchmark/src/bench.h',
    'tools/audio-bench.ps1','tools/prepare-lc3-lite.py','tools/build-lc3-lite-host.ps1',
    'tools/test-lc3-lite.py','tools/summarize-lc3-lite.py']
for name in sources:
    dest=a.output/'source'/name
    dest.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(ROOT/name,dest)
manifest={str(f.relative_to(a.output)):hashlib.sha256(f.read_bytes()).hexdigest()
          for f in a.output.rglob('*') if f.is_file() and f.name!='sha256.json'}
(a.output/'sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(f'Validated {sum(r["frames"] for r in results)} encodes, {sum(r["good"] for r in rx)} RF frames; all 14 RF cases missed cadence.')
print(f'RF retries: {sum(r["retries"] for r in results)}. Archive: {a.output}')
