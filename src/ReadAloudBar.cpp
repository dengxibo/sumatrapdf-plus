/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"

#include "utils/Log.h"
#include "WindowTab.h"
#include "MainWindow.h"
#include "Commands.h"
#include "FloatingPopupStyle.h"
#include "Translations.h"
#include "ReadAloudBar.h"

// indices are part of the trace log read by the UI test scripts: append new buttons
enum RabBtn {
    kRabBtnPrev,
    kRabBtnBack,
    kRabBtnPlay,
    kRabBtnFwd,
    kRabBtnNext,
    kRabBtnRate,
    kRabBtnTime,
    kRabBtnFollow,
    kRabBtnClose,
    kRabBtnSettings,
    kRabBtnCount,
};

// left to right
static const int kRabLayoutOrder[] = {kRabBtnPrev, kRabBtnBack, kRabBtnPlay,   kRabBtnFwd,      kRabBtnNext,
                                      kRabBtnRate, kRabBtnTime, kRabBtnFollow, kRabBtnSettings, kRabBtnClose};
static_assert(dimof(kRabLayoutOrder) == kRabBtnCount);

struct RabBar {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    ReadAloudBarSource* src = nullptr;
    HFONT font = nullptr;
    HFONT iconFont = nullptr;
    bool haveIcons = false;
    int dpi = 0;
    Rect btn[kRabBtnCount];
    bool visibleBtn[kRabBtnCount]{};
    int hot = -1;
    int pressed = -1;
    bool tracking = false;
    char* labels[kRabBtnCount]{};
    Tooltip* tooltip = nullptr;
    int tipIds[kRabBtnCount]{};
    char* tips[kRabBtnCount]{};
    bool followShown = false;
    bool shownPlaying = false;
};

static Vec<RabBar*> gRabBars;

static bool RabTraceOn() {
    static int on = -1;
    if (on < 0) {
        on = GetEnvironmentVariableW(L"SUMATRA_MO_TRACE", nullptr, 0) > 0 ? 1 : 0;
    }
    return on == 1;
}

static const char* kRabIconPrev = "\xEE\xA2\x92";     // U+E892
static const char* kRabIconNext = "\xEE\xA2\x93";     // U+E893
static const char* kRabIconPlay = "\xEE\x9D\xA8";     // U+E768
static const char* kRabIconPause = "\xEE\x9D\xA9";    // U+E769
static const char* kRabIconClose = "\xEE\xA2\xBB";    // U+E8BB
static const char* kRabIconSettings = "\xEE\x9C\x93"; // U+E713

static bool RabIsIconBtn(RabBar* bar, int i) {
    return bar->haveIcons &&
           (i == kRabBtnPrev || i == kRabBtnPlay || i == kRabBtnNext || i == kRabBtnClose || i == kRabBtnSettings);
}

// the source does not offer this button at all (no slot is reserved for it)
static bool RabBtnUnsupported(RabBar* bar, int i) {
    if (i == kRabBtnBack || i == kRabBtnFwd) {
        return !bar->src->CanSeek(bar->win);
    }
    if (i == kRabBtnTime) {
        return !bar->src->HasTime(bar->win);
    }
    return false;
}

static TempStr RabRateLabelTemp(double rate) {
    return str::FormatTemp("%gx", rate);
}

static const char* RabLabel(RabBar* bar, int i) {
    MainWindow* win = bar->win;
    ReadAloudBarSource* src = bar->src;
    switch (i) {
        case kRabBtnPrev:
            return bar->haveIcons ? kRabIconPrev : "|<";
        case kRabBtnBack:
            return "-10s";
        case kRabBtnPlay: {
            bool playing = src->IsPlaying(win);
            if (bar->haveIcons) {
                return playing ? kRabIconPause : kRabIconPlay;
            }
            return playing ? _TRA("Pause") : _TRA("Play");
        }
        case kRabBtnFwd:
            return "+10s";
        case kRabBtnNext:
            return bar->haveIcons ? kRabIconNext : ">|";
        case kRabBtnRate:
            return RabRateLabelTemp(src->Rate(win));
        case kRabBtnTime:
            return src->HasTime(win) ? src->TimeLabelTemp(win) : (TempStr) "";
        case kRabBtnFollow:
            return _TRA("Follow");
        case kRabBtnSettings:
            return bar->haveIcons ? kRabIconSettings : _TRA("Settings");
        case kRabBtnClose:
            return bar->haveIcons ? kRabIconClose : "x";
    }
    return "";
}

