/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/Timer.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "wingui/Layout.h"
#include "wingui/FrameRateWnd.h"

#include "Settings.h"
#include "DocController.h"
#include "GlobalPrefs.h"
#include "SumatraConfig.h"
#include "FileHistory.h"
#include "FileThumbnails.h"
#include "Annotation.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "resource.h"
#include "Commands.h"
#include "Canvas.h"
#include "Menu.h"
#include "HomePage.h"
#include "Translations.h"
#include "Theme.h"
#include "AppSettings.h"

#include "utils/Log.h"

static void OnPaintAbout(MainWindow* win) {
    auto t = TimeGet();
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(win->hwndCanvas, &ps);
    if (!win->buffer) {
        EndPaint(win->hwndCanvas, &ps);
        return;
    }
    HDC bufDC = win->buffer->GetDC();
    GlobalPrefs* prefs = gGlobalPrefs;
    bool hasPerms = HasPermission(Perm::SavePreferences | Perm::DiskAccess);
    bool drawHome = hasPerms && prefs->rememberOpenedFiles && prefs->showStartPage;
    if (drawHome) {
        DrawHomePage(win, bufDC);
    } else {
        HomePageDestroySearch(win);
        DrawAboutPage(win, bufDC);
    }
    win->buffer->Flush(hdc);

    EndPaint(win->hwndCanvas, &ps);
    if (gShowFrameRate) {
        win->frameRateWnd->ShowFrameRateDur(TimeSinceInMs(t));
    }
}

static void OnMouseLeftButtonDownAbout(MainWindow* win, int x, int y, WPARAM) {
    // lf("Left button clicked on %d %d", x, y);

    // remember a link under so that on mouse up we only activate
    // link if mouse up is on the same link as mouse down
    win->urlOnLastButtonDown.SetCopy(GetStaticLinkAtTemp(win->staticLinks, x, y, nullptr));
}

static bool IsLink(const char* url) {
    if (str::StartsWithI(url, "http:")) {
        return true;
    }
    if (str::StartsWithI(url, "https:")) {
        return true;
    }
    if (str::StartsWithI(url, "mailto:")) {
        return true;
    }
    return false;
}

static void RemoveHomePageFile(MainWindow* win, const char* path) {
    FileState* fs = gFileHistory.FindByPath(path);
    if (!fs) {
        return;
    }
    TempStr filePath = str::DupTemp(fs->filePath);
    if (!fs->favorites->IsEmpty()) {
        gFileHistory.MarkFileInexistent(fs->filePath, true);
    } else {
        gFileHistory.Remove(fs);
        DeleteFileState(fs);
    }
    DeleteThumbnailForFile(filePath);
    SaveSettings();
    win->DeleteToolTip();
    HomePageInvalidateScrollCache(win);
    win->RedrawAll(true);
}

static void ToggleHomePageFilePin(MainWindow* win, const char* path) {
    FileState* fs = gFileHistory.FindByPath(path);
    if (!fs) {
        return;
    }
    fs->isPinned = !fs->isPinned;
    SaveSettings();
    win->DeleteToolTip();
    HomePageInvalidateScrollCache(win);
    win->RedrawAll(true);
}

static void ResetHomePageScroll(MainWindow* win) {
    win->homePageScrollY = 0;
    win->homePageScrollTargetY = 0;
    HomePageInvalidateScrollCache(win);
    SaveSettings();
    win->RedrawAll(true);
}

static void OnMouseLeftButtonUpAbout(MainWindow* win, int x, int y, WPARAM) {
    char* url = GetStaticLinkAtTemp(win->staticLinks, x, y, nullptr);
    char* prevUrl = win->urlOnLastButtonDown;
    bool clickedURL = url && str::Eq(url, prevUrl);
    win->urlOnLastButtonDown.Set(nullptr);
    if (!clickedURL) {
        return;
    }
    if (str::Eq(url, kLinkOpenFile)) {
        HwndSendCommand(win->hwndFrame, CmdOpenFile);
    } else if (str::Eq(url, kLinkHideList)) {
        gGlobalPrefs->showStartPage = false;
        win->RedrawAll(true);
    } else if (str::Eq(url, kLinkShowList)) {
        gGlobalPrefs->showStartPage = true;
        win->RedrawAll(true);
    } else if (str::Eq(url, kLinkHomePageToggleView)) {
        bool listView = gGlobalPrefs && str::EqI(gGlobalPrefs->homePageViewMode, "list");
        str::ReplaceWithCopy(&gGlobalPrefs->homePageViewMode, listView ? "thumbnails" : "list");
        ResetHomePageScroll(win);
    } else if (str::Eq(url, kLinkHomePageToggleSort)) {
        gGlobalPrefs->homePageSortByFrequentlyRead = !gGlobalPrefs->homePageSortByFrequentlyRead;
        ResetHomePageScroll(win);
    } else if (str::StartsWith(url, kLinkHomePageRemoveFile)) {
        RemoveHomePageFile(win, url + str::Len(kLinkHomePageRemoveFile));
    } else if (str::StartsWith(url, kLinkHomePagePinFile)) {
        ToggleHomePageFilePin(win, url + str::Len(kLinkHomePagePinFile));
    } else if (str::Eq(url, kLinkNextTip)) {
        PickAnotherRandomPromotion();
        win->RedrawAll(true);
    } else if (str::StartsWith(url, "Cmd")) {
        int cmdId = GetCommandIdByName(url);
        if (cmdId > 0) {
            HwndSendCommand(win->hwndFrame, cmdId);
        }
    } else if (IsLink(url)) {
        SumatraLaunchBrowser(url);
    } else {
        // assume it's a thumbnail of a document
        auto path = url;
        ReportIf(!path);
        LoadArgs args(path, win);
        SetUserOpenActivateExisting(args);
        StartLoadDocument(&args);
    }
    // HwndSetFocus(win->hwndFrame);
}

