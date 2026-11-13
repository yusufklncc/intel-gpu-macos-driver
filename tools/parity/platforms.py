#!/usr/bin/env python3
"""The driver's scope across Intel GPU platforms: one matrix of platform x feature, two facts per cell.

  (a) what Intel's own stack offers on that platform, from upstream sources only: data/platforms.json "intel" (Linux xe /
      i915 display descriptors, Mesa's device list, compute-runtime's product directories, each with its line), and for
      the video rows data/caps.json (tools/parity/caps.py: intel/media-driver's capability tables);
  (b) our status, one of: works (verified on the device named, with the date and where the evidence is), same (same
      code path, untested on this platform), not yet; or "-" where Intel does not offer it either. From
      data/platforms.json "ours", and for the B580's video rows from data/video.json (the mediakit runs).

Outputs: docs/parity/svg/platforms.svg, docs/PLATFORMS.md (every cell with its sources), and the table between
<!-- platforms:begin --> and <!-- platforms:end --> in README.md. No estimates. Python stdlib only.

  python3 tools/parity/platforms.py
"""
import json, os, re, sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render import svg, text, write, FONT  # noqa: E402  (same SVG style as the other status images)
from xml.sax.saxutils import escape  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CODECS = [('h264', 'H.264'), ('hevc', 'HEVC'), ('vp9', 'VP9'), ('av1', 'AV1')]
ICON = {'works': '🟩', 'same': '🟨', 'notyet': '⬜', 'na': '—'}
CLS = {'works': 'ok', 'same': 'warn', 'notyet': 'empty', 'na': 'ns'}
SHORT = {'works': 'works', 'same': 'same code, untested', 'notyet': 'not yet', 'na': 'not offered by Intel'}


def size_txt(w, h):
    if not w:
        return ''
    k = {4096: '4K', 8192: '8K', 16384: '16K'}
    return ' ≤%s' % k.get(w, '%dx%d' % (w, h)) if w == h or h in (12288,) else ' ≤%dx%d' % (w, h)


def intel_video(caps, plat, codec, d):
    """(short text, source) of what media-driver offers for codec/d on the platform, or (None, why) if nothing."""
    p = next(x for x in caps['platforms'] if x['id'] == plat)
    s = p['summary'].get(codec, {}).get(d)
    if not s:
        return None, 'media-driver %s: no %s %s entrypoint (%s)' % (caps['meta']['commit'][:12], codec, d, p['source'])
    bits420 = sorted({int(c.split('-')[0]) for c in s['coded'] if c.endswith(' 420')}) or s['bits']
    txt = '%s-bit%s' % ('/'.join(map(str, bits420)), size_txt(s['max_w'], s['max_h']))
    return txt, 'media-driver %s: %s' % (caps['meta']['commit'][:12], ', '.join(s['src'][:3]) + (' ...' if len(s['src']) > 3 else ''))


def intelxe_video(v, codec, d):
    """(status, evidence, date) of our B580 video row from data/video.json."""
    cells = [c for c in v['cells'] if c['codec'] == codec and c['dir'] == d]
    measured = [c for c in cells if c.get('items') or c.get('speed')]
    if all(c['grade'] == 'unsupported' for c in cells):
        return 'na', 'not offered by Intel on Xe2 (data/caps.json)', '', ''
    if not measured:
        return 'notyet', 'not measured', '', ''
    res = {'1080p': 1080, '2160p': 2160, '4320p': 4320}
    ok, bad = [], []
    for c in measured:
        tag = '%d-bit %s' % (c['bits'], c['res'])
        if d == 'decode':
            (ok if all(i['exact'] for i in c['items']) else bad).append(tag)
        else:
            (bad if c.get('failed_streams') else ok).append(tag)
    test = any('test stack' in (i.get('stack') or '') for c in measured for i in c.get('items', []))
    top = max(ok, key=lambda t: res[t.split()[1]]) if ok else None
    ev = ('%s %s up to %s' % ('bit-exact' if d == 'decode' else 'streams', 'in %d cells' % len(ok), top.split()[1])) if ok else 'no good cell'
    if bad:
        ev += '; failed: %s' % ', '.join(bad)
    if test:
        ev += '; measured with a test stack (not the installed one)'
    date = max(re.findall(r'\w{3} (\w{3}) +(\d+) [\d:]+ \+\d+ (\d{4})', ' '.join(v['meta']['runs'])) or [('', '', '')], key=lambda x: x[1])
    months = {'Oct': '10', 'Nov': '11'}
    when = '%s-%s-%02d' % (date[2], months.get(date[0], date[0]), int(date[1])) if date[2] else ''
    name = {1080: '1080p', 2160: '4K', 4320: '8K'}
    scope = ('%s-bit ≤%s' % ('/'.join(sorted({t.split('-')[0] for t in ok}, key=int)), name[res[top.split()[1]]])) if ok else ''
    return ('works' if ok else 'notyet'), ev, '%s, data/video.json (graded in docs/devices/b580.md)' % when, scope


