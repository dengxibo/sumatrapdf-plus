/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include "utils/BitManip.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "Annotation.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "EngineMupdf.h"
#include "Translations.h"
#include "SumatraConfig.h"
#include "GlobalPrefs.h"
#include "DisplayModel.h"
#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "MainWindow.h"
#include "Toolbar.h"
#include "WindowTab.h"
#include "EditAnnotations.h"
#include "SumatraPDF.h"
#include "Canvas.h"
#include "Commands.h"
#include "DarkModeSubclass.h"
#include "EbookAnnotations.h"
#include "Selection.h"
#include "RenderCache.h"

#include "utils/Log.h"

#include "theme.h"

extern RenderCache* gRenderCache;

static void RerenderPdfAnnotationChange(WindowTab* tab, Annotation* overlayAnnot) {
    if (!tab) {
        return;
    }
    Annotation* annot = overlayAnnot ? overlayAnnot : tab->selectedAnnotation;
    int pageNo = annot ? annot->pageNo : 0;
    Annotation* overlay = nullptr;
    if (overlayAnnot && IsPdfTextMarkupAnnotation(overlayAnnot)) {
        overlay = overlayAnnot;
    } else if (annot && IsPdfTextMarkupAnnotation(annot)) {
        overlay = annot;
    }
    MainWindowRerenderAnnotationChange(tab->win, pageNo, overlay);
}

static COLORREF ColorRefFromPdfColor(PdfColor c) {
    if (c == 0) {
        return MkColor(0xff, 0xff, 0x00);
    }
    u8 r, g, b, a;
    UnpackPdfColor(c, r, g, b, a);
    return MkColor(r, g, b);
}

COLORREF ColorRefFromPdfAnnotationColor(PdfColor c) {
    return ColorRefFromPdfColor(c);
}

PdfColor PdfAnnotationColorFromColorRef(COLORREF c) {
    u8 r, g, b;
    UnpackColor(c, r, g, b);
    return MkPdfColor(r, g, b, 0xff);
}

void RemovePdfMarkupOverlayAnnot(WindowTab* tab, Annotation* annot) {
    if (!tab || !annot) {
        return;
    }
    for (int i = tab->pdfMarkupOverlays.size() - 1; i >= 0; i--) {
        if (tab->pdfMarkupOverlays.at(i).annot == annot) {
            tab->pdfMarkupOverlays.RemoveAt(i);
        }
    }
}

void ClearPdfMarkupOverlayForPage(WindowTab* tab, int pageNo) {
    if (!tab || pageNo <= 0) {
        return;
    }
    for (int i = tab->pdfMarkupOverlays.size() - 1; i >= 0; i--) {
        if (tab->pdfMarkupOverlays.at(i).pageNo == pageNo) {
            tab->pdfMarkupOverlays.RemoveAt(i);
        }
    }
}

void PaintPdfMarkupOverlayPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo) {
    if (!tab || tab->hideAnnotations || !dm || !dm->PageVisible(pageNo) || tab->pdfMarkupOverlays.empty()) {
        return;
    }
    for (auto& entry : tab->pdfMarkupOverlays) {
        if (entry.pageNo != pageNo || !entry.annot || !IsPdfTextMarkupAnnotation(entry.annot)) {
            continue;
        }
        Vec<RectF> pageRects = GetQuadPointsAsRect(entry.annot);
        if (pageRects.empty()) {
            RectF r = GetRect(entry.annot);
            if (!r.IsEmpty()) {
                pageRects.Append(r);
            }
        }
        NormalizeNearbyHighlightHeights(pageRects);
        Vec<Rect> screenRects;
        for (RectF rect : pageRects) {
            Rect screenRect = dm->CvtToScreen(pageNo, rect);
            if (!screenRect.IsEmpty()) {
                screenRects.Append(screenRect);
            }
        }
        COLORREF color = ColorRefFromPdfColor(GetColor(entry.annot));
        PaintTextMarkupOverlay(hdc, tab->win->canvasRc, entry.annot->type, color, screenRects);
    }
}

constexpr int borderWidthMin = 0;
constexpr int borderWidthMax = 12;

// clang-format off
static const char *gFileAttachmentUcons = "Graph\0Paperclip\0PushPin\0Tag\0";
static const char *gSoundIcons = "Speaker\0Mic\0";
static const char *gStampIcons = "Approved\0AsIs\0Confidential\0Departmental\0Draft\0Experimental\0Expired\0Final\0ForComment\0ForPublicRelease\0NotApproved\0NotForPublicRelease\0Sold\0TopSecret\0";
// those are in order of pdf_line_ending enum in annot.h
static const char *gLineEndingStyles = "None\0Square\0Circle\0Diamond\0OpenArrow\0ClosedArrow\0Butt\0ROpenArrow\0RClosedArrow\0Slash\0";
static const char* gColors = "Transparent\0Aqua\0Black\0Blue\0Fuchsia\0Gray\0Green\0Lime\0Maroon\0Navy\0Olive\0Orange\0Purple\0Red\0Silver\0Teal\0White\0Yellow\0";
static const char *gFontNames = "Cour\0Helv\0TiRo\0";
static const char *gFontReadableNames = "Courier\0Helvetica\0TimesRoman\0";
static const char* gQuaddingNames = "Left\0Center\0Right\0";

static PdfColor gColorsValues[] = {
	0x00000000, /* transparent */
	0xff00ffff, /* aqua */
	0xff000000, /* black */
	0xff0000ff, /* blue */
	0xffff00ff, /* fuchsia */
	0xff808080, /* gray */
	0xff008000, /* green */
	0xff00ff00, /* lime */
	0xff800000, /* maroon */
	0xff000080, /* navy */
	0xff808000, /* olive */
	0xffffa500, /* orange */
	0xff800080, /* purple */
	0xffff0000, /* red */
	0xffc0c0c0, /* silver */
	0xff008080, /* teal */
	0xffffffff, /* white */
	0xffffff00, /* yellow */
};

// list of annotations where GetColor() returns background color
// TODO: probably incomplete;
static AnnotationType gAnnotsIsColorBackground[] = {
    AnnotationType::FreeText,
};
// clang-format on

const char* GetPdfAnnotationColorNames() {
    return gColors;
}

const char* GetKnownColorName(PdfColor c) {
    int n = (int)dimof(gColorsValues);
    for (int i = 0; i < n; i++) {
        if (c == gColorsValues[i]) {
            const char* s = seqstrings::IdxToStr(gColors, i);
            return s;
        }
    }
    return nullptr;
}

struct EditAnnotationsWindow : Wnd {
    WindowTab* tab = nullptr;
    LayoutBase* mainLayout = nullptr;

    ListBox* listBox = nullptr;
    Static* staticRect = nullptr;
    Static* staticAuthor = nullptr;
    Static* staticModificationDate = nullptr;
    Static* staticPopup = nullptr;
    Static* staticContents = nullptr;
    Edit* editContents = nullptr;
    Static* staticTextAlignment = nullptr;
    DropDown* dropDownTextAlignment = nullptr;
    Static* staticTextFont = nullptr;
    DropDown* dropDownTextFont = nullptr;
    Static* staticTextSize = nullptr;
    Trackbar* trackbarTextSize = nullptr;
    Static* staticTextColor = nullptr;
    DropDown* dropDownTextColor = nullptr;

    Static* staticLineStart = nullptr;
    DropDown* dropDownLineStart = nullptr;
    Static* staticLineEnd = nullptr;
    DropDown* dropDownLineEnd = nullptr;

    Static* staticIcon = nullptr;
    DropDown* dropDownIcon = nullptr;

    Static* staticBorder = nullptr;
    Trackbar* trackbarBorder = nullptr;

    Static* staticColor = nullptr;
    DropDown* dropDownColor = nullptr;
    Static* staticInteriorColor = nullptr;
    DropDown* dropDownInteriorColor = nullptr;

    Static* staticOpacity = nullptr;
    Trackbar* trackbarOpacity = nullptr;

    Button* buttonSaveAttachment = nullptr;
    Button* buttonEmbedAttachment = nullptr;

    Button* buttonDelete = nullptr;
    Button* buttonExport = nullptr;

    Button* buttonSaveToCurrentFile = nullptr;
    Button* buttonSaveToNewFile = nullptr;

    // those are
    Vec<Annotation*> annotations;

    bool skipGoToPage = false;
    bool updatingControls = false;
    int dpi = 0;

    StrBuilder currTextColor;
    StrBuilder currCustomColor;
    StrBuilder currCustomInteriorColor;

