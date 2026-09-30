// Generates EPUB 3 Media Overlays test books into out/epub3-fixtures/.
// Narration is synthesized per phrase with Windows SAPI (System.Speech), concatenated so that
// clipBegin/clipEnd are exact, then encoded to MP3 (a core media type) with ffmpeg.
//   bun cmd/gen-epub3-mo-fixtures.ts
//   java -jar epubcheck.jar out/epub3-fixtures/mo-basic.epub
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { deflateRawSync } from "node:zlib";

const outDir = join("out", "epub3-fixtures");
const tmpDir = join(outDir, "_tmp");
const sampleRate = 22050;
const gapMs = 250;

// ---- zip ----

const crcTable = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(b: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < b.length; i++) {
    c = crcTable[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

type ZipEntry = { name: string; data: Uint8Array; store?: boolean };

function makeZip(entries: ZipEntry[]): Uint8Array {
  const parts: Uint8Array[] = [];
  const central: Uint8Array[] = [];
  let offset = 0;
  for (const e of entries) {
    const name = new TextEncoder().encode(e.name);
    const store = e.store || e.data.length === 0;
    const body = store ? e.data : new Uint8Array(deflateRawSync(e.data));
    const crc = crc32(e.data);
    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, 0x04034b50, true);
    local.setUint16(4, 20, true);
    local.setUint16(6, 0, true);
    local.setUint16(8, store ? 0 : 8, true);
    local.setUint16(10, 0, true);
    local.setUint16(12, 0x5b21, true); // 2025-10-01
    local.setUint32(14, crc, true);
    local.setUint32(18, body.length, true);
    local.setUint32(22, e.data.length, true);
    local.setUint16(26, name.length, true);
    local.setUint16(28, 0, true);
    parts.push(new Uint8Array(local.buffer), name, body);

    const cd = new DataView(new ArrayBuffer(46));
    cd.setUint32(0, 0x02014b50, true);
    cd.setUint16(4, 20, true);
    cd.setUint16(6, 20, true);
    cd.setUint16(8, 0, true);
    cd.setUint16(10, store ? 0 : 8, true);
    cd.setUint16(12, 0, true);
    cd.setUint16(14, 0x5b21, true);
    cd.setUint32(16, crc, true);
    cd.setUint32(20, body.length, true);
    cd.setUint32(24, e.data.length, true);
    cd.setUint16(28, name.length, true);
    cd.setUint32(42, offset, true);
    central.push(new Uint8Array(cd.buffer), name);
    offset += 30 + name.length + body.length;
  }
  const cdSize = central.reduce((n, b) => n + b.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, entries.length, true);
  end.setUint16(10, entries.length, true);
  end.setUint32(12, cdSize, true);
  end.setUint32(16, offset, true);
  const all = [...parts, ...central, new Uint8Array(end.buffer)];
  const out = new Uint8Array(all.reduce((n, b) => n + b.length, 0));
  let p = 0;
  for (const b of all) {
    out.set(b, p);
    p += b.length;
  }
  return out;
}

// ---- audio ----

type Clip = { beginMs: number; endMs: number };

let phraseCounter = 0;

// Synthesizes all phrases in one PowerShell run; returns 16-bit mono PCM per phrase.
function synthesize(phrases: string[]): Int16Array[] {
  const files: string[] = [];
  const lines: string[] = [
    "Add-Type -AssemblyName System.Speech",
    "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer",
    "$v = $s.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Name -like '*Desktop*' -and $_.VoiceInfo.Culture.Name -like 'en*' } | Select-Object -First 1",
    "if ($v) { $s.SelectVoice($v.VoiceInfo.Name) }",
    `$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(${sampleRate}, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)`,
  ];
  for (const text of phrases) {
    const wav = join(tmpDir, `p${phraseCounter++}.wav`);
    files.push(wav);
    const t = text.replace(/'/g, "''");
    lines.push(`$s.SetOutputToWaveFile('${wav}', $fmt); $s.Speak('${t}')`);
  }
  lines.push("$s.SetOutputToNull()");
  const script = join(tmpDir, "synth.ps1");
  writeFileSync(script, "\ufeff" + lines.join("\r\n"));
  const r = spawnSync("powershell", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script], { stdio: "inherit" });
  if (r.status !== 0) {
    throw new Error("speech synthesis failed");
  }
  return files.map((f) => {
    const b = readFileSync(f);
    // find the "data" chunk
    let p = 12;
    while (p + 8 <= b.length) {
      const id = b.toString("ascii", p, p + 4);
      const size = b.readUInt32LE(p + 4);
      if (id === "data") {
        const pcm = b.subarray(p + 8, p + 8 + size);
        return new Int16Array(pcm.buffer.slice(pcm.byteOffset, pcm.byteOffset + pcm.length));
      }
      p += 8 + size + (size & 1);
    }
    throw new Error("no data chunk in " + f);
  });
}

function wavBytes(pcm: Int16Array): Uint8Array {
  const h = new DataView(new ArrayBuffer(44));
  const dataLen = pcm.length * 2;
  h.setUint32(0, 0x46464952, true);
  h.setUint32(4, 36 + dataLen, true);
  h.setUint32(8, 0x45564157, true);
  h.setUint32(12, 0x20746d66, true);
  h.setUint32(16, 16, true);
  h.setUint16(20, 1, true);
  h.setUint16(22, 1, true);
  h.setUint32(24, sampleRate, true);
  h.setUint32(28, sampleRate * 2, true);
  h.setUint16(32, 2, true);
  h.setUint16(34, 16, true);
  h.setUint32(36, 0x61746164, true);
  h.setUint32(40, dataLen, true);
  const out = new Uint8Array(44 + dataLen);
  out.set(new Uint8Array(h.buffer), 0);
  out.set(new Uint8Array(pcm.buffer, pcm.byteOffset, dataLen), 44);
  return out;
}

// Concatenates phrases with silence between them; returns MP3 bytes and exact clip times.
function narrate(phrases: string[], name: string): { mp3: Uint8Array; clips: Clip[]; durationMs: number } {
  const pcms = synthesize(phrases);
  const gap = Math.round((sampleRate * gapMs) / 1000);
  const total = pcms.reduce((n, p) => n + p.length + gap, gap);
  const all = new Int16Array(total);
  const clips: Clip[] = [];
  let pos = gap;
  for (const p of pcms) {
    all.set(p, pos);
    clips.push({ beginMs: (pos * 1000) / sampleRate, endMs: ((pos + p.length) * 1000) / sampleRate });
    pos += p.length + gap;
  }
  const wav = join(tmpDir, name + ".wav");
  const mp3 = join(tmpDir, name + ".mp3");
  writeFileSync(wav, wavBytes(all));
  const r = spawnSync("ffmpeg", ["-y", "-loglevel", "error", "-i", wav, "-codec:a", "libmp3lame", "-b:a", "64k", mp3], {
    stdio: "inherit",
  });
  if (r.status !== 0) {
    throw new Error("ffmpeg failed");
  }
  return { mp3: new Uint8Array(readFileSync(mp3)), clips, durationMs: (total * 1000) / sampleRate };
}

// ---- EPUB building ----

function clock(ms: number, style = 0): string {
  // exercise the three SMIL clock syntaxes (EPUB 3.3 H.4)
  if (style === 1) {
    return (ms / 1000).toFixed(3) + "s";
  }
  if (style === 2) {
    return Math.round(ms) + "ms";
  }
  const t = Math.round(ms);
  const h = Math.floor(t / 3600000);
  const m = Math.floor((t / 60000) % 60);
  const s = ((t % 60000) / 1000).toFixed(3).padStart(6, "0");
  return `${h}:${String(m).padStart(2, "0")}:${s}`;
}

function esc(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

type Doc = {
  id: string;
  title: string;
  paras: string[][]; // paragraphs of sentences
  linear?: boolean;
  overlay?: boolean;
  audio?: string; // shared audio name, defaults to id
  footnote?: string;
  table?: string[][];
};

type Book = {
  name: string;
  title: string;
  docs: Doc[];
  activeClass?: boolean;
  badRefs?: boolean;
  clipEndOmitted?: boolean;
};

const css = `body { font-family: serif; line-height: 1.5; margin: 1em; }
h1 { font-size: 1.4em; }
.-epub-media-overlay-active { background-color: #b3e5fc; }
table { border-collapse: collapse; }
td { border: 1px solid #888; padding: 0.2em 0.5em; }
aside { font-size: 0.9em; border-top: 1px solid #888; }
`;

function xhtml(doc: Doc): { html: string; frags: { id: string; text: string; type?: string }[] } {
  const frags: { id: string; text: string; type?: string }[] = [];
  let n = 0;
  const body: string[] = [];
  const hid = `${doc.id}-h`;
  frags.push({ id: hid, text: doc.title });
  body.push(`<h1 id="${hid}">${esc(doc.title)}</h1>`);
  for (const p of doc.paras) {
    const spans = p.map((s) => {
      const id = `${doc.id}-s${++n}`;
      frags.push({ id, text: s });
      return `<span id="${id}">${esc(s)}</span>`;
    });
    body.push(`<p>${spans.join(" ")}</p>`);
  }
  if (doc.table) {
    const rows = doc.table.map((row, ri) => {
      const cells = row.map((c, ci) => {
        const id = `${doc.id}-t${ri}-${ci}`;
        frags.push({ id, text: c, type: "table-cell" });
        return `<td id="${id}">${esc(c)}</td>`;
      });
      return `<tr>${cells.join("")}</tr>`;
    });
    body.push(`<table id="${doc.id}-table" epub:type="table">${rows.join("")}</table>`);
  }
  if (doc.footnote) {
    const id = `${doc.id}-fn`;
    frags.push({ id, text: doc.footnote, type: "footnote" });
    body.push(`<aside id="${id}" epub:type="footnote"><p>${esc(doc.footnote)}</p></aside>`);
  }
  const html = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="en" lang="en">
<head><meta charset="UTF-8"/><title>${esc(doc.title)}</title><link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
${body.join("\n")}
</body>
</html>
`;
  return { html, frags };
}

function buildBook(book: Book) {
  const files: ZipEntry[] = [{ name: "mimetype", data: new TextEncoder().encode("application/epub+zip"), store: true }];
  const add = (name: string, s: string | Uint8Array) =>
    files.push({ name: "EPUB/" + name, data: typeof s === "string" ? new TextEncoder().encode(s) : s });
  files.push({
    name: "META-INF/container.xml",
    data: new TextEncoder().encode(`<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="EPUB/package.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
`),
  });

  const manifest: string[] = [];
  const spine: string[] = [];
  const metas: string[] = [];
  let totalMs = 0;
  const audioDone = new Map<string, { clips: Clip[]; used: number; durationMs: number }>();

  // shared audio: synthesize all phrases of the docs that share it, in order
  const audioPhrases = new Map<string, string[]>();
  const docFrags = new Map<string, { id: string; text: string; type?: string }[]>();
  for (const d of book.docs) {
    const { html, frags } = xhtml(d);
    docFrags.set(d.id, frags);
    add(`${d.id}.xhtml`, html);
    if (d.overlay !== false) {
      const a = d.audio ?? d.id;
      audioPhrases.set(a, [...(audioPhrases.get(a) ?? []), ...frags.map((f) => f.text)]);
    }
  }
  for (const [a, phrases] of audioPhrases) {
    const { mp3, clips, durationMs } = narrate(phrases, `${book.name}-${a}`);
    add(`audio/${a}.mp3`, mp3);
    manifest.push(`<item id="audio-${a}" href="audio/${a}.mp3" media-type="audio/mpeg"/>`);
    audioDone.set(a, { clips, used: 0, durationMs });
  }

  for (const d of book.docs) {
    const frags = docFrags.get(d.id)!;
    const linear = d.linear === false ? ` linear="no"` : "";
    if (d.overlay === false) {
      manifest.push(`<item id="${d.id}" href="${d.id}.xhtml" media-type="application/xhtml+xml"/>`);
      spine.push(`<itemref idref="${d.id}"${linear}/>`);
      continue;
    }
    const a = d.audio ?? d.id;
    const ad = audioDone.get(a)!;
    let docMs = 0;
    const pars: string[] = [];
    let style = 0;
    const clipOf = (i: number) => ad.clips[ad.used + i];
    const par = (f: { id: string }, i: number, last: boolean) => {
      const c = clipOf(i);
      docMs += c.endMs - c.beginMs;
      style = (style + 1) % 3;
      let audioSrc = `audio/${a}.mp3`;
      let textSrc = `${d.id}.xhtml#${f.id}`;
      if (book.badRefs && i === 1) {
        audioSrc = `audio/missing.mp3`;
      } else if (book.badRefs && i === 2) {
        audioSrc = `http://example.com/remote.mp3`;
      } else if (book.badRefs && i === 3) {
        textSrc = `${d.id}.xhtml#no-such-id`;
      } else if (book.badRefs && i === 4) {
        audioSrc = `../../outside.mp3`;
      }
      const end = book.clipEndOmitted && last ? "" : ` clipEnd="${clock(c.endMs, style)}"`;
      return `<par id="${f.id}-par"><text src="${textSrc}"/><audio src="${audioSrc}" clipBegin="${clock(c.beginMs, style)}"${end}/></par>`;
    };
    const plain = frags.filter((f) => !f.type);
    const cells = frags.filter((f) => f.type === "table-cell");
    const notes = frags.filter((f) => f.type === "footnote");
    let i = 0;
    for (const f of plain) {
      pars.push(par(f, i++, i === frags.length));
    }
    if (cells.length > 0) {
      const inner = cells.map((f) => par(f, i++, i === frags.length)).join("\n      ");
      pars.push(`<seq epub:type="table" epub:textref="${d.id}.xhtml#${d.id}-table">\n      ${inner}\n    </seq>`);
    }
    for (const f of notes) {
      // skippable structure: its par carries the epub:type
      pars.push(par(f, i++, i === frags.length).replace("<par ", `<par epub:type="footnote" `));
    }
    ad.used += frags.length;
    const smil = `<?xml version="1.0" encoding="UTF-8"?>
<smil xmlns="http://www.w3.org/ns/SMIL" xmlns:epub="http://www.idpf.org/2007/ops" version="3.0">
  <body>
    <seq id="${d.id}-seq" epub:textref="${d.id}.xhtml" epub:type="bodymatter chapter">
    ${pars.join("\n    ")}
    </seq>
  </body>
</smil>
`;
    add(`${d.id}.smil`, smil);
    manifest.push(
      `<item id="${d.id}" href="${d.id}.xhtml" media-type="application/xhtml+xml" media-overlay="${d.id}-mo"/>`,
    );
    manifest.push(`<item id="${d.id}-mo" href="${d.id}.smil" media-type="application/smil+xml"/>`);
    spine.push(`<itemref idref="${d.id}"${linear}/>`);
    metas.push(`<meta property="media:duration" refines="#${d.id}-mo">${clock(docMs)}</meta>`);
    totalMs += docMs;
  }

  const hasMo = metas.length > 0;
  const navItems = book.docs.map((d) => `<li><a href="${d.id}.xhtml">${esc(d.title)}</a></li>`).join("\n        ");
  add(
    "nav.xhtml",
    `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="en" lang="en">
<head><meta charset="UTF-8"/><title>Contents</title></head>
<body>
  <nav epub:type="toc" id="toc"><h1>Contents</h1>
    <ol>
        ${navItems}
    </ol>
  </nav>
</body>
</html>
`,
  );
  add("style.css", css);
  const opf = `<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid" xml:lang="en">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:5a0f6c1e-0000-4000-8000-${book.name.padEnd(12, "0").slice(0, 12).replace(/[^0-9a-f]/g, "0")}</dc:identifier>
    <dc:title>${esc(book.title)}</dc:title>
    <dc:language>en</dc:language>
    <meta property="dcterms:modified">2026-09-30T00:00:00Z</meta>
${
  hasMo
    ? `    <meta property="media:duration">${clock(totalMs)}</meta>
    ${metas.join("\n    ")}
    <meta property="media:narrator">Windows SAPI</meta>
${book.activeClass !== false ? `    <meta property="media:active-class">-epub-media-overlay-active</meta>\n` : ""}`
    : ""
}  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="css" href="style.css" media-type="text/css"/>
    ${manifest.join("\n    ")}
  </manifest>
  <spine>
    ${spine.join("\n    ")}
  </spine>
</package>
`;
  add("package.opf", opf);
  const path = join(outDir, `${book.name}.epub`);
  writeFileSync(path, makeZip(files));
  console.log(`wrote ${path}`);
}

