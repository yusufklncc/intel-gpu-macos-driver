#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""rename-intelxe.py: rename the driver stack from "B580" to "IntelXe" in a source tree.

  rename-intelxe.py [--dry-run] [--diff] [--review] [--no-default-excludes] [--exclude GLOB]... [--only GLOB]... TREE

The driver is for Intel Xe GPUs; "B580" stays only where it means the device (the Arc B580 card, its PCI ID 8086:e20b,
measurements, docs/devices/b580.md, the Metal device name "Intel Arc B580 Graphics"). The mapping (2026-11-05):

  B580.kext, org.hackintosh.B580            IntelXe.kext, io.github.yusufklncc.IntelXe
  B580FB.kext, org.hackintosh.B580FB        IntelXeFB.kext, io.github.yusufklncc.IntelXeFB
  class B580 / B580IntelFramebuffer         IntelXe / IntelXeFramebuffer
  B580Client, B580FB*, B580Ready, ...       IntelXeClient, IntelXeFB*, IntelXeReady, ...   (CamelCase: B580 -> IntelXe)
  B580_*, DRM_IOCTL_B580_*, NVMTL_B580_*    INTELXE_*, DRM_IOCTL_INTELXE_*, NVMTL_INTELXE_*  (ALL_CAPS: B580 -> INTELXE)
  b580_*, debug.b580.*, debug.b580fb_*,     intelxe_*, debug.intelxe.*, debug.intelxefb_*,
    /dev/b580obj, libb580venc, b580-guard     /dev/intelxeobj, libintelxevenc, intelxe-guard   (lowercase: b580 -> intelxe)
  boot-args b580surfvram, -b580nofb,        intelxe_surfvram, -intelxe_nofb, -intelxefb_novrr, intelxefb_vrr, ...
    -b580fbnovrr, b580fbvrr, ...              (BOOT_ARGS below: an underscore after the prefix)
  org.b580.*, org.hackintosh.b580*          io.github.yusufklncc.intelxe.*, io.github.yusufklncc.intelxe*
  file and directory names                  the same rules (kernel/B580 -> kernel/IntelXe, b580_drm.h -> intelxe_drm.h, ...)