def build(P, caps, v):
    """rows: [(feature id, english, turkish, {platform id: (status, ours text, evidence, intel text, intel source)})]"""
    rows = []
    for f in P['features']:
        if f['id'] == 'video':
            for codec, name in CODECS:
                for d in ('decode', 'encode'):
                    cells = {}
                    for p in P['platforms']:
                        it, isrc = intel_video(caps, p['caps'], codec, d)
                        scope = ''
                        if p['id'] == 'b580':
                            st, ev, when, scope = intelxe_video(v, codec, d)
                            if it is None:
                                st = 'na'
                        else:
                            st, ev, when = ('na', '', '') if it is None else ('notyet', '', '')
                        cells[p['id']] = (st, ev, when, it or '-', isrc, '', scope)
                    rows.append(('video-%s-%s' % (codec, d), '%s %s' % (name, d), '%s %s' % (name, {'decode': 'çözme', 'encode': 'kodlama'}[d]), cells))
            continue
        cells = {}
        for p in P['platforms']:
            st, ev, when = P['ours'][f['id']][p['id']][:3]
            dev = (P['ours'][f['id']][p['id']][3:] or [''])[0]
            it, isrc = P['intel'][f['id']][p['id']]
            cells[p['id']] = (st, ev, when, it, isrc, dev)
        rows.append((f['id'], f['en'], f['tr'], cells))
    return rows


SVG_TR = {
    'title': 'Intel GPU platformları: bizim durumumuz (renk ve ilk satır; videoda doğrulanmış kapsamımız) ve Intel\'in kendi Linux sürücülerinin sunduğu (ikinci satır)',
    'sources': 'Kaynaklar: Linux xe/i915 ekran kodu, Mesa cihaz listesi, compute-runtime, media-driver %s yetenek tabloları; her kutunun kanıtı docs/PLATFORMS.md\'de',
    'works': 'çalışıyor', 'same': 'aynı kod, denenmedi', 'notyet': 'henüz yok', 'na': 'Intel sunmuyor',
    'legend': [('ok', 'çalışıyor, adı geçen kartta doğrulandı'), ('warn', 'kod aynı, bu platformda denenmedi'),
               ('empty', 'henüz yok'), ('ns', 'Intel de bu platformda sunmuyor')],
    'video': 'Video: Intel satırı = 4:2:0\'da bit derinlikleri ve media-driver VA-API yeteneklerindeki en büyük görüntü; Gen12/DG2\'nin eski kodunda boyut tablosu yok',
    'one': 'Şimdiye kadar doğrulanan tek kart: Arc B580 (8086:E20B); IntelXe.kext B570\'e (E20C, 0.1.4\'ten beri), Pro B60 ve Pro B50\'ye (E211, E212) de bağlanıyor, bunlar denenmedi. Kanıt: docs/devices/b580.md',
    'svgtitle': 'Intel GPU platformları',
}


