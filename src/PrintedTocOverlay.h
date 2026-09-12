/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Printed TOC debug overlay: paints PtocOverlayPage data (PrintedTocModel.h)
// over the rendered page. Enabled with SUMATRA_PTOC_OVERLAY=1.
//
//   green  = entry title box          blue   = page-number anchor box
//   gray   = column bounds (dashed)   ticks  = indent-band x positions
//
// P0 scope: geometry only. Levels/labels appear as small text near boxes.

#pragma once

struct DisplayModel;

// No-op unless overlay data exists for the document (and the env flag is set).
void PaintPrintedTocOverlay(HDC hdc, DisplayModel* dm, int pageNo);
