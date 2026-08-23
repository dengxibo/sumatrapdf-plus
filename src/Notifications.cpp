/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/UITask.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "Annotation.h"
#include "SumatraPdf.h"
#include "AppTools.h"

#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "Theme.h"
#include "MainWindow.h"

#include "utils/Log.h"

struct MainWindow;
MainWindow* FindMainWindowByHwnd(HWND hwnd);
void UpdateTocFilterForDocumentLoading(MainWindow* win);
void RelayoutTocContainer(MainWindow* win);

using Gdiplus::Graphics;
using Gdiplus::Pen;
using Gdiplus::SolidBrush;

// defined in MainWindow.cpp
HWND GetHwndForNotification();

struct StrNode {
    StrNode* next;
    const char* s;
};

static StrNode* gDelayedNotifications = nullptr;

Kind kNotifCursorPos = "cursorPosHelper";
Kind kNotifActionResponse = "responseToAction";
Kind kNotifPageInfo = "pageInfoHelper";
// can have multiple of those
Kind kNotifAdHoc = "notifAdHoc";
Kind kNotifThemeRelayout = "themeRelayout";
Kind kNotifEbookFontLayout = "ebookFontLayout";
Kind kNotifDocumentLoading = "documentLoading";

constexpr int kPadding = 6;
constexpr int kTopLeftMargin = 8;
constexpr int kLoadingTopLeftMargin = 4;

constexpr UINT_PTR kNotifTimerTimeoutId = 1;
constexpr UINT_PTR kNotifTimerDelayId = 2;

struct NotificationWnd : Wnd {
    NotificationWnd() = default;
    ~NotificationWnd() override;

    HWND Create(const NotificationCreateArgs&);

    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    void OnTimer(UINT_PTR event_id) override;
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;

    void UpdateMessage(const char* msg, int timeoutMs = 0, bool highlight = false);

    bool HasProgress() const { return progressPerc >= 0; }
    void Layout(const char* message);

    int timeoutMs = kNotifDefaultTimeOut; // 0 means no timeout

    bool highlight = false; // TODO: should really be a color
    bool noClose = false;

    NotificationWndRemoved wndRemovedCb;

    // there can only be a single notification of a given group
    Kind groupId = nullptr;

    // to reduce flicker, we might ask the window to shrink the size less often
    // (notifcation windows are only shrunken if by less than factor shrinkLimit)
    float shrinkLimit = 1.0f;

    int progressPerc = -1;
    int delayInMs = 0;
    UINT_PTR delayTimerId = 0;

    Rect rTxt;
    Rect rClose;
    Rect rProgress;
};

constexpr int kMaxNotifs = 128;
static NotificationWnd* gNotifs[kMaxNotifs];
static int gNotifsCount = 0;

static int GetForHwnd(HWND hwnd, NotificationWnd* wnds[kMaxNotifs]) {
    int n = 0;
    for (int i = 0; i < gNotifsCount; i++) {
        auto* wnd = gNotifs[i];
        HWND parent = HwndGetParent(wnd->hwnd);
        if (parent == hwnd) {
            wnds[n++] = wnd;
        }
    }
    return n;
}

static int NotificationIndexOf(NotificationWnd* wnd) {
    for (int i = 0; i < gNotifsCount; i++) {
        if (gNotifs[i] == wnd) {
            return i;
        }
    }
    return -1;
}

static int GetForSameHwnd(NotificationWnd* wnd, NotificationWnd* wnds[kMaxNotifs]) {
    HWND parent = GetParent(wnd->hwnd);
    return GetForHwnd(parent, wnds);
}

static bool IsDocumentLoadingGroup(Kind groupId) {
    return groupId == kNotifDocumentLoading || groupId == kNotifEbookFontLayout;
}

NotificationWnd* GetDocumentLoadingNotification(HWND hwndFrame, HWND hwndCanvas) {
    NotificationWnd* nw = GetNotificationForGroup(hwndCanvas, kNotifDocumentLoading);
    if (nw) {
        return nw;
    }
    nw = GetNotificationForGroup(hwndFrame, kNotifDocumentLoading);
    if (nw) {
        return nw;
    }
    nw = GetNotificationForGroup(hwndCanvas, kNotifEbookFontLayout);
    if (nw) {
        return nw;
    }
    return GetNotificationForGroup(hwndFrame, kNotifEbookFontLayout);
}