def svg_view(P, rows, caps, lang='en'):
    cols = P['platforms']
    TRL = lang == 'tr'
    tx = tr_text if TRL else (lambda s: s)
    short = (lambda st: SVG_TR[st]) if TRL else (lambda st: SHORT[st])
    ptitle = (lambda p: TR_PLATFORM.get(p['short'], p['short'])) if TRL else (lambda p: p['short'])
    cw, ch, x0, y0 = 150, 40, 300, 96
    W, H = x0 + cw * len(cols) + 12, y0 + ch * len(rows) + 84
    body = [text(12, 22, SVG_TR['title'] if TRL else 'Intel GPU platforms: our status (colour, first line; for video our verified scope) and what Intel\'s own Linux stack offers (second line)', 't'),
            text(12, 40, (SVG_TR['sources'] if TRL else 'sources: Linux xe/i915 display, Mesa device list, compute-runtime, media-driver %s caps tables; ours: evidence per cell (docs/PLATFORMS.md)') % caps['meta']['commit'][:12])]
    for i, p in enumerate(cols):
        body.append(text(x0 + i * cw + cw / 2, y0 - 22, ptitle(p), 'l', 'middle'))
        body.append(text(x0 + i * cw + cw / 2, y0 - 8, p['title'].split(':')[0], 's', 'middle'))
    for j, (fid, en, tr, cells) in enumerate(rows):
        y = y0 + j * ch
        name = tr if TRL else en
        body.append(text(12, y + ch / 2 + 4, name if len(name) < 48 else name[:46] + '…', 'l'))
        for i, p in enumerate(cols):
            st, ev, when, it, isrc, dev = cells[p['id']][:6]
            x = x0 + i * cw
            body.append('<rect x="%d" y="%d" width="%d" height="%d" rx="4" class="%s"><title>%s</title></rect>' % (
                x + 2, y + 2, cw - 4, ch - 4, CLS[st], escape('%s / %s: %s%s. Intel: %s (%s)' % (ptitle(p), name, short(st), (' - ' + ev) if ev else '', tx(it), isrc))))
            ink = {'ok': '#ffffff', 'warn': '#0b0b0b'}.get(CLS[st])
            scope = cells[p['id']][6] if len(cells[p['id']]) > 6 else ''
            # a verified video cell shows our own scope (it can be narrower than Intel's, on the second line)
            w_ = short('works')
            first = ('%s: %s' % (w_, tx(scope)) if scope else '%s (%s)' % (w_, dev or ('B580' if p['id'] == 'b580' else '?'))) if st == 'works' else short(st)
            second = '' if it in ('-', '') else 'Intel: ' + tx(it)
            for yy, s_, wgt in ((y + (17 if second else 24), first, 600), (y + 31, second, 400)):
                if not s_:
                    continue
                s_ = s_ if len(s_) <= 26 else s_[:25] + '…'
                if ink:
                    body.append('<text x="%.1f" y="%.1f" text-anchor="middle" style="fill:%s;font:%d 10.5px %s">%s</text>' % (x + cw / 2, yy, ink, wgt, FONT.replace("'", ''), escape(s_)))
                else:
                    body.append(text(x + cw / 2, yy, s_, 'l' if wgt == 600 else 's', 'middle'))
    ly = y0 + ch * len(rows) + 24
    lx = 12
    legend = SVG_TR['legend'] if TRL else [('ok', 'works, verified on the device named'), ('warn', 'same code path, untested on this platform'),
                                         ('empty', 'not yet'), ('ns', 'not offered by Intel on this platform either')]
    for cls, name in legend:
        body.append('<rect x="%d" y="%d" width="10" height="10" rx="2" class="%s"/>' % (lx, ly - 9, cls))
        body.append(text(lx + 14, ly, name))
        lx += 34 + int(len(name) * 6.2)
    body.append(text(12, ly + 20, SVG_TR['video'] if TRL else 'video: Intel = bit depths (4:2:0) and largest picture in media-driver\'s VA-API caps; Gen12/DG2 legacy code gives no size table'))
    body.append(text(12, ly + 36, SVG_TR['one'] if TRL else 'one device verified so far: the Arc B580 (8086:E20B); IntelXe.kext also attaches, untested, to the B570 (E20C, since 0.1.4), the Pro B60 and the Pro B50 (E211, E212). Evidence: docs/devices/b580.md'))
    return svg(W, H, ''.join(body), SVG_TR['svgtitle'] if TRL else 'Intel GPU platforms')


