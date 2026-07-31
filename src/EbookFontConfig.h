/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "EbookTypography.h"

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

// CSS fragments for MuPDF user stylesheet.
TempStr BuildEbookReaderFontCss(EbookTypographyKind typographyKind);
TempStr BuildEbookFallbackFontCss();
TempStr BuildEbookForceFontCss(EbookTypographyKind typographyKind);
