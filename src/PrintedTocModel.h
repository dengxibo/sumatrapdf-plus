/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Printed TOC reconstruction: shared data model, central configuration and
// diagnostics (JSON dumps + debug overlay data).
//
// This module is the vocabulary of the PrintedTocReconstructor pipeline
// (scanned book -> printed TOC pages -> OCR -> geometry parsing -> TOC tree).
// It deliberately avoids EngineBase / MainWindow dependencies so every stage
// stays deterministic, side-effect free and unit-testable from plain JSON.
//
// Coordinate conventions:
//   - "page space"  = PDF points (1/72"), origin top-left, y grows down.
//                     Same space as EngineBase text coords / ScanLine.
//   - "image space" = pixels of the rasterized bitmap handed to OCR.
//                     Always recorded together with imgW/imgH so consumers
//                     can normalize without knowing the render DPI.
// Page numbering: PtPageData::pageIndex, PtEntryCandidate::tocSourcePage,
// PtPageAnchor::pdfPage and PtEntryCandidate::resolvedPdfPage are 1-based PDF
// page numbers (same convention as EngineBase/Canvas pageNo). Only
// PtocOverlayPage::pageNo is display data and shares it.
// All tunable thresholds live in PrintedTocConfig and are expressed in
// normalized units (page/column width ratios, median line height, median
// character width) - never in device pixels.

#pragma once

#include "utils/BaseUtil.h"
#include "OcrOnnx.h"

#include <optional>

struct ScanLine; // ExtractPdfToc.h (shared document line, page space)

// A geometry-only rectangle. Distinct from RectF so pipeline data can never
// silently mix GDI rects with page-space coordinates.
struct PtBoxF {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;

    float Dx() const { return x1 - x0; }
    float Dy() const { return y1 - y0; }
    float MidY() const { return (y0 + y1) * 0.5f; }
    bool IsEmpty() const { return x1 <= x0 || y1 <= y0; }
    void UnionWith(const PtBoxF& o) {
        if (o.IsEmpty()) {
            return;
        }
        if (IsEmpty()) {
            *this = o;
            return;
        }
        x0 = std::min(x0, o.x0);
        y0 = std::min(y0, o.y0);
        x1 = std::max(x1, o.x1);
        y1 = std::max(y1, o.y1);
    }
};

// ---------------------------------------------------------------------------
// OCR view of a page (the parser input; independent of OcrBox)
// ---------------------------------------------------------------------------

struct PtToken {
    char* text = nullptr; // UTF-8, owned by the enclosing PtPageData
    PtBoxF box;           // page space
    float confidence = 0; // 0 = unknown. RapidOCR det score exists; rec score
                          // is not plumbed through OcrBox yet.
};

// One reconstructed visual line. Line reconstruction from raw tokens is
// P1-B (ReconstructOcrLines); stages before that may carry tokens only.
struct PtRecLine {
    Vec<PtToken> tokens;
    PtBoxF box; // union of token boxes
    int pageIndex = 0;
    int columnIndex = 0; // assigned by PtReconstructLines (token-level column cut)
    float baselineY = 0; // box.y1 by default; refined when baseline data exists
};

// Ownership rules for the Vec-bearing pipeline structs (PtPageData holds
// Vec<PtRecLine*>, pages live in Vec<PtPageData*>): Vec copies its element
// ARRAY with a plain memcpy, so a struct-with-Vecs stored by value would keep
// inner buffers aliasing the source array - and the aliases would dangle as
// soon as the owning array reallocs. Heap objects held by pointer (the same
// pattern as the overlay / capture stores) never memcpy their Vec members.
struct PtPageData {
    int pageIndex = 0;
    float width = 0;  // page space
    float height = 0; // page space
    Vec<PtToken> tokens;
    Vec<PtRecLine*> lines;

    void Free() {
        for (PtToken& t : tokens) {
            str::Free(t.text);
        }
        tokens.Reset();
        for (PtRecLine* l : lines) {
            delete l;
        }
        lines.Reset();
    }
};

