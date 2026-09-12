/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/JsonParser.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "DocProperties.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "ExtractPdfToc.h"
#include "TocCalib.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "SumatraDialogs.h"
#include "Flags.h"
#include "ImageSaveCropResize.h"
#include "Notifications.h"
#include "Translations.h"
#include "AiToc.h"
#include "DarkModeSubclass.h"
#include "Theme.h"

static const char* kAiTocPrompt =
    "你正在帮助 PDF 阅读器恢复一本扫描书籍中的印刷目录。图片按原书顺序排列。请根据视觉布局、缩进、"
    "字体、粗细、居中位置、上下留白、点线、页码和双栏关系恢复目录结构。只返回合法 JSON，不要解释或 Markdown。格式必须是"
    "{\"items\":[{\"title\":\"章节标题\",\"page\":12,\"level\":1}]}。page 是目录中印刷的页码，"
    "level 从 1 "
    "开始。特别注意：目录中可能存在没有‘第X章’编号、但通过居中、加粗、字号更大、上下留白或单独成行来划分大分段的标题。"
    "这类分段标题也是一级目录项，必须单独输出，不能丢弃，也不能挂到后面的章节下面。例如‘地球和地图’、‘中国地理’、‘自然"
    "地理’等，"
    "即使没有章节编号，也应输出为 level:1，并使用该标题右侧对应的页码。"
    "层级必须按目录的视觉分组关系判断，而不是只看‘第X章’文字：如果一个无编号的大标题（如‘地球和地图’）位于若干章之前并"
    "作为总分段标题，"
    "则它是父级 level:1；属于该分段的‘第一章 地球’、‘第二章 地图’必须改为 "
    "level:2；这些章下面的‘第一节’、‘第二节’必须改为 level:3。"
    "同理，‘中国地理’是 level:1，它下面的‘第一章 疆域和行政区划’、‘第二章 人口和民族’、‘第三章 地形’等必须是 level:2，"
    "这些章下面的‘第一节’、‘课堂练习’必须是 "
    "level:3。不要因为章节文字本身通常是一级，就忽略目录中更高层的无编号分段标题。"
    "例如本目录应形成：地球和地图(1) → 第一章 地球(2) → 第一节 地球的形状和经纬网(3)；"
    "地球和地图(1) → 第二章 地图(2) → 课堂练习(3)；中国地理(1) → 第一章 疆域和行政区划(2)。"
    "如果无编号分段标题右侧没有印刷页码，可以暂时输出 "
    "page:null，但仍必须保留其父级层次；不要删除它，也不要把后续章节提升为同级。"
    "不要把无编号分段标题误当成普通说明文字，也不要把它与相邻章节合并。‘第一节’、‘第二节’等明确属于所在章节的下一级；"
    "‘课堂练习’若在章节条目缩进下也属于所在章节的下一级。"
    "只输出目录中实际印刷的条目，按页面从上到下、从左到右的顺序输出，不要补写图片中不存在的标题。";

struct AiTocPocWork {
    HWND mainHwnd = nullptr;
    HWND owner = nullptr;
    HANDLE dialogToken = nullptr;
    const WCHAR* error = nullptr;
    EngineBase* engine = nullptr;
    Vec<int> pageNos;
    StrVec files;
    char* session = nullptr;
    char* jsonResponse = nullptr;
    HWND browser = nullptr;
    AiChatService service = AiChatService::Doubao;
    int firstPage = 0;
    int lastPage = 0;

    ~AiTocPocWork() {
        if (engine) {
            engine->Release();
        }
        str::Free(session);
        str::Free(jsonResponse);
    }
};

struct AiTocDetectWork {
    MainWindow* win = nullptr;
    EngineBase* engine = nullptr;
    HWND notifyHwnd = nullptr;
    HANDLE dialogToken = nullptr;
    Vec<int> pageNos;
    Vec<bool> initiallySelected;
    Vec<RenderedBitmap*> thumbnails;

    ~AiTocDetectWork() {
        if (engine) {
            engine->Release();
        }
        for (int i = 0; i < thumbnails.Size(); i++) {
            delete thumbnails[i];
        }
    }
};

struct AiTocDialog {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    HWND pagesEdit = nullptr;
    HWND status = nullptr;
    HWND progress = nullptr;
    HWND send = nullptr;
    HWND thumbnailPane = nullptr;
    Vec<HWND> pageChecks;
    Vec<HWND> thumbnailImages;
    int thumbnailScroll = 0;
    int thumbnailMaxScroll = 0;
    HFONT font = nullptr;
    bool busy = false;
    bool manualPages = false;
    bool settingPages = false;
    HANDLE token = nullptr;
    AiTocPocWork* work = nullptr;
    AiTocDetectWork* detectWork = nullptr;
    bool submitted = false;
    bool clipboardReading = false;
    DWORD clipboardSequence = 0;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH controlBrush = nullptr;
};

static constexpr const WCHAR* kAiTocToken = L"SumatraAiTocToken";
static constexpr const WCHAR* kAiTocDetectToken = L"SumatraAiTocDetectToken";
static UINT_PTR gAiTocToken = 0;
static constexpr const WCHAR* kAiTocDialogClass = L"SUMATRA_AI_TOC_DIALOG";
static constexpr const WCHAR* kAiTocThumbPaneClass = L"SUMATRA_AI_TOC_THUMB_PANE";
static constexpr UINT kMsgAiTocDetected = WM_APP + 72;
static constexpr UINT kMsgAiTocDetectProgress = WM_APP + 73;
static bool gAiTocClassesRegistered = false;

static void AiTocPocWorker(AiTocPocWork* work);
static void AiTocUploadFinished(AiTocPocWork* work);
static RenderedBitmap* RenderAiTocPage(EngineBase* engine, int pageNo);
static RenderedBitmap* RenderAiTocThumbnail(EngineBase* engine, int pageNo);

static void AiTocSetImportProgress(AiTocDialog* dlg, const WCHAR* text) {
    SetWindowTextW(dlg->status, text);
    ShowWindow(dlg->progress, SW_SHOW);
    SendMessageW(dlg->progress, PBM_SETMARQUEE, TRUE, 35);
    UpdateWindow(dlg->status);
    UpdateWindow(dlg->progress);
    UpdateWindow(dlg->hwnd);
}

static void AiTocHideImportProgress(AiTocDialog* dlg) {
    SendMessageW(dlg->progress, PBM_SETMARQUEE, FALSE, 0);
    ShowWindow(dlg->progress, SW_HIDE);
}

