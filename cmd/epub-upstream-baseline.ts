// Compare scroll/page-load baseline between this fork and upstream master.
// Requires an upstream build at ../sumatrapdf-upstream/out/dbg64/SumatraPDF.exe
// or set UPSTREAM_EXE env var.
//
// Usage: bun cmd/epub-upstream-baseline.ts [path-to.epub]

import { $ } from "bun";
import * as fs from "node:fs";
import * as path from "node:path";

const repoRoot = path.resolve(import.meta.dir, "..");
const epubPath = process.argv[2] ?? path.join(repoRoot, "tmpbench.epub");
const forkExe = path.join(repoRoot, "out", "dbg64", "SumatraPDF.exe");
const upstreamExe =
  process.env.UPSTREAM_EXE ??
  path.join(repoRoot, "..", "sumatrapdf-upstream", "out", "dbg64", "SumatraPDF.exe");

async function runBench(label: string, exe: string) {
  if (!fs.existsSync(exe)) {
    console.error(`Missing ${label} binary: ${exe}`);
    return null;
  }
  console.log(`\n=== ${label} ===`);
  const result = await $`"${exe}" -bench "${epubPath}" "1-100" -exit-when-done`.quiet();
  return result.stdout.toString();
}

const forkOut = await runBench("fork", forkExe);
const upstreamOut = await runBench("upstream", upstreamExe);

if (forkOut && upstreamOut) {
  console.log("\nPaste both logs into out/perf/upstream-compare.txt for AI regression review.");
  const outDir = path.join(repoRoot, "out", "perf");
  fs.mkdirSync(outDir, { recursive: true });
  const outPath = path.join(outDir, "upstream-compare.txt");
  fs.writeFileSync(outPath, `FORK\n${forkOut}\n\nUPSTREAM\n${upstreamOut}\n`);
  console.log(`Wrote ${outPath}`);
}
