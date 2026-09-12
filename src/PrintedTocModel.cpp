/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Printed TOC reconstruction: implementation of the shared diagnostics.
// See PrintedTocModel.h for the data model and coordinate conventions.

#include "PrintedTocModel.h"
#include "ExtractPdfToc.h"
#include "utils/FileUtil.h"
#include "utils/JsonParser.h"
#include "utils/ThreadUtil.h"

// ---------------------------------------------------------------------------
// Central configuration
// ---------------------------------------------------------------------------

const PrintedTocConfig& PtocConfig() {
    // Single tuning point for the whole pipeline. Values are documented in
    // PrintedTocConfig (meaning + normalized unit); calibrate against the
    // golden fixtures in testdata/printed-toc, never against one book.
    static PrintedTocConfig cfg;
    return cfg;
}

// ---------------------------------------------------------------------------
// JSON writer
// ---------------------------------------------------------------------------

void PtJsonBuf::Escaped(const char* s) {
    Raw("\"");
    if (s) {
        for (const char* p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
                case '"':
                    Raw("\\\"");
                    break;
                case '\\':
                    Raw("\\\\");
                    break;
                case '\n':
                    Raw("\\n");
                    break;
                case '\r':
                    Raw("\\r");
                    break;
                case '\t':
                    Raw("\\t");
                    break;
                case '\b':
                    Raw("\\b");
                    break;
                case '\f':
                    Raw("\\f");
                    break;
                default:
                    if (c < 0x20) {
                        Raw(str::FormatTemp("\\u%04x", c));
                    } else {
                        // UTF-8 bytes >= 0x20 (incl. all multi-byte CJK) are
                        // valid inside a JSON string as-is.
                        buf.Append((char)c);
                    }
                    break;
            }
        }
    }
    Raw("\"");
}

void PtJsonBuf::Int(long long v) {
    Raw(str::FormatTemp("%lld", v));
}

void PtJsonBuf::Float(double v) {
    // Page-space coordinates never need more than 1/1000 pt of precision and
    // a fixed number of decimals keeps golden diffs stable.
    Raw(str::FormatTemp("%.3f", v));
}

// ---------------------------------------------------------------------------
// Dump plumbing
// ---------------------------------------------------------------------------

static bool EnvFlagEnabled(const char* name) {
    char buf[8] = {0};
    DWORD n = GetEnvironmentVariableA(name, buf, dimof(buf));
    return n > 0 && buf[0] != 0 && buf[0] != '0';
}

bool PtocDumpRequested() {
    return EnvFlagEnabled("SUMATRA_PTOC_DUMP");
}

bool PtocOverlayEnabled() {
    return EnvFlagEnabled("SUMATRA_PTOC_OVERLAY");
}

// "<dir>/<file>.ptoc" next to the PDF. A sidecar directory (not %TEMP%)
// keeps one book's diagnostics together and makes "open the dump" trivial.
TempStr PtocDumpDirForFileTemp(const char* pdfPath) {
    if (!pdfPath || !pdfPath[0]) {
        return nullptr;
    }
    TempStr noExt = path::GetPathNoExtTemp(pdfPath);
    if (!noExt) {
        return nullptr;
    }
    return str::FormatTemp("%s.ptoc", noExt);
}

static bool PtocWriteFileUtf8(const char* path, const char* data) {
    if (!path || !data) {
        return false;
    }
    if (!dir::CreateForFile(path)) {
        return false;
    }
    return file::WriteFile(path, ByteSlice((const u8*)data, str::Len(data)));
}

