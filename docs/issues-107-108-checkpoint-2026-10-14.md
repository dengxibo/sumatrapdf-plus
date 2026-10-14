# Issues 107 and 108: Options and OCR

Sources: https://github.com/dengxibo/sumatrapdf-plus/issues/107 and
https://github.com/dengxibo/sumatrapdf-plus/issues/108, including the OCR comments on 107.

## Development Plan and Implementation

1. Options scrolling: do not copy clipped control pixels when moving windows.
   Always erase the viewport before repainting transparent labels and group boxes.
   Repaint synchronously after a scroll, keeping navigation and footer outside
   the affected rectangle. Unchanged scroll positions still return early.
2. Editable combos: reserve five DPI-scaled pixels before the child edit so it
   cannot paint over the rounded corner. Use the same closed-face metrics for
   painting and viewport clipping. This addresses a plausible clipping cause;
   the reporter's high-DPI rendering has not yet been reproduced locally.
3. Auto OCR: stop worker-side recursive prefetch. Only a completed current-page
   automatic job may request the next page, on the UI thread, after checking
   the current engine, current page, and Auto OCR state. A prefetch completion
   cannot schedule another prefetch. Turning Auto OCR off removes queued auto
   jobs for the current engine, preserving explicitly requested OCR jobs.
   An inference already running is allowed to finish and cache its result.
4. Automatic enablement: inspect the current PDF page rather than requiring
   current, first, and last pages all to be textless. A native-text cover or
   appendix no longer prevents enabling OCR for a scanned current page.
5. Memory: keep ONNX CPU arenas and memory-pattern caching during consecutive
   recognition. A background timer checks every five seconds and releases OCR
   model sessions after all slots have been idle for 30 seconds. Dictionary,
   model paths, runtime, and semaphore state remain reusable; model sessions
   reload lazily on the next inference. Release partially created sessions on
   initialization failure. Shutdown joins the timer callback before teardown.
6. Thumbnail context menu: recognize one or multiple selected pages through
   the existing explicit current-page queue, without changing page order.
   Add simplified/traditional Chinese translations and release notes.
   Follow-up: multiple selected pages now use the shared document-batch queue
   and completed/total progress notification; one selected page retains its
   single-page notification. Batch totals are published under the queue lock
   before workers can consume the new jobs. Duplicate/invalid selections are
   skipped. Both document context and toolbar menus place current-page OCR
   immediately after region OCR.

## Verification

The initial no-arena measurements below are historical. The final policy uses
arenas with idle session reclamation, as measured in the follow-up section.

- `bun ./cmd/build.ts`: passed, zero warnings and errors.
- `out/dbg64/test_util.exe`: passed all 102548 existing tests.
- OCR benchmark on `tmp/ocralign/geo_fresh.pdf`: Fast, Balanced, and Hybrid
  each recognized all seven tested pages, 21 successful page/profile pairs.
- `tools/test-ocr-memory.ps1 -Pdf tmp/ocralign/geo_fresh.pdf -Pages 7`:
  exit 0, 230 process memory samples, peak private memory 552.4 MiB.
  Late samples fell to 275.04 MiB and subsequently varied with inference.
  Results are in ignored `tmp/issues107-108-memory/`, with recognized text,
  `summary.csv`, and `memory.csv`.
- This is not an A/B memory comparison against the previous executable.
  It does not prove that memory returns to the original startup baseline:
  cached model weights, PDF rendering, recognized text, and Windows allocator
  behavior all contribute to remaining memory.
- Computer Use opened the built application and inspected General/Reading
  options and category switching. Attempts to resize the owned dialog did not
  change its geometry, so scrolling visual verification is still outstanding.
- No real 150/200-percent or mixed-monitor DPI verification was completed.
  No interactive automatic-OCR toggle or thumbnail multiselection test was
  completed; those paths were reviewed in code, while inference was exercised
  by the benchmark.

## Remaining Interactive Regression Checks

- At 100/125/150/200 percent, check editable zoom/font/inverse-search combos:
  both left corners, text, caret, focus, and open list must remain intact.
- Shrink Options until a scrollbar appears. Wheel repeatedly in both directions,
  drag the thumb, click scrollbar arrows, and scroll at both limits. Check labels,
  group borders, fixed navigation/footer, focus traversal, and category switches.
- Move Options between monitors with different DPI, with and without scrolling.
- Open an image-only PDF with AutoOcrScanPages on; verify current-page text
  appears without toggling. Also test a native-text cover plus scanned pages.
- Toggle automatic OCR off during inference: only already-running work may
  finish. Wait to ensure further pages do not run. Toggle back on and turn pages.
- Select one and then multiple thumbnails, invoke recognition, and check that
  only the selected pages get explicit OCR and document geometry stays unchanged.
