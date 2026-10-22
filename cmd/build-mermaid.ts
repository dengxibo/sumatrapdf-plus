import { existsSync, mkdirSync, copyFileSync } from "node:fs";
import { resolve, join } from "node:path";

// An installed Cargo works too; the workspace toolchain keeps setup local.
const localCargo = resolve("out/rust/cargo/bin/cargo.exe");
const local = existsSync(localCargo);
const cargo = local ? localCargo : "cargo";
const env = { ...process.env };
// Match the C++ application's static CRT.
env.RUSTFLAGS = `${env.RUSTFLAGS || ""} -C target-feature=+crt-static`.trim();
if (local) {
  env.CARGO_HOME = resolve("out/rust/cargo");
  env.RUSTUP_HOME = resolve("out/rust/rustup");
}
const target = resolve("out/mermaid");
const platform = process.argv[3] || "x64";
const triples: Record<string, string> = {
  x64: "x86_64-pc-windows-msvc",
  Win32: "i686-pc-windows-msvc",
  ARM64: "aarch64-pc-windows-msvc",
};
const triple = triples[platform];
if (!triple) throw new Error(`Unsupported Mermaid platform: ${platform}`);
const proc = Bun.spawnSync([cargo, "build", "--release", "--locked", "--target", triple, "--manifest-path", "ext/mermaid/Cargo.toml", "--target-dir", target], {
  env, stdout: "inherit", stderr: "inherit",
});
if (proc.exitCode !== 0) process.exit(proc.exitCode || 1);
const outDir = process.argv[2] || "out/dbg64";
mkdirSync(outDir, { recursive: true });
copyFileSync(join(target, triple, "release", "sumatra_mermaid.lib"), join(outDir, "sumatra_mermaid.lib"));
console.log(`Mermaid static library copied to ${outDir}`);