HWND GetDocumentLoadingNotificationHwnd(HWND hwndFrame, HWND hwndCanvas) {
    NotificationWnd* nw = GetDocumentLoadingNotification(hwndFrame, hwndCanvas);
    return nw && nw->hwnd ? nw->hwnd : nullptr;
}

void RaiseDocumentLoadingNotification(HWND hwndFrame, HWND hwndCanvas) {
    NotificationWnd* nw = GetDocumentLoadingNotification(hwndFrame, hwndCanvas);
    if (!nw || !nw->hwnd) {
        return;
    }
    MainWindow* win = hwndCanvas ? FindMainWindowByHwnd(hwndCanvas) : nullptr;
    if (!win && hwndFrame) {
        win = FindMainWindowByHwnd(hwndFrame);
    }
    const char* msg = HwndGetTextTemp(nw->hwnd);
    nw->Layout(msg);
    if (win && win->tocVisible && win->hwndTocBox && IsWindowVisible(win->hwndTocBox)) {
        if (GetParent(nw->hwnd) != win->hwndTocBox) {
            SetParent(nw->hwnd, win->hwndTocBox);
        }
        RelayoutTocContainer(win);
    } else if (hwndCanvas) {
        if (GetParent(nw->hwnd) != hwndCanvas) {
            SetParent(nw->hwnd, hwndCanvas);
        }
        BringWindowToTop(nw->hwnd);
        RelayoutNotifications(hwndCanvas);
    }
}

void RelayoutNotifications(HWND hwnd) {
    NotificationWnd* wnds[kMaxNotifs];
    HWND parent = HwndGetParent(hwnd);
    int nWnds = GetForHwnd(parent, wnds);
    if (nWnds == 0) {
        return;
    }

    auto* first = wnds[0];
    HWND hwndCanvas = GetParent(first->hwnd);
    Rect frame = ClientRect(hwndCanvas);
    bool compact = IsDocumentLoadingGroup(first->groupId);
    int topLeftMargin = DpiScale(hwndCanvas, compact ? kLoadingTopLeftMargin : kTopLeftMargin);
    int dyPadding = DpiScale(hwndCanvas, kPadding);
    int y = topLeftMargin;
    for (int i = 0; i < nWnds; i++) {
        NotificationWnd* wnd = wnds[i];
        if (wnd->delayTimerId != 0) {
            // still in delay period, not yet visible
            continue;
        }
        if (GetParent(wnd->hwnd) != hwndCanvas) {
            continue;
        }
        Rect rect = WindowRect(wnd->hwnd);
        rect = MapRectToWindow(rect, HWND_DESKTOP, hwndCanvas);
        if (IsUIRtl()) {
            int cxVScroll = GetSystemMetrics(SM_CXVSCROLL);
            rect.x = frame.dx - rect.dx - topLeftMargin - cxVScroll;
        } else {
            rect.x = topLeftMargin;
        }
        uint flags = SWP_NOSIZE | SWP_NOZORDER;
        SetWindowPos(wnd->hwnd, nullptr, rect.x, y, 0, 0, flags);
        y += rect.dy + dyPadding;
    }
}

static void NotifsRemoveNotification(NotificationWnd* wnd) {
    int pos = NotificationIndexOf(wnd);
    if (pos < 0) {
        return;
    }
    gNotifsCount--;
    if (pos < gNotifsCount) {
        gNotifs[pos] = gNotifs[gNotifsCount];
    }
    gNotifs[gNotifsCount] = nullptr;
    if (IsDocumentLoadingGroup(wnd->groupId)) {
        HWND parent = GetParent(wnd->hwnd);
        MainWindow* win = FindMainWindowByHwnd(parent);
        if (!win && parent) {
            win = FindMainWindowByHwnd(GetAncestor(parent, GA_ROOT));
        }
        if (win) {
            UpdateTocFilterForDocumentLoading(win);
        }
    }
    RelayoutNotifications(wnd->hwnd);
    delete wnd;
}

int GetWndX(NotificationWnd* wnd) {
    Rect rect = WindowRect(wnd->hwnd);
    rect = MapRectToWindow(rect, HWND_DESKTOP, GetParent(wnd->hwnd));
    return rect.x;
}

NotificationWnd::~NotificationWnd() {
    if (delayTimerId != 0) {
        KillTimer(hwnd, delayTimerId);
        delayTimerId = 0;
    }
}