    void OnSize(UINT msg, UINT type, SIZE size) override;
    void OnFocus() override;
    bool PreTranslateMessage(MSG&) override;

    void ListBoxSelectionChanged();

    ~EditAnnotationsWindow() override;
};

#if 0
static Annotation* PickNewSelectedAnnotation(EditAnnotationsWindow* ew, int prevIdx) {
    int nAnnots = ew->annotations.Size();
    if (nAnnots == 0) {
        return nullptr;
    }
    if (prevIdx >= nAnnots) {
        prevIdx = nAnnots - 1;
    }
    return ew->annotations.at(prevIdx);
}
#endif

void DeleteAnnotationAndUpdateUI(WindowTab* tab, Annotation* annot) {
    EditAnnotationsWindow* ew = tab->editAnnotsWindow;
    Annotation* selectNext = nullptr;
    int pageNo = annot ? annot->pageNo : 0;
    if (annot != tab->selectedAnnotation) {
        // preserve current selection if we're not deleting it
        selectNext = tab->selectedAnnotation;
    }

    RemovePdfMarkupOverlayAnnot(tab, annot);
    DeleteAnnotation(annot);
    if (tab->selectedAnnotation == annot) {
        tab->selectedAnnotation = nullptr;
    }
    if (ew != nullptr) {
        // can be null if called from Menu.cpp and annotations window is not visible
        // ew->skipGoToPage = true;
        // int currSelIdx = ew ? ew->listBox->GetCurrentSelection() : -1;
        UpdateAnnotationsList(ew);
#if 0
        if ((selectNext == nullptr) && (currSelIdx >= 0)) {
            // if we're deleting currently selected, pick
            // next to select
            annot = PickNewSelectedAnnotation(ew, currSelIdx);
        }
#endif
    }
    SetSelectedAnnotation(tab, selectNext);
    MainWindowRerenderAnnotationChange(tab->win, pageNo, nullptr);
}

static void DeleteSelectedAnnotation(EditAnnotationsWindow* ew) {
    int idx = ew->listBox->GetCurrentSelection();
    if (idx < 0) {
        // can get out of sync e.g. after UpdateAnnotationsList during save/reload
        ew->tab->selectedAnnotation = nullptr;
        return;
    }
    Annotation* annot = ew->annotations.at(idx);
    if (ew->tab->selectedAnnotation != annot) {
        // can get out of sync if e.g. keyboard navigation in listbox
        // hasn't triggered ListBoxSelectionChanged yet
        ew->tab->selectedAnnotation = annot;
    }
    DeleteAnnotationAndUpdateUI(ew->tab, annot);

    // Note: auto-selecting next annotation might cause page jumping
#if 0
    annot = PickNewSelectedAnnotation(this, idx);
    skipGoToPage = false;
    if (annot) {
        SetSelectedAnnotation(tab, annot);
    }
#endif
}

static NO_INLINE EngineMupdf* GetEngineMupdf(EditAnnotationsWindow* ew) {
#if 0
    // TODO: shouldn't happen but seen in crash report
    if (!ew || !ew->tab) {
        return nullptr;
    }
#endif
    DisplayModel* dm = ew->tab->AsFixed();
#if 0
    if (!dm) {
        return nullptr;
    }
#endif
    return AsEngineMupdf(dm->GetEngine());
}

static void HidePerAnnotControls(EditAnnotationsWindow* ew) {
    ew->staticRect->SetIsVisible(false);
    ew->staticAuthor->SetIsVisible(false);
    ew->staticModificationDate->SetIsVisible(false);
    ew->staticPopup->SetIsVisible(false);
    ew->staticContents->SetIsVisible(false);
    ew->editContents->SetIsVisible(false);
    ew->staticTextAlignment->SetIsVisible(false);
    ew->dropDownTextAlignment->SetIsVisible(false);
    ew->staticTextFont->SetIsVisible(false);
    ew->dropDownTextFont->SetIsVisible(false);
    ew->staticTextSize->SetIsVisible(false);
    ew->trackbarTextSize->SetIsVisible(false);
    ew->staticTextColor->SetIsVisible(false);
    ew->dropDownTextColor->SetIsVisible(false);

    ew->staticLineStart->SetIsVisible(false);
    ew->dropDownLineStart->SetIsVisible(false);
    ew->staticLineEnd->SetIsVisible(false);
    ew->dropDownLineEnd->SetIsVisible(false);

    ew->staticIcon->SetIsVisible(false);
    ew->dropDownIcon->SetIsVisible(false);

    ew->staticBorder->SetIsVisible(false);
    ew->trackbarBorder->SetIsVisible(false);
    ew->staticColor->SetIsVisible(false);
    ew->dropDownColor->SetIsVisible(false);
    ew->staticInteriorColor->SetIsVisible(false);
    ew->dropDownInteriorColor->SetIsVisible(false);

    ew->staticOpacity->SetIsVisible(false);
    ew->trackbarOpacity->SetIsVisible(false);

    ew->buttonSaveAttachment->SetIsVisible(false);
    ew->buttonEmbedAttachment->SetIsVisible(false);

    ew->buttonDelete->SetIsVisible(false);
}

static int FindStringInArray(const char* items, const char* toFind, int valIfNotFound = -1) {
    int idx = seqstrings::StrToIdx(items, toFind);
    if (idx < 0) {
        idx = valIfNotFound;
    }
    return idx;
}

static bool IsAnnotationTypeInArray(AnnotationType* arr, size_t arrSize, AnnotationType toFind) {
    for (size_t i = 0; i < arrSize; i++) {
        if (toFind == arr[i]) {
            return true;
        }
    }
    return false;
}

// return true if closed the window, false if there was no window to close
bool CloseAndDeleteEditAnnotationsWindow(WindowTab* tab) {
    if (!tab->editAnnotsWindow) {
        return false;
    }
    auto ew = tab->editAnnotsWindow;
    tab->editAnnotsWindow = nullptr;
    // this will trigger closing the window
    delete ew;
    return true;
}

EditAnnotationsWindow::~EditAnnotationsWindow() {
    // hacky: we want the position of the main window
    // but the size of client area
    tab->lastEditAnnotsWindowPos = WindowRect(hwnd);
    auto cr = ClientRect(hwnd);
    tab->lastEditAnnotsWindowPos.dx = cr.dx;
    tab->lastEditAnnotsWindowPos.dy = cr.dy;
    tab->lastEditAnnotsWindowDpi = dpi > 0 ? dpi : DpiGet(hwnd);
    tab->lastEditAnnotsWindowMainWidth = WindowRect(tab->win->hwndFrame).dx;

    if (tab->selectedAnnotation != nullptr) {
        tab->selectedAnnotation = nullptr;
        if (!tab->win->isBeingClosed) {
            MainWindowRerender(tab->win);
            ToolbarUpdateStateForWindow(tab->win, false);
        }
    }
    delete mainLayout;
}

static bool DidAnnotationsChange(EditAnnotationsWindow* ew) {
    EngineMupdf* engine = GetEngineMupdf(ew);
    if (!engine) { // maybe seen in crash report
        ReportIf(true);
        return false;
    }
    return EngineMupdfHasUnsavedAnnotations(engine);
}

static void EnableSaveIfAnnotationsChanged(EditAnnotationsWindow* ew) {
    bool didChange = DidAnnotationsChange(ew);
    ew->buttonSaveToCurrentFile->SetIsEnabled(didChange);
    ew->buttonSaveToNewFile->SetIsEnabled(didChange);
}

void NotifyAnnotationsChanged(EditAnnotationsWindow* ew) {
    if (!ew) {
        return;
    }
    EnableSaveIfAnnotationsChanged(ew);
}

static void GetEditAnnotationsThemeColors(COLORREF& textOut, COLORREF& bgOut) {
    textOut = ThemeWindowTextColor();
    bgOut = ThemeWindowControlBackgroundColor();
}

