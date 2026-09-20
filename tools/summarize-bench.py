"""Join transmitter timing and independent receiver checks; emit JSON/Markdown."""
import argparse
import json
import re
from pathlib import Path

def rows(path, prefix):
    result = []
    for line in path.read_text(encoding='utf-8-sig', errors='replace').splitlines():
        if line.startswith(prefix+' '):
            result.append({k: int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',line)})
    return result

def summarize(tx, rx):
    receivers = {r['run']:r for r in rows(rx,'RX_RESULT')}
    results = []
    for r in rows(tx,'RESULT'):
        period_us = r.get('period_num_us', r['frame_us']) / r.get('period_den', 1)
        r['encoder_load_percent'] = round(100*r['enc_avg_us']/period_us,2)
        r['work_load_percent'] = round(100*r['work_avg_us']/period_us,2)
        payload_bits = (r['payload_bytes'] * 8 if 'payload_bytes' in r else
                        r['frames']*period_us*r['bitrate']/1000000)
        r['effective_audio_kbps'] = round(payload_bits*1000/r['elapsed_us'],3)
        r['encoded_payload_kbps'] = round(payload_bits*1000/(r['frames']*period_us),3)
        if r['mode']:
            r['receiver'] = receivers.get(r['id'])
            received = r['receiver']
            r['bench_pass'] = bool(received and received['good']==r['frames'] and
                received['bad']==0 and received['overflow']==0 and
                all(r[k]==0 for k in ('codec_errors','send_errors','late','skipped')))
        else:
            r['bench_pass'] = r['codec_errors']==0 and r['enc_max_us']<period_us
        if r.get('timing_errors', 0):
            r['bench_pass'] = False
        if r['mode'] == 3:
            # Source cadence and delivery latency are different deadlines.
            r['producer_cadence_pass'] = (r['late'] == r['skipped'] ==
                r['codec_errors'] == r.get('timing_errors', 0) == 0)
            r['delivery_budget_pass'] = (r.get('radio_late', 1) == 0 and
                r['send_errors'] == r['ack_fail'] == 0 and
                r.get('radio_completed') == r.get('data_packets',r['frames']))
            r['bench_pass'] = r['bench_pass'] and r['delivery_budget_pass']
        results.append(r)
    return results

if __name__ == '__main__':
    p=argparse.ArgumentParser()
    p.add_argument('tx',type=Path)
    p.add_argument('rx',type=Path)
    p.add_argument('output',type=Path)
    a=p.parse_args()
    result=summarize(a.tx,a.rx)
    a.output.write_text(json.dumps(result,indent=2)+'\n')
    print('| Mode | Frame ms | kb/s | Encode avg/max ms | Work avg/max ms | Late/skipped | RX good |')
    print('|---|---:|---:|---:|---:|---:|---:|')
    for r in result:
        rx=r.get('receiver') or {}
        print(f"| {r['mode']} | {r['frame_us']/1000:g} | {r['bitrate']/1000:g} | "
              f"{r['enc_avg_us']/1000:.3f}/{r['enc_max_us']/1000:.3f} | "
              f"{r['work_avg_us']/1000:.3f}/{r['work_max_us']/1000:.3f} | "
              f"{r['late']}/{r['skipped']} | {rx.get('good','—')} |")
