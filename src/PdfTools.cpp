/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayMode.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Translations.h"
#include "ExternalViewers.h"
#include "Flags.h"
#include "DisplayModel.h"
#include "Theme.h"
#include "AppDialogTheme.h"
#include "AppSettings.h"
#include "CaptionGlyphs.h"
#include "PdfPageAdjustment.h"
#include "SvgIcons.h"
#include "Toolbar.h"

#include "DarkModeSubclass.h"
#include "Notifications.h"

extern "C" int pdfbake_main(int argc, char** argv);
extern "C" int pdfclean_main(int argc, char** argv);
extern "C" int muconvert_main(int argc, char** argv);
extern "C" void fz_set_optind(int val);

// offset to align static label text with text inside an edit control
// accounts for WS_EX_CLIENTEDGE border (2px) + edit internal left margin (~2px)
constexpr int kEditTextXOffset = 4;

// DPI-scaled dialog layout metrics
struct DlgMetrics {
    int padding;
    int rowH;    // row height for edit controls and labels
    int rowGap;  // vertical gap between rows
    int btnW;    // button width
    int btnH;    // button height
    int btnGap;  // gap between buttons
    int browseW; // width of "..." browse button
    int editXOff;
};

static DlgMetrics GetDlgMetrics(HWND hwnd, HFONT hFont) {
    DlgMetrics m;
    m.padding = DpiScale(hwnd, 10);
    m.rowGap = DpiScale(hwnd, 6);
    m.editXOff = DpiScale(hwnd, kEditTextXOffset);

    // measure row height from the font
    Size textSize = HwndMeasureText(hwnd, "Xg", hFont);
    m.rowH = textSize.dy + DpiScale(hwnd, 8); // text height + border/padding

    // measure button size from a representative label
    m.btnW = DpiScale(hwnd, 75);
    m.btnH = m.rowH;
    m.btnGap = DpiScale(hwnd, 4);
    m.browseW = DpiScale(hwnd, 30);
    return m;
}

static int CalcDlgWidth(HWND hwndParent, HFONT font, const char* path, int minW, int padding) {
    HDC hdc = GetDC(nullptr);
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    TempWStr pathW = ToWStrTemp(path);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, pathW, str::Leni(pathW), &sz);
    SelectObject(hdc, oldFont);
    ReleaseDC(nullptr, hdc);
    int dlgW = sz.cx + 2 * padding + DpiScale(hwndParent, 32);
    dlgW = std::max(dlgW, minW);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    dlgW = std::min(dlgW, screenW * 80 / 100);
    return dlgW;
}

// calculate dialog height from number of rows
// extra 32 accounts for non-client area (title bar etc.)
static int CalcDlgHeight(HWND hwnd, const DlgMetrics& m, int nRows) {
    return 2 * m.padding + nRows * m.rowH + (nRows - 1) * m.rowGap + DpiScale(hwnd, 32);
}

static HFONT CreatePdfToolFont(int dpi) {
    AppDialogFonts fonts;
    fonts.CreateForDpi(dpi);
    HFONT body = fonts.body;
    fonts.body = nullptr;
    return body;
}

