/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct fz_context;
struct fz_device;

fz_device* PdfCadEnhanceWrapDevice(fz_context* ctx, fz_device* inner);
