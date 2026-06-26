/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "TextSelection.h"
#include "Theme.h"
#include "Translations.h"
#include "LookupAudio.h"
#include "FloatingPopupStyle.h"
#include "WordLookup.h"

#include "DarkModeSubclass.h"

#include "utils/Log.h"

#include <algorithm>

struct OfflineDictionary;

constexpr int kPopupDx = 340;
constexpr int kCardPad = 20;
constexpr int kCardPadTop = 12;
constexpr int kCardPadBottom = 14;
constexpr int kCloseBtnSz = 20;
constexpr UINT_PTR kSpeakerHoverTimerId = 9101;
constexpr int kSpeakerHoverTimerMs = 16;
constexpr float kSpeakerHoverAnimStep = 0.18f;
constexpr int kSpeakerPlayMinFrames = 48;
constexpr int kSpeakerWaveCycleFrames = 6;
constexpr int kLoadingDotCycleFrames = 10;
constexpr int kPopupGap = 10;
constexpr int kExampleMaxDy = 78;
constexpr int kPaintMaxContentDy = 560;
constexpr int kMaxLookupTabs = 8;
constexpr int kMaxLookupDefs = 24;
constexpr int kMaxChineseLookupWindowChars = 8;
constexpr int kMaxChineseLookupLookbackChars = 3;

struct DictDefinition {
    char* en = nullptr;
    char* zh = nullptr;
};

struct DictSense {
    char* label = nullptr;
    char* headword = nullptr;
    char* fl = nullptr;
    char* ipa = nullptr;
    char* exampleEn = nullptr;
    char* exampleZh = nullptr;
    bool hasAudio = false;
    Vec<DictDefinition> definitions;
};

static void FreeDictSense(DictSense* s) {
    if (!s) {
        return;
    }
    str::Free(s->label);
    str::Free(s->headword);
    str::Free(s->fl);
    str::Free(s->ipa);
    str::Free(s->exampleEn);
    str::Free(s->exampleZh);
    for (size_t i = 0; i < s->definitions.size(); i++) {
        str::Free(s->definitions.at(i).en);
        str::Free(s->definitions.at(i).zh);
    }
    s->definitions.Reset();
}

struct WordLookupWnd : Wnd {
    WordLookupWnd() = default;
    ~WordLookupWnd() override;

    bool Create(MainWindow* win, const char* word, Point screenPos);
    void SetLoading(const char* word);
    void SetResults(const char* word, DictSense* senses, int nSenses, OfflineDictionary* audioDict, u64 audioOffset,
                    u32 audioSize, const char* audioExt);
    void PlayAudio();
    void SelectTab(int tab);
    void UpdateChrome();
    void LayoutCloseButton();

    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override;

    MainWindow* win = nullptr;
    char* queryWord = nullptr;
    DictSense* senses = nullptr;
    int nSenses = 0;
    int currTab = 0;
    bool isLoading = false;
    Point anchorPos{};

    HFONT font = nullptr;
    HFONT headwordFont = nullptr;
    HFONT posFont = nullptr;
    bool fontOwned = false;
    bool headwordFontOwned = false;
    Rect closeBtnPos;
    Rect speakerBtnPos;
    Rect tabRects[kMaxLookupTabs];
    bool closeBtnHover = false;
    bool speakerBtnHover = false;
    float speakerHoverAnim = 0.f;
    bool speakerPlaying = false;
    int speakerWaveTick = 0;
    int speakerPlayFrames = 0;
    int loadingDotTick = 0;
    int tabHover = -1;

    OfflineDictionary* audioDict = nullptr;
    u64 audioOffset = 0;
    u32 audioSize = 0;
    char* audioExt = nullptr;
};

static WordLookupWnd* gWordLookupWnd = nullptr;
static void StopCurrentLookupAudio(bool clearSpeakerAnim = true);
static void PositionWordLookup(WordLookupWnd* wnd, Point screenPos);

static HFONT CreateBoldFontFrom(HFONT base) {
    if (!base) {
        return nullptr;
    }
    LOGFONTW lf{};
    GetObjectW(base, sizeof(lf), &lf);
    lf.lfWeight = FW_BOLD;
    return CreateFontIndirectW(&lf);
}

static HFONT CreateScaledFontFrom(HFONT base, int pct, int weight = 0) {
    if (!base) {
        return nullptr;
    }
    LOGFONTW lf{};
    GetObjectW(base, sizeof(lf), &lf);
    lf.lfHeight = MulDiv(lf.lfHeight, pct, 100);
    if (weight > 0) {
        lf.lfWeight = weight;
    }
    return CreateFontIndirectW(&lf);
}

static const char* AbbreviateFl(const char* fl) {
    if (str::IsEmpty(fl)) {
        return nullptr;
    }
    if (str::EqI(fl, "noun") || str::EqI(fl, "plural noun")) {
        return "n.";
    }
    if (str::EqI(fl, "verb") || str::EqI(fl, "transitive verb") || str::EqI(fl, "intransitive verb")) {
        return "v.";
    }
    if (str::EqI(fl, "adjective")) {
        return "adj.";
    }
    if (str::EqI(fl, "adverb")) {
        return "adv.";
    }
    if (str::EqI(fl, "preposition")) {
        return "prep.";
    }
    if (str::EqI(fl, "conjunction")) {
        return "conj.";
    }
    if (str::EqI(fl, "interjection")) {
        return "interj.";
    }
    if (str::EqI(fl, "pronoun")) {
        return "pron.";
    }
    if (str::Eq(fl, "名") || str::StartsWith(fl, "名")) {
        return "n.";
    }
    if (str::Eq(fl, "动")) {
        return "v.";
    }
    if (str::Eq(fl, "形")) {
        return "adj.";
    }
    if (str::Eq(fl, "副")) {
        return "adv.";
    }
    if (str::Eq(fl, "代")) {
        return "pron.";
    }
    if (str::Eq(fl, "介")) {
        return "prep.";
    }
    if (str::Eq(fl, "连")) {
        return "conj.";
    }
    if (str::Eq(fl, "叹")) {
        return "interj.";
    }
    return fl;
}

static COLORREF LookupSpeakerHoverBgColor() {
    if (ThemeUsesDarkChrome()) {
        return AccentColor(ThemeWindowControlBackgroundColor(), 22);
    }
    return AccentColor(FloatingPopupBg(), 22);
}

static bool HasAudioMeta(const DictSense* sense) {
    return sense && sense->hasAudio;
}

static TempStr LookingUpTextTemp(int dotPhase) {
    int dots = (dotPhase % 3) + 1;
    return str::FormatTemp("%s%.*s", _TRA("Looking up"), dots, "...");
}

static Rect LookupCardRect(HWND hwnd) {
    Rect card = ClientRect(hwnd);
    if (card.dx < 1) {
        card.dx = 1;
    }
    if (card.dy < 1) {
        card.dy = 1;
    }
    return card;
}

static void UpdateLookupWindowRgn(WordLookupWnd* wnd) {
    if (!wnd || !wnd->hwnd) {
        return;
    }
    UpdateFloatingPopupWindowRgn(wnd->hwnd, kFloatingPopupCornerRadius);
}

WordLookupWnd::~WordLookupWnd() {
    StopCurrentLookupAudio();
    if (hwnd) {
        KillTimer(hwnd, kSpeakerHoverTimerId);
    }
    str::Free(queryWord);
    str::Free(audioExt);
    if (fontOwned && font) {
        DeleteObject(font);
    }
    if (headwordFontOwned && headwordFont) {
        DeleteObject(headwordFont);
    }
    if (posFont) {
        DeleteObject(posFont);
    }
    if (senses) {
        for (int i = 0; i < nSenses; i++) {
            FreeDictSense(&senses[i]);
        }
        free(senses);
    }
}

static void SafeDeleteWordLookupWnd() {
    if (!gWordLookupWnd) {
        return;
    }
    WordLookupWnd* wnd = gWordLookupWnd;
    gWordLookupWnd = nullptr;
    delete wnd;
}

static void ScheduleDeleteWordLookupWnd() {
    if (!gWordLookupWnd) {
        return;
    }
    auto fn = MkFunc0Void(SafeDeleteWordLookupWnd);
    uitask::Post(fn, "SafeDeleteWordLookupWnd");
}

void CloseWordLookup() {
    ScheduleDeleteWordLookupWnd();
}

void RefreshWordLookupTheme() {
    WordLookupWnd* wnd = gWordLookupWnd;
    if (!wnd || !wnd->hwnd) {
        return;
    }
    HWND popupHwnd = wnd->hwnd;
    COLORREF colTxt = FloatingPopupTextColor();
    COLORREF colBg = FloatingPopupBg();
    wnd->SetColors(colTxt, colBg);
    if (UseDarkModeLib() && ThemeUsesDarkChrome()) {
        DarkMode::removeWindowCtlColorSubclass(popupHwnd);
    } else if (UseDarkModeLib()) {
        DarkMode::removeWindowCtlColorSubclass(popupHwnd);
        DarkMode::setDarkTitleBarEx(popupHwnd, true);
    }
    wnd->UpdateChrome();
    uint flags = RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN;
    RedrawWindow(popupHwnd, nullptr, nullptr, flags);
}

static bool IsAudioReady(const DictSense* sense) {
    return HasAudioMeta(sense);
}

static bool IsLookupAudioPlaying() {
    return LookupAudioIsPlaying();
}

static void EnsureSpeakerAnimTimer(WordLookupWnd* wnd);

static void StopCurrentLookupAudio(bool clearSpeakerAnim) {
    LookupAudioStop();
    if (clearSpeakerAnim && gWordLookupWnd) {
        gWordLookupWnd->speakerPlaying = false;
        gWordLookupWnd->speakerWaveTick = 0;
        gWordLookupWnd->speakerPlayFrames = 0;
        EnsureSpeakerAnimTimer(gWordLookupWnd);
    }
}

