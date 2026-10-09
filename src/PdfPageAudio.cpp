/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "Annotation.h"
#include "EngineMupdf.h"
#include "DisplayModel.h"
#include "WindowTab.h"
#include "MainWindow.h"
#include "TextToSpeech.h"
#include "Translations.h"
#include "MediaOverlayAudio.h"
#include "MediaOverlayPlayer.h"
#include "LookupAudio.h"
#include "ReadAloudBar.h"
#include "Toolbar.h"
#include "ReadAloudHighlight.h"
#include "ReadAloudFollow.h"
#include "Selection.h"
#include "OcrService.h"
#include "utils/Log.h"
#include "PdfPageAudio.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")

struct PdfDismiss {
    WindowTab* tab = nullptr;
    int pageNo = 0;
};

struct PdfAudioSession {
    WindowTab* tab = nullptr;
    int pageNo = 0;
    u64 token = 0;
    bool playing = false;
    UINT_PTR timer = 0;
    RectF speaker;
    bool hasSpeaker = false;
};

static PdfAudioSession* gPdf = nullptr;
static Vec<PdfDismiss> gDismissed;

struct PdfHlWord {
    int page = 0;
    RectF r;
    int weight = 0;
};

static Vec<PdfHlWord> gWords;
static int gWordIdx = -1;
static int gWordWeight = 0;
static bool gFollow = true;
// Cumulative speech time at the end of each 20 ms frame. Empty means the clip
// could not be measured, so the highlight advances across the whole duration.
static Vec<i64> gSpeechAtEnd;
static i64 gSpeechTotal = 0;
static i64 gFrameUs = 20000;

static void PdfSyncHighlight(bool forceFollow);
static void PdfClearHighlight(WindowTab* tab);
static void PdfAnalyzeSpeech(const u8* data, size_t size);
static void PdfBuildWords(WindowTab* tab, int pageNo);

static void PdfUpdateBar(WindowTab* tab) {
    if (tab && tab->win) {
        ReadAloudBarUpdate(tab->win);
        ToolbarUpdateStateForWindow(tab->win, false);
    }
}

static void PdfKillTimer() {
    if (gPdf && gPdf->timer) {
        KillTimer(nullptr, gPdf->timer);
        gPdf->timer = 0;
    }
}

static EngineMupdf* PdfEngine(WindowTab* tab) {
    if (!tab) {
        return nullptr;
    }
    return AsEngineMupdf(tab->GetEngine());
}

static int PdfCurrentPage(WindowTab* tab) {
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    if (!dm) {
        return 0;
    }
    int page = dm->CurrentPageNo();
    return dm->ValidPageNo(page) ? page : 0;
}

static bool PdfPageHasAudio(EngineMupdf* engine, int pageNo, bool load) {
    if (!engine || pageNo < 1) {
        return false;
    }
    // A toolbar status query must not wait for a render/page-load lock.
    // Explicit playback still loads the annotations as before.
    FzPageInfo* pi = load ? engine->GetFzPageInfo(pageNo, true) : engine->GetFzPageInfoCanFail(pageNo);
    if (!pi || (!load && !pi->fullyLoaded)) {
        return false;
    }
    for (Annotation* annot : pi->annotations) {
        if (annot && AnnotationSupportsMediaPlayback(annot->type)) {
            return true;
        }
    }
    return false;
}

static Annotation* PdfFirstAudioAnnot(EngineMupdf* engine, int pageNo, bool load) {
    if (!engine || pageNo < 1) {
        return nullptr;
    }
    FzPageInfo* pi = load ? engine->GetFzPageInfo(pageNo, true) : engine->GetFzPageInfoFast(pageNo);
    if (!pi) {
        return nullptr;
    }
    for (Annotation* annot : pi->annotations) {
        if (annot && AnnotationSupportsMediaPlayback(annot->type)) {
            return annot;
        }
    }
    return nullptr;
}

static bool PdfIsDismissed(WindowTab* tab, int pageNo) {
    for (PdfDismiss& d : gDismissed) {
        if (d.tab == tab && d.pageNo == pageNo) {
            return true;
        }
    }
    return false;
}

static void PdfUndismiss(WindowTab* tab, int pageNo) {
    for (int i = gDismissed.Size() - 1; i >= 0; i--) {
        if (gDismissed[i].tab == tab && gDismissed[i].pageNo == pageNo) {
            gDismissed.RemoveAt(i);
        }
    }
}

static void PdfRememberDismiss(WindowTab* tab, int pageNo) {
    if (!tab || pageNo < 1 || PdfIsDismissed(tab, pageNo)) {
        return;
    }
    PdfDismiss d;
    d.tab = tab;
    d.pageNo = pageNo;
    gDismissed.Append(d);
}

static void PdfClearDismissForTab(WindowTab* tab) {
    for (int i = gDismissed.Size() - 1; i >= 0; i--) {
        if (gDismissed[i].tab == tab) {
            gDismissed.RemoveAt(i);
        }
    }
}

