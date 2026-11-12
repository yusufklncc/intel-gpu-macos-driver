# SPDX-License-Identifier: MIT
"""Writes the fixtures of fixtures/fixtures.json into a work folder, one folder per fixture:
<work>/<name>/Report.json and <work>/<name>/ACPI/. Public files are fetched at their pinned commit and checked by
SHA-256; nothing fetched is written into the repository.

  python3 -I tests/fetch_fixtures.py WORK_DIR [NAME ...]
"""
import copy
import json
import os
import shutil
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from intelxe_efibuild import pins  # noqa: E402

FIXTURES = os.path.join(HERE, "fixtures", "fixtures.json")


def raw_url(source, path):
    repo = urllib.parse.urlsplit(source["repository"]).path.strip("/")
    return "https://raw.githubusercontent.com/{}/{}/{}".format(repo, source["commit"], urllib.parse.quote(path))


def materialize(work, names=None, cache=None):
    data = pins.load_json(FIXTURES)
    if data.get("format") != "intelxe-efibuild-fixtures":
        raise SystemExit("unknown fixtures format")
    cache = cache or pins.Cache()
    done = []
    for fx in data["fixtures"]:
        if names and fx["name"] not in names:
            continue
        dest = os.path.join(work, fx["name"])
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.makedirs(os.path.join(dest, "ACPI"))
        rep = fx["report"]
        if "file" in rep:
            with open(os.path.join(HERE, "fixtures", rep["file"]), encoding="utf-8") as f:
                report = json.load(f)
        else:
            src = data["sources"][rep["source"]]
            report = json.loads(cache.read(raw_url(src, src["report"]["path"]), src["report"]["sha256"]).decode("utf-8"))
        report = copy.deepcopy(report)
        for key, value in rep.get("replace", {}).items():
            report[key] = value
        with open(os.path.join(dest, "Report.json"), "w", encoding="utf-8") as f:
            json.dump(report, f, indent=4, ensure_ascii=False)
        src = data["sources"][fx["acpi"]]
        for table in src["acpi"]:
            body = cache.read(raw_url(src, table["path"]), table["sha256"])
            with open(os.path.join(dest, "ACPI", os.path.basename(table["path"])), "wb") as f:
                f.write(body)
        done.append(fx)
    return done


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for fx in materialize(sys.argv[1], sys.argv[2:] or None):
        print("{}\t{}\t{}".format(fx["name"], fx.get("macos", "26"), os.path.join(sys.argv[1], fx["name"])))