static void UpdateAnnotationContentsEditChrome(Edit* edit) {
    if (!edit || !edit->hwnd) {
        return;
    }
    bool recessed = ThemeUsesDarkChrome();
    SetWindowExStyle(edit->hwnd, WS_EX_CLIENTEDGE, recessed);
    SetWindowPos(edit->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    if (UseDarkModeLib() && !recessed) {
        DarkMode::removeCustomBorderForListBoxOrEditCtrlSubclass(edit->hwnd);
    }
}

struct EditAnnotThemeColors {
    COLORREF text = 0;
    COLORREF bg = 0;
};

static BOOL CALLBACK ApplyThemeColorsToChildWnd(HWND hwnd, LPARAM lparam) {
    auto* colors = (EditAnnotThemeColors*)lparam;
    Wnd* wnd = WndListFindByHwnd(hwnd);
    if (wnd) {
        wnd->SetColors(colors->text, colors->bg);
    }
    return TRUE;
}

static void ApplyEditAnnotationsWindowTheme(EditAnnotationsWindow* ew, bool installDarkMode) {
    if (!ew || !ew->hwnd) {
        return;
    }
    EditAnnotThemeColors colors;
    GetEditAnnotationsThemeColors(colors.text, colors.bg);
    ew->SetColors(colors.text, colors.bg);
    EnumChildWindows(ew->hwnd, ApplyThemeColorsToChildWnd, (LPARAM)&colors);
    ew->editContents->SetColors(colors.text, ThemeAnnotationContentsEditBackgroundColor());
    UpdateAnnotationContentsEditChrome(ew->editContents);

    if (UseDarkModeLib()) {
        if (installDarkMode) {
            DarkMode::setDarkWndNotifySafe(ew->hwnd);
            DarkMode::setWindowEraseBgSubclass(ew->hwnd);
        } else {
            DarkMode::setWindowCtlColorSubclass(ew->hwnd);
            DarkMode::setChildCtrlsTheme(ew->hwnd);
        }
    }
    UpdateWindowCaptionTheme(ew->hwnd);

    uint flags = RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN;
    RedrawWindow(ew->hwnd, nullptr, nullptr, flags);
}

void RefreshEditAnnotationsWindowsTheme() {
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            if (tab->editAnnotsWindow) {
                ApplyEditAnnotationsWindowTheme(tab->editAnnotsWindow, false);
            }
        }
    }
}

void DockOpenEditAnnotationsWindows(MainWindow* win) {
    if (!win || !win->hwndFrame) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        EditAnnotationsWindow* ew = tab->editAnnotsWindow;
        if (ew && ew->hwnd && IsWindowVisible(ew->hwnd)) {
            HwndDockToRightOf(ew->hwnd, win->hwndFrame);
        }
    }
}

void CloseEditAnnotationsWindowsForDpiMove(MainWindow* win) {
    if (!win) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        if (tab->editAnnotsWindow) {
            tab->reopenEditAnnotsAfterDpiMove = true;
            CloseAndDeleteEditAnnotationsWindow(tab);
        }
    }
}

void ReopenEditAnnotationsWindowsAfterDpiMove(MainWindow* win) {
    if (!win) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        if (!tab->reopenEditAnnotsAfterDpiMove) {
            continue;
        }
        tab->reopenEditAnnotsAfterDpiMove = false;
        ShowEditAnnotationsWindow(tab, nullptr);
    }
}

static void RebuildAnnotationsListBox(EditAnnotationsWindow* ew) {
    auto model = new ListBoxModelStrings();
    int n = 0;
    n = ew->annotations.Size();

    StrBuilder s;
    for (int i = 0; i < n; i++) {
        auto annot = ew->annotations.at(i);
        s.Reset();
        s.AppendFmt(_TRA("page %d,"), annot->pageNo);
        TempStr name = AnnotationReadableNameTemp(annot->type);
        s.AppendFmt(" %s", name);
        TempStr markedText = MarkupTextTemp(annot);
        TempStr previewSource = markedText ? markedText : Contents(annot);
        if (!str::IsEmptyOrWhiteSpace(previewSource)) {
            TempStr preview = str::DupTemp(previewSource);
            str::NormalizeWSInPlace(preview);
            preview = ShortenStringUtf8Temp(preview, 48);
            s.AppendFmt(" — %s", preview);
        }
        model->strings.Append(s.Get());
    }

    auto topIdx = ListBoxGetTopIndex(ew->listBox->hwnd);
    ew->listBox->SetModel(model);
    topIdx = std::min(ew->listBox->GetCount() - 1, topIdx);
    if (topIdx >= 0) {
        ListBoxSetTopIndex(ew->listBox->hwnd, topIdx);
    }
    EnableSaveIfAnnotationsChanged(ew);
    if (ew->buttonExport) {
        ew->buttonExport->SetIsEnabled(n > 0);
    }
}

struct PdfAnnotationSortItem {
    Annotation* annotation = nullptr;
    int pageNo = 0;
    float sortY = 0.f;
    float sortX = 0.f;
};

static void AppendUtcDateTime(StrBuilder& s, time_t secs) {
    if (secs <= 0) {
        return;
    }
    struct tm tm;
    gmtime_s(&tm, &secs);
    char buf[100];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
    s.Append(buf);
}

static void AppendMarkdownBlockquote(StrBuilder& out, const char* text) {
    if (str::IsEmpty(text)) {
        return;
    }
    const u8* p = (const u8*)text;
    bool lineStart = true;
    while (*p) {
        u8 c = *p++;
        if (lineStart) {
            out.Append("> ");
            lineStart = false;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            out.AppendChar('\n');
            lineStart = true;
            continue;
        }
        out.AppendChar((char)c);
    }
    if (!lineStart) {
        out.AppendChar('\n');
    }
}

static bool BuildPdfAnnotationsExport(WindowTab* tab, StrBuilder& out) {
    if (!tab || !EngineSupportsAnnotations(tab->GetEngine())) {
        return false;
    }
    Vec<Annotation*> annotations;
    EngineMupdfGetAnnotations(tab->GetEngine(), annotations);
    if (annotations.empty()) {
        return false;
    }

    Vec<PdfAnnotationSortItem> items;
    for (Annotation* annotation : annotations) {
        PdfAnnotationSortItem item;
        item.annotation = annotation;
        item.pageNo = PageNo(annotation);
        RectF bounds = GetBounds(annotation);
        item.sortY = bounds.y;
        item.sortX = bounds.x;
        items.Append(item);
    }
    std::sort(items.begin(), items.end(), [](const PdfAnnotationSortItem& a, const PdfAnnotationSortItem& b) {
        if (a.pageNo != b.pageNo) {
            return a.pageNo < b.pageNo;
        }
        if (a.sortY != b.sortY) {
            return a.sortY < b.sortY;
        }
        return a.sortX < b.sortX;
    });

    out.AppendFmt("# %s\n\n", tab->GetTabTitle());
    out.AppendFmt("%s: %s\n", _TRA("Source"), tab->filePath);
    out.Append(_TRA("Exported:"));
    out.Append(" ");
    AppendUtcDateTime(out, time(nullptr));
    out.Append("\n\n---\n\n");

    for (const PdfAnnotationSortItem& item : items) {
        Annotation* annotation = item.annotation;
        TempStr typeName = AnnotationReadableNameTemp(Type(annotation));
        out.AppendFmt("## %s %d — %s\n\n", _TRA("Page"), item.pageNo, typeName);

        TempStr excerpt = MarkupTextTemp(annotation);
        if (!str::IsEmpty(excerpt)) {
            AppendMarkdownBlockquote(out, excerpt);
            out.AppendChar('\n');
        }

        TempStr note = Contents(annotation);
        if (!str::IsEmpty(note)) {
            out.AppendFmt("**%s**\n\n", _TRA("Note:"));
            out.Append(note);
            out.Append("\n\n");
        }

        const char* author = Author(annotation);
        if (!str::IsEmpty(author)) {
            out.AppendFmt("%s: %s\n", _TRA("Author"), author);
        }
        time_t date = ModificationDate(annotation);
        if (date > 0) {
            out.Append(_TRA("Date:"));
            out.Append(" ");
            AppendUtcDateTime(out, date);
            out.AppendChar('\n');
        }
        out.Append("\n---\n\n");
    }
    return true;
}

bool PdfAnnotationsExportNotes(WindowTab* tab, HWND hwndParent) {
    if (!tab || !EngineSupportsAnnotations(tab->GetEngine())) {
        return false;
    }
    StrBuilder out;
    if (!BuildPdfAnnotationsExport(tab, out)) {
        NotificationCreateArgs nargs;
        nargs.hwndParent = hwndParent;
        nargs.font = GetDefaultGuiFont();
        nargs.timeoutMs = 4000;
        nargs.msg = _TRA("No annotations to export.");
        ShowNotification(nargs);
        return false;
    }

    TempStr defaultPath =
        path::JoinTemp(path::GetDirTemp(tab->filePath),
                       str::JoinTemp(path::GetBaseNameTemp(path::GetPathNoExtTemp(tab->filePath)), "-notes.md"));
    if (!SaveDataToFile(hwndParent, defaultPath, ByteSlice((const u8*)out.Get(), out.size()))) {
        return false;
    }

    NotificationCreateArgs nargs;
    nargs.hwndParent = hwndParent;
    nargs.font = GetDefaultGuiFont();
    nargs.timeoutMs = 5000;
    nargs.msg = _TRA("Exported annotations.");
    ShowNotification(nargs);
    return true;
}

