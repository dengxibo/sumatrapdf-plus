/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "Settings.h"
#include "SumatraPDF.h"
#include "EbookFontConfig.h"
#include "utils/GdiPlusUtil.h"

extern EBookUI* GetEBookUI();

static char gLatinFontFamily[128] = {};
static char gCjkFontFamily[128] = {};
static char gCjkFontFile[MAX_PATH] = "";
static AutoFreeWStr gLatinFontFamilyW;
static AutoFreeWStr gCjkFontFamilyW;
static bool gEbookFontDefaultsInited = false;

static void RefreshWideNames() {
    gLatinFontFamilyW.SetCopy(ToWStrTemp(gLatinFontFamily));
    gCjkFontFamilyW.SetCopy(ToWStrTemp(gCjkFontFamily));
}

static void EnsureEbookFontDefaults() {
    if (gEbookFontDefaultsInited) {
        return;
    }
    str::BufSet(gLatinFontFamily, sizeof(gLatinFontFamily), kDefaultEbookLatinFontFamily);
    str::BufSet(gCjkFontFamily, sizeof(gCjkFontFamily), kDefaultEbookCjkFontFamily);
    RefreshWideNames();
    gEbookFontDefaultsInited = true;
}

static bool Utf8Contains(const char* haystack, const char* needle) {
    return haystack && needle && strstr(haystack, needle) != nullptr;
}

bool IsBundledLatinFontFamily(const char* family) {
    if (!family || !family[0]) {
        return false;
    }
    return str::StartsWithI(family, "Literata") || str::StartsWithI(family, "Source Serif");
}

bool IsSourceHanScFontFamily(const char* family) {
    if (!family || !family[0]) {
        return false;
    }
    if (str::EqI(family, kDefaultEbookCjkFontFamily)) {
        return true;
    }
    if (str::EqI(family, "\xe6\x80\x9d\xe6\xba\x90\xe5\xae\x8b\xe4\xbd\x93") ||
        str::EqI(family, "\xe6\x80\x9d\xe6\xba\x90\xe5\xae\x8b\xe9\xab\x94")) {
        return true;
    }
    if (str::StartsWithI(family, "Source Han Serif") || str::StartsWithI(family, "Noto Serif CJK")) {
        return true;
    }
    return Utf8Contains(family, "\xe6\x80\x9d\xe6\xba\x90"); // 思源
}

bool IsLxgwWenkaiFontFamily(const char* family) {
    if (!family || !family[0]) {
        return false;
    }
    if (str::StartsWithI(family, "LXGW")) {
        return true;
    }
    return Utf8Contains(family, "\xe9\x9c\x9e\xe9\x82\x97") || // 霞鹜
           Utf8Contains(family, "\xe9\x9c\x9e\xe9\xb\xb") ||   // 霞鶩
           Utf8Contains(family, "\xe6\x96\x87\xe9\x91\x8b");   // 文楷
}

bool IsBundledCjkFontFamily(const char* family) {
    if (!family || !family[0]) {
        return false;
    }
    if (IsSourceHanScFontFamily(family)) {
        return true;
    }
    return IsLxgwWenkaiFontFamily(family);
}

const char* NormalizeEbookLatinFontFamily(const char* family) {
    if (str::EqI(family, "System")) {
        return kDefaultEbookLatinFontFamily;
    }
    if (!family || !family[0]) {
        return kDefaultEbookLatinFontFamily;
    }
    if (str::StartsWithI(family, "Literata")) {
        return kDefaultEbookLatinFontFamily;
    }
    if (str::StartsWithI(family, "Source Serif")) {
        return kBundledEbookLatinFontSourceSerif;
    }
    return family;
}

const char* NormalizeEbookCjkFontFamily(const char* family) {
    if (str::EqI(family, "System")) {
        return kDefaultEbookCjkFontFamily;
    }
    if (!family || !family[0]) {
        return kDefaultEbookCjkFontFamily;
    }
    if (IsSourceHanScFontFamily(family)) {
        return kDefaultEbookCjkFontFamily;
    }
    if (IsLxgwWenkaiFontFamily(family)) {
        return kBundledEbookCjkFontWenkai;
    }
    return family;
}

const char* GetEbookLatinFontMenuLabel(const char* canonicalFamily) {
    const char* family = NormalizeEbookLatinFontFamily(canonicalFamily);
    if (str::EqI(family, kDefaultEbookLatinFontFamily)) {
        return kDefaultEbookLatinFontFamily;
    }
    if (str::EqI(family, kBundledEbookLatinFontSourceSerif)) {
        return kBundledEbookLatinFontSourceSerif;
    }
    return family;
}

const char* GetEbookCjkFontMenuLabel(const char* canonicalFamily) {
    const char* family = NormalizeEbookCjkFontFamily(canonicalFamily);
    if (str::EqI(family, kDefaultEbookCjkFontFamily)) {
        return "\xe6\x80\x9d\xe6\xba\x90\xe5\xae\x8b\xe4\xbd\x93"; // 思源宋体
    }
    if (str::EqI(family, kBundledEbookCjkFontWenkai)) {
        return "\xe9\x9c\x9e\xe9\x82\x97\xe6\x96\x87\xe9\x91\x8b"; // 霞鹜文楷
    }
    return family;
}

