/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct fz_context;
struct fz_device;

struct CadMinLineWidthScope {
    CadMinLineWidthScope(fz_context* ctx, float zoom, bool active);
    ~CadMinLineWidthScope();

    CadMinLineWidthScope(const CadMinLineWidthScope&) = delete;
    CadMinLineWidthScope& operator=(const CadMinLineWidthScope&) = delete;

  private:
    fz_context* ctx = nullptr;
    float saved = 0;
    bool active = false;
};

fz_device* PdfCadEnhanceWrapDevice(fz_context* ctx, fz_device* inner);
