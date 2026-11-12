#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""site-guide.py: the install guide as pages of the project site (site/guide/*.html), English and Turkish, made from
the same templates the release packages fill (release/templates/README-INSTALL.md and .tr.md), so the site never
drifts from the guide in the packages.

    python3 tools/site-guide.py            writes site/guide/*.html for the version in VERSION (the Tahoe package's text)
    python3 tools/site-guide.py --check    exits 1 if the pages on disk differ from what it would write

The guide's sections ("## ...") are grouped into pages by PAGES below; both languages have the same sections in the same
order (checked). Markdown support is what the guide uses: headings, paragraphs, lists (nested by indentation), fenced
code, tables, block quotes (GitHub's > [!NOTE] / [!WARNING] callouts), links, inline code, bold and italics. Links to
"#anchor" become links to the page that holds that heading. No dependencies.
"""
import html
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "site", "guide")

# (file, English title, Turkish title, indexes of the "## " sections; -1 = the text before the first one)
PAGES = [
    ("index", "Before you start", "Başlamadan önce", [-1, 0, 1]),
    ("macos", "1. Install macOS", "1. macOS kurulumu", [2]),
    ("settings", "2. SIP, AMFI and quirks", "2. SIP, AMFI ve quirk'ler", [3]),
    ("driver", "3. The driver on macOS", "3. macOS'ta sürücü", [4]),
    ("efi", "4. The EFI: IntelXe.kext, SSDT-ARCOFF, HybridPM", "4. EFI: IntelXe.kext, SSDT-ARCOFF, HybridPM", [5, 6]),
    ("optional", "Optional: Zink and HiDPI", "İsteğe bağlı: Zink ve HiDPI", [7, 8]),
    ("troubleshooting", "If it does not come up", "Açılmazsa", [9, 12]),
    ("upgrading", "Upgrading and the package's contents", "Yükseltme ve paketin içindekiler", [10, 11]),
]

# release/package.sh fill() for the Tahoe package: the site shows the tested macOS
FILL = {
    "@MACOS@": "macOS 26 Tahoe", "@MAJOR@": "26", "@CODENAME@": "tahoe",
    "@TESTED@": "Experimental. Tested on one machine only: an Arc B580 in a Z790 desktop, macOS 26.7.1, OpenCore.",
    "@TESTED_TR@": "Deneysel bir sürüm. Yalnızca tek makinede denendi: Z790 masaüstü, Arc B580, macOS 26.7.1, OpenCore.",
}


def slug(text):
    """GitHub's heading anchor: lower case, punctuation dropped (except - and _), spaces to -."""
    t = re.sub(r"<[^>]+>", "", text).strip().lower()
    t = re.sub(r"[^\w\- ]", "", t, flags=re.UNICODE)
    return t.replace(" ", "-")


def inline(text, anchors):
    """Inline markdown to HTML. Code spans are set aside first (their content is literal) and put back last, so bold
    and links may span them."""
    codes = []

    def keep(m):
        codes.append("<code>" + html.escape(m.group(1)) + "</code>")
        return "\x00%d\x00" % (len(codes) - 1)
    s = html.escape(re.sub(r"`([^`]*)`", keep, text), quote=False)

    def link(m):
        label, url = m.group(1), html.unescape(m.group(2))
        if url.startswith("#"):
            page = anchors.get(url[1:])
            url = (page + ".html" if page else "") + url
        ext = url.startswith("http")
        return '<a href="%s"%s>%s</a>' % (html.escape(url), ' rel="noopener"' if ext else "", label)
    s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, s)
    s = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", s)
    s = re.sub(r"(?<![\w*])\*([^*\n]+?)\*(?![\w*])", r"<i>\1</i>", s)
    return re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], s)


def local_images(raw):
    """The guide's screenshots point at the repository's docs/images on GitHub; the site has the same files in images/."""
    def repl(m):
        name = m.group(1)
        return 'src="../images/%s"' % name if os.path.exists(os.path.join(ROOT, "site", "images", name)) else m.group(0)
    return re.sub(r'src="https://raw\.githubusercontent\.com/[^"]*/docs/images/([^"/]+)"', repl, raw)


def blocks(lines, anchors):
    """Block-level markdown to HTML."""
    out, i, n = [], 0, len(lines)
    while i < n:
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        m = re.match(r"^(#{2,6})\s+(.*)$", line)
        if m:
            level = min(len(m.group(1)) + 0, 6)
            text = m.group(2).strip()
            out.append('<h%d id="%s">%s</h%d>' % (level, slug(text), inline(text, anchors), level))
            i += 1
            continue
        if line.lstrip().startswith("<"):   # raw HTML (the guide's centred screenshots): kept, images served by the site
            raw = []
            while i < n and lines[i].strip():
                raw.append(lines[i])
                i += 1
            out.append(local_images("\n".join(raw)))
            continue
        if line.lstrip().startswith("```"):
            indent = len(line) - len(line.lstrip())
            i += 1
            code = []
            while i < n and not lines[i].lstrip().startswith("```"):
                code.append(lines[i][indent:] if lines[i][:indent].strip() == "" else lines[i])
                i += 1
            i += 1
            out.append("<pre><code>" + html.escape("\n".join(code)) + "</code></pre>")
            continue
        if line.startswith("|"):
            rows = []
            while i < n and lines[i].startswith("|"):
                rows.append(lines[i])
                i += 1
            out.append(table(rows, anchors))
            continue
        if line.startswith(">"):
            quote = []
            while i < n and lines[i].startswith(">"):
                quote.append(lines[i][1:].lstrip() if len(lines[i]) > 1 else "")
                i += 1
            kind = "note"
            if quote and re.match(r"^\[!(NOTE|WARNING|IMPORTANT|TIP|CAUTION)\]", quote[0]):
                kind = quote[0][2:quote[0].index("]")].lower()
                quote = quote[1:]
            out.append('<div class="callout %s">%s</div>' % (kind, blocks(quote, anchors)))
            continue
        if re.match(r"^\s*([-*]|\d+\.)\s+", line):
            j, items = list_block(lines, i, anchors)
            out.append(items)
            i = j
            continue
        para = [line.strip()]
        i += 1
        while i < n and lines[i].strip() and not re.match(r"^(#{2,6}\s|```|\||>|\s*([-*]|\d+\.)\s+)", lines[i]):
            para.append(lines[i].strip())
            i += 1
        out.append("<p>" + inline(" ".join(para), anchors) + "</p>")
    return "\n".join(out)


def list_block(lines, i, anchors):
    """A list starting at line i (and its nested content, by indentation); returns (next line, html)."""
    first = re.match(r"^(\s*)([-*]|\d+\.)\s+", lines[i])
    base = len(first.group(1))
    ordered = first.group(2)[0].isdigit()
    start = int(first.group(2)[:-1]) if ordered else 1
    items, n = [], len(lines)
    while i < n:
        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", lines[i])
        if not m or len(m.group(1)) != base:
            break
        body = [m.group(3)]
        cont = len(m.group(1)) + len(m.group(2)) + 1
        i += 1
        while i < n:
            l = lines[i]
            if not l.strip():
                # a blank line continues the item only if indented content follows
                if i + 1 < n and lines[i + 1].strip() and len(lines[i + 1]) - len(lines[i + 1].lstrip()) > base:
                    body.append("")
                    i += 1
                    continue
                break
            ind = len(l) - len(l.lstrip())
            if ind <= base and re.match(r"^\s*([-*]|\d+\.)\s+", l):
                break
            if ind <= base and not l.startswith(" "):
                break
            body.append(l[cont:] if l[:cont].strip() == "" else l.lstrip())
            i += 1
        if len(body) == 1:
            items.append("<li>" + inline(body[0], anchors) + "</li>")
        else:
            first_par, rest = [], []
            k = 0
            while k < len(body) and body[k].strip() and not re.match(r"^(\s*([-*]|\d+\.)\s+|```|\||>)", body[k]):
                first_par.append(body[k].strip())
                k += 1
            rest = body[k:]
            items.append("<li>" + inline(" ".join(first_par), anchors) + (blocks(rest, anchors) if rest else "") + "</li>")
    tag = "ol" if ordered else "ul"
    attr = ' start="%d"' % start if ordered and start != 1 else ""
    return i, "<%s%s>%s</%s>" % (tag, attr, "".join(items), tag)


def table(rows, anchors):
    cells = [[c.strip() for c in r.strip().strip("|").split("|")] for r in rows]
    head, body = cells[0], [c for c in cells[2:]] if len(cells) > 1 and set(cells[1][0]) <= set("-: ") else cells[1:]
    h = "".join("<th>%s</th>" % inline(c, anchors) for c in head)
    b = "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c, anchors) for c in r) for r in body)
    return '<div class="table"><table><thead><tr>%s</tr></thead><tbody>%s</tbody></table></div>' % (h, b)


def sections(path, version):
    text = open(path, encoding="utf-8").read().replace("@VER@", version)
    for k, v in FILL.items():
        text = text.replace(k, v)
    left = re.findall(r"@[A-Z_]+@", text)
    if left:
        sys.exit("%s: unfilled %s" % (path, ", ".join(sorted(set(left)))))
    lines = text.split("\n")
    intro, secs, cur = [], [], None
    for l in lines:
        if l.startswith("## "):
            cur = [l]
            secs.append(cur)
        elif cur is None:
            if not l.startswith("# "):   # the document title: the page has its own
                intro.append(l)
        else:
            cur.append(l)
    return intro, secs


def anchor_map(secs):
    """Every heading's anchor -> the page that shows it."""
    amap = {}
    for page, _, _, idx in PAGES:
        for k in idx:
            if k < 0:
                continue
            for l in secs[k]:
                m = re.match(r"^#{2,6}\s+(.*)$", l)
                if m:
                    amap.setdefault(slug(m.group(1)), page)
    return amap


def page_html(p, en, tr, version, nav_pages):
    file, title_en, title_tr, _ = p
    pos = [q[0] for q in nav_pages].index(file)
    prev = nav_pages[pos - 1] if pos > 0 else None
    nxt = nav_pages[pos + 1] if pos + 1 < len(nav_pages) else None
    side = "".join('<a href="%s.html"%s><span lang="en">%s</span><span lang="tr">%s</span></a>'
                   % (q[0], ' aria-current="page"' if q[0] == file else "", html.escape(q[1]), html.escape(q[2]))
                   for q in nav_pages)
    pn = '<div class="doc-pn">%s%s</div>' % (
        '<a href="%s.html">&larr; <span lang="en">%s</span><span lang="tr">%s</span></a>' % (prev[0], html.escape(prev[1]), html.escape(prev[2])) if prev else "<span></span>",
        '<a href="%s.html"><span lang="en">%s</span><span lang="tr">%s</span> &rarr;</a>' % (nxt[0], html.escape(nxt[1]), html.escape(nxt[2])) if nxt else "<span></span>")
    return TEMPLATE.format(title_en=html.escape(title_en), title_tr=html.escape(title_tr), version=version, side=side,
                           en=en, tr=tr, pn=pn)


TEMPLATE = """<!doctype html>
<html lang="en" data-lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title_en} · Install guide · intel-gpu-macos-driver</title>
<meta name="description" content="The install guide of intel-gpu-macos-driver {version}: {title_en}.">
<meta name="theme-color" content="#05070b">
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Cdefs%3E%3ClinearGradient id='g' x1='0' y1='0' x2='1' y2='1'%3E%3Cstop offset='0' stop-color='%232fe2ff'/%3E%3Cstop offset='1' stop-color='%237a6bff'/%3E%3C/linearGradient%3E%3C/defs%3E%3Crect x='6' y='6' width='20' height='20' rx='5' fill='url(%23g)'/%3E%3C/svg%3E">
<link rel="preload" href="../fonts/PlusJakartaSans-latin.woff2" as="font" type="font/woff2" crossorigin>
<link rel="stylesheet" href="../fonts.css">
<link rel="stylesheet" href="../styles.css">
</head>
<body>
<!-- generated by tools/site-guide.py from release/templates/README-INSTALL*.md; edit those, not this file -->
<div class="grain" aria-hidden="true"></div>
<nav class="nav" aria-label="Main">
  <a class="brand" href="../index.html"><span>IntelXe <small>for macOS</small></span></a>
  <div class="nav-links">
    <a href="../index.html"><span lang="en">Home</span><span lang="tr">Ana sayfa</span></a>
    <a href="index.html" aria-current="page"><span lang="en">Install guide</span><span lang="tr">Kurulum rehberi</span></a>
    <a href="../releases.html"><span lang="en">Releases</span><span lang="tr">Sürümler</span></a>
    <a href="../help.html"><span lang="en">Help</span><span lang="tr">Yardım</span></a>
    <a href="https://github.com/yusufklncc/intel-gpu-macos-driver">GitHub</a>
  </div>
  <div class="nav-right">
    <div class="lang" role="group" aria-label="Language">
      <button type="button" data-lang="en" aria-pressed="true">EN</button>
      <button type="button" data-lang="tr" aria-pressed="false">TR</button>
    </div>
    <a class="btn btn-primary btn-small" href="../index.html#download"><span lang="en">Download</span><span lang="tr">İndir</span></a>
  </div>
</nav>
<main class="doc wrap">
  <aside class="doc-side" aria-label="Guide pages">
    <p class="eyebrow"><span lang="en">Install guide {version}</span><span lang="tr">Kurulum rehberi {version}</span></p>
    {side}
  </aside>
  <article class="doc-body">
    <h1><span lang="en">{title_en}</span><span lang="tr">{title_tr}</span></h1>
    <div lang="en">
{en}
    </div>
    <div lang="tr">
{tr}
    </div>
    {pn}
  </article>
</main>
<script src="../main.js"></script>
</body>
</html>
"""


def main():
    version = open(os.path.join(ROOT, "VERSION")).read().strip()
    tmpl = os.path.join(ROOT, "release", "templates")
    intro_en, en = sections(os.path.join(tmpl, "README-INSTALL.md"), version)
    intro_tr, tr = sections(os.path.join(tmpl, "README-INSTALL.tr.md"), version)
    if len(en) != len(tr):
        sys.exit("the English guide has %d sections, the Turkish %d: keep them in step" % (len(en), len(tr)))
    used = sorted(k for p in PAGES for k in p[3] if k >= 0)
    if used != list(range(len(en))):
        sys.exit("PAGES must place every one of the %d sections exactly once (placed: %s)" % (len(en), used))
    amap_en, amap_tr = anchor_map(en), anchor_map(tr)
    want = {}
    for p in PAGES:
        body_en = "\n".join(blocks(intro_en if k < 0 else en[k], amap_en) for k in p[3])
        body_tr = "\n".join(blocks(intro_tr if k < 0 else tr[k], amap_tr) for k in p[3])
        want[p[0] + ".html"] = page_html(p, body_en, body_tr, version, PAGES)
    if "--check" in sys.argv:
        stale = [f for f, t in want.items() if not os.path.exists(os.path.join(OUT, f)) or open(os.path.join(OUT, f), encoding="utf-8").read() != t]
        if stale:
            print("site/guide is stale: " + ", ".join(stale) + " (python3 tools/site-guide.py)")
            sys.exit(1)
        print("site/guide matches the templates")
        return
    os.makedirs(OUT, exist_ok=True)
    for f, t in want.items():
        with open(os.path.join(OUT, f), "w", encoding="utf-8") as fh:
            fh.write(t)
    print("wrote %d pages to site/guide for %s" % (len(want), version))


if __name__ == "__main__":
    main()