static void FlushContentsFromEdit(EditAnnotationsWindow* ew);

static void ExportClicked(EditAnnotationsWindow* ew) {
    FlushContentsFromEdit(ew);
    PdfAnnotationsExportNotes(ew->tab, ew->hwnd);
}

// TODO: this should be OnDestroy()
static void OnClose(Wnd::CloseEvent* ev) {
    auto w = (EditAnnotationsWindow*)ev->e->self;
    FlushContentsFromEdit(w);
    HWND toActivate = w->tab->win->hwndFrame;
    w->tab->editAnnotsWindow = nullptr;
    delete w; // TODO: sketchy
    SetActiveWindow(toActivate);
}

void EditAnnotationsWindow::OnFocus() {
    SelectTabInWindow(tab);
}

extern bool SaveAnnotationsToMaybeNewPdfFile(WindowTab*);

static void ButtonSaveToNewFileHandler(EditAnnotationsWindow* ew) {
    FlushContentsFromEdit(ew);
    WindowTab* tab = ew->tab;
    bool ok = SaveAnnotationsToMaybeNewPdfFile(tab);
    if (!ok) {
        return;
    }
}

extern bool SaveAnnotationsToExistingFile(WindowTab* tab);

static void ButtonSaveToCurrentPDFHandler(EditAnnotationsWindow* ew) {
    FlushContentsFromEdit(ew);
    SaveAnnotationsToExistingFile(ew->tab);
}

constexpr int kMaxControls = 18;

static void AdvanceFocus(EditAnnotationsWindow* ew, bool forward) {
    HWND controls[kMaxControls];
    int n = 0;
    auto addIfVisible = [&](HWND h) {
        if (h && IsWindowVisible(h)) {
            ReportIf(n >= kMaxControls);
            controls[n++] = h;
        }
    };

    addIfVisible(ew->listBox->hwnd);
    addIfVisible(ew->editContents->hwnd);
    addIfVisible(ew->dropDownTextAlignment->hwnd);
    addIfVisible(ew->dropDownTextFont->hwnd);
    addIfVisible(ew->trackbarTextSize->hwnd);
    addIfVisible(ew->dropDownTextColor->hwnd);
    addIfVisible(ew->dropDownLineStart->hwnd);
    addIfVisible(ew->dropDownLineEnd->hwnd);
    addIfVisible(ew->dropDownIcon->hwnd);
    addIfVisible(ew->trackbarBorder->hwnd);
    addIfVisible(ew->dropDownColor->hwnd);
    addIfVisible(ew->dropDownInteriorColor->hwnd);
    addIfVisible(ew->trackbarOpacity->hwnd);
    addIfVisible(ew->buttonSaveAttachment->hwnd);
    addIfVisible(ew->buttonEmbedAttachment->hwnd);
    addIfVisible(ew->buttonDelete->hwnd);
    addIfVisible(ew->buttonExport->hwnd);
    addIfVisible(ew->buttonSaveToCurrentFile->hwnd);
    addIfVisible(ew->buttonSaveToNewFile->hwnd);

    if (n == 0) {
        return;
    }

    HWND focused = ::GetFocus();
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (controls[i] == focused || ::IsChild(controls[i], focused)) {
            idx = i;
            break;
        }
    }

    int next;
    if (forward) {
        next = (idx + 1) % n;
    } else {
        next = (idx <= 0) ? n - 1 : idx - 1;
    }
    HwndSetFocus(controls[next]);
}

static bool IsAnnotContentsEditActive(HWND msgHwnd, HWND editHwnd, HWND windowHwnd) {
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

bool IsPdfAnnotContentsEditFocused(HWND msgHwnd) {
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            EditAnnotationsWindow* ew = tab->editAnnotsWindow;
            if (!ew || !ew->editContents) {
                continue;
            }
            if (IsAnnotContentsEditActive(msgHwnd, ew->editContents->hwnd, ew->hwnd)) {
                return true;
            }
        }
    }
    return false;
}

bool EditAnnotationsWindow::PreTranslateMessage(MSG& msg) {
    if (msg.message == WM_KEYDOWN) {
        int key = (int)msg.wParam;
        bool inContentsEdit = IsAnnotContentsEditActive(msg.hwnd, editContents ? editContents->hwnd : nullptr, hwnd);
        if (key == VK_TAB) {
            bool forward = !IsShiftPressed();
            AdvanceFocus(this, forward);
            return true;
        }
        if (inContentsEdit && (key == VK_BACK || key == VK_DELETE)) {
            if (!IsCtrlPressed() && !IsAltPressed()) {
                return EditDeleteChar(editContents->hwnd, key == VK_BACK);
            }
            return false;
        }
        if (key == VK_DELETE) {
            if (IsCtrlPressed()) {
                DeleteSelectedAnnotation(this);
                return true;
            }
            DeleteSelectedAnnotation(this);
            return true;
        }
        if (key == 'S' && IsShiftPressed() && IsCtrlPressed()) {
            // TODO: delay by posting a message?
            // TODO: the keybinding could be changed so this should
            // be more sophisticated and match the shortcut
            ButtonSaveToCurrentPDFHandler(this);
            return true;
        }
    }
    return false;
}

static void ItemsFromSeqstrings(StrVec& items, const char* strings) {
    while (strings) {
        items.Append(strings);
        seqstrings::Next(strings);
    }
}

static void DropDownFillColors(DropDown* w, PdfColor col, StrBuilder& customColor) {
    StrVec items;
    ItemsFromSeqstrings(items, gColors);
    const char* colorName = GetKnownColorName(col);
    int idx = seqstrings::StrToIdx(gColors, colorName);
    if (idx < 0) {
        customColor.Reset();
        SerializePdfColor(col, customColor);
        items.Append(customColor.LendData());
        idx = items.Size() - 1;
    }
    w->SetItems(items);
    w->SetCurrentSelection(idx);
}

static PdfColor GetDropDownColor(const char* sv) {
    int idx = seqstrings::StrToIdx(gColors, sv);
    if (idx >= 0) {
        int nMaxColors = (int)dimof(gColorsValues);
        ReportIf(idx >= nMaxColors);
        if (idx < nMaxColors) {
            return gColorsValues[idx];
        }
        return 0;
    }
    ParsedColor col;
    ParseColor(col, sv);
    return col.pdfCol;
}

COLORREF GetAnnotationColorFromDropDown(const char* item) {
    return ColorRefFromPdfColor(GetDropDownColor(item));
}

void FillAnnotationColorDropDown(DropDown* w, COLORREF col, StrBuilder& customColor) {
    DropDownFillColors(w, PdfAnnotationColorFromColorRef(col), customColor);
}

COLORREF GetDefaultAnnotationColor(AnnotationType type) {
    auto& a = gGlobalPrefs->annotations;
    ParsedColor* col = nullptr;
    if (type == AnnotationType::Text) {
        col = GetParsedColor(a.textIconColor, a.textIconColorParsed);
    } else if (type == AnnotationType::Underline) {
        col = GetParsedColor(a.underlineColor, a.underlineColorParsed);
    } else if (type == AnnotationType::Highlight) {
        col = GetParsedColor(a.highlightColor, a.highlightColorParsed);
    } else if (type == AnnotationType::Squiggly) {
        col = GetParsedColor(a.squigglyColor, a.squigglyColorParsed);
    } else if (type == AnnotationType::StrikeOut) {
        col = GetParsedColor(a.strikeOutColor, a.strikeOutColorParsed);
    } else if (type == AnnotationType::FreeText) {
        col = GetParsedColor(a.freeTextColor, a.freeTextColorParsed);
    }
    if (col && col->parsedOk) {
        return col->col;
    }
    if (type == AnnotationType::Underline) {
        return ParseColor("#00ff00");
    }
    if (type == AnnotationType::Squiggly) {
        return ParseColor("#ff00ff");
    }
    if (type == AnnotationType::StrikeOut) {
        return ParseColor("#ff0000");
    }
    return ParseColor("#ffff00");
}

// TODO: mupdf shows it in 1.6 but not 1.7. Why?
bool gShowRect = true;

// TODO: only limit to widgets that have rect?
static void DoRect(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!gShowRect) {
        return;
    }
    StrBuilder s;
    RectF rect = GetBounds(annot);
    int x = (int)rect.x;
    int y = (int)rect.y;
    int dx = (int)rect.dx;
    int dy = (int)rect.dy;
    s.AppendFmt(_TRA("Rect: x=%d y=%d dx=%d dy=%d"), x, y, dx, dy);
    ew->staticRect->SetText(s.Get());
    ew->staticRect->SetIsVisible(true);
}

