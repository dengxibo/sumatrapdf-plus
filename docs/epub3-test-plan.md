# EPUB 3 / Media Overlays test plan

## 1. Unit tests (`test_util.exe`, `src/utils/tests/EpubMediaOverlay_ut.cpp`)

Clock values (EPUB 3.3 §H.4, SMIL 3.0):

- full clock `5:34:31.396`, `124:59:36`, `0:05:01.2`, `0:00:04`
- partial clock `09:58`, `00:56.78`
- timecount `76.2s`, `7.75h`, `13min`, `2345ms`, bare `12.345`
- rejects: empty, `-1s`, `1:60:00`, `00:61`, `abc`, `1.2.3`, `5x`, overflow

Path resolution:

- `Text/ch1.xhtml` relative to `OEBPS/`, `../Audio/a.mp3` relative to `OEBPS/Text/`
- `%20` percent-decoding, `./` and `a/../b`
- fragment split (`ch1.xhtml#p1`), empty fragment, fragment-only (`#p1` → same document)
- rejects `http://`, `https://`, `file:///`, `data:`, `C:\x`, `\\server\x`, `../../../x` escaping

OPF:

- manifest `media-overlay` idref, SMIL `media-type="application/smil+xml"`
- `media:duration` with and without `refines`, `media:active-class`, `media:playback-active-class`
- spine `linear="no"`, `rendition:layout` global and per itemref
- `properties="nav scripted"`
- media-overlay idref pointing to a missing / non-SMIL item → diagnostic, not a crash

SMIL:

- body with plain `par` list
- nested `seq` with `epub:textref` and `epub:type` inheritance (skippable `footnote`, escapable `table`)
- `par` without `audio` (text-only), `audio` without `clipEnd` (to end of file), missing `clipBegin`
- `clipEnd <= clipBegin` → diagnostic, clip dropped
- namespaced tags (`smil:par`), comments, CDATA, BOM
- `text` referencing another content document than the overlay owner → diagnostic

Timeline:

- total duration equals sum of clips; clip lookup by time; par lookup by `(doc, fragment)`
- dump format is stable (used for diffing against Thorium/Readium order)

## 2. Fixtures (`out/epub3-fixtures/`, built by `bun cmd/gen-epub3-mo-fixtures.ts`)

The generator synthesizes each phrase with Windows SAPI, concatenates the phrases with 250 ms
gaps so clip times are exact, and encodes MP3 (a core media type) with ffmpeg. Clip times cycle
through the three SMIL clock syntaxes. Books are written to `out/` and not committed.

| Fixture | What it checks |
| --- | --- |
| `mo-basic.epub` | 3 chapters, one audio file per chapter, sentence-level `par`, `media:active-class` with a CSS background |
| `mo-shared-audio.epub` | one audio file shared by two chapters, clips contiguous |
| `mo-nested-seq.epub` | `seq epub:type="table"` (escapable) and `par epub:type="footnote"` (skippable) |
| `mo-no-clipend.epub` | last clip runs to the end of the audio file |
| `mo-no-active-class.epub` | no `media:active-class`: default highlight colour |
| `mo-nonlinear.epub` | `linear="no"` item with an overlay is skipped in continuous playback |
| `mo-bad-refs.epub` | missing audio, missing fragment, `../` escaping path, remote `http:` audio |
| `plain-epub3.epub` | no overlay: no bar |

Validation (2026-09-30, EPUBCheck 5.4.0): all fixtures except `mo-bad-refs` report no errors or
warnings. `mo-bad-refs` reports exactly its deliberate faults (RSC-007 x2, RSC-008, RSC-012,
OPF-014 x2, RSC-031).

Timeline dump of an unpacked book: `test_util.exe --epub-mo-dump <dir>`.

## 3. External corpora

- W3C epub-tests `mol-*` (21 books, `git sparse-checkout` of `tests/mol-*` into
  `out/w3c-epub-tests`). All 21 parse; see the media overlay test matrix in
  `epub3-feature-matrix.md`.
- IDPF `moby-dick-mo`, `wasteland-otf-mo`: not run yet (manual).

## 4. Manual UI checks

1. Open `mo-basic.epub`: narration bar appears at the bottom of the canvas, paused.
2. Play: audio starts, the current sentence is highlighted, time advances.
3. Scroll away while playing: follow pauses and a Follow button appears; Follow, or scrolling
   the sentence back into view, resumes following.
4. Chapter end: playback continues into the next spine document, view follows.
5. Speeds 0.5/0.75/1.0/1.25/1.5/2.0; ±10 s; previous/next sentence.
6. Change theme / font size / window width during playback: highlight re-resolves, no drift.
7. Close tab / reload / open another file while playing: audio stops, no crash, no leak.
8. PDF, CBZ, EPUB 2, MOBI, FB2: TTS uses the same bottom bar (no ±10 s / time). Start from top /
   cursor plays the recording only if that location has an overlay; later chapters without
   audio use TTS from here. Settings → Read Aloud opens from the bar gear
   and from the speaker menu.

### Automated UI run

`powershell -File out\mo-suite.ps1 [-Only <regex>]` runs every check above plus all fixtures and
the 21 W3C books (about 10 minutes, the desktop must stay unused). Each case starts
`SumatraPDF-Plus.exe` with a fresh `-appdata`, drives it with posted `WM_COMMAND`, key and
narration-bar clicks (`out\mo-test.ps1`), saves screenshots to `out\mo-test\<case>-N.png`, and
checks the trace in `out\mo-test\<case>.log`. Results go to `out\mo-test\suite-results.txt`.

The trace is on when the environment variable `SUMATRA_MO_TRACE` is set (lines start with
`mo:` and go to the `-log-to-file` path): detection, each par with its rects, audio source, follow
on/off, skip targets, a state line every 400 ms (`hl=1` means the highlight is on screen), and
unload/resume across document reloads.

## 5. Regression list

- EPUB 2 open, TOC, links, search, selection, theme switch (AGENTS.md reflow checklist).
- Read Aloud (TTS) on EPUB without overlay, highlight + auto-scroll.
- Embedded PDF sound annotation playback (`LookupAudio`) still works and is stopped by narration start.
