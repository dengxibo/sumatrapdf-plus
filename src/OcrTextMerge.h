/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Paragraph-merged copy for OCR text: turns layout lines (text + page-space
// bbox) into paragraph lines by joining soft-wrapped lines. Pure functions so
// they can be unit tested without an engine.

#include "utils/BaseUtil.h"

struct OcrMergeLine {
    const char* text; // UTF-8, not owned
    Rect bbox;        // page-space union of the line's glyph rects
};

struct OcrMergeGlyph {
    WCHAR ch;
    RectF bbox; // page-space glyph rect
};

// s ends with sentence-final punctuation (。！？；…等) after trailing spaces
bool OcrTextEndsWithTerminalPunct(const char* s);

// Appends paragraph lines to out (each a merged run of consecutive visual
// lines). vertical=true expects columns in reading order (right to left).
void OcrMergeLayoutLines(StrVec& out, const Vec<OcrMergeLine>& lines, bool vertical);

// Sorts glyphs of a vertical page into visual order (columns right to left,
// glyphs top to bottom), bands them into columns by x-overlap, and merges the
// resulting column "lines" into paragraphs.
void OcrMergeVerticalGlyphs(StrVec& out, const Vec<OcrMergeGlyph>& glyphs);