const ch1 = [
  ["The rain had stopped by the time the train reached the coast.", "Mara pressed her face to the cold glass."],
  ["Beyond the dunes the sea was grey and quiet.", "She had not seen it in eleven years.", "Nothing about it had changed."],
];
const ch2 = [
  ["The house stood at the end of a sandy lane.", "Its shutters were painted a faded blue."],
  ["A key was hidden under the third stone, exactly where her grandmother had said it would be."],
];
const ch3 = [["That night the wind came back.", "It rattled the windows until dawn."]];

const books: Book[] = [
  {
    name: "mo-basic",
    title: "Media Overlays: basic",
    docs: [
      { id: "ch1", title: "Chapter One", paras: ch1 },
      { id: "ch2", title: "Chapter Two", paras: ch2 },
      { id: "ch3", title: "Chapter Three", paras: ch3 },
    ],
  },
  {
    name: "mo-shared-audio",
    title: "Media Overlays: one audio file for two chapters",
    docs: [
      { id: "ch1", title: "Chapter One", paras: ch1, audio: "book" },
      { id: "ch2", title: "Chapter Two", paras: ch2, audio: "book" },
    ],
  },
  {
    name: "mo-nested-seq",
    title: "Media Overlays: table and footnote",
    docs: [
      {
        id: "ch1",
        title: "Tides",
        paras: [["The tide table below lists the times for the first week."]],
        table: [
          ["Monday", "High water at six"],
          ["Tuesday", "High water at seven"],
        ],
        footnote: "All times are local and approximate.",
      },
    ],
  },
  {
    name: "mo-no-clipend",
    title: "Media Overlays: last clip plays to the end",
    clipEndOmitted: true,
    docs: [{ id: "ch1", title: "Chapter One", paras: ch1 }],
  },
  {
    name: "mo-nonlinear",
    title: "Media Overlays: non-linear spine item",
    docs: [
      { id: "ch1", title: "Chapter One", paras: ch1 },
      { id: "notes", title: "Notes", paras: [["This page is outside the default reading order."]], linear: false },
      { id: "ch2", title: "Chapter Two", paras: ch2 },
    ],
  },
  {
    name: "mo-no-active-class",
    title: "Media Overlays: no active class",
    activeClass: false,
    docs: [{ id: "ch1", title: "Chapter One", paras: ch1 }],
  },
  {
    name: "mo-bad-refs",
    title: "Media Overlays: broken references",
    badRefs: true,
    docs: [{ id: "ch1", title: "Chapter One", paras: ch1 }],
  },
  {
    name: "plain-epub3",
    title: "Plain EPUB 3 without narration",
    docs: [
      { id: "ch1", title: "Chapter One", paras: ch1, overlay: false },
      { id: "ch2", title: "Chapter Two", paras: ch2, overlay: false },
    ],
  },
];

mkdirSync(tmpDir, { recursive: true });
const only = process.argv[2];
for (const b of books) {
  if (!only || b.name === only) {
    buildBook(b);
  }
}
if (existsSync(tmpDir)) {
  rmSync(tmpDir, { recursive: true, force: true });
}