void PtocDumpOcrPageJson(const char* pdfPath, int pageNo, int imgW, int imgH, OcrProfile profile,
                         const Vec<OcrBox>& boxes) {
    if (!PtocDumpRequested()) {
        return;
    }
    TempStr dir = PtocDumpDirForFileTemp(pdfPath);
    if (!dir) {
        return;
    }
    PtJsonBuf jb;
    jb.Reserve(4096);
    jb.Raw("{\"version\":1,\"page\":");
    jb.Int(pageNo);
    jb.Raw(",\"imgW\":");
    jb.Int(imgW);
    jb.Raw(",\"imgH\":");
    jb.Int(imgH);
    jb.Raw(",\"profile\":");
    jb.Int((long long)profile);
    // Raw det/rec boxes in image space: the ground truth for OCR-vs-parser
    // regression triage. charX pairs are kept so token splitting can be
    // replayed offline.
    jb.Raw(",\"boxes\":[");
    bool first = true;
    for (int i = 0; i < boxes.Size(); i++) {
        const OcrBox& b = boxes[i];
        jb.Sep(first);
        jb.Raw("{\"text\":");
        jb.Escaped(b.text ? b.text : "");
        jb.Raw(",\"x\":");
        jb.Int(b.rect.x);
        jb.Raw(",\"y\":");
        jb.Int(b.rect.y);
        jb.Raw(",\"dx\":");
        jb.Int(b.rect.dx);
        jb.Raw(",\"dy\":");
        jb.Int(b.rect.dy);
        jb.Raw(",\"vertical\":");
        jb.Bool(b.vertical);
        if (b.charX && b.nChar > 0) {
            jb.Raw(",\"charX\":[");
            bool fc = true;
            for (int k = 0; k < 2 * b.nChar; k++) {
                jb.Sep(fc);
                jb.Int(b.charX[k]);
            }
            jb.Raw("]");
        }
        jb.Raw("}");
    }
    jb.Raw("]}");
    TempStr path = path::JoinTemp(dir, str::FormatTemp("ocr-p%d.json", pageNo));
    PtocWriteFileUtf8(path, jb.Steal());
}

void PtocDumpScanLinesJson(const char* pdfPath, const Vec<ScanLine>& lines, int nPages, const char* stage) {
    if (!PtocDumpRequested()) {
        return;
    }
    TempStr dir = PtocDumpDirForFileTemp(pdfPath);
    if (!dir) {
        return;
    }
    PtJsonBuf jb;
    jb.Reserve(4096);
    jb.Raw("{\"version\":1,\"stage\":");
    jb.Escaped(stage ? stage : "");
    jb.Raw(",\"nPages\":");
    jb.Int(nPages);
    // Shared ScanLine view in page space: the exact input the TOC parser sees
    // after line collection/reconstruction.
    jb.Raw(",\"lines\":[");
    bool first = true;
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& l = lines[i];
        jb.Sep(first);
        jb.Raw("{\"text\":");
        jb.Escaped(l.text ? l.text : "");
        jb.Raw(",\"page\":");
        jb.Int(l.srcPage);
        jb.Raw(",\"x\":");
        jb.Float(l.x);
        jb.Raw(",\"y\":");
        jb.Float(l.y);
        jb.Raw(",\"dx\":");
        jb.Float(l.dx);
        jb.Raw(",\"dy\":");
        jb.Float(l.dy);
        jb.Raw(",\"fontSize\":");
        jb.Float(l.fontSize);
        jb.Raw(",\"bold\":");
        jb.Bool(l.bold);
        jb.Raw("}");
    }
    jb.Raw("]}");
    TempStr path = path::JoinTemp(dir, "toc-lines.json");
    PtocWriteFileUtf8(path, jb.Steal());
}

// ---------------------------------------------------------------------------
// JSON reader (parser test harness input)
// ---------------------------------------------------------------------------

struct PtLoadTokTemp {
    char* text = nullptr;
    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;

    void Free() {
        str::Free(text);
        text = nullptr;
    }
};

// Builds "/boxes[3]" style state from the push parser's visit order.
struct PtLoadOcrVisitor : public json::ValueVisitor {
    PtPageData* page = nullptr;
    Vec<PtLoadTokTemp> toks;

    ~PtLoadOcrVisitor() {
        for (int i = 0; i < toks.Size(); i++) {
            toks[i].Free();
        }
    }

    PtLoadTokTemp& TokAt(int idx) {
        while (toks.Size() <= idx) {
            PtLoadTokTemp t;
            toks.Append(t);
        }
        return toks[idx];
    }

