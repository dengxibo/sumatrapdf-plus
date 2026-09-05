/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "DisplayModel.h"
#include "TextSelection.h"
#include "Notifications.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "Canvas.h"
#include "Translations.h"
#include "OcrOnnx.h"
#include "OcrService.h"
#include "ExtractPdfToc.h"
#include "Selection.h"
#include "SelectionToolbar.h"
#include "Toolbar.h"

#include "utils/Log.h"
#include "utils/Timer.h"

static Kind kNotifOcr = "ocrProgress";
constexpr int kOcrDoneTimeoutMs = 2000;
static bool gWarnedMissingModels = false;

static Mutex gQueueLock;
static Vec<void*> gQueue; // OcrJob*
static LONG gWorkersAlive = 0;
static LONG gOcrCancelSeq = 0;

static EngineBase* gOcrDocEngine = nullptr;
static HWND gOcrDocHwnd = nullptr;
static int gOcrDocTotal = 0;
static int gOcrDocDone = 0;
static LARGE_INTEGER gOcrDocT0{};
static Vec<float> gOcrDocPageMs;
static double gOcrDocRasterMs = 0;
static double gOcrDocDetMs = 0;
static double gOcrDocRecMs = 0;
static double gOcrDocTextMs = 0;

static EngineBase* gOcrPendingSaveEngine = nullptr;
static HWND gOcrPendingSaveHwnd = nullptr;
static char* gOcrPendingSavePath = nullptr;
static bool gOcrPendingSaveExtractToc = false;
static EngineBase* gOcrPendingExtractEngine = nullptr;
static HWND gOcrPendingExtractHwnd = nullptr;
static bool gOcrPendingExtractPersist = false;

struct OcrFlight {
    EngineBase* engine = nullptr;
    int pageNo = 0;
    HANDLE doneEvent = nullptr;
};

static Mutex gFlightLock;
static Vec<OcrFlight*> gFlights;

static OcrFlight* FindFlightLocked(EngineBase* engine, int pageNo) {
    for (int i = 0; i < gFlights.Size(); i++) {
        OcrFlight* f = gFlights[i];
        if (f->engine == engine && f->pageNo == pageNo) {
            return f;
        }
    }
    return nullptr;
}

static bool WaitIfOcrInFlight(EngineBase* engine, int pageNo) {
    HANDLE ev = nullptr;
    gFlightLock.Lock();
    OcrFlight* f = FindFlightLocked(engine, pageNo);
    if (f) {
        DuplicateHandle(GetCurrentProcess(), f->doneEvent, GetCurrentProcess(), &ev, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    gFlightLock.Unlock();
    if (!ev) {
        return false;
    }
    WaitForSingleObject(ev, 120000);
    CloseHandle(ev);
    return true;
}

struct OcrJob {
    HWND hwndCanvas = nullptr;
    EngineBase* engine = nullptr;
    int pageNo = 0;
    LONG cancelSeq = 0;
    bool showStatus = true;
    bool documentJob = false;
    bool regionJob = false;
    bool forceOcr = false;
    // snapshot of "Auto OCR is on" taken when an auto job was enqueued; the
    // worker thread uses it to chain nearby pages without touching UI state
    bool autoJob = false;
    OcrOperation op = OcrOperation::CurrentPage;
    RectF clipRect;
};

static int CountUsableChars(const WCHAR* s) {
    if (!s) {
        return 0;
    }
    int n = 0;
    for (; *s; s++) {
        if (!str::IsWs(*s) && *s != 0xFFFD) {
            n++;
        }
    }
    return n;
}

bool OcrDocumentHasFileTextLayer(EngineBase* engine) {
    if (!engine) {
        return false;
    }
    int n = engine->PageCount();
    for (int pageNo = 1; pageNo <= n; pageNo++) {
        if (engine->HasCachedOcrText(pageNo)) {
            continue;
        }
        int len = 0;
        const WCHAR* text = engine->GetTextForPage(pageNo, &len);
        if (CountUsableChars(text) >= 20) {
            return true;
        }
    }
    return false;
}

bool OcrEngineKindSupported(EngineBase* engine) {
    if (!engine) {
        return false;
    }
    Kind k = engine->kind;
    return k == kindEngineMupdf || k == kindEngineDjVu || k == kindEngineImage || k == kindEngineImageDir ||
           k == kindEngineComicBooks || k == kindEnginePostScript;
}

bool OcrAutoEnabled(MainWindow* win) {
    // Pure per-tab state read: the toolbar checked state and the auto-OCR
    // scheduling gates must all observe the SAME source of truth, otherwise
    // the toggle button looks dead (state written but never read).
    if (!win) {
        return false;
    }
    WindowTab* tab = win->CurrentTab();
    return tab && tab->autoOcrOn;
}

// Does this document have any usable text layer? Capped scan: native text
// PDFs almost always have text on the first pages, image-only scans have
// none anywhere, so scanning a bounded prefix keeps document load fast.
static bool DocHasAnyTextLayerCapped(EngineBase* engine, int maxPages) {
    if (!engine) {
        return false;
    }
    int n = std::min(engine->PageCount(), maxPages);
    for (int pageNo = 1; pageNo <= n; pageNo++) {
        if (engine->HasCachedOcrText(pageNo)) {
            return true;
        }
        int len = 0;
        const WCHAR* text = engine->GetTextForPage(pageNo, &len);
        if (CountUsableChars(text) >= 20) {
            return true;
        }
    }
    return false;
}

void ApplyAutoOcrDefaultForTab(WindowTab* tab) {
    // DEFAULT POLICY ONLY: auto OCR starts ON exclusively for image-only
    // scanned PDFs (supported engine, no usable text layer anywhere). Docs
    // with a native or already-OCR'd text layer - and non-PDF formats such
    // as EPUB - start OFF but stay manually toggleable via the toolbar.
    // This must never be used to block the user from enabling it later.
    if (!tab) {
        return;
    }
    tab->autoOcrOn = false;
    EngineBase* engine = tab->GetEngine();
    if (!OcrEngineKindSupported(engine) || !OcrModelsAvailable()) {
        return;
    }
    tab->autoOcrOn = !DocHasAnyTextLayerCapped(engine, 60);
}

bool OcrDeferExtractUntilDocumentReady(MainWindow* win, bool persistToDisk) {
    // Born-digital / already-OCR'd docs extract immediately. Image-only docs go through
    // OcrScheduleDocument(..., extractTocIfMissing) from the UI path instead.
    (void)win;
    (void)persistToDisk;
    return false;
}

bool OcrPageLooksScanned(EngineBase* engine, int pageNo) {
    if (!OcrEngineKindSupported(engine) || engine->WasOcrTried(pageNo)) {
        return false;
    }
    int len = 0;
    const WCHAR* text = engine->GetTextForPage(pageNo, &len);
    return CountUsableChars(text) < 20;
}

// 1 = Fast/Hybrid, 2 = Balanced. Balanced satisfies Fast.
static u8 OcrQualityForProfile(OcrProfile profile) {
    return profile == OcrProfile::Balanced ? 2 : 1;
}

static bool OcrPageShouldRecognize(EngineBase* engine, int pageNo, OcrOperation op, bool* forceOcrOut) {
    if (forceOcrOut) {
        *forceOcrOut = false;
    }
    if (!engine || pageNo < 1 || pageNo > engine->PageCount()) {
        return false;
    }
    u8 need = OcrQualityForProfile(GetOcrProfileForOperation(op));
    u8 have = engine->GetOcrCacheQuality(pageNo);
    if (have >= need) {
        return false;
    }
    if (have > 0) {
        if (forceOcrOut) {
            *forceOcrOut = true;
        }
        return true;
    }
    return OcrPageLooksScanned(engine, pageNo);
}

static void OcrOnProgressClosed(NotificationWnd* wnd);

static void ShowOcrStatus(HWND hwnd, const char* msg, int timeoutMs) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    NotificationCreateArgs args;
    args.hwndParent = hwnd;
    args.groupId = kNotifOcr;
    args.msg = msg;
    args.timeoutMs = timeoutMs;
    // Persistent chip: X means cancel. Timed "done" chips keep the default close.
    if (timeoutMs == kNotifNoTimeout) {
        args.onRemoved = MkFunc1Void(OcrOnProgressClosed);
    }
    ShowNotification(args);
}

static void OcrOnProgressClosed(NotificationWnd* wnd) {
    HWND hwndCanvas = GetNotificationParentHwnd(wnd);
    RemoveNotification(wnd);
    MainWindow* win = FindMainWindowByHwnd(hwndCanvas);
    OcrCancelQueued(win);
}

static void HideOcrStatus(HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) {
        RemoveNotificationsForGroup(hwnd, kNotifOcr);
    }
}

// Same OCR chip: change the text in place so progress becomes the done line.
static void OcrShowQuietDone(HWND hwnd, const char* msg) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    // Recreate so a timed "done" chip does not keep the progress close→cancel callback.
    HideOcrStatus(hwnd);
    ShowOcrStatus(hwnd, msg, kOcrDoneTimeoutMs);
}

void OcrNotifyMissingModels(HWND hwndCanvas, bool always) {
    if (!hwndCanvas) {
        return;
    }
    if (!always && gWarnedMissingModels) {
        return;
    }
    gWarnedMissingModels = true;
    TempStr msg =
        str::FormatTemp(_TRA("OCR models not found.\nPut onnxruntime.dll, det.onnx, rec.onnx and keys.txt in:\n%s"),
                        OcrSidecarDirTemp());
    ShowWarningNotification(hwndCanvas, msg, kNotif5SecsTimeOut);
}

static u8* CopyBitmapToRgb(HBITMAP hbmp, int* wOut, int* hOut, int* strideOut) {
    if (!hbmp) {
        return nullptr;
    }
    DIBSECTION ds{};
    int nObj = GetObject(hbmp, sizeof(ds), &ds);
    int w = ds.dsBm.bmWidth;
    int h = ds.dsBm.bmHeight;
    int bpp = ds.dsBm.bmBitsPixel;
    int srcStride = ds.dsBm.bmWidthBytes;
    const u8* src = (const u8*)ds.dsBm.bmBits;
    if (nObj < (int)sizeof(ds.dsBm) || w < 8 || h < 8) {
        return nullptr;
    }
    int dstStride = w * 3;
    u8* rgb = AllocArray<u8>((size_t)dstStride * (size_t)h);
    bool copied = false;
    if (src && bpp == 32 && srcStride >= w * 4) {
        for (int y = 0; y < h; y++) {
            const u8* row = src + (size_t)y * (size_t)srcStride;
            u8* dst = rgb + (size_t)y * (size_t)dstStride;
            for (int x = 0; x < w; x++) {
                dst[x * 3 + 0] = row[x * 4 + 2];
                dst[x * 3 + 1] = row[x * 4 + 1];
                dst[x * 3 + 2] = row[x * 4 + 0];
            }
        }
        copied = true;
    } else if (src && bpp == 24 && srcStride >= w * 3) {
        for (int y = 0; y < h; y++) {
            const u8* row = src + (size_t)y * (size_t)srcStride;
            u8* dst = rgb + (size_t)y * (size_t)dstStride;
            for (int x = 0; x < w; x++) {
                dst[x * 3 + 0] = row[x * 3 + 2];
                dst[x * 3 + 1] = row[x * 3 + 1];
                dst[x * 3 + 2] = row[x * 3 + 0];
            }
        }
        copied = true;
    } else if (src && bpp == 8) {
        // EngineMupdf palettizes pages with ≤256 colors. GetBitmapPixels rejects
        // paletted DIBs and used to Finalize(nullptr) here → AV at hdc offset 0x58.
        RGBQUAD pal[256]{};
        HDC hdc = CreateCompatibleDC(nullptr);
        int nPal = 0;
        if (hdc) {
            HGDIOBJ old = SelectObject(hdc, hbmp);
            nPal = GetDIBColorTable(hdc, 0, 256, pal);
            SelectObject(hdc, old);
            DeleteDC(hdc);
        }
        if (nPal > 0) {
            for (int y = 0; y < h; y++) {
                const u8* row = src + (size_t)y * (size_t)srcStride;
                u8* dst = rgb + (size_t)y * (size_t)dstStride;
                for (int x = 0; x < w; x++) {
                    RGBQUAD c = pal[row[x]];
                    dst[x * 3 + 0] = c.rgbRed;
                    dst[x * 3 + 1] = c.rgbGreen;
                    dst[x * 3 + 2] = c.rgbBlue;
                }
            }
            copied = true;
        }
    }
    if (!copied) {
        free(rgb);
        return nullptr;
    }
    *wOut = w;
    *hOut = h;
    *strideOut = dstStride;
    return rgb;
}

static RenderedBitmap* RenderPageForOcr(EngineBase* engine, int pageNo, const RectF* clip = nullptr) {
    RectF mb = clip ? *clip : engine->PageMediabox(pageNo);
    if (mb.IsEmpty() || mb.dx < 2 || mb.dy < 2) {
        return nullptr;
    }
    // RenderPage zoom is a scale factor (FzCreateViewCtm scales by it directly),
    // not a percentage.
    float fileDpi = engine->GetFileDPI();
    if (fileDpi <= 0.f) {
        fileDpi = 72.f;
    }
    float zoom = 200.f / fileDpi;
    float maxSide = mb.dx > mb.dy ? mb.dx : mb.dy;
    if (maxSide * zoom > 1920.f) {
        zoom = 1920.f / maxSide;
    }
    if (clip && maxSide * zoom < 400.f) {
        zoom = 400.f / maxSide;
    }
    RectF clipCopy = mb;
    RenderPageArgs args(pageNo, zoom, 0, clip ? &clipCopy : nullptr, RenderTarget::Export);
    RenderedBitmap* bmp = engine->RenderPage(args);
    return bmp;
}

// GB/T 9704 公文用字：仿宋汉字 1em；半角阿拉伯数字/拉丁字母约 0.5em；
// 全角标点 1em（开括号偏右、闭括号偏左，进距仍是 1em）。
static float OcrGlyphUnit(int cp) {
    if (cp <= 32) {
        return 0.5f;
    }
    if (cp >= '0' && cp <= '9') {
        return 0.5f;
    }
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
        return 0.5f;
    }
    if (cp < 127) {
        if (cp == '.' || cp == ',' || cp == ':' || cp == ';' || cp == '!' || cp == '?' || cp == '\'' || cp == '`') {
            return 0.28f;
        }
        if (cp == '"' || cp == '(' || cp == ')' || cp == '[' || cp == ']' || cp == '{' || cp == '}' || cp == '-' ||
            cp == '_' || cp == '/' || cp == '\\' || cp == '|') {
            return 0.4f;
        }
        return 0.5f;
    }
    if (cp == 0x2018 || cp == 0x2019 || cp == 0x201c || cp == 0x201d) {
        return 1.f;
    }
    if (cp >= 0x2000 && cp <= 0x206f) {
        return 0.5f;
    }
    if (cp >= 0xff01 && cp <= 0xff60) {
        return 1.f;
    }
    if (cp >= 0xff61 && cp <= 0xffdc) {
        return 0.5f;
    }
    if (cp >= 0xffe0 && cp <= 0xffe6) {
        return 1.f;
    }
    return 1.f;
}

