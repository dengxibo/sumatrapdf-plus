import { join, resolve, sep } from "node:path";
import { detectVisualStudio2026, runLogged, copyDistributionFonts, copyOpenccData, copyOcrSidecar } from "./util";
import { clearDirPreserveSettings } from "./clean";

let clean = false;

let t = `/t:SumatraPDF`;

async function main() {
  const timeStart = performance.now();

  console.log("debug build");
  if (clean) {
    const dirs = [join("out", "dbg64")];
    for (const dir of dirs) {
      clearDirPreserveSettings(dir);
    }
  }

  const { msbuildPath } = detectVisualStudio2026();
  const sln = String.raw`vs2022\SumatraPDF.sln`;
  // const t = `/t:SumatraPDF;test_util`;
  const outDirArg = process.argv.indexOf("--out-dir");
  const outDir = outDirArg >= 0 && process.argv[outDirArg + 1] ? process.argv[outDirArg + 1] : join("out", "dbg64");
  const outputProperty = outDirArg >= 0 ? `;OutDir=${resolve(outDir)}${sep};SumatraSeparateBuildIntermediate=true` : "";
  const p = `/p:Configuration=Debug;Platform=x64${outputProperty}`;
  await runLogged(msbuildPath, [sln, t, p, `/m`]);

  copyDistributionFonts(outDir);
  copyOpenccData(outDir);
  copyOcrSidecar(outDir);
  // await runLogged(resolve(join(outDir, "test_util.exe")), [], outDir);

  const elapsed = ((performance.now() - timeStart) / 1000).toFixed(1);
  console.log(`build took ${elapsed}s`);
}

await main();