static void OnMouseRightButtonDownAbout(MainWindow* win, int x, int y, WPARAM) {
    // lf("Right button clicked on %d %d", x, y);
    HwndSetFocus(win->hwndFrame);
    win->dragStart = Point(x, y);
}

static void OnMouseRightButtonUpAbout(MainWindow* win, int x, int y, WPARAM) {
    int isDrag = IsDragDistance(x, win->dragStart.x, y, win->dragStart.y);
    if (isDrag) {
        return;
    }
    OnAboutContextMenu(win, x, y);
}

static LRESULT OnSetCursorAbout(MainWindow* win, HWND hwnd) {
    Point pt = HwndGetCursorPos(hwnd);
    if (!pt.IsEmpty()) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        HomePageUpdateHover(win, pt.x, pt.y);
        StaticLink* link;
        if (GetStaticLinkAtTemp(win->staticLinks, pt.x, pt.y, &link)) {
            win->ShowToolTip(link->tooltip, link->rect);
            SetCursorCached(IDC_HAND);
        } else {
            win->DeleteToolTip();
            SetCursorCached(IDC_ARROW);
        }
        return TRUE;
    }

    HomePageUpdateHover(win, -1, -1);
    win->DeleteToolTip();
    return FALSE;
}

LRESULT WndProcCanvasAbout(MainWindow* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int x = GET_X_LPARAM(lp);
    int y = GET_Y_LPARAM(lp);
    switch (msg) {
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == win->hwndHomeSearch) {
                HDC hdcEdit = (HDC)wp;
                SetTextColor(hdcEdit, ThemeWindowTextColor());
                SetBkColor(hdcEdit, ThemeControlBackgroundColor());
                if (!win->brControlBgColor) {
                    win->brControlBgColor = CreateSolidBrush(ThemeControlBackgroundColor());
                }
                return (LRESULT)win->brControlBgColor;
            }
            break;

        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE && (HWND)lp == win->hwndHomeSearch) {
                HomePageScheduleSearchFilter(win);
                return 0;
            }
            break;

        case WM_LBUTTONDOWN:
            OnMouseLeftButtonDownAbout(win, x, y, wp);
            return 0;

        case WM_LBUTTONUP:
            OnMouseLeftButtonUpAbout(win, x, y, wp);
            return 0;

        case WM_LBUTTONDBLCLK:
            OnMouseLeftButtonDownAbout(win, x, y, wp);
            return 0;

        case WM_RBUTTONDOWN:
            OnMouseRightButtonDownAbout(win, x, y, wp);
            return 0;

        case WM_RBUTTONUP:
            OnMouseRightButtonUpAbout(win, x, y, wp);
            return 0;

        case WM_SETCURSOR:
            if (OnSetCursorAbout(win, hwnd)) {
                return TRUE;
            }
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_MOUSELEAVE:
            HomePageUpdateHover(win, -1, -1);
            win->DeleteToolTip();
            return 0;

        case WM_CONTEXTMENU: {
            int cx = GET_X_LPARAM(lp);
            int cy = GET_Y_LPARAM(lp);
            if (cx == -1 && cy == -1) {
                Point pt = HwndGetCursorPos(hwnd);
                cx = pt.x;
                cy = pt.y;
            } else {
                POINT p{cx, cy};
                ScreenToClient(hwnd, &p);
                cx = p.x;
                cy = p.y;
            }
            OnAboutContextMenu(win, cx, cy);
            return 0;
        }

        case WM_PAINT:
            if (gRedrawLog) {
                logf("redraw: WM_PAINT hwnd=0x%p (canvas-about)\n", hwnd);
            }
            OnPaintAbout(win);
            return 0;

        case WM_VSCROLL:
            HomePageOnVScroll(win, wp);
            return 0;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            HomePageOnMouseWheel(win, delta);
            return 0;
        }

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