    bool Visit(const char* path, const char* value, json::Type) override {
        if (str::Eq(path, "/page")) {
            page->pageIndex = ParseInt(value);
            return true;
        }
        if (str::Eq(path, "/imgW")) {
            // Fixture input has no separate page space; parsers normalize by
            // width/height, so image pixels stand in for page points here.
            page->width = (float)ParseInt(value);
            return true;
        }
        if (str::Eq(path, "/imgH")) {
            page->height = (float)ParseInt(value);
            return true;
        }
        // /boxes[i]/<field>
        if (!str::StartsWith(path, "/boxes[")) {
            return true;
        }
        const char* p = path + strlen("/boxes[");
        int idx = 0;
        bool any = false;
        while (*p >= '0' && *p <= '9') {
            idx = idx * 10 + (*p - '0');
            p++;
            any = true;
        }
        if (!any || *p != ']') {
            return true;
        }
        p++; // ']'
        if (*p != '/') {
            return true;
        }
        const char* field = p + 1;
        PtLoadTokTemp& t = TokAt(idx);
        if (str::Eq(field, "text")) {
            str::Free(t.text);
            t.text = str::Dup(value);
        } else if (str::Eq(field, "x")) {
            t.x = ParseInt(value);
        } else if (str::Eq(field, "y")) {
            t.y = ParseInt(value);
        } else if (str::Eq(field, "dx")) {
            t.dx = ParseInt(value);
        } else if (str::Eq(field, "dy")) {
            t.dy = ParseInt(value);
        }
        return true;
    }

    void Finish() {
        for (int i = 0; i < toks.Size(); i++) {
            PtLoadTokTemp& t = toks[i];
            PtToken tok;
            tok.text = t.text;
            t.text = nullptr;
            tok.box.x0 = (float)t.x;
            tok.box.y0 = (float)t.y;
            tok.box.x1 = (float)(t.x + t.dx);
            tok.box.y1 = (float)(t.y + t.dy);
            tok.confidence = 0; // OcrBox carries no rec confidence yet
            page->tokens.Append(tok);
        }
    }
};

bool PtocLoadOcrPageJson(const char* jsonPath, PtPageData* pageOut) {
    if (!jsonPath || !pageOut) {
        return false;
    }
    ByteSlice data = file::ReadFile(jsonPath);
    if (data.IsEmpty() || data.size() == 0) {
        return false;
    }
    // json::Parse requires a nullptr-terminated string
    char* text = str::Dup(data);
    data.Free();
    if (!text) {
        return false;
    }
    pageOut->tokens.Reset();
    pageOut->lines.Reset();
    PtLoadOcrVisitor visitor;
    visitor.page = pageOut;
    bool ok = json::Parse(text, &visitor);
    str::Free(text);
    if (ok) {
        visitor.Finish();
    }
    return ok && pageOut->width > 0 && pageOut->height > 0;
}

// ---------------------------------------------------------------------------
// Debug overlay store
//
// Written once per pipeline run (worker thread), copied out per page paint
// (UI thread). Pages are heap objects so replacing the store never memcpy's
// Vec members (Vec's copy is a plain memcpy and must never alias buffers).
// ---------------------------------------------------------------------------

struct PtocOverlayDoc {
    char* pdfPath = nullptr;     // owned
    Vec<PtocOverlayPage*> pages; // owned pointers

    void Free() {
        str::Free(pdfPath);
        pdfPath = nullptr;
        for (int i = 0; i < pages.Size(); i++) {
            delete pages[i];
        }
        pages.Reset();
    }
};

static Mutex& OverlayMutex() {
    static Mutex lock; // function-local static: thread-safe first init
    return lock;
}

// Pointer storage: Vec's copy is a plain memcpy, so a Vec of Vec-bearing
// structs would alias inner buffers. Docs and pages are heap objects and the
// store only ever copies pointers (POD-safe).
static Vec<PtocOverlayDoc*> gOverlayDocs;

void PtocOverlaySet(const char* pdfPath, Vec<PtocOverlayPage*>& pages) {
    if (pages.Size() == 0) {
        // an empty run clears this document's data
        OverlayMutex().Lock();
        for (int i = 0; i < gOverlayDocs.Size(); i++) {
            if (str::Eq(gOverlayDocs[i]->pdfPath, pdfPath)) {
                gOverlayDocs[i]->Free();
                delete gOverlayDocs[i];
                gOverlayDocs.RemoveAt(i);
                break;
            }
        }
        OverlayMutex().Unlock();
        return;
    }
    OverlayMutex().Lock();
    PtocOverlayDoc* doc = nullptr;
    for (int i = 0; i < gOverlayDocs.Size(); i++) {
        if (str::Eq(gOverlayDocs[i]->pdfPath, pdfPath)) {
            doc = gOverlayDocs[i];
            break;
        }
    }
    if (!doc) {
        doc = new PtocOverlayDoc();
        doc->pdfPath = str::Dup(pdfPath ? pdfPath : "");
        gOverlayDocs.Append(doc);
    }
    for (int i = 0; i < doc->pages.Size(); i++) {
        delete doc->pages[i];
    }
    doc->pages.Reset();
    for (int i = 0; i < pages.Size(); i++) {
        doc->pages.Append(pages[i]); // steal
        pages[i] = nullptr;
    }
    OverlayMutex().Unlock();
}

