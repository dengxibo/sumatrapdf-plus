// Merges Wikidata given/family names that have Chinese labels into SumatraDict.
// Append-only; no audio rewrite. Source files are ~7 MB total.
//
// Usage:
//   bun cmd/import-wikidata-names-dict.ts
//   bun cmd/import-wikidata-names-dict.ts --probe
//   bun cmd/import-wikidata-names-dict.ts --dict out/dbg64/dict --cache out/dict-src

import { existsSync } from "node:fs";
import { join, resolve } from "node:path";

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

const dictDir = resolve(argValue("--dict", join(process.cwd(), "out", "dbg64", "dict")));
const cacheDir = resolve(argValue("--cache", join(process.cwd(), "out", "dict-src")));
const pythonArg = argValue("--python", "");
const probe = hasFlag("--probe");
const pyScript = join(process.cwd(), "cmd", "import-wikidata-names-dict.py");

if (!existsSync(pyScript)) {
  throw new Error(`missing ${pyScript}`);
}

const extra = probe ? ["--probe"] : [];
const procArgs = pythonArg
  ? [pythonArg, pyScript, "--dict", dictDir, "--cache", cacheDir, ...extra]
  : ["cmd.exe", "/d", "/s", "/c", "python", pyScript, "--dict", dictDir, "--cache", cacheDir, ...extra];

const proc = Bun.spawn(procArgs, {
  stdout: "inherit",
  stderr: "inherit",
  env: { ...process.env, PYTHONIOENCODING: "utf-8" },
});
const exitCode = await proc.exited;
if (exitCode !== 0) {
  throw new Error(`dictionary import failed with exit code ${exitCode}`);
}
