/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/JsonParser.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "GlobalPrefs.h"
#include "DocProperties.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "ExtractPdfToc.h"
#include "TocExtraction.h"
#include "TocCalib.h"
#include "TableOfContents.h"
#include "TocStructureScan.h"
#include "TocAiPrompts.h"
#include "OcrService.h"
#include "DisplayModel.h"
#include "RenderCache.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "SumatraDialogs.h"
#include "Flags.h"
#include "ImageSaveCropResize.h"
#include "Notifications.h"
#include "Selection.h"
#include "Translations.h"
#include "AiToc.h"
#include "AppDialogTheme.h"
#include "DarkModeSubclass.h"
#include "Theme.h"
#include "PdfDarkMode.h"

static const char* kAiTocPrompt =
    "你正在帮助 PDF 阅读器恢复一本扫描书籍中的印刷目录。图片按原书顺序排列。请根据视觉布局、缩进、"
    "字体、粗细、居中位置、上下留白、点线、页码和双栏关系恢复目录结构。只返回合法 JSON，不要解释或 Markdown。格式必须是"
    "{\"items\":[{\"title\":\"章节标题\",\"page\":12,\"level\":1}]}。"
    "page 是目录中印刷的页码：阿拉伯数字用 JSON 数字（如 12）；罗马数字或附录页码用字符串（如 \"xiv\"、\"R1\"）；"
    "没有页码时用 null。"
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
    "同一行里若有多个带括号页码的小条目，必须逐条拆开，禁止合并成一个标题。"
    "例如一行「一、多元函数概念(1) 二、二元函数的极限(5) 三、二元函数的连续性(8) 习题8-1(11)」要输出四条，"
    "而不是一条 title 里连写全部。"
    "每个「一、」「二、」「三、」等条目单独一条 item：title 保留编号，写成「一、多元函数概念」，"
    "不要把页码括号留在 title 里；page 取括号内的数字。半角 (1) 与全角（1）同样处理。"
    "「习题8-1(11)」「习题 8-1（11）」也单独一条：title 为「习题8-1」（保留习题编号，去掉括号页码，题号前的空格去掉），"
    "page 为括号内数字。"
    "行首 * 表示选学，留在 title 里，例如「*二、全微分在近似计算中的应用」。"
    "当目录顶层就是「第X章」时，层级固定为：章 level=1，节 level=2，节下面的「一、」「二、」和习题 level=3。"
    "节名右侧点线后的页码是这一节自己的 page，不要用它替换节内第一条的页码。"
    "只有章的上面还有无编号大分段标题时，才按前面的规则把章、节、节内条目整体下移一层。"
    "书签只能显示一行普通文字，标题里的公式必须写成同一行 Unicode，整段留在同一个 title 字符串里，禁止换行把公式拆出 "
    "JSON。"
    "禁止 LaTeX：不要 $...$、不要反斜杠命令（如 \\lambda、\\frac、\\sqrt、\\sin、\\cos、\\int、\\sum、\\partial）。"
    "希腊字母直接写字符（α β γ δ ε θ λ μ π σ φ ω Δ Σ Ω）。"
    "函数名写成 sin、cos、tan、ln、log，不要加反斜杠。"
    "单字符上标用 Unicode 上标（x²、xⁿ、y⁽ⁿ⁾）；多字符上标写成 ^( )，例如 e^(λx)、e^(n+1)。"
    "单字符下标用 Unicode 下标（aₙ、x₁、x₂）；没有对应字符时写成 _ ，例如 P_m(x)。"
    "导数的撇用 ′ ″ ‴，不要用英文单引号：y′、y″，不要写成 y' 或 y''。"
    "分式写成 a/b，根号写成 √(x)，积分写成 ∫，求和写成 Σ，偏导写成 ∂，无穷写成 ∞，不等号写成 ≤ ≥ ≠。"
    "例如「$y''=f(x,y')$型」写成「y″=f(x,y′) 型」；"
    "「$f(x)=e^{\\lambda x}P_m(x)$型」写成「f(x)=e^(λx)P_m(x) 型」；"
    "「$f(x)=e^{\\lambda x}[P_l(x)\\cos\\omega x+P_n(x)\\sin\\omega x]$型」写成"
    "「f(x)=e^(λx)[P_l(x) cos ωx+P_n(x) sin ωx] 型」。"
    "只输出目录中实际印刷的条目，按页面从上到下、从左到右的顺序输出，不要补写图片中不存在的标题。"
    "同一本书若同时印刷了‘按单元目录’和‘按体裁/专题索引’，只输出按阅读顺序的主目录（单元/章节），"
    "不要把体裁索引、作者名行、专题对照表再重复导入一遍。作者名若单独成行且无页码，不要输出为独立条目。";

struct AiTocPocWork {
    HWND mainHwnd = nullptr;
    HWND owner = nullptr;
    HANDLE dialogToken = nullptr;
    // Translated on the UI thread via _TRW(): workers must not return
    // thread-local temp strings across the async boundary.
    const char* error = nullptr;
    EngineBase* engine = nullptr;
    Vec<int> pageNos;
    StrVec files;
    char* session = nullptr;
    char* jsonResponse = nullptr;
    HWND browser = nullptr;
    AiChatService service = AiChatService::Doubao;
    int firstPage = 0;
    int lastPage = 0;
    // Web chats accept about 10 images per message. 1 means one send.
    int imageBatches = 1;

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

// Whole-document body-structure scan ("从正文生成目录"). Runs on a worker
// thread; the dialog owns the result once kMsgAiTocBodyScanned is handled.
struct AiTocBodyScanWork {
    EngineBase* engine = nullptr;
    HWND notifyHwnd = nullptr;
    HANDLE dialogToken = nullptr;
    TocStructureScanResult* scan = nullptr;
    bool canceled = false;

    ~AiTocBodyScanWork() {
        if (engine) {
            engine->Release();
        }
        delete scan;
    }
};

// Text-only AI round (no page images): opens the chat browser, pastes the
// candidate-digest prompt and submits. The AI reply comes back through the
// same clipboard listener as the printed-TOC flow.
struct AiTocBodySendWork {
    EngineBase* engine = nullptr;
    HWND mainHwnd = nullptr;
    HWND owner = nullptr;
    HANDLE dialogToken = nullptr;
    AiChatService service = AiChatService::Doubao;
    char* prompt = nullptr;
    HWND browser = nullptr;
    const char* error = nullptr;

    ~AiTocBodySendWork() {
        if (engine) {
            engine->Release();
        }
        str::Free(prompt);
    }
};

// Geometry of the thumbnail grid, computed from the current viewport width
// and DPI. Never cached across relayouts — a DPI or size change recomputes
// every value from logical DIP, so no stale physical pixels survive.
struct AiTocThumbGeom {
    int cols = 3;
    int width = 0;
    int height = 0;
    int gap = 0;
    int pad = 0;
    int pitch = 0;   // height + gap
    int spanW = 0;   // full grid width incl. padding
    int originX = 0; // first card x within the pane
};

// Which AI source the workflow prepares. Kept separate from the UI stage:
// "printed TOC vs body structure" is the data source, "which step the
// workflow is at" is the stage (see AiTocUiState).
enum class AiTocSourceMode {
    PrintedToc,
    BodyStructure,
};

// Unified task stages shared by both source modes:
// 准备内容 → AI 处理 → 导入目录. The legacy view flags (scanning/review/
// waiting/...) remain derived view state; this enum is the source of truth
// for the phase hint row and for logging where a workflow got stuck.
enum class AiTocUiState {
    ScanningPrintedToc,    // walking pages, looking for a printed TOC
    ConfirmPrintedToc,     // thumbnail review of the detected TOC pages
    ChooseFallback,        // no printed TOC: body-structure vs manual pages
    ScanningBodyStructure, // whole-document heading-candidate scan
    SendingToAi,           // rendering/uploading pages or submitting the digest
    WaitingForAiClipboard, // sent; waiting for the user to copy the AI reply
    ImportingResult,       // clipboard JSON detected, parsing + calibrating
};

static const char* AiTocStateName(AiTocUiState s) {
    switch (s) {
        case AiTocUiState::ScanningPrintedToc:
            return "ScanningPrintedToc";
        case AiTocUiState::ConfirmPrintedToc:
            return "ConfirmPrintedToc";
        case AiTocUiState::ChooseFallback:
            return "ChooseFallback";
        case AiTocUiState::ScanningBodyStructure:
            return "ScanningBodyStructure";
        case AiTocUiState::SendingToAi:
            return "SendingToAi";
        case AiTocUiState::WaitingForAiClipboard:
            return "WaitingForAiClipboard";
        case AiTocUiState::ImportingResult:
            return "ImportingResult";
    }
    return "?";
}

// Chunked-calibration callback context (see AiTocImportCalibProgress); the
// verify job owns it until its done callback fires.
struct AiTocImportCbCtx;

struct AiTocDialog {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    HWND pagesEdit = nullptr;
    HWND pagesLabel = nullptr;
    HWND status = nullptr;
    HWND send = nullptr;
    // "从正文生成目录" fallback button, shown only when no printed TOC
    // pages were detected.
    HWND bodyBtn = nullptr;
    HWND thumbnailPane = nullptr;
    // Lightweight three-stage hint row under the caption:
    // "准备内容 → AI 处理 → 导入目录". Shared by both source modes; only the
    // highlighted stage moves with the workflow state.
    HWND phasePrep = nullptr;
    HWND phaseArrow = nullptr;
    HWND phaseAi = nullptr;
    HWND phaseArrow2 = nullptr;
    HWND phaseImport = nullptr;
    HWND scanCount = nullptr;
    // Fallback page ("未找到印刷目录页") section controls, shown only in
    // ChooseFallback: page headline + description and the body-structure
    // branch (bold title + description above its own button). The manual
    // pages entry moved to its own confirm page (opened via manualBtn).
    HWND stateTitle = nullptr;
    HWND stateDesc = nullptr;
    HWND bodyTitle = nullptr;
    HWND bodyDesc = nullptr;
    // [手工指定目录页]: fallback-only, bottom-right of the intro block; opens
    // the manual confirm page (pages edit + thumbnail grid) instead of the
    // former inline pages section.
    HWND manualBtn = nullptr;
    // [重新发送]: shown only on the unified waiting page; resends the cached
    // payload (confirmed pages for printed mode, body prompt for body mode).
    HWND resend = nullptr;
    Vec<HWND> thumbnailImages;
    AiTocThumbGeom thumbGeom; // recomputed on every thumbnail relayout
    int thumbnailScroll = 0;
    int thumbnailMaxScroll = 0;
    // ThemeEpoch() value the cached thumbnails were rendered for; a mismatch
    // means the bitmaps must be dropped and re-rendered for the new theme.
    int thumbnailThemeEpoch = 0;
    int hoverIdx = -1;
    bool hoverTracked = false;
    HFONT font = nullptr;
    HFONT fontBold = nullptr;
    // DPI of the monitor the dialog is currently on; all layout metrics are
    // logical DIP scaled through it. Refreshed on every WM_DPICHANGED so a
    // cross-monitor drag re-scales from the same DIP values (no accumulation).
    int dpi = 96;
    // Client-space y of the hairline separator above the button row, 0 = none.
    int sepY = 0;
    // Three-dot scanning marquee: drawn in the dialog's own background paint,
    // inside this small rect only (never a full-window invalidate).
    RECT dotsRect{};
    int dotPhase = 0;
    // True while the background detector is walking pages.
    bool scanning = false;
    // False = scanning phase (compact window), true = review phase (thumbnails).
    bool review = false;
    // True after 发送 was clicked: the thumbnail pane hides and the dialog
    // shrinks back to the compact height (top/left anchored) while waiting
    // for the AI reply, so it stops covering the document behind it.
    bool waiting = false;
    // True while the AI reply JSON is being parsed/imported (third phase).
    bool importing = false;
    // Bumped on every import start: callbacks of an aborted earlier
    // calibration carry a stale generation and become no-ops.
    int importGen = 0;
    // Live callback ctx of the chunked calibration (StartTocCalibAsync).
    // WM_NCDESTROY nulls ctx->dlg so a late callback never touches the freed
    // dialog, and aborts the session so a cancelled import never commits.
    AiTocImportCbCtx* importCtx = nullptr;
    // Captured frame HWND at creation. The dialog is deliberately NOT owned
    // (clicking it must not raise the main window above other apps); this is
    // only a watchdog handle so the dialog self-destructs if the frame dies.
    HWND ownerHwnd = nullptr;
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

    // --- body-structure TOC ("从正文生成目录") state ---
    // True when printed-TOC detection found nothing and the fallback choice
    // (generate from body / type pages manually) is on screen.
    bool bodyFallback = false;
    // True while the whole-document structure scan worker runs.
    bool bodyScanning = false;
    // True once the body prompt was submitted: clipboard replies are parsed as
    // v2 {"toc":[{"candidate_id",...}]} instead of printed-TOC {"items":[...]}.
    bool bodyMode = false;
    AiTocBodyScanWork* bodyScanWork = nullptr;
    AiTocBodySendWork* bodySendWork = nullptr;
    TocStructureScanResult* bodyScan = nullptr;
    // Cached body prompt (Structure Digest + instructions) so 重新发送 can
    // submit the very same data without re-scanning the document.
    char* bodyPrompt = nullptr;

