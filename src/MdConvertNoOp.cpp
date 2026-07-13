/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include "MdConvert.h"

// Stub implementations for binaries that compile EngineMupdf.cpp but not
// MdConvert.cpp / md4c (PdfFilter, PdfPreview, etc.).

ByteSlice MdFileToHTML(const char* path) {
    (void)path;
    return {};
}

extern "C" void fz_htdoc_reparse_html(fz_context* ctx, fz_document* doc, fz_buffer* buf, float w, float h, float em) {
    (void)ctx;
    (void)doc;
    (void)buf;
    (void)w;
    (void)h;
    (void)em;
}
