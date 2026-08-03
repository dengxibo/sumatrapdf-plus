/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "EbookTypography.h"

static void CountHtmlLetters(const char* s, size_t len, int* cjkOut, int* latinOut) {
    int cjk = 0;
    int latin = 0;
    bool inTag = false;
    if (!s) {
        *cjkOut = 0;
        *latinOut = 0;
        return;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '<') {
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (inTag) {
            continue;
        }
        if (c < 0x80) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                latin++;
            }
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            i += 1;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            unsigned char c1 = (unsigned char)s[i + 1];
            unsigned char c2 = (unsigned char)s[i + 2];
            uint cp = ((uint)(c & 0x0F) << 12) | ((uint)(c1 & 0x3F) << 6) | (uint)(c2 & 0x3F);
            if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF)) {
                cjk++;
            }
            i += 2;
        } else if ((c & 0xF8) == 0xF0) {
            i += 3;
        }
        if (cjk + latin >= 600) {
            break;
        }
    }
    *cjkOut = cjk;
    *latinOut = latin;
}

static const char* FindHtmlBodyStart(const char* html, size_t len) {
    for (size_t i = 0; i + 5 < len; i++) {
        if (html[i] != '<') {
            continue;
        }
        if (str::EqNI(html + i, "<body", 5)) {
            return html + i;
        }
    }
    return html;
}

static EbookTypographyKind ClassifyHtmlLetters(int cjk, int latin) {
    if (cjk >= 12 && latin >= 12 && cjk <= latin * 6 && latin <= cjk * 6) {
        return EbookTypographyKind::Bilingual;
    }
    if (cjk >= 8 && cjk * 2 >= latin) {
        return EbookTypographyKind::Cjk;
    }
    if (latin >= 40 && latin > cjk * 3) {
        return EbookTypographyKind::Latin;
    }
    return cjk >= latin ? EbookTypographyKind::Cjk : EbookTypographyKind::Latin;
}

EbookTypographyKind DetectHtmlTypographyKind(const ByteSlice& html) {
    size_t n = html.size();
    if (n > 2 * 1024 * 1024) {
        n = 2 * 1024 * 1024;
    }
    const char* data = (const char*)html.data();
    const char* start = FindHtmlBodyStart(data, n);
    size_t off = (size_t)(start - data);
    int cjk = 0;
    int latin = 0;
    CountHtmlLetters(start, n - off, &cjk, &latin);
    return ClassifyHtmlLetters(cjk, latin);
}

EbookTypographyKind DetectMobiReaderTypography(const ByteSlice& html) {
    EbookTypographyKind kind = DetectHtmlTypographyKind(html);
    if (kind != EbookTypographyKind::Latin) {
        return kind;
    }
    size_t n = html.size();
    if (n > 2 * 1024 * 1024) {
        n = 2 * 1024 * 1024;
    }
    const char* data = (const char*)html.data();
    const char* body = FindHtmlBodyStart(data, n);
    int cjk = 0;
    int latin = 0;
    CountHtmlLetters(body, n - (size_t)(body - data), &cjk, &latin);
    if (cjk >= 8) {
        EbookTypographyKind rekind = ClassifyHtmlLetters(cjk, latin);
        // Chinese MOBI files often have enough English metadata/brand names (e.g. "Facebook")
        // in the head or front matter to trip the Latin heuristic; never treat them as Latin.
        if (rekind == EbookTypographyKind::Latin) {
            rekind = EbookTypographyKind::Cjk;
        }
        return rekind;
    }
    return kind;
}

static EbookTypographyKind gEbookTypographyKind = EbookTypographyKind::Cjk;
static bool gEbookReaderStyleMobi = false;

void SetEbookTypographyKind(EbookTypographyKind kind) {
    gEbookTypographyKind = kind;
}

EbookTypographyKind GetEbookTypographyKind() {
    return gEbookTypographyKind;
}

void SetEbookReaderStyleMobi(bool readerStyle) {
    gEbookReaderStyleMobi = readerStyle;
}

bool EbookReaderStyleMobi() {
    return gEbookReaderStyleMobi;
}

bool EbookUsesCjkTypography() {
    return gEbookTypographyKind == EbookTypographyKind::Cjk || gEbookTypographyKind == EbookTypographyKind::Bilingual;
}
