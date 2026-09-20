/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Body-structure TOC scan ("从正文生成目录"): walks the WHOLE document once,
// reads the native text layer first and OCRs only pages without usable text,
// then extracts a high-recall set of heading candidates. The candidates are
// the local "source of fact" (original text, PDF page, bbox); the web AI only
// decides whether a candidate is a heading and which level it has.

#pragma once

#include "ExtractPdfToc.h" // ScanLine, ExtractedTocItem

class EngineBase;

enum class BodyNumberingKind {
    None = 0,
    Chapter = 1, // 第一章 / 第一编 / 第一部分
    Section = 2, // 第一节
    Dunhao = 3,  // 一、
    Paren = 4,   // （一） / （1）
    Dotted = 5,  // 1.1 / 1.1.1
    Arabic = 6,  // 1. / 1、
};

struct HeadingCandidate {
    int id = 0; // 1-based; serialized as "C<id>"
    int pdfPage = 0;
    float x = 0;
    float y = 0;
    float dx = 0;
    float dy = 0;
    float fontSize = 0;
    bool bold = false;
    bool fromOcr = false;
    BodyNumberingKind numKind = BodyNumberingKind::None;
    int numDepth = 0; // 1-based local prior; the AI may override it
    int localScore = 0;
    char* text = nullptr; // trimmed original text, UTF-8

    ~HeadingCandidate() { str::Free(text); }
};

struct TocStructureScanResult {
    EngineBase* engine = nullptr; // not owned; must outlive this result
    int totalPages = 0;
    int nativePages = 0;
    int ocrPages = 0;
    int rawLineCount = 0;
    int repeatedHeaderCandidates = 0;
    Vec<HeadingCandidate*> candidates; // page order; id == index + 1

    ~TocStructureScanResult() { Reset(); }
    void Reset();
    HeadingCandidate* FindById(int id) const; // 1-based; nullptr when out of range
};

// Runs the whole-document scan on a worker thread. Reports progress per page;
// isCanceled is polled between pages. Returns false when canceled (out is
// left empty) or when engine is null.
bool RunTocStructureScan(EngineBase* engine, TocStructureScanResult& out, bool (*isCanceled)(void*) = nullptr,
                         void* cancelCtx = nullptr, void (*onProgress)(void*, int, int) = nullptr,
                         void* progressCtx = nullptr);

// One AI selection: candidate id + level. Facts never come from the AI.
struct BodyTocSelection {
    int candidateId = 0; // 1-based
    int level = 1;
};

// Cheap pre-check used to decide which clipboard payload is a body-TOC reply
// ({"toc":[{"candidate_id":...}]}) vs. the printed-TOC reply ({"items":[...]}).
bool IsBodyTocJsonCandidate(const char* text);

// Parses + validates the v2 body JSON: strips fences, schema-checks and
// whitelists every candidate_id against the scan result. Unknown ids are
// rejected individually; returns false when no legal selection remains.
bool ParseBodyTocSelections(const char* text, const TocStructureScanResult& scan, Vec<BodyTocSelection>& out);

// Builds the bookmark tree from validated selections. Title/page/bbox all come
// from the local candidate; PrepareTocExtractionResult() must run afterwards.
bool BuildBodyTocItems(TocStructureScanResult& scan, const Vec<BodyTocSelection>& sel, Vec<ExtractedTocItem*>& roots);

// Assembles the <CANDIDATE> digest block embedded in the body prompt.
char* BuildBodyTocDigest(const TocStructureScanResult& scan);