// ---------------------------------------------------------------------------
// Printed page labels ("12", "xiv", "II", "2-18", "A-12")
// ---------------------------------------------------------------------------

enum class PtPageLabelType {
    Unknown = 0,
    Arabic,
    RomanLower,
    RomanUpper,
    Compound, // "2-18", "A-12": section-scoped page number
};

struct PtPageLabel {
    PtPageLabelType type = PtPageLabelType::Unknown;
    char rawText[32] = {0};

    // Ordinal when convertible: arabic value, roman value, or the primary
    // (last) number of a compound label. Compound labels cannot map to a
    // global page index without section info - hasOrdinal stays false.
    int ordinal = 0;
    bool hasOrdinal = false;

    // For Compound: components, e.g. "A-12" -> section='A', number=12.
    char section[8] = {0};
    int number = 0;
};

// ---------------------------------------------------------------------------
// Stage outputs (kept as structs so stages never communicate via strings)
// ---------------------------------------------------------------------------

// Structural per-page features for TOC page discovery (P1-A).
struct PtTocPageScores {
    float endingPageNumberRatio = 0;    // lines ending in a page label / lines
    float pageNumberXAlignment = 0;     // how tightly page anchors cluster in x
    float titleToPageGapRatio = 0;      // median gap between title and anchor
    float monotonicPageNumberScore = 0; // anchor numbers increase down the page
    float indentationBandScore = 0;     // distinct left-indented bands present
    float shortLineRatio = 0;           // short text lines / lines
    float tocKeywordScore = 0;          // "目录"/"Contents" heading evidence
    float repeatedRightAnchorScore = 0; // anchor column shared with sibling pages
    float total = 0;                    // weighted combination
    int columnIndex = 0;
};

// One candidate TOC entry before hierarchy/mapping/validation.
struct PtEntryCandidate {
    char* title = nullptr; // cleaned title (owned)
    char* rawText = nullptr;

    PtPageLabel printedPage;

    std::optional<int> resolvedPdfPage;

    PtBoxF titleBox;
    PtBoxF pageNumberBox;

    int tocSourcePage = 0; // printed TOC page the entry was parsed from (1-based)
    int column = 0;
    int level = 1;

    float indentNormalized = 0; // (titleBox.x0 - column.x0) / column.width

    // Geometry context for document-level indent clustering (page space):
    // median title char width and the width of the owning column.
    float charWidth = 0;
    float columnWidth = 0;

    // Split confidence (section 十三); each stage owns one component.
    float pageDetectionConf = 0;
    float parseConf = 0;
    float pageAnchorConf = 0;
    float hierarchyConf = 0;
    float pageMappingConf = 0;
    float bodyValidationConf = 0;

    // Debug overlay relationships
    int mergedWithPrev = 0; // >0: continuation chain length absorbed from above
    int ignoredLeader = 0;  // leader noise region index at parse time

    // anchorless 第X章-style line turned into a level-1 entry (printed page
    // inferred from the next anchored entry; see PtMergeCrossPage)
    bool chapterHeading = false;

    void Free() {
        str::Free(title);
        title = nullptr;
        str::Free(rawText);
        rawText = nullptr;
    }
};

// Piecewise printed->PDF page mapping (P2-B).
struct PtPageMappingSegment {
    PtPageLabelType labelType = PtPageLabelType::Arabic;
    int printedBegin = 0;
    int printedEnd = 0;
    int offset = 0; // pdfPage = printedPage + offset
    float confidence = 0;
};

// (printed, pdf) evidence pair for the P2-B mapping, e.g. a page number
// OCR'd from a body page footer or the header of a chapter page.
struct PtPageAnchor {
    PtPageLabelType type = PtPageLabelType::Arabic;
    int printed = 0;
    int pdfPage = 0; // 1-based
    float conf = 1.0f;
};

// Input for PtBuildDocumentToc. All vectors are owned by the caller and are
// not modified (body pages may be reconstructed internally).
struct PtDocTocInput {
    Vec<PtPageData*>* pages = nullptr; // available pages, any order
    int nPdfPages = 0;                 // total pages of the PDF; 0 = unknown

