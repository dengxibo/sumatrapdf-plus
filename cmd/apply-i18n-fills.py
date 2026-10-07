#!/usr/bin/env python3
"""Merge cmd/_fills_part_*.json into translations and rebuild lzsa."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAJORS = ["cn", "tw", "ja", "kr", "de", "fr", "es", "it", "pt", "br", "ru", "uk", "pl", "nl", "tr", "vn"]


def unescape_txt(s: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "n":
                out.append("\n")
            elif n == "t":
                out.append("\t")
            elif n == "r":
                out.append("\r")
            elif n == "\\":
                out.append("\\")
            else:
                out.append(n)
            i += 2
            continue
        out.append(s[i])
        i += 1
    return "".join(out)


def escape_txt(s: str) -> str:
    return (
        s.replace("\\", "\\\\")
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def parse_blocks(text: str):
    lines = text.splitlines()
    header, blocks, i = [], {}, 0
    while i < len(lines) and not lines[i].startswith(":"):
        header.append(lines[i])
        i += 1
    key, cur = None, {}
    for line in lines[i:]:
        if line.startswith(":"):
            if key is not None:
                blocks[key] = cur
            key, cur = unescape_txt(line[1:]), {}
        elif key is not None and ":" in line:
            lang, val = line.split(":", 1)
            if lang and all(c.isalnum() or c in "-_" for c in lang) and len(lang) <= 8:
                cur[lang] = unescape_txt(val)
    if key is not None:
        blocks[key] = cur
    return header, blocks


def serialize(header, blocks):
    out = list(header)
    while len(out) < 2:
        out.append("AppTranslator: SumatraPDF")
    for key in sorted(blocks.keys(), key=lambda s: s.casefold()):
        out.append(":" + escape_txt(key))
        for lang in sorted(blocks[key].keys()):
            out.append(f"{lang}:{escape_txt(blocks[key][lang])}")
    return "\n".join(out) + "\n"


def merge_entry(dst: dict, src: dict, key: str) -> None:
    """Prefer non-English-placeholder values; never wipe a good translation with a worse one."""
    for lang, val in src.items():
        if not val:
            continue
        old = dst.get(lang)
        if old is None or is_english_placeholder(key, old):
            dst[lang] = val
        elif is_english_placeholder(key, val):
            continue
        else:
            # Keep existing good translation unless new is also good and different —
            # later files still win when both are real translations.
            dst[lang] = val


def load_fills() -> dict:
    fills: dict = {}
    paths = sorted((ROOT / "cmd").glob("_fills_part_*.json"))
    # Seed first (lowest priority), then everything else alphabetically.
    paths = [p for p in paths if "seed" in p.name] + [
        p for p in paths if "seed" not in p.name and "partial" not in p.name
    ]
    for path in paths:
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise SystemExit(f"bad fills file {path}")
        for key, langs in data.items():
            if not isinstance(langs, dict):
                continue
            cur = dict(fills.get(key, {}))
            merge_entry(cur, langs, key)
            fills[key] = cur
        print(f"loaded {path.name}: {len(data)} keys")
    return fills


def is_english_placeholder(key: str, val: str) -> bool:
    if not val:
        return True
    if val == key:
        return True
    k, v = key.replace("&", ""), val.replace("&", "")
    return k == v


def upsert(path: Path, fills: dict) -> int:
    header, blocks = parse_blocks(path.read_text(encoding="utf-8"))
    changed = 0
    for key, langs in fills.items():
        cur = dict(blocks.get(key, {}))
        touched = False
        for lang in MAJORS:
            val = langs.get(lang)
            if not val:
                continue
            old = cur.get(lang)
            if old is None or is_english_placeholder(key, old) or old != val:
                # Always overwrite EN placeholders; otherwise only if missing/different fill
                if old is None or is_english_placeholder(key, old) or (lang in langs and old != val):
                    if old != val:
                        cur[lang] = val
                        touched = True
        if touched:
            blocks[key] = cur
            changed += 1
    path.write_text(serialize(header, blocks), encoding="utf-8")
    return changed


def main() -> int:
    fills = load_fills()
    if not fills:
        print("no fill files found")
        return 1
    for name in ("translations-good.txt", "translations.txt"):
        n = upsert(ROOT / "translations" / name, fills)
        print(f"Updated {name}: {n} keys")
    r = subprocess.run(["bun", str(ROOT / "cmd" / "sync-major-langs-to-good.ts")], cwd=ROOT)
    return r.returncode


if __name__ == "__main__":
    raise SystemExit(main())