static AiChatService AiTocActiveService() {
    const char* provider = gGlobalPrefs ? gGlobalPrefs->aiChatProvider : nullptr;
    if (str::EqI(provider, "deepseek")) return AiChatService::DeepSeek;
    if (str::EqI(provider, "chatgpt")) return AiChatService::ChatGPT;
    return AiChatService::Doubao;
}

static void StartAiTocRender(MainWindow* win, const Vec<int>& pageNos, HWND owner) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    if (pageNos.Size() == 0 || pageNos.Size() > 20) {
        MessageBoxWarning(owner, _TRA("Choose between 1 and 20 PDF pages."), _TRA("AI Recognize Table of Contents"));
        return;
    }
    auto* work = new AiTocPocWork();
    work->mainHwnd = win->hwndFrame;
    work->owner = owner;
    work->dialogToken = GetPropW(owner, kAiTocToken);
    work->service = AiTocActiveService();
    work->engine = engine;
    engine->AddRef();
    for (int i = 0; i < pageNos.Size(); i++) {
        int pageNo = pageNos[i];
        if (pageNo < 1 || pageNo > engine->PageCount()) {
            delete work;
            return;
        }
        work->pageNos.Append(pageNo);
    }
    work->firstPage = work->pageNos[0];
    work->lastPage = work->pageNos.Last();
    RunAsync(MkFunc0<AiTocPocWork>(AiTocPocWorker, work), "AiTocPoc");
}

static void AiTocDetectionFinished(AiTocDetectWork* work) {
    if (GetPropW(work->notifyHwnd, kAiTocToken) == work->dialogToken) {
        SendMessageW(work->notifyHwnd, kMsgAiTocDetected, (WPARAM)work, 0);
    } else {
        delete work;
    }
}

static bool AiTocDetectionCanceled(void* context) {
    auto* work = (AiTocDetectWork*)context;
    return GetPropW(work->notifyHwnd, kAiTocDetectToken) != work->dialogToken;
}

static void AiTocDetectionProgress(void* context, int pageNo, int pageLimit) {
    auto* work = (AiTocDetectWork*)context;
    PostMessageW(work->notifyHwnd, kMsgAiTocDetectProgress, (WPARAM)pageNo, (LPARAM)pageLimit);
}

static void AiTocDetectWorker(AiTocDetectWork* work) {
    PtocFindCachedTocPages(work->engine, work->pageNos, true, true, AiTocDetectionCanceled, work,
                           AiTocDetectionProgress, work);
    if (work->pageNos.Size() > 0) {
        int first = work->pageNos[0];
        int last = work->pageNos.Last();
        work->pageNos.Reset();
        for (int pageNo = 1; pageNo <= work->engine->PageCount(); pageNo++) {
            work->pageNos.Append(pageNo);
            work->initiallySelected.Append(pageNo >= first && pageNo <= last);
            // Thumbnail bitmaps are rendered only when their row becomes
            // visible in the scroll pane, rather than blocking on every
            // page of a large PDF before the user can make a choice.
            work->thumbnails.Append(nullptr);
        }
    }
    uitask::Post(MkFunc0<AiTocDetectWork>(AiTocDetectionFinished, work), "AiTocDetected");
}

static RenderedBitmap* RenderAiTocPage(EngineBase* engine, int pageNo) {
    RectF box = engine->PageMediabox(pageNo);
    if (box.IsEmpty()) {
        return nullptr;
    }
    float dpi = engine->GetFileDPI();
    if (dpi <= 0.f) {
        dpi = 72.f;
    }
    float zoom = 200.f / dpi;
    float longest = box.dx > box.dy ? box.dx : box.dy;
    if (longest * zoom > 2800.f) {
        zoom = 2800.f / longest;
    }
    RenderPageArgs args(pageNo, zoom, 0, nullptr, RenderTarget::Export);
    return engine->RenderPage(args);
}

static RenderedBitmap* RenderAiTocThumbnail(EngineBase* engine, int pageNo) {
    RectF box = engine->PageMediabox(pageNo);
    if (box.IsEmpty()) {
        return nullptr;
    }
    float longest = box.dx > box.dy ? box.dx : box.dy;
    if (longest <= 0.f) {
        return nullptr;
    }
    // The dialog displays the bitmap at 120 px wide; rendering it close to
    // that size keeps detection responsive even for a long TOC spread.
    float zoom = 360.f / longest;
    RenderPageArgs args(pageNo, zoom, 0, nullptr, RenderTarget::Export);
    return engine->RenderPage(args);
}

static void AiTocPocWorker(AiTocPocWork* work) {
    defer {
        uitask::Post(MkFunc0<AiTocPocWork>(AiTocUploadFinished, work), "AiTocUploadFinished");
    };
    TempStr root = path::JoinTemp(GetTempDirTemp(), "SumatraPDF-Plus\\AI-TOC");
    TempStr session = path::JoinTemp(root, str::FormatTemp("poc-%u", GetTickCount()));
    if (!dir::CreateAll(session)) {
        work->error = L"无法创建图片文件夹，请检查磁盘空间后重试。";
        return;
    }
    work->session = str::Dup(session);
    for (int i = 0; i < work->pageNos.Size(); i++) {
        int pageNo = work->pageNos[i];
        RenderedBitmap* bmp = RenderAiTocPage(work->engine, pageNo);
        if (!bmp) {
            work->error = L"目录页图片生成失败，请重试或调整页码。";
            return;
        }
        TempStr path = path::JoinTemp(session, str::FormatTemp("toc_page_%04d.png", i + 1));
        bool ok = SaveBitmapAsPng(bmp->GetBitmap(), path);
        delete bmp;
        if (!ok) {
            work->error = L"图片保存失败，请检查磁盘空间后重试。";
            return;
        }
        work->files.Append(str::Dup(path));
    }
    bool reused = false;
    HWND browser = nullptr;
    if (GetPropW(work->owner, kAiTocToken) != work->dialogToken) return;
    if (!LaunchAiChatBrowser(work->service, &reused, &browser, nullptr)) {
        work->error = L"无法打开已配置的 AI 网页。";
        return;
    }
    // Doubao's first real message initializes a fresh conversation composer.
    // Bootstrap it before image paste; subsequent images are then attached to
    // the stable composer just as they are in a warm browser session.
    if (!reused) {
        logf("AI TOC: cold browser bootstrap message\n");
        if (!CopyTextToClipboard("hi") || !PasteAndSubmitAiChatWhenReady(work->service, browser, true)) {
            work->error = L"AI 网页已打开，但无法发送初始化消息。";
            work->browser = browser;
            return;
        }
        Sleep(2500);
    }
    if (!PasteAiChatFilesWhenReady(work->service, browser, !reused, work->files)) {
        logf("AI TOC: unable to send pages to browser; temp files kept at %s\n", session);
        work->error = L"自动发送未完成，可在 AI 输入框粘贴剪贴板内容后发送。";
        work->browser = browser;
        return;
    }
    work->browser = browser;
    logf("AI TOC: pasted %d detected TOC page images\n", work->files.Size());
}

