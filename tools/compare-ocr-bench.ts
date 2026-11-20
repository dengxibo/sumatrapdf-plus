// Compare outputs from tools/test-ocr-memory.ps1 without accepting accuracy loss.
// Usage: bun tools/compare-ocr-bench.ts before-directory after-directory
import { join } from "node:path";

const [before, after] = process.argv.slice(2);
if (!before || !after) throw new Error("Expected before and after benchmark directories");
async function summary(folder: string) {
  const rows = (await Bun.file(join(folder, "summary.csv")).text()).trim().split(/\r?\n/);
  return new Map(rows.slice(1).map(row => {
    const fields = row.split(",");
    return [fields[0], { ms: Number(fields[1]), ok: Number(fields[7]) }] as const;
  }));
}
const oldTimes = await summary(before);
const newTimes = await summary(after);
for (const profile of ["tiny", "small", "hybrid"]) {
  const oldTime = oldTimes.get(profile);
  const newTime = newTimes.get(profile);
  if (!oldTime || !newTime || !oldTime.ok || oldTime.ok !== newTime.ok)
    throw new Error(`${profile}: successful page counts differ or are zero`);
  const oldText = await Bun.file(join(before, `${profile}_all.txt`)).text();
  const newText = await Bun.file(join(after, `${profile}_all.txt`)).text();
  if (!oldText.trim() || oldText !== newText) throw new Error(`${profile}: recognized text differs`);
  console.log(`${profile}: ${oldTime.ms} -> ${newTime.ms} ms/page; ${(oldTime.ms / newTime.ms).toFixed(2)}x; ${newTime.ok} pages, identical text`);
}
