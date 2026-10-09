/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Newest-first list of at most three font families, separated by '|'.
// A repeated choice moves to the front. The fourth distinct family drops the oldest.

inline void FreeTextMruParse(const char* stored, Vec<char*>& items) {
    items.Reset();
    if (!stored) {
        return;
    }
    const char* p = stored;
    while (*p) {
        const char* bar = p;
        while (*bar && *bar != '|') {
            bar++;
        }
        size_t n = (size_t)(bar - p);
        if (n > 0 && n <= 120) {
            items.Append(str::Dup(p, n));
        }
        if (!*bar) {
            break;
        }
        p = bar + 1;
    }
}

inline char* FreeTextMruPush(const char* stored, const char* font) {
    auto unchanged = [&]() { return str::Dup(stored ? stored : ""); };
    if (!font || !font[0] || str::Len(font) > 120) {
        return unchanged();
    }
    for (const char* p = font; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == '|' || c == 127) {
            return unchanged();
        }
    }
    Vec<char*> items;
    FreeTextMruParse(stored, items);
    for (int i = items.Size() - 1; i >= 0; i--) {
        if (str::EqI(items.at(i), font)) {
            str::Free(items.at(i));
            items.RemoveAt((size_t)i);
        }
    }
    items.InsertAt(0, str::Dup(font));
    while (items.Size() > 3) {
        str::Free(items.Pop());
    }
    char* out = nullptr;
    for (int i = 0; i < items.Size(); i++) {
        if (!out) {
            out = str::Dup(items.at(i));
        } else {
            char* next = str::Join(out, "|", items.at(i));
            str::Free(out);
            out = next;
        }
        str::Free(items.at(i));
    }
    if (!out) {
        out = str::Dup("");
    }
    return out;
}
