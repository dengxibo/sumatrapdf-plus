/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "AppSettings.h"
#include "DocController.h"
#include "Annotation.h"
#include "EbookFontMenu.h"
#include "EngineBase.h"
#include "EngineMupdf.h"
#include "Translations.h"
#include "SumatraConfig.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "EbookAnnotations.h"
#include "EditAnnotations.h"
#include "EditEbookAnnotations.h"
#include "SumatraPDF.h"
#include "SidebarThumbs.h"
#include "Theme.h"
#include "Toolbar.h"
#include "AppDialogTheme.h"

#include "DarkModeSubclass.h"
#include "AnnotationSidebar.h"

// Same as PDF annotations: DarkModeLib paints channel/thumb but leaves a white
// client body. Fill the page color and skip that default PREPAINT.
constexpr UINT_PTR kEbookAnnotTrackbarBgNotifyId = 0xA11F;

static COLORREF EbookAnnotTrackbarPanelColor() {
    return ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor() : ThemeWindowControlBackgroundColor();
}

static LRESULT CALLBACK EbookAnnotTrackbarBgNotifyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                                       DWORD_PTR) {
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, EbookAnnotTrackbarBgNotifyProc, id);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_NOTIFY) {
        auto* hdr = (NMHDR*)lp;
        if (hdr && hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom) {
            WCHAR cls[64]{};
            if (GetClassNameW(hdr->hwndFrom, cls, dimof(cls)) > 0 && str::EqI(cls, TRACKBAR_CLASS)) {
                auto* cd = (LPNMCUSTOMDRAW)lp;
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    SetWindowLongPtrW(hdr->hwndFrom, GWL_STYLE,
                                      GetWindowLongPtrW(hdr->hwndFrom, GWL_STYLE) | TBS_TRANSPARENTBKGND);
                    RECT rc{};
                    GetClientRect(hdr->hwndFrom, &rc);
                    ScopedGdiObj<HBRUSH> br(CreateSolidBrush(EbookAnnotTrackbarPanelColor()));
                    FillRect(cd->hdc, &rc, br);
                    return CDRF_NOTIFYITEMDRAW | CDRF_SKIPDEFAULT;
                }
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

struct EbookAnnotationsWindow : Wnd {
    WindowTab* tab = nullptr;
    LayoutBase* mainLayout = nullptr;
    AnnotInspectorPane* inspectorPane = nullptr;
    ILayout* footerLayout = nullptr;
    AnnotHeadingLine* staticHeading = nullptr;
    AnnotHeadingLine* staticMeta = nullptr;
    Button* buttonSaveStatus = nullptr;
    Button* buttonSaveCopy = nullptr;
    ListBox* listBox = nullptr;
    Static* staticContents = nullptr;
    AnnotNoteEdit* editContents = nullptr;
    Static* staticColor = nullptr;
    DropDown* dropDownColor = nullptr;
    Static* staticOpacity = nullptr;
    Trackbar* trackbarOpacity = nullptr;
    Static* staticTextAlignment = nullptr;
    DropDown* dropDownTextAlignment = nullptr;
    Static* staticTextFont = nullptr;
    Button* buttonTextFont = nullptr;
    Static* staticTextSize = nullptr;
    Trackbar* trackbarTextSize = nullptr;
    AnnotResetButton* buttonRestoreFreeText = nullptr;
    HBox* resetAppearanceRow = nullptr;
    Static* staticTextColor = nullptr;
    DropDown* dropDownTextColor = nullptr;
    Static* staticBorderColor = nullptr;
    DropDown* dropDownBorderColor = nullptr;
    Static* staticBorder = nullptr;
    Trackbar* trackbarBorder = nullptr;
    Static* staticLineStart = nullptr;
    AnnotIconDropDown* dropDownLineStart = nullptr;
    Static* staticLineEnd = nullptr;
    AnnotIconDropDown* dropDownLineEnd = nullptr;
    Static* staticInteriorColor = nullptr;
    DropDown* dropDownInteriorColor = nullptr;
    Static* staticIcon = nullptr;
    AnnotIconDropDown* dropDownIcon = nullptr;
    Button* buttonExport = nullptr;
    Vec<EbookAnnotation*> annotations;
    StrVec annotationExcerpts;
    int annotationLocationWidth = 0;
    int excerptPageCount = -1;
    DWORD excerptRefreshTime = 0;
    bool excerptLoading = true;
    EbookAnnotation* selected = nullptr;
    bool updatingControls = false;
    StrBuilder currCustomColor;
    StrBuilder currTextColor;
    StrBuilder currBorderColor;
    HFONT headingFont = nullptr;
    int dpi = 0;

    void OnSize(UINT msg, UINT type, SIZE size) override;
    void OnFocus() override;
    bool PreTranslateMessage(MSG& msg) override;
    ~EbookAnnotationsWindow() override;
};

static Static* CreateStatic(HWND parent, HFONT font, const char* text = nullptr) {
    auto control = new Static();
    Static::CreateArgs args;
    args.parent = parent;
    args.text = text;
    args.isRtl = IsUIRtl();
    args.font = font;
    ReportIf(!control->Create(args));
    return control;
}

static void LayoutEbookAnnotationsToClient(EbookAnnotationsWindow* window) {
    if (!window || !window->mainLayout || !window->hwnd) {
        return;
    }
    Rect client = ClientRect(window->hwnd);
    if (client.dx > 0 && client.dy > 0) {
        LayoutAnnotationSidebarControls(window->mainLayout, window->inspectorPane, {client.dx, client.dy});
    }
}

// Hiding a child window doesn't erase the area it previously occupied.
// Annotation types expose different sets of controls, so switching between
// them can otherwise leave the old controls painted behind the new layout.
static void RedrawAnnotationDetailPanel(EbookAnnotationsWindow* window) {
    if (!window || !window->hwnd) {
        return;
    }
    RedrawWindow(window->hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
}

static void RenderEbookIconPreview(EbookAnnotationsWindow* window, AnnotationIconPreviewRequest* request) {
    EbookAnnotation* annotation = window->selected;
    if (!annotation) return;
    EngineMupdf* engine = AsEngineMupdf(window->tab->GetEngine());
    int pageNo = EbookAnnotationGetPageNo(window->tab, annotation);
    if (!engine || pageNo < 1) return;
    request->bitmap = RenderAnnotationIconPreviewBitmap(engine, pageNo, EbookAnnotationGetType(annotation),
                                                        EbookAnnotationGetColor(annotation), request->name,
                                                        request->size, request->lineEnding, request->lineStart);
}

static void PrepareEbookIconPreviews(EbookAnnotationsWindow* window, AnnotIconDropDown* control) {
    if (!window->selected || !control) return;
    AnnotationType type = EbookAnnotationGetType(window->selected);
    if (control->lineEndPreview ? type != AnnotationType::Line
                                : (type != AnnotationType::Text && type != AnnotationType::Stamp))
        return;
    control->renderPreview = MkFunc1(RenderEbookIconPreview, window);
    control->PreparePreviews(EbookAnnotationGetType(window->selected), EbookAnnotationGetColor(window->selected));
}

static void UpdateEbookSaveStatus(EbookAnnotationsWindow* window) {
    if (!window || !window->buttonSaveStatus) return;
    bool pending = EbookAnnotationsHasUnsavedChanges(window->tab) || EbookAnnotationsLastSaveFailed(window->tab);
    window->buttonSaveStatus->SetText(_TRA("Save"));
    window->buttonSaveStatus->SetIsEnabled(pending);
    HWND status = window->buttonSaveStatus->hwnd;
    RemoveWindowSubclass(status, AnnotationSecondaryButtonProc, 0xA120);
    InvalidateRect(status, nullptr, FALSE);
}

static bool RecordEbookMutation(EbookAnnotationsWindow* window, bool saved) {
    UpdateEbookSaveStatus(window);
    if (saved && window->selected) {
        PrepareEbookIconPreviews(window, window->dropDownIcon);
        PrepareEbookIconPreviews(window, window->dropDownLineStart);
        PrepareEbookIconPreviews(window, window->dropDownLineEnd);
    }
    return saved;
}

static void RefreshAnnotationDetailPanel(EbookAnnotationsWindow* window) {
    UpdateEbookSaveStatus(window);
    LayoutEbookAnnotationsToClient(window);
    RedrawAnnotationDetailPanel(window);
}

static void GetEditAnnotationsThemeColors(COLORREF& textOut, COLORREF& bgOut) {
    // Match PDF annotations / Options dialog page colors.
    textOut = ThemeWindowTextColor();
    bgOut = ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor() : ThemeWindowControlBackgroundColor();
}

static void ApplyEbookAnnotationsWindowTheme(EbookAnnotationsWindow* window, bool installDarkMode) {
    if (!window || !window->hwnd) {
        return;
    }
    COLORREF text = 0;
    COLORREF bg = 0;
    GetEditAnnotationsThemeColors(text, bg);
    window->SetColors(text, bg);
    struct Colors {
        COLORREF text;
        COLORREF bg;
    } colors{text, bg};
    EnumChildWindows(
        window->hwnd,
        [](HWND hwnd, LPARAM lparam) -> BOOL {
            auto* c = (Colors*)lparam;
            Wnd* wnd = WndListFindByHwnd(hwnd);
            if (wnd) {
                wnd->SetColors(c->text, c->bg);
            }
            return TRUE;
        },
        (LPARAM)&colors);
    window->editContents->SetColors(text, ThemeAnnotationContentsEditBackgroundColor());
    UpdateAnnotationContentsEditChrome(window->editContents, window->editContents);
    if (window->listBox) {
        window->listBox->SetColors(text, bg);
    }

    if (UseDarkModeLib()) {
        if (ThemeUsesDarkChrome()) {
            DarkMode::setDarkWndNotifySafe(window->hwnd);
            DarkMode::setWindowEraseBgSubclass(window->hwnd);
            DarkMode::setChildCtrlsSubclassAndTheme(window->hwnd);
        } else if (installDarkMode) {
            DarkMode::setDarkWndNotifySafe(window->hwnd);
            DarkMode::setWindowEraseBgSubclass(window->hwnd);
        } else {
            DarkMode::setWindowCtlColorSubclass(window->hwnd);
            DarkMode::setChildCtrlsTheme(window->hwnd);
        }
    }
    UpdateWindowCaptionTheme(window->hwnd);
    // Same push/combo chrome as PDF annotations and Options.
    AppDialogSyncWarmPushButtons(window->hwnd);
    RemoveWindowSubclass(window->buttonSaveCopy->hwnd, AnnotationSecondaryButtonProc, 0xA11D);
    if (window->inspectorPane) {
        HWND pane = window->inspectorPane->hwnd;
        if (UseDarkModeLib() && ThemeUsesDarkChrome()) {
            DarkMode::setWindowNotifyCustomDrawSubclass(pane);
            DarkMode::setWindowCtlColorSubclass(pane);
            DarkMode::setChildCtrlsSubclassAndTheme(pane);
        }
        // Light-White: same grey-band fix as PDF annotation Border trackbar.
        RemoveWindowSubclass(pane, EbookAnnotTrackbarBgNotifyProc, kEbookAnnotTrackbarBgNotifyId);
        SetWindowSubclass(pane, EbookAnnotTrackbarBgNotifyProc, kEbookAnnotTrackbarBgNotifyId, 0);
    }

    uint flags = RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN;
    RedrawWindow(window->hwnd, nullptr, nullptr, flags);
}

void RefreshEbookAnnotationsWindowsTheme() {
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            if (tab->editEbookAnnotsWindow) {
                ApplyEbookAnnotationsWindowTheme(tab->editEbookAnnotsWindow, false);
            }
        }
    }
}

void DockOpenEbookAnnotationsWindows(MainWindow*) {
    // The editor is a child of the sidebar. Frame layout places it.
}

HWND EbookAnnotationsSidebarHwnd(WindowTab* tab) {
    if (!tab || !tab->editEbookAnnotsWindow) {
        return nullptr;
    }
    return tab->editEbookAnnotsWindow->hwnd;
}

void SyncEbookAnnotationsSidebar(MainWindow* win, bool show) {
    if (!win) {
        return;
    }
    WindowTab* cur = win->CurrentTab();
    for (WindowTab* tab : win->Tabs()) {
        EbookAnnotationsWindow* window = tab->editEbookAnnotsWindow;
        if (!window || !window->hwnd) {
            continue;
        }
        bool on = show && tab == cur;
        ShowWindow(window->hwnd, on ? SW_SHOWNA : SW_HIDE);
    }
}

void CloseEbookAnnotationsWindowsForDpiMove(MainWindow* win) {
    if (!win) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        if (tab->editEbookAnnotsWindow) {
            tab->reopenEbookAnnotsAfterDpiMove = true;
            CloseAndDeleteEditEbookAnnotationsWindow(tab);
        }
    }
}

