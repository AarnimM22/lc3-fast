"""Host decoder compatibility, signal-error probes and loss recovery checks.

These synthetic probes are not perceptual transparency or ETSI conformance tests.
All variants feed a separately built, unmodified Google decoder.
"""
import argparse
import ctypes as C
import json
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FLAGS = (0, 1, 2, 3, 4, 5, 7)
N, FS, FRAMES = 120, 48000, 240

def library(name):
    lib = C.CDLL(str(ROOT / '.bench/lc3-lite-host' / name))
    for kind in ('encoder', 'decoder'):
        f = getattr(lib, f'lc3_{kind}_size')
        f.argtypes, f.restype = [C.c_int, C.c_int], C.c_uint
        f = getattr(lib, f'lc3_setup_{kind}')
        f.argtypes, f.restype = [C.c_int]*3 + [C.c_void_p], C.c_void_p
    lib.lc3_encoder_disable_ltpf.argtypes = [C.c_void_p]
    lib.lc3_encoder_disable_ltpf.restype = None
    lib.lc3_encode.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.c_int, C.c_int, C.c_void_p]
    lib.lc3_decode.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_int]
    lib.lc3_encode.restype = lib.lc3_decode.restype = C.c_int
    lib.lc3_delay_samples.argtypes = [C.c_int, C.c_int]
    lib.lc3_delay_samples.restype = C.c_int
    return lib

base = library('lc3-baseline.dll')
lite = library('lc3-lite.dll')
lite.lc3_lite_set_flags.argtypes = [C.c_uint]
lite.lc3_lite_set_flags.restype = None
DELAY = base.lc3_delay_samples(2500, FS)

def state(lib, kind):
    mem = C.create_string_buffer(getattr(lib, f'lc3_{kind}_size')(2500, FS))
    ptr = getattr(lib, f'lc3_setup_{kind}')(2500, FS, FS, mem)
    assert ptr
    return mem, ptr

def encode(x, bitrate, flags, lib=lite):
    if lib is lite:
        lite.lc3_lite_set_flags(flags)
    states = [state(lib, 'encoder') for _ in range(2)]
    if flags & 1:
        for _, ptr in states:
            lib.lc3_encoder_disable_ltpf(ptr)
    nbytes = bitrate // 6400
    packets = []
    for frame in x.reshape(-1, N, 2):
        pair = []
        for ch in range(2):
            pcm = np.ascontiguousarray(frame[:, ch], dtype=np.int32)
            out = C.create_string_buffer(nbytes)
            assert lib.lc3_encode(states[ch][1], 1, pcm.ctypes.data, 1, nbytes, out) == 0
            pair.append(out.raw)
        packets.append(pair)
    return packets

def decode(packets, drops=()):
    states = [state(base, 'decoder') for _ in range(2)]
    output = np.empty((len(packets)*N, 2), dtype=np.float32)
    for seq, pair in enumerate(packets):
        for ch, data in enumerate(pair):
            pcm = np.empty(N, dtype=np.float32)
            lost = seq in drops
            packet = None if lost else C.create_string_buffer(data)
            rc = base.lc3_decode(states[ch][1], packet, len(data), 3, pcm.ctypes.data, 1)
            assert rc == int(lost), (seq, ch, lost, rc)
            assert np.isfinite(pcm).all()
            output[seq*N:(seq+1)*N, ch] = pcm
    return output.astype(np.float64)

