"""Build aligned, unclipped PCM24 listening examples with the current custom codec.

Dependencies are isolated in .bench/listening-python. No firmware is flashed.
The source configuration is deliberately fixed to the selected firmware settings.
"""
import argparse
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import sys
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.bench/listening-python'))
import numpy as np
import soundfile as sf
import soxr

FS, N, DT, SCALE = 48000, 120, 2500, 8388608
RATES = (400000, 320000, 256000, 224000, 192000, 160000, 140800)
DEFAULT_OUT = ROOT / 'firmware/listening/lc3-fast-2026-09-20'


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def quantize24(x, seed):
    """Round with deterministic, unshaped TPDF dither (one LSB peak per sign)."""
    rng = np.random.default_rng(seed)
    q = np.rint(x * SCALE + rng.random(x.shape) - rng.random(x.shape))
    if q.min() < -SCALE or q.max() >= SCALE:
        raise ValueError('PCM24 clipping; common headroom must be increased')
    return q.astype(np.int32)


def save24(path, q):
    flat = q.reshape(-1)
    packed = np.column_stack((flat & 255, (flat >> 8) & 255, (flat >> 16) & 255)).astype(np.uint8)
    with wave.open(str(path), 'wb') as f:
        f.setnchannels(2)
        f.setsampwidth(3)
        f.setframerate(FS)
        f.writeframes(packed.tobytes())
    # Verify the file actually contains the intended samples, not just a 24-bit header.
    readback, sr = sf.read(path, dtype='int32', always_2d=True)
    assert sr == FS and np.array_equal(readback >> 8, q)
    assert sf.info(path).subtype == 'PCM_24'


