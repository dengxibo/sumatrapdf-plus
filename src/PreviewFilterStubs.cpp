/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

#include "AppTools.h"
#include "EbookFontConfig.h"
#include "EbookTypography.h"
#include "OfficeConvert.h"

struct GlobalPrefs;

// Minimal stubs for PdfFilter/PdfPreview builds that compile EngineMupdf.cpp and
// EpubMeta.cpp without the full SumatraPDF settings / app-data stack.

GlobalPrefs* gGlobalPrefs = nullptr;

// Shell extensions must not launch Word COM. Classic .doc conversion stays in
// the main app (OfficeConvert.cpp); filter/preview just refuse those files.
char* ConvertOleOfficeToDocx(const char* srcPath, char* errOut, int errOutLen) {
    (void)srcPath;
    if (errOut && errOutLen > 0) {
        str::BufSet(errOut, errOutLen, "Office conversion is not available here.");
    }
    return nullptr;
}

bool IsCachedOleOfficeDocx(const char* path) {
    (void)path;
    return false;
}

void ForgetCachedOleOfficeDocx(const char* srcPath) {
    (void)srcPath;
}

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