void PtocOverlayClear() {
    OverlayMutex().Lock();
    for (int i = 0; i < gOverlayDocs.Size(); i++) {
        gOverlayDocs[i]->Free();
        delete gOverlayDocs[i];
    }
    gOverlayDocs.Reset();
    OverlayMutex().Unlock();
}

static void CopyOverlayPage(const PtocOverlayPage& src, PtocOverlayPage& dst) {
    dst.pageNo = src.pageNo;
    dst.isTocPage = src.isTocPage;
    dst.pageScore = src.pageScore;
    // Element types (PtBoxF / float / PtocOverlayEntry) are POD, so Vec
    // element copies are safe; it is the Vec-in-struct wholesale copy that
    // must be avoided.
    dst.columns.Append(src.columns);
    dst.indentBands.Append(src.indentBands);
    dst.entries.Append(src.entries);
}

void PtocOverlayCopyFor(const char* pdfPath, Vec<PtocOverlayPage*>& out) {
    out.Reset();
    if (!pdfPath) {
        return;
    }
    OverlayMutex().Lock();
    for (int i = 0; i < gOverlayDocs.Size(); i++) {
        PtocOverlayDoc* doc = gOverlayDocs[i];
        if (!str::Eq(doc->pdfPath, pdfPath)) {
            continue;
        }
        for (int p = 0; p < doc->pages.Size(); p++) {
            PtocOverlayPage* dst = new PtocOverlayPage();
            CopyOverlayPage(*doc->pages[p], *dst);
            out.Append(dst);
        }
        break;
    }
    OverlayMutex().Unlock();
}

// ---------------------------------------------------------------------------
// Live OCR capture (P3 integration)
// ---------------------------------------------------------------------------

// Books longer than this still capture their front pages, which is where a
// printed TOC lives; the cap only bounds memory for pathological documents.
static const int kPtocCaptureMaxPages = 400;

struct PtocCapturePage {
    PtPageData page; // tokens only, image space
};

struct PtocCaptureDoc {
    char* pdfPath = nullptr;     // owned
    Vec<PtocCapturePage*> pages; // owned pointers

    void Free() {
        str::Free(pdfPath);
        pdfPath = nullptr;
        for (int i = 0; i < pages.Size(); i++) {
            delete pages[i];
        }
        pages.Reset();
    }
};

static Mutex& CaptureMutex() {
    static Mutex lock; // function-local static: thread-safe first init
    return lock;
}

static Vec<PtocCaptureDoc*> gCaptureDocs;

void PtocCaptureOcrPage(const char* pdfPath, int pageNo, int imgW, int imgH, const Vec<OcrBox>& boxes) {
    if (!pdfPath || !pdfPath[0] || pageNo <= 0 || imgW <= 0 || imgH <= 0) {
        return;
    }
    // convert outside the lock: the OCR worker owns this call
    PtocCapturePage* cp = new PtocCapturePage();
    cp->page.pageIndex = pageNo;
    cp->page.width = (float)imgW;
    cp->page.height = (float)imgH;
    for (int i = 0; i < boxes.Size(); i++) {
        const OcrBox& b = boxes[i];
        if (!b.text || !b.text[0]) {
            continue;
        }
        PtToken tok;
        tok.text = str::Dup(b.text);
        tok.box.x0 = (float)b.rect.x;
        tok.box.y0 = (float)b.rect.y;
        tok.box.x1 = (float)(b.rect.x + b.rect.dx);
        tok.box.y1 = (float)(b.rect.y + b.rect.dy);
        cp->page.tokens.Append(tok);
    }

    CaptureMutex().Lock();
    PtocCaptureDoc* doc = nullptr;
    for (int i = 0; i < gCaptureDocs.Size(); i++) {
        if (str::Eq(gCaptureDocs[i]->pdfPath, pdfPath)) {
            doc = gCaptureDocs[i];
            break;
        }
    }
    if (!doc) {
        doc = new PtocCaptureDoc();
        doc->pdfPath = str::Dup(pdfPath);
        gCaptureDocs.Append(doc);
    }
    // OCR may process pages out of order (user jumps around), so a first
    // sighting of page 1 must not wipe already-captured pages. Only a
    // re-recognition of page 1 signals a fresh OCR pass -> restart.
    bool hasPage1 = false;
    for (int i = 0; i < doc->pages.Size(); i++) {
        if (doc->pages[i]->page.pageIndex == 1) {
            hasPage1 = true;
            break;
        }
    }
    if (pageNo == 1 && hasPage1) {
        for (int i = 0; i < doc->pages.Size(); i++) {
            delete doc->pages[i];
        }
        doc->pages.Reset();
    }
    for (int i = 0; i < doc->pages.Size(); i++) {
        if (doc->pages[i]->page.pageIndex == pageNo) {
            delete doc->pages[i]; // re-recognized page replaces the old one
            doc->pages.RemoveAt(i);
            break;
        }
    }
    if (doc->pages.Size() < kPtocCaptureMaxPages) {
        doc->pages.Append(cp);
    } else {
        delete cp;
    }
    CaptureMutex().Unlock();
}

