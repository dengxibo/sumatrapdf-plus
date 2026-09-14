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

struct PdfBakeDialog {
    HWND hwnd = nullptr;
    HWND hwndPathLabel = nullptr;
    HWND hwndDestEdit = nullptr;
    HWND hwndBrowseBtn = nullptr;
    HWND hwndBakeBtn = nullptr;
    HWND hwndCancelBtn = nullptr;
    HFONT hFont = nullptr;
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
        MessageBoxWarning(dlg->hwnd, "Failed to bake PDF file.", _TRA("Bake PDF"));
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
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
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
    dlg->hFont = GetDefaultGuiFont();

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
        MessageBoxWarning(dlg->hwnd, "Failed to extract text.", _TRA("Extract Text"));
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
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
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
    dlg->hFont = GetDefaultGuiFont();

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
        MessageBoxWarning(dlg->hwnd, "Failed to compress PDF file.", _TRA("Compress PDF"));
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
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
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
    dlg->hFont = GetDefaultGuiFont();

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
        MessageBoxWarning(dlg->hwnd, "Failed to decompress PDF file.", _TRA("Decompress PDF"));
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
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
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
    dlg->hFont = GetDefaultGuiFont();

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
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
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
    dlg->hFont = GetDefaultGuiFont();
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
    idRotateLeft90 = 110,
    idRotate180 = 111,
    idRotateRight90 = 112,
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
    HWND hwndRotationLabel = nullptr;
    HWND hwndLeft90 = nullptr;
    HWND hwnd180 = nullptr;
    HWND hwndRight90 = nullptr;
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
};

// the pages edit is editable only for "specified pages" scope; the apply
// button is always valid for current/all scope and follows the range
// syntax otherwise
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
    EnableWindow(dlg->hwndRotateBtn, valid);
}