static const char* RabTooltip(RabBar* bar, int i) {
    ReadAloudBarSource* src = bar->src;
    switch (i) {
        case kRabBtnPrev:
            return src->PrevTooltip();
        case kRabBtnBack:
            return _TRA("Back 10 seconds");
        case kRabBtnPlay:
            return src->PlayTooltip();
        case kRabBtnFwd:
            return _TRA("Forward 10 seconds");
        case kRabBtnNext:
            return src->NextTooltip();
        case kRabBtnRate:
            return _TRA("Playback speed");
        case kRabBtnFollow:
            return _TRA("Scroll to the text being read");
        case kRabBtnSettings:
            return _TRA("Read Aloud Settings");
        case kRabBtnClose:
            return src->CloseTooltip();
    }
    return nullptr;
}

static void RabEnsureFonts(RabBar* bar) {
    int dpi = DpiGet(bar->hwnd);
    if (bar->font && bar->dpi == dpi) {
        return;
    }
    if (bar->iconFont) {
        DeleteObject(bar->iconFont);
        bar->iconFont = nullptr;
    }
    bar->dpi = dpi;
    bar->font = GetAppFontForDpi(dpi);
    int h = -MulDiv(12, dpi, 72);
    bar->iconFont = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    bar->haveIcons = false;
    if (bar->iconFont) {
        HDC hdc = GetDC(bar->hwnd);
        HGDIOBJ prev = SelectObject(hdc, bar->iconFont);
        WCHAR face[LF_FACESIZE]{};
        GetTextFaceW(hdc, dimof(face), face);
        SelectObject(hdc, prev);
        ReleaseDC(bar->hwnd, hdc);
        // GDI silently substitutes a missing font
        bar->haveIcons = str::EqI(face, L"Segoe MDL2 Assets");
    }
}

static int RabTextDx(RabBar* bar, HDC hdc, int i, const char* s) {
    HFONT f = RabIsIconBtn(bar, i) ? bar->iconFont : bar->font;
    HGDIOBJ prev = SelectObject(hdc, f);
    WCHAR* ws = ToWStrTemp(s);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, ws, str::Leni(ws), &sz);
    SelectObject(hdc, prev);
    return sz.cx;
}

// returns bar size
static Size RabLayout(RabBar* bar, int maxDx) {
    RabEnsureFonts(bar);
    HDC hdc = GetDC(bar->hwnd);
    int pad = DpiScale(bar->hwnd, 6);
    int btnDy = DpiScale(bar->hwnd, 28);
    int gap = DpiScale(bar->hwnd, 2);
    int dxs[kRabBtnCount];
    bool fits[kRabBtnCount];
    int total = 2 * pad - gap;
    for (int i = 0; i < kRabBtnCount; i++) {
        fits[i] = !RabBtnUnsupported(bar, i);
        dxs[i] = 0;
        if (!fits[i]) {
            continue;
        }
        const char* s = RabLabel(bar, i);
        int dx = RabTextDx(bar, hdc, i, s) + DpiScale(bar->hwnd, 16);
        if (i == kRabBtnTime) {
            dx = RabTextDx(bar, hdc, i, "00:00:00 / 00:00:00") + DpiScale(bar->hwnd, 12);
        } else if (i == kRabBtnRate) {
            dx = RabTextDx(bar, hdc, i, "1.75x") + DpiScale(bar->hwnd, 16);
        } else if (i == kRabBtnFollow) {
            // the slot is reserved even when hidden, so buttons don't shift under the pointer
            dx = RabTextDx(bar, hdc, i, _TRA("Follow")) + DpiScale(bar->hwnd, 16);
        }
        dxs[i] = std::max(dx, btnDy);
        total += dxs[i] + gap;
    }
    ReleaseDC(bar->hwnd, hdc);
    // narrow canvas: drop the least essential controls first
    static const int kDropOrder[] = {kRabBtnTime, kRabBtnRate, kRabBtnBack, kRabBtnFwd, kRabBtnSettings};
    for (int k = 0; k < (int)dimof(kDropOrder) && total > maxDx; k++) {
        int i = kDropOrder[k];
        if (fits[i]) {
            fits[i] = false;
            total -= dxs[i] + gap;
        }
    }
    bool showFollow = !bar->src->IsFollowing(bar->win);
    int x = pad;
    for (int i : kRabLayoutOrder) {
        if (!fits[i]) {
            bar->btn[i] = Rect();
            bar->visibleBtn[i] = false;
            continue;
        }
        bar->visibleBtn[i] = i != kRabBtnFollow || showFollow;
        bar->btn[i] = Rect(x, pad, dxs[i], btnDy);
        x += dxs[i] + gap;
    }
    return Size(x - gap + pad, btnDy + 2 * pad);
}