def signals():
    t = np.arange(FRAMES*N) / FS
    rng = np.random.default_rng(48125340)
    src = {
        'tones': np.column_stack((.55*np.sin(2*np.pi*997*t), .5*np.sin(2*np.pi*1703*t))),
        'multitone': np.column_stack([sum(.085*np.sin(2*np.pi*f*t + ch*.23)
            for f in (173, 997, 3001, 6011, 11003, 17011)) for ch in range(2)]),
        'chirp': np.column_stack([.55*np.sin(2*np.pi*(90*t + (15000+ch*1000)*t*t)) for ch in range(2)]),
        'quiet_24bit': np.column_stack((2e-5*np.sin(2*np.pi*997*t), 3e-5*np.sin(2*np.pi*1703*t))),
        'noise': rng.normal(0, .16, (len(t), 2)),
    }
    percussion = np.zeros((len(t), 2))
    clicks = np.zeros_like(percussion)
    for onset in (2400, 4813, 7957, 12719, 16983, 23337):
        length = min(2800, len(t)-onset)
        env = np.exp(-np.arange(length)/350)
        percussion[onset:onset+length] += env[:, None]*rng.normal(0, .22, (length, 2))
        clicks[onset] = (.8, -.7)
    src['percussion'], src['clicks'] = percussion, clicks
    # Harmonics with changing envelope/pitch, not a substitute for speech/music.
    phase = 2*np.pi*(180*t + 14*np.sin(2*np.pi*2*t))
    env = .35*(.5+.5*np.sin(2*np.pi*4*t))
    src['harmonics'] = np.column_stack([env*sum(np.sin(k*phase+ch*.17)/k
        for k in range(1, 20)) for ch in range(2)])
    for name, x in src.items():
        yield name, np.rint(np.clip(x, -.95, .95)*8388608).astype(np.int32)

def db(value):
    return float(10*np.log10(max(float(value), 1e-30)))

def quality(x, y):
    # Align using the library's documented algorithmic delay; omit startup.
    ref = x[2*N:len(x)-DELAY].astype(np.float64)/8388608
    reconstructed = y[2*N+DELAY:]
    error = reconstructed-ref
    return {'snr_db': db(np.mean(ref*ref)/max(np.mean(error*error), 1e-30)),
            'error_rms_dbfs': db(np.mean(error*error)),
            'max_abs_error': float(np.max(np.abs(error)))}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    rows, losses = [], []
    exact = total = 0
    for name, x in signals():
        for bitrate in (256000, 320000):
            reference_packets = encode(x, bitrate, 0, base)
            reference = decode(reference_packets)
            for flags in FLAGS:
                packets = encode(x, bitrate, flags)
                y = decode(packets)
                if flags == 0:
                    exact += sum(a == b for pair_a, pair_b in zip(packets, reference_packets)
                                 for a, b in zip(pair_a, pair_b))
                    total += len(packets)*2
                rows.append({'signal': name, 'bitrate': bitrate, 'flags': flags,
                    **quality(x, y), 'delta_from_baseline_rms_dbfs': db(np.mean((y-reference)**2))})
                patterns = {'isolated': {60}, 'burst_4': set(range(60,64)),
                    'burst_20': set(range(60,80)), 'periodic_1pct': set(range(40, FRAMES-40,100)),
                    'periodic_5pct': set(range(40, FRAMES-40,20))}
                for pattern, drops in patterns.items():
                    z = decode(packets, drops)
                    error_by_frame = np.max(np.abs(z-y).reshape(-1, N, 2), axis=(1,2))
                    last_drop = max(drops)
                    affected = np.flatnonzero(error_by_frame[last_drop+1:] > 1e-6)
                    losses.append({'signal':name, 'bitrate':bitrate, 'flags':flags,
                        'pattern':pattern, 'lost_frames':len(drops),
                        'affected_good_frames_after_last_loss': int(affected[-1]+1) if len(affected) else 0,
                        'final_20_frames_max_difference': float(np.max(np.abs(z[-20*N:]-y[-20*N:]))),
                        'loss_window_peak_difference': float(np.max(np.abs(z-y)))})
            print(f'PASS {name} {bitrate}: 7 variants, 35 loss scenarios', flush=True)
    assert exact == total, ('flags-zero must match unmodified encoder', exact, total)
    assert all(r['final_20_frames_max_difference'] < 1e-6 for r in losses)
    result = {'settings':{'sample_rate':FS,'channels':2,'pcm_bits':24,'frame_us':2500,
        'frames_per_signal':FRAMES,'delay_samples':DELAY,'flags':list(FLAGS)},
        'baseline_identical_channel_packets':exact,'quality':rows,'loss_tests':losses,
        'limitations':'Synthetic signal errors are diagnostic, not perceptual scores. Host MSVC is not ARM bit-exact validation.'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(f'PASS baseline {exact}/{total} identical channel packets; {len(rows)} signal probes; {len(losses)} loss scenarios')
    print(f'Delay: {DELAY} samples. Results: {args.output}')

if __name__ == '__main__':
    main()
