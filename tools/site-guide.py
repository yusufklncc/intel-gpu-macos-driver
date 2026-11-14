#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""site-guide.py: keeps the project site's install guide (site/guide/*.html) true to the install guide of the release
packages (release/templates/README-INSTALL.md and .tr.md).

The site guide is written by hand, in English and Turkish on every page, ordered by path (introduction, installation,
troubleshooting, macOS versions, the project) rather than by the packages' four stages. This tool does not write it; it
checks that it has not drifted away from what the packages say:

    python3 tools/site-guide.py --check     exit 1 on drift (release/release.sh runs this before a release)
    python3 tools/site-guide.py --check --next 0.1.5
                                            the same against the working tree's templates filled for 0.1.5: a site
                                            prepared for the next release, checked before VERSION is bumped
    python3 tools/site-guide.py --tokens    print what --check looks for, per language
    python3 tools/site-guide.py --index     write site/guide/search-index.js, the guide's search index

What --check compares. From each template (English against the site's English text, Turkish against its Turkish
text; text outside any lang="..." element counts for both):
  - every line of every fenced code block (a trailing "  # comment" is dropped: comments may be worded differently);
  - every inline code span (`...`): commands, file, kext and path names, config.plist keys, boot-args, sysctls;
  - boot-args and NVRAM settings written in prose without backticks (name_with_underscore=value, -name_with_underscore);
  - version strings with three parts (26.7.1, 0.1.3, 1.7.2).
Each must appear, character for character after collapsing white space, somewhere in the site guide. It also checks
that every guide page carries the same sidebar, and that site/guide/search-index.js is what --index would write.