void ReopenEbookAnnotationsWindowsAfterDpiMove(MainWindow* win) {
    if (!win) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        if (!tab->reopenEbookAnnotsAfterDpiMove) {
            continue;
        }
        tab->reopenEbookAnnotsAfterDpiMove = false;
        ShowEditEbookAnnotationsWindow(tab, nullptr, EditAnnotFocus::Default, false);
    }
}

static void FillColorDropDown(EbookAnnotationsWindow* window, COLORREF color) {
    FillAnnotationColorDropDown(window->dropDownColor, color, window->currCustomColor);
}

static COLORREF GetSelectedColor(EbookAnnotationsWindow* window) {
    int idx = window->dropDownColor->GetCurrentSelection();
    if (idx < 0) {
        AnnotationType type = AnnotationType::Highlight;
        if (window->selected) {
            type = EbookAnnotationGetType(window->selected);
        }
        return GetDefaultAnnotationColor(type);
    }
    const char* item = window->dropDownColor->items.At(idx);
    return GetAnnotationColorFromDropDown(item);
}

static void HideAnnotationControls(EbookAnnotationsWindow* window) {
    window->staticHeading->SetIsVisible(false);
    window->staticMeta->SetIsVisible(false);
    window->staticContents->SetIsVisible(false);
    window->editContents->SetIsVisible(false);
    window->staticColor->SetIsVisible(false);
    window->dropDownColor->SetIsVisible(false);
    window->staticOpacity->SetIsVisible(false);
    window->trackbarOpacity->SetIsVisible(false);
    window->staticTextAlignment->SetIsVisible(false);
    window->dropDownTextAlignment->SetIsVisible(false);
    window->staticTextFont->SetIsVisible(false);
    if (window->buttonTextFont) {
        window->buttonTextFont->SetIsVisible(false);
    }
    window->staticTextSize->SetIsVisible(false);
    window->trackbarTextSize->SetIsVisible(false);
    if (window->buttonRestoreFreeText) {
        window->buttonRestoreFreeText->SetIsVisible(false);
    }
    if (window->resetAppearanceRow) {
        window->resetAppearanceRow->SetVisibility(Visibility::Collapse);
    }
    window->staticTextColor->SetIsVisible(false);
    window->dropDownTextColor->SetIsVisible(false);
    if (window->staticBorderColor) {
        window->staticBorderColor->SetIsVisible(false);
    }
    if (window->dropDownBorderColor) {
        window->dropDownBorderColor->SetIsVisible(false);
    }
    window->staticBorder->SetIsVisible(false);
    window->trackbarBorder->SetIsVisible(false);
    window->staticLineStart->SetIsVisible(false);
    window->dropDownLineStart->SetIsVisible(false);
    window->staticLineEnd->SetIsVisible(false);
    window->dropDownLineEnd->SetIsVisible(false);
    window->staticInteriorColor->SetIsVisible(false);
    window->dropDownInteriorColor->SetIsVisible(false);
    window->staticIcon->SetIsVisible(false);
    window->dropDownIcon->SetIsVisible(false);
}

static void ClearAnnotationDetailControls(EbookAnnotationsWindow* window) {
    if (!window) {
        return;
    }
    window->selected = nullptr;
    window->updatingControls = true;
    if (window->editContents) {
        window->editContents->SetText("");
    }
    window->updatingControls = false;
    HideAnnotationControls(window);
}

static void UpdateExportButton(EbookAnnotationsWindow* window) {
    bool hasAnnotations = window->listBox && window->listBox->GetCount() > 0;
    window->buttonExport->SetIsEnabled(hasAnnotations);
}

static bool IsEbookAnnotContentsEditActive(HWND msgHwnd, HWND editHwnd, HWND windowHwnd) {
    if (!editHwnd) {
        return false;
    }
    auto relatedToEdit = [&](HWND h) -> bool { return h && (h == editHwnd || ::IsChild(editHwnd, h)); };
    if (relatedToEdit(msgHwnd) || relatedToEdit(::GetFocus())) {
        return true;
    }
    HWND focus = ::GetFocus();
    if (focus && windowHwnd && ::IsChild(windowHwnd, focus)) {
        TempStr cls = HwndGetClassName(focus);
        if (str::EqI(cls, "Edit")) {
            return true;
        }
    }
    return false;
}

