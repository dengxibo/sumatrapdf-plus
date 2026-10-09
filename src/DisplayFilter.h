/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct MainWindow;
struct WindowTab;
struct DocController;
class EngineBase;

#include "DocumentEnhancer.h"

struct DisplayFilterParams {
    DocumentEnhancementMode mode = DocumentEnhancementMode::Off;
    int brightness = 0; // kept for Legacy / FileState; Auto uses 0
    int contrast = 0;
    int sharpness = 0;
    // EPUB and other native-text books already paint theme colors into the page.
    // PDF/XPS stay false: enhancement reads publisher pixels, then theme is reapplied.
    bool themedBitmap = false;

    // Active when enhancement is on (Auto, or forced Mild/Strong/Legacy).
    bool IsActive() const { return mode != DocumentEnhancementMode::Off; }
};

DisplayFilterParams GetDisplayFilterForTab(WindowTab* tab);
DisplayFilterParams GetDisplayFilterForController(DocController* ctrl);
void SetDisplayFilterForTab(WindowTab* tab, const DisplayFilterParams& p, bool saveAndRepaint);
// fs null, or use-default: wand off. Otherwise the mode saved for that file.
struct FileState;
void RestoreDisplayFilterForTab(WindowTab* tab, FileState* fs);
// PDF, XPS, and native-text books (EPUB, MOBI, FB2, …). Pictures and comics stay off.
bool DisplayFilterSupportedForEngine(EngineBase* engine);
bool DisplayFilterSupportedForTab(WindowTab* tab);

// BitBlt/StretchBlt from hdcSrc → hdcDst, applying filter when p is active.
bool BlitWithDisplayFilter(HDC hdcDst, int xDst, int yDst, int dxDst, int dyDst, HDC hdcSrc, int xSrc, int ySrc,
                           int dxSrc, int dySrc, const DisplayFilterParams& p);

// Build an enhanced copy of a cached tile (caller owns). nullptr on failure / Off.
struct RenderedBitmap;
RenderedBitmap* CreateDisplayFilteredBitmap(RenderedBitmap* src, const DisplayFilterParams& p);

void InvalidateDisplayFilterViews();

// Apply a named preset: "auto", "reading", "scan", "legacy", "reset", "off".
void ApplyDisplayFilterPreset(MainWindow* win, const char* preset);

void UpdateDisplayFilterToolbarButton(MainWindow* win);

// Toolbar one-shot: Off ↔ Auto (Mild/Strong chosen per tile).
void ToggleDisplayFilterActive(MainWindow* win);

// Panel removed — stubs keep MainWindow / theme hooks compiling.
void HideDisplayFilterPanel(MainWindow* win);
void DeleteDisplayFilterPanel(MainWindow* win);
bool IsDisplayFilterPanelVisible(MainWindow* win);
void RefreshDisplayFilterPanelsTheme();
