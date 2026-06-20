// Converts a local 现代汉语词典7 MDict package into SumatraPDF's Chinese offline dictionary files.
//
// Requires Python modules:
//   python -m pip install mdict-utils beautifulsoup4

import { existsSync, mkdirSync, unlinkSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const defaultSource = String.raw`C:\Data\01.装机\03.Dictionary\02.DictData\content\现代汉语词典7`;
const defaultOut = join(process.cwd(), "out", "dbg64", "dict");

function argValue(name: string, def: string): string {
  const idx = process.argv.indexOf(name);
  if (idx >= 0 && idx + 1 < process.argv.length) {
    return process.argv[idx + 1];
  }
  return def;
}

const source = resolve(argValue("--source", defaultSource));
const out = resolve(argValue("--out", defaultOut));
const pythonArg = argValue("--python", "");

function findPython(): string {
  const localAppData = process.env.LOCALAPPDATA || "";
  const userProfile = process.env.USERPROFILE || "";
  const candidates = [
    pythonArg,
    join(localAppData, "Microsoft", "WindowsApps", "python.exe"),
    join(localAppData, "Microsoft", "WindowsApps", "py.exe"),
    join(userProfile, "AppData", "Local", "Microsoft", "WindowsApps", "python.exe"),
    join(userProfile, "AppData", "Local", "Microsoft", "WindowsApps", "py.exe"),
  ].filter((s) => s.length > 0);
  for (const candidate of candidates) {
    if (existsSync(candidate)) {
      return candidate;
    }
  }
  throw new Error("could not find python.exe. Use --python C:\\path\\to\\python.exe");
}

if (!existsSync(source)) {
  throw new Error(`source directory does not exist: ${source}`);
}
mkdirSync(out, { recursive: true });

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
out = Path(sys.argv[2])
mdx_candidates = sorted(source.glob("*.mdx"))
if not mdx_candidates:
    raise SystemExit(f"missing *.mdx in {source}")
mdx_path = mdx_candidates[0]

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

def norm_key(s):
    s = html.unescape(s or "").strip()
    s = re.sub(r"[\u200b\u200c\u200d]", "", s)
    return s

def is_dict_key(key):
    if not key or key[0] == "0":
        return False
    if not re.search(r"[\u4e00-\u9fff]", key):
        return False
    return True

def text_without_tags(tag):
    if not tag:
        return ""
    nested = BeautifulSoup(str(tag), "html.parser")
    root = nested.find(True)
    if not root:
        return ""
    for bad in root.find_all(["a", "ci"]):
        bad.decompose()
    return clean_text(root.get_text(" "))

def extract_def_text(def_tag):
    parts = []
    num = def_tag.find("num")
    if num:
        parts.append(clean_text(num.get_text(" ")))
    ps = def_tag.find("ps")
    if ps:
        parts.append(clean_text(ps.get_text(" ")))
    for ex in def_tag.find_all("ex"):
        parts.append(clean_text(ex.get_text(" ")))
    body = clean_text(def_tag.get_text(" "))
    if body:
        for p in parts:
            body = body.replace(p, " ", 1)
        body = clean_text(body)
        if body:
            parts.append(body)
    return clean_text(" ".join(p for p in parts if p))

def short_pos(ps):
    p = (ps or "").strip()
    table = {
        "名": "n.", "动": "v.", "形": "adj.", "副": "adv.", "代": "pron.",
        "介": "prep.", "连": "conj.", "叹": "interj.", "量": "m.", "助": "part.",
    }
    return table.get(p, p or "def.")

def parse_entry_block(entry_tag, fallback_key):
    hwg = entry_tag.find("hwg")
    hw_tag = hwg.find("hw") if hwg else None
    headword = clean_text(hw_tag.get_text(" ")) if hw_tag else fallback_key
    pinyin_tag = hwg.find("pinyin") if hwg else None
    pinyin = clean_text(pinyin_tag.get_text(" ")) if pinyin_tag else ""
    first_ps = ""
    defs = []
    for def_tag in entry_tag.find_all("def", recursive=False):
        ps = def_tag.find("ps")
        if ps and not first_ps:
            first_ps = clean_text(ps.get_text(" "))
        text = extract_def_text(def_tag)
        if text:
            defs.append(text)
    if not defs:
        return None
    return {
        "headword": headword or fallback_key,
        "pinyin": pinyin,
        "label": pinyin if pinyin else short_pos(first_ps),
        "fl": first_ps or "def.",
        "defs": defs[:24],
    }

def extract_entry(key, value):
    key = norm_key(key)
    if not is_dict_key(key):
        return None
    soup = BeautifulSoup(b2s(value), "html.parser")
    for tag in soup(["script", "style"]):
        tag.decompose()
    blocks = []
    for entry_tag in soup.find_all("entry"):
        parsed = parse_entry_block(entry_tag, key)
        if parsed:
            blocks.append(parsed)
    if not blocks:
        return None
    return {
        "word": key,
        "display": blocks[0]["headword"],
        "ipa": blocks[0].get("pinyin", ""),
        "senses": blocks[:8],
    }

def encode_entry(e):
    lines = ["SDICT1", esc(e["display"]), esc(e["ipa"]), str(len(e["senses"]))]
    for s in e["senses"]:
        lines.append(esc(s["label"]))
        lines.append(esc(s["fl"]))
        lines.append(esc(s.get("pinyin", "")))
        lines.append("1")
        joined = "\\n".join(esc(d) for d in s["defs"])
        lines.append(joined)
        lines.append("")
        lines.append("")
    return ("\n".join(lines) + "\n").encode("utf-8")

def iter_mdx_entries():
    if HAS_MDICT_UTILS:
        for key, val in MdictUtilsMDX(str(mdx_path)).items():
            yield b2s(key), b2s(val)
        return
    for key, val in MDX(str(mdx_path)).items():
        yield b2s(key), b2s(val)

parsed_entries = []
seen = set()

print(f"reading entries: {mdx_path.name}", file=sys.stderr)
for key, val in iter_mdx_entries():
    e = extract_entry(key, val)
    if not e or e["word"] in seen:
        continue
    seen.add(e["word"])
    parsed_entries.append(e)

entries = []
dat = bytearray()

for e in parsed_entries:
    data_off = len(dat)
    payload = encode_entry(e)
    dat.extend(payload)
    entries.append((e["word"], data_off, len(payload)))

entries.sort(key=lambda r: r[0])
idx_lines = ["# SumatraDictZh idx v1"]
for r in entries:
    idx_lines.append("\t".join(str(x) for x in (r[0], r[1], r[2], 0, 0)))

(out / "SumatraDictZh.idx").write_text("\n".join(idx_lines) + "\n", encoding="utf-8")
(out / "SumatraDictZh.dat").write_bytes(dat)
print(json.dumps({"entries": len(seen), "indexRows": len(idx_lines) - 1, "dataBytes": len(dat)}, ensure_ascii=False))
`;

const pyScript = join(out, "import-xdhy7-dict.tmp.py");
writeFileSync(pyScript, py, "utf-8");
const procArgs = pythonArg
  ? [pythonArg, pyScript, source, out]
  : ["cmd.exe", "/d", "/s", "/c", "python", pyScript, source, out];
const proc = Bun.spawn(procArgs, {
  stdout: "pipe",
  stderr: "pipe",
});
const [stdout, stderr, exitCode] = await Promise.all([
  new Response(proc.stdout).text(),
  new Response(proc.stderr).text(),
  proc.exited,
]);
if (stderr.trim()) {
  console.error(stderr.trim());
}
if (exitCode !== 0) {
  throw new Error(`dictionary import failed with exit code ${exitCode}`);
}
if (existsSync(pyScript)) {
  unlinkSync(pyScript);
}
console.log(stdout.trim());
