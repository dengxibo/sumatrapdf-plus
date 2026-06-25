# Sumatra PDF Plus

**Unofficial fork** of [SumatraPDF](https://github.com/sumatrapdfreader/sumatrapdf).  
Not affiliated with [sumatrapdfreader.org](https://www.sumatrapdfreader.org/).

This repository publishes **source code** for **Sumatra PDF Plus**, a Windows build derived from SumatraPDF.  
Because SumatraPDF is licensed under **GNU GPLv3**, modified binaries must be accompanied by corresponding source under the same license. This repo satisfies that requirement.

- User guide (Chinese): [readme.txt](readme.txt)
- Upstream project: https://github.com/sumatrapdfreader/sumatrapdf
- License: [COPYING](COPYING) (GPLv3) + [AUTHORS](AUTHORS)

## Main changes vs upstream SumatraPDF

1. **Chinese EPUB/MOBI layout and TOC** — improved mixed text/image layout; fixes for incorrect or broken table-of-contents navigation.
2. **Faster opening of large ebooks** — loading performance for big EPUB/MOBI/AZW files.
3. **Session-restore crash fix** — when “reopen files from last session” is enabled, launching by double-clicking a file no longer crashes.
4. **Offline dictionary lookup** — double-click a word in the document; optional English–Chinese and Chinese dictionaries via a `dict` folder next to the exe.
5. **Dark mode** — light/dark theme toggle with UI polish (toolbar, word-lookup popup follows theme, etc.).

Product display name: **Sumatra PDF Plus** (`kAppName` in `src/Version.h`). The executable filename is still `SumatraPDF.exe`.

## Build (Windows)

Requirements: Visual Studio 2022 build tools, [Bun](https://bun.sh).

```powershell
bun ./cmd/build.ts          # debug → out/dbg64/SumatraPDF.exe
bun ./cmd/build-all.ts      # release → out/rel64/
```

See [AGENTS.md](AGENTS.md) for contributor notes.

Optional bundled fonts: place `.ttf` / `.otf` / `.ttc` under [fonts/](fonts/) (see [fonts/README.txt](fonts/README.txt)).

## Dictionary data

Dictionary **code** is in this repo (`src/WordLookup.cpp`, etc.). Dictionary **data files** (`.idx` / `.dat`) are not included due to size and licensing; users supply their own under `{exe}\dict\`.

## Disclaimer

Sumatra PDF Plus is a community modification for testing and personal use. Bug reports and pull requests are welcome on this repository only — not on the official SumatraPDF issue tracker.

---

## Upstream SumatraPDF (original readme)

[![Build](https://github.com/sumatrapdfreader/sumatrapdf/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/sumatrapdfreader/sumatrapdf/actions/workflows/build.yml)

SumatraPDF is a multi-format (PDF, EPUB, MOBI, CBZ, CBR, FB2, CHM, XPS, DjVu) reader
for Windows under (A)GPLv3 license, with some code under BSD license (see
AUTHORS).

More Information:

* [Website](https://www.sumatrapdfreader.org/free-pdf-reader)
* [Manual](https://www.sumatrapdfreader.org/manual)
* [Developer Information](https://www.sumatrapdfreader.org/docs/Contribute-to-SumatraPDF)