    // Optional footer/header (printed, pdf) evidence for the mapping. When
    // empty, a low-confidence seed anchor is derived from the TOC extent.
    Vec<PtPageAnchor>* anchors = nullptr;

    // Optional body pages used by P3 title validation. May alias ->pages.
    Vec<PtPageData*>* bodyPages = nullptr;
};

// ---------------------------------------------------------------------------
// Central configuration (section 二十一: no scattered magic numbers)
// ---------------------------------------------------------------------------

struct PrintedTocConfig {
    // --- TOC page discovery (P1-A). Unit: page/column width ratio unless
    // noted. A page is a TOC page when total >= pageScoreMin.
    float pageScoreMin = 0.50f;
    int searchFrontPct = 15;         // % of book scanned in the fast pass
    int searchFrontMinPages = 8;     // floor for tiny books
    int searchFrontMaxPages = 60;    // cap (spec: min(40~60, 15~20%))
    float anchorRightMin = 0.80f;    // anchor.x1 / column right edge >= this
    float anchorXClusterEps = 1.5f;  // 1D anchor cluster epsilon, in median
                                     // line height units
    float gapTitleAnchorMin = 0.03f; // min title->anchor gap, column ratio
    float gapTitleAnchorMax = 0.60f; // max plausible leader/gap span

    // --- line parsing (P1-B) ---
    float lineMidYTol = 0.40f;       // same-line if |dy| <= this * median height
    float leaderNoiseGapMin = 0.05f; // gap that starts a leader region

    // --- columns (P1-C) ---
    float colGapMinRatio = 0.05f;   // min interior whitespace band to split
                                    // columns, page width ratio
    float colMaxCrossRatio = 0.10f; // max fraction of tokens crossing a cut
    // Must stay above the decorative noise floor: a vertical 目录 banner plus
    // the page footer on the same side can reach ~0.17 * page width on an
    // otherwise single-column page (real two-column TOCs are >= ~0.30 wide).
    float colMinSideWidth = 0.22f; // both sides must be at least this wide

    // --- continuation merge (P1-C) ---
    float continuationMaxGap = 1.8f; // vertical gap in median line heights
    int continuationMaxChain = 4;    // max lines merged into one entry

    // --- hierarchy (P2-A) ---
    // Must stay below 1.0: a one-char deeper indent is exactly 1.0 median
    // char widths and must start a NEW band, not join the previous one.
    float indentClusterEpsilon = 0.6f; // in median char width units
    int indentMinBandLines = 2;        // bands with fewer entries are noise

    // --- page mapping (P2-B) ---
    float mappingVoteMinRatio = 0.6f; // winning offset needs this vote share
    int mappingMinSamples = 3;        // below this, offset = low confidence

    // --- body validation (P3) ---
    float bodyMatchThreshold = 0.55f; // CJK char-sim threshold
    int bodySearchNearPages = 2;      // predicted page +/- this
    int bodySearchFarPages = 5;       // extended search radius
};

const PrintedTocConfig& PtocConfig();

// ---------------------------------------------------------------------------
// P1 pure pipeline stages (implemented in PrintedTocReconstruct.cpp)
//
// Every stage is deterministic and side-effect free: the only inputs are the
// PtPageData produced by PtocLoadOcrPageJson (or live OCR) and PtocConfig.
// Stages run in the order below; PtProcessPage is the convenience driver.
// ---------------------------------------------------------------------------

// Parse a printed page label ("12", "xiv", "XII", "2-18", "A-12").
// Leading/trailing spaces are ignored. Returns false for anything else.
bool PtParsePageLabel(const char* s, PtPageLabel* out);

// Retry a failed label parse with common OCR confusions mapped (O/o -> 0,
// I/i/l -> 1). The run must become all digits and contain at least one real
// digit, so ordinary words never turn into page numbers.
bool PtParsePageLabelOcrFixed(const char* s, PtPageLabel* out);