static bool OcrIsOpenPunct(int cp) {
    return cp == '(' || cp == '[' || cp == '{' || cp == 0x2018 || cp == 0x201c || cp == 0x3008 || cp == 0x300a ||
           cp == 0x300c || cp == 0x300e || cp == 0x3010 || cp == 0x3014 || cp == 0x3016 || cp == 0x3018 ||
           cp == 0x301a || cp == 0xff08 || cp == 0xff3b || cp == 0xff5b || cp == 0xff62;
}

static bool OcrIsClosePunct(int cp) {
    return cp == ')' || cp == ']' || cp == '}' || cp == 0x2019 || cp == 0x201d || cp == 0x3001 || cp == 0x3002 ||
           cp == 0x3009 || cp == 0x300b || cp == 0x300d || cp == 0x300f || cp == 0x3011 || cp == 0x3015 ||
           cp == 0x3017 || cp == 0x3019 || cp == 0x301b || cp == 0xff01 || cp == 0xff09 || cp == 0xff0c ||
           cp == 0xff1a || cp == 0xff1b || cp == 0xff1f || cp == 0xff3d || cp == 0xff5d || cp == 0xff63;
}

static bool OcrIsGapGlue(int cp) {
    return cp == '.' || cp == ',' || cp == ':' || cp == ';' || cp == 0xb7 || cp == 0x2022 || cp == 0xff0e;
}

static bool OcrIsNarrow(int cp) {
    return cp < 127 || (cp >= 0xff61 && cp <= 0xffdc);
}

static bool OcrIsPunct(int cp) {
    return OcrIsOpenPunct(cp) || OcrIsClosePunct(cp) || OcrIsGapGlue(cp);
}

// 仿宋标点挤压（按开/闭类，不按单个码位）：
// 1) 相邻标点共用约 1em（”、  。」  等）
// 2) 后置标点跟在汉字/字母/数字后时占约 0.5em（用。  荐。  心、）
// 前置引号仍保持 1em：检测框贴墨，左空不在框内；压成 0.5 会让「四个意识」漂移。
static void OcrSqueezePunctUnits(const int* cps, float* units, int n) {
    if (!cps || !units || n < 2) {
        return;
    }
    for (int i = 1; i < n; i++) {
        if (OcrIsPunct(cps[i - 1]) && OcrIsPunct(cps[i])) {
            if (units[i - 1] > 0.5f) {
                units[i - 1] = 0.5f;
            }
            if (units[i] > 0.5f) {
                units[i] = 0.5f;
            }
        }
    }
    for (int i = 1; i < n; i++) {
        if (!OcrIsPunct(cps[i - 1]) && OcrIsClosePunct(cps[i]) && units[i] > 0.5f) {
            units[i] = 0.5f;
        }
    }
}

// Place per-glyph [x0,x1): 公文等宽汉字/全角标点，半角数字约半格。
// 只把空隙吸到「〕4」「4.号」这类半角邻接上，避免汉字格子忽宽忽窄。
static void OcrPlaceGlyphYs(int nCp, const Rect& box, int* y0, int* y1, const int* ctcX, int nCtc) {
    for (int i = 0; i < nCp; i++) {
        y0[i] = 0;
        y1[i] = box.dy;
    }
    if (nCp < 1 || box.dy < 1) {
        return;
    }
    if (ctcX && nCtc == nCp) {
        y0[0] = 0;
        y1[nCp - 1] = box.dy;
        for (int i = 0; i < nCp - 1; i++) {
            int a = ctcX[i * 2];
            int b = ctcX[i * 2 + 1];
            int c = ctcX[(i + 1) * 2];
            int d = ctcX[(i + 1) * 2 + 1];
            if (a < 0) {
                a = 0;
            }
            if (c < 0) {
                c = 0;
            }
            int mid = ((a + b) / 2 + (c + d) / 2) / 2;
            if (mid <= y0[i]) {
                mid = y0[i] + 1;
            }
            int remain = nCp - 1 - i;
            if (mid > box.dy - remain) {
                mid = box.dy - remain;
            }
            if (mid <= y0[i]) {
                mid = y0[i] + 1;
            }
            y1[i] = mid;
            y0[i + 1] = mid;
        }
        if (y1[nCp - 1] <= y0[nCp - 1]) {
            y1[nCp - 1] = y0[nCp - 1] + 1;
        }
        return;
    }
    int slice = box.dy / nCp;
    if (slice < 1) {
        slice = 1;
    }
    for (int i = 0; i < nCp; i++) {
        y0[i] = i * slice;
        y1[i] = (i + 1 == nCp) ? box.dy : (i + 1) * slice;
        if (y1[i] <= y0[i]) {
            y1[i] = y0[i] + 1;
        }
    }
}

static void OcrPlaceGlyphXs(const int* cps, const float* units, int nCp, float unitSum, const u8* rgb, int imgW,
                            int imgH, int stride, const Rect& box, int* x0, int* x1, const int* ctcX, int nCtc) {
    for (int i = 0; i < nCp; i++) {
        x0[i] = 0;
        x1[i] = box.dx;
    }
    if (nCp < 1 || box.dx < 1) {
        return;
    }
    int* cuts = AllocArray<int>(nCp + 1);
    int xL = 0;
    int xR = box.dx;
    int* dark = nullptr;
    int thr = 1;
    if (rgb && box.dy > 0) {
        dark = AllocArray<int>(box.dx);
        int maxDark = 0;
        for (int y = 0; y < box.dy; y++) {
            int iy = box.y + y;
            if (iy < 0 || iy >= imgH) {
                continue;
            }
            const u8* row = rgb + (size_t)iy * (size_t)stride;
            for (int x = 0; x < box.dx; x++) {
                int ix = box.x + x;
                if (ix < 0 || ix >= imgW) {
                    continue;
                }
                const u8* p = row + ix * 3;
                int d = 255 * 3 - (p[0] + p[1] + p[2]);
                dark[x] += d;
                if (dark[x] > maxDark) {
                    maxDark = dark[x];
                }
            }
        }
        thr = maxDark / 10;
        if (thr < 1) {
            thr = 1;
        }
        while (xL < xR && dark[xL] < thr) {
            xL++;
        }
        while (xR > xL && dark[xR - 1] < thr) {
            xR--;
        }
    }
    if (ctcX && nCtc == nCp && nCp > 0) {
        int* cx = AllocArray<int>(nCp);
        for (int i = 0; i < nCp; i++) {
            int a = ctcX[i * 2];
            int b = ctcX[i * 2 + 1];
            if (a < 0) {
                a = 0;
            }
            if (b > box.dx) {
                b = box.dx;
            }
            if (b < a + 1) {
                b = a + 1;
            }
            cx[i] = (a + b) / 2;
        }
        x0[0] = xL;
        x1[nCp - 1] = xR;
        for (int i = 0; i < nCp - 1; i++) {
            int mid = (cx[i] + cx[i + 1]) / 2;
            if (mid <= x0[i]) {
                mid = x0[i] + 1;
            }
            x1[i] = mid;
            x0[i + 1] = mid;
        }
        if (x1[nCp - 1] <= x0[nCp - 1]) {
            x1[nCp - 1] = x0[nCp - 1] + 1;
        }
        // 格子保持切开宽度（仿宋等宽字格），不按单字墨收成宽窄不一。
        free(cx);
        free(cuts);
        free(dark);
        return;
    }
    float acc = 0.f;
    int span = xR - xL;
    if (span < 1) {
        span = box.dx;
        xL = 0;
    }
    cuts[0] = xL;
    cuts[nCp] = xR;
    for (int i = 0; i < nCp; i++) {
        acc += units[i];
        if (i + 1 < nCp) {
            cuts[i + 1] = xL + (int)(acc / unitSum * (float)span + 0.5f);
        }
    }
    if (dark) {
        for (int i = 1; i < nCp; i++) {
            int x = cuts[i];
            if (x < 0) {
                x = 0;
            }
            if (x > box.dx) {
                x = box.dx;
            }
            int l = x;
            int r = x;
            while (l > 0 && dark[l - 1] < thr) {
                l--;
            }
            while (r < box.dx && dark[r] < thr) {
                r++;
            }
            if (r - l < 2) {
                continue;
            }
            int left = cps[i - 1];
            int right = cps[i];
            if (OcrIsGapGlue(left)) {
                cuts[i] = r;
            } else if (OcrIsClosePunct(left) && OcrIsNarrow(right)) {
                cuts[i] = r;
            } else if (OcrIsGapGlue(right)) {
                cuts[i] = l;
            } else if (OcrIsOpenPunct(right) && OcrIsNarrow(left)) {
                cuts[i] = l;
            }
        }
        for (int i = 1; i < nCp; i++) {
            if (cuts[i] <= cuts[i - 1]) {
                cuts[i] = cuts[i - 1] + 1;
            }
        }
        for (int i = nCp - 1; i >= 1; i--) {
            if (cuts[i] >= cuts[i + 1]) {
                cuts[i] = cuts[i + 1] - 1;
            }
            if (cuts[i] < cuts[i - 1] + 1) {
                cuts[i] = cuts[i - 1] + 1;
            }
        }
    }
    for (int i = 0; i < nCp; i++) {
        int a = cuts[i];
        int b = cuts[i + 1];
        if (b < a + 1) {
            b = a + 1;
        }
        x0[i] = a;
        x1[i] = b;
    }
    // 闭标点（。，、》）：向左找，取格子左半里最靠右的小团。
    // 开标点（《“）：只在格子里收，绝不向左（否则会罩住「将」「、」）。
    if (dark && unitSum > 0.01f) {
        int emPx = (int)((float)span / unitSum + 0.5f);
        if (emPx < 8) {
            emPx = 8;
        }
        int pad = emPx / 12;
        if (pad < 2) {
            pad = 2;
        }
        int maxW = emPx * 45 / 100;
        if (maxW < 6) {
            maxW = 6;
        }
        int blobS[32];
        int blobE[32];
        for (int i = 0; i < nCp; i++) {
            if (!OcrIsPunct(cps[i])) {
                continue;
            }
            int old0 = x0[i];
            int old1 = x1[i];
            int a = x0[i];
            int b = x1[i];
            bool wantClose = OcrIsClosePunct(cps[i]) || OcrIsGapGlue(cps[i]);
            int lo = a;
            int hi = b;
            if (wantClose) {
                lo = a - emPx * 8 / 10;
                if (lo < 0) {
                    lo = 0;
                }
                if (i > 0) {
                    int prevMid = x0[i - 1] + (x1[i - 1] - x0[i - 1]) / 3;
                    if (prevMid > lo) {
                        lo = prevMid;
                    }
                }
            } else {
                lo = a - pad;
                if (lo < 0) {
                    lo = 0;
                }
                hi = b + emPx * 3 / 10;
                if (hi > box.dx) {
                    hi = box.dx;
                }
                if (i + 1 < nCp) {
                    int nextMid = x1[i + 1] - (x1[i + 1] - x0[i + 1]) / 3;
                    if (nextMid < hi) {
                        hi = nextMid;
                    }
                }
            }
            int from = lo;
            int to = hi;
            if (from < 0) {
                from = 0;
            }
            if (to > box.dx) {
                to = box.dx;
            }
            int nBlob = 0;
            int x = from;
            while (x < to && nBlob < 32) {
                while (x < to && dark[x] < thr) {
                    x++;
                }
                if (x >= to) {
                    break;
                }
                blobS[nBlob] = x;
                while (x < to && dark[x] >= thr) {
                    x++;
                }
                blobE[nBlob] = x;
                nBlob++;
            }
            int pickL = -1, pickR = -1;
            int bestMass = -1;
            int nCand = 0;
            int mid = (a + b) / 2;
            if (wantClose) {
                for (int k = 0; k < nBlob; k++) {
                    int w = blobE[k] - blobS[k];
                    if (w < 3 || w > maxW) {
                        continue;
                    }
                    int mass = 0;
                    for (int t = blobS[k]; t < blobE[k]; t++) {
                        mass += dark[t];
                    }
                    if (mass > bestMass) {
                        bestMass = mass;
                    }
                    int cx = (blobS[k] + blobE[k]) / 2;
                    if (cx > mid + 2) {
                        continue;
                    }
                    pickL = blobS[k];
                    pickR = blobE[k];
                    nCand++;
                }
                if (pickL < 0) {
                    // 句号并进「荐」的宽团：取该团右沿，不要退回格右噪点。
                    for (int k = 0; k < nBlob; k++) {
                        int cx = (blobS[k] + blobE[k]) / 2;
                        if (cx > mid + 2) {
                            continue;
                        }
                        int e = blobE[k];
                        int s = e - maxW;
                        if (s < blobS[k]) {
                            s = blobS[k];
                        }
                        pickL = s;
                        pickR = e;
                        nCand++;
                    }
                }
            } else {
                for (int k = 0; k < nBlob; k++) {
                    int w = blobE[k] - blobS[k];
                    if (w < 3 || w > maxW) {
                        continue;
                    }
                    int mass = 0;
                    for (int t = blobS[k]; t < blobE[k]; t++) {
                        mass += dark[t];
                    }
                    if (mass > bestMass) {
                        bestMass = mass;
                    }
                    if (blobE[k] <= a) {
                        continue;
                    }
                    pickL = blobS[k];
                    pickR = blobE[k];
                    nCand++;
                    break;
                }
            }
            if (pickL >= 0 && pickR > pickL + 1) {
                int l = pickL - pad;
                int r = pickR + pad;
                if (l < 0) {
                    l = 0;
                }
                if (r > box.dx) {
                    r = box.dx;
                }
                if (i + 1 < nCp && r > x0[i + 1] && x0[i + 1] > l) {
                    r = x0[i + 1];
                }
                if (i > 0 && l < x0[i - 1] + 1) {
                    l = x0[i - 1] + 1;
                }
                if (r > l + 1) {
                    x0[i] = l;
                    x1[i] = r;
                    if (wantClose && i > 0 && !OcrIsPunct(cps[i - 1])) {
                        x1[i - 1] = x0[i];
                    }
                }
            }
        }
    }
    free(cuts);
    free(dark);
}

static int OcrMedianInts(int* a, int n) {
    if (n < 1) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (a[j] < a[i]) {
                int t = a[i];
                a[i] = a[j];
                a[j] = t;
            }
        }
    }
    return a[n / 2];
}

static bool OcrIsCnNumeral(int cp) {
    return cp == 0x4e00 || cp == 0x4e8c || cp == 0x4e09 || cp == 0x56db || cp == 0x4e94 || cp == 0x516d ||
           cp == 0x4e03 || cp == 0x516b || cp == 0x4e5d || cp == 0x5341;
}

