/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Base dimensions of the Home landscape cards and the old page preview cache.
constexpr int kThumbnailDx = 212;
constexpr int kThumbnailDy = 150;
constexpr int kPageThumbnailDx = 384;
constexpr int kPageThumbnailDy = 576;

RenderedBitmap* LoadThumbnail(FileState* fs);
bool HasThumbnail(FileState* fs);
void SetThumbnail(FileState* fs, RenderedBitmap* bmp);
void SaveThumbnail(FileState* fs);
void RemoveThumbnail(FileState* fs);
void InvalidateLoadedThumbnails();

TempStr GetThumbnailCacheDirTemp();
char* GetThumbnailPathTemp(const char* filePath);
void DeleteThumbnailForFile(const char* path);
void DeleteThumbnailCacheDirectory();