HWND NotificationWnd::Create(const NotificationCreateArgs& args) {
    highlight = args.warning;
    noClose = args.noClose;
    ReportIf(noClose && args.timeoutMs <= 0);
    shrinkLimit = args.shrinkLimit;
    if (shrinkLimit < 0.2f) {
        ReportIf(shrinkLimit < 0.2f);
        shrinkLimit = 1.f;
    }
    if (args.onRemoved.IsValid()) {
        wndRemovedCb = args.onRemoved;
    } else {
        wndRemovedCb = MkFunc1Void(NotifsRemoveNotification);
    }
    timeoutMs = args.timeoutMs;
    groupId = args.groupId;

    CreateCustomArgs cargs;
    cargs.parent = args.hwndParent;
    cargs.font = args.font;
    // TODO: was this important?
    // wcex.hCursor = LoadCursor(nullptr, IDC_APPSTARTING);
    cargs.exStyle = WS_EX_TOPMOST;
    cargs.style = WS_CHILD | SS_CENTER;
    cargs.title = args.msg;
    if (cargs.font == nullptr) {
        // Match chrome UI font (tabs/toolbar). BiggerFont made banners look oversized
        // vs the rest of the window; prefer parent hwnd DPI on cold start.
        cargs.font = args.hwndParent ? GetAppFontForHwnd(args.hwndParent) : GetAppFont();
    }
    cargs.pos = Rect(0, 0, 0, 0);
    cargs.visible = args.delayInMs == 0;
    cargs.isRtl = IsUIRtl();

    CreateCustom(cargs);
    if (!hwnd) {
        return nullptr;
    }

    Layout(args.msg);
    delayInMs = args.delayInMs;
    if (delayInMs > 0) {
        // create hidden, will show after delay
        delayTimerId = SetTimer(hwnd, kNotifTimerDelayId, delayInMs, nullptr);
    } else {
        ShowWindow(hwnd, SW_SHOW);
        if (timeoutMs != 0) {
            SetTimer(hwnd, kNotifTimerTimeoutId, timeoutMs, nullptr);
        }
    }
    return hwnd;
}

// returns 0% - 100%
int CalcPerc(int current, int total) {
    ReportIf(total <= 0 || current < 0);
    ReportIf(total < current);
    if (total <= 0) {
        total = 1;
    }
    int perc = limitValue(100 * current / total, 0, 100);
    return perc;
}

constexpr int kCloseLeftMargin = 16;
constexpr int kProgressDy = 5;

void NotificationWnd::Layout(const char* message) {
    bool compact = IsDocumentLoadingGroup(groupId);
    Size szText;
    {
        HDC hdc = GetDC(hwnd);
        uint fmt = DT_SINGLELINE | DT_NOPREFIX;
        szText = HdcMeasureText(hdc, message, fmt, font);
        ReleaseDC(hwnd, hdc);
    }

    int padX = DpiScale(hwnd, compact ? 8 : 12);
    int padY = DpiScale(hwnd, compact ? 3 : 8);
    int dx = padX + szText.dx + padX;
    int dy = padY + szText.dy + padY;
    rTxt = {padX, padY, szText.dx, szText.dy};
    if (!noClose) {
        int closeDx = DpiScale(hwnd, 16);
        int leftMargin = DpiScale(hwnd, kCloseLeftMargin - padX);
        rClose = {dx + leftMargin, padY, closeDx, closeDx + 2};
        // close button
        dx += leftMargin + closeDx + padX;
    } else {
        rClose = {};
        dx += padX;
    }
    int progressDy = DpiScale(hwnd, kProgressDy);
    rProgress = {padX, dy, szText.dx, progressDy};
    if (HasProgress()) {
        dy += padY + progressDy + padY;
    }

    Rect rCurr = WindowRect(hwnd);
    // for less flicker we don't want to shrink the window when the text shrinks
    if (dx < rCurr.dx) {
        int diff = rCurr.dx - dx;
        rClose.x += diff;
        dx = rCurr.dx;
    }
#if 0
    if (dy < rCurr.dy) {
        dy = rCurr.dy;
    }
#endif
#if 0
    if (wnd->shrinkLimit < 1.0f) {
        Rect rcOrig = ClientRect(wnd->hwnd);
        if (rMsg.dx < rcOrig.dx && rMsg.dx > rcOrig.dx * wnd->shrinkLimit) {
            rMsg.dx = rcOrig.dx;
        }
    }
#endif

    // y-center close
    if (!noClose) {
        int closeDx = rClose.dx;
        rClose.y = ((dy - closeDx) / 2) + 1;
    }

    if (dx == rCurr.dx && dy == rCurr.dy) {
        return;
    }

    // adjust the window to fit the message (only shrink the window when there's no progress bar)
    uint flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(hwnd, nullptr, 0, 0, dx, dy, flags);

    // move the window to the right for a right-to-left layout
    if (IsUIRtl()) {
        HWND parent = GetParent(hwnd);
        Rect r = MapRectToWindow(WindowRect(hwnd), HWND_DESKTOP, parent);
        int cxVScroll = GetSystemMetrics(SM_CXVSCROLL);
        r.x = WindowRect(parent).dx - r.dx - DpiScale(hwnd, kTopLeftMargin) - cxVScroll;
        flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOACTIVATE | SWP_DEFERERASE;
        SetWindowPos(hwnd, nullptr, r.x, r.y, 0, 0, flags);
    }
}

