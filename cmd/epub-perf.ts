// Run EpubPerfSuite against a fixed EPUB and write JSONL metrics.
// Usage: bun cmd/epub-perf.ts [path-to.epub]

import { $ } from "bun";
import * as path from "node:path";

const repoRoot = path.resolve(import.meta.dir, "..");
const epubPath = process.argv[2] ?? path.join(repoRoot, "tmpbench.epub");
const exe = path.join(repoRoot, "out", "dbg64", "SumatraPDF.exe");

console.log(`EpubPerfSuite: ${epubPath}`);
await $`"${exe}" -bench-epub "${epubPath}" -exit-when-done`;
console.log("See out/perf/epub-*.jsonl for structured results.");
