/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include <uxtheme.h>
#include <vsstyle.h>

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
#include "FloatingPopupStyle.h"
#include "AppDialogTheme.h"
#include "AppSettings.h"
#include "CaptionGlyphs.h"
#include "PdfPageAdjustment.h"
#include "PdfAdjustmentGeometry.h"
#include "PdfAdjustmentTrackbar.h"
#include "PdfDarkMode.h"
#include "RenderCache.h"
#include "SvgIcons.h"
#include "Toolbar.h"
#include "EditAnnotations.h"

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
    idRotateMinimize = 123,
    idRotateMaximize = 124,
    idRotateClose = 125,
    idRotateShowGrid = 126,
    idRotateGridDensity = 127,
    idRotateGridMode = 128,
    idRotateAngleSpin = 129,
    idRotateManualCrop = 130,
    idRotateCropRatio = 131,
    idRotateCropOverride = 132,
    idRotatePreviewPrev = 133,
    idRotatePreviewNext = 134,
    idRotatePreviewPage = 135,
    idRotateApply = IDOK,
    idRotateCancel = IDCANCEL,
};

struct PdfRotatePagesDialog {
    HWND hwnd = nullptr;
    bool applying = false;
    HWND hwndApplyToLabel = nullptr;
    HWND hwndCurrentPage = nullptr;
    HWND hwndAllPages = nullptr;
    HWND hwndSpecified = nullptr;
    HWND hwndPagesEdit = nullptr;
    HWND hwndAny = nullptr;
    HWND hwndSlider = nullptr;
    HWND hwndAngleEdit = nullptr;
    HWND hwndAngleMinus = nullptr;
    HWND hwndAnglePlus = nullptr;
    HWND hwndAngleSpin = nullptr;
    HWND hwndManualCrop = nullptr;
    HWND hwndCropRatio = nullptr;
    HWND hwndCropOverride = nullptr;
    HWND hwndPreviewPrev = nullptr;
    HWND hwndPreviewNext = nullptr;
    HWND hwndPreviewPage = nullptr;
    HWND hwndPreviewStatus = nullptr;
    PdfRotateCropState sharedCrop;
    Vec<PdfRotateCropState> cropOverrides;
    int originalPageNo = 1;
    int previewCachedPage = 0;
    bool cropUiSync = false;
    HWND hwndZoomIn = nullptr;
    HWND hwndZoomOut = nullptr;
    HWND hwndMinimize = nullptr;
    HWND hwndMaximize = nullptr;
    HWND hwndClose = nullptr;
    HWND hwndSidebarScroll = nullptr;
    HWND hwndSidebarPanel = nullptr;
    RECT previewPageRc{};
    HWND hwndGridTitle = nullptr;
    HWND hwndShowGrid = nullptr;
    HWND hwndGridDensity = nullptr;
    HWND hwndGridMode = nullptr;
    HWND hwndRotationTitle = nullptr;
    HWND hwndFlipTitle = nullptr;
    HWND hwndAngleHint = nullptr;
    Tooltip* tooltip = nullptr;
    HBITMAP iconCache[8][8]{};
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
    bool crossGrid = true;
    int gridDensity = 1;
    WindowTab* sourceTab = nullptr;
    EngineBase* sourceEngine = nullptr;
    u32 sourcePageStructureVersion = 0;
    HWND holdButton = nullptr;
    int holdDirection = 0;
    ULONGLONG holdStart = 0;
    double holdBase = 0;
    int separatorY[3]{};
    int angleTickX[3]{};
    bool syncing = false;
    RECT previewRc{};
    Gdiplus::Bitmap* gdiPreview = nullptr;
    int previewRenderSide = 0;
    int previewColorEpoch = -1;
    RectF previewSourceBox{};
    float previewDeskew = 0;
    RECT angleGroupRc{};
    Size minWindowSize;
    RECT sidebarRc{};
    int sidebarPos = 0;
    int sidebarMax = 0;
    int wheelRemainder = 0;
    float previewZoom = 1.f;
    float panX = 0;
    float panY = 0;
    int cropDrag = 0;
    RectF cropBefore{};
    bool manualCrop = false;
    RectF crop{0, 0, 1, 1};
    Gdiplus::RectF previewOutput{};
    bool dragging = false;
    POINT dragStart{};
    DoubleBuffer* paintBuffer = nullptr;
    Size paintBufferSize;
};

static float PdfRotateReadAngle(PdfRotatePagesDialog* dlg) {
    return dlg ? (float)dlg->adjustment.TotalAngle() : 0;
}