static void PdfClearDismissForWindow(MainWindow* win) {
    if (!win) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        PdfClearDismissForTab(tab);
    }
}

static TempStr PdfFmtTimeTemp(i64 us) {
    i64 s = std::max(us, (i64)0) / 1000000;
    if (s >= 3600) {
        return str::FormatTemp("%d:%02d:%02d", (int)(s / 3600), (int)((s / 60) % 60), (int)(s % 60));
    }
    return str::FormatTemp("%d:%02d", (int)(s / 60), (int)(s % 60));
}

static void PdfPausePlaying() {
    if (!gPdf || !gPdf->playing) {
        return;
    }
    MoAudioPause();
    gPdf->playing = false;
    PdfKillTimer();
    PdfUpdateBar(gPdf->tab);
}

static bool PdfSessionIsCurrent() {
    return gPdf && gPdf->tab && gPdf->tab->win && gPdf->tab->win->CurrentTab() == gPdf->tab;
}

static VOID CALLBACK PdfTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!gPdf) {
        return;
    }
    MoAudioTick();
    // Narration belongs to the tab on screen. TTS pauses in CloseDocumentInCurrentTab;
    // EPUB media-overlay pauses here when the session tab is no longer current.
    if (!PdfSessionIsCurrent()) {
        PdfPausePlaying();
        return;
    }
    if (gPdf->playing && (MoAudioEnded() || MoAudioHasError())) {
        gPdf->playing = false;
        PdfKillTimer();
    }
    PdfSyncHighlight(false);
    PdfUpdateBar(gPdf->tab);
}

static void PdfStartTimer() {
    if (!gPdf || gPdf->timer) {
        return;
    }
    gPdf->timer = SetTimer(nullptr, 0, 200, PdfTimerProc);
}

static double PdfSavedRate() {
    float r = gGlobalPrefs ? gGlobalPrefs->narrationSpeed : 1.0f;
    if (r < 0.25f || r > 4.0f) {
        r = 1.0f;
    }
    return r;
}

static void PdfClearHighlight(WindowTab* tab) {
    gWords.Reset();
    gWordIdx = -1;
    gWordWeight = 0;
    gSpeechAtEnd.Reset();
    gSpeechTotal = 0;
    gFrameUs = 20000;
    if (tab && tab->win) {
        InvalidateRect(tab->win->hwndCanvas, nullptr, FALSE);
    }
}

static bool PdfIsCjkChar(WCHAR ch) {
    return (ch >= 0x3400 && ch <= 0x9FFF) || (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0x3040 && ch <= 0x30FF) ||
           (ch >= 0xAC00 && ch <= 0xD7AF);
}

static int PdfCharWeight(WCHAR ch) {
    if (str::IsWs(ch) || ch < 32) {
        return 0;
    }
    if (PdfIsCjkChar(ch)) {
        return 2;
    }
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= 0x00C0 && ch <= 0x024F) || (ch >= 0x1E00 && ch <= 0x1EFF)) {
        return 1;
    }
    return 0;
}

static void PdfFlushWord(PdfHlWord* w) {
    if (w->r.IsEmpty()) {
        return;
    }
    if (w->weight <= 0) {
        w->weight = 1;
    }
    gWordWeight += w->weight;
    gWords.Append(*w);
    *w = PdfHlWord();
}

static float PdfMedianWordHeight() {
    Vec<float> h;
    for (int i = 0; i < gWords.Size(); i++) {
        if (gWords[i].r.dy > 0.5f) {
            h.Append(gWords[i].r.dy);
        }
    }
    if (h.Size() == 0) {
        return 10.f;
    }
    for (int i = 1; i < h.Size(); i++) {
        float x = h[i];
        int j = i;
        while (j > 0 && h[j - 1] > x) {
            h[j] = h[j - 1];
            j--;
        }
        h[j] = x;
    }
    return h[h.Size() / 2];
}

static void PdfCollectIllustrations(EngineBase* engine, int pageNo, const RectF& page, Vec<RectF>& out) {
    out.Reset();
    EngineMupdf* mu = AsEngineMupdf(engine);
    if (!mu || page.dx <= 1.f || page.dy <= 1.f) {
        return;
    }
    FzPageInfo* pi = mu->GetFzPageInfo(pageNo, true);
    if (!pi) {
        return;
    }
    float pageArea = page.dx * page.dy;
    for (FitzPageImageInfo* img : pi->images) {
        if (!img) {
            continue;
        }
        RectF r = ToRectF(img->rect);
        if (r.IsEmpty()) {
            continue;
        }
        float area = r.dx * r.dy;
        // A full-page scan is the page itself. A tiny mark is the speaker icon.
        if (area < pageArea * 0.04f || area > pageArea * 0.72f) {
            continue;
        }
        out.Append(r);
    }
}