class Codec:
    def __init__(self, path):
        self.lib = lib = C.CDLL(str(path.resolve()))
        for kind in ('encoder', 'decoder'):
            f = getattr(lib, f'lc3_{kind}_size')
            f.argtypes, f.restype = [C.c_int] * 2, C.c_uint
            f = getattr(lib, f'lc3_setup_{kind}')
            f.argtypes, f.restype = [C.c_int] * 3 + [C.c_void_p], C.c_void_p
        lib.lc3_custom_configure.argtypes = [C.c_uint] * 3
        lib.lc3_custom_configure.restype = None
        lib.lc3_custom_encode.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_int]
        lib.lc3_custom_decode.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_int]
        lib.lc3_custom_encode.restype = lib.lc3_custom_decode.restype = C.c_int
        lib.lc3_delay_samples.argtypes = [C.c_int] * 2
        lib.lc3_delay_samples.restype = C.c_int
        self.delay = lib.lc3_delay_samples(DT, FS)

    def state(self, kind):
        mem = C.create_string_buffer(getattr(self.lib, f'lc3_{kind}_size')(DT, FS))
        ptr = getattr(self.lib, f'lc3_setup_{kind}')(DT, FS, FS, mem)
        assert ptr
        return mem, ptr

    def roundtrip(self, x, rate, packet_path=None, measure_start=0, measure_length=None):
        assert rate >= min(RATES) and rate <= 652800 and len(x) % N == 0
        self.lib.lc3_custom_configure(1, 2, 0)  # coarse SNS, analytic VBR, TNS off
        enc = [self.state('encoder') for _ in range(2)]
        dec = [self.state('decoder') for _ in range(2)]
        y = np.empty(x.shape, np.float32)
        out = C.create_string_buffer(102)
        pcm_out = np.empty(N, np.float32)
        sizes = []
        container = bytearray(b'LC3FAST1')
        container += struct.pack('<6I', FS, DT, 2, rate, len(x) // N, 0x00000201)
        # Remaining container records: u8 left length, u8 right length, channel payloads.
        for seq, frame in enumerate(x.reshape(-1, N, 2)):
            pair = []
            for ch in range(2):
                block = seq + ch
                target = (block + 1) * rate // 6400 - block * rate // 6400
                pcm = np.ascontiguousarray(frame[:, ch], dtype=np.int32)
                size = self.lib.lc3_custom_encode(enc[ch][1], 1, pcm.ctypes.data, 1, target, out, 102)
                assert 22 <= size <= 102, (seq, ch, rate, size)
                packet = out.raw[:size]
                pair.append(packet)
                assert self.lib.lc3_custom_decode(dec[ch][1], out, size, 3, pcm_out.ctypes.data, 1) == 0
                y[seq * N:(seq + 1) * N, ch] = pcm_out
            sizes.append(len(pair[0]) + len(pair[1]))
            container += bytes((len(pair[0]), len(pair[1]))) + b''.join(pair)
        assert np.isfinite(y).all()
        if packet_path:
            packet_path.write_bytes(container)
        end = len(x) if measure_length is None else measure_start + measure_length
        lengths = np.array(sizes[measure_start // N:math.ceil(end / N)], dtype=np.int32)
        stats = dict(
            actual_kbps=float(lengths.sum() * 8 / (len(lengths) * .0025) / 1000),
            min_stereo_frame_bytes=int(lengths.min()), max_stereo_frame_bytes=int(lengths.max()),
            encoded_stereo_frames=len(sizes), measured_stereo_frames=len(lengths),
            capped_channel_frames=int(C.c_uint.in_dll(self.lib, 'custom_cap_frames').value),
            clipped_sns_values=int(C.c_uint.in_dll(self.lib, 'custom_sns_clips').value),
            decoder_errors=0, packet_losses=0,
        )
        return y, stats


def metrics(ref, y):
    err = y.astype(np.float64) - ref
    signal, noise = np.mean(ref ** 2), np.mean(err ** 2)
    return dict(waveform_snr_db=float(10 * np.log10(signal / max(noise, 1e-30))),
                error_rms_dbfs=float(10 * np.log10(max(noise, 1e-30))),
                decoded_rms_relative_db=float(10 * np.log10(np.mean(y.astype(np.float64) ** 2) / signal)),
                decoded_peak=float(np.max(np.abs(y))))


def alignment_check(codec):
    x = np.zeros((N * 30, 2), np.int32)
    x[N * 10, :] = SCALE // 4
    y, _ = codec.roundtrip(x, 400000)
    peaks = [int(np.argmax(np.abs(y[:, ch]))) - N * 10 for ch in range(2)]
    assert peaks == [codec.delay] * 2, (codec.delay, peaks)
    return dict(api_delay_samples=codec.delay, measured_impulse_delay_samples=peaks)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=DEFAULT_OUT)
    p.add_argument('--dll', type=Path, default=ROOT / '.bench/lc3-listening-host/lc3-custom.dll')
    p.add_argument('--track', help='Optional single source ID for an initial smoke run')
    a = p.parse_args()
    sources = json.loads((a.output / 'sources/downloads.json').read_text())
    codec = Codec(a.dll)
    result = dict(
        codec=dict(name='custom lc3-fast research format v1', standard_lc3_compatible=False,
                   frame_us=DT, sample_rate=FS, channels=2, pcm_bits=24, sns='coarse',
                   rate_control='analytic VBR', tns=False, ltpf=False, optimizations=31,
                   dll_sha256=sha(a.dll), rates_bps=RATES, **alignment_check(codec)),
        processing=dict(numpy=np.__version__, soundfile=sf.__version__,
                        libsndfile=sf.__libsndfile_version__, soxr=soxr.__version__,
                        resampler='libsoxr VHQ, float64, entire source before excerpt selection',
                        dither='deterministic unshaped TPDF at PCM24 quantization',
                        source_gain_db=-3, level_matching='same post-decode gain for reference and every bitrate'),
        tracks=[],
    )
    for source in sources:
        if a.track and source['id'] != a.track:
            continue
        src_path = a.output / 'sources' / source['audio_filename']
        assert sha(src_path) == source['audio_sha256']
        info = sf.info(src_path)
        assert info.samplerate in (48000, 96000) and info.subtype == 'PCM_24' and info.channels == 2, info
        audio, sr = sf.read(src_path, dtype='float64', always_2d=True)
        lower_byte_nonzero = float(np.mean((np.rint(audio * SCALE).astype(np.int32) & 255) != 0))
        if sr != FS:
            audio = soxr.resample(audio, sr, FS, quality='VHQ')
        # Fixed -3 dB before encoding supplies resampling headroom; never per-rate normalization.
        audio *= 10 ** (-3 / 20)
        x = quantize24(audio, 20260920)
        start = round(source['start_seconds'] * FS)
        length = min(round(source['duration_seconds'] * FS), len(x) - start)
        length = length // N * N
        assert length > FS
        # Include up to one second of real preceding music to initialize codec state.
        history_start = max(0, start - FS)
        offset = start - history_start
        end = start + length
        input_end = min(len(x), end + codec.delay + 2 * N)
        padded_length = math.ceil((offset + length + codec.delay + 2 * N) / N) * N
        stream = np.zeros((padded_length, 2), np.int32)
        stream[:input_end - history_start] = x[history_start:input_end]
        reference = x[start:end].astype(np.float64) / SCALE
        folder = a.output / source['id']
        folder.mkdir(exist_ok=True)
        encoded = folder / 'encoded'
        encoded.mkdir(exist_ok=True)
        track = dict(**source, source_sample_rate=sr, source_subtype=info.subtype,
                     source_duration_seconds=info.duration, source_nonzero_low_byte_fraction=lower_byte_nonzero,
                     excerpt_samples=length, excerpt_seconds=length / FS, pre_roll_samples=offset, variants=[])
        outputs = {}
        print(f"Processing {source['id']}: {length / FS:.3f}s, {sr} Hz / {info.subtype}", flush=True)
        for rate in RATES:
            label = f'{rate / 1000:g}k'
            y, stats = codec.roundtrip(stream, rate, encoded / f'{label}.lc3fast', offset, length)
            trimmed = y[offset + codec.delay:offset + codec.delay + length]
            assert trimmed.shape == reference.shape
            outputs[rate] = trimmed.copy()
            row = dict(target_kbps=rate / 1000, filename=f'lc3-fast-{label}.wav', **stats, **metrics(reference, trimmed))
            track['variants'].append(row)
            print(f"  {label}: actual {stats['actual_kbps']:.3f} kb/s; waveform SNR {row['waveform_snr_db']:.2f} dB", flush=True)
        largest_peak = max(float(np.max(np.abs(reference))), *(float(np.max(np.abs(y))) for y in outputs.values()))
        # Extra common output attenuation, only if needed: no file gets independently normalized.
        post_gain = min(1.0, 10 ** (-1 / 20) / max(largest_peak, 1e-30))
        track['common_post_gain_db'] = float(20 * np.log10(post_gain))
        track['reference_filename'] = 'reference-48k24.wav'
        ref_q = quantize24(reference * post_gain, 42)
        save24(folder / track['reference_filename'], ref_q)
        track['reference_sha256'] = sha(folder / track['reference_filename'])
        for row in track['variants']:
            q = quantize24(outputs[round(row['target_kbps'] * 1000)] * post_gain, 42)
            path = folder / row['filename']
            save24(path, q)
            row['wav_sha256'] = sha(path)
            row['wav_samples'] = len(q)
            row['saved_peak_dbfs'] = float(20 * np.log10(np.max(np.abs(q.astype(np.float64))) / SCALE))
            row['saved_nonzero_low_byte_fraction'] = float(np.mean((q & 255) != 0))
            assert not np.array_equal(q, ref_q)
        playlist = ['#EXTM3U', track['reference_filename']] + [r['filename'] for r in track['variants']]
        (folder / 'all-rates.m3u8').write_text('\n'.join(playlist) + '\n', encoding='utf-8')
        (folder / 'manifest.json').write_text(json.dumps(track, indent=2) + '\n', encoding='utf-8')
        result['tracks'].append(track)
    archive = a.output / 'reproducibility'
    archive.mkdir(exist_ok=True)
    shutil.copy2(a.dll, archive / 'lc3-custom.dll')
    shutil.copy2(__file__, archive / Path(__file__).name)
    codec_files = list((ROOT / '.bench/lc3-listening-host/generated').glob('*.c'))
    codec_files += list((ROOT / '.bench/lc3-listening-host/generated').glob('*.h'))
    codec_files += [ROOT / 'firmware/benchmark/src' / name for name in
                    ('lc3_custom.c', 'lc3_custom.h', 'lc3_lite.c', 'lc3_lite.h', 'bench_opts.h', 'custom_sns.inc', 'custom_spec.inc')]
    result['codec_source_hashes'] = {str(path.relative_to(ROOT)).replace('\\', '/'): sha(path) for path in codec_files}
    filename = 'manifest.json' if not a.track else f'smoke-{a.track}.json'
    (a.output / filename).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f"Saved {len(result['tracks']) * (len(RATES) + 1)} verified WAV files; delay removed = {codec.delay} samples", flush=True)


if __name__ == '__main__':
    main()