static void DoAuthor(EditAnnotationsWindow* ew, Annotation* annot) {
    const char* author = Author(annot);
    bool isVisible = !str::IsEmpty(author);
    if (!isVisible) {
        return;
    }
    StrBuilder s;
    s.AppendFmt(_TRA("Author: %s"), author);
    ew->staticAuthor->SetText(s.Get());
    ew->staticAuthor->SetIsVisible(true);
}

static void AppendPdfDate(StrBuilder& s, time_t secs) {
    struct tm tm;
    gmtime_s(&tm, &secs);
    char buf[100];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
    s.Append(buf);
}

static void DoModificationDate(EditAnnotationsWindow* ew, Annotation* annot) {
    bool isVisible = (ModificationDate(annot) != 0);
    if (!isVisible) {
        return;
    }
    StrBuilder s;
    s.Append(_TRA("Date:"));
    s.Append(" "); // apptranslator doesn't handle spaces at the end of translated string
    AppendPdfDate(s, ModificationDate(annot));
    ew->staticModificationDate->SetText(s.Get());
    ew->staticModificationDate->SetIsVisible(true);
}

static void DoPopup(EditAnnotationsWindow* ew, Annotation* annot) {
    int popupId = PopupId(annot);
    if (popupId < 0) {
        return;
    }
    StrBuilder s;
    s.AppendFmt(_TRA("Popup: %d 0 R"), popupId);
    ew->staticPopup->SetText(s.Get());
    ew->staticPopup->SetIsVisible(true);
}

static void FlushContentsFromEdit(EditAnnotationsWindow* ew) {
    if (!ew || !ew->editContents || ew->updatingControls) {
        return;
    }
    Annotation* a = ew->tab->selectedAnnotation;
    if (!a || !a->engine || !a->pdfannot) {
        return;
    }
    if (ew->annotations.Find(a) < 0) {
        return;
    }
    auto txt = ew->editContents->GetTextTemp();
    txt = str::ReplaceTemp(txt, "\r\n", "\n");
    SetContents(a, txt);
    EnableSaveIfAnnotationsChanged(ew);
}

static void DoContents(EditAnnotationsWindow* ew, Annotation* annot) {
    TempStr s = Contents(annot);
    // don't replace if already is "\r\n"
    s = str::ReplaceTemp(s, "\r\n", "\n");
    s = str::ReplaceTemp(s, "\n", "\r\n");
    ew->staticContents->SetIsVisible(true);
    ew->editContents->SetIsVisible(true);
    ew->updatingControls = true;
    ew->editContents->SetText(s);
    ew->updatingControls = false;
}

static void DoTextAlignment(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FreeText) {
        return;
    }
    int itemNo = Quadding(annot);
    const char* items = gQuaddingNames;
    ew->dropDownTextAlignment->SetItemsSeqStrings(items);
    ew->dropDownTextAlignment->SetCurrentSelection(itemNo);
    ew->staticTextAlignment->SetIsVisible(true);
    ew->dropDownTextAlignment->SetIsVisible(true);
}

static void TextAlignmentSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownTextAlignment->GetCurrentSelection();
    int newQuadding = idx;
    SetQuadding(annot, newQuadding);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoTextFont(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FreeText) {
        return;
    }
    const char* fontName = DefaultAppearanceTextFont(annot);
    // TODO: might have other fonts, like "Symb" and "ZaDb"
    auto itemNo = seqstrings::StrToIdx(gFontNames, fontName);
    if (itemNo < 0) {
        return;
    }
    ew->dropDownTextFont->SetItemsSeqStrings(gFontReadableNames);
    ew->dropDownTextFont->SetCurrentSelection(itemNo);
    ew->staticTextFont->SetIsVisible(true);
    ew->dropDownTextFont->SetIsVisible(true);
}

static void TextFontSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownTextFont->GetCurrentSelection();
    const char* font = seqstrings::IdxToStr(gFontNames, idx);
    SetDefaultAppearanceTextFont(annot, font);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoTextSize(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FreeText) {
        return;
    }
    int fontSize = DefaultAppearanceTextSize(annot);
    TempStr s = str::FormatTemp(_TRA("Text Size: %d"), fontSize);
    ew->staticTextSize->SetText(s);
    // TODO: DoTextSize() shouldn't modify the annotation but I'm not sure
    // if it's not needed to be called for free text annotations
    // at some point (i.e. when creating)
    // SetDefaultAppearanceTextSize(ew->tab->selectedAnnotation, fontSize);
    ew->trackbarTextSize->SetValue(fontSize);
    ew->staticTextSize->SetIsVisible(true);
    ew->trackbarTextSize->SetIsVisible(true);
}

static void TextFontSizeChanging(EditAnnotationsWindow* ew, Trackbar::PositionChangingEvent* ev) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    int fontSize = ev->pos;
    SetDefaultAppearanceTextSize(annot, fontSize);
    TempStr s = str::FormatTemp(_TRA("Text Size: %d"), fontSize);
    ew->staticTextSize->SetText(s);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoTextColor(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FreeText) {
        return;
    }
    PdfColor col = DefaultAppearanceTextColor(annot);
    DropDownFillColors(ew->dropDownTextColor, col, ew->currTextColor);
    ew->staticTextColor->SetIsVisible(true);
    ew->dropDownTextColor->SetIsVisible(true);
}

static void TextColorSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownTextColor->GetCurrentSelection();
    char* item = ew->dropDownTextColor->items.At(idx);
    auto col = GetDropDownColor(item);
    SetDefaultAppearanceTextColor(annot, col);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoBorder(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!AnnotationSupportsBorder(annot->type)) {
        return;
    }
    int borderWidth = BorderWidth(annot);
    borderWidth = std::clamp(borderWidth, borderWidthMin, borderWidthMax);
    TempStr s = str::FormatTemp(_TRA("Border: %d"), borderWidth);
    ew->staticBorder->SetText(s);
    ew->trackbarBorder->SetValue(borderWidth);
    ew->staticBorder->SetIsVisible(true);
    ew->trackbarBorder->SetIsVisible(true);
}

static void BorderWidthChanging(EditAnnotationsWindow* ew, Trackbar::PositionChangingEvent* ev) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    int borderWidth = ev->pos;
    SetBorderWidth(annot, borderWidth);
    TempStr s = str::FormatTemp(_TRA("Border: %d"), borderWidth);
    ew->staticBorder->SetText(s);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoLineStartEnd(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::Line) {
        return;
    }
    int start = 0;
    int end = 0;
    GetLineEndingStyles(annot, &start, &end);
    ew->dropDownLineStart->SetItemsSeqStrings(gLineEndingStyles);
    ew->dropDownLineStart->SetCurrentSelection(start);
    ew->dropDownLineEnd->SetItemsSeqStrings(gLineEndingStyles);
    ew->dropDownLineEnd->SetCurrentSelection(end);
    ew->staticLineStart->SetIsVisible(true);
    ew->dropDownLineStart->SetIsVisible(true);
    ew->staticLineEnd->SetIsVisible(true);
    ew->dropDownLineEnd->SetIsVisible(true);
}

static void LineStartSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto start = ew->dropDownLineStart->GetCurrentSelection();
    if (start < 0) {
        return;
    }
    SetLineStartStyles(annot, start);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void LineEndSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto end = ew->dropDownLineEnd->GetCurrentSelection();
    if (end < 0) {
        return;
    }
    SetLineEndStyles(annot, end);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoIcon(EditAnnotationsWindow* ew, Annotation* annot) {
    const char* itemName = IconName(annot);
    const char* items = nullptr;
    switch (Type(annot)) {
        case AnnotationType::Text:
            items = gAnnotationTextIcons;
            break;
        case AnnotationType::FileAttachment:
            items = gFileAttachmentUcons;
            break;
        case AnnotationType::Sound:
            items = gSoundIcons;
            break;
        case AnnotationType::Stamp:
            items = gStampIcons;
            break;
        default:
            // no-op
            break;
    }
    if (!items || str::IsEmpty(itemName)) {
        return;
    }
    ew->dropDownIcon->SetItemsSeqStrings(items);
    int idx = FindStringInArray(items, itemName, 0);
    ew->dropDownIcon->SetCurrentSelection(idx);
    ew->staticIcon->SetIsVisible(true);
    ew->dropDownIcon->SetIsVisible(true);
}

