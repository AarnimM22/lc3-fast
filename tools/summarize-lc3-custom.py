"""Check and summarize archived custom-codec experiments and refresh hashes."""
import argparse
import hashlib
import json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--root', type=Path, required=True)
a = p.parse_args()
root = a.root.resolve()
workspace = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads(path.read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest(folder):
    result = {f.relative_to(folder).as_posix(): digest(f)
              for f in sorted(folder.rglob('*'))
              if f.is_file() and f != folder / 'sha256.json'}
    (folder / 'sha256.json').write_text(json.dumps(result, indent=2) + '\n')


stages = []
for name in ('initial-sweep', 'revised-sweep', 'final-sweep', 'soak'):
    folder = root / name
    data = read(folder / 'hardware.json')
    tx, rx = data['results'], data['receiver']
    radio = {r['id']: r for r in tx if r['mode'] == 3}
    assert len(rx) == len(radio)
    for r in tx:
        assert all(r[k] == 0 for k in ('codec_errors', 'send_errors', 'ack_fail', 'skipped'))
    for r in rx:
        t = radio[r['run']]
        assert r['expected'] == r['good'] == t['frames'] == t['radio_completed']
        assert r['missing'] == r['bad'] == r['overflow'] == 0
    stages.append(dict(stage=name, cases=len(tx),
        encoded_stereo_frames=sum(r['frames'] for r in tx),
        received_stereo_frames=sum(r['good'] for r in rx),
        retries=sum(r['retries'] for r in tx),
        invalid_timing_cases=[r['id'] for r in tx if r['timing_errors']]))
    manifest(folder)

host = read(root / 'final-sweep' / 'host-validation.json')
assert len(host['quality']) == 416 and len(host['loss_tests']) == 1536
assert all(r['final_difference'] < 1e-6 for r in host['loss_tests'])
vectors = [read(root / name / 'arm-vector-validation.json') for name in ('final-sweep', 'soak')]

# Ensure the selected firmware archive still describes the current codec sources.
checked_sources = 0
for archived in (root / 'soak' / 'source').rglob('*'):
    if archived.is_file():
        current = workspace / archived.relative_to(root / 'soak' / 'source')
        assert digest(archived) == digest(current), str(current)
        checked_sources += 1

soak = read(root / 'soak' / 'hardware.json')
selected = []
for r in soak['results']:
    assert r['timing_errors'] == 0
    selected.append(dict(r, actual_payload_bitrate=r['payload_bytes'] * 8 / (r['frames'] * .0025)))

summary = dict(stages=stages,
    encoded_stereo_frames=sum(r['encoded_stereo_frames'] for r in stages),
    received_stereo_frames=sum(r['received_stereo_frames'] for r in stages),
    retries=sum(r['retries'] for r in stages),
    host_signal_probes=len(host['quality']), host_loss_scenarios=len(host['loss_tests']),
    identical_stock_channel_packets=host['baseline_identical_channel_packets'],
    validated_arm_stereo_packets=sum(r['stereo_packets'] for r in vectors),
    validated_arm_channel_packets=sum(r['channel_packets'] for r in vectors),
    current_source_files_matching_soak_archive=checked_sources,
    selected_soak_results=selected,
    limitations=['Synthetic PCM with 25% silence in hardware tests.',
                 'No USB input, receiver audio decoding or DAC playback on hardware.',
                 'Fixed-channel desk RF; no controlled coexistence stress.',
                 'Host waveform and loss tests do not establish perceptual transparency.',
                 'Soak TX log retrieved after execution; per-frame timing cross-checks passed.'])
(root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
manifest(root)
print(json.dumps({k: v for k, v in summary.items()
                  if k not in ('stages', 'selected_soak_results', 'limitations')}, indent=2))
print('Per-stage and overall SHA256 manifests refreshed.')
