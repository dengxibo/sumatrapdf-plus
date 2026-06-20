/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/CryptoUtil.h"
#include "utils/FileUtil.h"
#include "utils/DirIter.h"
#include "utils/GdiPlusUtil.h"
#include "utils/WinUtil.h"

#include "Settings.h"
#include "DocController.h"
#include "FzImgReader.h"
#include "FileHistory.h"

#include "AppTools.h"
#include "FileThumbnails.h"

#include "utils/Log.h"

static bool IsThumbnailStale(const char* filePath, const char* thumbPath) {
    FILETIME bmpTime = file::GetModificationTime(thumbPath);
    FILETIME fileTime = file::GetModificationTime(filePath);
    return FileTimeDiffInSecs(fileTime, bmpTime) > 0;
}

char* GetThumbnailPathTemp(const char* filePath) {
    // create a fingerprint of a (normalized) path for the file name
    // I'd have liked to also include the file's last modification time
    // in the fingerprint (much quicker than hashing the entire file's
    // content), but that's too expensive for files on slow drives
    u8 digest[16]{};
    // TODO: why is this happening? Seen in crash reports e.g. 35043
    if (!filePath) {
        return nullptr;
    }
    TempStr path = str::DupTemp(filePath);
    if (path::HasVariableDriveLetter(path)) {
        // ignore the drive letter, if it might change
        path[0] = '?';
    }
    CalcMD5Digest((u8*)path, str::Leni(path), digest);
    AutoFreeStr fingerPrint = str::MemToHex(digest, dimof(digest));

    TempStr thumbsDir = GetThumbnailCacheDirTemp();
    if (!thumbsDir) {
        return nullptr;
    }

    TempStr res = path::JoinTemp(thumbsDir, str::JoinTemp(fingerPrint, ".png"));
    return res;
}

TempStr GetThumbnailCacheDirTemp() {
    TempStr thumbsDir = GetPathInAppDataDirTemp("sumatrapdfcache");
    return thumbsDir;
}

void DeleteThumbnailCacheDirectory() {
    TempStr thumbsDir = GetThumbnailCacheDirTemp();
    dir::RemoveAll(thumbsDir);
}

void DeleteThumbnailForFile(const char* filePath) {
    TempStr thumbPath = GetThumbnailPathTemp(filePath);
    bool ok = file::Delete(thumbPath);
    auto status = ok ? "ok" : "failed";
    logf("DeleteThumbnailForFile: file::Remove('%s') %s\n", thumbPath, status);
}

RenderedBitmap* LoadThumbnail(FileState* fs) {
    if (fs->thumbnail) {
        TempStr currentPath = GetThumbnailPathTemp(fs->filePath);
        if (currentPath && !IsThumbnailStale(fs->filePath, currentPath)) {
            return fs->thumbnail;
        }
        delete fs->thumbnail;
        fs->thumbnail = nullptr;
        fs->thumbnailBlankKnown = false;
        fs->thumbnailIsBlank = false;
    }
    TempStr bmpPath = GetThumbnailPathTemp(fs->filePath);
    if (!bmpPath) {
        return nullptr;
    }
    if (IsThumbnailStale(fs->filePath, bmpPath)) {
        return nullptr;
    }

    RenderedBitmap* bmp = LoadRenderedBitmap(bmpPath);
    if (!bmp || bmp->GetSize().IsEmpty()) {
        delete bmp;
        return nullptr;
    }

    fs->thumbnail = bmp;
    return fs->thumbnail;
}

bool HasThumbnail(FileState* fs) {
    // TODO: optimize, LoadThumbnail() is probably not necessary
    if (!fs->thumbnail && !LoadThumbnail(fs)) {
        return false;
    }

    TempStr bmpPath = GetThumbnailPathTemp(fs->filePath);
    if (!bmpPath) {
        return true;
    }
    if (IsThumbnailStale(fs->filePath, bmpPath)) {
        delete fs->thumbnail;
        fs->thumbnail = nullptr;
    }

    return fs->thumbnail != nullptr;
}

// takes ownership of bmp
void SetThumbnail(FileState* fs, RenderedBitmap* bmp) {
    ReportIf(bmp && bmp->GetSize().IsEmpty());
    if (!fs || !bmp || bmp->GetSize().IsEmpty()) {
        delete bmp;
        return;
    }
    delete fs->thumbnail;
    fs->thumbnail = bmp;
    SaveThumbnail(fs);
}

void SaveThumbnail(FileState* fs) {
    if (!fs->thumbnail) {
        return;
    }

    TempStr thumbnailPath = GetThumbnailPathTemp(fs->filePath);
    if (!thumbnailPath) {
        return;
    }
    if (!dir::CreateForFile(thumbnailPath)) {
        logf("SaveThumbnail: dir::CreateForFile('%s') failed, file path: '%s'\n", thumbnailPath, fs->filePath);
        ReportIfFast(true);
    }
    ReportIfFast(!str::EndsWithI(thumbnailPath, ".png"));

    RenderedBitmap* thumbnail = fs->thumbnail;
    if (!thumbnail) {
        return;
    }
    Gdiplus::Bitmap bmp(thumbnail->GetBitmap(), nullptr);
    CLSID tmpClsid = GetGdiPlusEncoderClsid(L"image/png");
    TempWStr pathW = ToWStrTemp(thumbnailPath);
    bmp.Save(pathW, &tmpClsid, nullptr);
}

void RemoveThumbnail(FileState* fs) {
    if (!HasThumbnail(fs)) {
        return;
    }

    DeleteThumbnailForFile(fs->filePath);
    delete fs->thumbnail;
    fs->thumbnail = nullptr;
}

void InvalidateLoadedThumbnails() {
    if (!gFileHistory.states) {
        return;
    }
    for (FileState* fs : *gFileHistory.states) {
        delete fs->thumbnail;
        fs->thumbnail = nullptr;
        fs->thumbnailBlankKnown = false;
        fs->thumbnailIsBlank = false;
    }
}
