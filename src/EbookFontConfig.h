/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "EbookTypography.h"

enum class DisplayMode;

constexpr const char* kDefaultEbookLatinFontFamily = "Literata";
constexpr const char* kDefaultEbookCjkFontFamily = "Source Han Serif SC";
constexpr const char* kBundledEbookLatinFontSourceSerif = "Source Serif 4";
constexpr const char* kBundledEbookCjkFontWenkai = "LXGW WenKai";

#ifdef __cplusplus
extern "C" {
#endif
void sumatra_set_ebook_font_config(const char* cjk_family, const char* cjk_file);
#ifdef __cplusplus
}
#endif

// Read GlobalPrefs::eBookUI and push config to MuPDF + GDI+ paths.
void ApplyEbookFontSettingsFromPrefs();

const char* GetEbookLatinFontFamily();
const char* GetEbookCjkFontFamily();
const char* GetEbookCjkFontFile();

const WCHAR* GetEbookLatinFontFamilyW();
const WCHAR* GetEbookCjkFontFamilyW();

bool IsEbookCjkFontRequest(const char* fontName);
bool IsEbookCjkFontRequestW(const WCHAR* fontName);

bool IsBundledLatinFontFamily(const char* family);
bool IsBundledCjkFontFamily(const char* family);
bool IsSourceHanScFontFamily(const char* family);
bool IsLxgwWenkaiFontFamily(const char* family);

const char* NormalizeEbookLatinFontFamily(const char* family);
const char* NormalizeEbookCjkFontFamily(const char* family);
const char* GetEbookLatinFontMenuLabel(const char* canonicalFamily);
const char* GetEbookCjkFontMenuLabel(const char* canonicalFamily);
bool EbookLatinFontFamiliesEquivalent(const char* a, const char* b);
bool EbookCjkFontFamiliesEquivalent(const char* a, const char* b);

bool UsesCustomInstalledEbookFonts();
bool UsesNonDefaultEbookReaderFonts();

// Reader limits keep font sizes useful for reading.
constexpr float kEbookFontSizeMinPt = 8.f;
constexpr float kEbookFontSizeMaxPt = 32.f;
constexpr float kEbookFontSizeBuiltinPt = 11.f;
constexpr float kEbookFacingFontSizeBuiltinPt = 17.f;
constexpr float kEbookFontSizeStepPt = 2.f;

bool UsesEbookDisplayModeFontSizes(const char* filePath);
DisplayMode GetDefaultEbookDisplayMode(const char* filePath);
// Call on the UI thread when switching layouts, before rebuilding the engine.
void SetEbookFontDisplayMode(const char* filePath, DisplayMode mode);

float GetEbookReaderFontSizePt(const char* filePath = nullptr);
float GetEffectiveEbookFontSizePt(const char* filePath = nullptr);
bool UsesNonDefaultEbookFontSize(const char* filePath = nullptr);
bool CanIncreaseEbookFontSize(const char* filePath = nullptr);
bool CanDecreaseEbookFontSize(const char* filePath = nullptr);
// direction: +1 larger, -1 smaller. Returns false if already at limit.
bool AdjustEbookFontSize(int direction, const char* filePath);
// Clears the book override and restores the reading options default.
bool ResetEbookFontSize(const char* filePath = nullptr);

// CSS fragments for MuPDF user stylesheet.
TempStr BuildEbookReaderFontCss(EbookTypographyKind typographyKind);
TempStr BuildEbookFallbackFontCss();
TempStr BuildEbookForceFontCss(EbookTypographyKind typographyKind);
// displayDpi: same DPI used for fz_layout_document page size (EngineMupdf::displayDPI).
TempStr BuildEbookForceFontSizeCss(int displayDpi, const char* filePath = nullptr, bool preserveRelativeSizes = false);
