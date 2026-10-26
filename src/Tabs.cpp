/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinDynCalls.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "SumatraProperties.h"
#include "Notifications.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "resource.h"
#include "Commands.h"
#include "Menu.h"
#include "TableOfContents.h"
#include "Tabs.h"

void ResetFindUIForTabSwitch(MainWindow* win);
#include "FindBar.h"
#include "SumatraDialogs.h"
#include "FileHistory.h"
#include "Theme.h"
#include "Translations.h"

#include "utils/Log.h"

WindowTab* EnsureHomeTabForWindow(MainWindow* win);

void UpdateTabTitle(WindowTab* tab) {
    if (!tab) {
        return;
    }
    MainWindow* win = tab->win;
    int idx = win->GetTabIdx(tab);
    const char* title = tab->GetTabTitle();
    const char* tooltip = tab->filePath;
    win->tabsCtrl->SetTextAndTooltip(idx, title, tooltip);
}

void UpdateHomeTabTitles() {
    for (MainWindow* win : gWindows) {
        if (!win->tabsCtrl) {
            continue;
        }
        int n = win->TabCount();
        for (int i = 0; i < n; i++) {
            WindowTab* tab = win->GetTab(i);
            if (tab && tab->IsAboutTab()) {
                win->tabsCtrl->SetTextAndTooltip(i, _TRA("Home"), nullptr);
            }
        }
    }
}

static int LogFontHeightPx(HWND hwnd, HFONT hf) {
    LOGFONTW lf{};
    if (!hf || GetObjectW(hf, sizeof(lf), &lf) == 0) {
        return FontDyPx(hwnd, hf);
    }
    int h = lf.lfHeight;
    if (h < 0) {
        h = -h;
    }
    if (h < 1) {
        return FontDyPx(hwnd, hf);
    }
    return h;
}

int GetTabbarHeight(HWND hwnd, float factor) {
    int heightDip = gGlobalPrefs->tabBarHeight > 0 ? gGlobalPrefs->tabBarHeight : kTabBarDy;
    int tabDy = DpiScale(hwnd, heightDip);
    HFONT hfont = GetAppTabFontForHwnd(hwnd);
    int fontDyWithPadding = LogFontHeightPx(hwnd, hfont) + DpiScale(hwnd, 2);
    if (fontDyWithPadding > tabDy) {
        tabDy = fontDyWithPadding;
    }
    return (int)((float)tabDy * factor);
}

static int TabDefaultDxForHwnd(HWND hwnd) {
    int w = gGlobalPrefs->tabWidth;
    if (w < kTabMinDx) {
        w = kTabMinDx;
    }
    return DpiScale(hwnd, w);
}

static void ShowTabBar(MainWindow* win, bool show) {
    if (show == win->tabsVisible) {
        return;
    }
    win->tabsVisible = show;
    win->tabsCtrl->SetIsVisible(show);
    RelayoutWindow(win);
}

void UpdateTabWidth(MainWindow* win) {
    int nTabs = (int)win->TabCount();
    int tabWidth = TabDefaultDxForHwnd(win->hwndFrame);
    if (win->tabsCtrl) {
        win->tabsCtrl->tabDefaultDx = tabWidth;
    }
    if (!SettingsUseTabs()) {
        ShowTabBar(win, false);
        return;
    }
    bool showTabs = (nTabs > 1) || (nTabs > 0);
    if (!showTabs) {
        ShowTabBar(win, false);
        return;
    }
    ShowTabBar(win, true);
}

