/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"

#include "CharConv.h"

struct T2SPhrase {
    char* from = nullptr;
    char* to = nullptr;
    int fromLen = 0;
    int toLen = 0;
};

struct T2SChar {
    char* from = nullptr;
    char* to = nullptr;
    int fromLen = 0;
    int toLen = 0;
};

static Vec<T2SPhrase> gPhrases;
static Vec<T2SChar> gChars;
static bool gLoaded = false;
static bool gLoadFailed = false;
static LONG gLoadState = 0; // 0 = not started, 1 = loading, 2 = done

static int ComparePhraseByFromLenDesc(const void* a, const void* b) {
    const T2SPhrase* pa = (const T2SPhrase*)a;
    const T2SPhrase* pb = (const T2SPhrase*)b;
    if (pa->fromLen != pb->fromLen) {
        return pb->fromLen - pa->fromLen;
    }
    return strcmp(pa->from, pb->from);
}

static int CompareCharByFrom(const void* a, const void* b) {
    const T2SChar* ca = (const T2SChar*)a;
    const T2SChar* cb = (const T2SChar*)b;
    return strcmp(ca->from, cb->from);
}

static int CompareUtf8At(const char* p, int len, const T2SChar* e) {
    int n = len < e->fromLen ? len : e->fromLen;
    int cmp = strncmp(p, e->from, n);
    if (cmp != 0) {
        return cmp;
    }
    return len - e->fromLen;
}

static void FreeT2SData() {
    for (size_t i = 0; i < gPhrases.size(); i++) {
        T2SPhrase& p = gPhrases.at(i);
        str::Free(p.from);
        str::Free(p.to);
    }
    gPhrases.Reset();
    for (size_t i = 0; i < gChars.size(); i++) {
        T2SChar& c = gChars.at(i);
        str::Free(c.from);
        str::Free(c.to);
    }
    gChars.Reset();
}

static TempStr OpenccDataPathTemp(const char* fileName) {
    TempStr bundled = path::JoinTemp(GetPathInExeDirTemp("opencc"), fileName);
    if (file::Exists(bundled)) {
        return bundled;
    }
    TempStr dev = path::JoinTemp(GetSelfExeDirTemp(), "..\\..\\ext\\opencc", fileName);
    if (file::Exists(dev)) {
        return dev;
    }
    return bundled;
}

static bool ParseOpenccLine(char* line, char** keyOut, char** valOut) {
    if (str::IsEmpty(line) || line[0] == '#') {
        return false;
    }
    char* tab = strchr(line, '\t');
    if (!tab) {
        tab = strchr(line, ' ');
    }
    if (!tab) {
        return false;
    }
    *tab = 0;
    char* key = line;
    char* val = tab + 1;
    str::TrimWSInPlace(key, str::TrimOpt::Both);
    str::TrimWSInPlace(val, str::TrimOpt::Both);
    if (str::IsEmpty(key) || str::IsEmpty(val)) {
        return false;
    }
    char* sp = strchr(val, ' ');
    if (sp) {
        *sp = 0;
        str::TrimWSInPlace(val, str::TrimOpt::Both);
    }
    *keyOut = key;
    *valOut = val;
    return true;
}

static bool LoadOpenccFile(const char* path, bool isPhrase) {
    ByteSlice data = file::ReadFile(path);
    if (!data.data() || data.size() == 0) {
        return false;
    }
    char* buf = (char*)data.data();
    char* end = buf + data.size();
    char* line = buf;
    for (char* p = buf; p <= end; p++) {
        if (p == end || *p == '\n') {
            char saved = *p;
            *p = 0;
            char* s = line;
            if (str::StartsWith(s, UTF8_BOM)) {
                s += 3;
            }
            char* key = nullptr;
            char* val = nullptr;
            if (ParseOpenccLine(s, &key, &val)) {
                if (isPhrase) {
                    T2SPhrase e;
                    e.from = str::Dup(key);
                    e.to = str::Dup(val);
                    e.fromLen = str::Leni(e.from);
                    e.toLen = str::Leni(e.to);
                    gPhrases.Append(e);
                } else {
                    T2SChar e;
                    e.from = str::Dup(key);
                    e.to = str::Dup(val);
                    e.fromLen = str::Leni(e.from);
                    e.toLen = str::Leni(e.to);
                    gChars.Append(e);
                }
            }
            *p = saved;
            line = p + 1;
        }
    }
    free(data.data());
    return true;
}

static char* ConvertCharsOnly(const char* utf8);

