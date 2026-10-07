# Merges Wikidata given/family names that have Chinese labels into SumatraPDF's
# existing English offline dictionary. Append-only; no IPA/audio rewrite.
#
# Sources (~7 MB total):
#   https://names.toolforge.org/downloads/familynames.csv.gz
#   https://names.toolforge.org/downloads/givennames.csv.gz
#
# Usage:
#   python cmd/import-wikidata-names-dict.py --dict out/dbg64/dict --cache out/dict-src
#   python cmd/import-wikidata-names-dict.py --dict out/dbg64/dict --cache out/dict-src --probe

from __future__ import annotations

import argparse
import csv
import gzip
import json
import re
import shutil
import sys
import urllib.request
from collections import defaultdict
from pathlib import Path

USER_AGENT = (
    "SumatraPDF-dict-import/1.0 (Wikidata names; https://www.sumatrapdfreader.org/)"
)
FAMILY_URL = "https://names.toolforge.org/downloads/familynames.csv.gz"
GIVEN_URL = "https://names.toolforge.org/downloads/givennames.csv.gz"
TOKEN_RE = re.compile(r"^[a-z][a-z'-]*$")
HAS_CJK_RE = re.compile(r"[\u4e00-\u9fff]")
IDX_HEADER = "# SumatraDict idx v1"


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def esc(s: str) -> str:
    if not s:
        return ""
    return str(s).replace("\\", "\\\\").replace("\r", " ").replace("\n", "\\n").replace("\t", "\\t").strip()


def norm_word(s: str) -> str:
    s = (s or "").strip().lower()
    s = re.sub(r"[\u200b\u200c\u200d]", "", s)
    return s


def title_display(raw: str, word: str) -> str:
    raw = (raw or "").strip()
    if raw and not raw.islower():
        return raw
    if not word:
        return raw
    return word[:1].upper() + word[1:]


def zh_ok(name: str) -> bool:
    name = (name or "").strip()
    return bool(name) and bool(HAS_CJK_RE.search(name)) and len(name) <= 40