Bare "B580" is renamed where it names the kext, the class or a file: in C/C++/Objective-C code (outside comments and
strings), the string literals "B580" and "B580: ...", and in prose when followed by .kext/.h/.cpp/:: or a path
separator, as a path component, after IOServiceOpen( / IOServiceMatching(, "class B580", "B580 service", and in
hyphenated names (Info-NMX-B580.plist). Everywhere else bare "B580" (and bare lowercase "b580" outside code and
hyphenated names) is left alone and listed by --review: there it usually means the card, and a person decides.

Never touched: "Arc B580" and the Metal device name, hex constants (0xb5800001ull, ioctl numbers stay), the fourcc
'B580' (an IOServiceOpen type, ABI), references to files outside this tree (src/b580..., ~/b580, tools/b580,
docs/reports/b580-*, patches/b580/, build-b580-* directories), historical file names (mesa/patches/*-B580-*.patch), and
any line carrying the marker "intelxe-rename: keep" (the old boot-arg names of the compatibility tables).

Default excludes (history and data that must not change; --no-default-excludes drops them): mesa/patches/,
mesa/xe2-venc/, mesa/working-tree/, data/cts/, data/video.json, data/history.json, docs/parity/HISTORY.md, and the
*.diff files that record a tree against its upstream: mesa/*.diff is regenerated from the renamed Mesa tree, and
nullmoth/b580-changes-vs-6e37546.diff is a record of that time and keeps its names; release/install.sh and
release/README-INSTALL.md name the earlier kexts and boot-args on purpose (the upgrade path), as do the EFI rules' test
vectors (release/efi-rules-vectors.json: JSON has no place for the marker). docs/devices/b580.md keeps its name. Binary files are skipped.

The script is idempotent: a second run changes nothing. --dry-run lists what would change (files, counts, renames)
and changes nothing; --diff also prints the diffs; --review lists the bare B580/b580 that remain, for a person.
"""
import argparse, difflib, fnmatch, os, re, subprocess, sys

KEEP_MARK = 'intelxe-rename: keep'
PREFIX = 'io.github.yusufklncc'

# Boot-arg names (the token without its leading '-'): old -> new.
BOOT_ARGS = {
    # IntelXe.kext
    'b580off': 'intelxe_off', 'b580nohugepages': 'intelxe_nohugepages', 'b580novideo': 'intelxe_novideo',
    'b580nousm': 'intelxe_nousm', 'b580lrcsys': 'intelxe_lrcsys', 'b580vcssingle': 'intelxe_vcssingle',
    'b580nowhitelist': 'intelxe_nowhitelist', 'b580surfvram': 'intelxe_surfvram', 'b580svwsfull': 'intelxe_svwsfull',
    'b580fds': 'intelxe_fds', 'b580lazy': 'intelxe_lazy', 'b580nometal': 'intelxe_nometal',
    # IntelXeFB.kext (and -b580noaccel, also read by NMXAccel and NMXAGDC)
    'b580nofb': 'intelxe_nofb', 'b580noaccel': 'intelxe_noaccel', 'b580fboff': 'intelxefb_off',
    'b580fbnoedidfix': 'intelxefb_noedidfix', 'b580fbnoscale': 'intelxefb_noscale', 'b580fbnorates': 'intelxefb_norates',
    'b580fbedidonly': 'intelxefb_edidonly', 'b580fbnogamma': 'intelxefb_nogamma', 'b580fbnovrr': 'intelxefb_novrr',
    'b580fbnohwcursor': 'intelxefb_nohwcursor', 'b580fbvrr': 'intelxefb_vrr', 'b580fbvrrmodes': 'intelxefb_vrrmodes',
    'b580fbnoirq': 'intelxefb_noirq',
}

# Whole phrases, applied first (their output no longer contains b580/B580).
PHRASES = [
    (re.compile(r'\borg\.hackintosh\.B580FB\b'), PREFIX + '.IntelXeFB'),
    (re.compile(r'\borg\.hackintosh\.B580\b'), PREFIX + '.IntelXe'),
    (re.compile(r'\borg\.hackintosh\.b580fb\b'), PREFIX + '.intelxefb'),
    (re.compile(r'\borg\.hackintosh\.b580\.'), PREFIX + '.intelxe.'),
    (re.compile(r'\borg\.b580\.'), PREFIX + '.intelxe.'),
    (re.compile(r'B580IntelFramebuffer'), 'IntelXeFramebuffer'),
    (re.compile(r'\bb580-prod9\.diff\b'), 'intelxe.diff'),
]

# Spans never changed (the card, ABI numbers, files outside the tree, history).
PROTECT = [re.compile(p) for p in (
    r'\b(?:Intel )?Arc[ -]B580\b', r'\bARC B580\b', r'\barc-b580\b',
    r'\b[\w./-]*devices/b580\.md\b', r'\bb580\.md\b', r'#[\w-]*b580[\w-]*', r'\bwhat-works-on-the-b580\b',
    r'\b0[xX][0-9a-fA-F]+[uUlL]*\b', r'\b(?![bB]580[a-fA-F]*\b)[0-9a-fA-F]{7,}\b',   # hashes, not b580dec
    r"'B580'",
    r'(?:~|\$HOME|\$\{HOME\})/b580\b',
    r'\bsrc/b580[\w./-]*', r'\bsrc/nullmoth/build-b580[\w.-]*', r'\bbuild-b580-[\w.-]+', r'\btools/b580\b[\w./-]*',
    r'\bpatches/b580/[\w./-]*', r'\bdocs/reports/[\w./-]*', r'\bdocs/research/[\w./-]*', r'\bb5/[\w./-]*',
    r'\bb580-[\w-]*20\d{6}[\w-]*\.md\b',
    r'\bb580-(?:tile4\w*|rb\d\w*|probe|surfvram-vs-pv\.patch|webkit-[\w-]+\.patch|xeprobe-v1\.patch|driver-project|'
    r'backups|surfvram\b(?!/))',
    r'<b580-tile4\w*>',
    r'\bmesa/patches/[\w./-]*', r'\bxe2-venc/[\w./-]*', r'\b\d{4}-[\w.-]*B580[\w.-]*\.patch\b',
    r'\bb580-changes-vs-\w+\.diff\b',
)]

CLIKE = {'.c', '.h', '.cpp', '.cc', '.hpp', '.m', '.mm', '.inc', '.metal', '.comp'}
DEFAULT_EXCLUDES = ['mesa/patches/*', 'mesa/xe2-venc/*', 'mesa/working-tree/*', 'data/cts/*', 'data/video.json',
                    'data/history.json', 'docs/parity/HISTORY.md', '*-changes-vs-*.diff', 'mesa/*.diff',
                    'release/install.sh', 'release/README-INSTALL.md', 'tools/rename-intelxe.py', '.git/*',
                    'release/efi-rules-vectors.json']   # its vectors name the earlier B580.kext entry on purpose (JSON: no marker)
NO_RENAME = ['docs/devices/b580.md', 'mesa/patches/*', 'mesa/xe2-venc/*', 'mesa/working-tree/*', 'mesa/*.diff',
             '*-changes-vs-*.diff']

TOKEN = re.compile(r'\b\w*(?:B580|b580)\w*')


def case_map(tok):
    """A compound token (anything but bare B580/b580/B580FB and the boot-args)."""
    if tok.lower() in BOOT_ARGS:
        return BOOT_ARGS[tok.lower()]
    if not re.search(r'[a-z]', tok):                       # ALL_CAPS (and digits/underscores)
        return tok.replace('B580', 'INTELXE')
    return tok.replace('B580', 'IntelXe').replace('b580', 'intelxe')


def bare_upper_prose(s, a, b):
    """Bare B580 at s[a:b] outside code: renamed only where it names the kext, class, service or a file."""
    after, before = s[b:b + 40], s[max(0, a - 24):a]
    if re.match(r'\.(?:kext|h|cpp)\b|::|/[a-z]| service\b| kext\b', after):
        return 'IntelXe'
    # the kext's own threads, locks and allocators (its copy engine, mapper or VRAM are the card's)
    if re.match(r"'s (?:own )?(?:thread|threads|interrupt thread|resume|resume thread|will-suspend|work loop|"
                r"allocator|allocators|registry|records|own objects|power events)\b", after):
        return 'IntelXe'
    if re.search(r'(?:/|[A-Za-z0-9]-|IOServiceOpen\(|IOServiceMatching\(\\?"?|class )$', before):
        return 'IntelXe'
    return None


def bare_lower_prose(s, a, b):
    """Bare b580 outside code: hyphenated names (b580-guard, build-b580.sh) and debug.b580."""
    after, before = s[b:b + 2], s[max(0, a - 6):a]
    if after[:1] == '-' and re.match(r'-\w', after) or before.endswith('-') and re.search(r'\w-$', before):
        return 'intelxe'
    if before.endswith('debug.'):
        return 'intelxe'
    return None


def sub_tokens(seg, ctx, review):
    """Rename the tokens of one segment. ctx: code, string, comment, prose, path."""
    def rep(m):
        tok, a, b = m.group(0), m.start(), m.end()
        if tok == 'B580':
            if ctx in ('code', 'path'):
                return 'IntelXe'
            if ctx == 'string' and a == 1 and (seg == '"B580"' or seg.startswith('"B580: ')):
                return 'IntelXe'                            # IOServiceMatching("B580"), the log prefix "B580: "
            r = bare_upper_prose(seg, a, b)
            if r is None:
                review.append(tok)
                return tok
            return r
        if tok == 'b580':
            if ctx in ('code', 'path'):
                return 'intelxe'
            r = bare_lower_prose(seg, a, b)
            if r is None:
                review.append(tok)
                return tok
            return r
        if tok == 'B580FB':
            return 'IntelXeFB'
        return case_map(tok)
    return TOKEN.sub(rep, seg)


def protect(text):
    spans = []
    for rx in PROTECT:
        for m in rx.finditer(text):
            spans.append((m.start(), m.end()))
    spans.sort()
    merged = []
    for a, b in spans:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(b, merged[-1][1]))
        else:
            merged.append((a, b))
    keep, out, pos = [], [], 0
    for a, b in merged:
        out.append(text[pos:a]); out.append('\x01%d\x02' % len(keep)); keep.append(text[a:b]); pos = b
    out.append(text[pos:])
    return ''.join(out), keep


def unprotect(text, keep):
    return re.sub(r'\x01(\d+)\x02', lambda m: keep[int(m.group(1))], text)


def lex_c(text):
    """Split C-family text into (kind, segment): code, comment, string (with its quotes), char (with its quotes)."""
    out, i, n, start, kind = [], 0, len(text), 0, 'code'
    def push(k, a, b):
        if b > a:
            out.append((k, text[a:b]))
    while i < n:
        c = text[i]
        if c == '/' and text.startswith('//', i):
            j = text.find('\n', i); j = n if j < 0 else j
            push('code', start, i); push('comment', i, j); i = start = j; continue
        if c == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2); j = n if j < 0 else j + 2
            push('code', start, i); push('comment', i, j); i = start = j; continue
        if c == '"' or (c == "'" and not (i > 0 and (text[i - 1].isalnum()))):
            q, j = c, i + 1
            while j < n and text[j] != q and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            push('code', start, i); push('string' if q == '"' else 'char', i, j); i = start = j; continue
        i += 1
    push('code', start, n)
    return out


def transform_text(text, path, review):
    ext = os.path.splitext(path)[1].lower()
    lines = text.split('\n')
    # lines with the keep marker are set aside whole
    held = {}
    for k, ln in enumerate(lines):
        if KEEP_MARK in ln:
            held[k] = ln; lines[k] = '\x03%d\x04' % k
    text = '\n'.join(lines)
    for rx, new in PHRASES:
        text = rx.sub(new, text)
    text, keep = protect(text)
    if ext in CLIKE:
        parts = []
        for kind, seg in lex_c(text):
            if kind == 'char':
                parts.append(seg)
            elif kind == 'string':
                parts.append(sub_tokens(seg, 'string', review))
            else:
                parts.append(sub_tokens(seg, 'code' if kind == 'code' else 'comment', review))
        text = ''.join(parts)
    else:
        text = sub_tokens(text, 'prose', review)
    text = unprotect(text, keep)
    text = re.sub(r'\x03(\d+)\x04', lambda m: held[int(m.group(1))], text)
    return text


def new_name(name):
    """A file or directory name."""
    if name in ('B580', 'b580'):
        return 'IntelXe' if name == 'B580' else 'intelxe'
    for rx, new in PHRASES:
        name = rx.sub(new, name)
    text, keep = protect(name)
    return unprotect(sub_tokens(text, 'path', []), keep)


def matches(rel, globs):
    return any(fnmatch.fnmatch(rel, g) or fnmatch.fnmatch(rel + '/', g) for g in globs)


def is_text(path):
    with open(path, 'rb') as f:
        head = f.read(8192)
    return b'\0' not in head


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tree')
    ap.add_argument('--dry-run', action='store_true', help='list the changes, change nothing')
    ap.add_argument('--diff', action='store_true', help='with --dry-run: print the content diffs')
    ap.add_argument('--review', action='store_true', help='list the bare B580/b580 left for a person to decide')
    ap.add_argument('--no-default-excludes', action='store_true')
    ap.add_argument('--exclude', action='append', default=[], help='glob relative to TREE (repeatable)')
    ap.add_argument('--only', action='append', default=[], help='touch only these paths (globs relative to TREE, '
                    'repeatable): e.g. the files a patch series changes in an upstream tree such as Mesa')
    a = ap.parse_args()
    root = os.path.abspath(a.tree)
    excl = ([] if a.no_default_excludes else DEFAULT_EXCLUDES) + a.exclude
    norename = ([] if a.no_default_excludes else NO_RENAME) + a.exclude
    files, dirs = [], []
    for d, dn, fn in os.walk(root):
        dn[:] = [x for x in dn if x != '.git']
        for x in dn:
            dirs.append(os.path.relpath(os.path.join(d, x), root))
        for x in fn:
            files.append(os.path.relpath(os.path.join(d, x), root))
    changed = 0
    review = []
    for rel in sorted(files):
        p = os.path.join(root, rel)
        if os.path.islink(p) or matches(rel, excl) or (a.only and not matches(rel, a.only)) or not is_text(p):
            continue
        try:
            old = open(p, encoding='utf-8').read()
        except UnicodeDecodeError:
            continue
        rv = []
        new = transform_text(old, rel, rv)
        if a.review and rv:
            for k, ln in enumerate(new.split('\n'), 1):
                if re.search(r'\b[Bb]580\b', ln):
                    s, keep = protect(ln)
                    if re.search(r'\b[Bb]580\b', s):
                        review.append('%s:%d: %s' % (rel, k, ln.strip()[:200]))
        if new != old:
            changed += 1
            n = sum(1 for x, y in zip(old.split('\n'), new.split('\n')) if x != y)
            print('%s %s (%d line%s)' % ('would change' if a.dry_run else 'changed', rel, n, '' if n == 1 else 's'))
            if a.dry_run and a.diff:
                sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), new.splitlines(True), 'a/' + rel, 'b/' + rel))
            if not a.dry_run:
                with open(p, 'w', encoding='utf-8') as f:
                    f.write(new)
    # renames, deepest first so that parents move last
    renames = []
    for rel in sorted(files + dirs, key=lambda r: -r.count('/')):
        if matches(rel, norename) or rel.startswith('.git') or (a.only and not matches(rel, a.only)):
            continue
        base = os.path.basename(rel)
        nb = new_name(base)
        if nb != base:
            renames.append((rel, os.path.join(os.path.dirname(rel), nb)))
    git = os.path.isdir(os.path.join(root, '.git'))
    for old, new in renames:
        print('%s %s -> %s' % ('would rename' if a.dry_run else 'renamed', old, new))
        if a.dry_run:
            continue
        tracked = git and subprocess.run(['git', '-C', root, 'ls-files', '--error-unmatch', old], capture_output=True).returncode == 0
        if tracked:
            subprocess.run(['git', '-C', root, 'mv', old, new], check=True)
        else:
            os.rename(os.path.join(root, old), os.path.join(root, new))
    if a.review:
        print('== bare B580/b580 left (%d lines): the card, or a name a person must rename' % len(review))
        for r in review:
            print(r)
    print('== %d file(s) %s, %d rename(s)' % (changed, 'to change' if a.dry_run else 'changed', len(renames)), file=sys.stderr)


if __name__ == '__main__':
    main()