// `ready` stays false until children exist, so a WM_DPICHANGED delivered
// during CreateWindow does not scale an empty window.
static void PdfToolHandleDpi(HWND hwnd, int* dpiSlot, HFONT* fontSlot, bool* fontOwned, bool ready, WPARAM wp,
                             LPARAM lp) {
    if (!ready || !dpiSlot || !fontSlot) {
        return;
    }
    int dpi = RoundUp((int)LOWORD(wp), 4);
    if (dpi < 72) {
        dpi = 96;
    }
    int old = *dpiSlot;
    if (dpi != old && old >= 72) {
        HFONT next = CreatePdfToolFont(dpi);
        if (next) {
            HFONT prev = *fontSlot;
            *fontSlot = next;
            AppDialogApplyFontToChildren(hwnd, next);
            if (fontOwned && *fontOwned && prev) {
                DeleteObject(prev);
            }
            if (fontOwned) {
                *fontOwned = true;
            }
        }
        DpiResizeChildren(hwnd, old, dpi);
        *dpiSlot = dpi;
    }
    RECT* prc = (RECT*)lp;
    if (prc) {
        SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

struct PdfBakeDialog {
    HWND hwnd = nullptr;
    HWND hwndPathLabel = nullptr;
    HWND hwndDestEdit = nullptr;
    HWND hwndBrowseBtn = nullptr;
    HWND hwndBakeBtn = nullptr;
    HWND hwndCancelBtn = nullptr;
    HFONT hFont = nullptr;
    int dpi = 96;
    bool fontOwned = false;
    bool dpiReady = false;
    char* srcPath = nullptr;
    MainWindow* win = nullptr;
};

static void PdfBakeOnBrowse(PdfBakeDialog* dlg) {
    WCHAR dstFileName[MAX_PATH + 1]{};
    GetWindowTextW(dlg->hwndDestEdit, dstFileName, MAX_PATH);

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg->hwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"pdf";

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(dlg->hwndDestEdit, dstFileName);
    }
}

static void PdfBakeDoIt(PdfBakeDialog* dlg) {
    char destPath[MAX_PATH + 1]{};
    GetWindowTextA(dlg->hwndDestEdit, destPath, MAX_PATH);
    if (str::IsEmpty(destPath)) {
        return;
    }

    logf("PdfBakeDoIt: baking '%s' to '%s'\n", dlg->srcPath, destPath);

    // build argv for pdfbake_main: "bake" input output
    char* argv[] = {(char*)"bake", dlg->srcPath, destPath};
    int argc = 3;

    fz_set_optind(0);
    int res = pdfbake_main(argc, argv);
    if (res == 0) {
        logf("PdfBakeDoIt: baked successfully\n");
        MainWindow* win = dlg->win;
        TempStr path = str::DupTemp(destPath);
        DestroyWindow(dlg->hwnd);
        // open the baked file
        LoadArgs args(path, win);
        StartLoadDocument(&args);
    } else {
        logf("PdfBakeDoIt: pdfbake_main failed with %d\n", res);
        MessageBoxWarning(dlg->hwnd, _TRA("Failed to bake PDF file."), _TRA("Bake PDF"));
    }
}

static LRESULT CALLBACK PdfBakeDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PdfBakeDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        dlg = (PdfBakeDialog*)cs->lpCreateParams;
        dlg->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (PdfBakeDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_COMMAND: {
            int code = HIWORD(wp);
            HWND ctl = (HWND)lp;
            if (ctl == dlg->hwndBrowseBtn && code == BN_CLICKED) {
                PdfBakeOnBrowse(dlg);
                return 0;
            }
            if (ctl == dlg->hwndBakeBtn && code == BN_CLICKED) {
                PdfBakeDoIt(dlg);
                return 0;
            }
            if (ctl == dlg->hwndCancelBtn && code == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DPICHANGED:
            PdfToolHandleDpi(hwnd, &dlg->dpi, &dlg->hFont, &dlg->fontOwned, dlg->dpiReady, wp, lp);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (dlg->fontOwned && dlg->hFont) {
                DeleteObject(dlg->hFont);
                dlg->hFont = nullptr;
            }
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kPdfBakeWinClassName = L"SUMATRA_PDF_BAKE";
static bool gPdfBakeWinClassRegistered = false;

void ShowPdfBakeDialog(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    if (!CouldBePDFDoc(tab)) {
        return;
    }
    logf("ShowPdfBakeDialog: opening for '%s'\n", tab->filePath);

    if (!gPdfBakeWinClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PdfBakeDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPdfBakeWinClassName;
        RegisterClassExW(&wc);
        gPdfBakeWinClassRegistered = true;
    }

    PdfBakeDialog* dlg = new PdfBakeDialog();
    dlg->srcPath = str::Dup(tab->filePath);
    dlg->win = win;
    dlg->dpi = DpiGet(win->hwndFrame);
    if (dlg->dpi < 72) {
        dlg->dpi = 96;
    }
    dlg->hFont = CreatePdfToolFont(dlg->dpi);
    dlg->fontOwned = dlg->hFont != nullptr;
    if (!dlg->hFont) {
        dlg->hFont = GetDefaultGuiFont();
    }

    DlgMetrics m = GetDlgMetrics(win->hwndFrame, dlg->hFont);
    int minW = DpiScale(win->hwndFrame, 500);
    int dlgW = CalcDlgWidth(win->hwndFrame, dlg->hFont, tab->filePath, minW, m.padding);
    int dlgH = CalcDlgHeight(win->hwndFrame, m, 3);

    HINSTANCE h = GetModuleHandleW(nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPdfBakeWinClassName, _TRW("Bake PDF"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                dlgW, dlgH, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        str::Free(dlg->srcPath);
        delete dlg;
        return;
    }

    int x = m.padding;
    int y = m.padding;
    int w = dlgW - 2 * m.padding - DpiScale(hwnd, 16);

    // row 1: source path label (offset to align with text inside edit control)
    dlg->hwndPathLabel =
        CreateWindowExW(0, L"STATIC", ToWStrTemp(tab->filePath), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                        x + m.editXOff, y, w - m.editXOff, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPathLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 2: dest edit + browse button
    TempStr destPath = MakeUniqueFilePathTemp(tab->filePath);
    dlg->hwndDestEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(destPath), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y,
                        w - m.browseW - m.btnGap, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDestEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + w - m.browseW,
                                         y, m.browseW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndBrowseBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 3: Bake + Cancel buttons (right-aligned)
    int bx = x + w - m.btnW;
    dlg->hwndCancelBtn = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx, y,
                                         m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndCancelBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    bx -= m.btnW + m.btnGap;
    dlg->hwndBakeBtn = CreateWindowExW(0, L"BUTTON", _TRW("Bake PDF"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, bx, y,
                                       m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndBakeBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    CenterDialog(hwnd, win->hwndFrame);
    if (UseDarkModeLib()) {
        DarkMode::setDarkWndSafe(hwnd);
        DarkMode::setWindowEraseBgSubclass(hwnd);
    }
    UpdateWindowCaptionTheme(hwnd);
    dlg->dpiReady = true;
    ShowWindow(hwnd, SW_SHOW);
}

// --- Extract PDF Text dialog ---

struct PdfExtractTextDialog {
    HWND hwnd = nullptr;
    HWND hwndPathLabel = nullptr;
    HWND hwndDestEdit = nullptr;
    HWND hwndBrowseBtn = nullptr;
    HWND hwndPagesLabel = nullptr;
    HWND hwndPagesEdit = nullptr;
    HWND hwndExtractBtn = nullptr;
    HWND hwndCancelBtn = nullptr;
    HFONT hFont = nullptr;
    int dpi = 96;
    bool fontOwned = false;
    bool dpiReady = false;
    char* srcPath = nullptr;
    MainWindow* win = nullptr;
};

static void PdfExtractTextOnBrowse(PdfExtractTextDialog* dlg) {
    WCHAR dstFileName[MAX_PATH + 1]{};
    GetWindowTextW(dlg->hwndDestEdit, dstFileName, MAX_PATH);

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg->hwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = L"Text Files\0*.txt\0All Files\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"txt";

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(dlg->hwndDestEdit, dstFileName);
    }
}

static bool ExtractTextViaEngine(PdfExtractTextDialog* dlg, const char* destPath, const char* pages) {
    MainWindow* win = dlg->win;
    if (!win || !win->ctrl) {
        return false;
    }
    DisplayModel* dm = win->ctrl->AsFixed();
    if (!dm) {
        return false;
    }
    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }
    int pageCount = engine->PageCount();
    Vec<PageRange> ranges;
    if (!ParsePageRanges(pages, ranges)) {
        return false;
    }
    StrBuilder text;
    for (auto& range : ranges) {
        int start = std::max(range.start, 1);
        int end = std::min(range.end, pageCount);
        for (int pageNo = start; pageNo <= end; pageNo++) {
            PageTextUtf8 pt = engine->ExtractPageTextUtf8(pageNo);
            if (pt.text) {
                text.Append(pt.text);
                text.AppendChar('\n');
            }
            FreePageTextUtf8(&pt);
        }
    }
    return file::WriteFile(destPath, text.AsByteSlice());
}

static void PdfExtractTextDoIt(PdfExtractTextDialog* dlg) {
    char destPath[MAX_PATH + 1]{};
    GetWindowTextA(dlg->hwndDestEdit, destPath, MAX_PATH);
    if (str::IsEmpty(destPath)) {
        return;
    }

    char pages[256]{};
    GetWindowTextA(dlg->hwndPagesEdit, pages, dimof(pages) - 1);
    if (str::IsEmpty(pages)) {
        return;
    }

    logf("PdfExtractTextDoIt: extracting text from '%s' to '%s', pages: %s\n", dlg->srcPath, destPath, pages);

    bool ok = false;
    WindowTab* tab = dlg->win ? dlg->win->CurrentTab() : nullptr;
    bool isPdf = tab && CouldBePDFDoc(tab);
    if (isPdf) {
        // use muconvert for PDF
        char* argv[] = {(char*)"convert", (char*)"-o", destPath, dlg->srcPath, pages};
        int argc = 5;
        fz_set_optind(0);
        ok = muconvert_main(argc, argv) == 0;
    } else {
        // use engine text extraction for other formats (DjVu, etc.)
        ok = ExtractTextViaEngine(dlg, destPath, pages);
    }

    if (ok) {
        logf("PdfExtractTextDoIt: extracted successfully\n");
        DestroyWindow(dlg->hwnd);
        OpenPathInDefaultFileManager(destPath);
    } else {
        logf("PdfExtractTextDoIt: failed to extract text, isPdf: %d\n", (int)isPdf);
        MessageBoxWarning(dlg->hwnd, _TRA("Failed to extract text."), _TRA("Extract Text"));
    }
}

static LRESULT CALLBACK PdfExtractTextDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PdfExtractTextDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        dlg = (PdfExtractTextDialog*)cs->lpCreateParams;
        dlg->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (PdfExtractTextDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_COMMAND: {
            int code = HIWORD(wp);
            HWND ctl = (HWND)lp;
            if (ctl == dlg->hwndBrowseBtn && code == BN_CLICKED) {
                PdfExtractTextOnBrowse(dlg);
                return 0;
            }
            if (ctl == dlg->hwndExtractBtn && code == BN_CLICKED) {
                PdfExtractTextDoIt(dlg);
                return 0;
            }
            if (ctl == dlg->hwndCancelBtn && code == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DPICHANGED:
            PdfToolHandleDpi(hwnd, &dlg->dpi, &dlg->hFont, &dlg->fontOwned, dlg->dpiReady, wp, lp);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (dlg->fontOwned && dlg->hFont) {
                DeleteObject(dlg->hFont);
                dlg->hFont = nullptr;
            }
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kPdfExtractTextWinClassName = L"SUMATRA_PDF_EXTRACT_TEXT";
static bool gPdfExtractTextWinClassRegistered = false;

void ShowPdfExtractTextDialog(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    logf("ShowPdfExtractTextDialog: opening for '%s'\n", tab->filePath);

    if (!gPdfExtractTextWinClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PdfExtractTextDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPdfExtractTextWinClassName;
        RegisterClassExW(&wc);
        gPdfExtractTextWinClassRegistered = true;
    }

    PdfExtractTextDialog* dlg = new PdfExtractTextDialog();
    dlg->srcPath = str::Dup(tab->filePath);
    dlg->win = win;
    dlg->dpi = DpiGet(win->hwndFrame);
    if (dlg->dpi < 72) {
        dlg->dpi = 96;
    }
    dlg->hFont = CreatePdfToolFont(dlg->dpi);
    dlg->fontOwned = dlg->hFont != nullptr;
    if (!dlg->hFont) {
        dlg->hFont = GetDefaultGuiFont();
    }

    DlgMetrics m = GetDlgMetrics(win->hwndFrame, dlg->hFont);
    int minW = DpiScale(win->hwndFrame, 500);
    int dlgW = CalcDlgWidth(win->hwndFrame, dlg->hFont, tab->filePath, minW, m.padding);
    int dlgH = CalcDlgHeight(win->hwndFrame, m, 4);

    HINSTANCE h = GetModuleHandleW(nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPdfExtractTextWinClassName, _TRW("Extract Text"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                dlgW, dlgH, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        str::Free(dlg->srcPath);
        delete dlg;
        return;
    }

    int x = m.padding;
    int y = m.padding;
    int w = dlgW - 2 * m.padding - DpiScale(hwnd, 16);

    // row 1: source path label (offset to align with text inside edit control)
    dlg->hwndPathLabel =
        CreateWindowExW(0, L"STATIC", ToWStrTemp(tab->filePath), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                        x + m.editXOff, y, w - m.editXOff, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPathLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 2: dest edit + browse button
    TempStr noExt = path::GetPathNoExtTemp(tab->filePath);
    TempStr txtPath = str::JoinTemp(noExt, ".txt");
    TempStr destPath = MakeUniqueFilePathTemp(txtPath);
    dlg->hwndDestEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(destPath), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y,
                        w - m.browseW - m.btnGap, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDestEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + w - m.browseW,
                                         y, m.browseW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndBrowseBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 3: "Pages:" label + pages edit
    WCHAR* pagesLabelText = _TRW("Pages:");
    Size labelSize = HwndMeasureText(hwnd, ToUtf8Temp(pagesLabelText), dlg->hFont);
    int labelW = labelSize.dx + DpiScale(hwnd, 4);
    dlg->hwndPagesLabel = CreateWindowExW(0, L"STATIC", pagesLabelText, WS_CHILD | WS_VISIBLE | SS_LEFT, x + m.editXOff,
                                          y + m.editXOff, labelW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPagesLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    int pageCount = win->ctrl ? win->ctrl->PageCount() : 1;
    TempStr pagesStr = str::FormatTemp("1-%d", pageCount);
    int editX = x + m.editXOff + labelW + DpiScale(hwnd, 4);
    dlg->hwndPagesEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(pagesStr), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, editX,
                        y, x + w - editX, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPagesEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 4: Extract Text + Cancel buttons (right-aligned)
    int bx = x + w - m.btnW;
    dlg->hwndCancelBtn = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx, y,
                                         m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndCancelBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    bx -= m.btnW + m.btnGap;
    dlg->hwndExtractBtn = CreateWindowExW(0, L"BUTTON", _TRW("Extract Text"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                          bx, y, m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndExtractBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    CenterDialog(hwnd, win->hwndFrame);
    if (UseDarkModeLib()) {
        DarkMode::setDarkWndSafe(hwnd);
        DarkMode::setWindowEraseBgSubclass(hwnd);
    }
    UpdateWindowCaptionTheme(hwnd);
    dlg->dpiReady = true;
    ShowWindow(hwnd, SW_SHOW);
}

// --- Compress PDF dialog ---

struct PdfCompressDialog {
    HWND hwnd = nullptr;
    HWND hwndPathLabel = nullptr;
    HWND hwndDestEdit = nullptr;
    HWND hwndBrowseBtn = nullptr;
    HWND hwndCompressBtn = nullptr;
    HWND hwndCancelBtn = nullptr;
    HFONT hFont = nullptr;
    int dpi = 96;
    bool fontOwned = false;
    bool dpiReady = false;
    char* srcPath = nullptr;
    MainWindow* win = nullptr;
};

static void PdfCompressOnBrowse(PdfCompressDialog* dlg) {
    WCHAR dstFileName[MAX_PATH + 1]{};
    GetWindowTextW(dlg->hwndDestEdit, dstFileName, MAX_PATH);

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg->hwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"pdf";

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(dlg->hwndDestEdit, dstFileName);
    }
}

static void PdfCompressDoIt(PdfCompressDialog* dlg) {
    char destPath[MAX_PATH + 1]{};
    GetWindowTextA(dlg->hwndDestEdit, destPath, MAX_PATH);
    if (str::IsEmpty(destPath)) {
        return;
    }

    logf("PdfCompressDoIt: compressing '%s' to '%s'\n", dlg->srcPath, destPath);

    // equivalent of: clean -gggg -e 100 -f -i -t -Z input output
    char* argv[] = {
        (char*)"clean", (char*)"-gggg", (char*)"-e", (char*)"100", (char*)"-f",
        (char*)"-i",    (char*)"-t",    (char*)"-Z", dlg->srcPath, destPath,
    };
    int argc = 10;

    fz_set_optind(0);
    int res = pdfclean_main(argc, argv);
    if (res == 0) {
        logf("PdfCompressDoIt: compressed successfully\n");
        MainWindow* win = dlg->win;
        TempStr path = str::DupTemp(destPath);
        DestroyWindow(dlg->hwnd);
        LoadArgs args(path, win);
        StartLoadDocument(&args);
    } else {
        logf("PdfCompressDoIt: pdfclean_main failed with %d\n", res);
        MessageBoxWarning(dlg->hwnd, _TRA("Failed to compress PDF file."), _TRA("Compress PDF"));
    }
}

static LRESULT CALLBACK PdfCompressDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PdfCompressDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        dlg = (PdfCompressDialog*)cs->lpCreateParams;
        dlg->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (PdfCompressDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_COMMAND: {
            int code = HIWORD(wp);
            HWND ctl = (HWND)lp;
            if (ctl == dlg->hwndBrowseBtn && code == BN_CLICKED) {
                PdfCompressOnBrowse(dlg);
                return 0;
            }
            if (ctl == dlg->hwndCompressBtn && code == BN_CLICKED) {
                PdfCompressDoIt(dlg);
                return 0;
            }
            if (ctl == dlg->hwndCancelBtn && code == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DPICHANGED:
            PdfToolHandleDpi(hwnd, &dlg->dpi, &dlg->hFont, &dlg->fontOwned, dlg->dpiReady, wp, lp);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (dlg->fontOwned && dlg->hFont) {
                DeleteObject(dlg->hFont);
                dlg->hFont = nullptr;
            }
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kPdfCompressWinClassName = L"SUMATRA_PDF_COMPRESS";
static bool gPdfCompressWinClassRegistered = false;

void ShowPdfCompressDialog(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    if (!CouldBePDFDoc(tab)) {
        return;
    }
    logf("ShowPdfCompressDialog: opening for '%s'\n", tab->filePath);

    if (!gPdfCompressWinClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PdfCompressDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPdfCompressWinClassName;
        RegisterClassExW(&wc);
        gPdfCompressWinClassRegistered = true;
    }

    PdfCompressDialog* dlg = new PdfCompressDialog();
    dlg->srcPath = str::Dup(tab->filePath);
    dlg->win = win;
    dlg->dpi = DpiGet(win->hwndFrame);
    if (dlg->dpi < 72) {
        dlg->dpi = 96;
    }
    dlg->hFont = CreatePdfToolFont(dlg->dpi);
    dlg->fontOwned = dlg->hFont != nullptr;
    if (!dlg->hFont) {
        dlg->hFont = GetDefaultGuiFont();
    }

    DlgMetrics m = GetDlgMetrics(win->hwndFrame, dlg->hFont);
    int minW = DpiScale(win->hwndFrame, 500);
    int dlgW = CalcDlgWidth(win->hwndFrame, dlg->hFont, tab->filePath, minW, m.padding);
    int dlgH = CalcDlgHeight(win->hwndFrame, m, 3);

    HINSTANCE h = GetModuleHandleW(nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPdfCompressWinClassName, _TRW("Compress PDF"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                dlgW, dlgH, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        str::Free(dlg->srcPath);
        delete dlg;
        return;
    }

    int x = m.padding;
    int y = m.padding;
    int w = dlgW - 2 * m.padding - DpiScale(hwnd, 16);

    // row 1: source path label (offset to align with text inside edit control)
    dlg->hwndPathLabel =
        CreateWindowExW(0, L"STATIC", ToWStrTemp(tab->filePath), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                        x + m.editXOff, y, w - m.editXOff, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPathLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 2: dest edit + browse button
    TempStr destPath = MakeUniqueFilePathTemp(tab->filePath);
    dlg->hwndDestEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(destPath), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y,
                        w - m.browseW - m.btnGap, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDestEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + w - m.browseW,
                                         y, m.browseW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndBrowseBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 3: Compress + Cancel buttons (right-aligned)
    int bx = x + w - m.btnW;
    dlg->hwndCancelBtn = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx, y,
                                         m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndCancelBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    bx -= m.btnW + m.btnGap;
    dlg->hwndCompressBtn = CreateWindowExW(0, L"BUTTON", _TRW("Compress PDF"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                           bx, y, m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndCompressBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    CenterDialog(hwnd, win->hwndFrame);
    if (UseDarkModeLib()) {
        DarkMode::setDarkWndSafe(hwnd);
        DarkMode::setWindowEraseBgSubclass(hwnd);
    }
    UpdateWindowCaptionTheme(hwnd);
    dlg->dpiReady = true;
    ShowWindow(hwnd, SW_SHOW);
}

// --- Decompress PDF dialog ---

struct PdfDecompressDialog {
    HWND hwnd = nullptr;
    HWND hwndPathLabel = nullptr;
    HWND hwndDestEdit = nullptr;
    HWND hwndBrowseBtn = nullptr;
    HWND hwndDecompressBtn = nullptr;
    HWND hwndCancelBtn = nullptr;
    HFONT hFont = nullptr;
    int dpi = 96;
    bool fontOwned = false;
    bool dpiReady = false;
    char* srcPath = nullptr;
    MainWindow* win = nullptr;
};

static void PdfDecompressOnBrowse(PdfDecompressDialog* dlg) {
    WCHAR dstFileName[MAX_PATH + 1]{};
    GetWindowTextW(dlg->hwndDestEdit, dstFileName, MAX_PATH);

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg->hwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"pdf";

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(dlg->hwndDestEdit, dstFileName);
    }
}

static void PdfDecompressDoIt(PdfDecompressDialog* dlg) {
    char destPath[MAX_PATH + 1]{};
    GetWindowTextA(dlg->hwndDestEdit, destPath, MAX_PATH);
    if (str::IsEmpty(destPath)) {
        return;
    }

    logf("PdfDecompressDoIt: decompressing '%s' to '%s'\n", dlg->srcPath, destPath);

    // equivalent of: clean -d input output
    char* argv[] = {(char*)"clean", (char*)"-d", dlg->srcPath, destPath};
    int argc = 4;

    fz_set_optind(0);
    int res = pdfclean_main(argc, argv);
    if (res == 0) {
        logf("PdfDecompressDoIt: decompressed successfully\n");
        MainWindow* win = dlg->win;
        TempStr path = str::DupTemp(destPath);
        DestroyWindow(dlg->hwnd);
        LoadArgs args(path, win);
        StartLoadDocument(&args);
    } else {
        logf("PdfDecompressDoIt: pdfclean_main failed with %d\n", res);
        MessageBoxWarning(dlg->hwnd, _TRA("Failed to decompress PDF file."), _TRA("Decompress PDF"));
    }
}

static LRESULT CALLBACK PdfDecompressDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PdfDecompressDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        dlg = (PdfDecompressDialog*)cs->lpCreateParams;
        dlg->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (PdfDecompressDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_COMMAND: {
            int code = HIWORD(wp);
            HWND ctl = (HWND)lp;
            if (ctl == dlg->hwndBrowseBtn && code == BN_CLICKED) {
                PdfDecompressOnBrowse(dlg);
                return 0;
            }
            if (ctl == dlg->hwndDecompressBtn && code == BN_CLICKED) {
                PdfDecompressDoIt(dlg);
                return 0;
            }
            if (ctl == dlg->hwndCancelBtn && code == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DPICHANGED:
            PdfToolHandleDpi(hwnd, &dlg->dpi, &dlg->hFont, &dlg->fontOwned, dlg->dpiReady, wp, lp);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (dlg->fontOwned && dlg->hFont) {
                DeleteObject(dlg->hFont);
                dlg->hFont = nullptr;
            }
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kPdfDecompressWinClassName = L"SUMATRA_PDF_DECOMPRESS";
static bool gPdfDecompressWinClassRegistered = false;

void ShowPdfDecompressDialog(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    if (!CouldBePDFDoc(tab)) {
        return;
    }
    logf("ShowPdfDecompressDialog: opening for '%s'\n", tab->filePath);

    if (!gPdfDecompressWinClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PdfDecompressDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPdfDecompressWinClassName;
        RegisterClassExW(&wc);
        gPdfDecompressWinClassRegistered = true;
    }

    PdfDecompressDialog* dlg = new PdfDecompressDialog();
    dlg->srcPath = str::Dup(tab->filePath);
    dlg->win = win;
    dlg->dpi = DpiGet(win->hwndFrame);
    if (dlg->dpi < 72) {
        dlg->dpi = 96;
    }
    dlg->hFont = CreatePdfToolFont(dlg->dpi);
    dlg->fontOwned = dlg->hFont != nullptr;
    if (!dlg->hFont) {
        dlg->hFont = GetDefaultGuiFont();
    }

    DlgMetrics m = GetDlgMetrics(win->hwndFrame, dlg->hFont);
    int minW = DpiScale(win->hwndFrame, 500);
    int dlgW = CalcDlgWidth(win->hwndFrame, dlg->hFont, tab->filePath, minW, m.padding);
    int dlgH = CalcDlgHeight(win->hwndFrame, m, 3);

    HINSTANCE h = GetModuleHandleW(nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPdfDecompressWinClassName, _TRW("Decompress PDF"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                dlgW, dlgH, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        str::Free(dlg->srcPath);
        delete dlg;
        return;
    }

    int x = m.padding;
    int y = m.padding;
    int w = dlgW - 2 * m.padding - DpiScale(hwnd, 16);

    // source path label (offset to align with text inside edit control)
    dlg->hwndPathLabel =
        CreateWindowExW(0, L"STATIC", ToWStrTemp(tab->filePath), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                        x + m.editXOff, y, w - m.editXOff, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPathLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    TempStr destPath = MakeUniqueFilePathTemp(tab->filePath);
    dlg->hwndDestEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(destPath), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y,
                        w - m.browseW - m.btnGap, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDestEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + w - m.browseW,
                                         y, m.browseW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndBrowseBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    int bx = x + w - m.btnW;
    dlg->hwndCancelBtn = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx, y,
                                         m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndCancelBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    bx -= m.btnW + m.btnGap;
    dlg->hwndDecompressBtn =
        CreateWindowExW(0, L"BUTTON", _TRW("Decompress PDF"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, bx, y, m.btnW,
                        m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDecompressBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    CenterDialog(hwnd, win->hwndFrame);
    if (UseDarkModeLib()) {
        DarkMode::setDarkWndSafe(hwnd);
        DarkMode::setWindowEraseBgSubclass(hwnd);
    }
    UpdateWindowCaptionTheme(hwnd);
    dlg->dpiReady = true;
    ShowWindow(hwnd, SW_SHOW);
}

// --- Delete Pages From PDF dialog ---

struct PdfDeletePageDialog {
    HWND hwnd = nullptr;
    HWND hwndPathLabel = nullptr;
    HWND hwndDestEdit = nullptr;
    HWND hwndBrowseBtn = nullptr;
    HWND hwndPagesLabel = nullptr;
    HWND hwndPagesEdit = nullptr;
    HWND hwndTotalLabel = nullptr;
    HWND hwndDeleteBtn = nullptr; // also used as "Extract Pages" button
    HWND hwndCancelBtn = nullptr;
    HFONT hFont = nullptr;
    int dpi = 96;
    bool fontOwned = false;
    bool dpiReady = false;
    char* srcPath = nullptr;
    bool isExtract = false;
    MainWindow* win = nullptr;
    int pageCount = 0;
};

// Parse delete page ranges like "1,3-8,13-N" where N means last page.
// Returns a sorted list of unique 1-based page numbers to delete.
// Returns false if the syntax is invalid or any page is out of range.
static bool ParseDeletePages(const char* s, int pageCount, Vec<int>& pagesToDelete) {
    if (!s || !*s) {
        return false;
    }
    StrVec parts;
    Split(&parts, s, ",", true);
    if (parts.Size() == 0) {
        return false;
    }
    for (char* part : parts) {
        str::TrimWSInPlace(part, str::TrimOpt::Both);
        if (str::IsEmpty(part)) {
            return false;
        }
        // check for range "A-B" where A/B can be a number or "N"
        char* dash = (char*)str::FindChar(part, '-');
        if (dash) {
            *dash = 0;
            char* startStr = part;
            char* endStr = dash + 1;
            str::TrimWSInPlace(startStr, str::TrimOpt::Both);
            str::TrimWSInPlace(endStr, str::TrimOpt::Both);
            if (str::IsEmpty(startStr)) {
                return false;
            }
            // "8-" means "8-N" (from page 8 to the last page)
            bool endIsEmpty = str::IsEmpty(endStr);
            int start, end;
            if (str::EqI(startStr, "N")) {
                start = pageCount;
            } else {
                start = str::Parse(startStr, "%d%$", &start) ? start : -1;
            }
            if (endIsEmpty || str::EqI(endStr, "N")) {
                end = pageCount;
            } else {
                end = str::Parse(endStr, "%d%$", &end) ? end : -1;
            }
            if (start < 1 || start > pageCount || end < 1 || end > pageCount || start > end) {
                return false;
            }
            for (int i = start; i <= end; i++) {
                pagesToDelete.Append(i);
            }
        } else {
            // single page
            int page;
            if (str::EqI(part, "N")) {
                page = pageCount;
            } else {
                page = str::Parse(part, "%d%$", &page) ? page : -1;
            }
            if (page < 1 || page > pageCount) {
                return false;
            }
            pagesToDelete.Append(page);
        }
    }
    if (pagesToDelete.Size() == 0) {
        return false;
    }
    // sort and deduplicate
    pagesToDelete.SortTyped([](const int* a, const int* b) -> int { return *a - *b; });
    int prev = -1;
    Vec<int> unique;
    for (int p : pagesToDelete) {
        if (p != prev) {
            unique.Append(p);
            prev = p;
        }
    }
    pagesToDelete = unique;
    return true;
}

// Build the page range string of pages to KEEP (complement of pagesToDelete).
static TempStr BuildKeepPagesRange(int pageCount, const Vec<int>& pagesToDelete) {
    StrBuilder s;
    int delIdx = 0;
    int rangeStart = -1;
    int rangeEnd = -1;
    for (int p = 1; p <= pageCount; p++) {
        bool shouldDelete = (delIdx < pagesToDelete.Size() && pagesToDelete[delIdx] == p);
        if (shouldDelete) {
            delIdx++;
            if (rangeStart != -1) {
                if (s.Size() > 0) {
                    s.AppendChar(',');
                }
                if (rangeStart == rangeEnd) {
                    s.AppendFmt("%d", rangeStart);
                } else {
                    s.AppendFmt("%d-%d", rangeStart, rangeEnd);
                }
                rangeStart = -1;
            }
        } else {
            if (rangeStart == -1) {
                rangeStart = p;
            }
            rangeEnd = p;
        }
    }
    if (rangeStart != -1) {
        if (s.Size() > 0) {
            s.AppendChar(',');
        }
        if (rangeStart == rangeEnd) {
            s.AppendFmt("%d", rangeStart);
        } else {
            s.AppendFmt("%d-%d", rangeStart, rangeEnd);
        }
    }
    return str::DupTemp(s.Get());
}

// Format a sorted list of page numbers as a compact range string (e.g. "1-3,5,7-10").
static TempStr FormatPageRange(const Vec<int>& pages) {
    StrBuilder s;
    int i = 0;
    int n = pages.Size();
    while (i < n) {
        int start = pages[i];
        int end = start;
        while (i + 1 < n && pages[i + 1] == end + 1) {
            end = pages[++i];
        }
        if (s.Size() > 0) {
            s.AppendChar(',');
        }
        if (start == end) {
            s.AppendFmt("%d", start);
        } else {
            s.AppendFmt("%d-%d", start, end);
        }
        i++;
    }
    return str::DupTemp(s.Get());
}

static void PdfDeletePageUpdateButton(PdfDeletePageDialog* dlg) {
    char pages[256]{};
    GetWindowTextA(dlg->hwndPagesEdit, pages, dimof(pages) - 1);
    Vec<int> parsedPages;
    bool valid = ParseDeletePages(pages, dlg->pageCount, parsedPages);
    // for delete mode, can't delete all pages
    if (valid && !dlg->isExtract && parsedPages.Size() >= dlg->pageCount) {
        valid = false;
    }
    EnableWindow(dlg->hwndDeleteBtn, valid);
}

static void PdfDeletePageOnBrowse(PdfDeletePageDialog* dlg) {
    WCHAR dstFileName[MAX_PATH + 1]{};
    GetWindowTextW(dlg->hwndDestEdit, dstFileName, MAX_PATH);

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg->hwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"pdf";

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(dlg->hwndDestEdit, dstFileName);
    }
}

static void PdfDeletePageDoIt(PdfDeletePageDialog* dlg) {
    char destPath[MAX_PATH + 1]{};
    GetWindowTextA(dlg->hwndDestEdit, destPath, MAX_PATH);
    if (str::IsEmpty(destPath)) {
        return;
    }

    char pages[256]{};
    GetWindowTextA(dlg->hwndPagesEdit, pages, dimof(pages) - 1);

    Vec<int> parsedPages;
    if (!ParseDeletePages(pages, dlg->pageCount, parsedPages)) {
        return;
    }
    if (!dlg->isExtract && parsedPages.Size() >= dlg->pageCount) {
        return;
    }

    TempStr pageRange;
    if (dlg->isExtract) {
        // for extract: pass the specified pages directly to pdfclean
        pageRange = FormatPageRange(parsedPages);
    } else {
        // for delete: pass the complement (pages to keep) to pdfclean
        pageRange = BuildKeepPagesRange(dlg->pageCount, parsedPages);
    }

    const char* op = dlg->isExtract ? "extract" : "delete";
    logf("PdfDeletePageDoIt: %s pages '%s' from '%s' to '%s', range for pdfclean: %s\n", op, pages, dlg->srcPath,
         destPath, pageRange);

    // equivalent of: clean input.pdf output.pdf <page-range>
    char* argv[] = {(char*)"clean", dlg->srcPath, destPath, pageRange};
    int argc = 4;

    fz_set_optind(0);
    int res = pdfclean_main(argc, argv);
    if (res == 0) {
        logf("PdfDeletePageDoIt: %s pages successfully\n", op);
        MainWindow* win = dlg->win;
        TempStr path = str::DupTemp(destPath);
        DestroyWindow(dlg->hwnd);
        LoadArgs args(path, win);
        StartLoadDocument(&args);
    } else {
        logf("PdfDeletePageDoIt: pdfclean_main failed with %d for %s\n", res, op);
        const char* msg =
            dlg->isExtract ? "Failed to extract pages from PDF file." : "Failed to delete pages from PDF file.";
        const char* title = dlg->isExtract ? _TRA("Extract Pages From PDF") : _TRA("Delete Pages From PDF");
        MessageBoxWarning(dlg->hwnd, msg, title);
    }
}

static LRESULT CALLBACK PdfDeletePageDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PdfDeletePageDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        dlg = (PdfDeletePageDialog*)cs->lpCreateParams;
        dlg->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (PdfDeletePageDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_COMMAND: {
            int code = HIWORD(wp);
            HWND ctl = (HWND)lp;
            if (ctl == dlg->hwndBrowseBtn && code == BN_CLICKED) {
                PdfDeletePageOnBrowse(dlg);
                return 0;
            }
            if (ctl == dlg->hwndDeleteBtn && code == BN_CLICKED) {
                PdfDeletePageDoIt(dlg);
                return 0;
            }
            if (ctl == dlg->hwndCancelBtn && code == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (ctl == dlg->hwndPagesEdit && code == EN_CHANGE) {
                PdfDeletePageUpdateButton(dlg);
                return 0;
            }
            break;
        }
        case WM_DPICHANGED:
            PdfToolHandleDpi(hwnd, &dlg->dpi, &dlg->hFont, &dlg->fontOwned, dlg->dpiReady, wp, lp);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (dlg->fontOwned && dlg->hFont) {
                DeleteObject(dlg->hFont);
                dlg->hFont = nullptr;
            }
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kPdfDeletePageWinClassName = L"SUMATRA_PDF_DELETE_PAGE";
static bool gPdfDeletePageWinClassRegistered = false;

static void ShowPdfPageRangeDialog(MainWindow* win, bool isExtract) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    if (!CouldBePDFDoc(tab)) {
        return;
    }

    int pageCount = win->ctrl ? win->ctrl->PageCount() : 0;
    if (pageCount < 2) {
        return;
    }
    logf("ShowPdfPageRangeDialog: opening %s dialog for '%s', %d pages\n", isExtract ? "extract" : "delete",
         tab->filePath, pageCount);

    if (!gPdfDeletePageWinClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PdfDeletePageDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPdfDeletePageWinClassName;
        RegisterClassExW(&wc);
        gPdfDeletePageWinClassRegistered = true;
    }

    PdfDeletePageDialog* dlg = new PdfDeletePageDialog();
    dlg->srcPath = str::Dup(tab->filePath);
    dlg->win = win;
    dlg->dpi = DpiGet(win->hwndFrame);
    if (dlg->dpi < 72) {
        dlg->dpi = 96;
    }
    dlg->hFont = CreatePdfToolFont(dlg->dpi);
    dlg->fontOwned = dlg->hFont != nullptr;
    if (!dlg->hFont) {
        dlg->hFont = GetDefaultGuiFont();
    }
    dlg->pageCount = pageCount;
    dlg->isExtract = isExtract;

    DlgMetrics m = GetDlgMetrics(win->hwndFrame, dlg->hFont);
    int minW = DpiScale(win->hwndFrame, 500);
    int dlgW = CalcDlgWidth(win->hwndFrame, dlg->hFont, tab->filePath, minW, m.padding);
    int dlgH = CalcDlgHeight(win->hwndFrame, m, 4);

    HINSTANCE h = GetModuleHandleW(nullptr);
    WCHAR* dlgTitle = isExtract ? _TRW("Extract Pages From PDF") : _TRW("Delete Pages From PDF");
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPdfDeletePageWinClassName, dlgTitle,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                dlgW, dlgH, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        str::Free(dlg->srcPath);
        delete dlg;
        return;
    }

    int x = m.padding;
    int y = m.padding;
    int w = dlgW - 2 * m.padding - DpiScale(hwnd, 16);

    // row 1: source path label (offset to align with text inside edit control)
    dlg->hwndPathLabel =
        CreateWindowExW(0, L"STATIC", ToWStrTemp(tab->filePath), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                        x + m.editXOff, y, w - m.editXOff, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPathLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 2: dest edit + browse button
    TempStr destPath = MakeUniqueFilePathTemp(tab->filePath);
    dlg->hwndDestEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(destPath), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y,
                        w - m.browseW - m.btnGap, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDestEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + w - m.browseW,
                                         y, m.browseW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndBrowseBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 3: pages label + pages edit + total pages label
    // offset static labels to align with text inside edit control (same as input path label)
    int labelX = x + m.editXOff;
    WCHAR* pagesLabelText = isExtract ? _TRW("Pages To Extract:") : _TRW("Pages To Delete:");
    Size labelSize = HwndMeasureText(hwnd, ToUtf8Temp(pagesLabelText), dlg->hFont);
    int labelW = labelSize.dx + DpiScale(hwnd, 4);
    dlg->hwndPagesLabel = CreateWindowExW(0, L"STATIC", pagesLabelText, WS_CHILD | WS_VISIBLE | SS_LEFT, labelX,
                                          y + m.editXOff, labelW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPagesLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    TempStr totalStr = str::FormatTemp("of %d", pageCount);
    Size totalSize = HwndMeasureText(hwnd, totalStr, dlg->hFont);
    int totalW = totalSize.dx + DpiScale(hwnd, 4);
    int editX = labelX + labelW + DpiScale(hwnd, 8);
    int editW = x + w - editX - totalW - m.btnGap;
    int currentPage = win->ctrl ? win->ctrl->CurrentPageNo() : 1;
    TempStr pagesStr = str::FormatTemp("%d", currentPage);
    dlg->hwndPagesEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(pagesStr), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, editX,
                        y, editW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPagesEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndTotalLabel =
        CreateWindowExW(0, L"STATIC", ToWStrTemp(totalStr), WS_CHILD | WS_VISIBLE | SS_LEFT, editX + editW + m.btnGap,
                        y + m.editXOff, totalW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndTotalLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 4: syntax hint (left) + action + Cancel buttons (right)

    // syntax hint label, x-aligned with pages label and baseline-aligned with button text
    HWND hwndSyntax = CreateWindowExW(0, L"STATIC", L"Syntax: 2,5-7,13-", WS_CHILD | WS_VISIBLE | SS_LEFT, labelX,
                                      y + m.editXOff, w / 2, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(hwndSyntax, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    int bx = x + w - m.btnW;
    dlg->hwndCancelBtn = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx, y,
                                         m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndCancelBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    bx -= m.btnW + m.btnGap;
    WCHAR* actionBtnText = isExtract ? _TRW("Extract Pages") : _TRW("Delete Pages");
    dlg->hwndDeleteBtn = CreateWindowExW(0, L"BUTTON", actionBtnText, WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, bx, y,
                                         m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDeleteBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    // validate initial state
    PdfDeletePageUpdateButton(dlg);

    CenterDialog(hwnd, win->hwndFrame);
    if (UseDarkModeLib()) {
        DarkMode::setDarkWndSafe(hwnd);
        DarkMode::setWindowEraseBgSubclass(hwnd);
    }
    UpdateWindowCaptionTheme(hwnd);
    dlg->dpiReady = true;
    ShowWindow(hwnd, SW_SHOW);
}

void ShowPdfDeletePageDialog(MainWindow* win) {
    ShowPdfPageRangeDialog(win, false);
}

void ShowPdfExtractPagesDialog(MainWindow* win) {
    ShowPdfPageRangeDialog(win, true);
}

// --- Rotate PDF pages dialog ---

// child control ids; Enter/Esc reach the window proc as WM_COMMAND
// IDOK/IDCANCEL because the main loop runs IsDialogMessage on the
// registered modeless dialog (see WM_ACTIVATE handling below)
enum PdfRotatePagesCtl {
    idRotateApplyToCurrent = 100,
    idRotateApplyToAll = 101,
    idRotateApplyToSpecified = 102,
    idRotatePagesEdit = 103,
    idRotateAny = 110,
    idRotateLeft90 = 111,
    idRotateRight90 = 112,
    idRotate180 = 113,
    idRotateAngleEdit = 114,
    idRotateFlipH = 115,
    idRotateFlipV = 116,
    idRotateAutoCrop = 117,
    idRotateAngleMinus = 118,
    idRotateAnglePlus = 119,
    idRotateZoomIn = 120,
    idRotateZoomOut = 121,
    idRotateZoomFit = 122,
    idRotateMinimize = 123,
    idRotateMaximize = 124,
    idRotateClose = 125,
    idRotateShowGrid = 126,
    idRotateGridDensity = 127,
    idRotateApply = IDOK,
    idRotateCancel = IDCANCEL,
};

struct PdfRotatePagesDialog {
    HWND hwnd = nullptr;
    HWND hwndApplyToLabel = nullptr;
    HWND hwndCurrentPage = nullptr;
    HWND hwndAllPages = nullptr;
    HWND hwndSpecified = nullptr;
    HWND hwndPagesEdit = nullptr;
    HWND hwndPagesHint = nullptr;
    HWND hwndAny = nullptr;
    HWND hwndSlider = nullptr;
    HWND hwndAngleEdit = nullptr;
    HWND hwndAngleMinus = nullptr;
    HWND hwndAnglePlus = nullptr;
    HWND hwndZoomIn = nullptr;
    HWND hwndZoomOut = nullptr;
    HWND hwndZoomFit = nullptr;
    HWND hwndMinimize = nullptr;
    HWND hwndMaximize = nullptr;
    HWND hwndClose = nullptr;
    HWND hwndSidebarScroll = nullptr;
    HWND hwndGridTitle = nullptr;
    HWND hwndShowGrid = nullptr;
    HWND hwndGridDensity = nullptr;
    HWND hwndRotationTitle = nullptr;
    HWND hwndFlipTitle = nullptr;
    HWND hwndAngleHint = nullptr;
    HWND hwndPreviewInfo = nullptr;
    Tooltip* tooltip = nullptr;
    HBITMAP iconCache[6][8]{};
    int iconCacheDpi = 0;
    int iconCacheEpoch = -1;
    HWND hwndDegLabel = nullptr;
    HWND hwndLeft90 = nullptr;
    HWND hwndRight90 = nullptr;
    HWND hwnd180 = nullptr;
    HWND hwndFlipH = nullptr;
    HWND hwndFlipV = nullptr;
    HWND hwndAutoCrop = nullptr;
    HWND hwndRotateBtn = nullptr;
    HWND hwndCancelBtn = nullptr;
    HFONT hFont = nullptr;
    HFONT hFontBold = nullptr;
    // theme-adaptive painting (all four themes), mirrors AiTocDialog
    HBRUSH bgBrush = nullptr;   // ThemeWindowBackgroundColor
    HBRUSH ctrlBrush = nullptr; // ThemeWindowControlBackgroundColor
    MainWindow* win = nullptr;
    int pageCount = 0;
    int currentPageNo = 1;
    PdfPageAdjustment adjustment;
    bool angleValid = true;
    bool showGrid = true;
    int gridDensity = 1;
    WindowTab* sourceTab = nullptr;
    EngineBase* sourceEngine = nullptr;
    HWND holdButton = nullptr;
    int holdDirection = 0;
    ULONGLONG holdStart = 0;
    double holdBase = 0;
    int separatorY[3]{};
    bool syncing = false;
    RECT previewRc{};
    Gdiplus::Bitmap* gdiPreview = nullptr;
    int previewRenderSide = 0;
    Size minWindowSize;
    RECT sidebarRc{};
    int sidebarPos = 0;
    int sidebarMax = 0;
    int wheelRemainder = 0;
    float previewZoom = 1.f;
    float panX = 0;
    float panY = 0;
    bool dragging = false;
    POINT dragStart{};
    DoubleBuffer* paintBuffer = nullptr;
    Size paintBufferSize;
};

// Same crop as InscribedPageSize in EngineMupdf.cpp. False on a multiple of 90°.
static bool PdfRotateInscribed(float w, float h, float deg, float* wr, float* hr) {
    if (!wr || !hr || w < 1.f || h < 1.f) {
        return false;
    }
    float a = fabsf(fmodf(deg, 180.f));
    if (a > 90.f) {
        a = 180.f - a;
    }
    if (a < 0.05f || a > 89.95f) {
        return false;
    }
    float rad = a * 0.0174532925f;
    float sinA = fabsf(sinf(rad));
    float cosA = fabsf(cosf(rad));
    if (sinA < 1e-4f || cosA < 1e-4f) {
        return false;
    }
    bool widthIsLonger = w >= h;
    float sideLong = widthIsLonger ? w : h;
    float sideShort = widthIsLonger ? h : w;
    float outW = 0;
    float outH = 0;
    if (sideShort <= 2.f * sinA * cosA * sideLong || fabsf(sinA - cosA) < 1e-4f) {
        float x = 0.5f * sideShort;
        if (widthIsLonger) {
            outW = x / sinA;
            outH = x / cosA;
        } else {
            outW = x / cosA;
            outH = x / sinA;
        }
    } else {
        float cos2 = cosA * cosA - sinA * sinA;
        if (fabsf(cos2) < 1e-4f) {
            return false;
        }
        outW = (w * cosA - h * sinA) / cos2;
        outH = (h * cosA - w * sinA) / cos2;
    }
    if (outW < 1.f || outH < 1.f) {
        return false;
    }
    *wr = outW;
    *hr = outH;
    return true;
}

static float PdfRotateReadAngle(PdfRotatePagesDialog* dlg) {
    return dlg ? (float)dlg->adjustment.TotalAngle() : 0;
}

static bool PdfRotateSourceIsCurrent(PdfRotatePagesDialog* dlg) {
    return dlg && IsMainWindowValid(dlg->win) && dlg->win->CurrentTab() == dlg->sourceTab &&
           dlg->sourceTab->GetEngine() == dlg->sourceEngine;
}

static bool PdfRotateFlipH(PdfRotatePagesDialog* dlg) {
    return dlg && SendMessageW(dlg->hwndFlipH, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static bool PdfRotateFlipV(PdfRotatePagesDialog* dlg) {
    return dlg && SendMessageW(dlg->hwndFlipV, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static bool PdfRotateAutoCrop(PdfRotatePagesDialog* dlg) {
    return dlg && SendMessageW(dlg->hwndAutoCrop, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void PdfRotateInvalidatePreview(PdfRotatePagesDialog* dlg) {
    if (!dlg || !dlg->hwnd) {
        return;
    }
    if (dlg->previewRc.right > dlg->previewRc.left) {
        InvalidateRect(dlg->hwnd, &dlg->previewRc, FALSE);
    } else {
        InvalidateRect(dlg->hwnd, nullptr, FALSE);
    }
}

static void PdfRotateSetAngle(PdfRotatePagesDialog* dlg, double deg, bool updateEdit, bool updateSlider) {
    if (!dlg) {
        return;
    }
    dlg->adjustment.SetDeskew(deg);
    deg = dlg->adjustment.deskew;
    dlg->angleValid = true;
    dlg->syncing = true;
    if (updateSlider && dlg->hwndSlider) {
        int ticks = limitValue((int)round(deg * 100.0), -500, 500);
        SendMessageW(dlg->hwndSlider, TBM_SETPOS, TRUE, ticks);
    }
    if (updateEdit && dlg->hwndAngleEdit) {
        TempStr text = str::FormatTemp("%.2f", deg);
        SetWindowTextA(dlg->hwndAngleEdit, text);
    }
    dlg->syncing = false;
    HwndSetText(dlg->hwndAngleHint, _TRA("-5°                 0°                 +5°"));
    SendMessageW(dlg->hwndAny, BM_SETCHECK, dlg->adjustment.direction == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    PdfRotateInvalidatePreview(dlg);
}

static void PdfRotateLoadPreview(PdfRotatePagesDialog* dlg) {
    if (!dlg) {
        return;
    }
    int renderSide = std::max(
        900, (int)std::max(dlg->previewRc.right - dlg->previewRc.left, dlg->previewRc.bottom - dlg->previewRc.top));
    renderSide = std::min((int)(renderSide * dlg->previewZoom), 4096);
    if (dlg->gdiPreview && dlg->previewRenderSide >= renderSide) {
        return;
    }
    if (!PdfRotateSourceIsCurrent(dlg)) return;
    WindowTab* tab = dlg->sourceTab;
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    RectF mb = engine->PageMediabox(dlg->currentPageNo);
    float side = mb.dx > mb.dy ? mb.dx : mb.dy;
    if (side < 1.f) {
        side = 1.f;
    }
    float zoom = (float)renderSide / side;
    RenderPageArgs args(dlg->currentPageNo, zoom, 0, nullptr, RenderTarget::View);
    RenderedBitmap* rendered = engine->RenderPage(args);
    if (!rendered || !rendered->IsValid()) {
        delete rendered;
        return;
    }
    Size sz = rendered->GetSize();
    if (sz.dx < 2 || sz.dy < 2) {
        delete rendered;
        return;
    }
    auto* bmp = new Gdiplus::Bitmap(sz.dx, sz.dy, PixelFormat24bppRGB);
    if (bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        delete rendered;
        return;
    }
    Gdiplus::Graphics gg(bmp);
    HDC hdc = gg.GetHDC();
    bool blitted = false;
    if (hdc) {
        blitted = BlitHBITMAP(rendered->GetBitmap(), hdc, Rect(0, 0, sz.dx, sz.dy));
        gg.ReleaseHDC(hdc);
    }
    delete rendered;
    if (!blitted) {
        delete bmp;
        return;
    }
    delete dlg->gdiPreview;
    dlg->gdiPreview = bmp;
    dlg->previewRenderSide = renderSide;
}

static void PdfRotatePaintPreview(PdfRotatePagesDialog* dlg, HDC hdc) {
    RECT rc = dlg->previewRc;
    int rw = rc.right - rc.left;
    int rh = rc.bottom - rc.top;
    if (rw < 4 || rh < 4) {
        return;
    }
    Gdiplus::Graphics g(hdc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.SetClip(Gdiplus::Rect(rc.left, rc.top, rw, rh));
    COLORREF workspace = ThemeUsesDarkChrome() ? AccentColor(ThemeWindowBackgroundColor(), -12)
                                               : AccentColor(ThemeWindowControlBackgroundColor(), -28);
    Gdiplus::SolidBrush bg(Gdiplus::Color(255, GetRValue(workspace), GetGValue(workspace), GetBValue(workspace)));
    g.FillRectangle(&bg, rc.left, rc.top, rw, rh);
    if (dlg->gdiPreview) {
        float bw = (float)dlg->gdiPreview->GetWidth();
        float bh = (float)dlg->gdiPreview->GetHeight();
        float ang = PdfRotateReadAngle(dlg);
        float cropW = 0;
        float cropH = 0;
        if (PdfRotateAutoCrop(dlg)) {
            PdfRotateInscribed(bw, bh, ang, &cropW, &cropH);
        }
        int margin = DpiScale(dlg->hwnd, 20);
        if (margin < 6) {
            margin = 6;
        }
        float availW = (float)(rw - 2 * margin);
        float availH = (float)(rh - 2 * margin);
        // Keep the page scale stable while nudging the angle. Fit is based on
        // the source page, and zoom/pan are independent of rotation and crop.
        float scale = availW / bw;
        float scaleH = availH / bh;
        if (scaleH < scale) {
            scale = scaleH;
        }
        if (scale < 0.01f) {
            scale = 0.01f;
        }
        scale *= dlg->previewZoom;
        float dispW = bw * scale;
        float dispH = bh * scale;
        float cx = ((float)rc.left + (float)rc.right) * 0.5f + dlg->panX;
        float cy = ((float)rc.top + (float)rc.bottom) * 0.5f + dlg->panY;
        if (PdfRotateAutoCrop(dlg) && cropW > 1.f) {
            Gdiplus::RectF clip(cx - cropW * scale * 0.5f, cy - cropH * scale * 0.5f, cropW * scale, cropH * scale);
            g.SetClip(clip, Gdiplus::CombineModeIntersect);
        }
        float sx = PdfRotateFlipH(dlg) ? -1.f : 1.f;
        float sy = PdfRotateFlipV(dlg) ? -1.f : 1.f;
        // Last call is applied first: center the bitmap, flip, turn clockwise, then place it.
        g.TranslateTransform(cx, cy);
        g.RotateTransform(ang);
        g.ScaleTransform(sx, sy);
        g.TranslateTransform(-dispW * 0.5f, -dispH * 0.5f);
        g.DrawImage(dlg->gdiPreview, 0.f, 0.f, dispW, dispH);
        g.ResetTransform();
        g.ResetClip();
        g.SetClip(Gdiplus::Rect(rc.left, rc.top, rw, rh));
    }
    if (!dlg->showGrid) return;
    // A dark underlay and lighter theme-colored hairline remain visible both
    // over white paper and over the dark workspace. Grid coordinates never
    // inherit the page transform or the user's pan offset.
    COLORREF gridInk = ThemeSelectionFrameColor();
    Gdiplus::Pen grid(Gdiplus::Color(95, GetRValue(gridInk), GetGValue(gridInk), GetBValue(gridInk)), 1.f);
    Gdiplus::Pen majorGrid(Gdiplus::Color(155, GetRValue(gridInk), GetGValue(gridInk), GetBValue(gridInk)), 1.f);
    Gdiplus::Pen underlay(Gdiplus::Color(45, 0, 0, 0), 1.f);
    const int spacing[] = {32, 20, 12};
    int step = DpiScale(dlg->hwnd, spacing[dlg->gridDensity]);
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    for (int x = rc.left; x < rc.right; x += step) {
        g.DrawLine(&underlay, (float)x + 1, (float)rc.top, (float)x + 1, (float)rc.bottom);
        g.DrawLine((x - rc.left) / step % 5 == 0 ? &majorGrid : &grid, (float)x, (float)rc.top, (float)x,
                   (float)rc.bottom);
    }
    for (int y = rc.top; y < rc.bottom; y += step) {
        g.DrawLine(&underlay, (float)rc.left, (float)y + 1, (float)rc.right, (float)y + 1);
        g.DrawLine((y - rc.top) / step % 5 == 0 ? &majorGrid : &grid, (float)rc.left, (float)y, (float)rc.right,
                   (float)y);
    }
}

// the pages edit is editable only for "specified pages" scope; Apply stays
// off when the angle is 0 and nothing is flipped
static void PdfRotatePagesUpdateUi(PdfRotatePagesDialog* dlg) {
    bool specified = SendMessageW(dlg->hwndSpecified, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(dlg->hwndPagesEdit, specified);
    bool valid = true;
    if (specified) {
        char pages[256]{};
        GetWindowTextA(dlg->hwndPagesEdit, pages, dimof(pages) - 1);
        Vec<int> parsedPages;
        valid = ParseDeletePages(pages, dlg->pageCount, parsedPages);
    }
    float ang = PdfRotateReadAngle(dlg);
    bool trivial = fabsf(ang) < 0.00001f && !PdfRotateFlipH(dlg) && !PdfRotateFlipV(dlg);
    EnableWindow(dlg->hwndRotateBtn, valid && dlg->angleValid && !trivial && PdfRotateSourceIsCurrent(dlg));
}

static void PdfRotatePagesDoIt(PdfRotatePagesDialog* dlg) {
    if (!dlg->angleValid || !PdfRotateSourceIsCurrent(dlg)) {
        MessageBoxWarning(dlg->hwnd, _TRA("Check the angle and reopen this dialog if the document has changed."),
                          _TRA("Manually Adjust Pages"));
        return;
    }
    Vec<int> pageNos;
    if (SendMessageW(dlg->hwndSpecified, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        char pages[256]{};
        GetWindowTextA(dlg->hwndPagesEdit, pages, dimof(pages) - 1);
        if (!ParseDeletePages(pages, dlg->pageCount, pageNos)) {
            MessageBoxWarning(dlg->hwnd, _TRA("Invalid page range."), _TRA("Manually Adjust Pages"));
            return;
        }
    } else if (SendMessageW(dlg->hwndAllPages, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        for (int i = 1; i <= dlg->pageCount; i++) {
            pageNos.Append(i);
        }
    } else {
        pageNos.Append(dlg->currentPageNo);
    }

    float ang = PdfRotateReadAngle(dlg);
    bool flipH = PdfRotateFlipH(dlg);
    bool flipV = PdfRotateFlipV(dlg);
    bool autoCrop = PdfRotateAutoCrop(dlg);
    if (fabsf(ang) < 0.00001f && !flipH && !flipV) {
        MessageBoxWarning(dlg->hwnd, _TRA("Nothing to change."), _TRA("Manually Adjust Pages"));
        return;
    }

    MainWindow* win = dlg->win;
    WindowTab* tab = win->CurrentTab();
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }

    int changed = 0;
    for (int pageNo : pageNos) {
        if (EngineMupdfAdjustPageView(engine, pageNo, ang, flipH, flipV, autoCrop)) {
            changed++;
        }
    }
    if (changed == 0) {
        MessageBoxWarning(dlg->hwnd, _TRA("Nothing to change."), _TRA("Manually Adjust Pages"));
        return;
    }

    const char* path = engine->FilePath();
    EngineMupdfSetPdfTocModified(engine, true);
    tab->ignoreNextAutoReload = true;
    char* tmp = nullptr;
    bool ok = EngineMupdfSaveUpdated(engine, nullptr, {}, &tmp);
    if (!ok) {
        tab->ignoreNextAutoReload = false;
        MessageBoxWarning(dlg->hwnd, _TRA("Failed to save the adjusted pages."), _TRA("Manually Adjust Pages"));
        return;
    }

    DestroyWindow(dlg->hwnd);
    if (tmp) {
        bool replaced = SwitchCurrentTabToSavedFile(win, path, tmp);
        str::Free(tmp);
        if (!replaced || !IsMainWindowValid(win) || !win->IsDocLoaded()) {
            return;
        }
    } else {
        ReloadDocument(win, false);
    }
    const char* savedPath = (win->ctrl) ? win->ctrl->GetFilePath() : nullptr;
    if (savedPath) {
        TempStr notifMsg = str::FormatTemp(_TRA("Adjusted %d page(s) and saved to '%s'"), changed, savedPath);
        ShowTemporaryNotification(win->hwndCanvas, notifMsg, kNotif5SecsTimeOut);
    }
}

// 96-DPI logical pixels -> this window's DPI. Do not call DpiScale(hwnd): during
// WM_DPICHANGED, GetDpiForWindow / the monitor under the window can still be the old one.
static int PdfRotatePx(int logicalPx, int dpi) {
    if (dpi <= 0) {
        dpi = 96;
    }
    return MulDiv(logicalPx, dpi, 96);
}

static void PdfRotatePagesMoveChild(HWND hwnd, int x, int y, int dx, int dy) {
    if (!hwnd) {
        return;
    }
    SetWindowPos(hwnd, nullptr, x, y, dx, dy, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Client size in, outer size out. Caption + frame change with DPI; a fixed
// +32px fudge is shorter than the real non-client area on many monitors.
static Size PdfRotatePagesOuterSize(HWND hwnd, int dpi, int clientDx, int clientDy) {
    RECT rc = {0, 0, clientDx, clientDy};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD exStyle = WS_EX_DLGMODALFRAME;
    if (hwnd) {
        style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
        exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_DLGMODALFRAME;
    }
    // The caption is drawn inside the client area; retain only resize borders.
    style &= ~WS_CAPTION;

    using SigAdjustWindowRectExForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static SigAdjustWindowRectExForDpi pfnAdjust = nullptr;
    static bool triedAdjust = false;
    if (!triedAdjust) {
        triedAdjust = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            pfnAdjust = (SigAdjustWindowRectExForDpi)GetProcAddress(user32, "AdjustWindowRectExForDpi");
        }
    }
    BOOL ok = FALSE;
    if (pfnAdjust && dpi > 0) {
        ok = pfnAdjust(&rc, style, FALSE, exStyle, (UINT)dpi);
    }
    if (!ok) {
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    }
    return {rc.right - rc.left, rc.bottom - rc.top};
}

// One layout at every window size: preview on the left, scrollable controls on the right.
static Size PdfRotatePagesLayout(PdfRotatePagesDialog* dlg, int dpi, bool fitClient = false) {
    if (!dlg || !dlg->hwnd) return {};
    HWND hwnd = dlg->hwnd;
    int pad = PdfRotatePx(16, dpi);
    int gap = PdfRotatePx(8, dpi);
    int titleH = PdfRotatePx(32, dpi);
    int sideW = PdfRotatePx(350, dpi);
    int clientW = PdfRotatePx(1040, dpi);
    int clientH = PdfRotatePx(680, dpi);
    if (fitClient) {
        Rect client = ClientRect(hwnd);
        clientW = client.dx;
        clientH = client.dy;
    }
    int rowH = std::max(PdfRotatePx(28, dpi), HwndMeasureText(hwnd, "Xg", dlg->hFont).dy + gap);
    int buttonW = PdfRotatePx(46, dpi);
    PdfRotatePagesMoveChild(dlg->hwndMinimize, clientW - buttonW * 3, 0, buttonW, titleH);
    PdfRotatePagesMoveChild(dlg->hwndMaximize, clientW - buttonW * 2, 0, buttonW, titleH);
    PdfRotatePagesMoveChild(dlg->hwndClose, clientW - buttonW, 0, buttonW, titleH);
    int x = clientW - sideW - pad;
    int top = titleH + pad;
    int footer = clientH - pad - rowH;
    dlg->previewRc = {pad, top, x - pad, clientH - pad};
    int tool = PdfRotatePx(32, dpi);
    PdfRotatePagesMoveChild(dlg->hwndPreviewInfo, pad + gap * 4 + tool * 3, top + gap,
                            std::max(1, x - pad * 2 - tool * 3 - gap * 5), tool);
    PdfRotatePagesMoveChild(dlg->hwndZoomIn, pad + gap, top + gap, tool, tool);
    PdfRotatePagesMoveChild(dlg->hwndZoomOut, pad + gap * 2 + tool, top + gap, tool, tool);
    PdfRotatePagesMoveChild(dlg->hwndZoomFit, pad + gap * 3 + tool * 2, top + gap, tool, tool);
    // Deskew stays fixed above the scrollable auxiliary settings.
    int fixedY = top;
    PdfRotatePagesMoveChild(dlg->hwndAny, x, fixedY, sideW, rowH);
    fixedY += rowH + gap;
    int editW = PdfRotatePx(90, dpi);
    int stepGap = PdfRotatePx(4, dpi);
    int degreeW = PdfRotatePx(10, dpi);
    int stepW = rowH * 2 + editW + degreeW + stepGap * 3;
    int stepX = x + (sideW - stepW) / 2;
    PdfRotatePagesMoveChild(dlg->hwndAngleMinus, stepX, fixedY, rowH, rowH);
    PdfRotatePagesMoveChild(dlg->hwndAngleEdit, stepX + rowH + stepGap, fixedY, editW, rowH);
    PdfRotatePagesMoveChild(dlg->hwndDegLabel, stepX + rowH + editW + stepGap * 2, fixedY, degreeW, rowH);
    PdfRotatePagesMoveChild(dlg->hwndAnglePlus, stepX + stepW - rowH, fixedY, rowH, rowH);
    fixedY += rowH + gap;
    PdfRotatePagesMoveChild(dlg->hwndSlider, x, fixedY, sideW, rowH);
    fixedY += rowH;
    int hintH = std::max(PdfRotatePx(18, dpi), HwndMeasureText(hwnd, "Xg", dlg->hFont).dy);
    PdfRotatePagesMoveChild(dlg->hwndAngleHint, x, fixedY, sideW, hintH);
    top = fixedY + hintH + gap * 2;
    int contentH = 13 * (rowH + gap) + gap * 3;
    int viewportH = std::max(1, footer - gap - top);
    dlg->sidebarRc = {x, top, x + sideW, top + viewportH};
    dlg->sidebarMax = std::max(0, contentH - viewportH);
    dlg->sidebarPos = limitValue(dlg->sidebarPos, 0, dlg->sidebarMax);
    int barW = PdfRotatePx(16, dpi);
    int w = sideW - (dlg->sidebarMax ? barW + gap : 0);
    int y = top - dlg->sidebarPos;
    auto move = [&](HWND child, int px, int py, int width, int height) {
        PdfRotatePagesMoveChild(child, px, py, width, child == dlg->hwndGridDensity ? PdfRotatePx(180, dpi) : height);
        Rect r(px, py, width, height);
        Rect clip = r.Intersect(Rect(x, top, w, viewportH));
        HRGN region = clip.IsEmpty()
                          ? CreateRectRgn(0, 0, 0, 0)
                          : CreateRectRgn(clip.x - r.x, clip.y - r.y, clip.Right() - r.x, clip.Bottom() - r.y);
        SetWindowRgn(child, region, TRUE);
    };
    move(dlg->hwndGridTitle, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndShowGrid, x, y, w / 2, rowH);
    move(dlg->hwndGridDensity, x + w / 2, y, w / 2, rowH);
    y += rowH + gap * 2;
    dlg->separatorY[0] = y - gap;
    move(dlg->hwndRotationTitle, x, y, w, rowH);
    y += rowH + gap;
    int third = w / 3;
    move(dlg->hwndLeft90, x, y, third - gap / 2, rowH);
    move(dlg->hwndRight90, x + third, y, third - gap / 2, rowH);
    move(dlg->hwnd180, x + third * 2, y, w - third * 2, rowH);
    y += rowH + gap * 2;
    dlg->separatorY[1] = y - gap;
    move(dlg->hwndFlipTitle, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndFlipH, x, y, w / 2, rowH);
    move(dlg->hwndFlipV, x + w / 2, y, w - w / 2, rowH);
    y += rowH + gap;
    move(dlg->hwndAutoCrop, x, y, w, rowH);
    y += rowH + gap * 2;
    dlg->separatorY[2] = y - gap;
    move(dlg->hwndApplyToLabel, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndCurrentPage, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndAllPages, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndSpecified, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndPagesEdit, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndPagesHint, x, y, w, rowH);
    int applyW = std::max(PdfRotatePx(82, dpi), ButtonGetIdealSize(dlg->hwndRotateBtn).dx);
    int cancelW = std::max(PdfRotatePx(82, dpi), ButtonGetIdealSize(dlg->hwndCancelBtn).dx);
    PdfRotatePagesMoveChild(dlg->hwndRotateBtn, x + sideW - applyW, footer, applyW, rowH);
    PdfRotatePagesMoveChild(dlg->hwndCancelBtn, x + sideW - applyW - gap - cancelW, footer, cancelW, rowH);
    PdfRotatePagesMoveChild(dlg->hwndSidebarScroll, clientW - pad - barW, top, barW, viewportH);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
    si.nMax = contentH - 1;
    si.nPage = viewportH;
    si.nPos = dlg->sidebarPos;
    SetScrollInfo(dlg->hwndSidebarScroll, SB_CTL, &si, TRUE);
    ShowWindow(dlg->hwndSidebarScroll, dlg->sidebarMax ? SW_SHOWNA : SW_HIDE);
    return PdfRotatePagesOuterSize(hwnd, dpi, clientW, clientH);
}

// theme-adaptive control colors: the brush returned here fills the control
// background, so statics/radios sit on the window background while the pages
// edit gets the recessed control background. darkmodelib subclasses (dark
// themes) intercept these messages first and fall through when disabled,
// so this is the single source of truth for the light themes.
static LRESULT PdfRotatePagesColorControl(PdfRotatePagesDialog* dlg, HDC dc, HWND control) {
    bool edit = control == dlg->hwndPagesEdit || control == dlg->hwndAngleEdit;
    COLORREF text = ThemeWindowTextColor();
    if (!IsWindowEnabled(control) || control == dlg->hwndPagesHint) {
        text = ThemeWindowTextDisabledColor();
    }
    SetTextColor(dc, text);
    if (control == dlg->hwndPreviewInfo) {
        COLORREF workspace = ThemeUsesDarkChrome() ? AccentColor(ThemeWindowBackgroundColor(), -12)
                                                   : AccentColor(ThemeWindowControlBackgroundColor(), -28);
        SetBkColor(dc, workspace);
        SetDCBrushColor(dc, workspace);
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    SetBkColor(dc, edit ? ThemeWindowControlBackgroundColor() : AppDialogPanelBackgroundColor());
    return (LRESULT)(edit ? dlg->ctrlBrush : dlg->bgBrush);
}

static void PdfRotatePreviewZoom(PdfRotatePagesDialog* dlg, float factor, POINT anchor) {
    float next = limitValue(dlg->previewZoom * factor, 0.25f, 16.f);
    float ratio = next / dlg->previewZoom;
    float x = anchor.x - (dlg->previewRc.left + dlg->previewRc.right) * 0.5f;
    float y = anchor.y - (dlg->previewRc.top + dlg->previewRc.bottom) * 0.5f;
    dlg->panX = x - (x - dlg->panX) * ratio;
    dlg->panY = y - (y - dlg->panY) * ratio;
    dlg->previewZoom = next;
    HwndSetText(dlg->hwndPreviewInfo, str::FormatTemp(_TRA("Page %d  |  %.0f%%"), dlg->currentPageNo, next * 100));
    PdfRotateInvalidatePreview(dlg);
    SetTimer(dlg->hwnd, 1, 150, nullptr);
}

static void PdfRotateStopHold(PdfRotatePagesDialog* dlg) {
    HWND button = dlg->holdButton;
    dlg->holdButton = nullptr;
    KillTimer(dlg->hwnd, 2);
    if (button) {
        SendMessageW(button, BM_SETSTATE, FALSE, 0);
        if (GetCapture() == button) ReleaseCapture();
    }
}

static LRESULT CALLBACK PdfRotateFineButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                                DWORD_PTR data) {
    auto* dlg = (PdfRotatePagesDialog*)data;
    if (msg == WM_LBUTTONDOWN && IsWindowEnabled(hwnd)) {
        PdfRotateStopHold(dlg);
        SetFocus(hwnd);
        if (!dlg->angleValid || !PdfRotateSourceIsCurrent(dlg)) return 0;
        dlg->holdDirection = GetDlgCtrlID(hwnd) == idRotateAnglePlus ? 1 : -1;
        PdfRotateSetAngle(dlg, dlg->adjustment.deskew + dlg->holdDirection * 0.05, true, true);
        PdfRotatePagesUpdateUi(dlg);
        dlg->holdBase = dlg->adjustment.deskew;
        dlg->holdStart = GetTickCount64();
        dlg->holdButton = hwnd;
        SetCapture(hwnd);
        SendMessageW(hwnd, BM_SETSTATE, TRUE, 0);
        SetTimer(dlg->hwnd, 2, 16, nullptr);
        return 0;
    }
    if (msg == WM_MOUSEMOVE && dlg->holdButton == hwnd) {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        RECT client{};
        GetClientRect(hwnd, &client);
        // Moving outside cancels this press; reentry requires another press.
        if (!PtInRect(&client, pt)) PdfRotateStopHold(dlg);
        return 0;
    }
    if (msg == WM_LBUTTONUP && dlg->holdButton == hwnd) {
        PdfRotateStopHold(dlg);
        return 0;
    }
    if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE || msg == WM_KILLFOCUS || (msg == WM_ENABLE && !wp) ||
        msg == WM_NCDESTROY) {
        if (dlg->holdButton == hwnd) PdfRotateStopHold(dlg);
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, PdfRotateFineButtonProc, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void PdfRotateClearIconCache(PdfRotatePagesDialog* dlg) {
    for (auto& icons : dlg->iconCache) {
        for (auto& bitmap : icons) {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
    }
}

static LRESULT CALLBACK PdfRotateIconButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE) {
        if (!GetPropW(hwnd, L"PdfRotateHot")) {
            SetPropW(hwnd, L"PdfRotateHot", (HANDLE)1);
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (msg == WM_MOUSELEAVE || msg == WM_NCDESTROY) {
        RemovePropW(hwnd, L"PdfRotateHot");
        InvalidateRect(hwnd, nullptr, FALSE);
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, PdfRotateIconButtonProc, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void PdfRotateDrawIconButton(PdfRotatePagesDialog* dlg, DRAWITEMSTRUCT* item) {
    int id = item->CtlID;
    Rect r(item->rcItem.left, item->rcItem.top, item->rcItem.right - item->rcItem.left,
           item->rcItem.bottom - item->rcItem.top);
    bool hot = GetPropW(item->hwndItem, L"PdfRotateHot") != nullptr;
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    if (id >= idRotateMinimize && id <= idRotateClose) {
        CaptionSysButtonKind kind = id == idRotateMinimize ? CaptionSysButtonKind::Minimize
                                    : id == idRotateClose  ? CaptionSysButtonKind::Close
                                    : IsZoomed(dlg->hwnd)  ? CaptionSysButtonKind::Restore
                                                           : CaptionSysButtonKind::Maximize;
        DrawCaptionSysButton(item->hDC, kind, r, ThemeChromeBackgroundColor(), ThemeWindowTextColor(),
                             DpiScale(dlg->hwnd, 10), hot, pressed, GetActiveWindow() == dlg->hwnd);
        return;
    }
    bool direction = id == idRotateLeft90 || id == idRotateRight90 || id == idRotate180;
    bool selected = direction && dlg->adjustment.direction == (id == idRotateLeft90    ? -90
                                                               : id == idRotateRight90 ? 90
                                                                                       : 180);
    bool disabled = (item->itemState & ODS_DISABLED) != 0;
    COLORREF bg = direction               ? AppDialogPanelBackgroundColor()
                  : ThemeUsesDarkChrome() ? AccentColor(ThemeWindowBackgroundColor(), -12)
                                          : AccentColor(ThemeWindowControlBackgroundColor(), -28);
    if (selected)
        bg = ThemeInspectorSelectedBackgroundColor();
    else if (!disabled && (hot || pressed))
        bg = AccentColor(bg, pressed ? 12 : 8);
    COLORREF color = disabled   ? ThemeWindowTextDisabledColor()
                     : selected ? ThemeInspectorSelectedTextColor()
                                : ThemeWindowTextColor();
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(item->hDC, &item->rcItem, brush);
    DeleteObject(brush);
    int dpi = DpiGet(dlg->hwnd);
    if (dlg->iconCacheDpi != dpi || dlg->iconCacheEpoch != ThemeEpoch()) {
        PdfRotateClearIconCache(dlg);
        dlg->iconCacheDpi = dpi;
        dlg->iconCacheEpoch = ThemeEpoch();
    }
    int index = id == idRotateZoomIn    ? 0
                : id == idRotateZoomOut ? 1
                : id == idRotateZoomFit ? 2
                : id == idRotateLeft90  ? 3
                : id == idRotateRight90 ? 4
                                        : 5;
    int state = disabled ? 3 : pressed ? 2 : hot || selected ? 1 : 0;
    // Keep selected direction backgrounds separate from transient hover.
    if (selected) state += 4;
    int size = DpiScale(dlg->hwnd, 18);
    HDC memory = CreateCompatibleDC(item->hDC);
    HBITMAP& bitmap = dlg->iconCache[index][state];
    const TbIcon icons[] = {TbIcon::ZoomIn,     TbIcon::ZoomOut,     TbIcon::ArrowsDiagonalMinimize,
                            TbIcon::RotateLeft, TbIcon::RotateRight, TbIcon::RotateRight};
    if (!bitmap) {
        bitmap = CreateCompatibleBitmap(item->hDC, size, size);
        if (bitmap && memory) {
            HGDIOBJ old = SelectObject(memory, bitmap);
            DrawSvgIcon(memory, Rect(0, 0, size, size), icons[index], color, bg);
            SelectObject(memory, old);
        }
    }
    int ix = direction ? DpiScale(dlg->hwnd, 6) : (r.dx - size) / 2;
    if (bitmap && memory) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        BitBlt(item->hDC, ix, (r.dy - size) / 2, size, size, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
    }
    DeleteDC(memory);
    if (direction) {
        RECT text = item->rcItem;
        text.left += ix + size + DpiScale(dlg->hwnd, 4);
        text.right -= DpiScale(dlg->hwnd, 4);
        ScopedSelectObject font(item->hDC, dlg->hFont);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, color);
        const WCHAR* label = id == idRotate180 ? L"180°" : id == idRotateLeft90 ? _TRW("Left 90°") : _TRW("Right 90°");
        DrawTextW(item->hDC, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    if (item->itemState & ODS_FOCUS) {
        RECT focus = item->rcItem;
        InflateRect(&focus, -2, -2);
        DrawFocusRect(item->hDC, &focus);
    }
}

static LRESULT CALLBACK PdfRotatePagesDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PdfRotatePagesDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        dlg = (PdfRotatePagesDialog*)cs->lpCreateParams;
        dlg->hwnd = hwnd;
        dlg->bgBrush = CreateSolidBrush(AppDialogPanelBackgroundColor());
        dlg->ctrlBrush = CreateSolidBrush(ThemeWindowControlBackgroundColor());
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (PdfRotatePagesDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    if (msg == WM_ERASEBKGND) {
        // WM_PAINT presents a complete buffered frame, including background.
        // Erasing the visible canvas first exposes a dark flash while dragging.
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN) {
        return PdfRotatePagesColorControl(dlg, (HDC)wp, (HWND)lp);
    }

    switch (msg) {
        case WM_NCCALCSIZE: {
            RECT* rect = wp ? &((NCCALCSIZE_PARAMS*)lp)->rgrc[0] : (RECT*)lp;
            LONG top = rect->top;
            LRESULT result = DefWindowProc(hwnd, msg, wp, lp);
            rect->top = top + DpiScale(hwnd, 8);
            return result;
        }
        case WM_NCHITTEST: {
            LRESULT result = DefWindowProc(hwnd, msg, wp, lp);
            if (result != HTCLIENT && result != HTCAPTION) return result;
            POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
            ScreenToClient(hwnd, &pt);
            if (pt.y < DpiScale(hwnd, 32)) {
                if (pt.x >= ClientRect(hwnd).dx - DpiScale(hwnd, 46 * 3)) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_NCACTIVATE:
            InvalidateRect(hwnd, nullptr, FALSE);
            InvalidateRect(dlg->hwndMinimize, nullptr, FALSE);
            InvalidateRect(dlg->hwndMaximize, nullptr, FALSE);
            InvalidateRect(dlg->hwndClose, nullptr, FALSE);
            return DefWindowProc(hwnd, msg, wp, -1);
        case WM_DRAWITEM:
            if ((((DRAWITEMSTRUCT*)lp)->CtlID >= idRotateZoomIn && ((DRAWITEMSTRUCT*)lp)->CtlID <= idRotateClose) ||
                ((DRAWITEMSTRUCT*)lp)->CtlID == idRotateLeft90 || ((DRAWITEMSTRUCT*)lp)->CtlID == idRotateRight90 ||
                ((DRAWITEMSTRUCT*)lp)->CtlID == idRotate180) {
                PdfRotateDrawIconButton(dlg, (DRAWITEMSTRUCT*)lp);
                return TRUE;
            }
            break;
        case WM_MOUSEWHEEL: {
            POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
            ScreenToClient(hwnd, &pt);
            if ((GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) && PtInRect(&dlg->previewRc, pt)) {
                PdfRotatePreviewZoom(dlg, powf(1.2f, (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA), pt);
                return 0;
            }
            if (PtInRect(&dlg->sidebarRc, pt)) {
                dlg->wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
                int steps = dlg->wheelRemainder / WHEEL_DELTA;
                dlg->wheelRemainder %= WHEEL_DELTA;
                dlg->sidebarPos -= steps * DpiScale(hwnd, 72);
                PdfRotatePagesLayout(dlg, DpiGet(hwnd), true);
                RedrawWindow(hwnd, &dlg->sidebarRc, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
                return 0;
            }
            break;
        }
        case WM_VSCROLL:
            if ((HWND)lp == dlg->hwndSidebarScroll) {
                SCROLLINFO si{sizeof(si), SIF_ALL};
                GetScrollInfo(dlg->hwndSidebarScroll, SB_CTL, &si);
                switch (LOWORD(wp)) {
                    case SB_LINEUP:
                        dlg->sidebarPos -= DpiScale(hwnd, 28);
                        break;
                    case SB_LINEDOWN:
                        dlg->sidebarPos += DpiScale(hwnd, 28);
                        break;
                    case SB_PAGEUP:
                        dlg->sidebarPos -= si.nPage;
                        break;
                    case SB_PAGEDOWN:
                        dlg->sidebarPos += si.nPage;
                        break;
                    case SB_THUMBTRACK:
                        dlg->sidebarPos = si.nTrackPos;
                        break;
                }
                PdfRotatePagesLayout(dlg, DpiGet(hwnd), true);
                RedrawWindow(hwnd, &dlg->sidebarRc, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: {
            POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
            if (PtInRect(&dlg->previewRc, pt)) {
                dlg->dragging = true;
                dlg->dragStart = pt;
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (dlg->dragging) {
                POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
                dlg->panX += pt.x - dlg->dragStart.x;
                dlg->panY += pt.y - dlg->dragStart.y;
                dlg->dragStart = pt;
                PdfRotateInvalidatePreview(dlg);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (dlg->dragging) {
                dlg->dragging = false;
                ReleaseCapture();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            dlg->dragging = false;
            return 0;
        case WM_SETCURSOR: {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (LOWORD(lp) == HTCLIENT && PtInRect(&dlg->previewRc, pt)) {
                SetCursor(LoadCursorW(nullptr, dlg->dragging ? IDC_SIZEALL : IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_GETMINMAXINFO:
            if (!dlg->minWindowSize.IsEmpty()) {
                auto info = (MINMAXINFO*)lp;
                info->ptMinTrackSize = {dlg->minWindowSize.dx, dlg->minWindowSize.dy};
            }
            return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && dlg->hwndCancelBtn) {
                PdfRotatePagesLayout(dlg, DpiGet(hwnd), true);
                InvalidateRect(hwnd, nullptr, TRUE);
                SetTimer(hwnd, 1, 150, nullptr);
            }
            return 0;
        case WM_TIMER:
            if (wp == 2 && dlg->holdButton) {
                if (GetCapture() != dlg->holdButton || !PdfRotateSourceIsCurrent(dlg) || GetActiveWindow() != hwnd ||
                    !IsWindowEnabled(dlg->holdButton)) {
                    PdfRotateStopHold(dlg);
                    PdfRotatePagesUpdateUi(dlg);
                    return 0;
                }
                double elapsed = (GetTickCount64() - dlg->holdStart) / 1000.0;
                PdfRotateSetAngle(dlg, dlg->holdBase + dlg->holdDirection * PdfAdjustmentHoldTravel(elapsed), true,
                                  true);
                PdfRotatePagesUpdateUi(dlg);
                return 0;
            }
            if (wp == 1) {
                if (dlg->holdButton) return 0;
                KillTimer(hwnd, 1);
                PdfRotateLoadPreview(dlg);
                PdfRotateInvalidatePreview(dlg);
                return 0;
            }
            break;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            switch (id) {
                case idRotateMinimize:
                    ShowWindow(hwnd, SW_MINIMIZE);
                    return 0;
                case idRotateMaximize:
                    ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                    return 0;
                case idRotateClose:
                    SendMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                case idRotateZoomIn:
                case idRotateZoomOut: {
                    POINT center{(dlg->previewRc.left + dlg->previewRc.right) / 2,
                                 (dlg->previewRc.top + dlg->previewRc.bottom) / 2};
                    PdfRotatePreviewZoom(dlg, id == idRotateZoomIn ? 1.2f : 1.f / 1.2f, center);
                    return 0;
                }
                case idRotateZoomFit:
                    dlg->previewZoom = 1.f;
                    dlg->panX = dlg->panY = 0;
                    HwndSetText(dlg->hwndPreviewInfo,
                                str::FormatTemp(_TRA("Page %d  |  %.0f%%"), dlg->currentPageNo, 100.0));
                    PdfRotateInvalidatePreview(dlg);
                    return 0;
                case idRotateAngleMinus:
                case idRotateAnglePlus:
                    if (code == BN_CLICKED) {
                        if (!dlg->angleValid) return 0;
                        double step = id == idRotateAnglePlus ? 0.05 : -0.05;
                        PdfRotateSetAngle(dlg, dlg->adjustment.deskew + step, true, true);
                        PdfRotatePagesUpdateUi(dlg);
                        return 0;
                    }
                    break;
                case idRotateApply:
                    if (code == BN_CLICKED) {
                        PdfRotateStopHold(dlg);
                        PdfRotatePagesDoIt(dlg);
                        return 0;
                    }
                    break;
                case idRotateCancel:
                    if (code == BN_CLICKED) {
                        DestroyWindow(hwnd);
                        return 0;
                    }
                    break;
                case idRotateApplyToCurrent:
                case idRotateApplyToAll:
                case idRotateApplyToSpecified:
                    if (code == BN_CLICKED) {
                        PdfRotatePagesUpdateUi(dlg);
                        if (id == idRotateApplyToSpecified) {
                            SetFocus(dlg->hwndPagesEdit);
                        }
                        return 0;
                    }
                    break;
                case idRotatePagesEdit:
                    if (code == EN_CHANGE) {
                        PdfRotatePagesUpdateUi(dlg);
                        return 0;
                    }
                    break;
                case idRotateAny:
                    if (code == BN_CLICKED) {
                        if (!dlg->angleValid) return 0;
                        dlg->adjustment.direction = 0;
                        PdfRotateSetAngle(dlg, dlg->adjustment.deskew, false, true);
                        InvalidateRect(dlg->hwndLeft90, nullptr, FALSE);
                        InvalidateRect(dlg->hwndRight90, nullptr, FALSE);
                        InvalidateRect(dlg->hwnd180, nullptr, FALSE);
                        PdfRotatePagesUpdateUi(dlg);
                        PdfRotateInvalidatePreview(dlg);
                        return 0;
                    }
                    break;
                case idRotateLeft90:
                case idRotateRight90:
                case idRotate180:
                    if (code == BN_CLICKED) {
                        if (!dlg->angleValid) return 0;
                        dlg->adjustment.direction = id == idRotateLeft90 ? -90 : id == idRotateRight90 ? 90 : 180;
                        PdfRotateSetAngle(dlg, dlg->adjustment.deskew, false, true);
                        InvalidateRect(dlg->hwndLeft90, nullptr, FALSE);
                        InvalidateRect(dlg->hwndRight90, nullptr, FALSE);
                        InvalidateRect(dlg->hwnd180, nullptr, FALSE);
                        PdfRotatePagesUpdateUi(dlg);
                        return 0;
                    }
                    break;
                case idRotateAngleEdit:
                    if (code == EN_CHANGE) {
                        if (dlg->syncing) {
                            return 0;
                        }
                        char text[128]{};
                        GetWindowTextA(dlg->hwndAngleEdit, text, dimof(text));
                        double angle;
                        dlg->angleValid = PdfParseAdjustmentAngle(text, &angle);
                        if (dlg->angleValid) {
                            PdfRotateSetAngle(dlg, angle, false, true);
                        } else {
                            HwndSetText(dlg->hwndAngleHint, _TRA("Enter an angle from -180° to +180°."));
                        }
                        PdfRotatePagesUpdateUi(dlg);
                        return 0;
                    }
                    break;
                case idRotateShowGrid:
                    if (code == BN_CLICKED) {
                        dlg->showGrid = SendMessageW(dlg->hwndShowGrid, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        EnableWindow(dlg->hwndGridDensity, dlg->showGrid);
                        PdfRotateInvalidatePreview(dlg);
                        return 0;
                    }
                    break;
                case idRotateGridDensity:
                    if (code == CBN_SELCHANGE) {
                        dlg->gridDensity =
                            limitValue((int)SendMessageW(dlg->hwndGridDensity, CB_GETCURSEL, 0, 0), 0, 2);
                        PdfRotateInvalidatePreview(dlg);
                        return 0;
                    }
                    break;
                case idRotateFlipH:
                case idRotateFlipV:
                case idRotateAutoCrop:
                    if (code == BN_CLICKED) {
                        PdfRotatePagesUpdateUi(dlg);
                        PdfRotateInvalidatePreview(dlg);
                        return 0;
                    }
                    break;
            }
            break;
        }
        case WM_HSCROLL:
            if ((HWND)lp == dlg->hwndSlider && !dlg->syncing) {
                int pos = (int)SendMessageW(dlg->hwndSlider, TBM_GETPOS, 0, 0);
                PdfRotateSetAngle(dlg, pos / 100.0, true, false);
                PdfRotatePagesUpdateUi(dlg);
                return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC targetDc = BeginPaint(hwnd, &ps);
            Rect client = ClientRect(hwnd);
            if (!dlg->paintBuffer || dlg->paintBufferSize.dx != client.dx || dlg->paintBufferSize.dy != client.dy) {
                delete dlg->paintBuffer;
                dlg->paintBuffer = new DoubleBuffer(hwnd, client);
                dlg->paintBufferSize = {client.dx, client.dy};
            }
            HDC paintDc = dlg->paintBuffer->GetDC();
            RECT background{0, 0, client.dx, client.dy};
            FillRect(paintDc, &background, dlg->bgBrush);
            RECT caption{0, 0, client.dx, DpiScale(hwnd, 32)};
            HBRUSH brush = CreateSolidBrush(ThemeChromeBackgroundColor());
            FillRect(paintDc, &caption, brush);
            DeleteObject(brush);
            caption.left += DpiScale(hwnd, 12);
            caption.right -= DpiScale(hwnd, 46 * 3);
            SetBkMode(paintDc, TRANSPARENT);
            SetTextColor(paintDc, ThemeWindowTextColor());
            HGDIOBJ font = SelectObject(paintDc, dlg->hFont);
            DrawTextW(paintDc, _TRW("Manually Adjust Pages"), -1, &caption,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(paintDc, font);
            PdfRotatePaintPreview(dlg, paintDc);
            int savedDc = SaveDC(paintDc);
            IntersectClipRect(paintDc, dlg->sidebarRc.left, dlg->sidebarRc.top, dlg->sidebarRc.right,
                              dlg->sidebarRc.bottom);
            HPEN separatorPen = CreatePen(PS_SOLID, 1, ThemeSelectionFrameColor());
            HGDIOBJ oldPen = SelectObject(paintDc, separatorPen);
            for (int y : dlg->separatorY) {
                MoveToEx(paintDc, dlg->sidebarRc.left, y, nullptr);
                LineTo(paintDc, dlg->sidebarRc.right, y);
            }
            SelectObject(paintDc, oldPen);
            DeleteObject(separatorPen);
            RestoreDC(paintDc, savedDc);
            dlg->paintBuffer->Flush(targetDc, &ps.rcPaint);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case DM_GETDEFID:
            // Enter triggers "Apply Rotation"
            return MAKELRESULT(idRotateApply, DC_HASDEFID);
        case WM_DPICHANGED: {
            PdfRotateStopHold(dlg);
            // Trust LOWORD(wParam). GetDpiForWindow and monitor queries lag while
            // the window is crossing displays (see OnMainWindowDpiChanged).
            int dpi = (int)LOWORD(wp);
            if (dpi <= 0) {
                dpi = 96;
            }
            RECT* prc = (RECT*)lp;
            AppDialogFonts fonts;
            fonts.CreateForDpi(dpi);
            if (fonts.body) {
                DeleteObject(dlg->hFont);
                DeleteObject(dlg->hFontBold);
                dlg->hFont = fonts.body;
                dlg->hFontBold = fonts.semibold;
                fonts.body = fonts.semibold = nullptr;
                AppDialogApplyFontToChildren(hwnd, dlg->hFont);
                HFONT title = dlg->hFontBold ? dlg->hFontBold : dlg->hFont;
                if (dlg->hwndApplyToLabel) {
                    SendMessageW(dlg->hwndApplyToLabel, WM_SETFONT, (WPARAM)title, TRUE);
                }
                for (HWND heading : {dlg->hwndGridTitle, dlg->hwndRotationTitle, dlg->hwndFlipTitle}) {
                    SendMessageW(heading, WM_SETFONT, (WPARAM)title, TRUE);
                }
                if (dlg->tooltip) SendMessageW(dlg->tooltip->hwnd, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
            }
            // Keep the user's enlarged size when moving between monitors;
            // recompute the minimum and child geometry for the new DPI.
            dlg->minWindowSize = PdfRotatePagesOuterSize(hwnd, dpi, PdfRotatePx(780, dpi), PdfRotatePx(480, dpi));
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            int x = 0;
            int y = 0;
            if (prc) {
                x = prc->left;
                y = prc->top;
            } else {
                flags |= SWP_NOMOVE;
            }
            if (prc) {
                SetWindowPos(hwnd, nullptr, x, y, prc->right - prc->left, prc->bottom - prc->top, flags);
            }
            PdfRotatePagesLayout(dlg, dpi, true);
            return 0;
        }
        case WM_ACTIVATE:
            // track focus so the main loop routes Enter/Esc/Tab through
            // IsDialogMessage only while this window is the active one
            SetCurrentModelessDialog(LOWORD(wp) == WA_INACTIVE ? nullptr : hwnd);
            if (LOWORD(wp) == WA_INACTIVE) PdfRotateStopHold(dlg);
            PdfRotatePagesUpdateUi(dlg);
            return 0;
        case WM_CANCELMODE:
            PdfRotateStopHold(dlg);
            return 0;
        case WM_CLOSE:
            PdfRotateStopHold(dlg);
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            PdfRotateStopHold(dlg);
            PdfRotateClearIconCache(dlg);
            delete dlg->tooltip;
            dlg->tooltip = nullptr;
            UnregisterAppDialogForTheme(hwnd);
            if (GetCurrentModelessDialog() == hwnd) {
                SetCurrentModelessDialog(nullptr);
            }
            DeleteObject(dlg->hFontBold);
            DeleteObject(dlg->hFont);
            dlg->hFontBold = dlg->hFont = nullptr;
            DeleteObject(dlg->bgBrush);
            DeleteObject(dlg->ctrlBrush);
            dlg->bgBrush = dlg->ctrlBrush = nullptr;
            delete dlg->gdiPreview;
            dlg->gdiPreview = nullptr;
            delete dlg->paintBuffer;
            dlg->paintBuffer = nullptr;
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kPdfRotatePagesWinClassName = L"SUMATRA_PDF_ROTATE_PAGES";
static bool gPdfRotatePagesWinClassRegistered = false;

static void PdfRotatePagesThemeRefreshCb(HWND hwnd, void* ctx);

// compact, one-purpose dialog: apply-to scope, rotation direction, buttons
void ShowPdfRotatePagesDialog(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    if (!EngineMupdfCanAdjustPageView(tab->GetEngine())) {
        return;
    }

    int pageCount = win->ctrl ? win->ctrl->PageCount() : 0;
    int currentPageNo = win->ctrl ? win->ctrl->CurrentPageNo() : 1;
    if (pageCount < 1) {
        return;
    }
    logf("ShowPdfRotatePagesDialog: page %d of %d\n", currentPageNo, pageCount);

    if (!gPdfRotatePagesWinClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PdfRotatePagesDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPdfRotatePagesWinClassName;
        RegisterClassExW(&wc);
        gPdfRotatePagesWinClassRegistered = true;
    }

    PdfRotatePagesDialog* dlg = new PdfRotatePagesDialog();
    dlg->win = win;
    dlg->pageCount = pageCount;
    dlg->currentPageNo = currentPageNo;
    dlg->sourceTab = tab;
    dlg->sourceEngine = tab->GetEngine();

    HINSTANCE h = GetModuleHandleW(nullptr);
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kPdfRotatePagesWinClassName, _TRW("Manually Adjust Pages"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        delete dlg;
        return;
    }

    int dpi = DpiGet(hwnd);
    if (dpi <= 0) {
        dpi = DpiGet(win->hwndFrame);
    }
    AppDialogFonts fonts;
    fonts.CreateForDpi(dpi);
    if (!fonts.body) {
        DestroyWindow(hwnd);
        delete dlg;
        return;
    }
    dlg->hFont = fonts.body;
    dlg->hFontBold = fonts.semibold;
    fonts.body = fonts.semibold = nullptr;
    HFONT hfontTitle = dlg->hFontBold ? dlg->hFontBold : dlg->hFont;

    // group: apply to. Positions come from PdfRotatePagesLayout so creation
    // and WM_DPICHANGED cannot drift.
    dlg->hwndApplyToLabel = CreateWindowExW(0, L"STATIC", _TRW("Apply To"), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
                                            hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndApplyToLabel, WM_SETFONT, (WPARAM)hfontTitle, TRUE);

    WCHAR* curTxt = ToWStrTemp(str::FormatTemp(_TRA("Current Page (Page %d)"), dlg->currentPageNo));
    dlg->hwndCurrentPage =
        CreateWindowExW(0, L"BUTTON", curTxt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 0, 0,
                        0, 0, hwnd, (HMENU)(INT_PTR)idRotateApplyToCurrent, h, nullptr);
    SendMessageW(dlg->hwndCurrentPage, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SendMessageW(dlg->hwndCurrentPage, BM_SETCHECK, BST_CHECKED, 0);

    WCHAR* allTxt = ToWStrTemp(str::FormatTemp(_TRA("All Pages (%d)"), dlg->pageCount));
    dlg->hwndAllPages = CreateWindowExW(0, L"BUTTON", allTxt, WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 0, 0, 0, 0,
                                        hwnd, (HMENU)(INT_PTR)idRotateApplyToAll, h, nullptr);
    SendMessageW(dlg->hwndAllPages, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndSpecified =
        CreateWindowExW(0, L"BUTTON", _TRW("Specified Pages"), WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 0, 0, 0, 0,
                        hwnd, (HMENU)(INT_PTR)idRotateApplyToSpecified, h, nullptr);
    SendMessageW(dlg->hwndSpecified, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndPagesEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0,
                        0, hwnd, (HMENU)(INT_PTR)idRotatePagesEdit, h, nullptr);
    SendMessageW(dlg->hwndPagesEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndPagesHint = CreateWindowExW(0, L"STATIC", _TRW("e.g. 2, 5-7, 13-"), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0,
                                         0, 0, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPagesHint, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndAny = CreateWindowExW(0, L"BUTTON", _TRW("Deskew"),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 0, 0, 0, 0, hwnd,
                                   (HMENU)(INT_PTR)idRotateAny, h, nullptr);
    SendMessageW(dlg->hwndAny, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SendMessageW(dlg->hwndAny, BM_SETCHECK, BST_CHECKED, 0);

    dlg->hwndSlider =
        CreateWindowExW(0, TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, 0, 0, 0,
                        0, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndSlider, TBM_SETRANGEMIN, FALSE, (LPARAM)-500);
    SendMessageW(dlg->hwndSlider, TBM_SETRANGEMAX, TRUE, (LPARAM)500);
    SendMessageW(dlg->hwndSlider, TBM_SETTICFREQ, 500, 0);
    SendMessageW(dlg->hwndSlider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(dlg->hwndSlider, TBM_SETPAGESIZE, 0, 100);
    SendMessageW(dlg->hwndSlider, TBM_SETPOS, TRUE, 0);

    dlg->syncing = true;
    dlg->hwndAngleEdit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"0.00",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER, 0, 0, 0, 0,
                                         hwnd, (HMENU)(INT_PTR)idRotateAngleEdit, h, nullptr);
    SendMessageW(dlg->hwndAngleEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    dlg->syncing = false;

    dlg->hwndAngleMinus = CreateWindowExW(0, L"BUTTON", L"\u2212", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                          0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)idRotateAngleMinus, h, nullptr);
    dlg->hwndAnglePlus = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                         0, 0, hwnd, (HMENU)(INT_PTR)idRotateAnglePlus, h, nullptr);
    SendMessageW(dlg->hwndAngleMinus, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SendMessageW(dlg->hwndAnglePlus, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SetWindowSubclass(dlg->hwndAngleMinus, PdfRotateFineButtonProc, 1, (DWORD_PTR)dlg);
    SetWindowSubclass(dlg->hwndAnglePlus, PdfRotateFineButtonProc, 1, (DWORD_PTR)dlg);

    dlg->hwndDegLabel = CreateWindowExW(0, L"STATIC", L"°", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0,
                                        0, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDegLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndFlipH = CreateWindowExW(0, L"BUTTON", _TRW("Flip Horizontal"),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                                     (HMENU)(INT_PTR)idRotateFlipH, h, nullptr);
    SendMessageW(dlg->hwndFlipH, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    dlg->hwndFlipV = CreateWindowExW(0, L"BUTTON", _TRW("Flip Vertical"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0,
                                     0, 0, hwnd, (HMENU)(INT_PTR)idRotateFlipV, h, nullptr);
    SendMessageW(dlg->hwndFlipV, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    dlg->hwndAutoCrop = CreateWindowExW(0, L"BUTTON", _TRW("Auto Crop"),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX, 0, 0, 0, 0,
                                        hwnd, (HMENU)(INT_PTR)idRotateAutoCrop, h, nullptr);
    SendMessageW(dlg->hwndAutoCrop, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SendMessageW(dlg->hwndAutoCrop, BM_SETCHECK, BST_UNCHECKED, 0);

    WCHAR* applyTxt = _TRW("Apply");
    dlg->hwndRotateBtn = CreateWindowExW(0, L"BUTTON", applyTxt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)idRotateApply, h, nullptr);
    SendMessageW(dlg->hwndRotateBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    WCHAR* cancelTxt = _TRW("Cancel");
    dlg->hwndCancelBtn = CreateWindowExW(0, L"BUTTON", cancelTxt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                         0, 0, 0, hwnd, (HMENU)(INT_PTR)idRotateCancel, h, nullptr);
    SendMessageW(dlg->hwndCancelBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    auto label = [&](const WCHAR* text, bool bold = true) {
        HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0, 0,
                                       hwnd, nullptr, h, nullptr);
        SendMessageW(control, WM_SETFONT, (WPARAM)(bold ? hfontTitle : dlg->hFont), TRUE);
        return control;
    };
    dlg->hwndGridTitle = label(_TRW("Reference Grid"));
    dlg->hwndRotationTitle = label(_TRW("Page Rotation"));
    dlg->hwndFlipTitle = label(_TRW("Flip & Crop"));
    dlg->hwndAngleHint = label(_TRW("-5°                 0°                 +5°"), false);
    dlg->hwndPreviewInfo = label(ToWStrTemp(str::FormatTemp(_TRA("Page %d  |  %.0f%%"), currentPageNo, 100.0)), false);
    dlg->hwndShowGrid =
        CreateWindowExW(0, L"BUTTON", _TRW("Show Grid"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0,
                        0, hwnd, (HMENU)(INT_PTR)idRotateShowGrid, h, nullptr);
    SendMessageW(dlg->hwndShowGrid, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SendMessageW(dlg->hwndShowGrid, BM_SETCHECK, BST_CHECKED, 0);
    dlg->hwndGridDensity = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                           0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)idRotateGridDensity, h, nullptr);
    SendMessageW(dlg->hwndGridDensity, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    CbAddString(dlg->hwndGridDensity, _TRA("Sparse"));
    CbAddString(dlg->hwndGridDensity, _TRA("Normal"));
    CbAddString(dlg->hwndGridDensity, _TRA("Dense"));
    SendMessageW(dlg->hwndGridDensity, CB_SETCURSEL, 1, 0);
    dlg->tooltip = new Tooltip();
    Tooltip::CreateArgs tooltipArgs;
    tooltipArgs.parent = hwnd;
    tooltipArgs.font = dlg->hFont;
    dlg->tooltip->Create(tooltipArgs);

    auto iconButton = [&](int id, const WCHAR* name) {
        HWND button = CreateWindowExW(0, L"BUTTON", name, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                      hwnd, (HMENU)(INT_PTR)id, h, nullptr);
        SendMessageW(button, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
        SetWindowSubclass(button, PdfRotateIconButtonProc, 1, 0);
        TOOLINFOW info{sizeof(info)};
        info.hwnd = hwnd;
        info.uId = (UINT_PTR)button;
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.lpszText = (WCHAR*)name;
        SendMessageW(dlg->tooltip->hwnd, TTM_ADDTOOLW, 0, (LPARAM)&info);
        return button;
    };
    dlg->hwndLeft90 = iconButton(idRotateLeft90, _TRW("Left 90°"));
    dlg->hwndRight90 = iconButton(idRotateRight90, _TRW("Right 90°"));
    dlg->hwnd180 = iconButton(idRotate180, L"180°");
    dlg->hwndMinimize = iconButton(idRotateMinimize, _TRW("Minimize"));
    dlg->hwndMaximize = iconButton(idRotateMaximize, _TRW("Maximize"));
    dlg->hwndClose = iconButton(idRotateClose, _TRW("Close"));
    dlg->hwndZoomIn = iconButton(idRotateZoomIn, _TRW("Zoom In"));
    dlg->hwndZoomOut = iconButton(idRotateZoomOut, _TRW("Zoom Out"));
    dlg->hwndZoomFit = iconButton(idRotateZoomFit, _TRW("Fit Page"));
    dlg->hwndSidebarScroll =
        CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | SBS_VERT, 0, 0, 0, 0, hwnd, nullptr, h, nullptr);
    AppDialogThemeScrollBar(dlg->hwndSidebarScroll);

    Size outer = PdfRotatePagesLayout(dlg, dpi);
    dlg->minWindowSize = PdfRotatePagesOuterSize(hwnd, dpi, PdfRotatePx(780, dpi), PdfRotatePx(480, dpi));
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(win->hwndFrame, MONITOR_DEFAULTTONEAREST), &monitor)) {
        outer.dx = std::min(outer.dx, (int)(monitor.rcWork.right - monitor.rcWork.left));
        outer.dy = std::min(outer.dy, (int)(monitor.rcWork.bottom - monitor.rcWork.top));
    }
    if (!outer.IsEmpty()) {
        SetWindowPos(hwnd, nullptr, 0, 0, outer.dx, outer.dy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // initial enable/disable state (edit disabled for current-page scope)
    PdfRotatePagesUpdateUi(dlg);
    PdfRotateLoadPreview(dlg);

    CenterDialog(hwnd, win->hwndFrame);
    AppDialogApplyChrome(hwnd);
    RegisterAppDialogForTheme(hwnd, PdfRotatePagesThemeRefreshCb, dlg);
    ShowWindow(hwnd, SW_SHOW);
}

static void PdfRotatePagesThemeRefreshCb(HWND hwnd, void* ctx) {
    auto* dlg = (PdfRotatePagesDialog*)ctx;
    if (!dlg) {
        return;
    }
    PdfRotateStopHold(dlg);
    PdfRotateClearIconCache(dlg);
    if (dlg->tooltip) dlg->tooltip->UpdateTheme();
    DeleteObject(dlg->bgBrush);
    DeleteObject(dlg->ctrlBrush);
    dlg->bgBrush = CreateSolidBrush(AppDialogPanelBackgroundColor());
    dlg->ctrlBrush = CreateSolidBrush(ThemeWindowControlBackgroundColor());
    AppDialogApplyChrome(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

// theme switch while the modeless dialog is open: rebuild the theme brushes
// and repaint (light and dark). Also registered via AppDialogTheme.
void RefreshPdfRotatePagesTheme() {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM) -> BOOL {
            WCHAR cls[64]{};
            GetClassNameW(hwnd, cls, dimof(cls));
            if (!wcscmp(cls, kPdfRotatePagesWinClassName)) {
                auto* dlg = (PdfRotatePagesDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                PdfRotatePagesThemeRefreshCb(hwnd, dlg);
            }
            return TRUE;
        },
        0);
}

// --- Encrypt PDF dialog ---

struct PdfEncryptDialog {
    HWND hwnd = nullptr;
    HWND hwndPathLabel = nullptr;
    HWND hwndDestEdit = nullptr;
    HWND hwndBrowseBtn = nullptr;
    HWND hwndPasswordLabel = nullptr;
    HWND hwndPasswordEdit = nullptr;
    HWND hwndEncryptBtn = nullptr;
    HWND hwndCancelBtn = nullptr;
    HFONT hFont = nullptr;
    int dpi = 96;
    bool fontOwned = false;
    bool dpiReady = false;
    char* srcPath = nullptr;
    MainWindow* win = nullptr;
};

static void PdfEncryptUpdateButton(PdfEncryptDialog* dlg) {
    char pwd[256]{};
    GetWindowTextA(dlg->hwndPasswordEdit, pwd, dimof(pwd));
    BOOL enable = !str::IsEmpty(pwd);
    EnableWindow(dlg->hwndEncryptBtn, enable);
}

static void PdfEncryptOnBrowse(PdfEncryptDialog* dlg) {
    WCHAR dstFileName[MAX_PATH + 1]{};
    GetWindowTextW(dlg->hwndDestEdit, dstFileName, MAX_PATH);

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg->hwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"pdf";

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(dlg->hwndDestEdit, dstFileName);
    }
}

static void PdfEncryptDoIt(PdfEncryptDialog* dlg) {
    char destPath[MAX_PATH + 1]{};
    GetWindowTextA(dlg->hwndDestEdit, destPath, MAX_PATH);
    if (str::IsEmpty(destPath)) {
        return;
    }

    char pwd[256]{};
    GetWindowTextA(dlg->hwndPasswordEdit, pwd, dimof(pwd));
    if (str::IsEmpty(pwd)) {
        return;
    }

    logf("PdfEncryptDoIt: encrypting '%s' to '%s' with AES-256\n", dlg->srcPath, destPath);

    // equivalent of: clean -E aes-256 -U <pwd> -O <pwd> input output
    char* argv[] = {
        (char*)"clean", (char*)"-E", (char*)"aes-256", (char*)"-U", pwd, (char*)"-O", pwd, dlg->srcPath, destPath,
    };
    int argc = 9;

    fz_set_optind(0);
    int res = pdfclean_main(argc, argv);
    if (res == 0) {
        logf("PdfEncryptDoIt: encrypted successfully\n");
        MainWindow* win = dlg->win;
        TempStr path = str::DupTemp(destPath);
        DestroyWindow(dlg->hwnd);
        LoadArgs args(path, win);
        StartLoadDocument(&args);
    } else {
        logf("PdfEncryptDoIt: pdfclean_main failed with %d\n", res);
        MessageBoxWarning(dlg->hwnd, _TRA("Failed to encrypt PDF file."), _TRA("Encrypt PDF"));
    }
}

static LRESULT CALLBACK PdfEncryptDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PdfEncryptDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        dlg = (PdfEncryptDialog*)cs->lpCreateParams;
        dlg->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (PdfEncryptDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_COMMAND: {
            int code = HIWORD(wp);
            HWND ctl = (HWND)lp;
            if (ctl == dlg->hwndBrowseBtn && code == BN_CLICKED) {
                PdfEncryptOnBrowse(dlg);
                return 0;
            }
            if (ctl == dlg->hwndEncryptBtn && code == BN_CLICKED) {
                PdfEncryptDoIt(dlg);
                return 0;
            }
            if (ctl == dlg->hwndCancelBtn && code == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (ctl == dlg->hwndPasswordEdit && code == EN_CHANGE) {
                PdfEncryptUpdateButton(dlg);
                return 0;
            }
            break;
        }
        case WM_DPICHANGED:
            PdfToolHandleDpi(hwnd, &dlg->dpi, &dlg->hFont, &dlg->fontOwned, dlg->dpiReady, wp, lp);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (dlg->fontOwned && dlg->hFont) {
                DeleteObject(dlg->hFont);
                dlg->hFont = nullptr;
            }
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kPdfEncryptWinClassName = L"SUMATRA_PDF_ENCRYPT";
static bool gPdfEncryptWinClassRegistered = false;

void ShowPdfEncryptDialog(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    if (!CouldBePDFDoc(tab)) {
        return;
    }
    EngineBase* engine = tab->GetEngine();
    if (EngineMupdfIsEncrypted(engine)) {
        logf("ShowPdfEncryptDialog: '%s' is already encrypted, skipping\n", tab->filePath);
        return;
    }
    logf("ShowPdfEncryptDialog: opening for '%s'\n", tab->filePath);

    if (!gPdfEncryptWinClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PdfEncryptDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPdfEncryptWinClassName;
        RegisterClassExW(&wc);
        gPdfEncryptWinClassRegistered = true;
    }

    PdfEncryptDialog* dlg = new PdfEncryptDialog();
    dlg->srcPath = str::Dup(tab->filePath);
    dlg->win = win;
    dlg->dpi = DpiGet(win->hwndFrame);
    if (dlg->dpi < 72) {
        dlg->dpi = 96;
    }
    dlg->hFont = CreatePdfToolFont(dlg->dpi);
    dlg->fontOwned = dlg->hFont != nullptr;
    if (!dlg->hFont) {
        dlg->hFont = GetDefaultGuiFont();
    }

    DlgMetrics m = GetDlgMetrics(win->hwndFrame, dlg->hFont);
    int minW = DpiScale(win->hwndFrame, 500);
    int dlgW = CalcDlgWidth(win->hwndFrame, dlg->hFont, tab->filePath, minW, m.padding);
    int dlgH = CalcDlgHeight(win->hwndFrame, m, 4);

    HINSTANCE h = GetModuleHandleW(nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPdfEncryptWinClassName, _TRW("Encrypt PDF"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                dlgW, dlgH, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        str::Free(dlg->srcPath);
        delete dlg;
        return;
    }

    int x = m.padding;
    int y = m.padding;
    int w = dlgW - 2 * m.padding - DpiScale(hwnd, 16);

    // row 1: source path label (offset to align with text inside edit control)
    dlg->hwndPathLabel =
        CreateWindowExW(0, L"STATIC", ToWStrTemp(tab->filePath), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                        x + m.editXOff, y, w - m.editXOff, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPathLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 2: dest edit + browse button
    TempStr destPath = MakeUniqueFilePathTemp(tab->filePath);
    dlg->hwndDestEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(destPath), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y,
                        w - m.browseW - m.btnGap, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDestEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + w - m.browseW,
                                         y, m.browseW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndBrowseBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 3: password label + password edit
    const WCHAR* pwdLabel = _TRW("Password:");
    Size labelSize = HwndMeasureText(hwnd, ToUtf8Temp(pwdLabel), dlg->hFont);
    int labelW = labelSize.dx + DpiScale(hwnd, 4);

    dlg->hwndPasswordLabel = CreateWindowExW(0, L"STATIC", pwdLabel, WS_CHILD | WS_VISIBLE | SS_LEFT, x + m.editXOff,
                                             y + m.editXOff, labelW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPasswordLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndPasswordEdit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                            x + labelW, y, w - labelW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPasswordEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 4: Encrypt PDF + Cancel buttons (right-aligned)
    int bx = x + w - m.btnW;
    dlg->hwndCancelBtn = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx, y,
                                         m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndCancelBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    bx -= m.btnW + m.btnGap;
    dlg->hwndEncryptBtn = CreateWindowExW(0, L"BUTTON", _TRW("Encrypt PDF"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                          bx, y, m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndEncryptBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    // disable encrypt button until password is entered
    EnableWindow(dlg->hwndEncryptBtn, FALSE);

    CenterDialog(hwnd, win->hwndFrame);
    if (UseDarkModeLib()) {
        DarkMode::setDarkWndSafe(hwnd);
        DarkMode::setWindowEraseBgSubclass(hwnd);
    }
    UpdateWindowCaptionTheme(hwnd);
    dlg->dpiReady = true;
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(dlg->hwndPasswordEdit);
}

// --- Decrypt PDF dialog ---

struct PdfDecryptDialog {
    HWND hwnd = nullptr;
    HWND hwndPathLabel = nullptr;
    HWND hwndDestEdit = nullptr;
    HWND hwndBrowseBtn = nullptr;
    HWND hwndDecryptBtn = nullptr;
    HWND hwndCancelBtn = nullptr;
    HFONT hFont = nullptr;
    int dpi = 96;
    bool fontOwned = false;
    bool dpiReady = false;
    char* srcPath = nullptr;
    char* password = nullptr;
    MainWindow* win = nullptr;
};

static void PdfDecryptOnBrowse(PdfDecryptDialog* dlg) {
    WCHAR dstFileName[MAX_PATH + 1]{};
    GetWindowTextW(dlg->hwndDestEdit, dstFileName, MAX_PATH);

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg->hwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"pdf";

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(dlg->hwndDestEdit, dstFileName);
    }
}

static void PdfDecryptDoIt(PdfDecryptDialog* dlg) {
    char destPath[MAX_PATH + 1]{};
    GetWindowTextA(dlg->hwndDestEdit, destPath, MAX_PATH);
    if (str::IsEmpty(destPath)) {
        return;
    }

    logf("PdfDecryptDoIt: decrypting '%s' to '%s', password len: %d\n", dlg->srcPath, destPath,
         (int)str::Len(dlg->password));

    // equivalent of: clean -p <pwd> -D input output
    // -p provides the password to open the encrypted input, -D removes encryption from output
    char* argv[] = {
        (char*)"clean", (char*)"-p", dlg->password, (char*)"-D", dlg->srcPath, destPath,
    };
    int argc = 6;

    fz_set_optind(0);
    int res = pdfclean_main(argc, argv);
    if (res == 0) {
        logf("PdfDecryptDoIt: decrypted successfully\n");
        MainWindow* win = dlg->win;
        TempStr path = str::DupTemp(destPath);
        DestroyWindow(dlg->hwnd);
        LoadArgs args(path, win);
        StartLoadDocument(&args);
    } else {
        logf("PdfDecryptDoIt: pdfclean_main failed with %d, src: '%s', password len: %d\n", res, dlg->srcPath,
             (int)str::Len(dlg->password));
        MessageBoxWarning(dlg->hwnd, _TRA("Failed to decrypt PDF file."), _TRA("Decrypt PDF"));
    }
}

static LRESULT CALLBACK PdfDecryptDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PdfDecryptDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        dlg = (PdfDecryptDialog*)cs->lpCreateParams;
        dlg->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (PdfDecryptDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_COMMAND: {
            int code = HIWORD(wp);
            HWND ctl = (HWND)lp;
            if (ctl == dlg->hwndBrowseBtn && code == BN_CLICKED) {
                PdfDecryptOnBrowse(dlg);
                return 0;
            }
            if (ctl == dlg->hwndDecryptBtn && code == BN_CLICKED) {
                PdfDecryptDoIt(dlg);
                return 0;
            }
            if (ctl == dlg->hwndCancelBtn && code == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DPICHANGED:
            PdfToolHandleDpi(hwnd, &dlg->dpi, &dlg->hFont, &dlg->fontOwned, dlg->dpiReady, wp, lp);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (dlg->fontOwned && dlg->hFont) {
                DeleteObject(dlg->hFont);
                dlg->hFont = nullptr;
            }
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kPdfDecryptWinClassName = L"SUMATRA_PDF_DECRYPT";
static bool gPdfDecryptWinClassRegistered = false;

void ShowPdfDecryptDialog(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        return;
    }
    if (!CouldBePDFDoc(tab)) {
        return;
    }
    EngineBase* engine = tab->GetEngine();
    if (!EngineMupdfIsEncrypted(engine)) {
        logf("ShowPdfDecryptDialog: '%s' is not encrypted, skipping\n", tab->filePath);
        return;
    }
    const char* pwd = EngineMupdfGetPassword(engine);
    if (str::IsEmpty(pwd)) {
        logf("ShowPdfDecryptDialog: '%s' is encrypted but no password available\n", tab->filePath);
        return;
    }
    logf("ShowPdfDecryptDialog: opening for '%s', password len: %d\n", tab->filePath, (int)str::Len(pwd));

    if (!gPdfDecryptWinClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PdfDecryptDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPdfDecryptWinClassName;
        RegisterClassExW(&wc);
        gPdfDecryptWinClassRegistered = true;
    }

    PdfDecryptDialog* dlg = new PdfDecryptDialog();
    dlg->srcPath = str::Dup(tab->filePath);
    dlg->password = str::Dup(pwd);
    dlg->win = win;
    dlg->dpi = DpiGet(win->hwndFrame);
    if (dlg->dpi < 72) {
        dlg->dpi = 96;
    }
    dlg->hFont = CreatePdfToolFont(dlg->dpi);
    dlg->fontOwned = dlg->hFont != nullptr;
    if (!dlg->hFont) {
        dlg->hFont = GetDefaultGuiFont();
    }

    DlgMetrics m = GetDlgMetrics(win->hwndFrame, dlg->hFont);
    int minW = DpiScale(win->hwndFrame, 500);
    int dlgW = CalcDlgWidth(win->hwndFrame, dlg->hFont, tab->filePath, minW, m.padding);
    int dlgH = CalcDlgHeight(win->hwndFrame, m, 3);

    HINSTANCE h = GetModuleHandleW(nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPdfDecryptWinClassName, _TRW("Decrypt PDF"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                dlgW, dlgH, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        str::Free(dlg->srcPath);
        str::Free(dlg->password);
        delete dlg;
        return;
    }

    int x = m.padding;
    int y = m.padding;
    int w = dlgW - 2 * m.padding - DpiScale(hwnd, 16);

    // row 1: source path label (offset to align with text inside edit control)
    dlg->hwndPathLabel =
        CreateWindowExW(0, L"STATIC", ToWStrTemp(tab->filePath), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                        x + m.editXOff, y, w - m.editXOff, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPathLabel, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 2: dest edit + browse button
    TempStr destPath = MakeUniqueFilePathTemp(tab->filePath);
    dlg->hwndDestEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(destPath), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y,
                        w - m.browseW - m.btnGap, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDestEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x + w - m.browseW,
                                         y, m.browseW, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndBrowseBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    y += m.rowH + m.rowGap;

    // row 3: Decrypt PDF + Cancel buttons (right-aligned)
    int bx = x + w - m.btnW;
    dlg->hwndCancelBtn = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx, y,
                                         m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndCancelBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    bx -= m.btnW + m.btnGap;
    dlg->hwndDecryptBtn = CreateWindowExW(0, L"BUTTON", _TRW("Decrypt PDF"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                          bx, y, m.btnW, m.btnH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndDecryptBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    CenterDialog(hwnd, win->hwndFrame);
    if (UseDarkModeLib()) {
        DarkMode::setDarkWndSafe(hwnd);
        DarkMode::setWindowEraseBgSubclass(hwnd);
    }
    UpdateWindowCaptionTheme(hwnd);
    dlg->dpiReady = true;
    ShowWindow(hwnd, SW_SHOW);
}