    // --- unified workflow state machine ---
    // Data source of the current AI round (independent from the stage).
    AiTocSourceMode mode = AiTocSourceMode::PrintedToc;
    // Current task stage; source of truth for the phase hint row.
    AiTocUiState state = AiTocUiState::ScanningPrintedToc;
    // Printed-TOC pages confirmed by the last 发送目录页 click; 重新发送
    // re-renders exactly these pages instead of asking the user again.
    Vec<int> confirmedPages;
    // One JSON reply per image batch. Joined locally; the model is not asked
    // to merge, because that step invented entries.
    StrVec collectedBatchJson;
    int batchesGot = 0;
    // True when the last send failed but the payload is on the clipboard and
    // the chat browser is open: the waiting page then asks for a manual paste.
    bool waitManualPaste = false;
    // The body-mode privacy note ("仅发送结构化标题候选…") shows only on the
    // first entry into the waiting page, per the product copy guidelines.
    bool bodyWaitPrivacyShown = false;
};

// Single transition point so the debug log shows every workflow step change.
static void AiTocSetState(AiTocDialog* dlg, AiTocUiState next) {
    if (dlg->state == next) {
        return;
    }
    logf("[AITOC] %s -> %s\n", AiTocStateName(dlg->state), AiTocStateName(next));
    dlg->state = next;
}

// Each batch is copied back by the user. The app appends those JSON arrays
// itself, so the model never rewrites an earlier batch.
static void AiTocSetMultiBatchWaitCopy(AiTocDialog* dlg) {
    int total = dlg->work ? dlg->work->imageBatches : 1;
    if (total < 1) {
        total = 1;
    }
    int which = dlg->batchesGot + 1;
    if (which > total) {
        which = total;
    }
    const char* lang = trans::GetCurrentLangCode();
    bool zh = lang && (str::EqI(lang, "cn") || str::EqI(lang, "tw"));
    if (zh) {
        SetWindowTextW(dlg->stateTitle, L"请复制这一批的 JSON");
        SetWindowTextW(dlg->stateDesc, ToWStrTemp(str::FormatTemp("第 %d / %d 批已发送。复制这一批回复里的 JSON。\r\n"
                                                                  "程序按顺序自己接上各批，不再让 AI 合并。",
                                                                  which, total)));
        SetWindowTextW(dlg->status, ToWStrTemp(str::FormatTemp("等待第 %d / %d 批 JSON…", which, total)));
    } else {
        SetWindowTextW(dlg->stateTitle, _TRW("Copy this batch's JSON"));
        SetWindowTextW(dlg->stateDesc, ToWStrTemp(str::FormatTemp(
                                           "Batch %d of %d has been sent. Copy the JSON in that reply.\r\n"
                                           "The app joins the batches in order. The AI is not asked to merge them.",
                                           which, total)));
        SetWindowTextW(dlg->status, ToWStrTemp(str::FormatTemp("Waiting for batch %d of %d JSON…", which, total)));
    }
}

// Waiting page copy for the unified WaitingForAiClipboard state. The title /
// description come from the source mode (printed pages vs body structure) so
// users always know WHAT was sent; the hint row and buttons are shared.
static void AiTocApplyWaitingTexts(AiTocDialog* dlg) {
    bool body = dlg->mode == AiTocSourceMode::BodyStructure;
    const WCHAR* title;
    const WCHAR* desc;
    if (dlg->waitManualPaste) {
        title = _TRW("Automatic send incomplete");
        desc =
            body ? _TRW(
                       "The heading-candidates prompt is on the clipboard. Paste and send it in the AI page.\r\n"
                       "When the reply is ready, click Copy under the AI reply; the app reads it and imports the TOC.")
                 : _TRW(
                       "The TOC page images and prompt are on the clipboard. Paste and send them in the AI page.\r\n"
                       "When the reply is ready, click Copy under the AI reply; the app reads it and imports the TOC.");
    } else if (body) {
        title = _TRW("Body structure sent to the web AI");
        if (!dlg->bodyWaitPrivacyShown) {
            dlg->bodyWaitPrivacyShown = true;
            desc = _TRW(
                "The AI is organizing a TOC from the full-text structure.\r\n"
                "When it finishes, click Copy under the AI reply; the app reads it and imports the TOC.\r\n"
                "Only locally extracted heading candidates are sent — the PDF itself is not uploaded.");
        } else {
            desc = _TRW(
                "The AI is organizing a TOC from the full-text structure.\r\n"
                "When it finishes, click Copy under the AI reply; the app reads it and imports the TOC.");
        }
    } else if (dlg->work && dlg->work->imageBatches > 1) {
        AiTocSetMultiBatchWaitCopy(dlg);
        return;
    } else {
        title = _TRW("TOC pages sent to the web AI");
        desc = _TRW(
            "The AI is recognizing the printed TOC.\r\n"
            "When it finishes, click Copy under the AI reply; the app reads it and imports the TOC.");
    }
    SetWindowTextW(dlg->stateTitle, title);
    SetWindowTextW(dlg->stateDesc, desc);
    SetWindowTextW(dlg->status, _TRW("Waiting for the AI reply. Click Copy when it is ready…"));
}

// Restore the fallback page copy (the title/description controls are shared
// with the waiting page, so returning to ChooseFallback must reset them).
static void AiTocApplyFallbackTexts(AiTocDialog* dlg) {
    SetWindowTextW(dlg->stateTitle, _TRW("No printed TOC found"));
    SetWindowTextW(dlg->stateDesc,
                   _TRW("This document may not have a separate printed TOC.\r\n"
                        "You can generate one from the body structure or specify the TOC pages manually."));
    SetWindowTextW(dlg->status, L"");
}

static constexpr const WCHAR* kAiTocToken = L"SumatraAiTocToken";
static constexpr const WCHAR* kAiTocDetectToken = L"SumatraAiTocDetectToken";
static constexpr const WCHAR* kAiTocBodyScanToken = L"SumatraAiTocBodyScanToken";
static UINT_PTR gAiTocToken = 0;
static constexpr const WCHAR* kAiTocDialogClass = L"SUMATRA_AI_TOC_DIALOG";
static constexpr const WCHAR* kAiTocThumbPaneClass = L"SUMATRA_AI_TOC_THUMB_PANE";
static constexpr UINT kMsgAiTocDetected = WM_APP + 72;
static constexpr UINT kMsgAiTocDetectProgress = WM_APP + 73;
static constexpr UINT kMsgAiTocBodyProgress = WM_APP + 74;
static constexpr UINT kMsgAiTocBodyScanned = WM_APP + 75;
static constexpr UINT kMsgAiTocBodySent = WM_APP + 76;
static bool gAiTocClassesRegistered = false;

static void AiTocPocWorker(AiTocPocWork* work);
static void AiTocUploadFinished(AiTocPocWork* work);
static RenderedBitmap* RenderAiTocPage(EngineBase* engine, int pageNo);
static RenderedBitmap* RenderAiTocThumbnail(EngineBase* engine, int pageNo);
static void AiTocLayoutThumbnails(AiTocDialog* dlg);
static void AiTocRenderVisibleThumbnails(AiTocDialog* dlg);
static void AiTocApplyStateFonts(AiTocDialog* dlg);
static void AiTocLayoutControls(AiTocDialog* dlg);
static int AiTocS(AiTocDialog* dlg, int v);

// Place scanCount immediately after the status label on the same row (used by
// printed/body scan and by import calibration). Caller owns show/hide.
static void AiTocPlaceScanCountAfterStatus(AiTocDialog* dlg, int statusY, int lineH, int afterX) {
    if (!dlg->scanCount) {
        return;
    }
    MoveWindow(dlg->scanCount, afterX, statusY, AiTocS(dlg, 80), lineH, FALSE);
}

static void AiTocSetImportProgress(AiTocDialog* dlg, const WCHAR* text) {
    AiTocSetState(dlg, AiTocUiState::ImportingResult);
    if (!dlg->importing) {
        dlg->importing = true;
        AiTocApplyStateFonts(dlg);
        InvalidateRect(dlg->hwnd, nullptr, TRUE);
    }
    // Phase messages without a live counter: hide the shared scanCount so a
    // stale "n / m" cannot sit beside "parsing…" / "arranging…".
    if (dlg->scanCount) {
        ShowWindow(dlg->scanCount, SW_HIDE);
    }
    SetWindowTextW(dlg->status, text);
    InvalidateRect(dlg->status, nullptr, FALSE);
}

// Live calibration counter. Keep the status prefix fixed and only rewrite
// scanCount — updating the whole status string (or UpdateWindow on the dialog)
// reflows the line and makes "正在导入目录：校准书签位置 n / m" shake.
static void AiTocSetImportCalibProgress(AiTocDialog* dlg, int done, int total) {
    AiTocSetState(dlg, AiTocUiState::ImportingResult);
    if (!dlg->importing) {
        dlg->importing = true;
        AiTocApplyStateFonts(dlg);
        InvalidateRect(dlg->hwnd, nullptr, TRUE);
    }
    const WCHAR* prefix = _TRW("Importing TOC: calibrating bookmark destinations…");
    WCHAR cur[256]{};
    GetWindowTextW(dlg->status, cur, dimof(cur));
    if (!str::Eq(cur, prefix)) {
        SetWindowTextW(dlg->status, prefix);
    }
    if (!dlg->scanCount) {
        return;
    }
    SetWindowTextW(dlg->scanCount, ToWStrTemp(str::FormatTemp("%d / %d", done, total)));
    // Position once per tick is cheap; layout may not run during import.
    RECT statusRc{};
    GetClientRect(dlg->status, &statusRc);
    MapWindowPoints(dlg->status, dlg->hwnd, (POINT*)&statusRc, 2);
    HDC dc = GetDC(dlg->hwnd);
    HGDIOBJ oldF = SelectObject(dc, dlg->font);
    SIZE tsz{};
    GetTextExtentPoint32W(dc, prefix, (int)wcslen(prefix), &tsz);
    SelectObject(dc, oldF);
    ReleaseDC(dlg->hwnd, dc);
    int afterX = statusRc.left + tsz.cx + AiTocS(dlg, 8);
    AiTocPlaceScanCountAfterStatus(dlg, statusRc.top, statusRc.bottom - statusRc.top, afterX);
    ShowWindow(dlg->scanCount, SW_SHOWNOACTIVATE);
}

static void AiTocHideImportProgress(AiTocDialog* dlg) {
    // Import aborted (invalid/empty JSON): revert the phase hint highlight.
    AiTocSetState(dlg, AiTocUiState::WaitingForAiClipboard);
    dlg->importing = false;
    if (dlg->scanCount) {
        ShowWindow(dlg->scanCount, SW_HIDE);
        SetWindowTextW(dlg->scanCount, L"");
    }
    AiTocApplyStateFonts(dlg);
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

// --- body-structure scan ("从正文生成目录") workers -------------------------

static bool AiTocBodyScanCanceled(void* context) {
    auto* work = (AiTocBodyScanWork*)context;
    return GetPropW(work->notifyHwnd, kAiTocBodyScanToken) != work->dialogToken;
}

static void AiTocBodyScanProgress(void* context, int pageNo, int pageLimit) {
    auto* work = (AiTocBodyScanWork*)context;
    PostMessageW(work->notifyHwnd, kMsgAiTocBodyProgress, (WPARAM)pageNo, (LPARAM)pageLimit);
}

static void AiTocBodyScanWorker(AiTocBodyScanWork* work) {
    bool ok = RunTocStructureScan(work->engine, *work->scan, AiTocBodyScanCanceled, work, AiTocBodyScanProgress, work);
    if (!ok) {
        work->canceled = true;
    }
    if (GetPropW(work->notifyHwnd, kAiTocBodyScanToken) == work->dialogToken) {
        PostMessageW(work->notifyHwnd, kMsgAiTocBodyScanned, (WPARAM)work, 0);
    } else {
        delete work;
    }
}

static void AiTocBodySendWorker(AiTocBodySendWork* work) {
    defer {
        if (GetPropW(work->owner, kAiTocToken) == work->dialogToken) {
            PostMessageW(work->owner, kMsgAiTocBodySent, (WPARAM)work, 0);
        } else {
            delete work;
        }
    };
    // Text-only round: put the prompt on the clipboard, open the configured
    // chat page, bootstrap a fresh/blank composer with "hi!" (wait for reply),
    // then paste and submit the real prompt.
    if (!CopyTextToClipboard(work->prompt)) {
        work->error = "Cannot access the clipboard. Please retry.";
        return;
    }
    bool reused = false;
    HWND browser = nullptr;
    if (!LaunchAiChatBrowser(work->service, &reused, &browser, nullptr)) {
        work->error = "Cannot open the configured AI page.";
        return;
    }
    if (!EnsureAiChatComposerReady(work->service, browser, !reused)) {
        work->error = "The AI page opened, but the initial message could not be sent.";
        work->browser = browser;
        return;
    }
    if (!CopyTextToClipboard(work->prompt) || !PasteAndSubmitAiChatWhenReady(work->service, browser, false)) {
        work->error =
            "Automatic send incomplete: the heading-candidates prompt is on the clipboard. Paste it into the AI input "
            "box and send.";
        work->browser = browser;
        return;
    }
    work->browser = browser;
}

static void StartAiBodyStructureScan(AiTocDialog* dlg) {
    if (dlg->bodyScanning || dlg->busy || dlg->submitted) {
        return;
    }
    DisplayModel* dm = dlg->win ? dlg->win->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    dlg->bodyScanning = true;
    dlg->mode = AiTocSourceMode::BodyStructure;
    logf("[AITOC] User selected BodyStructure\n");
    AiTocSetState(dlg, AiTocUiState::ScanningBodyStructure);
    dlg->scanning = true; // reuse the scanning marquee
    SetTimer(dlg->hwnd, 2, 300, nullptr);
    EnableWindow(dlg->bodyBtn, FALSE);
    EnableWindow(dlg->send, FALSE);
    EnableWindow(dlg->pagesEdit, FALSE);
    SetWindowTextW(dlg->status, _TRW("Scanning document structure"));
    SetWindowTextW(dlg->scanCount, L"0 / 0");
    // Collapse the fallback page: the scan state shows only the progress row.
    AiTocLayoutControls(dlg);
    InvalidateRect(dlg->hwnd, nullptr, TRUE);

    auto* work = new AiTocBodyScanWork();
    work->engine = engine;
    engine->AddRef();
    work->notifyHwnd = dlg->hwnd;
    work->dialogToken = dlg->token;
    work->scan = new TocStructureScanResult();
    dlg->bodyScanWork = work;
    SetPropW(dlg->hwnd, kAiTocBodyScanToken, dlg->token);
    RunAsync(MkFunc0<AiTocBodyScanWork>(AiTocBodyScanWorker, work), "AiTocBodyScan");
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
    // Match RenderCache: always build the view theme profile. Light-Warm Auto
    // is PreserveImages (plain Export pixels + post gauze); dark chrome uses
    // FollowThemeV2 / SmartDark. ApplyRenderThemePostColors must get the
    // profile's pageBackground (not bg=0) or Warm thumbs go black.
    RenderPageArgs args(pageNo, zoom, 0, nullptr, RenderTarget::Export);
    DarkModeProfile darkProfile;
    BuildViewDarkModeProfile(engine, &darkProfile);
    if (darkProfile.mode != PageColorMode::Normal) {
        args.darkProfile = &darkProfile;
    }
    RenderedBitmap* bmp = engine->RenderPage(args);
    ApplyRenderThemePostColors(engine, bmp, pageNo, zoom, &box, args.darkProfile);
    return bmp;
}

// Doubao / DeepSeek / ChatGPT reject more than about 10 images in one message.
constexpr int kAiTocImagesPerMessage = 10;

// The paste runs on a worker. The dialog may close or replace its AiTocPocWork
// between batches, so the worker must not touch that object. Callers pass a
// StrVec they own for the whole paste.
struct AiTocPasteTarget {
    HWND owner = nullptr;
    HANDLE dialogToken = nullptr;
    HWND browser = nullptr;
    AiChatService service = AiChatService::Doubao;
    StrVec* files = nullptr;
    int* imageBatches = nullptr;
};

static bool AiTocDialogStillCurrent(HWND owner, HANDLE token) {
    return owner && GetPropW(owner, kAiTocToken) == token;
}

static bool PasteAiTocImageSlice(AiTocPasteTarget* target, int begin, int count, bool waitForPageReady) {
    if (!target || !target->files || !target->browser || begin < 0 || count < 1) {
        return false;
    }
    int nFiles = target->files->Size();
    if (count > nFiles || begin > nFiles - count) {
        return false;
    }
    StrVec slice;
    for (int i = 0; i < count; i++) {
        char* path = target->files->At(begin + i);
        if (str::IsEmpty(path)) {
            return false;
        }
        slice.Append(path);
    }
    return PasteAiChatFilesWhenReady(target->service, target->browser, waitForPageReady, slice);
}

// One image message extracts only the pictures attached to it. Later batches
// are sent after that JSON has been copied back. Joining is done locally.
static TempStr AiTocExtractBatchPrompt(int from1, int to1, int total) {
    const char* head = str::FormatTemp(
        "这些是印刷目录图片的第 %d-%d 张（共 %d 张，按阅读顺序）。\n"
        "请只根据本条消息里的图片提取目录，按下面的规则输出一个 items JSON。"
        "不要回复「已收到」，不要合并其他批次，不要输出本条消息之外的条目。\n\n",
        from1, to1, total);
    return str::FormatTemp("%s%s", head, kAiTocPrompt);
}

static bool SubmitAiTocImageBatch(AiTocPasteTarget* target, int from1, int to1) {
    if (!target || !target->files || !target->browser) {
        return false;
    }
    TempStr prompt = AiTocExtractBatchPrompt(from1, to1, target->files->Size());
    if (!CopyTextToClipboard(prompt) || !PasteAndSubmitAiChatWhenReady(target->service, target->browser, false)) {
        return false;
    }
    logf("AI TOC: batch %d-%d submitted\n", from1, to1);
    return true;
}

static bool AiTocPasteOneBatch(AiTocPasteTarget* target, int batchIndex, bool waitFirst, bool submitExtract) {
    if (!target || !target->files || batchIndex < 0) {
        return false;
    }
    int nFiles = target->files->Size();
    int begin = batchIndex * kAiTocImagesPerMessage;
    if (begin >= nFiles) {
        return false;
    }
    int count = nFiles - begin;
    if (count > kAiTocImagesPerMessage) {
        count = kAiTocImagesPerMessage;
    }
    if (target->owner && !AiTocDialogStillCurrent(target->owner, target->dialogToken)) {
        return false;
    }
    if (!PasteAiTocImageSlice(target, begin, count, waitFirst && batchIndex == 0)) {
        return false;
    }
    if (!submitExtract) {
        return true;
    }
    return SubmitAiTocImageBatch(target, begin + 1, begin + count);
}

static TempStr AiTocPromptForSend(const AiTocPocWork*) {
    return str::DupTemp(kAiTocPrompt);
}

// Paste cached TOC page images (batched at 10 per message). Used by the first
// send and by 重新发送 — resend must put the images back in the chat, not
// only the text prompt. Stop between batches if the dialog was closed; the
// next slice would otherwise read a file list the dialog already freed.
static bool AiTocPasteImages(AiTocPasteTarget* target, bool waitFirst) {
    if (!target || !target->browser || !target->files || target->files->Size() < 1) {
        return false;
    }
    if (target->owner && !AiTocDialogStillCurrent(target->owner, target->dialogToken)) {
        return false;
    }
    int nFiles = target->files->Size();
    int batches = (nFiles + kAiTocImagesPerMessage - 1) / kAiTocImagesPerMessage;
    if (batches < 1) {
        batches = 1;
    }
    if (target->imageBatches) {
        *target->imageBatches = batches;
    }
    // A single batch stays in the composer; SendAiTocPrompt submits the
    // extraction rules with those images. Further batches go out only after
    // the user has copied the previous batch's JSON.
    if (batches <= 1) {
        if (!PasteAiTocImageSlice(target, 0, nFiles, waitFirst)) {
            return false;
        }
        logf("AI TOC: pasted %d TOC page images in 1 batch\n", nFiles);
        return true;
    }
    if (!AiTocPasteOneBatch(target, 0, waitFirst, true)) {
        return false;
    }
    logf("AI TOC: pasted batch 1 of %d (%d images)\n", batches, nFiles);
    return true;
}

static bool AiTocPasteWorkImages(AiTocPocWork* work, bool waitFirst) {
    if (!work) {
        return false;
    }
    AiTocPasteTarget target;
    target.owner = work->owner;
    target.dialogToken = work->dialogToken;
    target.browser = work->browser;
    target.service = work->service;
    target.files = &work->files;
    target.imageBatches = &work->imageBatches;
    return AiTocPasteImages(&target, waitFirst);
}

struct AiTocResendPrintedWork {
    HWND owner = nullptr;
    HANDLE dialogToken = nullptr;
    // Identity of the dialog's work at start. The worker must not dereference
    // it: the dialog can delete that object during the between-batch wait.
    AiTocPocWork* poc = nullptr;
    HWND browser = nullptr;
    AiChatService service = AiChatService::Doubao;
    StrVec files; // copies of the paths, owned here
    int imageBatches = 1;
    bool ok = false;
};

static void AiTocResendPrintedFinished(AiTocResendPrintedWork* w);

static void AiTocResendPrintedWorker(AiTocResendPrintedWork* w) {
    defer {
        uitask::Post(MkFunc0<AiTocResendPrintedWork>(AiTocResendPrintedFinished, w), "AiTocResendPrintedFinished");
    };
    if (!w || !w->browser || w->files.Size() < 1) {
        if (w) {
            w->ok = false;
        }
        return;
    }
    // Paths were copied on the UI thread. Do not touch w->poc.
    AiTocPasteTarget target;
    target.owner = w->owner;
    target.dialogToken = w->dialogToken;
    target.browser = w->browser;
    target.service = w->service;
    target.files = &w->files;
    target.imageBatches = &w->imageBatches;
    w->ok = AiTocPasteImages(&target, false);
}

static void AiTocPocWorker(AiTocPocWork* work) {
    defer {
        uitask::Post(MkFunc0<AiTocPocWork>(AiTocUploadFinished, work), "AiTocUploadFinished");
    };
    TempStr root = path::JoinTemp(GetTempDirTemp(), "SumatraPDF-Plus\\AI-TOC");
    TempStr session = path::JoinTemp(root, str::FormatTemp("poc-%u", GetTickCount()));
    if (!dir::CreateAll(session)) {
        work->error = "Cannot create the image folder. Check disk space and try again.";
        return;
    }
    work->session = str::Dup(session);
    for (int i = 0; i < work->pageNos.Size(); i++) {
        int pageNo = work->pageNos[i];
        RenderedBitmap* bmp = RenderAiTocPage(work->engine, pageNo);
        if (!bmp) {
            work->error = "Failed to render the TOC page images. Try again or adjust the pages.";
            return;
        }
        TempStr path = path::JoinTemp(session, str::FormatTemp("toc_page_%04d.png", i + 1));
        bool ok = SaveBitmapAsPng(bmp->GetBitmap(), path);
        delete bmp;
        if (!ok) {
            work->error = "Failed to save images. Check disk space and try again.";
            return;
        }
        work->files.Append(str::Dup(path));
    }
    bool reused = false;
    HWND browser = nullptr;
    if (GetPropW(work->owner, kAiTocToken) != work->dialogToken) return;
    if (!LaunchAiChatBrowser(work->service, &reused, &browser, nullptr)) {
        work->error = "Cannot open the configured AI page.";
        return;
    }
    // Doubao/DeepSeek/ChatGPT need a first real message before image paste is
    // reliable on a cold or blank chat page. Send "hi!", wait for a reply, then
    // attach TOC images to the stable composer (same as a warm session).
    if (!EnsureAiChatComposerReady(work->service, browser, !reused)) {
        work->error = "The AI page opened, but the initial message could not be sent.";
        work->browser = browser;
        return;
    }
    work->browser = browser;
    if (!AiTocPasteWorkImages(work, !reused)) {
        logf("AI TOC: unable to send pages to browser; temp files kept at %s\n", session);
        work->error = "Automatic send incomplete. Paste the clipboard content into the AI input box and send.";
        return;
    }
}

static bool AiTocEnsureClipboardListener(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    if (AddClipboardFormatListener(hwnd)) {
        return true;
    }
    // Already registered: AddClipboardFormatListener fails with
    // ERROR_INVALID_PARAMETER. Resend / retry must not treat that as fatal.
    return GetLastError() == ERROR_INVALID_PARAMETER;
}

static void SendAiTocPrompt(AiTocDialog* dlg) {
    if (!AiTocEnsureClipboardListener(dlg->hwnd)) {
        SetWindowTextW(dlg->status,
                       _TRW("Cannot watch the clipboard. Close the window and try again. The prompt was not sent."));
        if (dlg->resend) {
            EnableWindow(dlg->resend, TRUE);
        }
        return;
    }
    TempStr prompt = AiTocPromptForSend(dlg->work);
    if (!CopyTextToClipboard(prompt) || !PasteAndSubmitAiChatWhenReady(dlg->work->service, dlg->work->browser, false)) {
        RemoveClipboardFormatListener(dlg->hwnd);
        if (dlg->resend) {
            EnableWindow(dlg->resend, TRUE);
        }
        MessageBoxWarning(dlg->hwnd, _TRA("Unable to paste the AI prompt into the browser."),
                          _TRA("AI Recognize Table of Contents"));
        return;
    }
    logf("AI TOC: prompt submitted for pages %d-%d\n", dlg->work->firstPage, dlg->work->lastPage);
    // Keep the page images + prompt on the clipboard as a backup for manual
    // re-pasting; the waiting page no longer mentions it.
    CopyAiChatPayloadToClipboard(dlg->work->files, prompt);
    dlg->submitted = true;
    dlg->waiting = true;
    AiTocSetState(dlg, AiTocUiState::WaitingForAiClipboard);
    dlg->clipboardSequence = GetClipboardSequenceNumber();
    SetTimer(dlg->hwnd, 1, 750, nullptr);
    SetTimer(dlg->hwnd, 2, 300, nullptr); // light dot marquee while waiting
    AiTocApplyWaitingTexts(dlg);
    EnableWindow(dlg->send, FALSE);
    EnableWindow(dlg->pagesEdit, FALSE);
    if (dlg->resend) {
        EnableWindow(dlg->resend, TRUE);
    }
    // The waiting copy was set above; recompute the compact height (the
    // description line count differs between manual-paste and normal texts).
    AiTocLayoutControls(dlg);
}

static void AiTocArmPrintedBatchWait(AiTocDialog* dlg) {
    if (!dlg || !dlg->work) {
        return;
    }
    if (!AiTocEnsureClipboardListener(dlg->hwnd)) {
        SetWindowTextW(dlg->status,
                       _TRW("Cannot watch the clipboard. Close the window and try again. The prompt was not sent."));
        if (dlg->resend) {
            EnableWindow(dlg->resend, TRUE);
        }
        dlg->busy = false;
        return;
    }
    dlg->busy = false;
    dlg->submitted = true;
    dlg->waiting = true;
    dlg->waitManualPaste = false;
    AiTocSetState(dlg, AiTocUiState::WaitingForAiClipboard);
    dlg->clipboardSequence = GetClipboardSequenceNumber();
    SetTimer(dlg->hwnd, 1, 750, nullptr);
    SetTimer(dlg->hwnd, 2, 300, nullptr);
    AiTocApplyWaitingTexts(dlg);
    EnableWindow(dlg->send, FALSE);
    EnableWindow(dlg->pagesEdit, FALSE);
    if (dlg->resend) {
        EnableWindow(dlg->resend, TRUE);
    }
    AiTocLayoutControls(dlg);
}

static void AiTocResendPrintedFinished(AiTocResendPrintedWork* w) {
    if (!w) {
        return;
    }
    HWND hwnd = w->owner;
    HANDLE token = w->dialogToken;
    bool ok = w->ok;
    int batches = w->imageBatches;
    AiTocPocWork* poc = w->poc;
    delete w;
    if (!hwnd || !IsWindow(hwnd) || GetPropW(hwnd, kAiTocToken) != token) {
        return;
    }
    auto* dlg = (AiTocDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return;
    }
    dlg->busy = false;
    if (ok && dlg->work && dlg->work == poc) {
        dlg->work->imageBatches = batches;
    }
    if (ok && dlg->work && dlg->work == poc && dlg->work->browser && dlg->work->imageBatches > 1) {
        dlg->collectedBatchJson.Reset();
        dlg->batchesGot = 0;
        AiTocArmPrintedBatchWait(dlg);
        return;
    }
    if (!ok || !dlg->work || dlg->work != poc || !dlg->work->browser) {
        SetWindowTextW(dlg->status, _TRW("Automatic send incomplete. Paste the clipboard content into the AI "
                                         "input box and send."));
        if (dlg->resend) {
            EnableWindow(dlg->resend, TRUE);
        }
        AiTocSetState(dlg, AiTocUiState::WaitingForAiClipboard);
        AiTocLayoutControls(dlg);
        return;
    }
    SetWindowTextW(dlg->status, _TRW("Sending prompt…"));
    SendAiTocPrompt(dlg);
    if (dlg->resend) {
        EnableWindow(dlg->resend, !dlg->busy);
    }
    AiTocLayoutControls(dlg);
}

// [重新发送] on the unified waiting page: submit the SAME payload again —
// cached confirmed pages (printed) or the cached Structure Digest prompt
// (body). Never re-runs detection or the whole-document scan.
static void AiTocResendToAi(AiTocDialog* dlg) {
    if (dlg->busy) {
        return;
    }
    logf("[AITOC] resend requested mode=%s\n", dlg->mode == AiTocSourceMode::BodyStructure ? "Body" : "Printed");
    dlg->waitManualPaste = false;
    if (dlg->mode == AiTocSourceMode::BodyStructure) {
        if (!dlg->bodyPrompt) {
            return;
        }
        if (dlg->resend) {
            EnableWindow(dlg->resend, FALSE);
        }
        dlg->busy = true;
        AiTocSetState(dlg, AiTocUiState::SendingToAi);
        auto* sendWork = new AiTocBodySendWork();
        sendWork->mainHwnd = dlg->win ? dlg->win->hwndFrame : nullptr;
        sendWork->owner = dlg->hwnd;
        sendWork->dialogToken = dlg->token;
        sendWork->service = AiTocActiveService();
        sendWork->prompt = str::Dup(dlg->bodyPrompt);
        dlg->bodySendWork = sendWork;
        SetWindowTextW(dlg->status, _TRW("Resending body structure…"));
        RunAsync(MkFunc0<AiTocBodySendWork>(AiTocBodySendWorker, sendWork), "AiTocBodyResend");
        return;
    }
    if (dlg->work && dlg->work->browser && dlg->work->files.Size() > 0) {
        // A full resend starts again at batch 1. JSON already copied is dropped.
        dlg->collectedBatchJson.Reset();
        dlg->batchesGot = 0;
        // Warm session: re-paste the cached page images, then the prompt.
        if (dlg->resend) {
            EnableWindow(dlg->resend, FALSE);
        }
        dlg->busy = true;
        AiTocSetState(dlg, AiTocUiState::SendingToAi);
        SetWindowTextW(dlg->status, _TRW("Resending TOC pages…"));
        auto* rw = new AiTocResendPrintedWork();
        rw->owner = dlg->hwnd;
        rw->dialogToken = dlg->token;
        rw->poc = dlg->work;
        rw->browser = dlg->work->browser;
        rw->service = dlg->work->service;
        for (int i = 0; i < dlg->work->files.Size(); i++) {
            rw->files.Append(dlg->work->files.At(i));
        }
        RunAsync(MkFunc0<AiTocResendPrintedWork>(AiTocResendPrintedWorker, rw), "AiTocResendPrinted");
        return;
    }
    if (dlg->confirmedPages.Size() == 0) {
        return;
    }
    // Cold path: re-render and re-upload the cached confirmed pages.
    if (dlg->resend) {
        EnableWindow(dlg->resend, FALSE);
    }
    dlg->busy = true;
    AiTocSetState(dlg, AiTocUiState::SendingToAi);
    SetWindowTextW(dlg->status, ToWStrTemp(str::FormatTemp(_TRA("Preparing %d TOC page images for sending…"),
                                                           dlg->confirmedPages.Size())));
    StartAiTocRender(dlg->win, dlg->confirmedPages, dlg->hwnd);
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
    char* printedLabel = nullptr;
    bool hasPage = false;
    int level = 1;

    void Free() {
        str::Free(title);
        title = nullptr;
        str::Free(printedLabel);
        printedLabel = nullptr;
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
        } else if (str::Eq(p, "page") && (type == json::Type::Number || type == json::Type::String)) {
            // Numbers: arabic printed pages. Strings: "xxxvi", "R1", or "12".
            // null is omitted by the visitor (hasPage stays false).
            int pr = 0;
            char* lab = nullptr;
            if (TocCalibParsePrintedText(value, &pr, &lab)) {
                str::Free(item.printedLabel);
                item.printedLabel = lab;
                item.printedPage = pr;
                item.hasPage = pr > 0 || (lab && lab[0]);
            } else {
                str::Free(lab);
            }
        } else if (str::Eq(p, "level") && type == json::Type::Number) {
            item.level = ParseInt(value);
        }
        return true;
    }
};

// AI can return structural headings such as "第一编" / "Reference Section"
// with page: null or 0. Keep them in the tree and point them at the first
// child that has a destination and/or a printed folio / R1 label (even when
// the child's PDF page is resolved only later during calib).
static int AiTocFillMissingPages(ExtractedTocItem* item) {
    int firstChildPage = 0;
    int firstChildPrintedPage = 0;
    const char* firstChildLabel = nullptr;
    for (int i = 0; i < item->children.Size(); i++) {
        auto* child = item->children[i];
        int childPage = AiTocFillMissingPages(child);
        if (!firstChildPage && childPage > 0) {
            firstChildPage = childPage;
            firstChildPrintedPage = child->printedPage;
            firstChildLabel = child->printedLabel;
        }
        if (!firstChildLabel && child->printedLabel && child->printedLabel[0]) {
            firstChildLabel = child->printedLabel;
            if (!(firstChildPrintedPage > 0) && child->printedPage > 0) {
                firstChildPrintedPage = child->printedPage;
            }
            if (!firstChildPage && child->pageNo > 0) {
                firstChildPage = child->pageNo;
            }
        }
        if (!(firstChildPrintedPage > 0) && child->printedPage > 0) {
            firstChildPrintedPage = child->printedPage;
            if (!firstChildPage && child->pageNo > 0) {
                firstChildPage = child->pageNo;
            }
            if (!firstChildLabel && child->printedLabel && child->printedLabel[0]) {
                firstChildLabel = child->printedLabel;
            }
        }
    }
    if (item->pageNo < 1 && firstChildPage > 0) {
        item->pageNo = firstChildPage;
    }
    if (!(item->printedPage > 0) && firstChildPrintedPage > 0) {
        item->printedPage = firstChildPrintedPage;
    }
    if ((!item->printedLabel || !item->printedLabel[0]) && firstChildLabel && firstChildLabel[0]) {
        str::ReplaceWithCopy(&item->printedLabel, firstChildLabel);
    }
    return item->pageNo;
}

// After the AI JSON import finished, bring the main window back to the front
// so the user lands seamlessly in SumatraPDF. Covers the main window being
// minimized or covered by other applications (the AI browser, typically).
static void AiTocActivateMainWindow(AiTocDialog* dlg) {
    HWND frame = dlg->win ? dlg->win->hwndFrame : nullptr;
    if (!frame || !IsWindow(frame)) {
        return;
    }
    if (IsIconic(frame)) {
        ShowWindow(frame, SW_RESTORE);
    } else {
        ShowWindow(frame, SW_SHOW);
    }
    // The foreground is usually the AI browser at this point (the import is
    // triggered by copying the reply), so SetForegroundWindow alone would be
    // rejected by the foreground lock. Attach to the foreground thread's
    // input queue to make the switch stick.
    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD curThread = GetCurrentThreadId();
    bool attached = fgThread && fgThread != curThread && AttachThreadInput(curThread, fgThread, TRUE);
    BringWindowToTop(frame);
    SetForegroundWindow(frame);
    if (attached) {
        AttachThreadInput(curThread, fgThread, FALSE);
    }
}

// Progress/finish callbacks for the chunked (async) bookmark calibration.
// Both run on the UI thread. ctx is an AiTocImportCbCtx (dialog + generation):
// a closed dialog nulls ctx->dlg and a re-import bumps dlg->importGen, so a
// stale or orphaned callback is a safe no-op.
struct AiTocImportCbCtx {
    AiTocDialog* dlg = nullptr;
    int gen = 0;
};

// Calibration slices fire this every ~25-40 ms; only the "n / m" counter
// (scanCount) updates — the status prefix stays put.
static void AiTocImportCalibProgress(int done, int total, void* ctx) {
    auto* cb = (AiTocImportCbCtx*)ctx;
    AiTocDialog* dlg = cb ? cb->dlg : nullptr;
    if (!dlg || cb->gen != dlg->importGen) return;
    if (GetPropW(dlg->hwnd, kAiTocToken) != dlg->token) return;
    AiTocSetImportCalibProgress(dlg, done, total);
}

static void AiTocImportDoneCb(bool ok, void* ctx) {
    auto* cb = (AiTocImportCbCtx*)ctx;
    AiTocDialog* dlg = cb ? cb->dlg : nullptr;
    int gen = cb ? cb->gen : 0;
    if (dlg && dlg->importCtx == cb) {
        dlg->importCtx = nullptr;
    }
    delete cb; // done fires exactly once: the ctx dies with it
    if (!dlg || gen != dlg->importGen) return;
    if (GetPropW(dlg->hwnd, kAiTocToken) != dlg->token) return;
    if (ok) {
        AiTocSetImportProgress(dlg, _TRW("TOC import finished."));
        dlg->submitted = false;
        // Seamless handoff back to the main window (restore/raise/activate)
        // before the dialog closes. Must run before DestroyWindow frees dlg.
        AiTocActivateMainWindow(dlg);
        DestroyWindow(dlg->hwnd);
    } else {
        AiTocHideImportProgress(dlg);
        SetWindowTextW(dlg->status, _TRW("TOC import failed. Copy the full AI TOC JSON and try again."));
    }
}

static bool AiTocImportJson(AiTocDialog* dlg, const char* text) {
    AiTocSetImportProgress(dlg, _TRW("Importing TOC: parsing the AI reply…"));
    AiTocJsonVisitor parsed;
    if (!json::Parse(text, &parsed)) {
        AiTocHideImportProgress(dlg);
        MessageBoxWarning(dlg->hwnd, _TRA("AI returned invalid JSON."), _TRA("AI Recognize Table of Contents"));
        return false;
    }
    Vec<ExtractedTocItem*> roots;
    Vec<ExtractedTocItem*> stack;
    AiTocSetImportProgress(dlg, _TRW("Importing TOC: arranging bookmark levels…"));
    // Seed from footers only when the file has no per-page PDG sheet names.
    // 000076.pdg is printed page 76; adding one offset on a textless scan
    // lands hundreds of pages away. Calib replaces either guess with the map.
    Vec<int> pdgMap;
    bool pdgBook = TocCalibBuildPdgPrintedMap(dlg->work->engine, pdgMap);
    int arabicOffset = -1;
    if (!pdgBook) {
        arabicOffset = TocCalibEstimateArabicOffset(dlg->work->engine, dlg->work->lastPage);
        if (arabicOffset >= 0) {
            logf("AI TOC: footer arabic offset=%d afterToc=%d\n", arabicOffset, dlg->work->lastPage);
        }
    }
    int nPages = dlg->work->engine ? dlg->work->engine->PageCount() : 0;
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
        NormalizeTocNumberingParens(&item->title);
        item->printedPage = src.printedPage > 0 ? src.printedPage : 0;
        if (src.printedLabel && src.printedLabel[0]) {
            item->printedLabel = str::Dup(src.printedLabel);
        }
        item->pageNo = 0;
        int labelPdf = 0;
        if (!pdgBook && item->printedPage > 0) {
            labelPdf = TocCalibPdfForPrintedLabel(dlg->work->engine, item->printedPage);
        }
        if (pdgBook && item->printedPage > 0) {
            if (item->printedPage < pdgMap.Size() && pdgMap[item->printedPage] > 0) {
                item->pageNo = pdgMap[item->printedPage];
            }
        } else if (labelPdf > 0) {
            item->pageNo = labelPdf;
        } else if (item->printedPage > 0 && arabicOffset >= 0) {
            item->pageNo = item->printedPage + arabicOffset;
            if (item->pageNo < 1) {
                item->pageNo = 1;
            }
            if (nPages > 0 && item->pageNo > nPages) {
                item->pageNo = nPages;
            }
        } else if (item->printedLabel && item->printedLabel[0] && dlg->work->engine) {
            int byLabel = 0;
            if (dlg->work->engine->HasPageLabels()) {
                byLabel = dlg->work->engine->GetPageByLabel(item->printedLabel);
            }
            if (byLabel > 0 && (nPages < 1 || byLabel <= nPages)) {
                item->pageNo = byLabel;
            }
        }
        item->level = stack.Size() + 1;
        item->confidence = 40;
        item->destinationSource = TocDestinationSource::Estimated;
        item->source = ExtractedTocSource::PrintedToc;
        // Stamp first/last selected TOC PDF pages so calib knows the spread.
        item->tocPageNo = (stack.Size() == 0 && roots.Size() == 0) ? dlg->work->firstPage : dlg->work->lastPage;
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
    AiTocSetImportProgress(dlg, _TRW("Importing TOC: calibrating bookmark destinations…"));
    PrepareTocExtractionResult(dlg->work->engine, roots);
    if (roots.Size() == 0 || !dm || dm->GetEngine() != dlg->work->engine) {
        AiTocHideImportProgress(dlg);
        DeleteExtractedTocItems(roots);
        MessageBoxWarning(dlg->hwnd, "The JSON contains no importable table-of-contents items.",
                          _TRA("AI Recognize Table of Contents"));
        return false;
    }
    // The (expensive) body verify runs in UI-thread slices via
    // StartTocCalibAsync: the progress callback keeps the status line counting
    // "n / m" instead of a frozen label, and the done callback finishes (or
    // rolls back) the import. Roots are consumed by the session on every path.
    auto* cb = new AiTocImportCbCtx{dlg, ++dlg->importGen};
    dlg->importCtx = cb;
    if (!StartTocCalibAsync(win, roots, dlg->work->engine, true, AiTocImportCalibProgress, AiTocImportDoneCb, cb)) {
        if (dlg->importCtx == cb) {
            dlg->importCtx = nullptr;
        }
        delete cb;
        AiTocHideImportProgress(dlg);
        MessageBoxWarning(dlg->hwnd, "The JSON contains no importable table-of-contents items.",
                          _TRA("AI Recognize Table of Contents"));
        return false;
    }
    logf("AI TOC: import started items=%d pages=%d-%d offset=%d\n", stack.Size(), dlg->work->firstPage,
         dlg->work->lastPage, arabicOffset);
    return true;
}

// Body-TOC v2 import: the AI only returns candidate_id + level. Every fact
// (title, PDF page, bbox) is restored from the local scan; invalid or unknown
// candidate ids are rejected by ParseBodyTocSelections().
static bool AiTocImportBodyJson(AiTocDialog* dlg, const char* text) {
    AiTocSetImportProgress(dlg, _TRW("Importing TOC: parsing the AI reply…"));
    if (!dlg->bodyScan) {
        AiTocHideImportProgress(dlg);
        return false;
    }
    Vec<BodyTocSelection> selections;
    if (!ParseBodyTocSelections(text, *dlg->bodyScan, selections)) {
        AiTocHideImportProgress(dlg);
        MessageBoxWarning(dlg->hwnd, _TRA("AI returned invalid JSON."), _TRA("AI Recognize Table of Contents"));
        return false;
    }
    AiTocSetImportProgress(dlg, _TRW("Importing TOC: restoring titles and page numbers…"));
    Vec<ExtractedTocItem*> roots;
    if (!BuildBodyTocItems(*dlg->bodyScan, selections, roots)) {
        AiTocHideImportProgress(dlg);
        DeleteExtractedTocItems(roots);
        MessageBoxWarning(dlg->hwnd, "The JSON contains no importable table-of-contents items.",
                          _TRA("AI Recognize Table of Contents"));
        return false;
    }
    MainWindow* win = dlg->win;
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    EngineBase* engine = dlg->bodyScan->engine;
    AiTocSetImportProgress(dlg, _TRW("Importing TOC: calibrating bookmark destinations…"));
    PrepareTocExtractionResult(engine, roots);
    if (roots.Size() == 0 || !dm || dm->GetEngine() != engine) {
        AiTocHideImportProgress(dlg);
        DeleteExtractedTocItems(roots);
        MessageBoxWarning(dlg->hwnd, "The JSON contains no importable table-of-contents items.",
                          _TRA("AI Recognize Table of Contents"));
        return false;
    }
    // Chunked calibration with live "n / m" progress, same as AiTocImportJson.
    auto* cb = new AiTocImportCbCtx{dlg, ++dlg->importGen};
    dlg->importCtx = cb;
    if (!StartTocCalibAsync(win, roots, engine, true, AiTocImportCalibProgress, AiTocImportDoneCb, cb)) {
        if (dlg->importCtx == cb) {
            dlg->importCtx = nullptr;
        }
        delete cb;
        AiTocHideImportProgress(dlg);
        MessageBoxWarning(dlg->hwnd, "The JSON contains no importable table-of-contents items.",
                          _TRA("AI Recognize Table of Contents"));
        return false;
    }
    logf("AI body TOC: import started selections=%d of candidates=%d\n", selections.Size(),
         dlg->bodyScan->candidates.Size());
    return true;
}

struct AiTocClipboardRead {
    HWND hwnd;
    HANDLE token;
    DWORD sequence;
    char* text = nullptr;
    bool retry = false;
};

static void AppendAiTocJsonString(StrBuilder& out, const char* s) {
    out.AppendChar('"');
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
            unsigned char c = *p;
            if (c == '"' || c == '\\') {
                out.AppendChar('\\');
                out.AppendChar((char)c);
            } else if (c == '\n') {
                out.Append("\\n");
            } else if (c == '\r') {
                out.Append("\\r");
            } else if (c < 0x20) {
                out.AppendFmt("\\u%04x", (unsigned)c);
            } else {
                out.AppendChar((char)c);
            }
        }
    }
    out.AppendChar('"');
}

