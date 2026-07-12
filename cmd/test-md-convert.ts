// Smoke-test Markdown -> HTML conversion (tables, code blocks, hr).
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

const root = join(import.meta.dir, "..");
const mdPath = join(root, "docs/samples/markdown-features-test.md");
const outDir = join(root, "out");
const htmlPath = join(outDir, "md-features-test.html");

if (!existsSync(mdPath)) {
  console.error("missing test file:", mdPath);
  process.exit(1);
}

const mdConvertSrc = readFileSync(join(root, "src/MdConvert.cpp"), "utf8");
const cssChecks: [string, RegExp][] = [
  ["MdConvert table border", /th, td[\s\S]*border:\s*1px\s+solid\s+#808080/],
  ["MdConvert table header", /thead th[\s\S]*background-color:\s*#f6f8fa/],
  ["MdConvert pre block", /pre[\s\S]*background-color:\s*#f6f8fa[\s\S]*border:\s*none/],
  ["MdConvert hr rule", /hr[\s\S]*border-top:\s*1px\s+solid\s+#808080/],
  ["MdConvert inline code", /p code, li code[\s\S]*background-color:\s*#eff1f3/],
  ["MdConvert sans body font", /font-family:\s*sans-serif\s*!important/],
  ["MdConvert mono code font", /font-family:\s*monospace\s*!important[\s\S]*font-style:\s*normal\s*!important/],
];
let failed = 0;
for (const [name, re] of cssChecks) {
  if (!re.test(mdConvertSrc)) {
    console.error("FAIL:", name);
    failed++;
  } else {
    console.log("OK:", name);
  }
}

const md = readFileSync(mdPath, "utf8");
const mdChecks: [string, RegExp][] = [
  ["sample table", /^\| Feature \|/m],
  ["sample fenced code", /```python/m],
  ["sample hr", /^---$/m],
  ["sample blockquote", /^> This is a blockquote/m],
];
for (const [name, re] of mdChecks) {
  if (!re.test(md)) {
    console.error("FAIL:", name);
    failed++;
  } else {
    console.log("OK:", name);
  }
}

const dumpSrc = join(root, "src/tools/md-html-dump.cpp");
const dumpExe = join(outDir, "dbg64/md-html-dump.exe");
const md4cDir = join(root, "ext/md4c/src");
mkdirSync(join(outDir, "dbg64"), { recursive: true });

if (!existsSync(dumpExe)) {
  const includes = `/I"${md4cDir}" /nologo /EHsc`;
  const sources = [
    `"${dumpSrc}"`,
    `"${join(md4cDir, "md4c-html.c")}"`,
    `"${join(md4cDir, "md4c.c")}"`,
    `"${join(md4cDir, "entity.c")}"`,
  ].join(" ");
  const vcvars =
    "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat";
  const script = `@echo off\r\ncall "${vcvars}"\r\ncl ${includes} ${sources} /Fe:"${dumpExe}"\r\n`;
  const batPath = join(outDir, "build-md-html-dump.bat");
  const fs = await import("node:fs/promises");
  await fs.writeFile(batPath, script);
  const compile = spawnSync(batPath, [], { cwd: root, encoding: "utf8", shell: true });
  if (compile.status !== 0) {
    console.warn("skip md-html-dump compile:", compile.stderr || compile.stdout);
  }
}

if (existsSync(dumpExe)) {
  const run = spawnSync(dumpExe, [mdPath, htmlPath], { encoding: "utf8" });
  if (run.status !== 0) {
    console.error(run.stdout);
    console.error(run.stderr);
    failed++;
  } else {
    const html = readFileSync(htmlPath, "utf8");
    const htmlChecks: [string, RegExp][] = [
      ["html table", /<table>/i],
      ["html th", /<th[^>]*>/i],
      ["html pre/code", /<pre><code/i],
      ["html hr", /<hr/i],
      ["html blockquote", /<blockquote>/i],
      ["html css border", /border:\s*1px\s+solid\s+#808080/i],
    ];
    for (const [name, re] of htmlChecks) {
      if (!re.test(html)) {
        console.error("FAIL:", name);
        failed++;
      } else {
        console.log("OK:", name);
      }
    }
  }
} else {
  console.warn("skip html dump checks (md-html-dump.exe not built)");
}

if (failed) {
  process.exit(1);
}
console.log("Markdown feature checks passed.");
