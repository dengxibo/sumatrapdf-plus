/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "utils/BaseUtil.h"

struct fz_context;

struct EpubMetaFragment {
    char* uri = nullptr;
    int chapter = -1;
    int pageInChapter = 0;
    float x = 0;
    float y = 0;
    int pageNo = 0;
};

struct EpubMetaBookProfile {
    int pages = 0;
    int chapters = 0;
    int loadMs = 0;
    u32 cssHash = 0;
};

struct EpubMetaData {
    EpubMetaBookProfile profile;
    Vec<int> chapterStartPage;
    Vec<EpubMetaFragment> fragments;
    Vec<int> tocResolvedPageNo; // parallel to outline walk order

    ~EpubMetaData();
    void Clear();
};

// Same cache key as .mupdfaccel but .epubmeta extension.
TempStr GetEbookMetaPathTemp(fz_context* ctx, const char* filePath);

bool LoadEpubMeta(fz_context* ctx, const char* filePath, EpubMetaData* out);
bool SaveEpubMeta(fz_context* ctx, const char* filePath, const EpubMetaData& data);

int EpubMetaLookupFragmentPage(const EpubMetaData& meta, const char* uri);
int EpubMetaLookupTocPage(const EpubMetaData& meta, int outlineIndex);
