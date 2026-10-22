# Built-in native Mermaid renderer

This small Rust C ABI wrapper uses the MIT-licensed `mermaid-rs-renderer`
crate (0.3.1, dependency versions recorded in Cargo.lock).
SumatraPDF converts fenced `mermaid` code blocks to inline SVG before MuPDF
lays out Markdown. No browser or JavaScript runtime is needed.
`usvg` converts SVG markers and text into vector paths. MuPDF does not implement
markers or the per-character positioning lists produced by SVG text normalization;
outlining preserves arrows, font shaping, and separate baselines in multiline labels.

Install the stable MSVC Rust toolchain and Visual Studio C++ tools, then run:

```
bun cmd/build.ts
```

The executable and DLL Visual Studio projects import `vs2022/Mermaid.props`,
which builds the Rust static library before the C++ build and links it into the
application. Cargo is required for development (an installed Cargo or the
workspace toolchain at `out/rust/cargo/bin/cargo.exe`). End users need neither
Rust nor a Mermaid DLL. An old `sumatra_mermaid.dll` is unused.

To build the library separately: `bun cmd/build-mermaid.ts out/dbg64 x64`.
Supported platform names are `x64`, `Win32`, and `ARM64`; install the matching
Rust MSVC target with `rustup target add` before cross-compiling. The library
uses the static C runtime, matching the C++ application.

The ABI exports `sumatra_mermaid_render(const char*, size_t)` and
`sumatra_mermaid_free(char*)`, using the C calling convention. Input is UTF-8;
output is a NUL-terminated UTF-8 SVG string, released with `sumatra_mermaid_free`.
Null indicates failure. Rust panics are caught at the ABI boundary.
Inputs above 256 KiB and outputs above 16 MiB are rejected. This is an in-process
renderer; these limits do not impose a rendering deadline.

Invalid syntax and renderer failures preserve the original code block.
Mermaid compatibility follows the upstream implementation, not the complete
official JavaScript implementation. Test document: `docs/samples/mermaid-test.md`.

Run ABI tests with `cargo test --locked --manifest-path ext/mermaid/Cargo.toml`.
