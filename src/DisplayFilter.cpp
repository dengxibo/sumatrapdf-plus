/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "AppSettings.h"
#include "GlobalPrefs.h"
#include "FileHistory.h"
#include "DocController.h"
#include "EngineBase.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "SumatraPDF.h"
#include "SumatraConfig.h"
#include "Toolbar.h"
#include "DisplayFilter.h"
#include "DocumentEnhancer.h"
#include "DisplayModel.h"
#include "RenderCache.h"

static BYTE gFilterLut[256];
static int gLutBrightness = INT_MIN;
static int gLutContrast = INT_MIN;

static int ClampFilterParam(int v) {
    if (v < -100) {
        return -100;
    }
    if (v > 100) {
        return 100;
    }
    return v;
}

static int ClampSharpness(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > 100) {
        return 100;
    }
    return v;
}

static void RebuildDisplayFilterLut(int brightness, int contrast) {
    brightness = ClampFilterParam(brightness);
    contrast = ClampFilterParam(contrast);
    if (brightness == gLutBrightness && contrast == gLutContrast) {
        return;
    }
    gLutBrightness = brightness;
    gLutContrast = contrast;
    float scale = (100.0f + (float)contrast) / 100.0f;
    float offset = (float)brightness * 255.0f / 100.0f;
    for (int i = 0; i < 256; i++) {
        float v = ((float)i - 128.0f) * scale + 128.0f + offset;
        if (v < 0.f) {
            v = 0.f;
        } else if (v > 255.f) {
            v = 255.f;
        }
        gFilterLut[i] = (BYTE)(v + 0.5f);
    }
}

static DisplayFilterParams ClampParams(DisplayFilterParams p) {
    if ((int)p.mode < 0 || (int)p.mode > (int)DocumentEnhancementMode::Auto) {
        p.mode = DocumentEnhancementMode::Off;
    }
    p.brightness = ClampFilterParam(p.brightness);
    p.contrast = ClampFilterParam(p.contrast);
    p.sharpness = ClampSharpness(p.sharpness);
    return p;
}

DisplayFilterParams GetDisplayFilterForTab(WindowTab* tab) {
    DisplayFilterParams p;
    if (!tab) {
        return p;
    }
    // Non-PDF: never apply enhancement, even if FileState still has it on.
    if (!DisplayFilterSupportedForTab(tab)) {
        return p;
    }
    p.mode = (DocumentEnhancementMode)tab->displayFilterMode;
    p.brightness = tab->displayFilterBrightness;
    p.contrast = tab->displayFilterContrast;
    p.sharpness = tab->displayFilterSharpness;
    DocumentEnhancementMode envMode;
    if (TryParseDocumentEnhancerEnvOverride(&envMode)) {
        p.mode = envMode;
    }
    return ClampParams(p);
}

bool DisplayFilterSupportedForEngine(EngineBase* engine) {
    if (!engine || engine->IsImageCollection()) {
        return false;
    }
    return engine->kind == kindEngineMupdf && str::EqI(engine->defaultExt, ".pdf");
}

bool DisplayFilterSupportedForTab(WindowTab* tab) {
    return tab && DisplayFilterSupportedForEngine(tab->GetEngine());
}

DisplayFilterParams GetDisplayFilterForController(DocController* ctrl) {
    if (!ctrl) {
        return {};
    }
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            if (tab->ctrl == ctrl) {
                return GetDisplayFilterForTab(tab);
            }
        }
    }
    return {};
}

static void SaveDisplayFilterToFileState(WindowTab* tab) {
    if (!tab || !tab->filePath || !gGlobalPrefs || !gGlobalPrefs->rememberStatePerDocument) {
        return;
    }
    FileState* fs = gFileHistory.FindByName(tab->filePath, nullptr);
    if (!fs) {
        return;
    }
    fs->displayFilterMode = tab->displayFilterMode;
    fs->displayFilterBrightness = tab->displayFilterBrightness;
    fs->displayFilterContrast = tab->displayFilterContrast;
    fs->displayFilterSharpness = tab->displayFilterSharpness;
}

