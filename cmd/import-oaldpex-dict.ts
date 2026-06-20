// Converts a local OALDPEX MDict package into SumatraPDF's offline dictionary files.
//
// Requires Python modules:
//   python -m pip install readmdict beautifulsoup4

import { existsSync, mkdirSync, unlinkSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const defaultSource = String.raw`C:\Data\01.装机\03.Dictionary\02.DictData\content\OALDPEX\OALDPEX`;
const defaultOut = join(process.cwd(), "out", "dbg64", "dict");
const defaultOaldpexSource = String.raw`C:\Data\01.装机\03.Dictionary\02.DictData\content\OALDPEX\OALDPEX`;

function argValue(name: string, def: string): string {
  const idx = process.argv.indexOf(name);
  if (idx >= 0 && idx + 1 < process.argv.length) {
    return process.argv[idx + 1];
  }
  return def;
}

const source = resolve(argValue("--source", defaultOaldpexSource));
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
import os
import re
import sys
from pathlib import Path

try:
    from readmdict import MDX, MDD
    HAS_READMDICT = True
except BaseException:
    HAS_READMDICT = False

try:
    from mdict_utils.reader import MDX as MdictUtilsMDX, MDD as MdictUtilsMDD
    HAS_MDICT_UTILS = True
except Exception:
    HAS_MDICT_UTILS = False

if not HAS_READMDICT and not HAS_MDICT_UTILS:
    print("Missing MDX reader. Install with: python -m pip install mdict-utils", file=sys.stderr)
    raise SystemExit(1)

try:
    from bs4 import BeautifulSoup
except Exception:
    print("Missing Python module 'beautifulsoup4'. Install with: python -m pip install beautifulsoup4", file=sys.stderr)
    raise

source = Path(sys.argv[1])
out = Path(sys.argv[2])
mdx_path = source / "oaldpe.mdx"
mdd_paths = sorted(source.glob("oaldpe*.mdd"))
if not mdx_path.exists():
    raise SystemExit(f"missing {mdx_path}")
if not mdd_paths:
    raise SystemExit(f"missing oaldpe*.mdd in {source}")

def b2s(v):
    if isinstance(v, bytes):
        return v.decode("utf-8", "ignore")
    return str(v)

def norm_word(s):
    s = html.unescape(s or "").strip().lower()
    s = re.sub(r"[\u200b\u200c\u200d]", "", s)
    return s

def esc(s):
    if not s:
        return ""
    return str(s).replace("\\", "\\\\").replace("\r", " ").replace("\n", "\\n").replace("\t", "\\t").strip()

def clean_text(s):
    s = html.unescape(s or "")
    s = re.sub(r"\s+", " ", s)
    return s.strip()

def class_has(tag, names):
    cls = " ".join(tag.get("class", []))
    return any(n in cls.lower() for n in names)

def has_class(tag, name):
    return name in tag.get("class", [])

def text_without_zh(tag):
    nested = BeautifulSoup(str(tag), "html.parser")
    root = nested.find(True)
    if not root:
        return ""
    for bad in root.find_all(["xt", "unxt", "deft", "chn"]):
        bad.decompose()
    return clean_text(root.get_text(" "))

def first_text(soup, classes):
    for tag in soup.find_all(True):
        if class_has(tag, classes):
            t = clean_text(tag.get_text(" "))
            if t:
                return t
    return ""

def all_texts(root, classes):
    vals = []
    for tag in root.find_all(True):
        if class_has(tag, classes):
            t = clean_text(tag.get_text(" "))
            if t and t not in vals:
                vals.append(t)
    return vals

def short_pos(pos):
    p = (pos or "").strip().lower()
    table = {
        "noun": "n.", "verb": "v.", "adjective": "adj.", "adverb": "adv.",
        "preposition": "prep.", "conjunction": "conj.", "pronoun": "pron.",
        "interjection": "interj.",
    }
    return table.get(p, pos or "def.")

def extract_audio_refs(soup):
    refs = []
    for tag in soup.find_all(True):
        for attr in ("href", "src", "data-src", "data-href", "sound"):
            v = tag.get(attr)
            if not v:
                continue
            v = str(v)
            if ".mp3" in v.lower() or ".wav" in v.lower() or v.lower().startswith("sound://"):
                refs.append(v.replace("sound://", ""))
    return refs

def audio_rank(ref):
    r = ref.lower()
    if any(x in r for x in ("us", "ame", "n_am", "american")):
        return 0
    if any(x in r for x in ("uk", "bre", "british")):
        return 1
    return 2

def audio_keys_for_ref(ref):
    r = ref.replace("\\", "/").strip().lower()
    r = re.sub(r"^[a-z]+://", "", r)
    base = r.split("/")[-1]
    return [r, "/" + r, "\\" + r.replace("/", "\\"), base]

def collect_mdd_audio(needed):
    audio = {}
    for path in mdd_paths:
        print(f"reading audio resources: {path.name}", file=sys.stderr)
        if HAS_READMDICT:
            items = MDD(str(path)).items()
        elif HAS_MDICT_UTILS:
            items = MdictUtilsMDD(str(path)).items()
        else:
            continue
        for key, val in items:
            k = b2s(key).replace("\\", "/").lstrip("/").lower()
            if not (k.endswith(".mp3") or k.endswith(".wav")):
                continue
            base = k.split("/")[-1]
            if k in needed or base in needed:
                b = bytes(val)
                audio[k] = b
                audio[base] = b
    return audio

def find_audio(audio_map, refs, word):
    for ref in sorted(refs, key=audio_rank):
        for k in audio_keys_for_ref(ref):
            k = k.replace("\\", "/").lstrip("/").lower()
            if k in audio_map:
                ext = "wav" if k.endswith(".wav") else "mp3"
                return audio_map[k], ext
    return b"", ""

def extract_aliases(soup, headword):
    aliases = set()
    for tag in soup.find_all(True):
        classes = set(tag.get("class", []))
        if classes.intersection({"inflection", "inflected", "if-g", "if"}):
            text = clean_text(tag.get_text(" "))
            for part in re.split(r"[,;/ ]+", text):
                part = norm_word(part)
                if re.fullmatch(r"[a-z][a-z'-]{2,}", part) and part != headword:
                    aliases.add(part)
    return sorted(aliases)

def find_entry_blocks(soup):
    # OALDPEX packs each homograph (verb/noun/...) in div.oald-entry-root > div.entry.
    blocks = []
    for root in soup.find_all("div", class_=lambda c: c and "oald-entry-root" in c):
        entry = root.find("div", class_=lambda c: c and "entry" in c)
        if entry:
            blocks.append(entry)
    if blocks:
        return blocks[:8]
    return [t for t in soup.find_all("div") if has_class(t, "entry")][:8]

def entry_ipa(block, fallback=""):
    phon = block.find("span", class_=lambda c: c and "phon" in c.split())
    if phon:
        t = clean_text(phon.get_text(" "))
        if t:
            return t.strip("/")
    return fallback

def find_main_senses(block):
    senses_ol = block.find("ol", class_=lambda c: c and "senses_multiple" in c)
    if senses_ol:
        return senses_ol.find_all("li", class_=lambda c: c and "sense" in c, recursive=False)
    senses_single = block.find("ol", class_=lambda c: c and "sense_single" in c)
    if senses_single:
        return senses_single.find_all("li", class_=lambda c: c and "sense" in c, recursive=False)
    return [li for li in block.find_all("li") if has_class(li, "sense")]

def extract_block(block, fallback_ipa=""):
    pos = first_text(block, ("pos", "partofspeech")) or "def."
    defs = []
    ex_en = ""
    ex_zh = ""
    senses = find_main_senses(block)
    if not senses:
        senses = [block]
    for sense in senses:
        def_tag = sense.find("span", class_=lambda c: c and "def" in c.split())
        if not def_tag:
            continue
        en = text_without_zh(def_tag)
        zh_tag = sense.find("deft")
        zh = clean_text(zh_tag.get_text(" ")) if zh_tag else ""
        if en or zh:
            defs.append((en, zh))
        if len(defs) == 1:
            for x_tag in sense.find_all("span", class_=lambda c: c and "x" in c.split()):
                cand_en = text_without_zh(x_tag)
                xt = x_tag.find("xt")
                cand_zh = clean_text(xt.get_text(" ")) if xt else ""
                if cand_en and cand_zh and len(cand_en) <= 180:
                    ex_en = cand_en
                    ex_zh = cand_zh
                    break
        if len(defs) >= 3:
            break
    if not defs:
        defs_en = all_texts(block, ("def", "definition", "d"))
        defs_zh = all_texts(block, ("chn", "chinese", "translation", "tran", "zh"))
        for i, en in enumerate(defs_en[:3]):
            zh = defs_zh[i] if i < len(defs_zh) else ""
            defs.append((en, zh))
    if not defs:
        return None
    return {
        "label": short_pos(pos),
        "fl": pos,
        "ipa": entry_ipa(block, fallback_ipa),
        "defs": defs,
        "exampleEn": ex_en,
        "exampleZh": ex_zh,
    }

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

def extract_entry(key, value):
    headword = norm_word(b2s(key))
    if not re.fullmatch(r"[a-z][a-z'-]*", headword or ""):
        return None
    soup = BeautifulSoup(b2s(value), "html.parser")
    for tag in soup(["script", "style"]):
        tag.decompose()
    display = first_text(soup, ("headword", "hwd", "hw")) or headword
    ipa = first_text(soup, ("phon", "ipa", "pron", "pron-g"))
    ipa = ipa.strip("/")
    blocks = []
    seen_labels = set()
    for block in find_entry_blocks(soup):
        parsed = extract_block(block, ipa)
        if not parsed:
            continue
        label = parsed["label"]
        if label in seen_labels:
            continue
        seen_labels.add(label)
        blocks.append(parsed)
    if not blocks:
        parsed = extract_block(soup, ipa)
        if parsed:
            blocks.append(parsed)
    if not blocks:
        return None
    return {
        "word": headword,
        "display": display,
        "ipa": ipa.strip("/"),
        "senses": blocks[:8],
        "aliases": extract_aliases(soup, headword),
        "audioRefs": extract_audio_refs(soup),
    }

def encode_entry(e):
    lines = ["SDICT1", esc(e["display"]), esc(e["ipa"]), str(len(e["senses"]))]
    for s in e["senses"]:
        lines.append(esc(s["label"]))
        lines.append(esc(s["fl"]))
        lines.append(str(len(s["defs"])))
        for en, zh in s["defs"]:
            lines.append(esc(en) + "\t" + esc(zh))
        lines.append(esc(s.get("exampleEn", "")))
        lines.append(esc(s.get("exampleZh", "")))
    return ("\n".join(lines) + "\n").encode("utf-8")

def irregular_plural_aliases(headword):
    aliases = set()
    irregular = {
        "women": "woman", "men": "man", "children": "child", "feet": "foot", "teeth": "tooth",
        "geese": "goose", "mice": "mouse", "oxen": "ox", "people": "person", "leaves": "leaf",
        "lives": "life", "loaves": "loaf", "knives": "knife", "wives": "wife", "wolves": "wolf",
        "halves": "half", "calves": "calf", "shelves": "shelf", "thieves": "thief", "selves": "self",
        "criteria": "criterion", "phenomena": "phenomenon", "bacteria": "bacterium", "fungi": "fungus",
        "cacti": "cactus", "foci": "focus", "alumni": "alumnus", "syllabi": "syllabus",
        "nuclei": "nucleus", "stimuli": "stimulus",
    }
    for plural, singular in irregular.items():
        if singular == headword:
            aliases.add(plural)
    if headword.endswith("man") and len(headword) > 3:
        aliases.add(headword[:-3] + "men")
    return aliases

def irregular_verb_aliases(headword):
    aliases = set()
    irregular = {
        "arisen": "arise", "arose": "arise", "awoken": "awake", "awoke": "awake", "beaten": "beat",
        "became": "become", "been": "be", "begun": "begin", "began": "begin", "bent": "bend",
        "bidden": "bid", "bitten": "bite", "blown": "blow", "blew": "blow", "broken": "break",
        "brought": "bring", "built": "build", "bought": "buy", "caught": "catch", "chosen": "choose",
        "chose": "choose", "came": "come", "dealt": "deal", "dug": "dig", "done": "do", "did": "do",
        "drawn": "draw", "drew": "draw", "dreamt": "dream", "driven": "drive", "drove": "drive",
        "drunk": "drink", "drank": "drink", "eaten": "eat", "ate": "eat", "fallen": "fall", "fell": "fall",
        "fed": "feed", "felt": "feel", "fought": "fight", "flown": "fly", "flew": "fly",
        "forbidden": "forbid", "forgotten": "forget", "forgot": "forget", "forgiven": "forgive",
        "froze": "freeze", "frozen": "freeze", "given": "give", "gave": "give", "gone": "go", "went": "go",
        "grown": "grow", "grew": "grow", "had": "have", "heard": "hear", "held": "hold", "hidden": "hide",
        "hid": "hide", "kept": "keep", "knelt": "kneel", "known": "know", "knew": "know", "laid": "lay",
        "led": "lead", "leant": "lean", "leapt": "leap", "learnt": "learn", "lent": "lend", "lost": "lose",
        "made": "make", "meant": "mean", "met": "meet", "mistaken": "mistake", "mistook": "mistake",
        "paid": "pay", "proven": "prove", "ridden": "ride", "rode": "ride", "risen": "rise", "rang": "ring",
        "rung": "ring", "ran": "run", "said": "say", "seen": "see", "saw": "see", "sold": "sell",
        "sent": "send", "sewn": "sew", "shaken": "shake", "shook": "shake", "shaven": "shave",
        "shone": "shine", "shown": "show", "shrank": "shrink", "shrunk": "shrink", "slept": "sleep",
        "slid": "slide", "slung": "sling", "smelt": "smell", "sought": "seek", "spoken": "speak",
        "spent": "spend", "spilt": "spill", "spoilt": "spoil", "sprung": "spring", "stood": "stand",
        "stolen": "steal", "stole": "steal", "stuck": "stick", "stung": "sting", "struck": "strike",
        "strung": "string", "striven": "strive", "strove": "strive", "sworn": "swear", "swore": "swear",
        "swept": "sweep", "swollen": "swell", "swam": "swim", "swum": "swim", "swung": "swing",
        "taken": "take", "took": "take", "taught": "teach", "told": "tell", "thought": "think",
        "threw": "throw", "thrown": "throw", "torn": "tear", "trod": "tread", "understood": "understand",
        "woken": "wake", "woke": "wake", "worn": "wear", "wove": "weave", "woven": "weave", "won": "win",
        "written": "write", "wrote": "write",
    }
    for form, lemma in irregular.items():
        if lemma == headword:
            aliases.add(form)
    return aliases

def regular_adjective_aliases(e):
    if not any(
        s.get("fl", "").lower() in ("adjective", "adverb") or s.get("label") in ("adj.", "adv.")
        for s in e["senses"]
    ):
        return set()
    w = e["word"]
    if not re.fullmatch(r"[a-z]{3,}", w):
        return set()
    aliases = set()
    if w.endswith("y") and len(w) > 2 and w[-2] not in "aeiou":
        aliases.update({w[:-1] + "ier", w[:-1] + "iest"})
    elif w.endswith("e") and not w.endswith("ee"):
        aliases.update({w + "r", w + "st"})
    else:
        aliases.update({w + "er", w + "est"})
        if re.search(r"[^aeiou][aeiou][^aeiouwxy]$", w):
            aliases.update({w + w[-1] + "er", w + w[-1] + "est"})
    aliases.discard(w)
    return aliases

def regular_verb_aliases(e):
    if not any(s.get("label") == "v." or s.get("fl", "").lower() == "verb" for s in e["senses"]):
        return set()
    w = e["word"]
    if not re.fullmatch(r"[a-z]{3,}", w):
        return set()
    aliases = {w + "s", w + "ed", w + "ing"}
    if w.endswith("e") and not w.endswith("ee"):
        aliases.add(w + "d")
        aliases.add(w[:-1] + "ing")
    if w.endswith("y") and len(w) > 2 and w[-2] not in "aeiou":
        aliases.add(w[:-1] + "ies")
        aliases.add(w[:-1] + "ied")
    if re.search(r"[^aeiou][aeiou][^aeiouwxy]$", w):
        aliases.add(w + w[-1] + "ed")
        aliases.add(w + w[-1] + "ing")
    aliases.discard(w)
    return aliases

parsed_entries = []
needed_audio = set()
seen = set()

print(f"reading entries: {mdx_path.name}", file=sys.stderr)
for key, val in iter_mdx_entries():
    e = extract_entry(key, val)
    if not e or e["word"] in seen:
        continue
    seen.add(e["word"])
    parsed_entries.append(e)
    for ref in e["audioRefs"]:
        for k in audio_keys_for_ref(ref):
            needed_audio.add(k.replace("\\", "/").lstrip("/").lower())

audio_map = collect_mdd_audio(needed_audio)
entries = []
dat = bytearray()
audio_dat = bytearray()

for e in parsed_entries:
    data_off = len(dat)
    payload = encode_entry(e)
    dat.extend(payload)
    audio_bytes, audio_ext = find_audio(audio_map, e["audioRefs"], e["word"])
    audio_off = 0
    audio_size = 0
    if audio_bytes:
        audio_off = len(audio_dat)
        audio_dat.extend(audio_bytes)
        audio_size = len(audio_bytes)
    row = (e["word"], 1, data_off, len(payload), audio_off, audio_size, audio_ext)
    entries.append(row)
    for alias in sorted(
        set(e["aliases"])
        | regular_verb_aliases(e)
        | irregular_verb_aliases(e["word"])
        | regular_adjective_aliases(e)
        | irregular_plural_aliases(e["word"])
    ):
        if alias != e["word"]:
            entries.append((alias, 0, data_off, len(payload), audio_off, audio_size, audio_ext))

entries.sort(key=lambda r: (r[0], r[1]))
idx_lines = ["# SumatraDict idx v1"]
last = None
for r in entries:
    if r[0] == last:
        continue
    last = r[0]
    idx_lines.append("\t".join(str(x) for x in (r[0], *r[2:])))

(out / "SumatraDict.idx").write_text("\n".join(idx_lines) + "\n", encoding="utf-8")
(out / "SumatraDict.dat").write_bytes(dat)
(out / "SumatraDictAudio.dat").write_bytes(audio_dat)
print(json.dumps({"entries": len(seen), "indexRows": len(idx_lines) - 1, "dataBytes": len(dat), "audioBytes": len(audio_dat)}, ensure_ascii=False))
`;

const pyScript = join(out, "import-oaldpex-dict.tmp.py");
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