// GB/T 9704 层次：一、 （一） 1. 附件
static bool OcrStartsLikeGbtHeading(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int cp = Utf8CodepointNext(s, len, i);
    if (cp <= 0) {
        return false;
    }
    if (cp == 0x9644) {
        int cp2 = Utf8CodepointNext(s, len, i);
        return cp2 == 0x4ef6;
    }
    if (cp == 0xff08 || cp == '(') {
        int cp2 = Utf8CodepointNext(s, len, i);
        if (OcrIsCnNumeral(cp2) || (cp2 >= '0' && cp2 <= '9')) {
            int cp3 = Utf8CodepointNext(s, len, i);
            return cp3 == 0xff09 || cp3 == ')';
        }
        return false;
    }
    if (OcrIsCnNumeral(cp)) {
        while (i < len) {
            int save = i;
            int next = Utf8CodepointNext(s, len, i);
            if (!OcrIsCnNumeral(next)) {
                i = save;
                break;
            }
        }
        int dun = Utf8CodepointNext(s, len, i);
        return dun == 0x3001;
    }
    if (cp >= '1' && cp <= '9') {
        while (i < len) {
            int save = i;
            int next = Utf8CodepointNext(s, len, i);
            if (next < '0' || next > '9') {
                i = save;
                break;
            }
        }
        int mark = Utf8CodepointNext(s, len, i);
        return mark == '.' || mark == 0xff0e || mark == 0x3001;
    }
    return false;
}

static bool OcrBoxesOnSameLine(const Rect& a, const Rect& b) {
    int ay = a.y + a.dy / 2;
    int by = b.y + b.dy / 2;
    int tol = a.dy > b.dy ? a.dy : b.dy;
    if (tol < 8) {
        tol = 8;
    }
    int d = ay > by ? ay - by : by - ay;
    return d < tol / 2;
}

static bool OcrIsDirtBulletCp(int cp) {
    return cp == '.' || cp == 0xFF0E || cp == 0x00B7 || cp == 0x2022 || cp == 0x30FB || cp == 0x2024 || cp == 0x2219 ||
           cp == 0xFF65;
}

// Drop a leading OCR speckle decoded as `.`/`．` when the rest is already `1.` / `1．`.
static bool OcrShouldDropLeadingDirtDot(const int* cps, int nCp, const char* rest, const int* ctcX, int nCtc) {
    if (!cps || nCp < 2 || !OcrIsDirtBulletCp(cps[0])) {
        return false;
    }
    if (ctcX && nCtc == nCp) {
        int ov = ctcX[1] - ctcX[2];
        if (ov > 0 && ((cps[1] >= '0' && cps[1] <= '9') || OcrIsCnNumeral(cps[1]))) {
            return true;
        }
    }
    return rest && OcrStartsLikeGbtHeading(rest);
}

static bool OcrIsLeftSplitRadicalCp(int cp) {
    switch (cp) {
        case 0x5F13: // 弓
        case 0x4EBB: // 亻
        case 0x6C35: // 氵
        case 0x624C: // 扌
        case 0x5F73: // 彳
        case 0x5FC4: // 忄
        case 0x72AD: // 犭
        case 0x961D: // 阝
        case 0x9485: // 钅
        case 0x7E9F: // 纟
        case 0x8BA0: // 讠
        case 0x9963: // 饣
        case 0x793B: // 礻
        case 0x8864: // 衤
        case 0x8279: // 艹
            return true;
        default:
            return false;
    }
}

// Drop only when geometry says the radical is a leftover of the next glyph.
// Overlap must be more than detector jitter; similar-width 弓+强 (e.g. a name) is kept.
static bool OcrShouldDropSplitRadical(int rad, int host, int wRad, int wHost, int overlap) {
    if (!OcrIsLeftSplitRadicalCp(rad) || host < 0x4E00 || host > 0x9FFF) {
        return false;
    }
    if (overlap >= 8) {
        return true;
    }
    if (wHost > 0 && wRad > 0 && wRad * 2 <= wHost) {
        return true;
    }
    return false;
}

static int OcrCtcSpanW(const int* ctcX, int nCtc, int nCp, int i) {
    if (!ctcX || nCtc != nCp || i < 0 || i >= nCp) {
        return 0;
    }
    return ctcX[i * 2 + 1] - ctcX[i * 2];
}

static int OcrCtcOverlap(const int* ctcX, int nCtc, int nCp, int i) {
    if (!ctcX || nCtc != nCp || i < 0 || i + 1 >= nCp) {
        return 0;
    }
    return ctcX[i * 2 + 1] - ctcX[(i + 1) * 2];
}

// 标点检测框贴墨，格子比汉字窄。扩成约 1em：闭标点向右长、开标点向左长，不吃邻字格子。
static void OcrExpandPunctCells(const int* cps, int nCp, int emPx, int* x0, int* x1) {
    if (!cps || !x0 || !x1 || nCp < 1 || emPx < 8) {
        return;
    }
    int minW = emPx * 4 / 5;
    if (minW < 8) {
        minW = 8;
    }
    for (int i = 0; i < nCp; i++) {
        if (!OcrIsPunct(cps[i])) {
            continue;
        }
        int w = x1[i] - x0[i];
        if (w >= minW) {
            continue;
        }
        if (OcrIsOpenPunct(cps[i])) {
            int n0 = x1[i] - emPx;
            if (i > 0 && n0 < x1[i - 1]) {
                n0 = x1[i - 1];
            }
            if (n0 < x1[i] - 1) {
                x0[i] = n0;
            }
        } else {
            int n1 = x0[i] + emPx;
            if (i + 1 < nCp && n1 > x0[i + 1]) {
                n1 = x0[i + 1];
            }
            if (n1 > x0[i] + 1) {
                x1[i] = n1;
            }
        }
    }
}

static void OcrInsetGlyphXs(int nCp, int pad, int* x0, int* x1) {
    if (!x0 || !x1 || nCp < 1 || pad < 1) {
        return;
    }
    for (int i = 0; i < nCp; i++) {
        int w = x1[i] - x0[i];
        if (w <= pad * 2 + 2) {
            continue;
        }
        x0[i] += pad;
        x1[i] -= pad;
    }
}

static int OcrSameLineMaxDy(const Vec<OcrBox>& boxes, int self, int em) {
    int h = em;
    if (self < 0 || self >= boxes.Size()) {
        return h;
    }
    const Rect& r = boxes[self].rect;
    if (r.dy > h) {
        h = r.dy;
    }
    for (int i = 0; i < boxes.Size(); i++) {
        if (i == self || !boxes[i].text || !boxes[i].text[0]) {
            continue;
        }
        if (!OcrBoxesOnSameLine(r, boxes[i].rect)) {
            continue;
        }
        if (boxes[i].rect.dy > h) {
            h = boxes[i].rect.dy;
        }
    }
    return h;
}

// 段内折行接上；段首缩进、短标题、层次序号另起一段。
static bool OcrBoxIsVerticalCol(const OcrBox& b) {
    if (b.vertical) {
        return true;
    }
    return b.rect.dy > b.rect.dx * 3 / 2 && b.rect.dy > 20;
}

static bool OcrBoxesInSameColumn(const Rect& a, const Rect& b) {
    int overlap = (a.x + a.dx < b.x + b.dx ? a.x + a.dx : b.x + b.dx) - (a.x > b.x ? a.x : b.x);
    int minW = a.dx < b.dx ? a.dx : b.dx;
    if (minW < 1 || overlap < 1) {
        return false;
    }
    return overlap * 100 > minW * 45;
}

static bool OcrShouldJoinDocLines(const OcrBox& a, const OcrBox& b, int bodyLeft, int bodyRight, int em, int gapMed) {
    if (!a.text || !b.text) {
        return false;
    }
    if (OcrBoxIsVerticalCol(a) && OcrBoxIsVerticalCol(b)) {
        if (!OcrBoxesInSameColumn(a.rect, b.rect)) {
            return false;
        }
        if (OcrStartsLikeGbtHeading(b.text)) {
            return false;
        }
        int gap = b.rect.y - (a.rect.y + a.rect.dy);
        if (gapMed > 0 && gap > gapMed * 2 + em / 2) {
            return false;
        }
        return gap >= -em;
    }
    if (OcrBoxesOnSameLine(a.rect, b.rect)) {
        return true;
    }
    if (OcrStartsLikeGbtHeading(b.text)) {
        return false;
    }
    int measure = bodyRight - bodyLeft;
    if (measure < em * 4) {
        measure = em * 8;
    }
    if (OcrStartsLikeGbtHeading(a.text) && a.rect.dx < measure * 3 / 4) {
        return false;
    }
    int aRight = a.rect.x + a.rect.dx;
    bool aFull = aRight >= bodyRight - em;
    if (!aFull) {
        return false;
    }
    int indent = b.rect.x - bodyLeft;
    if (indent >= em + em / 2) {
        return false;
    }
    int bRightPad = bodyRight - (b.rect.x + b.rect.dx);
    int bLeftPad = b.rect.x - bodyLeft;
    if (bLeftPad > em * 2 && bRightPad > em * 2 && b.rect.dx < measure * 2 / 3) {
        return false;
    }
    int gap = b.rect.y - (a.rect.y + a.rect.dy);
    if (gapMed > 0 && gap > gapMed * 2 + em / 2) {
        return false;
    }
    return true;
}

static int CountOcrCjkGlyphs(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int n = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (cp >= 0x4E00 && cp <= 0x9FFF) {
            n++;
        }
    }
    return n;
}

static int ScoreOcrBoxes(const Vec<OcrBox>& boxes) {
    int score = 0;
    for (int i = 0; i < boxes.Size(); i++) {
        const char* t = boxes[i].text;
        if (!t || !t[0]) {
            continue;
        }
        int cjk = CountOcrCjkGlyphs(t);
        score += cjk * 4 + (int)str::Len(t);
        if (str::Find(t, "\xE9\x99\x84\xE4\xBB\xB6")) { // 附件
            score += 90;
        }
        if (str::Find(t, "\xE6\x8A\xA5\xE5\x90\x8D")) { // 报名
            score += 40;
        }
        if (str::Find(t, "\xE5\xBA\x8F\xE5\x8F\xB7")) { // 序号
            score += 20;
        }
        if (str::Find(t, "\xE4\xBA\x8B\xE9\xA1\xB9")) { // 事项
            score += 20;
        }
    }
    return score;
}

static char* OcrJoinBoxText(const Vec<OcrBox>& boxes) {
    StrBuilder sb;
    for (int i = 0; i < boxes.Size(); i++) {
        const char* t = boxes[i].text;
        if (!t || !t[0]) {
            continue;
        }
        sb.Append(t);
        sb.Append("\n");
    }
    return sb.StealData();
}

static bool OcrBoxesLookLikeOfficialSideways(const Vec<OcrBox>& boxes) {
    char* t = OcrJoinBoxText(boxes);
    bool hit = t && (OcrTextLooksLikeOfficialForm(t) || OcrTextLooksLikeHuizongTable(t) ||
                     OcrTextLooksLikePortraitOfficialBody(t));
    str::Free(t);
    return hit;
}

static bool OcrFileNameHasOfficialKind(const char* filePath) {
    if (!filePath || !filePath[0]) {
        return false;
    }
    const char* name = path::GetBaseNameTemp(filePath);
    return name && (str::Find(name, "\xE5\x9B\x9E\xE5\xA4\x8D\xE6\x84\x8F\xE8\xA7\x81") || // 回复意见
                    str::Find(name, "\xE5\xBE\x81\xE6\xB1\x82\xE6\x84\x8F\xE8\xA7\x81") || // 征求意见
                    str::Find(name, "\xE6\x84\x8F\xE8\xA7\x81\xE7\xA8\xBF") ||             // 意见稿
                    str::Find(name, "\xE9\x80\x9A\xE7\x9F\xA5") ||                         // 通知
                    str::Find(name, "\xE8\xAF\xB7\xE7\xA4\xBA") ||                         // 请示
                    str::Find(name, "\xE6\x89\xB9\xE5\xA4\x8D") ||                         // 批复
                    str::Find(name, "\xE9\x80\x9A\xE6\x8A\xA5") ||                         // 通报
                    str::Find(name, "\xE7\x9A\x84\xE5\x87\xBD") ||                         // 的函
                    str::Find(name, "\xE5\x87\xBD"));                                      // 函
}

static bool OcrBoxesLookLikeVerticalBook(const Vec<OcrBox>& boxes, int imgW, int imgH) {
    if (imgW > 80 && imgH > 80 && imgW > imgH * 1.05f) {
        return false;
    }
    if (OcrBoxesLookLikeOfficialSideways(boxes)) {
        return false;
    }
    int n = 0;
    int tall = 0;
    int cjk = 0;
    int longBox = 0;
    for (int i = 0; i < boxes.Size(); i++) {
        const OcrBox& b = boxes[i];
        if (!b.text || !b.text[0]) {
            continue;
        }
        n++;
        int g = CountOcrCjkGlyphs(b.text);
        cjk += g;
        if (OcrBoxIsVerticalCol(b)) {
            tall++;
            if (g >= 4) {
                longBox++;
            }
        }
    }
    if (n < 2 || tall * 2 < n) {
        return false;
    }
    return cjk >= 6 || longBox >= 2;
}

static bool OcrPageBoxesAreVertical(const Vec<OcrBox>& boxes) {
    int n = 0;
    int v = 0;
    for (int i = 0; i < boxes.Size(); i++) {
        if (!boxes[i].text || !boxes[i].text[0]) {
            continue;
        }
        n++;
        if (OcrBoxIsVerticalCol(boxes[i])) {
            v++;
        }
    }
    return n >= 2 && v * 2 >= n;
}

static void SortOcrBoxesVerticalReading(Vec<OcrBox>& boxes) {
    int n = boxes.Size();
    for (int i = 0; i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            const Rect& a = boxes[best].rect;
            const Rect& b = boxes[j].rect;
            if (OcrBoxesInSameColumn(a, b)) {
                if (b.y < a.y) {
                    best = j;
                }
            } else {
                int ax = a.x + a.dx / 2;
                int bx = b.x + b.dx / 2;
                if (bx > ax) {
                    best = j;
                }
            }
        }
        if (best != i) {
            OcrBox tmp = boxes[i];
            boxes[i] = boxes[best];
            boxes[best] = tmp;
        }
    }
}

static bool OcrShouldTryPageRotate(const Vec<OcrBox>& boxes, int imgW, int imgH) {
    if (OcrBoxesLookLikeVerticalBook(boxes, imgW, imgH)) {
        return false;
    }
    int score = ScoreOcrBoxes(boxes);
    if (score >= 90) {
        return false;
    }
    int n = 0;
    int tall = 0;
    for (int i = 0; i < boxes.Size(); i++) {
        const OcrBox& b = boxes[i];
        if (!b.text || !b.text[0]) {
            continue;
        }
        n++;
        if (b.rect.dy > b.rect.dx * 1.5f && b.rect.dy > 20) {
            tall++;
        }
    }
    if (score < 30) {
        return true;
    }
    if (n >= 2 && tall * 2 >= n) {
        return true;
    }
    return imgW > 80 && imgH > 80 && score < 60;
}

