/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/CryptoUtil.h"
#include "utils/FileUtil.h"

#include "AppTools.h"
#include "EpubMeta.h"

#include "mupdf/fitz.h"

static TempStr GetEbookCacheKeyTemp(fz_context* ctx, const char* filePath) {
    if (!filePath || !*filePath) {
        return nullptr;
    }
    i64 fileSize = file::GetSize(filePath);
    if (fileSize <= 0) {
        return nullptr;
    }
    FILETIME ft = file::GetModificationTime(filePath);
    u32 cssHash = 0;
    const char* userCss = ctx ? fz_user_css(ctx) : nullptr;
    if (userCss) {
        cssHash = MurmurHash2(userCss, str::Leni(userCss));
    }
    return str::FormatTemp("%s|%lld|%lu|%lu|%u", filePath, (long long)fileSize, (unsigned long)ft.dwHighDateTime,
                           (unsigned long)ft.dwLowDateTime, (unsigned)cssHash);
}

TempStr GetEbookMetaPathTemp(fz_context* ctx, const char* filePath) {
    TempStr key = GetEbookCacheKeyTemp(ctx, filePath);
    if (!key) {
        return nullptr;
    }
    u8 digest[16]{};
    CalcMD5Digest((const u8*)key, str::Leni(key), digest);
    AutoFreeStr hex = str::MemToHex(digest, dimof(digest));
    TempStr dir = GetPathInAppDataDirTemp("sumatrapdfcache");
    if (!dir) {
        return nullptr;
    }
    return path::JoinTemp(dir, str::JoinTemp(hex.Get(), ".epubmeta"));
}

EpubMetaData::~EpubMetaData() {
    Clear();
}

void EpubMetaData::Clear() {
    for (auto& f : fragments) {
        str::FreePtr(&f.uri);
    }
    fragments.Clear();
    chapterStartPage.Clear();
    tocResolvedPageNo.Clear();
    profile = {};
}

static bool ParseIntField(const char* line, const char* key, int* out) {
    size_t keyLen = str::Leni(key);
    if (!str::StartsWith(line, key) || line[keyLen] != '=') {
        return false;
    }
    return str::Parse(line + keyLen + 1, "%d", out);
}

bool LoadEpubMeta(fz_context* ctx, const char* filePath, EpubMetaData* out) {
    if (!out) {
        return false;
    }
    out->Clear();
    TempStr path = GetEbookMetaPathTemp(ctx, filePath);
    if (!path || !file::Exists(path)) {
        return false;
    }
    ByteSlice data = file::ReadFile(path);
    if (data.empty()) {
        return false;
    }
    AutoFreeStr text = str::Dup((const char*)data.data(), data.size());
    StrVec lines;
    Split(&lines, text, "\n");
    for (int i = 0; i < lines.Size(); i++) {
        const char* line = lines.At(i);
        if (str::StartsWith(line, "ch=")) {
            int start = 0;
            if (str::Parse(line + 3, "%d", &start)) {
                out->chapterStartPage.Append(start);
            }
            continue;
        }
        if (str::StartsWith(line, "frag=")) {
            // frag=uri\tch\tpic\tx\ty\tpage
            const char* p = line + 5;
            const char* tab = strchr(p, '\t');
            if (!tab) {
                continue;
            }
            EpubMetaFragment frag;
            frag.uri = str::Dup(p, tab - p);
            int ch = 0, pic = 0, page = 0;
            float x = 0, y = 0;
            if (str::Parse(tab + 1, "%d\t%d\t%f\t%f\t%d", &ch, &pic, &x, &y, &page)) {
                frag.chapter = ch;
                frag.pageInChapter = pic;
                frag.x = x;
                frag.y = y;
                frag.pageNo = page;
                out->fragments.Append(frag);
            } else {
                str::FreePtr(&frag.uri);
            }
            continue;
        }
        if (str::StartsWith(line, "toc=")) {
            int idx = 0, page = 0;
            if (str::Parse(line + 4, "%d\t%d", &idx, &page)) {
                while (out->tocResolvedPageNo.Size() <= idx) {
                    out->tocResolvedPageNo.Append(0);
                }
                out->tocResolvedPageNo[idx] = page;
            }
            continue;
        }
        ParseIntField(line, "pages", &out->profile.pages);
        ParseIntField(line, "chapters", &out->profile.chapters);
        ParseIntField(line, "loadMs", &out->profile.loadMs);
        int css = 0;
        if (ParseIntField(line, "cssHash", &css)) {
            out->profile.cssHash = (u32)css;
        }
    }
    return out->profile.pages > 0 && out->chapterStartPage.Size() > 0;
}

bool SaveEpubMeta(fz_context* ctx, const char* filePath, const EpubMetaData& data) {
    TempStr path = GetEbookMetaPathTemp(ctx, filePath);
    if (!path) {
        return false;
    }
    if (!dir::CreateForFile(path)) {
        return false;
    }
    StrBuilder sb;
    sb.AppendFmt("v=1\n");
    sb.AppendFmt("pages=%d\n", data.profile.pages);
    sb.AppendFmt("chapters=%d\n", data.profile.chapters);
    sb.AppendFmt("loadMs=%d\n", data.profile.loadMs);
    sb.AppendFmt("cssHash=%u\n", data.profile.cssHash);
    for (int i = 0; i < data.chapterStartPage.Size(); i++) {
        sb.AppendFmt("ch=%d\n", data.chapterStartPage.At(i));
    }
    for (auto& f : data.fragments) {
        if (!f.uri) {
            continue;
        }
        sb.AppendFmt("frag=%s\t%d\t%d\t%.4f\t%.4f\t%d\n", f.uri, f.chapter, f.pageInChapter, f.x, f.y, f.pageNo);
    }
    for (int i = 0; i < data.tocResolvedPageNo.Size(); i++) {
        int page = data.tocResolvedPageNo.At(i);
        if (page > 0) {
            sb.AppendFmt("toc=%d\t%d\n", i, page);
        }
    }
    return file::WriteFile(path, sb.AsByteSlice());
}

int EpubMetaLookupFragmentPage(const EpubMetaData& meta, const char* uri) {
    if (!uri) {
        return 0;
    }
    for (auto& f : meta.fragments) {
        if (f.uri && str::Eq(f.uri, uri) && f.pageNo > 0) {
            return f.pageNo;
        }
    }
    return 0;
}

int EpubMetaLookupTocPage(const EpubMetaData& meta, int outlineIndex) {
    if (outlineIndex < 0 || outlineIndex >= meta.tocResolvedPageNo.Size()) {
        return 0;
    }
    return meta.tocResolvedPageNo.At(outlineIndex);
}