static void IconSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownIcon->GetCurrentSelection();
    auto item = ew->dropDownIcon->items.At(idx);
    SetIconName(annot, item);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoColor(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!AnnotationSupportsColor(annot->type)) {
        return;
    }
    PdfColor col = GetColor(annot);
    DropDownFillColors(ew->dropDownColor, col, ew->currCustomColor);
    int n = dimof(gAnnotsIsColorBackground);
    bool isBgCol = IsAnnotationTypeInArray(gAnnotsIsColorBackground, n, Type(annot));
    if (isBgCol) {
        ew->staticColor->SetText(_TRA("Background Color:"));
    } else {
        ew->staticColor->SetText(_TRA("Color:"));
    }
    ew->staticColor->SetIsVisible(true);
    ew->dropDownColor->SetIsVisible(true);
}

static void ColorSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownColor->GetCurrentSelection();
    auto item = ew->dropDownColor->items.At(idx);
    auto col = GetDropDownColor(item);
    SetColor(annot, col);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoInteriorColor(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!AnnotationSupportsInteriorColor(annot->type)) {
        return;
    }
    PdfColor col = InteriorColor(annot);
    DropDownFillColors(ew->dropDownInteriorColor, col, ew->currCustomInteriorColor);
    ew->staticInteriorColor->SetIsVisible(true);
    ew->dropDownInteriorColor->SetIsVisible(true);
}

static void InteriorColorSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownInteriorColor->GetCurrentSelection();
    auto item = ew->dropDownInteriorColor->items.At(idx);
    auto col = GetDropDownColor(item);
    SetInteriorColor(annot, col);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoOpacity(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::Highlight) {
        return;
    }
    int opacity = Opacity(ew->tab->selectedAnnotation);
    TempStr s = str::FormatTemp(_TRA("Opacity: %d"), opacity);
    ew->staticOpacity->SetText(s);
    ew->staticOpacity->SetIsVisible(true);
    ew->trackbarOpacity->SetIsVisible(true);
    ew->trackbarOpacity->SetValue(opacity);
}

static void DoSaveEmbed(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FileAttachment) {
        return;
    }
    ew->buttonSaveAttachment->SetIsVisible(true);
    ew->buttonEmbedAttachment->SetIsVisible(true);
}

static void OpacityChanging(EditAnnotationsWindow* ew, Trackbar::PositionChangingEvent* ev) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    int opacity = ev->pos;
    SetOpacity(annot, opacity);
    TempStr s = str::FormatTemp(_TRA("Opacity: %d"), opacity);
    ew->staticOpacity->SetText(s);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

// TODO: maybe use ew->tab->selectedAnnotation instead of annot
static void UpdateUIForSelectedAnnotation(EditAnnotationsWindow* ew, Annotation* annot, bool isNew = false,
                                          EditAnnotFocus focus = EditAnnotFocus::Default) {
    HidePerAnnotControls(ew);
    if (annot) {
        int itemNo = ew->annotations.Find(annot);
        if (itemNo < 0) {
            // can happen if annotations list is out of sync (e.g. after reload)
            return;
        }

        DoRect(ew, annot);
        DoAuthor(ew, annot);
        DoModificationDate(ew, annot);
        DoPopup(ew, annot);
        DoContents(ew, annot);

        DoTextAlignment(ew, annot);
        DoTextFont(ew, annot);
        DoTextSize(ew, annot);
        DoTextColor(ew, annot);

        DoLineStartEnd(ew, annot);

        DoIcon(ew, annot);

        DoBorder(ew, annot);
        DoColor(ew, annot);
        DoInteriorColor(ew, annot);

        DoOpacity(ew, annot);
        DoSaveEmbed(ew, annot);

        ew->listBox->SetCurrentSelection(itemNo);
        ew->buttonDelete->SetIsVisible(true);

        if (focus == EditAnnotFocus::Edit) {
            HwndSetFocus(ew->editContents->hwnd);
            ew->editContents->SelectAll();
        } else if (focus == EditAnnotFocus::List) {
            HwndSetFocus(ew->listBox->hwnd);
        } else if (isNew && annot->type == AnnotationType::FreeText) {
            HwndSetFocus(ew->editContents->hwnd);
            // ew->editContents->SetCursorPositionAtEnd();
            ew->editContents->SelectAll();
        } else {
            HwndSetFocus(ew->listBox->hwnd);
        }
    }

    // Keep the current client size so wrapped static text (e.g. annotation excerpt)
    // is measured at the correct width.
    Rect client = ClientRect(ew->hwnd);
    if (client.dx > 0 && client.dy > 0) {
        LayoutToSize(ew->mainLayout, {client.dx, client.dy});
    }

    // Hiding a child window doesn't erase the area it previously occupied.
    // Annotation types expose different sets of controls, so switching between
    // them can otherwise leave the old controls painted behind the new layout.
    RedrawWindow(ew->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);

    if (!annot) {
        return;
    }
    if (ew->skipGoToPage) {
        ew->skipGoToPage = false;
        return;
    }

    int annotPageNo = annot->pageNo;
    DisplayModel* dm = ew->tab->AsFixed();
    int nPages = dm->PageCount();
    if (annotPageNo > nPages) {
        // see https://github.com/sumatrapdfreader/sumatrapdf/issues/1701
        logf("UpdateUIForSelectedAnnotation: invalid annotPageNo (%d), should be <= than nPages (%d)\n", annotPageNo,
             nPages);
        ReportIf(annotPageNo > nPages);
        return;
    }

    // don't switch pages if already visible. needed for cases where
    // we show more than one page at a time and GoToPage() scrolls
    // to top page
    // TODO: this is not perfect. We should skipGoToPage if this
    // is caused by creating an annotation. by definition the page
    // was visible when user created an annotation.
    // but that requires passing down more stuff
    if (!dm->PageVisible(annotPageNo)) {
        dm->GoToPage(annotPageNo, true);
    }
}

static void ButtonSaveAttachment(EditAnnotationsWindow* ew) {
    Annotation* annot = ew->tab->selectedAnnotation;
    ReportIf(!annot);
    if (!annot || annot->type != AnnotationType::FileAttachment) {
        return;
    }
    EngineMupdf* engine = GetEngineMupdf(ew);
    if (!engine) {
        return;
    }
    fz_context* ctx = engine->Ctx();
    pdf_annot* pdfannot = annot->pdfannot;
    if (!pdfannot) {
        return;
    }

    int objNum = pdf_to_num(ctx, pdf_annot_obj(ctx, pdfannot));
    ByteSlice data = EngineMupdfLoadAnnotAttachment((EngineBase*)engine, objNum);
    if (data.empty()) {
        return;
    }

    const char* fileName = nullptr;
    pdf_obj* fs = pdf_annot_filespec(ctx, pdfannot);
    if (fs) {
        pdf_filespec_params fileParams = {};
        pdf_get_filespec_params(ctx, fs, &fileParams);
        fileName = fileParams.filename;
    }
    if (str::IsEmpty(fileName)) {
        fileName = "attachment";
    }

    TempStr dir = path::GetDirTemp(ew->tab->filePath);
    fileName = path::GetBaseNameTemp(fileName);
    TempStr dstPath = path::JoinTemp(dir, fileName);
    SaveDataToFile(ew->hwnd, dstPath, data);
    str::Free(data.data());
}

static void ButtonEmbedAttachment(EditAnnotationsWindow* ew) {
    ReportIf(!ew->tab->selectedAnnotation);
    // TODO: implement me
    MessageBoxNYI(ew->hwnd);
}

void SetSelectedAnnotation(WindowTab* tab, Annotation* annot, bool isNew, EditAnnotFocus focus) {
    // when we delete an annotation we automatically pick one to
    // set as selected and it might end up as currently selected
    // we still want to redraw to not show deleted annotation
    // but not do the rest of the logic as it triggers infinite loop
    // TODO: maybe if we already have selected annotation, do not auto-pick
    MainWindow* win = tab->win;
    auto ew = tab->editAnnotsWindow;
    if (annot == tab->selectedAnnotation) {
        MainWindowRerender(win);
        if (ew) {
            UpdateUIForSelectedAnnotation(ew, annot, isNew, focus);
        }
        ToolbarUpdateStateForWindow(win, false);
        return;
    }
    if (ew) {
        FlushContentsFromEdit(ew);
    }
    tab->selectedAnnotation = annot;
    tab->didScrollToSelectedAnnotation = false;
    // go to page with a given annotations before triggering repaint
    if (ew) {
        UpdateUIForSelectedAnnotation(ew, annot, isNew, focus);
        HwndMakeVisible(ew->hwnd);
    }
    MainWindowRerender(win);
    ToolbarUpdateStateForWindow(win, false);
}

