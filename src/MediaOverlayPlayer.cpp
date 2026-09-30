/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Archive.h"
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
#include "GlobalPrefs.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "DisplayMode.h"

#include "utils/Log.h"
#include "WindowTab.h"
#include "MainWindow.h"
#include "Canvas.h"
#include "Commands.h"
#include "Selection.h"
#include "Notifications.h"
#include "LookupAudio.h"
#include "FloatingPopupStyle.h"
#include "Toolbar.h"
#include "Theme.h"
#include "Translations.h"
#include "TextToSpeech.h"
#include "EpubMediaOverlay.h"
#include "MediaOverlayAudio.h"
#include "MediaOverlayPlayer.h"
#include "ReadAloudHighlight.h"
#include "ReadAloudFollow.h"
#include "ReadAloudBar.h"

// how close to clipEnd we advance, to hide timer granularity
constexpr i64 kMoAdvanceSlackUs = 20 * 1000;
// clips closer than this in the same audio file play back to back without a seek
constexpr i64 kMoContiguousUs = 40 * 1000;
constexpr i64 kMoSkipUs = 10 * 1000 * 1000;
constexpr UINT kMoTickMs = 40;
constexpr DWORD kMoRelayoutCheckMs = 400;
// auto-follow keeps the active fragment between these fractions of the viewport height
static const double kMoRates[] = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};

// playback trace for automated testing, enabled with the SUMATRA_MO_TRACE environment variable
static bool MoTraceOn() {
    static int on = -1;
    if (on < 0) {
        on = GetEnvironmentVariableA("SUMATRA_MO_TRACE", nullptr, 0) > 0 ? 1 : 0;
    }
    return on == 1;
}

#define MoTrace(...)           \
    do {                       \
        if (MoTraceOn()) {     \
            logf(__VA_ARGS__); \
        }                      \
    } while (0)

// ---- per-tab detection ----

enum class MoDetect {
    Pending,
    Present,
    Absent,
};

// playback position kept while the tab's document is reloaded
struct MoResume {
    bool valid = false;
    int spineIdx = -1;
    int parIdx = -1;
    i64 posUs = -1;
    bool playing = false;
    bool follow = true;
};

struct MoTabEntry {
    WindowTab* tab = nullptr;
    // nullptr while the document is unloaded
    DocController* ctrl = nullptr;
    int gen = 0;
    MoDetect state = MoDetect::Pending;
    EpubPackage* pkg = nullptr;
    char* filePath = nullptr;
    FILETIME fileTime{};
    bool hasActiveColor = false;
    COLORREF activeColor = 0;
    bool dismissed = false;
    MoResume resume;
};

struct MoDetectTask {
    WindowTab* tab = nullptr;
    int gen = 0;
    char* filePath = nullptr;
    EpubPackage* pkg = nullptr;
    bool hasActiveColor = false;
    COLORREF activeColor = 0;
};

static Vec<MoTabEntry*> gMoTabs;
static int gMoGen = 0;

struct MoResumeTask {
    WindowTab* tab = nullptr;
    int gen = 0;
};

static void MoResumeAfterReload(MoResumeTask* t);

// ---- playback session ----

struct MoAudioLoadTask {
    int gen = 0;
    bool prefetch = false;
    char* epubPath = nullptr;
    char* entryPath = nullptr;
    u8* data = nullptr;
    size_t size = 0;
};

struct MoHlWord {
    int page = 0;
    RectF r;
    int weight = 0;
};

struct MoSession {
    WindowTab* tab = nullptr;
    MoTabEntry* entry = nullptr;
    EpubPackage* pkg = nullptr;
    MultiFormatArchive* arch = nullptr; // UI thread only, for SMIL files

    int spineIdx = -1;
    int parIdx = -1;
    bool playing = false;
    bool follow = true;
    // playback ran past the last par; play starts over from the viewport
    bool ended = false;

    char* audioPath = nullptr; // entry currently loaded in the audio player
    bool audioLoading = false;
    int audioGen = 0;

    char* prefetchPath = nullptr;
    u8* prefetchData = nullptr;
    size_t prefetchSize = 0;
    bool prefetching = false;

    Vec<int> hlPages;
    Vec<RectF> hlRects;
    int hlChapterPage = 0;
    DWORD hlTick = 0;
    bool scrollPending = false;

    // letter-weighted words inside the active SMIL fragment; empty when we only have line rects
    Vec<MoHlWord> hlWords;
    int hlWordIdx = -1;
    int hlWordWeight = 0;

    // the current par has no audio and is being spoken with text-to-speech
    bool ttsPar = false;
    // text-to-speech was tried for the current par because its audio could not be loaded
    bool ttsTried = false;

    // audio position to restore instead of clipBegin once the first audio file is loaded
    i64 resumeSeekUs = -1;

    UINT_PTR timer = 0;
};

static MoSession* gMo = nullptr;
static double MoRate() {
    double r = gGlobalPrefs ? gGlobalPrefs->narrationSpeed : 1.0;
    return (r >= 0.25 && r <= 4.0) ? r : 1.0;
}

// the next preset after the current speed, wrapping to the slowest
static double MoNextRate() {
    double r = MoRate();
    for (double v : kMoRates) {
        if (v > r + 0.01) {
            return v;
        }
    }
    return kMoRates[0];
}

static void MoSetRate(double r) {
    MoTrace("mo: rate %g\n", r);
    if (gGlobalPrefs) {
        gGlobalPrefs->narrationSpeed = (float)r;
    }
    // apply to the current clip first; SaveSettings can stall the UI
    MoAudioSetRate(r);
    if (gGlobalPrefs) {
        SaveSettings();
    }
}

static void MoSessionStop();
static void MoUpdateBars();
static void MoStartPar(bool forceSeek);

// ---- archive helpers ----

static u8* MoReadEntry(MultiFormatArchive* arch, const char* name, size_t* sizeOut) {
    *sizeOut = 0;
    if (!arch || str::IsEmpty(name)) {
        return nullptr;
    }
    auto* fi = arch->GetFileDataByName(name);
    if (!fi || !fi->data) {
        return nullptr;
    }
    u8* d = (u8*)fi->data;
    fi->data = nullptr;
    *sizeOut = fi->fileSizeUncompressed;
    return d;
}

static MultiFormatArchive* MoOpenArchive(const char* path) {
    ArchiveExtractProgressCb emptyCb;
    return OpenArchiveFromFile(path, ArchiveLoadMode::Lazy, emptyCb);
}