The templates are read as released: from the tag vVERSION (git show) when it exists, so work on the next release does
not count as drift; before the tag exists (release.sh, preparing a release) from the working tree. They are filled as
release/package.sh fills them for the Tahoe package (the site's main path). --next VERSION reads them from the
working tree and fills them for that version, as release.sh will see them once VERSION says it. No dependencies.
"""
import html
import html.parser
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUIDE = os.path.join(ROOT, "site", "guide")
INDEX = os.path.join(GUIDE, "search-index.js")

# release/package.sh fill() for the Tahoe package
FILL = {
    "@MACOS@": "macOS 26 Tahoe", "@MAJOR@": "26", "@CODENAME@": "tahoe",
    "@TESTED@": "Experimental. Tested on one machine only: an Arc B580 in a Z790 desktop, macOS 26.7.1, OpenCore.",
    "@TESTED_TR@": "Deneysel bir sürüm. Yalnızca tek makinede denendi: Z790 masaüstü, Arc B580, macOS 26.7.1, OpenCore.",
}
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}
BLOCK = {"p", "li", "div", "pre", "td", "th", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "section", "br", "dt", "dd",
         "table", "ul", "ol", "dl", "figcaption", "summary", "details", "nav", "article", "aside", "header", "footer"}


def norm(s):
    return re.sub(r"\s+", " ", s).strip()


NEXT = None   # --next VERSION: the working tree's templates for that version


def template_text(rel, version):
    """release/templates/<rel> as released (tag vVERSION) when the tag exists, else from the working tree."""
    r = None if NEXT else subprocess.run(["git", "-C", ROOT, "show", "v%s:release/templates/%s" % (version, rel)],
                                         capture_output=True)
    if r is not None and r.returncode == 0:
        return r.stdout.decode("utf-8"), "v" + version
    with open(os.path.join(ROOT, "release", "templates", rel), encoding="utf-8") as fh:
        return fh.read(), "the working tree"


def filled(rel, version):
    text, source = template_text(rel, version)
    text = text.replace("@VER@", version)
    for k, v in FILL.items():
        text = text.replace(k, v)
    left = sorted(set(re.findall(r"@[A-Z_]+@", text)))
    if left:
        sys.exit("%s: unfilled %s" % (rel, ", ".join(left)))
    return text, source


def tokens(text):
    """[(token, section heading)] of one template, in order, each token once."""
    out, seen, section, fence = [], set(), "(intro)", None

    def add(tok):
        tok = norm(tok.replace("\\|", "|"))
        if len(tok) > 1 and tok not in seen:
            seen.add(tok)
            out.append((tok, section))
    para = []

    def flush():
        prose = " ".join(para)   # a paragraph at a time: a code span may run over a line break
        del para[:]
        for m in re.finditer(r"`([^`]+)`", prose):
            add(m.group(1))
        bare = re.sub(r"`[^`]*`", " ", prose)
        bare = re.sub(r"\]\([^)]*\)", "]", bare)   # link targets are not text
        for m in re.finditer(r"(?<![\w.-])(-?[a-z][a-z0-9]*_[a-z0-9_]+(?:=[0-9A-Za-z]+)?)(?![\w-])", bare):
            add(m.group(1))
        for m in re.finditer(r"(?<![\w.])(\d+\.\d+\.\d+)(?![\w.])", bare):
            add(m.group(1))
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("```"):
            flush()
            fence = None if fence is not None else s
            continue
        if fence is not None:
            if not s:
                continue
            if s.startswith("#"):
                add(s.lstrip("# "))           # an example of output, as a comment
            else:
                add(re.sub(r"\s{2,}#.*$", "", s))
            continue
        if not s or s.startswith("#") or s.startswith("|"):
            flush()
        if line.startswith("## ") or line.startswith("### "):
            section = line.lstrip("# ").strip()
            continue
        if s.startswith("<"):                  # raw HTML (the screenshots): not text
            continue
        if s:
            para.append(s)
            if s.startswith("|"):
                flush()                       # a table row is a paragraph of its own
    flush()
    return out


class Text(html.parser.HTMLParser):
    """The text of a page, per language: elements under lang="en" / lang="tr" go to that language, the rest to both."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack, self.buf = [], {"en": [], "tr": []}

    def lang(self):
        for _, l in reversed(self.stack):
            if l:
                return l
        return None

    def gap(self):
        for b in self.buf.values():
            b.append(" ")

    def handle_starttag(self, tag, attrs):
        if tag in BLOCK:
            self.gap()
        if tag in VOID:
            return
        a = dict(attrs)
        self.stack.append((tag, a.get("lang") if tag != "html" else None))

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                del self.stack[i:]
                break
        if tag in BLOCK:
            self.gap()

    def handle_data(self, data):
        if any(t in ("script", "style", "title") for t, _ in self.stack):
            return
        l = self.lang()
        for k in ("en", "tr"):
            if l in (None, k):
                self.buf[k].append(data)


def page_files():
    return sorted(f for f in os.listdir(GUIDE) if f.endswith(".html"))


def site_text():
    out = {"en": [], "tr": []}
    for f in page_files():
        p = Text()
        with open(os.path.join(GUIDE, f), encoding="utf-8") as fh:
            p.feed(fh.read())
        for k in out:
            out[k].append(norm("".join(p.buf[k])))
    return {k: " \n ".join(v) for k, v in out.items()}


def sidebar(f):
    with open(os.path.join(GUIDE, f), encoding="utf-8") as fh:
        m = re.search(r'<nav class="doc-side".*?</nav>', fh.read(), re.S)
    return m.group(0).replace(' aria-current="page"', "") if m else None


def guide_pages():
    """The guide's pages in sidebar order: [(file, English title, Turkish title)]."""
    nav = sidebar("index.html") or ""
    return [(m.group(1), html.unescape(m.group(2)), html.unescape(m.group(3))) for m in
            re.finditer(r'<a href="([\w-]+\.html)"><span lang="en">(.*?)</span><span lang="tr">(.*?)</span></a>', nav)]


class Sections(html.parser.HTMLParser):
    """For the search index: the article's sections (<section id>), each with its heading and text per language;
    the text before the first section is the page's own entry (id ""). Navigation (.doc-pn, the stage bar and anything
    else marked .noindex), the eyebrow and the h1 are left out."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.in_article, self.stack, self.secs, self.cur = False, [], [], None

    def new(self, sid):
        self.cur = {"id": sid, "h": {"en": [], "tr": []}, "t": {"en": [], "tr": []}}
        self.secs.append(self.cur)

    def gap(self):
        if self.cur:
            for k in ("en", "tr"):
                self.cur["t"][k].append(" ")

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "article":
            self.in_article = True
            self.new("")
            return
        if not self.in_article:
            return
        if tag == "section" and a.get("id"):
            self.new(a["id"])
        if tag in BLOCK:
            self.gap()
        if tag not in VOID:
            self.stack.append((tag, a.get("lang"), a.get("class", "")))

    def handle_endtag(self, tag):
        if tag == "article":
            self.in_article = False
        if not self.in_article or tag in VOID:
            return
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                del self.stack[i:]
                break
        if tag in BLOCK:
            self.gap()

    def handle_data(self, data):
        if not self.in_article or self.cur is None:
            return
        tags = [t for t, _, _ in self.stack]
        classes = " ".join(c for _, _, c in self.stack).split()
        if "h1" in tags or "doc-pn" in classes or "eyebrow" in classes or "noindex" in classes:
            return
        l = next((ll for _, ll, _ in reversed(self.stack) if ll), None)
        part = "h" if ("h2" in tags and self.cur["id"]) else "t"
        for k in ("en", "tr"):
            if l in (None, k):
                self.cur[part][k].append(data)


def build_index():
    pages = []
    for f, t_en, t_tr in guide_pages():
        p = Sections()
        with open(os.path.join(GUIDE, f), encoding="utf-8") as fh:
            p.feed(fh.read())
        secs = []
        for s in p.secs:
            e = {"id": s["id"], "h": [norm("".join(s["h"]["en"])), norm("".join(s["h"]["tr"]))],
                 "t": [norm("".join(s["t"]["en"])), norm("".join(s["t"]["tr"]))]}
            if e["id"] or e["t"][0] or e["t"][1]:
                secs.append(e)
        pages.append({"f": f, "t": [t_en, t_tr], "s": secs})
    body = json.dumps({"v": 1, "pages": pages}, ensure_ascii=False, separators=(",", ":"))
    return ("// The install guide's search index, written by tools/site-guide.py --index from site/guide/*.html. MIT.\n"
            "window.GUIDE_INDEX = " + body + ";\n")


def check(version):
    fails = 0
    text = site_text()
    for rel, lang in (("README-INSTALL.md", "en"), ("README-INSTALL.tr.md", "tr")):
        t, source = filled(rel, version)
        toks = tokens(t)
        missing = [(tok, sec) for tok, sec in toks if tok not in text[lang]]
        if missing:
            fails += len(missing)
            print("%s (%s): %d of %d not in the site guide's %s text:" % (rel, source, len(missing), len(toks),
                                                                          "English" if lang == "en" else "Turkish"))
            for tok, sec in missing:
                print("  [%s] %s" % (sec, tok))
        else:
            print("%s (%s): all %d found in the site guide's %s text" % (rel, source, len(toks),
                                                                         "English" if lang == "en" else "Turkish"))
    ref = sidebar("index.html")
    for f, _, _ in guide_pages():
        if not os.path.exists(os.path.join(GUIDE, f)):
            fails += 1
            print("the sidebar names %s, which does not exist" % f)
        elif sidebar(f) != ref:
            fails += 1
            print("%s: its sidebar differs from index.html's" % f)
    try:
        with open(INDEX, encoding="utf-8") as fh:
            stale = fh.read() != build_index()
    except FileNotFoundError:
        stale = True
    if stale:
        fails += 1
        print("site/guide/search-index.js is stale: python3 tools/site-guide.py --index")
    return fails


def main():
    global NEXT
    with open(os.path.join(ROOT, "VERSION")) as fh:
        version = fh.read().strip()
    if "--next" in sys.argv:
        i = sys.argv.index("--next")
        if i + 1 >= len(sys.argv) or not re.match(r"^\d+\.\d+\.\d+$", sys.argv[i + 1]):
            sys.exit("--next takes a version, such as 0.1.5")
        NEXT = version = sys.argv[i + 1]
    if "--tokens" in sys.argv:
        for rel in ("README-INSTALL.md", "README-INSTALL.tr.md"):
            print("== " + rel)
            for tok, sec in tokens(filled(rel, version)[0]):
                print("[%s] %s" % (sec, tok))
        return
    if "--index" in sys.argv:
        with open(INDEX, "w", encoding="utf-8") as fh:
            fh.write(build_index())
        print("wrote site/guide/search-index.js (%d pages)" % len(guide_pages()))
        return
    if "--check" in sys.argv:
        n = check(version)
        print("site/guide: %s" % ("no drift" if n == 0 else "%d problem(s)" % n))
        sys.exit(1 if n else 0)
    print(__doc__.strip())
    sys.exit(2)


if __name__ == "__main__":
    main()
