"""Restore the official, hash-pinned libopus 1.6.1 release."""
import hashlib
import io
from pathlib import Path
import tarfile
import urllib.request

root = Path(__file__).resolve().parents[1] / 'firmware/third_party'
root.mkdir(parents=True, exist_ok=True)
archive = root / 'opus-1.6.1.tar.gz'
url = 'https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz'
sha = '6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1'
if not archive.exists():
    archive.write_bytes(urllib.request.urlopen(url).read())
data = archive.read_bytes()
if hashlib.sha256(data).hexdigest() != sha:
    raise RuntimeError('Opus archive SHA256 mismatch')
with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
    for member in tar:
        if not member.isfile():
            continue
        relative = member.name.removeprefix('opus-1.6.1/')
        target = (root / 'opus' / relative).resolve()
        if not target.is_relative_to((root / 'opus').resolve()):
            raise RuntimeError('Unsafe archive path')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(tar.extractfile(member).read())
print('Prepared official libopus 1.6.1; archive SHA256 verified')
