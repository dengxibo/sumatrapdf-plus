/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "SearchSessionCache.h"

void SearchSessionEntry::Free() {
    str::FreePtr(&text);
    delete positions;
    positions = nullptr;
}

void SearchSessionCache::Clear() {
    for (int i = 0; i < (int)entries.size(); i++) {
        entries[i].Free();
    }
    entries.Reset();
}

void SearchSessionCache::Remove(const WCHAR* text, bool matchCase, bool matchWholeWord) {
    if (!text) {
        return;
    }
    for (int i = (int)entries.size() - 1; i >= 0; i--) {
        SearchSessionEntry& e = entries[i];
        if (e.text && str::Eq(e.text, text) && e.matchCase == matchCase && e.matchWholeWord == matchWholeWord) {
            e.Free();
            entries.RemoveAt(i);
        }
    }
}

bool SearchSessionCache::Lookup(EngineBase* engine, const WCHAR* text, bool matchCase, bool matchWholeWord,
                                int currentPageLimit, bool requireComplete, SearchSessionEntry* out) {
    if (!engine || !text || !out) {
        return false;
    }
    u32 gen = engine->GetTextCacheGeneration();
    for (int i = 0; i < (int)entries.size(); i++) {
        SearchSessionEntry& e = entries[i];
        if (e.textCacheGeneration != gen) {
            continue;
        }
        if (!e.text || !str::Eq(e.text, text)) {
            continue;
        }
        if (e.matchCase != matchCase || e.matchWholeWord != matchWholeWord) {
            continue;
        }
        if (e.partial) {
            if (requireComplete) {
                continue;
            }
            if (currentPageLimit != e.pageLimit) {
                continue;
            }
        } else if (e.scannedPageLimit < currentPageLimit) {
            continue;
        }
        *out = e;
        out->text = nullptr; // caller must not free; points into cache
        return true;
    }
    return false;
}

void SearchSessionCache::Store(EngineBase* engine, const WCHAR* text, bool matchCase, bool matchWholeWord,
                               int scanStartPage, bool partial, int pageLimit, int scannedPageLimit,
                               int continuationPage, Vec<u64>& positions) {
    if (!engine || !text) {
        return;
    }
    u32 gen = engine->GetTextCacheGeneration();
    for (int i = 0; i < (int)entries.size(); i++) {
        SearchSessionEntry& e = entries[i];
        if (e.text && str::Eq(e.text, text) && e.matchCase == matchCase && e.matchWholeWord == matchWholeWord) {
            e.scanStartPage = scanStartPage;
            e.partial = partial;
            e.pageLimit = pageLimit;
            e.scannedPageLimit = scannedPageLimit;
            e.continuationPage = continuationPage;
            e.textCacheGeneration = gen;
            delete e.positions;
            e.positions = new Vec<u64>(positions);
            return;
        }
    }
    while ((int)entries.size() >= kMaxEntries) {
        entries[0].Free();
        entries.RemoveAt(0);
    }
    SearchSessionEntry e;
    e.text = str::Dup(text);
    e.matchCase = matchCase;
    e.matchWholeWord = matchWholeWord;
    e.scanStartPage = scanStartPage;
    e.partial = partial;
    e.pageLimit = pageLimit;
    e.scannedPageLimit = scannedPageLimit;
    e.continuationPage = continuationPage;
    e.textCacheGeneration = gen;
    e.positions = new Vec<u64>(positions);
    entries.Append(e);
}
