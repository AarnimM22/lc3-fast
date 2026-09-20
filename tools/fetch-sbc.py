"""Fetch the pinned, unmodified Google SBC implementation."""
import hashlib
import io
from pathlib import Path
import tarfile
import urllib.request

REV = '6e505650145c9973d08a0bdd5e5f5e1914305e40'
SHA256 = 'fbb2e0b15d3f6717016bd61db26c134fe9c7231cc748d525714f0a2bca02177d'
root = Path(__file__).resolve().parents[1] / 'firmware/third_party'
root.mkdir(parents=True, exist_ok=True)
archive = root / f'libsbc-{REV}.tar.gz'
if not archive.exists():
    archive.write_bytes(urllib.request.urlopen(f'https://codeload.github.com/google/libsbc/tar.gz/{REV}').read())
data = archive.read_bytes()
digest = hashlib.sha256(data).hexdigest()
if digest != SHA256:
    raise RuntimeError('SBC archive SHA256 mismatch')
destination = (root / 'libsbc').resolve()
with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
    for member in tar:
        if not member.isfile():
            continue
        target = (destination / member.name.split('/', 1)[1]).resolve()
        if not target.is_relative_to(destination):
            raise RuntimeError('Unsafe archive path')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(tar.extractfile(member).read())
print(f'Prepared Google libsbc {REV}; archive SHA256 {digest}')
