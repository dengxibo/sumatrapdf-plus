/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

enum class EbookTypographyKind {
    Cjk,
    Latin,
    Bilingual,
};

EbookTypographyKind DetectEbookTypographyKind(const char* filePath, const char* nameHint);
EbookTypographyKind DetectHtmlTypographyKind(const ByteSlice& html);
EbookTypographyKind DetectMobiReaderTypography(const ByteSlice& html);
void SetEbookTypographyKind(EbookTypographyKind kind);
EbookTypographyKind GetEbookTypographyKind();

// Reader-styled MOBI/AZW (HtmlFormatter path); cleared for EPUB/MuPDF and other engines.
void SetEbookReaderStyleMobi(bool readerStyle);
bool EbookReaderStyleMobi();

// true only for pure CJK books (not bilingual).
bool EbookNeedsCjkTypography(const char* filePath, const char* nameHint);
bool EbookUsesCjkTypography();
