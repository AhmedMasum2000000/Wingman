# Makes the plugin's page (web/m1d1.html) from the preview build: a standards-mode document with a UTF-8
# charset, and the fonts embedded so the plugin works offline.
import os, re, sys
S = '/tmp/claude-0/-home-user-Wingman/ddb90114-40c2-513b-aa42-fe74a7e007de/scratchpad'
t = open(S + '/backline-studio.html').read()
links = re.findall(r'<link rel="(?:preconnect|stylesheet)" href="https://fonts\.(?:googleapis|gstatic)\.com[^>]*>\n?', t)
assert len(links) == 3, links
for l in links: t = t.replace(l, '')
fonts = open(S + '/fonts/fonts-embedded.css').read()
head = '<!doctype html>\n<html lang="en">\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1">\n<style>\n' + fonts + '\n</style>\n'
out = head + t
assert 'fonts.googleapis' not in out
# The note reader (Basic Pitch + onnxruntime-web 1.20.1) must be in the page, so the plugin reads notes offline: each
# block is gunzipped and checked against the pinned SHA-256; a block the preview build left empty is filled here from
# the repo's third_party/ (found by name). Without it the plugin would still work, reading chords from the spectrum only.
import base64, gzip, hashlib
THIRD = '/home/user/Wingman/third_party'
BP = [('bp-ort-js', '@@BP_ORT_JS@@', 'ort.wasm.min.js', 'e66568724f8848e57cc2a56e4bea3a5b86ce3ff81b2da11eac8ed7ab02b27bd4'),
      ('bp-ort-mjs', '@@BP_ORT_MJS@@', 'ort-wasm-simd-threaded.mjs', '745eb7c0ce6f18a6aa521971b2877babc7ffb27eecb58ab3bc6e5ef4692672e8'),
      ('bp-ort-wasm', '@@BP_ORT_WASM@@', 'ort-wasm-simd-threaded.wasm', '207d02be4591c156b0a98f024f3d58005b5b04c92274d759fb390338c63559ea'),
      ('bp-model', '@@BP_MODEL@@', 'nmp.onnx', '2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec')]
def third(name, sha):
    for root, dirs, files in os.walk(THIRD):
        if name in files and hashlib.sha256(open(os.path.join(root, name), 'rb').read()).hexdigest() == sha: return open(os.path.join(root, name), 'rb').read()
    sys.exit(f'{name} (sha256 {sha}) is not in {THIRD}')
src = re.search(r'<script type="text/plain" id="m1d1-pitch-src">\n([\s\S]*?)</script>', out)
assert src and 'Copyright 2022 Spotify AB' in src.group(1) and 'workerMain' in src.group(1) and '@@' not in src.group(1), 'the page has no note reader (m1d1-pitch.js)'
bp_total = len(src.group(1))
for bid, ph, name, sha in BP:
    m = re.search(r'<script type="application/octet-stream" id="%s">([^<]*)</script>' % bid, out)
    assert m, f'the page has no {bid} block'
    if m.group(1) == ph:
        enc = base64.b64encode(gzip.compress(third(name, sha), compresslevel=9, mtime=0)).decode()
        out = out[:m.start(1)] + enc + out[m.end(1):]; m = re.search(r'id="%s">([^<]*)</script>' % bid, out)
    assert hashlib.sha256(gzip.decompress(base64.b64decode(m.group(1)))).hexdigest() == sha, f'{bid}: not {name} 1.20.1 / fa5997a'
    bp_total += len(m.group(1))
print('note reader (m1d1-pitch.js, onnxruntime-web 1.20.1, nmp.onnx) checked:', bp_total, 'bytes of the page')
open('/home/user/Wingman/web/m1d1.html', 'w').write(out)
os.makedirs(S + '/pl', exist_ok=True); open(S + '/pl/backline-studio.html', 'w').write(out)
print('plugin page', len(out))