// 90° clockwise in top-left pixel space: (x,y) -> (h-1-y, x). Dest is h x w.
static u8* RotateRgb90(const u8* src, int w, int h, int stride, bool clockwise, int* nw, int* nh, int* nstride) {
    if (!src || w < 8 || h < 8) {
        return nullptr;
    }
    int dw = h;
    int dh = w;
    int ds = dw * 3;
    u8* dst = AllocArray<u8>((size_t)ds * (size_t)dh);
    if (!dst) {
        return nullptr;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = clockwise ? (h - 1 - y) : y;
            int dy = clockwise ? x : (w - 1 - x);
            const u8* s = src + (size_t)y * (size_t)stride + (size_t)x * 3;
            u8* d = dst + (size_t)dy * (size_t)ds + (size_t)dx * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    if (nw) {
        *nw = dw;
    }
    if (nh) {
        *nh = dh;
    }
    if (nstride) {
        *nstride = ds;
    }
    return dst;
}

static Rect MapOcrRectFrom90(const Rect& r, int origW, int origH, bool clockwise) {
    if (clockwise) {
        return Rect(r.y, origH - r.x - r.dx, r.dy, r.dx);
    }
    return Rect(origW - r.y - r.dy, r.x, r.dy, r.dx);
}

static void MapOcrBoxesFrom90(Vec<OcrBox>& boxes, int origW, int origH, bool clockwise) {
    for (int i = 0; i < boxes.Size(); i++) {
        OcrBox& b = boxes[i];
        b.rect = MapOcrRectFrom90(b.rect, origW, origH, clockwise);
        free(b.charX);
        b.charX = nullptr;
        b.nChar = 0;
        b.vertical = b.rect.dy > b.rect.dx * 3 / 2 && b.rect.dy > 20;
    }
}

static bool OcrRecognizeRgbScored(const u8* rgb, int w, int h, int stride, Vec<OcrBox>& boxes, int* scoreOut,
                                  OcrProfile profile) {
    boxes.Reset();
    bool ok = OcrRecognizeRgb(rgb, w, h, stride, boxes, profile);
    int score = ok ? ScoreOcrBoxes(boxes) : 0;
    if (scoreOut) {
        *scoreOut = score;
    }
    return ok && score > 0;
}

static void OcrLogPageTiming(int pageNo, OcrProfile profile, const OcrPageTiming& t) {
    double det = t.detPreprocessMs + t.detInferenceMs + t.detPostprocessMs;
    double rec = t.recPreprocessMs + t.recInferenceMs + t.recPostprocessMs;
    logf(
        "OCR timing page=%d profile=%s raster=%.0f det=%.0f (pre=%.0f inf=%.0f post=%.0f) crop=%.0f rec=%.0f "
        "(pre=%.0f inf=%.0f post=%.0f) text=%.0f total=%.0f detBoxes=%d recBoxes=%d batches=%d\n",
        pageNo, OcrProfileName(profile), t.rasterizeMs, det, t.detPreprocessMs, t.detInferenceMs, t.detPostprocessMs,
        t.cropMs, rec, t.recPreprocessMs, t.recInferenceMs, t.recPostprocessMs, t.textLayerMs, t.pageTotalMs,
        t.nDetBoxes, t.nRecBoxes, t.nRecBatches);
}

static int CmpFloatMs(const void* a, const void* b) {
    float fa = *(const float*)a;
    float fb = *(const float*)b;
    return (fa > fb) - (fa < fb);
}

static void OcrResetDocTiming() {
    gOcrDocT0 = TimeGet();
    gOcrDocPageMs.Reset();
    gOcrDocRasterMs = 0;
    gOcrDocDetMs = 0;
    gOcrDocRecMs = 0;
    gOcrDocTextMs = 0;
}

static void OcrAccumPageTiming(const OcrPageTiming& t) {
    gOcrDocPageMs.Append((float)t.pageTotalMs);
    gOcrDocRasterMs += t.rasterizeMs;
    gOcrDocDetMs += t.detPreprocessMs + t.detInferenceMs + t.detPostprocessMs;
    gOcrDocRecMs += t.cropMs + t.recPreprocessMs + t.recInferenceMs + t.recPostprocessMs;
    gOcrDocTextMs += t.textLayerMs;
}

static void OcrLogDocSummary(const char* tag, double saveMs) {
    int n = gOcrDocPageMs.Size();
    double totalMs = TimeSinceInMs(gOcrDocT0);
    if (n < 1) {
        logf("OCR doc %s pages=0 total_ms=%.0f save_ms=%.0f\n", tag ? tag : "", totalMs, saveMs);
        return;
    }
    float* copy = AllocArray<float>((size_t)n);
    for (int i = 0; i < n; i++) {
        copy[i] = gOcrDocPageMs[i];
    }
    qsort(copy, (size_t)n, sizeof(float), CmpFloatMs);
    float median = copy[n / 2];
    int p95i = (n * 95) / 100;
    if (p95i >= n) {
        p95i = n - 1;
    }
    float p95 = copy[p95i];
    free(copy);
    double avg = 0;
    for (int i = 0; i < n; i++) {
        avg += gOcrDocPageMs[i];
    }
    avg /= (double)n;
    double pagesPerSec = totalMs > 1 ? (1000.0 * (double)n / totalMs) : 0;
    logf(
        "OCR doc %s pages=%d total_s=%.2f pages/s=%.2f avg_ms=%.0f median=%.0f p95=%.0f raster=%.0f det=%.0f rec=%.0f "
        "text=%.0f save_ms=%.0f\n",
        tag ? tag : "", n, totalMs / 1000.0, pagesPerSec, avg, median, p95, gOcrDocRasterMs, gOcrDocDetMs, gOcrDocRecMs,
        gOcrDocTextMs, saveMs);
}

static void BoxesToPageText(const Vec<OcrBox>& boxes, const u8* rgb, int imgW, int imgH, int stride,
                            const RectF& pageBox, PageText* pt, PageTextUtf8* utf8) {
    if (!pt || !utf8 || imgW < 1 || imgH < 1 || pageBox.IsEmpty()) {
        return;
    }
    *pt = {};
    *utf8 = {};
    StrBuilder utf;
    Vec<Rect> utfCoords;
    float sx = (float)pageBox.dx / (float)imgW;
    float sy = (float)pageBox.dy / (float)imgH;
    int nBox = boxes.Size();
    int* lefts = AllocArray<int>(nBox);
    int* rights = AllocArray<int>(nBox);
    int* ems = AllocArray<int>(nBox);
    int* gaps = AllocArray<int>(nBox);
    int nLong = 0;
    int nGap = 0;
    int prevI = -1;
    for (int i = 0; i < nBox; i++) {
        const OcrBox& b = boxes[i];
        if (!b.text || !b.text[0]) {
            continue;
        }
        int nCpEst = Utf8CodepointCountN(b.text, (int)str::Len(b.text));
        if (nCpEst >= 8 && b.rect.dx >= 40) {
            lefts[nLong] = b.rect.x;
            rights[nLong] = b.rect.x + b.rect.dx;
            int cw = nCpEst > 0 ? b.rect.dx / nCpEst : b.rect.dy;
            ems[nLong] = cw > 0 ? cw : 16;
            nLong++;
        }
        if (prevI >= 0 && !OcrBoxesOnSameLine(boxes[prevI].rect, b.rect)) {
            int g = b.rect.y - (boxes[prevI].rect.y + boxes[prevI].rect.dy);
            if (g < 0) {
                g = 0;
            }
            gaps[nGap++] = g;
        }
        prevI = i;
    }
    int bodyLeft = OcrMedianInts(lefts, nLong);
    int bodyRight = OcrMedianInts(rights, nLong);
    int em = OcrMedianInts(ems, nLong);
    int gapMed = OcrMedianInts(gaps, nGap);
    if (em < 8) {
        em = 16;
    }
    for (int bi = 0; bi < boxes.Size(); bi++) {
        const OcrBox& b = boxes[bi];
        if (!b.text || !b.text[0]) {
            continue;
        }
        int tlen = (int)str::Len(b.text);
        int nCp = 0;
        int idx = 0;
        while (idx < tlen) {
            int before = idx;
            Utf8CodepointNext(b.text, tlen, idx);
            if (idx <= before) {
                break;
            }
            nCp++;
        }
        if (nCp < 1) {
            nCp = 1;
        }
        Rect pageR((int)(b.rect.x * sx + pageBox.x), (int)(b.rect.y * sy + pageBox.y), (int)(b.rect.dx * sx),
                   (int)(b.rect.dy * sy));
        if (pageR.dx < 1) {
            pageR.dx = 1;
        }
        if (pageR.dy < 1) {
            pageR.dy = 1;
        }
        float* units = AllocArray<float>(nCp);
        int* cps = AllocArray<int>(nCp);
        float unitSum = 0.f;
        idx = 0;
        int ui = 0;
        while (idx < tlen && ui < nCp) {
            int before = idx;
            int cp = Utf8CodepointNext(b.text, tlen, idx);
            if (idx <= before) {
                break;
            }
            cps[ui] = cp;
            units[ui] = OcrGlyphUnit(cp);
            ui++;
        }
        OcrSqueezePunctUnits(cps, units, ui);
        unitSum = 0.f;
        for (int i = 0; i < ui; i++) {
            unitSum += units[i];
        }
        if (unitSum < 0.01f) {
            unitSum = (float)nCp;
        }
        int* gx0 = AllocArray<int>(nCp);
        int* gx1 = AllocArray<int>(nCp);
        bool vert = OcrBoxIsVerticalCol(b);
        if (vert) {
            OcrPlaceGlyphYs(nCp, b.rect, gx0, gx1, b.charX, b.nChar);
        } else {
            OcrPlaceGlyphXs(cps, units, nCp, unitSum, rgb, imgW, imgH, stride, b.rect, gx0, gx1, b.charX, b.nChar);
        }
        int lineEm = em;
        int nHan = 0;
        int sumHan = 0;
        for (int i = 0; i < nCp; i++) {
            if (OcrIsPunct(cps[i])) {
                continue;
            }
            int tw = gx1[i] - gx0[i];
            if (tw > em / 2) {
                sumHan += tw;
                nHan++;
            }
        }
        if (nHan > 0) {
            int avg = sumHan / nHan;
            if (avg > lineEm) {
                lineEm = avg;
            }
        }
        if (vert && b.rect.dx > lineEm) {
            lineEm = b.rect.dx;
        }
        OcrExpandPunctCells(cps, nCp, lineEm, gx0, gx1);
        int padX = lineEm / 10;
        if (padX < 2) {
            padX = 2;
        }
        OcrInsetGlyphXs(nCp, padX, gx0, gx1);
        bool lonePunct = nCp == 1 && OcrIsPunct(cps[0]);
        if (lonePunct && !vert) {
            int lineH = OcrSameLineMaxDy(boxes, bi, lineEm);
            int wantDy = (int)((float)lineH * sy + 0.5f);
            if (wantDy < 1) {
                wantDy = 1;
            }
            if (pageR.dy < wantDy) {
                int cy = pageR.y + pageR.dy / 2;
                pageR.y = cy - wantDy / 2;
                pageR.dy = wantDy;
            }
        }
        int restOff = 0;
        if (tlen > 0) {
            Utf8CodepointNext(b.text, tlen, restOff);
        }
        const char* rest = (restOff > 0 && restOff < tlen) ? b.text + restOff : nullptr;
        int skipCp = 0;
        if (OcrShouldDropLeadingDirtDot(cps, nCp, rest, b.charX, b.nChar)) {
            skipCp = 1;
        }
        int nextDirt = bi + 1;
        while (nextDirt < nBox && (!boxes[nextDirt].text || !boxes[nextDirt].text[0])) {
            nextDirt++;
        }
        bool skipBox = false;
        if (nCp == 1 && OcrIsDirtBulletCp(cps[0]) && nextDirt < nBox &&
            OcrBoxesOnSameLine(b.rect, boxes[nextDirt].rect) && OcrStartsLikeGbtHeading(boxes[nextDirt].text)) {
            int nh = boxes[nextDirt].rect.dy > 0 ? boxes[nextDirt].rect.dy : em;
            if (b.rect.dx <= nh || b.rect.dy * 2 <= nh) {
                skipBox = true;
            }
        }
        if (!skipBox && nCp == 1 && OcrIsLeftSplitRadicalCp(cps[0]) && nextDirt < nBox &&
            OcrBoxesOnSameLine(b.rect, boxes[nextDirt].rect)) {
            int t2 = (int)str::Len(boxes[nextDirt].text);
            int j = 0;
            int host = t2 > 0 ? Utf8CodepointNext(boxes[nextDirt].text, t2, j) : 0;
            int ov = (b.rect.x + b.rect.dx) - boxes[nextDirt].rect.x;
            if (OcrShouldDropSplitRadical(cps[0], host, b.rect.dx, boxes[nextDirt].rect.dx, ov)) {
                skipBox = true;
            }
        }
        u8* dropCp = AllocArray<u8>(nCp);
        for (int i = 0; i + 1 < nCp; i++) {
            int w0 = OcrCtcSpanW(b.charX, b.nChar, nCp, i);
            int w1 = OcrCtcSpanW(b.charX, b.nChar, nCp, i + 1);
            int ov = OcrCtcOverlap(b.charX, b.nChar, nCp, i);
            if (OcrShouldDropSplitRadical(cps[i], cps[i + 1], w0, w1, ov)) {
                dropCp[i] = 1;
            }
        }
        if (nCp >= 1 && nextDirt < nBox && OcrBoxesOnSameLine(b.rect, boxes[nextDirt].rect)) {
            int t2 = (int)str::Len(boxes[nextDirt].text);
            int j = 0;
            int host = t2 > 0 ? Utf8CodepointNext(boxes[nextDirt].text, t2, j) : 0;
            int ov = (b.rect.x + b.rect.dx) - boxes[nextDirt].rect.x;
            int minH = b.rect.dy < boxes[nextDirt].rect.dy ? b.rect.dy : boxes[nextDirt].rect.dy;
            int minOv = minH / 4;
            if (minOv < 8) {
                minOv = 8;
            }
            if (ov < minOv) {
                ov = 0;
            }
            int w0 = OcrCtcSpanW(b.charX, b.nChar, nCp, nCp - 1);
            int w1 = OcrCtcSpanW(boxes[nextDirt].charX, boxes[nextDirt].nChar, boxes[nextDirt].nChar, 0);
            if (OcrShouldDropSplitRadical(cps[nCp - 1], host, w0, w1, ov)) {
                dropCp[nCp - 1] = 1;
            }
        }
        idx = 0;
        int cpI = 0;
        while (idx < tlen) {
            int before = idx;
            Utf8CodepointNext(b.text, tlen, idx);
            if (idx <= before) {
                break;
            }
            if (skipBox || cpI < skipCp || (cpI < nCp && dropCp[cpI])) {
                cpI++;
                continue;
            }
            Rect cr = pageR;
            int ix0 = (cpI < nCp) ? gx0[cpI] : 0;
            int ix1 = (cpI < nCp) ? gx1[cpI] : (vert ? b.rect.dy : b.rect.dx);
            if (vert) {
                cr.x = pageR.x;
                cr.dx = pageR.dx > 0 ? pageR.dx : 1;
                cr.y = pageR.y + (int)((float)ix0 * sy + 0.5f);
                int cellEnd = pageR.y + (int)((float)ix1 * sy + 0.5f);
                cr.dy = (cellEnd > cr.y) ? (cellEnd - cr.y) : 1;
            } else {
                cr.x = pageR.x + (int)((float)ix0 * sx + 0.5f);
                int cellEnd = pageR.x + (int)((float)ix1 * sx + 0.5f);
                cr.dx = (cellEnd > cr.x) ? (cellEnd - cr.x) : 1;
            }
            utf.Append(b.text + before, (size_t)(idx - before));
            for (int k = before; k < idx; k++) {
                utfCoords.Append(cr);
            }
            cpI++;
        }
        free(units);
        free(cps);
        free(gx0);
        free(gx1);
        free(dropCp);
        int next = bi + 1;
        while (next < nBox && (!boxes[next].text || !boxes[next].text[0])) {
            next++;
        }
        bool join = false;
        if (next < nBox) {
            join = OcrShouldJoinDocLines(b, boxes[next], bodyLeft, bodyRight, em, gapMed);
        }
        if (!join) {
            utf.Append("\n");
            utfCoords.Append(Rect());
        }
    }
    free(lefts);
    free(rights);
    free(ems);
    free(gaps);
    if (utf.Size() == 0) {
        return;
    }
    utf8->text = utf.StealData();
    utf8->len = (int)str::Len(utf8->text);
    utf8->coords = utfCoords.StealData();
    pt->text = ToWStr(utf8->text);
    pt->len = str::Leni(pt->text);
    if (pt->len > 0) {
        pt->coords = AllocArray<Rect>(pt->len);
        int srcByteIdx = 0;
        int dstIdx = 0;
        int srcByteLen = utf8->len;
        while (srcByteIdx < srcByteLen && dstIdx < pt->len) {
            int runeByteIdx = srcByteIdx;
            int rune = Utf8CodepointNext(utf8->text, srcByteLen, srcByteIdx);
            if (srcByteIdx <= runeByteIdx) {
                break;
            }
            Rect r = utf8->coords[runeByteIdx];
            pt->coords[dstIdx++] = r;
            if (rune >= 0x10000 && rune <= 0x10ffff && dstIdx < pt->len) {
                pt->coords[dstIdx++] = r;
            }
        }
        for (int i = dstIdx; i < pt->len; i++) {
            pt->coords[i] = dstIdx > 0 ? pt->coords[dstIdx - 1] : Rect{};
        }
    }
}

static void FinishOcrFlight(OcrFlight* owned) {
    if (!owned) {
        return;
    }
    SetEvent(owned->doneEvent);
    gFlightLock.Lock();
    for (int i = 0; i < gFlights.Size(); i++) {
        if (gFlights[i] == owned) {
            gFlights.RemoveAt((size_t)i);
            break;
        }
    }
    gFlightLock.Unlock();
    CloseHandle(owned->doneEvent);
    delete owned;
}

bool OcrRecognizeEnginePage(EngineBase* engine, int pageNo, bool forceOcr, OcrOperation op) {
    if (!engine || pageNo < 1 || pageNo > engine->PageCount()) {
        return false;
    }
    logfa("OCR[%d] ENTER forceOcr=%d op=%d modelsAvail=%d\n", pageNo, forceOcr, (int)op, OcrModelsAvailable());

    HANDLE waitEv = nullptr;
    OcrFlight* owned = nullptr;
    gFlightLock.Lock();
    OcrFlight* existing = FindFlightLocked(engine, pageNo);
    if (existing) {
        DuplicateHandle(GetCurrentProcess(), existing->doneEvent, GetCurrentProcess(), &waitEv, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    } else if (engine->WasOcrTried(pageNo) && !forceOcr) {
        gFlightLock.Unlock();
        logfa("OCR[%d] SKIP was tried earlier\n", pageNo);
        bool usable = engine->PageHasUsableText(pageNo);
        return usable;
    } else {
        owned = new OcrFlight();
        owned->engine = engine;
        owned->pageNo = pageNo;
        owned->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        gFlights.Append(owned);
    }
    gFlightLock.Unlock();

    if (waitEv) {
        WaitForSingleObject(waitEv, 120000);
        CloseHandle(waitEv);
        bool usable = engine->PageHasUsableText(pageNo);
        return usable;
    }

    bool ok = false;
    if (OcrModelsAvailable()) {
        bool scanned = OcrPageLooksScanned(engine, pageNo);
        logfa("OCR[%d] scanned=%d forceOcr=%d\n", pageNo, scanned, forceOcr);
        if (!scanned && !forceOcr) {
            FinishOcrFlight(owned);
            logfa("OCR[%d] SKIP not scanned\n", pageNo);
            bool usable = engine->PageHasUsableText(pageNo);
            return usable;
        }
        engine->MarkOcrTried(pageNo);
        OcrProfile profile = GetOcrProfileForOperation(op);
        OcrPageTiming timing{};
        LARGE_INTEGER tPage = TimeGet();
        LARGE_INTEGER tRaster = TimeGet();
        RenderedBitmap* bmp = RenderPageForOcr(engine, pageNo);
        timing.rasterizeMs = TimeSinceInMs(tRaster);
        logfa("OCR[%d] bmp=%p valid=%d\n", pageNo, bmp, bmp ? bmp->IsValid() : 0);
        if (bmp && bmp->IsValid()) {
            int w = 0, h = 0, stride = 0;
            u8* rgb = CopyBitmapToRgb(bmp->GetBitmap(), &w, &h, &stride);
            delete bmp;
            bmp = nullptr;
            logfa("OCR[%d] rgb=%p w=%d h=%d stride=%d\n", pageNo, rgb, w, h, stride);
            if (rgb) {
                Vec<OcrBox> boxes;
                int usedRot = 0;
                // RapidOrientation model pre-check: detect page direction before
                // spending time on OCR. Recommended rotation angles (90/270) are
                // added as rotation candidates alongside the heuristic 90/270 sweep.
                int modelDeg = 0;
                float modelConf = 0;
                bool modelOk = OcrClassifyPageOrientationRgb(rgb, w, h, stride, &modelDeg, &modelConf);
                bool hasModelHint = modelOk && (modelDeg == 90 || modelDeg == 270);
                bool ok0 = OcrRecognizeRgb(rgb, w, h, stride, boxes, profile, &timing);
                int score0 = ok0 ? ScoreOcrBoxes(boxes) : 0;
                logfa("OCR[%d] 0deg ocr: ok=%d score=%d nBoxes=%d\n", pageNo, ok0, score0, boxes.Size());
                if (!ok0) {
                    FreeOcrBoxes(boxes);
                    score0 = 0;
                }
                bool vertical0 = OcrBoxesLookLikeVerticalBook(boxes, w, h);
                bool shouldTryHeuristic = OcrShouldTryPageRotate(boxes, w, h);
                logfa("OCR[%d] vertical0=%d shouldTryHeuristic=%d\n", pageNo, vertical0, shouldTryHeuristic);
                if (hasModelHint || shouldTryHeuristic) {
                    int bestScore = score0;
                    int bestRot = 0;
                    Vec<OcrBox> bestBoxes;
                    // Build candidate rotation list. Model-recommended angle (if any)
                    // is evaluated first with a looser threshold; heuristic angles
                    // need a larger score gain before they win.
                    int candRots[3] = {};
                    int nCand = 0;
                    if (hasModelHint) {
                        candRots[nCand++] = modelDeg;
                    }
                    if (!hasModelHint || modelDeg != 90) {
                        candRots[nCand++] = 90;
                    }
                    if (!hasModelHint || modelDeg != 270) {
                        candRots[nCand++] = 270;
                    }
                    for (int ci = 0; ci < nCand; ci++) {
                        int rotDeg = candRots[ci];
                        bool cw = rotDeg == 90;
                        int nw = 0, nh = 0, ns = 0;
                        u8* rot = RotateRgb90(rgb, w, h, stride, cw, &nw, &nh, &ns);
                        if (!rot) {
                            logfa("OCR[%d] rot %d: RotateRgb90 failed\n", pageNo, rotDeg);
                            continue;
                        }
                        Vec<OcrBox> rotBoxes;
                        int rotScore = 0;
                        bool rotOk = OcrRecognizeRgbScored(rot, nw, nh, ns, rotBoxes, &rotScore, profile);
                        // Model-recommended angle: accept if score is close to or
                        // above baseline; heuristic angles need +18 gain to override.
                        int threshold = (hasModelHint && rotDeg == modelDeg) ? 0 : 18;
                        logfa("OCR[%d] rot %d: ocrOk=%d score=%d best=%d threshold=%d\n", pageNo, rotDeg, rotOk, rotScore,
                              bestScore, threshold);
                        if (rotOk && rotScore > bestScore + threshold) {
                            MapOcrBoxesFrom90(rotBoxes, w, h, cw);
                            FreeOcrBoxes(bestBoxes);
                            bestBoxes = rotBoxes;
                            rotBoxes.Reset();
                            bestScore = rotScore;
                            bestRot = rotDeg;
                            logfa("OCR[%d] rot %d WINS: score=%d > best+%d=%d\n", pageNo, rotDeg, rotScore, threshold,
                                  bestScore - rotScore);
                        } else {
                            FreeOcrBoxes(rotBoxes);
                        }
                        free(rot);
                    }
                    if (bestRot != 0) {
                        if (hasModelHint) {
                            logfa("OCR[%d] ROTATE %d FINAL score=%d > %d modelConf=%.2f\n", pageNo, bestRot, bestScore,
                                  score0, modelConf);
                        } else {
                            logfa("OCR[%d] ROTATE %d FINAL score=%d > %d (heuristic)\n", pageNo, bestRot, bestScore, score0);
                        }
                        FreeOcrBoxes(boxes);
                        boxes = bestBoxes;
                        bestBoxes.Reset();
                        usedRot = bestRot;
                    } else {
                        FreeOcrBoxes(bestBoxes);
                        if (hasModelHint) {
                            logfa("OCR[%d] model suggested %d (conf %.2f) but score %d <= baseline %d\n", pageNo,
                                  modelDeg, modelConf, bestScore, score0);
                        } else {
                            logfa("OCR[%d] no rotation won (heuristic only)\n", pageNo);
                        }
                    }
                } else {
                    logfa("OCR[%d] no rotation attempt (hasHint=%d shouldTry=%d)\n", pageNo, hasModelHint,
                          shouldTryHeuristic);
                }
                // Books / 竖版书: keep the page upright. 90° OCR may still feed
                // recognition (boxes already mapped back). 公文 forms still bake.
                // However: the orientation model has already visually classified
                // the page direction. Don't let the OCR-box-shape-based vertical0
                // heuristic override a model-driven rotation — sideways tables in
                // portrait PDFs look like vertical books when viewed at 0°.
                if (usedRot != 0 && !(hasModelHint && usedRot == modelDeg)) {
                    bool officialName = OcrFileNameHasOfficialKind(engine->FilePath());
                    bool officialText = OcrBoxesLookLikeOfficialSideways(boxes);
                    bool verticalNow = OcrBoxesLookLikeVerticalBook(boxes, w, h);
                    logfa("OCR[%d] post-check (heuristic): vertical0=%d verticalNow=%d officialName=%d officialText=%d\n",
                          pageNo, vertical0, verticalNow, officialName, officialText);
                    if ((vertical0 || verticalNow) && !officialName && !officialText) {
                        logfa("OCR[%d] KEEP 0 deg (vertical book, was %d)\n", pageNo, usedRot);
                        usedRot = 0;
                    }
                } else if (usedRot != 0) {
                    logfa("OCR[%d] post-check: rotation from model, skipping vertical0 guard\n", pageNo);
                }
                if (OcrPageBoxesAreVertical(boxes)) {
                    SortOcrBoxesVerticalReading(boxes);
                }
                ok = ScoreOcrBoxes(boxes) > 0;
                logfa("OCR[%d] final: ok=%d score=%d usedRot=%d\n", pageNo, ok, ScoreOcrBoxes(boxes), usedRot);
                if (ok) {
                    PageText pt{};
                    PageTextUtf8 utf8{};
                    LARGE_INTEGER tText = TimeGet();
                    BoxesToPageText(boxes, rgb, w, h, stride, engine->PageMediabox(pageNo), &pt, &utf8);
                    timing.textLayerMs = TimeSinceInMs(tText);
                    FreeOcrBoxes(boxes);
                    if (pt.text && pt.len > 0) {
                        engine->SetCachedPageText(pageNo, pt, utf8);
                        engine->SetOcrPageRotate(pageNo, usedRot);
                        engine->SetOcrCacheQuality(pageNo, OcrQualityForProfile(profile));
                        logfa("OCR[%d] SetOcrPageRotate(%d)\n", pageNo, usedRot);
                    } else {
                        FreePageText(&pt);
                        FreePageTextUtf8(&utf8);
                        ok = false;
                        engine->SetOcrPageRotate(pageNo, 0);
                    }
                } else {
                    FreeOcrBoxes(boxes);
                    engine->SetOcrPageRotate(pageNo, 0);
                }
                free(rgb);
            }
        } else {
            logfa("OCR[%d] bmp invalid\n", pageNo);
            delete bmp;
        }
        timing.pageTotalMs = TimeSinceInMs(tPage);
        OcrLogPageTiming(pageNo, profile, timing);
        if (op == OcrOperation::AllPages || op == OcrOperation::SaveSearchable) {
            OcrAccumPageTiming(timing);
        }
    } else {
        logfa("OCR[%d] MODELS NOT AVAILABLE\n", pageNo);
    }
    FinishOcrFlight(owned);
    logfa("OCR[%d] EXIT ok=%d\n", pageNo, ok);
    return ok;
}

static bool OcrRecognizePageClip(EngineBase* engine, int pageNo, const RectF& clip, PageText* ptOut,
                                 PageTextUtf8* utf8Out) {
    *ptOut = {};
    *utf8Out = {};
    if (!engine || !OcrModelsAvailable()) {
        return false;
    }
    RenderedBitmap* bmp = RenderPageForOcr(engine, pageNo, &clip);
    if (!bmp || !bmp->IsValid()) {
        delete bmp;
        return false;
    }
    int w = 0, h = 0, stride = 0;
    u8* rgb = CopyBitmapToRgb(bmp->GetBitmap(), &w, &h, &stride);
    delete bmp;
    if (!rgb) {
        return false;
    }
    Vec<OcrBox> boxes;
    OcrProfile profile = GetOcrProfileForOperation(OcrOperation::Region);
    OcrPageTiming timing{};
    LARGE_INTEGER t0 = TimeGet();
    bool ok = OcrRecognizeRgb(rgb, w, h, stride, boxes, profile, &timing);
    timing.pageTotalMs = TimeSinceInMs(t0);
    logfa("OCR region timing page=%d clip=%.0fx%.0f raster=%dx%d profile=%s det=%.0f rec=%.0f (pre=%.0f inf=%.0f "
          "post=%.0f) crop=%.0f total=%.0f detBoxes=%d recBoxes=%d batches=%d\n",
          pageNo, clip.dx, clip.dy, w, h, OcrProfileName(profile),
          timing.detPreprocessMs + timing.detInferenceMs + timing.detPostprocessMs,
          timing.recPreprocessMs + timing.recInferenceMs + timing.recPostprocessMs, timing.recPreprocessMs,
          timing.recInferenceMs, timing.recPostprocessMs, timing.cropMs, timing.pageTotalMs, timing.nDetBoxes,
          timing.nRecBoxes, timing.nRecBatches);
    if (ok) {
        BoxesToPageText(boxes, rgb, w, h, stride, clip, ptOut, utf8Out);
        ok = ptOut->text && ptOut->len > 0;
        if (!ok) {
            FreePageText(ptOut);
            FreePageTextUtf8(utf8Out);
        }
    }
    FreeOcrBoxes(boxes);
    free(rgb);
    return ok;
}

void OcrEnsurePageTextForSearch(EngineBase* engine, int pageNo) {
    if (!OcrEngineKindSupported(engine)) {
        return;
    }
    // Gate on the owning tab's Auto OCR state (search has no MainWindow*).
    WindowTab* tab = FindTabByFile(engine->FilePath());
    if (!tab || !tab->autoOcrOn) {
        return;
    }
    if (WaitIfOcrInFlight(engine, pageNo)) {
        return;
    }
    if (OcrPageShouldRecognize(engine, pageNo, OcrOperation::Auto, nullptr)) {
        OcrRecognizeEnginePage(engine, pageNo, false, OcrOperation::Auto);
    }
}

struct OcrDoneUi {
    HWND hwndCanvas = nullptr;
    EngineBase* engine = nullptr;
    int pageNo = 0;
    LONG cancelSeq = 0;
    bool ok = false;
    bool documentJob = false;
    bool regionJob = false;
    bool showStatus = false;
    RectF clipRect;
    char* regionText = nullptr;
};

static void OcrShowRecognizingUi(OcrDoneUi* d) {
    ShowOcrStatus(d->hwndCanvas, _TRA("Scanning…"), kNotifNoTimeout);
    delete d;
}

static void OcrShowDocumentProgress(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || gOcrDocTotal <= 0) {
        return;
    }
    int done = gOcrDocDone;
    int total = gOcrDocTotal;
    if (done > total) {
        done = total;
    }
    int perc = CalcPerc(done, total);
    TempStr msg = str::FormatTemp(_TRA("Scanning… %d / %d"), done, total);
    NotificationWnd* wnd = GetNotificationForGroup(hwnd, kNotifOcr);
    if (wnd) {
        UpdateNotificationProgress(wnd, msg, perc);
        return;
    }
    NotificationCreateArgs args;
    args.hwndParent = hwnd;
    args.groupId = kNotifOcr;
    args.msg = msg;
    args.timeoutMs = kNotifNoTimeout;
    args.onRemoved = MkFunc1Void(OcrOnProgressClosed);
    wnd = ShowNotification(args);
    if (wnd) {
        UpdateNotificationProgress(wnd, msg, perc);
    }
}

static bool OcrQueueHasDocumentJob(EngineBase* engine) {
    for (int i = 0; i < gQueue.Size(); i++) {
        auto* j = (OcrJob*)gQueue[i];
        if (j->documentJob && j->engine == engine) {
            return true;
        }
    }
    return false;
}

static void OcrClearPendingSave() {
    if (gOcrPendingSaveEngine) {
        gOcrPendingSaveEngine->Release();
        gOcrPendingSaveEngine = nullptr;
    }
    gOcrPendingSaveHwnd = nullptr;
    str::FreePtr(&gOcrPendingSavePath);
    gOcrPendingSaveExtractToc = false;
}

static void OcrClearPendingExtractToc() {
    if (gOcrPendingExtractEngine) {
        gOcrPendingExtractEngine->Release();
        gOcrPendingExtractEngine = nullptr;
    }
    gOcrPendingExtractHwnd = nullptr;
    gOcrPendingExtractPersist = false;
}

static void OcrSetPendingSave(EngineBase* engine, HWND hwnd, const char* destPath, bool extractTocWhenDone) {
    OcrClearPendingExtractToc();
    OcrClearPendingSave();
    if (!engine || str::IsEmpty(destPath)) {
        return;
    }
    engine->AddRef();
    gOcrPendingSaveEngine = engine;
    gOcrPendingSaveHwnd = hwnd;
    gOcrPendingSavePath = str::Dup(destPath);
    gOcrPendingSaveExtractToc = extractTocWhenDone;
}

static char* AllocOcrSaveTempPath(const char* destPath) {
    if (str::IsEmpty(destPath)) {
        return nullptr;
    }
    for (int i = 0; i < 32; i++) {
        char* p = (i == 0) ? str::Join(destPath, ".ocr-tmp") : str::Format("%s.ocr-tmp-%d", destPath, i);
        if (p && !file::Exists(p)) {
            return p;
        }
        str::Free(p);
    }
    return str::Join(destPath, ".ocr-tmp");
}

static bool OcrWriteSearchablePdfToPath(EngineBase* engine, HWND hwnd, const char* destPath, bool extractTocWhenDone,
                                        bool showDone) {
    if (!engine || str::IsEmpty(destPath)) {
        return false;
    }
    const char* srcPath = engine->FilePath();
    bool overwriteOpen = srcPath && path::IsSame(srcPath, destPath);
    char* tmpPath = nullptr;
    const char* writePath = destPath;
    if (overwriteOpen) {
        tmpPath = AllocOcrSaveTempPath(destPath);
        if (!tmpPath) {
            return false;
        }
        writePath = tmpPath;
    }
    char* err = nullptr;
    LARGE_INTEGER tSave = TimeGet();
    bool ok = EngineMupdfSaveSearchablePdf(engine, writePath, &err);
    double saveMs = TimeSinceInMs(tSave);
    OcrLogDocSummary("save", saveMs);
    if (!hwnd || !IsWindow(hwnd)) {
        if (tmpPath && !ok) {
            file::Delete(tmpPath);
        }
        str::Free(tmpPath);
        str::Free(err);
        return ok;
    }
    if (ok) {
        engine->ClearUnsavedOcrText();
        MainWindow* win = FindMainWindowByHwnd(hwnd);
        int loaded = 0;
        if (win) {
            SwitchCurrentTabToSavedFile(win, destPath, overwriteOpen ? tmpPath : nullptr);
            loaded = win->IsDocLoaded() ? 1 : 0;
        }
        str::Free(tmpPath);
        tmpPath = nullptr;
        if (extractTocWhenDone && win && loaded) {
            HandleExtractPdfTocCommand(win, true, true);
        }
        if (showDone) {
            OcrShowQuietDone(hwnd, _TRA("Saved."));
        }
        if (win) {
            ToolbarUpdateStateForWindow(win, false);
        }
    } else {
        if (tmpPath) {
            file::Delete(tmpPath);
            str::Free(tmpPath);
            tmpPath = nullptr;
        }
        HideOcrStatus(hwnd);
        const char* detail = err;
        if (detail && str::Eq(detail, "Could not save searchable PDF.")) {
            detail = nullptr;
        }
        if (detail && str::Eq(detail, "No OCR text to save. Recognize pages first.")) {
            ShowWarningNotification(hwnd, _TRA("No OCR text to save. Recognize pages first."), kNotif5SecsTimeOut);
        } else if (detail) {
            TempStr msg = str::FormatTemp("%s\n%s", _TRA("Could not save searchable PDF."), detail);
            ShowWarningNotification(hwnd, msg, kNotif5SecsTimeOut);
        } else {
            ShowWarningNotification(hwnd, _TRA("Could not save searchable PDF."), kNotif5SecsTimeOut);
        }
    }
    str::Free(err);
    return ok;
}

bool OcrSaveCachedSearchablePdf(MainWindow* win, const char* destPath) {
    if (!win || str::IsEmpty(destPath)) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return false;
    }
    return OcrWriteSearchablePdfToPath(engine, win->hwndCanvas, destPath, false, false);
}