static bool PdfWordIsPageNumber(const PdfHlWord& w, float body, const RectF& page) {
    if (page.dy <= 1.f) {
        return false;
    }
    float cy = w.r.y + w.r.dy * 0.5f;
    bool margin = cy < page.y + page.dy * 0.075f || cy > page.y + page.dy * 0.925f;
    if (!margin) {
        return false;
    }
    // A short token in the top or bottom margin is a page number even when OCR
    // boxes are close to the body size. Longer footer text stays.
    if (w.r.dy <= body * 0.85f) {
        return w.weight <= 8;
    }
    return w.weight <= 4;
}

static bool PdfWordIsCaption(const PdfHlWord& w, float body, const Vec<RectF>& illus) {
    if (w.r.dy > body * 0.88f) {
        return false;
    }
    float cx = w.r.x + w.r.dx * 0.5f;
    for (const RectF& im : illus) {
        if (cx < im.x - w.r.dy || cx > im.x + im.dx + w.r.dy) {
            continue;
        }
        // Page space grows downward. A caption under the picture starts just below it.
        float gap = w.r.y - (im.y + im.dy);
        float maxGap = body * 1.8f;
        if (gap >= -body * 0.2f && gap <= maxGap) {
            return true;
        }
    }
    return false;
}

static bool PdfWordAboveSpeaker(const PdfHlWord& w, const RectF& speaker, float body) {
    // Fitz page space grows downward from the top, for both OCR boxes and
    // annotation bounds. A word above the speaker ends above the icon's top.
    float wordBottom = w.r.y + w.r.dy;
    return wordBottom < speaker.y - body * 0.25f;
}

// 0: not after the icon. 1: starts to its right on the same line. 2: a later line.
// The sentence that ends at a trailing icon is not after it.
static int PdfWordAfterSpeaker(const PdfHlWord& w, const RectF& speaker, float body) {
    float wordCy = w.r.y + w.r.dy * 0.5f;
    float spCy = speaker.y + speaker.dy * 0.5f;
    float band = body > speaker.dy ? body : speaker.dy;
    float dy = wordCy - spCy;
    if (dy < 0) {
        dy = -dy;
    }
    if (dy <= band * 0.75f) {
        return w.r.x >= speaker.x + speaker.dx * 0.45f ? 1 : 0;
    }
    if (w.r.y > speaker.y + speaker.dy - body * 0.15f) {
        return 2;
    }
    return 0;
}

static void PdfKeepWords(const Vec<PdfHlWord>& src) {
    gWords.Reset();
    gWordWeight = 0;
    gWordIdx = -1;
    for (int i = 0; i < src.Size(); i++) {
        gWordWeight += src[i].weight;
        gWords.Append(src[i]);
    }
    if (gWords.Size() > 0) {
        gWordIdx = 0;
    }
}