bool IsEbookAnnotContentsEditFocused(HWND msgHwnd) {
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            EbookAnnotationsWindow* ew = tab->editEbookAnnotsWindow;
            if (!ew || !ew->editContents) {
                continue;
            }
            if (IsEbookAnnotContentsEditActive(msgHwnd, ew->editContents->hwnd, ew->hwnd)) {
                return true;
            }
        }
    }
    return false;
}

static void FlushContentsFromEdit(EbookAnnotationsWindow* window) {
    if (!window || !window->editContents || window->updatingControls || !window->selected) {
        return;
    }
    TempStr note = window->editContents->GetTextTemp();
    note = str::ReplaceTemp(note, "\r\n", "\n");
    RecordEbookMutation(window, EbookAnnotationSetNote(window->tab, window->selected, note));
    UpdateEbookSaveStatus(window);
}

static void DoIcon(EbookAnnotationsWindow* window, EbookAnnotation* annotation) {
    const char* itemName = EbookAnnotationGetIcon(annotation);
    const char* items = nullptr;
    switch (EbookAnnotationGetType(annotation)) {
        case AnnotationType::Text:
            items = gAnnotationTextIcons;
            break;
        case AnnotationType::Stamp:
            items = gStampIcons;
            break;
        default:
            break;
    }
    if (!items || str::IsEmpty(itemName)) {
        return;
    }
    window->dropDownIcon->SetItemsSeqStrings(items);
    int idx = seqstrings::StrToIdxIS(items, itemName);
    if (idx < 0 && EbookAnnotationGetType(annotation) == AnnotationType::Stamp) {
        idx = seqstrings::StrToIdxIS(items, "Draft");
    }
    window->dropDownIcon->SetCurrentSelection(idx < 0 ? -1 : idx);
    PrepareEbookIconPreviews(window, window->dropDownIcon);
    window->staticIcon->SetIsVisible(true);
    window->dropDownIcon->SetIsVisible(true);
}

static void UpdateSelectedAnnotation(EbookAnnotationsWindow* window, EbookAnnotation* annotation,
                                     EditAnnotFocus focus = EditAnnotFocus::Default, bool navigate = true) {
    InspectorUpdateLock hold(window->hwnd, window->inspectorPane ? window->inspectorPane->hwnd : nullptr);
    if (window->selected != annotation) EndFreeTextInPlaceEditForTab(window->tab, true);

    WindowTab* tab = window->tab;
    if (window->selected != annotation) {
        FlushContentsFromEdit(window);
    }
    window->selected = annotation;
    if (!annotation) {
        tab->selectedEbookAnnotation = nullptr;
        if (window->listBox && window->listBox->GetCurrentSelection() >= 0) {
            window->updatingControls = true;
            window->listBox->SetCurrentSelection(-1);
            window->updatingControls = false;
        }
        ClearAnnotationDetailControls(window);
        hold.erase = true;
        RefreshAnnotationDetailPanel(window);
        if (tab->win) {
            RefreshAnnotationOverlay(tab->win);
        }
        return;
    }

    int idx = window->annotations.Find(annotation);
    if (idx < 0) {
        tab->selectedEbookAnnotation = nullptr;
        ClearAnnotationDetailControls(window);
        hold.erase = true;
        RefreshAnnotationDetailPanel(window);
        if (tab->win) {
            RefreshAnnotationOverlay(tab->win);
        }
        return;
    }

    window->updatingControls = true;
    Vec<HWND> visHwnds;
    Vec<u8> visBefore;
    CaptureInspectorChildVis(window->inspectorPane ? window->inspectorPane->hwnd : nullptr, visHwnds, visBefore);
    HideAnnotationControls(window);

    TempStr note = str::ReplaceTemp(EbookAnnotationGetNote(annotation), "\r\n", "\n");
    note = str::ReplaceTemp(note, "\n", "\r\n");
    window->editContents->SetText(note);
    FillColorDropDown(window, EbookAnnotationGetColor(annotation));

    const char* author = EbookAnnotationGetAuthor(annotation);
    window->staticHeading->SetParts(
        str::FormatTemp("%s · §%d",
                        trans::GetTranslation(AnnotationReadableNameTemp(EbookAnnotationGetType(annotation))),
                        EbookAnnotationGetChapter(annotation) + 1),
        nullptr);
    window->staticHeading->SetIsVisible(false);
    time_t date = EbookAnnotationGetModified(annotation);
    if (date <= 0) date = EbookAnnotationGetCreated(annotation);
    char buf[100]{};
    if (date > 0) {
        struct tm tm;
        gmtime_s(&tm, &date);
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
    }
    window->staticMeta->SetParts(author ? author : "", buf);
    window->staticMeta->SetIsVisible(!str::IsEmpty(author) || date > 0);

    window->staticContents->SetIsVisible(true);
    window->editContents->SetIsVisible(true);
    if (EbookAnnotationGetType(annotation) == AnnotationType::FreeText) {
        constexpr const char* quadding = "Left\0Center\0Right\0";
        window->dropDownTextAlignment->SetItemsSeqStrings(quadding);
        window->dropDownTextAlignment->SetCurrentSelection(EbookAnnotationGetFreeTextAlignment(annotation));
        window->buttonTextFont->SetText(FreeTextFontLabel(EbookAnnotationGetFreeTextFont(annotation)));
        int textSize = EbookAnnotationGetFreeTextSize(annotation);
        window->staticTextSize->SetText(str::FormatTemp(_TRA("Text Size: %d"), textSize));
        window->trackbarTextSize->SetValue(textSize);
        FillAnnotationColorDropDown(window->dropDownTextColor, EbookAnnotationGetColor(annotation),
                                    window->currTextColor);
        int border = EbookAnnotationGetFreeTextBorderWidth(annotation);
        window->staticBorder->SetText(str::FormatTemp(_TRA("Border: %d"), border));
        window->trackbarBorder->SetValue(border);
        COLORREF background = 0;
        if (EbookAnnotationGetFreeTextBackground(annotation, &background)) {
            FillColorDropDown(window, background);
        } else {
            window->dropDownColor->SetItemsSeqStrings(GetPdfAnnotationColorNames());
            window->dropDownColor->SetCurrentSelection(0);
        }
        window->staticColor->SetText(_TRA("Background Color:"));
        window->staticTextAlignment->SetIsVisible(true);
        window->dropDownTextAlignment->SetIsVisible(true);
        window->staticTextFont->SetIsVisible(true);
        window->buttonTextFont->SetIsVisible(true);
        window->staticTextSize->SetIsVisible(true);
        window->trackbarTextSize->SetIsVisible(true);
        window->staticTextColor->SetIsVisible(true);
        window->dropDownTextColor->SetIsVisible(true);
        if (window->dropDownBorderColor) {
            FillAnnotationColorDropDown(window->dropDownBorderColor,
                                        EbookAnnotationGetFreeTextBorderColor(annotation), window->currBorderColor);
            window->staticBorderColor->SetIsVisible(true);
            window->dropDownBorderColor->SetIsVisible(true);
        }
        window->staticBorder->SetIsVisible(true);
        window->trackbarBorder->SetIsVisible(true);
    } else if (EbookAnnotationGetType(annotation) == AnnotationType::Line ||
               EbookAnnotationGetType(annotation) == AnnotationType::Square ||
               EbookAnnotationGetType(annotation) == AnnotationType::Circle ||
               EbookAnnotationGetType(annotation) == AnnotationType::Ink) {
        bool isLine = EbookAnnotationGetType(annotation) == AnnotationType::Line;
        bool isInk = EbookAnnotationGetType(annotation) == AnnotationType::Ink;
        constexpr const char* endings =
            "None\0Square\0Circle\0Diamond\0OpenArrow\0ClosedArrow\0Butt\0ROpenArrow\0RClosedArrow\0Slash\0";
        if (isLine) {
            window->dropDownLineStart->SetItemsSeqStrings(endings);
            window->dropDownLineStart->SetCurrentSelection(EbookAnnotationGetLineStart(annotation));
            PrepareEbookIconPreviews(window, window->dropDownLineStart);
            window->dropDownLineEnd->SetItemsSeqStrings(endings);
            window->dropDownLineEnd->SetCurrentSelection(EbookAnnotationGetLineEnd(annotation));
            PrepareEbookIconPreviews(window, window->dropDownLineEnd);
        }
        int border = EbookAnnotationGetBorderWidth(annotation);
        window->staticBorder->SetText(str::FormatTemp(_TRA("Border: %d"), border));
        window->trackbarBorder->SetValue(border);
        COLORREF interior = 0;
        if (EbookAnnotationGetInteriorColor(annotation, &interior)) {
            FillAnnotationColorDropDown(window->dropDownInteriorColor, interior, window->currCustomColor);
        } else {
            window->dropDownInteriorColor->SetItemsSeqStrings(GetPdfAnnotationColorNames());
            window->dropDownInteriorColor->SetCurrentSelection(0);
        }
        window->staticColor->SetText(_TRA("Color"));
        window->staticLineStart->SetIsVisible(isLine);
        window->dropDownLineStart->SetIsVisible(isLine);
        window->staticLineEnd->SetIsVisible(isLine);
        window->dropDownLineEnd->SetIsVisible(isLine);
        window->staticBorder->SetIsVisible(true);
        window->trackbarBorder->SetIsVisible(true);
        // Ink is a stroke, same as PDF: border width, no interior fill.
        window->staticInteriorColor->SetIsVisible(!isInk);
        window->dropDownInteriorColor->SetIsVisible(!isInk);
    } else {
        window->staticColor->SetText(_TRA("Color"));
    }
    DoIcon(window, annotation);
    if (AnnotationSupportsColor(EbookAnnotationGetType(annotation))) {
        window->staticColor->SetIsVisible(true);
        window->dropDownColor->SetIsVisible(true);
    }
    if (EbookAnnotationGetType(annotation) == AnnotationType::Highlight) {
        int opacity = EbookAnnotationGetOpacity(annotation);
        window->staticOpacity->SetText(str::FormatTemp(_TRA("Opacity: %d"), opacity));
        window->trackbarOpacity->SetValue(opacity);
        window->staticOpacity->SetIsVisible(true);
        window->trackbarOpacity->SetIsVisible(true);
    }
    if (window->buttonRestoreFreeText) {
        window->buttonRestoreFreeText->SetIsVisible(true);
    }
    if (window->resetAppearanceRow) {
        window->resetAppearanceRow->SetVisibility(Visibility::Visible);
    }
    if (window->staticColor && window->dropDownColor && window->staticBorder && !window->staticBorder->IsVisible()) {
        window->staticColor->SetInsetsPt(12, 0, 0, 0);
        window->dropDownColor->SetInsetsPt(12, 0, 0, 0);
        window->staticColor->insets.top -= 6;
        window->dropDownColor->insets.top -= 6;
    } else if (window->staticColor && window->dropDownColor) {
        window->staticColor->SetInsetsPt(12, 0, 0, 0);
        window->dropDownColor->SetInsetsPt(12, 0, 0, 0);
    }

    if (window->listBox->GetCurrentSelection() != idx) {
        window->listBox->SetCurrentSelection(idx);
    }

    HWND pane = window->inspectorPane ? window->inspectorPane->hwnd : nullptr;
    if (!InspectorChildVisUnchanged(pane, visHwnds, visBefore)) {
        hold.erase = true;
        RefreshAnnotationDetailPanel(window);
    }

    if (focus == EditAnnotFocus::Edit || EbookAnnotationGetType(annotation) == AnnotationType::Text) {
        HwndSetFocus(window->editContents->hwnd);
        window->editContents->SelectAll();
    } else {
        HwndSetFocus(window->listBox->hwnd);
    }

    int pageNo = EbookAnnotationGetPageNo(tab, annotation);
    DisplayModel* dm = tab->AsFixed();
    if (navigate && dm && dm->ValidPageNo(pageNo) && !dm->PageVisible(pageNo)) {
        dm->GoToPage(pageNo, true);
        RefreshAnnotationDetailPanel(window);
    }
    if (tab->win) {
        tab->selectedEbookAnnotation = annotation;
        RefreshAnnotationOverlay(tab->win);
    }
    window->updatingControls = false;
}