static void SendAiTocPrompt(AiTocDialog* dlg) {
    if (!AddClipboardFormatListener(dlg->hwnd)) {
        SetWindowTextW(dlg->status, L"无法监听剪贴板，请关闭窗口后重试。提示词尚未发送。");
        return;
    }
    if (!CopyTextToClipboard(kAiTocPrompt) ||
        !PasteAndSubmitAiChatWhenReady(dlg->work->service, dlg->work->browser, false)) {
        RemoveClipboardFormatListener(dlg->hwnd);
        MessageBoxWarning(dlg->hwnd, _TRA("Unable to paste the AI prompt into the browser."),
                          _TRA("AI Recognize Table of Contents"));
        return;
    }
    logf("AI TOC: prompt submitted for pages %d-%d\n", dlg->work->firstPage, dlg->work->lastPage);
    bool copied = CopyAiChatPayloadToClipboard(dlg->work->files, kAiTocPrompt);
    dlg->submitted = true;
    dlg->clipboardSequence = GetClipboardSequenceNumber();
    SetTimer(dlg->hwnd, 1, 750, nullptr);
    SetWindowTextW(dlg->status, copied ? L"已执行发送，等待 AI 回复。复制回复后将自动导入目录。\r\n"
                                         L"图片与提示词已保留在剪贴板，可用于补贴。"
                                       : L"已执行发送，等待 AI 回复。复制回复后将自动导入目录。\r\n"
                                         L"剪贴板备份失败，不影响已执行的发送操作。");
    EnableWindow(dlg->send, FALSE);
    EnableWindow(dlg->pagesEdit, FALSE);
}

static TempStr AiTocGetClipboardText() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) {
        return nullptr;
    }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    SIZE_T bytes = h ? GlobalSize(h) : 0;
    WCHAR* text = bytes >= sizeof(WCHAR) && bytes <= 2 * 1024 * 1024 ? (WCHAR*)GlobalLock(h) : nullptr;
    if (text && text[bytes / sizeof(WCHAR) - 1] != 0) {
        GlobalUnlock(h);
        text = nullptr;
    }
    TempStr res = text ? ToUtf8Temp(text) : nullptr;
    if (text) {
        GlobalUnlock(h);
    }
    CloseClipboard();
    return res;
}

static bool IsAiTocJsonCandidate(const char* text) {
    if (!text) {
        return false;
    }
    while (*text && *text <= ' ') {
        text++;
    }
    return *text == '{' && str::Find(text, "\"items\"");
}

struct AiTocJsonItem {
    char* title = nullptr;
    int printedPage = 0;
    bool hasPage = false;
    int level = 1;

    void Free() {
        str::Free(title);
        title = nullptr;
    }
};

struct AiTocJsonVisitor : json::ValueVisitor {
    Vec<AiTocJsonItem> items;

    ~AiTocJsonVisitor() {
        for (int i = 0; i < items.Size(); i++) {
            items[i].Free();
        }
    }

    AiTocJsonItem& ItemAt(int idx) {
        while (items.Size() <= idx) {
            items.Append({});
        }
        return items[idx];
    }

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (!str::StartsWith(path, "/items[")) {
            return true;
        }
        const char* p = path + str::Len("/items[");
        int idx = 0;
        bool hasIndex = false;
        while (*p >= '0' && *p <= '9') {
            if (idx >= 10000) return false;
            idx = idx * 10 + (*p - '0');
            p++;
            hasIndex = true;
        }
        if (idx >= 10000) return false;
        if (!hasIndex || *p++ != ']' || *p++ != '/') {
            return true;
        }
        AiTocJsonItem& item = ItemAt(idx);
        if (str::Eq(p, "title") && type == json::Type::String) {
            str::Free(item.title);
            item.title = str::Dup(value);
        } else if (str::Eq(p, "page") && type == json::Type::Number) {
            item.printedPage = ParseInt(value);
            item.hasPage = item.printedPage > 0;
        } else if (str::Eq(p, "level") && type == json::Type::Number) {
            item.level = ParseInt(value);
        }
        return true;
    }
};

// AI can return structural headings such as "第一编" with page: null. Keep
// them in the bookmark tree and initially point them at their first child.
static int AiTocFillMissingPages(ExtractedTocItem* item) {
    int firstChildPage = 0;
    int firstChildPrintedPage = 0;
    for (int i = 0; i < item->children.Size(); i++) {
        auto* child = item->children[i];
        int childPage = AiTocFillMissingPages(child);
        if (!firstChildPage && childPage > 0) {
            firstChildPage = childPage;
            firstChildPrintedPage = child->printedPage;
        }
    }
    if (item->pageNo < 1 && firstChildPage > 0) {
        item->pageNo = firstChildPage;
        item->printedPage = firstChildPrintedPage;
    }
    return item->pageNo;
}