void UpdateAnnotationsList(EditAnnotationsWindow* ew) {
    if (!ew) {
        return;
    }
    auto engine = GetEngineMupdf(ew);
    EngineMupdfGetAnnotations(engine, ew->annotations);
    RebuildAnnotationsListBox(ew);
}

static void ButtonDeleteHandler(EditAnnotationsWindow* ew) {
    ReportIf(!ew->tab->selectedAnnotation);
    DeleteSelectedAnnotation(ew);
}

static void ListBoxSelectionChanged(EditAnnotationsWindow* ew) {
    ew->ListBoxSelectionChanged();
}

void EditAnnotationsWindow::ListBoxSelectionChanged() {
    int itemNo = listBox->GetCurrentSelection();
    if (itemNo < 0) {
        // an item has been deselected because e.g. selected annotation was deleted
        return;
    }
    if (!annotations.isValidIndex(itemNo)) {
        logfa("EditAnnotationsWindow::ListBoxSelectionChanged: invalid itemNo=%d, annotations.size()=%d\n", itemNo,
              annotations.Size());
        ReportDebugIf(true);
        return;
    }
    Annotation* annot = annotations.at(itemNo);
    SetSelectedAnnotation(tab, annot);
}

static UINT_PTR gMainWindowRerenderTimer = 0;
static MainWindow* gMainWindowForRender = nullptr;

// TODO: there seems to be a leak
static void ContentsChanged(EditAnnotationsWindow* ew) {
    if (ew->updatingControls) {
        return;
    }
    auto a = ew->tab->selectedAnnotation;
    // TODO: saw a crash when this was null
    ReportDebugIf(!a);
    if (!a) {
        return;
    }
    auto txt = ew->editContents->GetTextTemp();
    txt = str::ReplaceTemp(txt, "\r\n", "\n");
    SetContents(a, txt);
    EnableSaveIfAnnotationsChanged(ew);

    MainWindow* win = ew->tab->win;
    if (gMainWindowRerenderTimer != 0) {
        // logf("ContentsChanged: killing existing timer for re-render of MainWindow\n");
        KillTimer(win->hwndCanvas, gMainWindowRerenderTimer);
        gMainWindowRerenderTimer = 0;
    }
    UINT timeoutInMs = 1000;
    gMainWindowForRender = win;
    gMainWindowRerenderTimer = SetTimer(win->hwndCanvas, 1, timeoutInMs, [](HWND, UINT, UINT_PTR, DWORD) {
        if (IsMainWindowValid(gMainWindowForRender)) {
            // logf("ContentsChanged: re-rendering MainWindow\n");
            WindowTab* tab = gMainWindowForRender->CurrentTab();
            if (tab) {
                RerenderPdfAnnotationChange(tab, tab->selectedAnnotation);
            }
        } else {
            // logf("ContentsChanged: NOT re-rendering MainWindow because is not valid anymore\n");
        }
        gMainWindowRerenderTimer = 0;
    });
}

void EditAnnotationsWindow::OnSize(UINT msg, UINT, SIZE size) {
    if (msg != WM_SIZE) {
        return;
    }
    if (!mainLayout) {
        return;
    }
    int dx = (int)size.cx;
    int dy = (int)size.cy;
    if (dx == 0 || dy == 0) {
        return;
    }
    InvalidateRect(hwnd, nullptr, false);
    if (false && mainLayout->lastBounds.EqSize(dx, dy)) {
        // avoid un-necessary layout
        return;
    }
    LayoutToSize(mainLayout, {dx, dy});
}

static Static* CreateStatic(HWND parent, HFONT font, const char* s = nullptr) {
    auto w = new Static();
    Static::CreateArgs args;
    args.parent = parent;
    args.text = s;
    args.isRtl = IsUIRtl();
    args.font = font;
    HWND hwnd = w->Create(args);
    ReportIf(!hwnd);
    return w;
}

static void CreateMainLayout(EditAnnotationsWindow* ew) {
    HWND parent = ew->hwnd;
    auto vbox = new VBox();
    vbox->alignMain = MainAxisAlign::MainStart;
    vbox->alignCross = CrossAxisAlign::Stretch;
    int dpi = ew->dpi > 0 ? ew->dpi : DpiGet(parent);
    HFONT fnt = GetAppFontForDpi(dpi);

    {
        ListBox::CreateArgs args;
        args.parent = parent;
        args.idealSizeLines = 5;
        args.font = fnt;
        args.isRtl = IsUIRtl();
        auto w = new ListBox();
        w->SetInsetsPt(4, 0);
        w->Create(args);
        auto lbModel = new ListBoxModelStrings();
        w->SetModel(lbModel);
        w->onSelectionChanged = MkFunc0(ListBoxSelectionChanged, ew);
        ew->listBox = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt);
        ew->staticRect = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt);
        // WindowBaseLayout* l2 = (WindowBaseLayout*)l;
        // l2->SetInsetsPt(20, 0, 0, 0);
        ew->staticAuthor = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt);
        ew->staticModificationDate = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt);
        ew->staticPopup = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Contents:"));
        ew->staticContents = w;
        w->SetInsetsPt(4, 0, 0, 0);
        vbox->AddChild(w);
    }

    {
        Edit::CreateArgs args;
        args.parent = parent;
        args.isMultiLine = true;
        args.cueText = _TRA("Write a note…");
        args.idealSizeLines = 5;
        args.font = fnt;
        args.isRtl = IsUIRtl();
        args.withBorder = ThemeUsesDarkChrome();
        auto w = new Edit();
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);
        w->maxDx = MulDiv(150, dpi, 96);
        w->SetColors(ThemeWindowTextColor(), ThemeAnnotationContentsEditBackgroundColor());
        w->onTextChanged = MkFunc0(ContentsChanged, ew);
        ew->editContents = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Text Alignment:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticTextAlignment = w;
        vbox->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->SetItemsSeqStrings(gQuaddingNames);
        w->onSelectionChanged = MkFunc0(TextAlignmentSelectionChanged, ew);
        ew->dropDownTextAlignment = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Text Font:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticTextFont = w;
        vbox->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();
        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);

        w->Create(args);
        w->SetItemsSeqStrings(gQuaddingNames);
        w->onSelectionChanged = MkFunc0(TextFontSelectionChanged, ew);
        ew->dropDownTextFont = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Text Size:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticTextSize = w;
        vbox->AddChild(w);
    }

    {
        Trackbar::CreateArgs args;
        args.parent = parent;
        args.rangeMin = 8;
        args.rangeMax = 36;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Trackbar();
        w->SetInsetsPt(4, 0, 0, 0);

        w->Create(args);

        w->onPositionChanging = MkFunc1(TextFontSizeChanging, ew);
        ew->trackbarTextSize = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Text Color:"));
        ew->staticTextColor = w;
        vbox->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->SetItemsSeqStrings(gColors);
        w->onSelectionChanged = MkFunc0(TextColorSelectionChanged, ew);
        ew->dropDownTextColor = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Line Start:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticLineStart = w;
        vbox->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->onSelectionChanged = MkFunc0(LineStartSelectionChanged, ew);
        ew->dropDownLineStart = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Line End:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticLineEnd = w;
        vbox->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->onSelectionChanged = MkFunc0(LineEndSelectionChanged, ew);
        ew->dropDownLineEnd = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Icon:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticIcon = w;
        vbox->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->onSelectionChanged = MkFunc0(IconSelectionChanged, ew);
        ew->dropDownIcon = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, "Border:");
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticBorder = w;
        vbox->AddChild(w);
    }

    {
        Trackbar::CreateArgs args;
        args.parent = parent;
        args.rangeMin = borderWidthMin;
        args.rangeMax = borderWidthMax;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Trackbar();
        w->Create(args);
        w->onPositionChanging = MkFunc1(BorderWidthChanging, ew);
        ew->trackbarBorder = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Color:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticColor = w;
        vbox->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);
        w->SetItemsSeqStrings(gColors);
        w->onSelectionChanged = MkFunc0(ColorSelectionChanged, ew);
        ew->dropDownColor = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Interior Color:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticInteriorColor = w;
        vbox->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->SetItemsSeqStrings(gColors);
        w->onSelectionChanged = MkFunc0(InteriorColorSelectionChanged, ew);
        ew->dropDownInteriorColor = w;
        vbox->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Opacity:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticOpacity = w;
        vbox->AddChild(w);
    }

    {
        Trackbar::CreateArgs args;
        args.parent = parent;
        args.rangeMin = 0;
        args.rangeMax = 255;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Trackbar();
        w->Create(args);

        w->onPositionChanging = MkFunc1(OpacityChanging, ew);
        ew->trackbarOpacity = w;
        vbox->AddChild(w);
    }

    {
        Button::CreateArgs args;
        args.parent = parent;
        args.text = _TRA("Save...");
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Button();
        w->SetInsetsPt(8, 0, 0, 0);
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);

        w->onClick = MkFunc0(ButtonSaveAttachment, ew);
        ew->buttonSaveAttachment = w;
        vbox->AddChild(w);
    }

    {
        Button::CreateArgs args;
        args.parent = parent;
        args.text = _TRA("Embed...");
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Button();
        w->SetInsetsPt(8, 0, 0, 0);
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);

        w->onClick = MkFunc0(ButtonEmbedAttachment, ew);
        ew->buttonEmbedAttachment = w;
        vbox->AddChild(w);
    }

    {
        Button::CreateArgs args;
        args.parent = parent;
        args.text = _TRA("Delete Annotation");
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Button();
        w->SetInsetsPt(8, 0, 0, 0);
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);

        // TODO: doesn't work
        // w->SetTextColor(MkColor(0xff, 0, 0));

        w->onClick = MkFunc0(ButtonDeleteHandler, ew);
        ew->buttonDelete = w;
        vbox->AddChild(w);
    }

    {
        Button::CreateArgs args;
        args.parent = parent;
        args.text = _TRA("Export Notes");
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Button();
        w->SetInsetsPt(8, 0, 0, 0);
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);

        w->SetIsEnabled(false);
        w->onClick = MkFunc0(ExportClicked, ew);
        ew->buttonExport = w;
        vbox->AddChild(w);
    }

    {
        // Keep annotation actions with the selected annotation and reserve the
        // bottom of the window for document-level save actions.
        auto w = new Spacer(0, 0);
        vbox->AddChild(w, 1);
    }

    {
        Button::CreateArgs args;
        args.parent = parent;
        // TODO: maybe  file name e.g. "Save changes to foo.pdf"
        args.text = _TRA("Save changes to existing PDF");
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Button();
        w->SetInsetsPt(8, 0, 0, 0);
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);

        w->SetIsEnabled(false); // only enabled if there are changes
        w->onClick = MkFunc0(ButtonSaveToCurrentPDFHandler, ew);
        ew->buttonSaveToCurrentFile = w;
        vbox->AddChild(w);
    }

    {
        Button::CreateArgs args;
        args.parent = parent;
        // TODO: maybe  file name e.g. "Save changes to foo.pdf"
        args.text = _TRA("Save changes to a new PDF");
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Button();
        w->SetInsetsPt(8, 0, 0, 0);
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);

        w->SetIsEnabled(false); // only enabled if there are changes
        w->onClick = MkFunc0(ButtonSaveToNewFileHandler, ew);
        ew->buttonSaveToNewFile = w;
        vbox->AddChild(w);
    }

    auto padding = new Padding(vbox, DpiScaledInsets(parent, 4, 8));
    ew->mainLayout = padding;
    HidePerAnnotControls(ew);
}

