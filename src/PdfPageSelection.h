/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#pragma once

// before is a 1-based page boundary; count + 1 means after the last page.
// The output keeps selected pages in document order, regardless of click order.
inline int PdfMoveSelectedPages(int count, const int* selected, int selectedCount, int before, int* output) {
    auto contains = [&](int page) {
        for (int i = 0; i < selectedCount; i++)
            if (selected[i] == page) return true;
        return false;
    };
    if (before < 1) before = 1;
    if (before > count + 1) before = count + 1;
    int written = 0, firstMoved = 0;
    for (int page = 1; page <= count + 1; page++) {
        if (page == before) {
            firstMoved = written + 1;
            for (int p = 1; p <= count; p++)
                if (contains(p)) output[written++] = p;
        }
        if (page <= count && !contains(page)) output[written++] = page;
    }
    return firstMoved;
}