static COLORREF MoRgbToColorRef(u32 rgb) {
    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

static bool MoIsCss(const EpubManifestItem& it) {
    return str::EqI(it.mediaType, "text/css") || str::EndsWithI(it.path, ".css");
}

static void MoDetectDone(MoDetectTask* t) {
    MoTabEntry* entry = nullptr;
    for (MoTabEntry* e : gMoTabs) {
        if (e->tab == t->tab && e->gen == t->gen) {
            entry = e;
        }
    }
    if (!entry) {
        delete t->pkg;
        str::Free(t->filePath);
        delete t;
        return;
    }
    entry->pkg = t->pkg;
    entry->state = t->pkg ? MoDetect::Present : MoDetect::Absent;
    entry->hasActiveColor = t->hasActiveColor;
    entry->activeColor = t->activeColor;
    if (t->pkg) {
        logf("MediaOverlay: '%s' has %d narrated documents\n", t->filePath, t->pkg->overlays.Size());
        for (char* d : t->pkg->diagnostics) {
            logf("MediaOverlay: %s\n", d);
        }
    }
    MoTrace("mo: detect present=%d activeColor=%d #%06x\n", (int)(t->pkg != nullptr), (int)t->hasActiveColor,
            (unsigned)t->activeColor);
    str::Free(t->filePath);
    delete t;
    MoUpdateBars();
}

// the active class may be defined in a <style> element of a narrated content document
static void MoFindActiveColorInStyleElements(MoDetectTask* t, MultiFormatArchive* arch, EpubPackage* pkg) {
    const int kMaxDocs = 3;
    int nDocs = 0;
    for (auto& si : pkg->spine) {
        if (nDocs >= kMaxDocs) {
            break;
        }
        if (si.manifestIdx < 0 || !pkg->manifest[si.manifestIdx].mediaOverlay) {
            continue;
        }
        nDocs++;
        size_t n = 0;
        char* doc = (char*)MoReadEntry(arch, pkg->manifest[si.manifestIdx].path, &n);
        if (!doc) {
            continue;
        }
        const char* end = doc + n;
        const char* s = doc;
        while (s < end) {
            const char* open = str::FindI(s, "<style");
            if (!open) {
                break;
            }
            const char* body = str::FindChar(open, '>');
            if (!body) {
                break;
            }
            body++;
            const char* close = str::FindI(body, "</style");
            if (!close) {
                break;
            }
            u32 rgb = 0;
            bool isBg = false;
            if (EpubCssFindClassColor(body, (size_t)(close - body), pkg->activeClass, &rgb, &isBg) && isBg) {
                t->hasActiveColor = true;
                t->activeColor = MoRgbToColorRef(rgb);
                free(doc);
                return;
            }
            s = close;
        }
        free(doc);
    }
}

static void MoDetectThread(MoDetectTask* t) {
    DWORD t0 = GetTickCount();
    EpubPackage* pkg = nullptr;
    MultiFormatArchive* arch = MoOpenArchive(t->filePath);
    if (arch) {
        size_t n = 0;
        u8* c = MoReadEntry(arch, "META-INF/container.xml", &n);
        char* opf = c ? EpubParseContainerXml((const char*)c, n) : nullptr;
        free(c);
        u8* x = opf ? MoReadEntry(arch, opf, &n) : nullptr;
        if (x) {
            pkg = new EpubPackage();
            if (!EpubParsePackage(pkg, opf, (const char*)x, n) || !pkg->HasMediaOverlays()) {
                delete pkg;
                pkg = nullptr;
            }
            free(x);
        }
        str::Free(opf);
        if (pkg && pkg->activeClass) {
            for (auto& it : pkg->manifest) {
                if (!MoIsCss(it)) {
                    continue;
                }
                u8* css = MoReadEntry(arch, it.path, &n);
                u32 rgb = 0;
                bool isBg = false;
                bool found = css && EpubCssFindClassColor((const char*)css, n, pkg->activeClass, &rgb, &isBg);
                free(css);
                // a text colour alone would not read well as a highlight band
                if (found && isBg) {
                    t->hasActiveColor = true;
                    t->activeColor = MoRgbToColorRef(rgb);
                    break;
                }
            }
        }
        if (pkg && pkg->activeClass && !t->hasActiveColor) {
            MoFindActiveColorInStyleElements(t, arch, pkg);
        }
        delete arch;
    }
    t->pkg = pkg;
    MoTrace("mo: detect took %u ms\n", (unsigned)(GetTickCount() - t0));
    uitask::Post(MkFunc0<MoDetectTask>(MoDetectDone, t), "MoDetectDone");
}

static EngineBase* MoEpubEngine(WindowTab* tab) {
    if (!tab || !tab->ctrl) {
        return nullptr;
    }
    DisplayModel* dm = tab->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || engine->kind != kindEngineMupdf || !str::EqI(engine->defaultExt, ".epub")) {
        return nullptr;
    }
    return engine;
}

static MoTabEntry* MoFindEntry(WindowTab* tab) {
    for (MoTabEntry* e : gMoTabs) {
        if (e->tab == tab) {
            return e;
        }
    }
    return nullptr;
}

static void MoFreeEntry(MoTabEntry* e) {
    delete e->pkg;
    str::Free(e->filePath);
    delete e;
}

// lazy: the package is only read once an EPUB is shown, on a worker thread
static MoTabEntry* MoEnsureEntry(WindowTab* tab) {
    EngineBase* engine = MoEpubEngine(tab);
    if (!engine) {
        return nullptr;
    }
    MoTabEntry* e = MoFindEntry(tab);
    if (e && e->ctrl && e->ctrl == tab->ctrl) {
        return e;
    }
    const char* path = engine->FilePath();
    if (str::IsEmpty(path)) {
        return nullptr;
    }
    if (e && !e->ctrl && str::Eq(e->filePath, path)) {
        FILETIME ft = file::GetModificationTime(path);
        if (CompareFileTime(&ft, &e->fileTime) == 0) {
            // the same file was reloaded (font size change, ...): keep what we know about it
            e->ctrl = tab->ctrl;
            MoTrace("mo: reloaded same file, resume=%d\n", (int)e->resume.valid);
            if (e->resume.valid) {
                auto t = new MoResumeTask();
                t->tab = tab;
                t->gen = e->gen;
                // not from inside WM_PAINT: resuming may scroll
                uitask::Post(MkFunc0<MoResumeTask>(MoResumeAfterReload, t), "MoResume");
            }
            return e;
        }
    }
    if (e) {
        if (gMo && gMo->entry == e) {
            MoSessionStop();
        }
        gMoTabs.Remove(e);
        MoFreeEntry(e);
    }
    e = new MoTabEntry();
    e->tab = tab;
    e->ctrl = tab->ctrl;
    e->gen = ++gMoGen;
    e->filePath = str::Dup(path);
    e->fileTime = file::GetModificationTime(path);
    gMoTabs.Append(e);

    auto t = new MoDetectTask();
    t->tab = tab;
    t->gen = e->gen;
    t->filePath = str::Dup(path);
    RunAsync(MkFunc0<MoDetectTask>(MoDetectThread, t), "MoDetect");
    return e;
}

// ---- timeline navigation ----

static EpubMoDocument* MoEnsureDoc(int spineIdx) {
    EpubMoDocument* doc = gMo->pkg->OverlayForSpine(spineIdx);
    if (!doc || doc->parsed || doc->failed) {
        return doc;
    }
    if (!gMo->arch) {
        gMo->arch = MoOpenArchive(gMo->entry->filePath);
    }
    size_t n = 0;
    u8* d = MoReadEntry(gMo->arch, doc->smilPath, &n);
    if (!d) {
        doc->failed = true;
        gMo->pkg->AddDiag("missing media overlay %s", doc->smilPath);
        return doc;
    }
    if (!EpubParseSmil(gMo->pkg, doc, (const char*)d, n)) {
        doc->failed = true;
    }
    free(d);
    return doc;
}

static bool MoSpinePlayable(int spineIdx) {
    auto& si = gMo->pkg->spine[spineIdx];
    if (si.overlayIdx < 0) {
        return false;
    }
    EpubMoDocument* doc = MoEnsureDoc(spineIdx);
    return doc && doc->parsed && !doc->failed && doc->pars.Size() > 0;
}

static EpubMoPar* MoParAt(int spineIdx, int parIdx) {
    if (!gMo || spineIdx < 0 || spineIdx >= gMo->pkg->spine.Size()) {
        return nullptr;
    }
    EpubMoDocument* doc = gMo->pkg->OverlayForSpine(spineIdx);
    if (!doc || !doc->parsed || parIdx < 0 || parIdx >= doc->pars.Size()) {
        return nullptr;
    }
    return &doc->pars[parIdx];
}

static EpubMoPar* MoCurPar() {
    return gMo ? MoParAt(gMo->spineIdx, gMo->parIdx) : nullptr;
}

// Continuous playback follows the spine's default reading order: non-linear items are only
// played when playback was started inside them (EPUB 3.3 5.4.1 / RS 4.1).
// One SMIL document may cover several content documents (its pars reference each of them), so
// spine items sharing the overlay that just played are already done.
static bool MoNextPos(int* si, int* pi) {
    EpubMoDocument* doc = gMo->pkg->OverlayForSpine(*si);
    if (doc && *pi + 1 < doc->pars.Size()) {
        *pi += 1;
        return true;
    }
    int n = gMo->pkg->spine.Size();
    for (int i = *si + 1; i < n; i++) {
        if (!gMo->pkg->spine[i].linear || gMo->pkg->OverlayForSpine(i) == doc) {
            continue;
        }
        if (MoSpinePlayable(i)) {
            *si = i;
            *pi = 0;
            return true;
        }
    }
    return false;
}

static bool MoPrevPos(int* si, int* pi) {
    if (*pi > 0) {
        *pi -= 1;
        return true;
    }
    EpubMoDocument* doc = gMo->pkg->OverlayForSpine(*si);
    for (int i = *si - 1; i >= 0; i--) {
        if (!gMo->pkg->spine[i].linear || gMo->pkg->OverlayForSpine(i) == doc) {
            continue;
        }
        if (MoSpinePlayable(i)) {
            *si = i;
            *pi = gMo->pkg->OverlayForSpine(i)->pars.Size() - 1;
            return true;
        }
    }
    return false;
}

// ---- time display ----