static bool PdfRotateSourceIsCurrent(PdfRotatePagesDialog* dlg) {
    return dlg && IsMainWindowValid(dlg->win) && dlg->win->CurrentTab() == dlg->sourceTab &&
           dlg->sourceTab->GetEngine() == dlg->sourceEngine &&
           EngineMupdfPageStructureVersion(dlg->sourceEngine) == dlg->sourcePageStructureVersion;
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

static PdfRotateCropState* PdfRotateCropForPage(PdfRotatePagesDialog* dlg, int pageNo) {
    return PdfAdjustmentCropForPage(&dlg->sharedCrop, dlg->cropOverrides.begin(), dlg->cropOverrides.Size(), pageNo);
}

static bool PdfRotatePageSize(PdfRotatePagesDialog* dlg, int pageNo, float* w, float* h) {
    if (!PdfRotateSourceIsCurrent(dlg)) return false;
    RectF box{};
    if (!EngineMupdfGetPageAdjustmentSource(dlg->sourceEngine, pageNo, &box)) return false;
    PdfAdjustmentOutputSize(box.dx, box.dy, PdfRotateReadAngle(dlg),
                            EngineMupdfGetPageDeskewDeg(dlg->sourceEngine, pageNo), PdfRotateFlipH(dlg),
                            PdfRotateFlipV(dlg), PdfRotateAutoCrop(dlg), w, h);
    return *w >= 1 && *h >= 1;
}

static void PdfRotateSyncCrop(PdfRotatePagesDialog* dlg) {
    float w, h;
    if (!PdfRotatePageSize(dlg, dlg->currentPageNo, &w, &h)) return;
    auto* c = PdfRotateCropForPage(dlg, dlg->currentPageNo);
    if (!c->initialized) {
        if (!dlg->manualCrop) return;
        c->box = {0, 0, w, h};
        c->initialized = true;
    }
    dlg->crop = {c->box.x / w, c->box.y / h, c->box.w / w, c->box.h / h};
    dlg->cropUiSync = true;
    SendMessageW(dlg->hwndCropRatio, CB_SETCURSEL, c->ratioMode, 0);
    SendMessageW(dlg->hwndCropOverride, BM_SETCHECK, c->pageNo ? BST_CHECKED : BST_UNCHECKED, 0);
    dlg->cropUiSync = false;
}

static bool PdfRotateHasManualCrop(PdfRotatePagesDialog* dlg) {
    return dlg->manualCrop && dlg->sharedCrop.initialized;
}

static bool PdfRotateTargetPages(PdfRotatePagesDialog* dlg, Vec<int>& pages) {
    if (SendMessageW(dlg->hwndSpecified, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        char text[256]{};
        GetWindowTextA(dlg->hwndPagesEdit, text, dimof(text));
        return ParseDeletePages(text, dlg->pageCount, pages) && pages.Size() > 0;
    }
    if (SendMessageW(dlg->hwndAllPages, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        for (int i = 1; i <= dlg->pageCount; i++) pages.Append(i);
    } else
        pages.Append(dlg->originalPageNo);
    return true;
}

// Share hit testing between hover feedback and the actual drag operation.
static int PdfRotateCropHitTest(PdfRotatePagesDialog* dlg, POINT pt) {
    auto o = dlg->previewOutput;
    if (o.Width <= 0 || o.Height <= 0) return 32;
    float nx = (pt.x - o.X) / o.Width, ny = (pt.y - o.Y) / o.Height;
    float tx = DpiScale(dlg->hwnd, 8) / o.Width, ty = DpiScale(dlg->hwnd, 8) / o.Height;
    auto c = dlg->crop;
    int hit = 0;
    if (ny >= c.y - ty && ny <= c.y + c.dy + ty) {
        if (fabsf(nx - c.x) < tx) hit |= 1;
        if (fabsf(nx - c.x - c.dx) < tx) hit |= 2;
    }
    if (nx >= c.x - tx && nx <= c.x + c.dx + tx) {
        if (fabsf(ny - c.y) < ty) hit |= 4;
        if (fabsf(ny - c.y - c.dy) < ty) hit |= 8;
    }
    if (hit) return hit;
    if ((c.dx < .999f || c.dy < .999f) && nx > c.x && nx < c.x + c.dx && ny > c.y && ny < c.y + c.dy) return 16;
    return 32;
}

static LPCWSTR PdfRotateCropCursor(int hit) {
    switch (hit) {
        case 1:
        case 2:
            return IDC_SIZEWE;
        case 4:
        case 8:
            return IDC_SIZENS;
        case 5:
        case 10:
            return IDC_SIZENWSE;
        case 6:
        case 9:
            return IDC_SIZENESW;
        case 16:
            return IDC_SIZEALL;
        default:
            return IDC_CROSS;
    }
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
    HwndSetText(dlg->hwndAngleHint, "");
    InvalidateRect(dlg->hwndAngleHint, nullptr, FALSE);
    PdfRotateInvalidatePreview(dlg);
}

static void PdfRotateLoadPreview(PdfRotatePagesDialog* dlg) {
    if (!dlg) {
        return;
    }
    int renderSide = std::max(
        900, (int)std::max(dlg->previewRc.right - dlg->previewRc.left, dlg->previewRc.bottom - dlg->previewRc.top));
    renderSide = std::min((int)(renderSide * dlg->previewZoom), 4096);
    int colorEpoch = gRenderCache ? gRenderCache->darkModeEpoch : ThemeEpoch();
    if (dlg->gdiPreview && dlg->previewCachedPage == dlg->currentPageNo && dlg->previewRenderSide >= renderSide &&
        dlg->previewColorEpoch == colorEpoch) {
        return;
    }
    delete dlg->gdiPreview;
    dlg->gdiPreview = nullptr;
    dlg->previewOutput = {};
    dlg->previewCachedPage = 0;
    if (!PdfRotateSourceIsCurrent(dlg)) return;
    WindowTab* tab = dlg->sourceTab;
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    RectF sourceBox{};
    if (!EngineMupdfGetPageAdjustmentSource(engine, dlg->currentPageNo, &sourceBox)) return;
    RectF mb = engine->PageMediabox(dlg->currentPageNo);
    float side = mb.dx > mb.dy ? mb.dx : mb.dy;
    if (side < 1.f) {
        side = 1.f;
    }
    float zoom = (float)renderSide / side;
    RenderPageArgs args(dlg->currentPageNo, zoom, 0, nullptr, RenderTarget::View);
    args.useDeskewOverride = true;
    args.deskewDegOverride = 0;
    DarkModeProfile profile;
    BuildViewDarkModeProfile(engine, &profile);
    if (profile.mode != PageColorMode::Normal) args.darkProfile = &profile;
    RenderedBitmap* rendered = engine->RenderPage(args);
    if (!rendered || !rendered->IsValid()) {
        delete rendered;
        return;
    }
    ApplyRenderThemePostColors(engine, rendered, dlg->currentPageNo, zoom, nullptr, args.darkProfile);
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
    dlg->previewCachedPage = dlg->currentPageNo;
    dlg->previewRenderSide = renderSide;
    dlg->previewColorEpoch = colorEpoch;
    dlg->previewSourceBox = sourceBox;
    dlg->previewDeskew = EngineMupdfGetPageDeskewDeg(engine, dlg->currentPageNo);
}

static COLORREF PdfRotateWorkspaceColor() {
    COLORREF paper = 0;
    ThemePageRenderColors(paper, true);
    // Scale RGB directly: AccentColor adapts its sign to the input luminance.
    // The canvas must always be quieter and darker than the current paper.
    int factor = GetRValue(paper) + GetGValue(paper) + GetBValue(paper) > 384 ? 84 : 65;
    return RGB(GetRValue(paper) * factor / 100, GetGValue(paper) * factor / 100, GetBValue(paper) * factor / 100);
}

static void PdfRotatePaintPreview(PdfRotatePagesDialog* dlg, HDC hdc, POINT origin = {}) {
    RECT rc = dlg->previewRc;
    int rw = rc.right - rc.left;
    int rh = rc.bottom - rc.top;
    if (rw < 4 || rh < 4) {
        return;
    }
    Gdiplus::Graphics g(hdc);
    g.TranslateTransform((float)-origin.x, (float)-origin.y);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.SetClip(Gdiplus::Rect(rc.left, rc.top, rw, rh));
    COLORREF workspace = PdfRotateWorkspaceColor();
    Gdiplus::SolidBrush bg(Gdiplus::Color(255, GetRValue(workspace), GetGValue(workspace), GetBValue(workspace)));
    g.FillRectangle(&bg, rc.left, rc.top, rw, rh);
    if (dlg->gdiPreview) {
        float w = dlg->previewSourceBox.dx;
        float h = dlg->previewSourceBox.dy;
        if (w < 1 || h < 1) return;
        float angle = PdfRotateReadAngle(dlg);
        bool flipH = PdfRotateFlipH(dlg), flipV = PdfRotateFlipV(dlg);
        float outW, outH;
        PdfAdjustmentOutputSize(w, h, angle, dlg->previewDeskew, flipH, flipV, PdfRotateAutoCrop(dlg), &outW, &outH);
        int margin = DpiScale(dlg->hwnd, 20);
        float scale = std::max(0.01f, std::min((rw - 2.f * margin) / w, (rh - 2.f * margin) / h));
        scale *= dlg->previewZoom;
        float cx = (rc.left + rc.right) * 0.5f + dlg->panX;
        float cy = (rc.top + rc.bottom) * 0.5f + dlg->panY;
        Gdiplus::RectF output(cx - outW * scale * .5f, cy - outH * scale * .5f, outW * scale, outH * scale);
        dlg->previewOutput = output;
        COLORREF paper = 0;
        ThemePageRenderColors(paper, true);
        Gdiplus::SolidBrush pagePaper(Gdiplus::Color(255, GetRValue(paper), GetGValue(paper), GetBValue(paper)));
        g.FillRectangle(&pagePaper, output);
        g.SetClip(output, Gdiplus::CombineModeIntersect);
        // Apply order: session deskew, flips, then pending clockwise turn.
        g.TranslateTransform(cx, cy);
        g.RotateTransform(angle);
        g.ScaleTransform(flipH ? -1.f : 1.f, flipV ? -1.f : 1.f);
        g.RotateTransform(dlg->previewDeskew);
        g.TranslateTransform(-w * scale * .5f, -h * scale * .5f);
        g.DrawImage(dlg->gdiPreview, 0.f, 0.f, w * scale, h * scale);
        g.ResetTransform();
        g.TranslateTransform((float)-origin.x, (float)-origin.y);
        g.ResetClip();
        g.SetClip(Gdiplus::Rect(rc.left, rc.top, rw, rh));
        COLORREF edge = ThemeWindowTextDisabledColor();
        Gdiplus::Pen pageEdge(Gdiplus::Color(155, GetRValue(edge), GetGValue(edge), GetBValue(edge)), 1.f);
        g.DrawRectangle(&pageEdge, output);
    }
    if (dlg->manualCrop && dlg->previewOutput.Width > 0) {
        auto o = dlg->previewOutput;
        PdfAdjustmentCropBounds mapped{};
        mapped = {o.X + dlg->crop.x * o.Width, o.Y + dlg->crop.y * o.Height, dlg->crop.dx * o.Width,
                  dlg->crop.dy * o.Height};
        Gdiplus::RectF c(mapped.x, mapped.y, mapped.w, mapped.h);
        Gdiplus::SolidBrush shade(Gdiplus::Color(110, 0, 0, 0));
        Gdiplus::Region outside(o);
        outside.Exclude(c);
        g.FillRegion(&shade, &outside);
        float pw, ph;
        bool fits = PdfRotatePageSize(dlg, dlg->currentPageNo, &pw, &ph) &&
                    PdfAdjustmentCropFits(PdfRotateCropForPage(dlg, dlg->currentPageNo)->box, pw, ph);
        Gdiplus::Pen border(fits ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 220, 55, 45), 1.f);
        g.DrawRectangle(&border, c);
        Gdiplus::SolidBrush handle(Gdiplus::Color(255, 255, 255, 255));
        float size = (float)DpiScale(dlg->hwnd, 6);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                if (i == 1 && j == 1) continue;
                g.FillRectangle(&handle, c.X + c.Width * i / 2 - size / 2, c.Y + c.Height * j / 2 - size / 2, size,
                                size);
            }
    }
    if (!dlg->showGrid) return;
    // A dark underlay and lighter theme-colored hairline remain visible both
    // over white paper and over the dark workspace. Grid coordinates never
    // inherit the page transform or the user's pan offset.
    COLORREF gridInk = ThemeWindowTextDisabledColor();
    Gdiplus::Pen grid(Gdiplus::Color(55, GetRValue(gridInk), GetGValue(gridInk), GetBValue(gridInk)), 1.f);
    Gdiplus::Pen majorGrid(Gdiplus::Color(90, GetRValue(gridInk), GetGValue(gridInk), GetBValue(gridInk)), 1.f);
    Gdiplus::Pen underlay(Gdiplus::Color(20, 0, 0, 0), 1.f);
    Gdiplus::Pen verticalGrid(Gdiplus::Color(38, GetRValue(gridInk), GetGValue(gridInk), GetBValue(gridInk)), 1.f);
    Gdiplus::Pen verticalMajor(Gdiplus::Color(65, GetRValue(gridInk), GetGValue(gridInk), GetBValue(gridInk)), 1.f);
    const int spacing[] = {32, 20, 12};
    int step = DpiScale(dlg->hwnd, spacing[dlg->gridDensity]);
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    for (int x = rc.left; dlg->crossGrid && x < rc.right; x += step) {
        g.DrawLine(&underlay, (float)x + 1, (float)rc.top, (float)x + 1, (float)rc.bottom);
        g.DrawLine((x - rc.left) / step % 5 == 0 ? &verticalMajor : &verticalGrid, (float)x, (float)rc.top, (float)x,
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
static void PdfRotateNavigate(PdfRotatePagesDialog* dlg, int step, int requested = 0) {
    bool continueIndividual = dlg->manualCrop && SendMessageW(dlg->hwndCropOverride, BM_GETCHECK, 0, 0) == BST_CHECKED;
    Vec<int> pages;
    if (!PdfRotateTargetPages(dlg, pages)) return;
    int index = -1;
    for (int i = 0; i < pages.Size(); i++)
        if (pages[i] == dlg->currentPageNo) index = i;
    int next = requested;
    if (!requested) next = pages[limitValue(index < 0 ? 0 : index + step, 0, pages.Size() - 1)];
    bool found = false;
    for (int p : pages)
        if (p == next) found = true;
    if (!found) return;
    // Carry the current adjustment forward when creating a per-page crop.
    // Capture it before navigation switches the preview and current page.
    auto previousCrop = *PdfRotateCropForPage(dlg, dlg->currentPageNo);
    if (next != dlg->currentPageNo) {
        if (dlg->dragging) {
            dlg->dragging = false;
            ReleaseCapture();
        }
        dlg->currentPageNo = next;
        dlg->panX = dlg->panY = 0;
        PdfRotateLoadPreview(dlg);
    }
    if (continueIndividual && dlg->gdiPreview) {
        auto copy = *PdfRotateCropForPage(dlg, dlg->currentPageNo);
        if (!copy.pageNo) {
            copy = previousCrop;
            copy.pageNo = dlg->currentPageNo;
            dlg->cropOverrides.Append(copy);
        }
    }
    PdfRotateSyncCrop(dlg);
    PdfRotateInvalidatePreview(dlg);
}

static void PdfRotatePagesUpdateUi(PdfRotatePagesDialog* dlg) {
    Vec<int> targets;
    bool rangeValid = PdfRotateTargetPages(dlg, targets);
    bool specified = SendMessageW(dlg->hwndSpecified, BM_GETCHECK, 0, 0) == BST_CHECKED;
    char rangeText[256]{};
    GetWindowTextA(dlg->hwndPagesEdit, rangeText, dimof(rangeText));
    str::TrimWSInPlace(rangeText, str::TrimOpt::Both);
    bool awaitingRange = specified && str::IsEmpty(rangeText);
    int previewIndex = -1;
    for (int i = 0; i < targets.Size(); i++)
        if (targets[i] == dlg->currentPageNo) previewIndex = i;
    if (rangeValid && previewIndex < 0) {
        PdfRotateNavigate(dlg, 0);
        previewIndex = 0;
    }
    PdfRotateSyncCrop(dlg);
    EnableWindow(dlg->hwndPreviewPrev, rangeValid && previewIndex > 0);
    EnableWindow(dlg->hwndPreviewNext, rangeValid && previewIndex + 1 < targets.Size());
    EnableWindow(dlg->hwndPreviewPage, rangeValid);
    EnableWindow(dlg->hwndCropRatio, dlg->manualCrop && dlg->gdiPreview);
    EnableWindow(dlg->hwndCropOverride, dlg->manualCrop && dlg->gdiPreview && rangeValid && targets.Size() > 1);
    dlg->cropUiSync = true;
    if (GetFocus() != dlg->hwndPreviewPage)
        SetWindowTextA(dlg->hwndPreviewPage, str::FormatTemp("%d", dlg->currentPageNo));
    const char* stateText = targets.Size() == 1                                     ? _TRA("Single page")
                            : !dlg->manualCrop                                      ? _TRA("Preview")
                            : PdfRotateCropForPage(dlg, dlg->currentPageNo)->pageNo ? _TRA("This page only")
                                                                                    : _TRA("Shared crop");
    float pw, ph;
    bool fits = !dlg->manualCrop || (PdfRotatePageSize(dlg, dlg->currentPageNo, &pw, &ph) &&
                                     PdfAdjustmentCropFits(PdfRotateCropForPage(dlg, dlg->currentPageNo)->box, pw, ph));
    const char* status = awaitingRange ? ""
                         : !rangeValid ? _TRA("Invalid page range.")
                         : !dlg->gdiPreview
                             ? _TRA("Unable to load page.")
                             : str::FormatTemp(_TRA("Preview  %d / %d   -   %s%s"), previewIndex + 1, targets.Size(),
                                               stateText, fits ? "" : _TRA(" - Crop outside page"));
    HwndSetText(dlg->hwndPreviewStatus, status);
    dlg->cropUiSync = false;

    EnableWindow(dlg->hwndPagesEdit, specified);
    bool valid = true;
    if (specified) {
        char pages[256]{};
        GetWindowTextA(dlg->hwndPagesEdit, pages, dimof(pages) - 1);
        Vec<int> parsedPages;
        valid = ParseDeletePages(pages, dlg->pageCount, parsedPages);
    }
    float ang = PdfRotateReadAngle(dlg);
    bool trivial = fabsf(ang) < 0.00001f && fabsf(dlg->previewDeskew) < 0.00001f && !PdfRotateFlipH(dlg) &&
                   !PdfRotateFlipV(dlg) && !PdfRotateHasManualCrop(dlg);
    EnableWindow(dlg->hwndRotateBtn,
                 rangeValid && valid && dlg->angleValid && !trivial && PdfRotateSourceIsCurrent(dlg));
}

static void PdfRotateCloseDialog(PdfRotatePagesDialog* dlg) {
    HWND hwnd = dlg->hwnd;
    HWND owner = IsMainWindowValid(dlg->win) ? dlg->win->hwndFrame : nullptr;
    HWND canvas = owner ? dlg->win->hwndCanvas : nullptr;
    bool restoreFocus = GetForegroundWindow() == hwnd && owner && IsWindowVisible(owner) && IsWindowEnabled(owner);
    // Activate the owner before destroying its active popup, so Windows does not
    // choose another application's window as the next foreground window.
    if (restoreFocus) {
        SetForegroundWindow(owner);
        SetActiveWindow(owner);
    }
    DestroyWindow(hwnd);
    if (restoreFocus && GetForegroundWindow() == owner && IsWindow(canvas)) {
        SetFocus(canvas);
    }
}

struct PdfRotateApplyJob {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    bool ownerWasEnabled = false;
    bool wasPaused = false;
    MainWindow* win = nullptr;
    WindowTab* tab = nullptr;
    DisplayModel* dm = nullptr;
    EngineBase* engine = nullptr;
    Vec<int> pageNos;
    Vec<RectF> crops;
    ScrollState scroll;
    float angle = 0;
    bool flipH = false, flipV = false, autoCrop = false, manualCrop = false;
    int changed = 0;
    ~PdfRotateApplyJob() { engine->Release(); }
};

static void PdfRotateApplyFinished(PdfRotateApplyJob* job) {
    AutoDelete cleanup(job);
    if (!IsMainWindowValid(job->win)) return;
    MainWindow* win = job->win;
    WindowTab* tab = win->CurrentTab();
    if (tab != job->tab || tab->AsFixed() != job->dm) return;
    DisplayModel* dm = job->dm;
    EngineBase* engine = job->engine;
    auto* dlg = IsWindow(job->hwnd) ? (PdfRotatePagesDialog*)GetWindowLongPtrW(job->hwnd, GWLP_USERDATA) : nullptr;
    dm->pauseRendering = job->wasPaused;
    if (job->ownerWasEnabled) EnableWindow(job->owner, TRUE);
    if (dlg) {
        dlg->applying = false;
        EnableWindow(job->hwnd, TRUE);
    }
    if (!job->changed) {
        if (dlg) {
            PdfRotatePagesUpdateUi(dlg);
            MessageBoxWarning(job->hwnd, _TRA("Nothing to change."), _TRA("Manually Adjust Pages"));
        }
        dm->RenderVisibleParts();
        return;
    }
    // Keep the PDF changes in the live engine. The normal save/close pipeline
    // owns writing them to disk and offers Save / Discard / Cancel on close.
    EngineMupdfSetPdfTocModified(engine, true);
    tab->askedToSaveAnnotations = false;
    if (gRenderCache) {
        gRenderCache->FreeForDisplayModel(dm);
    }
    for (int pageNo : job->pageNos) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        if (pi) {
            pi->state = PageInfoState::Unknown;
            pi->_mediaBox = {};
            pi->contentBox = {};
            pi->failedToRender = false;
            pi->renderFailCount = 0;
        }
    }
    dm->Relayout(dm->GetZoomVirtual(), dm->GetRotation());
    dm->SetScrollState(job->scroll);
    if (dlg) PdfRotateCloseDialog(dlg);
    win->RedrawAll(true);
    ToolbarUpdateStateForWindow(win, false);
}

static void PdfRotateApplyWorker(PdfRotateApplyJob* job) {
    if (gRenderCache) gRenderCache->WaitForRenderingComplete(job->dm);
    for (int i = 0; i < job->pageNos.Size(); i++) {
        if (EngineMupdfAdjustPageView(job->engine, job->pageNos[i], job->angle, job->flipH, job->flipV, job->autoCrop,
                                      job->manualCrop ? &job->crops[i] : nullptr)) {
            job->changed++;
        }
    }
    uitask::Post(MkFunc0<PdfRotateApplyJob>(PdfRotateApplyFinished, job), "PdfRotateApplyFinished");
}

static void PdfRotatePagesDoIt(PdfRotatePagesDialog* dlg) {
    if (dlg->applying) return;
    if (!dlg->angleValid || !PdfRotateSourceIsCurrent(dlg)) {
        MessageBoxWarning(dlg->hwnd, _TRA("Check the angle and reopen this dialog if the document has changed."),
                          _TRA("Manually Adjust Pages"));
        return;
    }
    Vec<int> pageNos;
    if (!PdfRotateTargetPages(dlg, pageNos)) {
        MessageBoxWarning(dlg->hwnd, _TRA("Invalid page range."), _TRA("Manually Adjust Pages"));
        return;
    }
    Vec<RectF> crops;
    StrBuilder badPages;
    for (int pageNo : pageNos) {
        float w, h;
        if (!PdfRotatePageSize(dlg, pageNo, &w, &h)) {
            MessageBoxWarning(dlg->hwnd, _TRA("Unable to load page."), _TRA("Manually Adjust Pages"));
            return;
        }
        auto box = PdfRotateCropForPage(dlg, pageNo)->box;
        if (PdfRotateHasManualCrop(dlg) && !PdfAdjustmentCropFits(box, w, h)) {
            if (badPages.Size()) badPages.Append(", ");
            badPages.Append(str::FormatTemp("%d", pageNo));
        }
        crops.Append(RectF(box.x / w, box.y / h, box.w / w, box.h / h));
    }
    if (badPages.Size()) {
        MessageBoxWarning(dlg->hwnd, str::FormatTemp(_TRA("Crop area outside page: %s"), badPages.Get()),
                          _TRA("Manually Adjust Pages"));
        return;
    }

    float ang = PdfRotateReadAngle(dlg);
    bool flipH = PdfRotateFlipH(dlg);
    bool flipV = PdfRotateFlipV(dlg);
    bool autoCrop = PdfRotateAutoCrop(dlg);
    if (fabsf(ang) < 0.00001f && fabsf(dlg->previewDeskew) < 0.00001f && !flipH && !flipV &&
        !PdfRotateHasManualCrop(dlg)) {
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

    // UI selections and the editor also borrow the page's annotation wrappers.
    // Commit any in-place text, then release these references before cache reset.
    EndFreeTextInPlaceEditForTab(tab, true);
    CloseAndDeleteEditAnnotationsWindow(tab);
    tab->selectedAnnotation = nullptr;
    tab->didScrollToSelectedAnnotation = false;
    ClearPdfMarkupOverlays(tab);
    win->DeleteToolTip();

    auto* job = new PdfRotateApplyJob;
    job->hwnd = dlg->hwnd;
    job->owner = win->hwndFrame;
    job->ownerWasEnabled = IsWindowEnabled(job->owner);
    job->win = win;
    job->tab = tab;
    job->dm = dm;
    job->engine = engine;
    engine->AddRef();
    job->pageNos = pageNos;
    job->crops = crops;
    job->scroll = dm->GetScrollState();
    job->angle = ang;
    job->flipH = flipH;
    job->flipV = flipV;
    job->autoCrop = autoCrop;
    job->manualCrop = PdfRotateHasManualCrop(dlg);
    job->wasPaused = dm->pauseRendering;
    dm->pauseRendering = true;
    dlg->applying = true;
    HwndSetText(dlg->hwndPreviewStatus, _TRA("Applying changes…"));
    EnableWindow(job->owner, FALSE);
    EnableWindow(dlg->hwnd, FALSE);
    if (gRenderCache) gRenderCache->CancelRendering(dm);
    HANDLE worker = StartThread(MkFunc0<PdfRotateApplyJob>(PdfRotateApplyWorker, job), "PdfRotateApply");
    if (worker) {
        CloseHandle(worker);
    } else {
        PdfRotateApplyFinished(job);
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
static LRESULT CALLBACK PdfRotateSidebarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* dlg = (PdfRotatePagesDialog*)data;
    switch (msg) {
        case WM_COMMAND:
        case WM_NOTIFY:
        case WM_DRAWITEM:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX:
        case WM_MOUSEWHEEL:
            return SendMessageW(dlg->hwnd, msg, wp, lp);
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, dlg->bgBrush);
            HPEN pen =
                CreatePen(PS_SOLID, 1, AccentColor(AppDialogPanelBackgroundColor(), ThemeUsesDarkChrome() ? 18 : -18));
            HGDIOBJ old = SelectObject(dc, pen);
            for (int y : dlg->separatorY) {
                y -= dlg->sidebarRc.top;
                MoveToEx(dc, 0, y, nullptr);
                LineTo(dc, rc.right, y);
            }
            SelectObject(dc, old);
            DeleteObject(pen);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, PdfRotateSidebarProc, id);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static Size PdfRotatePagesLayout(PdfRotatePagesDialog* dlg, int dpi, bool fitClient = false) {
    if (!dlg || !dlg->hwnd) return {};
    HWND hwnd = dlg->hwnd;
    int pad = PdfRotatePx(16, dpi);
    int gap = PdfRotatePx(5, dpi);
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
    dlg->previewRc = {pad, top, x - pad, clientH - pad - rowH - gap};
    int navY = clientH - pad - rowH;
    int navW = PdfRotatePx(32, dpi), pageW = PdfRotatePx(52, dpi);
    int navGap = PdfRotatePx(8, dpi);
    int navH = HwndMeasureText(hwnd, "Xg", dlg->hFont).dy + PdfRotatePx(8, dpi);
    int controlY = navY + (rowH - navH) / 2;
    PdfRotatePagesMoveChild(dlg->hwndPreviewPrev, pad, controlY, navW, navH);
    dlg->previewPageRc = {pad + navW + navGap, controlY, pad + navW + navGap + pageW, controlY + navH};
    int fieldInset = PdfRotatePx(4, dpi);
    int textH = HwndMeasureText(hwnd, "Xg", dlg->hFont).dy;
    PdfRotatePagesMoveChild(dlg->hwndPreviewPage, pad + navW + navGap + fieldInset, controlY + (navH - textH) / 2,
                            pageW - fieldInset * 2, textH);
    PdfRotatePagesMoveChild(dlg->hwndPreviewNext, pad + navW + pageW + 2 * navGap, controlY, navW, navH);
    int statusX = pad + 2 * navW + pageW + 2 * navGap + PdfRotatePx(16, dpi);
    PdfRotatePagesMoveChild(dlg->hwndPreviewStatus, statusX, navY, std::max(1, x - pad - statusX), rowH);
    int tool = PdfRotatePx(32, dpi);
    PdfRotatePagesMoveChild(dlg->hwndZoomIn, pad + gap, top + gap, tool, tool);
    PdfRotatePagesMoveChild(dlg->hwndZoomOut, pad + gap * 2 + tool, top + gap, tool, tool);
    // Deskew stays fixed above the scrollable auxiliary settings.
    int fixedY = top;
    PdfRotatePagesMoveChild(dlg->hwndAny, x, fixedY, sideW, rowH);
    fixedY += rowH + gap;
    int editW = PdfRotatePx(82, dpi), spinW = PdfRotatePx(18, dpi);
    int groupX = x + sideW - editW - spinW;
    int editH = HwndMeasureText(hwnd, "Xg", dlg->hFont).dy + PdfRotatePx(6, dpi);
    PdfRotatePagesMoveChild(dlg->hwndSlider, x, fixedY, sideW - editW - spinW - gap, rowH);
    // The native track sits above the slider window's vertical midpoint.
    // Align the field and spinner to the actual track, including at higher DPI.
    RECT channel{};
    SendMessageW(dlg->hwndSlider, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
    int trackY = channel.bottom > channel.top ? (channel.top + channel.bottom) / 2 : rowH / 2;
    int editY = fixedY + trackY - editH / 2;
    dlg->angleGroupRc = {groupX, editY, groupX + editW, editY + editH};
    int inset = std::max(1, PdfRotatePx(2, dpi));
    PdfRotatePagesMoveChild(dlg->hwndAngleEdit, groupX + inset, editY + inset, editW - inset * 2, editH - inset * 2);
    PdfRotatePagesMoveChild(dlg->hwndAngleSpin, groupX + editW, editY, spinW, editH);
    ShowWindow(dlg->hwndAngleMinus, SW_HIDE);
    ShowWindow(dlg->hwndAnglePlus, SW_HIDE);
    ShowWindow(dlg->hwndDegLabel, SW_HIDE);
    fixedY += rowH + gap;
    int hintH = dlg->angleValid ? 0 : rowH;
    PdfRotatePagesMoveChild(dlg->hwndAngleHint, x, fixedY, sideW, hintH);
    top = fixedY + hintH + gap;
    int contentH = 14 * (rowH + gap) + gap * 3;
    int viewportH = std::max(1, footer - gap - top);
    dlg->sidebarRc = {x, top, x + sideW, top + viewportH};
    dlg->sidebarMax = std::max(0, contentH - viewportH);
    dlg->sidebarPos = limitValue(dlg->sidebarPos, 0, dlg->sidebarMax);
    int barW = PdfRotatePx(16, dpi);
    int w = sideW - (dlg->sidebarMax ? barW + gap : 0);
    int y = top - dlg->sidebarPos;
    PdfRotatePagesMoveChild(dlg->hwndSidebarPanel, x, top, w, viewportH);
    auto move = [&](HWND child, int px, int py, int width, int height) {
        SetWindowPos(child, nullptr, px - x, py - top, width,
                     child == dlg->hwndGridDensity || child == dlg->hwndGridMode || child == dlg->hwndCropRatio
                         ? PdfRotatePx(180, dpi)
                         : height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);
    };
    move(dlg->hwndGridTitle, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndShowGrid, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndGridMode, x, y, w / 2 - gap, rowH);
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
    move(dlg->hwndAutoCrop, x, y, w / 2, rowH);
    move(dlg->hwndManualCrop, x + w / 2, y, w - w / 2, rowH);
    y += rowH + gap;
    move(dlg->hwndCropRatio, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndCropOverride, x, y, w, rowH);
    y += rowH + gap * 2;
    dlg->separatorY[2] = y - gap;
    move(dlg->hwndApplyToLabel, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndCurrentPage, x, y, w, rowH);
    y += rowH + gap;
    move(dlg->hwndAllPages, x, y, w, rowH);
    y += rowH + gap;
    int specifiedW = std::max(PdfRotatePx(90, dpi), ButtonGetIdealSize(dlg->hwndSpecified).dx);
    move(dlg->hwndSpecified, x, y, specifiedW, rowH);
    move(dlg->hwndPagesEdit, x + specifiedW + gap, y, std::max(1, w - specifiedW - gap), rowH);
    // The range example is a cue inside the edit, so this row ends the content.
    contentH = y + rowH + gap - top + dlg->sidebarPos;
    dlg->sidebarMax = std::max(0, contentH - viewportH);
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
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    return PdfRotatePagesOuterSize(hwnd, dpi, clientW, clientH);
}

// theme-adaptive control colors: the brush returned here fills the control
// background, so statics/radios sit on the window background while the pages
// edit gets the recessed control background. darkmodelib subclasses (dark
// themes) intercept these messages first and fall through when disabled,
// so this is the single source of truth for the light themes.
static LRESULT PdfRotatePagesColorControl(PdfRotatePagesDialog* dlg, HDC dc, HWND control) {
    bool edit = control == dlg->hwndPreviewPage || control == dlg->hwndPagesEdit || control == dlg->hwndAngleEdit ||
                control == dlg->hwndDegLabel;
    if (control == dlg->hwndPreviewPage) {
        SetTextColor(dc, IsWindowEnabled(control) ? ThemeWindowTextColor() : ThemeWindowTextDisabledColor());
        SetBkColor(dc, ThemeChromeBackgroundColor());
        SetDCBrushColor(dc, ThemeChromeBackgroundColor());
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    COLORREF text = ThemeWindowTextColor();
    if (!IsWindowEnabled(control)) {
        text = ThemeWindowTextDisabledColor();
    }
    SetTextColor(dc, text);
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
    EnableWindow(dlg->hwndZoomIn, next < 16.f);
    EnableWindow(dlg->hwndZoomOut, next > .25f);
    PdfRotatePagesLayout(dlg, DpiGet(dlg->hwnd), true);
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
    if (msg == WM_ERASEBKGND) return 1;
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

static void PdfRotateDrawIconButtonContent(PdfRotatePagesDialog* dlg, DRAWITEMSTRUCT* item) {
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
    bool disabled = (item->itemState & ODS_DISABLED) != 0;
    COLORREF color = disabled ? ThemeWindowTextDisabledColor() : ThemeWindowTextColor();
    bool navigation = id == idRotatePreviewPrev || id == idRotatePreviewNext;
    COLORREF base = direction || navigation ? AppDialogPanelBackgroundColor() : PdfRotateWorkspaceColor();
    bool zoom = id == idRotateZoomIn || id == idRotateZoomOut;
    if (zoom) {
        // Render clean preview pixels directly; never copy a buffer that could
        // already contain an alpha-blended icon from a previous frame.
        POINT origin{};
        MapWindowPoints(item->hwndItem, dlg->hwnd, &origin, 1);
        HDC background = CreateCompatibleDC(item->hDC);
        HBITMAP pixels = CreateCompatibleBitmap(item->hDC, r.dx, r.dy);
        if (background && pixels) {
            HGDIOBJ old = SelectObject(background, pixels);
            PdfRotatePaintPreview(dlg, background, origin);
            BitBlt(item->hDC, 0, 0, r.dx, r.dy, background, 0, 0, SRCCOPY);
            SelectObject(background, old);
        }
        DeleteObject(pixels);
        DeleteDC(background);
    } else {
        HBRUSH clear = CreateSolidBrush(base);
        FillRect(item->hDC, &item->rcItem, clear);
        DeleteObject(clear);
    }
    if (!zoom && !disabled && (hot || pressed)) {
        HBRUSH brush = CreateSolidBrush(AccentColor(base, pressed ? 12 : 6));
        HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
        HGDIOBJ oldPen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
        int radius = DpiScale(dlg->hwnd, 6);
        RoundRect(item->hDC, r.x, r.y, r.Right(), r.Bottom(), radius, radius);
        SelectObject(item->hDC, oldBrush);
        SelectObject(item->hDC, oldPen);
        DeleteObject(brush);
    }
    int dpi = DpiGet(dlg->hwnd);
    if (dlg->iconCacheDpi != dpi || dlg->iconCacheEpoch != ThemeEpoch()) {
        PdfRotateClearIconCache(dlg);
        dlg->iconCacheDpi = dpi;
        dlg->iconCacheEpoch = ThemeEpoch();
    }
    int index = id == idRotateZoomIn        ? 0
                : id == idRotateZoomOut     ? 1
                : id == idRotateLeft90      ? 3
                : id == idRotateRight90     ? 4
                : id == idRotatePreviewPrev ? 6
                : id == idRotatePreviewNext ? 7
                                            : 5;
    int state = disabled ? 3 : pressed ? 2 : hot ? 1 : 0;
    int size = DpiScale(dlg->hwnd, direction ? 22 : 18);
    HDC memory = CreateCompatibleDC(item->hDC);
    HBITMAP& bitmap = dlg->iconCache[index][state];
    const TbIcon icons[] = {TbIcon::ZoomIn,      TbIcon::ZoomOut,   TbIcon::None,     TbIcon::RotateLeft,
                            TbIcon::RotateRight, TbIcon::Rotate180, TbIcon::PagePrev, TbIcon::PageNext};
    if (!bitmap) bitmap = CreateSvgIconAlphaBitmap(size, icons[index], color);
    int ix = direction ? DpiScale(dlg->hwnd, 6) : (r.dx - size) / 2;
    if (bitmap && memory) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, (BYTE)(zoom && pressed ? 150 : zoom && hot ? 210 : 255), AC_SRC_ALPHA};
        int offset = 0;
        AlphaBlend(item->hDC, ix + offset, (r.dy - size) / 2 + offset, size, size, memory, 0, 0, size, size, blend);
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
    if (direction && (item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT)) {
        RECT focus = item->rcItem;
        InflateRect(&focus, -2, -2);
        DrawFocusRect(item->hDC, &focus);
    }
}

// Render a complete slider frame independently of transparent-parent custom
// draw. The native control still owns geometry, hit testing and input handling.
// Present the background, icon and label together, without exposing the clear
// between frames when the scroll container invalidates its children.
static void PdfRotateDrawIconButton(PdfRotatePagesDialog* dlg, DRAWITEMSTRUCT* item) {
    int w = item->rcItem.right - item->rcItem.left, h = item->rcItem.bottom - item->rcItem.top;
    HDC dc = CreateCompatibleDC(item->hDC);
    HBITMAP bitmap = CreateCompatibleBitmap(item->hDC, w, h);
    if (!dc || !bitmap) {
        DeleteDC(dc);
        DeleteObject(bitmap);
        PdfRotateDrawIconButtonContent(dlg, item);
        return;
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    DRAWITEMSTRUCT frame = *item;
    frame.hDC = dc;
    frame.rcItem = {0, 0, w, h};
    PdfRotateDrawIconButtonContent(dlg, &frame);
    BitBlt(item->hDC, item->rcItem.left, item->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

static void PdfRotatePaintSlider(HWND hwnd, HDC dc, PdfRotatePagesDialog* dlg) {
    RECT client{}, channel{}, thumb{};
    GetClientRect(hwnd, &client);
    SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
    SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
    FillRect(dc, &client, dlg->bgBrush);
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    int state = !IsWindowEnabled(hwnd) ? TUS_DISABLED
                : GetCapture() == hwnd ? TUS_PRESSED
                : PtInRect(&thumb, pt) ? TUS_HOT
                : GetFocus() == hwnd   ? TUS_FOCUSED
                                       : TUS_NORMAL;
    HTHEME theme = OpenThemeData(hwnd, L"TRACKBAR");
    if (theme) {
        DrawThemeBackground(theme, dc, TKP_TRACK, TRS_NORMAL, &channel, nullptr);
        DrawThemeBackground(theme, dc, TKP_THUMBBOTTOM, state, &thumb, nullptr);
        CloseThemeData(theme);
    } else {
        RECT line = channel;
        line.top = (line.top + line.bottom) / 2;
        line.bottom = line.top + std::max(1, DpiScale(hwnd, 1));
        HBRUSH brush = CreateSolidBrush(ThemeWindowTextDisabledColor());
        FillRect(dc, &line, brush);
        FillRect(dc, &thumb, brush);
        DeleteObject(brush);
    }
}

static LRESULT CALLBACK PdfRotateSliderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* dlg = (PdfRotatePagesDialog*)data;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        Rect client = ClientRect(hwnd);
        if (client.dx > 0 && client.dy > 0) {
            DoubleBuffer buffer(hwnd, client);
            PdfRotatePaintSlider(hwnd, buffer.GetDC(), dlg);
            buffer.Flush(dc, &ps.rcPaint);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_PRINTCLIENT) {
        PdfRotatePaintSlider(hwnd, (HDC)wp, dlg);
        return 0;
    }
    LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_SIZE || msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
        msg == WM_CAPTURECHANGED || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_THEMECHANGED)
        InvalidateRect(hwnd, nullptr, FALSE);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, PdfRotateSliderProc, 1);
    return result;
}

static LRESULT CALLBACK PdfRotateSpinProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* dlg = (PdfRotatePagesDialog*)data;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        HBRUSH bg = CreateSolidBrush(ThemeWindowControlBackgroundColor());
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        int mid = (r.top + r.bottom) / 2;
        if (GetCapture() == hwnd && (GetKeyState(VK_LBUTTON) & 0x8000)) {
            RECT pressed = r;
            if (pt.y < mid)
                pressed.bottom = mid;
            else
                pressed.top = mid;
            bg = CreateSolidBrush(ThemeInspectorSelectedBackgroundColor());
            FillRect(dc, &pressed, bg);
            DeleteObject(bg);
        }
        HPEN edge = CreatePen(PS_SOLID, 1, AccentColor(AppDialogPanelBackgroundColor(), 30));
        HGDIOBJ old = SelectObject(dc, edge);
        MoveToEx(dc, r.left, mid, nullptr);
        LineTo(dc, r.right, mid);
        MoveToEx(dc, r.left, r.top, nullptr);
        LineTo(dc, r.left, r.bottom);
        SelectObject(dc, old);
        DeleteObject(edge);
        // Up-down controls do not reliably implement WM_GETFONT. Use the
        // dialog font explicitly, as the combo box does, rather than a stock font.
        HDC glyphDc = CreateCompatibleDC(dc);
        HGDIOBJ oldFont = SelectObject(glyphDc, dlg->hFont);
        TEXTMETRICW tm{};
        GetTextMetricsW(glyphDc, &tm);
        int width = r.right - r.left;
        int height = std::max((int)tm.tmHeight + DpiScale(hwnd, 4), (int)r.bottom - mid);
        HBITMAP glyph = CreateCompatibleBitmap(dc, width, height);
        HGDIOBJ oldBitmap = SelectObject(glyphDc, glyph);
        SetBkMode(glyphDc, TRANSPARENT);
        SetTextColor(glyphDc, ThemeWindowTextColor());
        for (int i = 0; i < 2; i++) {
            RECT arrow = r;
            if (i == 0)
                arrow.bottom = mid;
            else
                arrow.top = mid;
            bool pressed = GetCapture() == hwnd && (GetKeyState(VK_LBUTTON) & 0x8000) && ((pt.y < mid) == (i == 0));
            HBRUSH fill = CreateSolidBrush(pressed ? ThemeInspectorSelectedBackgroundColor()
                                                   : ThemeWindowControlBackgroundColor());
            RECT source{0, 0, width, height};
            FillRect(glyphDc, &source, fill);
            DeleteObject(fill);
            DrawTextW(glyphDc, L"\u02c5", -1, &source, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            // Crop the font cell to the actual glyph ink. Scaling the entire
            // cell into a half-height button made the chevron unnecessarily tiny.
            COLORREF background =
                pressed ? ThemeInspectorSelectedBackgroundColor() : ThemeWindowControlBackgroundColor();
            RECT ink{width, height, 0, 0};
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++) {
                    if (GetPixel(glyphDc, x, y) == background) continue;
                    ink.left = std::min(ink.left, (LONG)x);
                    ink.top = std::min(ink.top, (LONG)y);
                    ink.right = std::max(ink.right, (LONG)x + 1);
                    ink.bottom = std::max(ink.bottom, (LONG)y + 1);
                }
            int sourceW = ink.right - ink.left, sourceH = ink.bottom - ink.top;
            if (sourceW > 0 && sourceH > 0) {
                int targetW = std::min(sourceW, width - DpiScale(hwnd, 4));
                int targetH = std::min(sourceH, (int)(arrow.bottom - arrow.top) - DpiScale(hwnd, 2));
                int x = arrow.left + (width - targetW) / 2;
                int y = arrow.top + (arrow.bottom - arrow.top - targetH) / 2;
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchBlt(dc, x, i == 0 ? y + targetH - 1 : y, targetW, i == 0 ? -targetH : targetH, glyphDc, ink.left,
                           ink.top, sourceW, sourceH, SRCCOPY);
            }
        }
        SelectObject(glyphDc, oldBitmap);
        SelectObject(glyphDc, oldFont);
        DeleteObject(glyph);
        DeleteDC(glyphDc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_MOUSEMOVE || msg == WM_CAPTURECHANGED)
        InvalidateRect(hwnd, nullptr, FALSE);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, PdfRotateSpinProc, 1);
    return result;
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

    // A disabled popup can still receive posted commands and activation
    // messages; never let those touch the engine while the worker edits it.
    if (dlg && dlg->applying && (msg == WM_COMMAND || msg == WM_ACTIVATE || msg == WM_CLOSE)) return 0;
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
            if (((DRAWITEMSTRUCT*)lp)->CtlID == idRotateAngleMinus ||
                ((DRAWITEMSTRUCT*)lp)->CtlID == idRotateAnglePlus) {
                auto item = (DRAWITEMSTRUCT*)lp;
                bool pressed = (item->itemState & ODS_SELECTED) != 0;
                bool hot = GetPropW(item->hwndItem, L"PdfRotateHot") != nullptr;
                COLORREF bg = ThemeWindowControlBackgroundColor();
                if (pressed || hot) bg = AccentColor(bg, pressed ? 12 : 6);
                HBRUSH brush = CreateSolidBrush(bg);
                FillRect(item->hDC, &item->rcItem, brush);
                DeleteObject(brush);
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC,
                             IsWindowEnabled(item->hwndItem) ? ThemeWindowTextColor() : ThemeWindowTextDisabledColor());
                ScopedSelectObject font(item->hDC, dlg->hFont);
                DrawTextW(item->hDC, item->CtlID == idRotateAngleMinus ? L"−" : L"+", -1, &item->rcItem,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &item->rcItem);
                return TRUE;
            }
            if ((((DRAWITEMSTRUCT*)lp)->CtlID >= idRotateZoomIn && ((DRAWITEMSTRUCT*)lp)->CtlID <= idRotateClose) ||
                ((DRAWITEMSTRUCT*)lp)->CtlID == idRotateLeft90 || ((DRAWITEMSTRUCT*)lp)->CtlID == idRotateRight90 ||
                ((DRAWITEMSTRUCT*)lp)->CtlID == idRotate180 || ((DRAWITEMSTRUCT*)lp)->CtlID == idRotatePreviewPrev ||
                ((DRAWITEMSTRUCT*)lp)->CtlID == idRotatePreviewNext) {
                PdfRotateDrawIconButton(dlg, (DRAWITEMSTRUCT*)lp);
                return TRUE;
            }
            break;
        case WM_NOTIFY: {
            auto hdr = (NMHDR*)lp;
            if (hdr->hwndFrom == dlg->hwndAngleSpin && hdr->code == UDN_DELTAPOS) {
                auto delta = (NMUPDOWN*)lp;
                if (dlg->angleValid) {
                    PdfRotateSetAngle(dlg, dlg->adjustment.deskew + delta->iDelta * 0.05, true, true);
                    PdfRotatePagesUpdateUi(dlg);
                }
                return TRUE;
            }

            break;
        }
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
                int next = limitValue(dlg->sidebarPos - steps * DpiScale(hwnd, 72), 0, dlg->sidebarMax);
                if (next == dlg->sidebarPos) return 0;
                if (dlg->tooltip) SendMessageW(dlg->tooltip->hwnd, TTM_POP, 0, 0);
                dlg->sidebarPos = next;
                PdfRotatePagesLayout(dlg, DpiGet(hwnd), true);
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
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: {
            POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
            if (PtInRect(&dlg->previewRc, pt) && dlg->gdiPreview) {
                PdfRotateSyncCrop(dlg);
                dlg->cropDrag = dlg->manualCrop ? PdfRotateCropHitTest(dlg, pt) : 32;
                dlg->cropBefore = dlg->crop;
                dlg->dragging = true;
                dlg->dragStart = pt;
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, dlg->manualCrop ? PdfRotateCropCursor(dlg->cropDrag) : IDC_SIZEALL));
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (dlg->dragging) {
                POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
                if (dlg->manualCrop && dlg->previewOutput.Width > 0) {
                    SetCursor(LoadCursorW(nullptr, PdfRotateCropCursor(dlg->cropDrag)));
                    auto o = dlg->previewOutput;
                    float pw, ph;
                    if (!PdfRotatePageSize(dlg, dlg->currentPageNo, &pw, &ph)) return 0;
                    auto* state = PdfRotateCropForPage(dlg, dlg->currentPageNo);
                    auto before = dlg->cropBefore;
                    PdfAdjustmentCropBounds c{before.x * pw, before.y * ph, before.dx * pw, before.dy * ph};
                    auto clampX = [&](float x) { return limitValue((x - o.X) / o.Width * pw, 0.f, pw); };
                    auto clampY = [&](float y) { return limitValue((y - o.Y) / o.Height * ph, 0.f, ph); };
                    auto next = PdfAdjustmentDragCrop(c, dlg->cropDrag, (pt.x - dlg->dragStart.x) / o.Width * pw,
                                                      (pt.y - dlg->dragStart.y) / o.Height * ph,
                                                      clampX((float)dlg->dragStart.x), clampY((float)dlg->dragStart.y),
                                                      clampX((float)pt.x), clampY((float)pt.y), state->ratio, pw, ph);
                    if (next.w >= 1 && next.h >= 1) state->box = next;
                    PdfRotateSyncCrop(dlg);
                    PdfRotatePagesUpdateUi(dlg);
                    PdfRotateInvalidatePreview(dlg);
                    return 0;
                }
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
                LPCWSTR cursor =
                    dlg->manualCrop ? PdfRotateCropCursor(dlg->dragging ? dlg->cropDrag : PdfRotateCropHitTest(dlg, pt))
                    : dlg->dragging ? IDC_SIZEALL
                                    : IDC_HAND;
                SetCursor(LoadCursorW(nullptr, cursor));
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
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                SetTimer(hwnd, 1, 150, nullptr);
            }
            return 0;
        case WM_TIMER:
            if (wp == 3) {
                if (!PdfRotateSourceIsCurrent(dlg)) {
                    PdfRotateStopHold(dlg);
                    delete dlg->gdiPreview;
                    dlg->gdiPreview = nullptr;
                    PdfRotatePagesUpdateUi(dlg);
                    PdfRotateInvalidatePreview(dlg);
                } else if (dlg->previewColorEpoch != (int)gRenderCache->darkModeEpoch) {
                    PdfRotateStopHold(dlg);
                    PdfRotateLoadPreview(dlg);
                    PdfRotatePagesUpdateUi(dlg);
                    PdfRotateInvalidatePreview(dlg);
                }
                return 0;
            }
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
                PdfRotatePagesUpdateUi(dlg);
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
                        PdfRotateCloseDialog(dlg);
                        return 0;
                    }
                    break;
                case idRotatePreviewPrev:
                case idRotatePreviewNext:
                    if (code == BN_CLICKED) {
                        PdfRotateNavigate(dlg, id == idRotatePreviewPrev ? -1 : 1);
                        PdfRotatePagesUpdateUi(dlg);
                        return 0;
                    }
                    break;
                case idRotatePreviewPage:
                    if (code == EN_KILLFOCUS && !dlg->cropUiSync) {
                        BOOL valid = FALSE;
                        int page = (int)GetDlgItemInt(hwnd, idRotatePreviewPage, &valid, FALSE);
                        if (valid) PdfRotateNavigate(dlg, 0, page);
                        PdfRotatePagesUpdateUi(dlg);
                        return 0;
                    }
                    break;
                case idRotateCropOverride:
                    if (code == BN_CLICKED && !dlg->cropUiSync) {
                        if (SendMessageW(dlg->hwndCropOverride, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                            auto copy = *PdfRotateCropForPage(dlg, dlg->currentPageNo);
                            if (!copy.pageNo) {
                                copy.pageNo = dlg->currentPageNo;
                                dlg->cropOverrides.Append(copy);
                            }
                        } else {
                            for (int i = 0; i < dlg->cropOverrides.Size(); i++)
                                if (dlg->cropOverrides[i].pageNo == dlg->currentPageNo) {
                                    dlg->cropOverrides.RemoveAt(i);
                                    break;
                                }
                        }
                        PdfRotatePagesUpdateUi(dlg);
                        PdfRotateInvalidatePreview(dlg);
                        return 0;
                    }
                    break;
                case idRotateCropRatio:
                    if (code == CBN_SELCHANGE && !dlg->cropUiSync) {
                        float w, h;
                        if (!PdfRotatePageSize(dlg, dlg->currentPageNo, &w, &h)) return 0;
                        auto* c = PdfRotateCropForPage(dlg, dlg->currentPageNo);
                        c->ratioMode = (int)SendMessageW(dlg->hwndCropRatio, CB_GETCURSEL, 0, 0);
                        const float preset[] = {0, 0, 0, 210.f / 297.f, 297.f / 210.f, 1, 4.f / 3.f, 3.f / 4.f};
                        c->ratio = c->ratioMode == 1   ? c->box.w / c->box.h
                                   : c->ratioMode == 2 ? dlg->previewSourceBox.dx / dlg->previewSourceBox.dy
                                                       : preset[limitValue(c->ratioMode, 0, 7)];
                        if (c->ratio > 0) c->box = PdfAdjustmentFitRatio(c->box, c->ratio, w, h);
                        PdfRotatePagesUpdateUi(dlg);
                        PdfRotateInvalidatePreview(dlg);
                        return 0;
                    }
                    break;
                case idRotateApplyToCurrent:
                case idRotateApplyToAll:
                case idRotateApplyToSpecified:
                    if (code == BN_CLICKED) {
                        // The controls live in a custom scroll container rather
                        // than a dialog manager-owned radio group.
                        SendMessageW(dlg->hwndCurrentPage, BM_SETCHECK,
                                     id == idRotateApplyToCurrent ? BST_CHECKED : BST_UNCHECKED, 0);
                        SendMessageW(dlg->hwndAllPages, BM_SETCHECK,
                                     id == idRotateApplyToAll ? BST_CHECKED : BST_UNCHECKED, 0);
                        SendMessageW(dlg->hwndSpecified, BM_SETCHECK,
                                     id == idRotateApplyToSpecified ? BST_CHECKED : BST_UNCHECKED, 0);
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
                case idRotateLeft90:
                case idRotateRight90:
                case idRotate180:
                    if (code == BN_CLICKED) {
                        if (!dlg->angleValid) return 0;
                        int direction = id == idRotateLeft90 ? -90 : id == idRotateRight90 ? 90 : 180;
                        dlg->adjustment.RotateBy(direction);
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
                        PdfRotatePagesLayout(dlg, DpiGet(hwnd), true);
                        PdfRotatePagesUpdateUi(dlg);
                        return 0;
                    }
                    break;
                case idRotateShowGrid:
                    if (code == BN_CLICKED) {
                        dlg->showGrid = SendMessageW(dlg->hwndShowGrid, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        EnableWindow(dlg->hwndGridDensity, dlg->showGrid);
                        EnableWindow(dlg->hwndGridMode, dlg->showGrid);
                        PdfRotateInvalidatePreview(dlg);
                        return 0;
                    }
                    break;
                case idRotateGridMode:
                    if (code == CBN_SELCHANGE) {
                        dlg->crossGrid = SendMessageW(dlg->hwndGridMode, CB_GETCURSEL, 0, 0) == 1;
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
                case idRotateManualCrop:
                    if (code == BN_CLICKED) {
                        dlg->manualCrop = SendMessageW(dlg->hwndManualCrop, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        if (dlg->manualCrop) SendMessageW(dlg->hwndAutoCrop, BM_SETCHECK, BST_UNCHECKED, 0);
                        PdfRotatePagesUpdateUi(dlg);
                        PdfRotateInvalidatePreview(dlg);
                        return 0;
                    }
                    break;
                case idRotateFlipH:
                case idRotateFlipV:
                case idRotateAutoCrop:
                    if (code == BN_CLICKED) {
                        if (id == idRotateAutoCrop && PdfRotateAutoCrop(dlg)) {
                            dlg->manualCrop = false;
                            SendMessageW(dlg->hwndManualCrop, BM_SETCHECK, BST_UNCHECKED, 0);
                        }
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
            HPEN separatorPen =
                CreatePen(PS_SOLID, 1, AccentColor(AppDialogPanelBackgroundColor(), ThemeUsesDarkChrome() ? 18 : -18));
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
            RECT previewDamage{};
            if (IntersectRect(&previewDamage, &ps.rcPaint, &dlg->previewRc)) {
                InvalidateRect(dlg->hwndZoomIn, nullptr, FALSE);
                InvalidateRect(dlg->hwndZoomOut, nullptr, FALSE);
            }
            return 0;
        }
        case DM_GETDEFID:
            return MAKELRESULT(GetFocus() == dlg->hwndPreviewPage ? idRotatePreviewPage : idRotateApply, DC_HASDEFID);
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
                for (HWND heading : {dlg->hwndAny, dlg->hwndGridTitle, dlg->hwndRotationTitle, dlg->hwndFlipTitle}) {
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
            if (dlg->applying) return 0;
            PdfRotateStopHold(dlg);
            PdfRotateCloseDialog(dlg);
            return 0;
        case WM_NCDESTROY:
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 3);
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
            dlg->cropOverrides.Reset();
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK PdfRotatePageInputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        auto* dlg = (PdfRotatePagesDialog*)data;
        InvalidateRect(dlg->hwnd, &dlg->previewPageRc, FALSE);
    }
    if (msg == WM_GETDLGCODE && lp && ((MSG*)lp)->message == WM_KEYDOWN && ((MSG*)lp)->wParam == VK_RETURN)
        return DLGC_WANTMESSAGE;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        auto* dlg = (PdfRotatePagesDialog*)data;
        BOOL valid = FALSE;
        int page = (int)GetDlgItemInt(dlg->hwnd, idRotatePreviewPage, &valid, FALSE);
        if (valid) PdfRotateNavigate(dlg, 0, page);
        SetWindowTextA(hwnd, str::FormatTemp("%d", dlg->currentPageNo));
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        PdfRotatePagesUpdateUi(dlg);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, PdfRotatePageInputProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
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
    dlg->originalPageNo = currentPageNo;
    dlg->sourceTab = tab;
    dlg->sourceEngine = tab->GetEngine();
    dlg->sourcePageStructureVersion = EngineMupdfPageStructureVersion(dlg->sourceEngine);

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

    SendMessageW(dlg->hwndPagesEdit, EM_SETCUEBANNER, TRUE, (LPARAM)_TRW("e.g. 2, 5-7, 13-"));

    dlg->hwndAny = CreateWindowExW(0, L"STATIC", _TRW("Deskew"), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0,
                                   0, 0, hwnd, (HMENU)(INT_PTR)idRotateAny, h, nullptr);
    SendMessageW(dlg->hwndAny, WM_SETFONT, (WPARAM)hfontTitle, TRUE);

    dlg->hwndSlider = CreateWindowExW(0, TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ, 0, 0, 0, 0,
                                      hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndSlider, TBM_SETRANGEMIN, FALSE, (LPARAM)-500);
    SendMessageW(dlg->hwndSlider, TBM_SETRANGEMAX, TRUE, (LPARAM)500);
    SendMessageW(dlg->hwndSlider, TBM_SETTICFREQ, 50, 0);
    SendMessageW(dlg->hwndSlider, TBM_SETTIC, 0, 0);
    SendMessageW(dlg->hwndSlider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(dlg->hwndSlider, TBM_SETPAGESIZE, 0, 100);
    SendMessageW(dlg->hwndSlider, TBM_SETPOS, TRUE, 0);

    dlg->syncing = true;
    dlg->hwndAngleEdit = CreateWindowExW(0, WC_EDITW, L"0.00", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0,
                                         0, 0, 0, hwnd, (HMENU)(INT_PTR)idRotateAngleEdit, h, nullptr);
    SendMessageW(dlg->hwndAngleEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    dlg->syncing = false;
    dlg->hwndAngleSpin = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_ARROWKEYS, 0, 0, 0, 0, hwnd,
                                         (HMENU)(INT_PTR)idRotateAngleSpin, h, nullptr);
    SendMessageW(dlg->hwndAngleSpin, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SendMessageW(dlg->hwndAngleSpin, UDM_SETBUDDY, (WPARAM)dlg->hwndAngleEdit, 0);
    SendMessageW(dlg->hwndAngleSpin, UDM_SETRANGE32, (WPARAM)-3600, 3600);

    dlg->hwndAngleMinus = CreateWindowExW(0, L"BUTTON", L"\u2212", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                                          0, 0, 0, hwnd, (HMENU)(INT_PTR)idRotateAngleMinus, h, nullptr);
    dlg->hwndAnglePlus = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0,
                                         0, hwnd, (HMENU)(INT_PTR)idRotateAnglePlus, h, nullptr);
    SendMessageW(dlg->hwndAngleMinus, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SendMessageW(dlg->hwndAnglePlus, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SetWindowSubclass(dlg->hwndAngleMinus, PdfRotateFineButtonProc, 1, (DWORD_PTR)dlg);
    SetWindowSubclass(dlg->hwndAnglePlus, PdfRotateFineButtonProc, 1, (DWORD_PTR)dlg);
    SetWindowSubclass(dlg->hwndAngleMinus, PdfRotateIconButtonProc, 2, 0);
    SetWindowSubclass(dlg->hwndAnglePlus, PdfRotateIconButtonProc, 2, 0);

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
    dlg->hwndManualCrop =
        CreateWindowExW(0, L"BUTTON", _TRW("Manual Crop"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0,
                        0, 0, hwnd, (HMENU)(INT_PTR)idRotateManualCrop, h, nullptr);
    SendMessageW(dlg->hwndManualCrop, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndCropRatio = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
                                         0, 0, 0, hwnd, (HMENU)(INT_PTR)idRotateCropRatio, h, nullptr);
    const char* ratios[] = {
        "Free", "Lock current ratio", "Original page ratio", "A4 Portrait", "A4 Landscape", "1:1", "4:3", "3:4"};
    for (auto text : ratios) SendMessageW(dlg->hwndCropRatio, CB_ADDSTRING, 0, (LPARAM)ToWStrTemp(_TRA(text)));
    SendMessageW(dlg->hwndCropRatio, CB_SETCURSEL, 0, 0);
    dlg->hwndCropOverride = CreateWindowExW(0, L"BUTTON", _TRW("Adjust this page separately"),
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                                            (HMENU)(INT_PTR)idRotateCropOverride, h, nullptr);
    dlg->hwndPreviewPrev = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                                           0, 0, hwnd, (HMENU)(INT_PTR)idRotatePreviewPrev, h, nullptr);
    dlg->hwndPreviewNext = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                                           0, 0, hwnd, (HMENU)(INT_PTR)idRotatePreviewNext, h, nullptr);
    SetWindowSubclass(dlg->hwndPreviewPrev, PdfRotateIconButtonProc, 1, 0);
    SetWindowSubclass(dlg->hwndPreviewNext, PdfRotateIconButtonProc, 1, 0);
    dlg->hwndPreviewPage =
        CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER,
                        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)idRotatePreviewPage, h, nullptr);
    SetWindowSubclass(dlg->hwndPreviewPage, PdfRotatePageInputProc, 1, (DWORD_PTR)dlg);
    dlg->hwndPreviewStatus =
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0, 0, 0,
                        0, hwnd, nullptr, h, nullptr);
    HWND cropControls[] = {dlg->hwndCropRatio,   dlg->hwndCropOverride, dlg->hwndPreviewPrev,
                           dlg->hwndPreviewNext, dlg->hwndPreviewPage,  dlg->hwndPreviewStatus};
    for (auto child : cropControls) SendMessageW(child, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

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
    dlg->hwndAngleHint = label(L"", false);

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
    dlg->hwndGridMode = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
                                        0, 0, 0, hwnd, (HMENU)(INT_PTR)idRotateGridMode, h, nullptr);
    SendMessageW(dlg->hwndGridMode, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    CbAddString(dlg->hwndGridMode, _TRA("Horizontal Lines"));
    CbAddString(dlg->hwndGridMode, _TRA("Cross Grid"));
    SendMessageW(dlg->hwndGridMode, CB_SETCURSEL, 1, 0);
    dlg->tooltip = new Tooltip();
    Tooltip::CreateArgs tooltipArgs;
    tooltipArgs.parent = hwnd;
    tooltipArgs.font = dlg->hFont;
    dlg->tooltip->Create(tooltipArgs);
    auto addTip = [&](HWND control, const WCHAR* text) {
        TOOLINFOW info{sizeof(info)};
        info.hwnd = hwnd;
        info.uId = (UINT_PTR)control;
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.lpszText = (WCHAR*)text;
        SendMessageW(dlg->tooltip->hwnd, TTM_ADDTOOLW, 0, (LPARAM)&info);
    };
    addTip(dlg->hwndCropRatio, _TRW("Crop aspect ratio"));
    addTip(dlg->hwndCropOverride,
           _TRW("Keep adjusting pages separately while browsing; uncheck to restore this page to the shared crop."));
    addTip(dlg->hwndPreviewPrev, _TRW("Previous page"));
    addTip(dlg->hwndPreviewNext, _TRW("Next page"));
    addTip(dlg->hwndPreviewPage, _TRW("Page number within the selected range; press Enter to preview."));
    addTip(dlg->hwndManualCrop, _TRW("Drag to select a crop area; drag its edges to resize or its inside to move."));
    addTip(dlg->hwndGridMode, _TRW("Reference grid mode: horizontal lines or cross grid"));
    addTip(dlg->hwndGridDensity, _TRW("Reference grid spacing: sparse, normal or dense"));

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

    dlg->hwndSidebarPanel =
        CreateWindowExW(WS_EX_CONTROLPARENT, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                        0, 0, 0, 0, hwnd, nullptr, h, nullptr);
    SetWindowSubclass(dlg->hwndSidebarPanel, PdfRotateSidebarProc, 1, (DWORD_PTR)dlg);
    HWND sidebarControls[] = {dlg->hwndGridTitle,     dlg->hwndShowGrid,  dlg->hwndGridMode,     dlg->hwndGridDensity,
                              dlg->hwndRotationTitle, dlg->hwndLeft90,    dlg->hwndRight90,      dlg->hwnd180,
                              dlg->hwndFlipTitle,     dlg->hwndFlipH,     dlg->hwndFlipV,        dlg->hwndAutoCrop,
                              dlg->hwndManualCrop,    dlg->hwndCropRatio, dlg->hwndCropOverride, dlg->hwndApplyToLabel,
                              dlg->hwndCurrentPage,   dlg->hwndAllPages,  dlg->hwndSpecified,    dlg->hwndPagesEdit};
    for (HWND child : sidebarControls) SetParent(child, dlg->hwndSidebarPanel);
    dlg->hwndMinimize = iconButton(idRotateMinimize, _TRW("Minimize"));
    dlg->hwndMaximize = iconButton(idRotateMaximize, _TRW("Maximize"));
    dlg->hwndClose = iconButton(idRotateClose, _TRW("Close"));
    dlg->hwndZoomIn = iconButton(idRotateZoomIn, _TRW("Zoom In"));
    dlg->hwndZoomOut = iconButton(idRotateZoomOut, _TRW("Zoom Out"));
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

    PdfRotateLoadPreview(dlg);
    // Session deskew is part of the pending geometry; load it before deciding
    // whether Apply is trivial. Specified-page input is initially disabled.
    PdfRotatePagesUpdateUi(dlg);

    CenterDialog(hwnd, win->hwndFrame);
    AppDialogApplyChrome(hwnd);
    SetWindowTheme(dlg->hwndAngleEdit, L"", L"");
    SetWindowTheme(dlg->hwndPreviewPage, L"", L"");
    SetWindowSubclass(dlg->hwndSlider, PdfRotateSliderProc, 1, (DWORD_PTR)dlg);
    SetWindowSubclass(dlg->hwndAngleSpin, PdfRotateSpinProc, 1, (DWORD_PTR)dlg);
    RegisterAppDialogForTheme(hwnd, PdfRotatePagesThemeRefreshCb, dlg);
    SetTimer(hwnd, 3, 250, nullptr);
    ShowWindow(hwnd, SW_SHOW);
}

static void PdfRotatePagesThemeRefreshCb(HWND hwnd, void* ctx) {
    auto* dlg = (PdfRotatePagesDialog*)ctx;
    if (!dlg) {
        return;
    }
    PdfRotateStopHold(dlg);
    // Theme hooks may invalidate/update native child controls synchronously.
    // Suspend the whole subtree until brushes, chrome and display cache agree.
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM) -> BOOL {
            // Preserve intentionally hidden controls (e.g. a non-overflow scrollbar).
            if (GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE) {
                SetPropW(child, L"PdfAdjustResumeRedraw", (HANDLE)1);
                SendMessageW(child, WM_SETREDRAW, FALSE, 0);
            }
            return TRUE;
        },
        0);
    PdfRotateClearIconCache(dlg);
    DeleteObject(dlg->bgBrush);
    DeleteObject(dlg->ctrlBrush);
    dlg->bgBrush = CreateSolidBrush(AppDialogPanelBackgroundColor());
    dlg->ctrlBrush = CreateSolidBrush(ThemeWindowControlBackgroundColor());
    AppDialogApplyChrome(hwnd);
    SetWindowTheme(dlg->hwndAngleEdit, L"", L"");
    SetWindowTheme(dlg->hwndPreviewPage, L"", L"");
    if (dlg->tooltip) dlg->tooltip->UpdateTheme();
    dlg->previewColorEpoch = -1;
    PdfRotateLoadPreview(dlg);
    PdfRotatePagesUpdateUi(dlg);
    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM) -> BOOL {
            if (GetPropW(child, L"PdfAdjustResumeRedraw")) {
                RemovePropW(child, L"PdfAdjustResumeRedraw");
                SendMessageW(child, WM_SETREDRAW, TRUE, 0);
            }
            return TRUE;
        },
        0);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