void RemoveTab(WindowTab* tab) {
    UpdateTabFileDisplayStateForTab(tab);
    MainWindow* win = tab->win;
    win->tabSelectionHistory->Remove(tab);
    int idx = win->GetTabIdx(tab);
    int selectedIdx = win->tabsCtrl ? win->tabsCtrl->GetSelected() : -1;
    // Must decide before tabsCtrl->RemoveTab(): RemoveTab() changes the selected
    // tab (often to index 0), so comparing against CurrentTab() afterwards falsely
    // reports that a non-first tab wasn't the current tab.
    bool closedCurrentTab = (tab == win->CurrentTab()) || (idx >= 0 && idx == selectedIdx);
    WindowTab* tab2 = win->tabsCtrl->RemoveTab<WindowTab*>(idx);
    ReportIf(tab != tab2);
    if (closedCurrentTab) {
        win->ctrl = nullptr;
        win->currentTabTemp = nullptr;
    }
    UpdateTabWidth(win);

    int nTabs = win->TabCount();
    if (nTabs < 1) {
        return;
    }
    if (!closedCurrentTab) {
        return;
    }
    // if the removed tab was the current one, select another
#if 0
    WindowTab* curr = win->CurrentTab();
    WindowTab* newCurrent = curr;
    if (!curr || newCurrent == tab) {
        // TODO(tabs): why do I need win->tabSelectionHistory.Size() > 0
        if (win->tabSelectionHistory->Size() > 0) {
            newCurrent = win->tabSelectionHistory->Pop();
        } else {
            newCurrent = win->GetTab(0);
        }
    }
    int newIdx = win->GetTabIdx(newCurrent);
    win->tabsCtrl->SetSelected(newIdx);
    tab = win->CurrentTab();
    LoadModelIntoTab(tab);
#else
    // select tab to the right or to the left if nothing to the right
    int newIdx = idx;
    int lastIdx = nTabs - 1;
    if (newIdx > lastIdx) {
        newIdx = lastIdx;
    }
    win->tabsCtrl->SetSelected(newIdx);
    tab = win->CurrentTab();
    // seen in crash report that tab was WindowTab::Type::None
    // TODO: don't know how it could have happened
    if (tab && tab->type != WindowTab::Type::None) {
        LoadModelIntoTab(tab);
    }
#endif
}

static void CloseWindowIfNoDocuments(MainWindow* win) {
    for (auto& tab : win->Tabs()) {
        if (!tab->IsAboutTab()) {
            return;
        }
    }
    if (SettingsUseTabs() && !gGlobalPrefs->noHomeTab) {
        WindowTab* home = EnsureHomeTabForWindow(win);
        if (home) {
            TabsSelect(win, win->GetTabIdx(home));
        }
        return;
    }
    CloseWindow(win, true, false);
}

