# SPDX-License-Identifier: MIT
"""release/templates/efi-rules.json read in Python: the cards, the CPU names and the "settings" (stage 2) and "safari"
items, planned and applied to a config.plist held by plistlib.

This is the fourth reader of the rules, after efi.sh, IntelXe-EFI.ps1 and the Windows app; like them it passes the
shared vectors of its plans (release/efi-rules-vectors.json, the stage-2 and Safari ones: efibuild.py vectors).
Stage 4 (IntelXe.kext, SSDT-ARCOFF, HybridPM) is not implemented here: it happens after macOS is installed, through
efi.sh, IntelXe-EFI.ps1 or the app, which read the same file.
"""
import base64
import copy
import json
import os
import plistlib
import re

from . import pins

REPO = os.path.dirname(os.path.dirname(pins.HERE))   # the repository root (windows/efibuild/../..)
# INTELXE_EFI_RULES, else the copy the Windows app's engine carries beside the package (build.ps1 puts
# release/templates/efi-rules.json at engine\efibuild\efi-rules.json: the published app has no release\ folder), else the
# repository's
_BUNDLED = os.path.join(pins.HERE, "efi-rules.json")
RULES_PATH = (os.environ.get("INTELXE_EFI_RULES")
              or (_BUNDLED if os.path.isfile(_BUNDLED) else os.path.join(REPO, "release", "templates", "efi-rules.json")))
VECTORS_PATH = os.path.join(REPO, "release", "efi-rules-vectors.json")


class RulesError(RuntimeError):
    pass


def load(path=None):
    path = path or RULES_PATH
    with open(path, encoding="utf-8") as f:
        rules = json.load(f)
    if rules.get("format") != "intelxe-efi-rules" or rules.get("format_version") != 1:
        raise RulesError("{} is not intelxe-efi-rules version 1".format(path))
    return rules


# ---- facts ------------------------------------------------------------------------------------------------------------

def is_card(rules, device_id):
    """device_id as Hardware Sniffer writes it ("8086-E20B"): one of the cards the driver supports."""
    vendor, _, device = (device_id or "").lower().partition("-")
    return vendor == rules["cards"]["vendor_id"] and device in rules["cards"]["device_ids"]


def needs_hybridpm(rules, cpu_name):
    h = rules["hybridpm"]
    return any(re.search(p, cpu_name) for p in h["cpu_include"]) and not any(re.search(p, cpu_name) for p in h["cpu_exclude"])


# ---- key paths --------------------------------------------------------------------------------------------------------

def _step(node, key):
    if isinstance(node, list):
        return node[int(key)] if key.isdigit() and int(key) < len(node) else None
    if isinstance(node, dict):
        return node.get(key)
    return None


def get(config, path):
    node = config
    for key in path.split("."):
        node = _step(node, key)
        if node is None:
            return None
    return node


def exists(config, path):
    return get(config, path) is not None


def raw(value):
    """A value as `plutil -extract KEY raw` prints it, which is what the rules compare with."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (bytes, bytearray)):
        return base64.b64encode(bytes(value)).decode()
    return str(value)


def show(path):
    return path.replace(".", " > ")


def typed(kind, value):
    if kind == "integer":
        return int(value)
    if kind == "bool":
        return value if isinstance(value, bool) else str(value) == "true"
    if kind == "string":
        return str(value)
    if kind == "data":
        return value if isinstance(value, str) else base64.b64encode(bytes(value)).decode()
    raise RulesError("unknown type " + kind)


def _csr_value(data):
    b = bytes(data or b"") + b"\0\0\0\0"
    return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24


def _csr_data(value):
    return base64.b64encode(bytes((value & 255, value >> 8 & 255, value >> 16 & 255, value >> 24 & 255))).decode()


# ---- plans ------------------------------------------------------------------------------------------------------------

def plan_items(rules, config, list_name):
    """(ops, lines, notes) for one list of items ("settings" or "safari"), as efi.sh's plan_items makes them."""
    ops, lines, notes = [], [], []
    items = rules.get(list_name)
    if not isinstance(items, list):
        raise RulesError("the rules have no list " + list_name)
    for item in items:
        kind, path, why = item.get("kind"), item.get("path"), item.get("why", "")
        tail = " ({})".format(why) if why else ""
        if kind in ("value", "value_if"):
            cur = raw(get(config, path))
            value = raw(item["value"])
            if kind == "value_if":
                if cur != raw(item["if"]):
                    continue
            elif cur == value:
                continue
            lines.append("config   {}: {} -> {}{}".format(show(path), cur or "missing", value, tail))
            ops.append({"op": "set", "path": path, "type": item["type"], "value": typed(item["type"], item["value"])})
        elif kind == "require":
            if not exists(config, path):
                notes.append(item["note"])
                return ops, lines, notes
        elif kind == "dict":
            if not exists(config, path):
                lines.append("config   {}: add (missing)".format(show(path)))
                ops.append({"op": "mkdict", "path": path})
        elif kind == "bits":
            bits = int(item["bits"], 0)
            cur = get(config, path)
            if item["type"] == "data":
                v = _csr_value(cur if isinstance(cur, (bytes, bytearray)) else b"")
            elif item["type"] == "integer":
                v = cur if isinstance(cur, int) and not isinstance(cur, bool) and cur >= 0 else 0
            else:
                raise RulesError("bits of a " + item["type"])
            if v & bits == bits:
                continue
            lines.append("config   {}: {} -> {}{}".format(show(path), hex(v), hex(v | bits), tail))
            if item["type"] == "data":
                ops.append({"op": "set", "path": path, "type": "data", "value": _csr_data(v | bits)})
            else:
                ops.append({"op": "set", "path": path, "type": "integer", "value": v | bits})
        elif kind == "boot_args":
            cur = raw(get(config, path))
            add = [a["add"] for a in item["args"] if not re.search(a["unless"], " " + cur + " ")]
            if not add:
                continue
            value = (cur + " " + " ".join(add)).lstrip(" ")
            lines.append("config   {}: add {} ('{}' -> '{}')".format(show(path), " ".join(add), cur, value))
            ops.append({"op": "set", "path": path, "type": "string", "value": value})
        elif kind == "strings":
            have = get(config, path)
            if have is None:
                lines.append("config   {}: add (missing)".format(show(path)))
                ops.append({"op": "mkarray", "path": path})
                have = []
            for s in item["strings"]:
                if s not in have:
                    lines.append("config   {}: add {}{}".format(show(path), s, tail))
                    ops.append({"op": "append", "path": path, "type": "string", "value": s})
        else:
            raise RulesError("item kind '{}' is not known to this reader (a newer rules file?)".format(kind))
    return ops, lines, notes


