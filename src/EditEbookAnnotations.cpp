/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "Annotation.h"
#include "EngineBase.h"
#include "Translations.h"
#include "SumatraConfig.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "EbookAnnotations.h"
#include "EditAnnotations.h"
#include "EditEbookAnnotations.h"
#include "SumatraPDF.h"
#include "Theme.h"

#include "DarkModeSubclass.h"

static const char* kEbookAnnotationColorNames = "Orange\0Yellow\0Green\0Blue\0Red\0Purple\0Pink\0";
static const COLORREF kEbookAnnotationColors[] = {
    RGB(255, 184, 77), RGB(255, 235, 59), RGB(76, 175, 80),  RGB(66, 133, 244),
    RGB(239, 83, 80),  RGB(156, 39, 176), RGB(236, 64, 122),
};

struct EbookAnnotationsWindow : Wnd {
    WindowTab* tab = nullptr;
    LayoutBase* mainLayout = nullptr;
    ListBox* listBox = nullptr;
    Static* staticText = nullptr;
    Static* staticAuthor = nullptr;
    Static* staticDate = nullptr;
    Static* staticContents = nullptr;
    Edit* editContents = nullptr;
    Static* staticColor = nullptr;
    DropDown* dropDownColor = nullptr;
    Button* buttonDelete = nullptr;
    Button* buttonExport = nullptr;
    Vec<EbookAnnotation*> annotations;
    EbookAnnotation* selected = nullptr;
    bool updatingControls = false;
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

static void GetEditAnnotationsThemeColors(COLORREF& textOut, COLORREF& bgOut) {
    textOut = ThemeWindowTextColor();
    bgOut = ThemeWindowControlBackgroundColor();
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

    if (UseDarkModeLib()) {
        if (installDarkMode) {
            DarkMode::setDarkWndNotifySafe(window->hwnd);
            DarkMode::setWindowEraseBgSubclass(window->hwnd);
        } else {
            DarkMode::setDarkTitleBarEx(window->hwnd, true);
            DarkMode::setWindowCtlColorSubclass(window->hwnd);
            DarkMode::setChildCtrlsTheme(window->hwnd);
        }
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

void DockOpenEbookAnnotationsWindows(MainWindow* win) {
    if (!win || !win->hwndFrame) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        EbookAnnotationsWindow* window = tab->editEbookAnnotsWindow;
        if (window && window->hwnd && IsWindowVisible(window->hwnd)) {
            HwndDockToRightOf(window->hwnd, win->hwndFrame);
        }
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
        ShowEditEbookAnnotationsWindow(tab, nullptr);
    }
}

static int FindColorIndex(COLORREF color) {
    for (int i = 0; i < (int)dimof(kEbookAnnotationColors); i++) {
        if (kEbookAnnotationColors[i] == color) {
            return i;
        }
    }
    return -1;
}

static void FillColorDropDown(EbookAnnotationsWindow* window, COLORREF color) {
    StrVec items;
    const char* name = kEbookAnnotationColorNames;
    while (name) {
        items.Append(name);
        seqstrings::Next(name);
    }
    int idx = FindColorIndex(color);
    if (idx < 0) {
        items.Append(SerializeColorTemp(color));
        idx = items.Size() - 1;
    }
    window->dropDownColor->SetItems(items);
    window->dropDownColor->SetCurrentSelection(idx);
}

static COLORREF GetSelectedColor(EbookAnnotationsWindow* window) {
    int idx = window->dropDownColor->GetCurrentSelection();
    if (idx >= 0 && idx < (int)dimof(kEbookAnnotationColors)) {
        return kEbookAnnotationColors[idx];
    }
    const char* item = window->dropDownColor->items.At(idx);
    return ParseColor(item, RGB(255, 184, 77));
}

static void HideAnnotationControls(EbookAnnotationsWindow* window) {
    window->staticText->SetIsVisible(false);
    window->staticAuthor->SetIsVisible(false);
    window->staticDate->SetIsVisible(false);
    window->staticContents->SetIsVisible(false);
    window->editContents->SetIsVisible(false);
    window->staticColor->SetIsVisible(false);
    window->dropDownColor->SetIsVisible(false);
    window->buttonDelete->SetIsVisible(false);
}

static void UpdateExportButton(EbookAnnotationsWindow* window) {
    bool hasAnnotations = window->listBox && window->listBox->GetCount() > 0;
    window->buttonExport->SetIsEnabled(hasAnnotations);
}

// Keep the current client size when updating selection. LayoutAndSizeToContent()
// would grow the window to fit long excerpt text (VBox min intrinsic width),
// pushing the right edge onto the next monitor.
static void RelayoutEbookAnnotationsWindow(EbookAnnotationsWindow* window) {
    if (!window || !window->mainLayout || !window->hwnd) {
        return;
    }
    Rect client = ClientRect(window->hwnd);
    if (client.dx <= 0 || client.dy <= 0) {
        return;
    }
    LayoutToSize(window->mainLayout, {client.dx, client.dy});
}

static void UpdateSelectedAnnotation(EbookAnnotationsWindow* window, EbookAnnotation* annotation,
                                     EditAnnotFocus focus = EditAnnotFocus::Default) {
    window->selected = annotation;
    HideAnnotationControls(window);
    if (annotation) {
        int idx = window->annotations.Find(annotation);
        if (idx < 0) {
            window->selected = nullptr;
            return;
        }
        window->updatingControls = true;
        TempStr excerpt = ShortenStringUtf8Temp(EbookAnnotationGetText(annotation), 100);
        window->staticText->SetText(excerpt);
        TempStr note = str::ReplaceTemp(EbookAnnotationGetNote(annotation), "\r\n", "\n");
        note = str::ReplaceTemp(note, "\n", "\r\n");
        window->editContents->SetText(note);
        FillColorDropDown(window, EbookAnnotationGetColor(annotation));

        const char* author = EbookAnnotationGetAuthor(annotation);
        if (!str::IsEmpty(author)) {
            StrBuilder authorText;
            authorText.AppendFmt("%s: %s", _TRA("Author"), author);
            window->staticAuthor->SetText(authorText.Get());
            window->staticAuthor->SetIsVisible(true);
        }
        time_t date = EbookAnnotationGetModified(annotation);
        if (date <= 0) {
            date = EbookAnnotationGetCreated(annotation);
        }
        if (date > 0) {
            StrBuilder dateText;
            dateText.Append(_TRA("Date:"));
            dateText.Append(" ");
            struct tm tm;
            gmtime_s(&tm, &date);
            char buf[100];
            strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
            dateText.Append(buf);
            window->staticDate->SetText(dateText.Get());
            window->staticDate->SetIsVisible(true);
        }

        window->updatingControls = false;

        window->staticText->SetIsVisible(true);
        window->staticContents->SetIsVisible(true);
        window->editContents->SetIsVisible(true);
        window->staticColor->SetIsVisible(true);
        window->dropDownColor->SetIsVisible(true);
        window->buttonDelete->SetIsVisible(true);
        window->listBox->SetCurrentSelection(idx);

        int pageNo = EbookAnnotationGetPageNo(window->tab, annotation);
        DisplayModel* dm = window->tab->AsFixed();
        if (dm && dm->ValidPageNo(pageNo) && !dm->PageVisible(pageNo)) {
            dm->GoToPage(pageNo, true);
        }
        if (focus == EditAnnotFocus::Edit || EbookAnnotationGetType(annotation) == AnnotationType::Text) {
            HwndSetFocus(window->editContents->hwnd);
            window->editContents->SelectAll();
        } else {
            HwndSetFocus(window->listBox->hwnd);
        }
    }
    RelayoutEbookAnnotationsWindow(window);
    MainWindowRerender(window->tab->win);
}

static void RebuildList(EbookAnnotationsWindow* window) {
    EbookAnnotation* selected = window->selected;
    window->annotations.Reset();
    EbookAnnotationsGetAll(window->tab, window->annotations);
    auto model = new ListBoxModelStrings();
    StrBuilder text;
    for (EbookAnnotation* annotation : window->annotations) {
        text.Reset();
        int pageNo = EbookAnnotationGetPageNo(window->tab, annotation);
        text.AppendFmt(_TRA("page %d,"), pageNo);
        text.AppendFmt(" %s", AnnotationReadableNameTemp(EbookAnnotationGetType(annotation)));
        const char* exact = EbookAnnotationGetText(annotation);
        if (!str::IsEmpty(exact)) {
            text.Append(" - ");
            text.Append(ShortenStringUtf8Temp(exact, 48));
        }
        model->strings.Append(text.Get());
    }
    window->listBox->SetModel(model);
    int idx = window->annotations.Find(selected);
    if (idx >= 0) {
        window->listBox->SetCurrentSelection(idx);
    }
    UpdateExportButton(window);
}

void UpdateEbookAnnotationsList(EbookAnnotationsWindow* window) {
    if (window) {
        RebuildList(window);
    }
}

static void ListSelectionChanged(EbookAnnotationsWindow* window) {
    int idx = window->listBox->GetCurrentSelection();
    if (window->annotations.isValidIndex(idx)) {
        UpdateSelectedAnnotation(window, window->annotations.at(idx));
    }
}

static void ContentsChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected) {
        return;
    }
    TempStr note = window->editContents->GetTextTemp();
    note = str::ReplaceTemp(note, "\r\n", "\n");
    EbookAnnotationSetNote(window->tab, window->selected, note);
}

