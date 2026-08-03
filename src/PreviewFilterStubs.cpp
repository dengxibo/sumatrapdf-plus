/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

#include "AppTools.h"
#include "EbookFontConfig.h"
#include "EbookTypography.h"

struct GlobalPrefs;

// Minimal stubs for PdfFilter/PdfPreview builds that compile EngineMupdf.cpp and
// EpubMeta.cpp without the full SumatraPDF settings / app-data stack.

GlobalPrefs* gGlobalPrefs = nullptr;

TempStr GetPathInAppDataDirTemp(const char* fileName) {
    (void)fileName;
    return nullptr;
}

bool UsesNonDefaultEbookReaderFonts() {
    return false;
}

bool UsesNonDefaultEbookFontSize() {
    return false;
}

EbookTypographyKind GetEbookTypographyKind() {
    return EbookTypographyKind::Latin;
}

void SetEbookTypographyKind(EbookTypographyKind kind) {
    (void)kind;
}

void SetEbookReaderStyleMobi(bool readerStyle) {
    (void)readerStyle;
}

TempStr BuildEbookReaderFontCss(EbookTypographyKind typographyKind) {
    (void)typographyKind;
    return nullptr;
}

TempStr BuildEbookFallbackFontCss() {
    return nullptr;
}

TempStr BuildEbookForceFontCss(EbookTypographyKind typographyKind) {
    (void)typographyKind;
    return nullptr;
}

TempStr BuildEbookForceFontSizeCss(int displayDpi) {
    (void)displayDpi;
    return nullptr;
}

EbookTypographyKind DetectMobiReaderTypography(const ByteSlice& html) {
    (void)html;
    return EbookTypographyKind::Latin;
}

bool EbookReaderStyleMobi() {
    return false;
}

bool EbookUsesCjkTypography() {
    return false;
}

const char* GetEbookLatinFontFamily() {
    return kDefaultEbookLatinFontFamily;
}

const char* GetEbookCjkFontFamily() {
    return kDefaultEbookCjkFontFamily;
}

const WCHAR* GetEbookLatinFontFamilyW() {
    return nullptr;
}

const WCHAR* GetEbookCjkFontFamilyW() {
    return nullptr;
}

float GetEbookReaderFontSizePt() {
    return kEbookFontSizeBuiltinPt;
}

bool IsEbookCjkFontRequestW(const WCHAR* fontName) {
    (void)fontName;
    return false;
}

bool IsBundledCjkFontFamily(const char* family) {
    (void)family;
    return false;
}
