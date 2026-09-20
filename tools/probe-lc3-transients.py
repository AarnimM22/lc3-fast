"""Diagnostic ablations of existing codec options; no firmware/codecs are changed."""
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import wave
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'firmware/listening/lc3-fast-2026-09-20'
OUT = ROOT / 'firmware/listening/lc3-fast-transient-diagnosis'
N, FS, SCALE = 120, 48000, 8388608


def read_pcm(path):
    with wave.open(str(path), 'rb') as f:
        assert (f.getnchannels(), f.getsampwidth(), f.getframerate()) == (2, 3, FS)
        raw = np.frombuffer(f.readframes(FS * 8 + 2 * N), np.uint8).reshape(-1, 3).astype(np.int32)
    q = raw[:, 0] | raw[:, 1] << 8 | raw[:, 2] << 16
    return ((q ^ 0x800000) - 0x800000).reshape(-1, 2)


def write_pcm(path, values):
    q = np.rint(values * SCALE).astype(np.int32).reshape(-1)
    assert q.min() > -SCALE and q.max() < SCALE
    b = np.column_stack((q & 255, q >> 8 & 255, q >> 16 & 255)).astype(np.uint8)
    with wave.open(str(path), 'wb') as f:
        f.setnchannels(2); f.setsampwidth(3); f.setframerate(FS); f.writeframes(b.tobytes())


def db_energy(x):
    return float(10 * np.log10(max(float(np.mean(x ** 2)), 1e-30)))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    dll = SOURCE / 'reproducibility/lc3-custom.dll'
    lib = C.CDLL(str(dll))
    for kind in ('encoder', 'decoder'):
        f = getattr(lib, f'lc3_{kind}_size'); f.argtypes = [C.c_int] * 2; f.restype = C.c_uint
        f = getattr(lib, f'lc3_setup_{kind}'); f.argtypes = [C.c_int] * 3 + [C.c_void_p]; f.restype = C.c_void_p
    lib.lc3_custom_configure.argtypes = [C.c_uint] * 3
    lib.lc3_custom_configure.restype = None
    lib.lc3_custom_encode.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_int]
    lib.lc3_custom_encode.restype = C.c_int
    lib.lc3_custom_decode.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_int]
    lib.lc3_custom_decode.restype = C.c_int
    lib.lc3_delay_samples.argtypes = [C.c_int] * 2
    lib.lc3_delay_samples.restype = C.c_int
    delay = lib.lc3_delay_samples(2500, FS)

    def state(kind):
        mem = C.create_string_buffer(getattr(lib, f'lc3_{kind}_size')(2500, FS))
        ptr = getattr(lib, f'lc3_setup_{kind}')(2500, FS, FS, mem)
        assert ptr
        return mem, ptr

    x = read_pcm(SOURCE / '01-bee-moved/reference-48k24.wav')
    reference = x[:8 * FS].astype(np.float64) / SCALE
    # Heuristic landmarks, selected from reference only, not codec error.
    diff = np.diff(reference[:3 * FS], axis=0, prepend=reference[:1])
    energy = np.mean(diff.reshape(-1, 12, 2) ** 2, axis=(1, 2))
    candidates = []
    for i in range(8, len(energy)):
        ratio = energy[i] / max(float(np.mean(energy[i - 8:i])), 1e-12)
        if ratio > 4 and energy[i] > 1e-5:
            candidates.append((ratio, i * 12))
    events = []
    for ratio, pos in sorted(candidates, reverse=True):
        if all(abs(pos - other) >= 960 for other in events):
            events.append(pos)
    events.sort()
    assert events
    write_pcm(OUT / 'reference-8s.wav', reference)
    configs = [
        ('320-current', 320000, (1, 2, 0)),
        ('400-current', 400000, (1, 2, 0)),
        ('320-tns-on', 320000, (1, 2, 1)),
        ('320-search-gain', 320000, (1, 1, 0)),
        ('320-original-envelope-scalar', 320000, (3, 2, 0)),
        ('320-cbr-gain-refinement', 320000, (1, 0, 0)),
    ]
    rows = []
    for label, rate, config in configs:
        lib.lc3_custom_configure(*config)
        enc, dec = [state('encoder') for _ in range(2)], [state('decoder') for _ in range(2)]
        y = np.empty(x.shape, np.float32)
        out, pcm = C.create_string_buffer(102), np.empty(N, np.float32)
        first3_caps, sizes = 0, []
        for seq, frame in enumerate(x.reshape(-1, N, 2)):
            total = 0
            for ch in range(2):
                block = seq + ch
                target = (block + 1) * rate // 6400 - block * rate // 6400
                data = np.ascontiguousarray(frame[:, ch], dtype=np.int32)
                length = lib.lc3_custom_encode(enc[ch][1], 1, data.ctypes.data, 1, target, out, 102)
                assert 22 <= length <= 102
                total += length
                assert lib.lc3_custom_decode(dec[ch][1], out, length, 3, pcm.ctypes.data, 1) == 0
                y[seq * N:(seq + 1) * N, ch] = pcm
            sizes.append(total)
            if seq == 1199:
                first3_caps = C.c_uint.in_dll(lib, 'custom_cap_frames').value
        y = y[delay:delay + len(reference)].astype(np.float64)
        assert y.shape == reference.shape and np.isfinite(y).all()
        err = y - reference
        windows = np.concatenate([err[p - 24:p] for p in events])
        event_error = [db_energy(err[p - 24:p]) for p in events]
        row = dict(label=label, rate=rate, sns=config[0], rate_mode=config[1], tns=config[2],
                   first3_actual_kbps=sum(sizes[:1200]) * 8 / 3000,
                   first3_capped_channel_frames=first3_caps, first3_channel_frames=2400,
                   first3_snr_db=db_energy(reference[:3 * FS]) - db_energy(err[:3 * FS]),
                   first3_error_rms_dbfs=db_energy(err[:3 * FS]),
                   heuristic_pre_onset_error_dbfs=db_energy(windows),
                   per_event_pre_onset_error_dbfs=event_error,
                   eight_second_actual_kbps=sum(sizes[:3200]) / 1000,
                   filename=label + '.wav', decoder_errors=0)
        write_pcm(OUT / row['filename'], y)
        rows.append(row)
        print(json.dumps({k: v for k, v in row.items() if k != 'per_event_pre_onset_error_dbfs'}), flush=True)
    result = dict(
        purpose='Diagnostic ablations, not an assertion of perceptual improvement or a firmware timing test.',
        input='Existing reference-48k24.wav including its final <=1.5 LSB dither. All variants receive identical input.',
        output_quantization='PCM24 round-to-nearest without added dither; diagnostic files only.',
        dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest(), delay_removed_samples=delay,
        event_selection='Reference first-difference energy in 0.25ms bins, >4x preceding 2ms mean and >-50 dBFS; strongest candidate within each 20ms neighborhood.',
        event_times_seconds=[p / FS for p in events],
        event_metric_limit='0.5ms immediately before a heuristic energy rise, not guaranteed silence or an exact attack boundary; not a perceptual/pre-echo score.',
        abx_one_sided_chance_tail={str(k): sum(math.comb(16, i) for i in range(k, 17)) / 2**16 for k in (16, 13, 10)},
        variants=rows)
    (OUT / 'diagnosis.json').write_text(json.dumps(result, indent=2) + '\n')
    (OUT / 'comparisons.m3u8').write_text('#EXTM3U\nreference-8s.wav\n' + '\n'.join(row['filename'] for row in rows) + '\n')


if __name__ == '__main__':
    main()
