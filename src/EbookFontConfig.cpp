/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "Settings.h"
#include "SumatraPDF.h"
#include "EbookFontConfig.h"
#include "EbookInstalledFonts.h"
#include "utils/GdiPlusUtil.h"
#include "mui/Mui.h"

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
    return GetInstalledCjkFontMenuLabel(family);
}

bool EbookLatinFontFamiliesEquivalent(const char* a, const char* b) {
    return str::EqI(NormalizeEbookLatinFontFamily(a), NormalizeEbookLatinFontFamily(b));
}

bool EbookCjkFontFamiliesEquivalent(const char* a, const char* b) {
    return str::EqI(NormalizeEbookCjkFontFamily(a), NormalizeEbookCjkFontFamily(b));
}

bool UsesCustomInstalledEbookFonts() {
    EnsureEbookFontDefaults();
    return !IsBundledLatinFontFamily(gLatinFontFamily) || !IsBundledCjkFontFamily(gCjkFontFamily);
}

bool UsesNonDefaultEbookReaderFonts() {
    EnsureEbookFontDefaults();
    if (!EbookLatinFontFamiliesEquivalent(gLatinFontFamily, kDefaultEbookLatinFontFamily)) {
        return true;
    }
    if (!EbookCjkFontFamiliesEquivalent(gCjkFontFamily, kDefaultEbookCjkFontFamily)) {
        return true;
    }
    return UsesCustomInstalledEbookFonts();
}

float GetEbookReaderFontSizePt() {
    auto* ui = GetEBookUI();
    if (!ui || ui->fontSize < kEbookFontSizeMinPt || ui->fontSize > kEbookFontSizeMaxPt) {
        return 0.f;
    }
    return ui->fontSize;
}

bool UsesNonDefaultEbookFontSize() {
    return GetEbookReaderFontSizePt() > 0.f;
}

float GetEffectiveEbookFontSizePt() {
    float pt = GetEbookReaderFontSizePt();
    if (pt > 0.f) {
        return pt;
    }
    return kEbookFontSizeBuiltinPt;
}

bool CanIncreaseEbookFontSize() {
    return GetEffectiveEbookFontSizePt() + kEbookFontSizeStepPt <= kEbookFontSizeMaxPt;
}

bool CanDecreaseEbookFontSize() {
    return GetEffectiveEbookFontSizePt() - kEbookFontSizeStepPt >= kEbookFontSizeMinPt;
}

bool AdjustEbookFontSize(int direction) {
    if (direction == 0) {
        return false;
    }
    if (direction > 0) {
        if (!CanIncreaseEbookFontSize()) {
            return false;
        }
    } else if (!CanDecreaseEbookFontSize()) {
        return false;
    }
    float effective = GetEffectiveEbookFontSizePt();
    float next = effective + (float)direction * kEbookFontSizeStepPt;
    next = limitValue(next, kEbookFontSizeMinPt, kEbookFontSizeMaxPt);
    auto* ui = GetEBookUI();
    if (!ui) {
        return false;
    }
    ui->fontSize = next;
    return true;
}

bool ResetEbookFontSize() {
    auto* ui = GetEBookUI();
    if (!ui || !UsesNonDefaultEbookFontSize()) {
        return false;
    }
    ui->fontSize = 0.f;
    return true;
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
    mui::ClearCachedFonts();
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
    EnsureEbookFontDefaults();
    return gLatinFontFamilyW;
}

const WCHAR* GetEbookCjkFontFamilyW() {
    EnsureEbookFontDefaults();
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
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman", serif !important;
}
)",
        latin);
}

TempStr BuildEbookFallbackFontCss() {
    return BuildEbookForceFontCss(EbookTypographyKind::Bilingual);
}

static const char* kEbookForceFontSelectors =
    "html, body, p, span, blockquote, h1, h2, h3, h4, h5, h6, li, td, th, div,\n"
    "section, article, main, header, footer,\n"
    ".calibre,\n"
    ".calibre1, .calibre2, .calibre3, .calibre4, .calibre5, .calibre6, .calibre7, .calibre8, .calibre9, .calibre10,\n"
    ".calibre11, .calibre12, .calibre13, .calibre14, .calibre15, .calibre16, .calibre17, .calibre18, .calibre19, "
    ".calibre20,\n"
    ".calibre_1, .calibre_2, .calibre_3, .calibre_4, .calibre_5, .calibre_6, .calibre_7, .calibre_8, .calibre_9, "
    ".calibre_10,\n"
    ".calibre_11, .calibre_12, .calibre_13, .calibre_14, .calibre_15, .calibre_16, .calibre_17, .calibre_18, "
    ".calibre_19, .calibre_20";