static bool AiTocImportJson(AiTocDialog* dlg, const char* text) {
    AiTocSetImportProgress(dlg, L"正在导入目录：解析 AI 返回内容…");
    AiTocJsonVisitor parsed;
    if (!json::Parse(text, &parsed)) {
        AiTocHideImportProgress(dlg);
        MessageBoxWarning(dlg->hwnd, "AI returned invalid JSON.", _TRA("AI Recognize Table of Contents"));
        return false;
    }
    Vec<ExtractedTocItem*> roots;
    Vec<ExtractedTocItem*> stack;
    AiTocSetImportProgress(dlg, L"正在导入目录：整理书签层级…");
    for (int i = 0; i < parsed.items.Size(); i++) {
        AiTocJsonItem& src = parsed.items[i];
        if (!src.title || !src.title[0]) {
            continue;
        }
        int wantLevel = src.level < 1 ? 1 : src.level;
        if (wantLevel > stack.Size() + 1) {
            wantLevel = stack.Size() + 1;
        }
        while (stack.Size() >= wantLevel) {
            stack.RemoveAt(stack.Size() - 1);
        }
        auto* item = new ExtractedTocItem();
        item->title = str::Dup(src.title);
        item->rawTitle = str::Dup(src.title);
        item->printedPage = src.hasPage ? src.printedPage : 0;
        // The smoke test assumes printed page 1 follows the selected TOC
        // spread. The calibration bar remains the final authority before Save.
        item->pageNo = src.hasPage ? dlg->work->lastPage + src.printedPage : 0;
        item->level = stack.Size() + 1;
        item->confidence = 80;
        item->source = ExtractedTocSource::PrintedToc;
        item->tocPageNo = dlg->work->firstPage;
        if (stack.Size() > 0) {
            item->parent = stack.Last();
            item->parent->children.Append(item);
        } else {
            roots.Append(item);
        }
        stack.Append(item);
    }
    for (int i = 0; i < roots.Size(); i++) {
        AiTocFillMissingPages(roots[i]);
    }
    MainWindow* win = FindMainWindowByHwnd(dlg->work->mainHwnd);
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    AiTocSetImportProgress(dlg, L"正在导入目录：创建左侧书签校准界面…");
    if (roots.Size() == 0 || !dm || dm->GetEngine() != dlg->work->engine ||
        !StartTocCalib(win, roots, dlg->work->engine, true, true)) {
        AiTocHideImportProgress(dlg);
        DeleteExtractedTocItems(roots);
        MessageBoxWarning(dlg->hwnd, "The JSON contains no importable table-of-contents items.",
                          _TRA("AI Recognize Table of Contents"));
        return false;
    }
    logf("AI TOC: imported JSON items=%d pages=%d-%d\n", stack.Size(), dlg->work->firstPage, dlg->work->lastPage);
    AiTocSetImportProgress(dlg, L"目录导入完成，正在打开书签校准…");
    return true;
}

struct AiTocClipboardRead {
    HWND hwnd;
    HANDLE token;
    DWORD sequence;
    char* text = nullptr;
    bool retry = false;
};

static void AiTocClipboardReady(AiTocClipboardRead* read) {
    defer {
        str::Free(read->text);
        delete read;
    };
    if (GetPropW(read->hwnd, kAiTocToken) != read->token) return;
    auto* dlg = (AiTocDialog*)GetWindowLongPtrW(read->hwnd, GWLP_USERDATA);
    dlg->clipboardReading = false;
    if (!dlg->submitted) return;
    if (read->retry) {
        SetWindowTextW(dlg->status, L"剪贴板暂时不可读，正在重试。也可以再次复制 AI 回复。");
        return;
    }
    dlg->clipboardSequence = read->sequence;
    if (!read->text) return;
    // Accept code fences and explanatory text surrounding the JSON object.
    char* start = strchr(read->text, '{');
    char* end = strrchr(read->text, '}');
    if (!start || !end || end < start) return;
    end[1] = 0;
    if (!IsAiTocJsonCandidate(start)) return;
    if (AiTocImportJson(dlg, start)) {
        dlg->submitted = false;
        DestroyWindow(read->hwnd);
    } else {
        SetWindowTextW(dlg->status, L"目录导入失败，请复制完整的 AI 目录 JSON 后重试。");
    }
}

static void AiTocReadClipboardWorker(AiTocClipboardRead* read) {
    // Delayed clipboard rendering can wait on the browser. Never perform this
    // cross-process read inside the application's window procedure.
    TempStr text = AiTocGetClipboardText();
    read->text = text ? str::Dup(text) : nullptr;
    read->retry = !text && IsClipboardFormatAvailable(CF_UNICODETEXT);
    uitask::Post(MkFunc0<AiTocClipboardRead>(AiTocClipboardReady, read), "AiTocClipboard");
}

static char* AiTocFormatPages(const Vec<int>& pages) {
    StrBuilder text;
    for (int i = 0; i < pages.Size();) {
        int first = pages[i];
        int last = first;
        while (i + 1 < pages.Size() && pages[i + 1] == last + 1) {
            last = pages[++i];
        }
        if (!text.IsEmpty()) {
            text.Append(", ");
        }
        if (first == last) {
            text.AppendFmt("%d", first);
        } else {
            text.AppendFmt("%d-%d", first, last);
        }
        i++;
    }
    return text.StealData();
}

static char* AiTocFormatSelectedPages(const Vec<int>& pages, const Vec<bool>& selected) {
    Vec<int> selectedPages;
    for (int i = 0; i < pages.Size() && i < selected.Size(); i++) {
        if (selected[i]) {
            selectedPages.Append(pages[i]);
        }
    }
    return AiTocFormatPages(selectedPages);
}