static i64 MoParDurationUs(const EpubMoPar& p) {
    if (!p.audioPath) {
        return 0;
    }
    i64 end = p.clipEndUs;
    if (end < 0 && gMo->audioPath && str::Eq(gMo->audioPath, p.audioPath)) {
        end = MoAudioDurationUs();
    }
    return end > p.clipBeginUs ? end - p.clipBeginUs : 0;
}

static i64 MoDocElapsedUs(i64* totalOut) {
    *totalOut = 0;
    EpubMoDocument* doc = gMo->pkg->OverlayForSpine(gMo->spineIdx);
    if (!doc) {
        return 0;
    }
    i64 elapsed = 0;
    for (int i = 0; i < doc->pars.Size(); i++) {
        auto& p = doc->pars[i];
        i64 d = MoParDurationUs(p);
        if (i < gMo->parIdx) {
            elapsed += d;
        } else if (i == gMo->parIdx && p.audioPath && !gMo->audioLoading && MoAudioIsReady()) {
            elapsed += std::clamp(MoAudioPositionUs() - p.clipBeginUs, (i64)0, d);
        }
        *totalOut += d;
    }
    return elapsed;
}

// book time when every narrated document declares media:duration (EPUB 3.3 9.3.2.2)
static bool MoBookElapsedUs(i64 docElapsed, i64* elapsedOut, i64* totalOut) {
    EpubPackage* pkg = gMo->pkg;
    i64 before = 0;
    i64 total = 0;
    for (int i = 0; i < pkg->spine.Size(); i++) {
        auto& si = pkg->spine[i];
        if (si.overlayIdx < 0) {
            continue;
        }
        EpubMoDocument* doc = pkg->overlays[si.overlayIdx];
        i64 d = doc->manifestIdx >= 0 ? pkg->manifest[doc->manifestIdx].durationUs : -1;
        if (d < 0) {
            return false;
        }
        if (i < gMo->spineIdx) {
            before += d;
        }
        total += d;
    }
    if (pkg->totalDurationUs > 0) {
        total = pkg->totalDurationUs;
    }
    *elapsedOut = std::min(before + docElapsed, total);
    *totalOut = total;
    return total > 0;
}

static TempStr MoFmtTimeTemp(i64 us) {
    i64 s = std::max(us, (i64)0) / 1000000;
    if (s >= 3600) {
        return str::FormatTemp("%d:%02d:%02d", (int)(s / 3600), (int)((s / 60) % 60), (int)(s % 60));
    }
    return str::FormatTemp("%d:%02d", (int)(s / 60), (int)(s % 60));
}

static TempStr MoTimeLabelTemp() {
    if (!gMo) {
        return (TempStr) "";
    }
    i64 docTotal = 0;
    i64 docElapsed = MoDocElapsedUs(&docTotal);
    i64 elapsed = docElapsed;
    i64 total = docTotal;
    MoBookElapsedUs(docElapsed, &elapsed, &total);
    return str::JoinTemp(MoFmtTimeTemp(elapsed), " / ", MoFmtTimeTemp(total));
}

// ---- highlight and follow ----

static DisplayModel* MoDisplayModel() {
    return gMo && gMo->tab ? gMo->tab->AsFixed() : nullptr;
}

static bool MoIsSessionVisible() {
    return gMo && gMo->tab && gMo->tab->win && gMo->tab->win->CurrentTab() == gMo->tab;
}

static void MoInvalidateCanvas() {
    if (gMo && gMo->tab && gMo->tab->win) {
        InvalidateRect(gMo->tab->win->hwndCanvas, nullptr, FALSE);
    }
}

static bool MoIsCjkSpeakChar(WCHAR ch) {
    return (ch >= 0x3400 && ch <= 0x9FFF) || (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0x3040 && ch <= 0x30FF) ||
           (ch >= 0xAC00 && ch <= 0xD7AF);
}

static int MoCharSpeakWeight(WCHAR ch) {
    if (str::IsWs(ch) || ch < 32) {
        return 0;
    }
    if (MoIsCjkSpeakChar(ch)) {
        return 2;
    }
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= 0x00C0 && ch <= 0x024F) || (ch >= 0x1E00 && ch <= 0x1EFF)) {
        return 1;
    }
    return 0;
}

static bool MoGlyphInFragment(const Rect& cr, int pageNo) {
    float cx = cr.x + cr.dx / 2.f;
    float cy = cr.y + cr.dy / 2.f;
    for (int i = 0; i < gMo->hlPages.Size(); i++) {
        if (gMo->hlPages[i] != pageNo) {
            continue;
        }
        RectF r = gMo->hlRects[i];
        if (cx >= r.x && cx <= r.x + r.dx && cy >= r.y && cy <= r.y + r.dy) {
            return true;
        }
    }
    return false;
}

static void MoFlushHlWord(MoHlWord* w) {
    if (w->r.IsEmpty()) {
        return;
    }
    if (w->weight <= 0) {
        w->weight = 1;
    }
    gMo->hlWords.Append(*w);
    *w = MoHlWord();
}

// map glyphs of the SMIL fragment to follow-units: Latin words, one CJK character each
static void MoCollectWords() {
    gMo->hlWords.Reset();
    gMo->hlWordIdx = -1;
    gMo->hlWordWeight = 0;
    DisplayModel* dm = MoDisplayModel();
    if (!dm || gMo->hlPages.Size() == 0) {
        return;
    }
    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return;
    }
    MoHlWord cur{};
    Vec<int> done;
    for (int i = 0; i < gMo->hlPages.Size(); i++) {
        int pageNo = gMo->hlPages[i];
        if (done.Contains(pageNo)) {
            continue;
        }
        done.Append(pageNo);
        int len = 0;
        Rect* coords = nullptr;
        const WCHAR* text = engine->GetTextForPage(pageNo, &len, &coords);
        for (int c = 0; text && coords && c < len; c++) {
            WCHAR ch = text[c];
            if (ch == '\n' || ch == '\r' || str::IsWs(ch)) {
                MoFlushHlWord(&cur);
                continue;
            }
            if (!MoGlyphInFragment(coords[c], pageNo)) {
                continue;
            }
            int w = MoCharSpeakWeight(ch);
            RectF gr = ToRectF(coords[c]);
            if (MoIsCjkSpeakChar(ch)) {
                MoFlushHlWord(&cur);
                MoHlWord one;
                one.page = pageNo;
                one.r = gr;
                one.weight = w > 0 ? w : 1;
                gMo->hlWords.Append(one);
                continue;
            }
            if (cur.r.IsEmpty()) {
                cur.page = pageNo;
                cur.r = gr;
            } else {
                cur.r = cur.r.Union(gr);
            }
            cur.weight += w;
        }
    }
    MoFlushHlWord(&cur);
    for (int i = 0; i < gMo->hlWords.Size(); i++) {
        gMo->hlWordWeight += gMo->hlWords[i].weight;
    }
    if (gMo->hlWords.Size() > 0) {
        gMo->hlWordIdx = 0;
    }
}

static i64 MoParClipEndUs(EpubMoPar* par) {
    if (!par) {
        return -1;
    }
    if (par->clipEndUs >= 0) {
        return par->clipEndUs;
    }
    return MoAudioDurationUs();
}

// karaoke inside one SMIL par: progress through speaking weight. 120 ms lag so the
// band stays on the word being heard rather than running ahead of pauses.
static int MoWordIdxForPos(i64 posUs, EpubMoPar* par) {
    int n = gMo->hlWords.Size();
    if (n <= 1 || gMo->hlWordWeight <= 0 || !par) {
        return n > 0 ? 0 : -1;
    }
    i64 begin = par->clipBeginUs < 0 ? 0 : par->clipBeginUs;
    i64 end = MoParClipEndUs(par);
    if (end <= begin) {
        return 0;
    }
    i64 pos = posUs - 120 * 1000;
    if (pos < begin) {
        pos = begin;
    }
    if (pos > end) {
        pos = end;
    }
    i64 target = (pos - begin) * (i64)gMo->hlWordWeight / (end - begin);
    i64 acc = 0;
    for (int i = 0; i < n; i++) {
        acc += gMo->hlWords[i].weight;
        if (acc > target) {
            return i;
        }
    }
    return n - 1;
}

static void MoSyncWordHighlight() {
    if (!gMo || gMo->ended || gMo->hlWords.Size() == 0) {
        return;
    }
    EpubMoPar* par = MoCurPar();
    int idx = 0;
    if (gMo->hlWords.Size() > 1 && par && par->audioPath && !gMo->ttsPar && MoAudioIsReady()) {
        idx = MoWordIdxForPos(MoAudioPositionUs(), par);
    }
    if (idx < 0) {
        idx = 0;
    }
    if (idx != gMo->hlWordIdx) {
        gMo->hlWordIdx = idx;
        MoInvalidateCanvas();
    }
}

