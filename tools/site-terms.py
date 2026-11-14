#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""site-terms.py: the install guide's term tips (site/guide/concepts.html and the dotted term links on the other pages).

    python3 tools/site-terms.py               write site/terms.js from concepts.html, then link the first use of each
                                              term, per language, on every guide page
    python3 tools/site-terms.py --check       change nothing; list the links it would add and say whether site/terms.js
                                              is stale; exit 1 if either
    python3 tools/site-terms.py --data        write site/terms.js only
    python3 tools/site-terms.py --site DIR    work on another copy of site/ (a trial run)
    python3 tools/site-terms.py a.html ...    only these pages (names in site/guide/, or paths)

One source of truth: site/guide/concepts.html. Each <section id> there is a term; its <h2> is the term's name and the
first sentence of its first <p lang="en"> / <p lang="tr"> is the tip main.js shows when the pointer or the keyboard
focus is on a term link. site/terms.js carries those, written by this tool; main.js loads it on pages that have a term
link. A term link is <a class="term" href="concepts.html#sip">SIP</a> (guide/concepts.html#.. from the site's root).

What gets linked: on each page and in each language, the first use of each term in the article's prose (first outside
folds, else the first inside one). Never inside code, pre, kbd, headings, summary, dt, th, existing links, buttons,
<i> (interface names), svg, navigation (the stage bar, prev/next), labels (.k, .status, .card-h, ..), .noindex
(diagrams, step strips, finders), screenshot captions or anything outside the article; never on concepts.html itself.
Text marked lang="en" / lang="tr" counts for that language; text with no language (shown in both) only when the term is
still unlinked in both. A Turkish suffix after an apostrophe stays outside the link (SIP'i -> <a>SIP</a>'i); Turkish
words that take a suffix without one are linked whole (sistem diskine). Idempotent: a page that already has a term link
for a term in a language keeps it and gets no second one. No dependencies.
"""
import html
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "site")

# id: (English patterns, Turkish patterns); within one term, the first pattern that matches at a position wins, so
# longer forms come first. Each match must stand alone: no letter, digit, ".", "-" or "/" right before it, and no
# letter, digit, "-", "/" or ".letter" (a file name) right after it.
TERMS = [
    ("opencore", [r"OpenCore(?! Legacy)"], [r"OpenCore(?! Legacy)"]),
    ("efi", [r"EFI"], [r"EFI"]),
    ("config-plist", [r"config\.plist"], [r"config\.plist"]),
    ("kext", [r"[Kk]exts?"], [r"[Kk]ext"]),
    ("sip", [r"SIP"], [r"SIP"]),
    ("amfi", [r"AMFI"], [r"AMFI"]),
    ("boot-args", [r"boot-args?"], [r"boot-args?"]),
    ("quirks", [r"quirks?"], [r"quirk"]),
    ("ssdt", [r"SSDT-ARCOFF(?:\.aml)?", r"SSDTs?"], [r"SSDT-ARCOFF(?:\.aml)?", r"SSDT"]),
    ("rebar", [r"Resizable BAR", r"Above 4G [Dd]ecoding", r"BAR"], [r"Resizable BAR", r"Above 4G [Dd]ecoding", r"BAR"]),
    ("system-volume", [r"(?:sealed )?system[- ]volume"], [r"(?:mühürlü )?sistem disk\w*"]),
    ("metal-plugin", [r"Metal (?:driver|plugin)"], [r"Metal (?:sürücüs|eklentis)\w*"]),
    ("boot-guard", [r"boot guard"], [r"[Aa]çılış korumas\w*"]),
    ("stages", [r"[Ss]tages? [1-4](?:(?:, | and | or | to |-|–)[1-4])*(?![.–-]\d)", r"four stages"],
               [r"(?<!scan-out )(?<!scan-out'un )[1-4]\.(?:(?:, | ve | ya da | ile |-|–)[1-4]\.)* aşama\w*", r"dört aşama\w*"]),
    ("manager", [r"IntelXe Manager(?: for Windows)?"], [r"(?:Windows için )?IntelXe Manager"]),
]
# pages where a term's words mean something else: XeProbe's own stages; nullmoth's original Metal driver for NVIDIA
# bold app page names that contain a term but name the page, not the concept
UI_NAMES = {"EFI (OpenCore)", "EFI"}
PAGE_SKIP = {"stages": {"support.html"}, "metal-plugin": {"credits.html"}}
LEFT, RIGHT = r"(?<![\w.\-/])", r"(?![\w/\-]|\.\w)"
PATTERNS = {tid: {"en": re.compile(LEFT + "(?:" + "|".join(en) + ")" + RIGHT),
                  "tr": re.compile(LEFT + "(?:" + "|".join(tr) + ")" + RIGHT)} for tid, en, tr in TERMS}

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}
SKIP_TAGS = {"code", "pre", "kbd", "samp", "var", "h1", "h2", "h3", "h4", "h5", "h6", "summary", "dt", "th", "a",
             "button", "i", "svg", "nav", "header", "footer", "aside", "script", "style", "title", "textarea", "select",
             "option", "label", "math"}
SKIP_CLASSES = {"noindex", "stagebar", "doc-pn", "eyebrow", "k", "status", "card-h", "expect-h", "checks-h", "fix-h",
                "shot-theme", "term", "doc-toc", "tab-list", "jm-head", "jm-chips", "jm-label", "jm-time", "badge"}
TAG = re.compile(r"<!--.*?-->|<![^>]*>|<(?:[^>\"']|\"[^\"]*\"|'[^']*')*>", re.S)
ATTR = re.compile(r"([\w:.-]+)(?:\s*=\s*(\"[^\"]*\"|'[^']*'|[^\s>]+))?")
NAME = re.compile(r"</?\s*([A-Za-z][\w:-]*)")


def tokenize(s):
    """[(kind, text)]: kind is "tag" or "text"; "".join(texts) == s. script/style bodies are one "raw" token."""
    out, i, n = [], 0, len(s)
    while i < n:
        j = s.find("<", i)
        if j == -1:
            out.append(("text", s[i:]))
            break
        if j > i:
            out.append(("text", s[i:j]))
        m = TAG.match(s, j)
        if not m:
            out.append(("text", "<"))
            i = j + 1
            continue
        tag = m.group(0)
        out.append(("tag", tag))
        i = m.end()
        nm = NAME.match(tag)
        if nm and not tag.startswith("</") and nm.group(1).lower() in ("script", "style"):
            end = s.lower().find("</" + nm.group(1).lower(), i)
            end = n if end == -1 else end
            if end > i:
                out.append(("raw", s[i:end]))
            i = end
    return out


def attrs(tag):
    body = re.sub(r"^<\s*[\w:-]+", "", tag).rstrip(">").rstrip("/")
    a = {}
    for m in ATTR.finditer(body):
        v = m.group(2) or ""
        if v[:1] in "\"'":
            v = v[1:-1]
        a[m.group(1).lower()] = html.unescape(v)
    return a


def walk(tokens):
    """For each token: (index, kind, text, stack) where stack is the list of open elements [(tag, attrs)]."""
    stack = []
    for i, (kind, text) in enumerate(tokens):
        if kind != "tag" or text.startswith("<!"):
            yield i, kind, text, stack
            continue
        m = NAME.match(text)
        if not m:
            continue
        name = m.group(1).lower()
        if text.startswith("</"):
            for k in range(len(stack) - 1, -1, -1):
                if stack[k][0] == name:
                    del stack[k:]
                    break
            yield i, kind, text, stack
            continue
        a = attrs(text)
        yield i, kind, text, stack + [(name, a)]
        if name not in VOID and not text.endswith("/>"):
            stack.append((name, a))


def context(stack):
    """(eligible, language or None for both, inside a fold) for text under this stack."""
    in_article = in_fold = False
    lang = None
    figure_shot = False
    for name, a in stack:
        cls = set(a.get("class", "").split())
        if name == "article" and "doc-body" in cls:
            in_article = True
        if name in SKIP_TAGS or cls & SKIP_CLASSES:
            return False, None, False
        if name == "figure" and cls & {"shot", "gal"}:
            figure_shot = True
        if name == "figcaption" and figure_shot:
            return False, None, False
        if name == "details":
            in_fold = True
        if name != "html" and a.get("lang") in ("en", "tr"):
            lang = a["lang"]
    return in_article, lang, in_fold


def href_for(page):
    rel = os.path.relpath(os.path.join(SITE, "guide", "concepts.html"), os.path.dirname(os.path.abspath(page)))
    return rel.replace(os.sep, "/")


def plan(page):
    """The links to add to one page: [(token index, start, end, term id, language or "both")]."""
    with open(page, encoding="utf-8") as fh:
        tokens = tokenize(fh.read())
    done = {"en": set(), "tr": set()}
    texts = []
    for i, kind, text, stack in walk(tokens):
        if kind == "tag" and not text.startswith("</") and NAME.match(text) and NAME.match(text).group(1).lower() == "a":
            a = stack[-1][1]
            if "term" in a.get("class", "").split() and "#" in a.get("href", ""):
                tid = a["href"].split("#", 1)[1]
                lang = next((x.get("lang") for _, x in reversed(stack[:-1]) if x.get("lang") in ("en", "tr")
                             and _ != "html"), None)
                for l in ((lang,) if lang else ("en", "tr")):
                    done[l].add(tid)
        elif kind == "text":
            ok, lang, fold = context(stack)
            if ok and stack and stack[-1][0] == "b" and text.strip().rstrip(":") in UI_NAMES:
                ok = False   # an app page's name in bold (EFI (OpenCore)), not the concept
            if ok and text.strip():
                texts.append((i, text, lang, fold))
    chosen = []          # (token index, start, end, term id, lang)
    spans = {}           # token index -> [(start, end)]
    for want_fold in (False, True):
        for i, text, lang, fold in texts:
            if fold != want_fold:
                continue
            langs = (lang,) if lang else ("en", "tr")
            for tid, _, _ in TERMS:
                if any(tid in done[l] for l in langs) or os.path.basename(page) in PAGE_SKIP.get(tid, ()):
                    continue
                pat = PATTERNS[tid][langs[0]]
                for m in pat.finditer(text):
                    if lang is None and not PATTERNS[tid]["tr"].fullmatch(m.group(0)):
                        continue
                    s, e = m.span()
                    taken = spans.get(i, [])
                    # not inside another link, and not right beside one (two links with only a space between)
                    if any(s < te and ts < e for ts, te in taken):
                        continue
                    if any(text[te:s].strip() == "" and te <= s or text[e:ts].strip() == "" and e <= ts
                           for ts, te in taken):
                        continue
                    spans.setdefault(i, []).append((s, e))
                    chosen.append((i, s, e, tid, lang or "both"))
                    for l in langs:
                        done[l].add(tid)
                    break
    return tokens, chosen


def apply(tokens, chosen, href):
    by_tok = {}
    for i, s, e, tid, _ in chosen:
        by_tok.setdefault(i, []).append((s, e, tid))
    out = []
    for i, (kind, text) in enumerate(tokens):
        if i in by_tok:
            for s, e, tid in sorted(by_tok[i], reverse=True):
                text = text[:s] + '<a class="term" href="%s#%s">%s</a>' % (href, tid, text[s:e]) + text[e:]
        out.append(text)
    return "".join(out)


# ---- the tips' data: concepts.html -> site/terms.js ----

def strip(s):
    return re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", s))).strip()


def first_sentence(text):
    """Up to the first . ! or ? followed by white space or the end; a lone digit and its dot (a Turkish ordinal:
    "2. aşama") does not end it. (Write no abbreviation with a dot, such as "e.g.", in an entry's first sentence.)"""
    for m in re.finditer(r"[.!?](?=\s|$)", text):
        k = m.start()
        if text[k] == "." and k >= 1 and text[k - 1].isdigit() and (k < 2 or not text[k - 2].isalnum()):
            continue
        return text[:k + 1]
    return text


def concepts(site):
    with open(os.path.join(site, "guide", "concepts.html"), encoding="utf-8") as fh:
        s = fh.read()
    s = s[s.index('<article class="doc-body">'):]
    terms = {}
    for m in re.finditer(r'<section id="([\w-]+)">(.*?)</section>', s, re.S):
        sid, body = m.group(1), m.group(2)
        h = re.search(r"<h2>(.*?)</h2>", body, re.S).group(1)
        t = [strip((re.search(r'<span lang="%s">(.*?)</span>' % l, h, re.S) or re.search(r"(.*)", h, re.S)).group(1))
             for l in ("en", "tr")]
        d = []
        for l in ("en", "tr"):
            p = re.search(r'<p lang="%s">(.*?)</p>' % l, body, re.S)
            d.append(first_sentence(strip(p.group(1))) if p else "")
        terms[sid] = {"t": t, "d": d}
    return terms


def terms_js(site):
    body = json.dumps({"v": 1, "terms": concepts(site)}, ensure_ascii=False, separators=(",", ":"))
    return ("// The guide's term tips, written by tools/site-terms.py from site/guide/concepts.html (the first sentence"
            " of each entry). MIT.\nwindow.SITE_TERMS = " + body + ";\n")


def main():
    global SITE
    args = sys.argv[1:]
    if "--site" in args:
        k = args.index("--site")
        SITE = os.path.abspath(args[k + 1])
        del args[k:k + 2]
    check = "--check" in args
    data_only = "--data" in args
    files = [a for a in args if not a.startswith("--")]
    problems = 0

    known = concepts(SITE)
    ids = [t[0] for t in TERMS]
    for tid in ids:
        if tid not in known:
            problems += 1
            print("concepts.html has no section for the term %s" % tid)
    for tid in known:
        if tid not in ids:
            print("note: concepts.html#%s has no patterns here (no links are added for it)" % tid)
    for tid, v in known.items():
        for l, d in zip(("en", "tr"), v["d"]):
            if not d:
                problems += 1
                print("concepts.html#%s has no <p lang=\"%s\">" % (tid, l))

    out_js = os.path.join(SITE, "terms.js")
    want = terms_js(SITE)
    try:
        with open(out_js, encoding="utf-8") as fh:
            stale = fh.read() != want
    except FileNotFoundError:
        stale = True
    if stale and check:
        problems += 1
        print("site/terms.js is stale: python3 tools/site-terms.py --data")
    elif stale:
        with open(out_js, "w", encoding="utf-8") as fh:
            fh.write(want)
        print("wrote site/terms.js (%d terms)" % len(known))
    if data_only:
        sys.exit(1 if problems else 0)

    guide = os.path.join(SITE, "guide")
    if files:
        pages = [f if os.path.exists(f) else os.path.join(guide, f) for f in files]
    else:
        pages = [os.path.join(guide, f) for f in sorted(os.listdir(guide)) if f.endswith(".html")]
    added = 0
    for page in pages:
        if os.path.basename(page) == "concepts.html":
            continue
        tokens, chosen = plan(page)
        if not chosen:
            continue
        added += len(chosen)
        name = os.path.relpath(page, SITE)
        if check:
            for i, s, e, tid, lang in sorted(chosen, key=lambda c: (c[4], c[0], c[1])):
                text = tokens[i][1]
                ctx = re.sub(r"\s+", " ", text[max(0, s - 40):s] + "[" + text[s:e] + "]" + text[e:e + 30])
                print("%s %s %-13s %s" % (name, lang, tid, ctx.strip()))
        else:
            with open(page, "w", encoding="utf-8") as fh:
                fh.write(apply(tokens, chosen, href_for(page)))
            print("%s: %d term link(s)" % (name, len(chosen)))
    if check:
        print("site-terms: %d link(s) to add%s" % (added, ", %d problem(s)" % problems if problems else ""))
        sys.exit(1 if added or problems else 0)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
