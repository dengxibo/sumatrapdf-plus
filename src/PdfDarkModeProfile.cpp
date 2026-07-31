/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "DocController.h"
#include "Theme.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "PdfDarkMode.h"

static float ColorChannel01(byte v) {
    return v / 255.f;
}

static DarkModePalette BuildPaletteFromColors(COLORREF textCol, COLORREF bgCol, COLORREF linkCol) {
    byte tr, tg, tb, br, bg, bb, lr, lg, lb;
    UnpackColor(textCol, tr, tg, tb);
    UnpackColor(bgCol, br, bg, bb);
    UnpackColor(linkCol, lr, lg, lb);

    DarkModePalette p;
    p.textR = ColorChannel01(tr);
    p.textG = ColorChannel01(tg);
    p.textB = ColorChannel01(tb);
    p.bgR = ColorChannel01(br);
    p.bgG = ColorChannel01(bg);
    p.bgB = ColorChannel01(bb);
    p.linkR = ColorChannel01(lr);
    p.linkG = ColorChannel01(lg);
    p.linkB = ColorChannel01(lb);
    p.diffR = p.bgR - p.textR;
    p.diffG = p.bgG - p.textG;
    p.diffB = p.bgB - p.textB;
    return p;
}

bool DarkModeProfileUsesObjectLevel(const DarkModeProfile* profile) {
    return profile && (profile->mode == PageColorMode::SmartDark || profile->mode == PageColorMode::FollowThemeDirect);
}

bool DarkModeProfileUsesFollowThemeDirect(const DarkModeProfile* profile) {
    return profile && profile->mode == PageColorMode::FollowThemeDirect;
}

bool DarkModeProfileUsesLegacyPostProcess(const DarkModeProfile* profile) {
    if (!profile) {
        return false;
    }
    return profile->mode == PageColorMode::LegacyInvert || profile->mode == PageColorMode::PreserveImages;
}

u32 PdfDarkModeComputeProfileHash(const DarkModeProfile* profile) {
    if (!profile) {
        return 0;
    }
    auto mix = [](u32 h, u32 v) -> u32 { return h * 31 + v; };
    u32 h = 0;
    h = mix(h, (u32)profile->mode);
    h = mix(h, (u32)profile->foreground);
    h = mix(h, (u32)profile->pageBackground);
    h = mix(h, (u32)profile->linkColor);
    h = mix(h, (u32)profile->preservePdfImages);
    h = mix(h, (u32)profile->preservePdfImagesMinSize);
    h = mix(h, *(u32*)&profile->options.scanImageCoverageThreshold);
    h = mix(h, *(u32*)&profile->options.minScanDominantCoverage);
    h = mix(h, *(u32*)&profile->options.maxScanAspectSkew);
    h = mix(h, (u32)profile->options.maxTextOpsForScanPage);
    h = mix(h, (u32)profile->options.maxVectorOpsForScanPage);
    h = mix(h, (u32)profile->options.followThemeBitmapRecolorMinTextOps);
    h = mix(h, (u32)profile->options.followThemeBitmapRecolorMaxImageOps);
    h = mix(h, *(u32*)&profile->options.followThemeBitmapRecolorMaxImageCoverage);
    h = mix(h, *(u32*)&profile->options.preserveImagePaperSoftening);
    h = mix(h, *(u32*)&profile->options.lightFillChromaThreshold);
    h = mix(h, *(u32*)&profile->options.lightFillLuminanceThreshold);
    h = mix(h, (u32)GetPdfDocumentColorMode());
    h = mix(h, ThemeUsesDarkChrome() ? 1 : 0);
    h = mix(h, ThemeUsesOriginalPageColors() ? 1 : 0);
    h = mix(h, ThemeUsesEyeCareChrome() ? 1 : 0);
    return h;
}

static bool IsFixedPageMupdfEngine(EngineBase* engine) {
    if (!engine || engine->kind != kindEngineMupdf || engine->IsImageCollection()) {
        return false;
    }
    return str::EqI(engine->defaultExt, ".pdf") || str::EqI(engine->defaultExt, ".xps");
}

