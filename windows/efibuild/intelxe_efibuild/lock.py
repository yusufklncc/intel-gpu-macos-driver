# SPDX-License-Identifier: MIT
"""Maintainer tool: fill downloads.lock.json from a build ("efibuild.py lock ...").

In lock mode OpCore Simplify resolves its downloads itself, as it would without the builder (Dortania's build list,
GitHub release pages, the URLs in its datasets), and the builder pins what the build actually used:

- a product that OpCore Simplify takes from Dortania's nightly build list is pinned to the newest GitHub release of
  the same repository instead (acidanthera/Lilu 1.7.x rather than a build of Lilu's latest commit), with the SHA-256
  GitHub publishes for the asset; OpenCorePkg likewise, so ocvalidate and macserial come from an OpenCore release;
- a file fetched from a branch (OcBinaryData's master.zip, patch lists on refs/heads) is pinned to the branch's
  current commit;
- every pinned file is downloaded once and its SHA-256 compared (or, where GitHub publishes none, recorded);
- ocvalidate and macserial for Windows, macOS and Linux are pinned by their SHA-256 inside the OpenCorePkg zip.

A normal build then uses only the lock. Review the diff of downloads.lock.json before committing it.
"""
import io
import json
import os
import re
import zipfile

from . import pins

DORTANIA = "https://raw.githubusercontent.com/dortania/build-repo/builds/latest.json"
API = "https://api.github.com/repos/"
TOOLS = {
    "ocvalidate": {"Darwin": "Utilities/ocvalidate/ocvalidate", "Windows": "Utilities/ocvalidate/ocvalidate.exe",
                   "Linux": "Utilities/ocvalidate/ocvalidate.linux"},
    "macserial": {"Darwin": "Utilities/macserial/macserial", "Windows": "Utilities/macserial/macserial.exe",
                  "Linux": "Utilities/macserial/macserial.linux"},
}

_dortania = None


def _api(path):
    headers = {"Accept": "application/vnd.github+json"}
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        headers["Authorization"] = "Bearer " + token
    return json.loads(pins.http_get(API + path, headers=headers))


def _dortania_repo(name):
    global _dortania
    if _dortania is None:
        _dortania = json.loads(pins.http_get(DORTANIA))
    try:
        url = _dortania[name]["versions"][0]["commit"]["url"]
    except (KeyError, IndexError, TypeError):
        return None
    m = re.match(r"https://github\.com/([^/]+)/([^/]+)/commit/", url)
    return (m.group(1), m.group(2)) if m else None


def _latest_release_asset(owner, repo):
    rel = _api("{}/{}/releases/latest".format(owner, repo))
    zips = [a for a in rel.get("assets", []) if a["name"].endswith(".zip")]
    pick = [a for a in zips if "RELEASE" in a["name"].upper()] or [a for a in zips if "DEBUG" not in a["name"].upper()]
    if len(pick) != 1:
        return None
    a = pick[0]
    digest = (a.get("digest") or "").partition("sha256:")[2] or None
    return {"url": a["browser_download_url"], "sha256": digest, "version": rel["tag_name"], "id": str(a["id"]),
            "source": "github release {}/{} {}".format(owner, repo, rel["tag_name"])}


def resolve_products(products):
    """OpCore Simplify's products, with Dortania nightlies swapped for the repository's latest release."""
    out = {}
    for name, p in products.items():
        p = dict(p)
        if "dortania/build-repo" in (p.get("url") or ""):
            repo = _dortania_repo(name)
            rel = _latest_release_asset(*repo) if repo else None
            if rel:
                p.update(rel)
            else:
                p["source"] = "dortania build " + p.get("url", "")
        else:
            p.setdefault("source", "as OpCore Simplify resolves it")
        out[name] = p
    return out


def _branch_head(owner, repo, branch):
    return _api("{}/{}/commits/{}".format(owner, repo, branch))["sha"]


def pin_file(lock, url):
    """Pin a branch URL to the branch's current commit; None for URLs that are not files on a branch."""
    m = re.match(r"https://github\.com/([^/]+)/([^/]+)/archive/refs/heads/([^/]+)\.zip$", url)
    if m:
        owner, repo, branch = m.groups()
        sha = _branch_head(owner, repo, branch)
        pinned = "https://github.com/{}/{}/archive/{}.zip".format(owner, repo, sha)
        data = pins.http_get(pinned)
        entry = {"url": pinned, "sha256": pins.sha256_bytes(data), "commit": sha, "top_folder": "{}-{}".format(repo, branch),
                 "source": "{}/{} {} at {}".format(owner, repo, branch, sha)}
        lock.files[url] = entry
        return entry["url"], entry["sha256"], entry
    m = (re.match(r"https://raw\.githubusercontent\.com/([^/]+)/([^/]+)/refs/heads/([^/]+)/(.+)$", url) or
         re.match(r"https://github\.com/([^/]+)/([^/]+)/raw/refs/heads/([^/]+)/(.+)$", url))
    if m:
        owner, repo, branch, path = m.groups()
        sha = _branch_head(owner, repo, branch)
        pinned = "https://raw.githubusercontent.com/{}/{}/{}/{}".format(owner, repo, sha, path)
        data = pins.http_get(pinned)
        entry = {"url": pinned, "sha256": pins.sha256_bytes(data), "commit": sha,
                 "source": "{}/{} {} at {}".format(owner, repo, branch, sha)}
        lock.files[url] = entry
        return entry["url"], entry["sha256"], entry
    return None


def pin_download(lock, used, url, cache):
    """A product download in lock mode: fetched, checked against the SHA-256 its source gives (if any), kept in the
    cache by digest and written into the lock. OpenCorePkg also pins its tools, which the same build needs."""
    name, p = next(((n, q) for n, q in used.items() if q.get("url") == url), (None, None))
    if name is None:
        return None
    data = pins.http_get(url)
    got = pins.sha256_bytes(data)
    if p.get("sha256") and p["sha256"].lower() != got:
        raise pins.PinError("{}: {} has SHA-256 {}, its source says {}".format(name, url, got, p["sha256"]))
    with open(cache.path(got), "wb") as f:
        f.write(data)
    entry = {"url": url, "sha256": got, "id": str(p.get("id", "")), "source": p.get("source", ""),
             "sha256_from": "the source" if p.get("sha256") else "computed when pinned"}
    if p.get("version"):
        entry["version"] = p["version"]
    lock.products[name] = entry
    if name == "OpenCorePkg":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for tool, members in TOOLS.items():
                lock.tools[tool] = {os_name: {"member": m, "sha256": pins.sha256_bytes(z.read(m))}
                                    for os_name, m in members.items()}
    return url, got, entry


def record(lock, used, cache):
    """Keep only the products this build used (and the lock's earlier ones), and write the lock."""
    missing = [n for n in used if n not in lock.products]
    if missing:
        raise pins.PinError("used but not downloaded, so not pinned: " + ", ".join(missing))
    lock.save()