void SetDisplayFilterForTab(WindowTab* tab, const DisplayFilterParams& pIn, bool saveAndRepaint) {
    if (!tab) {
        return;
    }
    DisplayFilterParams p = ClampParams(pIn);
    tab->displayFilterMode = (int)p.mode;
    tab->displayFilterBrightness = p.brightness;
    tab->displayFilterContrast = p.contrast;
    tab->displayFilterSharpness = p.sharpness;
    if (p.IsActive()) {
        tab->displayFilterLastMode = (int)p.mode;
    }
    if (saveAndRepaint) {
        SaveDisplayFilterToFileState(tab);
        if (tab->win) {
            InvalidateRect(tab->win->hwndCanvas, nullptr, FALSE);
            UpdateDisplayFilterToolbarButton(tab->win);
        }
        SaveSettings();
    }
}

static void ApplyLutToDibBits(BYTE* bits, int width, int height, int stride) {
    if (gLutBrightness == 0 && gLutContrast == 0) {
        return;
    }
    for (int y = 0; y < height; y++) {
        BYTE* row = bits + y * stride;
        for (int x = 0; x < width; x++) {
            BYTE* px = row + x * 4;
            px[0] = gFilterLut[px[0]];
            px[1] = gFilterLut[px[1]];
            px[2] = gFilterLut[px[2]];
        }
    }
}

static void ApplySharpenToDibBits(BYTE* bits, int width, int height, int stride, int sharpness) {
    if (sharpness <= 0 || width < 3 || height < 3) {
        return;
    }
    // 0..100 → unsharp strength. Former *0.65 was too weak on soft scans at screen res.
    float t = (float)ClampSharpness(sharpness) / 100.0f;
    float amount = t * (1.2f + t); // ~0.22 at 20, ~1.1 at 50, ~2.2 at 100
    size_t nbytes = (size_t)stride * (size_t)height;
    BYTE* src = (BYTE*)malloc(nbytes);
    if (!src) {
        return;
    }
    memcpy(src, bits, nbytes);

    auto sample = [&](int x, int y, int c) -> int {
        if (x < 0) {
            x = 0;
        } else if (x >= width) {
            x = width - 1;
        }
        if (y < 0) {
            y = 0;
        } else if (y >= height) {
            y = height - 1;
        }
        return src[y * stride + x * 4 + c];
    };

    for (int y = 0; y < height; y++) {
        BYTE* row = bits + y * stride;
        for (int x = 0; x < width; x++) {
            BYTE* px = row + x * 4;
            for (int c = 0; c < 3; c++) {
                int center = sample(x, y, c);
                // 8-neighbor Laplacian (stronger edge pickup than 4-neighbor on soft scans).
                int hp = 8 * center - sample(x, y - 1, c) - sample(x, y + 1, c) - sample(x - 1, y, c) -
                         sample(x + 1, y, c) - sample(x - 1, y - 1, c) - sample(x + 1, y - 1, c) -
                         sample(x - 1, y + 1, c) - sample(x + 1, y + 1, c);
                int v = (int)((float)center + amount * (float)hp / 8.0f + 0.5f);
                if (v < 0) {
                    v = 0;
                } else if (v > 255) {
                    v = 255;
                }
                px[c] = (BYTE)v;
            }
        }
    }
    free(src);
}

