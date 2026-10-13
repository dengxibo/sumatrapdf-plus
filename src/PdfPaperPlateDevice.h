/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
}

// Drop small near-white vector plates painted on top of an embedded image.
// Spreadsheet / doPDF exports often draw a white cell over a screenshot and
// hide a logo that is part of the raster. Dark mode already skips those
// plates; this applies the same rule when the page is not object-recolored.
fz_device* PdfPaperPlateWrapDevice(fz_context* ctx, fz_device* inner);