static void RabPaint(RabBar* bar) {
    PAINTSTRUCT ps;
    HDC hdcWin = BeginPaint(bar->hwnd, &ps);
    if (!bar->src) {
        EndPaint(bar->hwnd, &ps);
        return;
    }
    Rect rc = ClientRect(bar->hwnd);
    DoubleBuffer buffer(bar->hwnd, rc);
    HDC hdc = buffer.GetDC();

    COLORREF bg = FloatingPopupBg();
    COLORREF fg = FloatingPopupTextColor();
    COLORREF muted = FloatingPopupMutedTextColor();
    int radius = DpiScale(bar->hwnd, 8);
    RECT fillRc = ToRECT(rc);
    FillRect(hdc, &fillRc, AutoDeleteBrush(CreateSolidBrush(bg)));
    Rect inner = rc;
    inner.dx -= 1;
    inner.dy -= 1;
    StrokeFloatingPopupRoundedRect(hdc, inner, radius, FloatingPopupBorderColor());

    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < kRabBtnCount; i++) {
        if (!bar->visibleBtn[i]) {
            continue;
        }
        Rect r = bar->btn[i];
        bool clickable = i != kRabBtnTime;
        if (clickable && (bar->hot == i || bar->pressed == i)) {
            COLORREF hb = i == kRabBtnClose ? FloatingPopupCloseHoverBg(bg) : FloatingPopupHoverBg(bg);
            FillFloatingPopupRoundedRect(hdc, r, DpiScale(bar->hwnd, 4), hb);
        }
        COLORREF col = i == kRabBtnTime ? muted : fg;
        if (i == kRabBtnFollow) {
            col = FloatingPopupAccentColor();
        }
        SetTextColor(hdc, col);
        HGDIOBJ prev = SelectObject(hdc, RabIsIconBtn(bar, i) ? bar->iconFont : bar->font);
        DrawCenteredText(hdc, r, RabLabel(bar, i));
        SelectObject(hdc, prev);
    }
    buffer.Flush(hdcWin);
    EndPaint(bar->hwnd, &ps);
}

static int RabHitTest(RabBar* bar, int x, int y) {
    for (int i = 0; i < kRabBtnCount; i++) {
        if (bar->visibleBtn[i] && i != kRabBtnTime && bar->btn[i].Contains(Point(x, y))) {
            return i;
        }
    }
    return -1;
}

static void RabClick(RabBar* bar, int i) {
    MainWindow* win = bar->win;
    ReadAloudBarSource* src = bar->src;
    if (!src) {
        return;
    }
    if (RabTraceOn()) {
        logf("rab: click btn=%d src=%s\n", i, src->Name());
    }
    switch (i) {
        case kRabBtnPrev:
            src->Prev(win);
            break;
        case kRabBtnNext:
            src->Next(win);
            break;
        case kRabBtnBack:
            src->Skip(win, -1);
            break;
        case kRabBtnFwd:
            src->Skip(win, 1);
            break;
        case kRabBtnPlay:
            src->TogglePlay(win);
            break;
        case kRabBtnRate:
            src->NextRate(win);
            break;
        case kRabBtnFollow:
            src->Follow(win);
            break;
        case kRabBtnSettings:
            HwndSendCommand(win->hwndFrame, CmdReadAloudSettings);
            return;
        case kRabBtnClose:
            src->Close(win);
            break;
    }
    // the click may have closed the window's bar
    for (RabBar* b : gRabBars) {
        if (b->win == win) {
            ReadAloudBarUpdate(win);
            break;
        }
    }
}