bool BlitWithDisplayFilter(HDC hdcDst, int xDst, int yDst, int dxDst, int dyDst, HDC hdcSrc, int xSrc, int ySrc,
                           int dxSrc, int dySrc, const DisplayFilterParams& pIn) {
    if (dxDst <= 0 || dyDst <= 0 || dxSrc <= 0 || dySrc <= 0) {
        return false;
    }
    DisplayFilterParams p = ClampParams(pIn);
    DocumentEnhancementMode envMode;
    if (TryParseDocumentEnhancerEnvOverride(&envMode)) {
        p.mode = envMode;
    }

    // hdcSrc is the original page. Theme recolor is applied after enhancement.
    if (!p.IsActive()) {
        if (dxDst == dxSrc && dyDst == dySrc) {
            return BitBlt(hdcDst, xDst, yDst, dxDst, dyDst, hdcSrc, xSrc, ySrc, SRCCOPY);
        }
        SetStretchBltMode(hdcDst, HALFTONE);
        SetBrushOrgEx(hdcDst, 0, 0, nullptr);
        return StretchBlt(hdcDst, xDst, yDst, dxDst, dyDst, hdcSrc, xSrc, ySrc, dxSrc, dySrc, SRCCOPY);
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = dxDst;
    bmi.bmiHeader.biHeight = -dyDst;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(hdcDst, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) {
            DeleteObject(dib);
        }
        return false;
    }

    HDC tmpDC = CreateCompatibleDC(hdcDst);
    if (!tmpDC) {
        DeleteObject(dib);
        return false;
    }
    HGDIOBJ old = SelectObject(tmpDC, dib);
    SetStretchBltMode(tmpDC, HALFTONE);
    SetBrushOrgEx(tmpDC, 0, 0, nullptr);
    bool ok = StretchBlt(tmpDC, 0, 0, dxDst, dyDst, hdcSrc, xSrc, ySrc, dxSrc, dySrc, SRCCOPY);
    if (ok) {
        BITMAP bm{};
        int stride = dxDst * 4;
        if (GetObjectW(dib, sizeof(bm), &bm) && bm.bmWidthBytes > 0) {
            stride = bm.bmWidthBytes;
        }
        if (p.mode == DocumentEnhancementMode::Legacy) {
            // Keep the original RGB LUT + RGB Laplacian path for A/B.
            RebuildDisplayFilterLut(p.brightness, p.contrast);
            ApplyLutToDibBits((BYTE*)bits, dxDst, dyDst, stride);
            ApplySharpenToDibBits((BYTE*)bits, dxDst, dyDst, stride, p.sharpness);
        } else {
            DocumentEnhancementParams ep = BuildEnhancementParams(p.mode, p.brightness, p.contrast, p.sharpness);
            ApplyDocumentEnhancement((uint8_t*)bits, dxDst, dyDst, stride, ep, nullptr);
        }
        ok = BitBlt(hdcDst, xDst, yDst, dxDst, dyDst, tmpDC, 0, 0, SRCCOPY);
    }
    SelectObject(tmpDC, old);
    DeleteDC(tmpDC);
    DeleteObject(dib);
    return ok;
}

RenderedBitmap* CreateDisplayFilteredBitmap(RenderedBitmap* src, const DisplayFilterParams& pIn) {
    if (!src || !src->IsValid()) {
        return nullptr;
    }
    DisplayFilterParams p = ClampParams(pIn);
    DocumentEnhancementMode envMode;
    if (TryParseDocumentEnhancerEnvOverride(&envMode)) {
        p.mode = envMode;
    }
    if (!p.IsActive()) {
        return nullptr;
    }
    Size sz = src->GetSize();
    if (sz.dx <= 0 || sz.dy <= 0) {
        return nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz.dx;
    bmi.bmiHeader.biHeight = -sz.dy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) {
            DeleteObject(dib);
        }
        return nullptr;
    }

    HDC dstDC = CreateCompatibleDC(nullptr);
    HDC srcDC = CreateCompatibleDC(nullptr);
    if (!dstDC || !srcDC) {
        if (dstDC) {
            DeleteDC(dstDC);
        }
        if (srcDC) {
            DeleteDC(srcDC);
        }
        DeleteObject(dib);
        return nullptr;
    }
    HGDIOBJ oldDst = SelectObject(dstDC, dib);
    HGDIOBJ oldSrc = SelectObject(srcDC, src->GetBitmap());
    BitBlt(dstDC, 0, 0, sz.dx, sz.dy, srcDC, 0, 0, SRCCOPY);
    SelectObject(srcDC, oldSrc);
    SelectObject(dstDC, oldDst);
    DeleteDC(srcDC);
    DeleteDC(dstDC);

    BITMAP bm{};
    int stride = sz.dx * 4;
    if (GetObjectW(dib, sizeof(bm), &bm) && bm.bmWidthBytes > 0) {
        stride = bm.bmWidthBytes;
    }
    if (p.mode == DocumentEnhancementMode::Legacy) {
        RebuildDisplayFilterLut(p.brightness, p.contrast);
        ApplyLutToDibBits((BYTE*)bits, sz.dx, sz.dy, stride);
        ApplySharpenToDibBits((BYTE*)bits, sz.dx, sz.dy, stride, p.sharpness);
    } else {
        DocumentEnhancementParams ep = BuildEnhancementParams(p.mode, p.brightness, p.contrast, p.sharpness);
        ApplyDocumentEnhancement((uint8_t*)bits, sz.dx, sz.dy, stride, ep, nullptr);
    }
    return new RenderedBitmap(dib, sz);
}