// P1-B: group page.tokens into visual lines (fills page.lines; token
// ownership moves into the lines and page.tokens is emptied).
// Also detects the 1/2-column layout on the token boxes and fills
// PtRecLine::columnIndex (0-based reading order; tokens crossing the column
// cut - headings - are assigned column 0).
void PtReconstructLines(PtPageData& page);

// P1-A: structural TOC-ness score of a reconstructed page. Pure geometry
// (anchors, alignment, monotonicity, indent bands) plus a heading keyword
// bonus; never parses entry semantics. Requires PtReconstructLines to have
// run (uses page.lines and their columnIndex).
PtTocPageScores PtScoreTocPage(const PtPageData& page);

// P1-C: split each line into title + printed page anchor. Produces candidates
// in reading order (column asc, y asc). Lines without an anchor stay as
// continuation candidates; PtMergeWrappedEntries folds or drops them.
void PtParseEntryCandidates(const PtPageData& page, Vec<PtEntryCandidate>* out);

// P1-C: fold continuation candidates into their anchor-bearing predecessor
// when they are close below it, in the same column, and at least as deep.
// Absorbed candidates are erased; the survivor counts them in mergedWithPrev.
// Anchorless survivors after the merge (headings, far orphans) are dropped,
// unless keepTopOrphans: anchorless candidates appearing before the first
// anchored candidate of their column survive (they may be continuations of
// the previous TOC page and are folded by PtMergeCrossPage).
void PtMergeWrappedEntries(const PtPageData& page, Vec<PtEntryCandidate>* cands, bool keepTopOrphans = false);

// P2-A preview: cluster indentNormalized into bands per column and assign
// level = band rank (1-based). Small bands get lower hierarchyConf.
// Also fills charWidth (median title char width) on anchored candidates.
void PtAssignHierarchyLevels(const PtPageData& page, Vec<PtEntryCandidate>* cands);

// Convenience driver: reconstruct -> score -> candidates -> merge -> levels.
// Returns the page score; candsOut (optional) receives the entry candidates;
// overlayOut (optional) receives debug overlay data for this page. Both output
// args must be empty on entry (strings inside are owned by the vectors).
struct PtocOverlayPage;
PtTocPageScores PtProcessPage(PtPageData& page, Vec<PtEntryCandidate>* candsOut, PtocOverlayPage* overlayOut);

// ---------------------------------------------------------------------------
// P2: document-level stages. Input is the concatenation of per-page
// candidates (in ascending tocSourcePage reading order, as produced by
// PtProcessPage per page).
// ---------------------------------------------------------------------------

// P2-C: document-wide continuation merge. Drops TOC headings by text, folds
// anchorless orphans from the top of a TOC page (or the top of a second
// column) into the previous anchored entry, and drops unmatched orphans.
void PtMergeCrossPage(Vec<PtEntryCandidate>* cands);

// P2-A: re-cluster indentNormalized across all pages (per candidate epsilon
// from charWidth/columnWidth) and assign global band-rank levels.
void PtNormalizeLevelsAcrossPages(Vec<PtEntryCandidate>* cands);

// P2-A finalize: chapterHeading entries become level 1; all other entries get
// at least level 2 so they nest under the nearest preceding chapter.
void PtPromoteChapterHeadings(Vec<PtEntryCandidate>* cands);

// P2-B: build piecewise printed->PDF segments from footer/header anchors.
// Anchors are grouped by label type; consecutive anchors sharing the same
// offset form one segment. Segment confidence combines run size with the
// dominant-offset vote share (mappingVoteMinRatio / mappingMinSamples).
void PtBuildPageMapping(const Vec<PtPageAnchor>& anchors, Vec<PtPageMappingSegment>* segsOut);

// Maps a printed ordinal through the segments. Returns the 1-based PDF page
// or -1 when no segment covers (type, printed); *confOut (optional) gets the
// segment confidence.
int PtMapPrintedToPdf(const Vec<PtPageMappingSegment>& segs, PtPageLabelType type, int printed, float* confOut);

