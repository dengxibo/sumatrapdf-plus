/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct EpubPerfLog {
    char* path = nullptr;
    bool enabled = false;
};

void EpubPerfLogInit(const char* path);
void EpubPerfLogShutdown();
bool EpubPerfLogIsEnabled();

// Emit one JSON object per line. keys/values are simple scalars or quoted strings.
void EpubPerfLogEmit(const char* op, const char* kvPairs);

struct ScopedEpubPerfLog {
    const char* op = nullptr;
    LARGE_INTEGER start{};

    explicit ScopedEpubPerfLog(const char* opIn);
    ~ScopedEpubPerfLog();
};
