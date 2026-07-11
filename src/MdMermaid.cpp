/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "webview2.h"
#include "wingui/WebView.h"

#include "MdMermaid.h"
#include "utils/Log.h"

namespace {

constexpr const char* kMermaidClass = "language-mermaid";
constexpr DWORD kMermaidRenderTimeoutMs = 60000;

struct MermaidEnv {
    WebviewWnd* webviewWnd = nullptr;
    bool ready = false;
    bool initOk = false;
    char* dataDir = nullptr;
};

MermaidEnv* gMermaidEnv = nullptr;

class ExecuteScriptHandler : public ICoreWebView2ExecuteScriptCompletedHandler {
  public:
    ExecuteScriptHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

    ~ExecuteScriptHandler() {
        if (done) {
            CloseHandle(done);
        }
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refCount); }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = (ULONG)InterlockedDecrement(&refCount);
        if (c == 0) {
            delete this;
        }
        return c;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(ICoreWebView2ExecuteScriptCompletedHandler) || riid == IID_IUnknown) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR resultObjectAsJson) override {
        hr = errorCode;
        if (SUCCEEDED(errorCode) && resultObjectAsJson) {
            result.Set(str::Dup(resultObjectAsJson));
        }
        if (done) {
            SetEvent(done);
        }
        return S_OK;
    }

    volatile LONG refCount = 1;
    HRESULT hr = E_PENDING;
    AutoFreeWStr result;
    HANDLE done = nullptr;
};

class NavigationCompletedHandler : public ICoreWebView2NavigationCompletedEventHandler {
  public:
    NavigationCompletedHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

    ~NavigationCompletedHandler() {
        if (done) {
            CloseHandle(done);
        }
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refCount); }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = (ULONG)InterlockedDecrement(&refCount);
        if (c == 0) {
            delete this;
        }
        return c;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(ICoreWebView2NavigationCompletedEventHandler) || riid == IID_IUnknown) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) {
        BOOL success = FALSE;
        if (args) {
            args->get_IsSuccess(&success);
        }
        ok = success != FALSE;
        if (done) {
            SetEvent(done);
        }
        return S_OK;
    }

    volatile LONG refCount = 1;
    bool ok = false;
    HANDLE done = nullptr;
};

