/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

constexpr const char* kPalettePrefixCommands = ">";
constexpr const char* kPalettePrefixFileHistory = "#";
constexpr const char* kPalettePrefixTabs = "@";
constexpr const char* kPalettePrefixEverything = ":";

void RunCommandPalette(MainWindow*, const char* prefix, int smartTabAdvance);
HWND CommandPaletteHwndForAccelerator(HWND hwnd);
void SafeDeleteCommandPaletteWnd();

void SplitFilterToWords(const char* filter, StrVec& words);
bool FilterMatches(const char* str, const StrVec& words);

struct DrawMaybeHighlightedTextArgs {
    HDC hdc = nullptr;
    RECT rc{};
    const char* text = nullptr;
    const StrVec& filterWords;
    Vec<u8>& highlighted;
    COLORREF colBg = 0;
    bool isRtl = false;
    bool matchWholeWord = false; // only highlight whole-word occurrences (issue #4295)
    uint drawFmt = 0;
    int primaryHighlightStart = -1; // optional UTF-8 byte range rendered with the primary color
    int primaryHighlightEnd = -1;
    COLORREF secondaryHighlightColor = CLR_INVALID;

    DrawMaybeHighlightedTextArgs(const StrVec& fw, Vec<u8>& hl) : filterWords(fw), highlighted(hl) {}
};

void DrawMaybeHighlightedText(DrawMaybeHighlightedTextArgs& args);
