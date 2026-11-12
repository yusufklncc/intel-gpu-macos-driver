# SPDX-License-Identifier: MIT
"""Pinned downloads: every file the build fetches is named in downloads.lock.json with its SHA-256.

A download goes to the cache under its SHA-256 and is used only when its bytes have that digest, each time it is read.
A URL that the lock does not name is refused: the build never fetches "the latest" of anything.
"""
import hashlib
import json
import os
import shutil
import ssl
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # windows/efibuild
LOCK_PATH = os.path.join(HERE, "downloads.lock.json")
PINNED_PATH = os.path.join(HERE, "upstream", "PINNED.json")
USER_AGENT = "intelxe-efibuild"
TIMEOUT = 60


class PinError(RuntimeError):
    """A download that is not pinned, or whose bytes do not match the pin."""


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def default_cache():
    """%LOCALAPPDATA%\\IntelXe\\efibuild on Windows, ~/.cache/intelxe-efibuild elsewhere; INTELXE_EFIBUILD_CACHE wins."""
    if os.environ.get("INTELXE_EFIBUILD_CACHE"):
        return os.environ["INTELXE_EFIBUILD_CACHE"]
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        return os.path.join(os.environ["LOCALAPPDATA"], "IntelXe", "efibuild")
    return os.path.join(os.path.expanduser("~"), ".cache", "intelxe-efibuild")


def _ssl_context():
    context = ssl.create_default_context()
    try:
        import certifi  # optional: the embedded Python on Windows carries it
        context.load_verify_locations(cafile=certifi.where())
    except ImportError:
        pass
    return context


def http_get(url, timeout=TIMEOUT, tries=4, headers=None):
    """The body of an https URL, retried on network errors. Only https is accepted."""
    if not url.startswith("https://"):
        raise PinError("only https downloads are allowed: " + url)
    last = None
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers=dict({"User-Agent": USER_AGENT}, **(headers or {})))
            with urllib.request.urlopen(req, timeout=timeout, context=_ssl_context()) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            if isinstance(e, urllib.error.HTTPError) and e.code in (403, 404, 410):
                raise PinError("{} answered {}".format(url, e.code)) from e
            last = e
            time.sleep(2 * (attempt + 1))
    raise PinError("could not download {}: {}".format(url, last))


class Cache:
    """Files by SHA-256 under <root>/sha256/<digest>."""

    def __init__(self, root=None):
        self.root = root or default_cache()
        os.makedirs(os.path.join(self.root, "sha256"), exist_ok=True)

    def path(self, digest):
        return os.path.join(self.root, "sha256", digest.lower())

    def fetch(self, url, digest):
        """A local path holding the bytes of url, checked against digest (now, not only when downloaded)."""
        if not (isinstance(digest, str) and len(digest) == 64):
            raise PinError("no SHA-256 pinned for " + url)
        digest = digest.lower()
        path = self.path(digest)
        if os.path.isfile(path) and not os.path.islink(path) and sha256_file(path) == digest:
            return path
        data = http_get(url)
        got = sha256_bytes(data)
        if got != digest:
            raise PinError("{}: SHA-256 {} is not the pinned {}".format(url, got, digest))
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path))
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
        return path

    def read(self, url, digest):
        path = self.fetch(url, digest)
        with open(path, "rb") as f:
            data = f.read()
        if sha256_bytes(data) != digest.lower():
            raise PinError("the cached copy of {} changed".format(url))
        return data


class Lock:
    """downloads.lock.json: products (what OpCore Simplify downloads by product name), files (other URLs it fetches,
    mapped to pinned URLs) and tools (ocvalidate and macserial inside the pinned OpenCorePkg zip)."""

    def __init__(self, path=LOCK_PATH):
        self.path = path
        self.data = load_json(path) if os.path.exists(path) else {
            "SPDX-License-Identifier": "MIT", "format": "intelxe-efibuild-lock", "format_version": 1,
            "products": {}, "files": {}, "tools": {}}
        if self.data.get("format") != "intelxe-efibuild-lock" or self.data.get("format_version") != 1:
            raise PinError("{} is not an intelxe-efibuild-lock version 1".format(path))

    @property
    def products(self):
        return self.data["products"]

    @property
    def files(self):
        return self.data["files"]

    @property
    def tools(self):
        return self.data["tools"]

    def pin_for_url(self, url):
        """(url, sha256, entry) to fetch for a URL the engine asks for, or None when it is not pinned."""
        for entry in self.products.values():
            if entry.get("url") == url:
                return entry["url"], entry["sha256"], entry
        entry = self.files.get(url)
        if entry:
            return entry["url"], entry["sha256"], entry
        for entry in self.files.values():
            if entry.get("url") == url:
                return entry["url"], entry["sha256"], entry
        return None

    def save(self):
        self.data["products"] = dict(sorted(self.products.items()))
        self.data["files"] = dict(sorted(self.files.items()))
        order = ("SPDX-License-Identifier", "format", "format_version", "about", "products", "files", "tools", "windows_tools")
        self.data = dict(sorted(self.data.items(), key=lambda kv: order.index(kv[0]) if kv[0] in order else len(order)))
        with open(self.path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.data, f, indent=2, ensure_ascii=False)
            f.write("\n")


def copy_verified(src, dst, digest):
    shutil.copyfile(src, dst)
    if sha256_file(dst) != digest.lower():
        os.remove(dst)
        raise PinError("copy of {} does not match its pin".format(os.path.basename(src)))
