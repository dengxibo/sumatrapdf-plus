/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/Timer.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "EpubPerfLog.h"

#include "utils/Log.h"

static EpubPerfLog gEpubPerfLog;

static bool EpubPerfLogWanted() {
    if (gEpubPerfLog.enabled) {
        return true;
    }
    if (gGlobalPrefs && gGlobalPrefs->eBookUI.epubPerfLog) {
        return true;
    }
    return false;
}

void EpubPerfLogInit(const char* path) {
    if (!path || !*path) {
        return;
    }
    str::ReplaceWithCopy(&gEpubPerfLog.path, path);
    gEpubPerfLog.enabled = true;
    dir::CreateForFile(path);
}

void EpubPerfLogShutdown() {
    str::FreePtr(&gEpubPerfLog.path);
    gEpubPerfLog.enabled = false;
}

bool EpubPerfLogIsEnabled() {
    return EpubPerfLogWanted();
}

void EpubPerfLogEmit(const char* op, const char* kvPairs) {
    if (!EpubPerfLogWanted() || !op) {
        return;
    }
    StrBuilder line;
    line.AppendFmt("{\"op\":\"%s\"", op);
    if (kvPairs && *kvPairs) {
        line.AppendChar(',');
        line.Append(kvPairs);
    }
    line.Append("}\n");
    if (gEpubPerfLog.path) {
        auto f = fopen(gEpubPerfLog.path, "a");
        if (f) {
            fwrite(line.Get(), 1, line.Size(), f);
            fclose(f);
        }
    }
    logf("%s", line.Get());
}

ScopedEpubPerfLog::ScopedEpubPerfLog(const char* opIn) {
    op = opIn;
    if (EpubPerfLogWanted()) {
        start = TimeGet();
    }
}

ScopedEpubPerfLog::~ScopedEpubPerfLog() {
    if (!op || !EpubPerfLogWanted()) {
        return;
    }
    double ms = TimeSinceInMs(start);
    TempStr kv = str::FormatTemp("\"ms\":%.2f", ms);
    EpubPerfLogEmit(op, kv);
}