static bool AiTocSamePrintedItem(const AiTocJsonItem& a, const AiTocJsonItem& b) {
    if (!str::Eq(a.title ? a.title : "", b.title ? b.title : "")) {
        return false;
    }
    if (a.hasPage != b.hasPage || a.printedPage != b.printedPage) {
        return false;
    }
    return str::Eq(a.printedLabel ? a.printedLabel : "", b.printedLabel ? b.printedLabel : "");
}

// Concatenate each batch's items array in order. No rewriting, no invented
// rows. An item that repeats the previous one (the same heading copied at a
// batch boundary) is kept once.
static TempStr AiTocConcatBatchJson(StrVec& batches) {
    StrBuilder out;
    out.Append("{\"items\":[");
    bool any = false;
    AiTocJsonItem prev{};
    for (int b = 0; b < batches.Size(); b++) {
        AiTocJsonVisitor parsed;
        if (!json::Parse(batches.At(b), &parsed)) {
            continue;
        }
        for (int i = 0; i < parsed.items.Size(); i++) {
            AiTocJsonItem& it = parsed.items[i];
            if (str::IsEmpty(it.title)) {
                continue;
            }
            if (any && AiTocSamePrintedItem(prev, it)) {
                continue;
            }
            if (any) {
                out.AppendChar(',');
            }
            any = true;
            out.Append("{\"title\":");
            AppendAiTocJsonString(out, it.title);
            out.Append(",\"page\":");
            TempStr pageDigits = str::FormatTemp("%d", it.printedPage);
            bool labelIsDigits = it.printedLabel && it.printedPage > 0 && str::Eq(it.printedLabel, pageDigits);
            if (it.printedLabel && it.printedLabel[0] && !labelIsDigits) {
                AppendAiTocJsonString(out, it.printedLabel);
            } else if (it.hasPage && it.printedPage > 0) {
                out.Append(pageDigits);
            } else {
                out.Append("null");
            }
            int level = it.level < 1 ? 1 : it.level;
            out.AppendFmt(",\"level\":%d}", level);
            str::ReplaceWithCopy(&prev.title, it.title);
            str::ReplaceWithCopy(&prev.printedLabel, it.printedLabel);
            prev.printedPage = it.printedPage;
            prev.hasPage = it.hasPage;
        }
    }
    prev.Free();
    out.Append("]}");
    if (!any) {
        return nullptr;
    }
    return str::DupTemp(out.LendData());
}

