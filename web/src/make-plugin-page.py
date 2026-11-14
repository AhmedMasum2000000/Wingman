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
open('/home/user/Wingman/web/m1d1.html', 'w').write(out)
os.makedirs(S + '/pl', exist_ok=True); open(S + '/pl/backline-studio.html', 'w').write(out)
print('plugin page', len(out))