static bool IsFixedPageDjVuEngine(EngineBase* engine) {
    return engine && engine->kind == kindEngineDjVu;
}

static bool IsReflowableMupdfEbookEngine(EngineBase* engine) {
    if (!engine || engine->kind != kindEngineMupdf || engine->IsImageCollection()) {
        return false;
    }
    return !str::EqI(engine->defaultExt, ".pdf") && !str::EqI(engine->defaultExt, ".xps");
}

bool ReflowEbookUsesThemeBitmapRecolor() {
    if (GetPdfDocumentColorMode() == PdfDocumentColorMode::Light) {
        return false;
    }
    // Dark follow theme uses reflow CSS; bitmap recolor would invert images too.
    return ThemeUsesEyeCareChrome();
}

static void ApplyDocumentColorModeToReflowMupdfProfile(DarkModeProfile* profile) {
    switch (GetPdfDocumentColorMode()) {
        case PdfDocumentColorMode::Light:
            profile->mode = PageColorMode::Normal;
            break;
        case PdfDocumentColorMode::Auto:
        default:
            profile->mode = ReflowEbookUsesThemeBitmapRecolor() ? PageColorMode::LegacyInvert : PageColorMode::Normal;
            break;
    }
}

static void ApplyDocumentColorModeToFixedPageProfile(EngineBase* engine, DarkModeProfile* profile) {
    if (EngineMupdfIsFollowThemeProbePending(engine)) {
        profile->mode = PageColorMode::PreserveImages;
        return;
    }
    switch (GetPdfDocumentColorMode()) {
        case PdfDocumentColorMode::Light:
            profile->mode = PageColorMode::Normal;
            break;
        case PdfDocumentColorMode::Black:
        case PdfDocumentColorMode::Auto:
        default:
            if (EngineSupportsSmartDarkMode(engine) && PdfDarkModeUsesObjectLevel()) {
                if (PdfFollowThemePreservesEmbeddedImageColors()) {
                    profile->mode = PageColorMode::FollowThemeDirect;
                } else {
                    profile->mode = PageColorMode::SmartDark;
                }
            } else if (profile->preservePdfImages || PdfFollowThemePreservesEmbeddedImageColors()) {
                profile->mode = PageColorMode::PreserveImages;
            } else if (ThemeUsesDarkChrome()) {
                profile->mode = PageColorMode::LegacyInvert;
            } else {
                // Light-White Smart: original page pixels; Light-Warm uses PreserveImages when enabled.
                profile->mode = PageColorMode::Normal;
            }
            break;
    }
}

void BuildViewDarkModeProfile(EngineBase* engine, DarkModeProfile* profile) {
    ReportIf(!profile);
    if (!profile) {
        return;
    }
    *profile = DarkModeProfile{};

    COLORREF bgCol;
    COLORREF textCol = ThemePageRenderColors(bgCol, true);
    profile->foreground = textCol;
    profile->pageBackground = bgCol;
    profile->linkColor = ThemeUsesDarkChrome() ? ThemeWindowLinkColor() : 0;
    profile->strength = 1.f;
    profile->preservePdfImages = PdfSmartModePreservesEmbeddedImages();
    profile->preservePdfImagesMinSize = GetPreservePdfImagesMinSize();
    profile->options = PdfDarkModeCurrentOptions();
    profile->palette = BuildPaletteFromColors(textCol, bgCol, profile->linkColor);

    if (IsFixedPageMupdfEngine(engine) || IsFixedPageDjVuEngine(engine)) {
        ApplyDocumentColorModeToFixedPageProfile(engine, profile);
    } else if (IsReflowableMupdfEbookEngine(engine)) {
        ApplyDocumentColorModeToReflowMupdfProfile(profile);
    }

    profile->hash = PdfDarkModeComputeProfileHash(profile);
}
