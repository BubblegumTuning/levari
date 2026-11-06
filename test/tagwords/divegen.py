#!/usr/bin/env python3
"""Deep dive corpus generator (one-shot:
fi 5000, ja 5000, ru 1000, uk 1000 top frequency words; troublemakers get
promoted to words.txt by hand afterwards, clean words discarded.)"""
import importlib.util, os, sys

here = os.path.dirname(os.path.abspath(__file__)); repo = os.path.dirname(os.path.dirname(here))
spec = importlib.util.spec_from_file_location('tagcheck', os.path.join(here, 'tagcheck.py'))
tc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tc)

CACHE = os.path.join(repo, 'test-assets/corpus/frequencywords')
QUOTA = {'fi': 5000, 'ja': 5000, 'ru': 1000, 'uk': 1000, 'fr': 1000, 'de': 1000, 'no': 1000}
LATIN = ('fi', 'fr', 'de', 'no')   # latin-1 legacy-field languages

# upstream github.com/hermitdave/FrequencyWords (2018 batch except ja=2016).
# Fetch (lists are NOT in git; CC-by-sa-4.0). The per-language SHA256 pins
# below are the authoritative identity check for the cached lists:
#   https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/<year>/<lang>/<lang>_50k.txt
SHA256 = {
 'fi': '8bc1d43dd54ea0c4432b1f6ae497759a332e984224819cf8a4f2337681c803b4',
 'ja': '6f734851c4881295d66947e09987ccb17010bb71cd9df7b6e326870c847508d6',
 'ru': '6095f507cc167488ec66ada5a85ac50433503a08ad24a07c6eabdf54352c4e7f',
 'uk': '881686fe66a168b79f6a9ee2f6cbbf944cfb12bfb7040913ef535cb29bd86ec9',
 'fr': 'f81f7c570b6433764da99aa30f4dfb08d81c5926301377b709af23a13b1f9596',
 'de': 'd9e50546fd7e8b6fe6542a2b33c51d1331092b2a3916ec09f80d97856068705b',
 'no': 'd5808d8ec04603b765c09c06a03f0273ccf36bb35a3902287e56b7e7d872ca92',
}

def verify_cache():
    import hashlib, sys
    for lang, want in SHA256.items():
        p = os.path.join(CACHE, lang + '.txt')
        if not os.path.exists(p):
            sys.exit('missing %s - fetch it first (URL pattern in this file)' % p)
        got = hashlib.sha256(open(p, 'rb').read()).hexdigest()
        if got != want:
            sys.exit('sha256 mismatch for %s: %s (pinned %s)' % (p, got, want))

def words(lang, n):
    out = []
    for line in open(os.path.join(CACHE, lang + '.txt'), encoding='utf-8'):
        w = line.split(' ', 1)[0].strip()
        if w and not any(c in w for c in '\t\n\r'):
            out.append(w)
        if len(out) >= n: break
    return out

def ok(w, fn):
    try: fn(w); return True
    except SystemExit: return False

def multibyte_last(w):
    return len(w[-1].encode('utf-8')) > 1

def gen(lang):
    ws = words(lang, QUOTA[lang])
    lines = []
    for i, w in enumerate(ws):
        lines.append('u8\t' + w)
        if (multibyte_last(w)):
            lines.append('u8cut\t' + w)
            if len(w[-1].encode('utf-8')) >= 3:   # u8cut2 only meaningful for 3/4-byte chars
                lines.append('u8cut2\t' + w)
        if lang in LATIN:
            if ok(w, tc.enc_latin1):
                lines.append('l1\t' + w)
                if len(w.encode('latin-1')) <= 30:
                    lines.append('v1l\t' + w)
        elif lang == 'ja':
            if len(w) <= 30:                       # byte budget fine, id3v1 slot
                lines.append('v1u8\t' + w)
        else:  # ru / uk
            if ok(w, tc.enc_cyr):
                lines.append('cyr\t' + w)
                if len(tc.enc_cyr(w)) <= 30:
                    lines.append('v1c\t' + w)
                else:
                    lines.append('v1cut\t' + w)
    # bigrams: consecutive pairs, every other pair for volume
    for i in range(0, min(len(ws), 2000), 2):
        a, b = ws[i], ws[i+1] if i + 1 < len(ws) else ws[i]
        fn = tc.enc_latin1 if lang in LATIN else (tc.enc_cyr if lang in ('ru','uk') else None)
        if fn and ok(a, fn) and ok(b, fn) and ' ' not in a and ' ' not in b:
            lines.append(('bigl' if lang in LATIN else 'bigc') + '\t' + a + ' ' + b)
    return ws, lines

verify_cache()
for lang in QUOTA:
    ws, lines = gen(lang)
    out = os.path.join(os.path.dirname(CACHE), 'dive-%s.txt' % lang)   # git-ignored tree
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(lang, 'words', len(ws), 'entries', len(lines))
