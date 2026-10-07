/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "AppSettings.h"
#include "AppTools.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "Annotation.h"
#include "DisplayModel.h"
#include "Notifications.h"
#include "Translations.h"
#include "Commands.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "SumatraPDF.h"
#include "EditAnnotations.h"
#include "Toolbar.h"
#include "HandwrittenSignature.h"
#include "EbookAnnotations.h"
#include "EditEbookAnnotations.h"
#include "ImageSaveCropResize.h"
#include "Theme.h"
#include "AppDialogTheme.h"

#include <ole2.h>
#include <shlobj.h>

using Gdiplus::Bitmap;

constexpr float kSigWidthPt = 130.f;
constexpr int kPadClearId = 101;
constexpr int kPadOkId = 102;
constexpr int kPadCancelId = 103;
constexpr int kPadPhotoId = 104;
constexpr WCHAR kPadClassName[] = L"SumatraHandwrittenSignature";

// The pad is a sheet of the theme, not a white card. Ink is the theme's
// reading color so a dark theme does not put black strokes on a bright page.
static COLORREF SignaturePaperColor() {
    COLORREF paper = ThemeWindowControlBackgroundColor();
    if (ThemeUsesBlackChrome()) {
        // Control bg is #050505, which disappears into the black dialog.
        paper = AccentColor(paper, 18);
    }
    return paper;
}

static COLORREF SignatureInkColor() {
    if (ThemeUsesDarkChrome()) {
        return ThemeReadingTextColor();
    }
    return ThemeWindowTextColor();
}

static COLORREF SignatureGuideColor() {
    return AccentColor(SignaturePaperColor(), ThemeUsesDarkChrome() ? 36 : 32);
}

static COLORREF SignatureHintColor() {
    return AccentColor(SignaturePaperColor(), ThemeUsesDarkChrome() ? 58 : 52);
}

// Drag preview sits on the page, which may already be recolored. Match that
// page so the stroke stays visible. The stored stamp is still black ink.
static COLORREF SignatureOnPageInkColor() {
    COLORREF pageBg = 0;
    ThemePageRenderColors(pageBg, true);
    if (IsLightColor(pageBg)) {
        return RGB(0, 0, 0);
    }
    return ThemeReadingTextColor();
}

static COLORREF SignatureOnPageFrameColor() {
    COLORREF pageBg = 0;
    ThemePageRenderColors(pageBg, true);
    return AccentColor(pageBg, 48);
}

static bool gPlacing = false;
static MainWindow* gPlaceWin = nullptr;
static Vec<PointF> gPlacePts;
static Vec<int> gPlaceCounts;
static float gPlaceAspect = 0.35f;
static bool gPadClassRegistered = false;
static bool gPlaceDragging = false;
static int gDragX0 = 0;
static int gDragY0 = 0;
static int gDragX1 = 0;
static int gDragY1 = 0;
static bool gPlaceIsPhoto = false;
static HBITMAP gPlacePhoto = nullptr;
static int gPlacePhotoW = 0;
static int gPlacePhotoH = 0;

struct SigPad {
    HWND hwnd = nullptr;
    MainWindow* mainWin = nullptr;
    HWND btnPhoto = nullptr;
    HWND btnClear = nullptr;
    HWND btnOk = nullptr;
    HWND btnCancel = nullptr;
    HBRUSH bgBrush = nullptr;
    Bitmap* photoSrc = nullptr;
    HBITMAP photoDib = nullptr;
    int photoW = 0;
    int photoH = 0;
    int threshold = 180;
    bool hasPhoto = false;
    IDropTarget* dropTarget = nullptr;

    ~SigPad();
    RECT padRc{};
    int padW = 0;
    int padH = 0;
    Vec<PointF> pts;
    Vec<int> counts;
    Vec<PointF> loadedPts;
    Vec<int> loadedCounts;
    float loadedAspect = 0.35f;
    bool mapped = false;
    bool drawing = false;
    bool accepted = false;
};

static bool HasStroke(const Vec<int>& counts) {
    for (int i = 0; i < counts.Size(); i++) {
        if (counts.at(i) >= 2) {
            return true;
        }
    }
    return false;
}

static void AppendFixed(StrBuilder& sb, float v) {
    if (v < 0.f) {
        sb.AppendChar('-');
        v = -v;
    }
    if (v > 9.f) {
        v = 9.f;
    }
    int ip = (int)v;
    int fp = (int)((v - (float)ip) * 10000.f + 0.5f);
    if (fp >= 10000) {
        ip += 1;
        fp -= 10000;
    }
    sb.AppendFmt("%d.%04d", ip, fp);
}

static const char* NextLine(const char* p, char* buf, int bufCch) {
    if (!p || !*p || bufCch < 2) {
        if (buf && bufCch > 0) {
            buf[0] = 0;
        }
        return nullptr;
    }
    int i = 0;
    while (*p && *p != '\n' && *p != '\r' && i < bufCch - 1) {
        buf[i++] = *p++;
    }
    buf[i] = 0;
    if (*p == '\r') {
        p++;
    }
    if (*p == '\n') {
        p++;
    }
    return p;
}

static bool ParseFloatToken(const char*& s, float& out) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (!*s) {
        return false;
    }
    float sign = 1.f;
    if (*s == '-') {
        sign = -1.f;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (!(*s >= '0' && *s <= '9') && *s != '.') {
        return false;
    }
    float v = 0.f;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.f + (float)(*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        float place = 0.1f;
        while (*s >= '0' && *s <= '9') {
            v += (float)(*s - '0') * place;
            place *= 0.1f;
            s++;
        }
    }
    out = v * sign;
    return true;
}

static bool ParseIntToken(const char* s, int& out) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s < '0' || *s > '9') {
        return false;
    }
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 100000) {
            return false;
        }
        s++;
    }
    out = v;
    return true;
}

// Strokes sit beside the photo in sumatrapdfcache. Older builds wrote
// handwritten-signature.txt in the app-data root.
static TempStr SignatureStrokePath() {
    TempStr dir = GetPathInAppDataDirTemp("sumatrapdfcache");
    if (!dir) {
        return nullptr;
    }
    return path::JoinTemp(dir, "handwritten-signature.txt");
}

static TempStr SignatureStrokePathLegacy() {
    return GetPathInAppDataDirTemp("handwritten-signature.txt");
}

static void LoadSignatureFile(SigPad* pad) {
    TempStr path = SignatureStrokePath();
    bool legacy = false;
    if (!path || !file::Exists(path)) {
        path = SignatureStrokePathLegacy();
        legacy = path && file::Exists(path);
    }
    if (!path || !file::Exists(path)) {
        return;
    }
    ByteSlice data = file::ReadFile(path);
    if (data.empty()) {
        data.Free();
        return;
    }
    char* text = str::Dup(data);
    data.Free();
    if (!text) {
        return;
    }
    const char* p = text;
    char line[160];
    p = NextLine(p, line, dimof(line));
    if (!str::StartsWith(line, "aspect ")) {
        str::Free(text);
        return;
    }
    const char* aspectSrc = line + 7;
    float aspect = 0.f;
    if (!ParseFloatToken(aspectSrc, aspect) || aspect <= 0.05f || aspect >= 3.f) {
        str::Free(text);
        return;
    }
    pad->loadedAspect = aspect;
    while (p && *p && pad->loadedCounts.Size() < 64) {
        p = NextLine(p, line, dimof(line));
        if (!line[0]) {
            continue;
        }
        int n = 0;
        if (!ParseIntToken(line, n) || n < 2 || n > 8000) {
            break;
        }
        int got = 0;
        bool bad = false;
        for (int i = 0; i < n; i++) {
            if (!p || !*p) {
                bad = true;
                break;
            }
            p = NextLine(p, line, dimof(line));
            const char* s = line;
            float x = 0.f;
            float y = 0.f;
            if (!ParseFloatToken(s, x) || !ParseFloatToken(s, y)) {
                bad = true;
                break;
            }
            if (x < -0.05f || x > 1.05f || y < -0.05f || y > 1.05f) {
                bad = true;
                break;
            }
            PointF pt{x, y};
            pad->loadedPts.Append(pt);
            got++;
        }
        if (bad || got < 2) {
            while (got-- > 0 && pad->loadedPts.Size() > 0) {
                pad->loadedPts.Pop();
            }
            break;
        }
        pad->loadedCounts.Append(got);
    }
    str::Free(text);
    if (legacy && pad->loadedCounts.Size() > 0) {
        TempStr dest = SignatureStrokePath();
        TempStr src = SignatureStrokePathLegacy();
        if (dest && src && dir::CreateForFile(dest) && file::Copy(dest, src, false)) {
            file::Delete(src);
        }
    }
}