static void CacheEbookAnnotationExcerpts(EbookAnnotationsWindow* window) {
    window->annotationExcerpts.Reset();
    window->annotationLocationWidth = DpiScale(window->hwnd, 22);
    HFONT font = (HFONT)SendMessageW(window->listBox->hwnd, WM_GETFONT, 0, 0);
    for (EbookAnnotation* annotation : window->annotations) {
        int chapter = EbookAnnotationGetChapter(annotation);
        const char* location = chapter >= 0 ? str::FormatTemp("§%d", chapter + 1) : "—";
        window->annotationLocationWidth =
            std::max(window->annotationLocationWidth, HwndMeasureText(window->listBox->hwnd, location, font).dx);
        TempStr excerpt = EbookAnnotationExcerptTemp(window->tab, annotation);
        if (excerpt) str::NormalizeWSInPlace(excerpt);
        window->annotationExcerpts.Append(excerpt ? excerpt : "");
    }
}

void RefreshEbookAnnotationExcerpts(WindowTab* tab, bool loading) {
    auto window = tab ? tab->editEbookAnnotsWindow : nullptr;
    auto dm = tab ? tab->AsFixed() : nullptr;
    if (!window || !dm || window->updatingControls) return;
    int pageCount = dm->PageCount();
    bool finished = window->excerptLoading && !loading;
    if (!finished && pageCount == window->excerptPageCount) return;
    DWORD now = GetTickCount();
    if (loading && window->excerptRefreshTime && now - window->excerptRefreshTime < 1500) return;
    window->excerptPageCount = pageCount;
    window->excerptRefreshTime = now;
    window->excerptLoading = loading;
    CacheEbookAnnotationExcerpts(window);
    InvalidateRect(window->listBox->hwnd, nullptr, FALSE);
}

static const char* EbookAnnotationLocationTemp(EbookAnnotation* annotation) {
    int chapter = EbookAnnotationGetChapter(annotation);
    return chapter >= 0 ? str::FormatTemp("§%d", chapter + 1) : "—";
}

static void DrawEbookAnnotListItem(EbookAnnotationsWindow* window, ListBox::DrawItemEvent* ev) {
    if (!window || !ev || !ev->hdc || ev->itemIndex < 0 || ev->itemIndex >= window->annotations.Size()) return;
    EbookAnnotation* annotation = window->annotations.at(ev->itemIndex);
    const char* excerpt =
        ev->itemIndex < window->annotationExcerpts.Size() ? window->annotationExcerpts.At(ev->itemIndex) : "";
    DrawAnnotationSidebarRow(window->hwnd, window->listBox->hwnd, ev, ev->selected || annotation == window->selected,
                             true, EbookAnnotationGetColor(annotation), EbookAnnotationLocationTemp(annotation),
                             trans::GetTranslation(AnnotationReadableNameTemp(EbookAnnotationGetType(annotation))),
                             excerpt, window->annotationLocationWidth);
}

static void RebuildList(EbookAnnotationsWindow* window) {
    EbookAnnotation* selected = window->selected;
    window->annotations.Reset();
    EbookAnnotationsGetAll(window->tab, window->annotations);
    CacheEbookAnnotationExcerpts(window);
    auto model = new ListBoxModelStrings();
    StrBuilder text;
    for (int i = 0; i < window->annotations.Size(); i++) {
        EbookAnnotation* annotation = window->annotations.at(i);
        text.Reset();
        int pageNo = EbookAnnotationGetPageNo(window->tab, annotation);
        const char* name = trans::GetTranslation(AnnotationReadableNameTemp(EbookAnnotationGetType(annotation)));
        text.AppendFmt("%d  %s", pageNo, name);
        const char* excerpt = window->annotationExcerpts.At(i);
        if (!str::IsEmpty(excerpt)) {
            text.AppendFmt("  %s", excerpt);
        }
        model->strings.Append(text.Get());
    }
    auto topIdx = ListBoxGetTopIndex(window->listBox->hwnd);
    window->listBox->SetModel(model);
    topIdx = std::min(window->listBox->GetCount() - 1, topIdx);
    if (topIdx >= 0) {
        ListBoxSetTopIndex(window->listBox->hwnd, topIdx);
    }
    int idx = window->annotations.Find(selected);
    if (idx >= 0) {
        window->listBox->SetCurrentSelection(idx);
    }
    UpdateExportButton(window);
    RefreshAnnotationDetailPanel(window);
}

