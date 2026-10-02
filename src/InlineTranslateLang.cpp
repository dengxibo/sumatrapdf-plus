/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "InlineTranslateLang.h"

// Release builds omit UtAssert.cpp. The tests below are debug-only.
#ifdef DEBUG
// must be last due to assert() over-write
#include "utils/UtAssert.h"
#endif

static bool IsAsciiLetter(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

RoughLang DetectSourceLangRough(const char* text) {
    if (str::IsEmptyOrWhiteSpace(text)) {
        return RoughLang::Unknown;
    }
    int han = 0;
    int latin = 0;
    int kana = 0;
    int hangul = 0;
    int other = 0;
    const u8* p = (const u8*)text;
    while (*p) {
        if (*p < 0x80) {
            if (IsAsciiLetter(*p)) {
                latin++;
            }
            p++;
            continue;
        }
        int n = 0;
        u32 cp = 0;
        if ((*p & 0xE0) == 0xC0 && p[1]) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
            n = 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            n = 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            n = 4;
        } else {
            p++;
            continue;
        }
        p += n;
        if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) {
            kana++;
        } else if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF)) {
            hangul++;
        } else if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                   (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF)) {
            han++;
        } else if ((cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x1E00 && cp <= 0x1EFF)) {
            latin++;
        } else {
            other++;
        }
    }
    // Script priority: kana / hangul before shared Han.
    if (kana > 0 && kana >= hangul && kana * 2 >= han) {
        return RoughLang::Ja;
    }
    if (hangul > 0 && hangul >= kana) {
        return RoughLang::Ko;
    }
    int letters = han + latin + kana + hangul;
    if (letters == 0) {
        return RoughLang::Unknown;
    }
    if (han * 2 >= letters) {
        return RoughLang::Zh;
    }
    if (latin * 3 >= letters * 2) {
        return RoughLang::En;
    }
    if (other > letters) {
        return RoughLang::Other;
    }
    return latin >= han ? RoughLang::En : RoughLang::Zh;
}

// Sumatra UI code → Volc/translate code. Unknown codes pass through.
static const char* gUiToTranslate =
    "cn\0zh\0"
    "tw\0zh-Hant\0"
    "zh\0zh\0"
    "zh-CN\0zh\0"
    "zh-TW\0zh-Hant\0"
    "zh-HK\0zh-Hant\0"
    "en\0en\0"
    "kr\0ko\0"
    "ko\0ko\0"
    "vn\0vi\0"
    "vi\0vi\0"
    "cz\0cs\0"
    "cs\0cs\0"
    "by\0be\0"
    "am\0hy\0"
    "dk\0da\0"
    "jp\0ja\0"
    "ja\0ja\0"
    "ca-xv\0ca\0";

const char* UiLangToTranslateCode(const char* uiLangCode) {
    if (str::IsEmptyOrWhiteSpace(uiLangCode)) {
        return "en";
    }
    int idx = seqstrings::StrToIdx(gUiToTranslate, uiLangCode);
    if (idx >= 0 && idx % 2 == 0) {
        return seqstrings::IdxToStr(gUiToTranslate, idx + 1);
    }
    if (str::StartsWithI(uiLangCode, "en-")) {
        return "en";
    }
    return uiLangCode;
}

static const TranslateTargetOption gTranslateTargets[] = {
    {"auto", "Auto (follow UI, flip when source matches)"},
    {"ui", "UI language"},
    {"zh", "Chinese (Simplified)"},
    {"zh-Hant", "Chinese (Traditional)"},
    {"en", "English"},
    {"ja", "Japanese"},
    {"ko", "Korean"},
    {"fr", "French"},
    {"de", "German"},
    {"es", "Spanish"},
    {"ru", "Russian"},
    {"pt", "Portuguese"},
    {"it", "Italian"},
    {"vi", "Vietnamese"},
    {"th", "Thai"},
    {"ar", "Arabic"},
    {"id", "Indonesian"},
    {"hi", "Hindi"},
    {"tr", "Turkish"},
    {"nl", "Dutch"},
    {"pl", "Polish"},
    {"sv", "Swedish"},
    {"uk", "Ukrainian"},
    {"ms", "Malay"},
};

const TranslateTargetOption* GetTranslateTargetOptions(int* countOut) {
    if (countOut) {
        *countOut = dimof(gTranslateTargets);
    }
    return gTranslateTargets;
}

int FindTranslateTargetOptionIndex(const char* mode) {
    if (str::IsEmptyOrWhiteSpace(mode)) {
        return 0; // auto
    }
    for (int i = 0; i < dimof(gTranslateTargets); i++) {
        if (str::EqI(gTranslateTargets[i].mode, mode)) {
            return i;
        }
    }
    // Legacy "zh" aliases
    if (str::EqI(mode, "zh-CN") || str::EqI(mode, "zh-Hans")) {
        return FindTranslateTargetOptionIndex("zh");
    }
    if (str::EqI(mode, "zh-TW") || str::EqI(mode, "zh-HK")) {
        return FindTranslateTargetOptionIndex("zh-Hant");
    }
    return 0;
}