- Repeat manual OCR across large, varied pages; compare private bytes after
  each run, including the cost of multiple inference slots and model profiles.

## OCR Performance Follow-Up

Same debug executable, PDF (`tmp/ocralign/geo_fresh.pdf`), seven pages, three
profiles, model files, and thread settings. Each comparison runs in a fresh
process; results include model loading on the first page. This is a local
sample, not a general throughput guarantee or an isolated timing microbenchmark.

| Profile | Arena enabled, mean ms/page | Arena disabled, mean ms/page | Disabled/enabled |
| --- | ---: | ---: | ---: |
| Fast | 673 | 1655 | 2.46 |
| Balanced | 2011 | 5070 | 2.52 |
| Hybrid | 1149 | 2864 | 2.49 |

Peak process private bytes were 3337 MiB with arenas and 535 MiB without arenas.
Disabling arenas globally is therefore superseded by idle session reclamation.
Both switches are exposed only when the existing OCR file benchmark is active.

The first idle test exposed shared ownership of `OcrOrtBundle` IO names:
`CopyBundleIoNames` used `str::ReplacePtr`, which transfers the pointer rather
than duplicating it. Freeing independent bundles consequently freed the same
strings twice. WinDbg showed heap corruption (`0xc0000374`) during idle heap
compaction, with the main thread blocked allocating a PDF render buffer.
The copy now uses `str::ReplaceWithCopy`; each bundle owns its names.

After fixing ownership, the seven-page idle/reload test exited successfully.
All 21 page/profile pairs and all three post-idle recognition probes succeeded.
Private bytes fell from approximately 3317 MiB to 66.64 MiB before resuming.
Mean page times were Fast 700 ms, Balanced 2084 ms, Hybrid 1082 ms, comparable
to the arena-enabled baseline. Active peaks remain high: this policy reclaims
idle memory rather than imposing a hard cap during inference.

Reproduction (use a new output directory per run):

```powershell
./tools/test-ocr-memory.ps1 -Pdf tmp/ocralign/geo_fresh.pdf -Pages 7 -Output tmp/ocr-ab/arena -Arena
./tools/test-ocr-memory.ps1 -Pdf tmp/ocralign/geo_fresh.pdf -Pages 7 -Output tmp/ocr-ab/noarena -NoArena
./tools/test-ocr-memory.ps1 -Pdf tmp/ocralign/geo_fresh.pdf -Pages 7 -Output tmp/ocr-ab/idle -Arena -IdleSeconds 38
```

The idle benchmark also probes the same first page immediately before idle and
after session reload, checking its recognized text against the original result.
`page-times.csv` separates first, warm, before-idle, and resume inference timings.
`memory.csv` identifies recognition, idle, and resume sampling phases.

Final same-page probe (`tmp/ocr-ab/idle3`, inference time excludes rasterization
but includes lazy session loading; all text outputs matched byte for byte):

| Profile | Existing sessions, ms | After idle reclamation, ms | Difference, ms |
| --- | ---: | ---: | ---: |
| Fast | 651.91 | 755.02 | +103.11 |
| Balanced | 2253.30 | 2288.95 | +35.65 |
| Hybrid | 994.54 | 1109.75 | +115.21 |

This second successful idle run reclaimed private bytes to 66.54 MiB, exited
with code zero, and passed 21 page/profile recognitions plus six identical-text
same-page probes. First use after idle is not latency-free; these differences
are single observations and not percentile or worst-case guarantees. Idle
cleanup triggers between 30 and 35 seconds after the last slot release.
The final existing test suite run passed 102827 tests; the suite count varies
between runs. All final builds completed with zero warnings/errors.

Menu/progress follow-up verification: existing unit tests passed (102561).
The default executable was running, so compilation/link verification produced
`out/dbg64/SumatraPDF-Plus-menu-verify.exe` using existing dependency libraries.
The verification link succeeded with two expected MSB8012 warnings for the
temporary output-name override; the running default executable was preserved.
No interactive selected-batch progress/cancellation test has been completed.

## 3.7.48: model-path lifetime regression

OCR profiles retained `path::JoinTemp` results through `str::ReplacePtr`, although that helper transfers ownership without copying. The paths could become invalid when the message loop reset its temporary pool. Runtime version text also belongs to ONNX Runtime and must not be treated as an owned allocation. These retained strings now use `str::ReplaceWithCopy`.

The idle benchmark now resets the temporary allocator before waiting for session reclamation, with an independently owned output-directory string. A one-page test across Fast, Balanced and Hybrid, followed by 38 seconds idle, passed all nine first/before-idle/resume probes with identical recognized text and exit code zero (`tmp/ocr-path-lifetime-regression`). Debug build passed with zero warnings and errors. This verifies the ownership correction; it does not establish that every possible CreateSession access violation has the same cause.