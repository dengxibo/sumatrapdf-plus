/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// ---------- Issue #76 forensic harness (no UI, debug builds) ----------
// Simulates mouse drag selection by driving TextSelection through its real
// entry points (StartAt / SelectUpTo) with glyph coordinates taken from the
// engine's text page, then reports anchor, endpoint, normalized range, first
// highlight rect and extracted text so the anchor invariant can be verified.
// Invoked with: SumatraPDF-Plus.exe -seltest <file.pdf>
// Results are written to c:\src\sumatrapdf\seltest_out.txt

static int SelTestFindLine(const Vec<Vec<int>>& lines, int glyph) {
    for (size_t li = 0; li < lines.Size(); li++) {
        for (size_t k = 0; k < lines[li].Size(); k++) {
            if (lines[li][k] == glyph) {
                return (int)li;
            }
        }
    }
    return -1;
}

static void SelTestRunCase(FILE* f, EngineBase* engine, int pageNo, const Vec<Vec<int>>& lines, const char* name,
                           int aGlyph, int bGlyph) {
    int textLen = 0;
    Rect* coords = nullptr;
    engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || aGlyph < 0 || aGlyph >= textLen || bGlyph < 0 || bGlyph >= textLen) {
        fprintf(f, "[%s] SKIP (bad glyph)\n", name);
        return;
    }
    TextSelection ts(engine);
    PointF pa((coords[aGlyph].x + coords[aGlyph].dx * 0.5f), (coords[aGlyph].y + coords[aGlyph].dy * 0.5f));
    ts.StartAt(pageNo, pa.x, pa.y);
    int anchorAfterStart = ts.startGlyph;

    PointF pb((coords[bGlyph].x + coords[bGlyph].dx * 0.5f), (coords[bGlyph].y + coords[bGlyph].dy * 0.5f));
    ts.SelectUpTo(pageNo, pb.x, pb.y);

    int fp = -1, fg = -1, tp = -1, tg = -1;
    ts.GetGlyphRange(&fp, &fg, &tp, &tg);
    fprintf(f, "[%s] anchor=%d (clicked %d) end=%d (target %d) range=p%d:%d..p%d:%d rects=%d", name, anchorAfterStart,
            aGlyph, ts.endGlyph, bGlyph, fp, fg, tp, tg, ts.result.len);
    if (ts.result.len > 0) {
        Rect r0 = ts.result.rects[0];
        fprintf(f, " rect0.x=%d (fromGlyph.x=%d)", r0.x, coords[fg].x);
    }

    // regression assertions for issue #76:
    // 1. anchor invariant: StartAt must decide the anchor, SelectUpTo must not move it
    // 2. the first highlight rect starts at the first selected glyph (never the
    //    visual line start, which would mean merged line bboxes)
    // 3. a cross-line selection produces at least one rect per line
    int aLine = SelTestFindLine(lines, aGlyph);
    int bLine = SelTestFindLine(lines, bGlyph);
    bool crossLine = aLine >= 0 && bLine >= 0 && aLine != bLine;
    bool ok = anchorAfterStart == aGlyph;
    if (ts.result.len > 0) {
        ok = ok && ts.result.rects[0].x == coords[fg].x;
    }
    if (crossLine) {
        ok = ok && ts.result.len >= 2;
    }
    fprintf(f, " => %s\n", ok ? "PASS" : "FAIL");
    WCHAR* ws = ts.ExtractText("\r\n");
    fprintf(f, "\n    text='");
    for (int i = 0; i < 60 && ws && ws[i]; i++) {
        fputc(ws[i] < 128 ? (char)ws[i] : '#', f);
    }
    fprintf(f, "'\n");
    str::Free(ws);
}

