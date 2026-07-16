/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
}

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinDynCalls.h"
#include "utils/Dpi.h"
#include "utils/WinUtil.h"
#include "utils/BitManip.h"

#include "wingui/UIModels.h"

#include "Accelerators.h"
#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "FzImgReader.h"
#include "GlobalPrefs.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "resource.h"
#include "Commands.h"
#include "AppTools.h"
#include "Menu.h"
#include "SearchAndDDE.h"
#include "Toolbar.h"
#include "FindBar.h"
#include "TextToSpeech.h"

#include "Translations.h"
#include "SvgIcons.h"
#include "SumatraConfig.h"
#include "Theme.h"
#include "PdfDarkMode.h"
#include "DarkModeSubclass.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "utils/Log.h"

// https://docs.microsoft.com/en-us/windows/win32/controls/toolbar-control-reference

static int kButtonSpacingX = 4;

constexpr int kTextPaddingRight = 6;
// padding after the find separator line to "Find:"
constexpr int kFindLabelPadAfterSep = 8;

struct ToolbarButtonInfo {
    /* index in the toolbar bitmap (-1 for separators) */
    TbIcon bmpIndex;
    int cmdId;
    const char* toolTip;
};

// thos are not real commands but we have to refer to toolbar buttons
// is by a command. those are just background for area to be
// covered by other HWNDs. They need the right size
constexpr int PageInfoId = (int)CmdLast + 16;
constexpr int WarningMsgId = (int)CmdLast + 17;

static ToolbarButtonInfo gToolbarButtons[] = {
    {TbIcon::Open, CmdOpenFile, _TRN("Open")},
    {TbIcon::Print, CmdPrint, _TRN("Print")},
    {TbIcon::None, 0, nullptr},          // separator
    {TbIcon::None, PageInfoId, nullptr}, // text box for page number + show current page / no of pages
    {TbIcon::PagePrev, CmdGoToPrevPage, _TRN("Previous Page")},
    {TbIcon::PageNext, CmdGoToNextPage, _TRN("Next Page")},
    {TbIcon::None, 0, nullptr}, // separator
    {TbIcon::NavigateBack, CmdNavigateBack, _TRN("Navigate Back")},
    {TbIcon::NavigateForward, CmdNavigateForward, _TRN("Navigate Forward")},
    {TbIcon::None, 0, nullptr}, // separator
    {TbIcon::Bookmark, CmdToggleBookmarks, _TRN("Show &Bookmarks")},
    {TbIcon::LayoutContinuous, CmdZoomFitWidthAndContinuous, _TRN("Fit Width and Show Pages Continuously")},
    {TbIcon::LayoutSinglePage, CmdZoomFitPageAndSinglePage, _TRN("Fit a Single Page")},
    {TbIcon::RotateLeft, CmdRotateLeft, _TRN("Rotate &Left")},
    {TbIcon::RotateRight, CmdRotateRight, _TRN("Rotate &Right")},
    {TbIcon::ZoomOut, CmdZoomOut, _TRN("Zoom Out")},
    {TbIcon::ZoomIn, CmdZoomIn, _TRN("Zoom In")},
    {TbIcon::Search, CmdFindFirst, _TRN("Find")},
    {TbIcon::Dictionary, CmdToggleDoubleClickWordLookup, _TRN("Toggle Double-Click Word Lookup")},
    {TbIcon::ThemeMoon, CmdToggleLightDarkTheme, _TRN("Toggle &Light/Dark Theme")},
    {TbIcon::DocColorAuto, CmdSetPdfDocumentColorModeAuto,
     _TRN("Document Color Mode: Smart (adapt colors intelligently)")},
    {TbIcon::DocColorOriginal, CmdSetPdfDocumentColorModeLight,
     _TRN("Document Color Mode: Original (document colors unchanged)")},
    {TbIcon::DocColorFollowTheme, CmdSetPdfDocumentColorModeBlack,
     _TRN("Document Color Mode: Match theme (follow current theme colors)")},
    {TbIcon::Speak, CmdReadAloud, _TRN("Read Aloud")},
};
// unicode chars: https://www.compart.com/en/unicode/U+25BC

constexpr int kButtonsCount = dimof(gToolbarButtons);

// 128 should be more than enough
// we use static array so that we don't have to generate
// code for Vec<ToolbarButtonInfo>
constexpr int kMaxCustomButtons = 127;
// +1 to ensure there's always space for WarningsMsgId button
static ToolbarButtonInfo gCustomButtons[kMaxCustomButtons + 1];
int gCustomButtonsCount = 0;

static bool SkipBuiltInButton(const ToolbarButtonInfo& tbi) {
    return tbi.bmpIndex == TbIcon::None;
}

static void UpdateToolbarButtonStateByIdx(HWND hwnd, int idx, bool set, BYTE flag) {
    TBBUTTONINFOW bi{};
    bi.cbSize = sizeof(bi);
    bi.dwMask = TBIF_BYINDEX | TBIF_STATE;
    SendMessageW(hwnd, TB_GETBUTTONINFOW, idx, (LPARAM)&bi);
    bi.fsState = set ? bi.fsState | flag : bi.fsState & ~flag;
    SendMessageW(hwnd, TB_SETBUTTONINFOW, idx, (LPARAM)&bi);
}

static void SetToolbarButtonImageByIdx(HWND hwnd, int idx, TbIcon icon) {
    TBBUTTONINFOW bi{};
    bi.cbSize = sizeof(bi);
    bi.dwMask = TBIF_BYINDEX | TBIF_IMAGE;
    bi.iImage = (int)icon;
    SendMessageW(hwnd, TB_SETBUTTONINFOW, idx, (LPARAM)&bi);
}

static int TotalButtonsCount() {
    return kButtonsCount + gCustomButtonsCount;
}

static ToolbarButtonInfo& GetToolbarButtonInfoByIdx(int idx) {
    if (idx < kButtonsCount) return gToolbarButtons[idx];
    return gCustomButtons[idx - kButtonsCount];
}

// more than one because users can add custom buttons with overlapping ids
static int GetToolbarButtonsByID(int cmdId, int (&buttons)[4]) {
    int nFound = 0;
    int n = TotalButtonsCount();
    for (int idx = 0; idx < n; idx++) {
        ToolbarButtonInfo& tb = GetToolbarButtonInfoByIdx(idx);
        int tbCmdId = tb.cmdId;
        auto cmd = FindCustomCommand(tbCmdId);
        if (cmd) tbCmdId = cmd->origId;
        cmd = FindCustomCommand(cmdId);
        if (cmd) cmdId = cmd->origId;
        if (cmdId != tbCmdId) continue;
        buttons[nFound++] = idx;
        if (nFound >= 4) {
            return nFound;
        }
    }
    return nFound;
}