static void PumpMessagesUntil(HANDLE ev, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    for (;;) {
        if (ev && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0) {
            return;
        }
        if (GetTickCount() - start >= timeoutMs) {
            return;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

static TempStr PathToFileUrlTemp(const char* path) {
    TempStr norm = str::ReplaceTemp(path, "\\", "/");
    if (!norm) {
        return nullptr;
    }
    return str::JoinTemp("file:///", norm);
}

static TempStr EscapeJsStringTemp(const char* s) {
    if (!s) {
        return (TempStr) "";
    }
    StrBuilder b;
    for (const char* p = s; *p; p++) {
        char c = *p;
        switch (c) {
            case '\\':
                b.Append("\\\\");
                break;
            case '"':
                b.Append("\\\"");
                break;
            case '\r':
                break;
            case '\n':
                b.Append("\\n");
                break;
            case '\t':
                b.Append("\\t");
                break;
            default:
                b.AppendChar(c);
                break;
        }
    }
    return b.StealData();
}

static TempStr JsonStringToUtf8Temp(const WCHAR* jsonW) {
    if (!jsonW) {
        return (TempStr) "";
    }
    TempStr json = ToUtf8Temp(jsonW);
    if (str::IsEmpty(json)) {
        return (TempStr) "";
    }
    size_t n = str::Len(json);
    if (n < 2 || json[0] != '"' || json[n - 1] != '"') {
        return json;
    }
    StrBuilder decoded;
    for (size_t i = 1; i + 1 < n; i++) {
        char c = json[i];
        if (c == '\\' && i + 1 + 1 < n) {
            char e = json[i + 1];
            if (e == '"') {
                decoded.AppendChar('"');
                i++;
                continue;
            }
            if (e == '\\') {
                decoded.AppendChar('\\');
                i++;
                continue;
            }
            if (e == 'n') {
                decoded.AppendChar('\n');
                i++;
                continue;
            }
            if (e == 'r') {
                decoded.AppendChar('\r');
                i++;
                continue;
            }
            if (e == 't') {
                decoded.AppendChar('\t');
                i++;
                continue;
            }
        }
        decoded.AppendChar(c);
    }
    return decoded.StealData();
}

static bool ExecuteScriptSync(ICoreWebView2* webview, const char* js, TempStr* resultOut) {
    if (!webview || !js || !resultOut) {
        return false;
    }
    *resultOut = nullptr;
    auto handler = new ExecuteScriptHandler();
    handler->AddRef();
    TempWStr jsW = ToWStrTemp(js);
    HRESULT callHr = webview->ExecuteScript(jsW, handler);
    if (FAILED(callHr)) {
        handler->Release();
        return false;
    }
    PumpMessagesUntil(handler->done, kMermaidRenderTimeoutMs);
    bool ok = SUCCEEDED(handler->hr);
    if (ok) {
        *resultOut = JsonStringToUtf8Temp(handler->result);
        ok = !str::IsEmpty(*resultOut);
    }
    handler->Release();
    return ok;
}

static bool WaitForNavigation(ICoreWebView2* webview, NavigationCompletedHandler* handler) {
    if (!webview || !handler) {
        return false;
    }
    ::EventRegistrationToken token;
    webview->add_NavigationCompleted(handler, &token);
    PumpMessagesUntil(handler->done, kMermaidRenderTimeoutMs);
    webview->remove_NavigationCompleted(token);
    return handler->ok;
}

static bool EnsureMermaidEnvReady() {
    if (gMermaidEnv && gMermaidEnv->ready) {
        return gMermaidEnv->initOk;
    }
    if (!HasWebView()) {
        return false;
    }

    TempStr mermaidPath = GetPathInExeDirTemp("mermaid/mermaid.min.js");
    if (!file::Exists(mermaidPath)) {
        logf("MdMermaid: missing '%s'\n", mermaidPath);
        return false;
    }

    if (!gMermaidEnv) {
        gMermaidEnv = new MermaidEnv();
    }
    if (gMermaidEnv->ready) {
        return gMermaidEnv->initOk;
    }

    TempStr dataDir = path::JoinTemp(GetTempDirTemp(), "SumatraPDF-mermaid-webview");
    if (!dataDir) {
        return false;
    }
    gMermaidEnv->dataDir = str::Dup(dataDir);

    gMermaidEnv->webviewWnd = new WebviewWnd();
    gMermaidEnv->webviewWnd->dataDir = gMermaidEnv->dataDir;

    CreateWebViewArgs wargs;
    wargs.parent = nullptr;
    wargs.pos = Rect{0, 0, 1024, 768};
    HWND hwnd = gMermaidEnv->webviewWnd->Create(wargs);
    if (hwnd) {
        ShowWindow(hwnd, SW_HIDE);
    }
    if (!hwnd || !gMermaidEnv->webviewWnd->webview) {
        logf("MdMermaid: failed to create WebView2\n");
        gMermaidEnv->initOk = false;
        gMermaidEnv->ready = true;
        return false;
    }

    TempStr scriptUrl = PathToFileUrlTemp(mermaidPath);
    TempStr html = str::FormatTemp(
        R"(<!DOCTYPE html><html><head><meta charset="utf-8"><script src="%s"></script></head><body></body></html>)",
        scriptUrl);
    if (!html) {
        gMermaidEnv->initOk = false;
        gMermaidEnv->ready = true;
        return false;
    }

    auto navHandler = new NavigationCompletedHandler();
    navHandler->AddRef();
    gMermaidEnv->webviewWnd->SetHtml(html);
    bool navOk = WaitForNavigation(gMermaidEnv->webviewWnd->webview, navHandler);
    navHandler->Release();
    if (!navOk) {
        logf("MdMermaid: failed to load mermaid.min.js\n");
        gMermaidEnv->initOk = false;
        gMermaidEnv->ready = true;
        return false;
    }

    TempStr initResult;
    bool initOk = ExecuteScriptSync(gMermaidEnv->webviewWnd->webview,
                                    "mermaid.initialize({startOnLoad:false,securityLevel:'loose',theme:'default'}); "
                                    "'ok';",
                                    &initResult);
    gMermaidEnv->initOk = initOk;
    gMermaidEnv->ready = true;
    if (!initOk) {
        logf("MdMermaid: mermaid.initialize failed\n");
    }
    return initOk;
}

static TempStr HtmlDecodeTemp(const char* s, size_t len) {
    StrBuilder bodyOut;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '&') {
            if (i + 4 <= len && str::StartsWith(s + i, "&lt;")) {
                bodyOut.AppendChar('<');
                i += 3;
                continue;
            }
            if (i + 4 <= len && str::StartsWith(s + i, "&gt;")) {
                bodyOut.AppendChar('>');
                i += 3;
                continue;
            }
            if (i + 5 <= len && str::StartsWith(s + i, "&amp;")) {
                bodyOut.AppendChar('&');
                i += 4;
                continue;
            }
            if (i + 6 <= len && str::StartsWith(s + i, "&quot;")) {
                bodyOut.AppendChar('"');
                i += 5;
                continue;
            }
            if (i + 6 <= len && str::StartsWith(s + i, "&#39;")) {
                bodyOut.AppendChar('\'');
                i += 5;
                continue;
            }
        }
        bodyOut.AppendChar(s[i]);
    }
    return bodyOut.StealData();
}

static TempStr TrimDiagramSourceTemp(TempStr s) {
    if (str::IsEmpty(s)) {
        return (TempStr) "";
    }
    size_t len = str::Len(s);
    size_t start = 0;
    while (start < len && str::IsWs(s[start])) {
        start++;
    }
    size_t end = len;
    while (end > start && str::IsWs(s[end - 1])) {
        end--;
    }
    if (start == 0 && end == len) {
        return s;
    }
    char* slice = (char*)memdup(s + start, end - start);
    if (!slice) {
        return (TempStr) "";
    }
    slice[end - start] = '\0';
    return slice;
}

static bool RenderMermaidSourceToSvg(const char* source, TempStr* svgOut) {
    *svgOut = nullptr;
    if (str::IsEmpty(source) || !EnsureMermaidEnvReady()) {
        return false;
    }

    TempStr escaped = EscapeJsStringTemp(source);
    TempStr js = str::FormatTemp(
        R"((async function(){
  try {
    const src = "%s";
    const id = "md" + Date.now();
    const out = await mermaid.render(id, src);
    return out.svg;
  } catch (e) {
    return "";
  }
})())",
        escaped);
    if (!js) {
        return false;
    }
    return ExecuteScriptSync(gMermaidEnv->webviewWnd->webview, js, svgOut);
}