# Turkish for the compact table of README.tr.md: whole cell texts and the platform column titles that need it.
TR_CELL = {
    'ANV, Vulkan Video encode H.264/H.265': 'ANV, Vulkan Video kodlama H.264/H.265',
    'ANV, Vulkan Video encode incl. AV1': 'ANV, AV1 dahil Vulkan Video kodlama',
    'ANV; Vulkan Video encode off upstream on Xe2': "ANV; Xe2'de Vulkan Video kodlama upstream'de kapalı",
    'ANV; Vulkan Video encode off upstream on Xe3': "ANV; Xe3'te Vulkan Video kodlama upstream'de kapalı",
    'xe (default driver)': 'xe (varsayılan sürücü)',
    'xe GuC engine reset': 'xe GuC motor sıfırlama',
    'xe GuC engine reset, job timeout': 'xe GuC motor sıfırlama, iş zaman aşımı',
}
TR_PLATFORM = {'B570, other BMG': 'B570, diğer BMG'}


def tr_text(s):
    s = TR_CELL.get(s, s)
    return s.replace('-bit', '-bit')


def md_compact(P, rows, lang='en'):
    cols = P['platforms']
    tr = lang == 'tr'
    title = (lambda p: TR_PLATFORM.get(p['short'], p['short'])) if tr else (lambda p: p['short'])
    txt = tr_text if tr else (lambda s: s)
    ours, intel = ('bizim', 'Intel') if tr else ('ours', 'Intel')
    out = ['| | ' + ' | '.join(title(p) for p in cols) + ' |', '|---|' + '---|' * len(cols)]
    for fid, en, tr_name, cells in rows:
        # a verified cell names our own scope first when it is narrower than Intel's offer (e.g. VP9 8/10-bit vs 12-bit)
        cell = lambda c: ICON[c[0]] if c[0] == 'na' or c[3] in ('-', '') else (
            '%s %s %s · %s %s' % (ICON[c[0]], ours, txt(c[6]), intel, txt(c[3]).replace('|', '/')) if len(c) > 6 and c[6] and c[6] != c[3]
            else '%s %s' % (ICON[c[0]], txt(c[3]).replace('|', '/')))
        out.append('| %s | %s |' % (tr_name if tr else en, ' | '.join(cell(cells[p['id']]) for p in cols)))
    return '\n'.join(out)