void SetToolbarButtonCheckedState(MainWindow* win, int cmdId, bool isChecked) {
    int buttons[4];
    int n = GetToolbarButtonsByID(cmdId, buttons);
    if (n == 0) {
        return;
    }
    for (int i = 0; i < n; i++) {
        int idx = buttons[i];
        UpdateToolbarButtonStateByIdx(win->hwndToolbar, idx, isChecked, TBSTATE_CHECKED);
        RECT rc{};
        if (SendMessageW(win->hwndToolbar, TB_GETITEMRECT, idx, (LPARAM)&rc)) {
            // BTNS_CHECK auto-toggles before WM_COMMAND, so state may already match isChecked;
            // always redraw so custom-draw styling updates while the mouse is still over the button.
            RedrawWindow(win->hwndToolbar, &rc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }
}

static void UpdateThemeToolbarButton(MainWindow* win) {
    int buttons[4];
    int n = GetToolbarButtonsByID(CmdToggleLightDarkTheme, buttons);
    if (n == 0) {
        return;
    }
    bool isDark = ThemeUsesDarkChrome();
    SetToolbarButtonCheckedState(win, CmdToggleLightDarkTheme, isDark);
    const char* tip = isDark ? _TRN("&Dark Theme") : _TRN("&Light Theme");
    TempStr tipTranslated = (TempStr)trans::GetTranslation(tip);

    TBBUTTONINFOW bi{};
    bi.cbSize = sizeof(bi);
    bi.dwMask = TBIF_TEXT | TBIF_BYINDEX;
    bi.pszText = ToWStrTemp(tipTranslated);
    for (int i = 0; i < n; i++) {
        SendMessageW(win->hwndToolbar, TB_SETBUTTONINFOW, buttons[i], (LPARAM)&bi);
    }
    InvalidateRect(win->hwndToolbar, nullptr, FALSE);
}

bool NeedsDocumentColorModeUI(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return false;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || tab->IsAboutTab()) {
        return false;
    }
    return true;
}

bool NeedsPdfDocumentColorModeUI(MainWindow* win) {
    return NeedsDocumentColorModeUI(win);
}

// Keep toolbar button positions fixed on the home tab (tabs enabled).
static bool KeepToolbarLayoutOnHomeTab(MainWindow* win) {
    return win && SettingsUseTabs() && win->IsCurrentTabAbout();
}

void UpdatePdfDocumentColorModeToolbarButton(MainWindow* win) {
    static const int kPdfDocumentColorModeCmds[] = {
        CmdSetPdfDocumentColorModeAuto,
        CmdSetPdfDocumentColorModeBlack,
        CmdSetPdfDocumentColorModeLight,
    };
    bool showForDoc = NeedsDocumentColorModeUI(win);
    bool show = showForDoc || KeepToolbarLayoutOnHomeTab(win);
    for (int cmdId : kPdfDocumentColorModeCmds) {
        int buttons[4];
        int n = GetToolbarButtonsByID(cmdId, buttons);
        for (int i = 0; i < n; i++) {
            UpdateToolbarButtonStateByIdx(win->hwndToolbar, buttons[i], !show, TBSTATE_HIDDEN);
            if (showForDoc) {
                // ToolbarUpdateStateForWindow disables these while in light mode; unhide alone
                // leaves TBSTATE_ENABLED cleared so clicks are ignored until something else
                // re-enables them (e.g. a delayed ToolbarUpdateStateForWindow).
                UpdateToolbarButtonStateByIdx(win->hwndToolbar, buttons[i], true, TBSTATE_ENABLED);
            }
        }
    }
    if (!showForDoc) {
        return;
    }
    PdfDocumentColorMode mode = GetPdfDocumentColorMode();
    SetToolbarButtonCheckedState(win, CmdSetPdfDocumentColorModeAuto, mode == PdfDocumentColorMode::Auto);
    SetToolbarButtonCheckedState(win, CmdSetPdfDocumentColorModeBlack, mode == PdfDocumentColorMode::Black);
    SetToolbarButtonCheckedState(win, CmdSetPdfDocumentColorModeLight, mode == PdfDocumentColorMode::Light);
}

void UpdateDoubleClickWordLookupToolbarButton(MainWindow* win) {
    int buttons[4];
    int n = GetToolbarButtonsByID(CmdToggleDoubleClickWordLookup, buttons);
    if (n == 0) {
        return;
    }
    bool enabled = gGlobalPrefs->enableDoubleClickWordLookup;
    SetToolbarButtonCheckedState(win, CmdToggleDoubleClickWordLookup, enabled);
    const char* tip =
        enabled ? _TRN("Double-Click Word Lookup (enabled)") : _TRN("Double-Click Word Lookup (disabled)");
    TempStr tipTranslated = (TempStr)trans::GetTranslation(tip);

    TBBUTTONINFOW bi{};
    bi.cbSize = sizeof(bi);
    bi.dwMask = TBIF_TEXT | TBIF_BYINDEX;
    bi.pszText = ToWStrTemp(tipTranslated);
    for (int i = 0; i < n; i++) {
        SendMessageW(win->hwndToolbar, TB_SETBUTTONINFOW, buttons[i], (LPARAM)&bi);
    }
    InvalidateRect(win->hwndToolbar, nullptr, FALSE);
}

static void TbSetButtonDx(HWND hwndToolbar, int cmd, int dx) {
    TBBUTTONINFOW bi{};
    bi.cbSize = sizeof(bi);
    bi.dwMask = TBIF_SIZE;
    bi.cx = (WORD)dx;
    TbSetButtonInfoById(hwndToolbar, cmd, &bi);
}

// which documents support rotation
static bool NeedsRotateUI(MainWindow* win) {
    if (win->AsChm()) {
        return false;
    }
    return true;
}

// some commands are only avialble in certain contexts
// we remove toolbar buttons for un-availalbe commands
static bool IsCmdAvailable(MainWindow* win, int cmdId) {
    if (KeepToolbarLayoutOnHomeTab(win)) {
        return cmdId != WarningMsgId;
    }
    switch (cmdId) {
        case CmdZoomFitWidthAndContinuous:
        case CmdZoomFitPageAndSinglePage:
            return !win->AsChm();
        case CmdRotateLeft:
        case CmdRotateRight:
            return NeedsRotateUI(win);
        case CmdToggleBookmarks:
            return true;
        case CmdFindFirst:
        case CmdFindNext:
        case CmdFindPrev:
        case CmdFindToggleMatchCase:
        case CmdFindToggleMatchWholeWord:
            return NeedsFindUI(win);
        case PageInfoId:
            return true;
        case CmdToggleLightDarkTheme:
            return true;
        case CmdSetPdfDocumentColorModeAuto:
        case CmdSetPdfDocumentColorModeBlack:
        case CmdSetPdfDocumentColorModeLight:
            return NeedsDocumentColorModeUI(win);
    }
    auto ctx = NewBuildMenuCtx(win->CurrentTab(), Point{0, 0});
    AutoRun delCtx(DeleteBuildMenuCtx, ctx);
    auto [remove, disable] = GetCommandIdState(ctx, cmdId);
    return !remove;
}

static bool IsCmdEnabled(MainWindow* win, int cmdId) {
    auto ctx = NewBuildMenuCtx(win->CurrentTab(), Point{0, 0});
    AutoRun delCtx(DeleteBuildMenuCtx, ctx);

    switch (cmdId) {
        case CmdNextTab:
        case CmdPrevTab:
        case CmdNextTabSmart:
        case CmdPrevTabSmart:
            return SettingsUseTabs();
        case PageInfoId:
            return true;
        case CmdToggleBookmarks:
            return win->IsDocLoaded() && win->ctrl && win->ctrl->HasToc();
        case CmdToggleLightDarkTheme:
            return true;
        case CmdSetPdfDocumentColorModeAuto:
        case CmdSetPdfDocumentColorModeBlack:
        case CmdSetPdfDocumentColorModeLight:
            return NeedsDocumentColorModeUI(win);
    }

    auto [remove, disable] = GetCommandIdState(ctx, cmdId);
    if (remove || disable) {
        return false;
    }
    bool isAllowed = true;
    switch (cmdId) {
        case CmdOpenFile:
            isAllowed = CanAccessDisk();
            break;
        case CmdPrint:
            isAllowed = HasPermission(Perm::PrinterAccess);
            break;
    }
    if (!isAllowed) {
        return false;
    }

    // If no file open, only enable open button
    if (!win->IsDocLoaded()) {
        return CmdOpenFile == cmdId || CmdToggleLightDarkTheme == cmdId;
    }

    switch (cmdId) {
        case CmdOpenFile:
            // opening different files isn't allowed in plugin mode
            return !gPluginMode;

#ifndef DISABLE_DOCUMENT_RESTRICTIONS
        case CmdPrint:
            return !win->AsFixed() || win->AsFixed()->GetEngine()->AllowsPrinting();
#endif

        case CmdFindNext:
        case CmdFindPrev:
            // TODO: Update on whether there's more to find, not just on whether there is text.
            return HwndGetTextLen(win->hwndFindEdit) > 0;

        case CmdGoToNextPage:
            return win->ctrl->CurrentPageNo() < win->ctrl->PageCount();
        case CmdGoToPrevPage:
            return win->ctrl->CurrentPageNo() > 1;

        case CmdNavigateBack:
            return win->ctrl->CanNavigate(-1);
        case CmdNavigateForward:
            return win->ctrl->CanNavigate(1);

        default:
            return true;
    }
}

static TBBUTTON TbButtonFromButtonInfo(const ToolbarButtonInfo& bi, bool noTranslate = false) {
    TBBUTTON b{};
    b.idCommand = bi.cmdId;
    if (SkipBuiltInButton(bi)) {
        b.fsStyle = BTNS_SEP;
        return b;
    }
    b.iBitmap = (int)bi.bmpIndex;
    b.fsState = TBSTATE_ENABLED;
    b.fsStyle = BTNS_BUTTON;
    if (bi.cmdId == CmdReadAloud) {
        b.fsStyle |= BTNS_DROPDOWN;
    }
    if (bi.cmdId == CmdFindToggleMatchCase || bi.cmdId == CmdFindToggleMatchWholeWord ||
        bi.cmdId == CmdToggleDoubleClickWordLookup || bi.cmdId == CmdToggleLightDarkTheme ||
        bi.cmdId == CmdToggleBookmarks || bi.cmdId == CmdZoomFitWidthAndContinuous ||
        bi.cmdId == CmdZoomFitPageAndSinglePage || bi.cmdId == CmdSetPdfDocumentColorModeAuto ||
        bi.cmdId == CmdSetPdfDocumentColorModeBlack || bi.cmdId == CmdSetPdfDocumentColorModeLight) {
        b.fsStyle = BTNS_CHECK;
    }
    if (bi.bmpIndex == TbIcon::Text) {
        // b.fsStyle = BTNS_DROPDOWN;
        b.fsStyle |= BTNS_SHOWTEXT;
        b.fsStyle |= BTNS_AUTOSIZE;
    }
    auto s = noTranslate ? bi.toolTip : trans::GetTranslation(bi.toolTip);
    b.iString = (INT_PTR)ToWStrTemp(s);
    return b;
}

// Set toolbar button tooltips taking current language into account.
void UpdateToolbarButtonsToolTipsForWindow(MainWindow* win) {
    TBBUTTONINFO binfo{};
    HWND hwnd = win->hwndToolbar;
    for (int i = 0; i < kButtonsCount; i++) {
        const ToolbarButtonInfo& bi = gToolbarButtons[i];
        if (!bi.toolTip) {
            continue;
        }
        if (bi.bmpIndex == TbIcon::Text) {
            continue;
        }
        const char* accelStr = AppendAccelKeyToMenuStringTemp(nullptr, bi.cmdId);
        TempStr s = (TempStr)trans::GetTranslation(bi.toolTip);
        if (accelStr) {
            TempStr s2 = str::FormatTemp(" (%s)", accelStr + 1); // +1 to skip \t
            s = str::JoinTemp(s, s2);
        }

        binfo.cbSize = sizeof(TBBUTTONINFO);
        binfo.dwMask = TBIF_TEXT | TBIF_BYINDEX;
        binfo.pszText = ToWStrTemp(s);
        WPARAM buttonId = (WPARAM)i;
        TbSetButtonInfoById(hwnd, buttonId, &binfo);
    }
    UpdateThemeToolbarButton(win);
    UpdatePdfDocumentColorModeToolbarButton(win);
    UpdateDoubleClickWordLookupToolbarButton(win);
#if 0
    if (gCustomToolbarButtons) {
        int n = gCustomToolbarButtons->Size();
        for (int i = 0; i < n; i++) {
            const ToolbarButtonInfo& bi = gCustomToolbarButtons->At(i);
            const char* accelStr = AppendAccelKeyToMenuStringTemp(nullptr, bi.cmdId);
            TempStr s = (TempStr)bi.toolTip;
            if (accelStr) {
                TempStr s2 = str::FormatTemp(" (%s)", accelStr + 1); // +1 to skip \t
                s = str::JoinTemp(s, s2);
            }

            binfo.cbSize = sizeof(TBBUTTONINFO);
            binfo.dwMask = TBIF_TEXT | TBIF_BYINDEX;
            binfo.pszText = ToWStrTemp(s);
            WPARAM buttonId = (WPARAM)(kButtonsCount + i);
            TbSetButtonInfoById(hwnd, buttonId, &binfo);
        }
    }
#endif
}

// TODO: this is called too often
// TODO: also set checked state instead of calling SetToolbarButtonCheckedState() all over
void ToolbarUpdateStateForWindow(MainWindow* win, bool setButtonsVisibility) {
    HWND hwnd = win->hwndToolbar;
    int n = TotalButtonsCount();
    for (int i = 0; i < n; i++) {
        auto& tb = GetToolbarButtonInfoByIdx(i);
        int cmdId = tb.cmdId;
        if (setButtonsVisibility && cmdId != WarningMsgId && cmdId != 0) {
            bool hide = !IsCmdAvailable(win, cmdId);
            UpdateToolbarButtonStateByIdx(hwnd, i, hide, TBSTATE_HIDDEN);
        }
        if (SkipBuiltInButton(tb)) {
            continue;
        }
        bool isEnabled = IsCmdEnabled(win, cmdId);
        UpdateToolbarButtonStateByIdx(hwnd, i, isEnabled, TBSTATE_ENABLED);

        if (cmdId == CmdReadAloud || cmdId == CmdPauseReadAloud) {
            bool speaking = TtsIsSpeaking();
            SetToolbarButtonImageByIdx(hwnd, i, speaking ? TbIcon::PauseSpeaking : TbIcon::Speak);
            const char* tip = _TRA("Read Aloud");
            if (speaking) {
                tip = _TRA("Pause Reading");
            } else if (CanContinueReadAloud(win->CurrentTab())) {
                tip = _TRA("Continue Reading");
            }
            TBBUTTONINFOW tbi{};
            tbi.cbSize = sizeof(tbi);
            tbi.dwMask = TBIF_BYINDEX | TBIF_TEXT;
            tbi.pszText = (WCHAR*)ToWStrTemp(tip);
            SendMessageW(hwnd, TB_SETBUTTONINFO, i, (LPARAM)&tbi);
        }
    }

    // reposition the floating find bar over the search icon (and hide it if the
    // current document doesn't support find) when toolbar buttons change
    if (setButtonsVisibility) {
        UpdateToolbarFindText(win);
    }

    // update dirty (unsaved annotations) flag and tooltip on each tab
    if (win->tabsCtrl) {
        int nTabs = win->TabCount();
        for (int i = 0; i < nTabs; i++) {
            WindowTab* tab = win->GetTab(i);
            bool dirty = false;
            if (tab && tab->AsFixed()) {
                dirty = EngineHasUnsavedAnnotations(tab->AsFixed()->GetEngine());
            }
            // update tooltip before SetTabDirty (which rebuilds tooltips via LayoutTabs)
            TabInfo* ti = win->tabsCtrl->GetTab(i);
            if (ti && tab && tab->filePath) {
                const char* path = tab->filePath;
                if (dirty) {
                    TempStr tooltip = str::JoinTemp(path, " ", _TRA("(unsaved annotations)"));
                    str::ReplaceWithCopy(&ti->tooltip, tooltip);
                } else {
                    str::ReplaceWithCopy(&ti->tooltip, path);
                }
            }
            win->tabsCtrl->SetTabDirty(i, dirty);
        }
    }
}

void SetToolbarButtonEnableState(MainWindow* win, int cmdId, bool isEnabled) {
    int buttons[4];
    int n = GetToolbarButtonsByID(cmdId, buttons);
    if (n == 0) return;
    for (int i = 0; i < n; i++) {
        int idx = buttons[i];
        UpdateToolbarButtonStateByIdx(win->hwndToolbar, idx, isEnabled, TBSTATE_ENABLED);
    }
}
bool ShouldShowToolbar(MainWindow* win) {
    if (!gGlobalPrefs->showToolbar) {
        return false;
    }
    if (win->presentation) {
        return false;
    }
    if (win->isFullScreen) {
        return gGlobalPrefs->fullscreen.showToolbar;
    }
    // hide toolbar on about/home page when not using tabs
    if (!SettingsUseTabs() && win->IsCurrentTabAbout()) {
        return false;
    }
    return true;
}

void ShowOrHideToolbar(MainWindow* win) {
    bool show = ShouldShowToolbar(win);
    if (show == win->isToolbarVisible) {
        return;
    }
    win->isToolbarVisible = show;
    if (!show) {
        // Move the focus out of the toolbar
        if (HwndIsFocused(win->hwndFindEdit) || HwndIsFocused(win->hwndPageEdit)) {
            HwndSetFocus(win->hwndFrame);
        }
    }
    RelayoutWindow(win);
}

void UpdateFindbox(MainWindow* win) {
    // remove SS_WHITERECT so WM_CTLCOLORSTATIC controls the background color
    SetWindowStyle(win->hwndFindBg, SS_WHITERECT, false);
    SetWindowStyle(win->hwndPageBg, SS_WHITERECT, false);

    InvalidateRect(win->hwndToolbar, nullptr, TRUE);
    if (IsWindowVisible(win->hwndFrame)) {
        UpdateWindow(win->hwndToolbar);
    }

    auto cursorId = win->IsDocLoaded() ? IDC_IBEAM : IDC_ARROW;
    SetClassLongPtrW(win->hwndFindEdit, GCLP_HCURSOR, (LONG_PTR)GetCachedCursor(cursorId));
    if (!win->IsDocLoaded()) {
        // avoid focus on Find box
        HideCaret(nullptr);
    } else {
        ShowCaret(nullptr);
    }
}

static void ConfigureToolbarColors(HWND hwndToolbar) {
    if (!hwndToolbar) {
        return;
    }
    // disable themed toolbar rendering so NM_CUSTOMDRAW can override button highlights
    SetWindowTheme(hwndToolbar, L"", L"");
    // hide BTNS_SEP vertical rules by making groove colors match chrome background
    COLORREF bgCol = ThemeChromeBackgroundColor();
    COLORSCHEME cs{};
    cs.dwSize = sizeof(COLORSCHEME);
    cs.clrBtnHighlight = bgCol;
    cs.clrBtnShadow = bgCol;
    SendMessageW(hwndToolbar, TB_SETCOLORSCHEME, 0, (LPARAM)&cs);
}

static bool IsToolbarSeparatorAtIndex(HWND hwnd, int idx, TBBUTTON* outTb) {
    if (idx < 0) {
        return false;
    }
    TBBUTTON tb{};
    if (!SendMessageW(hwnd, TB_GETBUTTON, idx, (LPARAM)&tb)) {
        return false;
    }
    if ((tb.fsStyle & BTNS_SEP) == 0) {
        return false;
    }
    if (outTb) {
        *outTb = tb;
    }
    return true;
}

static COLORREF ToolbarButtonFillColor(COLORREF bgCol, bool isChecked, bool isSelected, bool isHot) {
    if (isChecked) {
        if (ThemeUsesBlackChrome()) {
            return AccentColor(bgCol, 20, 42);
        }
        if (ThemeUsesDarkChrome()) {
            return AccentColor(bgCol, 8, 28);
        }
        return AccentColor(bgCol, 24);
    }
    if (isSelected) {
        if (ThemeUsesBlackChrome()) {
            return AccentColor(bgCol, 16, 36);
        }
        if (ThemeUsesDarkChrome()) {
            return AccentColor(bgCol, 6, 14);
        }
    } else if (isHot) {
        if (ThemeUsesBlackChrome()) {
            return AccentColor(bgCol, 12, 28);
        }
        if (ThemeUsesDarkChrome()) {
            return AccentColor(bgCol, 6, 14);
        }
        return AccentColor(bgCol, -10);
    }
    return bgCol;
}

[[maybe_unused]] static int FindBeforeFindSeparatorIdx() {
    for (int i = 1; i < kButtonsCount; i++) {
        if (gToolbarButtons[i].cmdId != CmdFindFirst) {
            continue;
        }
        const ToolbarButtonInfo& prev = gToolbarButtons[i - 1];
        if (SkipBuiltInButton(prev) && prev.cmdId == 0) {
            return i - 1;
        }
    }
    return -1;
}

static bool IsToolbarButtonHidden(HWND hwnd, int idx) {
    TBBUTTONINFOW bi{};
    bi.cbSize = sizeof(bi);
    bi.dwMask = TBIF_BYINDEX | TBIF_STATE;
    SendMessageW(hwnd, TB_GETBUTTONINFOW, idx, (LPARAM)&bi);
    return (bi.fsState & TBSTATE_HIDDEN) != 0;
}

static bool GetVisibleNeighborItemRect(HWND hwnd, int startIdx, int delta, RECT* rc) {
    int count = (int)SendMessageW(hwnd, TB_BUTTONCOUNT, 0, 0);
    for (int j = startIdx; j >= 0 && j < count; j += delta) {
        if (IsToolbarButtonHidden(hwnd, j)) {
            continue;
        }
        if (SendMessageW(hwnd, TB_GETITEMRECT, j, (LPARAM)rc)) {
            return true;
        }
    }
    return false;
}

[[maybe_unused]] static bool GetToolbarSeparatorLineRect(HWND hwnd, int sepIdx, RECT* outRc) {
    RECT rcSep{};
    if (SendMessageW(hwnd, TB_GETITEMRECT, sepIdx, (LPARAM)&rcSep)) {
        int width = rcSep.right - rcSep.left;
        if (width > 0) {
            *outRc = rcSep;
            return true;
        }
    }
    RECT rcPrev{}, rcNext{};
    if (!GetVisibleNeighborItemRect(hwnd, sepIdx - 1, -1, &rcPrev)) {
        return false;
    }
    if (!GetVisibleNeighborItemRect(hwnd, sepIdx + 1, 1, &rcNext)) {
        return false;
    }
    int x = (rcPrev.right + rcNext.left) / 2;
    outRc->left = x;
    outRc->right = x + 1;
    outRc->top = rcPrev.top;
    outRc->bottom = rcPrev.bottom;
    return true;
}

// suppress default white/light-blue pressed and checked fills; use theme colors instead
static LRESULT PrepaintToolbarItem(NMTBCUSTOMDRAW* custDraw) {
    UINT itemState = custDraw->nmcd.uItemState;
    COLORREF bgCol = ThemeChromeBackgroundColor();
    COLORREF txtCol = ThemeWindowTextColor();
    if (itemState & CDIS_DISABLED) {
        txtCol = ThemeWindowTextDisabledColor();
    }
    custDraw->clrText = txtCol;
    custDraw->clrTextHighlight = txtCol;
    custDraw->clrBtnFace = bgCol;
    custDraw->clrBtnHighlight = bgCol;
    custDraw->clrHighlightHotTrack = bgCol;
    custDraw->nStringBkMode = TRANSPARENT;
    custDraw->nHLStringBkMode = TRANSPARENT;

    bool isSelected = (itemState & CDIS_SELECTED) != 0;
    bool isHot = (itemState & CDIS_HOT) != 0;

    custDraw->nmcd.uItemState &= ~(CDIS_CHECKED | CDIS_SELECTED);

    HWND hwndToolbar = custDraw->nmcd.hdr.hwndFrom;
    int idx = (int)custDraw->nmcd.dwItemSpec;
    TBBUTTONINFOW tbi{};
    tbi.cbSize = sizeof(tbi);
    tbi.dwMask = TBIF_STATE;
    SendMessageW(hwndToolbar, TB_GETBUTTONINFOW, idx, (LPARAM)&tbi);
    bool isChecked = (tbi.fsState & TBSTATE_CHECKED) != 0;

    COLORREF fillCol = ToolbarButtonFillColor(bgCol, isChecked, isSelected, isHot);

    RECT fillRc = custDraw->nmcd.rc;
    if (fillCol != bgCol) {
        fillRc.top += 2;
    }

    HBRUSH br = CreateSolidBrush(fillCol);
    FillRect(custDraw->nmcd.hdc, &fillRc, br);
    DeleteObject(br);

    if (isChecked) {
        COLORREF borderCol;
        if (ThemeUsesBlackChrome()) {
            borderCol = AccentColor(bgCol, 28, 52);
        } else {
            borderCol = ThemeUsesDarkChrome() ? AccentColor(bgCol, 24, 38) : AccentColor(bgCol, 38);
        }
        HBRUSH borderBr = CreateSolidBrush(borderCol);
        FrameRect(custDraw->nmcd.hdc, &fillRc, borderBr);
        DeleteObject(borderBr);
    }

    LRESULT ret = TBCDRF_USECDCOLORS | TBCDRF_NOBACKGROUND;
    return ret;
}

static bool IsToolbarDropdownButtonAtIndex(HWND hwnd, int idx) {
    TBBUTTONINFOW tbi{};
    tbi.cbSize = sizeof(tbi);
    tbi.dwMask = TBIF_STYLE | TBIF_STATE;
    if (SendMessageW(hwnd, TB_GETBUTTONINFOW, idx, (LPARAM)&tbi) == -1) {
        return false;
    }
    if (tbi.fsState & TBSTATE_HIDDEN) {
        return false;
    }
    return (tbi.fsStyle & BTNS_DROPDOWN) != 0;
}

static LRESULT PrepaintToolbarSeparatorItem(NMTBCUSTOMDRAW* custDraw) {
    COLORREF bgCol = ThemeChromeBackgroundColor();
    HBRUSH br = CreateSolidBrush(bgCol);
    FillRect(custDraw->nmcd.hdc, &custDraw->nmcd.rc, br);
    DeleteObject(br);

    custDraw->hpenLines = (HPEN)GetStockObject(NULL_PEN);
    custDraw->hbrLines = (HBRUSH)GetStockObject(NULL_BRUSH);
    custDraw->clrBtnFace = bgCol;
    custDraw->clrBtnHighlight = bgCol;

    return TBCDRF_USECDCOLORS | TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | CDRF_SKIPDEFAULT | CDRF_NOTIFYPOSTPAINT;
}

static void PostpaintToolbarSeparatorItem(NMTBCUSTOMDRAW* custDraw) {
    // light mode still draws BTNS_SEP grooves after ITEMPREPAINT; overpaint in POSTPAINT
    COLORREF bgCol = ThemeChromeBackgroundColor();
    HBRUSH br = CreateSolidBrush(bgCol);
    FillRect(custDraw->nmcd.hdc, &custDraw->nmcd.rc, br);
    DeleteObject(br);
}

static void PaintToolbarSeparatorBackgrounds(HWND hwnd, HDC hdc) {
    int count = (int)SendMessageW(hwnd, TB_BUTTONCOUNT, 0, 0);
    if (count <= 0) {
        return;
    }
    COLORREF bgCol = ThemeChromeBackgroundColor();
    HBRUSH br = CreateSolidBrush(bgCol);
    for (int i = 0; i < count; i++) {
        if (!IsToolbarSeparatorAtIndex(hwnd, i, nullptr)) {
            continue;
        }
        RECT rc{};
        if (SendMessageW(hwnd, TB_GETITEMRECT, i, (LPARAM)&rc)) {
            FillRect(hdc, &rc, br);
        }
    }
    DeleteObject(br);
}

static bool IsToolbarSeparatorDrawIndex(MainWindow* win, int idx) {
    if (idx >= 0 && idx < kButtonsCount && SkipBuiltInButton(gToolbarButtons[idx])) {
        return true;
    }
    return IsToolbarSeparatorAtIndex(win->hwndToolbar, idx, nullptr);
}

constexpr UINT_PTR kToolbarNotifySubclassId = 1;

// installed after darkmodelib so toolbar NM_CUSTOMDRAW uses theme colors, not white press chrome
static LRESULT CALLBACK ToolbarNotifyWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass,
                                             DWORD_PTR dwRefData) {
    if (WM_NOTIFY == uMsg) {
        NMHDR* hdr = (NMHDR*)lParam;
        auto win = FindMainWindowByHwnd(hWnd);
        if (!win && hdr->hwndFrom) {
            win = FindMainWindowByHwnd(hdr->hwndFrom);
        }
        if (win && hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == win->hwndToolbar) {
            NMTBCUSTOMDRAW* custDraw = (NMTBCUSTOMDRAW*)hdr;
            switch (custDraw->nmcd.dwDrawStage) {
                case CDDS_PREPAINT: {
                    HBRUSH brush = CreateSolidBrush(ThemeChromeBackgroundColor());
                    FillRect(custDraw->nmcd.hdc, &custDraw->nmcd.rc, brush);
                    DeleteObject(brush);
                    return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
                }
                case CDDS_ITEMPREPAINT: {
                    int idx = (int)custDraw->nmcd.dwItemSpec;
                    if (IsToolbarSeparatorDrawIndex(win, idx)) {
                        return PrepaintToolbarSeparatorItem(custDraw);
                    }
                    if (IsToolbarDropdownButtonAtIndex(win->hwndToolbar, idx)) {
                        if (UseDarkModeLib() && DarkMode::isEnabled()) {
                            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
                        }
                        return CDRF_DODEFAULT;
                    }
                    return PrepaintToolbarItem(custDraw);
                }
                case CDDS_ITEMPOSTPAINT: {
                    int idx = (int)custDraw->nmcd.dwItemSpec;
                    if (IsToolbarSeparatorDrawIndex(win, idx)) {
                        PostpaintToolbarSeparatorItem(custDraw);
                        return CDRF_SKIPDEFAULT;
                    }
                    if (IsToolbarDropdownButtonAtIndex(win->hwndToolbar, idx)) {
                        if (UseDarkModeLib() && DarkMode::isEnabled()) {
                            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
                        }
                        return CDRF_DODEFAULT;
                    }
                    break;
                }
                case CDDS_POSTPAINT:
                    PaintToolbarSeparatorBackgrounds(win->hwndToolbar, custDraw->nmcd.hdc);
                    break;
            }
        }
    }
    if (WM_NCDESTROY == uMsg) {
        RemoveWindowSubclass(hWnd, ToolbarNotifyWndProc, uIdSubclass);
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK ReBarWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass,
                              DWORD_PTR dwRefData) {
    if (WM_ERASEBKGND == uMsg && ThemeColorizeControls()) {
        HDC hdc = (HDC)wParam;
        RECT rect;
        GetClientRect(hWnd, &rect);
        SetTextColor(hdc, ThemeWindowTextColor());
        COLORREF bgCol = ThemeChromeBackgroundColor();
        SetBkColor(hdc, bgCol);
        auto bgBrush = CreateSolidBrush(bgCol);
        FillRect(hdc, &rect, bgBrush);
        DeleteObject(bgBrush);
        return 1;
    }
    // allow window dragging from empty rebar area (main toolbar)
    if (WM_LBUTTONDOWN == uMsg) {
        auto win = FindMainWindowByHwnd(hWnd);
        if (win && win->tabsInTitlebar) {
            HWND hwndFrame = GetAncestor(hWnd, GA_ROOT);
            ReleaseCapture();
            SendMessageW(hwndFrame, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
    }
    if (WM_LBUTTONDBLCLK == uMsg) {
        auto win = FindMainWindowByHwnd(hWnd);
        if (win && win->tabsInTitlebar) {
            HWND hwndFrame = GetAncestor(hWnd, GA_ROOT);
            WPARAM cmd = IsZoomed(hwndFrame) ? SC_RESTORE : SC_MAXIMIZE;
            PostMessageW(hwndFrame, WM_SYSCOMMAND, cmd, 0);
            return 0;
        }
    }
    if (WM_NCDESTROY == uMsg) {
        RemoveWindowSubclass(hWnd, ReBarWndProc, uIdSubclass);
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static WNDPROC DefWndProcEditBg = nullptr;
static LRESULT CALLBACK WndProcEditBg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT res = CallWindowProc(DefWndProcEditBg, hwnd, msg, wp, lp);
    if (msg == WM_PAINT) {
        HDC hdc = GetDC(hwnd);
        RECT rc;
        GetClientRect(hwnd, &rc);
        COLORREF bgCol2 = ThemeChromeBackgroundColor();
        COLORREF col = AccentColor(bgCol2, 40);
        HBRUSH br = CreateSolidBrush(col);
        FrameRect(hdc, &rc, br);
        DeleteObject(br);
        ReleaseDC(hwnd, hdc);
    }
    return res;
}

static WNDPROC DefWndProcToolbar = nullptr;
static LRESULT CALLBACK WndProcToolbar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (WM_CTLCOLORSTATIC == msg || WM_CTLCOLOREDIT == msg) {
        HWND hwndCtrl = (HWND)lp;
        HDC hdc = (HDC)wp;
        MainWindow* win = FindMainWindowByHwnd(hwndCtrl);
        if (!win) {
            return CallWindowProc(DefWndProcToolbar, hwnd, msg, wp, lp);
        }
        {
            bool isBgCtrl = (win->hwndFindBg == hwndCtrl || win->hwndPageBg == hwndCtrl);
            bool isEditCtrl = (win->hwndFindEdit == hwndCtrl || win->hwndPageEdit == hwndCtrl);
            SetTextColor(hdc, ThemeWindowTextColor());
            SetBkMode(hdc, TRANSPARENT);
            if ((isBgCtrl || isEditCtrl) && !ThemeColorizeControls()) {
                SetBkColor(hdc, RGB(0xff, 0xff, 0xff));
                return (LRESULT)GetStockObject(WHITE_BRUSH);
            }
            return (LRESULT)win->brControlBgColor;
        }
    }

    if (WM_COMMAND == msg) {
        HWND hEdit = (HWND)lp;
        MainWindow* win = FindMainWindowByHwnd(hEdit);
        // "find as you type" - skip if edit was not modified by user (e.g. programmatic text set)
        if (EN_UPDATE == HIWORD(wp) && hEdit == win->hwndFindEdit && gGlobalPrefs->showToolbar) {
            bool wasModified = Edit_GetModify(win->hwndFindEdit);
            if (wasModified) {
                FindTextOnThread(win, TextSearch::Direction::Forward, false);
            }
        }
    }

    // allow window dragging from empty toolbar areas
    if (WM_LBUTTONDOWN == msg || WM_LBUTTONDBLCLK == msg) {
        MainWindow* win = FindMainWindowByHwnd(hwnd);
        if (win && win->tabsInTitlebar) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = (int)SendMessageW(hwnd, TB_HITTEST, 0, (LPARAM)&pt);
            if (idx < 0) {
                // also check we're not over a child control (find box, page box)
                POINT ptScreen = pt;
                ClientToScreen(hwnd, &ptScreen);
                HWND childAtPoint = ChildWindowFromPoint(hwnd, pt);
                if (!childAtPoint || childAtPoint == hwnd) {
                    HWND hwndFrame = GetAncestor(hwnd, GA_ROOT);
                    if (WM_LBUTTONDBLCLK == msg) {
                        WPARAM cmd = IsZoomed(hwndFrame) ? SC_RESTORE : SC_MAXIMIZE;
                        PostMessageW(hwndFrame, WM_SYSCOMMAND, cmd, 0);
                    } else {
                        ReleaseCapture();
                        SendMessageW(hwndFrame, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    }
                    return 0;
                }
            }
        }
    }

    return CallWindowProc(DefWndProcToolbar, hwnd, msg, wp, lp);
}

#if 0
static WNDPROC DefWndProcEditSearch = nullptr;
static LRESULT CALLBACK WndProcEditSearch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win || !win->IsDocLoaded()) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    if (ExtendedEditWndProc(hwnd, msg, wp, lp)) {
        // select the whole find box on a non-selecting click
    } else if (WM_CHAR == msg) {
        switch (wp) {
            case VK_ESCAPE:
                if (win->findThread) {
                    AbortFinding(win, true);
                } else {
                    HwndSetFocus(win->hwndFrame);
                }
                return 1;

            case VK_RETURN: {
                if (IsShiftPressed()) {
                    FindPrev(win);
                } else {
                    FindNext(win);
                }
                return 1;
            }

            case VK_TAB:
                AdvanceFocus(win);
                return 1;
        }
    } else if (WM_ERASEBKGND == msg) {
        RECT r;
        Edit_GetRect(hwnd, &r);
        if (r.left == 0 && r.top == 0) { // virgin box
            r.left += 4;
            r.top += 3;
            r.bottom += 3;
            r.right -= 2;
            Edit_SetRectNoPaint(hwnd, &r);
        }
    } else if (WM_KEYDOWN == msg) {
        // TODO: if user re-binds F3 it'll not be picked up
        // we would have to either run accelerators after
        if (wp == VK_F3) {
            auto searchDir = IsShiftPressed() ? TextSearch::Direction::Backward : TextSearch::Direction::Forward;
            FindTextOnThread(win, searchDir, true);
            // Note: we don't return but let default processing take place
        }
        // TODO: here we used to call FrameOnKeydown() to make keys
        // like pageup etc. work even when focus is in text field
        // that no longer works because we moved most keys handling
        // to accelerators and we don't want to process acceleratos
        // while in edit control.
        // We could try to manually run accelerators but only if they
        // are virtual and don't prevent edit control from working
        // or maybe explicitly forword built-in accelerator for
        // white-listed shortucts but only if they were not modified by the user
    }

    LRESULT ret = CallWindowProc(DefWndProcEditSearch, hwnd, msg, wp, lp);

    // TOOD: why do we do it? re-eneable when we notice what breaks
    // the intent seems to be "after content of edit box changed"
    // but how does that afect state of the toolbar?

#if 0
    switch (msg) {
        case WM_CHAR:
        case WM_PASTE:
        case WM_CUT:
        case WM_CLEAR:
        case WM_UNDO:
        case WM_KEYUP:
            ToolbarUpdateStateForWindow(win, false);
            break;
    }
#endif

    return ret;
}

static int FindLabelXAfterToolbarAnchor(HWND hwndToolbar, HWND hwndFrame, const RECT& anchorR, const RECT& findSlotR) {
    int labelPad = DpiScale(hwndFrame, kFindLabelPadAfterSep);
    int minSepDx = DpiScale(hwndFrame, 6);
    int sepDx = findSlotR.left - anchorR.right;
    if (sepDx < minSepDx) {
        sepDx = minSepDx;
    }
    return anchorR.right + sepDx + labelPad;
}

static bool LayoutToolbarFindControls(MainWindow* win, Size size, int findDy, int minFindBoxDx) {
    int labelPad = DpiScale(win->hwndFrame, kFindLabelPadAfterSep);
    int edgePadding = GetSystemMetrics(SM_CXEDGE);
    int sepIdx = FindBeforeFindSeparatorIdx();

    RECT anchorR{}, findSlotR{}, searchPrevR{};
    TbGetRectById(win->hwndToolbar, CmdZoomIn, &anchorR);
    TbGetRectById(win->hwndToolbar, CmdFindFirst, &findSlotR);
    TbGetRectById(win->hwndToolbar, CmdFindPrev, &searchPrevR);
    if (searchPrevR.left <= anchorR.right || searchPrevR.left <= findSlotR.left) {
        return false;
    }

    RECT sepRc{};
    int sepRight = findSlotR.left;
    if (sepIdx >= 0 && GetToolbarSeparatorLineRect(win->hwndToolbar, sepIdx, &sepRc)) {
        sepRight = sepRc.right;
    }
    int currX = FindLabelXAfterToolbarAnchor(win->hwndToolbar, win->hwndFrame, anchorR, findSlotR);

    int findBoxDx = searchPrevR.left - currX - size.dx - edgePadding * 2;
    if (findBoxDx < minFindBoxDx) {
        findBoxDx = minFindBoxDx;
    }

    int slotOffset = currX - findSlotR.left;
    if (slotOffset < 0) {
        slotOffset = labelPad;
    }
    int findAreaDx = slotOffset + size.dx + findBoxDx + edgePadding;
    TbSetButtonDx(win->hwndToolbar, CmdFindFirst, findAreaDx);
    SendMessageW(win->hwndToolbar, TB_AUTOSIZE, 0, 0);
    UpdateWindow(win->hwndToolbar);

    TbGetRectById(win->hwndToolbar, CmdZoomIn, &anchorR);
    TbGetRectById(win->hwndToolbar, CmdFindFirst, &findSlotR);
    TbGetRectById(win->hwndToolbar, CmdFindPrev, &searchPrevR);
    if (searchPrevR.left <= anchorR.right || searchPrevR.left <= findSlotR.left) {
        return false;
    }
    if (sepIdx >= 0 && GetToolbarSeparatorLineRect(win->hwndToolbar, sepIdx, &sepRc)) {
        sepRight = sepRc.right;
    }
    currX = FindLabelXAfterToolbarAnchor(win->hwndToolbar, win->hwndFrame, anchorR, findSlotR);
    findBoxDx = searchPrevR.left - currX - size.dx - edgePadding * 2;
    if (findBoxDx < minFindBoxDx) {
        findBoxDx = minFindBoxDx;
    }
    if (searchPrevR.left <= currX + (int)size.dx) {
        return false;
    }

    int currY = (findSlotR.bottom + findSlotR.top - findDy) / 2;
    int x = currX;
    int y = (findDy - size.dy + 1) / 2 + currY;
    MoveWindow(win->hwndFindLabel, x, y, size.dx, size.dy, TRUE);
    x = currX + size.dx;
    MoveWindow(win->hwndFindBg, x, currY, findBoxDx, findDy, FALSE);
    x = currX + size.dx + edgePadding;
    y = (findDy - size.dy + 1) / 2 + currY;
    MoveWindow(win->hwndFindEdit, x, y, findBoxDx - 2 * edgePadding, size.dy, FALSE);
    UpdateWindow(win->hwndFindLabel);
    UpdateWindow(win->hwndFindBg);
    UpdateWindow(win->hwndFindEdit);

    return true;
}

#endif

void UpdateToolbarFindText(MainWindow* win) {
    FindBarReposition(win);
}

void UpdateToolbarState(MainWindow* win) {
    UpdatePdfDocumentColorModeToolbarButton(win);
    UpdateDoubleClickWordLookupToolbarButton(win);
    if (!win->IsDocLoaded()) {
        return;
    }
    HWND hwnd = win->hwndToolbar;
    DisplayMode dm = win->ctrl->GetDisplayMode();
    float zoomVirtual = win->ctrl->GetZoomVirtual();
    {
        bool isChecked = dm == DisplayMode::Continuous && zoomVirtual == kZoomFitWidth;
        SetToolbarButtonCheckedState(win, CmdZoomFitWidthAndContinuous, isChecked);
    }
    {
        bool isChecked = dm == DisplayMode::SinglePage && zoomVirtual == kZoomFitPage;
        SetToolbarButtonCheckedState(win, CmdZoomFitPageAndSinglePage, isChecked);
        if (!isChecked) {
            win->CurrentTab()->prevZoomVirtual = kInvalidZoom;
        }
    }
    SetToolbarButtonCheckedState(win, CmdToggleBookmarks, win->tocVisible);
}

#if 0
static void CreateFindBox(MainWindow* win, HFONT hfont, int iconDy) {
    bool isRtl = IsUIRtl();
    int findBoxDx = HwndMeasureText(win->hwndFrame, "this is a story of my", hfont).dx;
    HMODULE hmod = GetModuleHandleW(nullptr);
    HWND p = win->hwndToolbar;
    DWORD style = WS_VISIBLE | WS_CHILD;
    DWORD exStyle = 0;
    if (isRtl) exStyle |= WS_EX_LAYOUTRTL;
    int dy = iconDy + 2;
    // Size textSize = HwndMeasureText(win->hwndFrame, L"M", hfont);
    HWND findBg =
        CreateWindowEx(exStyle, WC_STATIC, L"", style, 0, 1, findBoxDx, dy, p, (HMENU) nullptr, hmod, nullptr);

    if (!DefWndProcEditBg) {
        DefWndProcEditBg = (WNDPROC)GetWindowLongPtr(findBg, GWLP_WNDPROC);
    }
    SetWindowLongPtr(findBg, GWLP_WNDPROC, (LONG_PTR)WndProcEditBg);

    style = WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL;
    // dy = iconDy + DpiScale(win->hwndFrame, 2);
    dy = iconDy;
    exStyle = 0;
    if (isRtl) exStyle |= WS_EX_LAYOUTRTL;
    HWND find = CreateWindowExW(exStyle, WC_EDIT, L"", style, 0, 1, findBoxDx, dy, p, (HMENU) nullptr, hmod, nullptr);

    style = WS_VISIBLE | WS_CHILD;
    exStyle = 0;
    if (isRtl) exStyle |= WS_EX_LAYOUTRTL;
    HWND label = CreateWindowExW(exStyle, WC_STATIC, L"", style, 0, 1, 0, 0, p, (HMENU) nullptr, hmod, nullptr);

    SetWindowFont(label, hfont, FALSE);
    SetWindowFont(find, hfont, FALSE);

    if (!DefWndProcToolbar) {
        DefWndProcToolbar = (WNDPROC)GetWindowLongPtr(win->hwndToolbar, GWLP_WNDPROC);
    }
    SetWindowLongPtr(win->hwndToolbar, GWLP_WNDPROC, (LONG_PTR)WndProcToolbar);

    if (!DefWndProcEditSearch) {
        DefWndProcEditSearch = (WNDPROC)GetWindowLongPtr(find, GWLP_WNDPROC);
    }
    SetWindowLongPtr(find, GWLP_WNDPROC, (LONG_PTR)WndProcEditSearch);

    win->hwndFindLabel = label;
    win->hwndFindEdit = find;
    win->hwndFindBg = findBg;
}
#endif

static WNDPROC DefWndProcPageBox = nullptr;
static LRESULT CALLBACK WndProcPageBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win || !win->IsDocLoaded()) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    if (ExtendedEditWndProc(hwnd, msg, wp, lp)) {
        // select the whole page box on a non-selecting click
    } else if (WM_CHAR == msg) {
        switch (wp) {
            case VK_RETURN: {
                char* s = HwndGetTextTemp(win->hwndPageEdit);
                int newPageNo = win->ctrl->GetPageByLabel(s);
                if (win->ctrl->ValidPageNo(newPageNo)) {
                    win->ctrl->GoToPage(newPageNo, true);
                    HwndSetFocus(win->hwndFrame);
                }
                return 1;
            }
            case VK_ESCAPE:
                HwndSetFocus(win->hwndFrame);
                return 1;

            case VK_TAB:
                AdvanceFocus(win);
                return 1;
        }
    } else if (WM_ERASEBKGND == msg) {
        RECT r;
        Edit_GetRect(hwnd, &r);
        if (r.left == 0 && r.top == 0) { // virgin box
            r.left += 4;
            r.top += 3;
            r.bottom += 3;
            r.right -= 2;
            Edit_SetRectNoPaint(hwnd, &r);
        }
    } else if (WM_KEYDOWN == msg) {
        // TODO: see WndProcEditSearch for note on enabling accelerators here as well
    }

    return CallWindowProc(DefWndProcPageBox, hwnd, msg, wp, lp);
}