static void ColorSelectionChanged(EbookAnnotationsWindow* window) {
    if (window->updatingControls || !window->selected) {
        return;
    }
    if (EbookAnnotationSetColor(window->tab, window->selected, GetSelectedColor(window))) {
        MainWindowRerender(window->tab->win);
    }
}

static void DeleteSelected(EbookAnnotationsWindow* window) {
    if (!window->selected) {
        return;
    }
    EbookAnnotation* selected = window->selected;
    window->selected = nullptr;
    if (!EbookAnnotationsDelete(window->tab, selected)) {
        window->selected = selected;
        return;
    }
    RebuildList(window);
    UpdateSelectedAnnotation(window, nullptr);
}

static void DeleteClicked(EbookAnnotationsWindow* window) {
    DeleteSelected(window);
}

static void ExportClicked(EbookAnnotationsWindow* window) {
    EbookAnnotationsExportNotes(window->tab, window->hwnd);
}

static void OnClose(Wnd::CloseEvent* event) {
    auto window = (EbookAnnotationsWindow*)event->e->self;
    HWND activate = window->tab->win->hwndFrame;
    window->tab->editEbookAnnotsWindow = nullptr;
    delete window;
    SetActiveWindow(activate);
}

void EbookAnnotationsWindow::OnFocus() {
    SelectTabInWindow(tab);
}