// Bilingual EPUB (bookworm/cnepub): Latin default on body elements; Han via fallback +
// fz_purge_fallback_font_cache on font change. CJK in CSS only on cnepub classes
// (bodyContent_N, kindle-cn-*, publisher overrides). Do NOT use html[lang=zh] or broad
// p { CJK }: English chapters also declare zh lang (H21 logs).
static const char* kEbookForceCjkCnepubSelectors =
    "p.kindle-cn-hei,\n"
    "p.kindle-cn-hei0,\n"
    "p.kindle-cn-para-center,\n"
    "p.kindle-cn-para-center1,\n"
    "p.kindle-cn-copyright-text,\n"
    "p.kindle-cn-signature,\n"
    "p.kindle-cn-heading-1,\n"
    "span.kindle-cn-hei,\n"
    "span.font3";

static TempStr BuildBodyContentCjkCss(const char* cjk) {
    StrBuilder css(16 * 1024);
    // MuPDF rejects an excessively long comma-separated selector list. Keep
    // each rule small while covering cnepub classes well above 100.
    constexpr int kSelectorsPerRule = 16;
    for (int first = 0; first <= 320; first += kSelectorsPerRule) {
        int last = std::min(first + kSelectorsPerRule - 1, 320);
        for (int i = first; i <= last; i++) {
            if (i > first) {
                css.Append(",\n");
            }
            css.AppendFmt("p.bodyContent_%d", i);
        }
        css.AppendFmt(
            " {\n"
            "  font-family: %s, \"Source Han Serif\", \"Noto Serif CJK SC\", \"NSimSun\", \"SimSun\", "
            "serif !important;\n"
            "  line-height: 1.45 !important;\n"
            "}\n",
            cjk);
    }
    return str::DupTemp(css.Get());
}

static TempStr BuildCalibreClassLatinSelectors() {
    static char storage[8 * 1024];
    static bool inited = false;
    if (inited) {
        return storage;
    }
    static const char* kTags[] = {"p", "span", "blockquote", "h1", "h2", "h3", "h4", "h5", "h6", "li"};
    char* dst = storage;
    char* end = storage + sizeof(storage);
    bool first = true;
    auto appendClass = [&](const char* cls) {
        for (const char* tag : kTags) {
            int n = snprintf(dst, (size_t)(end - dst), "%s%s.%s", first ? "" : ",\n", tag, cls);
            if (n <= 0 || dst + n >= end) {
                return false;
            }
            dst += n;
            first = false;
        }
        return true;
    };
    if (!appendClass("calibre")) {
        goto Done;
    }
    for (int n = 1; n <= 20; n++) {
        char cls[32];
        snprintf(cls, sizeof(cls), "calibre%d", n);
        if (!appendClass(cls)) {
            goto Done;
        }
    }
    for (int n = 1; n <= 20; n++) {
        char cls[32];
        snprintf(cls, sizeof(cls), "calibre_%d", n);
        if (!appendClass(cls)) {
            goto Done;
        }
    }
Done:
    *dst = '\0';
    inited = true;
    return storage;
}

static const char* kEbookForceLatinCalibreSelectors =
    ".calibre,\n"
    ".calibre1, .calibre2, .calibre3, .calibre4, .calibre5, .calibre6, .calibre7, .calibre8, .calibre9, .calibre10,\n"
    ".calibre11, .calibre12, .calibre13, .calibre14, .calibre15, .calibre16, .calibre17, .calibre18, .calibre19, "
    ".calibre20,\n"
    ".calibre_1, .calibre_2, .calibre_3, .calibre_4, .calibre_5, .calibre_6, .calibre_7, .calibre_8, .calibre_9, "
    ".calibre_10,\n"
    ".calibre_11, .calibre_12, .calibre_13, .calibre_14, .calibre_15, .calibre_16, .calibre_17, .calibre_18, "
    ".calibre_19, .calibre_20";

