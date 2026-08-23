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

bool OcrPageLooksScanned(EngineBase* engine, int pageNo) {
    if (!OcrEngineKindSupported(engine) || engine->WasOcrTried(pageNo)) {
        return false;
    }
    int len = 0;
    const WCHAR* text = engine->GetTextForPage(pageNo, &len);
    return CountUsableChars(text) < 20;
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
static bool OcrShouldDropLeadingDirtDot(const int* cps, int nCp, const char* rest, const int* ctcX, int nCtc,
                                        int* ctcW0, int* ctcW1, int* ctcOv) {
    if (ctcW0) {
        *ctcW0 = 0;
    }
    if (ctcW1) {
        *ctcW1 = 0;
    }
    if (ctcOv) {
        *ctcOv = 0;
    }
    if (!cps || nCp < 2 || !OcrIsDirtBulletCp(cps[0])) {
        return false;
    }
    if (ctcX && nCtc == nCp) {
        int w0 = ctcX[1] - ctcX[0];
        int w1 = ctcX[3] - ctcX[2];
        int ov = ctcX[1] - ctcX[2];
        if (ctcW0) {
            *ctcW0 = w0;
        }
        if (ctcW1) {
            *ctcW1 = w1;
        }
        if (ctcOv) {
            *ctcOv = ov;
        }
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
static bool OcrShouldJoinDocLines(const OcrBox& a, const OcrBox& b, int bodyLeft, int bodyRight, int em, int gapMed) {
    if (!a.text || !b.text) {
        return false;
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

static void BoxesToPageText(const Vec<OcrBox>& boxes, const u8* rgb, int imgW, int imgH, int stride,
                            const RectF& pageBox, PageText* pt, PageTextUtf8* utf8, int pageNo) {
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
        OcrPlaceGlyphXs(cps, units, nCp, unitSum, rgb, imgW, imgH, stride, b.rect, gx0, gx1, b.charX, b.nChar);
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
        OcrExpandPunctCells(cps, nCp, lineEm, gx0, gx1);
        int padX = lineEm / 10;
        if (padX < 2) {
            padX = 2;
        }
        OcrInsetGlyphXs(nCp, padX, gx0, gx1);
        bool lonePunct = nCp == 1 && OcrIsPunct(cps[0]);
        int lineY0 = 0;
        int lineY1 = b.rect.dy;
        if (lonePunct) {
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
        int ctcW0 = 0, ctcW1 = 0, ctcOv = 0;
        int skipCp = 0;
        if (OcrShouldDropLeadingDirtDot(cps, nCp, rest, b.charX, b.nChar, &ctcW0, &ctcW1, &ctcOv)) {
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
                // #region agent log
                {
                    FILE* f = fopen("c:\\src\\sumatrapdf\\debug-705e63.log", "ab");
                    if (f) {
                        fprintf(f,
                                "{\"sessionId\":\"705e63\",\"hypothesisId\":\"B\",\"location\":\"OcrService.cpp:"
                                "BoxesToPageText\",\"message\":\"drop-radical-box\",\"data\":{\"rad\":%d,\"host\":%d,"
                                "\"dx\":%d,\"ndx\":%d,\"ov\":%d},\"timestamp\":%llu}\n",
                                cps[0], host, b.rect.dx, boxes[nextDirt].rect.dx, ov,
                                (unsigned long long)GetTickCount64());
                        fclose(f);
                    }
                }
                // #endregion
            }
        }
        u8* dropCp = AllocArray<u8>(nCp);
        for (int i = 0; i + 1 < nCp; i++) {
            int w0 = OcrCtcSpanW(b.charX, b.nChar, nCp, i);
            int w1 = OcrCtcSpanW(b.charX, b.nChar, nCp, i + 1);
            int ov = OcrCtcOverlap(b.charX, b.nChar, nCp, i);
            if (OcrShouldDropSplitRadical(cps[i], cps[i + 1], w0, w1, ov)) {
                dropCp[i] = 1;
                // #region agent log
                {
                    FILE* f = fopen("c:\\src\\sumatrapdf\\debug-705e63.log", "ab");
                    if (f) {
                        fprintf(f,
                                "{\"sessionId\":\"705e63\",\"hypothesisId\":\"A\",\"location\":\"OcrService.cpp:"
                                "BoxesToPageText\",\"message\":\"drop-split-radical\",\"data\":{\"rad\":%d,\"host\":%d,"
                                "\"w0\":%d,\"w1\":%d,\"ov\":%d,\"nCp\":%d},\"timestamp\":%llu}\n",
                                cps[i], cps[i + 1], w0, w1, ov, nCp, (unsigned long long)GetTickCount64());
                        fclose(f);
                    }
                }
                // #endregion
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
                // #region agent log
                {
                    FILE* f = fopen("c:\\src\\sumatrapdf\\debug-705e63.log", "ab");
                    if (f) {
                        fprintf(
                            f,
                            "{\"sessionId\":\"705e63\",\"hypothesisId\":\"D\",\"location\":\"OcrService.cpp:"
                            "BoxesToPageText\",\"message\":\"drop-trailing-radical\",\"data\":{\"rad\":%d,\"host\":%"
                            "d,\"w0\":%d,\"w1\":%d,\"ov\":%d,\"nCp\":%d,\"bx0\":%d,\"bx1\":%d,\"nx0\":%d},"
                            "\"timestamp\":%llu}\n",
                            cps[nCp - 1], host, w0, w1, ov, nCp, b.rect.x, b.rect.x + b.rect.dx, boxes[nextDirt].rect.x,
                            (unsigned long long)GetTickCount64());
                        fclose(f);
                    }
                }
                // #endregion
            }
        }
        float cellStep = (float)pageR.dx / unitSum;
        int deX0 = -1, deX1 = -1, deI = -1;
        int d4I = -1, d4x0 = -1, d4x1 = -1, d4prev = 0, d4next = 0;
        int dunI = -1, dunx0 = -1, dunx1 = -1, dunNext = 0, dunPrev = 0;
        float dunU = 0.f, dunPrevU = 0.f;
        int qI = -1, qx0 = -1, qx1 = -1, qNext = 0;
        float qU = 0.f;
        int perI = -1, perx0 = -1, perx1 = -1, perNext = 0, perPrev = 0;
        float perU = 0.f, perPrevU = 0.f;
        idx = 0;
        int cpI = 0;
        while (idx < tlen) {
            int before = idx;
            int cp = Utf8CodepointNext(b.text, tlen, idx);
            if (idx <= before) {
                break;
            }
            if (skipBox || cpI < skipCp || (cpI < nCp && dropCp[cpI])) {
                cpI++;
                continue;
            }
            Rect cr = pageR;
            int ix0 = (cpI < nCp) ? gx0[cpI] : 0;
            int ix1 = (cpI < nCp) ? gx1[cpI] : b.rect.dx;
            cr.x = pageR.x + (int)((float)ix0 * sx + 0.5f);
            int cellEnd = pageR.x + (int)((float)ix1 * sx + 0.5f);
            cr.dx = (cellEnd > cr.x) ? (cellEnd - cr.x) : 1;
            if (cp == 0x7684) {
                deI = cpI;
                deX0 = cr.x;
                deX1 = cr.x + cr.dx;
            }
            if (cp == '4' && d4I < 0) {
                d4I = cpI;
                d4x0 = ix0;
                d4x1 = ix1;
                d4prev = (cpI > 0) ? cps[cpI - 1] : 0;
                d4next = (cpI + 1 < nCp) ? cps[cpI + 1] : 0;
            }
            if (cp == 0x3001) {
                int prev = (cpI > 0) ? cps[cpI - 1] : 0;
                if (dunI < 0 || prev == 0x201d || prev == 0x2019 || cpI + 1 == nCp) {
                    dunI = cpI;
                    dunx0 = ix0;
                    dunx1 = ix1;
                    dunNext = (cpI + 1 < nCp) ? cps[cpI + 1] : 0;
                    dunPrev = prev;
                    dunU = (cpI < nCp) ? units[cpI] : 0.f;
                    dunPrevU = (cpI > 0) ? units[cpI - 1] : 0.f;
                }
            }
            if (cp == 0x201d) {
                int next = (cpI + 1 < nCp) ? cps[cpI + 1] : 0;
                if (qI < 0 || next == 0x3001) {
                    qI = cpI;
                    qx0 = ix0;
                    qx1 = ix1;
                    qNext = next;
                    qU = (cpI < nCp) ? units[cpI] : 0.f;
                }
            }
            if (cp == 0x3002) {
                int prev = (cpI > 0) ? cps[cpI - 1] : 0;
                if (perI < 0 || cpI + 1 == nCp || !OcrIsPunct(prev)) {
                    perI = cpI;
                    perx0 = ix0;
                    perx1 = ix1;
                    perNext = (cpI + 1 < nCp) ? cps[cpI + 1] : 0;
                    perPrev = prev;
                    perU = (cpI < nCp) ? units[cpI] : 0.f;
                    perPrevU = (cpI > 0) ? units[cpI - 1] : 0.f;
                }
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
            if ((b.text && (str::Find(b.text, "下一步") || str::Find(b.text, "今后"))) ||
                (boxes[next].text && (str::Find(boxes[next].text, "下一步") || str::Find(boxes[next].text, "今后")))) {
                // #region agent log
                {
                    FILE* f = fopen("c:\\src\\sumatrapdf\\debug-705e63.log", "ab");
                    if (f) {
                        fprintf(f,
                                "{\"sessionId\":\"705e63\",\"hypothesisId\":\"I\",\"location\":\"OcrService.cpp:"
                                "BoxesToPageText\",\"message\":\"ocr-join\",\"data\":{\"join\":%d,\"ax\":%d,\"ay\":%d,"
                                "\"adx\":%d,\"ady\":%d,\"bx\":%d,\"by\":%d,\"bdx\":%d,\"bdy\":%d,\"a\":\"%.40s\",\"b\":"
                                "\"%.40s\"},\"timestamp\":%llu}\n",
                                join ? 1 : 0, b.rect.x, b.rect.y, b.rect.dx, b.rect.dy, boxes[next].rect.x,
                                boxes[next].rect.y, boxes[next].rect.dx, boxes[next].rect.dy, b.text, boxes[next].text,
                                (unsigned long long)GetTickCount64());
                        fclose(f);
                    }
                }
                // #endregion
            }
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

bool OcrRecognizeEnginePage(EngineBase* engine, int pageNo, bool forceOcr) {
    if (!engine || pageNo < 1 || pageNo > engine->PageCount()) {
        return false;
    }

    HANDLE waitEv = nullptr;
    OcrFlight* owned = nullptr;
    gFlightLock.Lock();
    OcrFlight* existing = FindFlightLocked(engine, pageNo);
    if (existing) {
        DuplicateHandle(GetCurrentProcess(), existing->doneEvent, GetCurrentProcess(), &waitEv, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    } else if (engine->WasOcrTried(pageNo) && !forceOcr) {
        gFlightLock.Unlock();
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
    const char* path = "unknown";
    if (OcrModelsAvailable()) {
        bool scanned = OcrPageLooksScanned(engine, pageNo);
        if (!scanned && !forceOcr) {
            FinishOcrFlight(owned);
            bool usable = engine->PageHasUsableText(pageNo);
            return usable;
        }
        engine->MarkOcrTried(pageNo);
        RenderedBitmap* bmp = RenderPageForOcr(engine, pageNo);
        if (bmp && bmp->IsValid()) {
            int w = 0, h = 0, stride = 0;
            u8* rgb = CopyBitmapToRgb(bmp->GetBitmap(), &w, &h, &stride);
            delete bmp;
            bmp = nullptr;
            if (rgb) {
                Vec<OcrBox> boxes;
                ok = OcrRecognizeRgb(rgb, w, h, stride, boxes);
                int nBoxes = boxes.Size();
                if (ok) {
                    PageText pt{};
                    PageTextUtf8 utf8{};
                    BoxesToPageText(boxes, rgb, w, h, stride, engine->PageMediabox(pageNo), &pt, &utf8, pageNo);
                    FreeOcrBoxes(boxes);
                    if (pt.text && pt.len > 0) {
                        engine->SetCachedPageText(pageNo, pt, utf8);
                        path = "cached";
                    } else {
                        FreePageText(&pt);
                        FreePageTextUtf8(&utf8);
                        ok = false;
                        path = "emptyPageText";
                    }
                } else {
                    FreeOcrBoxes(boxes);
                    path = nBoxes > 0 ? "recEmpty" : "recognizeFalse";
                }
                free(rgb);
            } else {
                path = "copyRgbFail";
            }
        } else {
            delete bmp;
            path = "renderFail";
        }
    } else {
        path = "modelsUnavailable";
    }
    FinishOcrFlight(owned);
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
    bool ok = OcrRecognizeRgb(rgb, w, h, stride, boxes);
    if (ok) {
        BoxesToPageText(boxes, rgb, w, h, stride, clip, ptOut, utf8Out, pageNo);
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
    if (WaitIfOcrInFlight(engine, pageNo)) {
        return;
    }
    if (!gGlobalPrefs || !gGlobalPrefs->autoOcrScanPages) {
        return;
    }
    if (OcrPageLooksScanned(engine, pageNo)) {
        OcrRecognizeEnginePage(engine, pageNo);
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
    bool ok = EngineMupdfSaveSearchablePdf(engine, writePath, &err);
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
                OcrFinishPendingSaveIfAny(d->engine);
            } else if (extractToc) {
                HWND hwnd = gOcrPendingExtractHwnd;
                bool persist = gOcrPendingExtractPersist;
                OcrClearPendingExtractToc();
                HideOcrStatus(hwnd ? hwnd : d->hwndCanvas);
                MainWindow* win = hwnd && IsWindow(hwnd) ? FindMainWindowByHwnd(hwnd) : nullptr;
                if (d->engine && d->engine->CountOcrCachedPages() > 0) {
                    d->engine->MarkUnsavedOcrText();
                }
                if (win) {
                    HandleExtractPdfTocCommand(win, true, persist);
                    ToolbarUpdateStateForWindow(win, false);
                }
            } else {
                if (d->engine && d->engine->CountOcrCachedPages() > 0) {
                    d->engine->MarkUnsavedOcrText();
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
            ok = OcrRecognizeEnginePage(job->engine, job->pageNo, job->forceOcr);
            if (!job->documentJob && !job->regionJob && gGlobalPrefs && gGlobalPrefs->autoOcrScanPages) {
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
    if (!OcrPageLooksScanned(engine, pageNo)) {
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
    job->cancelSeq = gOcrCancelSeq;
    gQueue.Append(job);
    gQueueLock.Unlock();
}

static void OcrQueueAutoNearby(EngineBase* engine, HWND hwnd, int centerPage) {
    if (!engine || !gGlobalPrefs || !gGlobalPrefs->autoOcrScanPages) {
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
    if (!ignoreAutoPref && !gGlobalPrefs->autoOcrScanPages) {
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

    if (ignoreAutoPref) {
        bool tried = engine->WasOcrTried(pageNo);
        bool usable = engine->PageHasUsableText(pageNo);
        if (usable && !tried) {
            OcrShowQuietDone(win->hwndCanvas, _TRA("Ready to search"));
            return;
        }
        if (tried && usable) {
            OcrShowQuietDone(win->hwndCanvas, _TRA("Ready to search"));
            return;
        }
        if (tried) {
            engine->ClearOcrTried(pageNo);
        }
    } else if (!OcrPageLooksScanned(engine, pageNo)) {
        return;
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
    job->cancelSeq = gOcrCancelSeq;
    gQueue.Append(job);
    gQueueLock.Unlock();
    if (ignoreAutoPref) {
        ShowOcrStatus(win->hwndCanvas, _TRA("Scanning…"), kNotifNoTimeout);
    }
    StartOcrWorkerIfNeeded();
}

static int OcrQueueUnscannedPages(MainWindow* win, bool forceUncached) {
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
    int nCached = 0;
    int nTried = 0;
    int nUsable = 0;
    gQueueLock.Lock();
    bool busyOther = gOcrDocTotal > 0 && gOcrDocEngine && gOcrDocEngine != engine;
    EngineBase* busyEng = gOcrDocEngine;
    int busyDone = gOcrDocDone;
    int busyTotal = gOcrDocTotal;
    gQueueLock.Unlock();
    if (busyOther) {
        ShowWarningNotification(win->hwndCanvas, _TRA("OCR is already running. Please wait until it finishes."),
                                kNotif5SecsTimeOut);
        return -1;
    }
    gQueueLock.Lock();
    for (int pageNo = 1; pageNo <= n; pageNo++) {
        bool cached = engine->HasCachedOcrText(pageNo);
        bool tried = engine->WasOcrTried(pageNo);
        bool usable = engine->PageHasUsableText(pageNo);
        if (cached && !forceUncached) {
            nCached++;
            continue;
        }
        if (!forceUncached) {
            if (tried) {
                nTried++;
                continue;
            }
            if (usable) {
                nUsable++;
                continue;
            }
        }
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
        job->forceOcr = forceUncached;
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
    }
    gOcrDocEngine = engine;
    gOcrDocHwnd = win->hwndCanvas;
    gOcrDocTotal += queued;
    OcrShowDocumentProgress(win->hwndCanvas);
    StartOcrWorkerIfNeeded();
    return queued;
}

void OcrScheduleDocument(MainWindow* win, bool extractTocIfMissing, bool forceOcrAll) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    bool needToc =
        extractTocIfMissing && engine && EngineMupdfCanEditPdfToc(engine) && !EngineMupdfHasStoredOutline(engine);
    int queued = OcrQueueUnscannedPages(win, forceOcrAll);
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
            OcrShowQuietDone(win->hwndCanvas, _TRA("Ready to search"));
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
    int queued = OcrQueueUnscannedPages(win, false);
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
    int queued = OcrQueueUnscannedPages(win, true);
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

void OcrCancelQueued(MainWindow* win) {
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
        if (removed > 0 || hadPending || hadExtract || hadDoc) {
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
    gQueue.Append(job);
    gQueueLock.Unlock();
    ShowOcrStatus(win->hwndCanvas, _TRA("Scanning…"), kNotifNoTimeout);
    StartOcrWorkerIfNeeded();
}