void EbookAnnotationsWindow::OnSize(UINT msg, UINT, SIZE size) {
    if (msg == WM_SIZE && mainLayout) {
        LayoutToSize(mainLayout, {(int)size.cx, (int)size.cy});
    }
}

bool EbookAnnotationsWindow::PreTranslateMessage(MSG& msg) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_DELETE && ::GetFocus() != editContents->hwnd) {
        DeleteSelected(this);
        return true;
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
    delete mainLayout;
}

static void CreateMainLayout(EbookAnnotationsWindow* window) {
    HWND parent = window->hwnd;
    int dpi = window->dpi > 0 ? window->dpi : DpiGet(parent);
    HFONT font = GetAppFontForDpi(dpi);
    auto vbox = new VBox();
    vbox->alignMain = MainAxisAlign::MainStart;
    vbox->alignCross = CrossAxisAlign::Stretch;

    ListBox::CreateArgs listArgs;
    listArgs.parent = parent;
    listArgs.idealSizeLines = 8;
    listArgs.font = font;
    listArgs.isRtl = IsUIRtl();
    auto list = new ListBox();
    list->Create(listArgs);
    list->insets = DpiScaledInsets(parent, 4, 0);
    list->idealSize = {MulDiv(120, dpi, 96), MulDiv(32, dpi, 96)};
    list->SetModel(new ListBoxModelStrings());
    list->onSelectionChanged = MkFunc0(ListSelectionChanged, window);
    window->listBox = list;
    vbox->AddChild(list);

    window->staticText = CreateStatic(parent, font);
    window->staticText->insets = DpiScaledInsets(parent, 6, 0, 0, 0);
    vbox->AddChild(window->staticText);

    window->staticAuthor = CreateStatic(parent, font);
    window->staticAuthor->insets = DpiScaledInsets(parent, 4, 0, 0, 0);
    vbox->AddChild(window->staticAuthor);

    window->staticDate = CreateStatic(parent, font);
    window->staticDate->insets = DpiScaledInsets(parent, 2, 0, 0, 0);
    vbox->AddChild(window->staticDate);

    window->staticContents = CreateStatic(parent, font, _TRA("Contents:"));
    window->staticContents->insets = DpiScaledInsets(parent, 8, 0, 0, 0);
    vbox->AddChild(window->staticContents);

    Edit::CreateArgs editArgs;
    editArgs.parent = parent;
    editArgs.isMultiLine = true;
    editArgs.withBorder = true;
    editArgs.idealSizeLines = 6;
    editArgs.font = font;
    editArgs.isRtl = IsUIRtl();
    auto edit = new Edit();
    ReportIf(!edit->Create(editArgs));
    edit->maxDx = MulDiv(180, dpi, 96);
    edit->onTextChanged = MkFunc0(ContentsChanged, window);
    window->editContents = edit;
    vbox->AddChild(edit);

    window->staticColor = CreateStatic(parent, font, _TRA("Color:"));
    window->staticColor->insets = DpiScaledInsets(parent, 8, 0, 0, 0);
    vbox->AddChild(window->staticColor);

    DropDown::CreateArgs colorArgs;
    colorArgs.parent = parent;
    colorArgs.font = font;
    colorArgs.isRtl = IsUIRtl();
    auto color = new DropDown();
    color->Create(colorArgs);
    color->insets = DpiScaledInsets(parent, 4, 0, 0, 0);
    color->onSelectionChanged = MkFunc0(ColorSelectionChanged, window);
    window->dropDownColor = color;
    vbox->AddChild(color);

    auto spacer = new Spacer(0, 0);
    vbox->AddChild(spacer, 1);

    Button::CreateArgs buttonArgs;
    buttonArgs.parent = parent;
    buttonArgs.text = _TRA("Delete Annotation");
    buttonArgs.font = font;
    buttonArgs.isRtl = IsUIRtl();
    auto button = new Button();
    ReportIf(!button->Create(buttonArgs));
    button->insets = DpiScaledInsets(parent, 10, 0, 0, 0);
    button->onClick = MkFunc0(DeleteClicked, window);
    window->buttonDelete = button;
    vbox->AddChild(button);

    Button::CreateArgs exportArgs;
    exportArgs.parent = parent;
    exportArgs.text = _TRA("Export Notes");
    exportArgs.font = font;
    exportArgs.isRtl = IsUIRtl();
    auto exportButton = new Button();
    ReportIf(!exportButton->Create(exportArgs));
    exportButton->insets = DpiScaledInsets(parent, 8, 0, 0, 0);
    exportButton->onClick = MkFunc0(ExportClicked, window);
    exportButton->SetIsEnabled(false);
    window->buttonExport = exportButton;
    vbox->AddChild(exportButton);

    window->mainLayout = new Padding(vbox, DpiScaledInsets(parent, 4, 8));
    HideAnnotationControls(window);
}