static char* UnescapeDictField(const char* s) {
    if (!s) {
        return nullptr;
    }
    StrBuilder res;
    for (const char* p = s; *p; p++) {
        if (*p == '\\') {
            p++;
            if (*p == 'n') {
                res.AppendChar('\n');
            } else if (*p == 't') {
                res.AppendChar('\t');
            } else if (*p == 'r') {
                res.AppendChar('\r');
            } else if (*p) {
                res.AppendChar(*p);
            }
            continue;
        }
        res.AppendChar(*p);
    }
    return res.StealData();
}

static char* BuildDefinitionsText(const DictSense* sense) {
    StrBuilder res;
    int n = (int)std::min<size_t>(sense->definitions.size(), kMaxLookupDefs);
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            res.Append("\r\n");
        }
        DictDefinition def = sense->definitions.at(i);
        if (n > 1) {
            res.AppendFmt("%d. ", i + 1);
        }
        if (def.en) {
            res.Append(def.en);
        }
        if (def.zh) {
            if (def.en) {
                res.Append(" ");
            }
            res.Append(def.zh);
        }
        if (!def.en && !def.zh) {
            res.Append("");
        }
    }
    return res.StealData();
}

static char* BuildExampleText(const DictSense* sense) {
    if (!sense || (!sense->exampleEn && !sense->exampleZh)) {
        return nullptr;
    }
    StrBuilder res;
    if (sense->exampleEn) {
        res.Append(sense->exampleEn);
    }
    if (sense->exampleZh) {
        if (sense->exampleEn) {
            res.Append("\r\n");
        }
        res.Append(sense->exampleZh);
    }
    return res.StealData();
}

struct DictIndexEntry {
    char* word = nullptr;
    u64 dataOffset = 0;
    u32 dataSize = 0;
    u64 audioOffset = 0;
    u32 audioSize = 0;
    char* audioExt = nullptr;
};

struct OfflineDictionary {
    char* dir = nullptr;
    char* dataPath = nullptr;
    char* audioPath = nullptr;
    Vec<DictIndexEntry*> entries;
    bool loaded = false;
    bool failed = false;
};

static OfflineDictionary gOfflineDict;
static OfflineDictionary gOfflineDictZh;

static CRITICAL_SECTION gOfflineDictCs;
static volatile LONG gOfflineDictCsReady = 0;

static CRITICAL_SECTION* OfflineDictCs() {
    if (InterlockedCompareExchange(&gOfflineDictCsReady, 1, 0) == 0) {
        InitializeCriticalSection(&gOfflineDictCs);
        InterlockedExchange(&gOfflineDictCsReady, 2);
        return &gOfflineDictCs;
    }
    while (InterlockedCompareExchange(&gOfflineDictCsReady, 2, 2) != 2) {
        YieldProcessor();
    }
    return &gOfflineDictCs;
}

static bool IsDictPackagePresent(const char* dir) {
    if (str::IsEmpty(dir)) {
        return false;
    }
    return file::Exists(path::JoinTemp(dir, "SumatraDict.idx")) &&
           file::Exists(path::JoinTemp(dir, "SumatraDict.dat")) &&
           file::Exists(path::JoinTemp(dir, "SumatraDictAudio.dat"));
}

static bool IsDictPackagePresentZh(const char* dir) {
    if (str::IsEmpty(dir)) {
        return false;
    }
    return file::Exists(path::JoinTemp(dir, "SumatraDictZh.idx")) &&
           file::Exists(path::JoinTemp(dir, "SumatraDictZh.dat"));
}

static TempStr OfflineDictionaryDirTemp() {
    if (!str::IsEmpty(gGlobalPrefs->offlineDictionaryPath)) {
        return str::DupTemp(gGlobalPrefs->offlineDictionaryPath);
    }
    TempStr dictSubDir = GetPathInExeDirTemp("dict");
    if (IsDictPackagePresent(dictSubDir) || IsDictPackagePresentZh(dictSubDir)) {
        return dictSubDir;
    }
    // Legacy layout: dictionaries next to the executable.
    TempStr exeDir = GetSelfExeDirTemp();
    if (IsDictPackagePresent(exeDir) || IsDictPackagePresentZh(exeDir)) {
        return exeDir;
    }
    return dictSubDir;
}

static DictIndexEntry* ParseDictIndexLine(char* line) {
    char* cols[6]{};
    int nCols = 0;
    char* start = line;
    for (char* p = line; *p && nCols < dimof(cols); p++) {
        if (*p == '\t') {
            *p = 0;
            cols[nCols++] = start;
            start = p + 1;
        }
    }
    if (nCols < dimof(cols)) {
        cols[nCols++] = start;
    }
    if (nCols < 5 || str::IsEmpty(cols[0])) {
        return nullptr;
    }
    auto* e = new DictIndexEntry();
    e->word = str::Dup(cols[0]);
    e->dataOffset = (u64)_strtoui64(cols[1], nullptr, 10);
    e->dataSize = (u32)strtoul(cols[2], nullptr, 10);
    e->audioOffset = (u64)_strtoui64(cols[3], nullptr, 10);
    e->audioSize = (u32)strtoul(cols[4], nullptr, 10);
    if (nCols >= 6 && !str::IsEmpty(cols[5])) {
        e->audioExt = str::Dup(cols[5]);
    } else {
        e->audioExt = str::Dup("");
    }
    return e;
}

static bool LoadOfflineDictionaryIndex(OfflineDictionary* dict, const char* dictDir, const char* idxName,
                                       const char* dataName, const char* audioName) {
    ScopedCritSec lock(OfflineDictCs());
    if (dict->loaded) {
        return true;
    }
    if (dict->failed) {
        return false;
    }

    TempStr idxPath = path::JoinTemp(dictDir, idxName);
    ByteSlice idxData = file::ReadFile(idxPath);
    if (!idxData.data() || idxData.size() == 0) {
        dict->failed = true;
        return false;
    }
    char* data = (char*)idxData.data();
    char* end = data + idxData.size();
    char* line = data;
    for (char* p = data; p <= end; p++) {
        if (p == end || *p == '\n') {
            char saved = *p;
            *p = 0;
            char* s = line;
            if (str::StartsWith(s, UTF8_BOM)) {
                s += 3;
            }
            // Index lines may end with a field-separator tab; don't use TrimWSInPlace here
            // because it treats '\t' as whitespace and drops the last empty column.
            size_t sLen = str::Len(s);
            while (sLen > 0 && (s[sLen - 1] == '\n' || s[sLen - 1] == '\r')) {
                s[--sLen] = 0;
            }
            if (!str::IsEmpty(s) && !str::StartsWith(s, "#")) {
                DictIndexEntry* e = ParseDictIndexLine(s);
                if (e) {
                    dict->entries.Append(e);
                }
            }
            *p = saved;
            line = p + 1;
        }
    }
    free(idxData.data());

    if (dict->entries.size() == 0) {
        dict->failed = true;
        return false;
    }
    dict->dir = str::Dup(dictDir);
    dict->dataPath = path::Join(nullptr, dictDir, dataName);
    if (audioName) {
        dict->audioPath = path::Join(nullptr, dictDir, audioName);
    }
    dict->loaded = true;
    return true;
}

static bool LoadOfflineDictionary() {
    if (gOfflineDict.loaded) {
        return true;
    }
    if (gOfflineDict.failed) {
        return false;
    }

    TempStr dictDir = OfflineDictionaryDirTemp();
    if (!IsDictPackagePresent(dictDir)) {
        gOfflineDict.failed = true;
        return false;
    }
    return LoadOfflineDictionaryIndex(&gOfflineDict, dictDir, "SumatraDict.idx", "SumatraDict.dat",
                                      "SumatraDictAudio.dat");
}

static bool LoadOfflineDictionaryZh() {
    if (gOfflineDictZh.loaded) {
        return true;
    }
    if (gOfflineDictZh.failed) {
        return false;
    }

    TempStr dictDir = OfflineDictionaryDirTemp();
    if (!IsDictPackagePresentZh(dictDir)) {
        gOfflineDictZh.failed = true;
        return false;
    }
    return LoadOfflineDictionaryIndex(&gOfflineDictZh, dictDir, "SumatraDictZh.idx", "SumatraDictZh.dat", nullptr);
}

static bool AnyOfflineDictionaryLoaded() {
    bool en = LoadOfflineDictionary();
    bool zh = LoadOfflineDictionaryZh();
    return en || zh;
}

static int CompareDictWord(const char* a, const char* b) {
    return strcmp(a, b);
}

