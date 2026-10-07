// Merges single-token proper nouns (people/places) from a local The Little Dict
// MDict package into SumatraPDF's existing English offline dictionary.
// IPA is converted to the same OALD-style field as import-oaldpex-dict.ts
// (BrE first, AmE second, "BrE/ /AmE"). Does not read TLD*.mdd audio.
//
// Requires Python modules:
//   python -m pip install mdict-utils beautifulsoup4
//
// Usage:
//   bun cmd/import-tld-names-dict.ts
//   bun cmd/import-tld-names-dict.ts --probe
//   bun cmd/import-tld-names-dict.ts --source "C:\\path\\to\\The little dict" --dict out/dbg64/dict

import { existsSync, mkdirSync, unlinkSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const defaultSource = String.raw`C:\baidunetdiskdownload\01 The little dict`;
const defaultDict = join(process.cwd(), "out", "dbg64", "dict");

function argValue(name: string, def: string): string {
  const idx = process.argv.indexOf(name);
  if (idx >= 0 && idx + 1 < process.argv.length) {
    return process.argv[idx + 1];
  }
  return def;
}

function hasFlag(name: string): boolean {
  return process.argv.includes(name);
}

const source = resolve(argValue("--source", defaultSource));
const dictDir = resolve(argValue("--dict", defaultDict));
const pythonArg = argValue("--python", "");
const probe = hasFlag("--probe");

if (!existsSync(source)) {
  throw new Error(`source directory does not exist: ${source}`);
}
mkdirSync(dictDir, { recursive: true });

const py = String.raw`
import html
import json
import re
import sys
from pathlib import Path

try:
    from mdict_utils.reader import MDX as MdictUtilsMDX
    HAS_MDICT_UTILS = True
except Exception:
    HAS_MDICT_UTILS = False

try:
    from readmdict import MDX
    HAS_READMDICT = True
except BaseException:
    HAS_READMDICT = False

if not HAS_MDICT_UTILS and not HAS_READMDICT:
    print("Missing MDX reader. Install with: python -m pip install mdict-utils", file=sys.stderr)
    raise SystemExit(1)

try:
    from bs4 import BeautifulSoup
except Exception:
    print("Missing Python module 'beautifulsoup4'. Install with: python -m pip install beautifulsoup4", file=sys.stderr)
    raise

source = Path(sys.argv[1])
dict_dir = Path(sys.argv[2])
probe = "--probe" in sys.argv[3:]
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

mdx_path = source / "TLD.mdx"
if not mdx_path.exists():
    mdxs = sorted(source.glob("*.mdx"))
    if not mdxs:
        raise SystemExit(f"missing TLD.mdx in {source}")
    mdx_path = mdxs[0]

TOKEN_RE = re.compile(r"^[a-z][a-z'-]*$")
NAME_MARK_RE = re.compile(r"\[人名\]|\[地名\]|\[国名\]|专有名词")
BIO_RE = re.compile(
    r"人名|地名|国名|男子名|女子名|姓氏|首都|数学家|物理学家|哲学家|科学家|化学家|文学家|作家|诗人|剧作家|音乐家|画家"
)
WEB_PLACE_RE = re.compile(r"人名|地名|国名|火车站|火车总站|城市|首都|大学")
HAS_CJK_RE = re.compile(r"[\u4e00-\u9fff]")
IDX_HEADER = "# SumatraDict idx v1"


def b2s(v):
    if isinstance(v, bytes):
        return v.decode("utf-8", "ignore")
    return str(v)


def esc(s):
    if not s:
        return ""
    return str(s).replace("\\", "\\\\").replace("\r", " ").replace("\n", "\\n").replace("\t", "\\t").strip()


def clean_text(s):
    s = html.unescape(s or "")
    s = re.sub(r"\s+", " ", s)
    return s.strip()


def norm_word(s):
    s = html.unescape(s or "").strip().lower()
    s = re.sub(r"[\u200b\u200c\u200d]", "", s)
    return s


def iter_mdx_entries():
    if HAS_READMDICT:
        try:
            for key, val in MDX(str(mdx_path)).items():
                yield b2s(key), b2s(val)
            return
        except BaseException as e:
            print(f"readmdict failed ({e}); falling back to mdict-utils", file=sys.stderr)
    if not HAS_MDICT_UTILS:
        raise SystemExit("no MDX reader available")
    for key, val in MdictUtilsMDX(str(mdx_path)).items():
        yield b2s(key), b2s(val)


def is_name_html(html_s):
    if "class=\"orm\"" in html_s:
        return False
    if NAME_MARK_RE.search(html_s) or BIO_RE.search(html_s):
        return True
    if "[网络]" in html_s and WEB_PLACE_RE.search(html_s):
        return True
    return "名词" in html_s and "100%" in html_s


def html_score(html_s):
    n = 0
    if "class=\"ipa\"" in html_s:
        n += 4
    if "class=\"dcn\"" in html_s:
        n += 2
    if "class=\"hwrap\"" in html_s:
        n += 1
    n += min(len(html_s) // 400, 3)
    return n


# TLD stores KK-flavored transcriptions in [square brackets]. Convert to the
# OALD IPA field: optional "BrE/ /AmE", no outer slashes, IPA stress/length.
def kk_to_ipa(s):
    s = clean_text(s)
    if not s:
        return ""
    s = s.strip()
    s = re.sub(r"^\[+|\]+$", "", s)
    s = s.strip("/")
    s = s.replace("ә", "ə").replace("ε", "e")
    s = s.replace(":", "ː")
    s = s.replace("'", "ˈ").replace("ˈˈ", "ˈ")
    s = s.replace(".", "")
    s = s.replace("^", "")
    if "," in s:
        parts = [p.strip() for p in s.split(",") if p.strip()]
        if len(parts) >= 2:
            if parts[0] == parts[1]:
                return parts[0]
            return parts[0] + "/ /" + parts[1]
        if parts:
            s = parts[0]
    repl = (
        ("eɪ", "\x00"),
        ("aɪ", "\x01"),
        ("ɔɪ", "\x02"),
        ("əʊ", "\x03"),
        ("aʊ", "\x04"),
        ("ɪə", "\x05"),
        ("eə", "\x06"),
        ("ʊə", "\x07"),
        ("ei", "\x00"),
        ("ai", "\x01"),
        ("ɔi", "\x02"),
        ("oi", "\x02"),
        ("əu", "\x03"),
        ("au", "\x04"),
        ("iə", "\x05"),
        ("uə", "\x07"),
    )
    for a, b in repl:
        s = s.replace(a, b)
    s = (
        s.replace("\x00", "eɪ")
        .replace("\x01", "aɪ")
        .replace("\x02", "ɔɪ")
        .replace("\x03", "əʊ")
        .replace("\x04", "aʊ")
        .replace("\x05", "ɪə")
        .replace("\x06", "eə")
        .replace("\x07", "ʊə")
    )
    return s.strip()


def phon_kind(tag):
    cls = " ".join(tag.get("class") or []).lower()
    blob = cls
    parent = tag.parent
    if parent is not None:
        blob += " " + " ".join(parent.get("class") or []).lower()
    if any(x in blob for x in ("ame", "us", "n_am", "american")):
        return "us"
    if any(x in blob for x in ("bre", "uk", "british")):
        return "uk"
    return "unk"


def extract_ipa(soup):
    tags = [t for t in soup.find_all(True) if "ipa" in (t.get("class") or []) or t.name in ("ipa", "phon")]
    if not tags:
        for t in soup.find_all(True):
            cls = " ".join(t.get("class") or []).lower()
            if "phon" in cls or "pron" in cls:
                tags.append(t)
    uk, us, unk = [], [], []
    for tag in tags:
        ipa = kk_to_ipa(tag.get_text(" "))
        if not ipa or len(ipa) > 80:
            continue
        kind = phon_kind(tag)
        if kind == "uk":
            uk.append(ipa)
        elif kind == "us":
            us.append(ipa)
        else:
            unk.append(ipa)
    if uk and us:
        if uk[0] == us[0]:
            return uk[0]
        return uk[0] + "/ /" + us[0]
    if uk:
        return uk[0]
    if us:
        return us[0]
    if len(unk) >= 2:
        if unk[0] == unk[1]:
            return unk[0]
        return unk[0] + "/ /" + unk[1]
    if unk:
        return unk[0]
    return ""


def extract_zh(soup):
    vals = []
    for cls in ("dcn", "dne"):
        for tag in soup.find_all(class_=cls):
            t = clean_text(tag.get_text(" "))
            if not t or t in vals:
                continue
            if "的复数" in t:
                continue
            vals.append(t)
            if len(vals) >= 2:
                return vals
    return vals


def looks_like_name(soup, html_s, display, zh_defs):
    if soup.find(class_="orm"):
        return False
    first = zh_defs[0] if zh_defs else ""
    if NAME_MARK_RE.search(first) or BIO_RE.search(first):
        return True
    blob = " ".join(zh_defs)
    if ("[网络]" in html_s or soup.find(class_="dne")) and HAS_CJK_RE.search(blob) and WEB_PLACE_RE.search(blob):
        return True
    if not (display and display[:1].isupper() and not display.isupper()):
        return False
    if "名词" not in html_s or "100%" not in html_s:
        return False
    pos_ok = False
    for tag in soup.find_all(class_="pos"):
        p = clean_text(tag.get_text(" ")).lower()
        if p in ("n.", "n", "noun"):
            pos_ok = True
            break
    if not pos_ok:
        return False
    return bool(HAS_CJK_RE.search(first))


def encode_entry(display, ipa, zh_defs):
    lines = ["SDICT1", esc(display), esc(ipa), "1", "n.", "noun", str(len(zh_defs))]
    for zh in zh_defs:
        lines.append("\t" + esc(zh))
    lines.append("")
    lines.append("")
    return ("\n".join(lines) + "\n").encode("utf-8")


def load_existing_idx(path):
    rows = []
    words = set()
    if not path.exists():
        return rows, words
    text = path.read_text(encoding="utf-8")
    for line in text.splitlines():
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


print(f"reading entries: {mdx_path.name}", file=sys.stderr)
best = {}
n_scanned = 0
for key, html_s in iter_mdx_entries():
    n_scanned += 1
    word = norm_word(key)
    if not TOKEN_RE.fullmatch(word):
        continue
    if not is_name_html(html_s):
        continue
    prev = best.get(word)
    if prev is None or html_score(html_s) > html_score(prev):
        best[word] = html_s

parsed = []
for word, html_s in best.items():
    soup = BeautifulSoup(html_s, "html.parser")
    for tag in soup(["script", "style"]):
        tag.decompose()
    h2 = soup.find("h2")
    display = clean_text(h2.get_text(" ")) if h2 else word
    if display and display.islower():
        display = display[:1].upper() + display[1:]
    zh_defs = extract_zh(soup)
    if not zh_defs:
        continue
    if not looks_like_name(soup, html_s, display, zh_defs):
        continue
    ipa = extract_ipa(soup)
    parsed.append({"word": word, "display": display, "ipa": ipa, "zh": zh_defs})

parsed.sort(key=lambda e: e["word"])

if probe:
    print(
        json.dumps(
            {
                "scanned": n_scanned,
                "candidates": len(best),
                "parsed": len(parsed),
                "samples": parsed[:40],
            },
            ensure_ascii=False,
        )
    )
    raise SystemExit(0)

idx_path = dict_dir / "SumatraDict.idx"
dat_path = dict_dir / "SumatraDict.dat"
if not idx_path.exists() or not dat_path.exists():
    raise SystemExit(f"missing SumatraDict.idx/dat in {dict_dir}; run import-oaldpex-dict.ts first")

idx_rows, existing = load_existing_idx(idx_path)
dat = bytearray(dat_path.read_bytes())
added = 0
skipped = 0
for e in parsed:
    if e["word"] in existing:
        skipped += 1
        continue
    payload = encode_entry(e["display"], e["ipa"], e["zh"])
    off = len(dat)
    dat.extend(payload)
    idx_rows.append("\t".join((e["word"], str(off), str(len(payload)), "0", "0", "")))
    existing.add(e["word"])
    added += 1

body = []
for line in idx_rows:
    word = line.split("\t", 1)[0]
    body.append((word, line))
body.sort(key=lambda r: (r[0], r[1]))
idx_text = IDX_HEADER + "\n" + "\n".join(r[1] for r in body) + "\n"
idx_path.write_text(idx_text, encoding="utf-8")
dat_path.write_bytes(dat)
print(
    json.dumps(
        {
            "scanned": n_scanned,
            "parsed": len(parsed),
            "added": added,
            "skippedExisting": skipped,
            "indexRows": len(body),
            "dataBytes": len(dat),
        },
        ensure_ascii=False,
    )
)
`;

const pyScript = join(dictDir, "import-tld-names-dict.tmp.py");
writeFileSync(pyScript, py, "utf-8");
const extra = probe ? ["--probe"] : [];
const procArgs = pythonArg
  ? [pythonArg, pyScript, source, dictDir, ...extra]
  : ["cmd.exe", "/d", "/s", "/c", "python", pyScript, source, dictDir, ...extra];
const proc = Bun.spawn(procArgs, {
  stdout: "pipe",
  stderr: "pipe",
  env: { ...process.env, PYTHONIOENCODING: "utf-8" },
});
const [stdout, stderr, exitCode] = await Promise.all([
  new Response(proc.stdout).text(),
  new Response(proc.stderr).text(),
  proc.exited,
]);
if (existsSync(pyScript)) {
  unlinkSync(pyScript);
}
if (stderr.trim()) {
  console.error(stderr.trim());
}
if (exitCode !== 0) {
  throw new Error(`dictionary import failed with exit code ${exitCode}`);
}
console.log(stdout.trim());
