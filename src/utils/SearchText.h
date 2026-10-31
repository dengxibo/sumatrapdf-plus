/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

static bool SearchTextIsWhitespace(int cp) {
    // Unicode White_Space, checked before any narrowing conversion. A Han
    // character such as 选 (U+9009) must not be mistaken for a tab (U+0009).
    return cp == 0x20 || (cp >= 0x09 && cp <= 0x0d) || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f ||
           cp == 0x3000;
}

static bool SearchTextIsHanQuery(const WCHAR* text) {
    bool hasHan = false;
    for (int i = 0; text && text[i]; i++) {
        int cp = text[i];
        if (cp >= 0xd800 && cp <= 0xdbff && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (text[++i] - 0xdc00);
        }
        if (SearchTextIsWhitespace(cp)) {
            continue;
        }
        bool han = cp == 0x3007 || (cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
                   (cp >= 0x20000 && cp <= 0x323af);
        if (!han) {
            return false;
        }
        hasHan = true;
    }
    return hasHan;
}