static const char* kEbookForceLatinEnSelectors =
    "html[lang=\"en\"] body,\n"
    "html[lang=\"en\"] p,\n"
    "html[lang=\"en\"] span,\n"
    "html[lang=\"en\"] blockquote,\n"
    "html[lang=\"en\"] h1, html[lang=\"en\"] h2, html[lang=\"en\"] h3, html[lang=\"en\"] h4,\n"
    "html[lang=\"en\"] h5, html[lang=\"en\"] h6,\n"
    "html[lang=\"en\"] li, html[lang=\"en\"] td, html[lang=\"en\"] th, html[lang=\"en\"] div,\n"
    "html[lang=\"en\"] section, html[lang=\"en\"] article, html[lang=\"en\"] main,\n"
    "html[lang=\"en\"] header, html[lang=\"en\"] footer,\n"
    "html[lang=\"en-us\"] body,\n"
    "html[lang=\"en-us\"] p,\n"
    "html[lang=\"en-us\"] span,\n"
    "html[lang=\"en-US\"] body,\n"
    "html[lang=\"en-US\"] p,\n"
    "html[lang=\"en-US\"] span,\n"
    "html[lang=\"en-GB\"] body,\n"
    "html[lang=\"en-GB\"] p,\n"
    "html[lang=\"en-GB\"] span";

static const char* kEbookForceLatinZhCnInlineSelectors =
    "html[lang=\"zh-CN\"] span.kindle-cn-italic,\n"
    "html[lang=\"zh-CN\"] span.kindle-cn-eng-yinbiao,\n"
    "html[lang=\"zh-cn\"] span.kindle-cn-italic,\n"
    "html[lang=\"zh-cn\"] span.kindle-cn-eng-yinbiao,\n"
    "html[lang=\"zh\"] span.kindle-cn-italic,\n"
    "html[lang=\"zh\"] span.kindle-cn-eng-yinbiao";

static TempStr BuildCalibreDescendantLatinSelectors() {
    static char storage[16 * 1024];
    static bool inited = false;
    if (inited) {
        return storage;
    }
    static const char* kTags[] = {"p", "span", "blockquote", "h1", "h2", "h3", "h4", "h5", "h6", "li"};
    char* dst = storage;
    char* end = storage + sizeof(storage);
    bool first = true;
    auto appendRule = [&](const char* parent, const char* tag) {
        int n = snprintf(dst, (size_t)(end - dst), "%s%s %s", first ? "" : ",\n", parent, tag);
        if (n <= 0 || dst + n >= end) {
            return false;
        }
        dst += n;
        first = false;
        return true;
    };
    for (int n = 0; n <= 20; n++) {
        char parent[32];
        if (n == 0) {
            snprintf(parent, sizeof(parent), ".calibre");
        } else {
            snprintf(parent, sizeof(parent), ".calibre%d", n);
        }
        for (const char* tag : kTags) {
            if (!appendRule(parent, tag)) {
                goto Done;
            }
        }
    }
    for (int n = 1; n <= 20; n++) {
        char parent[32];
        snprintf(parent, sizeof(parent), ".calibre_%d", n);
        for (const char* tag : kTags) {
            if (!appendRule(parent, tag)) {
                goto Done;
            }
        }
    }
Done:
    *dst = '\0';
    inited = true;
    return storage;
}

// Legacy cnepub class names (lowercase bodycontent) on some publishers.
static const char* kEbookForceCjkOverrideSelectors =
    "p.noindent-bodycontent-1-fangsong,\n"
    "p.bodycontent-1-fangsong,\n"
    "p.bodycontent-2-fangsong,\n"
    "p.bodycontent-1-fangsong-top,\n"
    "p.noindent-bodycontent-1-fangsong-top,\n"
    "p.bodycontent-1-top,\n"
    "p.noindent-bodycontent-1-top,\n"
    "p.bodycontent-1-fangsong-top1,\n"
    "p.bodycontent-2-fangsong-top,\n"
    "p.hang-bodycontent-1-fangsong,\n"
    "p.noindent-bodycontent,\n"
    "p.bodycontent,\n"
    "p.songti,\n"
    "span.songti";

static TempStr BuildEbookForceCjkOverrideCss() {
    TempStr cjk = CssFontFamilyToken(gCjkFontFamily);
    return str::FormatTemp(
        R"(%s {
  font-family: %s, "思源宋体", "Source Han Serif", "Noto Serif CJK SC", "NSimSun", "SimSun", "宋体" !important;
})",
        kEbookForceCjkOverrideSelectors, cjk);
}