static void MoResolveHighlight(bool parChanged) {
    gMo->hlTick = GetTickCount();
    DisplayModel* dm = MoDisplayModel();
    EpubMoPar* par = gMo->ended ? nullptr : MoCurPar();
    Vec<int> pages;
    Vec<RectF> rects;
    int chPage = 0;
    if (dm && par) {
        EngineMupdfEpubFragmentRects(dm->GetEngine(), par->textPath, par->textFragment, pages, rects, &chPage);
    }
    bool changed = pages.Size() != gMo->hlPages.Size() || chPage != gMo->hlChapterPage;
    for (int i = 0; !changed && i < pages.Size(); i++) {
        RectF a = rects[i];
        RectF b = gMo->hlRects[i];
        changed = pages[i] != gMo->hlPages[i] || a.x != b.x || a.y != b.y || a.dx != b.dx || a.dy != b.dy;
    }
    if (changed) {
        gMo->hlPages.Reset();
        gMo->hlRects.Reset();
        gMo->hlPages.Append(pages.LendData(), pages.Size());
        gMo->hlRects.Append(rects.LendData(), rects.Size());
        gMo->hlChapterPage = chPage;
    }
    if (changed || parChanged) {
        MoCollectWords();
    }
    if (parChanged) {
        gMo->scrollPending = true;
    }
    if (changed || parChanged) {
        MoInvalidateCanvas();
    }
}

static int MoHighlightPage(RectF* rOut) {
    if (gMo->hlWordIdx >= 0 && gMo->hlWordIdx < gMo->hlWords.Size()) {
        *rOut = gMo->hlWords[gMo->hlWordIdx].r;
        return gMo->hlWords[gMo->hlWordIdx].page;
    }
    if (gMo->hlPages.Size() > 0) {
        *rOut = gMo->hlRects[0];
        return gMo->hlPages[0];
    }
    // whole-document text reference (no fragment) or a fragment we could not locate
    *rOut = RectF();
    return gMo->hlChapterPage;
}

// fraction-of-viewport position of the highlight, or false when it is not on screen; a whole
// document without fragment is visible when its first page is
static bool MoHighlightScreenPos(DisplayModel* dm, float* topOut, float* bottomOut) {
    RectF r;
    int pageNo = MoHighlightPage(&r);
    return ReadAloudFollowScreenPos(dm, pageNo, r, topOut, bottomOut);
}

static bool MoHighlightInSafeZone(DisplayModel* dm) {
    RectF r;
    int pageNo = MoHighlightPage(&r);
    return ReadAloudFollowInSafeZone(dm, pageNo, r);
}

static bool MoHighlightFullyVisible(DisplayModel* dm) {
    RectF r;
    int pageNo = MoHighlightPage(&r);
    return ReadAloudFollowFullyVisible(dm, pageNo, r);
}

static void MoScrollToHighlight(MainWindow* win, DisplayModel* dm) {
    RectF r;
    int pageNo = MoHighlightPage(&r);
    int before = dm->yOffset();
    ReadAloudFollowScrollTo(win, dm, pageNo, r);
    MoTrace("mo: scroll page=%d y=%d -> %d target=%d cur=%d\n", pageNo, before, dm->yOffset(), win->scrollTargetY,
            dm->CurrentPageNo());
}

static void MoFollow() {
    MainWindow* win = gMo->tab->win;
    DisplayModel* dm = MoDisplayModel();
    if (!win || !dm || !ReadAloudFollowEnabled()) {
        return;
    }
    if (!gMo->follow) {
        // the reader brought the narration back into view: resume following
        if (gMo->playing && MoHighlightFullyVisible(dm)) {
            MoTrace("mo: follow on (back in view)\n");
            gMo->follow = true;
            gMo->scrollPending = false;
            MoUpdateBars();
        }
        return;
    }
    // a smooth scroll we started is still animating
    if (win->readAloudScrollFromCode) {
        return;
    }
    bool pending = gMo->scrollPending;
    gMo->scrollPending = false;
    if (MoHighlightInSafeZone(dm)) {
        return;
    }
    // re-anchor on par change, and when relayout (zoom, theme, resize) moved the text off screen
    float top, bottom;
    if (pending || (gMo->playing && !MoHighlightScreenPos(dm, &top, &bottom))) {
        MoScrollToHighlight(win, dm);
    }
}

// ---- audio ----

static void MoAudioLoadThread(MoAudioLoadTask* t);

static void MoApplyAudioSource(const char* path, u8* data, size_t size) {
    gMo->audioLoading = false;
    if (!MoAudioSetSource(data, size, path)) {
        // no current source: the timer skips this file's clips
        str::FreePtr(&gMo->audioPath);
        logf("MediaOverlay: cannot play '%s'\n", path);
        return;
    }
    str::ReplaceWithCopy(&gMo->audioPath, path);
    MoTrace("mo: audio source %s (%d bytes)\n", path, (int)size);
    MoAudioSetRate(MoRate());
    EpubMoPar* par = MoCurPar();
    if (par && par->audioPath && str::Eq(par->audioPath, path)) {
        i64 seek = par->clipBeginUs;
        if (gMo->resumeSeekUs >= par->clipBeginUs && (par->clipEndUs < 0 || gMo->resumeSeekUs < par->clipEndUs)) {
            seek = gMo->resumeSeekUs;
        }
        gMo->resumeSeekUs = -1;
        MoAudioSeekUs(seek);
    }
    if (gMo->playing) {
        MoAudioPlay();
    }
}

static void MoAudioLoaded(MoAudioLoadTask* t) {
    bool stale = !gMo || t->gen != gMo->audioGen;
    if (!stale && t->prefetch) {
        gMo->prefetching = false;
        free(gMo->prefetchData);
        str::ReplaceWithCopy(&gMo->prefetchPath, t->entryPath);
        gMo->prefetchData = t->data;
        gMo->prefetchSize = t->size;
        t->data = nullptr;
    } else if (!stale) {
        if (t->data) {
            MoApplyAudioSource(t->entryPath, t->data, t->size);
            t->data = nullptr;
        } else {
            gMo->audioLoading = false;
            MoAudioClose();
            str::FreePtr(&gMo->audioPath);
            logf("MediaOverlay: missing audio '%s'\n", t->entryPath);
        }
    }
    free(t->data);
    str::Free(t->epubPath);
    str::Free(t->entryPath);
    delete t;
}

static void MoAudioLoadThread(MoAudioLoadTask* t) {
    MultiFormatArchive* arch = MoOpenArchive(t->epubPath);
    t->data = MoReadEntry(arch, t->entryPath, &t->size);
    delete arch;
    uitask::Post(MkFunc0<MoAudioLoadTask>(MoAudioLoaded, t), "MoAudioLoaded");
}

static void MoStartAudioLoad(const char* path, bool prefetch) {
    auto t = new MoAudioLoadTask();
    if (!prefetch) {
        gMo->audioGen++;
    }
    t->gen = gMo->audioGen;
    t->prefetch = prefetch;
    t->epubPath = str::Dup(gMo->entry->filePath);
    t->entryPath = str::Dup(path);
    RunAsync(MkFunc0<MoAudioLoadTask>(MoAudioLoadThread, t), "MoAudioLoad");
}

static void MoLoadAudio(const char* path) {
    if (gMo->prefetchPath && str::Eq(gMo->prefetchPath, path) && gMo->prefetchData) {
        u8* d = gMo->prefetchData;
        size_t n = gMo->prefetchSize;
        gMo->prefetchData = nullptr;
        str::FreePtr(&gMo->prefetchPath);
        gMo->audioGen++;
        MoApplyAudioSource(path, d, n);
        return;
    }
    MoAudioPause();
    gMo->audioLoading = true;
    MoStartAudioLoad(path, false);
}

// Load the next audio file in reading order ahead of time, so chapter changes don't stall.
static void MoPrefetchNextAudio() {
    if (gMo->prefetching || gMo->audioLoading || !gMo->audioPath) {
        return;
    }
    int si = gMo->spineIdx;
    int pi = gMo->parIdx;
    for (int i = 0; i < 64 && MoNextPos(&si, &pi); i++) {
        EpubMoPar* p = MoParAt(si, pi);
        if (!p || !p->audioPath || str::Eq(p->audioPath, gMo->audioPath)) {
            continue;
        }
        if (gMo->prefetchPath && str::Eq(gMo->prefetchPath, p->audioPath)) {
            return;
        }
        gMo->prefetching = true;
        MoStartAudioLoad(p->audioPath, true);
        return;
    }
}