static DictIndexEntry* FindDictIndexEntryIn(OfflineDictionary* dict, const char* word) {
    if (!dict || !dict->loaded) {
        return nullptr;
    }
    int lo = 0;
    int hi = (int)dict->entries.size() - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        DictIndexEntry* e = dict->entries.at(mid);
        int cmp = CompareDictWord(word, e->word);
        if (cmp == 0) {
            return e;
        }
        if (cmp < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return nullptr;
}

struct DictLookupHit {
    DictIndexEntry* entry = nullptr;
    OfflineDictionary* dict = nullptr;
};

static DictLookupHit FindDictLookupHit(const char* word) {
    DictLookupHit hit;
    if (gOfflineDict.loaded) {
        hit.entry = FindDictIndexEntryIn(&gOfflineDict, word);
        if (hit.entry) {
            hit.dict = &gOfflineDict;
            return hit;
        }
    }
    if (gOfflineDictZh.loaded) {
        hit.entry = FindDictIndexEntryIn(&gOfflineDictZh, word);
        if (hit.entry) {
            hit.dict = &gOfflineDictZh;
        }
    }
    return hit;
}

static void AppendLookupCandidate(StrVec& candidates, const char* s) {
    if (!str::IsEmpty(s)) {
        candidates.Append(str::Dup(s));
    }
}

static void AddIrregularPluralFallbacks(StrVec& candidates, const char* word) {
    size_t n = str::Len(word);
    if (n >= 3 && str::EndsWith(word, "men")) {
        TempStr base = str::FormatTemp("%.*sman", (int)n - 3, word);
        AppendLookupCandidate(candidates, base);
    }
    static const char* kIrregularPlurals[][2] = {
        {"children", "child"},     {"feet", "foot"},        {"teeth", "tooth"},        {"geese", "goose"},
        {"mice", "mouse"},         {"oxen", "ox"},          {"people", "person"},      {"leaves", "leaf"},
        {"lives", "life"},         {"loaves", "loaf"},      {"knives", "knife"},       {"wives", "wife"},
        {"wolves", "wolf"},        {"halves", "half"},      {"calves", "calf"},        {"shelves", "shelf"},
        {"thieves", "thief"},      {"selves", "self"},      {"criteria", "criterion"}, {"phenomena", "phenomenon"},
        {"bacteria", "bacterium"}, {"fungi", "fungus"},     {"cacti", "cactus"},       {"foci", "focus"},
        {"alumni", "alumnus"},     {"syllabi", "syllabus"}, {"nuclei", "nucleus"},     {"stimuli", "stimulus"},
    };
    for (size_t i = 0; i < dimof(kIrregularPlurals); i++) {
        if (str::Eq(word, kIrregularPlurals[i][0])) {
            AppendLookupCandidate(candidates, kIrregularPlurals[i][1]);
        }
    }
}

static void AddIrregularVerbFallbacks(StrVec& candidates, const char* word) {
    static const char* kIrregularVerbs[][2] = {
        {"arisen", "arise"},  {"arose", "arise"},      {"awoken", "awake"},     {"awoke", "awake"},
        {"beaten", "beat"},   {"became", "become"},    {"been", "be"},          {"begun", "begin"},
        {"began", "begin"},   {"bent", "bend"},        {"bidden", "bid"},       {"bitten", "bite"},
        {"blown", "blow"},    {"blew", "blow"},        {"broken", "break"},     {"brought", "bring"},
        {"built", "build"},   {"bought", "buy"},       {"caught", "catch"},     {"chosen", "choose"},
        {"chose", "choose"},  {"came", "come"},        {"dealt", "deal"},       {"dug", "dig"},
        {"done", "do"},       {"did", "do"},           {"drawn", "draw"},       {"drew", "draw"},
        {"dreamt", "dream"},  {"driven", "drive"},     {"drove", "drive"},      {"drunk", "drink"},
        {"drank", "drink"},   {"eaten", "eat"},        {"ate", "eat"},          {"fallen", "fall"},
        {"fell", "fall"},     {"fed", "feed"},         {"felt", "feel"},        {"fought", "fight"},
        {"flown", "fly"},     {"flew", "fly"},         {"forbidden", "forbid"}, {"forgotten", "forget"},
        {"forgot", "forget"}, {"forgiven", "forgive"}, {"froze", "freeze"},     {"frozen", "freeze"},
        {"given", "give"},    {"gave", "give"},        {"gone", "go"},          {"went", "go"},
        {"grown", "grow"},    {"grew", "grow"},        {"had", "have"},         {"heard", "hear"},
        {"held", "hold"},     {"hidden", "hide"},      {"hid", "hide"},         {"kept", "keep"},
        {"knelt", "kneel"},   {"known", "know"},       {"knew", "know"},        {"laid", "lay"},
        {"led", "lead"},      {"leant", "lean"},       {"leapt", "leap"},       {"learnt", "learn"},
        {"lent", "lend"},     {"lost", "lose"},        {"made", "make"},        {"meant", "mean"},
        {"met", "meet"},      {"mistaken", "mistake"}, {"mistook", "mistake"},  {"paid", "pay"},
        {"proven", "prove"},  {"ridden", "ride"},      {"rode", "ride"},        {"risen", "rise"},
        {"rang", "ring"},     {"rung", "ring"},        {"ran", "run"},          {"said", "say"},
        {"seen", "see"},      {"saw", "see"},          {"sold", "sell"},        {"sent", "send"},
        {"sewn", "sew"},      {"shaken", "shake"},     {"shook", "shake"},      {"shaven", "shave"},
        {"shone", "shine"},   {"shown", "show"},       {"shrank", "shrink"},    {"shrunk", "shrink"},
        {"slept", "sleep"},   {"slid", "slide"},       {"slung", "sling"},      {"smelt", "smell"},
        {"sought", "seek"},   {"spoken", "speak"},     {"spent", "spend"},      {"spilt", "spill"},
        {"spoilt", "spoil"},  {"sprung", "spring"},    {"stood", "stand"},      {"stolen", "steal"},
        {"stole", "steal"},   {"stuck", "stick"},      {"stung", "sting"},      {"struck", "strike"},
        {"strung", "string"}, {"striven", "strive"},   {"strove", "strive"},    {"sworn", "swear"},
        {"swore", "swear"},   {"swept", "sweep"},      {"swollen", "swell"},    {"swam", "swim"},
        {"swum", "swim"},     {"swung", "swing"},      {"taken", "take"},       {"took", "take"},
        {"taught", "teach"},  {"told", "tell"},        {"thought", "think"},    {"threw", "throw"},
        {"thrown", "throw"},  {"torn", "tear"},        {"trod", "tread"},       {"understood", "understand"},
        {"woken", "wake"},    {"woke", "wake"},        {"worn", "wear"},        {"wove", "weave"},
        {"woven", "weave"},   {"won", "win"},          {"written", "write"},    {"wrote", "write"},
    };
    for (size_t i = 0; i < dimof(kIrregularVerbs); i++) {
        if (str::Eq(word, kIrregularVerbs[i][0])) {
            AppendLookupCandidate(candidates, kIrregularVerbs[i][1]);
        }
    }
}

static void AddInflectionFallbacks(StrVec& candidates, const char* word) {
    size_t n = str::Len(word);
    if (n < 4) {
        return;
    }
    if (str::EndsWith(word, "ies") && n > 4) {
        TempStr base = str::FormatTemp("%.*sy", (int)n - 3, word);
        AppendLookupCandidate(candidates, base);
    }
    if (str::EndsWith(word, "ied") && n > 4) {
        TempStr base = str::FormatTemp("%.*sy", (int)n - 3, word);
        AppendLookupCandidate(candidates, base);
    }
    if (str::EndsWith(word, "ing") && n > 5) {
        TempStr base = str::DupTemp(word, n - 3);
        AppendLookupCandidate(candidates, base);
        if (n > 6 && word[n - 4] == word[n - 5]) {
            base = str::DupTemp(word, n - 4);
            AppendLookupCandidate(candidates, base);
        }
        base = str::FormatTemp("%.*se", (int)n - 3, word);
        AppendLookupCandidate(candidates, base);
    }
    if (str::EndsWith(word, "ed") && n > 4) {
        TempStr base = str::DupTemp(word, n - 2);
        AppendLookupCandidate(candidates, base);
        if (n > 5 && word[n - 3] == word[n - 4]) {
            base = str::DupTemp(word, n - 3);
            AppendLookupCandidate(candidates, base);
        }
        base = str::FormatTemp("%.*se", (int)n - 2, word);
        AppendLookupCandidate(candidates, base);
    }
    if (str::EndsWith(word, "es") && n > 4) {
        TempStr base = str::DupTemp(word, n - 2);
        AppendLookupCandidate(candidates, base);
    }
    if (str::EndsWith(word, "s") && n > 3) {
        TempStr base = str::DupTemp(word, n - 1);
        AppendLookupCandidate(candidates, base);
    }
    if (str::EndsWith(word, "ier") && n > 4) {
        TempStr base = str::FormatTemp("%.*sy", (int)n - 3, word);
        AppendLookupCandidate(candidates, base);
    }
    if (str::EndsWith(word, "iest") && n > 5) {
        TempStr base = str::FormatTemp("%.*sy", (int)n - 4, word);
        AppendLookupCandidate(candidates, base);
    }
    if (str::EndsWith(word, "er") && n > 4) {
        AppendLookupCandidate(candidates, str::DupTemp(word, n - 2));
        if (word[n - 2] == 'e') {
            AppendLookupCandidate(candidates, str::DupTemp(word, n - 1));
        }
        if (n > 5 && word[n - 3] == word[n - 4]) {
            AppendLookupCandidate(candidates, str::DupTemp(word, n - 3));
        }
    }
    if (str::EndsWith(word, "est") && n > 5) {
        AppendLookupCandidate(candidates, str::DupTemp(word, n - 3));
        if (word[n - 3] == 'e') {
            AppendLookupCandidate(candidates, str::DupTemp(word, n - 2));
        }
        if (n > 6 && word[n - 4] == word[n - 5]) {
            AppendLookupCandidate(candidates, str::DupTemp(word, n - 4));
        }
    }
}

static char* NormalizeLookupWord(const char* word) {
    char* normalized = str::Dup(word);
    str::TrimWSInPlace(normalized, str::TrimOpt::Both);
    str::ToLowerInPlace(normalized);
    return normalized;
}

static bool IsChineseLookupWord(const char* word) {
    if (str::IsEmpty(word)) {
        return false;
    }
    const char* p = word;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) {
            return false;
        }
        int len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        for (int i = 1; i < len; i++) {
            if (!p[i] || ((unsigned char)p[i] & 0xC0) != 0x80) {
                return false;
            }
        }
        if (len < 3) {
            return false;
        }
        p += len;
    }
    return p > word;
}

static DictLookupHit FindBestChineseDictLookupHit(const char* word, char** matchedWordOut) {
    DictLookupHit hit;
    if (!gOfflineDictZh.loaded) {
        return hit;
    }
    size_t n = str::Len(word);
    size_t maxBytes = (size_t)kMaxChineseLookupWindowChars * 3;
    if (n > maxBytes) {
        n = maxBytes;
    }
    for (size_t len = n; len >= 3; len -= 3) {
        TempStr candidate = str::DupTemp(word, len);
        hit.entry = FindDictIndexEntryIn(&gOfflineDictZh, candidate);
        if (hit.entry) {
            hit.dict = &gOfflineDictZh;
            *matchedWordOut = str::Dup(candidate);
            return hit;
        }
    }
    return hit;
}