static const char* FindMermaidBlockStart(const char* html, size_t len, size_t* startOut, size_t* contentStartOut) {
    const char* kOpen = "<pre><code class=\"";
    const char* kCloseClass = "\">";
    size_t openLen = str::Len(kOpen);
    for (size_t i = 0; i + openLen < len; i++) {
        if (!str::StartsWith(html + i, kOpen)) {
            continue;
        }
        size_t classStart = i + openLen;
        size_t classEnd = classStart;
        while (classEnd < len && html[classEnd] != '"') {
            classEnd++;
        }
        if (classEnd >= len) {
            continue;
        }
        size_t clsLen = classEnd - classStart;
        bool isMermaid = (clsLen == str::Len(kMermaidClass) && str::EqN(html + classStart, kMermaidClass, clsLen)) ||
                         (clsLen == 7 && str::EqN(html + classStart, "mermaid", 7));
        if (!isMermaid) {
            continue;
        }
        if (classEnd + 1 >= len || html[classEnd] != '"' || html[classEnd + 1] != '>') {
            continue;
        }
        *startOut = i;
        *contentStartOut = classEnd + 2;
        return html + *contentStartOut;
    }
    return nullptr;
}

static char* ReplaceMermaidBlocksOnUiThread(const char* bodyHtml, size_t bodyLen, size_t* outLen) {
    *outLen = 0;
    if (!bodyHtml || bodyLen == 0) {
        return nullptr;
    }

    StrBuilder htmlBuilder;
    size_t pos = 0;
    while (pos < bodyLen) {
        size_t blockStart = 0;
        size_t contentStart = 0;
        const char* content = FindMermaidBlockStart(bodyHtml + pos, bodyLen - pos, &blockStart, &contentStart);
        if (!content) {
            htmlBuilder.Append(bodyHtml + pos, bodyLen - pos);
            break;
        }

        blockStart += pos;
        contentStart += pos;
        const char* endTag = "</code></pre>";
        const char* contentEnd = str::Find(bodyHtml + contentStart, endTag);
        if (!contentEnd) {
            htmlBuilder.Append(bodyHtml + pos, bodyLen - pos);
            break;
        }

        htmlBuilder.Append(bodyHtml + pos, blockStart - pos);

        size_t rawLen = (size_t)(contentEnd - (bodyHtml + contentStart));
        TempStr diagram = HtmlDecodeTemp(bodyHtml + contentStart, rawLen);
        diagram = TrimDiagramSourceTemp(diagram);

        TempStr svg;
        bool rendered = RenderMermaidSourceToSvg(diagram, &svg);
        if (rendered && !str::IsEmpty(svg)) {
            htmlBuilder.Append(R"(<div class="mermaid-diagram">)");
            htmlBuilder.Append(svg);
            htmlBuilder.Append("</div>");
        } else {
            htmlBuilder.Append(bodyHtml + blockStart,
                               (size_t)(contentEnd + str::Len(endTag) - (bodyHtml + blockStart)));
        }

        pos = (size_t)(contentEnd + str::Len(endTag) - bodyHtml);
    }

    size_t sz = htmlBuilder.size();
    char* res = htmlBuilder.StealData();
    if (!res) {
        return nullptr;
    }
    *outLen = sz;
    return res;
}