PLANS = {"settings": ("settings",), "safari": ("safari",), "settings-safari": ("settings", "safari")}


def plan(rules, config, which):
    """The plan of stage 2 ("settings"), the Safari setting ("safari") or both ("settings-safari") on a config whose
    changes are made in order: each list sees the config as the earlier list leaves it, as efi.sh plans them."""
    ops, lines, notes = [], [], []
    work = copy.deepcopy(config)
    for name in PLANS[which]:
        o, l, n = plan_items(rules, work, name)
        apply(work, o)
        ops += o
        lines += l
        notes += n
    return ops, lines, notes


# ---- applying ---------------------------------------------------------------------------------------------------------

def _parent(config, path):
    keys = path.split(".")
    node = config
    for key in keys[:-1]:
        nxt = _step(node, key)
        if nxt is None:
            raise RulesError("no {} in config.plist".format(show(path.rsplit(".", 1)[0])))
        node = nxt
    return node, keys[-1]


def _value(op):
    t, v = op.get("type"), op.get("value")
    if t == "data":
        return base64.b64decode(v)
    if t == "integer":
        return int(v)
    if t == "bool":
        return bool(v)
    return v


def apply(config, ops):
    for op in ops:
        kind = op["op"]
        if kind == "remove":
            parent, key = _parent(config, op["path"])
            if isinstance(parent, list):
                del parent[int(key)]
            else:
                parent.pop(key, None)
            continue
        parent, key = _parent(config, op["path"])
        if kind == "set":
            if isinstance(parent, list):
                parent[int(key)] = _value(op)
            else:
                parent[key] = _value(op)
        elif kind == "mkdict":
            parent[key] = {}
        elif kind == "mkarray":
            parent[key] = []
        elif kind == "append":
            parent[key].append(_value(op))
        else:
            raise RulesError("unknown op " + kind)
    return config


def apply_plan(config_path, which, rules=None):
    """Plan and make stage 2 / the Safari setting in config_path. Returns (ops, lines, notes)."""
    rules = rules or load()
    with open(config_path, "rb") as f:
        config = plistlib.load(f)
    ops, lines, notes = plan(rules, config, which)
    apply(config, ops)
    with open(config_path, "wb") as f:
        plistlib.dump(config, f, sort_keys=False)
    with open(config_path, "rb") as f:
        again = plistlib.load(f)
    left, _, _ = plan(rules, again, which)
    if left:
        raise RulesError("config.plist still needs {} after the change".format(left))
    return ops, lines, notes


# ---- the shared vectors -----------------------------------------------------------------------------------------------

def run_vectors(vectors_path=VECTORS_PATH, rules_path=None):
    """The stage-2 and Safari vectors: their ops exactly, their notes by substring, their config values after."""
    with open(vectors_path, encoding="utf-8") as f:
        vectors = json.load(f)
    rules = load(rules_path or os.path.join(REPO, vectors["rules"]))
    with open(os.path.join(REPO, vectors["config"]), "rb") as f:
        base = plistlib.load(f)
    apply(base, vectors["stage1"])
    failures, count = [], 0
    for v in vectors["vectors"]:
        if v["plan"] not in PLANS:
            continue
        count += 1
        config = copy.deepcopy(base)
        apply(config, v.get("edits", []))
        ops, _, notes = plan(rules, config, v["plan"])
        want = v["expect"]
        if ops != want["ops"]:
            failures.append("{}: ops {} != {}".format(v["name"], ops, want["ops"]))
            continue
        wn = want.get("notes", [])
        if len(notes) != len(wn) or any(w not in n for w, n in zip(wn, notes)):
            failures.append("{}: notes {} != {}".format(v["name"], notes, wn))
            continue
        apply(config, ops)
        for c in want.get("after", {}).get("config", []):
            got = get(config, c["path"])
            if c.get("absent"):
                if got is not None:
                    failures.append("{}: {} should be absent".format(v["name"], c["path"]))
            elif raw(got) != raw(typed(c["type"], c["value"])) if c["type"] != "data" else raw(got) != c["value"]:
                failures.append("{}: {} = {!r}, want {!r}".format(v["name"], c["path"], raw(got), c["value"]))
        again, _, _ = plan(rules, config, v["plan"])
        if again:
            failures.append("{}: a second plan still changes {}".format(v["name"], again))
    return failures, count