static void PdfRotatePagesDoIt(PdfRotatePagesDialog* dlg) {
    Vec<int> pageNos;
    if (SendMessageW(dlg->hwndSpecified, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        char pages[256]{};
        GetWindowTextA(dlg->hwndPagesEdit, pages, dimof(pages) - 1);
        if (!ParseDeletePages(pages, dlg->pageCount, pageNos)) {
            MessageBoxWarning(dlg->hwnd, _TRA("Invalid page range."), _TRA("Rotate Pages"));
            return;
        }
    } else if (SendMessageW(dlg->hwndAllPages, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        for (int i = 1; i <= dlg->pageCount; i++) {
            pageNos.Append(i);
        }
    } else {
        pageNos.Append(dlg->currentPageNo);
    }

    // user-facing left/right maps to clockwise deltas: left 90° = +270
    int delta = 90;
    if (SendMessageW(dlg->hwndLeft90, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        delta = 270;
    } else if (SendMessageW(dlg->hwnd180, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        delta = 180;
    }

    MainWindow* win = dlg->win;
    WindowTab* tab = win->CurrentTab();
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }

    // apply the chosen clockwise delta on top of each page's current /Rotate
    int rotated = 0;
    for (int pageNo : pageNos) {
        int cur = EngineMupdfGetPageRotateCw(engine, pageNo);
        int want = (cur + delta) % 360;
        if (EngineMupdfSetPageRotateCw(engine, pageNo, want)) {
            rotated++;
        }
    }
    if (rotated == 0) {
        MessageBoxWarning(dlg->hwnd, _TRA("Selected pages are already at that rotation."), _TRA("Rotate Pages"));
        return;
    }

    // persist the rotation to the PDF file (temp sidecar if the file is locked)
    const char* path = engine->FilePath();
    EngineMupdfSetPdfTocModified(engine, true);
    tab->ignoreNextAutoReload = true;
    char* tmp = nullptr;
    bool ok = EngineMupdfSaveUpdated(engine, nullptr, {}, &tmp);
    if (!ok) {
        tab->ignoreNextAutoReload = false;
        MessageBoxWarning(dlg->hwnd, _TRA("Failed to save rotated PDF pages."), _TRA("Rotate Pages"));
        return;
    }

    TempStr notifMsg = str::FormatTemp(_TRA("Rotated %d page(s) and saved to '%s'"), rotated, path);
    DestroyWindow(dlg->hwnd);
    if (tmp) {
        // in-place overwrite failed (file locked); the rewrite went to a
        // sidecar temp, switch the tab over to it (replace-and-reload)
        SwitchCurrentTabToSavedFile(win, path, tmp);
        str::Free(tmp);
    } else {
        ReloadDocument(win, false);
    }
    ShowTemporaryNotification(win->hwndCanvas, notifMsg, kNotif5SecsTimeOut);
}

// theme-adaptive control colors: the brush returned here fills the control
// background, so statics/radios sit on the window background while the pages
// edit gets the recessed control background. darkmodelib subclasses (dark
// themes) intercept these messages first and fall through when disabled,
// so this is the single source of truth for the light themes.
static LRESULT PdfRotatePagesColorControl(PdfRotatePagesDialog* dlg, HDC dc, HWND control) {
    bool edit = control == dlg->hwndPagesEdit;
    COLORREF text = ThemeWindowTextColor();
    if (!IsWindowEnabled(control) || control == dlg->hwndPagesHint) {
        text = ThemeWindowTextDisabledColor();
    }
    SetTextColor(dc, text);
    SetBkColor(dc, edit ? ThemeWindowControlBackgroundColor() : ThemeWindowBackgroundColor());
    return (LRESULT)(edit ? dlg->ctrlBrush : dlg->bgBrush);
}

static LRESULT CALLBACK PdfRotatePagesDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PdfRotatePagesDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        dlg = (PdfRotatePagesDialog*)cs->lpCreateParams;
        dlg->hwnd = hwnd;
        dlg->bgBrush = CreateSolidBrush(ThemeWindowBackgroundColor());
        dlg->ctrlBrush = CreateSolidBrush(ThemeWindowControlBackgroundColor());
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (PdfRotatePagesDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, dlg->bgBrush);
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN) {
        return PdfRotatePagesColorControl(dlg, (HDC)wp, (HWND)lp);
    }

    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            switch (id) {
                case idRotateApply:
                    if (code == BN_CLICKED) {
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
            }
            break;
        }
        case DM_GETDEFID:
            // Enter triggers "Apply Rotation"
            return MAKELRESULT(idRotateApply, DC_HASDEFID);
        case WM_ACTIVATE:
            // track focus so the main loop routes Enter/Esc/Tab through
            // IsDialogMessage only while this window is the active one
            SetCurrentModelessDialog(LOWORD(wp) == WA_INACTIVE ? nullptr : hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (GetCurrentModelessDialog() == hwnd) {
                SetCurrentModelessDialog(nullptr);
            }
            if (dlg->hFontBold) {
                DeleteObject(dlg->hFontBold);
                dlg->hFontBold = nullptr;
            }
            DeleteObject(dlg->bgBrush);
            DeleteObject(dlg->ctrlBrush);
            dlg->bgBrush = dlg->ctrlBrush = nullptr;
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static constexpr const WCHAR* kPdfRotatePagesWinClassName = L"SUMATRA_PDF_ROTATE_PAGES";
static bool gPdfRotatePagesWinClassRegistered = false;

// compact, one-purpose dialog: apply-to scope, rotation direction, buttons
void ShowPdfRotatePagesDialog(MainWindow* win) {
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
    dlg->hFont = GetDefaultGuiFont();
    dlg->pageCount = pageCount;
    dlg->currentPageNo = currentPageNo;
    LOGFONTW lfw{};
    if (GetObjectW(dlg->hFont, sizeof(lfw), &lfw)) {
        lfw.lfWeight = FW_BOLD;
        dlg->hFontBold = CreateFontIndirectW(&lfw);
    }

    DlgMetrics m = GetDlgMetrics(win->hwndFrame, dlg->hFont);
    int pad = DpiScale(win->hwndFrame, 22);    // window side padding
    int rowGap = DpiScale(win->hwndFrame, 9);  // between options in a group
    int ttlGap = DpiScale(win->hwndFrame, 8);  // group title to first option
    int hintGap = DpiScale(win->hwndFrame, 5); // edit to hint text
    int grpGap = DpiScale(win->hwndFrame, 20); // between groups
    int indent = DpiScale(win->hwndFrame, 20); // edit/hint under "specified"

    int dlgW = DpiScale(win->hwndFrame, 480);
    // walk the rows once to derive both the window height and the control
    // y positions, so the two can never disagree
    int y = pad;
    int yTitle1 = y;
    y += m.rowH + ttlGap;
    int yCur = y;
    y += m.rowH + rowGap;
    int yAll = y;
    y += m.rowH + rowGap;
    int ySpec = y;
    y += m.rowH + hintGap;
    int yEdit = y;
    y += m.rowH + hintGap;
    int yHint = y;
    y += m.rowH + grpGap;
    int yTitle2 = y;
    y += m.rowH + ttlGap;
    int yDir = y;
    y += m.rowH + grpGap;
    int yBtn = y;
    // +32: non-client area (title bar, frame)
    int dlgH = y + m.btnH + pad + DpiScale(win->hwndFrame, 32);

    HINSTANCE h = GetModuleHandleW(nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPdfRotatePagesWinClassName, _TRW("Rotate Pages"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                dlgW, dlgH, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        if (dlg->hFontBold) {
            DeleteObject(dlg->hFontBold);
        }
        delete dlg;
        return;
    }

    int x = pad;
    int w = dlgW - 2 * pad;
    HFONT hfontTitle = dlg->hFontBold ? dlg->hFontBold : dlg->hFont;

    // group: apply to
    dlg->hwndApplyToLabel = CreateWindowExW(0, L"STATIC", _TRW("Apply To"), WS_CHILD | WS_VISIBLE | SS_LEFT, x,
                                            yTitle1, w, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndApplyToLabel, WM_SETFONT, (WPARAM)hfontTitle, TRUE);

    WCHAR* curTxt = ToWStrTemp(str::FormatTemp(_TRA("Current Page (Page %d)"), dlg->currentPageNo));
    dlg->hwndCurrentPage = CreateWindowExW(0, L"BUTTON", curTxt,
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, x, yCur,
                                           w, m.rowH, hwnd, (HMENU)(INT_PTR)idRotateApplyToCurrent, h, nullptr);
    SendMessageW(dlg->hwndCurrentPage, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SendMessageW(dlg->hwndCurrentPage, BM_SETCHECK, BST_CHECKED, 0);

    WCHAR* allTxt = ToWStrTemp(str::FormatTemp(_TRA("All Pages (%d)"), dlg->pageCount));
    dlg->hwndAllPages =
        CreateWindowExW(0, L"BUTTON", allTxt, WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, x, yAll, w, m.rowH, hwnd,
                        (HMENU)(INT_PTR)idRotateApplyToAll, h, nullptr);
    SendMessageW(dlg->hwndAllPages, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndSpecified = CreateWindowExW(0, L"BUTTON", _TRW("Specified Pages"),
                                         WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, x, ySpec, w, m.rowH, hwnd,
                                         (HMENU)(INT_PTR)idRotateApplyToSpecified, h, nullptr);
    SendMessageW(dlg->hwndSpecified, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndPagesEdit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x + indent, yEdit,
                                         w - indent, m.rowH, hwnd, (HMENU)(INT_PTR)idRotatePagesEdit, h, nullptr);
    SendMessageW(dlg->hwndPagesEdit, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    dlg->hwndPagesHint = CreateWindowExW(0, L"STATIC", _TRW("e.g. 2, 5-7, 13-"), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         x + indent, yHint, w - indent, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndPagesHint, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    // group: rotation direction
    dlg->hwndRotationLabel = CreateWindowExW(0, L"STATIC", _TRW("Rotation"), WS_CHILD | WS_VISIBLE | SS_LEFT, x,
                                             yTitle2, w, m.rowH, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->hwndRotationLabel, WM_SETFONT, (WPARAM)hfontTitle, TRUE);

    // three options spread left/center/right; WS_GROUP on the first binds
    // them for arrow-key navigation
    int third = w / 3;
    dlg->hwndLeft90 = CreateWindowExW(0, L"BUTTON", _TRW("Left 90°"),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, x, yDir,
                                      third, m.rowH, hwnd, (HMENU)(INT_PTR)idRotateLeft90, h, nullptr);
    SendMessageW(dlg->hwndLeft90, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    dlg->hwnd180 = CreateWindowExW(0, L"BUTTON", L"180°", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, x + third, yDir,
                                   third, m.rowH, hwnd, (HMENU)(INT_PTR)idRotate180, h, nullptr);
    SendMessageW(dlg->hwnd180, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    dlg->hwndRight90 = CreateWindowExW(0, L"BUTTON", _TRW("Right 90°"), WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                       x + 2 * third, yDir, w - 2 * third, m.rowH, hwnd,
                                       (HMENU)(INT_PTR)idRotateRight90, h, nullptr);
    SendMessageW(dlg->hwndRight90, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    SendMessageW(dlg->hwndRight90, BM_SETCHECK, BST_CHECKED, 0);

    // bottom-right: [Cancel] [Apply Rotation]
    WCHAR* applyTxt = _TRW("Apply Rotation");
    int applyW = std::max(m.btnW, HwndMeasureText(hwnd, ToUtf8Temp(applyTxt), dlg->hFont).dx + DpiScale(hwnd, 28));
    WCHAR* cancelTxt = _TRW("Cancel");
    int cancelW = std::max(m.btnW, HwndMeasureText(hwnd, ToUtf8Temp(cancelTxt), dlg->hFont).dx + DpiScale(hwnd, 28));
    int bx = x + w - applyW;
    dlg->hwndRotateBtn = CreateWindowExW(0, L"BUTTON", applyTxt,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, bx, yBtn, applyW,
                                         m.btnH, hwnd, (HMENU)(INT_PTR)idRotateApply, h, nullptr);
    SendMessageW(dlg->hwndRotateBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);
    dlg->hwndCancelBtn = CreateWindowExW(0, L"BUTTON", cancelTxt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         bx - cancelW - m.btnGap, yBtn, cancelW, m.btnH, hwnd,
                                         (HMENU)(INT_PTR)idRotateCancel, h, nullptr);
    SendMessageW(dlg->hwndCancelBtn, WM_SETFONT, (WPARAM)dlg->hFont, TRUE);

    // initial enable/disable state (edit disabled for current-page scope)
    PdfRotatePagesUpdateUi(dlg);

    CenterDialog(hwnd, win->hwndFrame);
    if (UseDarkModeLib()) {
        // dark themes: darkmodelib fills the background and custom-paints the
        // child controls; light themes fall through to the WM_ERASEBKGND /
        // WM_CTLCOLOR* handlers in PdfRotatePagesDlgProc (theme-brush based)
        DarkMode::setWindowEraseBgSubclass(hwnd);
        DarkMode::setChildCtrlsSubclassAndTheme(hwnd);
    }
    UpdateWindowCaptionTheme(hwnd);
    ShowWindow(hwnd, SW_SHOW);
}

// theme switch while the modeless dialog is open: rebuild the theme brushes
// and repaint, mirroring RefreshAiTocWindowsTheme
void RefreshPdfRotatePagesTheme() {
    if (!UseDarkModeLib()) {
        return;
    }
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM) -> BOOL {
            WCHAR cls[64]{};
            GetClassNameW(hwnd, cls, dimof(cls));
            if (!wcscmp(cls, kPdfRotatePagesWinClassName)) {
                auto* dlg = (PdfRotatePagesDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                if (dlg) {
                    DeleteObject(dlg->bgBrush);
                    DeleteObject(dlg->ctrlBrush);
                    dlg->bgBrush = CreateSolidBrush(ThemeWindowBackgroundColor());
                    dlg->ctrlBrush = CreateSolidBrush(ThemeWindowControlBackgroundColor());
                    DarkMode::setChildCtrlsSubclassAndTheme(hwnd);
                    UpdateWindowCaptionTheme(hwnd);
                    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
                }
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
        MessageBoxWarning(dlg->hwnd, "Failed to encrypt PDF file.", _TRA("Encrypt PDF"));
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
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
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
    dlg->hFont = GetDefaultGuiFont();

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
        MessageBoxWarning(dlg->hwnd, "Failed to decrypt PDF file.", _TRA("Decrypt PDF"));
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
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
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
    dlg->hFont = GetDefaultGuiFont();

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
    ShowWindow(hwnd, SW_SHOW);
}
