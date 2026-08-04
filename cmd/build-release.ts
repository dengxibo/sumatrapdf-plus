import { join } from "node:path";
import { detectVisualStudio2026, runLogged, copyDistributionFonts, copyOpenccData } from "./util";

async function main() {
  const timeStart = performance.now();
  console.log("release build");
  const { msbuildPath } = detectVisualStudio2026();
  const sln = String.raw`vs2022\SumatraPDF.sln`;
  const p = `/p:Configuration=Release;Platform=x64`;
  await runLogged(msbuildPath, [sln, `/t:SumatraPDF`, p, `/m`]);
  copyDistributionFonts(join("out", "rel64"));
  copyOpenccData(join("out", "rel64"));
  const elapsed = ((performance.now() - timeStart) / 1000).toFixed(1);
  console.log(`build took ${elapsed}s`);
  console.log(join("out", "rel64", "SumatraPDF.exe"));
}

await main();