struct ReplaceMermaidTask {
    const char* bodyHtml;
    size_t bodyLen;
    size_t outLen;
    char* result;
};

static void ReplaceMermaidTaskRun(ReplaceMermaidTask* task) {
    task->result = ReplaceMermaidBlocksOnUiThread(task->bodyHtml, task->bodyLen, &task->outLen);
}

} // namespace

bool MdBodyHasMermaidBlocks(const char* bodyHtml, size_t bodyLen) {
    if (!bodyHtml || bodyLen == 0) {
        return false;
    }
    size_t start = 0;
    size_t contentStart = 0;
    return FindMermaidBlockStart(bodyHtml, bodyLen, &start, &contentStart) != nullptr;
}

char* MdReplaceMermaidBlocks(const char* bodyHtml, size_t bodyLen, size_t* outLen) {
    if (!outLen) {
        return nullptr;
    }
    *outLen = 0;
    if (!bodyHtml || bodyLen == 0 || !MdBodyHasMermaidBlocks(bodyHtml, bodyLen)) {
        char* copy = (char*)memdup(bodyHtml, bodyLen + 1);
        if (copy) {
            copy[bodyLen] = '\0';
            *outLen = bodyLen;
        }
        return copy;
    }

    ReplaceMermaidTask task{bodyHtml, bodyLen, 0, nullptr};
    auto fn = MkFunc0<ReplaceMermaidTask>(ReplaceMermaidTaskRun, &task);
    uitask::Invoke(fn);
    *outLen = task.outLen;
    return task.result;
}

void MdMermaidShutdown() {
    delete gMermaidEnv;
    gMermaidEnv = nullptr;
}