void UpdateToolbarPageText(MainWindow* win, int pageCount, bool updateOnly) {
    if (!win->hwndToolbar) {
        return;
    }
    const char* text = _TRA("Page:");
    if (!updateOnly) {
        HwndSetText(win->hwndPageLabel, text);
    }
    int padX = DpiScale(win->hwndFrame, kTextPaddingRight);
    Size size = HwndMeasureText(win->hwndPageLabel, text);
    size.dx += padX;
    size.dx += DpiScale(win->hwndFrame, kButtonSpacingX);

    Rect pageWndRect = WindowRect(win->hwndPageBg);

    // TB_GETRECT fails for hidden buttons, so anchor on a button that's still
    // visible. CmdPrint is hidden when PrinterAccess is revoked via
    // sumatrapdfrestrict.ini (issue #5563); fall back to CmdOpenFile in that case.
    RECT r{};
    int anchorCmd = HasPermission(Perm::PrinterAccess) ? CmdPrint : CmdOpenFile;
    BOOL gotAnchor = (BOOL)SendMessageW(win->hwndToolbar, TB_GETRECT, anchorCmd, (LPARAM)&r);
    int currX;
    int currY;
    if (gotAnchor) {
        currX = r.right + DpiScale(win->hwndFrame, 10);
        currY = (r.bottom - pageWndRect.dy) / 2;
    } else {
        // anchor button is hidden (e.g. on the Home tab), so align to the left
        // of the toolbar with a small margin
        currX = DpiScale(win->hwndFrame, 6);
        currY = 0;
    }

    TempStr txt = nullptr;
    Size size2;
    Size minSize = HwndMeasureText(win->hwndPageTotal, "999 / 999");
    minSize.dx += padX;
    int labelDx = 0;
    if (-1 == pageCount) {
#if 0
        // preserve hwndPageTotal's text and size
        txt = HwndGetTextTemp(win->hwndPageTotal);
        size2 = ClientRect(win->hwndPageTotal).Size();
        size2.dx -= padX;
        size2.dx -= DpiScale(win->hwndFrame, kButtonSpacingX);
#endif
        // hack: https://github.com/sumatrapdfreader/sumatrapdf/issues/4475
        txt = (TempStr) " ";
        minSize.dx = 0;
        size2.dx = 0;
    } else if (!pageCount) {
        // hack: https://github.com/sumatrapdfreader/sumatrapdf/issues/4475
        txt = (TempStr) " ";
        if (KeepToolbarLayoutOnHomeTab(win)) {
            size2.dx = minSize.dx;
        } else {
            minSize.dx = 0;
            size2.dx = 0;
        }
    } else if (!win->ctrl || !win->ctrl->HasPageLabels()) {
        txt = str::FormatTemp(" / %d", pageCount);
        size2 = HwndMeasureText(win->hwndPageTotal, txt);
        minSize.dx = size2.dx;
    } else {
        txt = str::FormatTemp("%d / %d", win->ctrl->CurrentPageNo(), pageCount);
        // TempStr txt2 = str::FormatTemp(" (%d / %d)", pageCount, pageCount);
        size2 = HwndMeasureText(win->hwndPageTotal, txt);
    }
    labelDx = size2.dx;
    size2.dx = std::max(size2.dx, minSize.dx);

    HwndSetText(win->hwndPageTotal, txt);
    if (0 == size2.dx) {
        size2 = HwndMeasureText(win->hwndPageTotal, txt);
    }
    size2.dx += padX;
    size2.dx += DpiScale(win->hwndFrame, kButtonSpacingX);

    if (win->ctrl && pageCount > 0 && win->ctrl->ValidPageNo(win->ctrl->CurrentPageNo())) {
        TempStr label = win->ctrl->GetPageLabeTemp(win->ctrl->CurrentPageNo());
        HwndSetText(win->hwndPageEdit, label);
    } else if (KeepToolbarLayoutOnHomeTab(win)) {
        HwndSetText(win->hwndPageEdit, "");
    }

    int padding = GetSystemMetrics(SM_CXEDGE);
    int x = currX - 1;
    int y = (pageWndRect.dy - size.dy + 1) / 2 + currY;
    MoveWindow(win->hwndPageLabel, x, y, size.dx, size.dy, FALSE);
    if (IsUIRtl()) {
        currX += size2.dx;
        currX -= padX;
        currX -= DpiScale(win->hwndFrame, kButtonSpacingX);
    }
    x = currX + size.dx;
    y = currY;
    MoveWindow(win->hwndPageBg, x, y, pageWndRect.dx, pageWndRect.dy, FALSE);
    x = currX + size.dx + padding;
    y = (pageWndRect.dy - size.dy + 1) / 2 + currY;
    int dx = pageWndRect.dx - 2 * padding;
    MoveWindow(win->hwndPageEdit, x, y, dx, size.dy, FALSE);
    // in right-to-left layout, the total comes "before" the current page number
    if (IsUIRtl()) {
        currX -= size2.dx;
        x = currX + size.dx;
        y = (pageWndRect.dy - size.dy + 1) / 2 + currY;
        MoveWindow(win->hwndPageTotal, x, y, size2.dx, size.dy, FALSE);
    } else {
        x = currX + size.dx + pageWndRect.dx;
        int midX = (size2.dx - labelDx) / 2;
        y = (pageWndRect.dy - size.dy + 1) / 2 + currY;
        MoveWindow(win->hwndPageTotal, x + midX, y, labelDx, size.dy, FALSE);
    }

    TBBUTTONINFOW bi{};
    bi.cbSize = sizeof(bi);
    bi.dwMask = TBIF_SIZE;
    SendMessageW(win->hwndToolbar, TB_GETBUTTONINFO, PageInfoId, (LPARAM)&bi);
    size2.dx += size.dx + pageWndRect.dx + 12;
    int oldPageSlotDx = bi.cx;
    bool pageSlotResized = false;
    if (bi.cx != size2.dx || !updateOnly) {
        TbSetButtonDx(win->hwndToolbar, PageInfoId, size2.dx);
        pageSlotResized = (oldPageSlotDx != size2.dx);
    }
    if (pageSlotResized) {
        SendMessageW(win->hwndToolbar, TB_AUTOSIZE, 0, 0);
        UpdateWindow(win->hwndToolbar);
        if (NeedsFindUI(win)) {
            UpdateToolbarFindText(win);
        }
    }
    InvalidateRect(win->hwndToolbar, nullptr, TRUE);
}

