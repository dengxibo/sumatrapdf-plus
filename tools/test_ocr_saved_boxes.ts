// Exercise the same Song font descriptor and text matrices as saved OCR PDFs.
// Usage: bun tools/test_ocr_saved_boxes.ts path/to/SumatraPDF-Plus.exe
import { resolve } from "node:path";
import { mkdirSync } from "node:fs";

const exe = resolve(process.argv[2] || "out/dbg64/SumatraPDF-Plus.exe");
const folder = resolve("out/test-ocr-saved-boxes");
mkdirSync(folder, { recursive: true });
const content = `q
1 0 0 -1 0 600 cm
BT
/OCR0 1 Tf
3 Tr
31 0 0 -30.833333 192 200.83333 Tm <7684> Tj
30 0 0 -30.833333 230 200.83333 Tm <65B9> Tj
31 0 0 -30.833333 192 260.83333 Tm <7684> Tj
30 0 0 -30.833333 230 260.83333 Tm <65B9> Tj
0 Tr
31 0 0 -30.833333 192 320.83333 Tm <7684> Tj
ET
Q
`;
const objects = [
  "<< /Type /Catalog /Pages 2 0 R >>",
  "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
  "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 600] /Resources << /Font << /OCR0 4 0 R >> >> /Contents 7 0 R >>",
  "<< /Type /Font /Subtype /Type0 /BaseFont /Song /Encoding /UniGB-UTF16-H /DescendantFonts [5 0 R] >>",
  "<< /Type /Font /Subtype /CIDFontType0 /BaseFont /Song /CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 5 >> /FontDescriptor 6 0 R >>",
  "<< /Type /FontDescriptor /FontName (Song) /FontBBox [-200 -200 1200 1200] /Flags 6 /ItalicAngle 0 /Ascent 1000 /Descent -200 /StemV 80 >>",
  `<< /Length ${content.length} >>\nstream\n${content}endstream`,
];
let pdf = "%PDF-1.4\n";
const offsets = [0];
for (let n = 0; n < objects.length; n++) {
  offsets.push(pdf.length);
  pdf += `${n + 1} 0 obj\n${objects[n]}\nendobj\n`;
}
const xref = pdf.length;
pdf += `xref\n0 8\n0000000000 65535 f \n`;
for (const offset of offsets.slice(1)) pdf += `${String(offset).padStart(10, "0")} 00000 n \n`;
pdf += `trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
const path = `${folder}/saved.pdf`;
await Bun.write(path, pdf);
const report = `${folder}/glyphs.txt`;
const run = Bun.spawn([exe, "-appdata", `${folder}/appdata`, "-render", "1", path], {
  env: { ...process.env, SUMATRA_TEST_GLYPH_COORDS: report }, stdout: "pipe", stderr: "pipe",
});
if (await run.exited) throw new Error(await new Response(run.stderr).text());
const rows = (await Bun.file(report).text()).trim().split(/\r?\n/).map(r => r.split(" "));
const glyphs = rows.filter(r => r[0] !== "U" && ["7684", "65b9"].includes(r[1]));
if (glyphs.length !== 5) throw new Error(`Unexpected glyph count: ${glyphs.length}`);
for (let n = 0; n < 4; n++) {
  const r = glyphs[n].slice(2).map(Number);
  const expectedY = n < 2 ? 170 : 230;
  if (Math.abs(r[1] - expectedY) > 1 || Math.abs(r[3] - 37) > 1)
    throw new Error(`Saved OCR box changed: ${glyphs[n].join(" ")}; expected y=${expectedY}, height=37`);
}
// The old accurate outline for this visible glyph must remain unchanged,
// including when it follows invisible text in the same content stream.
const visible = glyphs[4].slice(2).map(Number);
if (visible.join() !== "192,265,31,89") throw new Error(`Visible text bounds changed: ${visible}`);
const utf8Rows = rows.filter(r => r[0] === "U");
for (let n = 0; n < 4; n++) {
  for (const row of utf8Rows.slice(n * 3, n * 3 + 3)) {
    if (row.slice(3).join() !== glyphs[n].slice(2).join())
      throw new Error("UTF-8 and UTF-16 OCR boxes differ");
  }
}
console.log("PASS: saved OCR boxes restored; UTF-8/UTF-16 agree; visible text unchanged.");