// text of the active fragment as laid out, for speaking a par that has no audio
static TempStr MoHighlightTextTemp() {
    DisplayModel* dm = MoDisplayModel();
    if (!dm || gMo->hlPages.Size() == 0) {
        return nullptr;
    }
    EngineBase* engine = dm->GetEngine();
    Vec<WCHAR> s;
    for (int i = 0; i < gMo->hlPages.Size(); i++) {
        int len = 0;
        Rect* coords = nullptr;
        const WCHAR* text = engine->GetTextForPage(gMo->hlPages[i], &len, &coords);
        RectF r = gMo->hlRects[i];
        for (int c = 0; text && coords && c < len; c++) {
            Rect cr = coords[c];
            float cx = cr.x + cr.dx / 2.f;
            float cy = cr.y + cr.dy / 2.f;
            if (cx < r.x || cx > r.x + r.dx || cy < r.y || cy > r.y + r.dy) {
                continue;
            }
            WCHAR ch = text[c];
            s.Append(ch == '\n' || ch == '\r' ? L' ' : ch);
        }
        s.Append(L' ');
    }
    s.Append(0);
    TempStr res = ToUtf8Temp(s.LendData());
    str::TrimWSInPlace(res, str::TrimOpt::Both);
    return str::IsEmpty(res) ? nullptr : res;
}

static void MoStopTts() {
    if (gMo && gMo->ttsPar) {
        gMo->ttsPar = false;
        TtsStop();
    }
}

// Media Overlays 3.3: a par without audio may be rendered with text-to-speech
static void MoSpeakPar() {
    MoStopTts();
    TempStr text = MoHighlightTextTemp();
    if (text && TtsSpeakUtf8(text)) {
        gMo->ttsPar = true;
    }
    MoTrace("mo: tts '%s' speaking=%d\n", text ? text : "", (int)gMo->ttsPar);
}

static void MoStartPar(bool forceSeek) {
    EpubMoPar* par = MoCurPar();
    if (!par) {
        return;
    }
    MoStopTts();
    gMo->ttsTried = false;
    MoResolveHighlight(true);
    MoTrace("mo: par t=%u spine=%d par=%d text=%s#%s audio=%s clip=%lld..%lld rects=%d page=%d\n",
            (unsigned)GetTickCount(), gMo->spineIdx, gMo->parIdx, par->textPath,
            par->textFragment ? par->textFragment : "", par->audioPath ? par->audioPath : "-",
            (long long)par->clipBeginUs, (long long)par->clipEndUs, gMo->hlRects.Size(),
            gMo->hlPages.Size() > 0 ? gMo->hlPages[0] : gMo->hlChapterPage);
    if (!par->audioPath) {
        MoAudioPause();
        if (gMo->playing) {
            MoSpeakPar();
        }
        return;
    }
    if (gMo->audioPath && str::Eq(gMo->audioPath, par->audioPath) && !gMo->audioLoading) {
        if (forceSeek) {
            MoAudioSeekUs(par->clipBeginUs);
        }
        if (gMo->playing) {
            MoAudioPlay();
        }
    } else if (!gMo->audioLoading || !str::Eq(gMo->audioPath, par->audioPath)) {
        MoLoadAudio(par->audioPath);
    }
    MoPrefetchNextAudio();
}

static void MoGoTo(int si, int pi, bool forceSeek) {
    EpubMoPar* prev = MoCurPar();
    EpubMoPar* next = MoParAt(si, pi);
    if (!next) {
        return;
    }
    bool contiguous = !forceSeek && prev && prev->audioPath && next->audioPath &&
                      str::Eq(prev->audioPath, next->audioPath) && prev->clipEndUs >= 0 &&
                      std::abs(next->clipBeginUs - prev->clipEndUs) <= kMoContiguousUs;
    gMo->ended = false;
    gMo->spineIdx = si;
    gMo->parIdx = pi;
    MoStartPar(!contiguous);
}

static void MoReachedEnd() {
    MoTrace("mo: end\n");
    gMo->playing = false;
    gMo->ended = true;
    MoAudioPause();
    // no element is active any more
    gMo->hlPages.Reset();
    gMo->hlRects.Reset();
    gMo->hlWords.Reset();
    gMo->hlWordIdx = -1;
    gMo->hlWordWeight = 0;
    gMo->hlChapterPage = 0;
    MoInvalidateCanvas();
    if (gMo->tab->win) {
        ShowTemporaryNotification(gMo->tab->win->hwndCanvas, _TRA("End of narration"));
        ToolbarUpdateStateForWindow(gMo->tab->win, true);
    }
    MoUpdateBars();
}

static void MoAdvance() {
    int si = gMo->spineIdx;
    int pi = gMo->parIdx;
    if (MoNextPos(&si, &pi)) {
        MoGoTo(si, pi, false);
        return;
    }
    MoReachedEnd();
}

// ---- start position ----

// Viewport top as (pageNo, y in page coordinates), for comparing against fragment rects.
static bool MoViewportTop(DisplayModel* dm, int* pageOut, float* yOut) {
    int n = dm->PageCount();
    Rect vp = dm->GetViewPort();
    for (int p = 1; p <= n; p++) {
        PageInfo* pi = dm->GetPageInfo(p);
        if (!pi || pi->visibleRatio <= 0.f) {
            continue;
        }
        PointF pt = dm->CvtFromScreen(Point(vp.dx / 2, 0), p);
        *pageOut = p;
        *yOut = std::max(pt.y, 0.f);
        return true;
    }
    return false;
}

// 1 when par starts after (page, y), -1 before, 0 unknown
static int MoParVsPos(DisplayModel* dm, EpubMoPar& par, int page, float y) {
    Vec<int> pages;
    Vec<RectF> rects;
    int chPage = 0;
    if (!EngineMupdfEpubFragmentRects(dm->GetEngine(), par.textPath, par.textFragment, pages, rects, &chPage)) {
        return 0;
    }
    if (par.textFragment && rects.Size() == 0) {
        // the fragment is not in the document
        return 0;
    }
    int pp = pages.Size() > 0 ? pages[0] : chPage;
    if (pp <= 0) {
        return 0;
    }
    if (pp != page) {
        return pp > page ? 1 : -1;
    }
    if (rects.Size() == 0) {
        // whole document starting on this page
        return 1;
    }
    return rects[0].y + rects[0].dy > y ? 1 : -1;
}

static bool MoPathsSame(const char* a, const char* b) {
    if (!a || !b) {
        return false;
    }
    while (*a == '/') {
        a++;
    }
    while (*b == '/') {
        b++;
    }
    if (str::EqI(a, b)) {
        return true;
    }
    size_t la = str::Len(a);
    size_t lb = str::Len(b);
    if (la > lb && a[la - lb - 1] == '/' && str::EqI(a + (la - lb), b)) {
        return true;
    }
    if (lb > la && b[lb - la - 1] == '/' && str::EqI(b + (lb - la), a)) {
        return true;
    }
    return false;
}

// First par of this content document whose text ends below (page, y). Unknown
// fragment positions stay on this document; they do not fall back to an earlier one.
static int MoFindStartPar(DisplayModel* dm, EpubMoDocument* doc, int page, float y, const char* spinePath) {
    int firstInDoc = -1;
    int lastBefore = -1;
    for (int i = 0; i < doc->pars.Size(); i++) {
        EpubMoPar& par = doc->pars[i];
        if (spinePath && par.textPath && !MoPathsSame(par.textPath, spinePath)) {
            continue;
        }
        if (firstInDoc < 0) {
            firstInDoc = i;
        }
        int cmp = MoParVsPos(dm, par, page, y);
        MoTrace("mo: find start page=%d y=%.1f par=%d cmp=%d\n", page, y, i, cmp);
        if (cmp > 0) {
            return i;
        }
        if (cmp < 0) {
            lastBefore = i;
        }
    }
    if (lastBefore >= 0) {
        return lastBefore;
    }
    return firstInDoc >= 0 ? firstInDoc : 0;
}

