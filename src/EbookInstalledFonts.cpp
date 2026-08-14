/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "EbookInstalledFonts.h"

struct FontEnumCtx {
    Vec<char*>* families = nullptr;
    bool wantCjk = false;
};

struct CjkFontMenuLabelMap {
    const char* gdiName;
    const char* zhLabel;
};

static bool HasCjkScriptInName(const WCHAR* name) {
    if (!name) {
        return false;
    }
    for (const WCHAR* p = name; *p; p++) {
        WCHAR c = *p;
        if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xAC00 && c <= 0xD7AF) ||
            (c >= 0x3040 && c <= 0x30FF) || (c >= 0xF900 && c <= 0xFAFF)) {
            return true;
        }
    }
    return false;
}

static bool WStrStartsWithI(const WCHAR* s, const WCHAR* prefix) {
    if (!s || !prefix) {
        return false;
    }
    while (*prefix) {
        if (towlower(*s) != towlower(*prefix)) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

static bool IsJapaneseOrKoreanFontFamily(const WCHAR* faceName) {
    if (!faceName) {
        return false;
    }
    static const WCHAR* kJpKrPrefixes[] = {
        L"Malgun Gothic", L"MS Gothic", L"MS UI Gothic", L"MS PGothic",   L"MS Mincho",
        L"Yu Gothic",     L"Yu Mincho", L"Meiryo",       L"MS UI Gothic", nullptr,
    };
    for (int i = 0; kJpKrPrefixes[i]; i++) {
        if (str::EqI(faceName, kJpKrPrefixes[i]) || WStrStartsWithI(faceName, kJpKrPrefixes[i])) {
            return true;
        }
    }
    return false;
}

static bool IsKnownChineseFamilyName(const WCHAR* faceName) {
    if (!faceName) {
        return false;
    }
    static const WCHAR* kNames[] = {
        L"Microsoft YaHei",
        L"DengXian",
        L"SimSun",
        L"NSimSun",
        L"SimHei",
        L"KaiTi",
        L"FangSong",
        L"STSong",
        L"STKaiti",
        L"STXihei",
        L"STFangsong",
        L"STHeiti",
        L"Microsoft JhengHei",
        L"PMingLiU",
        L"MingLiU",
        L"Noto Sans SC",
        L"Noto Serif SC",
        L"Source Han Sans SC",
        L"Source Han Serif SC",
        L"Source Han Sans",
        L"Source Han Serif",
        nullptr,
    };
    for (int i = 0; kNames[i]; i++) {
        if (str::EqI(faceName, kNames[i]) || WStrStartsWithI(faceName, kNames[i])) {
            return true;
        }
    }
    return false;
}

static bool IsUiOrSpecialPurposeFontFamily(const WCHAR* faceName, const char* name) {
    if (!faceName || !name) {
        return false;
    }
    // UI-tuned faces (e.g. Microsoft YaHei UI, Segoe UI, Yu Gothic UI).
    if (wcsstr(faceName, L" UI")) {
        return true;
    }
    if (str::StartsWithI(name, "Bahnschrift")) {
        return true;
    }
    static const char* kSkipExact[] = {
        "Segoe UI",
        "Cambria Math",
        "Segoe Print",
        "Segoe Script",
        "Segoe Marker",
        "Segoe Fluent Icons",
        "Segoe UI Emoji",
        "Segoe UI Symbol",
        "Segoe MDL2 Assets",
        "HoloLens MDL2 Assets",
        "Microsoft Sans Serif",
        "Ink Free",
        "Gadugi",
        "Marlett",
        "Webdings",
        "Wingdings",
        "Wingdings 2",
        "Wingdings 3",
        "Symbol",
        "MT Extra",
        "MS Reference Specialty",
        "Bookshelf Symbol 7",
        "Fixedsys",
        "Modern",
        "Roman",
        "Script",
        "Small Fonts",
        "System",
        "Terminal",
        "Global Monospace",
        "Global Sans Serif",
        "Global Serif",
        "Global User Interface",
        nullptr,
    };
    for (int i = 0; kSkipExact[i]; i++) {
        if (str::EqI(name, kSkipExact[i])) {
            return true;
        }
    }
    static const char* kSkipSubstrings[] = {
        "Emoji", "MDL2", "HoloLens", "Math", "Icons", "Symbol", "Wingdings", "Webdings", "OCR ", nullptr,
    };
    for (int i = 0; kSkipSubstrings[i]; i++) {
        if (str::Find(name, kSkipSubstrings[i])) {
            return true;
        }
    }
    if (str::EndsWithI(name, " Symbols")) {
        return true;
    }
    return false;
}

static bool ShouldSkipFontFamily(const WCHAR* faceName, BYTE charset) {
    if (!faceName || !faceName[0]) {
        return true;
    }
    if (faceName[0] == L'@') {
        return true;
    }
    if (charset == SYMBOL_CHARSET) {
        return true;
    }
    TempStr name = ToUtf8Temp(faceName);
    if (!name) {
        return true;
    }
    if (IsUiOrSpecialPurposeFontFamily(faceName, name)) {
        return true;
    }
    return false;
}

static bool ClassifyAsChineseMenuFont(const WCHAR* faceName, BYTE charset) {
    if (ShouldSkipFontFamily(faceName, charset)) {
        return false;
    }
    if (IsJapaneseOrKoreanFontFamily(faceName)) {
        return false;
    }
    if (HasCjkScriptInName(faceName)) {
        return true;
    }
    if (charset == GB2312_CHARSET || charset == CHINESEBIG5_CHARSET) {
        return true;
    }
    return IsKnownChineseFamilyName(faceName);
}

static bool ClassifyAsLatinFont(const WCHAR* faceName, BYTE charset) {
    if (ShouldSkipFontFamily(faceName, charset)) {
        return false;
    }
    if (ClassifyAsChineseMenuFont(faceName, charset)) {
        return false;
    }
    if (IsJapaneseOrKoreanFontFamily(faceName)) {
        return false;
    }
    if (charset == SHIFTJIS_CHARSET || charset == HANGUL_CHARSET || charset == JOHAB_CHARSET) {
        return false;
    }
    return true;
}

static bool FamilyListContainsI(Vec<char*>* families, const char* family) {
    if (!families || !family) {
        return false;
    }
    for (char* existing : *families) {
        if (str::EqI(existing, family)) {
            return true;
        }
    }
    return false;
}

static void AppendUniqueFamily(Vec<char*>* families, const char* family) {
    if (!families || !family || !family[0] || FamilyListContainsI(families, family)) {
        return;
    }
    families->Append(str::Dup(family));
}

static int CALLBACK EnumInstalledFontFamExProc(ENUMLOGFONTEXW* elf, NEWTEXTMETRICEXW* /*ntm*/, DWORD fontType,
                                               LPARAM lParam) {
    auto* ctx = (FontEnumCtx*)lParam;
    if (!ctx || !ctx->families) {
        return 1;
    }
    if (!(fontType & TRUETYPE_FONTTYPE)) {
        return 1;
    }
    const WCHAR* face = elf->elfLogFont.lfFaceName;
    BYTE charset = elf->elfLogFont.lfCharSet;
    if (ShouldSkipFontFamily(face, charset)) {
        return 1;
    }
    if (ctx->wantCjk) {
        if (!ClassifyAsChineseMenuFont(face, charset)) {
            return 1;
        }
    } else if (!ClassifyAsLatinFont(face, charset)) {
        return 1;
    }
    TempStr utf8 = ToUtf8Temp(face);
    if (!utf8) {
        return 1;
    }
    AppendUniqueFamily(ctx->families, utf8);
    return 1;
}

static void CollectInstalledFontFamilies(bool wantCjk, Vec<char*>* families) {
    if (!families) {
        return;
    }
    HDC hdc = GetDC(nullptr);
    if (!hdc) {
        return;
    }
    FontEnumCtx ctx;
    ctx.families = families;
    ctx.wantCjk = wantCjk;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfFaceName[0] = L'\0';
    lf.lfPitchAndFamily = 0;
    EnumFontFamiliesExW(hdc, &lf, (FONTENUMPROCW)EnumInstalledFontFamExProc, (LPARAM)&ctx, 0);
    ReleaseDC(nullptr, hdc);
}

void CollectInstalledLatinFontFamilies(Vec<char*>* families) {
    CollectInstalledFontFamilies(false, families);
}

void CollectInstalledCjkFontFamilies(Vec<char*>* families) {
    CollectInstalledFontFamilies(true, families);
}

static TempStr LabelFromPrefix(const char* family, const char* gdiPrefix, const char* zhPrefix) {
    size_t prefixLen = strlen(gdiPrefix);
    if (str::EqI(family, gdiPrefix)) {
        return str::DupTemp(zhPrefix);
    }
    if (str::StartsWithI(family, gdiPrefix) && family[prefixLen] == ' ') {
        return str::FormatTemp("%s%s", zhPrefix, family + prefixLen);
    }
    return nullptr;
}

const char* GetInstalledCjkFontMenuLabel(const char* family) {
    if (!family || !family[0]) {
        return family;
    }
    if (HasCjkScriptInName(ToWStrTemp(family))) {
        return family;
    }

    TempStr mapped =
        LabelFromPrefix(family, "Microsoft YaHei UI", "\xe5\xbe\xae\xe8\xbd\xaf\xe9\x9b\x85\xe9\xbb\x91 UI");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "Microsoft YaHei", "\xe5\xbe\xae\xe8\xbd\xaf\xe9\x9b\x85\xe9\xbb\x91");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "Microsoft JhengHei UI",
                             "\xe5\xbe\xae\xe8\xbd\xaf\xe6\xad\xa3\xe9\xbb\x91\xe9\xab\x94 UI");
    if (mapped) {
        return mapped;
    }
    mapped =
        LabelFromPrefix(family, "Microsoft JhengHei", "\xe5\xbe\xae\xe8\xbd\xaf\xe6\xad\xa3\xe9\xbb\x91\xe9\xab\x94");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "DengXian", "\xe7\xad\x89\xe7\xba\xbf");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "SimSun-ExtB", "\xe5\xae\x8b\xe4\xbd\x93-ExtB");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "NSimSun", "\xe6\x96\xb0\xe5\xae\x8b\xe4\xbd\x93");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "SimSun", "\xe5\xae\x8b\xe4\xbd\x93");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "SimHei", "\xe9\xbb\x91\xe4\xbd\x93");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "KaiTi", "\xe6\xa5\xb7\xe4\xbd\x93");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "FangSong", "\xe4\xbb\xbf\xe5\xae\x8b");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "Noto Serif SC", "\xe6\x80\x9d\xe6\xba\x90\xe5\xae\x8b\xe4\xbd\x93");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "Noto Sans SC", "\xe6\x80\x9d\xe6\xba\x90\xe9\xbb\x91\xe4\xbd\x93");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "Source Han Serif SC", "\xe6\x80\x9d\xe6\xba\x90\xe5\xae\x8b\xe4\xbd\x93");
    if (mapped) {
        return mapped;
    }
    mapped = LabelFromPrefix(family, "Source Han Sans SC", "\xe6\x80\x9d\xe6\xba\x90\xe9\xbb\x91\xe4\xbd\x93");
    if (mapped) {
        return mapped;
    }

    static const CjkFontMenuLabelMap kExact[] = {
        {"STSong", "\xe5\x8d\x8e\xe6\x96\x87\xe5\xae\x8b\xe4\xbd\x93"},
        {"STKaiti", "\xe5\x8d\x8e\xe6\x96\x87\xe6\xa5\xb7\xe4\xbd\x93"},
        {"STXihei", "\xe5\x8d\x8e\xe6\x96\x87\xe7\xbb\x86\xe9\xbb\x91"},
        {"STFangsong", "\xe5\x8d\x8e\xe6\x96\x87\xe4\xbb\xbf\xe5\xae\x8b"},
        {"STHeiti", "\xe5\x8d\x8e\xe6\x96\x87\xe9\xbb\x91\xe4\xbd\x93"},
        {"PMingLiU", "\xe6\x96\xb0\xe7\xb4\xb0\xe6\x98\x8e\xe9\xab\x94"},
        {"MingLiU", "\xe7\xb4\xb0\xe6\x98\x8e\xe9\xab\x94"},
        {"MingLiU-ExtB", "\xe7\xb4\xb0\xe6\x98\x8e\xe9\xab\x94-ExtB"},
    };
    for (size_t i = 0; i < dimof(kExact); i++) {
        if (str::EqI(family, kExact[i].gdiName)) {
            return kExact[i].zhLabel;
        }
    }
    return family;
}