void UpdateEbookAnnotationsList(EbookAnnotationsWindow* window, EbookAnnotation* preferredSelection) {
    if (!window) {
        return;
    }
    if (preferredSelection) {
        // Set this before rebuilding: changing the list model can synchronously
        // notify its current selection. The new annotation must win that race.
        window->selected = preferredSelection;
    }
    EbookAnnotation* selected = window->selected;
    RebuildList(window);
    if (selected && window->annotations.Find(selected) >= 0) {
        UpdateSelectedAnnotation(window, selected);
        return;
    }
    int idx = window->listBox->GetCurrentSelection();
    if (window->annotations.isValidIndex(idx)) {
        UpdateSelectedAnnotation(window, window->annotations.at(idx));
        return;
    }
    ClearAnnotationDetailControls(window);
    RefreshAnnotationDetailPanel(window);
}

static void ListSelectionChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls) {
        return;
    }
    int idx = window->listBox->GetCurrentSelection();
    if (!window->annotations.isValidIndex(idx)) {
        ClearAnnotationDetailControls(window);
        RefreshAnnotationDetailPanel(window);
        return;
    }
    UpdateSelectedAnnotation(window, window->annotations.at(idx));
}

void SyncEbookFreeTextDraft(WindowTab* tab, EbookAnnotation* annotation, const char* text) {
    auto window = tab ? tab->editEbookAnnotsWindow : nullptr;
    if (!window || window->selected != annotation || !window->editContents || window->updatingControls) return;
    if (str::Eq(window->editContents->GetTextTemp(), text)) return;
    window->updatingControls = true;
    window->editContents->SetText(text);
    window->updatingControls = false;
}

static void ContentsChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected) {
        return;
    }
    TempStr note = window->editContents->GetTextTemp();
    note = str::ReplaceTemp(note, "\r\n", "\n");
    RecordEbookMutation(window, EbookAnnotationSetNote(window->tab, window->selected, note));
    AnnotationType type = EbookAnnotationGetType(window->selected);
    if (type == AnnotationType::Text || type == AnnotationType::FreeText) {
        int idx = window->annotations.Find(window->selected);
        if (idx >= 0 && idx < window->annotationExcerpts.Size()) {
            TempStr excerpt = EbookAnnotationExcerptTemp(window->tab, window->selected);
            if (excerpt) {
                str::NormalizeWSInPlace(excerpt);
            }
            window->annotationExcerpts.SetAt(idx, excerpt ? excerpt : "");
        }
        InvalidateRect(window->listBox->hwnd, nullptr, FALSE);
    }
}