static void OcrFinishPendingSaveIfAny(EngineBase* engine) {
    if (!gOcrPendingSavePath || gOcrPendingSaveEngine != engine) {
        return;
    }
    HWND hwnd = gOcrPendingSaveHwnd;
    char* path = gOcrPendingSavePath;
    EngineBase* pendingEngine = gOcrPendingSaveEngine;
    bool extractToc = gOcrPendingSaveExtractToc;
    gOcrPendingSavePath = nullptr;
    gOcrPendingSaveEngine = nullptr;
    gOcrPendingSaveHwnd = nullptr;
    gOcrPendingSaveExtractToc = false;
    OcrWriteSearchablePdfToPath(pendingEngine, hwnd, path, extractToc, true);
    pendingEngine->Release();
    str::Free(path);
}

static void OcrShowPageFailed(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    HideOcrStatus(hwnd);
    const char* err = OcrLastError();
    if (err) {
        ShowWarningNotification(hwnd, str::FormatTemp("%s\n%s", _TRA("Could not recognize text on this page."), err),
                                kNotif5SecsTimeOut);
    } else {
        ShowWarningNotification(hwnd, _TRA("Could not recognize text on this page."), kNotif5SecsTimeOut);
    }
}

static void OcrApplyRegionResult(OcrDoneUi* d) {
    MainWindow* win = FindMainWindowByHwnd(d->hwndCanvas);
    if (d->regionText && d->regionText[0]) {
        CopyTextToClipboard(d->regionText);
    }
    if (!win) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (dm && dm->GetEngine() == d->engine) {
        // Highlight the drawn box (geometric), not a reading-order range from
        // the box corners — that selected glyphs outside the rectangle.
        DeleteOldSelectionInfo(win, true);
        Rect screen = dm->CvtToScreen(d->pageNo, d->clipRect);
        WindowTab* tab = win->CurrentTab();
        if (tab) {
            tab->selectionOnPage = SelectionOnPage::FromRectangle(dm, screen);
            win->showSelection = tab->selectionOnPage != nullptr;
            if (win->showSelection) {
                ShowSelectionToolbar(win);
            }
        }
    }
    ScheduleRepaint(win, 0);
}