const char* TranslateCodeDisplayName(const char* code) {
    if (str::IsEmptyOrWhiteSpace(code)) {
        return "the target language";
    }
    for (int i = 0; i < dimof(gTranslateTargets); i++) {
        if (str::EqI(gTranslateTargets[i].mode, code)) {
            // Skip auto/ui meta entries
            if (i >= 2) {
                return gTranslateTargets[i].label;
            }
        }
    }
    if (str::EqI(code, "zh") || str::EqI(code, "zh-CN")) {
        return "Chinese (Simplified)";
    }
    if (str::EqI(code, "zh-Hant") || str::EqI(code, "zh-TW")) {
        return "Chinese (Traditional)";
    }
    return code;
}

static bool TargetIsZh(const char* code) {
    return str::EqI(code, "zh") || str::EqI(code, "zh-CN") || str::EqI(code, "zh-Hans") || str::EqI(code, "zh-Hant") ||
           str::EqI(code, "zh-TW") || str::EqI(code, "zh-HK");
}

static bool TargetIsEn(const char* code) {
    return str::EqI(code, "en") || str::StartsWithI(code, "en-");
}

static bool TargetIsJa(const char* code) {
    return str::EqI(code, "ja") || str::EqI(code, "jp");
}

static bool TargetIsKo(const char* code) {
    return str::EqI(code, "ko") || str::EqI(code, "kr");
}

static const char* FlipWhenSourceMatches(RoughLang src, const char* target) {
    if (src == RoughLang::Zh && TargetIsZh(target)) {
        return "en";
    }
    if (src == RoughLang::En && TargetIsEn(target)) {
        return "zh";
    }
    if (src == RoughLang::Ja && TargetIsJa(target)) {
        return "en";
    }
    if (src == RoughLang::Ko && TargetIsKo(target)) {
        return "en";
    }
    return target;
}

const char* ResolveTranslateTarget(const char* text, const char* targetMode, const char* uiLangCode) {
    const char* mode = targetMode ? targetMode : "auto";
    const char* uiTarget = UiLangToTranslateCode(uiLangCode);

    if (str::EqI(mode, "ui")) {
        return uiTarget;
    }
    if (str::EqI(mode, "auto")) {
        return FlipWhenSourceMatches(DetectSourceLangRough(text), uiTarget);
    }

    // Explicit language code (including legacy zh/en).
    int idx = FindTranslateTargetOptionIndex(mode);
    if (idx >= 2) {
        return gTranslateTargets[idx].mode;
    }
    // Unknown mode: behave like auto
    return FlipWhenSourceMatches(DetectSourceLangRough(text), uiTarget);
}

void InlineTranslateLang_UnitTests() {
#ifdef DEBUG
    utassert(DetectSourceLangRough("Hello world") == RoughLang::En);
    utassert(DetectSourceLangRough("这是一段中文") == RoughLang::Zh);
    utassert(DetectSourceLangRough("こんにちは世界") == RoughLang::Ja);
    utassert(DetectSourceLangRough("안녕하세요") == RoughLang::Ko);

    utassert(str::Eq(UiLangToTranslateCode("cn"), "zh"));
    utassert(str::Eq(UiLangToTranslateCode("tw"), "zh-Hant"));
    utassert(str::Eq(UiLangToTranslateCode("en"), "en"));
    utassert(str::Eq(UiLangToTranslateCode("kr"), "ko"));
    utassert(str::Eq(UiLangToTranslateCode("vn"), "vi"));
    utassert(str::Eq(UiLangToTranslateCode("jp"), "ja"));

    utassert(str::Eq(ResolveTranslateTarget("Hello world", "auto", "en"), "zh"));
    utassert(str::Eq(ResolveTranslateTarget("这是一段中文", "auto", "cn"), "en"));
    utassert(str::Eq(ResolveTranslateTarget("这是一段中文", "auto", "en"), "en"));
    utassert(str::Eq(ResolveTranslateTarget("Hello world", "auto", "cn"), "zh"));
    utassert(str::Eq(ResolveTranslateTarget("こんにちは", "auto", "jp"), "en"));
    utassert(str::Eq(ResolveTranslateTarget("Hello", "ja", "en"), "ja"));
    utassert(str::Eq(ResolveTranslateTarget("你好", "ko", "cn"), "ko"));
    utassert(str::Eq(ResolveTranslateTarget("Hello world", "ui", "en"), "en"));
    utassert(str::Eq(ResolveTranslateTarget("Hello", "fr", "cn"), "fr"));
    utassert(FindTranslateTargetOptionIndex("ja") >= 2);
#endif
}
