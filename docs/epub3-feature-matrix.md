# EPUB 3 feature matrix (2026-09-30)

Status: **Yes** implemented and checked at runtime, **Impl** implemented but not yet checked in
the UI, **Partial**, **No**. The references are EPUB 3.3 and EPUB Reading Systems 3.3.

## 1. Package and rendering

| Feature | Status | Notes |
| --- | --- | --- |
| OCF container, `mimetype`, `container.xml`, OPF | Yes | MuPDF `epub-doc.c` |
| `unknown epub version: 3.0` warning | Yes | fixed: 2.x and 3.x are accepted |
| Navigation document (`properties="nav"`, token match) | Yes | fixed: `nav scripted` / `nav svg` |
| NCX fallback (EPUB 2) | Yes | unchanged |
| Spine `linear="no"` | Partial | still shown in reading order (moving them would shift stored bookmark pages); narration skips them |
| `rendition:layout` pre-paginated (fixed layout) | Partial | rendered as reflow; `<meta viewport>` sets the page size. Fixed: a page appended during progressive load was laid out 0x0 and could not be scrolled to |
| `page-progression-direction="rtl"` | No | parsed only |
| `page-list` navigation | No | |
| Scripted content | No | never executed, by design |
| Remote resources, `file:` URLs | No | never fetched, by design |
| SVG spine documents | Partial | as MuPDF renders them (text flowed); overlay highlight works on the `<g id>` fragments |

## 2. Media Overlays

| Feature | Status | Notes |
| --- | --- | --- |
| `media-overlay` on manifest items, SMIL items | Yes | `EpubMediaOverlay.cpp` |
| SMIL `body` / `seq` / `par` / `text` / `audio` | Yes | nested `seq` flattened into one par timeline |
| `epub:textref` on `body` / `seq` | Yes | kept for the timeline dump; not used for playback |
| Clock values: full, partial, timecount (`h`/`min`/`s`/`ms`) | Yes | unit tests plus three syntaxes in the fixtures |
| Missing `clipBegin` (0) / `clipEnd` (end of file) | Yes | W3C `mol-audio-no-*` |
| `clipEnd` beyond the audio length | Yes | par ends when the audio ends |
| Fragment `#id` highlighting from MuPDF layout | Yes | per-line rects from the element's box; within a par the current word is chosen by letter-weighted time in the clip (CJK per character). Word-level SMIL is unchanged |
| Par with no fragment (whole document) | Yes | whole text highlighted (`mol-tts_single`) |
| Text-only par (no `audio`) | Yes | spoken with Windows TTS |
| Par whose audio file is missing | Yes | falls back to TTS for that par |
| Par whose fragment is missing | Yes | audio plays, no highlight; start-from-viewport treats it as unknown position |
| `media:active-class` background colour (CSS file or `<style>`) | Yes | highlight band in that colour. Near-white colours (luminance > 0.9) fall back to the Read Aloud colour on light themes; dark themes lighten the colour so the black marker text stays readable |
| `media:active-class` other properties (text colour, fonts) | No | styles are resolved when the chapter is laid out |
| `media:playback-active-class` | No | parsed and reported only |
| `media:duration` (book and per item) | Yes | book elapsed/total time on the bar |
| `media:narrator` | Impl | parsed; not shown |
| Skippability (`epub:type` footnote, pagebreak, ...) | Partial | parsed and flagged; no "skip notes" setting, all pars play |
| Escapability (`table`, `list`, `figure`, ...) | Partial | parsed and flagged; no "escape" command |
| One SMIL document shared by several spine items | Yes | `mo-shared-audio`, `mol-support_xhtml-load*` |
| Continuous playback across spine items | Yes | auto page turn into the next chapter |
| Start from the current viewport | Yes | also after pausing and scrolling elsewhere: Play restarts from the viewport |
| End of narration | Yes | highlight cleared; Play starts again from the viewport |
| Auto follow, pause on manual scroll, resume | Yes | Follow button, and scrolling the highlight back into view resumes following |
| Speed 0.5x-2x | Yes | pitch preserved by Media Foundation; saved as `NarrationSpeed` |
| Previous / next phrase, back / forward 10 s | Yes | Previous within 1.5 s of a phrase start goes to the previous phrase, otherwise restarts it; ±10 s crosses into the next document |
| Audio formats: MP3, AAC/MP4, Ogg, WAV | Yes (MP3, MP4) / Impl (others) | sniffed; Ogg depends on the installed codec |
| Missing audio, missing fragment, `..` path, remote URL | Yes | diagnostics, playback continues to the end |
| Theme / zoom / window resize / single page during playback | Yes | rects re-resolved; follow re-anchors |
| Font size change (document reload) during playback | Yes | position (document, phrase, audio time) restored after the reload |
| Narrow window | Yes | bar drops Time, Speed, −10 s, +10 s in that order |
| Tabs | Yes | switching away pauses, back resumes; playing in another tab stops the old session; closing stops |