static void LimitEditAnnotationsClientSizeToScreen(HWND hwnd, HWND hwndRelative, SIZE& size) {
    Rect work = GetWorkAreaRect(WindowRect(hwndRelative), hwndRelative);
    WINDOWINFO wi{};
    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(hwnd, &wi)) {
        LimitWindowSizeToScreen(hwndRelative, size);
        return;
    }

    int nonClientDx = RectDx(wi.rcWindow) - RectDx(wi.rcClient);
    int nonClientDy = RectDy(wi.rcWindow) - RectDy(wi.rcClient);
    int maxClientDx = work.dx - nonClientDx;
    int maxClientDy = work.dy - nonClientDy;
    if (size.cx > maxClientDx) {
        size.cx = maxClientDx;
    }
    if (size.cy > maxClientDy) {
        size.cy = maxClientDy;
    }
}

void ShowEditAnnotationsWindow(WindowTab* tab, Annotation* annot, EditAnnotFocus focus) {
    if (!tab) return;
    auto engine = tab->GetEngine();
    auto canAnnotate = EngineSupportsAnnotations(engine);
    if (!canAnnotate) {
        ReportDebugIf(true);
        return;
    }
    EditAnnotationsWindow* ew = tab->editAnnotsWindow;
    if (ew) {
        bool isNew = annot != ew->tab->win->annotationUnderCursor;
        HwndDockToRightOf(ew->hwnd, tab->win->hwndFrame);
        HwndMakeVisible(ew->hwnd);
        SetForegroundWindow(ew->hwnd);
        if (ew->listBox && ew->listBox->model->ItemsCount() > 0) {
            HwndSetFocus(ew->listBox->hwnd);
        }
        if (!annot) return;
        SetSelectedAnnotation(tab, annot, isNew, focus);
        return;
    }
    ew = new EditAnnotationsWindow();
    ew->onClose = MkFunc1Void(OnClose);
    CreateCustomArgs args;
    HMODULE h = GetModuleHandleW(nullptr);
    WCHAR* iconName = MAKEINTRESOURCEW(GetAppIconID());
    args.icon = LoadIconW(h, iconName);
    // mainWindow->isDialog = true;
    args.bgColor = ThemeWindowControlBackgroundColor();

    args.title = str::JoinTemp(_TRA("Annotations"), ": ", tab->GetTabTitle());
    args.visible = false;
    HWND parentHwnd = tab->win->hwndFrame;
    int parentDpi = tab->win->frameDpi > 0 ? tab->win->frameDpi : DpiGet(parentHwnd);
    args.font = GetAppFontForDpi(parentDpi);

    // PositionCloseTo(w, args->hwndRelatedTo);
    // SIZE winSize = {w->initialSize.dx, w->initialSize.Height};
    // LimitWindowSizeToScreen(args->hwndRelatedTo, winSize);
    // w->initialSize = {winSize.cx, winSize.cy};
    ew->CreateCustom(args);
    HwndDockToRightOf(ew->hwnd, tab->win->hwndFrame, MulDiv(520, parentDpi > 0 ? parentDpi : 96, 96));
    ew->dpi = parentDpi > 0 ? parentDpi : DpiGet(ew->hwnd);

    CreateMainLayout(ew);
    ew->tab = tab;
    tab->editAnnotsWindow = ew;

    UpdateAnnotationsList(ew);

    Rect lastPos = tab->lastEditAnnotsWindowPos;
    // Prefer a remembered width proportional to the main window; height follows the main window when docked.
    int mainWidth = WindowRect(tab->win->hwndFrame).dx;
    int width;
    if (lastPos.dx > 0 && tab->lastEditAnnotsWindowMainWidth > 0 && mainWidth > 0) {
        width = MulDiv(lastPos.dx, mainWidth, tab->lastEditAnnotsWindowMainWidth);
    } else if (lastPos.dx > 0 && tab->lastEditAnnotsWindowDpi > 0) {
        width = MulDiv(lastPos.dx, ew->dpi, tab->lastEditAnnotsWindowDpi);
    } else if (lastPos.dx > 0) {
        width = lastPos.dx;
    } else if (mainWidth > 0) {
        width = MulDiv(mainWidth, 520, 1920);
    } else {
        width = MulDiv(520, ew->dpi, 96);
    }
    int height = WindowRect(tab->win->hwndFrame).dy;
    if (height <= 0) {
        height = MulDiv(720, ew->dpi, 96);
        HWND hwnd = tab->win->hwndCanvas;
        auto rc = ClientRect(hwnd);
        if (rc.dy > 0) {
            height = rc.dy;
        }
    }

    // if it's a tall window, up the number of items in list box
    // from 5 to 14
    if (height > MulDiv(1024, ew->dpi, 96)) {
        ew->listBox->idealSizeLines = 14;
    }

    SIZE size = {width, height};
    LimitEditAnnotationsClientSizeToScreen(ew->hwnd, tab->win->hwndFrame, size);
    LayoutAndSizeToContent(ew->mainLayout, size.cx, size.cy, ew->hwnd);
    HwndDockToRightOf(ew->hwnd, tab->win->hwndFrame, WindowRect(ew->hwnd).dx);

    if (!annot) annot = ew->tab->selectedAnnotation;
    ew->skipGoToPage = (annot != nullptr);
    if (annot) {
        bool isNew = annot != ew->tab->win->annotationUnderCursor;
        SetSelectedAnnotation(tab, annot, isNew, focus);
    }
    ApplyEditAnnotationsWindowTheme(ew, true);

    // important to call this after hooking up onSize to ensure
    // first layout is triggered
    ew->SetIsVisible(true);
}