static TempStr BuildEbookForceBilingualFontCss(TempStr latin, TempStr cjk) {
    TempStr latinCss = str::FormatTemp(
        R"(%s {
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman" !important;
  line-height: 1.45 !important;
}
p {
  margin: 0.35em 0;
}
)",
        kEbookForceFontSelectors, latin);
    TempStr cjkCnepubCss = str::FormatTemp(
        R"(%s {
  font-family: %s, "思源宋体", "Source Han Serif", "Noto Serif CJK SC", "NSimSun", "SimSun", "宋体" !important;
  line-height: 1.45 !important;
}
)",
        kEbookForceCjkCnepubSelectors, cjk);
    TempStr cjkBodyContentCss = BuildBodyContentCjkCss(cjk);
    TempStr cjkOverride = BuildEbookForceCjkOverrideCss();
    TempStr latinCalibreClassCss = str::FormatTemp(
        R"(%s {
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman" !important;
  line-height: 1.45 !important;
}
)",
        BuildCalibreClassLatinSelectors(), latin);
    TempStr latinEnCss = str::FormatTemp(
        R"(%s {
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman" !important;
  line-height: 1.45 !important;
}
)",
        kEbookForceLatinEnSelectors, latin);
    TempStr latinCalibreCss = str::FormatTemp(
        R"(%s {
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman" !important;
  line-height: 1.45 !important;
}
)",
        kEbookForceLatinCalibreSelectors, latin);
    TempStr latinCalibreDescCss = str::FormatTemp(
        R"(%s {
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman" !important;
  line-height: 1.45 !important;
}
)",
        BuildCalibreDescendantLatinSelectors(), latin);
    TempStr latinInlineCss = str::FormatTemp(
        R"(%s {
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman" !important;
}
)",
        kEbookForceLatinZhCnInlineSelectors, latin);
    TempStr css = str::JoinTemp(latinCss, "\n", cjkCnepubCss);
    css = str::JoinTemp(css, "\n", cjkBodyContentCss);
    css = str::JoinTemp(css, "\n", cjkOverride);
    css = str::JoinTemp(css, "\n", latinCalibreClassCss);
    css = str::JoinTemp(css, "\n", latinEnCss);
    css = str::JoinTemp(css, "\n", latinCalibreCss);
    css = str::JoinTemp(css, "\n", latinCalibreDescCss);
    return str::JoinTemp(css, "\n", latinInlineCss);
}

TempStr BuildEbookForceFontSizeCss(int displayDpi) {
    float pt = GetEbookReaderFontSizePt();
    if (pt <= 0.f) {
        return nullptr;
    }
    ReportIf(displayDpi < 70);
    // MuPDF layout coordinates are dpi-scaled (see EngineMupdf::DpiScale). CSS pt/px
    // values are layout units, not physical points, so match fz_layout_document.
    float layoutPx = pt * (float)displayDpi / 96.f;
    return str::FormatTemp(
        R"(%s {
  font-size: %.1fpx !important;
}
)",
        kEbookForceFontSelectors, layoutPx);
}

TempStr BuildEbookForceFontCss(EbookTypographyKind typographyKind) {
    TempStr latin = CssFontFamilyToken(gLatinFontFamily);
    TempStr cjk = CssFontFamilyToken(gCjkFontFamily);
    const char* selectors = kEbookForceFontSelectors;
    if (typographyKind == EbookTypographyKind::Bilingual) {
        return BuildEbookForceBilingualFontCss(latin, cjk);
    }
    if (typographyKind == EbookTypographyKind::Latin) {
        // Latin books: user Latin face only in CSS. Han glyphs are filled in via
        // load_windows_fallback_font (sumatra_set_ebook_font_config). Do not list the reader CJK
        // font here: fonts like WenKai include Latin glyphs and MuPDF would draw English in Kai
        // for mixed or misclassified paragraphs.
        TempStr latinCss = str::FormatTemp(
            R"(%s {
  font-family: %s, Georgia, Charter, "Palatino Linotype", "Times New Roman" !important;
  line-height: 1.45 !important;
}
p {
  margin: 0.35em 0;
}
)",
            selectors, latin);
        TempStr cjkOverride = BuildEbookForceCjkOverrideCss();
        return str::JoinTemp(latinCss, "\n", cjkOverride);
    }
    // Pure CJK books: CJK face first so the primary font loads for body text.
    return str::FormatTemp(
        R"(%s {
  font-family: %s, %s, "思源宋体", "Source Han Serif", "Noto Serif CJK SC", Georgia, "NSimSun", "SimSun", "宋体", serif !important;
  line-height: 1.45 !important;
}
p {
  margin: 0.35em 0;
}
)",
        selectors, cjk, latin);
}