// Spine item for the content document that contains this page, or -1.
static int MoSpineForPage(EngineBase* engine, EpubPackage* pkg, int page) {
    if (!engine || !pkg) {
        return -1;
    }
    int chapter = -1;
    int chStart = 0;
    if (!EngineMupdfGetReflowPageChapter(engine, page, &chapter, &chStart)) {
        return -1;
    }
    const char* mupdfPath = EngineMupdfEpubPathForChapter(engine, chapter);
    for (int i = 0; i < pkg->spine.Size(); i++) {
        const char* path = pkg->SpinePath(i);
        if (!path) {
            continue;
        }
        if (mupdfPath && MoPathsSame(path, mupdfPath)) {
            return i;
        }
        if (EngineMupdfEpubChapterForPath(engine, path) == chapter) {
            return i;
        }
    }
    return -1;
}

static int MoSpineForPage(EngineBase* engine, int page) {
    return gMo ? MoSpineForPage(engine, gMo->pkg, page) : -1;
}

// this page's spine document ships an overlay (cheap: no SMIL parse)
static bool MoViewHasOverlay(WindowTab* tab) {
    if (!MediaOverlayTabHasNarration(tab)) {
        return false;
    }
    MoTabEntry* e = MoFindEntry(tab);
    DisplayModel* dm = tab->AsFixed();
    if (!e || !e->pkg || !dm) {
        return false;
    }
    int page = 0;
    float y = 0;
    if (!MoViewportTop(dm, &page, &y)) {
        return false;
    }
    int si = MoSpineForPage(dm->GetEngine(), e->pkg, page);
    if (si < 0 || si >= e->pkg->spine.Size()) {
        return false;
    }
    return e->pkg->spine[si].overlayIdx >= 0;
}

// Narrated par at or after (page, y) in that page's document only. No jump to
// another chapter: a document without an overlay is not playable from here.
static bool MoFindStartAt(DisplayModel* dm, bool havePos, int page, float y, int* siOut, int* piOut) {
    EpubPackage* pkg = gMo->pkg;
    if (dm && havePos) {
        int si = MoSpineForPage(dm->GetEngine(), page);
        MoTrace("mo: find spine page=%d spine=%d playable=%d\n", page, si, si >= 0 ? (int)MoSpinePlayable(si) : 0);
        if (si < 0 || !MoSpinePlayable(si)) {
            return false;
        }
        *siOut = si;
        *piOut = MoFindStartPar(dm, pkg->OverlayForSpine(si), page, y, pkg->SpinePath(si));
        return true;
    }
    for (int i = 0; i < pkg->spine.Size(); i++) {
        if (pkg->spine[i].linear && MoSpinePlayable(i)) {
            *siOut = i;
            *piOut = 0;
            return true;
        }
    }
    return false;
}

static bool MoFindStart(int* siOut, int* piOut) {
    DisplayModel* dm = MoDisplayModel();
    int page = 0;
    float y = 0;
    bool havePos = dm && MoViewportTop(dm, &page, &y);
    return MoFindStartAt(dm, havePos, page, y, siOut, piOut);
}

// the par under a point on the canvas (the first one whose text ends below it)
static bool MoFindStartAtPoint(Point screenPt, int* siOut, int* piOut) {
    DisplayModel* dm = MoDisplayModel();
    int page = dm ? dm->GetPageNoByPoint(screenPt) : 0;
    if (!dm || page <= 0) {
        return MoFindStart(siOut, piOut);
    }
    float y = std::max(dm->CvtFromScreen(screenPt, page).y, 0.f);
    return MoFindStartAt(dm, true, page, y, siOut, piOut);
}

// ---- session ----

static void CALLBACK MoTimerProc(HWND, UINT, UINT_PTR, DWORD);

static void MoFreeSession(MoSession* s) {
    if (s->timer) {
        KillTimer(nullptr, s->timer);
    }
    delete s->arch;
    str::Free(s->audioPath);
    str::Free(s->prefetchPath);
    free(s->prefetchData);
    delete s;
}

static void MoSessionStop() {
    if (!gMo) {
        return;
    }
    MoTrace("mo: session stop\n");
    MoStopTts();
    MoSession* s = gMo;
    gMo = nullptr;
    MoAudioClose();
    MainWindow* win = s->tab ? s->tab->win : nullptr;
    MoFreeSession(s);
    if (win) {
        InvalidateRect(win->hwndCanvas, nullptr, FALSE);
        ToolbarUpdateStateForWindow(win, true);
    }
    MoUpdateBars();
}

static void MoSessionNew(WindowTab* tab, MoTabEntry* entry) {
    MoSessionStop();
    LookupAudioStop();
    gMo = new MoSession();
    gMo->tab = tab;
    gMo->entry = entry;
    gMo->pkg = entry->pkg;
}

static void MoSessionBegin(int si, int pi, bool playing, bool follow, i64 seekUs) {
    MoTrace("mo: session start spine=%d par=%d playing=%d seek=%lld\n", si, pi, (int)playing, (long long)seekUs);
    gMo->playing = playing;
    gMo->follow = follow;
    gMo->resumeSeekUs = seekUs;
    gMo->timer = SetTimer(nullptr, 0, kMoTickMs, MoTimerProc);
    MoGoTo(si, pi, true);
}

static bool MoSessionStart(WindowTab* tab, MoTabEntry* entry) {
    MoSessionNew(tab, entry);
    int si = -1;
    int pi = -1;
    if (!MoFindStart(&si, &pi)) {
        MoSessionStop();
        if (tab->win) {
            ShowTemporaryNotification(tab->win->hwndCanvas, _TRA("Narration could not be loaded"));
        }
        return false;
    }
    MoSessionBegin(si, pi, true, true, -1);
    return true;
}

static void MoResumeAfterReload(MoResumeTask* t) {
    MoTabEntry* e = nullptr;
    for (MoTabEntry* it : gMoTabs) {
        if (it->tab == t->tab && it->gen == t->gen) {
            e = it;
        }
    }
    delete t;
    if (!e || !e->resume.valid) {
        return;
    }
    MoResume r = e->resume;
    e->resume = MoResume();
    // the reader started narration again, or the document is gone again
    if ((gMo && gMo->tab == e->tab) || !MediaOverlayTabHasNarration(e->tab)) {
        MoTrace("mo: resume skipped session=%d ctrl=%d same=%d state=%d\n", (int)(gMo && gMo->tab == e->tab),
                (int)(e->ctrl != nullptr), (int)(e->ctrl == e->tab->ctrl), (int)e->state);
        return;
    }
    MoSessionNew(e->tab, e);
    if (r.spineIdx < 0 || r.spineIdx >= gMo->pkg->spine.Size() || !MoSpinePlayable(r.spineIdx) ||
        !MoParAt(r.spineIdx, r.parIdx)) {
        MoTrace("mo: resume position gone\n");
        MoSessionStop();
        return;
    }
    MoSessionBegin(r.spineIdx, r.parIdx, r.playing, r.follow, r.posUs);
    if (e->tab->win) {
        ToolbarUpdateStateForWindow(e->tab->win, true);
    }
    MoUpdateBars();
}

static void MoPlay() {
    DisplayModel* dm = MoDisplayModel();
    // after the reader moved elsewhere while paused, start where they are reading now
    float top, bottom;
    bool hlVisible = dm && MoHighlightScreenPos(dm, &top, &bottom);
    MoTrace("mo: play hlVisible=%d ended=%d\n", (int)hlVisible, (int)gMo->ended);
    if (dm && (gMo->ended || !hlVisible)) {
        int si = -1;
        int pi = -1;
        bool found = MoFindStart(&si, &pi);
        MoTrace("mo: play restart found=%d spine=%d par=%d\n", (int)found, si, pi);
        if (found && (gMo->ended || si != gMo->spineIdx || pi != gMo->parIdx)) {
            gMo->ended = false;
            gMo->playing = true;
            gMo->follow = true;
            MoGoTo(si, pi, true);
            return;
        }
    }
    gMo->ended = false;
    gMo->playing = true;
    gMo->follow = true;
    gMo->scrollPending = true;
    EpubMoPar* par = MoCurPar();
    if (par && !par->audioPath) {
        MoSpeakPar();
    } else if (par && !gMo->audioLoading) {
        if (!gMo->audioPath || !str::Eq(gMo->audioPath, par->audioPath)) {
            MoStartPar(true);
        } else {
            MoAudioPlay();
        }
    }
}

static void MoPauseInternal() {
    MoTrace("mo: pause\n");
    gMo->playing = false;
    MoAudioPause();
    // text-to-speech cannot pause mid-phrase: the phrase restarts on play
    MoStopTts();
}