// TODO: figure out why it flickers
void NotificationWnd::OnPaint(HDC hdcIn, PAINTSTRUCT* ps) {
    Rect rc = ClientRect(hwnd);
    DoubleBuffer buffer(hwnd, rc);
    HDC hdc = buffer.GetDC();
    // HDC hdc = hdcIn;

    COLORREF colBg = ThemeNotificationsBackgroundColor();
    COLORREF colBorder = MkGray(0xdd);
    COLORREF colTxt = ThemeNotificationsTextColor();
    if (highlight) {
        colBg = ThemeNotificationsHighlightColor();
        colBorder = colBg;
        colTxt = ThemeNotificationsHighlightTextColor();
    }

    // Pure GDI fill (avoid Gdiplus::Graphics leaving a large system font on the HDC).
    // Re-resolve UI font every paint — cached HFONT can be deleted by InvalidateUiFonts()
    // during DPI/chrome refresh while a loading banner is still up.
    {
        AutoDeleteBrush brBg(CreateSolidBrush(colBg));
        RECT rFill = ToRECT(rc);
        FillRect(hdc, &rFill, brBg);
    }

    HFONT paintFont = GetAppFontForHwnd(hwnd);
    font = paintFont;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, colTxt);
    char* text = HwndGetTextTemp(hwnd);
    uint format = DT_SINGLELINE | DT_NOPREFIX;
    RECT rTmp = ToRECT(rTxt);
    HdcDrawText(hdc, text, &rTmp, format, paintFont);

    if (!noClose) {
        Point curPos = HwndGetCursorPos(hwnd);
        DrawCloseButtonArgs args;
        args.hdc = hdc;
        args.r = rClose;
        args.isHover = rClose.Contains(curPos);
        DrawCloseButton(args);
    }

    if (HasProgress()) {
        Rect rp = rProgress;
        int progressWidth = rp.dx;
        COLORREF col = ThemeNotificationsProgressColor();
        Graphics graphics(hdc);
        Pen pen(GdiRgbFromCOLORREF(col));
        auto grc = Gdiplus::Rect(rp.x, rp.y, rp.dx, rp.dy);
        graphics.DrawRectangle(&pen, grc);

        rp.x += 2;
        rp.dx = (progressWidth - 3) * progressPerc / 100;
        rp.y += 2;
        rp.dy -= 3;

        SolidBrush br(GdiRgbFromCOLORREF(col));
        grc = {rp.x, rp.y, rp.dx, rp.dy};
        graphics.FillRectangle(&br, grc);
    }

    buffer.Flush(hdcIn);
}

void NotificationWnd::UpdateMessage(const char* msg, int timeoutMs, bool highlight) {
    HwndSetText(hwnd, msg);
    this->highlight = highlight;
    this->timeoutMs = timeoutMs;
    HwndSetRtl(hwnd, IsUIRtl());
    Layout(msg);
    HwndRepaintNow(hwnd);
    if (timeoutMs != 0) {
        SetTimer(hwnd, kNotifTimerTimeoutId, timeoutMs, nullptr);
    } else {
        KillTimer(hwnd, kNotifTimerTimeoutId);
    }
}

