/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/JsonParser.h"

#include "TtsPronunciation.h"

static Vec<TtsPronunciationEntry> gPronEntries;
static bool gPronSorted = false;

void TtsPronunciationClear() {
    for (TtsPronunciationEntry& e : gPronEntries) {
        str::Free(e.from);
        str::Free(e.to);
    }
    gPronEntries.Reset();
    gPronSorted = false;
}

int TtsPronunciationEntryCount() {
    return gPronEntries.Size();
}

const Vec<TtsPronunciationEntry>& TtsPronunciationEntriesForTest() {
    return gPronEntries;
}

static void TtsPronunciationSortEntries() {
    if (gPronSorted || gPronEntries.Size() <= 1) {
        gPronSorted = true;
        return;
    }
    // Longest `from` first so "重庆" wins over "重".
    for (int i = 0; i < gPronEntries.Size(); i++) {
        for (int j = i + 1; j < gPronEntries.Size(); j++) {
            size_t li = str::Len(gPronEntries[i].from);
            size_t lj = str::Len(gPronEntries[j].from);
            if (lj > li) {
                TtsPronunciationEntry tmp = gPronEntries[i];
                gPronEntries[i] = gPronEntries[j];
                gPronEntries[j] = tmp;
            }
        }
    }
    gPronSorted = true;
}

struct PronJsonVisitor : json::ValueVisitor {
    struct Pending {
        char* from = nullptr;
        char* to = nullptr;
        bool wholeWord = true;
        bool hasFrom = false;
        bool hasTo = false;
    };
    Vec<Pending> pending;
    int maxIndex = -1;

    ~PronJsonVisitor() override {
        for (Pending& p : pending) {
            str::Free(p.from);
            str::Free(p.to);
        }
    }

    Pending* Ensure(int idx) {
        while (pending.Size() <= idx) {
            pending.Append(Pending{});
        }
        if (idx > maxIndex) {
            maxIndex = idx;
        }
        return &pending[idx];
    }

    static bool ParseIndex(const char* path, int* outIdx, const char** fieldOut) {
        // "/entries[3]/from" or "[3]/from"
        const char* p = str::FindChar(path, '[');
        if (!p) {
            return false;
        }
        p++;
        int idx = 0;
        if (!str::Parse(p, "%d", &idx)) {
            return false;
        }
        const char* field = str::FindChar(p, ']');
        if (!field || field[1] != '/') {
            return false;
        }
        *outIdx = idx;
        *fieldOut = field + 2;
        return true;
    }

    bool Visit(const char* path, const char* value, json::Type type) override {
        int idx = 0;
        const char* field = nullptr;
        if (!ParseIndex(path, &idx, &field)) {
            return true;
        }
        Pending* pe = Ensure(idx);
        if (str::EqI(field, "from") || str::EqI(field, "word") || str::EqI(field, "src")) {
            str::ReplaceWithCopy(&pe->from, value);
            pe->hasFrom = !str::IsEmpty(value);
        } else if (str::EqI(field, "to") || str::EqI(field, "speak") || str::EqI(field, "replace") ||
                   str::EqI(field, "pinyin")) {
            // Prefer explicit "to"/"speak" over "pinyin" if both appear; last write wins
            // which is fine for simple dictionaries that use one of them.
            str::ReplaceWithCopy(&pe->to, value);
            pe->hasTo = !str::IsEmpty(value);
        } else if (str::EqI(field, "wholeWord") || str::EqI(field, "whole") || str::EqI(field, "exact")) {
            if (type == json::Type::Bool) {
                pe->wholeWord = str::EqI(value, "true");
            } else if (type == json::Type::String) {
                pe->wholeWord = !(str::EqI(value, "false") || str::Eq(value, "0"));
            } else if (type == json::Type::Number) {
                pe->wholeWord = !str::Eq(value, "0");
            }
        }
        return true;
    }
};

bool TtsPronunciationLoadFromJson(const char* json) {
    if (str::IsEmpty(json)) {
        return false;
    }
    PronJsonVisitor visitor;
    if (!json::Parse(json, &visitor)) {
        return false;
    }
    TtsPronunciationClear();
    for (int i = 0; i <= visitor.maxIndex; i++) {
        PronJsonVisitor::Pending& p = visitor.pending[i];
        if (!p.hasFrom || !p.hasTo) {
            continue;
        }
        TtsPronunciationEntry e{};
        e.from = p.from;
        e.to = p.to;
        e.wholeWord = p.wholeWord;
        p.from = nullptr;
        p.to = nullptr;
        gPronEntries.Append(e);
    }
    gPronSorted = false;
    TtsPronunciationSortEntries();
    return true;
}