static void CreatePageBox(MainWindow* win, HFONT font, int iconDy) {
    bool isRtl = IsUIRtl();

    auto hwndFrame = win->hwndFrame;
    auto hwndToolbar = win->hwndToolbar;
    // TODO: this is broken, result is way too small
    int boxWidth = HwndMeasureText(hwndFrame, "999999", font).dx;
    DWORD style = WS_VISIBLE | WS_CHILD;
    auto h = GetModuleHandle(nullptr);
    int dx = boxWidth;
    int dy = iconDy + 2;
    DWORD exStyle = 0;
    if (isRtl) exStyle |= WS_EX_LAYOUTRTL;

    HWND pageBg =
        CreateWindowExW(exStyle, WC_STATICW, L"", style, 0, 1, dx, dy, hwndToolbar, (HMENU) nullptr, h, nullptr);
    if (!DefWndProcEditBg) {
        DefWndProcEditBg = (WNDPROC)GetWindowLongPtr(pageBg, GWLP_WNDPROC);
    }
    SetWindowLongPtr(pageBg, GWLP_WNDPROC, (LONG_PTR)WndProcEditBg);
    HWND label = CreateWindowExW(0, WC_STATICW, L"", style, 0, 1, 0, 0, hwndToolbar, (HMENU) nullptr, h, nullptr);
    HWND total = CreateWindowExW(0, WC_STATICW, L"", style, 0, 1, 0, 0, hwndToolbar, (HMENU) nullptr, h, nullptr);

    style = WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER | ES_RIGHT;
    dx = boxWidth - DpiScale(hwndFrame, 4); // 4 pixels padding on the right side of the text box
    dy = iconDy;
    exStyle = 0;
    if (isRtl) exStyle |= WS_EX_LAYOUTRTL;
    HWND page = CreateWindowExW(exStyle, WC_EDIT, L"0", style, 0, 1, dx, dy, hwndToolbar, (HMENU) nullptr, h, nullptr);

    SetWindowFont(label, font, FALSE);
    SetWindowFont(page, font, FALSE);
    SetWindowFont(total, font, FALSE);

    if (!DefWndProcPageBox) {
        DefWndProcPageBox = (WNDPROC)GetWindowLongPtr(page, GWLP_WNDPROC);
    }
    SetWindowLongPtr(page, GWLP_WNDPROC, (LONG_PTR)WndProcPageBox);

    win->hwndPageLabel = label;
    win->hwndPageEdit = page;
    win->hwndPageBg = pageBg;
    win->hwndPageTotal = total;
}

