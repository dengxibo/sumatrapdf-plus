import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

const root = join(import.meta.dir, "..", "src");
const bypasses: string[] = [];
let routed = 0;
function scan(dir: string) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (entry.isDirectory()) {
      if (entry.name !== "utils" && entry.name !== "tools") scan(join(dir, entry.name));
      continue;
    }
    if (!entry.name.endsWith(".cpp") || entry.name === "TestPlugin.cpp") continue;
    const path = join(dir, entry.name);
    const text = readFileSync(path, "utf8");
    // Keep literals intact while removing comments, including URLs in strings.
    const clean = text.replace(/"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\/\/[^\n]*|\/\*[\s\S]*?\*\//g,
      value => value.startsWith("//") || value.startsWith("/*") ? value.replace(/[^\n]/g, " ") : value);
    for (const match of clean.matchAll(/\b(MessageBoxW|MessageBoxA|MessageBox|TaskDialogIndirect|TaskDialog)\s*\(/g)) {
      if (entry.name !== "AppDialogTheme.cpp")
        bypasses.push(`${path}:${clean.slice(0, match.index).split("\n").length}: ${match[1]}`);
    }
    routed += [...clean.matchAll(/\b(AppMessageBoxW|AppTaskDialogIndirect|MsgBox|MessageBoxWarning)\s*\(/g)].length;
  }
}
scan(root);
if (bypasses.length) {
  console.error(bypasses.join("\n"));
  process.exit(1);
}
console.log(`Message-box routing audit passed; ${routed} routed references (including helper definitions).`);