def download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and dest.stat().st_size > 1024:
        log(f"cache hit {dest.name} ({dest.stat().st_size} bytes)")
        return
    log(f"downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    tmp = dest.with_suffix(dest.suffix + ".part")
    with urllib.request.urlopen(req, timeout=120) as r, tmp.open("wb") as out:
        shutil.copyfileobj(r, out)
    tmp.replace(dest)
    log(f"saved {dest.name} ({dest.stat().st_size} bytes)")


def parse_names_csv(path: Path, kind: str) -> dict[str, dict]:
    log(f"reading {path.name}")
    by_id: dict[str, dict] = defaultdict(lambda: {"en": [], "zh": []})
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8", errors="replace", newline="") as f:
        for row in csv.DictReader(f):
            name = (row.get("Name") or "").strip()
            qid = (row.get("WikidataID") or "").strip()
            if not name or not qid:
                continue
            if zh_ok(name):
                by_id[qid]["zh"].append(name)
                continue
            word = norm_word(name)
            if TOKEN_RE.fullmatch(word) and len(word) >= 2:
                by_id[qid]["en"].append((word, name))
    label = "姓氏" if kind == "surname" else "人名"
    out: dict[str, dict] = {}
    for rec in by_id.values():
        zh_vals = []
        seen_zh = set()
        for z in rec["zh"]:
            if z not in seen_zh:
                seen_zh.add(z)
                zh_vals.append(z)
        if not zh_vals:
            continue
        for word, display in rec["en"]:
            zh = [f"{label}。{z}" for z in zh_vals[:3]]
            prev = out.get(word)
            if prev is None:
                out[word] = {
                    "word": word,
                    "display": title_display(display, word),
                    "ipa": "",
                    "zh": zh,
                    "src": kind,
                }
            else:
                for z in zh:
                    if z not in prev["zh"] and len(prev["zh"]) < 3:
                        prev["zh"].append(z)
    log(f"{path.name}: {len(out)} english names with chinese")
    return out


def merge_records(family: dict[str, dict], given: dict[str, dict]) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for src in (family, given):
        for word, rec in src.items():
            prev = out.get(word)
            if prev is None:
                out[word] = {
                    "word": rec["word"],
                    "display": rec["display"],
                    "ipa": rec["ipa"],
                    "zh": list(rec["zh"]),
                    "src": rec["src"],
                }
                continue
            kinds = {prev["src"], rec["src"]}
            if kinds == {"surname", "given"}:
                prev["src"] = "both"
            for z in rec["zh"]:
                if z not in prev["zh"] and len(prev["zh"]) < 3:
                    prev["zh"].append(z)
            if rec["display"][:1].isupper() and prev["display"][:1].islower():
                prev["display"] = rec["display"]
    return out


def encode_entry(rec: dict) -> bytes:
    zh_defs = rec.get("zh") or ["人名"]
    lines = ["SDICT1", esc(rec["display"]), "", "1", "n.", "noun", str(len(zh_defs))]
    for zh in zh_defs:
        lines.append("\t" + esc(zh))
    lines.append("")
    lines.append("")
    return ("\n".join(lines) + "\n").encode("utf-8")


def load_existing_idx(path: Path) -> tuple[list[str], set[str]]:
    rows = []
    words = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("\ufeff"):
            line = line[1:]
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if not cols or not cols[0]:
            continue
        rows.append(line)
        words.add(cols[0])
    return rows, words


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--dict", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--probe", action="store_true")
    args = ap.parse_args()
    dict_dir = Path(args.dict)
    cache = Path(args.cache)
    cache.mkdir(parents=True, exist_ok=True)

    family_path = cache / "familynames.csv.gz"
    given_path = cache / "givennames.csv.gz"
    download(FAMILY_URL, family_path)
    download(GIVEN_URL, given_path)
    family = parse_names_csv(family_path, "surname")
    given = parse_names_csv(given_path, "given")
    parsed = merge_records(family, given)
    samples = {k: parsed.get(k) for k in ("bronterre", "mitchens", "einstein", "smith", "ada")}
    if args.probe:
        preview = list(parsed.values())[:12]
        print(
            json.dumps(
                {
                    "family": len(family),
                    "given": len(given),
                    "merged": len(parsed),
                    "samples": samples,
                    "preview": preview,
                },
                ensure_ascii=False,
            )
        )
        return 0

    idx_path = dict_dir / "SumatraDict.idx"
    dat_path = dict_dir / "SumatraDict.dat"
    if not idx_path.exists() or not dat_path.exists():
        log("missing SumatraDict.idx/dat; run import-oaldpex-dict.ts first")
        return 1
    idx_rows, existing = load_existing_idx(idx_path)
    dat = bytearray(dat_path.read_bytes())
    added = 0
    skipped = 0
    by_src: dict[str, int] = defaultdict(int)
    for word in sorted(parsed):
        rec = parsed[word]
        if word in existing:
            skipped += 1
            continue
        payload = encode_entry(rec)
        off = len(dat)
        dat.extend(payload)
        idx_rows.append("\t".join((word, str(off), str(len(payload)), "0", "0", "")))
        existing.add(word)
        added += 1
        by_src[rec["src"]] += 1
    body = [(line.split("\t", 1)[0], line) for line in idx_rows]
    body.sort(key=lambda r: (r[0], r[1]))
    idx_path.write_text(IDX_HEADER + "\n" + "\n".join(r[1] for r in body) + "\n", encoding="utf-8")
    dat_path.write_bytes(dat)
    rel = dict_dir.parent.parent / "rel64" / "dict"
    copied = False
    if rel.is_dir():
        shutil.copy2(idx_path, rel / "SumatraDict.idx")
        shutil.copy2(dat_path, rel / "SumatraDict.dat")
        copied = True
    print(
        json.dumps(
            {
                "family": len(family),
                "given": len(given),
                "merged": len(parsed),
                "added": added,
                "skippedExisting": skipped,
                "addedBySource": dict(by_src),
                "indexRows": len(body),
                "dataBytes": len(dat),
                "copiedRel64": copied,
                "samples": samples,
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
