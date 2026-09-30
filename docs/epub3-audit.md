# EPUB 3 audit (SumatraPDF Plus)

Date: 2026-09-30. Scope: EPUB 3.3 / Reading Systems 3.3 / Media Overlays 3.3 as implemented by
SumatraPDF Plus on top of its vendored MuPDF.

References:

- EPUB 3.3 — https://www.w3.org/TR/epub-33/ (package document, OCF, content documents, media overlays §9)
- EPUB Reading Systems 3.3 — https://www.w3.org/TR/epub-rs-33/
- EPUB 3.4 (CR drafts) — media overlay text is unchanged from 3.3 for the parts used here
- W3C EPUB tests — https://github.com/w3c/epub-tests (`mol-*` media overlay tests)
- EPUBCheck — https://github.com/w3c/epubcheck
- IDPF samples — https://github.com/IDPF/epub3-samples (`moby-dick-mo`, `wasteland-otf-mo`, …)

## 1. How an EPUB is opened today

1. `EngineCreate.cpp`: `gEnableEpubWithPdfEngine = true`, so every `.epub` goes to
   `CreateEngineMupdfFromFile` (MuPDF `epub_document_handler`). `EngineEbook` (`EbookDoc.cpp`,
   Sumatra's own HTML formatter) is only a fallback if MuPDF fails to open the file.
2. MuPDF (`mupdf/source/html/epub-doc.c`) parses `META-INF/container.xml`, the first `rootfile`, the
   OPF `manifest` + `spine`, `dc:title` / `dc:creator`, the NCX (`spine@toc`) or the nav document
   (`manifest item@properties == "nav"`).
3. Each spine `itemref` whose `idref` resolves becomes a MuPDF *chapter*. A chapter is parsed by
   `fz_parse_html` → box tree (`fz_html_box`, keeps `id`, `tag`, `href`) → `fz_layout_html` →
   flows (`fz_html_flow`, each with `x,y,w,h` and a pointer to its box).
4. `EngineMupdf` flattens chapters into Sumatra page numbers:
   `pageNo = reflowChapterStartPage[chapter] + pageInChapter + 1` (trailing blank pages trimmed by
   `EffectiveReflowChapterPageCount`). Counting is progressive/background (`reflowChaptersCounted`).
5. Theme / font / document color changes restyle and relayout the whole book (see AGENTS.md
   *Reflowable EPUB* rules; `ReflowMupdfRelayoutUiAfterCanvasResize`).

## 2. What MuPDF already does

| Area | State |
| --- | --- |
| OCF container, prefixed container | yes (`read_container_and_prefix`) |
| `encryption.xml` | encrypted entries hidden (no decryption); font obfuscation not handled |
| OPF manifest / spine | yes; `linear`, `properties`, `media-overlay`, `rendition:*` **ignored** |
| NCX TOC | yes |
| nav TOC | yes, but only when `properties` is exactly `nav` (fixed: token match) |
| `page-list`, `landmarks` | not read |
| XHTML5 + CSS | yes (MuPDF HTML/CSS subset; Sumatra adds many layout fixes) |
| SVG in XHTML / SVG spine items | inline SVG images via MuPDF SVG; basic |
| Fixed layout (`pre-paginated`) | not supported: laid out as reflow |
| `audio` / `video` elements | not rendered (no timed media in MuPDF) |
| Scripting | none (good: RS 3.3 allows no scripting) |
| Remote resources | never fetched (archive-only loader) |
| Fragment `#id` → position | `fz_find_html_target` gives y of first flow of the box |
| Fragment `#id` → rectangles | **missing**, added as a small bridge (see §5) |
| SMIL / Media Overlays | **missing** |

## 3. Sumatra-side building blocks that are reused

- `ReadAloudHighlight.cpp`: TTS-driven highlight + auto-follow (safe-zone ratios, "user scrolled
  away" detection via `readAloudScrollFromCode`, smooth scroll timer). Media Overlays reuse the
  same painting (`PaintFindMatchHighlightRectangles`) and the same follow policy, but through a
  separate state object so TTS and narration never share mutable state.
- `LookupAudio.cpp`: Media Foundation decode (`IMFSourceReader`) → WASAPI. It is fire-and-forget,
  cannot seek, pause, or change rate, so narration uses a new `IMFMediaEngine` player instead.
- `SelectionToolbar.cpp` + `FloatingPopupStyle`: rounded floating bar, theme-aware. The narration
  bar reuses the same drawing primitives.
- `utils/Archive.h` (`MultiFormatArchive`, lazy mode): reads OPF / SMIL / audio entries without
  touching MuPDF's context, so parsing and audio loading can run off the UI thread.
- `utils/HtmlPullParser.h`: streaming XML tokenizer (namespace prefixes handled by `NameIsNS`).

## 4. Gaps found (not media overlays)

1. nav detection needs space-separated `properties` token matching (`nav scripted`, `nav svg`).
   Fixed in `epub-doc.c` (`path_from_prop` uses token match).
2. `unknown epub version: 3.0` warning for every EPUB 3. Fixed (accept 2.x / 3.x).
3. `linear="no"` spine items are shown in reading order. Changing the chapter count would shift
   stored page numbers of existing bookmarks/annotations, so this is recorded as a limitation;
   media-overlay continuous playback does honor `linear="no"` (skips them).
4. Fixed layout, `page-progression-direction`, `page-list` are not implemented (see matrix).

## 5. Architecture chosen for Media Overlays

```
EpubMediaOverlay.{h,cpp}        pure parsing, no UI, unit-tested
  - clock values → int64 microseconds
  - OCF path resolution (percent-decode, dot segments, rejects schemes / escaping the root)
  - OPF: manifest, spine (linear, media-overlay), media:duration / active-class /
         playback-active-class / narrator, rendition:layout
  - SMIL: body/seq/par/text/audio, epub:textref, epub:type (inherited), skippable/escapable flags
  - flattened timeline (one entry per par) + validation diagnostics + text dump

MediaOverlayAudio.{h,cpp}       IMFMediaEngine (audio only) player: open from memory,
                                play/pause/seek/rate/current time; COM on UI thread, decode on MF threads

mupdf epub-doc.c / html-outline.c
  fz_epub_fragment_rects()      chapter + element id → per-line rects in chapter page coordinates
                                (walks flows whose box ancestor is the target box)

EngineMupdf.cpp
  EngineMupdfEpubFragmentRects() archive path + id → Sumatra page + page-space rects
                                 (under the engine's MuPDF lock, maps chapter pages → Sumatra pages)

MediaOverlayPlayer.{h,cpp}      session: timeline cursor, clip scheduling, cross-document
                                continuation, highlight rects, auto-follow, floating control bar
```

Design rules:

- Plain EPUB pays nothing: detection reads only `container.xml` + OPF on a background thread after
  load, and only for `.epub`. SMIL is parsed and audio read only when playback starts.
- Position identity is semantic (spine path + fragment id + par index), never a page number, so
  relayout (theme, font, zoom, window size) only re-resolves rectangles.
- The element rectangles come from MuPDF's own layout (structural anchor), not text search.
- `media:active-class`: MuPDF styles are resolved at parse time; re-parsing a chapter per clip is
  too expensive. The player reads the publisher CSS rule for that class and uses its
  `background-color` as the overlay highlight colour (drawn with the same marker blend as
  Read Aloud in dark themes); a rule with only a text `color` falls back to the Sumatra read-aloud
  colour. `playback-active-class` is parsed and reported but not applied.
- Security: only archive entries are read; `http:`, `https:`, `file:`, `data:` and any other
  scheme, absolute Windows paths, and `..` escaping the container root are rejected. No network,
  no scripting, no temp files (audio is played from memory). The audio container is sniffed
  (ID3/MPEG sync, `ftyp`, `OggS`, `RIFF/WAVE`) and Media Foundation gets a fixed format hint, so
  playlist formats (`.m3u8`, `.asx`) that could make it fetch network resources are never parsed.

## 6. Implementation notes (2026-09-30)

- Detection runs from the first canvas paint of an EPUB tab (`MediaOverlayOnCanvasPaint` →
  worker thread), then posts back to the UI thread. The bar only exists for tabs with overlays.
- The playback clock is a 40 ms thread timer. A par ends at `clipEnd - 20 ms` or when the audio
  ends; the next par in the same file within 40 ms of the previous `clipEnd` plays without a seek.
- A new audio file is loaded on a worker thread; the next distinct file in reading order is
  prefetched into one slot.
- One SMIL document may reference several content documents: spine items that share the overlay
  that just finished are not replayed.
- MuPDF stores a word flow's baseline in `flow->y` with `h` = em; highlight rects span
  0.9 em above to 0.3 em below the baseline. Images use their box.
- Within a SMIL par, the current word is chosen by letter-weighted time in `[clipBegin, clipEnd]`
  (CJK per character, 120 ms lag). Word-level overlays already one `par` per word stay as they are.
- Start position: the viewport top is mapped to a spine document through
  `fz_epub_chapter_for_path` + the reflow chapter page range, then the first par whose text ends
  below the viewport top is found by binary search over layout positions.
- Manual view changes arrive through `ReadAloudOnUserViewChanged`. Following stops only once the
  active text is off screen; it resumes when the text is back in the safe zone (10–82 % of the
  viewport height) or on Follow.