static int Utf8CharBytes(const char* s) {
    if (!s || !*s) {
        return 0;
    }
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

static bool IsAsciiWordChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static bool PronWholeWordOk(const char* text, size_t textLen, size_t pos, size_t matchLen, bool wholeWord) {
    if (!wholeWord) {
        return true;
    }
    // Latin-style boundaries: avoid matching inside alphanumeric tokens.
    // CJK has no spaces; longest-match already protects compounds like 重庆 vs 重.
    bool leftAscii = false;
    bool rightAscii = false;
    if (pos > 0) {
        // Look at previous byte only for ASCII word chars (sufficient for Latin).
        leftAscii = IsAsciiWordChar((unsigned char)text[pos - 1]);
    }
    if (pos + matchLen < textLen) {
        rightAscii = IsAsciiWordChar((unsigned char)text[pos + matchLen]);
    }
    // If either side is mid-Latin-word, require that the match itself is not purely CJK
    // — i.e. only enforce for rules that look Latin-ish.
    bool matchHasLatin = false;
    for (size_t i = 0; i < matchLen; i++) {
        unsigned char c = (unsigned char)text[pos + i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            matchHasLatin = true;
            break;
        }
    }
    if (!matchHasLatin) {
        return true;
    }
    return !leftAscii && !rightAscii;
}

static const TtsPronunciationEntry* PronFindMatch(const char* text, size_t textLen, size_t pos) {
    TtsPronunciationSortEntries();
    for (TtsPronunciationEntry& e : gPronEntries) {
        if (str::IsEmpty(e.from) || str::IsEmpty(e.to)) {
            continue;
        }
        size_t flen = str::Len(e.from);
        if (flen == 0 || pos + flen > textLen) {
            continue;
        }
        if (strncmp(text + pos, e.from, flen) != 0) {
            continue;
        }
        if (!PronWholeWordOk(text, textLen, pos, flen, e.wholeWord)) {
            continue;
        }
        return &e;
    }
    return nullptr;
}

char* TtsPronunciationApply(const char* text, Vec<int>* spokenToSourceUtf8Out) {
    if (str::IsEmpty(text)) {
        return str::Dup("");
    }
    if (gPronEntries.Size() == 0) {
        if (spokenToSourceUtf8Out) {
            spokenToSourceUtf8Out->Reset();
            int n = (int)str::Len(text);
            for (int i = 0; i <= n; i++) {
                spokenToSourceUtf8Out->Append(i);
            }
        }
        return str::Dup(text);
    }

    size_t textLen = str::Len(text);
    StrBuilder out;
    Vec<int> map;
    size_t pos = 0;
    while (pos < textLen) {
        const TtsPronunciationEntry* hit = PronFindMatch(text, textLen, pos);
        if (hit) {
            size_t flen = str::Len(hit->from);
            size_t tlen = str::Len(hit->to);
            // Map every spoken byte produced by this replacement back to the start
            // of the matched source span (good enough for highlight sync).
            for (size_t i = 0; i < tlen; i++) {
                map.Append((int)pos);
            }
            out.Append(hit->to);
            pos += flen;
            continue;
        }
        int nb = Utf8CharBytes(text + pos);
        if (nb < 1) {
            nb = 1;
        }
        if (pos + (size_t)nb > textLen) {
            nb = (int)(textLen - pos);
        }
        for (int i = 0; i < nb; i++) {
            map.Append((int)(pos + i));
            out.AppendChar(text[pos + i]);
        }
        pos += (size_t)nb;
    }
    map.Append((int)textLen);

    if (spokenToSourceUtf8Out) {
        spokenToSourceUtf8Out->Reset();
        spokenToSourceUtf8Out->Append(map.LendData(), map.Size());
    }
    return out.StealData();
}

struct PinyinToneMapEntry {
    const char* marked; // UTF-8 tone vowel
    const char* base;   // ASCII replacement (ü → v for SAPI)
    int tone;           // 1..4, or 0 if unmarked base vowel
};

static const PinyinToneMapEntry kPinyinToneMap[] = {
    {"ā", "a", 1}, {"á", "a", 2}, {"ǎ", "a", 3}, {"à", "a", 4}, {"ō", "o", 1}, {"ó", "o", 2}, {"ǒ", "o", 3},
    {"ò", "o", 4}, {"ē", "e", 1}, {"é", "e", 2}, {"ě", "e", 3}, {"è", "e", 4}, {"ī", "i", 1}, {"í", "i", 2},
    {"ǐ", "i", 3}, {"ì", "i", 4}, {"ū", "u", 1}, {"ú", "u", 2}, {"ǔ", "u", 3}, {"ù", "u", 4}, {"ǖ", "v", 1},
    {"ǘ", "v", 2}, {"ǚ", "v", 3}, {"ǜ", "v", 4}, {"ü", "v", 0}, {"ń", "n", 2}, {"ň", "n", 3}, {"ǹ", "n", 4},
};

static bool AppendPinyinSyllableSapi(StrBuilder& out, const char* syl, size_t sylLen) {
    if (sylLen == 0) {
        return false;
    }
    // Dictionary light-tone markers: ".de" / "·de"
    while (sylLen > 0) {
        if (syl[0] == '.' || syl[0] == '\'') {
            syl++;
            sylLen--;
            continue;
        }
        // UTF-8 middle dot U+00B7
        if (sylLen >= 2 && (unsigned char)syl[0] == 0xC2 && (unsigned char)syl[1] == 0xB7) {
            syl += 2;
            sylLen -= 2;
            continue;
        }
        break;
    }
    if (sylLen == 0) {
        return false;
    }
    // Already numbered: "qiao4" / "nv3"
    char last = syl[sylLen - 1];
    if (last >= '1' && last <= '5') {
        StrBuilder bare;
        for (size_t i = 0; i + 1 < sylLen; i++) {
            unsigned char c = (unsigned char)syl[i];
            if (c >= 'A' && c <= 'Z') {
                c = (unsigned char)(c - 'A' + 'a');
            }
            if (c == 0xC3 && i + 1 < sylLen - 1 && (unsigned char)syl[i + 1] == 0xBC) {
                bare.AppendChar('v'); // ü
                i++;
                continue;
            }
            if (c == ':' && bare.Size() > 0 && bare.Last() == 'u') {
                bare.RemoveLast();
                bare.AppendChar('v'); // u:
                continue;
            }
            if (c == '.' || c == '\'') {
                continue;
            }
            bare.AppendChar((char)c);
        }
        if (bare.IsEmpty()) {
            return false;
        }
        if (!out.IsEmpty()) {
            out.AppendChar(' ');
        }
        out.Append(bare.CStr());
        out.AppendChar(' ');
        out.AppendChar(last);
        return true;
    }

    StrBuilder bare;
    int tone = 0;
    size_t i = 0;
    while (i < sylLen) {
        bool matched = false;
        for (const PinyinToneMapEntry& e : kPinyinToneMap) {
            size_t mlen = str::Len(e.marked);
            if (mlen == 0 || i + mlen > sylLen) {
                continue;
            }
            if (strncmp(syl + i, e.marked, mlen) != 0) {
                continue;
            }
            bare.Append(e.base);
            if (e.tone > 0 && tone == 0) {
                tone = e.tone;
            }
            i += mlen;
            matched = true;
            break;
        }
        if (matched) {
            continue;
        }
        char c = syl[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c == '\'' || c == '-' || c == '.') {
            i++;
            continue;
        }
        bare.AppendChar(c);
        i++;
    }
    if (bare.IsEmpty()) {
        return false;
    }
    if (tone == 0) {
        tone = 5; // unmarked / neutral / leading "." light tone
    }
    if (!out.IsEmpty()) {
        out.AppendChar(' ');
    }
    out.Append(bare.CStr());
    out.AppendChar(' ');
    out.AppendChar((char)('0' + tone));
    return true;
}

char* TtsPinyinToSapiPh(const char* pinyin) {
    if (str::IsEmpty(pinyin)) {
        return nullptr;
    }
    StrBuilder out;
    const char* p = pinyin;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '/' || *p == ',') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '/' && *p != ',') {
            p++;
        }
        if (!AppendPinyinSyllableSapi(out, start, (size_t)(p - start))) {
            return nullptr;
        }
    }
    if (out.IsEmpty()) {
        return nullptr;
    }
    return out.StealData();
}
