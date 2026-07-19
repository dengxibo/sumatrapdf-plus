/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

class EngineBase;

struct SearchSessionEntry {
    WCHAR* text = nullptr;
    bool matchCase = false;
    bool matchWholeWord = false;
    int scanStartPage = 1;
    bool partial = false;
    int pageLimit = 0;
    int scannedPageLimit = 0; // pages actually scanned (may be < pageLimit during progressive load)
    u32 textCacheGeneration = 0;
    // SearchSessionEntry is stored in our POD Vec, which relocates elements with
    // memcpy. An inline Vec is not relocatable because els can point at its own
    // small buffer. Keep the vector at a stable address instead.
    Vec<u64>* positions = nullptr;

    void Free();
};

// Per-document cache of full-document match positions for recent search terms.
struct SearchSessionCache {
    static const int kMaxEntries = 8;
    Vec<SearchSessionEntry> entries;

    void Clear();
    void Remove(const WCHAR* text, bool matchCase, bool matchWholeWord);
    bool Lookup(EngineBase* engine, const WCHAR* text, bool matchCase, bool matchWholeWord, int currentPageLimit,
                bool requireComplete, SearchSessionEntry* out);
    void Store(EngineBase* engine, const WCHAR* text, bool matchCase, bool matchWholeWord, int scanStartPage,
               bool partial, int pageLimit, int scannedPageLimit, Vec<u64>& positions);
};