static void OcrFinishUi(OcrDoneUi* d) {
    LONG liveSeq = InterlockedCompareExchange(&gOcrCancelSeq, 0, 0);
    if (d->cancelSeq != liveSeq) {
        if (d->engine) {
            d->engine->Release();
        }
        str::Free(d->regionText);
        delete d;
        return;
    }
    if (d->documentJob) {
        bool more = false;
        int done = 0;
        int total = 0;
        int qLeft = 0;
        gQueueLock.Lock();
        gOcrDocDone++;
        done = gOcrDocDone;
        total = gOcrDocTotal;
        more = OcrQueueHasDocumentJob(d->engine) || gOcrDocDone < gOcrDocTotal;
        for (int i = 0; i < gQueue.Size(); i++) {
            auto* j = (OcrJob*)gQueue[i];
            if (j->documentJob && j->engine == d->engine) {
                qLeft++;
            }
        }
        gQueueLock.Unlock();
        if (d->ok && d->hwndCanvas && IsWindow(d->hwndCanvas)) {
            InvalidateRect(d->hwndCanvas, nullptr, FALSE);
        }
        if (more) {
            OcrShowDocumentProgress(d->hwndCanvas);
        } else {
            bool saving = gOcrPendingSavePath && gOcrPendingSaveEngine == d->engine;
            bool extractToc = gOcrPendingExtractEngine == d->engine;
            gOcrDocEngine = nullptr;
            gOcrDocHwnd = nullptr;
            gOcrDocTotal = 0;
            gOcrDocDone = 0;
            if (saving) {
                // Apply rotations + MediaBox swaps to the running engine BEFORE saving,
                // so the display is updated AND the file copy opened by Save sees the
                // correct state as well (the running engine is the authoritative copy).
                if (d->engine) {
                    int changed = EngineMupdfApplyPendingOcrPageRotates(d->engine);
                    logfa("OCR[doc-save] ApplyPendingOcrPageRotates changed=%d\n", changed);
                    if (changed > 0) {
                        MainWindow* win =
                            d->hwndCanvas && IsWindow(d->hwndCanvas) ? FindMainWindowByHwnd(d->hwndCanvas) : nullptr;
                        DisplayModel* dm = win ? win->AsFixed() : nullptr;
                        if (dm && dm->GetEngine() == d->engine) {
                            // Must clear DisplayModel's cached page sizes so Relayout
                            // re-reads the (now rotated) MediaBox from the engine.
                            // Without this, pageOnScreen stays portrait and clips the
                            // right half of landscape pages.
                            dm->InvalidateReflowLayoutAfterEngineReparse();
                            dm->Relayout(dm->GetZoomVirtual(), dm->GetRotation());
                        }
                    }
                }
                OcrFinishPendingSaveIfAny(d->engine);
            } else if (extractToc) {
                OcrLogDocSummary("all-pages", 0);
                // Also apply orientation-detected page rotations before TOC extraction
                // (the else-for-extractToc branch had been skipping this entirely, so
                // sideways tables would never get rotated until a later save)
                if (d->engine) {
                    int changed = EngineMupdfApplyPendingOcrPageRotates(d->engine);
                    logfa("OCR[doc-extractToc] ApplyPendingOcrPageRotates changed=%d\n", changed);
                }
                HWND hwnd = gOcrPendingExtractHwnd;
                bool persist = gOcrPendingExtractPersist;
                OcrClearPendingExtractToc();
                HideOcrStatus(hwnd ? hwnd : d->hwndCanvas);
                MainWindow* win = hwnd && IsWindow(hwnd) ? FindMainWindowByHwnd(hwnd) : nullptr;
                if (d->engine && d->engine->CountOcrCachedPages() > 0) {
                    d->engine->MarkUnsavedOcrText();
                }
                if (win) {
                    // Relayout so rotated pages are shown upright when TOC is displayed
                    DisplayModel* dm = win->AsFixed();
                    if (dm && dm->GetEngine() == d->engine) {
                        dm->InvalidateReflowLayoutAfterEngineReparse();
                        dm->Relayout(dm->GetZoomVirtual(), dm->GetRotation());
                    }
                    HandleExtractPdfTocCommand(win, true, persist);
                    ToolbarUpdateStateForWindow(win, false);
                }
            } else {
                OcrLogDocSummary("all-pages", 0);
                if (d->engine && d->engine->CountOcrCachedPages() > 0) {
                    d->engine->MarkUnsavedOcrText();
                }
                if (d->engine) {
                    int changed = EngineMupdfApplyPendingOcrPageRotates(d->engine);
                    logfa("OCR[doc] ApplyPendingOcrPageRotates changed=%d\n", changed);
                    if (changed > 0) {
                        MainWindow* win =
                            d->hwndCanvas && IsWindow(d->hwndCanvas) ? FindMainWindowByHwnd(d->hwndCanvas) : nullptr;
                        DisplayModel* dm = win ? win->AsFixed() : nullptr;
                        if (dm && dm->GetEngine() == d->engine) {
                            dm->InvalidateReflowLayoutAfterEngineReparse();
                            dm->Relayout(dm->GetZoomVirtual(), dm->GetRotation());
                        }
                    }
                }
                OcrShowQuietDone(d->hwndCanvas, _TRA("Ready to search"));
                MainWindow* win =
                    d->hwndCanvas && IsWindow(d->hwndCanvas) ? FindMainWindowByHwnd(d->hwndCanvas) : nullptr;
                if (win) {
                    ToolbarUpdateStateForWindow(win, false);
                }
            }
        }
        if (d->engine) {
            d->engine->Release();
        }
        str::Free(d->regionText);
        delete d;
        return;
    }

    if (d->ok && d->hwndCanvas && IsWindow(d->hwndCanvas)) {
        InvalidateRect(d->hwndCanvas, nullptr, FALSE);
    }
    if (d->ok && d->engine && d->pageNo > 0) {
        int rot = d->engine->GetOcrPageRotate(d->pageNo);
        logfa("OCR[page-done] page=%d rotate=%d ensureResult=%d\n", d->pageNo, rot,
              rot > 0 ? EngineMupdfEnsurePageOcrRotate(d->engine, d->pageNo) : 0);
        if (rot > 0) {
            MainWindow* win = d->hwndCanvas && IsWindow(d->hwndCanvas) ? FindMainWindowByHwnd(d->hwndCanvas) : nullptr;
            DisplayModel* dm = win ? win->AsFixed() : nullptr;
            if (dm && dm->GetEngine() == d->engine) {
                dm->Relayout(dm->GetZoomVirtual(), dm->GetRotation());
            }
        }
    }
    if (d->regionJob) {
        if (d->ok) {
            OcrApplyRegionResult(d);
            OcrShowQuietDone(d->hwndCanvas, _TRA("Copied."));
        } else {
            OcrShowPageFailed(d->hwndCanvas);
        }
    } else if (d->showStatus) {
        if (d->ok) {
            OcrShowQuietDone(d->hwndCanvas, _TRA("Ready to search"));
        } else {
            OcrShowPageFailed(d->hwndCanvas);
        }
    }
    // Auto OCR stays silent. Do not HideOcrStatus: a document scan may own the chip.
    if (d->engine) {
        d->engine->Release();
    }
    str::Free(d->regionText);
    delete d;
}

