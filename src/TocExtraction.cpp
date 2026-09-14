/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"
#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "ExtractPdfToc.h"
#include "AiToc.h"
#include "TocCalib.h"
#include "TocExtraction.h"
#include "Notifications.h"
#include "SumatraDialogs.h"
#include "resource.h"
#include "Theme.h"
#include "Translations.h"
#include "DarkModeSubclass.h"

void EngineMupdfEnsurePageLinksForHitTest(EngineBase* engine, int pageNo);
bool EngineMupdfCanEditPdfToc(EngineBase* engine);

// Resource dialogs use the existing PerMonitorV2 dialog-manager scaling.
// title is an English translation msgid (translated when the dialog starts);
// msgid is a static message given as an English msgid (translated on init);
// message is a caller-built wide string (may contain dynamic content).
struct TocDialogData {
    const char* title;
    const char* msgid = nullptr;
    const WCHAR* message = nullptr;

    bool method = false;
    HBRUSH background = nullptr;
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
};

static void TocMethodFonts(HWND hwnd, TocDialogData* data) {
    NONCLIENTMETRICS metrics{};
    metrics.cbSize = sizeof(metrics);
    int dpi = DpiGetForHwnd(hwnd);
    if (!GetNonClientMetricsForDpi(dpi, &metrics)) return;
    metrics.lfMessageFont.lfHeight = -MulDiv(19, dpi, 144); // 9.5 pt
    metrics.lfMessageFont.lfWeight = FW_NORMAL;
    HFONT body = CreateFontIndirectW(&metrics.lfMessageFont);
    metrics.lfMessageFont.lfHeight = -MulDiv(11, dpi, 72);
    metrics.lfMessageFont.lfWeight = FW_SEMIBOLD;
    HFONT heading = CreateFontIndirectW(&metrics.lfMessageFont);
    if (!body || !heading) {
        DeleteObject(body);
        DeleteObject(heading);
        return;
    }
    for (int id : {IDC_TOC_MESSAGE, IDC_TOC_LOCAL_SUBTITLE, IDC_TOC_WEB_SUBTITLE}) {
        SendDlgItemMessageW(hwnd, id, WM_SETFONT, (WPARAM)heading, TRUE);
    }
    for (int id : {IDC_TOC_LOCAL_DESCRIPTION, IDC_TOC_WEB_DESCRIPTION, IDC_TOC_LOCAL_PRIVACY, IDC_TOC_WEB_PRIVACY}) {
        SendDlgItemMessageW(hwnd, id, WM_SETFONT, (WPARAM)body, TRUE);
    }
    DeleteObject(data->headingFont);
    DeleteObject(data->bodyFont);
    data->headingFont = heading;
    data->bodyFont = body;
}

static INT_PTR CALLBACK TocDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = (TocDialogData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_INITDIALOG) {
        data = (TocDialogData*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, lp);
        data->background = CreateSolidBrush(ThemeWindowBackgroundColor());
        SetWindowTextW(hwnd, _TRW(data->title));
        SetDlgItemTextW(hwnd, IDC_TOC_MESSAGE, data->msgid ? _TRW(data->msgid) : data->message);
        SetDlgItemTextW(hwnd, IDCANCEL, _TRW("Cancel"));
        SetDlgItemTextW(hwnd, IDOK, _TRW("Send"));
        if (data->method) {
            SetDlgItemTextW(hwnd, 100, _TRW("Local Extraction"));
            SetDlgItemTextW(hwnd, 103, _TRW("Online AI Recognition"));
            SetDlgItemTextW(hwnd, IDC_TOC_LOCAL_GROUP, _TRW("Local Extraction"));
            SetDlgItemTextW(hwnd, IDC_TOC_WEB_GROUP, _TRW("Online AI Recognition"));
            SetDlgItemTextW(hwnd, IDC_TOC_LOCAL_SUBTITLE, _TRW("Generate TOC From Document Structure"));
            SetDlgItemTextW(hwnd, IDC_TOC_WEB_SUBTITLE, _TRW("AI Recognition for Printed TOC and Body Structure"));
            SetDlgItemTextW(hwnd, IDC_TOC_LOCAL_DESCRIPTION,
                            _TRW("Analyzes text, heading levels, numbering and link targets to build the TOC locally "
                                 "and fast.\n"
                                 "Best for official-document style PDFs; works even better when the PDF already has "
                                 "native text and a clickable TOC."));
            SetDlgItemTextW(hwnd, IDC_TOC_WEB_DESCRIPTION,
                            _TRW("Extracts the structure from printed TOC pages, or from the body text structure.\n"
                                 "Recognized by the web AI; slower than local extraction."));
            SetDlgItemTextW(hwnd, IDC_TOC_LOCAL_PRIVACY,
                            _TRW("Processed locally and fast. Nothing is uploaded."));
            SetDlgItemTextW(hwnd, IDC_TOC_WEB_PRIVACY,
                            _TRW("Needs internet: only TOC page images or heading candidates are uploaded."));
            TocMethodFonts(hwnd, data);
        }
        if (UseDarkModeLib()) DarkMode::setChildCtrlsSubclassAndTheme(hwnd);
        UpdateWindowCaptionTheme(hwnd);
        CenterDialog(hwnd);

        int focus = IDCANCEL;
        SendMessageW(hwnd, DM_SETDEFID, focus, 0);
        SetFocus(GetDlgItem(hwnd, focus));
        return FALSE;
    }
    if (!data) return FALSE;
    switch (msg) {
        case WM_DPICHANGED:
            // Let the PMv2 dialog manager scale the resource layout first.
            if (data->method) PostMessageW(hwnd, WM_APP + 71, 0, 0);
            return FALSE;
        case WM_APP + 71:
            if (data->method) TocMethodFonts(hwnd, data);
            return TRUE;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetTextColor((HDC)wp, ThemeWindowTextColor());
            SetBkColor((HDC)wp, ThemeWindowBackgroundColor());
            return (INT_PTR)data->background;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDCANCEL || id == IDOK || (data->method && (id == 100 || id == 103))) {
                EndDialog(hwnd, id);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        case WM_DESTROY:

            DeleteObject(data->background);
            data->background = nullptr;
            DeleteObject(data->headingFont);
            DeleteObject(data->bodyFont);
            data->headingFont = data->bodyFont = nullptr;
            break;
    }
    return FALSE;
}