static void MoSkipTime(i64 deltaUs) {
    EpubMoPar* par = MoCurPar();
    if (!par || !par->audioPath || gMo->audioLoading || !MoAudioIsReady()) {
        MoTrace("mo: skip ignored (audio not ready)\n");
        return;
    }
    i64 target = std::max((i64)0, MoAudioPositionUs() + deltaUs);
    EpubMoDocument* doc = gMo->pkg->OverlayForSpine(gMo->spineIdx);
    // the par of this document that covers target in the same audio file
    int found = -1;
    for (int i = 0; i < doc->pars.Size(); i++) {
        auto& p = doc->pars[i];
        if (!p.audioPath || !str::Eq(p.audioPath, par->audioPath)) {
            continue;
        }
        i64 end = p.clipEndUs >= 0 ? p.clipEndUs : MoAudioDurationUs();
        if (target >= p.clipBeginUs && target < end) {
            found = i;
            break;
        }
    }
    MoTrace("mo: skip pos=%lld target=%lld found=%d\n", (long long)MoAudioPositionUs(), (long long)target, found);
    if (found < 0) {
        if (deltaUs < 0) {
            MoGoTo(gMo->spineIdx, 0, true);
            return;
        }
        // past this document's narration: continue with the next document
        int si = gMo->spineIdx;
        int pi = doc->pars.Size() - 1;
        if (MoNextPos(&si, &pi)) {
            MoGoTo(si, pi, true);
        } else {
            MoReachedEnd();
        }
        return;
    }
    if (found != gMo->parIdx) {
        gMo->parIdx = found;
        MoResolveHighlight(true);
    }
    MoAudioSeekUs(target);
}

static void MoStepPar(int dir) {
    int si = gMo->spineIdx;
    int pi = gMo->parIdx;
    if (dir < 0) {
        EpubMoPar* par = MoCurPar();
        // like media players: "previous" first restarts the current phrase
        if (par && par->audioPath && MoAudioIsReady() && !gMo->audioLoading &&
            MoAudioPositionUs() - par->clipBeginUs > 1500 * 1000) {
            MoGoTo(si, pi, true);
            return;
        }
        if (MoPrevPos(&si, &pi)) {
            MoGoTo(si, pi, true);
        }
        return;
    }
    if (MoNextPos(&si, &pi)) {
        MoGoTo(si, pi, true);
    }
}

static void CALLBACK MoTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!gMo) {
        return;
    }
    MoAudioTick();
    if (!gMo->tab->win || !MoIsSessionVisible()) {
        // narration belongs to what the reader is looking at
        if (gMo->playing) {
            MoPauseInternal();
            if (gMo->tab->win) {
                ToolbarUpdateStateForWindow(gMo->tab->win, true);
            }
        }
        return;
    }
    if (gMo->playing && !gMo->audioLoading) {
        EpubMoPar* par = MoCurPar();
        if (!par) {
            MoReachedEnd();
        } else if (!par->audioPath) {
            if (!gMo->ttsPar || !TtsIsSpeaking()) {
                gMo->ttsPar = false;
                MoAdvance();
            }
        } else if (!gMo->audioPath || !str::Eq(gMo->audioPath, par->audioPath)) {
            // audio could not be loaded: speak the text instead, like a par without audio
            if (!gMo->ttsTried) {
                gMo->ttsTried = true;
                MoSpeakPar();
                if (!gMo->ttsPar) {
                    MoAdvance();
                }
            } else if (!gMo->ttsPar || !TtsIsSpeaking()) {
                gMo->ttsPar = false;
                MoAdvance();
            }
        } else if (MoAudioHasError()) {
            logf("MediaOverlay: audio error in '%s'\n", par->audioPath);
            MoReachedEnd();
        } else if (MoAudioIsReady()) {
            i64 pos = MoAudioPositionUs();
            bool clipDone = par->clipEndUs >= 0 && pos >= par->clipEndUs - kMoAdvanceSlackUs;
            if (clipDone || MoAudioEnded()) {
                MoAdvance();
            }
        }
    }
    if (!gMo) {
        return;
    }
    MoSyncWordHighlight();
    if (GetTickCount() - gMo->hlTick >= kMoRelayoutCheckMs) {
        MoResolveHighlight(false);
        if (MoTraceOn()) {
            DisplayModel* dm = MoDisplayModel();
            float top = -1, bottom = -1;
            bool vis = dm && MoHighlightScreenPos(dm, &top, &bottom);
            logf("mo: state spine=%d par=%d playing=%d follow=%d pos=%lld rate=%g hl=%d %.2f..%.2f pages=%d\n",
                 gMo->spineIdx, gMo->parIdx, (int)gMo->playing, (int)gMo->follow,
                 (long long)(MoAudioIsReady() ? MoAudioPositionUs() : -1), MoRate(), (int)vis, top, bottom,
                 dm ? dm->PageCount() : 0);
        }
    }
    MoFollow();
    MoUpdateBars();
}

// ---- read-aloud bar ----

static void MoUpdateBars() {
    Vec<MainWindow*> wins;
    for (MoTabEntry* e : gMoTabs) {
        if (e->tab && e->tab->win && !wins.Contains(e->tab->win)) {
            wins.Append(e->tab->win);
        }
    }
    if (gMo && gMo->tab && gMo->tab->win && !wins.Contains(gMo->tab->win)) {
        wins.Append(gMo->tab->win);
    }
    for (MainWindow* w : wins) {
        ReadAloudBarUpdate(w);
    }
}

static bool MoIsSessionOfWindow(MainWindow* win) {
    return gMo && gMo->tab && gMo->tab == win->CurrentTab();
}

// navigation before narration started: start paused at the current position
static bool MoEnsureSessionForNav(MainWindow* win) {
    if (MoIsSessionOfWindow(win)) {
        return true;
    }
    WindowTab* tab = win->CurrentTab();
    MoTabEntry* entry = tab ? MoFindEntry(tab) : nullptr;
    if (!entry || !MediaOverlayTabHasNarration(tab) || !MoSessionStart(tab, entry)) {
        return false;
    }
    MoPauseInternal();
    ToolbarUpdateStateForWindow(win, true);
    return true;
}

struct MoBarSource : ReadAloudBarSource {
    const char* Name() override { return "mo"; }
    bool Wanted(MainWindow* win) override {
        WindowTab* tab = win->CurrentTab();
        MoTabEntry* e = tab ? MoFindEntry(tab) : nullptr;
        return e && MediaOverlayTabHasNarration(tab) && !e->dismissed;
    }
    bool IsPlaying(MainWindow* win) override { return MoIsSessionOfWindow(win) && gMo->playing; }
    bool CanSeek(MainWindow* win) override {
        WindowTab* tab = win ? win->CurrentTab() : nullptr;
        return MediaOverlayHasSessionInTab(tab) || MoViewHasOverlay(tab);
    }
    bool HasTime(MainWindow* win) override { return CanSeek(win); }
    TempStr TimeLabelTemp(MainWindow* win) override {
        return MoIsSessionOfWindow(win) ? MoTimeLabelTemp() : (TempStr) "";
    }
    double Rate(MainWindow*) override { return MoRate(); }
    bool IsFollowing(MainWindow* win) override {
        return !MoIsSessionOfWindow(win) || (gMo->follow && ReadAloudFollowEnabled());
    }
    const char* PrevTooltip() override { return _TRA("Previous phrase"); }
    const char* NextTooltip() override { return _TRA("Next phrase"); }
    const char* PlayTooltip() override { return _TRA("Play / Pause narration"); }
    const char* CloseTooltip() override { return _TRA("Hide narration controls"); }
    void Prev(MainWindow* win) override {
        if (MoEnsureSessionForNav(win)) {
            MoStepPar(-1);
            gMo->scrollPending = gMo->follow;
        }
    }
    void Next(MainWindow* win) override {
        if (MoEnsureSessionForNav(win)) {
            MoStepPar(1);
            gMo->scrollPending = gMo->follow;
        }
    }
    void TogglePlay(MainWindow* win) override {
        // through the command so text-to-speech state is stopped consistently
        HwndSendCommand(win->hwndFrame, CmdReadAloud);
    }
    void Skip(MainWindow* win, int dir) override {
        if (MoEnsureSessionForNav(win)) {
            MoSkipTime(dir * kMoSkipUs);
            gMo->scrollPending = gMo->follow;
        }
    }
    void NextRate(MainWindow*) override {
        MoSetRate(MoNextRate());
        MoUpdateBars();
    }
    void Follow(MainWindow* win) override {
        if (!MoEnsureSessionForNav(win)) {
            return;
        }
        MoTrace("mo: follow on (button)\n");
        gMo->follow = true;
        if (DisplayModel* dm = MoDisplayModel()) {
            MoScrollToHighlight(win, dm);
        }
        gMo->scrollPending = true;
        MoUpdateBars();
    }
    void Close(MainWindow* win) override {
        WindowTab* tab = win->CurrentTab();
        if (MoTabEntry* entry = tab ? MoFindEntry(tab) : nullptr) {
            entry->dismissed = true;
        }
        if (MoIsSessionOfWindow(win)) {
            MoSessionStop();
        }
        MoUpdateBars();
    }
};