static void SaveSignatureFile(const Vec<PointF>& pts, const Vec<int>& counts, float aspect) {
    TempStr path = SignatureStrokePath();
    if (!path || !dir::CreateForFile(path)) {
        return;
    }
    StrBuilder sb;
    sb.Append("aspect ");
    AppendFixed(sb, aspect);
    sb.AppendChar('\n');
    int src = 0;
    for (int i = 0; i < counts.Size(); i++) {
        int n = counts.at(i);
        if (n >= 2) {
            sb.AppendFmt("%d\n", n);
            for (int k = 0; k < n; k++) {
                PointF pt = pts.at(src + k);
                AppendFixed(sb, pt.x);
                sb.AppendChar(' ');
                AppendFixed(sb, pt.y);
                sb.AppendChar('\n');
            }
        }
        if (n > 0) {
            src += n;
        }
    }
    ByteSlice slice((const u8*)sb.Get(), sb.size());
    if (file::WriteFile(path, slice)) {
        TempStr old = SignatureStrokePathLegacy();
        if (old && file::Exists(old)) {
            file::Delete(old);
        }
    }
}

static bool CropSignature(Vec<PointF>& pts, Vec<int>& counts, float padW, float padH, float& aspect) {
    if (padW < 2.f || padH < 2.f || !HasStroke(counts)) {
        return false;
    }
    float minX = 1.f;
    float minY = 1.f;
    float maxX = 0.f;
    float maxY = 0.f;
    int src = 0;
    bool any = false;
    for (int i = 0; i < counts.Size(); i++) {
        int n = counts.at(i);
        if (n >= 2) {
            for (int k = 0; k < n; k++) {
                PointF p = pts.at(src + k);
                if (p.x < minX) {
                    minX = p.x;
                }
                if (p.y < minY) {
                    minY = p.y;
                }
                if (p.x > maxX) {
                    maxX = p.x;
                }
                if (p.y > maxY) {
                    maxY = p.y;
                }
                any = true;
            }
        }
        if (n > 0) {
            src += n;
        }
    }
    if (!any) {
        return false;
    }
    float px = 6.f / padW;
    float py = 6.f / padH;
    minX -= px;
    maxX += px;
    minY -= py;
    maxY += py;
    float bw = maxX - minX;
    float bh = maxY - minY;
    if (bw < 0.01f) {
        minX -= 0.005f;
        maxX += 0.005f;
        bw = maxX - minX;
    }
    if (bh < 0.01f) {
        minY -= 0.005f;
        maxY += 0.005f;
        bh = maxY - minY;
    }
    float inkW = bw * padW;
    float inkH = bh * padH;
    if (inkW < 1.f) {
        inkW = 1.f;
    }
    if (inkH < 1.f) {
        inkH = 1.f;
    }
    aspect = inkH / inkW;
    if (aspect < 0.12f) {
        float extra = (inkW * 0.12f - inkH) * 0.5f;
        minY -= extra / padH;
        maxY += extra / padH;
        bh = maxY - minY;
        aspect = 0.12f;
    } else if (aspect > 1.5f) {
        float extra = (inkH / 1.5f - inkW) * 0.5f;
        minX -= extra / padW;
        maxX += extra / padW;
        bw = maxX - minX;
        aspect = 1.5f;
    }
    if (bw < 0.001f || bh < 0.001f) {
        return false;
    }
    Vec<PointF> outPts;
    Vec<int> outCounts;
    src = 0;
    for (int i = 0; i < counts.Size(); i++) {
        int n = counts.at(i);
        if (n >= 2) {
            for (int k = 0; k < n; k++) {
                PointF p = pts.at(src + k);
                PointF q;
                q.x = (p.x - minX) / bw;
                q.y = (p.y - minY) / bh;
                if (q.x < 0.f) {
                    q.x = 0.f;
                }
                if (q.y < 0.f) {
                    q.y = 0.f;
                }
                if (q.x > 1.f) {
                    q.x = 1.f;
                }
                if (q.y > 1.f) {
                    q.y = 1.f;
                }
                outPts.Append(q);
            }
            outCounts.Append(n);
        }
        if (n > 0) {
            src += n;
        }
    }
    pts = outPts;
    counts = outCounts;
    return outCounts.Size() > 0;
}

static void UpdateOkButton(SigPad* pad) {
    if (pad->btnOk) {
        bool ok = HasStroke(pad->counts) || pad->hasPhoto;
        EnableWindow(pad->btnOk, ok ? TRUE : FALSE);
    }
}

static void MapLoadedIntoPad(SigPad* pad) {
    if (pad->mapped || pad->loadedPts.Size() == 0 || pad->padW < 10 || pad->padH < 10) {
        return;
    }
    float sigAspect = pad->loadedAspect;
    if (sigAspect < 0.12f) {
        sigAspect = 0.12f;
    }
    if (sigAspect > 1.5f) {
        sigAspect = 1.5f;
    }
    float padW = (float)pad->padW;
    float padH = (float)pad->padH;
    float padAspect = padH / padW;
    float contentW = 0.f;
    float contentH = 0.f;
    if (sigAspect <= padAspect) {
        contentW = padW * 0.84f;
        contentH = contentW * sigAspect;
    } else {
        contentH = padH * 0.84f;
        contentW = contentH / sigAspect;
    }
    float ox = (padW - contentW) * 0.5f;
    float oy = (padH - contentH) * 0.5f;
    int src = 0;
    for (int i = 0; i < pad->loadedCounts.Size(); i++) {
        int n = pad->loadedCounts.at(i);
        if (n >= 2) {
            for (int k = 0; k < n; k++) {
                PointF p = pad->loadedPts.at(src + k);
                PointF q;
                q.x = (ox + p.x * contentW) / padW;
                q.y = (oy + p.y * contentH) / padH;
                pad->pts.Append(q);
            }
            pad->counts.Append(n);
        }
        if (n > 0) {
            src += n;
        }
    }
    pad->mapped = true;
    UpdateOkButton(pad);
}

static int PadButtonWidth(HWND btn, int minW) {
    if (!btn) {
        return minW;
    }
    Size ideal = ButtonGetIdealSize(btn);
    return std::max(minW, ideal.dx);
}

