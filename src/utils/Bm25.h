/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include <math.h>
#include <wctype.h>

// Query-local vocabulary: no persistent index, dictionary, or model is needed.
// CJK unigrams recall abbreviations; bigrams favor consecutive wording.
struct Bm25Token {
    u64 key;
    bool primary;
    int offset = 0; // codepoint offset within the passage
};

static bool Bm25IsCjk(int cp) {
    return (cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0x20000 && cp <= 0x323af) || (cp >= 0x3040 && cp <= 0x30ff) ||
           (cp >= 0xac00 && cp <= 0xd7af);
}

static void Bm25Tokenize(const int* cps, int count, Vec<Bm25Token>& out) {
    out.Reset();
    int prevCjk = 0;
    for (int i = 0; i < count; i++) {
        int cp = cps[i];
        if (Bm25IsCjk(cp)) {
            out.Append({(u64)cp, true, i});
            if (prevCjk) {
                out.Append({((u64)prevCjk << 21) | (u64)cp, false, i - 1});
            }
            prevCjk = cp;
        } else if (cp <= 0xffff && iswalnum((wint_t)cp)) {
            u64 hash = 14695981039346656037ull;
            int start = i;
            do {
                hash = (hash ^ (u64)towlower((wint_t)cps[i])) * 1099511628211ull;
                i++;
            } while (i < count && cps[i] <= 0xffff && !Bm25IsCjk(cps[i]) && iswalnum((wint_t)cps[i]));
            i--;
            out.Append({hash | (1ull << 63), true, start});
            prevCjk = 0;
        } else if (cp != '\r' && cp != '\n') {
            prevCjk = 0;
        }
    }
}

// Return the start of the complete analyzed phrase, retaining repeated terms.
static int Bm25PhraseOffset(const Vec<Bm25Token>& passage, const Vec<Bm25Token>& query) {
    for (int start = 0; start < passage.Size(); start++) {
        if (!passage[start].primary) continue;
        int p = start, q = 0;
        while (q < query.Size()) {
            if (!query[q].primary) {
                q++;
                continue;
            }
            while (p < passage.Size() && !passage[p].primary) p++;
            if (p == passage.Size() || passage[p].key != query[q].key) break;
            p++;
            q++;
        }
        if (q == query.Size() && q > 0) return passage[start].offset;
    }
    return -1;
}

static float Bm25TermScore(int tf, int df, int documents, int length, float averageLength) {
    if (!tf || !documents || averageLength <= 0) {
        return 0;
    }
    float idf = logf(1.f + (documents - df + 0.5f) / (df + 0.5f));
    float norm = 1.2f * (0.25f + 0.75f * length / averageLength);
    return idf * (tf * 2.2f) / (tf + norm);
}

static float Bm25PassageScore(const Vec<Bm25Token>& terms, const int* tf, const int* df, int documents, int length,
                              float averageLength) {
    int covered = 0, primaryCount = 0;
    float score = 0;
    for (int t = 0; t < terms.Size(); t++) {
        primaryCount += terms[t].primary;
        covered += terms[t].primary && tf[t] > 0;
        float weight = !terms[t].primary || (terms[t].key >> 63) ? 1.f : 0.4f;
        score += weight * Bm25TermScore(tf[t], df[t], documents, length, averageLength);
    }
    return covered > 0 && covered * 5 >= primaryCount * 3 ? score : 0;
}