// Applies the mapping to all candidates with a convertible printed ordinal.
// resolvedPdfPage stays unset for uncovered or out-of-range pages.
void PtResolvePdfPages(const Vec<PtPageMappingSegment>& segs, int nPdfPages, Vec<PtEntryCandidate>* cands);

// Similarity of two title strings (0..1): normalized codepoint LCS after
// dropping whitespace/punctuation; ASCII case-insensitive. CJK-aware.
float PtTitleSimilarity(const char* a, const char* b);

// Arabic ordinals in TOC reading order must be non-decreasing; an ordinal
// that dips below its predecessor is a parse error (e.g. OCR clipping a
// trailing page-number digit). Flags such entries with low pageMappingConf
// so PtValidateEntriesWithBody will search the whole body and re-anchor.
void PtFlagOrdinalMonotonicityBreaks(Vec<PtEntryCandidate>* cands);

// P3: fuzzy-validate entry titles against body page text near the resolved
// page (bodySearchNearPages full weight, up to bodySearchFarPages decaying).
// Suspect mappings (pageMappingConf < 0.5) search the whole body instead.
// Fills bodyValidationConf; re-anchors to the best-matching page when the
// mapping confidence is low and the match is strong, restoring the printed
// ordinal from the re-anchored page via the entry's own mapping offset.
void PtValidateEntriesWithBody(const Vec<PtPageData*>& bodyPages, Vec<PtEntryCandidate>* cands);

// Document driver: reconstruct + score every page, keep pages scoring
// >= pageScoreMin as TOC pages, concatenate their candidates, run
// PtMergeCrossPage + PtNormalizeLevelsAcrossPages + mapping (anchors or a
// seed derived from the TOC extent) + optional body validation. Returns true
// when at least one entry was produced. overlayOut (optional) receives one
// heap PtocOverlayPage per processed page (isTocPage flags acceptance);
// ownership passes to the caller.
bool PtBuildDocumentToc(const PtDocTocInput& in, Vec<PtEntryCandidate>* out, Vec<PtocOverlayPage*>* overlayOut);

// ---------------------------------------------------------------------------
// Diagnostics (JSON dumps + debug overlay source data)
//
// Nothing here writes to disk unless the user explicitly sets
// SUMATRA_PTOC_DUMP=1 (debug builds / explicit diagnostic mode). Dumps land
// next to the PDF in a "<pdf-no-ext>.ptoc/" directory:
//
//   ocr-p<N>.json     raw OCR boxes for page N (image space)
//   toc-lines.json    ScanLine view after line reconstruction (page space)
//
// The parser tests later feed fixed ocr-*.json into the pure functions, which
// separates OCR regressions from parser regressions.
// ---------------------------------------------------------------------------

// Directory holding diagnostics for pdfPath ("...x.ptoc"). Temp allocation.
TempStr PtocDumpDirForFileTemp(const char* pdfPath);
bool PtocDumpRequested();  // env SUMATRA_PTOC_DUMP=1
bool PtocOverlayEnabled(); // env SUMATRA_PTOC_OVERLAY=1

// Write ocr-p<pageNo>.json. boxes are in image space (imgW/imgH pixels).
void PtocDumpOcrPageJson(const char* pdfPath, int pageNo, int imgW, int imgH, OcrProfile profile,
                         const Vec<OcrBox>& boxes);

// Write toc-lines.json (stage="collected" or "reconstructed").
void PtocDumpScanLinesJson(const char* pdfPath, const Vec<ScanLine>& lines, int nPages, const char* stage);

// Read back ocr-p<N>.json written by PtocDumpOcrPageJson. This is the parser
// test harness input: golden fixtures are fixed OCR JSONs, so parser
// regressions stay independent from OCR model updates. Returns false when the
// file is missing or malformed. Text strings are owned by pageOut.
bool PtocLoadOcrPageJson(const char* jsonPath, PtPageData* pageOut);

// Minimal UTF-8 JSON writer shared by the dumpers and tests.
struct PtJsonBuf {
    Vec<char> buf;

