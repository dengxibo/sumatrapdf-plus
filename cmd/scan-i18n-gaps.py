#!/usr/bin/env python3
"""
Find UI translation keys used in src/ that are missing or still English
for the 17 major languages in translations-good.txt.
"""
from __future__ import annotations

import json
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAJORS = ["cn", "tw", "ja", "kr", "de", "fr", "es", "it", "pt", "br", "ru", "uk", "pl", "nl", "tr", "vn"]

# Keys that are intentionally kept as Latin / brand / symbolic.
ALLOW_SAME_AS_EN = {
    "OK",
    "OCR",
    "AI",
    "Web AI",
    "PDF",
    "EPUB",
    "EPUB 3",
    "URL",
    "API",
    "DPI",
    "DPI:",
    "Ctrl+Tab",
    "Esc",
    "F3",
    "F8",
    "F9",
    "F11",
    "F12",
    "Doubao",
    "Volc",
    "ChatGPT",
    "DeepSeek",
    "SumatraPDF",
    "KB",
    "MB",
    "GB",
    "(dbg)",
    "You",
    "You:",
    "AI:",
    "none",
    "On",
    "Off",
    "Copy",
    "Send",
    "Play",
    "Set",
    "Menu",
    "Settings",
    "Source",
    "Page",
    "Chinese",
    "Conservative",
    "Detailed",
    "GB",
    # Cognates / identical loanwords kept as English in many locales
    "&File",
    "&Help",
    "&Image",
    "&Manual",
    "&Model:",
    "&Name:",
    "&Options",
    "&Options...",
    "&Parallel (1-8):",
    "&Password:",
    "&Platform:",
    "&Test",
    "&Text",
    "&Zoom",
    "(page %s)",
    "32-bit",
    "64-bit",
    "Annotations",
    "Auto OCR",
    "Bytes",
    "Color",
    "Copyright:",
    "Error",
    "File:",
    "Flash:",
    "General",
    "Home",
    "Images",
    "Interface",
    "No",
    "git commit",
    "OK",
}

TRA_RE = re.compile(
    r"""(?:_TRA|_TRN|_TRW|_TR)\(\s*(?:u8)?\s*"((?:\\.|[^"\\])*)"\s*\)""",
    re.M,
)


def unescape(s: str) -> str:
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
            elif n == '"':
                out.append('"')
            elif n == "\\":
                out.append("\\")
            else:
                out.append(n)
            i += 2
            continue
        out.append(s[i])
        i += 1
    return "".join(out)


def parse_blocks(text: str) -> dict[str, dict[str, str]]:
    """Parse translations-*.txt; keys/values use \\n escapes like the runtime loader."""
    blocks: dict[str, dict[str, str]] = {}
    key = None
    cur: dict[str, str] = {}
    for line in text.splitlines():
        if line.startswith(":"):
            if key is not None:
                blocks[key] = cur
            key = unescape(line[1:])
            cur = {}
        elif key is not None and ":" in line:
            lang, val = line.split(":", 1)
            # Skip garbage lines from a prior bad upsert that split multiline keys
            if lang and all(c.isalnum() or c in "-_" for c in lang) and len(lang) <= 8:
                cur[lang] = unescape(val)
    if key is not None:
        blocks[key] = cur
    return blocks


def collect_keys() -> dict[str, list[str]]:
    """key -> list of source files referencing it."""
    refs: dict[str, set[str]] = defaultdict(set)
    src = ROOT / "src"
    for path in src.rglob("*"):
        if path.suffix.lower() not in {".cpp", ".h", ".c"}:
            continue
        # Skip generated / huge generated tables if any
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = str(path.relative_to(ROOT)).replace("\\", "/")
        for m in TRA_RE.finditer(text):
            key = unescape(m.group(1))
            if not key or key.startswith("%") and len(key) < 3:
                continue
            # Skip pure format fragments that are not UI labels
            refs[key].add(rel)
    return {k: sorted(v) for k, v in refs.items()}


def is_allow_same(key: str) -> bool:
    if key in ALLOW_SAME_AS_EN:
        return True
    # Single-letter / symbolic / mostly ASCII brands
    bare = key.replace("&", "").strip()
    if bare in ALLOW_SAME_AS_EN:
        return True
    if bare in {"°", "180°", "-10s", "+10s", "1x"}:
        return True
    return False


def looks_english_placeholder(key: str, val: str) -> bool:
    if val == key:
        return True
    # English placeholder that only differs by mnemonic position
    k = key.replace("&", "")
    v = val.replace("&", "")
    if k == v and any(c.isalpha() and ord(c) < 128 for c in k):
        # Still English text — treat as untranslated when it has latin letters
        # and no CJK / Hangul / Cyrillic / accented chars beyond &
        if re.fullmatch(r"[A-Za-z0-9 .,:;!?\-\(\)/%+'\"\[\]…]+", v):
            return True
    return False


def main() -> None:
    keys = collect_keys()
    good = parse_blocks((ROOT / "translations" / "translations-good.txt").read_text(encoding="utf-8"))
    rows = []
    for key in sorted(keys.keys()):
        if is_allow_same(key):
            continue
        block = good.get(key, {})
        missing = []
        english = []
        for lang in MAJORS:
            val = block.get(lang)
            if val is None or val == "":
                missing.append(lang)
            elif looks_english_placeholder(key, val):
                english.append(lang)
        if not missing and not english:
            continue
        rows.append(
            {
                "key": key,
                "missing": missing,
                "english": english,
                "files": keys[key][:8],
                "cn": block.get("cn"),
                "ja": block.get("ja"),
            }
        )

    out_dir = ROOT / "cmd"
    report = out_dir / "_i18n-gap-report.json"
    report.write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")

    # Human summary
    lines = [
        f"scanned_keys={len(keys)}",
        f"gap_keys={len(rows)}",
        f"needs_any_major={sum(1 for r in rows if r['missing'] or r['english'])}",
        f"missing_ja={sum(1 for r in rows if 'ja' in r['missing'])}",
        f"english_ja={sum(1 for r in rows if 'ja' in r['english'])}",
        f"missing_cn={sum(1 for r in rows if 'cn' in r['missing'])}",
        "",
        "=== top gaps (ja missing or english) ===",
    ]
    ja_gaps = [r for r in rows if "ja" in r["missing"] or "ja" in r["english"]]
    for r in ja_gaps[:200]:
        kind = "NOJA" if "ja" in r["missing"] else "EN"
        lines.append(f"{kind}\t{r['key']}\tfiles={','.join(Path(f).name for f in r['files'][:3])}")
    if len(ja_gaps) > 200:
        lines.append(f"... and {len(ja_gaps) - 200} more")
    summary = out_dir / "_i18n-gap-summary.txt"
    summary.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {report} ({len(rows)} gap keys)")
    print(f"Wrote {summary}")
    print(f"ja gaps: {len(ja_gaps)}")


if __name__ == "__main__":
    main()