static void BlitPixmap(u8* dstSamples, ptrdiff_t dstStride, fz_pixmap* src, int dstX, int dstY, COLORREF bgCol) {
    int dx = src->w;
    int dy = src->h;
    int srcN = src->n;
    int dstN = 4;
    auto srcStride = src->stride;
    u8 r, g, b;
    UnpackColor(bgCol, r, g, b);
    for (size_t y = 0; y < (size_t)dy; y++) {
        u8* s = src->samples + (srcStride * (size_t)y);
        size_t atY = y + (size_t)dstY;
        u8* d = dstSamples + (dstStride * atY) + ((size_t)dstX * dstN);
        for (int x = 0; x < dx; x++) {
            bool isTransparent = (s[0] == r) && (s[1] == g) && (s[2] == b);
            // note: we're swapping red and green channel because src is rgb
            // and we want bgr for Toolbar's IMAGELIST
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            if (isTransparent) {
                d[3] = 0;
            } else {
                d[3] = 0xff;
            }
            d += dstN;
            s += srcN;
        }
    }
}

static HBITMAP BuildIconsBitmap(int dx, int dy) {
    fz_context* ctx = fz_new_context_windows();
    int nIcons = (int)TbIcon::kMax;
    int destDx = dx * nIcons;
    ptrdiff_t dstStride;

    u8* hbmpData = nullptr;
    HBITMAP hbmp;
    {
        int w = destDx;
        int h = dy;
        int n = 4;
        dstStride = destDx * n;
        int imgSize = (int)dstStride * h;
        int bitsCount = n * 8;

        size_t bmiSize = sizeof(BITMAPINFO) + 255 * sizeof(RGBQUAD);
        auto bmi = (BITMAPINFO*)calloc(1, bmiSize);
        defer {
            free(bmi);
        };
        BITMAPINFOHEADER* bmih = &bmi->bmiHeader;
        bmih->biSize = sizeof(*bmih);
        bmih->biWidth = w;
        bmih->biHeight = -h;
        bmih->biPlanes = 1;
        bmih->biCompression = BI_RGB;
        bmih->biBitCount = bitsCount;
        bmih->biSizeImage = imgSize;
        bmih->biClrUsed = 0;
        HANDLE hFile = INVALID_HANDLE_VALUE;
        DWORD fl = PAGE_READWRITE;
        HANDLE hMap = CreateFileMappingW(hFile, nullptr, fl, 0, imgSize, nullptr);
        uint usage = DIB_RGB_COLORS;
        hbmp = CreateDIBSection(nullptr, bmi, usage, (void**)&hbmpData, hMap, 0);
    }

    COLORREF fgCol = ThemeWindowTextColor();
    COLORREF bgCol = ThemeChromeBackgroundColor();
    for (int i = 0; i < nIcons; i++) {
        const char* svgData = GetSvgIcon((TbIcon)i);
        TempStr strokeCol = SerializeColorTemp(fgCol);
        TempStr fillCol = SerializeColorTemp(bgCol);
        TempStr fillColRepl = str::JoinTemp("fill=\"", fillCol, "\""); // fill="${col}"
        svgData = str::ReplaceTemp(svgData, "currentColor", strokeCol);
        svgData = str::ReplaceTemp(svgData, R"(fill="none")", fillColRepl);
        fz_buffer* buf = fz_new_buffer_from_copied_data(ctx, (u8*)svgData, str::Len(svgData));
        fz_image* image = fz_new_image_from_svg(ctx, buf, nullptr, nullptr);
        image->w = dx;
        image->h = dy;
        fz_pixmap* pixmap = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
        BlitPixmap(hbmpData, dstStride, pixmap, dx * i, 0, bgCol);
        fz_drop_pixmap(ctx, pixmap);
        fz_drop_image(ctx, image);
        fz_drop_buffer(ctx, buf);
    }

    fz_drop_context_windows(ctx);
    return hbmp;
}