static void PdfBuildWords(WindowTab* tab, int pageNo) {
    gWords.Reset();
    gWordIdx = -1;
    gWordWeight = 0;
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || pageNo < 1) {
        return;
    }
    int len = 0;
    Rect* coords = nullptr;
    const WCHAR* text = engine->GetTextForPage(pageNo, &len, &coords);
    if (!text || !coords || len <= 0) {
        if (tab->win && !engine->WasOcrTried(pageNo)) {
            OcrScheduleForPage(tab->win, pageNo, true);
        }
        return;
    }
    PdfHlWord cur{};
    for (int c = 0; c < len; c++) {
        WCHAR ch = text[c];
        if (ch == '\n' || ch == '\r' || str::IsWs(ch) || PdfCharWeight(ch) <= 0) {
            PdfFlushWord(&cur);
            continue;
        }
        RectF gr = ToRectF(coords[c]);
        if (PdfIsCjkChar(ch)) {
            PdfFlushWord(&cur);
            PdfHlWord one;
            one.page = pageNo;
            one.r = gr;
            one.weight = PdfCharWeight(ch);
            PdfFlushWord(&one);
            continue;
        }
        if (!cur.r.IsEmpty()) {
            float prevRight = cur.r.x + cur.r.dx;
            float cy = gr.y + gr.dy * 0.5f;
            float prevCy = cur.r.y + cur.r.dy * 0.5f;
            float dy = cy - prevCy;
            if (dy < 0) {
                dy = -dy;
            }
            if (dy > gr.dy * 0.6f || gr.x > prevRight + gr.dy * 0.35f) {
                PdfFlushWord(&cur);
            }
        }
        if (cur.r.IsEmpty()) {
            cur.page = pageNo;
            cur.r = gr;
        } else {
            cur.r = cur.r.Union(gr);
        }
        cur.weight += PdfCharWeight(ch);
    }
    PdfFlushWord(&cur);
    if (gWords.Size() == 0) {
        return;
    }
    float body = PdfMedianWordHeight();
    RectF page = engine->PageMediabox(pageNo);
    Vec<RectF> illus;
    PdfCollectIllustrations(engine, pageNo, page, illus);
    Vec<PdfHlWord> bodyWords;
    int droppedCaption = 0;
    int droppedPageNo = 0;
    for (int i = 0; i < gWords.Size(); i++) {
        if (PdfWordIsPageNumber(gWords[i], body, page)) {
            droppedPageNo++;
            continue;
        }
        if (PdfWordIsCaption(gWords[i], body, illus)) {
            droppedCaption++;
            continue;
        }
        bodyWords.Append(gWords[i]);
    }
    if (bodyWords.Size() == 0) {
        bodyWords.Reset();
        for (int i = 0; i < gWords.Size(); i++) {
            bodyWords.Append(gWords[i]);
        }
    }
    Vec<PdfHlWord> fromSpeaker;
    bool haveSpeaker = gPdf && gPdf->hasSpeaker && !gPdf->speaker.IsEmpty();
    bool anyAfter = false;
    if (haveSpeaker) {
        // A lone page number under the last line is not body text after the icon.
        int belowWeight = 0;
        for (int i = 0; i < bodyWords.Size(); i++) {
            int where = PdfWordAfterSpeaker(bodyWords[i], gPdf->speaker, body);
            if (where == 1) {
                anyAfter = true;
                break;
            }
            if (where == 2) {
                belowWeight += bodyWords[i].weight;
            }
        }
        if (!anyAfter && belowWeight >= 4) {
            anyAfter = true;
        }
    }
    // The icon leads into the words after it. A trailing icon has nothing
    // after it, so the recording belongs to the text in front.
    if (haveSpeaker && anyAfter) {
        for (int i = 0; i < bodyWords.Size(); i++) {
            if (!PdfWordAboveSpeaker(bodyWords[i], gPdf->speaker, body)) {
                fromSpeaker.Append(bodyWords[i]);
            }
        }
    }
    int droppedAbove = 0;
    if (anyAfter && fromSpeaker.Size() > 0) {
        droppedAbove = bodyWords.Size() - fromSpeaker.Size();
        PdfKeepWords(fromSpeaker);
    } else {
        PdfKeepWords(bodyWords);
    }
    if (engine->HasCachedOcrText(pageNo)) {
        for (int i = 0; i < gWords.Size(); i++) {
            gWords[i].r = ExpandOcrXHeightBand(gWords[i].r, gWords[i].weight);
        }
    }
    logf("PdfAudio: page %d words=%d body=%.1f drop caption=%d pageNo=%d aboveSpeaker=%d afterSpeaker=%d\n", pageNo,
         gWords.Size(), body, droppedCaption, droppedPageNo, droppedAbove, anyAfter ? 1 : 0);
}