bool PtocCaptureCopyFor(const char* pdfPath, Vec<PtPageData*>* pagesOut) {
    pagesOut->Reset();
    if (!pdfPath || !pdfPath[0]) {
        return false;
    }
    CaptureMutex().Lock();
    for (int i = 0; i < gCaptureDocs.Size(); i++) {
        PtocCaptureDoc* doc = gCaptureDocs[i];
        if (!str::Eq(doc->pdfPath, pdfPath)) {
            continue;
        }
        // capture order follows the user's viewing order; emit the pages in
        // ascending PDF page order regardless (selection over indices; the
        // pages are heap objects so the array only ever moves pointers)
        int nSrc = doc->pages.Size();
        Vec<bool> used;
        for (int s = 0; s < nSrc; s++) {
            used.Append(false);
        }
        for (int p = 0; p < nSrc; p++) {
            int best = -1;
            for (int q = 0; q < nSrc; q++) {
                if (!used[q] && (best < 0 || doc->pages[q]->page.pageIndex < doc->pages[best]->page.pageIndex)) {
                    best = q;
                }
            }
            if (best < 0) {
                break;
            }
            used[best] = true;
            const PtPageData& src = doc->pages[best]->page;
            PtPageData* dst = new PtPageData();
            dst->pageIndex = src.pageIndex;
            dst->width = src.width;
            dst->height = src.height;
            for (int t = 0; t < src.tokens.Size(); t++) {
                PtToken tok;
                tok.text = str::Dup(src.tokens[t].text ? src.tokens[t].text : "");
                tok.box = src.tokens[t].box;
                tok.confidence = src.tokens[t].confidence;
                dst->tokens.Append(tok);
            }
            pagesOut->Append(dst);
        }
        break;
    }
    CaptureMutex().Unlock();
    return pagesOut->Size() > 0;
}

bool PtocCaptureCopyPageFor(const char* pdfPath, int pageNo, PtPageData& out) {
    if (!pdfPath) return false;
    bool found = false;
    CaptureMutex().Lock();
    for (auto* doc : gCaptureDocs) {
        if (!str::Eq(doc->pdfPath, pdfPath)) continue;
        for (auto* captured : doc->pages) {
            const auto& src = captured->page;
            if (src.pageIndex != pageNo) continue;
            out.Free();
            out.pageIndex = src.pageIndex;
            out.width = src.width;
            out.height = src.height;
            for (const auto& token : src.tokens) {
                PtToken copy = token;
                copy.text = str::Dup(token.text);
                out.tokens.Append(copy);
            }
            found = true;
            break;
        }
        break;
    }
    CaptureMutex().Unlock();
    return found;
}

void PtocCaptureForget(const char* pdfPath) {
    if (!pdfPath || !pdfPath[0]) {
        return;
    }
    CaptureMutex().Lock();
    for (int i = 0; i < gCaptureDocs.Size(); i++) {
        if (str::Eq(gCaptureDocs[i]->pdfPath, pdfPath)) {
            gCaptureDocs[i]->Free();
            delete gCaptureDocs[i];
            gCaptureDocs.RemoveAt(i);
            break;
        }
    }
    CaptureMutex().Unlock();
}

void PtocCaptureClear() {
    CaptureMutex().Lock();
    for (int i = 0; i < gCaptureDocs.Size(); i++) {
        gCaptureDocs[i]->Free();
        delete gCaptureDocs[i];
    }
    gCaptureDocs.Reset();
    CaptureMutex().Unlock();
}
