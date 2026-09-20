"""Download pinned codec sources; run with the ordinary local Python runtime."""
import argparse
import hashlib
import io
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1] / 'firmware' / 'third_party'
ETSI_URL = 'https://www.etsi.org/deliver/etsi_ts/103600_103699/103634/01.07.01_60/ts_103634v010701p0.zip'
ETSI_SHA = '0b9de69247d2f24ae782ada4255e31c044a1ab4f14ec6798ccf6dd3651d77f7e'
ETSI_PREFIX = 'ETSI_Release/LC3plus_ETSI_src_v78d6b913cb5_20260615/'
GOOGLE_COMMIT = '8e1e722cda8dbdcc4b3cb9ba559d11c236c33d07'

def fetch_etsi():
    archive = ROOT / 'ts_103634v010701p0.zip'
    if not archive.exists():
        archive.write_bytes(urllib.request.urlopen(ETSI_URL).read())
    payload = archive.read_bytes()
    if hashlib.sha256(payload).hexdigest() != ETSI_SHA:
        raise RuntimeError('ETSI download SHA256 mismatch')
    with zipfile.ZipFile(io.BytesIO(payload)) as z:
        for name in z.namelist():
            if name.endswith('/') or not name.startswith(ETSI_PREFIX):
                continue
            relative = name.removeprefix(ETSI_PREFIX)
            if relative.startswith('src/fixed_point/'):
                relative = relative.removeprefix('src/')
            elif relative != 'Readme.txt':
                continue
            target = (ROOT / 'lc3plus' / relative).resolve()
            if not target.is_relative_to(ROOT.resolve()):
                raise RuntimeError('Unsafe archive path')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(z.read(name))
    # Upstream's WMOPS=0 option encounters three incorrect #ifdef guards.
    header = ROOT / 'lc3plus' / 'fixed_point' / 'basop_util.h'
    text = header.read_text()
    assert text.count('#ifdef WMOPS') == 3
    header.write_text(text.replace('#ifdef WMOPS', '#if defined(WMOPS) && WMOPS'))
    print('ETSI TS 103 634 v1.7.1 / reference software 1.9.2 prepared')

def fetch_google():
    url = f'https://codeload.github.com/google/liblc3/zip/{GOOGLE_COMMIT}'
    payload = urllib.request.urlopen(url).read()
    with zipfile.ZipFile(io.BytesIO(payload)) as z:
        prefix = f'liblc3-{GOOGLE_COMMIT}/'
        for name in z.namelist():
            if name.endswith('/') or not name.startswith(prefix):
                continue
            target = (ROOT / 'liblc3' / name.removeprefix(prefix)).resolve()
            if not target.is_relative_to(ROOT.resolve()):
                raise RuntimeError('Unsafe archive path')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(z.read(name))
    print(f'Google liblc3 {GOOGLE_COMMIT} prepared')

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--google', action='store_true', help='Also fetch optional comparison codec')
    a = p.parse_args()
    ROOT.mkdir(parents=True, exist_ok=True)
    fetch_etsi()
    if a.google:
        fetch_google()