static void ColorSelectionChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected) {
        return;
    }
    if (EbookAnnotationGetType(window->selected) == AnnotationType::FreeText) {
        int idx = window->dropDownColor->GetCurrentSelection();
        const char* item = idx >= 0 ? window->dropDownColor->items.At(idx) : "Transparent";
        bool transparent = str::EqI(item, "Transparent");
        if (RecordEbookMutation(window,
                                EbookAnnotationSetFreeTextBackground(window->tab, window->selected, transparent,
                                                                     transparent ? 0 : GetSelectedColor(window)))) {
            RememberEbookDrawStyle(window->selected);
            RefreshAnnotationOverlay(window->tab->win);
        }
        return;
    }
    if (RecordEbookMutation(window, EbookAnnotationSetColor(window->tab, window->selected, GetSelectedColor(window)))) {
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void HighlightOpacityChanging(EbookAnnotationsWindow* window, Trackbar::PositionChangingEvent* event) {
    if (window->updatingControls || !window->selected) return;
    if (RecordEbookMutation(window, EbookAnnotationSetOpacity(window->tab, window->selected, event->pos))) {
        window->staticOpacity->SetText(str::FormatTemp(_TRA("Opacity: %d"), event->pos));
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void FreeTextAlignmentChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected) return;
    int idx = window->dropDownTextAlignment->GetCurrentSelection();
    if (RecordEbookMutation(window, EbookAnnotationSetFreeTextAlignment(window->tab, window->selected, idx))) {
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static COLORREF DefaultEbookAppearanceColor(AnnotationType type) {
    return FactoryAnnotationColor(type);
}

static void RestoreEbookFreeTextDefaults(EbookAnnotationsWindow* window) {
    ResetFreeTextPreset();
    if (!window->selected || EbookAnnotationGetType(window->selected) != AnnotationType::FreeText) {
        return;
    }
    EbookAnnotation* annotation = window->selected;
    RecordEbookMutation(window, EbookAnnotationSetFreeTextFont(window->tab, annotation, "Helv"));
    RecordEbookMutation(window, EbookAnnotationSetFreeTextSize(window->tab, annotation, 21));
    RecordEbookMutation(window, EbookAnnotationSetFreeTextAlignment(window->tab, annotation, 0));
    RecordEbookMutation(window, EbookAnnotationSetFreeTextBorderWidth(window->tab, annotation, 1));
    RecordEbookMutation(window, EbookAnnotationSetFreeTextBorderColor(window->tab, annotation, RGB(0, 0, 0)));
    RecordEbookMutation(window, EbookAnnotationSetColor(window->tab, annotation, RGB(0, 0, 0)));
    RecordEbookMutation(window, EbookAnnotationSetFreeTextBackground(window->tab, annotation, true, 0));
    UpdateSelectedAnnotation(window, annotation, EditAnnotFocus::Default, false);
    RememberEbookDrawStyle(annotation);
    RefreshAnnotationOverlay(window->tab->win);
}

static void RestoreEbookAppearance(EbookAnnotationsWindow* window) {
    if (!window->selected) {
        return;
    }
    AnnotationType type = EbookAnnotationGetType(window->selected);
    if (type == AnnotationType::FreeText) {
        RestoreEbookFreeTextDefaults(window);
        return;
    }
    EbookAnnotation* annotation = window->selected;
    if (AnnotationSupportsColor(type)) {
        RecordEbookMutation(window,
                            EbookAnnotationSetColor(window->tab, annotation, DefaultEbookAppearanceColor(type)));
    }
    if (type == AnnotationType::Line || type == AnnotationType::Square || type == AnnotationType::Circle ||
        type == AnnotationType::Ink) {
        RecordEbookMutation(window, EbookAnnotationSetBorderWidth(window->tab, annotation, 1));
    }
    if (type == AnnotationType::Line || type == AnnotationType::Square || type == AnnotationType::Circle) {
        RecordEbookMutation(window, EbookAnnotationSetInteriorColor(window->tab, annotation, true, 0));
    }
    if (type == AnnotationType::Line) {
        RecordEbookMutation(window, EbookAnnotationSetLineEnds(window->tab, annotation, 0, 0));
    }
    if (type == AnnotationType::Text) {
        RecordEbookMutation(window, EbookAnnotationSetIcon(window->tab, annotation, "Comment"));
    } else if (type == AnnotationType::Stamp) {
        RecordEbookMutation(window, EbookAnnotationSetIcon(window->tab, annotation, "Final"));
    }
    if (type == AnnotationType::Highlight) {
        RecordEbookMutation(window, EbookAnnotationSetOpacity(window->tab, annotation, 100));
    }
    UpdateSelectedAnnotation(window, annotation, EditAnnotFocus::Default, false);
    RememberEbookDrawStyle(annotation);
    RefreshAnnotationOverlay(window->tab->win);
}

static void OnEbookFreeTextFontPicked(const char* family, void* ctx) {
    auto* window = (EbookAnnotationsWindow*)ctx;
    if (!window || !IsWindow(window->hwnd) || str::IsEmpty(family) || !window->selected) {
        return;
    }
    if (EbookAnnotationGetType(window->selected) != AnnotationType::FreeText) {
        return;
    }
    if (RecordEbookMutation(window, EbookAnnotationSetFreeTextFont(window->tab, window->selected, family))) {
        RememberFreeTextPreset(family, 0, 0, 0);
        window->buttonTextFont->SetText(FreeTextFontLabel(family));
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void ButtonPickEbookFreeTextFont(EbookAnnotationsWindow* window) {
    if (!window || !window->selected) {
        return;
    }
    HWND owner = GetAncestor(window->hwnd, GA_ROOT);
    if (!owner) {
        owner = window->hwnd;
    }
    ShowFreeTextFontPicker(owner, EbookAnnotationGetFreeTextFont(window->selected), OnEbookFreeTextFontPicked, window);
}

static void FreeTextSizeChanging(EbookAnnotationsWindow* window, Trackbar::PositionChangingEvent* event) {
    if (window->updatingControls || !window->selected) return;
    if (RecordEbookMutation(window, EbookAnnotationSetFreeTextSize(window->tab, window->selected, event->pos))) {
        RememberFreeTextPreset(nullptr, event->pos, 0, 0);
        window->staticTextSize->SetText(str::FormatTemp(_TRA("Text Size: %d"), event->pos));
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void FreeTextBorderColorChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected || !window->dropDownBorderColor) {
        return;
    }
    int idx = window->dropDownBorderColor->GetCurrentSelection();
    if (idx < 0) {
        return;
    }
    COLORREF color = GetAnnotationColorFromDropDown(window->dropDownBorderColor->items.At(idx));
    if (color == 0) {
        color = RGB(0, 0, 0);
    }
    if (RecordEbookMutation(window, EbookAnnotationSetFreeTextBorderColor(window->tab, window->selected, color))) {
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void FreeTextColorChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected) return;
    int idx = window->dropDownTextColor->GetCurrentSelection();
    if (idx >= 0 &&
        RecordEbookMutation(window, EbookAnnotationSetColor(
                                        window->tab, window->selected,
                                        GetAnnotationColorFromDropDown(window->dropDownTextColor->items.At(idx))))) {
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void PointBorderChanging(EbookAnnotationsWindow* window, Trackbar::PositionChangingEvent* event);

static void FreeTextBorderChanging(EbookAnnotationsWindow* window, Trackbar::PositionChangingEvent* event) {
    if (window->updatingControls || !window->selected) return;
    AnnotationType type = EbookAnnotationGetType(window->selected);
    if (type == AnnotationType::Line || type == AnnotationType::Square || type == AnnotationType::Circle ||
        type == AnnotationType::Ink) {
        PointBorderChanging(window, event);
        return;
    }
    if (RecordEbookMutation(window, EbookAnnotationSetFreeTextBorderWidth(window->tab, window->selected, event->pos))) {
        window->staticBorder->SetText(str::FormatTemp(_TRA("Border: %d"), event->pos));
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void LineEndsChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected) return;
    int start = window->dropDownLineStart->GetCurrentSelection();
    int end = window->dropDownLineEnd->GetCurrentSelection();
    if (RecordEbookMutation(window, EbookAnnotationSetLineEnds(window->tab, window->selected, start, end))) {
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void LineInteriorColorChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected) return;
    int idx = window->dropDownInteriorColor->GetCurrentSelection();
    const char* item = idx >= 0 ? window->dropDownInteriorColor->items.At(idx) : "Transparent";
    bool transparent = str::EqI(item, "Transparent");
    COLORREF color = transparent ? 0 : GetAnnotationColorFromDropDown(item);
    if (RecordEbookMutation(window,
                            EbookAnnotationSetInteriorColor(window->tab, window->selected, transparent, color))) {
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void PointBorderChanging(EbookAnnotationsWindow* window, Trackbar::PositionChangingEvent* event) {
    if (window->updatingControls || !window->selected) return;
    if (RecordEbookMutation(window, EbookAnnotationSetBorderWidth(window->tab, window->selected, event->pos))) {
        window->staticBorder->SetText(str::FormatTemp(_TRA("Border: %d"), event->pos));
        RememberEbookDrawStyle(window->selected);
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void IconSelectionChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected) {
        return;
    }
    EbookAnnotation* annotation = window->selected;
    AnnotationType type = EbookAnnotationGetType(annotation);
    if (type != AnnotationType::Text && type != AnnotationType::Stamp) {
        return;
    }
    int idx = window->dropDownIcon->GetCurrentSelection();
    if (idx < 0) {
        return;
    }
    const char* icons = type == AnnotationType::Stamp ? gStampIcons : gAnnotationTextIcons;
    const char* icon = seqstrings::IdxToStr(icons, idx);
    if (!icon) {
        return;
    }
    if (RecordEbookMutation(window, EbookAnnotationSetIcon(window->tab, annotation, icon))) {
        RememberEbookDrawStyle(annotation);
    }
}

static void DeleteAnnotationListItem(EbookAnnotationsWindow* window, int deletedIdx) {
    if (!window->annotations.isValidIndex(deletedIdx)) return;
    FlushContentsFromEdit(window);
    EbookAnnotation* target = window->annotations.at(deletedIdx);
    EbookAnnotation* keep = window->selected == target ? nullptr : window->selected;
    if (!EbookAnnotationsDelete(window->tab, target)) {
        return;
    }
    window->selected = keep;
    window->tab->selectedEbookAnnotation = keep;
    RebuildList(window);

    int count = window->listBox->GetCount();
    if (count > 0) {
        int idx = deletedIdx;
        if (idx >= count) {
            idx = count - 1;
        }
        if (idx < 0) {
            idx = 0;
        }
        UpdateSelectedAnnotation(window, keep ? keep : window->annotations.at(idx), EditAnnotFocus::List, false);
    } else {
        ClearAnnotationDetailControls(window);
        RefreshAnnotationDetailPanel(window);
    }

    if (window->tab->win) {
        RefreshAnnotationOverlay(window->tab->win);
    }
}

static void DeleteSelected(EbookAnnotationsWindow* window) {
    DeleteAnnotationListItem(window, window->annotations.Find(window->selected));
}

static void ExportClicked(EbookAnnotationsWindow* window) {
    FlushContentsFromEdit(window);
    EbookAnnotationsExportNotes(window->tab, window->hwnd);
}

static void OnClose(Wnd::CloseEvent* event) {
    auto window = (EbookAnnotationsWindow*)event->e->self;
    FlushContentsFromEdit(window);
    HWND activate = window->tab->win->hwndFrame;
    window->tab->editEbookAnnotsWindow = nullptr;
    delete window;
    SetActiveWindow(activate);
}

void EbookAnnotationsWindow::OnFocus() {
    // Only a click on this panel should bring its document forward. Focus handed
    // over when another window closes must not change the current tab.
    if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0 && (GetKeyState(VK_RBUTTON) & 0x8000) == 0) {
        return;
    }
    SelectTabInWindow(tab);
}

void EbookAnnotationsWindow::OnSize(UINT msg, UINT, SIZE size) {
    if (msg == WM_SIZE) {
        LayoutAnnotationSidebar(hwnd, mainLayout, listBox, inspectorPane, footerLayout, (int)size.cx, (int)size.cy,
                                IsSidebarSplitterLiveDrag());
    }
}

void ClearSelectedEbookAnnotation(WindowTab* tab) {
    EndFreeTextInPlaceEditForTab(tab, true);
    if (!tab) {
        return;
    }
    EbookAnnotationsWindow* window = tab->editEbookAnnotsWindow;
    if (window) {
        bool listSelected = window->listBox && window->listBox->GetCurrentSelection() >= 0;
        if (window->selected || tab->selectedEbookAnnotation || listSelected) {
            UpdateSelectedAnnotation(window, nullptr);
        }
        return;
    }
    if (!tab->selectedEbookAnnotation) {
        return;
    }
    tab->selectedEbookAnnotation = nullptr;
    if (tab->win) {
        RefreshAnnotationOverlay(tab->win);
    }
}

bool EbookAnnotationsWindow::PreTranslateMessage(MSG& msg) {
    if (msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_ESCAPE && (selected || tab->selectedEbookAnnotation)) {
            ClearSelectedEbookAnnotation(tab);
            return true;
        }
        bool inContentsEdit =
            IsEbookAnnotContentsEditActive(msg.hwnd, editContents ? editContents->hwnd : nullptr, hwnd);
        if (inContentsEdit && (msg.wParam == VK_BACK || msg.wParam == VK_DELETE)) {
            if (!IsCtrlPressed() && !IsAltPressed()) {
                return EditDeleteChar(editContents->hwnd, msg.wParam == VK_BACK);
            }
            return false;
        }
        if (msg.wParam == VK_DELETE) {
            DeleteSelected(this);
            return true;
        }
    }
    return false;
}

EbookAnnotationsWindow::~EbookAnnotationsWindow() {
    tab->lastEditAnnotsWindowPos = WindowRect(hwnd);
    Rect client = ClientRect(hwnd);
    tab->lastEditAnnotsWindowPos.dx = client.dx;
    tab->lastEditAnnotsWindowPos.dy = client.dy;
    tab->lastEditAnnotsWindowDpi = dpi > 0 ? dpi : DpiGet(hwnd);
    tab->lastEditAnnotsWindowMainWidth = WindowRect(tab->win->hwndFrame).dx;
    if (tab->selectedEbookAnnotation != nullptr) {
        tab->selectedEbookAnnotation = nullptr;
        if (!tab->win->isBeingClosed) {
            RefreshAnnotationOverlay(tab->win);
            ToolbarUpdateStateForWindow(tab->win, false);
        }
    }
    DeleteObject(headingFont);
    headingFont = nullptr;
    delete mainLayout;
}

static void SaveCopyClicked(EbookAnnotationsWindow* window) {
    FlushContentsFromEdit(window);
    EbookAnnotationsSaveCopy(window->tab, window->hwnd);
    UpdateEbookSaveStatus(window);
}

static void RetrySaveClicked(EbookAnnotationsWindow* window) {
    FlushContentsFromEdit(window);
    EbookAnnotationsRetrySave(window->tab);
    UpdateEbookSaveStatus(window);
}

static void CreateMainLayout(EbookAnnotationsWindow* window) {
    HWND parent = window->hwnd;
    int dpi = window->dpi > 0 ? window->dpi : DpiGet(parent);
    HFONT font = GetAppFontForDpi(dpi);
    LOGFONTW lf{};
    if (GetObjectW(font, sizeof(lf), &lf) == sizeof(lf)) {
        lf.lfWeight = FW_SEMIBOLD;
        window->headingFont = CreateFontIndirectW(&lf);
    }
    HFONT headFont = window->headingFont ? window->headingFont : font;
    COLORREF panelText = 0;
    COLORREF panel = 0;
    GetEditAnnotationsThemeColors(panelText, panel);

    auto shell =
        CreateAnnotationSidebarShell(parent, font, headFont, IsUIRtl(), MkFunc1(DrawEbookAnnotListItem, window),
                                     MkFunc0(ListSelectionChanged, window), _TRA("Annotations"));
    auto root = shell.root;
    auto vbox = shell.inspector;
    window->listBox = shell.list;
    shell.list->onDelete = MkFunc1(DeleteAnnotationListItem, window);
    window->inspectorPane = shell.pane;
    window->footerLayout = shell.footer;
    HWND sidebarParent = parent;
    parent = shell.pane->hwnd;

    auto makeHeading = [&](HFONT titleFont) {
        auto heading = new AnnotHeadingLine();
        heading->titleFont = titleFont;
        heading->metaFont = font;
        CreateCustomArgs args;
        args.parent = parent;
        args.style = WS_CHILD | WS_VISIBLE;
        args.bgColor = panel;
        args.font = titleFont;
        args.pos = {0, 0, 10, 10};
        heading->CreateCustom(args);
        heading->SetColors(panelText, panel);
        heading->SetInsetsPt(4, 0, 0, 0);
        vbox->AddChild(heading);
        return heading;
    };
    window->staticHeading = makeHeading(headFont);
    window->staticMeta = makeHeading(headFont);
    window->staticMeta->SetInsetsPt(0, 0, 0, 0);
    window->staticContents = CreateStatic(parent, font, _TRA("Note"));
    window->staticContents->SetInsetsPt(12, 0, 4, 0);
    vbox->AddChild(window->staticContents);

    Edit::CreateArgs editArgs;
    editArgs.parent = parent;
    editArgs.isMultiLine = true;
    editArgs.cueText = _TRA("Write a note…");
    editArgs.idealSizeLines = 4;
    editArgs.font = font;
    editArgs.isRtl = IsUIRtl();
    editArgs.withBorder = false;
    auto edit = new AnnotNoteEdit();
    ReportIf(!edit->Create(editArgs));
    int pad = DpiScale(edit->hwnd, 8);
    SendMessageW(edit->hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(pad, pad));
    edit->maxDx = MulDiv(150, dpi, 96);
    edit->SetColors(ThemeWindowTextColor(), ThemeAnnotationContentsEditBackgroundColor());
    edit->onTextChanged = MkFunc0(ContentsChanged, window);
    window->editContents = edit;
    shell.pane->note = edit;
    vbox->AddChild(edit);

    auto addFreeTextLabel = [&](Static*& target, const char* text) {
        target = CreateStatic(parent, font, text);
        target->SetInsetsPt(8, 0, 0, 0);
        vbox->AddChild(target);
    };
    auto addFreeTextDropDown = [&](DropDown*& target, const char* items, auto event) {
        DropDown::CreateArgs args{parent, font, IsUIRtl()};
        auto color = items == GetPdfAnnotationColorNames() ? new AnnotColorDropDown() : nullptr;
        target = color ? color : new DropDown();
        target->SetInsetsPt(4, 0, 0, 0);
        HWND hwnd = color ? color->Create(args) : target->Create(args);
        ReportIf(!hwnd);
        target->SetItemsSeqStrings(items);
        target->onSelectionChanged = event;
        vbox->AddChild(target);
    };
    constexpr const char* quadding = "Left\0Center\0Right\0";
    {
        auto row = new HBox();
        row->alignMain = MainAxisAlign::MainEnd;
        row->alignCross = CrossAxisAlign::CrossCenter;
        auto reset = new AnnotResetButton();
        reset->SetInsetsPt(-2, 0, 4, 0);
        ReportIf(!reset->Create(parent));
        reset->onClick = MkFunc0(RestoreEbookAppearance, window);
        row->AddChild(reset);
        vbox->AddChild(row);
        window->buttonRestoreFreeText = reset;
        window->resetAppearanceRow = row;
    }
    addFreeTextLabel(window->staticTextAlignment, _TRA("Text Alignment:"));
    window->staticTextAlignment->SetInsetsPt(2, 0, 0, 0);
    addFreeTextDropDown(window->dropDownTextAlignment, quadding, MkFunc0(FreeTextAlignmentChanged, window));
    addFreeTextLabel(window->staticTextFont, _TRA("Text Font:"));
    {
        Button::CreateArgs args;
        args.parent = parent;
        args.text = _TRA("Helvetica");
        args.font = font;
        args.isRtl = IsUIRtl();
        auto button = new Button();
        button->SetInsetsPt(4, 0, 0, 0);
        ReportIf(!button->Create(args));
        button->onClick = MkFunc0(ButtonPickEbookFreeTextFont, window);
        SetPropW(button->hwnd, L"AnnotLeftAligned", (HANDLE)1);
        SetWindowLongPtrW(button->hwnd, GWL_STYLE, GetWindowLongPtrW(button->hwnd, GWL_STYLE) | BS_LEFT);
        window->buttonTextFont = button;
        vbox->AddChild(button);
    }
    addFreeTextLabel(window->staticTextSize, _TRA("Text Size:"));
    {
        Trackbar::CreateArgs args;
        args.parent = parent;
        args.rangeMin = 5;
        args.rangeMax = 128;
        args.font = font;
        args.isRtl = IsUIRtl();
        auto trackbar = new Trackbar();
        trackbar->SetInsetsPt(4, 0, 0, 0);
        ReportIf(!trackbar->Create(args));
        trackbar->onPositionChanging = MkFunc1(FreeTextSizeChanging, window);
        window->trackbarTextSize = trackbar;
        vbox->AddChild(trackbar);
    }
    addFreeTextLabel(window->staticTextColor, _TRA("Text Color:"));
    addFreeTextDropDown(window->dropDownTextColor, GetPdfAnnotationColorNames(), MkFunc0(FreeTextColorChanged, window));
    constexpr const char* lineEndings =
        "None\0Square\0Circle\0Diamond\0OpenArrow\0ClosedArrow\0Butt\0ROpenArrow\0RClosedArrow\0Slash\0";
    addFreeTextLabel(window->staticLineStart, _TRA("Line Start:"));
    auto addLineEnd = [&](AnnotIconDropDown*& control, bool start) {
        control = new AnnotIconDropDown();
        control->lineEndPreview = true;
        control->lineStartPreview = start;
        DropDown::CreateArgs args{parent, font, IsUIRtl()};
        control->SetInsetsPt(4, 0, 0, 0);
        control->Create(args);
        control->SetItemsSeqStrings(lineEndings);
        control->onSelectionChanged = MkFunc0(LineEndsChanged, window);
        vbox->AddChild(control);
    };
    addLineEnd(window->dropDownLineStart, true);
    addFreeTextLabel(window->staticLineEnd, _TRA("Line End:"));
    addLineEnd(window->dropDownLineEnd, false);

    window->staticIcon = CreateStatic(parent, font, _TRA("Icon:"));
    window->staticIcon->SetInsetsPt(8, 0, 0, 0);
    vbox->AddChild(window->staticIcon);

    DropDown::CreateArgs iconArgs;
    iconArgs.parent = parent;
    iconArgs.font = font;
    iconArgs.isRtl = IsUIRtl();
    auto icon = new AnnotIconDropDown();
    icon->SetInsetsPt(4, 0, 0, 0);
    icon->Create(iconArgs);
    icon->SetItemsSeqStrings(gAnnotationTextIcons);
    icon->onSelectionChanged = MkFunc0(IconSelectionChanged, window);
    window->dropDownIcon = icon;
    vbox->AddChild(icon);

    addFreeTextLabel(window->staticBorderColor, _TRA("Border Color:"));
    addFreeTextDropDown(window->dropDownBorderColor, GetPdfAnnotationColorNames(),
                        MkFunc0(FreeTextBorderColorChanged, window));
    addFreeTextLabel(window->staticBorder, _TRA("Border:"));
    // Same 6px lift as the PDF inspector: border / color / fill sit higher.
    if (window->staticBorder) {
        window->staticBorder->insets.top -= 6;
    }
    {
        Trackbar::CreateArgs args;
        args.parent = parent;
        args.rangeMin = 0;
        args.rangeMax = 12;
        args.font = font;
        args.isRtl = IsUIRtl();
        auto trackbar = new Trackbar();
        trackbar->SetInsetsPt(4, 0, 0, 0);
        ReportIf(!trackbar->Create(args));
        trackbar->onPositionChanging = MkFunc1(FreeTextBorderChanging, window);
        window->trackbarBorder = trackbar;
        vbox->AddChild(trackbar);
    }
    auto colorProperty =
        AddAnnotationColorProperty(vbox, parent, font, IsUIRtl(), _TRA("Color"), GetPdfAnnotationColorNames(),
                                   MkFunc0(ColorSelectionChanged, window));
    window->staticColor = colorProperty.label;
    window->dropDownColor = colorProperty.value;

    window->staticInteriorColor = CreateStatic(parent, font, _TRA("Interior Color:"));
    window->staticInteriorColor->SetInsetsPt(8, 0, 0, 0);
    vbox->AddChild(window->staticInteriorColor);
    auto interiorColor = new AnnotColorDropDown();
    DropDown::CreateArgs interiorColorArgs;
    interiorColorArgs.parent = parent;
    interiorColorArgs.font = font;
    interiorColor->Create(interiorColorArgs);
    interiorColor->SetItemsSeqStrings(GetPdfAnnotationColorNames());
    interiorColor->onSelectionChanged = MkFunc0(LineInteriorColorChanged, window);
    window->dropDownInteriorColor = interiorColor;
    vbox->AddChild(interiorColor);

    window->staticOpacity = CreateStatic(parent, font, _TRA("Opacity:"));
    window->staticOpacity->SetInsetsPt(8, 0, 0, 0);
    vbox->AddChild(window->staticOpacity);
    {
        Trackbar::CreateArgs args;
        args.parent = parent;
        args.rangeMin = 0;
        args.rangeMax = 100;
        args.font = font;
        args.isRtl = IsUIRtl();
        auto trackbar = new Trackbar();
        trackbar->SetInsetsPt(4, 0, 0, 0);
        ReportIf(!trackbar->Create(args));
        trackbar->onPositionChanging = MkFunc1(HighlightOpacityChanging, window);
        window->trackbarOpacity = trackbar;
        vbox->AddChild(trackbar);
    }

    auto footer = shell.footer;
    parent = sidebarParent;
    auto commands = AddAnnotationCommandBar(footer);
    window->buttonExport = AddAnnotationFooterAction(commands, parent, font, IsUIRtl(), _TRA("Export Notes"), false,
                                                     MkFunc0(ExportClicked, window));
    window->buttonSaveStatus = AddAnnotationFooterAction(commands, parent, font, IsUIRtl(), _TRA("Save"), false,
                                                         MkFunc0(RetrySaveClicked, window));
    window->buttonSaveCopy = AddAnnotationFooterAction(commands, parent, font, IsUIRtl(), _TRA("Save as…"), true,
                                                       MkFunc0(SaveCopyClicked, window));
    commands->exportButton = window->buttonExport;
    commands->saveButton = window->buttonSaveStatus;
    commands->copyButton = window->buttonSaveCopy;
    ((AnnotCommandButton*)commands->exportButton)->tooltipText = _TRA("Export Notes");
    ((AnnotCommandButton*)commands->saveButton)->tooltipText = _TRA("Save");
    ((AnnotCommandButton*)commands->saveButton)->reserveSaveWidth = true;
    ((AnnotCommandButton*)commands->copyButton)->tooltipText = _TRA("Save annotation copy…");
    window->mainLayout = new Padding(root, DpiScaledInsets(parent, 8, 12));
    HideAnnotationControls(window);
}

void FlushEbookAnnotationEdits(WindowTab* tab) {
    EndFreeTextInPlaceEditForTab(tab, true);
    if (tab) FlushContentsFromEdit(tab->editEbookAnnotsWindow);
}

bool CloseAndDeleteEditEbookAnnotationsWindow(WindowTab* tab) {
    EndFreeTextInPlaceEditForTab(tab, true);
    if (!tab || !tab->editEbookAnnotsWindow) {
        return false;
    }
    EbookAnnotationsWindow* window = tab->editEbookAnnotsWindow;
    tab->editEbookAnnotsWindow = nullptr;
    delete window;
    return true;
}

void ShowEditEbookAnnotationsWindow(WindowTab* tab, EbookAnnotation* annotation) {
    ShowEditEbookAnnotationsWindow(tab, annotation, EditAnnotFocus::Default);
}

static void RevealEbookAnnotationsSidebar(WindowTab* tab, bool reveal) {
    if (!tab || !tab->win) {
        return;
    }
    if (reveal) {
        ShowSidebarPage(tab->win, SidebarView::Annotations);
    }
    ApplySidebarViewLayout(tab->win);
}

void ShowEditEbookAnnotationsWindow(WindowTab* tab, EbookAnnotation* annotation, EditAnnotFocus focus,
                                    bool revealInSidebar) {
    if (!tab || !EbookAnnotationsSupported(tab)) {
        return;
    }
    EbookAnnotationsWindow* window = tab->editEbookAnnotsWindow;
    if (window) {
        if (annotation) {
            window->selected = annotation;
        }
        RebuildList(window);
        if (annotation) {
            UpdateSelectedAnnotation(window, annotation, focus);
        } else if (window->listBox->GetCount() > 0) {
            UpdateSelectedAnnotation(window, window->annotations.at(0), focus);
        }
        RevealEbookAnnotationsSidebar(tab, revealInSidebar);
        return;
    }

    HWND parentHwnd = tab->win->hwndTocBox;
    if (!parentHwnd) {
        return;
    }
    window = new EbookAnnotationsWindow();
    window->tab = tab;
    window->onClose = MkFunc1Void(OnClose);
    CreateCustomArgs args;
    HMODULE module = GetModuleHandleW(nullptr);
    args.icon = LoadIconW(module, MAKEINTRESOURCEW(GetAppIconID()));
    COLORREF text = 0;
    COLORREF bg = 0;
    GetEditAnnotationsThemeColors(text, bg);
    args.bgColor = bg;
    args.title = str::JoinTemp(_TRA("Annotations"), ": ", tab->GetTabTitle());
    args.visible = false;
    args.parent = parentHwnd;
    args.style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    args.pos = {0, 0, 10, 10};
    int parentDpi = tab->win->frameDpi > 0 ? tab->win->frameDpi : DpiGet(parentHwnd);
    args.font = GetAppFontForDpi(parentDpi);
    window->CreateCustom(args);
    window->dpi = parentDpi > 0 ? parentDpi : DpiGet(window->hwnd);
    // Same as PDF annotations: theme before the first visible paint.
    window->SuspendRedraw();
    CreateMainLayout(window);
    tab->editEbookAnnotsWindow = window;
    window->selected = annotation;
    RebuildList(window);

    int height = WindowRect(tab->win->hwndFrame).dy;
    if (height > MulDiv(1024, window->dpi, 96)) {
        window->listBox->idealSizeLines = 14;
    }

    if (annotation) {
        UpdateSelectedAnnotation(window, annotation, focus);
    } else if (!window->annotations.empty()) {
        UpdateSelectedAnnotation(window, window->annotations.at(0), focus);
    }
    ApplyEbookAnnotationsWindowTheme(window, true);
    window->ResumeRedraw();
    RevealEbookAnnotationsSidebar(tab, revealInSidebar);
}