bool UpdateNotificationProgress(NotificationWnd* wnd, const char* msg, int perc) {
    if (NotificationIndexOf(wnd) < 0) {
        return false;
    }
    ReportIf(perc < 0 || perc > 100);
    wnd->progressPerc = perc;
    wnd->UpdateMessage(msg);
    HwndRepaintNow(wnd->hwnd);
    return true;
}

static void NotifRemove(NotificationWnd* wnd) {
    wnd->wndRemovedCb.Call(wnd);
}

static void NotifDelete(NotificationWnd* wnd) {
    delete wnd;
}

void NotificationWnd::OnTimer(UINT_PTR timerId) {
    if (timerId == kNotifTimerDelayId) {
        // delay elapsed, now show the notification
        KillTimer(hwnd, delayTimerId);
        delayTimerId = 0;
        BringWindowToTop(hwnd);
        ShowWindow(hwnd, SW_SHOW);
        RelayoutNotifications(hwnd);
        if (timeoutMs != 0) {
            SetTimer(hwnd, kNotifTimerTimeoutId, timeoutMs, nullptr);
        }
        return;
    }
    ReportIf(kNotifTimerTimeoutId != timerId);
    // TODO a better way to delete myself
    if (wndRemovedCb.IsValid()) {
        auto fn = MkFunc0<NotificationWnd>(NotifRemove, this);
        uitask::Post(fn, "TaskNotifOnTimerRemove");
    } else {
        auto fn = MkFunc0<NotificationWnd>(NotifDelete, this);
        uitask::Post(fn, "TaskNotifOnTimerDelete");
    }
}

LRESULT NotificationWnd::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (WM_SETCURSOR == msg && !noClose) {
        Point pt = HwndGetCursorPos(hwnd);
        if (!pt.IsEmpty() && rClose.Contains(pt)) {
            SetCursorCached(IDC_HAND);
            return TRUE;
        }
    }

    if (WM_ERASEBKGND == msg) {
        // avoid flicker by telling we took care of erasing background
        return TRUE;
    }

    if (WM_MOUSEMOVE == msg) {
        HwndScheduleRepaint(hwnd);

        if (!noClose && IsMouseOverRect(hwnd, rClose)) {
            TrackMouseLeave(hwnd);
        }
        goto DoDefault;
    }

    if (WM_MOUSELEAVE == msg) {
        HwndScheduleRepaint(hwnd);
        return 0;
    }

    if (WM_LBUTTONUP) {
        Point pt = Point(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (!noClose && rClose.Contains(pt)) {
            // TODO a better way to delete myself
            if (wndRemovedCb.IsValid()) {
                auto fn = MkFunc0<NotificationWnd>(NotifRemove, this);
                uitask::Post(fn, "TaskNotifWndProcRemove");
            } else {
                auto fn = MkFunc0<NotificationWnd>(NotifDelete, this);
                uitask::Post(fn, "TaskNotifWndProcDelete");
            }
            return 0;
        }
    }

DoDefault:
    return WndProcDefault(hwnd, msg, wp, lp);
}

static int NotifsRemoveForGroup(NotificationWnd** wnds, int nWnds, Kind groupId) {
    ReportIf(groupId == nullptr);
    NotificationWnd* toRemove[kMaxNotifs];
    int nRemove = 0;
    for (int i = 0; i < nWnds; i++) {
        if (wnds[i]->groupId == groupId) {
            toRemove[nRemove++] = wnds[i];
        }
    }
    for (int i = 0; i < nRemove; i++) {
        NotifsRemoveNotification(toRemove[i]);
    }
    return nRemove;
}

static bool NotifsAdd(NotificationWnd** wnds, int nWnds, NotificationWnd* wnd, Kind groupId) {
    bool skipRemove = (groupId == nullptr) || (groupId == kNotifAdHoc);
    if (!skipRemove) {
        NotifsRemoveForGroup(wnds, nWnds, groupId);
    }
    if (gNotifsCount >= kMaxNotifs) {
        return false;
    }
    wnd->groupId = groupId;
    gNotifs[gNotifsCount] = wnd;
    gNotifsCount++;
    RelayoutNotifications(wnd->hwnd);
    return true;
}