void InvalidateDisplayFilterViews() {
    if (gRenderCache) {
        gRenderCache->ClearFilteredBitmaps();
    }
    for (MainWindow* win : gWindows) {
        if (win && win->hwndCanvas) {
            InvalidateRect(win->hwndCanvas, nullptr, FALSE);
        }
        UpdateDisplayFilterToolbarButton(win);
    }
}

void ApplyDisplayFilterPreset(MainWindow* win, const char* preset) {
    if (!win || !preset) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->IsDocLoaded()) {
        return;
    }
    DisplayFilterParams p = GetDisplayFilterForTab(tab);
    if (str::EqI(preset, "auto") || str::EqI(preset, "on")) {
        p.mode = DocumentEnhancementMode::Auto;
        p.brightness = 0;
        p.contrast = 0;
        p.sharpness = 0;
    } else if (str::EqI(preset, "reading")) {
        p.mode = DocumentEnhancementMode::Reading;
        p.brightness = 0;
        p.contrast = 0;
        p.sharpness = 0;
    } else if (str::EqI(preset, "scan") || str::EqI(preset, "scanned")) {
        p.mode = DocumentEnhancementMode::Scanned;
        p.brightness = 0;
        p.contrast = 0;
        p.sharpness = 0;
    } else if (str::EqI(preset, "legacy")) {
        p.mode = DocumentEnhancementMode::Legacy;
        p.brightness = 8;
        p.contrast = 12;
        p.sharpness = 20;
    } else if (str::EqI(preset, "off") || str::EqI(preset, "none") || str::EqI(preset, "reset")) {
        p.mode = DocumentEnhancementMode::Off;
        p.brightness = 0;
        p.contrast = 0;
        p.sharpness = 0;
    }
    SetDisplayFilterForTab(tab, p, true);
}

void UpdateDisplayFilterToolbarButton(MainWindow* win) {
    if (!win || !win->hwndToolbar) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    bool supported = DisplayFilterSupportedForTab(tab);
    bool checked = supported && GetDisplayFilterForTab(tab).IsActive();
    SetToolbarButtonEnableState(win, CmdDisplayFilter, supported);
    SetToolbarButtonCheckedState(win, CmdDisplayFilter, checked);
    UpdateDisplayFilterToolbarTip(win);
}

void ToggleDisplayFilterActive(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!DisplayFilterSupportedForTab(tab)) {
        return;
    }
    DisplayFilterParams p = GetDisplayFilterForTab(tab);
    bool wasActive = p.IsActive();
    if (p.IsActive()) {
        p.mode = DocumentEnhancementMode::Off;
        p.brightness = 0;
        p.contrast = 0;
        p.sharpness = 0;
    } else {
        p.mode = DocumentEnhancementMode::Auto;
        p.brightness = 0;
        p.contrast = 0;
        p.sharpness = 0;
    }
    SetDisplayFilterForTab(tab, p, true);
    if (gRenderCache) {
        gRenderCache->ClearFilteredBitmaps();
        // Cached tiles may already be theme-tinted. Enhancement reads the
        // original page, so drop them and render 原稿 again.
        if (wasActive != p.IsActive()) {
            gRenderCache->darkModeEpoch++;
        }
    }
}

void HideDisplayFilterPanel(MainWindow* win) {
    (void)win;
}

void DeleteDisplayFilterPanel(MainWindow* win) {
    if (!win) {
        return;
    }
    win->displayFilterPanel = nullptr;
}

bool IsDisplayFilterPanelVisible(MainWindow* win) {
    (void)win;
    return false;
}

void RefreshDisplayFilterPanelsTheme() {}
