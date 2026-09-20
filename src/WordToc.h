// Copyright 2026 Authors of SumatraPDF
// License: GPLv3
// Editable outline for DOC/DOCX. Stored inside the OOXML package as Word
// custom document properties (chunked; each value stays within Word's
// 255-character custom-property limit) so a later reopen, including a
// .doc round-trip through Word, still sees the edited table of contents.

#pragma once

#include "PdfTocEditModel.h"

struct WordTocModel {
    // True once this model is the sidebar outline (loaded from the file or edited).
    bool active = false;
    Vec<PdfTocEditNode*> roots;
};

void WordTocModelFree(WordTocModel* m);

// True when docxPath contains a Sumatra outline (including an intentionally empty one).
// On success, m->roots is replaced. Does not set m->active.
bool WordTocLoadStored(const char* docxPath, WordTocModel* m);

char* WordTocFormatUri(int pageNo, float x, float y);
bool WordTocParseUri(const char* uri, int* pageNo, float* x, float* y);

// Rewrite srcDocx to dstDocx with the outline embedded. src and dst must differ
// (the open package may still be locked).
bool WordTocWriteDocx(const char* srcDocx, const char* dstDocx, WordTocModel* m, char** errOut);

// Embed the outline in a temp docx, then ask Word to SaveAs classic .doc.
// Fails if Word is not installed.
bool WordTocWriteClassicDoc(const char* srcDocx, const char* dstDoc, WordTocModel* m, char** errOut);