static void OcrWorker();
static void OcrQueueAutoNearby(EngineBase* engine, HWND hwnd, int centerPage);

static void StartOcrWorkerIfNeeded() {
    int want = OcrInferenceSlotCount();
    if (want < 1) {
        want = 1;
    }
    // Spawn at most min(want, queued jobs). Do not keep restarting idle workers
    // until gWorkersAlive==want: extra threads exit immediately on an empty
    // queue and this loop used to run forever on the UI thread (auto OCR).
    for (;;) {
        gQueueLock.Lock();
        int q = gQueue.Size();
        gQueueLock.Unlock();
        if (q <= 0) {
            return;
        }
        LONG cur = InterlockedCompareExchange(&gWorkersAlive, 0, 0);
        if (cur >= want || cur >= q) {
            return;
        }
        if (InterlockedCompareExchange(&gWorkersAlive, cur + 1, cur) != cur) {
            continue;
        }
        RunAsync(MkFunc0Void(OcrWorker), "OcrWorker");
    }
}

static void OcrWorker() {
    for (;;) {
        OcrJob* job = nullptr;
        gQueueLock.Lock();
        if (gQueue.Size() > 0) {
            job = (OcrJob*)gQueue.PopAt(0);
        }
        gQueueLock.Unlock();
        if (!job) {
            InterlockedDecrement(&gWorkersAlive);
            gQueueLock.Lock();
            bool more = gQueue.Size() > 0;
            gQueueLock.Unlock();
            if (more) {
                StartOcrWorkerIfNeeded();
            }
            return;
        }
        LONG liveSeq = InterlockedCompareExchange(&gOcrCancelSeq, 0, 0);
        if (job->cancelSeq != liveSeq) {
            if (job->engine) {
                job->engine->Release();
            }
            delete job;
            continue;
        }
        // Document jobs already show "Scanning… n / m". Auto OCR stays silent.
        if (job->showStatus && job->hwndCanvas && !job->documentJob) {
            auto* d = new OcrDoneUi();
            d->hwndCanvas = job->hwndCanvas;
            uitask::Post(MkFunc0<OcrDoneUi>(OcrShowRecognizingUi, d), "OcrStatus");
        }
        bool ok = false;
        char* regionTextCopy = nullptr;
        if (job->regionJob) {
            PageText regionPt{};
            PageTextUtf8 regionUtf8{};
            ok = OcrRecognizePageClip(job->engine, job->pageNo, job->clipRect, &regionPt, &regionUtf8);
            if (ok) {
                regionTextCopy = str::Dup(regionUtf8.text);
                job->engine->AppendCachedPageText(job->pageNo, regionPt, regionUtf8);
                job->engine->ClearOcrTried(job->pageNo);
            } else {
                FreePageText(&regionPt);
                FreePageTextUtf8(&regionUtf8);
            }
        } else {
            ok = OcrRecognizeEnginePage(job->engine, job->pageNo, job->forceOcr, job->op);
            if (!job->documentJob && !job->regionJob && job->autoJob) {
                OcrQueueAutoNearby(job->engine, job->hwndCanvas, job->pageNo);
            }
        }
        auto* done = new OcrDoneUi();
        done->hwndCanvas = job->hwndCanvas;
        done->engine = job->engine;
        done->pageNo = job->pageNo;
        done->cancelSeq = job->cancelSeq;
        done->ok = ok;
        done->documentJob = job->documentJob;
        done->regionJob = job->regionJob;
        done->showStatus = job->showStatus;
        done->clipRect = job->clipRect;
        done->regionText = regionTextCopy;
        job->engine = nullptr;
        delete job;
        uitask::Post(MkFunc0<OcrDoneUi>(OcrFinishUi, done), "OcrFinish");
    }
}

static bool QueueHas(EngineBase* engine, int pageNo) {
    for (int i = 0; i < gQueue.Size(); i++) {
        auto* j = (OcrJob*)gQueue[i];
        if (j->engine == engine && j->pageNo == pageNo) {
            return true;
        }
    }
    return false;
}

static void OcrEnqueueAutoPage(EngineBase* engine, HWND hwnd, int pageNo) {
    if (!engine || pageNo < 1 || pageNo > engine->PageCount()) {
        return;
    }
    bool forceOcr = false;
    if (!OcrPageShouldRecognize(engine, pageNo, OcrOperation::CurrentPage, &forceOcr)) {
        return;
    }
    gFlightLock.Lock();
    bool inFlight = FindFlightLocked(engine, pageNo) != nullptr;
    gFlightLock.Unlock();
    if (inFlight) {
        return;
    }
    gQueueLock.Lock();
    if (QueueHas(engine, pageNo)) {
        gQueueLock.Unlock();
        return;
    }
    auto* job = new OcrJob();
    job->hwndCanvas = hwnd;
    job->engine = engine;
    engine->AddRef();
    job->pageNo = pageNo;
    job->showStatus = false;
    job->forceOcr = forceOcr;
    // chained nearby-page jobs are auto jobs by definition
    job->autoJob = true;
    job->op = OcrOperation::Auto;
    job->cancelSeq = gOcrCancelSeq;
    gQueue.Append(job);
    gQueueLock.Unlock();
}

static void OcrQueueAutoNearby(EngineBase* engine, HWND hwnd, int centerPage) {
    if (!engine) {
        return;
    }
    OcrEnqueueAutoPage(engine, hwnd, centerPage + 1);
    OcrEnqueueAutoPage(engine, hwnd, centerPage - 1);
    OcrEnqueueAutoPage(engine, hwnd, centerPage + 2);
    StartOcrWorkerIfNeeded();
}

void OcrScheduleForPage(MainWindow* win, int pageNo) {
    OcrScheduleForPage(win, pageNo, false);
}

void OcrScheduleForPage(MainWindow* win, int pageNo, bool ignoreAutoPref) {
    if (!win || !gGlobalPrefs) {
        return;
    }
    // Auto path follows the owning tab's Auto OCR switch (the toolbar toggle).
    // ignoreAutoPref (explicit "OCR current page") bypasses it by design.
    if (!ignoreAutoPref && !OcrAutoEnabled(win)) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        if (ignoreAutoPref) {
            ShowWarningNotification(win->hwndCanvas, _TRA("OCR is only available for PDF and similar documents."),
                                    kNotif5SecsTimeOut);
        }
        return;
    }
    EngineBase* engine = dm->GetEngine();
    if (pageNo < 1) {
        pageNo = dm->CurrentPageNo();
    }
    if (pageNo < 1 || pageNo > engine->PageCount()) {
        if (ignoreAutoPref) {
            ShowWarningNotification(win->hwndCanvas, _TRA("No page to recognize."), kNotif5SecsTimeOut);
        }
        return;
    }
    if (!OcrEngineKindSupported(engine)) {
        if (ignoreAutoPref) {
            ShowWarningNotification(win->hwndCanvas, _TRA("OCR is not available for this document type."),
                                    kNotif5SecsTimeOut);
        }
        return;
    }
    if (!OcrSidecarLooksPresent()) {
        OcrNotifyMissingModels(win->hwndCanvas, ignoreAutoPref);
        return;
    }

    bool forceOcr = false;
    bool shouldRun = true;
    if (ignoreAutoPref) {
        // Explicit "Recognize Current Page": always re-run Balanced, even if
        // the page already has a native text layer or a previous OCR cache.
        forceOcr = true;
    } else {
        shouldRun = OcrPageShouldRecognize(engine, pageNo, OcrOperation::CurrentPage, &forceOcr);
        if (!shouldRun) {
            return;
        }
    }

    gQueueLock.Lock();
    if (QueueHas(engine, pageNo)) {
        gQueueLock.Unlock();
        if (ignoreAutoPref) {
            ShowOcrStatus(win->hwndCanvas, _TRA("Scanning…"), kNotifNoTimeout);
        }
        return;
    }
    auto* job = new OcrJob();
    job->hwndCanvas = win->hwndCanvas;
    job->engine = engine;
    engine->AddRef();
    job->pageNo = pageNo;
    job->showStatus = ignoreAutoPref;
    job->forceOcr = forceOcr;
    // Latency-first: chained Auto OCR always resolves to the Fast/Tiny profile.
    // Explicit "OCR current page" (ignoreAutoPref) stays CurrentPage/Balanced.
    job->op = ignoreAutoPref ? OcrOperation::CurrentPage : OcrOperation::Auto;
    // snapshot the auto path for the worker: auto jobs chain nearby pages,
    // explicit "OCR current page" jobs do not
    job->autoJob = !ignoreAutoPref;
    job->cancelSeq = gOcrCancelSeq;
    gQueue.Append(job);
    gQueueLock.Unlock();
    if (ignoreAutoPref) {
        ShowOcrStatus(win->hwndCanvas, _TRA("Scanning…"), kNotifNoTimeout);
    }
    StartOcrWorkerIfNeeded();
}

static int OcrQueueUnscannedPages(MainWindow* win, bool forceUncached, OcrOperation op) {
    if (!win) {
        return -1;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        ShowWarningNotification(win->hwndCanvas, _TRA("OCR is only available for PDF and similar documents."),
                                kNotif5SecsTimeOut);
        return -1;
    }
    EngineBase* engine = dm->GetEngine();
    if (!OcrEngineKindSupported(engine)) {
        ShowWarningNotification(win->hwndCanvas, _TRA("OCR is not available for this document type."),
                                kNotif5SecsTimeOut);
        return -1;
    }
    if (!OcrSidecarLooksPresent()) {
        OcrNotifyMissingModels(win->hwndCanvas, true);
        return -1;
    }
    int n = engine->PageCount();
    int queued = 0;
    gQueueLock.Lock();
    bool busyOther = gOcrDocTotal > 0 && gOcrDocEngine && gOcrDocEngine != engine;
    gQueueLock.Unlock();
    if (busyOther) {
        ShowWarningNotification(win->hwndCanvas, _TRA("OCR is already running. Please wait until it finishes."),
                                kNotif5SecsTimeOut);
        return -1;
    }
    Vec<int> pageNos;
    Vec<u8> forceFlags;
    for (int pageNo = 1; pageNo <= n; pageNo++) {
        bool forceOcr = false;
        if (forceUncached) {
            pageNos.Append(pageNo);
            forceFlags.Append(1);
            continue;
        }
        if (!OcrPageShouldRecognize(engine, pageNo, op, &forceOcr)) {
            continue;
        }
        pageNos.Append(pageNo);
        forceFlags.Append(forceOcr ? 1 : 0);
    }
    gQueueLock.Lock();
    for (int i = 0; i < pageNos.Size(); i++) {
        int pageNo = pageNos[i];
        if (QueueHas(engine, pageNo)) {
            continue;
        }
        auto* job = new OcrJob();
        job->hwndCanvas = win->hwndCanvas;
        job->engine = engine;
        engine->AddRef();
        job->pageNo = pageNo;
        job->showStatus = false;
        job->documentJob = true;
        job->forceOcr = forceFlags[i] != 0;
        job->op = op;
        job->cancelSeq = gOcrCancelSeq;
        gQueue.Append(job);
        queued++;
    }
    gQueueLock.Unlock();
    if (queued == 0) {
        return 0;
    }
    if (gOcrDocEngine != engine) {
        gOcrDocDone = 0;
        gOcrDocTotal = 0;
        OcrResetDocTiming();
    }
    gOcrDocEngine = engine;
    gOcrDocHwnd = win->hwndCanvas;
    gOcrDocTotal += queued;
    OcrShowDocumentProgress(win->hwndCanvas);
    StartOcrWorkerIfNeeded();
    return queued;
}

static void OcrClearSessionResults(EngineBase* engine) {
    if (!engine) {
        return;
    }
    int n = engine->PageCount();
    for (int pageNo = 1; pageNo <= n; pageNo++) {
        if (!engine->WasOcrTried(pageNo)) {
            continue;
        }
        engine->ClearTextCacheForPage(pageNo);
        engine->ClearOcrTried(pageNo);
        engine->SetOcrPageRotate(pageNo, 0);
    }
}

void OcrRerunAllPages(MainWindow* win, bool accurate) {
    if (gGlobalPrefs) {
        str::ReplaceWithCopy(&gGlobalPrefs->ocrFullDocumentMode, accurate ? "accurate" : "fast");
        SaveSettings();
    }
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    bool extractToc = true;
    bool autoSave = gGlobalPrefs && gGlobalPrefs->ocrAutoSave && engine && engine->kind == kindEngineMupdf &&
                    engine->FilePath() && CanAccessDisk() && !gPluginMode;
    if (autoSave && !ConfirmOcrAutoSave(win, &extractToc)) {
        return;
    }
    OcrCancelQueued(win, true);
    OcrClearSessionResults(engine);
    if (win && win->hwndCanvas && IsWindow(win->hwndCanvas)) {
        InvalidateRect(win->hwndCanvas, nullptr, FALSE);
    }
    OcrScheduleDocument(win, !autoSave, true);
    if (autoSave && engine && engine->FilePath()) {
        OcrSaveSearchablePdfAfterOcr(win, engine->FilePath(), extractToc);
    }
}