struct AiTocNextBatchWork {
    HWND owner = nullptr;
    HANDLE dialogToken = nullptr;
    HWND browser = nullptr;
    AiChatService service = AiChatService::Doubao;
    StrVec files;
    int batchIndex = 0;
    bool ok = false;
};

static void AiTocNextBatchFinished(AiTocNextBatchWork* w);

static void AiTocNextBatchWorker(AiTocNextBatchWork* w) {
    defer {
        uitask::Post(MkFunc0<AiTocNextBatchWork>(AiTocNextBatchFinished, w), "AiTocNextBatchFinished");
    };
    if (!w || !w->browser || w->files.Size() < 1) {
        if (w) {
            w->ok = false;
        }
        return;
    }
    AiTocPasteTarget target;
    target.owner = w->owner;
    target.dialogToken = w->dialogToken;
    target.browser = w->browser;
    target.service = w->service;
    target.files = &w->files;
    w->ok = AiTocPasteOneBatch(&target, w->batchIndex, false, true);
}

static void AiTocNextBatchFinished(AiTocNextBatchWork* w) {
    if (!w) {
        return;
    }
    HWND hwnd = w->owner;
    HANDLE token = w->dialogToken;
    bool ok = w->ok;
    delete w;
    if (!hwnd || !IsWindow(hwnd) || GetPropW(hwnd, kAiTocToken) != token) {
        return;
    }
    auto* dlg = (AiTocDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!dlg) {
        return;
    }
    if (!ok) {
        dlg->busy = false;
        dlg->submitted = true;
        SetWindowTextW(dlg->status, _TRW("Automatic send incomplete. Paste the clipboard content into the AI "
                                         "input box and send."));
        if (dlg->resend) {
            EnableWindow(dlg->resend, TRUE);
        }
        return;
    }
    AiTocArmPrintedBatchWait(dlg);
}

static void AiTocSendNextPrintedBatch(AiTocDialog* dlg) {
    if (!dlg->work || !dlg->work->browser || dlg->work->files.Size() < 1) {
        return;
    }
    dlg->submitted = false;
    dlg->busy = true;
    if (dlg->resend) {
        EnableWindow(dlg->resend, FALSE);
    }
    AiTocSetState(dlg, AiTocUiState::SendingToAi);
    const char* lang = trans::GetCurrentLangCode();
    bool zh = lang && (str::EqI(lang, "cn") || str::EqI(lang, "tw"));
    int which = dlg->batchesGot + 1;
    if (zh) {
        SetWindowTextW(dlg->status, ToWStrTemp(str::FormatTemp("正在发送第 %d 批…", which)));
    } else {
        SetWindowTextW(dlg->status, ToWStrTemp(str::FormatTemp("Sending batch %d…", which)));
    }
    auto* w = new AiTocNextBatchWork();
    w->owner = dlg->hwnd;
    w->dialogToken = dlg->token;
    w->browser = dlg->work->browser;
    w->service = dlg->work->service;
    w->batchIndex = dlg->batchesGot;
    for (int i = 0; i < dlg->work->files.Size(); i++) {
        w->files.Append(dlg->work->files.At(i));
    }
    RunAsync(MkFunc0<AiTocNextBatchWork>(AiTocNextBatchWorker, w), "AiTocNextBatch");
}

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
        SetWindowTextW(dlg->status,
                       _TRW("Clipboard is temporarily unreadable. Retrying; you can also copy the AI reply again."));
        return;
    }
    dlg->clipboardSequence = read->sequence;
    if (!read->text) return;
    // Accept code fences and explanatory text surrounding the JSON object.
    char* start = strchr(read->text, '{');
    char* end = strrchr(read->text, '}');
    if (!start || !end || end < start) return;
    end[1] = 0;
    // Body-structure round uses the v2 schema; the printed-TOC round keeps v1.
    if (dlg->bodyMode) {
        if (!IsBodyTocJsonCandidate(start)) return;
        if (AiTocImportBodyJson(dlg, start)) {
            // Calibration runs in chunked slices; AiTocImportDoneCb closes the
            // dialog (or rolls the status back) when the job finishes.
            return;
        }
        SetWindowTextW(dlg->status, _TRW("TOC import not finished. Copy the AI reply again to retry."));
        return;
    }
    if (!IsAiTocJsonCandidate(start)) return;
    if (dlg->work && dlg->work->imageBatches > 1) {
        if (dlg->collectedBatchJson.Size() > 0 &&
            str::Eq(dlg->collectedBatchJson.At(dlg->collectedBatchJson.Size() - 1), start)) {
            AiTocSetMultiBatchWaitCopy(dlg);
            return;
        }
        AiTocJsonVisitor probe;
        if (!json::Parse(start, &probe)) {
            SetWindowTextW(dlg->status, _TRW("TOC import not finished. Copy the AI reply again to retry."));
            return;
        }
        bool titled = false;
        for (int i = 0; i < probe.items.Size(); i++) {
            if (!str::IsEmpty(probe.items[i].title)) {
                titled = true;
                break;
            }
        }
        if (!titled) {
            SetWindowTextW(dlg->status, _TRW("TOC import not finished. Copy the AI reply again to retry."));
            return;
        }
        dlg->collectedBatchJson.Append(start);
        dlg->batchesGot++;
        logf("AI TOC: stored batch %d of %d\n", dlg->batchesGot, dlg->work->imageBatches);
        if (dlg->batchesGot < dlg->work->imageBatches) {
            AiTocSendNextPrintedBatch(dlg);
            return;
        }
        TempStr merged = AiTocConcatBatchJson(dlg->collectedBatchJson);
        if (merged && AiTocImportJson(dlg, merged)) {
            return;
        }
        dlg->batchesGot--;
        if (dlg->collectedBatchJson.Size() > 0) {
            dlg->collectedBatchJson.RemoveAt(dlg->collectedBatchJson.Size() - 1);
        }
        SetWindowTextW(dlg->status, _TRW("TOC import not finished. Copy the AI reply again to retry."));
        return;
    }
    if (AiTocImportJson(dlg, start)) {
        return;
    }
    SetWindowTextW(dlg->status, _TRW("TOC import not finished. Copy the AI reply again to retry."));
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

// Scale a logical DIP metric to physical pixels with the dialog's current
// monitor DPI. Always DIP -> physical, never physical -> physical, so moving
// the dialog across monitors can never accumulate scaling.
static int AiTocS(AiTocDialog* dlg, int v) {
    return MulDiv(v, dlg->dpi, 96);
}

// Muted one-step-weaker text color for hints, notes and inactive phase labels.
static COLORREF AiTocMutedTextColor() {
    bool dark = ThemeUsesDarkChrome();
    return dark ? RGB(148, 152, 160) : RGB(110, 114, 122);
}

static int AiTocSelectedCount(AiTocDialog* dlg) {
    int n = 0;
    if (!dlg->detectWork) {
        return 0;
    }
    for (int i = 0; i < dlg->detectWork->initiallySelected.Size() && i < dlg->detectWork->pageNos.Size(); i++) {
        if (dlg->detectWork->initiallySelected[i]) {
            n++;
        }
    }
    return n;
}

static void AiTocUpdateSendEnabled(AiTocDialog* dlg) {
    bool canSend = AiTocSelectedCount(dlg) > 0;
    if (dlg->detectWork && dlg->detectWork->pageNos.Size() == 0) {
        Str pageText = GetWindowTextTemp(dlg->pagesEdit);
        canSend = pageText.s && *pageText.s;
    }
    if (dlg->busy || dlg->submitted) {
        canSend = false;
    }
    EnableWindow(dlg->send, canSend);
}

// Esc cancels the dialog even when the pages edit has focus.
static WNDPROC gAiTocEditOrigProc = nullptr;

static LRESULT CALLBACK AiTocEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)hwnd);
        return 0;
    }
    return CallWindowProcW(gAiTocEditOrigProc, hwnd, msg, wp, lp);
}

// All layout metrics for one dialog state, in physical px (computed from
// logical DIP with the dialog's current DPI — never cached).
struct AiTocThumbGeom;
static AiTocThumbGeom AiTocThumbGeomFor(AiTocDialog* dlg, int viewportW);

struct AiTocLayoutMetrics {
    int w = 0;
    int m = 0;
    int lineH = 0;
    int phaseH = 0;
    int editH = 0;
    int btnH = 0;
    int btnGap = 0;
    int phaseY = 0;
    int labelY = 0;
    int editY = 0;
    int statusY = 0;
    int paneTop = 0;
    int clientH = 0;
    int btnBottomMargin = 0;
    // Fallback-only rows (ChooseFallback state).
    int stateTitleY = 0;
    int stateDescY = 0;
    int manualBtnY = 0;
    int bodyTitleY = 0;
    int bodyDescY = 0;
    int bodyBtnY = 0;
    // Actual line count of the shared stateDesc control (re-read from its
    // current text on every pass): sizing the static larger than its text
    // lets its empty rows erase the status line below (z-order).
    int descLines = 0;
    // Shared width of the two content buttons ("Specify TOC Pages" /
    // "Generate TOC From Body"): ideal-size of the widest label, so no
    // language clips inside a fixed box. Both always match.
    int branchBtnW = 0;
};