HIMAGELIST BuildStdToolbarImageList(int dx) {
    HIMAGELIST himl = ImageList_Create(dx, dx, ILC_COLOR32, (int)TbIcon::kMax, 0);
    HBITMAP hbmp = BuildIconsBitmap(dx, dx);
    ImageList_Add(himl, hbmp, nullptr);
    DeleteObject(hbmp);
    return himl;
}

Rect GetToolbarButtonScreenRect(MainWindow* win, int cmdId) {
    RECT r{};
    if (!win->hwndToolbar) {
        return {};
    }
    TbGetRectById(win->hwndToolbar, cmdId, &r);
    MapWindowPoints(win->hwndToolbar, HWND_DESKTOP, (POINT*)&r, 2);
    return Rect{r.left, r.top, r.right - r.left, r.bottom - r.top};
}

constexpr int kDefaultIconSize = 18;

static int SetToolbarIconsImageList(MainWindow* win) {
    HWND hwndToolbar = win->hwndToolbar;
    HWND hwndParent = GetParent(hwndToolbar);

    // we call it ToolbarSize for users, but it's really size of the icon
    // toolbar size is iconSize + padding (seems to be 6)
    int iconSize = gGlobalPrefs->toolbarSize;
    if (iconSize == kDefaultIconSize) {
        // scale if default size
        iconSize = DpiScale(hwndParent, iconSize);
    }
    // icon sizes must be multiple of 4 or else they are sheared
    // TODO: I must be doing something wrong, any size should be ok
    // it might be about size of buttons / bitmaps
    iconSize = RoundUp(iconSize, 4);
    int dx = iconSize;
    // this doesn't seem to be required and doesn't help with weird sizes like 22
    // but the docs say to do it
    SendMessage(hwndToolbar, TB_SETBITMAPSIZE, 0, (LPARAM)MAKELONG(dx, dx));

    // assume square icons
    HIMAGELIST himl = ImageList_Create(dx, dx, ILC_COLOR32, kButtonsCount, 0);
    HBITMAP hbmp = BuildIconsBitmap(dx, dx);
    ImageList_Add(himl, hbmp, nullptr);
    DeleteObject(hbmp);
    SendMessageW(hwndToolbar, TB_SETIMAGELIST, 0, (LPARAM)himl);
    return iconSize;
}

