"""Fetch the official WavPack-stream source at an immutable revision."""
import hashlib
import io
from pathlib import Path
import tarfile
import urllib.request

revision = '79ec9e178bd12c1445a09bec096db28707f1c55b'
root = Path(__file__).resolve().parents[1] / 'firmware/third_party'
root.mkdir(parents=True, exist_ok=True)
archive = root / ('wavpack-stream-' + revision + '.tar.gz')
if not archive.exists():
    archive.write_bytes(urllib.request.urlopen(
        'https://codeload.github.com/dbry/wavpack-stream/tar.gz/' + revision).read())
data = archive.read_bytes()
if hashlib.sha256(data).hexdigest() != '458f0cb99d2980f475281fac10c1d785c10cf3795ac196a72c55dd7f5c98dbad':
    raise RuntimeError('WavPack-stream archive SHA256 mismatch')
destination = (root / 'wavpack-stream').resolve()
with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
    for member in tar:
        if not member.isfile():
            continue
        relative = member.name.split('/', 1)[1]
        target = (destination / relative).resolve()
        if not target.is_relative_to(destination):
            raise RuntimeError('Unsafe archive path')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(tar.extractfile(member).read())
print('WavPack-stream revision', revision)
print('Archive SHA256', hashlib.sha256(data).hexdigest())