bool CloseAndDeleteEditEbookAnnotationsWindow(WindowTab* tab) {
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

static void LimitEbookAnnotationsClientSizeToScreen(HWND hwnd, HWND hwndRelative, SIZE& size) {
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

void ShowEditEbookAnnotationsWindow(WindowTab* tab, EbookAnnotation* annotation, EditAnnotFocus focus) {
    if (!tab || !EbookAnnotationsSupported(tab)) {
        return;
    }
    EbookAnnotationsWindow* window = tab->editEbookAnnotsWindow;
    if (window) {
        HwndDockToRightOf(window->hwnd, tab->win->hwndFrame);
        HwndMakeVisible(window->hwnd);
        SetForegroundWindow(window->hwnd);
        RebuildList(window);
        if (annotation) {
            UpdateSelectedAnnotation(window, annotation, focus);
        } else if (window->listBox->GetCount() > 0) {
            UpdateSelectedAnnotation(window, window->annotations.at(0), focus);
        }
        HwndDockToRightOf(window->hwnd, tab->win->hwndFrame);
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
    HWND parentHwnd = tab->win->hwndFrame;
    int parentDpi = tab->win->frameDpi > 0 ? tab->win->frameDpi : DpiGet(parentHwnd);
    args.font = GetAppFontForDpi(parentDpi);
    // Seed position/size on the main window's monitor so the first dock/DPI
    // measurement is not based on CW_USEDEFAULT (which can land anywhere).
    {
        Rect frame = WindowRect(parentHwnd);
        int seedWidth = MulDiv(520, parentDpi > 0 ? parentDpi : 96, 96);
        args.pos = {frame.x + frame.dx, frame.y, seedWidth, frame.dy > 0 ? frame.dy : seedWidth};
    }
    window->CreateCustom(args);
    HwndDockToRightOf(window->hwnd, tab->win->hwndFrame, MulDiv(520, parentDpi > 0 ? parentDpi : 96, 96));
    window->dpi = parentDpi > 0 ? parentDpi : DpiGet(window->hwnd);
    CreateMainLayout(window);
    tab->editEbookAnnotsWindow = window;
    RebuildList(window);

    Rect lastPos = tab->lastEditAnnotsWindowPos;
    // Prefer a remembered width proportional to the main window; height follows the main window when docked.
    int mainWidth = WindowRect(tab->win->hwndFrame).dx;
    int width;
    if (lastPos.dx > 0 && tab->lastEditAnnotsWindowMainWidth > 0 && mainWidth > 0) {
        width = MulDiv(lastPos.dx, mainWidth, tab->lastEditAnnotsWindowMainWidth);
    } else if (lastPos.dx > 0 && tab->lastEditAnnotsWindowDpi > 0) {
        width = MulDiv(lastPos.dx, window->dpi, tab->lastEditAnnotsWindowDpi);
    } else if (lastPos.dx > 0) {
        width = lastPos.dx;
    } else if (mainWidth > 0) {
        width = MulDiv(mainWidth, 520, 1920);
    } else {
        width = MulDiv(520, window->dpi, 96);
    }
    int height = WindowRect(tab->win->hwndFrame).dy;
    if (height <= 0) {
        height = MulDiv(720, window->dpi, 96);
        HWND hwnd = tab->win->hwndCanvas;
        auto rc = ClientRect(hwnd);
        if (rc.dy > 0) {
            height = rc.dy;
        }
    }

    if (height > MulDiv(1024, window->dpi, 96)) {
        window->listBox->idealSizeLines = 14;
    }

    SIZE size = {width, height};
    LimitEbookAnnotationsClientSizeToScreen(window->hwnd, tab->win->hwndFrame, size);
    LayoutAndSizeToContent(window->mainLayout, size.cx, size.cy, window->hwnd);
    // preferredWidth is outer window width, not client width (see HwndDockToRightOf).
    HwndDockToRightOf(window->hwnd, tab->win->hwndFrame, WindowRect(window->hwnd).dx);

    if (annotation) {
        UpdateSelectedAnnotation(window, annotation, focus);
    } else if (!window->annotations.empty()) {
        UpdateSelectedAnnotation(window, window->annotations.at(0), focus);
    }
    ApplyEbookAnnotationsWindowTheme(window, true);
    window->SetIsVisible(true);
}