def md_full(P, rows, caps, v):
    L = ['# Platformlar / Platforms', '',
         'Üretildi / generated: `tools/parity/platforms.py` (`tools/parity/build.sh`) from `data/platforms.json`, `data/caps.json`, '
         '`data/video.json`. Elle değiştirmeyin / do not edit by hand.', '',
         '**TR:** Her hücrede iki bilgi var: Intel\'in kendi yığınının o platformda sunduğu (yalnızca kaynaktan, satırıyla) ve bizim '
         'durumumuz: 🟩 çalışıyor, adı geçen cihazda doğrulandı (tarih ve kanıt) · 🟨 aynı kod yolu, bu platformda test edilmedi · '
         '⬜ henüz yok · — Intel de sunmuyor. Tahmin yok.', '',
         '**EN:** Two facts per cell: what Intel\'s own stack offers on the platform (from sources only, with the line) and our '
         'status: 🟩 works, verified on the device named (date and evidence) · 🟨 same code path, untested on this platform · '
         '⬜ not yet · — not offered by Intel either. No estimates.', '',
         'Kaynak ağaçlar / source trees: ' + '; '.join('%s: %s' % (k, s) for k, s in P['sources'].items()) +
         '; media-driver: %s %s (%s).' % (caps['meta']['commit'][:12], caps['meta']['date'], caps['meta']['build']), '',
         '![Intel GPU platforms](parity/svg/platforms.svg)', '', '## Platformlar / platforms', '']
    for p in P['platforms']:
        cp = next(x for x in caps['platforms'] if x['id'] == p['caps'])
        L.append('- **%s** — %s media-driver device IDs (%s): %s.' % (p['title'], p['detail'], cp['device_ids_source'], ', '.join(cp['device_ids'])))
    L.append('')
    for fid, en, tr, cells in rows:
        L += ['## %s / %s' % (tr, en), '', '| Platform | Bizim durum / our status | Kanıt / evidence | Intel | Intel kaynak / source |', '|---|---|---|---|---|']
        for p in P['platforms']:
            st, ev, when, it, isrc, dev = cells[p['id']][:6]
            L.append('| %s | %s %s | %s | %s | %s |' % (p['short'], ICON[st], SHORT[st], '; '.join(x for x in (ev, when) if x).replace('|', '/') or '—',
                                                     it.replace('|', '/'), (isrc or '—').replace('|', '/')))
        L.append('')
    L += ['## Video yeteneği, platform başına / video capability per platform (media-driver)', '',
          'Her codec ve yön için VA-API profilleri, `VAConfigAttribRTFormat` bit derinlikleri ve en büyük resim; satır numaralı kaynak '
          '`data/caps.json`\'da. / VA-API profiles, bit depths from `VAConfigAttribRTFormat` and the largest picture per codec and '
          'direction; every entry\'s source line is in `data/caps.json`.', '',
          '| media-driver platform | codec | decode | encode |', '|---|---|---|---|']
    for cp in caps['platforms']:
        for codec, name in CODECS + [('jpeg', 'JPEG'), ('vp8', 'VP8'), ('mpeg2', 'MPEG-2'), ('vvc', 'VVC')]:
            s = cp['summary'].get(codec)
            if not s:
                continue
            f = lambda d: '—' if d not in s else '%s%s%s' % (', '.join(s[d]['coded']) or 'yes', size_txt(s[d]['max_w'], s[d]['max_h']),
                                                              (' (' + ', '.join(s[d]['profiles']) + ')') if s[d]['profiles'] else '')
            L.append('| %s (%s) | %s | %s | %s |' % (cp['name'], cp['id'], name, f('decode'), f('encode')))
    L.append('')
    return '\n'.join(L)


def main():
    D = lambda n: json.load(open(os.path.join(ROOT, 'data', n)))
    P, caps, v = D('platforms.json'), D('caps.json'), D('video.json')
    rows = build(P, caps, v)
    write('platforms.svg', svg_view(P, rows, caps))
    write('platforms.tr.svg', svg_view(P, rows, caps, 'tr'))
    open(os.path.join(ROOT, 'docs/PLATFORMS.md'), 'w').write(md_full(P, rows, caps, v))
    m = None
    for readme in ('README.md', 'README.tr.md'):   # README.tr.md gets the table in Turkish
        rp = os.path.join(ROOT, readme)
        if not os.path.exists(rp):
            continue
        r = open(rp).read()
        mm = re.search(r'(<!-- platforms:begin -->\n)(.*?)(<!-- platforms:end -->)', r, re.S)
        if mm:
            m = mm
            r = r[:mm.start(2)] + md_compact(P, rows, 'tr' if readme.endswith('.tr.md') else 'en') + '\n' + r[mm.end(2):]
            open(rp, 'w').write(r)
    n = {}
    for _, _, _, cells in rows:
        for st, *_ in cells.values():
            n[st] = n.get(st, 0) + 1
    print('platforms: %d rows x %d platforms; %s%s' % (len(rows), len(P['platforms']), n, '' if m else ' (README has no platforms markers)'))


if __name__ == '__main__':
    main()