static bool LoadTwPhrasesReverse(const char* path) {
    ByteSlice data = file::ReadFile(path);
    if (!data.data() || data.size() == 0) {
        return true; // optional
    }
    char* buf = (char*)data.data();
    char* end = buf + data.size();
    char* line = buf;
    for (char* p = buf; p <= end; p++) {
        if (p == end || *p == '\n') {
            char saved = *p;
            *p = 0;
            char* s = line;
            if (str::StartsWith(s, UTF8_BOM)) {
                s += 3;
            }
            char* simp = nullptr;
            char* trad = nullptr;
            if (ParseOpenccLine(s, &simp, &trad)) {
                T2SPhrase e;
                e.from = str::Dup(trad);
                e.to = ConvertCharsOnly(simp);
                e.fromLen = str::Leni(e.from);
                e.toLen = str::Leni(e.to);
                gPhrases.Append(e);
            }
            *p = saved;
            line = p + 1;
        }
    }
    free(data.data());
    return true;
}

static bool EnsureCharConvLoaded() {
    if (gLoaded) {
        return true;
    }
    if (gLoadFailed) {
        return false;
    }
    if (InterlockedCompareExchange(&gLoadState, 1, 0) != 0) {
        while (InterlockedCompareExchange(&gLoadState, 2, 2) != 2) {
            YieldProcessor();
        }
        return gLoaded;
    }

    TempStr charsPath = OpenccDataPathTemp("TSCharacters.txt");
    TempStr phrasesPath = OpenccDataPathTemp("TSPhrases.txt");
    TempStr twPhrasesPath = OpenccDataPathTemp("TWPhrases.txt");
    bool ok = LoadOpenccFile(charsPath, false) && LoadOpenccFile(phrasesPath, true) && LoadTwPhrasesReverse(twPhrasesPath);
    if (ok && gChars.size() > 0) {
        gChars.Sort(CompareCharByFrom);
        gPhrases.Sort(ComparePhraseByFromLenDesc);
        gLoaded = true;
    } else {
        FreeT2SData();
        gLoadFailed = true;
    }
    InterlockedExchange(&gLoadState, 2);
    return gLoaded;
}

bool CharConvIsReady() {
    return EnsureCharConvLoaded();
}

static const char* LookupCharReplacement(const char* p, int len) {
    if (gChars.size() == 0 || len <= 0) {
        return nullptr;
    }
    int lo = 0;
    int hi = (int)gChars.size() - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const T2SChar& e = gChars.at(mid);
        int cmp = CompareUtf8At(p, len, &e);
        if (cmp == 0) {
            return e.to;
        }
        if (cmp < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return nullptr;
}

static char* ConvertCharsOnly(const char* utf8) {
    if (str::IsEmpty(utf8)) {
        return str::Dup(utf8);
    }
    StrBuilder out;
    const char* p = utf8;
    bool changed = false;
    while (*p) {
        int len = utf8RuneLen((const u8*)p);
        if (len <= 0) {
            len = 1;
        }
        const char* repl = LookupCharReplacement(p, len);
        if (repl) {
            out.Append(repl);
            TempStr orig = str::DupTemp(p, len);
            if (!str::Eq(repl, orig)) {
                changed = true;
            }
        } else {
            out.Append(p, len);
        }
        p += len;
    }
    if (!changed) {
        return str::Dup(utf8);
    }
    return out.StealData();
}

char* TraditionalToSimplified(const char* utf8) {
    if (str::IsEmpty(utf8)) {
        return str::Dup(utf8);
    }
    if (!EnsureCharConvLoaded()) {
        return str::Dup(utf8);
    }

    StrBuilder out;
    const char* p = utf8;
    bool changed = false;
    while (*p) {
        bool matched = false;
        size_t restLen = str::Len(p);
        for (size_t i = 0; i < gPhrases.size(); i++) {
            const T2SPhrase& ph = gPhrases.at(i);
            if (ph.fromLen > 0 && (size_t)ph.fromLen <= restLen && strncmp(p, ph.from, (size_t)ph.fromLen) == 0) {
                out.Append(ph.to);
                if (!str::Eq(ph.from, ph.to)) {
                    changed = true;
                }
                p += ph.fromLen;
                matched = true;
                break;
            }
        }
        if (matched) {
            continue;
        }

        int len = utf8RuneLen((const u8*)p);
        if (len <= 0) {
            len = 1;
        }
        const char* repl = LookupCharReplacement(p, len);
        if (repl) {
            out.Append(repl);
            TempStr orig = str::DupTemp(p, len);
            if (!str::Eq(repl, orig)) {
                changed = true;
            }
        } else {
            out.Append(p, len);
        }
        p += len;
    }

    if (!changed) {
        return str::Dup(utf8);
    }
    return out.StealData();
}