static void PdfAnalyzeSpeech(const u8* data, size_t size) {
    gSpeechAtEnd.Reset();
    gSpeechTotal = 0;
    gFrameUs = 20000;
    if (!data || size < 16) {
        return;
    }
    static bool mfStarted = false;
    if (!mfStarted) {
        HRESULT hrStart = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(hrStart)) {
            return;
        }
        mfStarted = true;
    }
    HRESULT hr = S_OK;
    IStream* mem = SHCreateMemStream(data, (UINT)size);
    if (!mem) {
        return;
    }
    IMFByteStream* byteStream = nullptr;
    hr = MFCreateMFByteStreamOnStream(mem, &byteStream);
    mem->Release();
    if (FAILED(hr) || !byteStream) {
        return;
    }
    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromByteStream(byteStream, nullptr, &reader);
    byteStream->Release();
    if (FAILED(hr) || !reader) {
        return;
    }
    IMFMediaType* outType = nullptr;
    hr = MFCreateMediaType(&outType);
    if (FAILED(hr) || !outType) {
        reader->Release();
        return;
    }
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 16000);
    hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, outType);
    outType->Release();
    if (FAILED(hr)) {
        reader->Release();
        return;
    }
    UINT32 rate = 16000;
    UINT32 channels = 1;
    UINT32 bits = 16;
    IMFMediaType* actual = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)) && actual) {
        actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        actual->Release();
    }
    if (bits != 16 || channels < 1 || rate < 8000) {
        reader->Release();
        return;
    }
    int frameSamples = (int)rate / 50;
    if (frameSamples < 1) {
        frameSamples = 1;
    }
    gFrameUs = 1000000 / 50;
    Vec<i16> samples;
    int guard = 0;
    while (guard++ < 200000 && samples.Size() < (int)rate * 180) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) {
            if (sample) {
                sample->Release();
            }
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) {
                sample->Release();
            }
            break;
        }
        if (!sample) {
            continue;
        }
        IMFMediaBuffer* buf = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf) {
            sample->Release();
            continue;
        }
        BYTE* bytes = nullptr;
        DWORD cur = 0;
        if (SUCCEEDED(buf->Lock(&bytes, nullptr, &cur)) && bytes && cur >= sizeof(i16) * channels) {
            int n = (int)(cur / (sizeof(i16) * channels));
            i16* s = (i16*)bytes;
            for (int i = 0; i < n; i++) {
                int sum = 0;
                for (UINT32 c = 0; c < channels; c++) {
                    sum += s[i * channels + c];
                }
                samples.Append((i16)(sum / (int)channels));
            }
            buf->Unlock();
        }
        buf->Release();
        sample->Release();
    }
    reader->Release();
    int nFrames = samples.Size() / frameSamples;
    if (nFrames < 5) {
        return;
    }
    Vec<int> rms;
    int peak = 1;
    for (int f = 0; f < nFrames; f++) {
        i64 acc = 0;
        int base = f * frameSamples;
        for (int i = 0; i < frameSamples; i++) {
            int v = samples[base + i];
            acc += (i64)v * (i64)v;
        }
        int e = (int)(acc / frameSamples);
        // compare squares; threshold uses the same scale
        int root = 0;
        int lo = 0;
        int hi = 32768;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if ((i64)mid * (i64)mid <= e) {
                root = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        rms.Append(root);
        if (root > peak) {
            peak = root;
        }
    }
    if (peak < 200) {
        return;
    }
    int thresh = peak / 12;
    if (thresh < 40) {
        thresh = 40;
    }
    Vec<u8> loud;
    for (int f = 0; f < nFrames; f++) {
        loud.Append(rms[f] >= thresh ? 1 : 0);
    }
    Vec<u8> speech;
    for (int f = 0; f < nFrames; f++) {
        u8 on = 0;
        for (int d = -3; d <= 3; d++) {
            int j = f + d;
            if (j >= 0 && j < nFrames && loud[j]) {
                on = 1;
                break;
            }
        }
        speech.Append(on);
    }
    i64 spoken = 0;
    int spokenFrames = 0;
    for (int f = 0; f < nFrames; f++) {
        if (speech[f]) {
            spoken += gFrameUs;
            spokenFrames++;
        }
        gSpeechAtEnd.Append(spoken);
    }
    // A clip that is almost all silence, or almost all noise, has nothing to align.
    if (spokenFrames < nFrames / 20 || spokenFrames > nFrames * 19 / 20) {
        gSpeechAtEnd.Reset();
        gSpeechTotal = 0;
        logf("PdfAudio: speech map unused frames=%d spoken=%d\n", nFrames, spokenFrames);
        return;
    }
    gSpeechTotal = spoken;
    logf("PdfAudio: speech %d/%d frames, %d ms\n", spokenFrames, nFrames, (int)(spoken / 1000));
}

static int PdfWordIdxForPos(i64 posUs) {
    int n = gWords.Size();
    if (n <= 0 || gWordWeight <= 0) {
        return -1;
    }
    if (n == 1) {
        return 0;
    }
    const i64 kLagUs = 120 * 1000;
    if (posUs > kLagUs) {
        posUs -= kLagUs;
    } else {
        posUs = 0;
    }
    i64 target = 0;
    if (gSpeechTotal > 0 && gSpeechAtEnd.Size() > 0 && gFrameUs > 0) {
        int frame = (int)(posUs / gFrameUs);
        if (frame < 0) {
            frame = 0;
        }
        if (frame >= gSpeechAtEnd.Size()) {
            frame = gSpeechAtEnd.Size() - 1;
        }
        target = gSpeechAtEnd[frame] * (i64)gWordWeight / gSpeechTotal;
    } else {
        i64 dur = MoAudioDurationUs();
        if (dur <= 0) {
            return 0;
        }
        if (posUs > dur) {
            posUs = dur;
        }
        target = posUs * (i64)gWordWeight / dur;
    }
    i64 acc = 0;
    for (int i = 0; i < n; i++) {
        acc += gWords[i].weight;
        if (acc > target) {
            return i;
        }
    }
    return n - 1;
}

static void PdfScrollToWord() {
    if (!gPdf || !gPdf->tab || !gPdf->tab->win || gWordIdx < 0 || gWordIdx >= gWords.Size()) {
        return;
    }
    if (!gFollow || !ReadAloudFollowEnabled()) {
        return;
    }
    DisplayModel* dm = gPdf->tab->AsFixed();
    if (!dm) {
        return;
    }
    const PdfHlWord& w = gWords[gWordIdx];
    if (ReadAloudFollowInSafeZone(dm, w.page, w.r)) {
        return;
    }
    ReadAloudFollowScrollTo(gPdf->tab->win, dm, w.page, w.r);
}

static void PdfSyncHighlight(bool forceFollow) {
    if (!gPdf || gWords.Size() == 0) {
        return;
    }
    i64 pos = 0;
    if (gPdf->playing || MoAudioIsReady()) {
        pos = MoAudioPositionUs();
    }
    int idx = PdfWordIdxForPos(pos);
    bool changed = idx != gWordIdx;
    if (changed) {
        gWordIdx = idx;
    }
    if ((changed || forceFollow) && gPdf->playing) {
        PdfScrollToWord();
    }
    if (changed && gPdf->tab && gPdf->tab->win) {
        InvalidateRect(gPdf->tab->win->hwndCanvas, nullptr, FALSE);
    }
}