    void Reserve(int n) { buf.EnsureCap(n); }
    void Raw(const char* s) { buf.Append(s, str::Len(s)); }
    void Escaped(const char* s); // writes a quoted, escaped JSON string
    void Key(const char* k) {
        Escaped(k);
        Raw(":");
    }
    void Int(long long v);
    void Float(double v); // %.3f - coordinates never need more precision
    void Bool(bool v) { Raw(v ? "true" : "false"); }
    void Sep(bool& first) {
        if (!first) {
            Raw(",");
        }
        first = false;
    }
    char* Steal() {
        buf.Append((char)0);
        return buf.StealData();
    }
};

// ---------------------------------------------------------------------------
// Debug overlay data (P0: minimal). The pipeline fills one PtocOverlayPage per
// processed page; Canvas paints them over the rendered page. All geometry is
// in page space so painting needs no knowledge of how it was derived.
//
// green = title box, blue = page anchor box, dashed = column bounds,
// vertical marks = indent bands, header text = page score / level labels.
// ---------------------------------------------------------------------------

struct PtocOverlayEntry {
    PtBoxF titleBox;      // page space
    PtBoxF pageNumberBox; // page space; empty when the entry has no anchor
    int level = 1;
    int mergedWithPrev = 0; // continuation chain length absorbed from above
    float parseConf = 0;
};

struct PtocOverlayPage {
    int pageNo = 0;
    bool isTocPage = false;
    float pageScore = 0;
    Vec<PtBoxF> columns;    // detected column bounds, page space (1 = single)
    Vec<float> indentBands; // normalized (title.x0 - column.x0) / column width
    Vec<PtocOverlayEntry> entries;

    void Free() {
        columns.Reset();
        indentBands.Reset();
        entries.Reset();
    }
};

// Replaces the overlay data for pdfPath (steals the page pointers; on return
// pages contains nullptrs). An empty vec clears pdfPath's data. Called on the
// pipeline worker thread after a run.
void PtocOverlaySet(const char* pdfPath, Vec<PtocOverlayPage*>& pages);
// Forgets all overlay data (e.g. when the document is closed).
void PtocOverlayClear();
// Fresh heap copies of the overlay pages for pdfPath (empty when none).
// Ownership passes to the caller; safe to call from the UI thread; used by
// the canvas overlay painter.
void PtocOverlayCopyFor(const char* pdfPath, Vec<PtocOverlayPage*>& out);

// ---------------------------------------------------------------------------
// Live OCR capture (P3 integration)
//
// The OCR worker hands every fully recognized page's det boxes to the capture
// store, keyed by document path (same locking pattern as the overlay store).
// The TOC extraction fallback later drains the store and feeds PtPageData to
// PtBuildDocumentToc, so a scanned book's TOC is parsed from full OCR geometry
// instead of the flattened OCR text layer. Pages carry tokens only; consumers
// reconstruct lines via PtReconstructLines.
// ---------------------------------------------------------------------------

// Converts boxes (image space) into a captured PtPageData. Recapturing page 1
// resets the document's capture (a fresh OCR pass); duplicate pageNo replaces
// the previous page. No-op when pdfPath is empty or the page cap is reached.
void PtocCaptureOcrPage(const char* pdfPath, int pageNo, int imgW, int imgH, const Vec<OcrBox>& boxes);

// Deep-copies the captured pages of pdfPath (tokens; ascending pageNo) into
// fresh heap pages. Returns false when nothing was captured. Caller owns and
// frees the returned page pointers.
bool PtocCaptureCopyFor(const char* pdfPath, Vec<PtPageData*>* pagesOut);
// Copy one page for streaming page detection without copying the whole book.
bool PtocCaptureCopyPageFor(const char* pdfPath, int pageNo, PtPageData& out);

// Forgets the captured pages of one document (called when the document is
// unloaded; capture is keyed by path so other tabs are unaffected).
void PtocCaptureForget(const char* pdfPath);

// Forgets all captured pages (tests, full reset).
void PtocCaptureClear();