void OcrScheduleDocument(MainWindow* win, bool extractTocIfMissing, bool forceOcrAll) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    // After full-document OCR (scan-only or force-all), always extract bookmarks
    // when this PDF can store an outline. Existing bookmarks are replaced in
    // memory (persistToDisk=false); the file is not overwritten.
    bool needToc = extractTocIfMissing && engine && EngineMupdfCanEditPdfToc(engine);
    int queued = OcrQueueUnscannedPages(win, forceOcrAll, OcrOperation::AllPages);
    if (queued < 0) {
        return;
    }
    if (queued == 0) {
        if (engine && engine->CountOcrCachedPages() > 0) {
            engine->MarkUnsavedOcrText();
            ToolbarUpdateStateForWindow(win, false);
        }
        if (needToc) {
            HandleExtractPdfTocCommand(win, true, false);
        } else if (win) {
            if (engine && engine->CountOcrCachedPages() > 0) {
                OcrShowQuietDone(win->hwndCanvas, _TRA("Ready to search"));
            } else {
                OcrShowQuietDone(win->hwndCanvas, _TRA("No scanned pages to recognize."));
            }
        }
        return;
    }
    if (needToc) {
        OcrClearPendingExtractToc();
        engine->AddRef();
        gOcrPendingExtractEngine = engine;
        gOcrPendingExtractHwnd = win->hwndCanvas;
        gOcrPendingExtractPersist = false;
    }
}

void OcrExtractTocAfterDocumentOcr(MainWindow* win) {
    if (!win) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    int queued = OcrQueueUnscannedPages(win, false, OcrOperation::AllPages);
    if (queued < 0) {
        return;
    }
    if (queued == 0) {
        HandleExtractPdfTocCommand(win, true, true);
        return;
    }
    OcrClearPendingExtractToc();
    engine->AddRef();
    gOcrPendingExtractEngine = engine;
    gOcrPendingExtractHwnd = win->hwndCanvas;
    gOcrPendingExtractPersist = true;
}

void OcrSaveSearchablePdfAfterOcr(MainWindow* win, const char* destPath, bool extractTocWhenDone) {
    if (!win || str::IsEmpty(destPath)) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || engine->kind != kindEngineMupdf) {
        ShowWarningNotification(win->hwndCanvas, _TRA("Save as searchable PDF is only available for PDF files."),
                                kNotif5SecsTimeOut);
        return;
    }
    int queued = OcrQueueUnscannedPages(win, false, OcrOperation::SaveSearchable);
    if (queued < 0) {
        return;
    }
    bool running = false;
    gQueueLock.Lock();
    running = gOcrDocEngine == engine && (gOcrDocTotal > 0 || OcrQueueHasDocumentJob(engine));
    gQueueLock.Unlock();
    if (queued == 0 && !running) {
        OcrWriteSearchablePdfToPath(engine, win->hwndCanvas, destPath, extractTocWhenDone, true);
        return;
    }
    OcrSetPendingSave(engine, win->hwndCanvas, destPath, extractTocWhenDone);
}

bool OcrHasQueuedJobs() {
    gQueueLock.Lock();
    bool has = gQueue.Size() > 0 || gOcrDocTotal > 0;
    gQueueLock.Unlock();
    return has || InterlockedCompareExchange(&gWorkersAlive, 0, 0) != 0;
}

void OcrCancelQueued(MainWindow* win, bool quiet) {
    HWND hwnd = win ? win->hwndCanvas : nullptr;
    LONG prevSeq = InterlockedCompareExchange(&gOcrCancelSeq, 0, 0);
    InterlockedIncrement(&gOcrCancelSeq);

    int removed = 0;
    bool hadDoc = false;
    int qBefore = 0;
    gQueueLock.Lock();
    qBefore = gQueue.Size();
    hadDoc = gOcrDocTotal > 0 && (!hwnd || gOcrDocHwnd == hwnd);
    for (int i = gQueue.Size() - 1; i >= 0; i--) {
        auto* job = (OcrJob*)gQueue[i];
        if (hwnd && job->hwndCanvas && job->hwndCanvas != hwnd) {
            continue;
        }
        if (job->engine) {
            job->engine->Release();
        }
        delete job;
        gQueue.RemoveAt((size_t)i);
        removed++;
    }
    if (!hwnd || gOcrDocHwnd == hwnd) {
        gOcrDocEngine = nullptr;
        gOcrDocHwnd = nullptr;
        gOcrDocTotal = 0;
        gOcrDocDone = 0;
    }
    gQueueLock.Unlock();

    bool hadPending = gOcrPendingSavePath && (!hwnd || gOcrPendingSaveHwnd == hwnd);
    if (hadPending) {
        OcrClearPendingSave();
    }
    bool hadExtract = gOcrPendingExtractEngine && (!hwnd || gOcrPendingExtractHwnd == hwnd);
    if (hadExtract) {
        OcrClearPendingExtractToc();
    }
    if (hwnd) {
        if (quiet) {
            HideOcrStatus(hwnd);
        } else if (removed > 0 || hadPending || hadExtract || hadDoc) {
            OcrShowQuietDone(hwnd, _TRA("Cancelled."));
        } else {
            HideOcrStatus(hwnd);
        }
    }
}

void OcrCancelForEngine(EngineBase* engine) {
    HWND hwnd = nullptr;
    gQueueLock.Lock();
    bool ours = engine && engine == gOcrDocEngine && gOcrDocTotal > 0;
    hwnd = gOcrDocHwnd;
    gQueueLock.Unlock();
    if (!ours) {
        return;
    }
    MainWindow* win = hwnd ? FindMainWindowByHwnd(hwnd) : nullptr;
    OcrCancelQueued(win);
}

void OcrBeginRegionSelect(MainWindow* win) {
    if (!win) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        ShowWarningNotification(win->hwndCanvas, _TRA("OCR is only available for PDF and similar documents."),
                                kNotif5SecsTimeOut);
        return;
    }
    EngineBase* engine = dm->GetEngine();
    if (!OcrEngineKindSupported(engine)) {
        ShowWarningNotification(win->hwndCanvas, _TRA("OCR is not available for this document type."),
                                kNotif5SecsTimeOut);
        return;
    }
    if (!OcrSidecarLooksPresent()) {
        OcrNotifyMissingModels(win->hwndCanvas, true);
        return;
    }
    SetAnnotCreateTool(win, 0);
    win->ocrRegionPending = true;
    SetCursorCached(IDC_CROSS);
}

void OcrCancelRegionSelect(MainWindow* win) {
    if (!win) {
        return;
    }
    win->ocrRegionPending = false;
    if (win->mouseAction == MouseAction::OcrRegion) {
        win->mouseAction = MouseAction::None;
        win->dragStartPending = false;
        win->selectionRect = Rect();
        if (GetCapture() == win->hwndCanvas) {
            ReleaseCapture();
        }
        ScheduleRepaint(win, 0);
    }
}

void OcrFinishRegionSelect(MainWindow* win, Rect screenRect) {
    if (!win) {
        return;
    }
    win->ocrRegionPending = false;
    if (screenRect.dx < 0) {
        screenRect.x += screenRect.dx;
        screenRect.dx = -screenRect.dx;
    }
    if (screenRect.dy < 0) {
        screenRect.y += screenRect.dy;
        screenRect.dy = -screenRect.dy;
    }
    if (screenRect.dx < 4 && screenRect.dy < 4) {
        ShowWarningNotification(win->hwndCanvas, _TRA("Selection too small."), kNotif5SecsTimeOut);
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return;
    }
    EngineBase* engine = dm->GetEngine();
    Point mid(screenRect.x + screenRect.dx / 2, screenRect.y + screenRect.dy / 2);
    int pageNo = dm->GetPageNoByPoint(mid);
    if (!dm->ValidPageNo(pageNo)) {
        ShowWarningNotification(win->hwndCanvas, _TRA("No page to recognize."), kNotif5SecsTimeOut);
        return;
    }
    if (!OcrSidecarLooksPresent()) {
        OcrNotifyMissingModels(win->hwndCanvas, true);
        return;
    }
    RectF clip = dm->CvtFromScreen(screenRect, pageNo);
    if (clip.dx < 0) {
        clip.x += clip.dx;
        clip.dx = -clip.dx;
    }
    if (clip.dy < 0) {
        clip.y += clip.dy;
        clip.dy = -clip.dy;
    }
    clip = clip.Intersect(engine->PageMediabox(pageNo));
    if (clip.IsEmpty() || clip.dx < 2 || clip.dy < 2) {
        ShowWarningNotification(win->hwndCanvas, _TRA("Selection too small."), kNotif5SecsTimeOut);
        return;
    }
    gQueueLock.Lock();
    auto* job = new OcrJob();
    job->hwndCanvas = win->hwndCanvas;
    job->engine = engine;
    engine->AddRef();
    job->pageNo = pageNo;
    job->showStatus = true;
    job->regionJob = true;
    job->clipRect = clip;
    job->cancelSeq = gOcrCancelSeq;
    // Region OCR is an explicit user action waiting on the result: jump the
    // queue ahead of queued Auto OCR page prefetches. In-flight jobs finish
    // first; this job is picked up by the next free worker.
    gQueue.InsertAt(0, job);
    gQueueLock.Unlock();
    ShowOcrStatus(win->hwndCanvas, _TRA("Scanning…"), kNotifNoTimeout);
    StartOcrWorkerIfNeeded();
}

static void OcrWriteUtf8File(const char* path, const char* text) {
    if (!path) {
        return;
    }
    const char* s = text ? text : "";
    file::WriteFile(path, ByteSlice((const u8*)s, str::Len(s)));
}

static char* OcrBoxesJoinText(const Vec<OcrBox>& boxes) {
    StrBuilder sb;
    for (int i = 0; i < boxes.Size(); i++) {
        if (boxes[i].text && boxes[i].text[0]) {
            sb.Append(boxes[i].text);
            sb.Append("\n");
        }
    }
    return sb.StealData();
}

int OcrRunFileBenchmark(const char* pdfPath, const char* outDir, int maxPages) {
    if (str::IsEmpty(pdfPath)) {
        logf("OCR bench: missing pdf path\n");
        return 1;
    }
    if (!file::Exists(pdfPath)) {
        logf("OCR bench: file not found %s\n", pdfPath);
        return 1;
    }
    const char* dir = outDir && outDir[0] ? outDir : "c:\\src\\sumatrapdf\\_ocr_bench";
    dir::CreateAll(dir);
    if (!OcrModelsAvailable()) {
        logf("OCR bench: models not available: %s\n", OcrLastError() ? OcrLastError() : OcrModelsMissingHint());
        return 1;
    }
    EngineBase* engine = CreateEngineFromFile(pdfPath, nullptr, true);
    if (!engine) {
        logf("OCR bench: failed to open %s\n", pdfPath);
        return 1;
    }
    int nPages = engine->PageCount();
    if (maxPages < 1) {
        maxPages = 6;
    }
    if (maxPages > nPages) {
        maxPages = nPages;
    }
    logf("OCR bench: %s pages=%d benchPages=%d out=%s\n", pdfPath, nPages, maxPages, dir);

    OcrProfile profiles[3] = {OcrProfile::Fast, OcrProfile::Balanced, OcrProfile::Hybrid};
    const char* names[3] = {"tiny", "small", "hybrid"};
    double sumMs[3]{};
    double sumRaster[3]{};
    double sumDet[3]{};
    double sumRec[3]{};
    double sumText[3]{};
    int nOk[3]{};

    for (int pi = 0; pi < 3; pi++) {
        StrBuilder allText;
        logf("OCR bench: --- profile %s ---\n", names[pi]);
        LARGE_INTEGER tProf = TimeGet();
        for (int pageNo = 1; pageNo <= maxPages; pageNo++) {
            LARGE_INTEGER tRaster = TimeGet();
            RenderedBitmap* bmp = RenderPageForOcr(engine, pageNo);
            double rasterMs = TimeSinceInMs(tRaster);
            if (!bmp || !bmp->IsValid()) {
                delete bmp;
                logf("OCR bench: render failed page %d profile %s\n", pageNo, names[pi]);
                continue;
            }
            int w = 0, h = 0, stride = 0;
            u8* rgb = CopyBitmapToRgb(bmp->GetBitmap(), &w, &h, &stride);
            delete bmp;
            if (!rgb) {
                continue;
            }
            Vec<OcrBox> boxes;
            OcrPageTiming timing{};
            timing.rasterizeMs = rasterMs;
            bool ok = OcrRecognizeRgb(rgb, w, h, stride, boxes, profiles[pi], &timing);
            LARGE_INTEGER tText = TimeGet();
            char* pageText = OcrBoxesJoinText(boxes);
            timing.textLayerMs = TimeSinceInMs(tText);
            timing.pageTotalMs = rasterMs + timing.pageTotalMs + timing.textLayerMs;
            OcrLogPageTiming(pageNo, profiles[pi], timing);
            TempStr path = path::JoinTemp(dir, str::FormatTemp("%s_p%02d.txt", names[pi], pageNo));
            OcrWriteUtf8File(path, pageText);
            if (pageText) {
                allText.AppendFmt("--- page %d ---\n", pageNo);
                allText.Append(pageText);
                allText.Append("\n");
            }
            if (ok) {
                nOk[pi]++;
            }
            sumMs[pi] += timing.pageTotalMs;
            sumRaster[pi] += timing.rasterizeMs;
            sumDet[pi] += timing.detPreprocessMs + timing.detInferenceMs + timing.detPostprocessMs;
            sumRec[pi] += timing.cropMs + timing.recPreprocessMs + timing.recInferenceMs + timing.recPostprocessMs;
            sumText[pi] += timing.textLayerMs;
            str::Free(pageText);
            FreeOcrBoxes(boxes);
            free(rgb);
        }
        double profMs = TimeSinceInMs(tProf);
        TempStr allPath = path::JoinTemp(dir, str::FormatTemp("%s_all.txt", names[pi]));
        OcrWriteUtf8File(allPath, allText.CStr());
        double avg = maxPages > 0 ? sumMs[pi] / (double)maxPages : 0;
        double pps = profMs > 1 ? 1000.0 * (double)maxPages / profMs : 0;
        logf(
            "OCR bench summary %s pages=%d ok=%d avg_ms=%.0f pages/s=%.2f raster=%.0f det=%.0f rec=%.0f text=%.0f "
            "total_s=%.2f\n",
            names[pi], maxPages, nOk[pi], avg, pps, sumRaster[pi], sumDet[pi], sumRec[pi], sumText[pi],
            profMs / 1000.0);
    }

    StrBuilder sum;
    sum.Append("profile,avg_ms,pages_per_sec,raster_ms,det_ms,rec_ms,text_ms,ok_pages\n");
    for (int pi = 0; pi < 3; pi++) {
        double avg = maxPages > 0 ? sumMs[pi] / (double)maxPages : 0;
        double pps = avg > 1 ? 1000.0 / avg : 0;
        sum.AppendFmt("%s,%.0f,%.2f,%.0f,%.0f,%.0f,%.0f,%d\n", names[pi], avg, pps, sumRaster[pi] / (double)maxPages,
                      sumDet[pi] / (double)maxPages, sumRec[pi] / (double)maxPages, sumText[pi] / (double)maxPages,
                      nOk[pi]);
    }
    TempStr sumPath = path::JoinTemp(dir, "summary.csv");
    OcrWriteUtf8File(sumPath, sum.CStr());
    logf("OCR bench wrote %s\n", sumPath);
    SafeEngineRelease(&engine);
    return 0;
}
