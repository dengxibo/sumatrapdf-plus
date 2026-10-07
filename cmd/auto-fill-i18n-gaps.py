#!/usr/bin/env python3
"""
Auto-translate remaining i18n gaps for 17 majors.
Uses MyMemory (fallback Google) with checkpoint resume.
"""
from __future__ import annotations

import json
import re
import time
from pathlib import Path

from deep_translator import GoogleTranslator, MyMemoryTranslator

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "cmd" / "_i18n-gap-report.json"
CHECKPOINT = ROOT / "cmd" / "_fills_auto.json"
MAJORS = ["cn", "tw", "ja", "kr", "de", "fr", "es", "it", "pt", "br", "ru", "uk", "pl", "nl", "tr", "vn"]

# MyMemory lang pairs (source en-US)
MM = {
    "cn": "zh-CN",
    "tw": "zh-TW",
    "ja": "ja-JP",
    "kr": "ko-KR",
    "de": "de-DE",
    "fr": "fr-FR",
    "es": "es-ES",
    "it": "it-IT",
    "pt": "pt-PT",
    "br": "pt-BR",
    "ru": "ru-RU",
    "uk": "uk-UA",
    "pl": "pl-PL",
    "nl": "nl-NL",
    "tr": "tr-TR",
    "vn": "vi-VN",
}

# Google targets
GG = {
    "cn": "zh-CN",
    "tw": "zh-TW",
    "ja": "ja",
    "kr": "ko",
    "de": "de",
    "fr": "fr",
    "es": "es",
    "it": "it",
    "pt": "pt",
    "br": "pt",
    "ru": "ru",
    "uk": "uk",
    "pl": "pl",
    "nl": "nl",
    "tr": "tr",
    "vn": "vi",
}

BRAND_KEEP = {
    "ChatGPT",
    "DeepSeek",
    "Doubao",
    "OCR",
    "AI",
    "PDF",
    "EPUB",
    "KB",
    "MB",
    "GB",
    "DPI:",
    "OK",
    "(dbg)",
    "You",
    "You:",
    "AI:",
    "none",
}


def extract_mnemonic(key: str) -> tuple[str, str | None]:
    m = re.search(r"&([A-Za-z0-9])", key)
    if not m:
        return key, None
    letter = m.group(1).upper()
    plain = key.replace("&", "", 1)
    return plain, letter


def apply_mnemonic(text: str, letter: str | None, lang: str) -> str:
    if not letter or "&" in text:
        return text
    if lang in {"cn", "tw", "ja", "kr", "vn"}:
        # Avoid duplicating if already has （&X）
        if f"(&{letter})" in text or f"(&{letter.lower()})" in text:
            return text
        return f"{text}(&{letter})"
    # European: insert & before first matching letter if present
    low = letter.lower()
    for i, ch in enumerate(text):
        if ch.lower() == low:
            return text[:i] + "&" + text[i:]
    return text + f" (&{letter})"


def is_useless(key: str, val: str | None) -> bool:
    if not val:
        return True
    if val == key:
        return True
    return key.replace("&", "") == val.replace("&", "")


def translate_one(text: str, lang: str) -> str:
    text = text.strip()
    if not text:
        return text
    if text in BRAND_KEEP or text.rstrip(":") in BRAND_KEEP:
        return text
    # Protect placeholders
    holders: list[str] = []

    def protect(m: re.Match) -> str:
        holders.append(m.group(0))
        return f"⟦{len(holders) - 1}⟧"

    protected = re.sub(r"%[%sd]|%\d*\$?[sd]|\{[^}]+\}", protect, text)
    out = None
    last_err = None
    for attempt in range(4):
        try:
            # Prefer Google with delay; MyMemory as fallback
            if attempt % 2 == 0:
                out = GoogleTranslator(source="en", target=GG[lang]).translate(protected)
            else:
                out = MyMemoryTranslator(source="en-US", target=MM[lang]).translate(protected)
            if out:
                break
        except Exception as e:
            last_err = e
            time.sleep(1.2 + attempt)
    if not out:
        raise RuntimeError(f"translate failed {lang}: {last_err}")
    for i, h in enumerate(holders):
        out = out.replace(f"⟦{i}⟧", h).replace(f"[[{i}]]", h)
    return out


def main() -> None:
    rows = json.loads(REPORT.read_text(encoding="utf-8"))
    fills: dict = {}
    if CHECKPOINT.exists():
        fills = json.loads(CHECKPOINT.read_text(encoding="utf-8"))
        print(f"resumed checkpoint {len(fills)} keys")

    total = len(rows)
    for idx, row in enumerate(rows):
        key = row["key"]
        if key in fills and all(lang in fills[key] for lang in MAJORS):
            continue
        plain, letter = extract_mnemonic(key)
        entry = dict(fills.get(key, {}))

        # Seed from existing good cn/ja in report when useful
        if row.get("cn") and not is_useless(key, row.get("cn")):
            entry.setdefault("cn", row["cn"])
        if row.get("ja") and not is_useless(key, row.get("ja")):
            entry.setdefault("ja", row["ja"])

        for lang in MAJORS:
            if lang in entry and not is_useless(key, entry.get(lang)):
                continue
            if plain in BRAND_KEEP or key in BRAND_KEEP:
                entry[lang] = key if lang not in {"cn", "tw", "ja", "kr"} or key in BRAND_KEEP else key
                # For brands keep English
                entry[lang] = key.replace("&", "") if letter and lang in {"cn", "tw", "ja", "kr"} else key
                if letter and lang in {"cn", "tw", "ja", "kr"} and key.startswith("&"):
                    entry[lang] = apply_mnemonic(plain, letter, lang)
                continue
            try:
                translated = translate_one(plain, lang)
                entry[lang] = apply_mnemonic(translated, letter, lang)
                time.sleep(0.35)
            except Exception as e:
                print(f"FAIL {idx+1}/{total} {lang} {key!r}: {e}")
                # keep going; leave missing for later resume
                time.sleep(2.0)

        fills[key] = entry
        if (idx + 1) % 5 == 0 or idx + 1 == total:
            CHECKPOINT.write_text(json.dumps(fills, ensure_ascii=False, indent=2), encoding="utf-8")
            done_langs = sum(1 for v in entry.values() if v)
            print(f"[{idx+1}/{total}] {key[:60]!r} langs={done_langs}")

    CHECKPOINT.write_text(json.dumps(fills, ensure_ascii=False, indent=2), encoding="utf-8")
    # Also write as part file for apply script
    out = ROOT / "cmd" / "_fills_part_auto.json"
    out.write_text(json.dumps(fills, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"wrote {out} keys={len(fills)}")


if __name__ == "__main__":
    main()