static MainWindow* MaybeMigrateTab(WindowTab* tab, MainWindow* newWin, const Point* position = nullptr) {
    MainWindow* oldWin = tab->win;

    // don't migrate if it's only one document tab and not
    // dragging over a window
    int nDocTabs = 0;
    for (auto& t : oldWin->Tabs()) {
        if (t->IsAboutTab()) continue;
        nDocTabs++;
    }
    if (nDocTabs == 1 && !newWin) return position ? oldWin : nullptr;

    auto engine = tab->GetEngine();
    if (EngineHasUnsavedAnnotations(engine)) {
        return nullptr;
    }

    if (!newWin) {
        newWin = CreateAndShowMainWindow(nullptr, false);
        if (!newWin) {
            return nullptr;
        }
    }

    if (position) {
        WINDOWPLACEMENT placement = {};
        placement.length = sizeof(placement);
        GetWindowPlacement(oldWin->hwndFrame, &placement);
        RECT rc = placement.rcNormalPosition;
        SetWindowPos(newWin->hwndFrame, nullptr, position->x - 80, position->y - 16, rc.right - rc.left,
                     rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RemoveTab(tab);

    // TODO: we should be able to just slide the existing tab
    // into the new window, preserving its controller and
    // the entire state, but this crashes/renders badly etc.
    // More work needed. The code that should work but doesn't:
    // tab->win = newWin;
    // newWin->currentTabTemp = AddTabToWindow(newWin, tab);
    // newWin->ctrl = tab->ctrl;
    // UpdateUiForCurrentTab(newWin);
    // newWin->showSelection = tab->selectionOnPage != nullptr;
    // HwndSetFocus(newWin->hwndFrame);
    // newWin->RedrawAll(true);
    // TabsOnChangedDoc(newWin);
    WindowTab* newTab = new WindowTab(newWin);
    newTab->SetFilePath(tab->filePath);
    newWin->currentTabTemp = AddTabToWindow(newWin, newTab);
    newWin->ctrl = nullptr;
    LoadArgs args(tab->filePath, newWin);
    args.forceReuse = true;
    args.noSavePrefs = true;
    args.noPlaceWindow = position != nullptr;
    args.showWin = position == nullptr;
    LoadDocument(&args);
    delete tab;

    CloseWindowIfNoDocuments(oldWin);
    // A detached window stays hidden until its real tab grab point is known.
    return newWin;
}

// Selects the given tab (0-based index)
// tabIndex can come from settings file so must be sanitized
void TabsSelect(MainWindow* win, int tabIndex) {
    auto tabs = win->Tabs();
    int nTabs = tabs.Size();
    logf("TabsSelect: tabIndex: %d, nTabs: %d\n", tabIndex, nTabs);
    if (nTabs == 0) {
        logf("TabsSelect: skipping because nTabs = %d\n", nTabs);
        return;
    }
    if (tabIndex < 0 || tabIndex >= nTabs) {
        tabIndex = 0;
        logf("TabsSelect: fixing tabIndex to 0\n");
    }
    TabsCtrl* tabsCtrl = win->tabsCtrl;
    int currIdx = tabsCtrl->GetSelected();
    if (tabIndex == currIdx) {
        WindowTab* tab = tabs[tabIndex];
        if (tab && !tab->IsAboutTab() && !tab->ctrl) {
            LoadModelIntoTab(tab);
        }
        return;
    }

    bool isShowingPageInfo = (GetNotificationForGroup(win->hwndCanvas, kNotifPageInfo) != nullptr);

    // same work as in onSelectionChanging and onSelectionChanged
    SaveCurrentWindowTab(win);
    ResetFindUIForTabSwitch(win);
    int prevIdx = tabsCtrl->SetSelected(tabIndex);
    if (prevIdx < 0) {
        return;
    }
    WindowTab* tab = tabs[tabIndex];
    LoadModelIntoTab(tab);
    if (isShowingPageInfo) {
        PostMessageW(win->hwndFrame, WM_COMMAND, CmdTogglePageInfo, 0);
    }
}

// clang-format off
static MenuDef menuDefContextTab[] = {
    {
        _TRN("Properties..."),
        CmdProperties,
    },
    {
        _TRN("Show in folder"),
        CmdShowInFolder,
    },
    {
        _TRN("Copy File Path"),
        CmdCopyFilePath,
    },
    {
        _TRN("Open In New Window"),
        CmdDuplicateInNewWindow,
    },
    {
        _TRN("Set Tab Color"),
        CmdSetTabColor,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Close"),
        CmdClose,
    },
    {
        _TRN("Close Other Tabs"),
        CmdCloseOtherTabs,
    },
    {
        _TRN("Close Tabs To The Right"),
        CmdCloseTabsToTheRight,
    },
    {
        _TRN("Close Tabs To The Left"),
        CmdCloseTabsToTheLeft,
    },
    {
        _TRN("Close All Tabs"),
        CmdCloseAllTabs,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Save Tab Group"),
        CmdTabGroupSave,
    },
    {
        _TRN("Restore Tab Group"),
        CmdTabGroupRestore,
    },
    {
        nullptr,
        0,
    },
};
// clang-format on

void CollectTabsToClose(MainWindow* win, WindowTab* currTab, Vec<WindowTab*>& toCloseOther,
                        Vec<WindowTab*>& toCloseRight, Vec<WindowTab*>& toCloseLeft) {
    int nTabs = win->TabCount();
    bool seenCurrent = false;
    for (int i = 0; i < nTabs; i++) {
        WindowTab* tab = win->Tabs()[i];
        if (tab->IsAboutTab()) {
            continue;
        }
        if (currTab == tab) {
            seenCurrent = true;
            continue;
        }
        toCloseOther.Append(tab);
        if (seenCurrent) {
            toCloseRight.Append(tab);
        } else {
            toCloseLeft.Append(tab);
        }
    }
}

void CloseAllTabs(MainWindow* win) {
    // can't close while iterating over the tabs so collect them first
    Vec<WindowTab*> toClose;
    int nTabs = win->TabCount();
    for (int i = 0; i < nTabs; i++) {
        WindowTab* t = win->GetTab(i);
        if (t->IsAboutTab()) {
            continue;
        }
        toClose.Append(t);
    }
    for (WindowTab* t : toClose) {
        CloseTab(t, false);
    }
}

// TODO: add "Move to another window" sub-menu
static void TabsContextMenu(ContextMenuEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    TabsCtrl* tabsCtrl = (TabsCtrl*)ev->w;
    TabsCtrl::MouseState tabState = tabsCtrl->TabStateFromMousePosition(ev->mouseWindow);
    int tabIdx = tabState.tabIdx;
    if (tabIdx < 0) {
        return;
    }

    int nTabs = tabsCtrl->TabCount();
    WindowTab* tabUnderMouse = win->Tabs()[tabIdx];
    if (tabUnderMouse->IsAboutTab()) {
        return;
    }
    POINT pt = ToPOINT(ev->mouseScreen);
    auto menuCtx = NewBuildMenuCtx(tabUnderMouse, Point{0, 0});
    AutoRun freeMenuCtx(DeleteBuildMenuCtx, menuCtx);
    HMENU popup = BuildMenuFromDef(menuDefContextTab, CreatePopupMenu(), menuCtx);

    Vec<WindowTab*> toCloseOther;
    Vec<WindowTab*> toCloseRight;
    Vec<WindowTab*> toCloseLeft;
    CollectTabsToClose(win, tabUnderMouse, toCloseOther, toCloseRight, toCloseLeft);

    if (!tabUnderMouse->ctrl) {
        MenuSetEnabled(popup, CmdSetTabColor, false);
    }
    if (toCloseOther.IsEmpty()) {
        MenuSetEnabled(popup, CmdCloseOtherTabs, false);
    }
    if (toCloseRight.IsEmpty()) {
        MenuSetEnabled(popup, CmdCloseTabsToTheRight, false);
    }
    if (toCloseLeft.IsEmpty()) {
        MenuSetEnabled(popup, CmdCloseTabsToTheLeft, false);
    }
    if (!HasOpenedDocuments(win)) {
        MenuSetEnabled(popup, CmdTabGroupSave, false);
    }
    MarkMenuOwnerDraw(popup);
    uint flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    int cmdId = TrackPopupMenu(popup, flags, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);
    switch (cmdId) {
        case CmdClose: {
            CloseTab(tabUnderMouse, false);
            return;
        }

        case CmdCloseAllTabs: {
            CloseAllTabs(win);
            return;
        }
        case CmdCloseOtherTabs: {
            for (WindowTab* t : toCloseOther) {
                CloseTab(t, false);
            }
            return;
        }
        case CmdCloseTabsToTheRight: {
            for (WindowTab* t : toCloseRight) {
                CloseTab(t, false);
            }
            return;
        }
        case CmdCloseTabsToTheLeft: {
            for (WindowTab* t : toCloseLeft) {
                CloseTab(t, false);
            }
            return;
        }
        case CmdShowInFolder: {
            SumatraOpenPathInDefaultFileManager(tabUnderMouse->filePath);
            return;
        }
        case CmdCopyFilePath: {
            CopyFilePath(tabUnderMouse);
            return;
        }
        case CmdDuplicateInNewWindow: {
            DuplicateTabInNewWindow(tabUnderMouse);
            return;
        }
        case CmdProperties: {
            ShowProperties(win->hwndFrame, tabUnderMouse->ctrl);
            return;
        }
        case CmdSetTabColor: {
            COLORREF curColor = tabUnderMouse->tabColor;
            bool isUnset = (curColor == kColorUnset);
            if (isUnset) {
                curColor = ThemeControlBackgroundColor();
            }
            COLORREF newColor;
            bool newIsUnset;
            if (!Dialog_SetTabColor(win->hwndFrame, curColor, isUnset, newColor, newIsUnset)) {
                return;
            }
            tabUnderMouse->tabColor = newIsUnset ? kColorUnset : newColor;
            // update TabInfo
            TabInfo* ti = tabsCtrl->GetTab(tabIdx);
            if (ti) {
                ti->tabColor = tabUnderMouse->tabColor;
            }
            // persist to FileState
            FileState* fs = gFileHistory.FindByPath(tabUnderMouse->filePath);
            if (fs) {
                if (newIsUnset) {
                    str::ReplaceWithCopy(&fs->tabCol, "");
                } else {
                    TempStr colorStr = SerializeColorTemp(newColor);
                    str::ReplaceWithCopy(&fs->tabCol, colorStr);
                }
                fs->tabColParsed.wasParsed = false;
            }
            SaveSettings();
            tabsCtrl->ScheduleRepaint();
            return;
        }
    }
    // everything we forward to main window
    HwndSendCommand(win->hwndFrame, cmdId);
}

static void MainWindowTabClosed(MainWindow* win, TabsCtrl::ClosedEvent* ev) {
    int closedTabIdx = ev->tabIdx;
    WindowTab* tab = win->GetTab(closedTabIdx);
    CloseTab(tab, false);
}

static void MainWindowTabSelectionChanging(MainWindow* win, TabsCtrl::SelectionChangingEvent* ev) {
    // TODO: Should we allow the switch of the tab if we are in process of printing?
    SaveCurrentWindowTab(win);
    // Tear down find UI and abort background search immediately when the user
    // starts switching tabs (ctrl still refers to the tab being left).
    ResetFindUIForTabSwitch(win);
    ev->preventChanging = false;
}

static void MainWindowTabSelectionChanged(MainWindow* win, TabsCtrl::SelectionChangedEvent* ev) {
    bool isShowingPageInfo = (GetNotificationForGroup(win->hwndCanvas, kNotifPageInfo) != nullptr);
    int currentIdx = win->tabsCtrl->GetSelected();
    WindowTab* tab = win->Tabs()[currentIdx];
    LoadModelIntoTab(tab);
    if (isShowingPageInfo) {
        PostMessageW(win->hwndFrame, WM_COMMAND, CmdTogglePageInfo, 0);
    }
}

struct TabDragPosition {
    MainWindow* win;
    HWND frame;
    TabDragPosition(MainWindow* window, Point offset, int dpi) : win(window), frame(window->hwndFrame) {
        win->tabDragGrabOffset = offset;
        win->tabDragSourceDpi = dpi > 0 ? dpi : 96;
        win->tabDragActive = true;
        win->tabDragAnchorValid = false;
    }
    ~TabDragPosition() {
        // The native move loop dispatches messages and may destroy the frame.
        MainWindow* current = FindMainWindowByHwnd(frame);
        if (current) current->tabDragActive = false;
    }
};

bool TabsAdjustDragWindowRect(MainWindow* win, RECT* rect) {
    if (!win || !win->tabDragActive || !win->tabsCtrl || !rect) return false;
    HWND frame = win->hwndFrame;
    TabInfo* selected = win->tabsCtrl->GetTab(win->tabsCtrl->GetSelected());
    if (!selected || selected->r.dx <= 0 || selected->r.dy <= 0) return false;
    if (!win->tabDragAnchorValid || win->tabDragAnchorTabRect != selected->r) {
        RECT rc;
        if (!GetWindowRect(frame, &rc)) return false;
        int dpi = DpiGetForHwnd(frame);
        int grabX = MulDiv(win->tabDragGrabOffset.x, dpi, win->tabDragSourceDpi);
        int grabY = MulDiv(win->tabDragGrabOffset.y, dpi, win->tabDragSourceDpi);
        if (grabX < 0) grabX = 0;
        if (grabY < 0) grabY = 0;
        if (grabX >= selected->r.dx) grabX = selected->r.dx - 1;
        if (grabY >= selected->r.dy) grabY = selected->r.dy - 1;
        POINT anchor = {selected->r.x + grabX, selected->r.y + grabY};
        if (HwndIsRtl(win->tabsCtrl->hwnd)) {
            anchor.x = ClientRect(win->tabsCtrl->hwnd).dx - 1 - anchor.x;
        }
        if (!ClientToScreen(win->tabsCtrl->hwnd, &anchor)) return false;
        win->tabDragFrameOffset = Point(anchor.x - rc.left, anchor.y - rc.top);
        win->tabDragAnchorTabRect = selected->r;
        win->tabDragAnchorValid = true;
    }
    POINT cursor;
    if (!GetCursorPos(&cursor)) return false;
    // WM_MOVING supplies a proposed screen rectangle. Preserve its dimensions
    // but replace the native loop's stale offset with the actual tab geometry.
    OffsetRect(rect, cursor.x - win->tabDragFrameOffset.x - rect->left,
               cursor.y - win->tabDragFrameOffset.y - rect->top);
    return true;
}

void TabsUpdateDragPosition(MainWindow* win) {
    if (!win || !win->tabDragActive || win->tabDragPositioning) return;
    win->tabDragPositioning = true;
    // Moving to another monitor can synchronously change the DPI and lay out
    // the tab strip again. Re-measure instead of keeping the pre-move offset.
    for (int i = 0; i < 3; i++) {
        win->tabDragAnchorValid = false;
        RECT rc;
        GetWindowRect(win->hwndFrame, &rc);
        RECT target = rc;
        if (!TabsAdjustDragWindowRect(win, &target) || EqualRect(&rc, &target)) break;
        SetWindowPos(win->hwndFrame, nullptr, target.left, target.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    win->tabDragPositioning = false;
}

static void PositionTabDragWindow(TabDragPosition* data) {
    TabsUpdateDragPosition(data->win);
}

// Limit this to the detached frame and this gesture. DWM's show/restore zoom
// animation otherwise fights the mouse as it takes over moving the window.
struct TabDragTransitions {
    HWND frame;
    bool disabled = false;
    explicit TabDragTransitions(HWND hwnd) : frame(hwnd) {
        BOOL disable = TRUE;
        disabled = DynDwmSetWindowAttribute && SUCCEEDED(DynDwmSetWindowAttribute(
                                                   frame, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable)));
    }
    ~TabDragTransitions() {
        if (disabled && IsWindow(frame)) {
            BOOL disable = FALSE;
            DynDwmSetWindowAttribute(frame, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
        }
    }
};

static void MainWindowTabMigration(MainWindow* win, TabsCtrl::MigrationEvent* ev) {
    WindowTab* tab = win->GetTab(ev->tabIdx);
    if (ev->moveWindow) {
        MainWindow* moving = MaybeMigrateTab(tab, nullptr, &ev->releasePoint);
        if (!moving) {
            return;
        }
        HWND frame = moving->hwndFrame;
        TabDragTransitions transitions(frame);
        TabDragPosition position{moving, ev->grabOffset, ev->sourceDpi};
        Func0 prepare = MkFunc0(PositionTabDragWindow, &position);
        bool newWindow = moving != win;
        bool restoreWindow = IsZoomed(frame);
        bool previousSuppressFrameRedraw = moving->suppressFrameRedraw;
        if (restoreWindow) {
            moving->suppressFrameRedraw = true;
            SendMessageW(frame, WM_SETREDRAW, FALSE, 0);
            ShowWindow(frame, SW_RESTORE);
        }
        if (newWindow) {
            ShowMainWindow(moving, SW_NORMAL, &prepare);
        } else {
            PositionTabDragWindow(&position);
            if (restoreWindow) {
                moving->suppressFrameRedraw = previousSuppressFrameRedraw;
                SendMessageW(frame, WM_SETREDRAW, TRUE, 0);
            }
            RedrawWindow(frame, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
        }
        POINT cursor;
        // Showing the frame can dispatch deferred DPI and caption layouts
        // after its before-first-paint callback. Measure their final geometry.
        TabsUpdateDragPosition(moving);
        UpdateWindow(frame);
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
            // The native move loop takes its grab point from lParam. Passing
            // (0, 0) offsets the frame from the mouse, especially on monitors
            // with negative screen coordinates.
            GetCursorPos(&cursor);
            SendMessageW(frame, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
        }
        // The native move loop pumps messages; revalidate before using the window.
        moving = FindMainWindowByHwnd(frame);
        if (!moving) {
            return;
        }
        GetCursorPos(&cursor);
        MainWindow* target = nullptr;
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (h == frame || !IsWindowVisible(h) || IsIconic(h)) {
                continue;
            }
            RECT bounds;
            GetWindowRect(h, &bounds);
            if (!PtInRect(&bounds, cursor)) {
                continue;
            }
            MainWindow* candidate = FindMainWindowByHwnd(h);
            if (candidate && candidate->tabsCtrl) {
                GetWindowRect(candidate->tabsCtrl->hwnd, &bounds);
                if (PtInRect(&bounds, cursor)) {
                    target = candidate;
                }
            }
            break;
        }
        if (target) {
            MaybeMigrateTab(moving->CurrentTab(), target);
        }
        return;
    }
    MainWindow* releaseWnd = nullptr;
    POINT p;
    p.x = ev->releasePoint.x;
    p.y = ev->releasePoint.y;
    HWND hwnd = WindowFromPoint(p);
    if (hwnd != nullptr) {
        releaseWnd = FindMainWindowByHwnd(hwnd);
    }
    if (releaseWnd == win) {
        // don't re-add to the same window
        releaseWnd = nullptr;
    }
    MainWindow* detached = MaybeMigrateTab(tab, releaseWnd, releaseWnd ? nullptr : &ev->releasePoint);
    if (!releaseWnd && detached && detached != win) {
        TabDragTransitions transitions(detached->hwndFrame);
        TabDragPosition position{detached, ev->grabOffset, ev->sourceDpi};
        Func0 prepare = MkFunc0(PositionTabDragWindow, &position);
        ShowMainWindow(detached, SW_NORMAL, &prepare);
    }
}

void CreateTabbar(MainWindow* win) {
    TabsCtrl::CreateArgs args;
    args.parent = win->hwndFrame;
    args.withToolTips = true;
    args.font = GetAppTabFontForHwnd(win->hwndFrame);
    args.tabDefaultDx = TabDefaultDxForHwnd(win->hwndFrame);
    args.isRtl = IsUIRtl();

    TabsCtrl* tabsCtrl = new TabsCtrl();
    tabsCtrl->onTabClosed = MkFunc1(MainWindowTabClosed, win);
    tabsCtrl->onSelectionChanging = MkFunc1(MainWindowTabSelectionChanging, win);
    tabsCtrl->onSelectionChanged = MkFunc1(MainWindowTabSelectionChanged, win);
    tabsCtrl->onContextMenu = MkFunc1Void(TabsContextMenu);
    tabsCtrl->onTabMigration = MkFunc1(MainWindowTabMigration, win);
    tabsCtrl->Create(args);
    win->tabsCtrl = tabsCtrl;
    win->tabSelectionHistory = new Vec<WindowTab*>();
}

static void SyncTabFromWindow(MainWindow* win, WindowTab* tab) {
    if (!win || !tab) {
        return;
    }
    win->UpdateCanvasSize();
    tab->canvasRc = win->canvasRc;
    if (PM_DISABLED == win->presentation) {
        tab->showToc = win->tocVisible;
    } else if (PM_ENABLED == win->presentation) {
        tab->showTocPresentation = win->tocVisible;
    }
}

static bool ShouldSkipVerifyWindowTab(MainWindow* win, WindowTab* tab) {
    if (!win || !tab) {
        return true;
    }
    if (win->ctrl == tab->ctrl) {
        return false;
    }
    // Transient during CloseDocumentInCurrentTab / async load handoff.
    return !win->ctrl || !tab->ctrl;
}

// verifies that WindowTab state is consistent with MainWindow state
static NO_INLINE void VerifyWindowTab(MainWindow* win, WindowTab* tdata) {
    if (ShouldSkipVerifyWindowTab(win, tdata)) {
        return;
    }
    ReportIf(tdata->ctrl != win->ctrl);
#if 0
    // disabling this check. best I can tell, external apps can change window
    // title and trigger this
    auto winTitle = win::GetTextTemp(win->hwndFrame);
    if (!str::Eq(winTitle.Get(), tdata->frameTitle.Get())) {
        logf(L"VerifyWindowTab: winTitle: '%s', tdata->frameTitle: '%s'\n", winTitle.Get(), tdata->frameTitle.Get());
        ReportIf(!str::Eq(winTitle.Get(), tdata->frameTitle));
    }
#endif
    bool expectedTocVisibility = tdata->showToc; // if not in presentation mode
    if (PM_DISABLED != win->presentation) {
        expectedTocVisibility = false; // PM_BLACK_SCREEN, PM_WHITE_SCREEN
        if (PM_ENABLED == win->presentation) {
            expectedTocVisibility = tdata->showTocPresentation;
        }
    }
    ReportDebugIf(win->tocVisible != expectedTocVisibility);
    ReportIf(tdata->canvasRc != win->canvasRc);
}

// Must be called when the active tab is losing selection.
// This happens when a new document is loaded or when another tab is selected.
void SaveCurrentWindowTab(MainWindow* win) {
    if (!win) {
        return;
    }
    if (!win->tabsCtrl) {
        return;
    }
    // tab switch teardown is done in LoadModelIntoTab via ResetFindUIForTabSwitch

    int current = win->tabsCtrl->GetSelected();
    if (-1 == current) {
        return;
    }
    if (win->CurrentTab() != win->Tabs().at(current)) {
        return; // TODO: restore ReportIf() ?
    }

    WindowTab* tab = win->CurrentTab();
    if (win->tocLoaded && tab->ctrl) {
        TocTree* tocTree = tab->ctrl->GetToc();
        UpdateTocExpansionState(tab->tocState, win->tocTreeView, tocTree);
    }
    // Live window state is authoritative; persist it on the tab before verify/switch.
    SyncTabFromWindow(win, tab);
    VerifyWindowTab(win, tab);

    // Capture the reading position while this tab still owns the live canvas.
    // A background controller can later be relaid out without its viewport and
    // report page 1 during shutdown, so SessionData must use this snapshot.
    if (tab->ctrl && tab->filePath) {
        FileState* fs = NewFileState(tab->filePath);
        tab->ctrl->GetDisplayState(fs);
        logf("SESSIONTRACE SaveCurrentWindowTab file='%s' page=%d scroll=%.1f,%.1f zoom='%s'\n", tab->filePath,
             fs->pageNo, fs->scrollPos.x, fs->scrollPos.y, fs->zoom);
        fs->showToc = tab->showToc;
        *fs->tocState = tab->tocState;
        TabState* state = NewTabState(fs);
        DeleteFileState(fs);
        FreeTabState(tab->tabState);
        tab->tabState = state;
    }

    // update the selection history
    win->tabSelectionHistory->Remove(tab);
    win->tabSelectionHistory->Append(tab);
}

WindowTab* EnsureHomeTabForWindow(MainWindow* win) {
    if (!win || !win->tabsCtrl || !SettingsUseTabs() || gGlobalPrefs->noHomeTab) {
        return nullptr;
    }
    for (auto& tab : win->Tabs()) {
        if (tab->IsAboutTab()) {
            return tab;
        }
    }
    WindowTab* homeTab = new WindowTab(win);
    homeTab->type = WindowTab::Type::About;
    homeTab->canvasRc = win->canvasRc;
    TabInfo* newTab = new TabInfo();
    newTab->text = str::Dup(_TRA("Home"));
    newTab->tooltip = nullptr;
    newTab->isPinned = true;
    newTab->canClose = false;
    newTab->userData = (UINT_PTR)homeTab;
    int insertedIdx = win->tabsCtrl->InsertTab(0, newTab);
    ReportIf(insertedIdx != 0);
    UpdateTabWidth(win);
    return homeTab;
}

WindowTab* AddTabToWindow(MainWindow* win, WindowTab* tab) {
    ReportIf(!win);
    if (!win) {
        return nullptr;
    }
    if (!win->tabsCtrl) {
        return nullptr;
    }

    auto tabs = win->tabsCtrl;
    int idx = win->TabCount();
    bool useTabs = SettingsUseTabs();
    bool noHomeTab = gGlobalPrefs->noHomeTab;
    bool createHomeTab = useTabs && !noHomeTab && (idx == 0);
    if (createHomeTab) {
        WindowTab* homeTab = new WindowTab(win);
        homeTab->type = WindowTab::Type::About;
        homeTab->canvasRc = win->canvasRc;
        TabInfo* newTab = new TabInfo();
        newTab->text = str::Dup(_TRA("Home"));
        newTab->tooltip = nullptr;
        newTab->isPinned = true;
        newTab->canClose = false;
        newTab->userData = (UINT_PTR)homeTab;
        int insertedIdx = tabs->InsertTab(idx, newTab);
        ReportIf(insertedIdx != 0);
        idx++;
    }

    tab->canvasRc = win->canvasRc;
    TabInfo* newTab = new TabInfo();
    newTab->text = str::Dup(tab->GetTabTitle());
    newTab->tooltip = str::Dup(tab->filePath);
    newTab->userData = (UINT_PTR)tab;
    newTab->tabColor = tab->tabColor;

    int insertedIdx = tabs->InsertTab(idx, newTab);
    ReportIf(insertedIdx == -1);
    tabs->SetSelected(insertedIdx);
    UpdateTabWidth(win);
    return tab;
}

// Refresh the tab's title
void TabsOnChangedDoc(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    ReportIf(!tab != !win->TabCount());
    if (!tab) {
        return;
    }

    int tabIdx = win->GetTabIdx(tab);
    int selectedIdx = win->tabsCtrl->GetSelected();
    if (tabIdx != selectedIdx) {
        logf("TabsonChangeDoc: tabIdx (%d) != selectedIdx (%d)\n", tabIdx, selectedIdx);
    }
    SyncTabFromWindow(win, tab);
    VerifyWindowTab(win, tab);
    UpdateTabTitle(tab);
}

// Called when we're closing an entire window (quitting)
void TabsOnCloseWindow(MainWindow* win) {
    // TODO: I've seen a crash here where it seems like we've deleted the only main window
    // but somehow we still process Esc and we get here. this might not be enough
    if (!win->tabsCtrl) {
        return;
    }
    auto tabs = win->Tabs();
    DeleteVecMembers(tabs);
    win->tabsCtrl->RemoveAllTabs();
    win->tabSelectionHistory->Reset();
    win->currentTabTemp = nullptr;
    win->ctrl = nullptr;
}

void SetTabsInTitlebar(MainWindow* win, bool inTitleBar) {
    if (inTitleBar == win->tabsInTitlebar) {
        return;
    }
    win->tabsInTitlebar = inTitleBar;
    win->tabsCtrl->inTitleBar = inTitleBar;
    if (inTitleBar) {
        SyncMenuBarForTabsInTitlebar(win);
    } else {
        SyncMainWindowForNoTabsInTitlebar(win);
    }
    uint flags = SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOSIZE | SWP_NOMOVE;
    SetWindowPos(win->hwndFrame, nullptr, 0, 0, 0, 0, flags);
}

// Selects the next (or previous) tab.
void TabsOnCtrlTab(MainWindow* win, bool reverse) {
    if (!win) {
        return;
    }
    int count = win->TabCount();
    if (count < 2) {
        return;
    }
    int idx = win->tabsCtrl->GetSelected() + 1;
    if (reverse) {
        idx -= 2;
    }
    idx += count; // ensure > 0
    idx = idx % count;
    TabsSelect(win, idx);
}

void MoveTab(MainWindow* win, int dir) {
    if (!win) {
        return;
    }
    int nTabs = win->TabCount();
    int idx = win->tabsCtrl->GetSelected();
    int newIdx = idx + dir;
    if (newIdx < 0) {
        return;
    }
    if (newIdx >= nTabs) {
        return;
    }
    win->tabsCtrl->SwapTabs(idx, newIdx);
    win->tabsCtrl->SetSelected(newIdx);
    win->tabsCtrl->LayoutTabs();
}