ReadAloudBarSource* MediaOverlayBarSource() {
    static MoBarSource src;
    return &src;
}

// ---- public API ----

bool MediaOverlayTabHasNarration(WindowTab* tab) {
    if (gGlobalPrefs && !gGlobalPrefs->narrationUseBookAudio) {
        return false;
    }
    MoTabEntry* e = tab ? MoFindEntry(tab) : nullptr;
    return e && e->ctrl && e->ctrl == tab->ctrl && e->state == MoDetect::Present;
}

bool MediaOverlayIsPlayingInTab(WindowTab* tab) {
    return gMo && gMo->tab == tab && gMo->playing;
}

bool MediaOverlayIsPlaying() {
    return gMo && gMo->playing;
}

bool MediaOverlayHasSessionInTab(WindowTab* tab) {
    return gMo && gMo->tab == tab;
}

bool MediaOverlayHandleReadAloud(WindowTab* tab) {
    if (!MediaOverlayTabHasNarration(tab)) {
        return false;
    }
    MoTabEntry* entry = MoFindEntry(tab);
    entry->dismissed = false;
    if (gMo && gMo->tab == tab) {
        if (gMo->playing) {
            MoPauseInternal();
        } else {
            MoPlay();
        }
        MoUpdateBars();
        return true;
    }
    bool started = MoSessionStart(tab, entry);
    MoUpdateBars();
    return started;
}

static bool MoStartFrom(WindowTab* tab, bool atPoint, Point screenPt) {
    if (!MediaOverlayTabHasNarration(tab)) {
        return false;
    }
    MoTabEntry* entry = MoFindEntry(tab);
    entry->dismissed = false;
    bool fresh = !gMo || gMo->tab != tab;
    if (fresh) {
        MoSessionNew(tab, entry);
    }
    int si = -1;
    int pi = -1;
    bool found = atPoint ? MoFindStartAtPoint(screenPt, &si, &pi) : MoFindStart(&si, &pi);
    MoTrace("mo: start from %s found=%d spine=%d par=%d\n", atPoint ? "point" : "viewport", (int)found, si, pi);
    if (!found) {
        if (fresh) {
            MoSessionStop();
        }
        return false;
    }
    if (fresh) {
        MoSessionBegin(si, pi, true, true, -1);
    } else {
        gMo->ended = false;
        gMo->playing = true;
        gMo->follow = true;
        MoGoTo(si, pi, true);
    }
    MoUpdateBars();
    return true;
}

bool MediaOverlayStartFromViewport(WindowTab* tab) {
    return MoStartFrom(tab, false, Point());
}

bool MediaOverlayStartAtPoint(WindowTab* tab, Point screenPt) {
    return MoStartFrom(tab, true, screenPt);
}

bool MediaOverlayPause() {
    if (!gMo || !gMo->playing) {
        return false;
    }
    MoPauseInternal();
    MoUpdateBars();
    return true;
}

void MediaOverlayStop() {
    MoSessionStop();
}

void MediaOverlaySetRate(double rate) {
    if (rate >= 0.25 && rate <= 4.0) {
        MoSetRate(rate);
        MoUpdateBars();
    }
}

void MediaOverlayOnTabDocumentGone(WindowTab* tab) {
    if (gMo && gMo->tab == tab) {
        MoSessionStop();
    }
    MoTabEntry* e = MoFindEntry(tab);
    if (e) {
        gMoTabs.Remove(e);
        MoFreeEntry(e);
    }
    MoUpdateBars();
}

void MediaOverlayOnTabDocumentUnloaded(WindowTab* tab) {
    MoTabEntry* e = MoFindEntry(tab);
    if (!e) {
        return;
    }
    // unloading can be reported more than once per reload: only a live session replaces the record
    if (gMo && gMo->tab == tab) {
        e->resume = MoResume();
    }
    if (gMo && gMo->tab == tab && !gMo->ended && MoCurPar()) {
        MoResume& r = e->resume;
        r.valid = true;
        r.spineIdx = gMo->spineIdx;
        r.parIdx = gMo->parIdx;
        r.playing = gMo->playing;
        r.follow = gMo->follow;
        EpubMoPar* par = MoCurPar();
        if (par->audioPath && gMo->audioPath && str::Eq(par->audioPath, gMo->audioPath) && !gMo->audioLoading &&
            MoAudioIsReady()) {
            r.posUs = MoAudioPositionUs();
        }
        MoTrace("mo: unloaded, resume spine=%d par=%d pos=%lld playing=%d\n", r.spineIdx, r.parIdx, (long long)r.posUs,
                (int)r.playing);
    }
    if (gMo && gMo->tab == tab) {
        MoSessionStop();
    }
    e->ctrl = nullptr;
    MoUpdateBars();
}

void MediaOverlayOnWindowClosing(MainWindow* win) {
    if (gMo && gMo->tab && gMo->tab->win == win) {
        MoSessionStop();
    }
    for (int i = gMoTabs.Size() - 1; i >= 0; i--) {
        MoTabEntry* e = gMoTabs[i];
        if (e->tab && e->tab->win == win) {
            gMoTabs.RemoveAt(i);
            MoFreeEntry(e);
        }
    }
    ReadAloudBarOnWindowClosing(win);
}

void MediaOverlayOnCanvasPaint(MainWindow* win, HDC hdc) {
    WindowTab* tab = win->CurrentTab();
    MoEnsureEntry(tab);
    ReadAloudBarUpdate(win);
    if (!gMo || gMo->tab != tab || gMo->hlPages.Size() == 0) {
        return;
    }
    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return;
    }
    Vec<Rect> screen;
    if (gMo->hlWordIdx >= 0 && gMo->hlWordIdx < gMo->hlWords.Size()) {
        const MoHlWord& w = gMo->hlWords[gMo->hlWordIdx];
        if (dm->ValidPageNo(w.page)) {
            PageInfo* pi = dm->GetPageInfo(w.page);
            if (pi && pi->visibleRatio > 0.f) {
                Rect sr = dm->CvtToScreen(w.page, w.r).Intersect(win->canvasRc);
                if (!sr.IsEmpty()) {
                    screen.Append(sr);
                }
            }
        }
    } else {
        for (int i = 0; i < gMo->hlPages.Size(); i++) {
            int pageNo = gMo->hlPages[i];
            if (!dm->ValidPageNo(pageNo)) {
                continue;
            }
            PageInfo* pi = dm->GetPageInfo(pageNo);
            if (!pi || pi->visibleRatio <= 0.f) {
                continue;
            }
            Rect sr = dm->CvtToScreen(pageNo, gMo->hlRects[i]).Intersect(win->canvasRc);
            if (!sr.IsEmpty()) {
                screen.Append(sr);
            }
        }
    }
    if (screen.Size() == 0) {
        return;
    }
    COLORREF col = ReadAloudResolveHighlightColor(gMo->entry->hasActiveColor, gMo->entry->activeColor);
    PaintFindMatchHighlightRectangles(hdc, win->canvasRc, screen, col, kSelectionHighlightAlpha);
}

void MediaOverlayOnUserViewChanged(MainWindow* win) {
    if (!gMo || !gMo->follow || !gMo->playing || !gMo->tab || gMo->tab->win != win) {
        return;
    }
    if (win->readAloudScrollFromCode || !MoIsSessionVisible()) {
        return;
    }
    DisplayModel* dm = MoDisplayModel();
    float top, bottom;
    // small nudges keep following; scrolling the narration out of view stops it
    if (dm && !MoHighlightScreenPos(dm, &top, &bottom)) {
        MoTrace("mo: follow off\n");
        gMo->follow = false;
        MoUpdateBars();
    }
}