static RabBar* RabFromHwnd(HWND hwnd) {
    for (RabBar* b : gRabBars) {
        if (b->hwnd == hwnd) {
            return b;
        }
    }
    return nullptr;
}

static void RabSetHot(RabBar* bar, int hot) {
    if (bar->hot == hot) {
        return;
    }
    bar->hot = hot;
    InvalidateRect(bar->hwnd, nullptr, FALSE);
}

static void RabUpdateTooltips(RabBar* bar) {
    if (!bar->tooltip) {
        bar->tooltip = new Tooltip();
        Tooltip::CreateArgs args;
        args.parent = bar->hwnd;
        args.font = bar->font;
        bar->tooltip->Create(args);
    }
    for (int i = 0; i < kRabBtnCount; i++) {
        const char* tip = RabTooltip(bar, i);
        if (!tip) {
            continue;
        }
        Rect r = bar->visibleBtn[i] ? bar->btn[i] : Rect();
        if (bar->tipIds[i] <= 0) {
            bar->tipIds[i] = bar->tooltip->Add(tip, r, false);
        } else {
            bar->tooltip->Update(bar->tipIds[i], tip, r, false);
        }
        str::ReplaceWithCopy(&bar->tips[i], tip);
    }
}

static bool RabTooltipsChanged(RabBar* bar) {
    for (int i = 0; i < kRabBtnCount; i++) {
        const char* tip = RabTooltip(bar, i);
        if (tip && !str::Eq(tip, bar->tips[i])) {
            return true;
        }
    }
    return false;
}

