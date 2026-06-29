/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct fz_context;
struct fz_device;

struct CadEnhanceRenderOpts {
    float zoom = 1.f;
    bool hairlineVector = false;
};

struct CadMinLineWidthScope {
    CadMinLineWidthScope(fz_context* ctx, float zoom, bool active, bool hairlineDoc = false);
    ~CadMinLineWidthScope();

    CadMinLineWidthScope(const CadMinLineWidthScope&) = delete;
    CadMinLineWidthScope& operator=(const CadMinLineWidthScope&) = delete;

  private:
    fz_context* ctx = nullptr;
    float saved = 0;
    bool active = false;
};

fz_device* PdfCadEnhanceWrapDevice(fz_context* ctx, fz_device* inner, const CadEnhanceRenderOpts& opts);
// Post-process a rendered page bitmap for raster/screenshot CAD PDFs.
void PdfCadEnhancePixmap(fz_context* ctx, fz_pixmap* pix, float zoom, bool rasterDominant);
