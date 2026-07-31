/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "EbookInstalledFonts.h"

struct FontEnumCtx {
    Vec<char*>* families = nullptr;
    bool wantCjk = false;
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

static bool IsCjkCharset(BYTE charset) {
    return charset == GB2312_CHARSET || charset == CHINESEBIG5_CHARSET || charset == SHIFTJIS_CHARSET ||
           charset == HANGUL_CHARSET || charset == JOHAB_CHARSET;
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

static bool IsKnownCjkFamilyName(const WCHAR* faceName) {
    if (!faceName) {
        return false;
    }
    static const WCHAR* kNames[] = {
        L"Microsoft YaHei",
        L"Microsoft YaHei UI",
        L"SimSun",
        L"NSimSun",
        L"SimHei",
        L"KaiTi",
        L"FangSong",
        L"STSong",
        L"STKaiti",
        L"STXihei",
        L"STFangsong",
        L"Microsoft JhengHei",
        L"PMingLiU",
        L"MingLiU",
        L"Malgun Gothic",
        L"MS Gothic",
        L"MS Mincho",
        L"Yu Gothic",
        L"Noto Sans CJK",
        L"Noto Serif CJK",
        L"Source Han Sans",
        L"Source Han Serif",
        nullptr,
    };
    for (int i = 0; kNames[i]; i++) {
        if (str::EqI(faceName, kNames[i]) || WStrStartsWithI(faceName, kNames[i])) {
            return true;
        }
    }
    return HasCjkScriptInName(faceName);
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
    static const char* kSkipExact[] = {
        "Marlett",
        "Webdings",
        "Wingdings",
        "Wingdings 2",
        "Wingdings 3",
        "Symbol",
        "MT Extra",
        "MS Reference Specialty",
        "Segoe MDL2 Assets",
        "HoloLens MDL2 Assets",
        "Segoe Fluent Icons",
        "Segoe UI Emoji",
        "Segoe UI Symbol",
        "Bahnschrift",
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
    if (str::StartsWithI(name, "OCR ") || str::EndsWithI(name, " Symbols")) {
        return true;
    }
    if (str::Find(name, "Emoji") || str::Find(name, "MDL2") || str::Find(name, "HoloLens")) {
        return true;
    }
    return false;
}

static bool ClassifyAsCjkFont(const WCHAR* faceName, BYTE charset) {
    if (IsCjkCharset(charset)) {
        return true;
    }
    return IsKnownCjkFamilyName(faceName);
}

static bool ClassifyAsLatinFont(const WCHAR* faceName, BYTE charset) {
    if (ShouldSkipFontFamily(faceName, charset)) {
        return false;
    }
    if (ClassifyAsCjkFont(faceName, charset)) {
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
    bool isCjk = ClassifyAsCjkFont(face, charset);
    if (ctx->wantCjk) {
        if (!isCjk) {
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
