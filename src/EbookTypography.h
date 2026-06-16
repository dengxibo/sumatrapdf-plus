/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

enum class EbookTypographyKind {
    Cjk,
    Latin,
    Bilingual,
};

EbookTypographyKind DetectEbookTypographyKind(const char* filePath, const char* nameHint);
void SetEbookTypographyKind(EbookTypographyKind kind);
EbookTypographyKind GetEbookTypographyKind();

// true only for pure CJK books (not bilingual).
bool EbookNeedsCjkTypography(const char* filePath, const char* nameHint);
bool EbookUsesCjkTypography();