static LRESULT CALLBACK RabWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RabBar* bar = RabFromHwnd(hwnd);
    if (!bar) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_PAINT:
            RabPaint(bar);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_MOUSEMOVE: {
            if (!bar->tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                bar->tracking = true;
            }
            RabSetHot(bar, RabHitTest(bar, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
            return 0;
        }
        case WM_MOUSELEAVE:
            bar->tracking = false;
            RabSetHot(bar, -1);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            bar->pressed = RabHitTest(bar, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (bar->pressed >= 0) {
                SetCapture(hwnd);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            int i = bar->pressed;
            bar->pressed = -1;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            if (i >= 0 && i == RabHitTest(bar, GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {
                RabClick(bar, i);
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            // keep the canvas scrollable with the pointer over the bar
            return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kRabClass = L"SUMATRA_PDF_READ_ALOUD_BAR";

static RabBar* RabForWindow(MainWindow* win, bool create) {
    for (RabBar* b : gRabBars) {
        if (b->win == win) {
            return b;
        }
    }
    if (!create) {
        return nullptr;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(wcex);
        wcex.style = CS_DBLCLKS;
        wcex.lpfnWndProc = RabWndProc;
        wcex.hInstance = GetModuleHandleW(nullptr);
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcex.lpszClassName = kRabClass;
        RegisterClassExW(&wcex);
        registered = true;
    }
    auto bar = new RabBar();
    bar->win = win;
    gRabBars.Append(bar);
    DWORD style = WS_CHILD | WS_CLIPSIBLINGS;
    bar->hwnd = CreateWindowExW(0, kRabClass, L"", style, 0, 0, 10, 10, win->hwndCanvas, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    return bar;
}

static void RabDestroy(RabBar* bar) {
    gRabBars.Remove(bar);
    delete bar->tooltip;
    if (bar->hwnd) {
        DestroyWindow(bar->hwnd);
    }
    if (bar->iconFont) {
        DeleteObject(bar->iconFont);
    }
    for (char*& s : bar->labels) {
        str::FreePtr(&s);
    }
    for (char*& s : bar->tips) {
        str::FreePtr(&s);
    }
    delete bar;
}

static ReadAloudBarSource* RabWantedSource(MainWindow* win) {
    if (!win || win->presentation) {
        return nullptr;
    }
    ReadAloudBarSource* mo = MediaOverlayBarSource();
    ReadAloudBarSource* tts = TextToSpeechBarSource();
    bool moWanted = mo && mo->Wanted(win);
    bool ttsWanted = tts && tts->Wanted(win);
    // book audio that is actually playing wins; otherwise a TTS session (playing or
    // paused) keeps the TTS bar so ±10 s does not flash in and out
    if (moWanted && mo->IsPlaying(win)) {
        return mo;
    }
    if (ttsWanted) {
        return tts;
    }
    if (moWanted) {
        return mo;
    }
    return nullptr;
}

void ReadAloudBarUpdate(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    ReadAloudBarSource* src = RabWantedSource(win);
    RabBar* bar = RabForWindow(win, src != nullptr);
    if (!bar || !bar->hwnd) {
        return;
    }
    if (!src) {
        if (IsWindowVisible(bar->hwnd)) {
            ShowWindow(bar->hwnd, SW_HIDE);
            if (RabTraceOn()) {
                logf("rab: bar hidden\n");
            }
        }
        bar->src = nullptr;
        return;
    }
    bool srcChanged = bar->src != src;
    bool changed = srcChanged;
    bar->src = src;
    bool playing = src->IsPlaying(win);
    if (playing != bar->shownPlaying) {
        bar->shownPlaying = playing;
        changed = true;
    }
    for (int i = 0; i < kRabBtnCount; i++) {
        const char* s = RabLabel(bar, i);
        if (!str::Eq(s, bar->labels[i])) {
            str::ReplaceWithCopy(&bar->labels[i], s);
            changed = true;
        }
    }
    Rect crc = ClientRect(win->hwndCanvas);
    int margin = DpiScale(bar->hwnd, 12);
    Size sz = RabLayout(bar, crc.dx - 2 * margin);
    int x = crc.x + (crc.dx - sz.dx) / 2;
    int y = crc.y + crc.dy - sz.dy - margin;
    RECT wr;
    GetWindowRect(bar->hwnd, &wr);
    MapWindowPoints(HWND_DESKTOP, win->hwndCanvas, (POINT*)&wr, 2);
    bool moved = wr.left != x || wr.top != y || wr.right - wr.left != sz.dx || wr.bottom - wr.top != sz.dy;
    bool followShown = bar->visibleBtn[kRabBtnFollow];
    if (moved) {
        SetWindowPos(bar->hwnd, HWND_TOP, x, y, sz.dx, sz.dy, SWP_NOACTIVATE);
        UpdateFloatingPopupWindowRgn(bar->hwnd, DpiScale(bar->hwnd, 8));
    }
    if (moved || srcChanged || followShown != bar->followShown || RabTooltipsChanged(bar)) {
        bar->followShown = followShown;
        RabUpdateTooltips(bar);
        changed = true;
        if (RabTraceOn()) {
            StrBuilder s;
            for (int i = 0; i < kRabBtnCount; i++) {
                Rect r = bar->visibleBtn[i] ? bar->btn[i] : Rect();
                s.AppendFmt(" %d:%d,%d,%d,%d", i, r.x, r.y, r.dx, r.dy);
            }
            logf("mo: bar hwnd=%p src=%s follow=%d btns%s\n", bar->hwnd, src->Name(), (int)followShown, s.Get());
        }
    }
    if (!IsWindowVisible(bar->hwnd)) {
        ShowWindow(bar->hwnd, SW_SHOWNA);
        changed = true;
    }
    if (changed) {
        InvalidateRect(bar->hwnd, nullptr, FALSE);
    }
}

void ReadAloudBarOnWindowClosing(MainWindow* win) {
    if (RabBar* bar = RabForWindow(win, false)) {
        RabDestroy(bar);
    }
}