// Pure math pass: stack the fixed header top-down (identical in Scanning and
// Review — the header is the visual anchor), then add the state-specific body
// and footer. Used by both the live relayout and the initial centering, so
// the scanning window can be placed where the expanded review window will be.
static void AiTocComputeLayout(AiTocDialog* dlg, bool expanded, bool fallback, AiTocLayoutMetrics& mt) {
    mt.w = AiTocS(dlg, 660);
    mt.m = AiTocS(dlg, 22);
    mt.lineH = AiTocS(dlg, 17);
    mt.phaseH = AiTocS(dlg, 20);
    mt.editH = AiTocS(dlg, 26);
    mt.btnH = AiTocS(dlg, 23);
    mt.btnGap = AiTocS(dlg, 8);
    // The stateDesc control is shared by the fallback page and the waiting
    // page; its height must follow the text that is currently set, otherwise
    // the static's empty rows cover the status line underneath it (later
    // created statics sit higher in z-order).
    WCHAR desc[512]{};
    GetWindowTextW(dlg->stateDesc, desc, 512);
    mt.descLines = 1;
    for (const WCHAR* p = desc; *p; p++) {
        if (*p == L'\n') {
            mt.descLines++;
        }
    }
    // Content buttons size to their own label (BCM_GETIDEALSIZE): the wider
    // body-branch label wins and Specify TOC Pages matches it, per design.
    mt.branchBtnW = AiTocS(dlg, 122);
    for (HWND b : {dlg->manualBtn, dlg->bodyBtn}) {
        SIZE sz{};
        if (b && SendMessageW(b, BCM_GETIDEALSIZE, 0, (LPARAM)&sz) && sz.cx > mt.branchBtnW) {
            mt.branchBtnW = sz.cx;
        }
    }
    int y = AiTocS(dlg, 18);
    mt.phaseY = y;
    y += mt.phaseH + AiTocS(dlg, 16);
    mt.labelY = y;
    y += mt.lineH + AiTocS(dlg, 6);
    mt.editY = y;
    y += mt.editH + AiTocS(dlg, 16);
    mt.statusY = y;
    y += mt.lineH;
    mt.btnBottomMargin = AiTocS(dlg, 14);
    int footerReserve = mt.btnH + mt.btnBottomMargin + AiTocS(dlg, 10);
    if (fallback) {
        // Fallback choice page: two compact branches, each a bold title +
        // description + one right-aligned action button. The manual-pages
        // entry (图4) is reached via 手工指定目录页, which opens the full
        // confirm page with the pages edit and the thumbnail grid — it no
        // longer lives inline on this page.
        y = mt.phaseY + mt.phaseH + AiTocS(dlg, 18);
        mt.stateTitleY = y;
        y += mt.lineH + AiTocS(dlg, 8);
        mt.stateDescY = y;
        y += mt.lineH * mt.descLines + AiTocS(dlg, 6); // description lines
        mt.manualBtnY = y;                             // bottom-right of the intro block
        y += mt.btnH + AiTocS(dlg, 18);
        mt.bodyTitleY = y;
        y += mt.lineH + AiTocS(dlg, 7);
        mt.bodyDescY = y;
        // 从正文生成目录 button: no row of its own; place it just below the
        // description baseline for a more balanced second branch.
        mt.bodyBtnY = mt.bodyDescY + AiTocS(dlg, 5);
        y += mt.lineH + AiTocS(dlg, 14);
        mt.statusY = y;
        y += mt.lineH;
        mt.clientH = y + AiTocS(dlg, 20) + footerReserve;
    } else if (expanded) {
        y += AiTocS(dlg, 12);
        mt.paneTop = y;
        // Exactly two full rows: 3 columns x 2 rows show 6 candidate pages;
        // anything more scrolls inside the viewport. The dialog height never
        // follows the page count.
        AiTocThumbGeom g = AiTocThumbGeomFor(dlg, mt.w - 2 * mt.m);
        int paneDesired = 2 * g.height + g.gap + 2 * g.pad;
        mt.clientH = mt.paneTop + paneDesired + AiTocS(dlg, 12) + footerReserve;
    } else {
        // Scanning body: a single status row (text + dot marquee + counter).
        // Waiting body: the bold state title + description replace the manual
        // pages section (same rows), then the status row. The description line
        // count follows the current text (privacy note adds a third line).
        if (dlg->state == AiTocUiState::WaitingForAiClipboard) {
            mt.stateTitleY = mt.labelY;
            mt.stateDescY = mt.editY;
            mt.statusY = mt.stateDescY + mt.lineH * mt.descLines + AiTocS(dlg, 10);
            y = mt.statusY + mt.lineH; // consume the status row
        }
        mt.clientH = y + AiTocS(dlg, 24) + footerReserve;
    }
}

static void AiTocCenterOverMainWindow(AiTocDialog* dlg) {
    if (!dlg || !dlg->win || !dlg->win->hwndFrame) {
        return;
    }
    // Center the FINAL review size (same logical width, full review height),
    // not the small scanning window: the scanning phase then sits slightly
    // above center, and the later top-fixed expansion lands near the middle.
    AiTocLayoutMetrics mt;
    AiTocComputeLayout(dlg, true, false, mt);
    RECT outer{}, client{};
    GetWindowRect(dlg->hwnd, &outer);
    GetClientRect(dlg->hwnd, &client);
    int width = mt.w + (outer.right - outer.left) - client.right;
    int height = mt.clientH + (outer.bottom - outer.top) - client.bottom;
    HMONITOR mon = MonitorFromWindow(dlg->win->hwndFrame, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    int x = 0;
    int y = 0;
    if (GetMonitorInfoW(mon, &mi)) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - height) / 2;
        if (y < mi.rcWork.top + AiTocS(dlg, 16)) {
            y = mi.rcWork.top + AiTocS(dlg, 16);
        }
        if (y + height > mi.rcWork.bottom - AiTocS(dlg, 16)) {
            y = mi.rcWork.bottom - AiTocS(dlg, 16) - height;
        }
    } else {
        RECT owner{};
        GetWindowRect(dlg->win->hwndFrame, &owner);
        x = owner.left + ((owner.right - owner.left) - width) / 2;
        y = owner.top + ((owner.bottom - owner.top) - height) / 2;
    }
    SetWindowPos(dlg->hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

// Which phase label the current workflow stage highlights. Derived solely
// from AiTocUiState so both source modes light up the same task stages:
// preparation states bold 准备内容, send/wait states bold AI 处理, import
// states bold 导入目录.
static bool AiTocPhaseActive(AiTocDialog* dlg, HWND phase) {
    switch (dlg->state) {
        case AiTocUiState::ScanningPrintedToc:
        case AiTocUiState::ConfirmPrintedToc:
        case AiTocUiState::ChooseFallback:
        case AiTocUiState::ScanningBodyStructure:
            return phase == dlg->phasePrep;
        case AiTocUiState::SendingToAi:
        case AiTocUiState::WaitingForAiClipboard:
            return phase == dlg->phaseAi;
        case AiTocUiState::ImportingResult:
            return phase == dlg->phaseImport;
    }
    return false;
}

static LRESULT AiTocColorControl(AiTocDialog* dlg, HDC dc, HWND control) {
    bool edit = control == dlg->pagesEdit;
    COLORREF text = ThemeWindowTextColor();
    // Current phase gets the theme selection color, everything auxiliary is
    // muted one step, matching a classic Win32 settings page hierarchy.
    if (control == dlg->phasePrep || control == dlg->phaseAi || control == dlg->phaseImport) {
        bool active = AiTocPhaseActive(dlg, control);
        text = active ? GetSelectionHighlightColor() : AiTocMutedTextColor();
    } else if (control == dlg->phaseArrow || control == dlg->phaseArrow2) {
        text = AiTocMutedTextColor();
    }
    SetTextColor(dc, text);
    SetBkColor(dc, edit ? ThemeWindowControlBackgroundColor() : ThemeWindowBackgroundColor());
    return (LRESULT)(edit ? dlg->controlBrush : dlg->backgroundBrush);
}

// Deferred second pass after a theme switch: if the synchronous re-render in
// RefreshAiTocWindowsTheme raced with the render pipeline and left visible
// slots null, fill them once the theme change has fully settled.
static void AiTocThemeRenderRetry(AiTocDialog* dlg) {
    if (!GetPropW(dlg->hwnd, kAiTocToken)) {
        return;
    }
    if (dlg->thumbnailThemeEpoch != ThemeEpoch()) {
        return; // another theme change is already pending
    }
    AiTocLayoutThumbnails(dlg);
    AiTocRenderVisibleThumbnails(dlg);
}

// Thumbnail grid constants, in logical DIP. The card width is not fixed:
// the viewport is divided evenly across 3 columns (falling back to 2 when
// the per-column width would drop below kAiTocThumbMinWidthDip), and the
// card height keeps the PDF page aspect ratio (3:4).
static constexpr int kAiTocThumbGapDip = 12;
static constexpr int kAiTocPanePadDip = 10;
static constexpr int kAiTocThumbMinWidthDip = 120;
static constexpr int kAiTocThumbCols = 3;
// Owner-drawn thumbnail STATICs carry id base + page index so both click
// handling and painting can recover the row they belong to.
static constexpr int kAiTocImgBaseId = 1000;

static void AiTocCreateThumbnailSlot(AiTocDialog* dlg) {
    // SS_NOTIFY makes the thumbnail itself clickable, matching the home-page
    // book-cover selection style instead of a detached checkbox below.
    HWND image =
        CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | SS_OWNERDRAW | SS_NOTIFY, 0, 0, 1, 1, dlg->thumbnailPane,
                        (HMENU)(INT_PTR)kAiTocImgBaseId, GetModuleHandleW(nullptr), nullptr);
    dlg->thumbnailImages.Append(image);
    if (UseDarkModeLib()) DarkMode::setChildCtrlsSubclassAndTheme(dlg->thumbnailPane);
}

// (Re)compute the grid geometry from the current viewport width and DPI:
// DIP -> physical on every call, so a DPI or monitor change simply recomputes.
static AiTocThumbGeom AiTocThumbGeomFor(AiTocDialog* dlg, int viewportW) {
    HWND ref = dlg->thumbnailPane ? dlg->thumbnailPane : dlg->hwnd;
    AiTocThumbGeom g;
    g.gap = DpiScale(ref, kAiTocThumbGapDip);
    g.pad = DpiScale(ref, kAiTocPanePadDip);
    int minW = DpiScale(ref, kAiTocThumbMinWidthDip);
    // Reserve room for the pane's vertical scrollbar so cards never sit
    // underneath it.
    int avail = viewportW - 2 * g.pad - GetSystemMetrics(SM_CXVSCROLL);
    g.cols = kAiTocThumbCols;
    while (g.cols > 1 && (avail - (g.cols - 1) * g.gap) / g.cols < minW) {
        g.cols--;
    }
    if (avail < g.cols * 40) {
        avail = g.cols * 40;
    }
    g.width = (avail - (g.cols - 1) * g.gap) / g.cols;
    g.height = MulDiv(g.width, 4, 3); // keep the page aspect ratio
    g.pitch = g.height + g.gap;
    // Center the whole grid within the FULL pane client width, not the
    // scrollbar-excluded area: centering inside `avail` left the right side
    // wider by exactly the scrollbar width. Here left/right whitespace to
    // the pane edges is symmetric (the thin scrollbar sits inside the right
    // margin); gridW <= avail still guarantees clearance from the scrollbar.
    int gridW = g.cols * g.width + (g.cols - 1) * g.gap;
    g.originX = (viewportW - gridW) / 2;
    return g;
}

static void AiTocLayoutThumbnails(AiTocDialog* dlg) {
    if (!dlg->thumbnailPane || !dlg->detectWork) {
        return;
    }
    RECT client{};
    GetClientRect(dlg->thumbnailPane, &client);
    AiTocThumbGeom g = AiTocThumbGeomFor(dlg, client.right);
    dlg->thumbGeom = g;
    int viewportHeight = client.bottom;
    int rows = (dlg->detectWork->pageNos.Size() + g.cols - 1) / g.cols;
    int contentHeight = 2 * g.pad + rows * g.height + (rows - 1) * g.gap;
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
    AiTocThumbGeom g = AiTocThumbGeomFor(dlg, client.right);
    dlg->thumbGeom = g;
    int firstRow = dlg->thumbnailScroll / g.pitch;
    int lastRow = (dlg->thumbnailScroll + client.bottom + g.pitch - 1) / g.pitch;
    firstRow = firstRow > 0 ? firstRow - 1 : 0;
    int first = firstRow * g.cols;
    int last = (lastRow + 1) * g.cols;
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
            continue;
        }
        int i = first + slot;
        int col = i % g.cols;
        int row = i / g.cols;
        int x = g.originX + col * (g.width + g.gap);
        int y = g.pad + row * g.pitch - dlg->thumbnailScroll;
        SetWindowLongPtrW(dlg->thumbnailImages[slot], GWL_ID, kAiTocImgBaseId + i);
        MoveWindow(dlg->thumbnailImages[slot], x, y, g.width, g.height, FALSE);
        if (i < dlg->detectWork->thumbnails.Size() && !dlg->detectWork->thumbnails[i]) {
            dlg->detectWork->thumbnails[i] = RenderAiTocThumbnail(dlg->detectWork->engine, dlg->detectWork->pageNos[i]);
        }
        auto* bitmap = i < dlg->detectWork->thumbnails.Size() ? dlg->detectWork->thumbnails[i] : nullptr;
        SetWindowLongPtrW(dlg->thumbnailImages[slot], GWLP_USERDATA, (LONG_PTR)bitmap);
        InvalidateRect(dlg->thumbnailImages[slot], nullptr, TRUE);
        ShowWindow(dlg->thumbnailImages[slot], SW_SHOWNA);
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

// Per-DPI fonts for the dialog. Both fonts are owned by the dialog and
// recreated whenever the monitor DPI changes.
static void AiTocRecreateFonts(AiTocDialog* dlg) {
    AppDialogFonts fonts;
    fonts.CreateForHwnd(dlg->hwnd);
    if (!fonts.body || !fonts.semibold) {
        fonts.Destroy();
        return;
    }
    if (dlg->font) {
        DeleteObject(dlg->font);
    }
    if (dlg->fontBold) {
        DeleteObject(dlg->fontBold);
    }
    dlg->font = fonts.body;
    dlg->fontBold = fonts.semibold;
    fonts.body = fonts.semibold = nullptr; // ownership transferred
}

static void AiTocRefreshOneTheme(HWND hwnd, AiTocDialog* dlg) {
    if (!dlg || !hwnd) {
        return;
    }
    DeleteObject(dlg->backgroundBrush);
    DeleteObject(dlg->controlBrush);
    dlg->backgroundBrush = CreateSolidBrush(ThemeWindowBackgroundColor());
    dlg->controlBrush = CreateSolidBrush(ThemeWindowControlBackgroundColor());
    if (dlg->detectWork && dlg->thumbnailThemeEpoch != ThemeEpoch()) {
        dlg->thumbnailThemeEpoch = ThemeEpoch();
        for (int i = 0; i < dlg->detectWork->thumbnails.Size(); i++) {
            delete dlg->detectWork->thumbnails[i];
            dlg->detectWork->thumbnails[i] = nullptr;
        }
    }
    AppDialogApplyChrome(hwnd);
    if (UseDarkModeLib() && dlg->thumbnailPane) {
        DarkMode::setDarkScrollBar(dlg->thumbnailPane);
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    AiTocLayoutThumbnails(dlg);
    AiTocRenderVisibleThumbnails(dlg);
    uitask::Post(MkFunc0<AiTocDialog>(AiTocThemeRenderRetry, dlg), "AiTocThemeRenderRetry");
}

static void AiTocThemeRefreshCb(HWND hwnd, void* ctx) {
    AiTocRefreshOneTheme(hwnd, (AiTocDialog*)ctx);
}

void RefreshAiTocWindowsTheme() {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM) -> BOOL {
            if (!GetPropW(hwnd, kAiTocToken)) {
                return TRUE;
            }
            auto* dlg = (AiTocDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            AiTocRefreshOneTheme(hwnd, dlg);
            return TRUE;
        },
        0);
}