// Size from the client area: the caption and borders must not consume the
// space reserved for controls. Scale the layout with the actual GUI font.
static void SizeAiTocDialog(HWND hwnd, HFONT font, int width, int height) {
    HDC dc = GetDC(hwnd);
    HGDIOBJ oldFont = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd, dc);
    int unit = metrics.tmHeight > 16 ? metrics.tmHeight : 16;
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect{};
        GetWindowRect(child, &rect);
        MapWindowPoints(nullptr, hwnd, (POINT*)&rect, 2);
        MoveWindow(child, MulDiv(rect.left, unit, 16), MulDiv(rect.top, unit, 16),
                   MulDiv(rect.right - rect.left, unit, 16), MulDiv(rect.bottom - rect.top, unit, 16), TRUE);
    }
    RECT outer{}, client{};
    GetWindowRect(hwnd, &outer);
    GetClientRect(hwnd, &client);
    SetWindowPos(hwnd, nullptr, 0, 0, MulDiv(width, unit, 16) + outer.right - outer.left - client.right,
                 MulDiv(height, unit, 16) + outer.bottom - outer.top - client.bottom,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void AiTocCenterOverMainWindow(AiTocDialog* dlg) {
    if (!dlg || !dlg->win || !dlg->win->hwndFrame) {
        return;
    }
    RECT owner{};
    RECT dialog{};
    GetWindowRect(dlg->win->hwndFrame, &owner);
    GetWindowRect(dlg->hwnd, &dialog);
    int width = dialog.right - dialog.left;
    int height = dialog.bottom - dialog.top;
    int x = owner.left + ((owner.right - owner.left) - width) / 2;
    int y = owner.top + ((owner.bottom - owner.top) - height) / 2;
    SetWindowPos(dlg->hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static LRESULT AiTocColorControl(AiTocDialog* dlg, HDC dc, HWND control) {
    bool edit = control == dlg->pagesEdit;
    SetTextColor(dc, ThemeWindowTextColor());
    SetBkColor(dc, edit ? ThemeWindowControlBackgroundColor() : ThemeWindowBackgroundColor());
    return (LRESULT)(edit ? dlg->controlBrush : dlg->backgroundBrush);
}

void RefreshAiTocWindowsTheme() {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM) -> BOOL {
            if (!GetPropW(hwnd, kAiTocToken)) return TRUE;
            auto* dlg = (AiTocDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            DeleteObject(dlg->backgroundBrush);
            DeleteObject(dlg->controlBrush);
            dlg->backgroundBrush = CreateSolidBrush(ThemeWindowBackgroundColor());
            dlg->controlBrush = CreateSolidBrush(ThemeWindowControlBackgroundColor());
            if (UseDarkModeLib()) DarkMode::setChildCtrlsSubclassAndTheme(hwnd);
            if (UseDarkModeLib() && dlg->thumbnailPane) DarkMode::setDarkScrollBar(dlg->thumbnailPane);
            UpdateWindowCaptionTheme(hwnd);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            return TRUE;
        },
        0);
}

static constexpr int kAiTocThumbWidth = 260;
static constexpr int kAiTocThumbHeight = 360;
static constexpr int kAiTocThumbPitch = 400;

static void AiTocCreateThumbnailSlot(AiTocDialog* dlg) {
    HWND image = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | SS_OWNERDRAW, 0, 0, 1, 1, dlg->thumbnailPane,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND check = CreateWindowExW(0, L"BUTTON", nullptr, WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 1, 1, dlg->thumbnailPane,
                                 (HMENU)(INT_PTR)(2000 + dlg->pageChecks.Size()), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(check, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    dlg->thumbnailImages.Append(image);
    dlg->pageChecks.Append(check);
    if (UseDarkModeLib()) DarkMode::setChildCtrlsSubclassAndTheme(dlg->thumbnailPane);
}

static void AiTocLayoutThumbnails(AiTocDialog* dlg) {
    if (!dlg->thumbnailPane || !dlg->detectWork) {
        return;
    }
    RECT client{};
    GetClientRect(dlg->thumbnailPane, &client);
    int viewportHeight = client.bottom;
    int rows = (dlg->detectWork->pageNos.Size() + 1) / 2;
    int contentHeight = rows * kAiTocThumbPitch;
    dlg->thumbnailMaxScroll = contentHeight > viewportHeight ? contentHeight - viewportHeight : 0;
    if (dlg->thumbnailScroll > dlg->thumbnailMaxScroll) {
        dlg->thumbnailScroll = dlg->thumbnailMaxScroll;
    }
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = contentHeight > 0 ? contentHeight - 1 : 0;
    info.nPage = viewportHeight > 0 ? (UINT)viewportHeight : 0;
    info.nPos = dlg->thumbnailScroll;
    SetScrollInfo(dlg->thumbnailPane, SB_VERT, &info, TRUE);
}

static void AiTocRenderVisibleThumbnails(AiTocDialog* dlg) {
    if (!dlg->detectWork || !dlg->thumbnailPane) {
        return;
    }
    RECT client{};
    GetClientRect(dlg->thumbnailPane, &client);
    int firstRow = dlg->thumbnailScroll / kAiTocThumbPitch;
    int lastRow = (dlg->thumbnailScroll + client.bottom + kAiTocThumbPitch - 1) / kAiTocThumbPitch;
    firstRow = firstRow > 0 ? firstRow - 1 : 0;
    int first = firstRow * 2;
    int last = (lastRow + 1) * 2;
    if (last > dlg->detectWork->pageNos.Size()) {
        last = dlg->detectWork->pageNos.Size();
    }
    int slots = last - first;
    while (dlg->thumbnailImages.Size() < slots) {
        AiTocCreateThumbnailSlot(dlg);
    }
    for (int slot = 0; slot < dlg->thumbnailImages.Size(); slot++) {
        if (slot >= slots) {
            ShowWindow(dlg->thumbnailImages[slot], SW_HIDE);
            ShowWindow(dlg->pageChecks[slot], SW_HIDE);
            continue;
        }
        int i = first + slot;
        int col = i % 2;
        int row = i / 2;
        int columnWidth = client.right / 2;
        int x = col * columnWidth + (columnWidth - kAiTocThumbWidth) / 2;
        int y = row * kAiTocThumbPitch - dlg->thumbnailScroll;
        MoveWindow(dlg->thumbnailImages[slot], x, y, kAiTocThumbWidth, kAiTocThumbHeight, FALSE);
        MoveWindow(dlg->pageChecks[slot], x, y + kAiTocThumbHeight + 4, kAiTocThumbWidth, 25, FALSE);
        TempStr label = str::FormatTemp("第 %d 页", dlg->detectWork->pageNos[i]);
        SetWindowTextW(dlg->pageChecks[slot], ToWStrTemp(label));
        SetWindowLongPtrW(dlg->pageChecks[slot], GWLP_USERDATA, i);
        bool selected = i < dlg->detectWork->initiallySelected.Size() && dlg->detectWork->initiallySelected[i];
        SendMessageW(dlg->pageChecks[slot], BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
        if (i < dlg->detectWork->thumbnails.Size() && !dlg->detectWork->thumbnails[i]) {
            dlg->detectWork->thumbnails[i] = RenderAiTocThumbnail(dlg->detectWork->engine, dlg->detectWork->pageNos[i]);
        }
        auto* bitmap = i < dlg->detectWork->thumbnails.Size() ? dlg->detectWork->thumbnails[i] : nullptr;
        SetWindowLongPtrW(dlg->thumbnailImages[slot], GWLP_USERDATA, (LONG_PTR)bitmap);
        InvalidateRect(dlg->thumbnailImages[slot], nullptr, TRUE);
        ShowWindow(dlg->thumbnailImages[slot], SW_SHOWNA);
        ShowWindow(dlg->pageChecks[slot], SW_SHOWNA);
    }
}

static void AiTocScrollThumbnails(AiTocDialog* dlg, int nextPos) {
    if (nextPos < 0) {
        nextPos = 0;
    }
    if (nextPos > dlg->thumbnailMaxScroll) {
        nextPos = dlg->thumbnailMaxScroll;
    }
    if (nextPos == dlg->thumbnailScroll) {
        return;
    }
    dlg->thumbnailScroll = nextPos;
    AiTocLayoutThumbnails(dlg);
    AiTocRenderVisibleThumbnails(dlg);
    RedrawWindow(dlg->thumbnailPane, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void AiTocSetPagesTextFromChecks(AiTocDialog* dlg) {
    if (!dlg->detectWork) {
        return;
    }
    Vec<int> pages;
    for (int i = 0; i < dlg->detectWork->pageNos.Size(); i++) {
        if (i < dlg->detectWork->initiallySelected.Size() && dlg->detectWork->initiallySelected[i]) {
            pages.Append(dlg->detectWork->pageNos[i]);
        }
    }
    AutoFree formatted(AiTocFormatPages(pages));
    dlg->settingPages = true;
    SetWindowTextW(dlg->pagesEdit, ToWStrTemp(formatted));
    dlg->settingPages = false;
}

static void AiTocSetChecksFromPageText(AiTocDialog* dlg) {
    if (!dlg->detectWork) {
        return;
    }
    DisplayModel* dm = dlg->win ? dlg->win->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    Str text = GetWindowTextTemp(dlg->pagesEdit);
    Vec<PageRange> ranges;
    if (!ParsePageRanges(text.s, ranges)) {
        return;
    }
    for (int i = 0; i < dlg->detectWork->pageNos.Size(); i++) {
        int pageNo = dlg->detectWork->pageNos[i];
        bool select = false;
        for (int j = 0; j < ranges.Size(); j++) {
            int first = ranges[j].start > 1 ? ranges[j].start : 1;
            int last = ranges[j].end < engine->PageCount() ? ranges[j].end : engine->PageCount();
            if (pageNo >= first && pageNo <= last) {
                select = true;
                break;
            }
        }
        dlg->detectWork->initiallySelected[i] = select;
    }
    AiTocRenderVisibleThumbnails(dlg);
}

static LRESULT CALLBACK AiTocThumbPaneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* dlg = (AiTocDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        dlg = (AiTocDialog*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
    }
    if (!dlg) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect((HDC)wp, &rect, dlg->backgroundBrush);
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN) {
        return AiTocColorControl(dlg, (HDC)wp, (HWND)lp);
    }
    if (msg == WM_COMMAND) {
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    if (msg == WM_DRAWITEM) {
        auto* draw = (DRAWITEMSTRUCT*)lp;
        auto* bitmap = (RenderedBitmap*)GetWindowLongPtrW(draw->hwndItem, GWLP_USERDATA);
        FillRect(draw->hDC, &draw->rcItem, dlg->backgroundBrush);
        if (bitmap) {
            HBITMAP image = bitmap->GetBitmap();
            BITMAP size{};
            GetObjectW(image, sizeof(size), &size);
            int width = draw->rcItem.right;
            int height = draw->rcItem.bottom;
            int dw = width;
            int dh = MulDiv(size.bmHeight, width, size.bmWidth);
            if (dh > height) {
                dh = height;
                dw = MulDiv(size.bmWidth, height, size.bmHeight);
            }
            HDC source = CreateCompatibleDC(draw->hDC);
            HGDIOBJ old = SelectObject(source, image);
            SetStretchBltMode(draw->hDC, HALFTONE);
            SetBrushOrgEx(draw->hDC, 0, 0, nullptr);
            // Invert only the preview; keep the cached page and AI upload images unchanged.
            StretchBlt(draw->hDC, (width - dw) / 2, (height - dh) / 2, dw, dh, source, 0, 0, size.bmWidth,
                       size.bmHeight, ThemeUsesDarkChrome() ? NOTSRCCOPY : SRCCOPY);
            SelectObject(source, old);
            DeleteDC(source);
        }
        return TRUE;
    }
    if (msg == WM_VSCROLL) {
        int nextPos = dlg->thumbnailScroll;
        switch (LOWORD(wp)) {
            case SB_LINEUP:
                nextPos -= 45;
                break;
            case SB_LINEDOWN:
                nextPos += 45;
                break;
            case SB_PAGEUP:
                nextPos -= 320;
                break;
            case SB_PAGEDOWN:
                nextPos += 320;
                break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
                nextPos = HIWORD(wp);
                break;
            case SB_TOP:
                nextPos = 0;
                break;
            case SB_BOTTOM:
                nextPos = dlg->thumbnailMaxScroll;
                break;
        }
        AiTocScrollThumbnails(dlg, nextPos);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        AiTocScrollThumbnails(dlg, dlg->thumbnailScroll - (delta / WHEEL_DELTA) * 90);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK AiTocDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AiTocDialog* dlg = nullptr;
    if (msg == WM_CREATE) {
        dlg = (AiTocDialog*)((CREATESTRUCTW*)lp)->lpCreateParams;
        dlg->hwnd = hwnd;
        dlg->backgroundBrush = CreateSolidBrush(ThemeWindowBackgroundColor());
        dlg->controlBrush = CreateSolidBrush(ThemeWindowControlBackgroundColor());
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
        return 0;
    }
    dlg = (AiTocDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect((HDC)wp, &rect, dlg->backgroundBrush);
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN) {
        return AiTocColorControl(dlg, (HDC)wp, (HWND)lp);
    }
    if (msg == WM_COMMAND && (HWND)lp == dlg->pagesEdit && HIWORD(wp) == EN_CHANGE && !dlg->settingPages) {
        if (dlg->detectWork && dlg->detectWork->pageNos.Size() > 0) {
            AiTocSetChecksFromPageText(dlg);
            SetWindowTextW(dlg->status, L"页码与下方缩略图已同步；确认后点击“发送”。");
            return 0;
        }
        dlg->manualPages = true;
        RemovePropW(hwnd, kAiTocDetectToken);
        if (!dlg->busy && !dlg->submitted) {
            EnableWindow(dlg->send, TRUE);
            SetWindowTextW(dlg->status, L"将按输入的 PDF 页码逐页取图，点击“发送”开始。");
        }
        return 0;
    }
    if (msg == kMsgAiTocDetected) {
        auto* work = (AiTocDetectWork*)wp;
        if (dlg->manualPages || dlg->busy || dlg->submitted) {
            delete work;
            return 0;
        }
        dlg->settingPages = true;
        delete dlg->detectWork;
        dlg->detectWork = work;
        if (work->pageNos.Size() == 0) {
            SetWindowTextW(dlg->pagesEdit, L"");
            SetWindowTextW(dlg->status, L"未找到目录页，请手动填写上方页码。");
        } else {
            SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
            AutoFree detected(AiTocFormatSelectedPages(work->pageNos, work->initiallySelected));
            SetWindowTextW(dlg->pagesEdit, ToWStrTemp(detected));
            AiTocHideImportProgress(dlg);
            RECT statusRect{};
            GetWindowRect(dlg->status, &statusRect);
            MapWindowPoints(nullptr, hwnd, (POINT*)&statusRect, 2);
            int margin = statusRect.left;
            int lineHeight = statusRect.bottom - statusRect.top;
            RECT outer{}, client{};
            GetWindowRect(hwnd, &outer);
            GetClientRect(hwnd, &client);
            int width = client.right > 720 ? client.right : 720;
            int height = 800;
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                int available =
                    monitor.rcWork.bottom - monitor.rcWork.top - (outer.bottom - outer.top - client.bottom) - 24;
                height = height < available ? height : available;
            }
            SetWindowPos(hwnd, nullptr, 0, 0, width + outer.right - outer.left - client.right,
                         height + outer.bottom - outer.top - client.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetClientRect(hwnd, &client);
            MoveWindow(dlg->status, margin, statusRect.top, client.right - 2 * margin, 2 * lineHeight, TRUE);
            int paneTop = statusRect.top + 2 * lineHeight + 12;
            int buttonHeight = lineHeight > 28 ? lineHeight : 28;
            int buttonTop = client.bottom - margin - buttonHeight;
            dlg->thumbnailPane = CreateWindowExW(
                0, kAiTocThumbPaneClass, nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, margin, paneTop,
                client.right - 2 * margin, buttonTop - 16 - paneTop, hwnd, nullptr, GetModuleHandleW(nullptr), dlg);
            if (UseDarkModeLib()) {
                DarkMode::setChildCtrlsTheme(dlg->thumbnailPane);
                DarkMode::setDarkScrollBar(dlg->thumbnailPane);
            }
            SetWindowTextW(dlg->status, L"已自动勾选目录页；可滚动查看缩略图并补选或取消，再点击“发送”。");
            RECT edit{};
            GetWindowRect(dlg->pagesEdit, &edit);
            MapWindowPoints(nullptr, hwnd, (POINT*)&edit, 2);
            MoveWindow(dlg->pagesEdit, margin, edit.top, client.right - 2 * margin, edit.bottom - edit.top, TRUE);
            MoveWindow(dlg->progress, margin, paneTop - 10, client.right - 2 * margin, 6, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDCANCEL), client.right - margin - 210, buttonTop, 100, buttonHeight, TRUE);
            MoveWindow(dlg->send, client.right - margin - 100, buttonTop, 100, buttonHeight, TRUE);
            for (int i = 0; i < work->initiallySelected.Size(); i++) {
                if (work->initiallySelected[i]) {
                    dlg->thumbnailScroll = (i / 2) * kAiTocThumbPitch;
                    break;
                }
            }
            AiTocLayoutThumbnails(dlg);
            AiTocRenderVisibleThumbnails(dlg);
            AiTocCenterOverMainWindow(dlg);
            SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        EnableWindow(dlg->send, TRUE);
        dlg->settingPages = false;
        return 0;
    }
    if (msg == kMsgAiTocDetectProgress && !dlg->detectWork && !dlg->manualPages && !dlg->busy && !dlg->submitted) {
        SetWindowTextW(dlg->status, ToWStrTemp(str::FormatTemp("正在扫描目录页：第 %d / %d 页…", (int)wp, (int)lp)));
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) >= 2000 && LOWORD(wp) < 2000 + dlg->pageChecks.Size() &&
        HIWORD(wp) == BN_CLICKED && dlg->detectWork) {
        int index = (int)GetWindowLongPtrW((HWND)lp, GWLP_USERDATA);
        if (index >= 0 && index < dlg->detectWork->initiallySelected.Size()) {
            dlg->detectWork->initiallySelected[index] = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
        }
        AiTocSetPagesTextFromChecks(dlg);
        SetWindowTextW(dlg->status, L"页码与下方缩略图已同步；确认后点击“发送”。");
        return 0;
    }
    if (msg == WM_VSCROLL && dlg->thumbnailImages.Size() > 0) {
        int nextPos = dlg->thumbnailScroll;
        switch (LOWORD(wp)) {
            case SB_LINEUP:
                nextPos -= 45;
                break;
            case SB_LINEDOWN:
                nextPos += 45;
                break;
            case SB_PAGEUP:
                nextPos -= 320;
                break;
            case SB_PAGEDOWN:
                nextPos += 320;
                break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
                nextPos = HIWORD(wp);
                break;
            case SB_TOP:
                nextPos = 0;
                break;
            case SB_BOTTOM:
                nextPos = dlg->thumbnailMaxScroll;
                break;
        }
        AiTocScrollThumbnails(dlg, nextPos);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL && dlg->thumbnailImages.Size() > 0) {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        AiTocScrollThumbnails(dlg, dlg->thumbnailScroll - (delta / WHEEL_DELTA) * 90);
        return 0;
    }
    if ((msg == WM_CLIPBOARDUPDATE || (msg == WM_TIMER && wp == 1)) && dlg->submitted) {
        DWORD sequence = GetClipboardSequenceNumber();
        if (!dlg->clipboardReading && sequence != dlg->clipboardSequence) {
            dlg->clipboardReading = true;
            auto* read = new AiTocClipboardRead{hwnd, dlg->token, sequence};
            RunAsync(MkFunc0<AiTocClipboardRead>(AiTocReadClipboardWorker, read), "AiTocClipboardRead");
        }
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == 1001 && HIWORD(wp) == BN_CLICKED && !dlg->busy && !dlg->submitted) {
        DisplayModel* dm = dlg->win ? dlg->win->AsFixed() : nullptr;
        EngineBase* engine = dm ? dm->GetEngine() : nullptr;
        Vec<int> pages;
        if (dlg->detectWork && dlg->detectWork->pageNos.Size() > 0) {
            for (int i = 0; i < dlg->detectWork->pageNos.Size(); i++) {
                if (i < dlg->detectWork->initiallySelected.Size() && dlg->detectWork->initiallySelected[i]) {
                    pages.Append(dlg->detectWork->pageNos[i]);
                }
            }
        } else if (engine) {
            Str pageText = GetWindowTextTemp(dlg->pagesEdit);
            Vec<PageRange> ranges;
            if (!ParsePageRanges(pageText.s, ranges)) {
                MessageBoxWarning(hwnd, _TRA("Choose between 1 and 20 PDF pages."),
                                  _TRA("AI Recognize Table of Contents"));
                return 0;
            }
            for (int i = 0; i < ranges.Size(); i++) {
                int first = ranges[i].start > 1 ? ranges[i].start : 1;
                int last = ranges[i].end < engine->PageCount() ? ranges[i].end : engine->PageCount();
                for (int pageNo = first; pageNo <= last && pages.Size() <= 20; pageNo++) {
                    pages.Append(pageNo);
                }
            }
        }
        if (pages.Size() == 0 || pages.Size() > 20) {
            MessageBoxWarning(hwnd, _TRA("Choose between 1 and 20 PDF pages."), _TRA("AI Recognize Table of Contents"));
            return 0;
        }
        dlg->busy = true;
        RemovePropW(hwnd, kAiTocDetectToken);
        EnableWindow(dlg->send, FALSE);
        EnableWindow(dlg->pagesEdit, FALSE);
        SetWindowTextW(dlg->status, ToWStrTemp(str::FormatTemp("正在准备 %d 张目录图片并发送…", pages.Size())));
        StartAiTocRender(dlg->win, pages, hwnd);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
    if (msg == WM_CLOSE) DestroyWindow(hwnd);
    if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, 1);
        RemovePropW(hwnd, kAiTocToken);
        RemovePropW(hwnd, kAiTocDetectToken);
        RemoveClipboardFormatListener(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete dlg->work;
        delete dlg->detectWork;
        DeleteObject(dlg->backgroundBrush);
        DeleteObject(dlg->controlBrush);
        delete dlg;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RegisterAiTocClasses() {
    if (gAiTocClassesRegistered) {
        return;
    }
    HINSTANCE h = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = h;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kAiTocDialogClass;
    wc.lpfnWndProc = AiTocDialogProc;
    RegisterClassExW(&wc);
    wc.lpszClassName = kAiTocThumbPaneClass;
    wc.lpfnWndProc = AiTocThumbPaneProc;
    RegisterClassExW(&wc);
    gAiTocClassesRegistered = true;
}

static void AiTocUploadFinished(AiTocPocWork* work) {
    if (GetPropW(work->owner, kAiTocToken) != work->dialogToken) {
        delete work;
        return;
    }
    auto* dlg = (AiTocDialog*)GetWindowLongPtrW(work->owner, GWLP_USERDATA);
    delete dlg->work;
    dlg->work = work;
    dlg->busy = false;
    EnableWindow(dlg->pagesEdit, work->error && !work->browser);
    EnableWindow(dlg->send, TRUE);
    SetWindowTextW(dlg->send, L"发送");
    if (work->error) {
        SetWindowTextW(dlg->status, work->error);
        if (work->files.Size() == work->pageNos.Size() && CopyAiChatPayloadToClipboard(work->files, kAiTocPrompt)) {
            if (AddClipboardFormatListener(dlg->hwnd)) {
                dlg->submitted = true;
                EnableWindow(dlg->send, FALSE);
                SetWindowTextW(dlg->status,
                               L"自动发送未完成。图片与提示词已放入剪贴板，\r\n"
                               L"可在 AI 网页粘贴发送；复制回复后将自动导入目录。");
            }
        }
    } else {
        SetWindowTextW(dlg->status, L"正在发送提示词…");
        SendAiTocPrompt(dlg);
    }
}

void StartAiTocProofOfConcept(MainWindow* win) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    if (!dm || !dm->GetEngine()) {
        return;
    }
    RegisterAiTocClasses();
    auto* dlg = new AiTocDialog();
    dlg->win = win;
    dlg->font = GetDefaultGuiFont();
    HINSTANCE h = GetModuleHandleW(nullptr);
    HWND hwnd =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kAiTocDialogClass, _TRW("AI Recognize Table of Contents"),
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 520,
                        190, win->hwndFrame, nullptr, h, dlg);
    if (!hwnd) {
        delete dlg;
        return;
    }
    dlg->token = (HANDLE)++gAiTocToken;
    SetPropW(hwnd, kAiTocToken, dlg->token);
    SetPropW(hwnd, kAiTocDetectToken, dlg->token);
    HWND label = CreateWindowExW(0, L"STATIC", L"目录所在的 PDF 页码", WS_CHILD | WS_VISIBLE | SS_LEFT, 18, 18, 470, 22,
                                 hwnd, nullptr, h, nullptr);
    SendMessageW(label, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    dlg->pagesEdit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 18, 46,
                                     480, 25, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->pagesEdit, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    SendMessageW(dlg->pagesEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"例如：3-4, 7（使用 PDF 页码，而非书上印刷页码）");
    dlg->status = CreateWindowExW(0, L"STATIC", L"正在分析文档前段的印刷目录页…", WS_CHILD | WS_VISIBLE | SS_LEFT, 18,
                                  80, 480, 26, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->status, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    dlg->progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | PBS_MARQUEE, 18, 150, 480, 16, hwnd,
                                    nullptr, h, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 290, 185, 95, 28,
                                  hwnd, (HMENU)IDCANCEL, h, nullptr);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    dlg->send = CreateWindowExW(0, L"BUTTON", L"发送", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 395, 185, 103, 28,
                                hwnd, (HMENU)1001, h, nullptr);
    SendMessageW(dlg->send, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    EnableWindow(dlg->send, FALSE);
    SizeAiTocDialog(hwnd, dlg->font, 516, 235);
    if (UseDarkModeLib()) {
        DarkMode::setDarkWndNotifySafe(hwnd);
        DarkMode::setChildCtrlsTheme(hwnd);
    }
    UpdateWindowCaptionTheme(hwnd);
    AiTocCenterOverMainWindow(dlg);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    auto* work = new AiTocDetectWork();
    work->win = win;
    work->engine = dm->GetEngine();
    work->engine->AddRef();
    work->notifyHwnd = hwnd;
    work->dialogToken = dlg->token;
    RunAsync(MkFunc0<AiTocDetectWork>(AiTocDetectWorker, work), "AiTocDetect");
}
