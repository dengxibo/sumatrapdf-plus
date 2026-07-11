// Download mermaid.min.js for offline Markdown diagram rendering.
import { existsSync, mkdirSync } from "node:fs";
import { join } from "node:path";

const root = join(import.meta.dir, "..");
const dstDir = join(root, "ext", "mermaid");
const dst = join(dstDir, "mermaid.min.js");
const url = "https://cdn.jsdelivr.net/npm/mermaid@11.6.0/dist/mermaid.min.js";

mkdirSync(dstDir, { recursive: true });
if (existsSync(dst)) {
  console.log("already present:", dst);
  process.exit(0);
}

const res = await fetch(url);
if (!res.ok) {
  console.error("fetch failed:", res.status, url);
  process.exit(1);
}
await Bun.write(dst, await res.arrayBuffer());
console.log("wrote", dst);
