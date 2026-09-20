"""Independently check saved music, archived packet decoding and delay alignment."""
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import wave
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'firmware/listening/lc3-fast-2026-09-20'
SCALE, N = 8388608, 120


def read24(path, expected_hash, expected_samples):
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == expected_hash, path
    with wave.open(str(path), 'rb') as f:
        assert (f.getnchannels(), f.getsampwidth(), f.getframerate(), f.getnframes()) == (2, 3, 48000, expected_samples)
        b = np.frombuffer(f.readframes(f.getnframes()), np.uint8).reshape(-1, 3).astype(np.int32)
    q = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
    q = ((q ^ 0x800000) - 0x800000).reshape(-1, 2)
    assert np.max(np.abs(q)) < SCALE - 1
    assert np.mean((q & 255) != 0) > .9
    return q.astype(np.float64) / SCALE


def main():
    manifest = json.loads((OUT / 'manifest.json').read_text())
    dll = OUT / 'reproducibility/lc3-custom.dll'
    assert hashlib.sha256(dll.read_bytes()).hexdigest() == manifest['codec']['dll_sha256']
    lib = C.CDLL(str(dll))
    lib.lc3_decoder_size.argtypes, lib.lc3_decoder_size.restype = [C.c_int] * 2, C.c_uint
    lib.lc3_setup_decoder.argtypes, lib.lc3_setup_decoder.restype = [C.c_int] * 3 + [C.c_void_p], C.c_void_p
    lib.lc3_custom_decode.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_int]
    lib.lc3_custom_decode.restype = C.c_int
    rows = []
    total_frames = 0
    for track in manifest['tracks']:
        folder = OUT / track['id']
        ref = read24(folder / track['reference_filename'], track['reference_sha256'], track['excerpt_samples'])
        for row in track['variants']:
            y = read24(folder / row['filename'], row['wav_sha256'], track['excerpt_samples'])
            packet_path = folder / 'encoded' / f"{row['target_kbps']:g}k.lc3fast"
            data = packet_path.read_bytes()
            assert data[:8] == b'LC3FAST1'
            sr, dt, channels, bitrate, count, config = struct.unpack_from('<6I', data, 8)
            assert (sr, dt, channels, bitrate, count, config) == (48000, 2500, 2, round(row['target_kbps'] * 1000), row['encoded_stereo_frames'], 0x201)
            states = []
            for ch in range(2):
                mem = C.create_string_buffer(lib.lc3_decoder_size(dt, sr))
                ptr = lib.lc3_setup_decoder(dt, sr, sr, mem)
                assert ptr
                states.append((mem, ptr))
            decoded = np.empty((count * N, 2), np.float32)
            pcm = np.empty(N, np.float32)
            offset = 32
            for seq in range(count):
                lengths = data[offset:offset + 2]
                assert len(lengths) == 2
                offset += 2
                for ch, length in enumerate(lengths):
                    assert 22 <= length <= 102
                    packet = data[offset:offset + length]
                    offset += length
                    assert len(packet) == length and packet[0] == 0xd1 and packet[1] == length - 2
                    buf = C.create_string_buffer(packet)
                    assert lib.lc3_custom_decode(states[ch][1], buf, length, 3, pcm.ctypes.data, 1) == 0
                    decoded[seq * N:(seq + 1) * N, ch] = pcm
            assert offset == len(data)
            begin = track['pre_roll_samples'] + manifest['codec']['api_delay_samples']
            expected = decoded[begin:begin + len(y)].astype(np.float64) * 10 ** (track['common_post_gain_db'] / 20)
            assert np.isfinite(expected).all()
            error_lsb = float(np.max(np.abs(expected - y)) * SCALE)
            assert error_lsb <= 1.501, error_lsb  # TPDF +/-1 LSB plus +/-0.5 LSB rounding
            # Independent alignment on actual music, avoiding initial/terminal transients.
            a = ref[48000:96000]
            lags = range(-8, 9)
            best = []
            for ch in range(2):
                errors = [np.mean((a[:, ch] - y[48000 + lag:96000 + lag, ch]) ** 2) for lag in lags]
                best.append(int(list(lags)[int(np.argmin(errors))]))
            assert best == [0, 0], (track['id'], row['target_kbps'], best)
            rows.append(dict(track=track['id'], target_kbps=row['target_kbps'],
                             archive_stereo_frames=count, best_residual_lag_samples=best,
                             max_wav_vs_redecoded_difference_lsb=error_lsb))
            total_frames += count
        print(f"PASS {track['id']}: eight WAVs, seven packet archives, zero residual sample shift", flush=True)
    result = dict(wav_count=len(rows) + len(manifest['tracks']), decoded_channel_packets=total_frames * 2,
                  wav_bytes=sum(p.stat().st_size for p in OUT.glob('*/*.wav')),
                  errors=0, variants=rows)
    (OUT / 'verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"PASS {result['wav_count']} WAVs; {total_frames * 2} archived channel packets independently decoded", flush=True)


if __name__ == '__main__':
    main()