static bool NotifsAdd(NotificationWnd* wnd, Kind groupId) {
    NotificationWnd* wnds[kMaxNotifs];
    int nWnds = GetForSameHwnd(wnd, wnds);
    return NotifsAdd(wnds, nWnds, wnd, groupId);
}

NotificationWnd* NotifsGetForGroup(NotificationWnd** wnds, int nWnds, Kind groupId) {
    ReportIf(!groupId);
    for (int i = 0; i < nWnds; i++) {
        if (wnds[i]->groupId == groupId) {
            return wnds[i];
        }
    }
    return nullptr;
}

NotificationWnd* ShowNotification(const NotificationCreateArgs& args) {
    ReportIf(!args.hwndParent);

    NotificationWnd* wnd = new NotificationWnd();
    wnd->Create(args);
    if (!wnd->hwnd) {
        delete wnd;
        return nullptr;
    }
    if (wnd->delayTimerId == 0 && !IsDocumentLoadingGroup(args.groupId)) {
        BringWindowToTop(wnd->hwnd);
    }
    bool ok = NotifsAdd(wnd, args.groupId);
    if (!ok) {
        delete wnd;
        return nullptr;
    }
    return wnd;
}

// show a temporary notification that will go away after a timeout
NotificationWnd* ShowTemporaryNotification(HWND hwnd, const char* msg, int timeoutMs) {
    if (timeoutMs <= 0) {
        timeoutMs = kNotifDefaultTimeOut;
    }
    NotificationCreateArgs args;
    args.hwndParent = hwnd;
    args.msg = msg;
    args.timeoutMs = timeoutMs;
    return ShowNotification(args);
}

NotificationWnd* ShowWarningNotification(HWND hwndParent, const char* msg, int timeoutMs) {
    if (timeoutMs < 0) {
        timeoutMs = kNotifDefaultTimeOut;
    }
    NotificationCreateArgs args;
    args.hwndParent = hwndParent;
    args.msg = msg;
    args.warning = true;
    args.timeoutMs = timeoutMs;
    return ShowNotification(args);
}

void NotificationUpdateMessage(NotificationWnd* wnd, const char* msg, int timeoutMs, bool highlight) {
    if (!wnd) {
        return;
    }
    wnd->UpdateMessage(msg, timeoutMs, highlight);
}

void RemoveNotification(NotificationWnd* wnd) {
    NotifsRemoveNotification(wnd);
}

bool RemoveNotificationsForGroup(HWND hwnd, Kind kind) {
    NotificationWnd* wnds[kMaxNotifs];
    int nWnds = GetForHwnd(hwnd, wnds);
    int n = NotifsRemoveForGroup(wnds, nWnds, kind);
    return n > 0;
}

NotificationWnd* GetNotificationForGroup(HWND hwnd, Kind kind) {
    NotificationWnd* wnds[kMaxNotifs];
    int nWnds = GetForHwnd(hwnd, wnds);
    return NotifsGetForGroup(wnds, nWnds, kind);
}

HWND GetNotificationParentHwnd(NotificationWnd* wnd) {
    if (!wnd || !wnd->hwnd) {
        return nullptr;
    }
    return GetParent(wnd->hwnd);
}

static StrNode* AllocStrNode(const char* s) {
    size_t n = str::Len(s) + 1;
    size_t cbAlloc = sizeof(StrNode) + n;
    auto* node = (StrNode*)malloc(cbAlloc);
    char* dst = (char*)node + sizeof(StrNode);
    memcpy(dst, s, n);
    node->next = nullptr;
    node->s = dst;
    return node;
}

void MaybeDelayedWarningNotification(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char* msg = str::FmtV(fmt, args);
    va_end(args);

    log(msg);

    HWND hwnd = GetHwndForNotification();
    if (hwnd) {
        ShowWarningNotification(hwnd, msg, kNotifNoTimeout);
    } else {
        StrNode* node = AllocStrNode(msg);
        ListInsertFront(&gDelayedNotifications, node);
    }
    str::Free(msg);
}

void ShowMaybeDelayedNotifications(HWND hwndParent) {
    // reverse so notifications show in the order they were added
    ListReverse(&gDelayedNotifications);
    StrNode* curr = gDelayedNotifications;
    while (curr) {
        ShowWarningNotification(hwndParent, curr->s, kNotifNoTimeout);
        curr = curr->next;
    }
    ListDelete(gDelayedNotifications);
    gDelayedNotifications = nullptr;
}