static int RunSelTextTest(const char* pdfPath) {
    const char* outPath = "c:\\src\\sumatrapdf\\seltest_out.txt";
    FILE* f = fopen(outPath, "w");
    if (!f) {
        return 2;
    }
    fprintf(f, "seltest: %s\n", pdfPath);
    EngineBase* engine = CreateEngineMupdfFromFile(pdfPath, kindEngineMupdf, 96, nullptr);
    if (!engine) {
        fprintf(f, "failed to open engine\n");
        fclose(f);
        return 3;
    }

    // find a body-text page (several long lines) to test on; fall back to the
    // page with the most glyph lines
    int pageNo = 1;
    int textLen = 0;
    Rect* coords = nullptr;
    const WCHAR* text = nullptr;
    int pageCount = engine->PageCount();
    int bestPage = -1;
    int bestLines = 0;
    for (int p = 1; p <= std::min(pageCount, 20); p++) {
        int len = 0;
        Rect* co = nullptr;
        const WCHAR* tx = engine->GetTextForPage(p, &len, &co);
        (void)tx;
        if (!co || len <= 0) {
            continue;
        }
        // count lines and longest line
        int nLines = 0;
        int longest = 0;
        int curLen = 0;
        float lastY = -1e9f;
        float lastH = 0;
        for (int i = 0; i < len; i++) {
            if (!co[i].dx && !co[i].dy) {
                if (curLen > longest) {
                    longest = curLen;
                }
                curLen = 0;
                continue;
            }
            float cy = co[i].y + co[i].dy * 0.5f;
            if (lastY < -1e8f || fabsf(cy - lastY) > std::max((float)co[i].dy, lastH) * 0.6f) {
                nLines++;
                if (curLen > longest) {
                    longest = curLen;
                }
                curLen = 0;
            }
            curLen++;
            lastY = cy;
            lastH = co[i].dy;
        }
        if (curLen > longest) {
            longest = curLen;
        }
        fprintf(f, "page %d: len=%d lines~%d longest~%d\n", p, len, nLines, longest);
        if (nLines > bestLines) {
            bestLines = nLines;
            bestPage = p;
        }
        if (nLines >= 8 && longest >= 45) {
            pageNo = p;
            textLen = len;
            coords = co;
            text = tx;
            break;
        }
    }
    if (!coords && bestPage > 0) {
        pageNo = bestPage;
        text = engine->GetTextForPage(pageNo, &textLen, &coords);
    }
    if (!coords || textLen <= 0) {
        fprintf(f, "no text found in first 20 pages\n");
        fclose(f);
        engine->Release();
        return 4;
    }
    fprintf(f, "testing on page %d\n", pageNo);
    Vec<Vec<int>> lines;
    for (int i = 0; i < textLen; i++) {
        if (!coords[i].dx && !coords[i].dy) {
            continue;
        }
        float cy = coords[i].y + coords[i].dy * 0.5f;
        int found = -1;
        for (size_t li = 0; li < lines.Size(); li++) {
            int rep = lines[li][0];
            float ry = coords[rep].y + coords[rep].dy * 0.5f;
            float tol = std::max(coords[rep].dy, coords[i].dy) * 0.6f;
            if (fabsf(cy - ry) < tol) {
                found = (int)li;
                break;
            }
        }
        if (found < 0) {
            Vec<int> v;
            v.Append(i);
            lines.Append(v);
        } else {
            lines[found].Append(i);
        }
    }
    fprintf(f, "page %d: textLen=%d lines=%d\n", pageNo, textLen, (int)lines.Size());
    for (size_t li = 0; li < lines.Size() && li < 5; li++) {
        Vec<int>& L = lines[li];
        fprintf(f, "  line %d: glyphs %d..%d (n=%d) '", (int)li, L[0], L[L.Size() - 1], (int)L.Size());
        for (size_t k = 0; k < L.Size() && k < 40; k++) {
            WCHAR c = text[L[k]];
            fputc(c >= 32 && c < 127 ? (char)c : '#', f);
        }
        fprintf(f, "'\n");
    }
    // dump glyph geometry around the first lines (to check for zero-width
    // line-break placeholders between lines)
    int dumpEnd = std::min(textLen, 40);
    for (int i = 0; i < dumpEnd; i++) {
        Rect& r = coords[i];
        WCHAR c = text[i];
        fprintf(f, "  g%-3d x=%-5d y=%-5d dx=%-5d dy=%-5d c=%d\n", i, r.x, r.y, r.dx, r.dy,
                (c >= 32 && c < 127) ? c : (c == '\n' ? 'n' : '?'));
    }
    if (lines.Size() < 2) {
        fprintf(f, "need >= 2 lines\n");
        fclose(f);
        engine->Release();
        return 5;
    }

    Vec<int>& L0 = lines[0];
    Vec<int>& L1 = lines[1];
    Vec<int>& L2 = lines.Size() > 2 ? lines[2] : lines[1];
    int mid0 = L0[L0.Size() / 2];
    int mid1 = L1[L1.Size() / 2];
    int mid2 = L2[L2.Size() / 2];
    int end0 = L0[L0.Size() - 1];
    int start0 = L0[0];

    SelTestRunCase(f, engine, pageNo, lines, "A.same-line-fwd   ", mid0, end0);
    SelTestRunCase(f, engine, pageNo, lines, "B.same-line-bwd   ", mid0, start0);
    SelTestRunCase(f, engine, pageNo, lines, "C.cross-line-DOWN ", mid0, mid1);
    SelTestRunCase(f, engine, pageNo, lines, "D.cross-line-UP   ", mid1, mid0);
    SelTestRunCase(f, engine, pageNo, lines, "E.cross-3-lines   ", mid0, mid2);
    SelTestRunCase(f, engine, pageNo, lines, "F.anchor-at-start ", start0, mid1);

    fprintf(f, "DONE\n");
    fclose(f);
    engine->Release();
    return 0;
}