void PdfPageAudioOnOcrPageReady(EngineBase* engine, int pageNo) {
    if (!gPdf || !gPdf->tab || pageNo != gPdf->pageNo) {
        return;
    }
    DisplayModel* dm = gPdf->tab->AsFixed();
    if (!dm || dm->GetEngine() != engine) {
        return;
    }
    PdfBuildWords(gPdf->tab, pageNo);
    PdfSyncHighlight(true);
    if (gPdf->tab->win) {
        InvalidateRect(gPdf->tab->win->hwndCanvas, nullptr, FALSE);
        PdfUpdateBar(gPdf->tab);
    }
}

void PdfPageAudioOnUserViewChanged(MainWindow* win) {
    if (!gPdf || !gPdf->tab || gPdf->tab->win != win || !win || win->readAloudScrollFromCode) {
        return;
    }
    if (gWordIdx < 0 || gWordIdx >= gWords.Size() || !gPdf->playing) {
        return;
    }
    DisplayModel* dm = gPdf->tab->AsFixed();
    if (!dm || !ReadAloudFollowEnabled()) {
        return;
    }
    const PdfHlWord& w = gWords[gWordIdx];
    if (gFollow) {
        float top = 0;
        float bottom = 0;
        if (!ReadAloudFollowScreenPos(dm, w.page, w.r, &top, &bottom)) {
            gFollow = false;
            PdfUpdateBar(gPdf->tab);
        }
        return;
    }
    if (ReadAloudFollowFullyVisible(dm, w.page, w.r)) {
        gFollow = true;
        PdfUpdateBar(gPdf->tab);
    }
}

void PdfPageAudioAbandon() {
    if (!gPdf) {
        return;
    }
    PdfKillTimer();
    WindowTab* tab = gPdf->tab;
    PdfClearHighlight(tab);
    delete gPdf;
    gPdf = nullptr;
    PdfUpdateBar(tab);
}

void PdfPageAudioStop() {
    WindowTab* tab = gPdf ? gPdf->tab : nullptr;
    PdfPageAudioAbandon();
    MoAudioPause();
    PdfUpdateBar(tab);
}

bool PdfPageAudioIsThisClip(u64 token) {
    return token != 0 && gPdf && gPdf->playing && gPdf->token == token;
}

static WindowTab* PdfTabForEngine(EngineBase* engine) {
    if (!engine) {
        return nullptr;
    }
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            if (tab->GetEngine() == engine) {
                return tab;
            }
        }
    }
    return nullptr;
}

static bool PdfBeginClip(WindowTab* tab, int pageNo, u64 token, u8* data, size_t size, RectF speaker) {
    if (!tab || !data || size == 0) {
        free(data);
        return false;
    }
    PdfPageAudioAbandon();
    MediaOverlayStop();
    TtsStop();
    LookupAudioStop();
    PdfAnalyzeSpeech(data, size);
    double rate = PdfSavedRate();
    if (!MoAudioSetSource(data, size, "page-audio")) {
        return false;
    }
    MoAudioSetRate(rate);
    gPdf = new PdfAudioSession();
    gPdf->tab = tab;
    gPdf->pageNo = pageNo;
    gPdf->token = token;
    gPdf->playing = true;
    gPdf->speaker = speaker;
    gPdf->hasSpeaker = !speaker.IsEmpty();
    PdfUndismiss(tab, pageNo);
    MoAudioPlay();
    PdfStartTimer();
    gFollow = ReadAloudFollowEnabled();
    PdfBuildWords(tab, pageNo);
    PdfSyncHighlight(true);
    PdfUpdateBar(tab);
    return true;
}

bool PdfPageAudioPlayClip(Annotation* annot, u8* data, size_t size) {
    if (!annot) {
        free(data);
        return false;
    }
    WindowTab* tab = PdfTabForEngine(annot->engine);
    if (!tab) {
        free(data);
        return false;
    }
    return PdfBeginClip(tab, annot->pageNo, AnnotationEmbeddedAudioToken(annot), data, size, GetRect(annot));
}

static bool PdfPlayPage(WindowTab* tab, int pageNo, bool goThere) {
    EngineMupdf* engine = PdfEngine(tab);
    Annotation* annot = PdfFirstAudioAnnot(engine, pageNo, true);
    if (!annot) {
        return false;
    }
    u8* data = nullptr;
    size_t size = 0;
    u64 token = AnnotationEmbeddedAudioToken(annot);
    if (!AnnotationCopyEmbeddedAudio(annot, &data, &size)) {
        return false;
    }
    DisplayModel* dm = tab->AsFixed();
    if (goThere && dm && dm->CurrentPageNo() != pageNo) {
        dm->GoToPage(pageNo, true);
    }
    return PdfBeginClip(tab, pageNo, token, data, size, GetRect(annot));
}

