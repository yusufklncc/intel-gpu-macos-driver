#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""site-links.py: checks the project site's internal links (site/*.html and site/guide/*.html).

    python3 tools/site-links.py            list every broken link; exit 1 if any
    python3 tools/site-links.py --site DIR another copy of site/

For every href and src that is not external (http:, https:, mailto:, data:, javascript:), the file it names must exist
under site/, and its #fragment, when it has one, must be an id (or <a name>) in that file. A link to "#" alone or a
fragment only a script makes would be listed too. No dependencies.
"""
import html.parser
import os
import sys
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTERNAL = ("http:", "https:", "mailto:", "data:", "javascript:", "tel:")


class Page(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.ids, self.links = set(), []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if a.get("id"):
            self.ids.add(a["id"])
        if tag == "a" and a.get("name"):
            self.ids.add(a["name"])
        for k in ("href", "src"):
            if a.get(k) and tag != "base":
                self.links.append((self.getpos()[0], a[k]))

    handle_startendtag = handle_starttag


def pages(site):
    out = []
    for d in ("", "guide"):
        base = os.path.join(site, d)
        out += [os.path.join(base, f) for f in sorted(os.listdir(base)) if f.endswith(".html")]
    return out


def main():
    site = os.path.join(ROOT, "site")
    if "--site" in sys.argv:
        site = os.path.abspath(sys.argv[sys.argv.index("--site") + 1])
    parsed = {}

    def get(path):
        if path not in parsed:
            p = Page()
            with open(path, encoding="utf-8") as fh:
                p.feed(fh.read())
            parsed[path] = p
        return parsed[path]

    bad = checked = 0
    for path in pages(site):
        for line, link in get(path).links:
            if link.startswith(EXTERNAL) or link.startswith("//"):
                continue
            checked += 1
            u = urllib.parse.urlsplit(link)
            target = path if not u.path else os.path.normpath(os.path.join(os.path.dirname(path),
                                                                           urllib.parse.unquote(u.path)))
            if os.path.isdir(target):
                target = os.path.join(target, "index.html")
            rel = os.path.relpath(path, site)
            if not os.path.exists(target):
                bad += 1
                print("%s:%d: %s: no such file" % (rel, line, link))
                continue
            if u.fragment and target.endswith(".html") and u.fragment not in get(target).ids:
                bad += 1
                print("%s:%d: %s: no id \"%s\" in %s" % (rel, line, link, u.fragment, os.path.relpath(target, site)))
    print("site links: %d checked, %s" % (checked, "all resolve" if not bad else "%d broken" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