bool EbookLatinFontFamiliesEquivalent(const char* a, const char* b) {
    return str::EqI(NormalizeEbookLatinFontFamily(a), NormalizeEbookLatinFontFamily(b));
}

bool EbookCjkFontFamiliesEquivalent(const char* a, const char* b) {
    return str::EqI(NormalizeEbookCjkFontFamily(a), NormalizeEbookCjkFontFamily(b));
}

void ApplyEbookFontSettingsFromPrefs() {
    EnsureEbookFontDefaults();
    auto* ui = GetEBookUI();
    const char* latin = ui && ui->fontFamily ? ui->fontFamily : kDefaultEbookLatinFontFamily;
    const char* cjk = ui && ui->cjkFontFamily ? ui->cjkFontFamily : kDefaultEbookCjkFontFamily;
    str::BufSet(gLatinFontFamily, sizeof(gLatinFontFamily), NormalizeEbookLatinFontFamily(latin));
    str::BufSet(gCjkFontFamily, sizeof(gCjkFontFamily), NormalizeEbookCjkFontFamily(cjk));
    if (ui && ui->cjkFontFile && ui->cjkFontFile[0] && IsBundledCjkFontFamily(gCjkFontFamily)) {
        str::BufSet(gCjkFontFile, sizeof(gCjkFontFile), ui->cjkFontFile);
    } else {
        gCjkFontFile[0] = '\0';
    }
    RefreshWideNames();
    ConfigureBundledReaderLatinFont(gLatinFontFamily);
    ConfigureBundledReaderCjkFont(gCjkFontFamily, gCjkFontFile[0] ? gCjkFontFile : nullptr);
    sumatra_set_ebook_font_config(gCjkFontFamily, gCjkFontFile);
    ResetBundledReaderFonts();
}

const char* GetEbookLatinFontFamily() {
    EnsureEbookFontDefaults();
    return gLatinFontFamily;
}

const char* GetEbookCjkFontFamily() {
    EnsureEbookFontDefaults();
    return gCjkFontFamily;
}

const char* GetEbookCjkFontFile() {
    return gCjkFontFile[0] ? gCjkFontFile : nullptr;
}

const WCHAR* GetEbookLatinFontFamilyW() {
    return gLatinFontFamilyW;
}

const WCHAR* GetEbookCjkFontFamilyW() {
    return gCjkFontFamilyW;
}

static bool NamesMatchIW(const WCHAR* a, const WCHAR* b) {
    return a && b && str::EqI(a, b);
}

bool IsEbookCjkFontRequest(const char* fontName) {
    if (!fontName) {
        return false;
    }
    return EbookCjkFontFamiliesEquivalent(fontName, gCjkFontFamily);
}

bool IsEbookCjkFontRequestW(const WCHAR* fontName) {
    if (!fontName) {
        return false;
    }
    if (NamesMatchIW(fontName, gCjkFontFamilyW)) {
        return true;
    }
    TempStr utf8 = ToUtf8Temp(fontName);
    return utf8 && EbookCjkFontFamiliesEquivalent(utf8, gCjkFontFamily);
}

static TempStr CssFontFamilyToken(const char* family) {
    if (!family || !family[0]) {
        return nullptr;
    }
    if (str::Find(family, " ")) {
        return str::FormatTemp("\"%s\"", family);
    }
    return (TempStr)family;
}

TempStr BuildEbookReaderFontCss(EbookTypographyKind typographyKind) {
    TempStr latin = CssFontFamilyToken(gLatinFontFamily);
    TempStr cjk = CssFontFamilyToken(gCjkFontFamily);
    if (typographyKind == EbookTypographyKind::Cjk) {
        return str::FormatTemp(
            R"(/* Default body face only; book CSS (e.g. STKai / MKai PRC) keeps priority on styled elements. */
html, body {
  font-family: %s, "思源宋体", "Source Han Serif", "Noto Serif CJK SC", %s, Georgia, "NSimSun", "SimSun", "宋体", serif !important;
}
)",
            cjk, latin);
    }
    return str::FormatTemp(
        R"(/* Default body face only; book CSS (e.g. STKai for 书虫) keeps priority on styled elements. */
html, body {
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman", %s, "思源宋体", "Source Han Serif", "Noto Serif CJK SC", "NSimSun", "SimSun", "宋体", serif !important;
}
)",
        latin, cjk);
}

TempStr BuildEbookFallbackFontCss() {
    TempStr latin = CssFontFamilyToken(gLatinFontFamily);
    TempStr cjk = CssFontFamilyToken(gCjkFontFamily);
    return str::FormatTemp(
        R"(html, body, p, span, blockquote, h1, h2, h3, h4, h5, h6, li, td, th, div {
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman", %s, "思源宋体", "NSimSun", "SimSun", "宋体", serif !important;
  line-height: 1.45 !important;
}
p {
  margin: 0.35em 0;
}
)",
        latin, cjk);
}