static DictLookupHit FindBestDictLookupHit(const char* word, char** matchedWordOut) {
    if (IsChineseLookupWord(word)) {
        char* trimmed = str::Dup(word);
        str::TrimWSInPlace(trimmed, str::TrimOpt::Both);
        defer {
            str::Free(trimmed);
        };
        return FindBestChineseDictLookupHit(trimmed, matchedWordOut);
    }

    char* normalized = NormalizeLookupWord(word);
    defer {
        str::Free(normalized);
    };
    StrVec candidates;
    AppendLookupCandidate(candidates, normalized);
    AddIrregularPluralFallbacks(candidates, normalized);
    AddIrregularVerbFallbacks(candidates, normalized);
    AddInflectionFallbacks(candidates, normalized);
    DictLookupHit hit;
    for (int i = 0; i < candidates.Size(); i++) {
        char* candidate = candidates.At(i);
        hit = FindDictLookupHit(candidate);
        if (hit.entry) {
            *matchedWordOut = str::Dup(candidate);
            break;
        }
    }
    candidates.Reset();
    return hit;
}

static char* ReadFileSliceTemp(const char* path, u64 offset, u32 size) {
    FILE* f = file::OpenFILE(path);
    if (!f) {
        return nullptr;
    }
    defer {
        fclose(f);
    };
    if (_fseeki64(f, (i64)offset, SEEK_SET) != 0) {
        return nullptr;
    }
    char* data = AllocArrayTemp<char>((size_t)size + 1);
    size_t nRead = fread(data, 1, size, f);
    if (nRead != size) {
        return nullptr;
    }
    data[size] = 0;
    return data;
}

static u8* ReadAudioSlice(const OfflineDictionary* dict, u64 offset, u32 size) {
    if (!dict || !dict->audioPath || size == 0) {
        return nullptr;
    }
    FILE* f = file::OpenFILE(dict->audioPath);
    if (!f) {
        return nullptr;
    }
    defer {
        fclose(f);
    };
    if (_fseeki64(f, (i64)offset, SEEK_SET) != 0) {
        return nullptr;
    }
    u8* data = AllocArray<u8>(size);
    size_t nRead = fread(data, 1, size, f);
    if (nRead != size) {
        free(data);
        return nullptr;
    }
    return data;
}

static int ReadLineField(char*& p, char* end, char** out) {
    if (p >= end) {
        *out = nullptr;
        return 0;
    }
    char* start = p;
    while (p < end && *p != '\n') {
        p++;
    }
    char saved = *p;
    *p = 0;
    str::TrimWSInPlace(start, str::TrimOpt::Right);
    *out = UnescapeDictField(start);
    *p = saved;
    if (p < end) {
        p++;
    }
    return 1;
}

static int ReadLineInt(char*& p, char* end) {
    char* s = nullptr;
    ReadLineField(p, end, &s);
    int n = atoi(s ? s : "0");
    str::Free(s);
    return n;
}

static bool ParseDefinitionLine(char* line, DictDefinition* def) {
    char* tab = strchr(line, '\t');
    if (tab) {
        *tab = 0;
        def->en = UnescapeDictField(line);
        def->zh = UnescapeDictField(tab + 1);
    } else {
        def->en = UnescapeDictField(line);
    }
    return def->en || def->zh;
}

