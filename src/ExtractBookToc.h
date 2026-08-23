/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Book printed-TOC engine: restore the author's printed 目录.
// Do not invent numbering or rewrite titles. Body heading inference
// (style clustering) is a fallback only when no printed TOC exists.

class EngineBase;
struct ScanLine;
struct ExtractedTocItem;

bool ExtractBookPrintedToc(EngineBase* engine, const Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                           Vec<ExtractedTocItem*>& roots, const char* debugPath = nullptr);
bool ExtractBookBodyHeadings(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots);
int StripBookPrintedPageFromTitle(char* title);