static void SyncToolbarRebarChrome(MainWindow* win) {
    if (!win->hwndReBar) {
        return;
    }
    SendMessageW(win->hwndReBar, RB_SETBKCOLOR, 0, ThemeChromeBackgroundColor());
    // Keep borderless like the menu rebar: WS_BORDER draws a visible 1px line
    // below the toolbar when layout is correct after theme/tab switches.
    LONG style = GetWindowLong(win->hwndReBar, GWL_STYLE);
    bool hadBorders = (style & (WS_BORDER | RBS_BANDBORDERS)) != 0;
    if (hadBorders) {
        style &= ~(WS_BORDER | RBS_BANDBORDERS);
        SetWindowLong(win->hwndReBar, GWL_STYLE, style);
        SetWindowPos(win->hwndReBar, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        SendMessageW(win->hwndToolbar, TB_AUTOSIZE, 0, 0);
    }
}

void UpdateToolbarAfterThemeChange(MainWindow* win) {
    SyncToolbarRebarChrome(win);
    SetToolbarIconsImageList(win);
    ConfigureToolbarColors(win->hwndToolbar);
    UpdateThemeToolbarButton(win);
    UpdatePdfDocumentColorModeToolbarButton(win);
    UpdateDoubleClickWordLookupToolbarButton(win);
    if (win->hwndReBar) {
        InvalidateRect(win->hwndReBar, nullptr, TRUE);
    }
    HwndScheduleRepaint(win->hwndToolbar);
}

// https://docs.microsoft.com/en-us/windows/win32/controls/toolbar-control-reference
void CreateToolbar(MainWindow* win) {
    bool isRtl = IsUIRtl();

    kButtonSpacingX = 0;
    HINSTANCE hinst = GetModuleHandle(nullptr);
    HWND hwndParent = win->hwndFrame;

    DWORD style = WS_CHILD | WS_CLIPCHILDREN | RBS_VARHEIGHT;
    // no WS_BORDER (avoids 1px line) and no RBS_BANDBORDERS (avoids gray band separators)
    style |= CCS_NODIVIDER | CCS_NOPARENTALIGN | WS_VISIBLE;
    DWORD exStyle = WS_EX_TOOLWINDOW;
    if (isRtl) exStyle |= WS_EX_LAYOUTRTL;

    win->hwndReBar = CreateWindowExW(exStyle, REBARCLASSNAME, nullptr, style, 0, 0, 0, 0, hwndParent, (HMENU)IDC_REBAR,
                                     hinst, nullptr);
    SetWindowSubclass(win->hwndReBar, ReBarWndProc, 0, 0);

    REBARINFO rbi{};
    rbi.cbSize = sizeof(REBARINFO);
    rbi.fMask = 0;
    rbi.himl = (HIMAGELIST) nullptr;
    SendMessageW(win->hwndReBar, RB_SETBARINFO, 0, (LPARAM)&rbi);
    SendMessageW(win->hwndReBar, RB_SETBKCOLOR, 0, ThemeChromeBackgroundColor());

    style = WS_CHILD | WS_CLIPSIBLINGS | TBSTYLE_TOOLTIPS | TBSTYLE_FLAT;
    style |= TBSTYLE_LIST | CCS_NODIVIDER | CCS_NOPARENTALIGN;
    exStyle = 0;
    if (isRtl) exStyle |= WS_EX_LAYOUTRTL;
    HMENU cmd = (HMENU)IDC_TOOLBAR;
    HWND hwndToolbar =
        CreateWindowExW(exStyle, TOOLBARCLASSNAME, nullptr, style, 0, 0, 0, 0, win->hwndReBar, cmd, hinst, nullptr);
    win->hwndToolbar = hwndToolbar;
    SendMessageW(hwndToolbar, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);

    // always disable themed toolbar rendering; custom draw handles button states
    SetWindowTheme(hwndToolbar, L"", L"");
    ConfigureToolbarColors(hwndToolbar);

    if (UseDarkModeLib()) {
        DarkMode::setWindowNotifyCustomDrawSubclass(win->hwndReBar);
    }
    SetWindowSubclass(win->hwndReBar, ToolbarNotifyWndProc, kToolbarNotifySubclassId, 0);

    int iconSize = SetToolbarIconsImageList(win);

    TBMETRICS tbMetrics{};
    tbMetrics.cbSize = sizeof(tbMetrics);
    // tbMetrics.dwMask = TBMF_PAD;
    tbMetrics.dwMask = TBMF_BUTTONSPACING;
    TbGetMetrics(hwndToolbar, &tbMetrics);
    int yPad = DpiScale(win->hwndFrame, 2);
    tbMetrics.cxPad += DpiScale(win->hwndFrame, 14);
    tbMetrics.cyPad += yPad;
    tbMetrics.cxButtonSpacing += DpiScale(win->hwndFrame, kButtonSpacingX);
    // tbMetrics.cyButtonSpacing += DpiScale(win->hwndFrame, 4);
    TbSetMetrics(hwndToolbar, &tbMetrics);

    LRESULT exstyle = SendMessageW(hwndToolbar, TB_GETEXTENDEDSTYLE, 0, 0);
    exstyle |= TBSTYLE_EX_MIXEDBUTTONS | TBSTYLE_EX_DRAWDDARROWS;
    SendMessageW(hwndToolbar, TB_SETEXTENDEDSTYLE, 0, exstyle);

    TBBUTTON tbButtons[kButtonsCount];
    for (int i = 0; i < kButtonsCount; i++) {
        const ToolbarButtonInfo& bi = gToolbarButtons[i];
        tbButtons[i] = TbButtonFromButtonInfo(bi);
    }
    SendMessageW(hwndToolbar, TB_ADDBUTTONS, kButtonsCount, (LPARAM)tbButtons);

    gCustomButtonsCount = 0;

    char* text;
    for (Shortcut* shortcut : *gGlobalPrefs->shortcuts) {
        if (gCustomButtonsCount >= kMaxCustomButtons) {
            break;
        }
        text = shortcut->toolbarText;
        if (str::IsEmptyOrWhiteSpace(text)) {
            continue;
        }
        ToolbarButtonInfo tbi;
        tbi.bmpIndex = TbIcon::Text;
        tbi.cmdId = shortcut->cmdId;
        tbi.toolTip = text;
        gCustomButtons[gCustomButtonsCount++] = tbi;
    }

    // add toolbar buttons from custom commands with ToolbarText (e.g. ExternalViewers)
    for (auto cc = gFirstCustomCommand; cc; cc = cc->next) {
        if (gCustomButtonsCount >= kMaxCustomButtons) {
            break;
        }
        const char* tbText = GetCommandStringArg(cc, kCmdArgToolbarText, nullptr);
        if (str::IsEmptyOrWhiteSpace(tbText)) {
            continue;
        }
        ToolbarButtonInfo tbi;
        tbi.bmpIndex = TbIcon::Text;
        tbi.cmdId = cc->id;
        tbi.toolTip = tbText;
        gCustomButtons[gCustomButtonsCount++] = tbi;
    }

    TBBUTTON* buttons = AllocArrayTemp<TBBUTTON>(gCustomButtonsCount);
    for (int i = 0; i < gCustomButtonsCount; i++) {
        ToolbarButtonInfo& tbi = gCustomButtons[i];
        buttons[i] = TbButtonFromButtonInfo(tbi, true);
    }
    SendMessageW(hwndToolbar, TB_ADDBUTTONS, gCustomButtonsCount, (LPARAM)buttons);
    SendMessageW(hwndToolbar, TB_SETBUTTONSIZE, 0, MAKELONG(iconSize, iconSize));
    SendMessageW(hwndToolbar, TB_AUTOSIZE, 0, 0);

    RECT rc;
    LRESULT res = SendMessageW(hwndToolbar, TB_GETITEMRECT, 0, (LPARAM)&rc);
    if (!res) {
        rc.left = rc.right = rc.top = rc.bottom = 0;
    }

    ShowWindow(hwndToolbar, SW_SHOW);

    REBARBANDINFOW rbBand{};
    rbBand.cbSize = sizeof(REBARBANDINFOW);
    rbBand.fMask = RBBIM_STYLE | RBBIM_CHILD | RBBIM_CHILDSIZE;
    rbBand.fStyle = RBBS_FIXEDSIZE;
    if (theme::IsAppThemed() && !ThemeUsesDarkChrome()) {
        rbBand.fStyle |= RBBS_CHILDEDGE;
    }
    rbBand.hbmBack = nullptr;
    rbBand.lpText = (WCHAR*)L"Toolbar"; // NOLINT
    rbBand.hwndChild = hwndToolbar;
    rbBand.cxMinChild = (rc.right - rc.left) * kButtonsCount;
    rbBand.cyMinChild = (rc.bottom - rc.top) + 2 * rc.top;
    rbBand.cx = 0;
    SendMessageW(win->hwndReBar, RB_INSERTBAND, (WPARAM)-1, (LPARAM)&rbBand);

    SetWindowPos(win->hwndReBar, nullptr, 0, 0, 0, 0, SWP_NOZORDER);

    int defFontSize = GetAppFontSize();
    // 18 was the default toolbar size, we want to scale the fonts in proportion
    int newSize = (defFontSize * gGlobalPrefs->toolbarSize) / kDefaultIconSize;
    int maxFontSize = iconSize - yPad * 2 - 2; // -2 determined empirically
    if (newSize > maxFontSize) {
        newSize = maxFontSize;
    }
    auto font = GetDefaultGuiFontOfSize(newSize);
    HwndSetFont(hwndToolbar, font);

    CreatePageBox(win, font, iconSize);
    if (!DefWndProcToolbar) {
        DefWndProcToolbar = (WNDPROC)GetWindowLongPtr(win->hwndToolbar, GWLP_WNDPROC);
    }
    SetWindowLongPtr(win->hwndToolbar, GWLP_WNDPROC, (LONG_PTR)WndProcToolbar);

    UpdateToolbarPageText(win, -1);
    UpdateToolbarFindText(win);
    UpdateThemeToolbarButton(win);
    UpdatePdfDocumentColorModeToolbarButton(win);
    UpdateDoubleClickWordLookupToolbarButton(win);
    ConfigureToolbarColors(hwndToolbar);
    InvalidateRect(hwndToolbar, nullptr, TRUE);
}

void ReCreateToolbar(MainWindow* win) {
    if (win->hwndReBar) {
        HwndDestroyWindowSafe(&win->hwndPageLabel);
        HwndDestroyWindowSafe(&win->hwndPageEdit);
        HwndDestroyWindowSafe(&win->hwndPageBg);
        HwndDestroyWindowSafe(&win->hwndPageTotal);
        HwndDestroyWindowSafe(&win->hwndToolbar);
        HwndDestroyWindowSafe(&win->hwndReBar);
    }
    CreateToolbar(win);
}

// --- Menu bar as rebar control (used when tabs are in titlebar) ---

static LRESULT CALLBACK MenuBarReBarWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass,
                                            DWORD_PTR dwRefData) {
    if (WM_ERASEBKGND == uMsg) {
        // always paint background with theme color to avoid gray strips in light theme
        HDC hdc = (HDC)wParam;
        RECT rect;
        GetClientRect(hWnd, &rect);
        COLORREF bgCol = ThemeChromeBackgroundColor();
        auto bgBrush = CreateSolidBrush(bgCol);
        FillRect(hdc, &rect, bgBrush);
        DeleteObject(bgBrush);
        return 1;
    }
    if (WM_NOTIFY == uMsg) {
        auto win = FindMainWindowByHwnd(hWnd);
        NMHDR* hdr = (NMHDR*)lParam;
        if (win && hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == win->hwndMenuToolbar) {
            NMTBCUSTOMDRAW* custDraw = (NMTBCUSTOMDRAW*)hdr;
            switch (custDraw->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT: {
                    auto col = ThemeWindowTextColor();
                    UINT itemState = custDraw->nmcd.uItemState;
                    if (itemState & CDIS_DISABLED) {
                        col = ThemeWindowTextDisabledColor();
                    }
                    custDraw->clrText = col;
                    return CDRF_DODEFAULT;
                }
            }
        }
    }
    if (WM_NCDESTROY == uMsg) {
        RemoveWindowSubclass(hWnd, MenuBarReBarWndProc, uIdSubclass);
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK MenuBarToolbarWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass,
                                              DWORD_PTR dwRefData) {
    if (WM_ERASEBKGND == uMsg) {
        // don't erase background here; toolbar paints its own background during WM_PAINT
        // filling here causes visible flicker (erase then paint) during window resize
        return 1;
    }
    if (WM_NCDESTROY == uMsg) {
        RemoveWindowSubclass(hWnd, MenuBarToolbarWndProc, uIdSubclass);
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

constexpr int kMenuBarCmdFirst = 50000;
constexpr int kMenuBarCmdLast = 50020;

struct MenuBarPopupNav {
    MainWindow* win = nullptr;
    HMENU rootMenu = nullptr;
    HMENU currentMenu = nullptr;
    UINT currentFlags = 0;
    int nextMenuIdx = -1;
};

static MenuBarPopupNav gMenuBarPopupNav;

// track when a menu popup was last dismissed so a second click on the same
// menu bar button closes the popup instead of immediately reopening it
static int gMenuBarLastDismissedIdx = -1;
static DWORD gMenuBarLastDismissedTick = 0;

static bool ShouldSwitchCustomMenuBarPopup(UINT vk) {
    if (!gMenuBarPopupNav.win || !gMenuBarPopupNav.rootMenu) {
        return false;
    }
    if (!gMenuBarPopupNav.currentMenu || gMenuBarPopupNav.currentMenu != gMenuBarPopupNav.rootMenu) {
        return false;
    }
    if (bit::IsMaskSet(gMenuBarPopupNav.currentFlags, (UINT)MF_POPUP)) {
        return false;
    }

    int menuCount = GetMenuItemCount(gMenuBarPopupNav.win->menu);
    if (menuCount <= 1) {
        return false;
    }

    int step = 0;
    if (vk == VK_LEFT) {
        step = -1;
    } else if (vk == VK_RIGHT) {
        step = 1;
    }
    if (step == 0) {
        return false;
    }

    gMenuBarPopupNav.nextMenuIdx += step;
    if (gMenuBarPopupNav.nextMenuIdx < 0) {
        gMenuBarPopupNav.nextMenuIdx = menuCount - 1;
    } else if (gMenuBarPopupNav.nextMenuIdx >= menuCount) {
        gMenuBarPopupNav.nextMenuIdx = 0;
    }
    return true;
}

// check if mouse is over a different toolbar button and switch to it
static bool ShouldSwitchMenuBarOnMouseMove() {
    if (!gMenuBarPopupNav.win || !gMenuBarPopupNav.win->hwndMenuToolbar) {
        return false;
    }
    HWND hwndTb = gMenuBarPopupNav.win->hwndMenuToolbar;

    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwndTb, &pt);

    // hit-test the toolbar
    int btnCount = (int)SendMessageW(hwndTb, TB_BUTTONCOUNT, 0, 0);
    for (int i = 0; i < btnCount; i++) {
        RECT rc;
        SendMessageW(hwndTb, TB_GETITEMRECT, i, (LPARAM)&rc);
        if (PtInRect(&rc, pt)) {
            TBBUTTON tb{};
            SendMessageW(hwndTb, TB_GETBUTTON, i, (LPARAM)&tb);
            int menuIdx = tb.idCommand - kMenuBarCmdFirst;
            if (menuIdx != gMenuBarPopupNav.nextMenuIdx) {
                gMenuBarPopupNav.nextMenuIdx = menuIdx;
                return true;
            }
            return false;
        }
    }
    return false;
}

static LRESULT CALLBACK MenuBarMsgFilterHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == MSGF_MENU && gMenuBarPopupNav.win) {
        MSG* msg = (MSG*)lParam;
        if ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) &&
            ShouldSwitchCustomMenuBarPopup((UINT)msg->wParam)) {
            EndMenu();
            return 1;
        }
        if (msg->message == WM_MOUSEMOVE && ShouldSwitchMenuBarOnMouseMove()) {
            EndMenu();
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void UpdateCustomMenuBarMenuSelect(MainWindow* win, WPARAM wp, LPARAM lp) {
    if (gMenuBarPopupNav.win != win) {
        return;
    }

    UINT flags = HIWORD(wp);
    HMENU menu = (HMENU)lp;
    if (flags == 0xFFFF && !menu) {
        gMenuBarPopupNav.currentMenu = nullptr;
        gMenuBarPopupNav.currentFlags = 0;
        return;
    }

    gMenuBarPopupNav.currentMenu = menu;
    gMenuBarPopupNav.currentFlags = flags;
}

void RebuildMenuBarButtons(MainWindow* win) {
    HWND hwndMb = win->hwndMenuToolbar;
    if (!hwndMb) {
        return;
    }

    // remove existing buttons
    while (SendMessageW(hwndMb, TB_DELETEBUTTON, 0, 0)) {
    }

    HMENU menu = win->menu;
    int count = GetMenuItemCount(menu);
    if (count <= 0) {
        return;
    }

    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(MENUITEMINFOW);
    mii.fMask = MIIM_SUBMENU | MIIM_STRING;

    for (int i = 0; i < count && i < (kMenuBarCmdLast - kMenuBarCmdFirst); i++) {
        mii.dwTypeData = nullptr;
        mii.cch = 0;
        GetMenuItemInfoW(menu, i, TRUE, &mii);
        if (!mii.hSubMenu || !mii.cch) {
            continue;
        }
        mii.cch++;
        AutoFreeWStr name(AllocArray<WCHAR>(mii.cch));
        mii.dwTypeData = name;
        GetMenuItemInfoW(menu, i, TRUE, &mii);

        LRESULT strIdx = SendMessageW(hwndMb, TB_ADDSTRING, 0, (LPARAM)name.Get());

        TBBUTTON b{};
        b.iBitmap = I_IMAGENONE;
        b.idCommand = kMenuBarCmdFirst + i;
        b.fsState = TBSTATE_ENABLED;
        b.fsStyle = BTNS_AUTOSIZE | BTNS_SHOWTEXT;
        b.iString = (INT_PTR)strIdx;
        SendMessageW(hwndMb, TB_ADDBUTTONS, 1, (LPARAM)&b);
    }
}

void CreateMenuBarRebar(MainWindow* win) {
    if (win->hwndMenuReBar) {
        return;
    }

    bool isRtl = IsUIRtl();
    HINSTANCE hinst = GetModuleHandle(nullptr);
    HWND hwndParent = win->hwndFrame;

    // create hidden; caller shows after RelayoutWindow positions it
    // no WS_BORDER (avoids 1px gap) and no RBS_BANDBORDERS (avoids gray band separators)
    DWORD style = WS_CHILD | WS_CLIPCHILDREN | RBS_VARHEIGHT;
    style |= CCS_NODIVIDER | CCS_NOPARENTALIGN;
    DWORD exStyle = WS_EX_TOOLWINDOW;
    if (isRtl) {
        exStyle |= WS_EX_LAYOUTRTL;
    }

    win->hwndMenuReBar = CreateWindowExW(exStyle, REBARCLASSNAME, nullptr, style, 0, 0, 0, 0, hwndParent,
                                         (HMENU)IDC_MENUBAR_REBAR, hinst, nullptr);
    SetWindowSubclass(win->hwndMenuReBar, MenuBarReBarWndProc, 0, 0);

    REBARINFO rbi{};
    rbi.cbSize = sizeof(REBARINFO);
    SendMessageW(win->hwndMenuReBar, RB_SETBARINFO, 0, (LPARAM)&rbi);
    SendMessageW(win->hwndMenuReBar, RB_SETBKCOLOR, 0, ThemeChromeBackgroundColor());

    style = WS_CHILD | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_LIST;
    style |= CCS_NODIVIDER | CCS_NOPARENTALIGN;
    exStyle = 0;
    if (isRtl) {
        exStyle |= WS_EX_LAYOUTRTL;
    }

    win->hwndMenuToolbar = CreateWindowExW(exStyle, TOOLBARCLASSNAME, nullptr, style, 0, 0, 0, 0, win->hwndMenuReBar,
                                           (HMENU)IDC_MENUBAR, hinst, nullptr);
    SetWindowSubclass(win->hwndMenuToolbar, MenuBarToolbarWndProc, 0, 0);
    SendMessageW(win->hwndMenuToolbar, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);

    if (!UseDarkModeLib() || !DarkMode::isEnabled()) {
        if (ThemeUsesDarkChrome()) {
            SetWindowTheme(win->hwndMenuToolbar, L"", L"");
        }
    }

    HFONT font = GetAppMenuFontForHwnd(win->hwndFrame);
    HwndSetFont(win->hwndMenuToolbar, font);

    LRESULT tbExStyle = SendMessageW(win->hwndMenuToolbar, TB_GETEXTENDEDSTYLE, 0, 0);
    tbExStyle |= TBSTYLE_EX_MIXEDBUTTONS;
    SendMessageW(win->hwndMenuToolbar, TB_SETEXTENDEDSTYLE, 0, tbExStyle);

    RebuildMenuBarButtons(win);

    RECT rc;
    LRESULT res = SendMessageW(win->hwndMenuToolbar, TB_GETITEMRECT, 0, (LPARAM)&rc);
    if (!res) {
        rc.left = rc.right = rc.top = rc.bottom = 0;
    }

    ShowWindow(win->hwndMenuToolbar, SW_SHOW);

    REBARBANDINFOW rbBand{};
    rbBand.cbSize = sizeof(REBARBANDINFOW);
    rbBand.fMask = RBBIM_STYLE | RBBIM_CHILD | RBBIM_CHILDSIZE;
    rbBand.fStyle = RBBS_FIXEDSIZE;
    rbBand.hwndChild = win->hwndMenuToolbar;
    rbBand.cxMinChild = 0;
    rbBand.cyMinChild = (rc.bottom - rc.top) + 2 * rc.top;
    rbBand.cx = 0;
    SendMessageW(win->hwndMenuReBar, RB_INSERTBAND, (WPARAM)-1, (LPARAM)&rbBand);

    if (UseDarkModeLib()) {
        DarkMode::setWindowNotifyCustomDrawSubclass(win->hwndMenuReBar);
        DarkMode::setChildCtrlsSubclassAndTheme(win->hwndMenuReBar);
    }
}

void ShowMenuBarRebar(MainWindow* win) {
    if (win->hwndMenuReBar) {
        ShowWindow(win->hwndMenuReBar, SW_SHOW);
    }
}

void DestroyMenuBarRebar(MainWindow* win) {
    HwndDestroyWindowSafe(&win->hwndMenuToolbar);
    HwndDestroyWindowSafe(&win->hwndMenuReBar);
}

bool IsShowingMenuBarRebar(MainWindow* win) {
    if (!win->hwndMenuReBar) {
        return false;
    }
    if (win->presentation) {
        return false;
    }
    return true;
}

bool HandleMenuBarCommand(MainWindow* win, int cmdId) {
    if (cmdId < kMenuBarCmdFirst || cmdId >= kMenuBarCmdLast) {
        return false;
    }
    if (!win->hwndMenuToolbar) {
        return false;
    }

    int menuCount = GetMenuItemCount(win->menu);
    int menuIdx = cmdId - kMenuBarCmdFirst;

    // if same button was clicked shortly after dismissing its popup, treat as toggle-close
    DWORD now = GetTickCount();
    if (menuIdx == gMenuBarLastDismissedIdx && (now - gMenuBarLastDismissedTick) < 500) {
        gMenuBarLastDismissedIdx = -1;
        return true;
    }

    UINT flags = TPM_LEFTALIGN | TPM_TOPALIGN;
    if (IsUIRtl()) {
        flags = TPM_RIGHTALIGN | TPM_TOPALIGN;
    }

    for (;;) {
        HMENU subMenu = GetSubMenu(win->menu, menuIdx);
        if (!subMenu) {
            return true;
        }

        // get button rect in screen coordinates
        RECT btnRect{};
        int btnCmdId = kMenuBarCmdFirst + menuIdx;
        int btnIdx = (int)SendMessageW(win->hwndMenuToolbar, TB_COMMANDTOINDEX, btnCmdId, 0);
        SendMessageW(win->hwndMenuToolbar, TB_GETITEMRECT, btnIdx, (LPARAM)&btnRect);
        MapWindowPoints(win->hwndMenuToolbar, nullptr, (POINT*)&btnRect, 2);
        Rect anchor = ToRect(btnRect);
        Rect exclude = anchor;
        exclude.Inflate(DpiScale(win->hwndFrame, 4), DpiScale(win->hwndFrame, 4));
        TPMPARAMS tpm{};
        tpm.cbSize = sizeof(TPMPARAMS);
        tpm.rcExclude = ToRECT(exclude);

        gMenuBarPopupNav.win = win;
        gMenuBarPopupNav.rootMenu = subMenu;
        gMenuBarPopupNav.currentMenu = subMenu;
        gMenuBarPopupNav.currentFlags = 0;
        gMenuBarPopupNav.nextMenuIdx = menuIdx;

        MarkMenuOwnerDraw(subMenu, false);
        SetForegroundWindow(win->hwndFrame);
        HHOOK hook = SetWindowsHookExW(WH_MSGFILTER, MenuBarMsgFilterHook, nullptr, GetCurrentThreadId());
        TrackPopupMenuEx(subMenu, flags, anchor.x, anchor.y + anchor.dy, win->hwndFrame, &tpm);
        PostMessageW(win->hwndFrame, WM_NULL, 0, 0);
        if (hook) {
            UnhookWindowsHookEx(hook);
        }

        int nextMenuIdx = gMenuBarPopupNav.nextMenuIdx;
        gMenuBarPopupNav = {};
        if (nextMenuIdx == menuIdx || menuCount <= 1) {
            gMenuBarLastDismissedIdx = menuIdx;
            gMenuBarLastDismissedTick = GetTickCount();
            break;
        }
        menuIdx = nextMenuIdx;
    }

    return true;
}

// Activate a menu bar button by accelerator key (Alt+letter).
// If accel is 0, activate the first menu item.
// Returns true if handled.
bool ActivateMenuBarByAccel(MainWindow* win, WCHAR accel) {
    if (!win->hwndMenuToolbar || !win->menu) {
        return false;
    }

    int count = GetMenuItemCount(win->menu);
    if (count <= 0) {
        return false;
    }

    // if accel is 0 (bare Alt press), open the first menu
    if (accel == 0) {
        return HandleMenuBarCommand(win, kMenuBarCmdFirst);
    }

    // normalize to uppercase for matching
    if (accel >= 'a' && accel <= 'z') {
        accel -= 'a' - 'A';
    }

    // find the menu item whose text has &<accel>
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(MENUITEMINFOW);
    mii.fMask = MIIM_STRING;

    for (int i = 0; i < count && i < (kMenuBarCmdLast - kMenuBarCmdFirst); i++) {
        mii.dwTypeData = nullptr;
        mii.cch = 0;
        GetMenuItemInfoW(win->menu, i, TRUE, &mii);
        if (!mii.cch) {
            continue;
        }
        mii.cch++;
        AutoFreeWStr name(AllocArray<WCHAR>(mii.cch));
        mii.dwTypeData = name;
        GetMenuItemInfoW(win->menu, i, TRUE, &mii);

        // look for &X where X matches accel
        for (WCHAR* p = name.Get(); *p; p++) {
            if (*p == '&' && p[1]) {
                WCHAR ch = p[1];
                if (ch >= 'a' && ch <= 'z') {
                    ch -= 'a' - 'A';
                }
                if (ch == accel) {
                    return HandleMenuBarCommand(win, kMenuBarCmdFirst + i);
                }
                break;
            }
        }
    }

    return false;
}