static void AiTocApplyStateFonts(AiTocDialog* dlg) {
    SendMessageW(dlg->phasePrep, WM_SETFONT,
                 (WPARAM)(AiTocPhaseActive(dlg, dlg->phasePrep) ? dlg->fontBold : dlg->font), TRUE);
    SendMessageW(dlg->phaseArrow, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    SendMessageW(dlg->phaseAi, WM_SETFONT, (WPARAM)(AiTocPhaseActive(dlg, dlg->phaseAi) ? dlg->fontBold : dlg->font),
                 TRUE);
    SendMessageW(dlg->phaseArrow2, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    SendMessageW(dlg->phaseImport, WM_SETFONT,
                 (WPARAM)(AiTocPhaseActive(dlg, dlg->phaseImport) ? dlg->fontBold : dlg->font), TRUE);
    // Fallback page headings stay bold in every layout pass.
    SendMessageW(dlg->stateTitle, WM_SETFONT, (WPARAM)dlg->fontBold, TRUE);
    SendMessageW(dlg->bodyTitle, WM_SETFONT, (WPARAM)dlg->fontBold, TRUE);
}

static void AiTocApplyFonts(AiTocDialog* dlg) {
    HWND ctrls[] = {dlg->phasePrep,  dlg->phaseArrow, dlg->phaseAi,   dlg->phaseArrow2, dlg->phaseImport,
                    dlg->pagesLabel, dlg->pagesEdit,  dlg->status,    dlg->scanCount,   dlg->send,
                    dlg->resend,     dlg->bodyBtn,    dlg->manualBtn, dlg->stateTitle,  dlg->stateDesc,
                    dlg->bodyTitle,  dlg->bodyDesc};
    for (HWND c : ctrls) {
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    }
    HWND cancel = GetDlgItem(dlg->hwnd, IDCANCEL);
    if (cancel) SendMessageW(cancel, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    AiTocApplyStateFonts(dlg);
}

// Single source of truth for the dialog layout. Every metric is a logical
// DIP value scaled by the current monitor DPI, and the window height is
// decided first (per state, clamped to the monitor work area); the thumbnail
// viewport then simply takes the remaining space and scrolls internally.
static void AiTocLayoutControls(AiTocDialog* dlg) {
    HWND hwnd = dlg->hwnd;
    AiTocLayoutMetrics mt;
    // Waiting (after 发送) reuses the compact scanning layout: pane hidden,
    // single status row — the dialog shrinks back so it stops covering the
    // document behind it.
    bool expanded = dlg->review && !dlg->waiting;
    bool fallback = dlg->state == AiTocUiState::ChooseFallback;
    AiTocComputeLayout(dlg, expanded, fallback, mt);

    // Clamp the desired size to the current monitor's work area (recomputed
    // on every relayout, so a move to another display picks up its own work
    // area and the dialog never touches the screen or taskbar edges).
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    int safeTop = 0;
    int safeBottom = INT_MAX;
    if (GetMonitorInfoW(mon, &mi)) {
        int maxW = MulDiv(mi.rcWork.right - mi.rcWork.left, 90, 100);
        int maxH = MulDiv(mi.rcWork.bottom - mi.rcWork.top, 80, 100);
        if (mt.w > maxW) mt.w = maxW;
        if (mt.clientH > maxH) mt.clientH = maxH;
        safeTop = mi.rcWork.top + AiTocS(dlg, 16);
        safeBottom = mi.rcWork.bottom - AiTocS(dlg, 16);
    }

    RECT outer{}, oldClient{};
    GetWindowRect(hwnd, &outer);
    GetClientRect(hwnd, &oldClient);
    int frameW = (outer.right - outer.left) - oldClient.right;
    int frameH = (outer.bottom - outer.top) - oldClient.bottom;
    if ((dlg->review || fallback) && IsWindowVisible(hwnd)) {
        // State transition, not a new window: keep left/top (and the width,
        // which is identical in both states) and only grow downward. Shift
        // up by the minimal overflow only when the bottom would cross the
        // work area — never re-center.
        int newH = mt.clientH + frameH;
        int newTop = outer.top;
        if (newTop + newH > safeBottom) {
            newTop = std::max(safeTop, newTop - (newTop + newH - safeBottom));
        }
        SetWindowPos(hwnd, nullptr, outer.left, newTop, mt.w + frameW, newH, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(hwnd, nullptr, 0, 0, mt.w + frameW, mt.clientH + frameH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    GetClientRect(hwnd, &oldClient);

    // --- move pass with final, clamped metrics ---
    int contentW = mt.w - 2 * mt.m;
    // Phase hint row: widths follow the label text measured with the BOLD
    // font (the widest state of each label), so no language clips ("Prepare
    // Content" is far wider than 准备内容). Left-to-right stacking with a
    // fixed small gap replaces the old hardcoded DIP offsets.
    {
        HDC dc = GetDC(hwnd);
        HGDIOBJ oldFont = SelectObject(dc, dlg->fontBold);
        WCHAR buf[64]{};
        SIZE tsz{};
        // Measure with the bold font + a little right slack: the statics use
        // SS_LEFTNOWORDWRAP, so any shortfall shows up as clipped text.
        auto labelW = [&](HWND h) -> int {
            GetWindowTextW(h, buf, 64);
            GetTextExtentPoint32W(dc, buf, (int)wcslen(buf), &tsz);
            return tsz.cx + AiTocS(dlg, 2);
        };
        int wPrep = labelW(dlg->phasePrep);
        int wAi = labelW(dlg->phaseAi);
        int wImport = labelW(dlg->phaseImport);
        SelectObject(dc, dlg->font);
        int wArrow = std::max(AiTocS(dlg, 14), labelW(dlg->phaseArrow));
        SelectObject(dc, oldFont);
        ReleaseDC(hwnd, dc);
        int gap = AiTocS(dlg, 3);
        int x = mt.m;
        MoveWindow(dlg->phasePrep, x, mt.phaseY, wPrep, mt.phaseH, TRUE);
        x += wPrep + gap;
        MoveWindow(dlg->phaseArrow, x, mt.phaseY, wArrow, mt.phaseH, TRUE);
        x += wArrow + gap;
        MoveWindow(dlg->phaseAi, x, mt.phaseY, wAi, mt.phaseH, TRUE);
        x += wAi + gap;
        MoveWindow(dlg->phaseArrow2, x, mt.phaseY, wArrow, mt.phaseH, TRUE);
        x += wArrow + gap;
        MoveWindow(dlg->phaseImport, x, mt.phaseY, wImport, mt.phaseH, TRUE);
    }
    MoveWindow(dlg->pagesLabel, mt.m, mt.labelY, contentW, mt.lineH, TRUE);
    MoveWindow(dlg->pagesEdit, mt.m, mt.editY, contentW, mt.editH, TRUE);
    // Fallback page sections: the two separated branches and the divider are
    // fallback-only; the state title/description also serve the unified
    // waiting page (different texts, see AiTocApplyWaitingTexts).
    // Import reuses the waiting page chrome (title/desc/resend); only the
    // status row swaps in a live counter.
    bool waitingState =
        dlg->state == AiTocUiState::WaitingForAiClipboard || dlg->state == AiTocUiState::ImportingResult;
    ShowWindow(dlg->stateTitle, (fallback || waitingState) ? SW_SHOWNOACTIVATE : SW_HIDE);
    ShowWindow(dlg->stateDesc, (fallback || waitingState) ? SW_SHOWNOACTIVATE : SW_HIDE);
    ShowWindow(dlg->bodyTitle, fallback ? SW_SHOWNOACTIVATE : SW_HIDE);
    ShowWindow(dlg->bodyDesc, fallback ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (fallback) {
        MoveWindow(dlg->stateTitle, mt.m, mt.stateTitleY, contentW, mt.lineH, TRUE);
        MoveWindow(dlg->stateDesc, mt.m, mt.stateDescY, contentW, mt.lineH * mt.descLines, TRUE);
        // Body-branch text must NOT extend under its button: later-created
        // statics sit above the button in z-order and would erase its bottom
        // edge on the first paint (hover then "fixed" it). Reserve the shared
        // button column (+gap) so the rects never intersect.
        int bodyTextW = contentW - (mt.branchBtnW + AiTocS(dlg, 12));
        MoveWindow(dlg->bodyTitle, mt.m, mt.bodyTitleY, bodyTextW, mt.lineH, TRUE);
        MoveWindow(dlg->bodyDesc, mt.m, mt.bodyDescY, bodyTextW, mt.lineH, TRUE);
        // 手工指定目录页 sits at the bottom-right of the intro block, visually
        // answering the "或手动指定目录页" sentence above it; its row mirrors
        // the body branch's button row for a balanced two-branch rhythm.
        if (dlg->manualBtn) {
            MoveWindow(dlg->manualBtn, mt.w - mt.m - mt.branchBtnW, mt.manualBtnY, mt.branchBtnW, mt.btnH, TRUE);
            ShowWindow(dlg->manualBtn, SW_SHOWNOACTIVATE);
        }
        // 从正文生成目录: same size as 手工指定目录页, vertically centered
        // with a small downward offset from its description (text left,
        // action right).
        if (dlg->bodyBtn) {
            MoveWindow(dlg->bodyBtn, mt.w - mt.m - mt.branchBtnW, mt.bodyBtnY, mt.branchBtnW, mt.btnH, TRUE);
            ShowWindow(dlg->bodyBtn, SW_SHOWNOACTIVATE);
        }
    } else if (waitingState) {
        // Unified waiting page: bold title + description in place of the
        // manual pages section. The description height follows its real line
        // count so it never reaches into the status row below (z-order).
        MoveWindow(dlg->stateTitle, mt.m, mt.stateTitleY, contentW, mt.lineH, TRUE);
        MoveWindow(dlg->stateDesc, mt.m, mt.stateDescY, contentW, mt.lineH * mt.descLines, TRUE);
    } else if (dlg->bodyBtn) {
        ShowWindow(dlg->bodyBtn, SW_HIDE);
    }
    // The manual pages section shows only on printed-flow pages (scanning /
    // confirm review). The fallback choice page and the unified waiting page
    // never show it — manual entry has its own confirm page now.
    bool bodyFlow = dlg->mode == AiTocSourceMode::BodyStructure;
    ShowWindow(dlg->pagesLabel, (fallback || bodyFlow || waitingState) ? SW_HIDE : SW_SHOWNOACTIVATE);
    ShowWindow(dlg->pagesEdit, (fallback || bodyFlow || waitingState) ? SW_HIDE : SW_SHOWNOACTIVATE);
    // Status row: scanning shows "正在扫描目录页 ● ○ ○ 12 / 30" as one tight
    // cluster (counter right after the dot marquee, not far-aligned right);
    // import calibration shows "正在导入目录：校准书签位置…  n / m" with the
    // same scanCount control so only the digits invalidate.
    bool showScanWidgets = dlg->scanning && !dlg->review && !dlg->waiting;
    // Only reveal the shared counter once calib has written "n / m"; parsing /
    // arranging phases keep it hidden (AiTocSetImportProgress clears it).
    bool showImportCount = false;
    if (dlg->importing && dlg->state == AiTocUiState::ImportingResult && !showScanWidgets && dlg->scanCount) {
        WCHAR cnt[32]{};
        GetWindowTextW(dlg->scanCount, cnt, 32);
        showImportCount = cnt[0] != 0;
    }
    MoveWindow(dlg->status, mt.m, mt.statusY, contentW, mt.lineH, TRUE);
    ShowWindow(dlg->scanCount, (showScanWidgets || showImportCount) ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (!showScanWidgets) {
        dlg->dotsRect = {};
    }
    if (showScanWidgets || showImportCount) {
        WCHAR statusText[128]{};
        GetWindowTextW(dlg->status, statusText, 128);
        HDC dc = GetDC(hwnd);
        HGDIOBJ oldF = SelectObject(dc, dlg->font);
        SIZE tsz{};
        GetTextExtentPoint32W(dc, statusText, (int)wcslen(statusText), &tsz);
        SelectObject(dc, oldF);
        ReleaseDC(hwnd, dc);
        if (showScanWidgets) {
            // Dot marquee rect: right after the status text on the same row, so
            // the timer can invalidate just this tiny area.
            int dotD = AiTocS(dlg, 7);
            int dotGap = AiTocS(dlg, 6);
            int dotTop = mt.statusY + (mt.lineH - dotD) / 2;
            int dotLeft = mt.m + tsz.cx + AiTocS(dlg, 12);
            dlg->dotsRect = {dotLeft, dotTop, dotLeft + 3 * dotD + 2 * dotGap, dotTop + dotD + 1};
            // Counter sits right after the dots: "正在扫描目录页 ● ○ ○  12 / 30".
            AiTocPlaceScanCountAfterStatus(dlg, mt.statusY, mt.lineH, dlg->dotsRect.right + AiTocS(dlg, 10));
        } else {
            AiTocPlaceScanCountAfterStatus(dlg, mt.statusY, mt.lineH, mt.m + tsz.cx + AiTocS(dlg, 8));
        }
    }

    int btnTop = oldClient.bottom - mt.btnBottomMargin - mt.btnH;
    int btnW = AiTocS(dlg, 92); // 发送目录页 needs more room than the old 发送
    dlg->sepY = btnTop - AiTocS(dlg, 10);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), mt.w - mt.m - btnW, btnTop, btnW, mt.btnH, TRUE);
    MoveWindow(dlg->send, mt.w - mt.m - btnW * 2 - mt.btnGap, btnTop, btnW, mt.btnH, TRUE);
    // Fallback page keeps only 取消 in the footer: both branch actions are
    // content buttons. Hide the send button entirely once the body branch is
    // running: the body flow sends automatically and must not invite manual
    // resending.
    if (dlg->send) {
        ShowWindow(dlg->send, !fallback && !bodyFlow && !waitingState ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    if (dlg->manualBtn) {
        ShowWindow(dlg->manualBtn, fallback ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    // [重新发送] lives on the unified waiting page only, left of 取消; it
    // resubmits the cached payload without re-scanning.
    if (dlg->resend) {
        MoveWindow(dlg->resend, mt.w - mt.m - btnW * 2 - mt.btnGap, btnTop, btnW, mt.btnH, TRUE);
        ShowWindow(dlg->resend, waitingState ? SW_SHOWNOACTIVATE : SW_HIDE);
    }

    if (dlg->thumbnailPane) {
        // Flexible viewport: whatever is left between the status row and the
        // footer separator. The pane's own scrollbar handles overflow.
        int paneH = btnTop - AiTocS(dlg, 10 + 8) - mt.paneTop;
        if (paneH < AiTocS(dlg, 60)) {
            paneH = AiTocS(dlg, 60);
        }
        MoveWindow(dlg->thumbnailPane, mt.m, mt.paneTop, contentW, paneH, TRUE);
        ShowWindow(dlg->thumbnailPane, (dlg->review && !dlg->waiting) ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (dlg->review && !dlg->waiting && dlg->detectWork) {
            AiTocLayoutThumbnails(dlg);
            AiTocRenderVisibleThumbnails(dlg);
        }
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

// Transition from the scanning phase to the review phase in place: same
// HWND, same left/top/width, phase fonts swap, scan widgets hide and the
// thumbnail pane appears below while the window grows downward only.
static void AiTocEnterReview(AiTocDialog* dlg) {
    dlg->review = true;
    AiTocSetState(dlg, AiTocUiState::ConfirmPrintedToc);
    dlg->scanning = false;
    KillTimer(dlg->hwnd, 2); // stop the dot marquee immediately
    AiTocApplyStateFonts(dlg);
    ShowWindow(dlg->scanCount, SW_HIDE);
    if (!dlg->thumbnailPane) {
        dlg->thumbnailPane = CreateWindowExW(0, kAiTocThumbPaneClass, nullptr, WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN,
                                             0, 0, 10, 10, dlg->hwnd, nullptr, GetModuleHandleW(nullptr), dlg);
        if (UseDarkModeLib()) {
            DarkMode::setChildCtrlsTheme(dlg->thumbnailPane);
            DarkMode::setDarkScrollBar(dlg->thumbnailPane);
        }
    }
    int firstSelected = -1;
    for (int i = 0; i < dlg->detectWork->initiallySelected.Size(); i++) {
        if (dlg->detectWork->initiallySelected[i]) {
            firstSelected = i;
            break;
        }
    }
    dlg->thumbnailScroll = 0;
    SetWindowTextW(dlg->status, _TRW("✓ Likely TOC pages found. Please confirm the selection."));
    AiTocLayoutControls(dlg);
    // Open scrolled to the first auto-selected page (grid is laid out now,
    // so the geometry is valid).
    if (firstSelected > 0) {
        int row = firstSelected / dlg->thumbGeom.cols;
        AiTocScrollThumbnails(dlg, row * dlg->thumbGeom.pitch);
    }
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

static int AiTocHitTestThumb(AiTocDialog* dlg, const RECT& client, POINT pt) {
    if (!dlg->detectWork) {
        return -1;
    }
    // Use the live grid geometry (recomputed at each relayout).
    RECT paneClient{};
    GetClientRect(dlg->thumbnailPane, &paneClient);
    AiTocThumbGeom g = AiTocThumbGeomFor(dlg, paneClient.right);
    int px = pt.x - g.originX;
    if (px < 0 || px >= g.cols * g.width + (g.cols - 1) * g.gap) {
        return -1;
    }
    int col = px / (g.width + g.gap);
    if (col >= g.cols) {
        col = g.cols - 1;
    }
    int py = pt.y + dlg->thumbnailScroll - g.pad;
    if (py < 0) {
        return -1;
    }
    int inRow = py % g.pitch;
    if (inRow >= g.height) {
        return -1; // in the gap between rows
    }
    int i = (py / g.pitch) * g.cols + col;
    if (i >= dlg->detectWork->pageNos.Size()) {
        return -1;
    }
    return i;
}

static Gdiplus::Color AiTocGdip(COLORREF col, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(col), GetGValue(col), GetBValue(col));
}

// Shared icon color tokens, mirroring the home-page thumb actions:
// idle glyph at muted alpha, active glyph near-black (near-white in dark).
static COLORREF AiTocBadgeIconColor(bool dark) {
    return dark ? RGB(214, 214, 220) : RGB(72, 72, 78);
}

static COLORREF AiTocBadgeActiveGlyphColor(bool dark) {
    return dark ? RGB(248, 248, 250) : RGB(28, 28, 32);
}

// Filled checkmark from Material Design's standard "check" icon
// (24x24 viewBox, path "M9 16.17 4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"),
// used the same way as the home page's filled pushpin path.
// clang-format off
static const Gdiplus::PointF kAiTocCheckOuter[] = {
    {9.f, 16.17f}, {4.83f, 12.f}, {3.41f, 13.41f}, {9.f, 19.f}, {21.f, 7.f}, {19.59f, 5.59f},
};
// clang-format on

static void AiTocFillIconPath(Gdiplus::Graphics& g, const Gdiplus::PointF* pts, int n, float viewBox,
                              const Gdiplus::PointF& origin, float size, float pad, COLORREF col, BYTE alpha) {
    if (size < 4.f) {
        return;
    }
    Gdiplus::REAL s = (size - pad * 2.f) / viewBox;
    Gdiplus::Matrix xf;
    xf.Translate(origin.X + pad, origin.Y + pad);
    xf.Scale(s, s);
    Gdiplus::GraphicsPath path;
    path.AddPolygon(pts, n);
    path.Transform(&xf);
    Gdiplus::SolidBrush br(AiTocGdip(col, alpha));
    g.FillPath(&br, &path);
}

// Opaque overlay fill shared by the page label and the selected check badge:
// theme accent tone in light chrome, clearly visible dark gray in dark chrome.
static COLORREF AiTocOverlayFillColor(bool dark) {
    return dark ? RGB(56, 59, 66) : AccentColor(ThemeMainWindowBackgroundColor(), 10);
}

// Plain "第 N 页" tag at the bottom-center of the card: solid theme control
// background, hairline frame, regular small text — a native label, not a
// rounded web-style pill.
static void AiTocDrawPageLabel(HDC hdc, const RECT& rcCard, AiTocDialog* dlg, int pageNo) {
    HFONT font = CreateFontW(-DpiScale(dlg->thumbnailPane, 11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(hdc, font);
    TempStr label = str::FormatTemp(_TRA("Page %d"), pageNo);
    RECT textRc = {0, 0, 0, 0};
    DrawTextW(hdc, ToWStrTemp(label), -1, &textRc, DT_NOPREFIX | DT_CALCRECT | DT_SINGLELINE);
    int padX = DpiScale(dlg->thumbnailPane, 8);
    int h = DpiScale(dlg->thumbnailPane, 18);
    int w = textRc.right - textRc.left + padX * 2;
    int inset = DpiScale(dlg->thumbnailPane, 8);
    RECT rc{rcCard.left + ((rcCard.right - rcCard.left) - w) / 2, rcCard.bottom - inset - h,
            rcCard.left + ((rcCard.right - rcCard.left) - w) / 2 + w, rcCard.bottom - inset};
    FillRect(hdc, &rc, dlg->controlBrush);
    bool dark = ThemeUsesDarkChrome();
    HBRUSH frame = CreateSolidBrush(dark ? RGB(78, 80, 88) : RGB(206, 208, 214));
    FrameRect(hdc, &rc, frame);
    DeleteObject(frame);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeWindowTextColor());
    DrawTextW(hdc, ToWStrTemp(label), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// Selection check badge in the top-right corner of a thumbnail card: an
// opaque circle with a hairline ring and a filled vector glyph from a
// standard SVG icon. Selected draws the solid accent circle with the active
// glyph color; a hovered unselected card shows the muted glyph on the plain
// backing. No shadows or translucency — flat native rendering.
static void AiTocDrawCheckBadge(AiTocDialog* dlg, HDC hdc, const RECT& rcCard, bool selected, bool hovered) {
    if (!selected && !hovered) {
        return;
    }
    bool dark = ThemeUsesDarkChrome();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    Gdiplus::REAL d = (Gdiplus::REAL)DpiScale(dlg->thumbnailPane, 22);
    int inset = DpiScale(dlg->thumbnailPane, 8);
    Gdiplus::REAL x = (Gdiplus::REAL)(rcCard.right - inset) - d;
    Gdiplus::REAL y = (Gdiplus::REAL)(rcCard.top + inset);

    Gdiplus::GraphicsPath back;
    back.AddEllipse(x, y, d, d);
    COLORREF fillCol = selected ? AiTocOverlayFillColor(dark) : (dark ? RGB(50, 52, 58) : RGB(255, 255, 255));
    Gdiplus::SolidBrush br(AiTocGdip(fillCol, 255));
    g.FillPath(&br, &back);
    Gdiplus::Pen ring(AiTocGdip(dark ? RGB(255, 255, 255) : RGB(20, 20, 24), dark ? 36 : 24), 1.0f);
    g.DrawPath(&ring, &back);

    Gdiplus::PointF origin{x, y};
    float pad = d * 0.24f;
    if (selected) {
        AiTocFillIconPath(g, kAiTocCheckOuter, dimof(kAiTocCheckOuter), 24.f, origin, d, pad,
                          AiTocBadgeActiveGlyphColor(dark), 255);
    } else {
        AiTocFillIconPath(g, kAiTocCheckOuter, dimof(kAiTocCheckOuter), 24.f, origin, d, pad, AiTocBadgeIconColor(dark),
                          178);
    }
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
        // Content-zone chrome: control-toned fill (subtle step from the
        // window background) plus a hairline frame delineate the thumbnail
        // area from the form controls above.
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect((HDC)wp, &rect, dlg->controlBrush);
        bool dark = ThemeUsesDarkChrome();
        COLORREF frame = dark ? RGB(58, 60, 66) : RGB(208, 210, 216);
        HBRUSH brush = CreateSolidBrush(frame);
        FrameRect((HDC)wp, &rect, brush);
        DeleteObject(brush);
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN) {
        return AiTocColorControl(dlg, (HDC)wp, (HWND)lp);
    }
    if (msg == WM_COMMAND) {
        int id = (int)LOWORD(wp);
        // A click on the thumbnail card itself toggles its selection.
        if (HIWORD(wp) == STN_CLICKED && id >= kAiTocImgBaseId && dlg->detectWork) {
            int i = id - kAiTocImgBaseId;
            if (i >= 0 && i < dlg->detectWork->initiallySelected.Size() && i < dlg->detectWork->pageNos.Size()) {
                dlg->detectWork->initiallySelected[i] = !dlg->detectWork->initiallySelected[i];
                AiTocSetPagesTextFromChecks(dlg);
                SetWindowTextW(dlg->status, _TRW("Page numbers and the thumbnails below are in sync."));
                AiTocUpdateSendEnabled(dlg);
                InvalidateRect((HWND)lp, nullptr, FALSE);
            }
            return 0;
        }
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    if (msg == WM_SETCURSOR) {
        // Hand cursor over the clickable thumbnail cards.
        DWORD pos = GetMessagePos();
        POINT pt{(short)LOWORD(pos), (short)HIWORD(pos)};
        ScreenToClient(hwnd, &pt);
        HWND child = ChildWindowFromPoint(hwnd, pt);
        if (child && child != hwnd) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
    }
    if (msg == WM_MOUSEMOVE) {
        RECT client{};
        GetClientRect(hwnd, &client);
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        int idx = AiTocHitTestThumb(dlg, client, pt);
        if (idx != dlg->hoverIdx) {
            int old = dlg->hoverIdx;
            dlg->hoverIdx = idx;
            if (old >= 0 && old < dlg->thumbnailImages.Size()) {
                InvalidateRect(dlg->thumbnailImages[old], nullptr, FALSE);
            }
            if (idx >= 0 && idx < dlg->thumbnailImages.Size()) {
                InvalidateRect(dlg->thumbnailImages[idx], nullptr, FALSE);
            }
        }
        if (!dlg->hoverTracked) {
            TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tm);
            dlg->hoverTracked = true;
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        dlg->hoverTracked = false;
        int old = dlg->hoverIdx;
        dlg->hoverIdx = -1;
        if (old >= 0 && old < dlg->thumbnailImages.Size()) {
            InvalidateRect(dlg->thumbnailImages[old], nullptr, FALSE);
        }
        return 0;
    }
    if (msg == WM_DRAWITEM) {
        auto* draw = (DRAWITEMSTRUCT*)lp;
        auto* bitmap = (RenderedBitmap*)GetWindowLongPtrW(draw->hwndItem, GWLP_USERDATA);
        int i = (int)GetWindowLongPtrW(draw->hwndItem, GWL_ID) - kAiTocImgBaseId;
        bool selected = dlg->detectWork && i >= 0 && i < dlg->detectWork->initiallySelected.Size() &&
                        dlg->detectWork->initiallySelected[i];
        // Image covers the whole card — no letterbox gaps on any side. The
        // aspect ratio is preserved by over-scaling and center-cropping the
        // overflowing axis; the small edge crop never hides the page's
        // overall layout (what the TOC judgement relies on).
        RECT imgRc = draw->rcItem;
        FillRect(draw->hDC, &imgRc, dlg->backgroundBrush);
        if (bitmap) {
            HBITMAP image = bitmap->GetBitmap();
            BITMAP size{};
            GetObjectW(image, sizeof(size), &size);
            int width = imgRc.right - imgRc.left;
            int height = imgRc.bottom - imgRc.top;
            int dw = MulDiv(size.bmWidth, height, size.bmHeight);
            int dh = height;
            if (dw < width) {
                dw = width;
                dh = MulDiv(size.bmHeight, width, size.bmWidth);
            }
            HDC source = CreateCompatibleDC(draw->hDC);
            HGDIOBJ old = SelectObject(source, image);
            SetStretchBltMode(draw->hDC, HALFTONE);
            SetBrushOrgEx(draw->hDC, 0, 0, nullptr);
            StretchBlt(draw->hDC, imgRc.left + (width - dw) / 2, imgRc.top + (height - dh) / 2, dw, dh, source, 0, 0,
                       size.bmWidth, size.bmHeight, SRCCOPY);
            SelectObject(source, old);
            DeleteDC(source);
        }
        // Plain page label at the bottom-center of the card.
        if (dlg->detectWork && i >= 0 && i < dlg->detectWork->pageNos.Size()) {
            AiTocDrawPageLabel(draw->hDC, draw->rcItem, dlg, dlg->detectWork->pageNos[i]);
        }
        // Selection chrome: 2 px accent frame on selected cards; resting
        // hairline otherwise, slightly brighter while hovered.
        bool hovered = dlg->hoverIdx == i;
        if (selected) {
            bool dark = ThemeUsesDarkChrome();
            COLORREF accent = AiTocOverlayFillColor(dark);
            HPEN pen = CreatePen(PS_INSIDEFRAME, 2, accent);
            HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
            HGDIOBJ oldBrush = SelectObject(draw->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(draw->hDC, draw->rcItem.left, draw->rcItem.top, draw->rcItem.right, draw->rcItem.bottom);
            SelectObject(draw->hDC, oldPen);
            SelectObject(draw->hDC, oldBrush);
            DeleteObject(pen);
        } else {
            // Subtle resting border, slightly brighter while hovered.
            bool dark = ThemeUsesDarkChrome();
            COLORREF frame = dark ? (hovered ? RGB(122, 126, 136) : RGB(78, 80, 88))
                                  : (hovered ? RGB(122, 126, 136) : RGB(208, 210, 216));
            HBRUSH brush = CreateSolidBrush(frame);
            FrameRect(draw->hDC, &draw->rcItem, brush);
            DeleteObject(brush);
        }
        AiTocDrawCheckBadge(dlg, draw->hDC, draw->rcItem, selected, hovered);
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

// Hairline separator color, shared by the footer line and the fallback
// manual-pages divider.
static HBRUSH AiTocHairlineBrush() {
    return CreateSolidBrush(ThemeUsesDarkChrome() ? RGB(68, 70, 76) : RGB(206, 208, 214));
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
        // Very light hairline separator above the button row.
        if (dlg->sepY > 0) {
            RECT line{AiTocS(dlg, 22), dlg->sepY, rect.right - AiTocS(dlg, 22), dlg->sepY + 1};
            HBRUSH brush = AiTocHairlineBrush();
            FillRect((HDC)wp, &line, brush);
            DeleteObject(brush);
        }
        // Scanning marquee: three small dots, one highlighted per phase.
        // Painted here so the 300 ms timer only invalidates the tiny dots
        // rect — never the whole window (no flicker, no thumbnail repaints).
        if (dlg->scanning && !dlg->review && (dlg->dotsRect.right > dlg->dotsRect.left)) {
            int dotD = dlg->dotsRect.bottom - dlg->dotsRect.top - 1;
            int dotGap = AiTocS(dlg, 6);
            HBRUSH active = CreateSolidBrush(GetSelectionHighlightColor());
            HBRUSH inactive = CreateSolidBrush(AiTocMutedTextColor());
            HPEN pen = (HPEN)GetStockObject(NULL_PEN);
            for (int k = 0; k < 3; k++) {
                RECT dot{dlg->dotsRect.left + k * (dotD + dotGap), dlg->dotsRect.top,
                         dlg->dotsRect.left + k * (dotD + dotGap) + dotD + 1, dlg->dotsRect.top + dotD + 1};
                SelectObject((HDC)wp, k == dlg->dotPhase ? active : inactive);
                SelectObject((HDC)wp, pen);
                SetBkMode((HDC)wp, TRANSPARENT);
                Ellipse((HDC)wp, dot.left, dot.top, dot.right, dot.bottom);
            }
            DeleteObject(active);
            DeleteObject(inactive);
        }
        return 1;
    }
    if (msg == WM_TIMER && wp == 2 && dlg->scanning && !dlg->review) {
        dlg->dotPhase = (dlg->dotPhase + 1) % 3;
        InvalidateRect(hwnd, &dlg->dotsRect, TRUE);
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN) {
        return AiTocColorControl(dlg, (HDC)wp, (HWND)lp);
    }
    if (msg == WM_DPICHANGED) {
        // Per-monitor DPI change (cross-monitor drag): rescale from the same
        // logical DIP values with the new DPI — never from the old pixels.
        auto prc = (RECT*)lp;
        dlg->dpi = LOWORD(wp);
        AiTocRecreateFonts(dlg);
        AiTocApplyFonts(dlg);
        // Windows' suggested rect keeps the user's drag position; the full
        // relayout below then recomputes the exact size from DIP + work area.
        SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        AiTocLayoutControls(dlg);
        return 0;
    }
    if (msg == WM_COMMAND && (HWND)lp == dlg->pagesEdit && HIWORD(wp) == EN_CHANGE && !dlg->settingPages) {
        if (dlg->detectWork && dlg->detectWork->pageNos.Size() > 0) {
            AiTocSetChecksFromPageText(dlg);
            SetWindowTextW(dlg->status, _TRW("Page numbers and the thumbnails below are in sync."));
            AiTocUpdateSendEnabled(dlg);
            return 0;
        }
        dlg->manualPages = true;
        dlg->scanning = false;
        KillTimer(hwnd, 2);
        RemovePropW(hwnd, kAiTocDetectToken);
        if (!dlg->busy && !dlg->submitted) {
            Str pageText = GetWindowTextTemp(dlg->pagesEdit);
            EnableWindow(dlg->send, pageText.s && *pageText.s);
            SetWindowTextW(dlg->status, _TRW("The entered PDF pages will be rendered and queued for sending."));
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
        dlg->scanning = false;
        KillTimer(hwnd, 2);
        delete dlg->detectWork;
        dlg->detectWork = work;
        if (work->pageNos.Size() == 0) {
            // Expose every page to the manual confirm page: typed ranges check
            // the matching thumbnails and 发送目录页 collects the checked
            // pages — the same confirm flow as a detected review.
            int pageCount = work->engine->PageCount();
            for (int pageNo = 1; pageNo <= pageCount; pageNo++) {
                work->pageNos.Append(pageNo);
                work->initiallySelected.Append(false);
                work->thumbnails.Append(nullptr);
            }
            SetWindowTextW(dlg->pagesEdit, L"");
            // No printed TOC: show the two separated fallback branches (body
            // structure vs manual page entry). pagesLabel keeps its standard
            // manual-section text; the section headers come from stateTitle.
            dlg->bodyFallback = true;
            AiTocSetState(dlg, AiTocUiState::ChooseFallback);
            SetWindowTextW(dlg->status, L"");
            SetWindowTextW(dlg->scanCount, L"");
            EnableWindow(dlg->send, FALSE);
            if (dlg->bodyBtn) {
                EnableWindow(dlg->bodyBtn, TRUE);
            }
            AiTocLayoutControls(dlg);
        } else {
            AutoFree detected(AiTocFormatSelectedPages(work->pageNos, work->initiallySelected));
            SetWindowTextW(dlg->pagesEdit, ToWStrTemp(detected));
            AiTocEnterReview(dlg);
        }
        AiTocUpdateSendEnabled(dlg);
        dlg->settingPages = false;
        return 0;
    }
    if (msg == kMsgAiTocDetectProgress && !dlg->detectWork && !dlg->manualPages && !dlg->busy && !dlg->submitted) {
        SetWindowTextW(dlg->status, _TRW("Scanning for TOC pages"));
        SetWindowTextW(dlg->scanCount, ToWStrTemp(str::FormatTemp("%d / %d", (int)wp, (int)lp)));
        return 0;
    }
    if (msg == kMsgAiTocBodyProgress && dlg->bodyScanning) {
        SetWindowTextW(dlg->status, _TRW("Scanning document structure"));
        SetWindowTextW(dlg->scanCount, ToWStrTemp(str::FormatTemp("%d / %d", (int)wp, (int)lp)));
        return 0;
    }
    if (msg == kMsgAiTocBodyScanned) {
        auto* work = (AiTocBodyScanWork*)wp;
        if (GetPropW(hwnd, kAiTocBodyScanToken) != dlg->token) {
            delete work;
            return 0;
        }
        dlg->bodyScanWork = nullptr;
        RemovePropW(hwnd, kAiTocBodyScanToken);
        dlg->bodyScanning = false;
        dlg->scanning = false;
        KillTimer(hwnd, 2);
        bool canceled = work->canceled;
        TocStructureScanResult* scan = work->scan;
        int candidateCount = scan ? scan->candidates.Size() : 0;
        if (canceled || candidateCount == 0) {
            delete work;
            AiTocSetState(dlg, AiTocUiState::ChooseFallback);
            EnableWindow(dlg->bodyBtn, TRUE);
            EnableWindow(dlg->send, TRUE);
            EnableWindow(dlg->pagesEdit, TRUE);
            SetWindowTextW(dlg->status,
                           canceled ? _TRW("Scan canceled.")
                                    : _TRW("No obvious body headings detected. Enter the TOC PDF page numbers below."));
            AiTocLayoutControls(dlg);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        // Hand the scan to the dialog and build the text-only AI round.
        delete dlg->bodyScan;
        dlg->bodyScan = scan;
        work->scan = nullptr;
        EngineBase* engine = work->engine;
        engine->AddRef(); // ownership passes to sendWork; acquire before deleting work
        delete work;

        AutoFreeStr digest(BuildBodyTocDigest(*dlg->bodyScan));
        auto* sendWork = new AiTocBodySendWork();
        sendWork->engine = engine;
        sendWork->mainHwnd = dlg->win ? dlg->win->hwndFrame : nullptr;
        sendWork->owner = hwnd;
        sendWork->dialogToken = dlg->token;
        sendWork->service = AiTocActiveService();
        sendWork->prompt = BuildBodyTocPrompt(dlg->bodyScan->totalPages, digest);
        // [重新发送] must be able to resubmit the very same prompt without
        // re-scanning: the dialog keeps its own copy (the worker's prompt is
        // freed with the work item).
        dlg->bodyPrompt = str::Dup(sendWork->prompt);
        dlg->bodySendWork = sendWork;
        dlg->busy = true;
        logf("[AITOC] Body digest generated: candidates=%d\n", candidateCount);
        AiTocSetState(dlg, AiTocUiState::SendingToAi);
        SetWindowTextW(dlg->status,
                       ToWStrTemp(str::FormatTemp(_TRA("Scan finished: %d likely headings found. Opening the AI page…"),
                                                  candidateCount)));
        RunAsync(MkFunc0<AiTocBodySendWork>(AiTocBodySendWorker, sendWork), "AiTocBodySend");
        return 0;
    }
    if (msg == kMsgAiTocBodySent) {
        auto* work = (AiTocBodySendWork*)wp;
        if (GetPropW(hwnd, kAiTocToken) != dlg->token) {
            delete work;
            return 0;
        }
        dlg->bodySendWork = nullptr;
        const char* error = work->error;
        bool browserOpen = work->browser != nullptr;
        delete work;
        if (error && !browserOpen) {
            dlg->busy = false;
            AiTocSetState(dlg, AiTocUiState::ChooseFallback);
            EnableWindow(dlg->bodyBtn, TRUE);
            EnableWindow(dlg->send, TRUE);
            EnableWindow(dlg->pagesEdit, TRUE);
            AiTocApplyFallbackTexts(dlg); // stale waiting texts may be on screen
            SetWindowTextW(dlg->status, _TRW(error));
            AiTocLayoutControls(dlg);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        // Prompt sent (or already on the clipboard with the chat browser open
        // for a manual paste): listen for the v2 reply and shrink to compact.
        AiTocEnsureClipboardListener(hwnd);
        dlg->busy = false;
        dlg->submitted = true;
        dlg->bodyMode = true;
        dlg->waiting = true;
        dlg->waitManualPaste = error != nullptr;
        AiTocSetState(dlg, AiTocUiState::WaitingForAiClipboard);
        dlg->clipboardSequence = GetClipboardSequenceNumber();
        SetTimer(hwnd, 1, 750, nullptr);
        EnableWindow(dlg->send, FALSE);
        EnableWindow(dlg->pagesEdit, FALSE);
        EnableWindow(dlg->bodyBtn, FALSE);
        if (dlg->resend) {
            EnableWindow(dlg->resend, TRUE);
        }
        AiTocApplyWaitingTexts(dlg);
        AiTocApplyStateFonts(dlg);
        InvalidateRect(hwnd, nullptr, TRUE);
        AiTocLayoutControls(dlg);
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
    if (msg == WM_COMMAND && LOWORD(wp) == 1002 && HIWORD(wp) == BN_CLICKED && !dlg->busy && !dlg->submitted &&
        !dlg->bodyScanning) {
        StartAiBodyStructureScan(dlg);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == 1003 && HIWORD(wp) == BN_CLICKED) {
        AiTocResendToAi(dlg);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == 1004 && HIWORD(wp) == BN_CLICKED && !dlg->busy && !dlg->submitted) {
        // 手工指定目录页: jump to the manual confirm page — same layout as a
        // detected review (pages edit + full thumbnail grid), but nothing is
        // preselected and the edit starts empty.
        dlg->bodyFallback = false;
        dlg->mode = AiTocSourceMode::PrintedToc;
        AiTocEnterReview(dlg);
        SetWindowTextW(dlg->status, _TRW("Enter the PDF pages of the TOC; matching thumbnails will be checked. You can "
                                         "also click thumbnails directly."));
        AiTocUpdateSendEnabled(dlg);
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
        WCHAR confirmation[512]{};
        swprintf_s(confirmation,
                   _TRW("The selected %d TOC page images will be sent to the configured web AI.\n\nPlease verify the "
                        "pages and content before sending."),
                   pages.Size());
        if (!ConfirmTocPageSend(hwnd, confirmation)) return 0;
        // Cache what was confirmed: 重新发送 re-renders exactly these pages
        // instead of asking the user again.
        dlg->mode = AiTocSourceMode::PrintedToc;
        dlg->waitManualPaste = false;
        dlg->confirmedPages.Reset();
        for (int i = 0; i < pages.Size(); i++) {
            dlg->confirmedPages.Append(pages[i]);
        }
        dlg->busy = true;
        AiTocSetState(dlg, AiTocUiState::SendingToAi);
        RemovePropW(hwnd, kAiTocDetectToken);
        EnableWindow(dlg->send, FALSE);
        EnableWindow(dlg->pagesEdit, FALSE);
        // Immediate visual feedback for "sent": hide the thumbnail grid and
        // shrink the dialog (top/left anchored) so the document behind it is
        // visible again while the AI works.
        dlg->waiting = true;
        // Advance the phase hint: bold+highlight "发送目录页给AI处理" and
        // demote "确认目录页" (fonts + colors need an explicit repaint).
        AiTocApplyStateFonts(dlg);
        InvalidateRect(hwnd, nullptr, TRUE);
        AiTocLayoutControls(dlg);
        SetWindowTextW(dlg->status,
                       ToWStrTemp(str::FormatTemp(_TRA("Preparing %d TOC page images for sending…"), pages.Size())));
        StartAiTocRender(dlg->win, pages, hwnd);
        return 0;
    }
    if (msg == WM_TIMER && wp == 3) {
        // Ownerless dialog watchdog: if the main frame (or its document) is
        // gone, self-destruct instead of lingering with a dangling session.
        if (!dlg->ownerHwnd || !IsWindow(dlg->ownerHwnd)) {
            DestroyWindow(hwnd);
        }
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
    if (msg == WM_CLOSE) DestroyWindow(hwnd);
    if (msg == WM_NCDESTROY) {
        UnregisterAppDialogForTheme(hwnd);
        logf("[AITOC] workflow ended at %s\n", AiTocStateName(dlg->state));
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        KillTimer(hwnd, 3);
        if (dlg->importCtx) {
            // Cancel mid-import: detach the callbacks so the running
            // calibration never touches this freed dialog, and abort its
            // session before the commit step (an aborted verify never writes
            // the outline). StartTocCalibAsync hid the TOC tree for the
            // calibration; the engine outline is untouched, so just reload it.
            dlg->importCtx->dlg = nullptr;
            dlg->importCtx = nullptr;
            MainWindow* win = dlg->win;
            WindowTab* tab = win ? win->CurrentTab() : nullptr;
            if (tab && tab->tocCalib) {
                DeleteTocCalibSession(tab->tocCalib);
                tab->tocCalib = nullptr;
            }
            if (win) {
                if (win->tocLoaded) {
                    ClearTocBox(win);
                }
                LoadTocTree(win);
            }
        }
        // Signal a running body scan to stop between pages and break the OCR
        // queue; the worker checks the token on exit and deletes itself. The
        // send worker likewise self-destructs once kAiTocToken is gone.
        if (dlg->bodyScanWork) {
            OcrCancelForEngine(dlg->bodyScanWork->engine);
        }
        RemovePropW(hwnd, kAiTocBodyScanToken);
        RemovePropW(hwnd, kAiTocToken);
        RemovePropW(hwnd, kAiTocDetectToken);
        RemoveClipboardFormatListener(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete dlg->work;
        delete dlg->detectWork;
        // bodyScanWork / bodySendWork outlive the dialog and self-delete.
        delete dlg->bodyScan;
        str::Free(dlg->bodyPrompt);
        DeleteObject(dlg->backgroundBrush);
        DeleteObject(dlg->controlBrush);
        DeleteObject(dlg->font);
        DeleteObject(dlg->fontBold);
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
    SetWindowTextW(dlg->send, _TRW("Send TOC Pages"));
    if (work->error) {
        if (work->files.Size() == work->pageNos.Size() && CopyAiChatPayloadToClipboard(work->files, kAiTocPrompt) &&
            AiTocEnsureClipboardListener(dlg->hwnd)) {
            // Auto-send failed but the payload is on the clipboard and the
            // chat browser is open: unified waiting page in manual-paste mode.
            dlg->submitted = true;
            dlg->waiting = true;
            dlg->waitManualPaste = true;
            AiTocSetState(dlg, AiTocUiState::WaitingForAiClipboard);
            dlg->clipboardSequence = GetClipboardSequenceNumber();
            SetTimer(dlg->hwnd, 1, 750, nullptr);
            EnableWindow(dlg->send, FALSE);
            if (dlg->resend) {
                EnableWindow(dlg->resend, TRUE);
            }
            AiTocApplyWaitingTexts(dlg);
            AiTocApplyStateFonts(dlg);
            InvalidateRect(dlg->hwnd, nullptr, TRUE);
            AiTocLayoutControls(dlg);
        } else {
            SetWindowTextW(dlg->status, work->error ? _TRW(work->error) : nullptr);
        }
    } else if (work->imageBatches > 1) {
        dlg->collectedBatchJson.Reset();
        dlg->batchesGot = 0;
        AiTocArmPrintedBatchWait(dlg);
    } else {
        SetWindowTextW(dlg->status, _TRW("Sending prompt…"));
        SendAiTocPrompt(dlg);
        AiTocLayoutControls(dlg);
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
    dlg->ownerHwnd = win->hwndFrame;
    dlg->thumbnailThemeEpoch = ThemeEpoch();
    HINSTANCE h = GetModuleHandleW(nullptr);
    // Deliberately ownerless (parent = nullptr): clicking the dialog must not
    // pull the main window above other applications. Lifecycle is guarded by
    // the ownerHwnd watchdog timer instead of the owner-destroy mechanism.
    HWND hwnd =
        CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kAiTocDialogClass, _TRW("AI Recognize Table of Contents"),
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 520,
                        190, nullptr, nullptr, h, dlg);
    if (!hwnd) {
        delete dlg;
        return;
    }
    dlg->token = (HANDLE)++gAiTocToken;
    SetPropW(hwnd, kAiTocToken, dlg->token);
    SetPropW(hwnd, kAiTocDetectToken, dlg->token);
    logf("[AITOC] workflow started mode=Printed state=%s\n", AiTocStateName(dlg->state));
    // Per-monitor DPI + owned fonts from the start, so the layout is always
    // logical DIP scaled by the creating monitor's DPI.
    dlg->dpi = DpiGetForHwnd(hwnd);
    AiTocRecreateFonts(dlg);

    // Stage hint row: "准备内容 → AI 处理 → 导入目录" — shared by both source
    // modes, only the highlight moves with the state. SS_LEFTNOWORDWRAP: a
    // bold label wider than its box must clip, never wrap into a clipped
    // second line (looks like garbage).
    dlg->phasePrep = CreateWindowExW(0, L"STATIC", _TRW("Prepare Content"), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                     0, 0, 10, 10, hwnd, nullptr, h, nullptr);
    dlg->phaseArrow = CreateWindowExW(0, L"STATIC", L"→", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10, 10, hwnd,
                                      nullptr, h, nullptr);
    dlg->phaseAi = CreateWindowExW(0, L"STATIC", _TRW("AI Processing"), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0,
                                   10, 10, hwnd, nullptr, h, nullptr);
    dlg->phaseArrow2 = CreateWindowExW(0, L"STATIC", L"→", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10, 10,
                                       hwnd, nullptr, h, nullptr);
    dlg->phaseImport = CreateWindowExW(0, L"STATIC", _TRW("Import TOC"), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0,
                                       0, 10, 10, hwnd, nullptr, h, nullptr);
    HWND label = CreateWindowExW(0, L"STATIC", _TRW("TOC pages (PDF page numbers, not printed page numbers)"),
                                 WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd, nullptr, h, nullptr);
    dlg->pagesLabel = label;
    SendMessageW(label, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    dlg->pagesEdit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10,
                                     10, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->pagesEdit, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    SendMessageW(dlg->pagesEdit, EM_SETCUEBANNER, TRUE, (LPARAM)_TRW("e.g. 3-4, 7"));
    gAiTocEditOrigProc = (WNDPROC)SetWindowLongPtrW(dlg->pagesEdit, GWLP_WNDPROC, (LONG_PTR)AiTocEditProc);
    dlg->status = CreateWindowExW(0, L"STATIC", _TRW("Analyzing the first pages for a printed TOC…"),
                                  WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->status, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    // SS_LEFT so growing digit widths extend rightward and do not shift the
    // fixed status prefix (import calib) or the dot marquee (scan).
    dlg->scanCount =
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd, nullptr, h, nullptr);
    SendMessageW(dlg->scanCount, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    HWND cancel = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10,
                                  hwnd, (HMENU)IDCANCEL, h, nullptr);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    dlg->send = CreateWindowExW(0, L"BUTTON", _TRW("Send TOC Pages"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0,
                                10, 10, hwnd, (HMENU)1001, h, nullptr);
    SendMessageW(dlg->send, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    EnableWindow(dlg->send, FALSE);
    // Fallback for documents without a printed TOC. Hidden until detection
    // confirms there is no contents spread.
    dlg->bodyBtn = CreateWindowExW(0, L"BUTTON", _TRW("Generate TOC From Body"), WS_CHILD | BS_PUSHBUTTON, 0, 0, 10, 10,
                                   hwnd, (HMENU)1002, h, nullptr);
    SendMessageW(dlg->bodyBtn, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    ShowWindow(dlg->bodyBtn, SW_HIDE);
    // [重新发送] on the unified waiting page: resubmits the cached payload
    // (confirmed printed pages or the body Structure Digest prompt) without
    // re-running detection or the whole-document scan.
    dlg->resend = CreateWindowExW(0, L"BUTTON", _TRW("Resend"), WS_CHILD | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd,
                                  (HMENU)1003, h, nullptr);
    SendMessageW(dlg->resend, WM_SETFONT, (WPARAM)dlg->font, TRUE);
    ShowWindow(dlg->resend, SW_HIDE);
    // Fallback page sections (hidden until detection finds no printed TOC).
    // SS_LEFTNOWORDWRAP on single-line labels: bold text must clip, not wrap.
    dlg->stateTitle = CreateWindowExW(0, L"STATIC", _TRW("No printed TOC found"), WS_CHILD | SS_LEFTNOWORDWRAP, 0, 0,
                                      10, 10, hwnd, nullptr, h, nullptr);
    dlg->stateDesc =
        CreateWindowExW(0, L"STATIC",
                        _TRW("This document may not have a separate printed TOC.\r\n"
                             "You can generate one from the body structure or specify the TOC pages manually."),
                        WS_CHILD | SS_LEFT, 0, 0, 10, 10, hwnd, nullptr, h, nullptr);
    dlg->bodyTitle = CreateWindowExW(0, L"STATIC", _TRW("Generate TOC From Body"), WS_CHILD | SS_LEFTNOWORDWRAP, 0, 0,
                                     10, 10, hwnd, nullptr, h, nullptr);
    dlg->bodyDesc = CreateWindowExW(0, L"STATIC",
                                    _TRW("Scans the full text and extracts heading candidates locally; the AI then "
                                         "organizes them into a TOC."),
                                    WS_CHILD | SS_LEFT, 0, 0, 10, 10, hwnd, nullptr, h, nullptr);
    // [手工指定目录页]: opens the manual confirm page (pages edit + thumbnail
    // grid) instead of the former inline manual section on the fallback page.
    dlg->manualBtn = CreateWindowExW(0, L"BUTTON", _TRW("Specify TOC Pages"), WS_CHILD | BS_PUSHBUTTON, 0, 0, 10, 10,
                                     hwnd, (HMENU)1004, h, nullptr);

    // Scanning starts immediately: the marquee widgets participate in the
    // layout, so the flag must be set before the first AiTocLayoutControls.
    dlg->scanning = true;
    AiTocApplyFonts(dlg);
    AiTocLayoutControls(dlg);
    if (UseDarkModeLib()) {
        DarkMode::setDarkWndNotifySafe(hwnd);
    }
    AppDialogApplyChrome(hwnd);
    RegisterAppDialogForTheme(hwnd, AiTocThemeRefreshCb, dlg);
    AiTocCenterOverMainWindow(dlg);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    // Scanning marquee: 300 ms per phase, invalidating only the dots rect.
    SetTimer(hwnd, 2, 300, nullptr);
    // Ownerless-dialog watchdog: die together with the main frame.
    SetTimer(hwnd, 3, 1000, nullptr);
    auto* work = new AiTocDetectWork();
    work->win = win;
    work->engine = dm->GetEngine();
    work->engine->AddRef();
    work->notifyHwnd = hwnd;
    work->dialogToken = dlg->token;
    RunAsync(MkFunc0<AiTocDetectWork>(AiTocDetectWorker, work), "AiTocDetect");
}