static void PdfTogglePlayback(WindowTab* tab) {
    if (!gPdf || gPdf->tab != tab) {
        return;
    }
    if (gPdf->playing) {
        MoAudioPause();
        gPdf->playing = false;
        PdfKillTimer();
    } else {
        if (MoAudioEnded()) {
            MoAudioSeekUs(0);
        }
        gPdf->playing = true;
        MoAudioPlay();
        PdfStartTimer();
    }
    PdfUpdateBar(tab);
}

bool PdfPageAudioPauseForLookup(WindowTab* tab) {
    if (!PdfPageAudioIsPlayingInTab(tab)) {
        return false;
    }
    PdfPausePlaying();
    return true;
}

void PdfPageAudioPauseIfNotCurrent() {
    if (!PdfSessionIsCurrent()) {
        PdfPausePlaying();
    }
}

bool PdfPageAudioResumeAfterLookup(WindowTab* tab) {
    if (!gPdf || !tab || gPdf->tab != tab || gPdf->playing) {
        return false;
    }
    if (MoAudioEnded()) {
        MoAudioSeekUs(0);
    }
    gPdf->playing = true;
    MoAudioPlay();
    PdfStartTimer();
    PdfUpdateBar(tab);
    return true;
}

bool PdfPageAudioIsPlayingInTab(WindowTab* tab) {
    return gPdf && tab && gPdf->tab == tab && gPdf->playing;
}

bool PdfPageAudioCanContinueInTab(WindowTab* tab) {
    int page = PdfCurrentPage(tab);
    return gPdf && tab && gPdf->tab == tab && !gPdf->playing && page > 0 && gPdf->pageNo == page;
}

bool PdfPageAudioVisiblePageHas(WindowTab* tab) {
    int page = PdfCurrentPage(tab);
    return page > 0 && PdfPageHasAudio(PdfEngine(tab), page, false);
}

bool PdfPageAudioOwnsToolbarSpeaker(WindowTab* tab) {
    if (PdfPageAudioIsPlayingInTab(tab) || PdfPageAudioCanContinueInTab(tab)) {
        return true;
    }
    int page = PdfCurrentPage(tab);
    return page > 0 && PdfPageHasAudio(PdfEngine(tab), page, true);
}

bool PdfPageAudioHandleReadAloud(WindowTab* tab) {
    if (!tab) {
        return false;
    }
    int page = PdfCurrentPage(tab);
    bool sessionHere = gPdf && gPdf->tab == tab && page > 0 && gPdf->pageNo == page;
    if (sessionHere) {
        PdfTogglePlayback(tab);
        return true;
    }
    if (gPdf && gPdf->tab == tab && gPdf->playing) {
        if (page < 1 || !PdfPageHasAudio(PdfEngine(tab), page, true)) {
            PdfTogglePlayback(tab);
            return true;
        }
        return PdfPlayPage(tab, page, false);
    }
    if (page < 1 || !PdfPageHasAudio(PdfEngine(tab), page, true)) {
        return false;
    }
    return PdfPlayPage(tab, page, false);
}

static int PdfFindAudioPage(WindowTab* tab, int fromPage, int dir) {
    EngineMupdf* engine = PdfEngine(tab);
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    if (!engine || !dm || dir == 0) {
        return 0;
    }
    int n = dm->PageCount();
    for (int page = fromPage; page >= 1 && page <= n; page += dir) {
        if (PdfPageHasAudio(engine, page, true)) {
            return page;
        }
    }
    return 0;
}

static bool PdfSessionFor(MainWindow* win) {
    return gPdf && win && gPdf->tab && gPdf->tab == win->CurrentTab();
}