static bool LineLooksLikeDefCount(const char* line) {
    if (str::IsEmpty(line)) {
        return false;
    }
    for (const char* p = line; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

static bool IsPosAbbrevLabel(const char* label) {
    if (str::IsEmpty(label)) {
        return false;
    }
    if (str::EqI(label, "n.") || str::EqI(label, "v.") || str::EqI(label, "adj.") || str::EqI(label, "adv.") ||
        str::EqI(label, "pron.") || str::EqI(label, "prep.") || str::EqI(label, "conj.") ||
        str::EqI(label, "interj.") || str::EqI(label, "m.") || str::EqI(label, "part.") || str::EqI(label, "def.")) {
        return true;
    }
    // single-character Chinese part-of-speech markers from the dictionary
    if (str::Len(label) <= 4) {
        const char* p = label;
        if ((unsigned char)*p >= 0x80) {
            return str::Eq(label, "名") || str::Eq(label, "动") || str::Eq(label, "形") || str::Eq(label, "副") ||
                   str::Eq(label, "代") || str::Eq(label, "介") || str::Eq(label, "连") || str::Eq(label, "叹") ||
                   str::Eq(label, "量") || str::Eq(label, "助");
        }
    }
    return false;
}

static const char* LookupChinesePinyin(DictSense* sense) {
    if (!sense) {
        return nullptr;
    }
    if (sense->ipa && sense->ipa[0]) {
        return sense->ipa;
    }
    // older Chinese imports stored pinyin in the tab label for polyphones
    if (sense->label && !IsPosAbbrevLabel(sense->label)) {
        return sense->label;
    }
    return nullptr;
}

static TempStr LookupPhoneticLineTemp(DictSense* sense, const char* title) {
    if (!sense || str::IsEmpty(title)) {
        return nullptr;
    }
    if (IsChineseLookupWord(title)) {
        const char* py = LookupChinesePinyin(sense);
        return py ? str::DupTemp(py) : nullptr;
    }
    if (sense->ipa && sense->ipa[0]) {
        return str::FormatTemp("/%s/", sense->ipa);
    }
    return nullptr;
}

static DictSense* ParseDictEntry(const char* data, int* nSensesOut) {
    *nSensesOut = 0;
    if (str::IsEmpty(data) || !str::StartsWith(data, "SDICT1\n")) {
        return nullptr;
    }
    char* copy = str::Dup(data);
    defer {
        str::Free(copy);
    };
    char* p = copy + str::Len("SDICT1\n");
    char* end = copy + str::Len(copy);
    char* headword = nullptr;
    char* ipa = nullptr;
    ReadLineField(p, end, &headword);
    ReadLineField(p, end, &ipa);
    int nSenses = ReadLineInt(p, end);
    if (nSenses <= 0 || nSenses > kMaxLookupTabs) {
        str::Free(headword);
        str::Free(ipa);
        return nullptr;
    }
    auto* senses = AllocArray<DictSense>(nSenses);
    for (int i = 0; i < nSenses; i++) {
        DictSense* s = &senses[i];
        s->headword = str::Dup(headword);
        s->ipa = str::Dup(ipa);
        ReadLineField(p, end, &s->label);
        ReadLineField(p, end, &s->fl);
        char* pinyinOrCount = nullptr;
        ReadLineField(p, end, &pinyinOrCount);
        int nDefsTotal = 0;
        if (LineLooksLikeDefCount(pinyinOrCount)) {
            nDefsTotal = str::IsEmpty(pinyinOrCount) ? 0 : atoi(pinyinOrCount);
        } else {
            str::Free(s->ipa);
            s->ipa = str::Dup(pinyinOrCount);
            nDefsTotal = ReadLineInt(p, end);
        }
        str::Free(pinyinOrCount);
        for (int j = 0; j < nDefsTotal; j++) {
            char* line = nullptr;
            ReadLineField(p, end, &line);
            if (line && j < kMaxLookupDefs) {
                DictDefinition def;
                ParseDefinitionLine(line, &def);
                s->definitions.Append(def);
            }
            str::Free(line);
        }
        ReadLineField(p, end, &s->exampleEn);
        ReadLineField(p, end, &s->exampleZh);
    }
    str::Free(headword);
    str::Free(ipa);
    *nSensesOut = nSenses;
    return senses;
}

struct LookupResult {
    char* word = nullptr;
    DictSense* senses = nullptr;
    int nSenses = 0;
    bool ok = false;
    OfflineDictionary* audioDict = nullptr;
    u64 audioOffset = 0;
    u32 audioSize = 0;
    char* audioExt = nullptr;
};

static LookupResult* FetchWordLookup(const char* word) {
    auto* res = new LookupResult();
    res->word = str::Dup(word);

    char* matchedWord = nullptr;
    DictLookupHit hit = FindBestDictLookupHit(word, &matchedWord);
    defer {
        str::Free(matchedWord);
    };
    if (!hit.entry || !hit.dict) {
        return res;
    }
    char* entryData = ReadFileSliceTemp(hit.dict->dataPath, hit.entry->dataOffset, hit.entry->dataSize);
    if (!entryData) {
        return res;
    }
    res->senses = ParseDictEntry(entryData, &res->nSenses);
    if (res->senses && res->nSenses > 0) {
        bool hasAudio = hit.dict != &gOfflineDictZh && hit.entry->audioSize > 0 && !str::IsEmpty(hit.entry->audioExt);
        if (hasAudio) {
            res->audioDict = hit.dict;
            res->audioOffset = hit.entry->audioOffset;
            res->audioSize = hit.entry->audioSize;
            res->audioExt = str::Dup(hit.entry->audioExt);
            for (int i = 0; i < res->nSenses; i++) {
                res->senses[i].hasAudio = true;
            }
        }
        str::ReplaceWithCopy(&res->word, res->senses[0].headword ? res->senses[0].headword : word);
        res->ok = true;
    }
    return res;
}

static int CalcTextDy(HDC hdc, HFONT font, const char* txt, int dx, UINT fmt) {
    if (str::IsEmpty(txt)) {
        return 0;
    }
    HFONT oldFont = (HFONT)SelectObject(hdc, font ? font : GetAppFont());
    TempWStr txtW = ToWStrTemp(txt);
    RECT r{0, 0, dx, 0};
    DrawTextW(hdc, txtW, -1, &r, fmt | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    return RectDy(r);
}

static int CalcFontLineDy(HDC hdc, HFONT font) {
    HFONT oldFont = (HFONT)SelectObject(hdc, font ? font : GetAppFont());
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, oldFont);
    return tm.tmHeight + tm.tmExternalLeading;
}

static int DrawLookupText(HDC hdc, HFONT font, const char* txt, RECT* r, COLORREF col, UINT fmt) {
    if (str::IsEmpty(txt)) {
        return 0;
    }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, col);
    HFONT oldFont = (HFONT)SelectObject(hdc, font ? font : GetAppFont());
    TempWStr txtW = ToWStrTemp(txt);
    RECT calc = *r;
    DrawTextW(hdc, txtW, -1, &calc, fmt | DT_CALCRECT | DT_NOPREFIX);
    DrawTextW(hdc, txtW, -1, r, fmt | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    return RectDy(calc);
}

static int LookupTabCount(WordLookupWnd* wnd) {
    if (!wnd || wnd->isLoading || wnd->nSenses <= 1) {
        return 0;
    }
    return std::min(wnd->nSenses, kMaxLookupTabs);
}

static int CalcLookupTabsDy(HWND hwnd, HDC hdc, WordLookupWnd* wnd) {
    if (LookupTabCount(wnd) == 0) {
        return 0;
    }
    int lineDy = CalcFontLineDy(hdc, wnd->font);
    return lineDy + DpiScale(hwnd, 7);
}

static void ClearLookupTabRects(WordLookupWnd* wnd) {
    for (int i = 0; i < kMaxLookupTabs; i++) {
        wnd->tabRects[i] = Rect();
    }
}

static COLORREF LookupTabBgColor(bool selected, bool hover) {
    if (selected) {
        return ThemeUsesDarkChrome() ? AccentColor(ThemeWindowControlBackgroundColor(), 25) : RGB(168, 182, 198);
    }
    if (hover) {
        return FloatingPopupHoverBg(FloatingPopupBg());
    }
    return ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor() : AccentColor(FloatingPopupBg(), 4);
}

static COLORREF LookupTabBorderColor(bool selected) {
    if (selected) {
        return ThemeUsesDarkChrome() ? AccentColor(ThemeWindowLinkColor(), -20) : RGB(148, 162, 178);
    }
    return FloatingPopupSeparatorColor();
}

static COLORREF LookupTabTextColor(bool selected) {
    if (selected) {
        return ThemeUsesDarkChrome() ? FloatingPopupAccentColor() : RGB(48, 62, 80);
    }
    return FloatingPopupMutedTextColor();
}

static int DrawLookupTabs(HWND hwnd, HDC dc, WordLookupWnd* wnd, int x, int y, int right) {
    ClearLookupTabRects(wnd);
    int nTabs = LookupTabCount(wnd);
    if (nTabs == 0) {
        return 0;
    }

    int tabDy = CalcLookupTabsDy(hwnd, dc, wnd);
    int gap = DpiScale(hwnd, 6);
    int padX = DpiScale(hwnd, 9);
    int minDx = DpiScale(hwnd, 34);
    int radius = DpiScale(hwnd, 8);
    int currX = x;

    HFONT oldFont = (HFONT)SelectObject(dc, wnd->font ? wnd->font : GetAppFont());
    for (int i = 0; i < nTabs; i++) {
        const char* label = wnd->senses[i].label ? wnd->senses[i].label : "def.";
        TempWStr labelW = ToWStrTemp(label);
        SIZE labelSz{};
        GetTextExtentPoint32W(dc, labelW, str::Len(labelW), &labelSz);
        int tabDx = (int)labelSz.cx + 2 * padX;
        if (tabDx < minDx) {
            tabDx = minDx;
        }
        if (currX + tabDx > right) {
            break;
        }

        Rect tabRc(currX, y, tabDx, tabDy);
        bool selected = i == wnd->currTab;
        bool hover = i == wnd->tabHover;
        FillFloatingPopupRoundedRect(dc, tabRc, radius, LookupTabBgColor(selected, hover));
        StrokeFloatingPopupRoundedRect(dc, tabRc, radius, LookupTabBorderColor(selected));

        RECT textR{tabRc.x + padX, tabRc.y, tabRc.x + tabRc.dx - padX, tabRc.y + tabRc.dy};
        COLORREF textCol = LookupTabTextColor(selected);
        DrawLookupText(dc, wnd->font, label, &textR, textCol, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);

        wnd->tabRects[i] = tabRc;
        currX += tabDx + gap;
        if (currX >= right) {
            break;
        }
    }
    SelectObject(dc, oldFont);
    return tabDy;
}

static int HitTestLookupTab(WordLookupWnd* wnd, Point pt) {
    int nTabs = LookupTabCount(wnd);
    for (int i = 0; i < nTabs; i++) {
        if (wnd->tabRects[i].Contains(pt)) {
            return i;
        }
    }
    return -1;
}

static void EnsureSpeakerAnimTimer(WordLookupWnd* wnd) {
    if (!wnd || !wnd->hwnd) {
        return;
    }
    float hoverTarget = wnd->speakerBtnHover ? 1.f : 0.f;
    bool hoverAnimating = wnd->speakerHoverAnim != hoverTarget;
    if (!hoverAnimating && !wnd->speakerPlaying && !wnd->isLoading) {
        KillTimer(wnd->hwnd, kSpeakerHoverTimerId);
        return;
    }
    SetTimer(wnd->hwnd, kSpeakerHoverTimerId, kSpeakerHoverTimerMs, nullptr);
}

static Gdiplus::Color LookupGdipColor(COLORREF col, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(col), GetGValue(col), GetBValue(col));
}

static void DrawLookupSpeakerIcon(HWND hwnd, HDC dc, const Rect& rc, bool audioReady, float hoverT, bool playing,
                                  int waveTick) {
    COLORREF col = audioReady ? FloatingPopupAccentColor() : FloatingPopupMutedTextColor();
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPageUnit(Gdiplus::UnitPixel);
    if (hoverT > 0.f) {
        COLORREF bgCol = BlendFloatingPopupColors(FloatingPopupBg(), LookupSpeakerHoverBgColor(), hoverT);
        Gdiplus::SolidBrush br(LookupGdipColor(bgCol));
        g.FillEllipse(&br, (Gdiplus::REAL)rc.x, (Gdiplus::REAL)rc.y, (Gdiplus::REAL)(rc.dx - 1),
                      (Gdiplus::REAL)(rc.dy - 1));
    }
    int pad = DpiScale(hwnd, 5);
    Rect ir = rc;
    ir.SubLR(pad, pad);
    ir.SubTB(pad, pad);
    Gdiplus::REAL penW = (Gdiplus::REAL)DpiScale(hwnd, 1) * 1.5f;
    Gdiplus::Pen pen(LookupGdipColor(col), penW);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    float sx = (float)ir.dx / 24.0f;
    float sy = (float)ir.dy / 24.0f;
    auto px = [&](float v) { return (float)ir.x + v * sx; };
    auto py = [&](float v) { return (float)ir.y + v * sy; };

    Gdiplus::GraphicsPath speaker;
    speaker.AddLine(px(5), py(9), px(9), py(9));
    speaker.AddLine(px(9), py(9), px(14), py(5));
    speaker.AddLine(px(14), py(5), px(14), py(19));
    speaker.AddLine(px(14), py(19), px(9), py(15));
    speaker.AddLine(px(9), py(15), px(5), py(15));
    speaker.AddLine(px(5), py(15), px(5), py(9));
    g.DrawPath(&pen, &speaker);

    if (audioReady) {
        struct WaveSpec {
            float cx;
            float cy;
            float w;
            float h;
        };
        static const WaveSpec kWaves[] = {
            {15.f, 9.f, 4.f, 6.f},
            {17.f, 7.f, 5.f, 10.f},
            {18.5f, 5.5f, 6.5f, 12.f},
        };
        int waveCount = 2;
        if (playing) {
            waveCount = (waveTick / kSpeakerWaveCycleFrames) % 3 + 1;
        }
        for (int i = 0; i < waveCount; i++) {
            const WaveSpec& wv = kWaves[i];
            g.DrawArc(&pen, px(wv.cx), py(wv.cy), sx * wv.w, sy * wv.h, -45, 90);
        }
    }
}

static DictSense* CurrentSense(WordLookupWnd* wnd) {
    if (!wnd || wnd->currTab < 0 || wnd->currTab >= wnd->nSenses || !wnd->senses) {
        return nullptr;
    }
    return &wnd->senses[wnd->currTab];
}

static HFONT LookupTitleFont(WordLookupWnd* wnd) {
    return wnd->headwordFont ? wnd->headwordFont : (wnd->font ? wnd->font : GetAppFont());
}

static bool LookupShowsSpeaker(WordLookupWnd* wnd, DictSense* sense) {
    if (!wnd || IsChineseLookupWord(wnd->queryWord)) {
        return false;
    }
    return wnd->isLoading || HasAudioMeta(sense);
}

static int CalcLookupTitleRowDy(HWND hwnd, HDC hdc, WordLookupWnd* wnd, HFONT titleFont, DictSense* sense) {
    int pad = DpiScale(hwnd, kCardPad);
    int contentDx = DpiScale(hwnd, kPopupDx) - 2 * pad;
    int iconSz = DpiScale(hwnd, 22);
    bool showSpeaker = LookupShowsSpeaker(wnd, sense);
    const char* title = (sense && sense->headword) ? sense->headword : wnd->queryWord;
    int titleMaxDx = contentDx - DpiScale(hwnd, kCloseBtnSz + 14);
    if (showSpeaker) {
        titleMaxDx -= iconSz + DpiScale(hwnd, 8);
    }
    int titleLineDy = CalcFontLineDy(hdc, titleFont);
    int titleDy = CalcTextDy(hdc, titleFont, title, titleMaxDx, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    int rowDy = showSpeaker ? std::max(titleLineDy, iconSz) : titleLineDy;
    rowDy = std::max(rowDy, titleDy);
    return rowDy + DpiScale(hwnd, 2);
}

static int CalcLookupWindowDy(WordLookupWnd* wnd) {
    HWND hwnd = wnd->hwnd;
    HDC hdc = GetDC(hwnd);
    int pad = DpiScale(hwnd, kCardPad);
    int padTop = DpiScale(hwnd, kCardPadTop);
    int padBottom = DpiScale(hwnd, kCardPadBottom);
    int contentDx = DpiScale(hwnd, kPopupDx) - 2 * pad;
    int dy = padTop;
    int lineDy = CalcFontLineDy(hdc, wnd->font);
    DictSense* sense = CurrentSense(wnd);
    HFONT titleFont = LookupTitleFont(wnd);

    dy += CalcLookupTitleRowDy(hwnd, hdc, wnd, titleFont, sense);
    TempStr phoneticLine = LookupPhoneticLineTemp(sense, wnd->queryWord);
    if (phoneticLine) {
        dy += CalcTextDy(hdc, wnd->font, phoneticLine, contentDx, DT_SINGLELINE) + DpiScale(hwnd, 4);
    }
    int tabsDy = CalcLookupTabsDy(hwnd, hdc, wnd);
    if (tabsDy > 0) {
        dy += tabsDy + DpiScale(hwnd, 6);
    }
    if (sense && sense->fl && LookupTabCount(wnd) == 0) {
        dy += CalcTextDy(hdc, wnd->posFont ? wnd->posFont : wnd->font, AbbreviateFl(sense->fl), contentDx,
                         DT_SINGLELINE) +
              DpiScale(hwnd, 8);
    }

    if (wnd->isLoading) {
        TempStr lookingUpText = LookingUpTextTemp(2);
        dy += CalcTextDy(hdc, wnd->font, lookingUpText, contentDx, DT_WORDBREAK | DT_EDITCONTROL) + DpiScale(hwnd, 4);
    } else if (!sense) {
        TempStr noDefText = str::FormatTemp(_TRA("No definition for \"%s\"."), wnd->queryWord);
        dy += CalcTextDy(hdc, wnd->font, noDefText, contentDx, DT_WORDBREAK | DT_EDITCONTROL) + lineDy;
    } else {
        char* defs = BuildDefinitionsText(sense);
        dy += CalcTextDy(hdc, wnd->font, defs, contentDx, DT_WORDBREAK | DT_EDITCONTROL) + lineDy;
        str::Free(defs);
        char* example = BuildExampleText(sense);
        if (example) {
            dy += DpiScale(hwnd, 22);
            dy += std::min(CalcTextDy(hdc, wnd->font, example, contentDx, DT_WORDBREAK | DT_EDITCONTROL),
                           DpiScale(hwnd, kExampleMaxDy));
            str::Free(example);
        }
    }
    dy += padBottom;
    ReleaseDC(hwnd, hdc);
    dy = std::max(dy, DpiScale(hwnd, sense ? 112 : 88));
    dy = std::min(dy, DpiScale(hwnd, kPaintMaxContentDy));
    return dy;
}

static void ResizeLookupForPaint(WordLookupWnd* wnd) {
    int dx = DpiScale(wnd->hwnd, kPopupDx);
    int dy = CalcLookupWindowDy(wnd);
    SetWindowPos(wnd->hwnd, nullptr, 0, 0, dx, dy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static bool PlayLookupAudio(WordLookupWnd* wnd) {
    if (!wnd || !wnd->audioDict || wnd->audioSize == 0) {
        return false;
    }
    u8* data = ReadAudioSlice(wnd->audioDict, wnd->audioOffset, wnd->audioSize);
    if (!data) {
        return false;
    }
    return LookupAudioPlayOwned(data, wnd->audioSize, wnd->audioExt);
}

void WordLookupWnd::LayoutCloseButton() {
    Rect card = LookupCardRect(hwnd);
    int pad = DpiScale(hwnd, kCardPad);
    int padTop = DpiScale(hwnd, kCardPadTop);
    int sz = DpiScale(hwnd, kCloseBtnSz);
    closeBtnPos = Rect(card.x + card.dx - pad - sz, card.y + padTop, sz, sz);
}

void WordLookupWnd::UpdateChrome() {
    LayoutCloseButton();
    UpdateLookupWindowRgn(this);
    HwndScheduleRepaint(hwnd);
}

void WordLookupWnd::PlayAudio() {
    if (currTab < 0 || currTab >= nSenses) {
        return;
    }
    DictSense* sense = &senses[currTab];
    if (!IsAudioReady(sense)) {
        return;
    }
    if (PlayLookupAudio(this)) {
        speakerPlaying = true;
        speakerWaveTick = 0;
        speakerPlayFrames = kSpeakerPlayMinFrames;
        EnsureSpeakerAnimTimer(this);
        HwndScheduleRepaint(hwnd);
    }
}

void WordLookupWnd::SelectTab(int tab) {
    if (tab < 0 || tab >= nSenses || tab == currTab) {
        return;
    }
    StopCurrentLookupAudio();
    speakerPlaying = false;
    speakerWaveTick = 0;
    speakerPlayFrames = 0;
    currTab = tab;
    tabHover = -1;
    ResizeLookupForPaint(this);
    PositionWordLookup(this, anchorPos);
    HwndScheduleRepaint(hwnd);
}

void WordLookupWnd::SetLoading(const char* word) {
    isLoading = true;
    speakerPlaying = false;
    speakerWaveTick = 0;
    speakerPlayFrames = 0;
    loadingDotTick = 0;
    audioDict = nullptr;
    audioOffset = 0;
    audioSize = 0;
    str::Free(audioExt);
    audioExt = nullptr;
    str::ReplaceWithCopy(&queryWord, word);
    currTab = 0;
    tabHover = -1;
    ClearLookupTabRects(this);
    ResizeLookupForPaint(this);
    PositionWordLookup(this, anchorPos);
    EnsureSpeakerAnimTimer(this);
    HwndScheduleRepaint(hwnd);
}

void WordLookupWnd::SetResults(const char* word, DictSense* newSenses, int newNSenses, OfflineDictionary* newAudioDict,
                               u64 newAudioOffset, u32 newAudioSize, const char* newAudioExt) {
    isLoading = false;
    str::ReplaceWithCopy(&queryWord, word);
    if (senses) {
        for (int i = 0; i < nSenses; i++) {
            FreeDictSense(&senses[i]);
        }
        free(senses);
    }
    senses = newSenses;
    nSenses = newNSenses;
    audioDict = newAudioDict;
    audioOffset = newAudioOffset;
    audioSize = newAudioSize;
    str::ReplaceWithCopy(&audioExt, newAudioExt);
    currTab = 0;
    tabHover = -1;
    ClearLookupTabRects(this);

    ResizeLookupForPaint(this);
    PositionWordLookup(this, anchorPos);
    HwndScheduleRepaint(hwnd);
}

static void PositionWordLookup(WordLookupWnd* wnd, Point screenPos) {
    HWND hwnd = wnd->hwnd;
    if (!hwnd) {
        return;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint({screenPos.x, screenPos.y}, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    Rect work = ToRect(mi.rcWork);
    if (wnd->win && wnd->win->hwndCanvas) {
        Rect canvas = WindowRect(wnd->win->hwndCanvas);
        int x1 = std::max(work.x, canvas.x);
        int y1 = std::max(work.y, canvas.y);
        int x2 = std::min(work.x + work.dx, canvas.x + canvas.dx);
        int y2 = std::min(work.y + work.dy, canvas.y + canvas.dy);
        if (x2 > x1 && y2 > y1) {
            work = Rect(x1, y1, x2 - x1, y2 - y1);
        }
    }

    Rect rc = WindowRect(hwnd);
    int dx = rc.dx;
    int dy = rc.dy;
    int gap = DpiScale(hwnd, kPopupGap);

    bool showBelow = screenPos.y + gap + dy <= work.y + work.dy;
    if (!showBelow && screenPos.y - gap - dy < work.y) {
        int belowSpace = work.y + work.dy - screenPos.y;
        int aboveSpace = screenPos.y - work.y;
        showBelow = belowSpace >= aboveSpace;
    }

    int x = screenPos.x - DpiScale(hwnd, 42);
    int y = showBelow ? screenPos.y + gap : screenPos.y - dy - gap;
    if (x + dx > work.x + work.dx) {
        x = work.x + work.dx - dx;
    }
    if (x < work.x) {
        x = work.x;
    }
    if (y + dy > work.y + work.dy) {
        y = work.y + work.dy - dy;
    }
    if (y < work.y) {
        y = work.y;
    }

    wnd->anchorPos = screenPos;
    uint flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, flags);
    wnd->UpdateChrome();
}

bool WordLookupWnd::Create(MainWindow* winIn, const char* word, Point screenPos) {
    win = winIn;
    font = CreateScaledFontFrom(GetAppFont(), 132);
    if (font) {
        fontOwned = font != GetAppFont();
    } else {
        font = GetAppFont();
    }
    headwordFont = CreateScaledFontFrom(GetAppBiggerFont(), 115);
    if (headwordFont) {
        headwordFontOwned = headwordFont != GetAppBiggerFont();
    } else {
        headwordFont = GetAppBiggerFont();
    }
    posFont = CreateBoldFontFrom(font);

    COLORREF colTxt = FloatingPopupTextColor();
    COLORREF colBg = FloatingPopupBg();
    CreateCustomArgs args;
    args.visible = false;
    args.style = WS_POPUP;
    args.exStyle = WS_EX_TOOLWINDOW;
    args.font = font;
    args.bgColor = colBg;
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    SetColors(colTxt, colBg);

    anchorPos = screenPos;
    SetLoading(word);
    if (UseDarkModeLib() && ThemeUsesDarkChrome()) {
        DarkMode::removeWindowCtlColorSubclass(hwnd);
        DarkMode::removeWindowEraseBgSubclass(hwnd);
    }
    visibility = Visibility::Visible;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void WordLookupWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    DoubleBuffer buffer(hwnd, ToRect(ps->rcPaint));
    HDC dc = buffer.GetDC();
    COLORREF colBg = FloatingPopupBg();
    COLORREF borderCol = FloatingPopupBorderColor();
    Rect card = LookupCardRect(hwnd);
    int radius = DpiScale(hwnd, kFloatingPopupCornerRadius);

    COLORREF outerBg = ThemeWindowBackgroundColor();
    HBRUSH outerBr = CreateSolidBrush(outerBg);
    FillRect(dc, &ps->rcPaint, outerBr);
    DeleteObject(outerBr);

    FillFloatingPopupRoundedRect(dc, card, radius, colBg);
    Gdiplus::Graphics borderG(dc);
    borderG.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen borderPen(LookupGdipColor(borderCol), 1);
    Gdiplus::GraphicsPath borderPath;
    borderPath.AddArc(card.x, card.y, radius, radius, 180, 90);
    borderPath.AddArc(card.x + card.dx - radius - 1, card.y, radius, radius, 270, 90);
    borderPath.AddArc(card.x + card.dx - radius - 1, card.y + card.dy - radius - 1, radius, radius, 0, 90);
    borderPath.AddArc(card.x, card.y + card.dy - radius - 1, radius, radius, 90, 90);
    borderPath.CloseFigure();
    borderG.DrawPath(&borderPen, &borderPath);

    int pad = DpiScale(hwnd, kCardPad);
    int padTop = DpiScale(hwnd, kCardPadTop);
    int padBottom = DpiScale(hwnd, kCardPadBottom);
    int x = card.x + pad;
    int y = card.y + padTop;
    int right = card.x + card.dx - pad;
    int topRight = right - DpiScale(hwnd, kCloseBtnSz + 14);
    int bottom = card.y + card.dy - padBottom;
    DictSense* sense = CurrentSense(this);
    const char* title = sense && sense->headword ? sense->headword : queryWord;
    HFONT titleFont = LookupTitleFont(this);
    bool audioReady = !isLoading && sense && IsAudioReady(sense);
    bool showSpeaker = LookupShowsSpeaker(this, sense);

    int iconSz = DpiScale(hwnd, 22);
    int titleLineDy = CalcFontLineDy(dc, titleFont);
    int titleRowDy = showSpeaker ? std::max(titleLineDy, iconSz) : titleLineDy;
    int titleRight = showSpeaker ? topRight - iconSz - DpiScale(hwnd, 8) : topRight;
    RECT titleR{x, y, titleRight, y + titleRowDy};
    int titleDy = DrawLookupText(dc, titleFont, title, &titleR, FloatingPopupTextColor(),
                                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SIZE titleSz{};
    HFONT oldTitleFont = (HFONT)SelectObject(dc, titleFont ? titleFont : font);
    const char* titleForMeasure = title ? title : "";
    TempWStr titleForMeasureW = ToWStrTemp(titleForMeasure);
    GetTextExtentPoint32W(dc, titleForMeasureW, str::Len(titleForMeasureW), &titleSz);
    SelectObject(dc, oldTitleFont);
    int maxTitleDx = (int)(titleR.right - titleR.left);
    int titleDx = (int)titleR.left + std::min((int)titleSz.cx, maxTitleDx);
    if (showSpeaker) {
        int speakerX = titleDx + DpiScale(hwnd, 6);
        int maxSpeakerX = topRight - iconSz - DpiScale(hwnd, 2);
        if (speakerX > maxSpeakerX) {
            speakerX = maxSpeakerX;
        }
        int speakerY = y + (titleRowDy - iconSz) / 2 + DpiScale(hwnd, 1);
        speakerBtnPos = Rect(speakerX, speakerY, iconSz, iconSz);
        speakerBtnPos.Inflate(DpiScale(hwnd, 3), DpiScale(hwnd, 3));
        DrawLookupSpeakerIcon(hwnd, dc, speakerBtnPos, audioReady, speakerHoverAnim, speakerPlaying, speakerWaveTick);
    }
    y += std::max(std::max(titleDy, titleRowDy), titleLineDy) + DpiScale(hwnd, 2);

    TempStr phoneticLine = LookupPhoneticLineTemp(sense, title ? title : "");
    if (phoneticLine && y < bottom) {
        RECT r{x, y, right, bottom};
        y +=
            DrawLookupText(dc, font, phoneticLine, &r, FloatingPopupMutedTextColor(), DT_SINGLELINE | DT_END_ELLIPSIS) +
            DpiScale(hwnd, 4);
    }

    int tabsDy = DrawLookupTabs(hwnd, dc, this, x, y, right);
    if (tabsDy > 0) {
        y += tabsDy + DpiScale(hwnd, 6);
    }

    if (sense && sense->fl && y < bottom && LookupTabCount(this) == 0) {
        RECT r{x, y, right, bottom};
        y += DrawLookupText(dc, posFont ? posFont : font, AbbreviateFl(sense->fl), &r, FloatingPopupAccentColor(),
                            DT_SINGLELINE | DT_END_ELLIPSIS) +
             DpiScale(hwnd, 8);
    }

    const char* statusText = nullptr;
    TempStr lookingUpText;
    if (isLoading) {
        int dotPhase = (loadingDotTick / kLoadingDotCycleFrames) % 3;
        lookingUpText = LookingUpTextTemp(dotPhase);
        statusText = lookingUpText;
    } else if (!sense) {
        statusText = str::FormatTemp(_TRA("No definition for \"%s\"."), queryWord);
    }
    if (statusText && y < bottom) {
        RECT r{x, y, right, bottom};
        DrawLookupText(dc, font, statusText, &r, FloatingPopupMutedTextColor(), DT_WORDBREAK | DT_EDITCONTROL);
    } else if (sense && y < bottom) {
        char* defs = BuildDefinitionsText(sense);
        if (defs) {
            RECT r{x, y, right, bottom};
            y += DrawLookupText(dc, font, defs, &r, FloatingPopupTextColor(), DT_WORDBREAK | DT_EDITCONTROL);
            str::Free(defs);
        }
        char* example = BuildExampleText(sense);
        if (example && y + DpiScale(hwnd, 28) < bottom) {
            y += DpiScale(hwnd, 11);
            HPEN sepPen = CreatePen(PS_SOLID, 1, FloatingPopupSeparatorColor());
            HGDIOBJ oldSep = SelectObject(dc, sepPen);
            MoveToEx(dc, x, y, nullptr);
            LineTo(dc, right, y);
            SelectObject(dc, oldSep);
            DeleteObject(sepPen);
            y += DpiScale(hwnd, 12);

            RECT r{x, y, right, bottom};
            DrawLookupText(dc, font, example, &r, FloatingPopupMutedTextColor(), DT_WORDBREAK | DT_EDITCONTROL);
        }
        str::Free(example);
    }

    DrawCloseButtonArgs cbArgs;
    cbArgs.hdc = dc;
    cbArgs.r = closeBtnPos;
    cbArgs.isHover = closeBtnHover;
    cbArgs.colX = FloatingPopupMutedTextColor();
    cbArgs.colXHover = FloatingPopupTextColor();
    cbArgs.colHoverBg = FloatingPopupCloseHoverBg(colBg);
    DrawCloseButton(cbArgs);

    buffer.Flush(hdc);
}

LRESULT WordLookupWnd::WndProc(HWND hwndIn, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            UpdateChrome();
            break;
        case WM_MOUSEMOVE: {
            Point pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            bool wasHover = closeBtnHover;
            bool wasSpeakerHover = speakerBtnHover;
            int oldTabHover = tabHover;
            closeBtnHover = closeBtnPos.Contains(pt);
            {
                DictSense* sense = CurrentSense(this);
                speakerBtnHover = speakerBtnPos.Contains(pt) && LookupShowsSpeaker(this, sense);
            }
            tabHover = HitTestLookupTab(this, pt);
            if (closeBtnHover != wasHover || speakerBtnHover != wasSpeakerHover || tabHover != oldTabHover) {
                if (closeBtnHover || speakerBtnHover || tabHover >= 0) {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwndIn;
                    TrackMouseEvent(&tme);
                }
                if (speakerBtnHover != wasSpeakerHover) {
                    EnsureSpeakerAnimTimer(this);
                }
                HwndScheduleRepaint(hwndIn);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            closeBtnHover = false;
            if (speakerBtnHover) {
                speakerBtnHover = false;
                EnsureSpeakerAnimTimer(this);
            } else {
                speakerBtnHover = false;
            }
            tabHover = -1;
            HwndScheduleRepaint(hwndIn);
            return 0;
        case WM_TIMER:
            if (wp == kSpeakerHoverTimerId) {
                float target = speakerBtnHover ? 1.f : 0.f;
                if (speakerHoverAnim < target) {
                    speakerHoverAnim = std::min(1.f, speakerHoverAnim + kSpeakerHoverAnimStep);
                } else if (speakerHoverAnim > target) {
                    speakerHoverAnim = std::max(0.f, speakerHoverAnim - kSpeakerHoverAnimStep);
                }
                if (isLoading) {
                    loadingDotTick++;
                }
                if (speakerPlaying) {
                    speakerWaveTick++;
                    if (speakerPlayFrames > 0) {
                        speakerPlayFrames--;
                    }
                    if (speakerPlayFrames == 0 && !IsLookupAudioPlaying()) {
                        speakerPlaying = false;
                        speakerWaveTick = 0;
                    }
                }
                HwndScheduleRepaint(hwndIn);
                EnsureSpeakerAnimTimer(this);
                return 0;
            }
            break;
        case WM_LBUTTONUP: {
            Point pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (closeBtnPos.Contains(pt)) {
                ScheduleDeleteWordLookupWnd();
                return 0;
            }
            if (speakerBtnPos.Contains(pt)) {
                PlayAudio();
                return 0;
            }
            int tab = HitTestLookupTab(this, pt);
            if (tab >= 0) {
                SelectTab(tab);
                return 0;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                ScheduleDeleteWordLookupWnd();
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (wp == WA_INACTIVE && !isLoading) {
                ScheduleDeleteWordLookupWnd();
                return 0;
            }
            break;
    }
    return WndProcDefault(hwndIn, msg, wp, lp);
}

static bool IsEnglishLookupWord(const char* word) {
    if (str::IsEmpty(word)) {
        return false;
    }
    int nLetters = 0;
    for (const char* p = word; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            nLetters++;
            continue;
        }
        if (c == '\'' || c == '-') {
            continue;
        }
        return false;
    }
    return nLetters > 0;
}

static bool IsLookupWord(const char* word) {
    return IsEnglishLookupWord(word) || IsChineseLookupWord(word);
}

struct LookupFinishData {
    LookupResult* result = nullptr;
    WordLookupWnd* wnd = nullptr;
};

static void OnLookupFinish(LookupFinishData* d) {
    LookupResult* res = d->result;
    WordLookupWnd* lw = d->wnd;
    delete d;

    defer {
        if (res->senses) {
            for (int i = 0; i < res->nSenses; i++) {
                FreeDictSense(&res->senses[i]);
            }
            free(res->senses);
        }
        str::Free(res->word);
        str::Free(res->audioExt);
        delete res;
    };

    if (!gWordLookupWnd || gWordLookupWnd != lw) {
        return;
    }

    if (res->ok) {
        lw->SetResults(res->word, res->senses, res->nSenses, res->audioDict, res->audioOffset, res->audioSize,
                       res->audioExt);
        res->senses = nullptr;
        res->audioExt = nullptr;
    } else {
        lw->SetResults(res->word, nullptr, 0, nullptr, 0, 0, nullptr);
    }
}

struct FetchLookupData {
    char* word = nullptr;
    WordLookupWnd* wnd = nullptr;
};

static void FetchWordLookupAsync(FetchLookupData* d) {
    LookupResult* res = FetchWordLookup(d->word);
    WordLookupWnd* lookupWnd = d->wnd;
    str::Free(d->word);
    delete d;

    auto* finish = new LookupFinishData();
    finish->result = res;
    finish->wnd = lookupWnd;
    auto uiFn = MkFunc0<LookupFinishData>(OnLookupFinish, finish);
    uitask::Post(uiFn, "WordLookupResult");
}

// stext / DrawInstr bboxes use line or em-box height; shrink selection chrome only.
static void TightenWordLookupHighlightBox(RectF* r, int charCount) {
    if (!r || r->dy <= 1.f || charCount <= 0) {
        return;
    }
    float targetDy = r->dy;
    if (r->dx > 0.f) {
        float avgW = r->dx / (float)charCount;
        float capDy = avgW * 1.1f;
        if (capDy >= 2.f && capDy < targetDy) {
            targetDy = capDy;
        }
    }
    float insetDy = r->dy * 0.18f;
    if (insetDy >= 1.f) {
        float fromInset = r->dy - 2.f * insetDy;
        if (fromInset < targetDy) {
            targetDy = fromInset;
        }
    }
    if (targetDy >= r->dy - 0.5f) {
        return;
    }
    float pad = (r->dy - targetDy) * 0.5f;
    r->y += pad;
    r->dy = targetDy;
}

static RectF WordLookupHighlightFromCoords(const Rect* coords, int start, int end) {
    Rect bbox;
    bool any = false;
    for (int i = start; i < end; i++) {
        const Rect& c = coords[i];
        if (!c.x && !c.dx) {
            continue;
        }
        bbox = any ? bbox.Union(c) : c;
        any = true;
    }
    if (!any) {
        return RectF();
    }
    return ToRectF(bbox);
}

bool ShowEbookWordLookupAt(MainWindow* win, DisplayModel* dm, int pageNo, PointF pagePt, Point screenPos) {
    if (!win || !dm) {
        return false;
    }
    TextSelection* ts = dm->textSelection;
    EngineBase* engine = dm->GetEngine();
    if (!ts || !engine || !EngineIsFixedLayoutEbook(engine)) {
        return false;
    }
    if (!AnyOfflineDictionaryLoaded()) {
        return false;
    }

    EbookTextHit hit;
    if (!EngineEbookHitTestText(engine, pageNo, pagePt, &hit)) {
        return false;
    }

    TempWStr run = EngineEbookGetRunTextTemp(engine, pageNo, hit.instrIndex);
    int runLen = run ? str::Leni(run) : 0;
    if (!run || runLen <= 0 || hit.charIndex < 0 || hit.charIndex >= runLen) {
        return false;
    }

    WCHAR clickChar = run[hit.charIndex];
    char* matchedWord = nullptr;
    int matchStart = hit.charIndex;
    int matchEnd = hit.charIndex + 1;

    if (isCjkWordChar(clickChar) && LoadOfflineDictionaryZh()) {
        int winStart = hit.charIndex;
        int winEnd = hit.charIndex + 1;
        int lookback = 0;
        while (winStart > 0 && (winEnd - winStart) < kMaxChineseLookupWindowChars &&
               lookback < kMaxChineseLookupLookbackChars) {
            winStart--;
            lookback++;
        }
        while (winEnd < runLen && (winEnd - winStart) < kMaxChineseLookupWindowChars) {
            winEnd++;
        }
        int winLen = winEnd - winStart;
        int clickOff = hit.charIndex - winStart;
        for (int len = winLen; len >= 1; len--) {
            for (int start = 0; start <= winLen - len; start++) {
                if (start > clickOff || clickOff >= start + len) {
                    continue;
                }
                TempStr candidate = ToUtf8(run + winStart + start, len);
                if (FindDictIndexEntryIn(&gOfflineDictZh, candidate)) {
                    matchedWord = str::Dup(candidate);
                    matchStart = winStart + start;
                    matchEnd = winStart + start + len;
                    len = 0;
                    break;
                }
            }
        }
        if (!matchedWord) {
            matchedWord = ToUtf8(run + hit.charIndex, 1);
            matchStart = hit.charIndex;
            matchEnd = hit.charIndex + 1;
        }
    } else if (isNonCjkWordChar(clickChar)) {
        int ws = hit.charIndex;
        int we = hit.charIndex + 1;
        while (ws > 0 && isNonCjkWordChar(run[ws - 1])) {
            ws--;
        }
        while (we < runLen && isNonCjkWordChar(run[we])) {
            we++;
        }
        matchedWord = ToUtf8(run + ws, we - ws);
        matchStart = ws;
        matchEnd = we;
    } else {
        matchedWord = ToUtf8(run + hit.charIndex, 1);
    }

    if (!matchedWord || !IsLookupWord(matchedWord)) {
        str::Free(matchedWord);
        return false;
    }

    RectF hlBox = hit.bbox;
    if (!EngineEbookGetCharRangeBbox(engine, pageNo, hit.instrIndex, matchStart, matchEnd, &hlBox)) {
        hlBox = hit.bbox;
    }
    TightenWordLookupHighlightBox(&hlBox, matchEnd - matchStart);
    ts->SelectPageBbox(pageNo, hlBox);

    Point anchor = dm->CvtToScreen(pageNo, PointF(hlBox.x + hlBox.dx * 0.5f, hlBox.y + hlBox.dy));
    ShowWordLookup(win, matchedWord, anchor);
    str::Free(matchedWord);
    return true;
}

bool ShowChineseWordLookupAt(MainWindow* win, TextSelection* ts, EngineBase* engine, int pageNo, PointF pagePt,
                             Point screenPos) {
    if (!win || !ts || !engine) {
        return false;
    }
    if (!LoadOfflineDictionaryZh()) {
        return false;
    }

    int clickGlyph = ts->GlyphIndexAt(pageNo, pagePt.x, pagePt.y);
    int textLen = 0;
    const WCHAR* text = engine->GetTextForPage(pageNo, &textLen);
    if (!text || clickGlyph < 0 || clickGlyph >= textLen) {
        return false;
    }
    if (!isCjkWordChar(text[clickGlyph])) {
        return false;
    }

    int runStart = clickGlyph;
    while (runStart > 0 && isCjkWordChar(text[runStart - 1])) {
        runStart--;
    }
    int runEnd = clickGlyph + 1;
    while (runEnd < textLen && isCjkWordChar(text[runEnd])) {
        runEnd++;
    }

    int winStart = clickGlyph;
    int winEnd = clickGlyph + 1;
    int lookback = 0;
    while (winStart > runStart && (winEnd - winStart) < kMaxChineseLookupWindowChars &&
           lookback < kMaxChineseLookupLookbackChars) {
        winStart--;
        lookback++;
    }
    while (winEnd < runEnd && (winEnd - winStart) < kMaxChineseLookupWindowChars) {
        winEnd++;
    }

    int clickOff = clickGlyph - winStart;
    int winLen = winEnd - winStart;

    char* matchedWord = nullptr;
    int matchStart = clickGlyph;
    int matchEnd = clickGlyph + 1;
    for (int len = winLen; len >= 1; len--) {
        for (int start = 0; start <= winLen - len; start++) {
            if (start > clickOff || clickOff >= start + len) {
                continue;
            }
            TempStr candidate = ToUtf8(text + winStart + start, len);
            if (FindDictIndexEntryIn(&gOfflineDictZh, candidate)) {
                matchedWord = str::Dup(candidate);
                matchStart = winStart + start;
                matchEnd = winStart + start + len;
                len = 0;
                break;
            }
        }
    }

    if (!matchedWord) {
        matchedWord = ToUtf8(text + clickGlyph, 1);
        matchStart = clickGlyph;
        matchEnd = clickGlyph + 1;
    }

    int matchLen = matchEnd - matchStart;
    Rect* coords = nullptr;
    int coordsLen = 0;
    engine->GetTextForPage(pageNo, &coordsLen, &coords);
    if (coords && matchStart >= 0 && matchEnd <= coordsLen && matchLen > 0) {
        RectF hlBox = WordLookupHighlightFromCoords(coords, matchStart, matchEnd);
        if (hlBox.dx > 0.f && hlBox.dy > 0.f) {
            TightenWordLookupHighlightBox(&hlBox, matchLen);
            ts->SelectPageBbox(pageNo, hlBox);
        } else {
            ts->SelectGlyphRange(pageNo, matchStart, matchEnd);
        }
    } else {
        ts->SelectGlyphRange(pageNo, matchStart, matchEnd);
    }
    ShowWordLookup(win, matchedWord, screenPos);
    str::Free(matchedWord);
    return true;
}

void ShowWordLookup(MainWindow* win, const char* word, Point screenPos) {
    if (!win || str::IsEmpty(word)) {
        return;
    }
    if (!AnyOfflineDictionaryLoaded()) {
        return;
    }

    char* trimmed = str::DupTemp(word);
    str::TrimWSInPlace(trimmed, str::TrimOpt::Both);
    if (!IsLookupWord(trimmed)) {
        return;
    }

    CloseWordLookup();

    Point screenPt = screenPos;
    if (win->hwndCanvas) {
        MapWindowPoints(win->hwndCanvas, HWND_DESKTOP, (POINT*)&screenPt, 1);
    }

    auto wnd = new WordLookupWnd();
    gWordLookupWnd = wnd;
    if (!wnd->Create(win, trimmed, screenPt)) {
        gWordLookupWnd = nullptr;
        delete wnd;
        return;
    }

    auto* fetchData = new FetchLookupData();
    fetchData->word = str::Dup(trimmed);
    fetchData->wnd = wnd;
    auto fn = MkFunc0<FetchLookupData>(FetchWordLookupAsync, fetchData);
    RunAsync(fn, "WordLookupFetch");
}
