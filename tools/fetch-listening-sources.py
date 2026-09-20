"""Download publisher-provided music examples; keep provenance beside the audio."""
import argparse
import hashlib
import json
from html.parser import HTMLParser
from pathlib import Path
import shutil
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'firmware/listening/lc3-fast-2026-09-20'
PAGES = {
    'sony': 'https://helpguide.sony.net/high-res/sample1/v1/en/index.html',
    '3delite': 'https://www.3delite.hu/MP4%20Stream%20Editor/music.html',
}


def fetch(url, path):
    if path.exists():
        print(f'Cached {path.name}: {path.stat().st_size} bytes', flush=True)
        return
    request = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
    temp = path.with_suffix(path.suffix + '.part')
    with urllib.request.urlopen(request, timeout=60) as response, temp.open('wb') as dst:
        shutil.copyfileobj(response, dst)
    temp.replace(path)
    print(f'Downloaded {path.name}: {path.stat().st_size} bytes', flush=True)


class Links(HTMLParser):
    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'a' and any(k in attrs for k in ('href', 'onclick')):
            text = str(attrs)
            if any(s in text.lower() for s in ('download', '.zip', '.flac', '.wav')):
                print(text)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--inspect', action='store_true')
    p.add_argument('--manifest', type=Path)
    a = p.parse_args()
    sources = OUT / 'sources'
    sources.mkdir(parents=True, exist_ok=True)
    if a.inspect:
        for name, url in PAGES.items():
            path = sources / (name + '-source-page.html')
            fetch(url, path)
            print(name, url)
            Links().feed(path.read_text(encoding='utf-8', errors='replace'))
    if a.manifest:
        items = json.loads(a.manifest.read_text(encoding='utf-8'))
        for item in items:
            path = sources / item['download_filename']
            fetch(item['download_url'], path)
            item['download_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
            if path.suffix == '.zip':
                with zipfile.ZipFile(path) as z:
                    names = [n for n in z.namelist() if n.lower().endswith(('.flac', '.wav'))]
                    if len(names) != 1:
                        raise ValueError(f'Expected one audio file, got {names}')
                    # Copy the chosen member only; never extract arbitrary archive paths.
                    audio_path = sources / item['audio_filename']
                    with z.open(names[0]) as src, audio_path.open('wb') as dst:
                        shutil.copyfileobj(src, dst)
            item['audio_sha256'] = hashlib.sha256((sources / item['audio_filename']).read_bytes()).hexdigest()
        (sources / 'downloads.json').write_text(json.dumps(items, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