bool ConfirmTocPageSend(HWND parent, const WCHAR* message) {
    TocDialogData data{"Confirm Sending TOC Pages"};
    data.message = message;
    return CreateAppDialogBox(IDD_DIALOG_TOC_SEND, parent, TocDialogProc, (LPARAM)&data) == IDOK;
}
void ShowTocExtraction(MainWindow* win) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || !EngineMupdfCanEditPdfToc(engine)) return;
    if (TocCalibIsActive(win)) {
        ShowTocCalib(win);
        return;
    }
    if (ExtractPdfTocIsRunning()) return;
    // Display explanations only. Analysis starts after an explicit choice.
    engine->AddRef();
    defer {
        engine->Release();
    };
    TocDialogData choice{"Extract Table of Contents"};
    choice.msgid = "Choose an extraction method:";
    choice.method = true;
    INT_PTR button = CreateAppDialogBox(IDD_DIALOG_TOC_METHOD, win->hwndFrame, TocDialogProc, (LPARAM)&choice);
    if (button != 100 && button != 103) return;
    if (!IsMainWindowValid(win) || !win->AsFixed() || win->AsFixed()->GetEngine() != engine) return;
    if (button == 100)
        HandleExtractPdfTocCommand(win);
    else
        StartAiTocProofOfConcept(win);
}

int ApplyTocLinkEvidence(EngineBase* engine, Vec<ExtractedTocItem*>& roots) {
    if (!engine) return 0;
    Vec<ExtractedTocItem*> flat;
    FlattenExtractedTocItems(roots, flat);
    int matched = 0;
    for (ExtractedTocItem* it : flat) {
        if (it->tocPageNo < 1 || it->tocPageNo > engine->PageCount() || (it->tocX == 0 && it->tocY == 0)) continue;
        EngineMupdfEnsurePageLinksForHitTest(engine, it->tocPageNo);
        Vec<IPageElement*> elements = engine->GetElements(it->tocPageNo);
        IPageDestination* found = nullptr;
        bool ambiguous = false;
        for (IPageElement* el : elements) {
            IPageDestination* dest = el && el->IsLink() ? el->AsLink() : nullptr;
            if (!dest || dest->pageNo < 1 || dest->pageNo > engine->PageCount()) continue;
            RectF r = el->GetRect();
            if (it->tocX < r.x - 3 || it->tocX > r.x + r.dx + 3 || it->tocY < r.y - 3 || it->tocY > r.y + r.dy + 3)
                continue;
            if (found &&
                (found->pageNo != dest->pageNo || found->rect.x != dest->rect.x || found->rect.y != dest->rect.y))
                ambiguous = true;
            found = dest;
        }
        if (!found || ambiguous) continue;
        it->pageNo = found->pageNo;
        it->x = (float)found->rect.x;
        it->y = (float)found->rect.y;
        it->destinationSource = TocDestinationSource::PdfLink;
        it->verified = true;
        it->confidence = 100;
        matched++;
    }
    return matched;
}

void PrepareTocExtractionResult(EngineBase* engine, Vec<ExtractedTocItem*>& roots) {
    ApplyTocLinkEvidence(engine, roots);
    Vec<ExtractedTocItem*> flat;
    FlattenExtractedTocItems(roots, flat);
    for (ExtractedTocItem* it : flat) {
        if (it->pageNo < 1 || it->pageNo > engine->PageCount()) {
            it->pageNo = 0;
            it->destinationSource = TocDestinationSource::Unknown;
            it->verified = it->bodyMatched = false;
        } else if (it->destinationSource == TocDestinationSource::Unknown) {
            it->destinationSource = it->bodyMatched || it->source == ExtractedTocSource::BodyInference
                                        ? TocDestinationSource::BodyMatch
                                        : TocDestinationSource::Estimated;
        }
        if (it->destinationSource == TocDestinationSource::Estimated) {
            it->verified = false;
            it->confidence = std::min(it->confidence, 60);
        }
    }
}

bool PreviewTocExtraction(MainWindow* win, EngineBase* engine, Vec<ExtractedTocItem*>& roots, bool persistToDisk,
                          HWND parent) {
    (void)parent;
    if (!win || !engine) return false;
    PrepareTocExtractionResult(engine, roots);
    return StartTocCalib(win, roots, engine, persistToDisk, true);
}
