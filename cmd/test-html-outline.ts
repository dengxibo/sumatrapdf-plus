// Run after bun cmd/build.ts. Exercises the Markdown/HTML refresh crash path.
import { mkdirSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { detectVisualStudio2026, runLogged } from "./util";

const { vsRoot } = detectVisualStudio2026();
const out = resolve("out");
mkdirSync(out, { recursive: true });
const libraries = ["mupdf", "mupdf-libs", "libarchive", "zlib", "libwebp", "libheif", "dav1d", "cmark-gfm"]
  .map(name => `"${resolve("out/dbg64", name + ".lib")}"`).join(" ");
const exe = join(out, "html-outline-test.exe");
const script = join(out, "build-html-outline-test.cmd");
writeFileSync(script, [
  "@echo off",
  `call "${join(vsRoot, "VC/Auxiliary/Build/vcvars64.bat")}" -vcvars_ver=14.44`,
  "if errorlevel 1 exit /b 1",
  `cl /nologo /std:clatest /MT /DDEBUG /Imupdf/include src/tools/html-outline-test.c /Fo"${join(out, "html-outline-test.obj")}" /Fe"${exe}" /link /DEBUG:NONE ${libraries} windowscodecs.lib gdiplus.lib comctl32.lib shlwapi.lib version.lib wininet.lib shcore.lib wintrust.lib crypt32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib`,
  "exit /b %errorlevel%",
  "",
].join("\r\n"));
await runLogged("cmd.exe", ["/c", script]);
await runLogged(exe, []);