static void LayoutPad(HWND hwnd, SigPad* pad) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int margin = DpiScale(hwnd, 12);
    int btnH = DpiScale(hwnd, 26);
    int minBtnW = DpiScale(hwnd, 72);
    int gap = DpiScale(hwnd, 8);
    int btnY = rc.bottom - margin - btnH;
    if (btnY < margin + 40) {
        btnY = margin + 40;
    }
    int photoW = PadButtonWidth(pad->btnPhoto, minBtnW);
    int clearW = PadButtonWidth(pad->btnClear, minBtnW);
    int okW = PadButtonWidth(pad->btnOk, minBtnW);
    int cancelW = PadButtonWidth(pad->btnCancel, minBtnW);
    int avail = rc.right - 2 * margin;
    int need = photoW + clearW + okW + cancelW + 3 * gap;
    if (need > avail && avail > 40) {
        // Prefer shrinking Open Image; keep Cancel/OK readable for long locales.
        int keep = clearW + okW + cancelW + 3 * gap;
        int room = avail - keep;
        if (room >= DpiScale(hwnd, 48)) {
            photoW = room;
        } else {
            float scale = (float)avail / (float)need;
            photoW = std::max(DpiScale(hwnd, 48), (int)(photoW * scale));
            clearW = std::max(DpiScale(hwnd, 52), (int)(clearW * scale));
            okW = std::max(DpiScale(hwnd, 52), (int)(okW * scale));
            cancelW = std::max(DpiScale(hwnd, 60), (int)(cancelW * scale));
        }
    }
    pad->padRc.left = margin;
    pad->padRc.top = margin;
    pad->padRc.right = rc.right - margin;
    pad->padRc.bottom = btnY - margin;
    if (pad->padRc.right < pad->padRc.left + 40) {
        pad->padRc.right = pad->padRc.left + 40;
    }
    if (pad->padRc.bottom < pad->padRc.top + 40) {
        pad->padRc.bottom = pad->padRc.top + 40;
    }
    pad->padW = pad->padRc.right - pad->padRc.left;
    pad->padH = pad->padRc.bottom - pad->padRc.top;
    int x = rc.right - margin - cancelW;
    if (pad->btnCancel) {
        SetWindowPos(pad->btnCancel, nullptr, x, btnY, cancelW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    x -= gap + okW;
    if (pad->btnOk) {
        SetWindowPos(pad->btnOk, nullptr, x, btnY, okW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    x -= gap + clearW;
    if (pad->btnClear) {
        SetWindowPos(pad->btnClear, nullptr, x, btnY, clearW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (pad->btnPhoto) {
        int maxPhoto = x - gap - margin;
        if (maxPhoto > DpiScale(hwnd, 40) && photoW > maxPhoto) {
            photoW = maxPhoto;
        }
        SetWindowPos(pad->btnPhoto, nullptr, margin, btnY, photoW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!pad->hasPhoto) {
        MapLoadedIntoPad(pad);
    }
}

static TempStr SignaturePngPath() {
    TempStr dir = GetPathInAppDataDirTemp("sumatrapdfcache");
    if (!dir) {
        return nullptr;
    }
    return path::JoinTemp(dir, "handwritten-signature.png");
}

// Older builds wrote the photo beside the program. Still read it so a
// signature made before the cache folder is not lost.
static TempStr SignaturePngPathLegacy() {
    return GetPathInAppDataDirTemp("handwritten-signature.png");
}

static void DeleteSignaturePng() {
    TempStr path = SignaturePngPath();
    if (path && file::Exists(path)) {
        file::Delete(path);
    }
    TempStr old = SignaturePngPathLegacy();
    if (old && file::Exists(old)) {
        file::Delete(old);
    }
}

static bool SaveSignaturePng(Bitmap* bmp) {
    TempStr path = SignaturePngPath();
    if (!path || !bmp) {
        return false;
    }
    if (!dir::CreateForFile(path)) {
        return false;
    }
    CLSID clsid = GetGdiPlusEncoderClsid(L"image/png");
    TempWStr wpath = ToWStrTemp(path);
    if (bmp->Save(wpath, &clsid, nullptr) != Gdiplus::Ok) {
        return false;
    }
    TempStr old = SignaturePngPathLegacy();
    if (old && file::Exists(old)) {
        file::Delete(old);
    }
    return true;
}

static int LumaOf(const u8* p) {
    int a = p[3];
    if (a < 16) {
        return -1;
    }
    int b = p[0];
    int g = p[1];
    int r = p[2];
    return (r * 54 + g * 183 + b * 19) >> 8;
}

static const u8* ArgbRow(const Gdiplus::BitmapData& data, int y) {
    return (const u8*)data.Scan0 + (ptrdiff_t)y * data.Stride;
}

static bool LockArgb(Bitmap* bmp, Gdiplus::BitmapData& data) {
    if (!bmp) {
        return false;
    }
    int w = (int)bmp->GetWidth();
    int h = (int)bmp->GetHeight();
    if (w <= 0 || h <= 0) {
        return false;
    }
    Gdiplus::Rect rc(0, 0, w, h);
    return bmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok;
}

static int PaperThreshold(Bitmap* bmp) {
    Gdiplus::BitmapData data{};
    if (!LockArgb(bmp, data)) {
        return 190;
    }
    int w = (int)bmp->GetWidth();
    int h = (int)bmp->GetHeight();
    int span = w < 24 ? w : 24;
    int spanH = h < 24 ? h : 24;
    int sum = 0;
    int n = 0;
    for (int band = 0; band < 4; band++) {
        int y0 = (band == 0 || band == 1) ? 0 : h - spanH;
        int x0 = (band == 0 || band == 2) ? 0 : w - span;
        for (int y = y0; y < y0 + spanH; y++) {
            const u8* row = ArgbRow(data, y);
            for (int x = x0; x < x0 + span; x++) {
                int lum = LumaOf(row + x * 4);
                if (lum >= 0) {
                    sum += lum;
                    n++;
                }
            }
        }
    }
    bmp->UnlockBits(&data);
    if (n < 16) {
        return 190;
    }
    int avg = sum / n;
    int t = avg - 36;
    if (t < 40) {
        t = 190;
    }
    if (t > 240) {
        t = 240;
    }
    return t;
}

static void FreePhotoDib(SigPad* pad) {
    if (pad->photoDib) {
        DeleteObject(pad->photoDib);
        pad->photoDib = nullptr;
    }
    pad->photoW = 0;
    pad->photoH = 0;
}

static void FreePhoto(SigPad* pad) {
    delete pad->photoSrc;
    pad->photoSrc = nullptr;
    FreePhotoDib(pad);
    pad->hasPhoto = false;
}

SigPad::~SigPad() {
    FreePhoto(this);
}

static HBITMAP DibFromLocked(const Gdiplus::BitmapData& src, int w, int h, int threshold, COLORREF ink) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP hbmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!hbmp || !bits) {
        if (hbmp) {
            DeleteObject(hbmp);
        }
        return nullptr;
    }
    u8* dst = (u8*)bits;
    for (int y = 0; y < h; y++) {
        const u8* row = ArgbRow(src, y);
        u8* out = dst + (ptrdiff_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            const u8* p = row + x * 4;
            int lum = LumaOf(p);
            if (lum < 0 || lum >= threshold) {
                out[0] = out[1] = out[2] = out[3] = 0;
            } else {
                out[0] = GetBValue(ink);
                out[1] = GetGValue(ink);
                out[2] = GetRValue(ink);
                out[3] = 255;
            }
            out += 4;
        }
    }
    return hbmp;
}

static void RebuildPhotoPreview(SigPad* pad) {
    FreePhotoDib(pad);
    if (!pad->photoSrc) {
        return;
    }
    Gdiplus::BitmapData data{};
    if (!LockArgb(pad->photoSrc, data)) {
        return;
    }
    int w = (int)pad->photoSrc->GetWidth();
    int h = (int)pad->photoSrc->GetHeight();
    pad->photoDib = DibFromLocked(data, w, h, pad->threshold, SignatureInkColor());
    pad->photoSrc->UnlockBits(&data);
    if (pad->photoDib) {
        pad->photoW = w;
        pad->photoH = h;
    }
}

static Bitmap* ThresholdBitmap(Bitmap* src, int threshold) {
    if (!src) {
        return nullptr;
    }
    int w = (int)src->GetWidth();
    int h = (int)src->GetHeight();
    Gdiplus::BitmapData data{};
    if (!LockArgb(src, data)) {
        return nullptr;
    }
    Bitmap* dst = new Bitmap(w, h, PixelFormat32bppARGB);
    Gdiplus::BitmapData out{};
    Gdiplus::Rect rc(0, 0, w, h);
    if (!dst || dst->LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &out) != Gdiplus::Ok) {
        src->UnlockBits(&data);
        delete dst;
        return nullptr;
    }
    for (int y = 0; y < h; y++) {
        const u8* row = ArgbRow(data, y);
        u8* drow = (u8*)out.Scan0 + (ptrdiff_t)y * out.Stride;
        for (int x = 0; x < w; x++) {
            const u8* p = row + x * 4;
            u8* q = drow + x * 4;
            int lum = LumaOf(p);
            if (lum < 0 || lum >= threshold) {
                q[0] = q[1] = q[2] = q[3] = 0;
            } else {
                q[0] = q[1] = q[2] = 0;
                q[3] = 255;
            }
        }
    }
    dst->UnlockBits(&out);
    src->UnlockBits(&data);
    return dst;
}

static void SetPhotoSource(SigPad* pad, Bitmap* src) {
    if (pad->photoSrc != src) {
        delete pad->photoSrc;
        pad->photoSrc = nullptr;
    }
    FreePhotoDib(pad);
    pad->pts.Reset();
    pad->counts.Reset();
    pad->mapped = true;
    pad->photoSrc = src;
    pad->hasPhoto = src != nullptr;
    pad->threshold = src ? PaperThreshold(src) : 180;
    RebuildPhotoPreview(pad);
    if (pad->hwnd) {
        LayoutPad(pad->hwnd, pad);
        UpdateOkButton(pad);
        InvalidateRect(pad->hwnd, nullptr, FALSE);
    }
}

static void ClearPhoto(SigPad* pad) {
    FreePhoto(pad);
    if (pad->hwnd) {
        LayoutPad(pad->hwnd, pad);
        UpdateOkButton(pad);
        InvalidateRect(pad->hwnd, nullptr, FALSE);
    }
}

static void ShowCantRead(SigPad* pad) {
    HWND owner = pad && pad->hwnd ? pad->hwnd : nullptr;
    MessageBoxW(owner, ToWStrTemp(_TRA("Couldn't read that image.")), ToWStrTemp(_TRA("Handwritten Signature")),
                MB_OK | MB_ICONINFORMATION);
}

static bool IsSignatureImagePath(const char* path) {
    if (!path) {
        return false;
    }
    TempStr ext = path::GetExtTemp(path);
    return str::EqI(ext, ".jpg") || str::EqI(ext, ".jpeg") || str::EqI(ext, ".png") || str::EqI(ext, ".bmp") ||
           str::EqI(ext, ".webp");
}

static int ViewRotationForPath(MainWindow* win, const char* path) {
    if (!win || !path) {
        return 0;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath || !path::IsSame(tab->filePath, path)) {
        return 0;
    }
    if (tab->GetEngineType() != kindEngineImage) {
        return 0;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return 0;
    }
    int rot = dm->GetRotation() % 360;
    if (rot < 0) {
        rot += 360;
    }
    return rot;
}

static void ApplyViewRotation(Bitmap* bmp, int rotation) {
    if (!bmp) {
        return;
    }
    if (rotation == 90) {
        bmp->RotateFlip(Gdiplus::Rotate90FlipNone);
    } else if (rotation == 180) {
        bmp->RotateFlip(Gdiplus::Rotate180FlipNone);
    } else if (rotation == 270) {
        bmp->RotateFlip(Gdiplus::Rotate270FlipNone);
    }
}

static void OnSignatureCropped(Bitmap* cropped, void* user);

static void BeginPhotoBitmap(SigPad* pad, Bitmap* bmp) {
    if (!pad || !bmp || bmp->GetWidth() < 2 || bmp->GetHeight() < 2) {
        delete bmp;
        if (pad) {
            ShowCantRead(pad);
        }
        return;
    }
    HWND crop = ShowImageCropForBitmap(pad->mainWin, bmp, OnSignatureCropped, pad);
    delete bmp;
    if (!crop) {
        ShowCantRead(pad);
        return;
    }
    SetWindowLongPtrW(crop, GWLP_HWNDPARENT, (LONG_PTR)pad->hwnd);
    SetForegroundWindow(crop);
}

static void BeginPhotoFromFile(SigPad* pad, const char* path) {
    if (!pad || !path) {
        return;
    }
    ByteSlice data = file::ReadFile(path);
    Bitmap* bmp = nullptr;
    if (!data.empty()) {
        bmp = BitmapFromDataWin(data);
    }
    data.Free();
    if (!bmp) {
        ShowCantRead(pad);
        return;
    }
    ApplyViewRotation(bmp, ViewRotationForPath(pad->mainWin, path));
    BeginPhotoBitmap(pad, bmp);
}

static void OnSignatureCropped(Bitmap* cropped, void* user) {
    SigPad* pad = (SigPad*)user;
    if (!pad || !pad->hwnd || !cropped) {
        delete cropped;
        return;
    }
    Bitmap* argb = cropped->Clone(0, 0, cropped->GetWidth(), cropped->GetHeight(), PixelFormat32bppARGB);
    delete cropped;
    if (!argb) {
        ShowCantRead(pad);
        return;
    }
    SetPhotoSource(pad, argb);
}

static Bitmap* BitmapFromDibBytes(const void* dib, size_t size) {
    if (!dib || size < sizeof(BITMAPINFOHEADER)) {
        return nullptr;
    }
    const BITMAPINFOHEADER* hdr = (const BITMAPINFOHEADER*)dib;
    if (hdr->biSize < sizeof(BITMAPINFOHEADER) || hdr->biSize > size) {
        return nullptr;
    }
    size_t offset = hdr->biSize;
    if (hdr->biSize == sizeof(BITMAPINFOHEADER) && hdr->biCompression == BI_BITFIELDS) {
        offset += 12;
    } else if (hdr->biBitCount > 0 && hdr->biBitCount <= 8) {
        int colors = (int)hdr->biClrUsed;
        if (colors <= 0) {
            colors = 1 << hdr->biBitCount;
        }
        offset += (size_t)colors * 4;
    }
    if (offset >= size) {
        return nullptr;
    }
    Bitmap* tmp = new Bitmap((BITMAPINFO*)dib, (void*)((const u8*)dib + offset));
    if (!tmp || tmp->GetWidth() == 0 || tmp->GetHeight() == 0) {
        delete tmp;
        return nullptr;
    }
    Bitmap* clone = tmp->Clone(0, 0, tmp->GetWidth(), tmp->GetHeight(), PixelFormat32bppARGB);
    delete tmp;
    return clone;
}

static Bitmap* BitmapFromPngBytes(const ByteSlice& bytes) {
    if (bytes.empty()) {
        return nullptr;
    }
    Bitmap* bmp = BitmapFromDataWin(bytes);
    if (!bmp || bmp->GetWidth() == 0) {
        delete bmp;
        return nullptr;
    }
    Bitmap* argb = bmp->Clone(0, 0, bmp->GetWidth(), bmp->GetHeight(), PixelFormat32bppARGB);
    delete bmp;
    return argb;
}

static UINT PngClipboardFormat() {
    static UINT fmt = RegisterClipboardFormatW(L"PNG");
    return fmt;
}

static UINT PngMimeClipboardFormat() {
    static UINT fmt = RegisterClipboardFormatW(L"image/png");
    return fmt;
}

static Bitmap* BitmapFromClipboard() {
    if (!OpenClipboard(nullptr)) {
        return nullptr;
    }
    Bitmap* bmp = nullptr;
    UINT png = PngClipboardFormat();
    UINT mime = PngMimeClipboardFormat();
    UINT fmts[4] = {CF_DIB, CF_DIBV5, png, mime};
    for (int i = 0; i < 4 && !bmp; i++) {
        HANDLE h = GetClipboardData(fmts[i]);
        if (!h) {
            continue;
        }
        void* p = GlobalLock(h);
        SIZE_T n = GlobalSize(h);
        if (p && n > 0) {
            if (fmts[i] == png || fmts[i] == mime) {
                ByteSlice slice((const u8*)p, (size_t)n);
                bmp = BitmapFromPngBytes(slice);
            } else {
                bmp = BitmapFromDibBytes(p, (size_t)n);
            }
        }
        if (p) {
            GlobalUnlock(h);
        }
    }
    if (!bmp) {
        HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP);
        if (hb) {
            Bitmap tmp(hb, nullptr);
            if (tmp.GetWidth() > 0 && tmp.GetHeight() > 0) {
                bmp = tmp.Clone(0, 0, tmp.GetWidth(), tmp.GetHeight(), PixelFormat32bppARGB);
            }
        }
    }
    CloseClipboard();
    return bmp;
}

static ByteSlice ReadAllStream(IStream* stream) {
    ByteSlice empty;
    if (!stream) {
        return empty;
    }
    Vec<u8> buf;
    u8 tmp[64 * 1024];
    for (;;) {
        ULONG n = 0;
        HRESULT hr = stream->Read(tmp, sizeof(tmp), &n);
        if (n > 0) {
            buf.Append(tmp, (size_t)n);
        }
        if (n == 0 || FAILED(hr) || buf.Size() > 80 * 1024 * 1024) {
            break;
        }
    }
    if (buf.Size() < 8) {
        return empty;
    }
    u8* out = AllocArray<u8>((size_t)buf.Size());
    memcpy(out, buf.LendData(), (size_t)buf.Size());
    return ByteSlice(out, (size_t)buf.Size());
}

static Bitmap* BitmapFromFileGroup(IDataObject* dataObj) {
    if (!dataObj) {
        return nullptr;
    }
    CLIPFORMAT cfDesc = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    CLIPFORMAT cfContents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    FORMATETC fmtDesc = {cfDesc, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medDesc{};
    if (FAILED(dataObj->GetData(&fmtDesc, &medDesc)) || !medDesc.hGlobal) {
        return nullptr;
    }
    FILEGROUPDESCRIPTORW* group = (FILEGROUPDESCRIPTORW*)GlobalLock(medDesc.hGlobal);
    Bitmap* bmp = nullptr;
    if (group) {
        UINT count = group->cItems;
        if (count > 32) {
            count = 32;
        }
        for (UINT i = 0; i < count && !bmp; i++) {
            const WCHAR* name = group->fgd[i].cFileName;
            char* path = ToUtf8Temp(name);
            if (path && path[0] && !IsSignatureImagePath(path)) {
                continue;
            }
            FORMATETC fmtFile = {cfContents, nullptr, DVASPECT_CONTENT, (LONG)i, TYMED_ISTREAM};
            STGMEDIUM medFile{};
            if (SUCCEEDED(dataObj->GetData(&fmtFile, &medFile)) && medFile.pstm) {
                ByteSlice bytes = ReadAllStream(medFile.pstm);
                bmp = BitmapFromPngBytes(bytes);
                bytes.Free();
                ReleaseStgMedium(&medFile);
            }
            if (!bmp) {
                fmtFile.tymed = TYMED_HGLOBAL;
                if (SUCCEEDED(dataObj->GetData(&fmtFile, &medFile)) && medFile.hGlobal) {
                    void* p = GlobalLock(medFile.hGlobal);
                    SIZE_T n = GlobalSize(medFile.hGlobal);
                    if (p && n > 8) {
                        ByteSlice slice((const u8*)p, (size_t)n);
                        bmp = BitmapFromPngBytes(slice);
                    }
                    if (p) {
                        GlobalUnlock(medFile.hGlobal);
                    }
                    ReleaseStgMedium(&medFile);
                }
            }
        }
        GlobalUnlock(medDesc.hGlobal);
    }
    ReleaseStgMedium(&medDesc);
    return bmp;
}

static Bitmap* BitmapFromDataObject(IDataObject* dataObj, char** filePathOut) {
    if (filePathOut) {
        *filePathOut = nullptr;
    }
    if (!dataObj) {
        return nullptr;
    }
    FORMATETC fmtHDrop = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med{};
    if (SUCCEEDED(dataObj->GetData(&fmtHDrop, &med)) && med.hGlobal) {
        HDROP drop = (HDROP)med.hGlobal;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            WCHAR pathW[MAX_PATH * 4] = {};
            if (!DragQueryFileW(drop, i, pathW, dimof(pathW))) {
                continue;
            }
            char* path = str::Dup(ToUtf8Temp(pathW));
            if (!IsSignatureImagePath(path)) {
                str::Free(path);
                continue;
            }
            if (filePathOut) {
                *filePathOut = path;
            } else {
                str::Free(path);
            }
            ReleaseStgMedium(&med);
            return nullptr;
        }
        ReleaseStgMedium(&med);
    }
    Bitmap* bmp = BitmapFromFileGroup(dataObj);
    if (bmp) {
        return bmp;
    }
    UINT png = PngClipboardFormat();
    UINT mime = PngMimeClipboardFormat();
    UINT fmts[4] = {CF_DIB, CF_DIBV5, png, mime};
    DWORD tymeds[4] = {TYMED_HGLOBAL, TYMED_HGLOBAL, TYMED_HGLOBAL, TYMED_HGLOBAL};
    for (int i = 0; i < 4 && !bmp; i++) {
        FORMATETC fmt = {(CLIPFORMAT)fmts[i], nullptr, DVASPECT_CONTENT, -1, tymeds[i]};
        STGMEDIUM medium{};
        if (FAILED(dataObj->GetData(&fmt, &medium)) || !medium.hGlobal) {
            continue;
        }
        void* p = GlobalLock(medium.hGlobal);
        SIZE_T n = GlobalSize(medium.hGlobal);
        if (p && n > 0) {
            if (fmts[i] == png || fmts[i] == mime) {
                ByteSlice slice((const u8*)p, (size_t)n);
                bmp = BitmapFromPngBytes(slice);
            } else {
                bmp = BitmapFromDibBytes(p, (size_t)n);
            }
        }
        if (p) {
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    if (!bmp) {
        FORMATETC fmtBmp = {CF_BITMAP, nullptr, DVASPECT_CONTENT, -1, TYMED_GDI};
        STGMEDIUM medBmp{};
        if (SUCCEEDED(dataObj->GetData(&fmtBmp, &medBmp)) && medBmp.hBitmap) {
            Bitmap tmp(medBmp.hBitmap, nullptr);
            if (tmp.GetWidth() > 0 && tmp.GetHeight() > 0) {
                bmp = tmp.Clone(0, 0, tmp.GetWidth(), tmp.GetHeight(), PixelFormat32bppARGB);
            }
            ReleaseStgMedium(&medBmp);
        }
    }
    return bmp;
}

static bool DataObjectCanFeedSignature(IDataObject* dataObj) {
    if (!dataObj) {
        return false;
    }
    CLIPFORMAT cfDesc = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    UINT png = PngClipboardFormat();
    UINT mime = PngMimeClipboardFormat();
    FORMATETC fmts[] = {
        {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {CF_DIB, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {CF_DIBV5, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {(CLIPFORMAT)png, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {(CLIPFORMAT)mime, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {CF_BITMAP, nullptr, DVASPECT_CONTENT, -1, TYMED_GDI},
        {cfDesc, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
    };
    for (int i = 0; i < (int)dimof(fmts); i++) {
        if (dataObj->QueryGetData(&fmts[i]) == S_OK) {
            return true;
        }
    }
    return false;
}

class SigDropTarget : public IDropTarget {
    LONG refCount = 1;
    SigPad* pad = nullptr;
    bool accept = false;

  public:
    explicit SigDropTarget(SigPad* p) : pad(p) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refCount);
        if (r == 0) {
            delete this;
        }
        return (ULONG)r;
    }
    STDMETHODIMP DragEnter(IDataObject* dataObj, __unused DWORD grfKeyState, __unused POINTL pt,
                           DWORD* effect) override {
        accept = DataObjectCanFeedSignature(dataObj);
        *effect = accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(__unused DWORD grfKeyState, __unused POINTL pt, DWORD* effect) override {
        *effect = accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override { return S_OK; }
    STDMETHODIMP Drop(IDataObject* dataObj, __unused DWORD grfKeyState, __unused POINTL pt, DWORD* effect) override {
        *effect = DROPEFFECT_COPY;
        if (!pad || !pad->hwnd) {
            return S_OK;
        }
        char* path = nullptr;
        Bitmap* bmp = BitmapFromDataObject(dataObj, &path);
        if (path) {
            delete bmp;
            BeginPhotoFromFile(pad, path);
            str::Free(path);
            return S_OK;
        }
        if (bmp) {
            BeginPhotoBitmap(pad, bmp);
        }
        return S_OK;
    }
};

static void RegisterPadDrop(SigPad* pad) {
    if (!pad || !pad->hwnd || pad->dropTarget) {
        return;
    }
    OleInitialize(nullptr);
    auto* dt = new SigDropTarget(pad);
    if (SUCCEEDED(RegisterDragDrop(pad->hwnd, dt))) {
        pad->dropTarget = dt;
    } else {
        dt->Release();
    }
}

static void RevokePadDrop(SigPad* pad) {
    if (!pad || !pad->hwnd || !pad->dropTarget) {
        return;
    }
    RevokeDragDrop(pad->hwnd);
    pad->dropTarget->Release();
    pad->dropTarget = nullptr;
}

static void PickPhotoFile(SigPad* pad) {
    if (!pad || !pad->hwnd) {
        return;
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = pad->hwnd;
    ofn.lpstrFilter = L"Images (*.jpg;*.png;*.bmp;*.webp)\0*.jpg;*.jpeg;*.png;*.bmp;*.webp\0\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    WCHAR file[MAX_PATH * 4] = {};
    ofn.lpstrFile = file;
    ofn.nMaxFile = dimof(file);
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    BeginPhotoFromFile(pad, ToUtf8Temp(file));
}

static void PastePhoto(SigPad* pad) {
    Bitmap* bmp = BitmapFromClipboard();
    if (!bmp) {
        ShowCantRead(pad);
        return;
    }
    BeginPhotoBitmap(pad, bmp);
}

static Bitmap* LoadSignaturePng() {
    TempStr path = SignaturePngPath();
    if (!path || !file::Exists(path)) {
        path = SignaturePngPathLegacy();
    }
    if (!path || !file::Exists(path)) {
        return nullptr;
    }
    ByteSlice data = file::ReadFile(path);
    Bitmap* bmp = nullptr;
    if (!data.empty()) {
        bmp = BitmapFromPngBytes(data);
    }
    data.Free();
    return bmp;
}

static void PaintPad(SigPad* pad, HDC hdc, const RECT& client) {
    HBRUSH face = pad->bgBrush;
    HBRUSH owned = nullptr;
    if (!face) {
        owned = CreateSolidBrush(ThemeWindowBackgroundColor());
        face = owned;
    }
    FillRect(hdc, &client, face);
    if (owned) {
        DeleteObject(owned);
    }
    HBRUSH paper = CreateSolidBrush(SignaturePaperColor());
    FillRect(hdc, &pad->padRc, paper);
    DeleteObject(paper);
    HPEN border = CreatePen(PS_SOLID, 1, SignatureGuideColor());
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, pad->padRc.left, pad->padRc.top, pad->padRc.right, pad->padRc.bottom);
    if (pad->hasPhoto && pad->photoDib && pad->photoW > 0 && pad->photoH > 0) {
        float aspect = (float)pad->photoH / (float)pad->photoW;
        float dw = (float)pad->padW;
        float dh = dw * aspect;
        if (dh > (float)pad->padH) {
            dh = (float)pad->padH;
            dw = aspect > 0.01f ? dh / aspect : dw;
        }
        int dx = (int)(dw + 0.5f);
        int dy = (int)(dh + 0.5f);
        if (dx < 1) {
            dx = 1;
        }
        if (dy < 1) {
            dy = 1;
        }
        int x = pad->padRc.left + (pad->padW - dx) / 2;
        int y = pad->padRc.top + (pad->padH - dy) / 2;
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ oldBmp = SelectObject(mem, pad->photoDib);
        BLENDFUNCTION bf{};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(hdc, x, y, dx, dy, mem, 0, 0, pad->photoW, pad->photoH, bf);
        SelectObject(mem, oldBmp);
        DeleteDC(mem);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(border);
        return;
    }
    int baseY = pad->padRc.bottom - DpiScale(pad->hwnd, 36);
    if (baseY > pad->padRc.top + 8) {
        HPEN basePen = CreatePen(PS_SOLID, 1, SignatureGuideColor());
        SelectObject(hdc, basePen);
        int inset = DpiScale(pad->hwnd, 16);
        MoveToEx(hdc, pad->padRc.left + inset, baseY, nullptr);
        LineTo(hdc, pad->padRc.right - inset, baseY);
        SelectObject(hdc, border);
        DeleteObject(basePen);
    }
    if (pad->pts.Size() == 0) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, SignatureHintColor());
        HFONT font = GetAppFontForHwnd(pad->hwnd);
        HGDIOBJ oldFont = nullptr;
        if (font) {
            oldFont = SelectObject(hdc, font);
        }
        RECT hint = pad->padRc;
        hint.bottom = (hint.top + hint.bottom) / 2;
        TempWStr label = ToWStrTemp(_TRA("Draw your signature here"));
        DrawTextW(hdc, label, -1, &hint, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        hint.top = hint.bottom;
        hint.bottom = pad->padRc.bottom;
        TempWStr photoHint = ToWStrTemp(_TRA("Paste or drop an image"));
        DrawTextW(hdc, photoHint, -1, &hint, DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (oldFont) {
            SelectObject(hdc, oldFont);
        }
    }
    int penW = DpiScale(pad->hwnd, 2);
    if (penW < 2) {
        penW = 2;
    }
    LOGBRUSH lb{BS_SOLID, SignatureInkColor(), 0};
    HPEN ink = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, penW, &lb, 0, nullptr);
    SelectObject(hdc, ink);
    int src = 0;
    for (int i = 0; i < pad->counts.Size(); i++) {
        int n = pad->counts.at(i);
        if (n >= 2) {
            Vec<POINT> poly;
            for (int k = 0; k < n; k++) {
                PointF p = pad->pts.at(src + k);
                POINT q;
                q.x = pad->padRc.left + (int)(p.x * (float)pad->padW + 0.5f);
                q.y = pad->padRc.top + (int)(p.y * (float)pad->padH + 0.5f);
                poly.Append(q);
            }
            Polyline(hdc, poly.LendData(), poly.Size());
        }
        if (n > 0) {
            src += n;
        }
    }
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(ink);
    DeleteObject(border);
}

static PointF PadNormPoint(SigPad* pad, int x, int y) {
    PointF p;
    p.x = pad->padW > 1 ? ((float)x - (float)pad->padRc.left) / (float)pad->padW : 0.f;
    p.y = pad->padH > 1 ? ((float)y - (float)pad->padRc.top) / (float)pad->padH : 0.f;
    if (p.x < 0.f) {
        p.x = 0.f;
    }
    if (p.y < 0.f) {
        p.y = 0.f;
    }
    if (p.x > 1.f) {
        p.x = 1.f;
    }
    if (p.y > 1.f) {
        p.y = 1.f;
    }
    return p;
}

static bool InPad(SigPad* pad, int x, int y) {
    return x >= pad->padRc.left && x < pad->padRc.right && y >= pad->padRc.top && y < pad->padRc.bottom;
}

static void EndStroke(SigPad* pad) {
    if (!pad->drawing) {
        return;
    }
    pad->drawing = false;
    if (GetCapture() == pad->hwnd) {
        ReleaseCapture();
    }
    if (pad->counts.Size() == 0) {
        UpdateOkButton(pad);
        return;
    }
    int n = pad->counts.Last();
    if (n < 2) {
        while (n-- > 0 && pad->pts.Size() > 0) {
            pad->pts.Pop();
        }
        pad->counts.Pop();
    }
    UpdateOkButton(pad);
}

static void ClearPad(SigPad* pad) {
    EndStroke(pad);
    pad->pts.Reset();
    pad->counts.Reset();
    pad->loadedPts.Reset();
    pad->loadedCounts.Reset();
    pad->mapped = true;
    ClearPhoto(pad);
}

// Re-enable the document before the pad goes away. While it is still disabled,
// Windows activates another owned window, and the annotations panel then
// switches back to the tab it belongs to.
static void ClosePad(SigPad* pad, bool accepted) {
    if (!pad || !pad->hwnd) {
        return;
    }
    pad->accepted = accepted;
    HWND owner = GetWindow(pad->hwnd, GW_OWNER);
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    DestroyWindow(pad->hwnd);
    if (owner && IsWindow(owner)) {
        SetForegroundWindow(owner);
        SetFocus(owner);
    }
}

static void AcceptPad(SigPad* pad) {
    if (!HasStroke(pad->counts) && !pad->hasPhoto) {
        return;
    }
    EndStroke(pad);
    ClosePad(pad, true);
}

static void CancelPad(SigPad* pad) {
    ClosePad(pad, false);
}

static void PadRecreateThemeBrush(SigPad* pad) {
    if (!pad) {
        return;
    }
    DeleteObject(pad->bgBrush);
    pad->bgBrush = CreateSolidBrush(ThemeWindowBackgroundColor());
}

static void PadThemeRefreshCb(HWND hwnd, void* ctx) {
    auto* pad = (SigPad*)ctx;
    if (!pad) {
        return;
    }
    PadRecreateThemeBrush(pad);
    AppDialogApplyChrome(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static HWND CreatePadButton(HWND parent, int id, const char* text, bool isDefault) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
    TempWStr label = ToWStrTemp(_TRA(text));
    HWND btn = CreateWindowExW(0, L"BUTTON", label, style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    if (btn) {
        HFONT font = GetAppFontForHwnd(parent);
        if (font) {
            SendMessageW(btn, WM_SETFONT, (WPARAM)font, FALSE);
        }
    }
    return btn;
}

static LRESULT CALLBACK PadWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SigPad* pad = (SigPad*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        pad = (SigPad*)cs->lpCreateParams;
        pad->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pad);
        pad->btnPhoto = CreatePadButton(hwnd, kPadPhotoId, "Open Image", false);
        pad->btnClear = CreatePadButton(hwnd, kPadClearId, "Clear", false);
        pad->btnOk = CreatePadButton(hwnd, kPadOkId, "OK", true);
        pad->btnCancel = CreatePadButton(hwnd, kPadCancelId, "&Cancel", false);
        PadRecreateThemeBrush(pad);
        AppDialogApplyChrome(hwnd);
        RegisterAppDialogForTheme(hwnd, PadThemeRefreshCb, pad);
        RegisterPadDrop(pad);
        return 0;
    }
    if (!pad) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_SIZE:
            LayoutPad(hwnd, pad);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG: {
            HBRUSH br = AppDialogCtlColorBrush(msg, wp, lp, pad->bgBrush);
            if (br) {
                return (LRESULT)br;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (rc.right > 0 && rc.bottom > 0) {
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
                HGDIOBJ old = SelectObject(mem, bmp);
                PaintPad(pad, mem, rc);
                BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED) {
                int id = LOWORD(wp);
                if (id == kPadClearId) {
                    ClearPad(pad);
                } else if (id == kPadOkId) {
                    AcceptPad(pad);
                } else if (id == kPadCancelId) {
                    CancelPad(pad);
                } else if (id == kPadPhotoId) {
                    PickPhotoFile(pad);
                }
            }
            return 0;
        case WM_PASTE:
            PastePhoto(pad);
            return 0;
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            if (!InPad(pad, x, y)) {
                return 0;
            }
            if (pad->hasPhoto) {
                ClearPhoto(pad);
            }
            if (pad->drawing) {
                EndStroke(pad);
            }
            PointF p = PadNormPoint(pad, x, y);
            pad->pts.Append(p);
            pad->counts.Append(1);
            pad->drawing = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, &pad->padRc, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (pad->drawing && (wp & MK_LBUTTON)) {
                PointF p = PadNormPoint(pad, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (pad->pts.Size() > 0) {
                    PointF prev = pad->pts.Last();
                    float dx = (p.x - prev.x) * (float)pad->padW;
                    float dy = (p.y - prev.y) * (float)pad->padH;
                    if (dx * dx + dy * dy < 2.25f) {
                        return 0;
                    }
                }
                pad->pts.Append(p);
                pad->counts.Last() += 1;
                InvalidateRect(hwnd, &pad->padRc, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            EndStroke(pad);
            InvalidateRect(hwnd, &pad->padRc, FALSE);
            return 0;
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursor(nullptr, InPad(pad, pt.x, pt.y) ? IDC_CROSS : IDC_ARROW));
            return TRUE;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = DpiScale(hwnd, 360);
            mmi->ptMinTrackSize.y = DpiScale(hwnd, 220);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                CancelPad(pad);
                return 0;
            }
            if (wp == VK_RETURN) {
                AcceptPad(pad);
                return 0;
            }
            break;
        case WM_CLOSE:
            CancelPad(pad);
            return 0;
        case WM_DESTROY:
            UnregisterAppDialogForTheme(hwnd);
            RevokePadDrop(pad);
            DeleteObject(pad->bgBrush);
            pad->bgBrush = nullptr;
            pad->hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RegisterPadClass() {
    if (gPadClassRegistered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PadWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kPadClassName;
    if (RegisterClassExW(&wc)) {
        gPadClassRegistered = true;
    }
}

// Returns true when the user pressed OK with at least one stroke.
static bool RunSignaturePad(HWND owner, MainWindow* mainWin, Vec<PointF>& pts, Vec<int>& counts, float& aspect,
                            int& padW, int& padH, Bitmap** photoOut) {
    if (photoOut) {
        *photoOut = nullptr;
    }
    RegisterPadClass();
    SigPad pad;
    pad.mainWin = mainWin;
    Bitmap* saved = LoadSignaturePng();
    if (saved) {
        SetPhotoSource(&pad, saved);
    } else {
        LoadSignatureFile(&pad);
    }
    int width = DpiScale(owner, 580);
    int height = DpiScale(owner, 280);
    RECT orc{};
    GetWindowRect(owner, &orc);
    int x = orc.left + ((orc.right - orc.left) - width) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - height) / 2;
    TempWStr title = ToWStrTemp(_TRA("Handwritten Signature"));
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPadClassName, title,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, width,
                                height, owner, nullptr, GetModuleHandleW(nullptr), &pad);
    if (!hwnd) {
        return false;
    }
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetFocus(hwnd);
    MSG msg;
    while (IsWindow(hwnd)) {
        int r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (r < 0) {
            break;
        }
        HWND focus = GetFocus();
        bool padFocus = focus == hwnd || IsChild(hwnd, focus);
        if (padFocus && msg.message == WM_KEYDOWN && msg.wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hwnd, WM_PASTE, 0, 0);
            continue;
        }
        if (padFocus && msg.message == WM_KEYDOWN && (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN)) {
            SendMessageW(hwnd, msg.message, msg.wParam, msg.lParam);
            continue;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetFocus(owner);
    if (!pad.accepted) {
        return false;
    }
    if (pad.hasPhoto && pad.photoSrc) {
        Bitmap* out = ThresholdBitmap(pad.photoSrc, pad.threshold);
        if (!out) {
            return false;
        }
        int w = (int)out->GetWidth();
        int h = (int)out->GetHeight();
        if (photoOut) {
            *photoOut = out;
        } else {
            delete out;
        }
        aspect = w > 0 ? (float)h / (float)w : 0.35f;
        padW = w;
        padH = h;
        return true;
    }
    if (!HasStroke(pad.counts)) {
        return false;
    }
    pts = pad.pts;
    counts = pad.counts;
    aspect = pad.loadedAspect;
    padW = pad.padW;
    padH = pad.padH;
    return true;
}

static void ShowPlaceHint(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    NotificationCreateArgs args;
    args.hwndParent = win->hwndCanvas;
    args.msg = _TRA(
        "Hold and drag to set the size. A click places it so you can move or resize it. Click the page to leave "
        "edit mode. Ctrl+click opens the annotation editor. Esc cancels.");
    args.timeoutMs = 0;
    args.replaceExisting = true;
    ShowNotification(args);
}

static void StartPlacing(MainWindow* win, const Vec<PointF>& pts, const Vec<int>& counts, float aspect) {
    if (gPlacing && gPlaceWin && gPlaceWin != win) {
        HandwrittenSignatureCancelPlace(gPlaceWin);
    }
    SetAnnotCreateTool(win, 0);
    gPlacePts = pts;
    gPlaceCounts = counts;
    gPlaceAspect = aspect;
    gPlaceIsPhoto = false;
    if (gPlacePhoto) {
        DeleteObject(gPlacePhoto);
        gPlacePhoto = nullptr;
    }
    gPlacePhotoW = 0;
    gPlacePhotoH = 0;
    gPlacing = true;
    gPlaceWin = win;
    UpdateAnnotToolToolbarButtons(win);
    ShowPlaceHint(win);
    if (win->hwndFrame) {
        HwndSetFocus(win->hwndFrame);
    }
    if (win->hwndCanvas) {
        SendMessageW(win->hwndCanvas, WM_SETCURSOR, (WPARAM)win->hwndCanvas, MAKELPARAM(HTCLIENT, 0));
    }
}

bool HandwrittenSignatureIsPlacing(MainWindow* win) {
    return gPlacing && win && win == gPlaceWin;
}

void HandwrittenSignatureCancelPlace(MainWindow* win) {
    if (!gPlacing) {
        return;
    }
    if (win && gPlaceWin && win != gPlaceWin) {
        return;
    }
    MainWindow* placeWin = gPlaceWin;
    gPlacing = false;
    gPlaceDragging = false;
    gPlaceWin = nullptr;
    gPlacePts.Reset();
    gPlaceCounts.Reset();
    gPlaceIsPhoto = false;
    if (gPlacePhoto) {
        DeleteObject(gPlacePhoto);
        gPlacePhoto = nullptr;
    }
    gPlacePhotoW = 0;
    gPlacePhotoH = 0;
    if (placeWin && placeWin->hwndCanvas) {
        if (GetCapture() == placeWin->hwndCanvas) {
            ReleaseCapture();
        }
        RemoveNotificationsForGroup(placeWin->hwndCanvas, kNotifActionResponse);
    }
    if (placeWin) {
        UpdateAnnotToolToolbarButtons(placeWin);
        if (placeWin->hwndCanvas) {
            SendMessageW(placeWin->hwndCanvas, WM_SETCURSOR, (WPARAM)placeWin->hwndCanvas, MAKELPARAM(HTCLIENT, 0));
        }
    }
}

static void NotifyCantStore(MainWindow* win) {
    HWND hwnd = nullptr;
    if (win) {
        hwnd = win->hwndCanvas ? win->hwndCanvas : win->hwndFrame;
    }
    if (!hwnd) {
        return;
    }
    ShowTemporaryNotification(hwnd, _TRA("This document can't store a signature."), kNotif5SecsTimeOut);
}

void HandwrittenSignatureOpen(MainWindow* win) {
    if (!win) {
        return;
    }
    if (HandwrittenSignatureIsPlacing(win)) {
        HandwrittenSignatureCancelPlace(win);
        return;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    WindowTab* tab = win->CurrentTab();
    bool pdfOk = engine && EngineSupportsAnnotations(engine);
    bool ebookOk = tab && EbookAnnotationsSupported(tab);
    if (!pdfOk && !ebookOk) {
        NotifyCantStore(win);
        return;
    }
    if (!win->hwndFrame) {
        return;
    }
    Vec<PointF> pts;
    Vec<int> counts;
    float aspect = 0.35f;
    int padW = 0;
    int padH = 0;
    Bitmap* photo = nullptr;
    if (!RunSignaturePad(win->hwndFrame, win, pts, counts, aspect, padW, padH, &photo)) {
        return;
    }
    if (photo) {
        bool saved = SaveSignaturePng(photo);
        HBITMAP dib = nullptr;
        int w = (int)photo->GetWidth();
        int h = (int)photo->GetHeight();
        if (saved) {
            Gdiplus::BitmapData data{};
            if (LockArgb(photo, data)) {
                dib = DibFromLocked(data, w, h, 256, SignatureOnPageInkColor());
                photo->UnlockBits(&data);
            }
        }
        delete photo;
        if (!saved || !dib) {
            if (dib) {
                DeleteObject(dib);
            }
            ShowTemporaryNotification(win->hwndCanvas, _TRA("Couldn't add the signature."), kNotif5SecsTimeOut);
            return;
        }
        if (gPlacing && gPlaceWin && gPlaceWin != win) {
            HandwrittenSignatureCancelPlace(gPlaceWin);
        }
        SetAnnotCreateTool(win, 0);
        gPlacePts.Reset();
        gPlaceCounts.Reset();
        gPlaceAspect = h > 0 && w > 0 ? (float)h / (float)w : 0.35f;
        gPlaceIsPhoto = true;
        if (gPlacePhoto) {
            DeleteObject(gPlacePhoto);
        }
        gPlacePhoto = dib;
        gPlacePhotoW = w;
        gPlacePhotoH = h;
        gPlacing = true;
        gPlaceWin = win;
        UpdateAnnotToolToolbarButtons(win);
        ShowPlaceHint(win);
        if (win->hwndFrame) {
            HwndSetFocus(win->hwndFrame);
        }
        if (win->hwndCanvas) {
            SendMessageW(win->hwndCanvas, WM_SETCURSOR, (WPARAM)win->hwndCanvas, MAKELPARAM(HTCLIENT, 0));
        }
        return;
    }
    if (!CropSignature(pts, counts, (float)padW, (float)padH, aspect)) {
        return;
    }
    SaveSignatureFile(pts, counts, aspect);
    DeleteSignaturePng();
    StartPlacing(win, pts, counts, aspect);
}

static float ClampedAspect() {
    float aspect = gPlaceAspect;
    float lo = gPlaceIsPhoto ? 0.05f : 0.12f;
    float hi = gPlaceIsPhoto ? 8.f : 1.5f;
    if (aspect < lo) {
        aspect = lo;
    }
    if (aspect > hi) {
        aspect = hi;
    }
    return aspect;
}

static void DefaultScreenBox(DisplayModel* dm, int x, int y, int pageNo, int& left, int& top, int& sw, int& sh) {
    float zoom = dm->GetZoomReal(pageNo);
    if (zoom < 0.05f) {
        zoom = dm->zoomReal > 0.05f ? dm->zoomReal : 1.f;
    }
    float aspect = ClampedAspect();
    sw = (int)(kSigWidthPt * zoom + 0.5f);
    sh = (int)((float)sw * aspect + 0.5f);
    if (sw < 12) {
        sw = 12;
    }
    if (sh < 8) {
        sh = 8;
    }
    left = x - sw / 2;
    top = y - sh / 2;
    PageInfo* pi = dm->GetPageInfo(pageNo);
    if (pi && pi->pageOnScreen.dx > sw && pi->pageOnScreen.dy > sh) {
        Rect pr = pi->pageOnScreen;
        if (left < pr.x) {
            left = pr.x;
        }
        if (top < pr.y) {
            top = pr.y;
        }
        if (left + sw > pr.x + pr.dx) {
            left = pr.x + pr.dx - sw;
        }
        if (top + sh > pr.y + pr.dy) {
            top = pr.y + pr.dy - sh;
        }
    }
}

// A real drag sizes the signature. A click keeps the default size.
static bool DragScreenBox(int x0, int y0, int x1, int y1, int& left, int& top, int& sw, int& sh) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx < 8 && ady < 8) {
        return false;
    }
    float aspect = ClampedAspect();
    if ((float)ady <= (float)adx * aspect) {
        sw = adx < 12 ? 12 : adx;
        sh = (int)((float)sw * aspect + 0.5f);
        if (sh < 8) {
            sh = 8;
        }
    } else {
        sh = ady < 8 ? 8 : ady;
        sw = aspect > 0.01f ? (int)((float)sh / aspect + 0.5f) : adx;
        if (sw < 12) {
            sw = 12;
        }
    }
    left = dx < 0 ? x0 - sw : x0;
    top = dy < 0 ? y0 - sh : y0;
    return true;
}

static bool CommitSignatureBox(MainWindow* win, int left, int top, int sw, int sh) {
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        HandwrittenSignatureCancelPlace(win);
        return true;
    }
    int pageNo = dm->GetPageNoByPoint(Point(left + sw / 2, top + sh / 2));
    if (!dm->ValidPageNo(pageNo)) {
        pageNo = dm->GetPageNoByPoint(Point(left, top));
    }
    if (!dm->ValidPageNo(pageNo)) {
        return true;
    }
    EngineBase* engine = dm->GetEngine();
    WindowTab* tab = win->CurrentTab();
    bool pdfOk = engine && EngineSupportsAnnotations(engine);
    bool ebookOk = tab && EbookAnnotationsSupported(tab);
    if (!pdfOk && !ebookOk) {
        NotifyCantStore(win);
        HandwrittenSignatureCancelPlace(win);
        return true;
    }
    if (gPlaceIsPhoto) {
        PointF a = dm->CvtFromScreen(Point(left, top), pageNo);
        PointF b = dm->CvtFromScreen(Point(left + sw, top + sh), pageNo);
        RectF rect = RectF::FromXY(a, b);
        if (rect.dx < 1.f || rect.dy < 1.f) {
            return true;
        }
        TempStr pngPath = SignaturePngPath();
        ByteSlice png = file::ReadFile(pngPath);
        if (ebookOk && !pdfOk) {
            EbookAnnotation* ebookAnnot =
                EbookAnnotationsCreateSignatureImage(tab, dm, pageNo, rect, png.data(), png.Size());
            png.Free();
            HandwrittenSignatureCancelPlace(win);
            if (!ebookAnnot) {
                ShowTemporaryNotification(win->hwndCanvas, _TRA("Couldn't add the signature."), kNotif5SecsTimeOut);
                return true;
            }
            tab->selectedEbookAnnotation = ebookAnnot;
            ShowEditEbookAnnotationsWindow(tab, ebookAnnot);
            MainWindowRerender(win);
            ToolbarUpdateStateForWindow(win, true);
            return true;
        }
        Annotation* annot = EngineMupdfCreateAnnotationStampPng(engine, pageNo, rect, png);
        png.Free();
        HandwrittenSignatureCancelPlace(win);
        if (!annot) {
            ShowTemporaryNotification(win->hwndCanvas, _TRA("Couldn't add the signature."), kNotif5SecsTimeOut);
            return true;
        }
        if (tab) {
            UpdateAnnotationsList(tab->editAnnotsWindow);
            SetSelectedAnnotation(tab, annot);
        }
        MainWindowRerenderAnnotationChange(win, pageNo, annot);
        ToolbarUpdateStateForWindow(win, true);
        return true;
    }
    PointF origin = dm->CvtFromScreen(Point(left, top), pageNo);
    PointF right = dm->CvtFromScreen(Point(left + sw, top), pageNo);
    PointF down = dm->CvtFromScreen(Point(left, top + sh), pageNo);
    float vx = right.x - origin.x;
    float vy = right.y - origin.y;
    float ux = down.x - origin.x;
    float uy = down.y - origin.y;
    float area = vx * uy - vy * ux;
    if (area > -0.5f && area < 0.5f) {
        return true;
    }
    Vec<PointF> flat;
    Vec<int> counts;
    int src = 0;
    for (int i = 0; i < gPlaceCounts.Size(); i++) {
        int n = gPlaceCounts.at(i);
        if (n >= 2) {
            for (int k = 0; k < n; k++) {
                PointF nrm = gPlacePts.at(src + k);
                PointF p;
                p.x = origin.x + vx * nrm.x + ux * nrm.y;
                p.y = origin.y + vy * nrm.x + uy * nrm.y;
                flat.Append(p);
            }
            counts.Append(n);
        }
        if (n > 0) {
            src += n;
        }
    }
    if (!HasStroke(counts)) {
        HandwrittenSignatureCancelPlace(win);
        return true;
    }
    if (ebookOk && !pdfOk) {
        EbookAnnotation* ebookAnnot = EbookAnnotationsCreateInkStrokes(
            tab, dm, pageNo, flat.LendData(), counts.LendData(), counts.Size(), RGB(0, 0, 0), 2);
        HandwrittenSignatureCancelPlace(win);
        if (!ebookAnnot) {
            ShowTemporaryNotification(win->hwndCanvas, _TRA("Couldn't add the signature."), kNotif5SecsTimeOut);
            return true;
        }
        tab->selectedEbookAnnotation = ebookAnnot;
        ShowEditEbookAnnotationsWindow(tab, ebookAnnot);
        MainWindowRerender(win);
        ToolbarUpdateStateForWindow(win, true);
        return true;
    }
    AnnotCreateArgs args;
    args.annotType = AnnotationType::Ink;
    Annotation* annot = EngineMupdfCreateAnnotationInkStrokes(engine, pageNo, flat.LendData(), counts.LendData(),
                                                              counts.Size(), &args, 1.6f);
    HandwrittenSignatureCancelPlace(win);
    if (!annot) {
        ShowTemporaryNotification(win->hwndCanvas, _TRA("Couldn't add the signature."), kNotif5SecsTimeOut);
        return true;
    }
    if (tab) {
        UpdateAnnotationsList(tab->editAnnotsWindow);
        // Selected immediately so the new signature can be moved and resized.
        SetSelectedAnnotation(tab, annot);
    }
    MainWindowRerenderAnnotationChange(win, pageNo, annot);
    ToolbarUpdateStateForWindow(win, true);
    return true;
}

static bool PreviewBox(MainWindow* win, int& left, int& top, int& sw, int& sh) {
    if (!gPlaceDragging || !HandwrittenSignatureIsPlacing(win)) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }
    if (!DragScreenBox(gDragX0, gDragY0, gDragX1, gDragY1, left, top, sw, sh)) {
        return false;
    }
    return sw > 2 && sh > 2;
}

bool HandwrittenSignatureOnMouseDown(MainWindow* win, int x, int y) {
    if (!HandwrittenSignatureIsPlacing(win)) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        HandwrittenSignatureCancelPlace(win);
        return true;
    }
    if (!dm->ValidPageNo(dm->GetPageNoByPoint(Point{x, y}))) {
        return true;
    }
    gPlaceDragging = true;
    gDragX0 = gDragX1 = x;
    gDragY0 = gDragY1 = y;
    SetCapture(win->hwndCanvas);
    ScheduleRepaint(win, 0);
    return true;
}

bool HandwrittenSignatureOnMouseMove(MainWindow* win, int x, int y) {
    if (!gPlaceDragging || !HandwrittenSignatureIsPlacing(win)) {
        return false;
    }
    if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
        return HandwrittenSignatureOnMouseUp(win, x, y);
    }
    gDragX1 = x;
    gDragY1 = y;
    ScheduleRepaint(win, 0);
    return true;
}

bool HandwrittenSignatureOnMouseUp(MainWindow* win, int x, int y) {
    if (!gPlaceDragging || !HandwrittenSignatureIsPlacing(win)) {
        return false;
    }
    gPlaceDragging = false;
    gDragX1 = x;
    gDragY1 = y;
    if (win->hwndCanvas && GetCapture() == win->hwndCanvas) {
        ReleaseCapture();
    }
    int left = 0;
    int top = 0;
    int sw = 0;
    int sh = 0;
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        HandwrittenSignatureCancelPlace(win);
        return true;
    }
    if (!DragScreenBox(gDragX0, gDragY0, gDragX1, gDragY1, left, top, sw, sh)) {
        int pageNo = dm->GetPageNoByPoint(Point(x, y));
        if (!dm->ValidPageNo(pageNo)) {
            ScheduleRepaint(win, 0);
            return true;
        }
        DefaultScreenBox(dm, x, y, pageNo, left, top, sw, sh);
    }
    CommitSignatureBox(win, left, top, sw, sh);
    ScheduleRepaint(win, 0);
    return true;
}

void HandwrittenSignaturePaintPreview(HDC hdc, MainWindow* win) {
    int left = 0;
    int top = 0;
    int sw = 0;
    int sh = 0;
    if (!PreviewBox(win, left, top, sw, sh)) {
        return;
    }
    HPEN border = CreatePen(PS_SOLID, 1, SignatureOnPageFrameColor());
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    if (gPlaceIsPhoto && gPlacePhoto && gPlacePhotoW > 0 && gPlacePhotoH > 0) {
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ oldBmp = SelectObject(mem, gPlacePhoto);
        BLENDFUNCTION bf{};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(hdc, left, top, sw, sh, mem, 0, 0, gPlacePhotoW, gPlacePhotoH, bf);
        SelectObject(mem, oldBmp);
        DeleteDC(mem);
        Rectangle(hdc, left, top, left + sw, top + sh);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(border);
        return;
    }
    Rectangle(hdc, left, top, left + sw, top + sh);
    int penW = 2;
    LOGBRUSH lb{BS_SOLID, SignatureOnPageInkColor(), 0};
    HPEN ink = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, penW, &lb, 0, nullptr);
    if (ink) {
        SelectObject(hdc, ink);
    }
    int src = 0;
    for (int i = 0; i < gPlaceCounts.Size(); i++) {
        int n = gPlaceCounts.at(i);
        if (n >= 2) {
            Vec<POINT> poly;
            for (int k = 0; k < n; k++) {
                PointF p = gPlacePts.at(src + k);
                POINT q;
                q.x = left + (int)(p.x * (float)sw + 0.5f);
                q.y = top + (int)(p.y * (float)sh + 0.5f);
                poly.Append(q);
            }
            Polyline(hdc, poly.LendData(), poly.Size());
        }
        if (n > 0) {
            src += n;
        }
    }
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    if (ink) {
        DeleteObject(ink);
    }
    DeleteObject(border);
}