## 3. Media overlay test matrix

The fixtures come from `bun cmd/gen-epub3-mo-fixtures.ts`. "Parse" means `test_util --epub-mo-dump`
produced the expected timeline and diagnostics. "UI" means the book was played in SumatraPDF by
`out/mo-suite.ps1` (see the test plan) and the listed behaviour was seen in the trace and
screenshots.

| Book | Parse | UI | What was seen |
| --- | --- | --- | --- |
| mo-basic | Yes | Yes | basic, full run at 2x to "end", controls, ±10 s, start position, relayout, tabs |
| mo-shared-audio | Yes | Yes | plays to the end across chapters |
| mo-nested-seq | Yes | Yes | table and footnote pars play in order |
| mo-no-clipend | Yes | Yes | last phrase plays to the end of the file |
| mo-nonlinear | Yes | Yes | Notes document skipped |
| mo-no-active-class | Yes | Yes | Read Aloud colour |
| mo-bad-refs | Yes | Yes | all 6 pars, TTS for the missing-audio and text-only pars |
| plain-epub3 | Yes | Yes | no narration bar, Read Aloud uses TTS |
| W3C mol-audio, -exceeding-clipend, -no-clipbegin, -no-clipend | Yes | Yes | |
| W3C mol-css | Yes | Yes | colour from an inline `<style>` |
| W3C mol-ignore | Yes | Yes | test is for readers without MO; plays normally here |
| W3C mol-navigation | Yes | Yes | |
| W3C mol-support_xhtml (4 variants) | Yes | Yes | shared SMIL |
| W3C mol-timing-synchronization | Yes | Yes | word-level pars, follow button and scroll-back resume |
| W3C mol-timing-synchronization_multiple_audio | Yes | Yes | switches audio files |
| W3C mol-timing-synchronization_svg | Yes | Yes | publisher colour `#fbf4ef` is near-white; Read Aloud colour used |
| W3C `*-fxl` variants (5) | Yes | Yes | rendered as reflow; the SVG page is reachable after the layout fix |
| W3C mol-tts_single / mol-tts_multi | Yes | Yes | text-only pars spoken with TTS |

Regression: EPUB 2 (`cat.epub`), PDF (`zlib.3.pdf`) and a 206 MB EPUB 2 (`firefly.epub`, detection
63 ms in the background, scrolling) show no narration bar and behave as before.

EPUBCheck 5.4.0: every fixture except `mo-bad-refs` is clean; `mo-bad-refs` reports only its
deliberate faults.

## 4. Limitations

- Audio needs Windows 8 or later (`IMFMediaEngine`). On Windows 7 the bar still appears but the audio
  cannot be played (not tested).
- Only the MuPDF EPUB engine is supported; the EngineEbook fallback has no narration.
- `playback-active-class` is not applied, and `active-class` contributes only its background
  colour.
- No UI for skippable or escapable structures.
- TTS phrases use the TTS voice and rate from Read Aloud, not the narration speed; the bar time does
  not advance during TTS.
- TTS and recorded narration share the bottom bar, follow logic and Settings → Read Aloud page.
- No real fixed layout, `page-progression-direction` or `page-list`.
- `linear="no"` items still appear in the reading order.