struct PdfBarSource : ReadAloudBarSource {
    const char* Name() override { return "pdf"; }
    bool Wanted(MainWindow* win) override {
        WindowTab* tab = win ? win->CurrentTab() : nullptr;
        int page = PdfCurrentPage(tab);
        if (page < 1) {
            return false;
        }
        if (gPdf && gPdf->tab == tab && (gPdf->playing || gPdf->pageNo == page)) {
            return true;
        }
        if (PdfIsDismissed(tab, page)) {
            return false;
        }
        return PdfPageHasAudio(PdfEngine(tab), page, false);
    }
    bool IsPlaying(MainWindow* win) override { return PdfSessionFor(win) && gPdf->playing; }
    bool CanSeek(MainWindow* win) override { return PdfSessionFor(win); }
    bool HasTime(MainWindow* win) override { return PdfSessionFor(win); }
    TempStr TimeLabelTemp(MainWindow* win) override {
        if (!PdfSessionFor(win)) {
            return (TempStr) "";
        }
        return str::JoinTemp(PdfFmtTimeTemp(MoAudioPositionUs()), " / ", PdfFmtTimeTemp(MoAudioDurationUs()));
    }
    double Rate(MainWindow*) override { return PdfSavedRate(); }
    bool IsFollowing(MainWindow* win) override {
        if (!PdfSessionFor(win) || gWords.Size() == 0) {
            return true;
        }
        return gFollow && ReadAloudFollowEnabled();
    }
    const char* PrevTooltip() override { return _TRA("Previous page recording"); }
    const char* NextTooltip() override { return _TRA("Next page recording"); }
    const char* PlayTooltip() override { return _TRA("Play / Pause narration"); }
    const char* CloseTooltip() override { return _TRA("Hide narration controls"); }
    void Prev(MainWindow* win) override {
        WindowTab* tab = win ? win->CurrentTab() : nullptr;
        int page = gPdf && gPdf->tab == tab ? gPdf->pageNo : PdfCurrentPage(tab);
        int prev = PdfFindAudioPage(tab, page - 1, -1);
        if (prev > 0) {
            PdfPlayPage(tab, prev, true);
        }
    }
    void Next(MainWindow* win) override {
        WindowTab* tab = win ? win->CurrentTab() : nullptr;
        int page = gPdf && gPdf->tab == tab ? gPdf->pageNo : PdfCurrentPage(tab);
        int next = PdfFindAudioPage(tab, page + 1, 1);
        if (next > 0) {
            PdfPlayPage(tab, next, true);
        }
    }
    void TogglePlay(MainWindow* win) override { PdfPageAudioHandleReadAloud(win ? win->CurrentTab() : nullptr); }
    void Skip(MainWindow* win, int dir) override {
        if (!PdfSessionFor(win)) {
            return;
        }
        i64 pos = MoAudioPositionUs() + (i64)dir * 10 * 1000000;
        if (pos < 0) {
            pos = 0;
        }
        MoAudioSeekUs(pos);
        PdfSyncHighlight(true);
        if (!gPdf->playing) {
            gPdf->playing = true;
            MoAudioPlay();
            PdfStartTimer();
        }
        PdfUpdateBar(gPdf->tab);
    }
    void SetRate(MainWindow*, double rate) override {
        if (rate < 0.25 || rate > 4.0) {
            return;
        }
        if (gGlobalPrefs) {
            gGlobalPrefs->narrationSpeed = (float)rate;
            SaveSettings();
        }
        MoAudioSetRate(rate);
        if (gPdf) {
            PdfUpdateBar(gPdf->tab);
        }
    }
    void Follow(MainWindow* win) override {
        gFollow = true;
        PdfScrollToWord();
        PdfUpdateBar(win ? win->CurrentTab() : nullptr);
    }
    void Close(MainWindow* win) override {
        WindowTab* tab = win ? win->CurrentTab() : nullptr;
        int page = gPdf && gPdf->tab == tab ? gPdf->pageNo : PdfCurrentPage(tab);
        if (gPdf && gPdf->tab == tab) {
            PdfPageAudioStop();
        }
        if (tab && page > 0) {
            PdfRememberDismiss(tab, page);
        }
        PdfUpdateBar(tab);
    }
};

ReadAloudBarSource* PdfPageAudioBarSource() {
    static PdfBarSource src;
    return &src;
}

void PdfPageAudioOnCanvasPaint(MainWindow* win, HDC hdc) {
    if (!win) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    int page = PdfCurrentPage(tab);
    if (page >= 1) {
        static WindowTab* seenTab = nullptr;
        static int seenPage = 0;
        static bool seenHas = false;
        bool has = PdfPageHasAudio(PdfEngine(tab), page, false);
        if (tab != seenTab || page != seenPage || has != seenHas) {
            seenTab = tab;
            seenPage = page;
            seenHas = has;
            ReadAloudBarUpdate(win);
        }
    }
    if (!hdc || !gPdf || gPdf->tab != tab || gWordIdx < 0 || gWordIdx >= gWords.Size()) {
        return;
    }
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    if (!dm) {
        return;
    }
    const PdfHlWord& w = gWords[gWordIdx];
    if (!dm->ValidPageNo(w.page)) {
        return;
    }
    PageInfo* pi = dm->GetPageInfo(w.page);
    if (!pi || pi->visibleRatio <= 0.f) {
        return;
    }
    Rect sr = dm->CvtToScreen(w.page, w.r).Intersect(win->canvasRc);
    if (sr.IsEmpty()) {
        return;
    }
    Vec<Rect> screen;
    screen.Append(sr);
    COLORREF col = ReadAloudResolveHighlightColor(false, 0);
    PaintFindMatchHighlightRectangles(hdc, win->canvasRc, screen, col, kSelectionHighlightAlpha);
}

void PdfPageAudioOnTabGone(WindowTab* tab) {
    PdfClearDismissForTab(tab);
    if (gPdf && gPdf->tab == tab) {
        PdfPageAudioStop();
    }
}

void PdfPageAudioOnWindowClosing(MainWindow* win) {
    PdfClearDismissForWindow(win);
    if (gPdf && gPdf->tab && gPdf->tab->win == win) {
        PdfPageAudioStop();
    }
}
