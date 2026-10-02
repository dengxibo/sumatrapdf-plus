/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include "utils/JsonParser.h"
#include "utils/CryptoUtil.h"
#include "utils/WinDynCalls.h"
#include "utils/Log.h"

#include <richedit.h>

// RichEdit 4.1 turns this on by default. Older SDK headers do not declare it.
#ifndef IMF_SPELLCHECKING
#define IMF_SPELLCHECKING 0x0800
#endif

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "GlobalPrefs.h"
#include "Translations.h"
#include "Theme.h"
#include "FloatingPopupStyle.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "SumatraConfig.h"
#include "Selection.h"
#include "SelectionToolbar.h"
#include "WindowTab.h"
#include "Notifications.h"
#include "AiTocApi.h"
#include "InlineTranslateLang.h"
#include "InlineTranslate.h"
#include "SumatraDialogs.h"
#include "WordLookup.h"
#include "DarkModeSubclass.h"

#include <bcrypt.h>
#include <wininet.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wininet.lib")

constexpr int kMaxTranslateChars = 4000;
constexpr int kTranslateCacheN = 16;
constexpr int kChatTimeoutMs = 90 * 1000;
constexpr int kVolcTimeoutMs = 30 * 1000;

constexpr int kPopupPad = 14;
constexpr int kPopupGap = 10;
constexpr int kCloseBtn = 16;
constexpr int kPopupMinW = 340;
constexpr int kPopupMaxW = 480;
constexpr int kPopupMaxH = 600;

#define kInlineTranslateClassName L"SumatraInlineTranslatePopup"

enum class PopupMode {
    Translate = 0,
    AskAi = 1
};

// ---------------------------------------------------------------------------
// secrets / config

bool InlineTranslateVolcIsConfigured() {
    if (!gGlobalPrefs) {
        return false;
    }
    return !str::IsEmptyOrWhiteSpace(gGlobalPrefs->translateVolcAccessKey) &&
           !str::IsEmptyOrWhiteSpace(gGlobalPrefs->translateVolcSecretKey);
}

bool InlineTranslateAiApiIsConfigured() {
    if (!gGlobalPrefs) {
        return false;
    }
    AiTocApiConfig cfg;
    cfg.baseUrl = gGlobalPrefs->aiTocApiBaseUrl;
    cfg.key = gGlobalPrefs->aiTocApiKey;
    cfg.model = gGlobalPrefs->aiTocApiModel;
    return AiTocApiIsConfigured(cfg);
}

void InlineTranslateUnprotectPrefsSecrets(GlobalPrefs* prefs) {
    if (!prefs) {
        return;
    }
    if (char* plain = UnprotectStringDpapi(prefs->translateVolcAccessKey)) {
        str::ReplacePtr(&prefs->translateVolcAccessKey, plain);
    }
    if (char* plain = UnprotectStringDpapi(prefs->translateVolcSecretKey)) {
        str::ReplacePtr(&prefs->translateVolcSecretKey, plain);
    }
}

void InlineTranslateProtectPrefsSecretsForSave(GlobalPrefs* prefs, char** plainAkOut, char** plainSkOut) {
    *plainAkOut = nullptr;
    *plainSkOut = nullptr;
    if (!prefs) {
        return;
    }
    *plainAkOut = prefs->translateVolcAccessKey;
    *plainSkOut = prefs->translateVolcSecretKey;
    if (char* protectedAk = ProtectStringDpapi(prefs->translateVolcAccessKey)) {
        prefs->translateVolcAccessKey = protectedAk;
    } else {
        *plainAkOut = nullptr;
    }
    if (char* protectedSk = ProtectStringDpapi(prefs->translateVolcSecretKey)) {
        prefs->translateVolcSecretKey = protectedSk;
    } else {
        *plainSkOut = nullptr;
    }
}

void InlineTranslateRestorePrefsSecretsAfterSave(GlobalPrefs* prefs, char* plainAk, char* plainSk) {
    if (!prefs) {
        return;
    }
    if (plainAk) {
        str::Free(prefs->translateVolcAccessKey);
        prefs->translateVolcAccessKey = plainAk;
    }
    if (plainSk) {
        str::Free(prefs->translateVolcSecretKey);
        prefs->translateVolcSecretKey = plainSk;
    }
}

static AiTocApiConfig SnapshotAiCfg() {
    AiTocApiConfig cfg;
    cfg.baseUrl = str::Dup(gGlobalPrefs ? gGlobalPrefs->aiTocApiBaseUrl : nullptr);
    cfg.key = str::Dup(gGlobalPrefs ? gGlobalPrefs->aiTocApiKey : nullptr);
    cfg.model = str::Dup(gGlobalPrefs ? gGlobalPrefs->aiTocApiModel : nullptr);
    cfg.concurrency = gGlobalPrefs ? gGlobalPrefs->aiTocApiConcurrency : 4;
    return cfg;
}

static void FreeAiCfg(AiTocApiConfig& cfg) {
    free((void*)cfg.baseUrl);
    free((void*)cfg.key);
    free((void*)cfg.model);
    cfg.baseUrl = cfg.key = cfg.model = nullptr;
}

static const char* TargetLangDisplayName(const char* code) {
    return TranslateCodeDisplayName(code);
}

// ---------------------------------------------------------------------------
// short translation cache

struct TranslateCacheEntry {
    char* key = nullptr;
    char* translation = nullptr;
};

static TranslateCacheEntry gTranslateCache[kTranslateCacheN];
static int gTranslateCacheNext = 0;
static Mutex gTranslateCacheMutex;

static char* MakeCacheKey(const char* src, const char* target) {
    return str::Format("%s\n%s", src ? src : "", target ? target : "");
}

static char* CacheLookup(const char* src, const char* target) {
    char* key = MakeCacheKey(src, target);
    defer {
        str::Free(key);
    };
    gTranslateCacheMutex.Lock();
    char* hit = nullptr;
    for (int i = 0; i < kTranslateCacheN; i++) {
        if (gTranslateCache[i].key && str::Eq(gTranslateCache[i].key, key)) {
            hit = str::Dup(gTranslateCache[i].translation);
            break;
        }
    }
    gTranslateCacheMutex.Unlock();
    return hit;
}

static void CacheStore(const char* src, const char* target, const char* translation) {
    if (str::IsEmptyOrWhiteSpace(translation)) {
        return;
    }
    char* key = MakeCacheKey(src, target);
    gTranslateCacheMutex.Lock();
    int i = gTranslateCacheNext % kTranslateCacheN;
    gTranslateCacheNext++;
    str::Free(gTranslateCache[i].key);
    str::Free(gTranslateCache[i].translation);
    gTranslateCache[i].key = key;
    gTranslateCache[i].translation = str::Dup(translation);
    gTranslateCacheMutex.Unlock();
}

// ---------------------------------------------------------------------------
// crypto / HTTP

static bool HmacSha256(const void* key, size_t keyLen, const void* data, size_t dataLen, u8 out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    DWORD objLen = 0;
    DWORD cb = 0;
    PUCHAR obj = nullptr;
    if (!BCRYPT_SUCCESS(
            BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        return false;
    }
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0))) {
        goto Exit;
    }
    obj = (PUCHAR)malloc(objLen);
    if (!obj) {
        goto Exit;
    }
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, obj, objLen, (PUCHAR)key, (ULONG)keyLen, 0))) {
        goto Exit;
    }
    if (!BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)data, (ULONG)dataLen, 0))) {
        goto Exit;
    }
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, out, 32, 0))) {
        goto Exit;
    }
    ok = true;
Exit:
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (alg) {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    free(obj);
    return ok;
}

static void Sha256Hex(const void* data, size_t dataLen, char outHex[65]) {
    u8 dig[32];
    CalcSHA2Digest(data, (int)dataLen, dig);
    for (int i = 0; i < 32; i++) {
        _snprintf_s(outHex + i * 2, 3, _TRUNCATE, "%02x", dig[i]);
    }
    outHex[64] = 0;
}

static void HexEncode(const u8* dig, int n, char* outHex) {
    for (int i = 0; i < n; i++) {
        _snprintf_s(outHex + i * 2, 3, _TRUNCATE, "%02x", dig[i]);
    }
    outHex[n * 2] = 0;
}

static void AppendJsonEscapedLocal(StrBuilder& out, const char* s) {
    out.AppendChar('"');
    if (s) {
        for (const u8* p = (const u8*)s; *p; p++) {
            u8 c = *p;
            if (c == '"') {
                out.Append("\\\"");
            } else if (c == '\\') {
                out.Append("\\\\");
            } else if (c == '\n') {
                out.Append("\\n");
            } else if (c == '\r') {
                out.Append("\\r");
            } else if (c == '\t') {
                out.Append("\\t");
            } else if (c < 0x20) {
                out.AppendFmt("\\u%04x", c);
            } else {
                out.AppendChar((char)c);
            }
        }
    }
    out.AppendChar('"');
}

struct HttpResult {
    DWORD status = 0;
    StrBuilder body;
};

static bool HttpPostRaw(const char* host, const char* pathAndQuery, const char* headers, const char* body,
                        int timeoutMs, HttpResult& res, char** errOut) {
    *errOut = nullptr;
    HINTERNET hInet =
        InternetOpenA("SumatraPDF-Plus/InlineTranslate", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) {
        *errOut = str::Dup("Cannot initialize WinINet.");
        return false;
    }
    HINTERNET hConn =
        InternetConnectA(hInet, host, INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) {
        InternetCloseHandle(hInet);
        *errOut = str::Format("Cannot connect to '%s'.", host);
        return false;
    }
    DWORD flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE;
    HINTERNET hReq = HttpOpenRequestA(hConn, "POST", pathAndQuery, nullptr, nullptr, nullptr, flags, 0);
    if (!hReq) {
        InternetCloseHandle(hConn);
        InternetCloseHandle(hInet);
        *errOut = str::Dup("Cannot create the HTTP request.");
        return false;
    }
    DWORD connectTimeout = 30 * 1000;
    DWORD recvTimeout = (DWORD)timeoutMs;
    InternetSetOptionA(hReq, INTERNET_OPTION_CONNECT_TIMEOUT, &connectTimeout, sizeof(connectTimeout));
    InternetSetOptionA(hReq, INTERNET_OPTION_SEND_TIMEOUT, &recvTimeout, sizeof(recvTimeout));
    InternetSetOptionA(hReq, INTERNET_OPTION_RECEIVE_TIMEOUT, &recvTimeout, sizeof(recvTimeout));
    bool ok = false;
    if (!HttpSendRequestA(hReq, headers, (DWORD)-1, (LPVOID)body, body ? (DWORD)strlen(body) : 0)) {
        *errOut = str::Format("Network error (WinINet %u).", (unsigned)GetLastError());
        goto Exit;
    }
    {
        DWORD status = 0;
        DWORD len = sizeof(status);
        HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr);
        res.status = status;
        for (;;) {
            char buf[8192];
            DWORD n = 0;
            if (!InternetReadFile(hReq, buf, sizeof(buf), &n)) {
                *errOut = str::Dup("Failed reading the response.");
                goto Exit;
            }
            if (n == 0) {
                break;
            }
            res.body.Append(buf, n);
        }
        ok = true;
    }
Exit:
    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hInet);
    return ok;
}

static bool VolcTranslateText(const char* ak, const char* sk, const char* text, const char* targetLang, char** outText,
                              char** errOut) {
    *outText = nullptr;
    *errOut = nullptr;
    if (str::IsEmptyOrWhiteSpace(ak) || str::IsEmptyOrWhiteSpace(sk)) {
        *errOut = str::Dup("Volcengine keys are not configured.");
        return false;
    }
    const char* host = "translate.volcengineapi.com";
    const char* region = "cn-north-1";
    const char* service = "translate";
    const char* action = "TranslateText";
    const char* version = "2020-06-01";

    SYSTEMTIME st{};
    GetSystemTime(&st);
    char dateStamp[16]{};
    char amzDate[32]{};
    _snprintf_s(dateStamp, _TRUNCATE, "%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
    _snprintf_s(amzDate, _TRUNCATE, "%04d%02d%02dT%02d%02d%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                st.wSecond);

    StrBuilder body;
    body.Append("{\"SourceLanguage\":\"\",\"TargetLanguage\":");
    AppendJsonEscapedLocal(body, targetLang ? targetLang : "zh");
    body.Append(",\"TextList\":[");
    AppendJsonEscapedLocal(body, text ? text : "");
    body.Append("]}");

    char payloadHash[65]{};
    Sha256Hex(body.Get(), body.size(), payloadHash);

    StrBuilder canonical;
    canonical.Append("POST\n/\n");
    canonical.AppendFmt("Action=%s&Version=%s\n", action, version);
    canonical.AppendFmt("content-type:application/json\nhost:%s\nx-content-sha256:%s\nx-date:%s\n\n", host, payloadHash,
                        amzDate);
    canonical.Append("content-type;host;x-content-sha256;x-date\n");
    canonical.Append(payloadHash);

    char canonicalHash[65]{};
    Sha256Hex(canonical.Get(), canonical.size(), canonicalHash);

    TempStr credentialScope = str::FormatTemp("%s/%s/%s/request", dateStamp, region, service);
    StrBuilder stringToSign;
    stringToSign.Append("HMAC-SHA256\n");
    stringToSign.Append(amzDate);
    stringToSign.AppendChar('\n');
    stringToSign.Append(credentialScope);
    stringToSign.AppendChar('\n');
    stringToSign.Append(canonicalHash);

    u8 kDate[32], kRegion[32], kService[32], kSigning[32], signature[32];
    if (!HmacSha256(sk, strlen(sk), dateStamp, strlen(dateStamp), kDate) ||
        !HmacSha256(kDate, 32, region, strlen(region), kRegion) ||
        !HmacSha256(kRegion, 32, service, strlen(service), kService) ||
        !HmacSha256(kService, 32, "request", 7, kSigning) ||
        !HmacSha256(kSigning, 32, stringToSign.Get(), stringToSign.size(), signature)) {
        *errOut = str::Dup("Failed to sign the Volcengine request.");
        return false;
    }
    char sigHex[65]{};
    HexEncode(signature, 32, sigHex);

    TempStr auth = str::FormatTemp(
        "HMAC-SHA256 Credential=%s/%s, SignedHeaders=content-type;host;x-content-sha256;x-date, Signature=%s", ak,
        credentialScope, sigHex);

    StrBuilder headers;
    headers.AppendFmt(
        "Content-Type: application/json\r\nHost: %s\r\nX-Date: %s\r\nX-Content-Sha256: %s\r\n"
        "Authorization: %s\r\n",
        host, amzDate, payloadHash, auth);

    TempStr path = str::FormatTemp("/?Action=%s&Version=%s", action, version);
    HttpResult res;
    if (!HttpPostRaw(host, path, headers.Get(), body.Get(), kVolcTimeoutMs, res, errOut)) {
        return false;
    }

    struct TranslationVisitor : json::ValueVisitor {
        StrBuilder translation;
        StrBuilder errCode;
        StrBuilder errMsg;
        bool Visit(const char* pathIn, const char* value, json::Type type) override {
            if (type != json::Type::String) {
                return true;
            }
            if (str::Eq(pathIn, "/TranslationList[0]/Translation") ||
                str::Eq(pathIn, "/TranslationList[0]/translation")) {
                translation.Append(value);
            } else if (str::Eq(pathIn, "/ResponseMetadata/Error/Code")) {
                errCode.Append(value);
            } else if (str::Eq(pathIn, "/ResponseMetadata/Error/Message") || str::Eq(pathIn, "/message")) {
                errMsg.Append(value);
            }
            return true;
        }
    } vis;
    bool parsed = res.body.Get() && json::Parse(res.body.Get(), &vis);
    bool httpOk = res.status >= 200 && res.status < 300;
    if (httpOk && parsed && vis.translation.size() > 0) {
        *outText = str::Dup(vis.translation.Get());
        return true;
    }
    if (vis.errCode.size() > 0 || vis.errMsg.size() > 0) {
        *errOut = str::Format("HTTP %u %s %s", (unsigned)res.status, vis.errCode.size() > 0 ? vis.errCode.Get() : "",
                              vis.errMsg.size() > 0 ? vis.errMsg.Get() : "");
    } else if (!httpOk) {
        *errOut = str::Format("HTTP %u", (unsigned)res.status);
    } else if (!parsed) {
        *errOut = str::Dup("Failed to parse Volcengine response.");
    } else {
        *errOut = str::Dup("Volcengine returned no translation.");
    }
    return false;
}

bool InlineTranslateVolcTest(const char* ak, const char* sk, char** resultOut, char** errOut) {
    return VolcTranslateText(ak, sk, "Hello, world.", "zh", resultOut, errOut);
}

static bool LlmTranslateText(const AiTocApiConfig& cfg, const char* text, const char* targetLang, char** outText,
                             char** errOut) {
    *outText = nullptr;
    *errOut = nullptr;
    if (!AiTocApiIsConfigured(cfg)) {
        *errOut = str::Dup("AI API is not configured.");
        return false;
    }
    TempStr system = str::FormatTemp(
        "You are a translation engine. Translate the user's text into %s. "
        "Output only the translation, with no quotes, notes, or explanation.",
        TargetLangDisplayName(targetLang));
    AiTocChatMessage msgs[2] = {};
    msgs[0].role = "system";
    msgs[0].content = system;
    msgs[1].role = "user";
    msgs[1].content = text;
    return AiTocApiChat(cfg, msgs, 2, kChatTimeoutMs, outText, errOut);
}

// ---------------------------------------------------------------------------
// selection text

static bool IsAsciiLower(char c) {
    return c >= 'a' && c <= 'z';
}

static bool IsAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// PDF selections are hard-wrapped at line ends. Join wrapped lines so the
// engine sees whole sentences; a blank line stays a paragraph break.
static char* NormalizeSelectionText(const char* s) {
    StrBuilder out;
    for (const char* p = s ? s : ""; *p;) {
        char c = *p;
        if (c != '\r' && c != '\n') {
            out.AppendChar(c == '\t' ? ' ' : c);
            p++;
            continue;
        }
        int nl = 0;
        const char* q = p;
        while (*q == '\r' || *q == '\n' || *q == ' ' || *q == '\t') {
            if (*q == '\n') {
                nl++;
            }
            q++;
        }
        if (!*q) {
            break;
        }
        while (out.size() > 0 && out.LastChar() == ' ') {
            out.RemoveLast();
        }
        if (out.size() > 0) {
            if (nl >= 2) {
                out.Append("\n\n");
            } else {
                char prev = out.LastChar();
                char next = *q;
                if (prev == '-' && IsAsciiLower(next) && out.size() >= 2 && IsAsciiAlpha(out.Get()[out.size() - 2])) {
                    out.RemoveLast();
                } else if ((u8)prev < 0x80 && (u8)next < 0x80) {
                    out.AppendChar(' ');
                }
            }
        }
        p = q;
    }
    char* res = str::Dup(out.size() > 0 ? out.Get() : "");
    str::TrimWSInPlace(res, str::TrimOpt::Both);
    return res;
}

static const char* RoughLangCode(RoughLang lang) {
    switch (lang) {
        case RoughLang::Zh:
            return "zh";
        case RoughLang::En:
            return "en";
        case RoughLang::Ja:
            return "ja";
        case RoughLang::Ko:
            return "ko";
        default:
            return nullptr;
    }
}

// Display name for a translate code in the current UI language.
static const char* LangCodeUiName(const char* code) {
    if (str::IsEmptyOrWhiteSpace(code)) {
        return nullptr;
    }
    if (str::EqI(code, "zh")) {
        return _TRA("Chinese");
    }
    return _TRA(TranslateCodeDisplayName(code));
}

// ---------------------------------------------------------------------------
// popup UI

constexpr UINT_PTR kInputSubclassId = 0x51A7;
constexpr UINT_PTR kCopiedTimerId = 1;
constexpr int kCopiedFlashMs = 1400;

constexpr int kHeaderH = 26;
constexpr int kFieldPadX = 8;
constexpr int kFieldPadY = 6;
constexpr int kFieldRadius = 6;
constexpr int kInputRowH = 30;
constexpr int kSendW = 64;
// matches the selection toolbar buttons
constexpr int kBtnPadX = 8;
constexpr int kBtnRadius = 6;
constexpr int kPopupMinH = 150;
constexpr int kResizeGrip = 8;

struct ChatTurn {
    char* role = nullptr;    // "user" / "assistant"
    char* content = nullptr; // sent to the API
    char* display = nullptr; // shown instead of content when set
    void Free() {
        str::FreePtr(&role);
        str::FreePtr(&content);
        str::FreePtr(&display);
    }
};

enum class PopupHot {
    None,
    Close,
    Copy,
    Hint,
    Send,
};

struct InlineTranslatePopup {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    HWND hwndTrans = nullptr;
    HWND hwndChat = nullptr;
    HWND hwndInput = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr; // owned
    HFONT bodyFont = nullptr;  // owned; translation and AI answers, two points above the UI font
    HBRUSH fieldBrush = nullptr;
    PopupMode mode = PopupMode::Translate;
    char* selection = nullptr;   // normalized
    char* translation = nullptr; // set once a translation arrived
    char* transError = nullptr;
    char* dstCode = nullptr;
    const char* srcCode = nullptr; // static
    const char* engine = nullptr;  // static, English, passed to _TRA
    Vec<ChatTurn> turns;
    bool translating = false;
    bool asking = false;
    bool closing = false;
    bool userSized = false;
    bool inLayout = false;
    bool deferPaint = false; // theme switch: keep the old pixels until one final paint
    bool placedAbove = false;
    bool copied = false;
    bool trackingMouse = false;
    bool chatIsRich = false;
    bool chatScrollPending = false;
    int chatScrollCp = 0;
    int generation = 0;
    Vec<SelectionOnPage>* sourceSelection = nullptr;
    PopupHot hot = PopupHot::None;
    Rect closeRc{};
    Rect copyRc{};
    Rect headerRc{};
    Rect origRc{};
    Rect transRc{};
    Rect chatRc{};
    Rect inputRc{};
    Rect hintRc{};
    Rect sendRc{};
};

static InlineTranslatePopup* gPopup = nullptr;

static void ClearTurns(InlineTranslatePopup* p) {
    for (auto& t : p->turns) {
        t.Free();
    }
    p->turns.Reset();
}

static void FreePopupData(InlineTranslatePopup* p) {
    ClearTurns(p);
    str::FreePtr(&p->selection);
    str::FreePtr(&p->translation);
    str::FreePtr(&p->transError);
    str::FreePtr(&p->dstCode);
    if (p->fieldBrush) {
        DeleteObject(p->fieldBrush);
        p->fieldBrush = nullptr;
    }
    if (p->titleFont) {
        DeleteObject(p->titleFont);
        p->titleFont = nullptr;
    }
    if (p->bodyFont) {
        DeleteObject(p->bodyFont);
        p->bodyFont = nullptr;
    }
}

static void CloseInlineTranslatePopupDismissingSelection() {
    CloseInlineTranslatePopup(true);
}

void CloseInlineTranslatePopup(bool clearSelection) {
    InlineTranslatePopup* p = gPopup;
    if (!p) {
        return;
    }
    MainWindow* win = p->win;
    Vec<SelectionOnPage>* sel = p->sourceSelection;
    gPopup = nullptr;
    p->closing = true;
    if (p->hwnd && IsWindow(p->hwnd)) {
        DestroyWindow(p->hwnd);
    }
    FreePopupData(p);
    delete p;
    // Keep the highlight while a lookup popup is still showing the same passage.
    if (clearSelection && !IsWordLookupVisible()) {
        ClearSelectionIfCurrent(win, sel);
    }
}

// Window procedures must not destroy their own window mid-message.
static void PostClosePopup() {
    uitask::Post(MkFunc0Void(CloseInlineTranslatePopupDismissingSelection), "CloseInlineTranslatePopup");
}

bool IsInlineTranslatePopupVisible() {
    return gPopup && gPopup->hwnd && IsWindowVisible(gPopup->hwnd);
}

static void SetEditTextUtf8(HWND hwnd, const char* s) {
    if (!hwnd) {
        return;
    }
    // multi-line edit controls only break lines on CRLF
    StrBuilder sb;
    for (const char* c = s ? s : ""; *c; c++) {
        if (*c == '\r') {
            continue;
        }
        if (*c == '\n') {
            sb.Append("\r\n");
        } else {
            sb.AppendChar(*c);
        }
    }
    SetWindowTextW(hwnd, ToWStrTemp(sb.size() > 0 ? sb.Get() : ""));
}

static char* GetEditTextUtf8(HWND hwnd) {
    int n = GetWindowTextLengthW(hwnd);
    if (n <= 0) {
        return str::Dup("");
    }
    WCHAR* buf = AllocArray<WCHAR>(n + 1);
    GetWindowTextW(hwnd, buf, n + 1);
    char* utf8 = ToUtf8(buf);
    free(buf);
    return utf8;
}

static const char* TransDisplayText(InlineTranslatePopup* p) {
    if (p->translating) {
        return _TRA("Translating…");
    }
    if (p->transError) {
        return p->transError;
    }
    return p->translation ? p->translation : "";
}

static TempStr ChatDisplayTextTemp(InlineTranslatePopup* p) {
    StrBuilder sb;
    for (auto& t : p->turns) {
        bool user = str::Eq(t.role, "user");
        if (sb.size() > 0) {
            sb.Append("\n\n");
        }
        sb.Append(user ? _TRA("You:") : _TRA("AI:"));
        sb.AppendChar('\n');
        const char* text = t.display ? t.display : t.content;
        sb.Append(text ? text : "");
    }
    if (p->asking) {
        if (sb.size() > 0) {
            sb.Append("\n\n");
        }
        sb.Append(_TRA("AI:"));
        sb.AppendChar('\n');
        sb.Append(_TRA("Thinking…"));
    }
    return str::DupTemp(sb.size() > 0 ? sb.Get() : "");
}

// ---- Markdown -> RTF for the chat view
// Covers what chat models emit: headings, **bold**, *italic*, `code`, ~~strike~~,
// [links](url), bullet / numbered lists, > quotes, ``` fences, tables, --- rules.

enum RtfColor {
    kRtfText = 1,
    kRtfMuted = 2,
    kRtfAccent = 3,
    kRtfCodeBg = 4,
};

struct RtfDoc {
    StrBuilder sb;
    int halfPt = 18;
    bool hasPara = false;
    bool pendingGap = false;

    int GapTw() const { return halfPt * 6; }

    void Para(int li = 0, int fi = 0, int spaceBefore = -1, int spaceAfter = 0) {
        if (spaceBefore < 0) {
            spaceBefore = pendingGap && hasPara ? GapTw() : 0;
        }
        pendingGap = false;
        if (hasPara) {
            sb.Append("\\par\n");
        }
        hasPara = true;
        sb.AppendFmt("\\pard\\sl276\\slmult1\\li%d\\fi%d\\sb%d\\sa%d", li, fi, spaceBefore, spaceAfter);
        if (li > 0) {
            sb.AppendFmt("\\tx%d", li);
        }
        sb.AppendChar(' ');
    }
};

static void RtfAppendEscaped(StrBuilder& sb, const char* s, const char* e) {
    if (!s || e <= s) {
        return;
    }
    TempWStr ws = ToWStrTemp(s, (size_t)(e - s));
    for (const WCHAR* c = ws; c && *c; c++) {
        WCHAR wc = *c;
        if (wc == '\\' || wc == '{' || wc == '}') {
            sb.AppendChar('\\');
            sb.AppendChar((char)wc);
        } else if (wc == '\n') {
            sb.Append("\\line ");
        } else if (wc == '\t') {
            sb.Append("\\tab ");
        } else if (wc == '\r') {
            continue;
        } else if (wc < 0x80) {
            sb.AppendChar((char)wc);
        } else {
            sb.AppendFmt("\\u%d?", (int)(short)wc);
        }
    }
}

static const char* FindSeq(const char* s, const char* e, const char* seq) {
    size_t n = str::Len(seq);
    for (; s + n <= e; s++) {
        if (memcmp(s, seq, n) == 0) {
            return s;
        }
    }
    return nullptr;
}

static void RtfAppendInline(StrBuilder& sb, const char* s, const char* e) {
    while (s < e) {
        char c = *s;
        if (c == '`') {
            const char* close = FindSeq(s + 1, e, "`");
            if (close && close > s + 1) {
                sb.AppendFmt("{\\f1\\highlight%d ", kRtfCodeBg);
                RtfAppendEscaped(sb, s + 1, close);
                sb.Append("}");
                s = close + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && s + 1 < e && s[1] == c) {
            const char* close = FindSeq(s + 2, e, c == '*' ? "**" : "__");
            if (close && close > s + 2) {
                sb.Append("{\\b ");
                RtfAppendInline(sb, s + 2, close);
                sb.Append("}");
                s = close + 2;
                continue;
            }
        }
        if (c == '~' && s + 1 < e && s[1] == '~') {
            const char* close = FindSeq(s + 2, e, "~~");
            if (close && close > s + 2) {
                sb.Append("{\\strike ");
                RtfAppendInline(sb, s + 2, close);
                sb.Append("}");
                s = close + 2;
                continue;
            }
        }
        if (c == '*' && s + 1 < e && s[1] != ' ' && s[1] != '*') {
            const char* close = FindSeq(s + 1, e, "*");
            if (close && close[-1] != ' ') {
                sb.Append("{\\i ");
                RtfAppendInline(sb, s + 1, close);
                sb.Append("}");
                s = close + 1;
                continue;
            }
        }
        if (c == '[') {
            const char* mid = FindSeq(s + 1, e, "](");
            const char* close = mid ? FindSeq(mid + 2, e, ")") : nullptr;
            if (close && mid > s + 1) {
                sb.AppendFmt("{\\cf%d ", kRtfAccent);
                RtfAppendInline(sb, s + 1, mid);
                sb.Append("}");
                s = close + 1;
                continue;
            }
        }
        const char* run = s + 1;
        while (run < e && !strchr("`*_~[", *run)) {
            run++;
        }
        RtfAppendEscaped(sb, s, run);
        s = run;
    }
}

static bool IsMdRule(const char* s, const char* e) {
    char m = 0;
    int n = 0;
    for (; s < e; s++) {
        if (*s == ' ') {
            continue;
        }
        if (*s != '-' && *s != '*' && *s != '_') {
            return false;
        }
        if (m && *s != m) {
            return false;
        }
        m = *s;
        n++;
    }
    return n >= 3;
}

static bool IsMdTableSeparator(const char* s, const char* e) {
    bool dash = false;
    for (; s < e; s++) {
        if (*s == '-') {
            dash = true;
        } else if (!strchr("|: ", *s)) {
            return false;
        }
    }
    return dash;
}

// "1. " / "12) " -> length of the marker including the space, else 0
static int MdOrderedMarkerLen(const char* s, const char* e) {
    const char* d = s;
    while (d < e && d - s < 3 && *d >= '0' && *d <= '9') {
        d++;
    }
    if (d == s || d + 1 >= e || (*d != '.' && *d != ')') || d[1] != ' ') {
        return 0;
    }
    return (int)(d - s) + 2;
}

static void RtfAppendMarkdown(RtfDoc& doc, const char* md) {
    StrBuilder& sb = doc.sb;
    const int indentTw = doc.halfPt * 10 * 3 / 2;
    bool inFence = false;
    int tableRow = 0;
    const char* s = md ? md : "";
    while (*s) {
        const char* eol = s;
        while (*eol && *eol != '\n') {
            eol++;
        }
        const char* e = eol;
        if (e > s && e[-1] == '\r') {
            e--;
        }
        const char* next = *eol ? eol + 1 : eol;

        int indent = 0;
        const char* t = s;
        while (t < e && (*t == ' ' || *t == '\t')) {
            indent += *t == '\t' ? 4 : 1;
            t++;
        }
        const char* te = e;
        while (te > t && (te[-1] == ' ' || te[-1] == '\t')) {
            te--;
        }

        if (te - t >= 3 && str::StartsWith(t, "```")) {
            inFence = !inFence;
            s = next;
            continue;
        }
        if (inFence) {
            doc.Para(indentTw / 2, 0, -1, 0);
            sb.AppendFmt("{\\f1\\highlight%d ", kRtfCodeBg);
            RtfAppendEscaped(sb, s, e);
            sb.Append("}");
            s = next;
            continue;
        }
        if (t == te) {
            doc.pendingGap = true;
            tableRow = 0;
            s = next;
            continue;
        }
        if (*t != '|') {
            tableRow = 0;
        }

        int hashes = 0;
        while (t + hashes < te && t[hashes] == '#') {
            hashes++;
        }
        if (hashes >= 1 && hashes <= 6 && t + hashes < te && t[hashes] == ' ') {
            const int scale[] = {130, 118, 108, 100, 100, 100};
            int fs = doc.halfPt * scale[hashes - 1] / 100;
            doc.Para(0, 0, doc.hasPara ? doc.GapTw() * 3 / 2 : 0, doc.halfPt * 3);
            sb.AppendFmt("{\\b\\fs%d ", fs);
            RtfAppendInline(sb, t + hashes + 1, te);
            sb.Append("}");
            s = next;
            continue;
        }
        if (IsMdRule(t, te)) {
            doc.pendingGap = true;
            s = next;
            continue;
        }
        if (*t == '|') {
            if (IsMdTableSeparator(t, te)) {
                s = next;
                continue;
            }
            doc.Para(0, 0, tableRow == 0 ? -1 : 0, 0);
            if (tableRow == 0) {
                sb.Append("{\\b ");
            }
            const char* cell = t + 1;
            bool firstCell = true;
            while (cell < te) {
                const char* bar = cell;
                while (bar < te && *bar != '|') {
                    bar++;
                }
                const char* cs = cell;
                const char* ce = bar;
                while (cs < ce && *cs == ' ') {
                    cs++;
                }
                while (ce > cs && ce[-1] == ' ') {
                    ce--;
                }
                if (!firstCell) {
                    sb.AppendFmt("{\\cf%d   \\u9474?   }", kRtfMuted);
                }
                firstCell = false;
                RtfAppendInline(sb, cs, ce);
                cell = bar + 1;
            }
            if (tableRow == 0) {
                sb.Append("}");
            }
            tableRow++;
            s = next;
            continue;
        }
        if (*t == '>') {
            int depth = 0;
            while (t < te && (*t == '>' || *t == ' ')) {
                depth += *t == '>' ? 1 : 0;
                t++;
            }
            doc.Para(indentTw * std::min(depth, 3), 0, -1, 0);
            sb.AppendFmt("{\\cf%d ", kRtfMuted);
            RtfAppendInline(sb, t, te);
            sb.Append("}");
            s = next;
            continue;
        }
        int level = std::min(indent / 2, 3);
        bool bullet = te - t >= 2 && (*t == '-' || *t == '*' || *t == '+') && t[1] == ' ';
        int olen = bullet ? 0 : MdOrderedMarkerLen(t, te);
        if (bullet || olen > 0) {
            int li = indentTw * (level + 1);
            doc.Para(li, -indentTw, -1, doc.halfPt * 2);
            if (bullet) {
                sb.AppendFmt("{\\cf%d %s}\\tab ", kRtfMuted, level == 0 ? "\\u8226?" : "\\u9702?");
                t += 2;
            } else {
                RtfAppendEscaped(sb, t, t + olen - 1);
                sb.Append("\\tab ");
                t += olen;
            }
            while (t < te && *t == ' ') {
                t++;
            }
            RtfAppendInline(sb, t, te);
            s = next;
            continue;
        }
        doc.Para(level > 0 ? indentTw * level : 0, 0, -1, doc.halfPt * 2);
        RtfAppendInline(sb, t, te);
        s = next;
    }
}

static void RtfAppendColorEntry(StrBuilder& sb, COLORREF c) {
    sb.AppendFmt("\\red%d\\green%d\\blue%d;", GetRValue(c), GetGValue(c), GetBValue(c));
}

static int FontHalfPoints(HWND hwnd, HFONT font) {
    LOGFONTW lf{};
    if (!font || !GetObjectW(font, sizeof(lf), &lf) || lf.lfHeight == 0) {
        return 18;
    }
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);
    int px = lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight * 4 / 5;
    return std::max(MulDiv(px, 144, dpi), 12);
}

// Builds the chat as RTF. With stopAtTurn >= 0 only the turns before it are emitted,
// which lets the caller find where that turn starts.
static void BuildChatRtf(InlineTranslatePopup* p, RtfDoc& doc, int stopAtTurn) {
    LOGFONTW lf{};
    HFONT reading = p->bodyFont ? p->bodyFont : p->font;
    GetObjectW(reading, sizeof(lf), &lf);
    doc.halfPt = FontHalfPoints(p->hwndChat, reading);
    COLORREF codeBg = BlendFloatingPopupColors(FloatingPopupFieldBg(), FloatingPopupTextColor(), 0.10f);

    StrBuilder& sb = doc.sb;
    sb.Append("{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0\\fnil ");
    TempStr face = ToUtf8Temp(lf.lfFaceName[0] ? lf.lfFaceName : L"Segoe UI");
    RtfAppendEscaped(sb, face, face + str::Len(face));
    sb.Append(";}{\\f1\\fmodern Consolas;}}{\\colortbl ;");
    RtfAppendColorEntry(sb, FloatingPopupTextColor());
    RtfAppendColorEntry(sb, FloatingPopupMutedTextColor());
    RtfAppendColorEntry(sb, FloatingPopupAccentColor());
    RtfAppendColorEntry(sb, codeBg);
    sb.AppendFmt("}\\f0\\fs%d\\cf%d ", doc.halfPt, kRtfText);

    int n = stopAtTurn >= 0 ? stopAtTurn : p->turns.Size();
    for (int i = 0; i < n; i++) {
        ChatTurn& t = p->turns[i];
        bool user = str::Eq(t.role, "user");
        doc.Para(0, 0, doc.hasPara ? doc.GapTw() * 2 : 0, doc.halfPt * 2);
        sb.AppendFmt("{\\b\\cf%d ", user ? kRtfText : kRtfMuted);
        const char* label = user ? _TRA("You:") : _TRA("AI:");
        RtfAppendEscaped(sb, label, label + str::Len(label));
        sb.Append("}");
        const char* text = t.display ? t.display : t.content;
        if (user) {
            doc.Para();
            RtfAppendEscaped(sb, text, text + str::Len(text));
        } else {
            doc.pendingGap = false;
            RtfAppendMarkdown(doc, text);
        }
    }
    if (stopAtTurn < 0 && p->asking) {
        doc.Para(0, 0, doc.hasPara ? doc.GapTw() * 2 : 0, doc.halfPt * 2);
        sb.AppendFmt("{\\b\\cf%d ", kRtfMuted);
        const char* label = _TRA("AI:");
        RtfAppendEscaped(sb, label, label + str::Len(label));
        sb.AppendFmt("}\\par\n\\pard{\\i\\cf%d ", kRtfMuted);
        const char* thinking = _TRA("Thinking…");
        RtfAppendEscaped(sb, thinking, thinking + str::Len(thinking));
        sb.Append("}");
    }
    sb.Append("}");
}

static void SetRichTextRtf(HWND hwnd, const char* rtf) {
    SETTEXTEX st{ST_DEFAULT, CP_ACP};
    SendMessageW(hwnd, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)rtf);
}

static int RichTextLen(HWND hwnd) {
    GETTEXTLENGTHEX gl{GTL_NUMCHARS | GTL_PRECISE, 1200};
    return (int)SendMessageW(hwnd, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
}

// height of the rich edit content laid out at width px
static int MeasureRichHeight(HWND hwnd, int width) {
    if (width < 1 || RichTextLen(hwnd) == 0) {
        return 0;
    }
    HDC hdc = GetDC(hwnd);
    int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
    int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    FORMATRANGE fr{};
    fr.hdc = hdc;
    fr.hdcTarget = hdc;
    fr.rc = {0, 0, MulDiv(width, 1440, dpiX), 1440 * 10000};
    fr.rcPage = fr.rc;
    fr.chrg = {0, -1};
    SendMessageW(hwnd, EM_FORMATRANGE, FALSE, (LPARAM)&fr);
    SendMessageW(hwnd, EM_FORMATRANGE, FALSE, 0);
    ReleaseDC(hwnd, hdc);
    return MulDiv(fr.rc.bottom, dpiY, 1440);
}

static void RebuildChatView(InlineTranslatePopup* p) {
    if (!p || !p->hwndChat) {
        return;
    }
    if (!p->chatIsRich) {
        SetEditTextUtf8(p->hwndChat, ChatDisplayTextTemp(p));
        int len = GetWindowTextLengthW(p->hwndChat);
        SendMessageW(p->hwndChat, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(p->hwndChat, EM_SCROLLCARET, 0, 0);
        return;
    }
    SendMessageW(p->hwndChat, EM_SETBKGNDCOLOR, 0, (LPARAM)FloatingPopupFieldBg());
    int lastUser = -1;
    for (int i = 0; i < p->turns.Size(); i++) {
        if (str::Eq(p->turns[i].role, "user")) {
            lastUser = i;
        }
    }
    // scroll so the latest question is at the top and its answer reads from the start
    p->chatScrollCp = 0;
    if (lastUser > 0) {
        RtfDoc head;
        BuildChatRtf(p, head, lastUser);
        SetRichTextRtf(p->hwndChat, head.sb.Get());
        p->chatScrollCp = RichTextLen(p->hwndChat) + 1;
    }
    RtfDoc doc;
    BuildChatRtf(p, doc, -1);
    SetRichTextRtf(p->hwndChat, doc.sb.Get());
    p->chatScrollPending = true;
}

static void ApplyChatScroll(InlineTranslatePopup* p) {
    if (!p->chatIsRich || !p->chatScrollPending) {
        return;
    }
    p->chatScrollPending = false;
    HWND h = p->hwndChat;
    int target = (int)SendMessageW(h, EM_EXLINEFROMCHAR, 0, (LPARAM)p->chatScrollCp);
    int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(h, EM_LINESCROLL, 0, (LPARAM)(target - first));
}

static int FontLineHeight(HWND hwnd, HFONT font) {
    HDC hdc = GetDC(hwnd);
    HFONT old = (HFONT)SelectObject(hdc, font);
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
    return tm.tmHeight + tm.tmExternalLeading;
}

static int MeasureTextHeight(HWND hwnd, HFONT font, const char* text, int width) {
    if (width < 1 || str::IsEmptyOrWhiteSpace(text)) {
        return 0;
    }
    HDC hdc = GetDC(hwnd);
    HFONT old = (HFONT)SelectObject(hdc, font);
    RECT rc{0, 0, width, 0};
    DrawTextW(hdc, ToWStrTemp(text), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
    return rc.bottom - rc.top;
}

static int MeasureTextWidth(HWND hwnd, HFONT font, const char* text) {
    HDC hdc = GetDC(hwnd);
    HFONT old = (HFONT)SelectObject(hdc, font);
    RECT rc{};
    DrawTextW(hdc, ToWStrTemp(text ? text : ""), -1, &rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
    return rc.right - rc.left;
}

static const char* CopyLabel(InlineTranslatePopup* p) {
    return p->copied ? _TRA("Copied") : _TRA("Copy");
}

static const char* HintText(InlineTranslatePopup* p) {
    if (p->mode == PopupMode::Translate && !InlineTranslateVolcIsConfigured() && !InlineTranslateAiApiIsConfigured()) {
        return _TRA("Set up Volcengine Translate or an AI API in Settings → AI");
    }
    return _TRA("Set up an AI API in Settings → AI to ask follow-up questions");
}

static Rect MonitorWorkArea(HWND hwnd) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return ToRect(mi.rcWork);
}

// Change height while staying on screen. A popup placed above the selection
// grows upward so it never covers the selected text.
static void ResizePopupHeight(InlineTranslatePopup* p, int dy) {
    Rect wr = WindowRect(p->hwnd);
    Rect work = MonitorWorkArea(p->hwnd);
    int y = p->placedAbove ? wr.y + wr.dy - dy : wr.y;
    if (y + dy > work.y + work.dy) {
        y = work.y + work.dy - dy;
    }
    if (y < work.y) {
        y = work.y;
    }
    SetWindowPos(p->hwnd, nullptr, wr.x, y, wr.dx, dy, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void SyncEditScrollbar(HWND edit, int contentH, int visibleH) {
    ShowScrollBar(edit, SB_VERT, contentH > visibleH);
}

static void LayoutPopup(InlineTranslatePopup* p) {
    if (!p || !p->hwnd || p->inLayout) {
        return;
    }
    p->inLayout = true;
    defer {
        p->inLayout = false;
    };
    HWND h = p->hwnd;
    Rect rc = ClientRect(h);
    int pad = DpiScale(h, kPopupPad);
    int gap = DpiScale(h, kPopupGap);
    int fx = DpiScale(h, kFieldPadX);
    int fy = DpiScale(h, kFieldPadY);
    int lineH = FontLineHeight(h, p->font);
    HFONT reading = p->bodyFont ? p->bodyFont : p->font;
    int readLine = FontLineHeight(h, reading);
    int scrollW = GetSystemMetrics(SM_CXVSCROLL);
    int contentW = rc.dx - pad * 2;
    int innerW = contentW - fx * 2 - scrollW;

    bool showTrans = p->mode == PopupMode::Translate;
    bool showChat = p->turns.Size() > 0 || p->asking;
    bool aiOk = InlineTranslateAiApiIsConfigured();
    bool showCopy = showTrans && p->translation && !p->translating;

    int headerH = DpiScale(h, kHeaderH);
    p->headerRc = Rect(pad, pad, contentW, headerH);
    int closeSz = DpiScale(h, kCloseBtn);
    p->closeRc = Rect(rc.dx - pad - closeSz, pad + (headerH - closeSz) / 2, closeSz, closeSz);
    if (showCopy) {
        int btnH = DpiScale(h, 22);
        int copyW = std::max(MeasureTextWidth(h, p->font, _TRA("Copied")), MeasureTextWidth(h, p->font, _TRA("Copy")));
        copyW += DpiScale(h, 2 * kBtnPadX);
        p->copyRc = Rect(p->closeRc.x - DpiScale(h, 6) - copyW, pad + (headerH - btnH) / 2, copyW, btnH);
    } else {
        p->copyRc = Rect();
    }
    int y = pad + headerH + DpiScale(h, 4);

    p->origRc = Rect();
    if (!str::IsEmptyOrWhiteSpace(p->selection)) {
        int origH = std::min(MeasureTextHeight(h, p->font, p->selection, contentW), lineH * 2);
        p->origRc = Rect(pad, y, contentW, origH);
        y += origH + gap;
    }

    int rowH = DpiScale(h, kInputRowH);
    int hintH = aiOk ? 0 : std::min(MeasureTextHeight(h, p->font, HintText(p), contentW), lineH * 2);
    int bottomH = aiOk ? rowH : hintH;

    int minField = readLine * 2 + fy * 2;
    int transNat = 0;
    int chatNat = 0;
    if (showTrans) {
        transNat = std::max(MeasureTextHeight(h, reading, TransDisplayText(p), innerW) + fy * 2 + 2, minField);
    }
    if (showChat) {
        int textH = p->chatIsRich ? MeasureRichHeight(p->hwndChat, innerW)
                                  : MeasureTextHeight(h, reading, ChatDisplayTextTemp(p), innerW);
        chatNat = std::max(textH + fy * 2 + 2, readLine * 3 + fy * 2);
    }
    int fieldsGap = (showTrans ? gap : 0) + (showChat ? gap : 0);
    int fixedH = y + fieldsGap + bottomH + pad;

    if (!p->userSized) {
        Rect work = MonitorWorkArea(h);
        int maxH = std::min(DpiScale(h, kPopupMaxH), work.dy);
        int want = limitValue(fixedH + transNat + chatNat, DpiScale(h, kPopupMinH), maxH);
        if (want != rc.dy) {
            ResizePopupHeight(p, want);
            UpdateFloatingPopupWindowRgn(h, kFloatingPopupCornerRadius);
            rc = ClientRect(h);
        }
    }

    int avail = std::max(rc.dy - fixedH, 0);
    int transH = 0;
    int chatH = 0;
    if (showTrans && showChat) {
        transH = std::min(transNat, std::max(minField, avail * 2 / 5));
        chatH = std::max(minField, avail - transH);
    } else if (showTrans) {
        transH = std::max(minField, avail);
    } else if (showChat) {
        chatH = std::max(minField, avail);
    }

    auto placeField = [&](HWND edit, Rect& fieldRc, bool show, int fieldH, int natH) {
        if (!show) {
            fieldRc = Rect();
            ShowWindow(edit, SW_HIDE);
            return;
        }
        fieldRc = Rect(pad, y, contentW, fieldH);
        MoveWindow(edit, fieldRc.x + fx, fieldRc.y + fy, fieldRc.dx - fx * 2, fieldRc.dy - fy * 2, TRUE);
        SyncEditScrollbar(edit, natH, fieldH);
        ShowWindow(edit, SW_SHOWNA);
        y += fieldH + gap;
    };
    placeField(p->hwndTrans, p->transRc, showTrans, transH, transNat);
    placeField(p->hwndChat, p->chatRc, showChat, chatH, chatNat);
    if (showChat) {
        ApplyChatScroll(p);
    }

    y = rc.dy - pad - bottomH;
    if (aiOk) {
        int sendW = std::max(DpiScale(h, kSendW), MeasureTextWidth(h, p->font, _TRA("Send")) + DpiScale(h, 28));
        p->inputRc = Rect(pad, y, contentW - sendW - DpiScale(h, 8), rowH);
        int editH = lineH + DpiScale(h, 2);
        MoveWindow(p->hwndInput, p->inputRc.x + fx, p->inputRc.y + (rowH - editH) / 2, p->inputRc.dx - fx * 2, editH,
                   TRUE);
        p->sendRc = Rect(rc.dx - pad - sendW, y, sendW, rowH);
        ShowWindow(p->hwndInput, SW_SHOWNA);
        p->hintRc = Rect();
    } else {
        ShowWindow(p->hwndInput, SW_HIDE);
        p->inputRc = Rect();
        p->sendRc = Rect();
        p->hintRc = Rect(pad, y, contentW, hintH);
    }
    InvalidateRect(h, nullptr, FALSE);
}

static void DrawFieldBox(HDC hdc, HWND hwnd, const Rect& r, bool focused) {
    if (r.IsEmpty()) {
        return;
    }
    int radius = DpiScale(hwnd, kFieldRadius);
    FillFloatingPopupRoundedRect(hdc, r, radius, FloatingPopupFieldBg());
    COLORREF border = focused ? FloatingPopupAccentColor() : FloatingPopupSeparatorColor();
    StrokeFloatingPopupRoundedRect(hdc, r, radius, border);
}

// Same look as the selection toolbar: text only, rounded hover fill.
// outlined adds a field-style frame for a button that sits next to a text field.
static void DrawPopupButton(HDC hdc, HWND hwnd, const Rect& r, const char* label, bool hot, bool enabled, bool outlined,
                            COLORREF textOverride = kColorUnset) {
    if (r.IsEmpty()) {
        return;
    }
    COLORREF bg = FloatingPopupBg();
    int radius = DpiScale(hwnd, kBtnRadius);
    bool hover = hot && enabled;
    if (outlined) {
        // Same surface as the popup. A field fill made Send a second, lighter block.
        COLORREF fill = bg;
        if (hover) {
            fill = AccentColor(bg, ThemeUsesDarkChrome() ? 8 : 10);
        }
        FillFloatingPopupRoundedRect(hdc, r, radius, fill);
        StrokeFloatingPopupRoundedRect(hdc, r, radius, FloatingPopupSeparatorColor());
    } else if (hover) {
        FillFloatingPopupRoundedRect(hdc, r, radius, FloatingPopupHoverBg(bg));
    }
    COLORREF col = enabled ? FloatingPopupTextColor() : FloatingPopupMutedTextColor();
    if (!outlined && enabled && !hover) {
        col = FloatingPopupMutedTextColor();
    }
    if (textOverride != kColorUnset) {
        col = textOverride;
    }
    SetTextColor(hdc, col);
    RECT tr = ToRECT(r);
    DrawTextW(hdc, ToWStrTemp(label), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static TempStr HeaderSubtitleTemp(InlineTranslatePopup* p) {
    if (p->mode == PopupMode::AskAi) {
        const char* model = gGlobalPrefs ? gGlobalPrefs->aiTocApiModel : nullptr;
        return str::IsEmptyOrWhiteSpace(model) ? nullptr : str::DupTemp(model);
    }
    StrBuilder sb;
    const char* src = LangCodeUiName(p->srcCode);
    const char* dst = LangCodeUiName(p->dstCode);
    if (src && dst && !str::EqI(p->srcCode, p->dstCode)) {
        sb.AppendFmt("%s → %s", src, dst);
    } else if (dst) {
        sb.AppendFmt("→ %s", dst);
    }
    if (p->engine && !p->translating && !p->transError) {
        if (sb.size() > 0) {
            sb.Append("  ·  ");
        }
        sb.Append(_TRA(p->engine));
    }
    return str::DupTemp(sb.size() > 0 ? sb.Get() : "");
}

static void PaintPopup(InlineTranslatePopup* p, HDC hdcWnd) {
    HWND h = p->hwnd;
    Rect rc = ClientRect(h);
    DoubleBuffer buffer(h, rc);
    HDC hdc = buffer.GetDC();

    COLORREF bg = FloatingPopupBg();
    COLORREF txt = FloatingPopupTextColor();
    COLORREF muted = FloatingPopupMutedTextColor();
    RECT full = ToRECT(rc);
    ScopedGdiObj<HBRUSH> bgBrush(CreateSolidBrush(bg));
    FillRect(hdc, &full, bgBrush);
    int radius = DpiScale(h, kFloatingPopupCornerRadius);
    FillFloatingPopupRoundedRect(hdc, rc, radius, bg);
    StrokeFloatingPopupRoundedRect(hdc, rc, radius, FloatingPopupBorderColor());

    SetBkMode(hdc, TRANSPARENT);
    HFONT old = (HFONT)SelectObject(hdc, p->titleFont ? p->titleFont : p->font);

    // header: title, subtitle, copy, close
    const char* title = p->mode == PopupMode::AskAi ? _TRA("Ask AI") : _TRA("Translate");
    int rightLimit = (p->copyRc.IsEmpty() ? p->closeRc.x : p->copyRc.x) - DpiScale(h, 8);
    RECT titleRc{p->headerRc.x, p->headerRc.y, rightLimit, p->headerRc.y + p->headerRc.dy};
    SetTextColor(hdc, txt);
    DrawTextW(hdc, ToWStrTemp(title), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT measured = titleRc;
    DrawTextW(hdc, ToWStrTemp(title), -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, p->font);
    TempStr subtitle = HeaderSubtitleTemp(p);
    if (!str::IsEmptyOrWhiteSpace(subtitle)) {
        RECT subRc = titleRc;
        subRc.left = measured.right + DpiScale(h, 10);
        if (subRc.left < subRc.right) {
            SetTextColor(hdc, muted);
            DrawTextW(hdc, ToWStrTemp(subtitle), -1, &subRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    DrawPopupButton(hdc, h, p->copyRc, CopyLabel(p), p->hot == PopupHot::Copy, true, false,
                    p->copied ? FloatingPopupAccentColor() : kColorUnset);

    DrawCloseButtonArgs cb;
    cb.hdc = hdc;
    cb.r = p->closeRc;
    cb.isHover = p->hot == PopupHot::Close;
    cb.colX = muted;
    cb.colXHover = txt;
    cb.colHoverBg = FloatingPopupCloseHoverBg(bg);
    DrawCloseButton(cb);

    // original text preview
    if (!p->origRc.IsEmpty()) {
        RECT r = ToRECT(p->origRc);
        SetTextColor(hdc, muted);
        DrawTextW(hdc, ToWStrTemp(p->selection), -1, &r,
                  DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    HWND focus = GetFocus();
    DrawFieldBox(hdc, h, p->transRc, false);
    DrawFieldBox(hdc, h, p->chatRc, false);
    DrawFieldBox(hdc, h, p->inputRc, focus && focus == p->hwndInput);
    DrawPopupButton(hdc, h, p->sendRc, _TRA("Send"), p->hot == PopupHot::Send, !p->asking, true);

    if (!p->hintRc.IsEmpty()) {
        RECT r = ToRECT(p->hintRc);
        bool hot = p->hot == PopupHot::Hint;
        SetTextColor(hdc, hot ? FloatingPopupAccentColor() : muted);
        DrawTextW(hdc, ToWStrTemp(HintText(p)), -1, &r, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    }

    SelectObject(hdc, old);
    buffer.Flush(hdcWnd);
}

static void ApplyEditTheme(HWND edit, bool rich) {
    if (!edit) {
        return;
    }
    if (rich) {
        // SetWindowTheme(rich, L"DarkMode_Explorer" / L"Explorer", ...) loads the
        // text-services stack the first time and freezes the UI for many seconds.
        // The scrollbar class list only recolors the bar. Answer colors stay in the RTF.
        if (DynSetWindowTheme) {
            const WCHAR* scroll = nullptr;
            if (UseDarkModeLib() && ThemeUsesDarkChrome() && DarkMode::isExperimentalActive()) {
                scroll = L"DarkMode_Explorer::ScrollBar";
            }
            DynSetWindowTheme(edit, nullptr, scroll);
        }
        return;
    }
    if (UseDarkModeLib() && ThemeUsesDarkChrome()) {
        DarkMode::setDarkScrollBar(edit);
    } else if (DynSetWindowTheme) {
        DynSetWindowTheme(edit, L"Explorer", nullptr);
    }
}

static void ApplyPopupTheme(InlineTranslatePopup* p) {
    if (!p || !p->hwnd) {
        return;
    }
    if (p->fieldBrush) {
        DeleteObject(p->fieldBrush);
    }
    p->fieldBrush = CreateSolidBrush(FloatingPopupFieldBg());
    ApplyEditTheme(p->hwndTrans, false);
    ApplyEditTheme(p->hwndChat, p->chatIsRich);
    ApplyEditTheme(p->hwndInput, false);
    if (p->chatIsRich) {
        // colors live in the RTF color table
        RebuildChatView(p);
        if (IsWindowVisible(p->hwnd) || p->deferPaint) {
            LayoutPopup(p);
        }
    }
    UpdateFloatingPopupWindowRgn(p->hwnd, kFloatingPopupCornerRadius);
    if (!p->deferPaint) {
        RedrawWindow(p->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }
}

// WM_SETREDRAW clears WS_VISIBLE. Put the bit back so the window stays logically
// visible and the old pixels stay on screen until the one final paint.
constexpr int kThemeFreezeMax = 4;
static HWND gThemeFrozen[kThemeFreezeMax];
static int gThemeFreezeCount = 0;

static void FreezeWindowRedraw(HWND hwnd) {
    if (!hwnd || gThemeFreezeCount >= kThemeFreezeMax) {
        return;
    }
    if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) == 0) {
        return;
    }
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_VISIBLE) == 0) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_VISIBLE);
    }
    gThemeFrozen[gThemeFreezeCount++] = hwnd;
}

static void FreezePopupRedraw(InlineTranslatePopup* p) {
    gThemeFreezeCount = 0;
    p->deferPaint = true;
    FreezeWindowRedraw(p->hwndTrans);
    FreezeWindowRedraw(p->hwndChat);
    FreezeWindowRedraw(p->hwndInput);
    FreezeWindowRedraw(p->hwnd);
}

static void ThawPopupRedraw(InlineTranslatePopup* p) {
    HWND parent = p ? p->hwnd : nullptr;
    for (int i = 0; i < gThemeFreezeCount; i++) {
        HWND hwnd = gThemeFrozen[i];
        if (!hwnd || hwnd == parent) {
            continue;
        }
        bool keep = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        if (!keep) {
            ShowWindow(hwnd, SW_HIDE);
        }
    }
    gThemeFreezeCount = 0;
    if (!p || !parent) {
        return;
    }
    bool keep = (GetWindowLongPtrW(parent, GWL_STYLE) & WS_VISIBLE) != 0;
    SendMessageW(parent, WM_SETREDRAW, TRUE, 0);
    p->deferPaint = false;
    if (!keep) {
        ShowWindow(parent, SW_HIDE);
        return;
    }
    RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

void RefreshInlineTranslatePopupTheme() {
    if (!gPopup) {
        return;
    }
    bool live = gPopup->hwnd && (GetWindowLongPtrW(gPopup->hwnd, GWL_STYLE) & WS_VISIBLE);
    if (live) {
        FreezePopupRedraw(gPopup);
    }
    ApplyPopupTheme(gPopup);
}

void FinishInlineTranslatePopupTheme() {
    if (gThemeFreezeCount == 0) {
        if (gPopup) {
            gPopup->deferPaint = false;
        }
        return;
    }
    ThawPopupRedraw(gPopup);
}

static void PositionPopupNear(InlineTranslatePopup* p, Point anchor) {
    HWND h = p->hwnd;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint({anchor.x, anchor.y}, MONITOR_DEFAULTTONEAREST), &mi);
    Rect work = ToRect(mi.rcWork);
    int w = std::min(DpiScale(h, kPopupMaxW), work.dx);
    w = std::max(w, std::min(DpiScale(h, kPopupMinW), work.dx));
    int hgt = DpiScale(h, kPopupMinH);
    int gap = DpiScale(h, 16);
    int x = anchor.x - w / 2;
    int y = anchor.y + gap;
    // decide side by the space a grown popup will need, not the initial height
    int expected = DpiScale(h, 320);
    p->placedAbove = y + expected > work.y + work.dy && anchor.y - gap - expected >= work.y;
    if (p->placedAbove) {
        y = anchor.y - gap - hgt;
    }
    x = limitValue(x, work.x, work.x + work.dx - w);
    y = limitValue(y, work.y, work.y + work.dy - hgt);
    SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateFloatingPopupWindowRgn(h, kFloatingPopupCornerRadius);
}

static void OnSendClicked(InlineTranslatePopup* p);

static LRESULT CALLBACK InputSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* p = (InlineTranslatePopup*)data;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                if (p == gPopup) {
                    OnSendClicked(p);
                }
                return 0;
            }
            if (wp == VK_ESCAPE) {
                PostClosePopup();
                return 0;
            }
            break;
        case WM_CHAR:
            // swallow Enter / Esc so the single-line edit doesn't beep
            if (wp == '\r' || wp == 27) {
                return 0;
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(GetParent(hwnd), nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, InputSubclassProc, id);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK InlineTranslateWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static void RegisterPopupClass() {
    static ATOM atom = 0;
    if (atom) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = InlineTranslateWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kInlineTranslateClassName;
    atom = RegisterClassExW(&wc);
}

static HFONT CreateTitleFont(HFONT base) {
    LOGFONTW lf{};
    if (!base || !GetObjectW(base, sizeof(lf), &lf)) {
        return nullptr;
    }
    lf.lfWeight = FW_SEMIBOLD;
    return CreateFontIndirectW(&lf);
}

// Two points larger than the UI font, for the translation and the AI answer.
// A fixed step, not a percentage, so a large UI font is not scaled again.
static HFONT CreateReadingFont(HFONT base, HWND dpiHwnd) {
    LOGFONTW lf{};
    if (!base || !GetObjectW(base, sizeof(lf), &lf)) {
        return nullptr;
    }
    HDC hdc = GetDC(dpiHwnd);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) {
        ReleaseDC(dpiHwnd, hdc);
    }
    int bump = std::max(MulDiv(2, dpi, 72), 1);
    if (lf.lfHeight < 0) {
        lf.lfHeight -= bump;
    } else if (lf.lfHeight > 0) {
        lf.lfHeight += bump;
    } else {
        lf.lfHeight = -MulDiv(11, dpi, 72);
    }
    return CreateFontIndirectW(&lf);
}

static InlineTranslatePopup* CreatePopup(MainWindow* win) {
    RegisterPopupClass();
    auto* p = new InlineTranslatePopup();
    p->win = win;
    p->font = GetAppFont();
    p->titleFont = CreateTitleFont(p->font);
    p->bodyFont = CreateReadingFont(p->font, win->hwndFrame);
    // owned by the frame: stays above it, hides with it, no TOPMOST over other apps
    p->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kInlineTranslateClassName, L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0,
                              win->hwndFrame, nullptr, GetModuleHandle(nullptr), p);
    if (!p->hwnd) {
        FreePopupData(p);
        delete p;
        return nullptr;
    }
    HINSTANCE inst = GetModuleHandle(nullptr);
    DWORD roStyle = WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL;
    p->hwndTrans = CreateWindowExW(0, WC_EDITW, L"", roStyle, 0, 0, 0, 0, p->hwnd, nullptr, inst, nullptr);
    static HMODULE richEditLib = LoadLibraryW(L"Msftedit.dll");
    if (richEditLib) {
        p->hwndChat = CreateWindowExW(0, MSFTEDIT_CLASS, L"", roStyle, 0, 0, 0, 0, p->hwnd, nullptr, inst, nullptr);
    }
    p->chatIsRich = p->hwndChat != nullptr;
    if (p->chatIsRich) {
        SendMessageW(p->hwndChat, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(p->hwndChat, EM_SETEVENTMASK, 0, 0);
        // Spell checking is on by default. Its first use loads a language service
        // and can freeze the UI for many seconds, which showed up as the first
        // light/dark switch while an answer was on screen.
        DWORD langOpt = (DWORD)SendMessageW(p->hwndChat, EM_GETLANGOPTIONS, 0, 0);
        langOpt &= ~IMF_SPELLCHECKING;
        SendMessageW(p->hwndChat, EM_SETLANGOPTIONS, 0, (LPARAM)langOpt);
    } else {
        p->hwndChat = CreateWindowExW(0, WC_EDITW, L"", roStyle, 0, 0, 0, 0, p->hwnd, nullptr, inst, nullptr);
    }
    p->hwndInput = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, p->hwnd,
                                   nullptr, inst, nullptr);
    HFONT reading = p->bodyFont ? p->bodyFont : p->font;
    SendMessageW(p->hwndTrans, WM_SETFONT, (WPARAM)reading, FALSE);
    SendMessageW(p->hwndChat, WM_SETFONT, (WPARAM)reading, FALSE);
    SendMessageW(p->hwndInput, WM_SETFONT, (WPARAM)p->font, FALSE);
    SendMessageW(p->hwndInput, EM_SETCUEBANNER, TRUE, (LPARAM)ToWStrTemp(_TRA("Ask about the selected text…")));
    SendMessageW(p->hwndInput, EM_SETLIMITTEXT, 2000, 0);
    SetWindowSubclass(p->hwndInput, InputSubclassProc, kInputSubclassId, (DWORD_PTR)p);
    ApplyPopupTheme(p);
    return p;
}

// Reuses the open popup for the same window; a new session always starts clean.
static InlineTranslatePopup* BeginPopupSession(MainWindow* win, const char* selection, PopupMode mode) {
    if (gPopup && gPopup->win != win) {
        CloseInlineTranslatePopup();
    }
    if (!gPopup) {
        gPopup = CreatePopup(win);
        if (!gPopup) {
            return nullptr;
        }
    }
    InlineTranslatePopup* p = gPopup;
    WindowTab* tab = win->CurrentTab();
    p->sourceSelection = tab ? tab->selectionOnPage : nullptr;
    // global so a late result from a closed popup can't match a new one
    static int gNextGeneration = 0;
    p->generation = ++gNextGeneration;
    p->mode = mode;
    p->translating = false;
    p->asking = false;
    p->userSized = false;
    p->copied = false;
    p->engine = nullptr;
    p->srcCode = nullptr;
    KillTimer(p->hwnd, kCopiedTimerId);
    ClearTurns(p);
    str::ReplacePtr(&p->selection, NormalizeSelectionText(selection));
    str::FreePtr(&p->translation);
    str::FreePtr(&p->transError);
    str::FreePtr(&p->dstCode);
    SetEditTextUtf8(p->hwndTrans, "");
    SetEditTextUtf8(p->hwndChat, "");
    SetEditTextUtf8(p->hwndInput, "");

    POINT cur{};
    GetCursorPos(&cur);
    PositionPopupNear(p, Point{cur.x, cur.y});
    return p;
}

static void ShowPopup(InlineTranslatePopup* p) {
    LayoutPopup(p);
    // don't steal focus from the document; clicking into the popup activates it
    ShowWindow(p->hwnd, SW_SHOWNA);
}

static void RefreshTranslationView(InlineTranslatePopup* p) {
    SetEditTextUtf8(p->hwndTrans, TransDisplayText(p));
    LayoutPopup(p);
}

static char* TruncateForTranslate(const char* s) {
    if (!s) {
        return str::Dup("");
    }
    const u8* p = (const u8*)s;
    int runes = 0;
    while (*p && runes < kMaxTranslateChars) {
        int n = utf8RuneLen(p);
        if (n < 1) {
            break;
        }
        p += n;
        runes++;
    }
    return str::Dup(s, (size_t)(p - (const u8*)s));
}

struct TranslateJob {
    int generation = 0;
    char* text = nullptr;
    char* target = nullptr;
    char* ak = nullptr;
    char* sk = nullptr;
    AiTocApiConfig cfg{};
    const char* engine = nullptr;
    char* result = nullptr;
    char* error = nullptr;
};

static void TranslateJobFinished(TranslateJob* job) {
    defer {
        str::Free(job->text);
        str::Free(job->target);
        str::Free(job->ak);
        str::Free(job->sk);
        FreeAiCfg(job->cfg);
        str::Free(job->result);
        str::Free(job->error);
        delete job;
    };
    InlineTranslatePopup* p = gPopup;
    if (!p || p->generation != job->generation) {
        return;
    }
    p->translating = false;
    if (job->result) {
        str::ReplacePtr(&p->translation, job->result);
        job->result = nullptr;
        p->engine = job->engine;
    } else {
        TempStr err = str::FormatTemp("%s\n%s", _TRA("Translation failed."), job->error ? job->error : "");
        str::ReplaceWithCopy(&p->transError, err);
    }
    RefreshTranslationView(p);
}

static void TranslateJobWorker(TranslateJob* job) {
    if (char* cached = CacheLookup(job->text, job->target)) {
        job->result = cached;
        job->engine = !str::IsEmptyOrWhiteSpace(job->ak) ? "Volcengine" : "AI";
    } else {
        char* volcErr = nullptr;
        bool haveVolc = !str::IsEmptyOrWhiteSpace(job->ak) && !str::IsEmptyOrWhiteSpace(job->sk);
        if (haveVolc && VolcTranslateText(job->ak, job->sk, job->text, job->target, &job->result, &volcErr)) {
            job->engine = "Volcengine";
        } else if (AiTocApiIsConfigured(job->cfg)) {
            if (volcErr) {
                logf("InlineTranslate: Volcengine failed, falling back to AI: %s\n", volcErr);
            }
            if (LlmTranslateText(job->cfg, job->text, job->target, &job->result, &job->error)) {
                job->engine = "AI";
            }
        } else {
            job->error = volcErr;
            volcErr = nullptr;
        }
        str::Free(volcErr);
        if (job->result) {
            CacheStore(job->text, job->target, job->result);
        }
    }
    uitask::Post(MkFunc0(TranslateJobFinished, job), "InlineTranslateDone");
}

static void StartTranslate(InlineTranslatePopup* p) {
    p->translating = true;
    auto* job = new TranslateJob();
    job->generation = p->generation;
    job->text = TruncateForTranslate(p->selection);
    job->target = str::Dup(p->dstCode);
    job->ak = str::Dup(gGlobalPrefs->translateVolcAccessKey);
    job->sk = str::Dup(gGlobalPrefs->translateVolcSecretKey);
    job->cfg = SnapshotAiCfg();
    RunAsync(MkFunc0(TranslateJobWorker, job), "InlineTranslate");
    RefreshTranslationView(p);
}

struct AskJob {
    int generation = 0;
    AiTocApiConfig cfg{};
    Vec<AiTocChatMessage> msgs; // points into owned
    Vec<char*> owned;
    char* result = nullptr;
    char* error = nullptr;
    void FreeAll() {
        for (char* s : owned) {
            str::Free(s);
        }
        owned.Reset();
        msgs.Reset();
        FreeAiCfg(cfg);
        str::FreePtr(&result);
        str::FreePtr(&error);
    }
};

static void AskJobAppend(AskJob* job, const char* role, const char* content) {
    char* r = str::Dup(role);
    char* c = str::Dup(content ? content : "");
    job->owned.Append(r);
    job->owned.Append(c);
    AiTocChatMessage m;
    m.role = r;
    m.content = c;
    job->msgs.Append(m);
}

static void AskJobFinished(AskJob* job) {
    defer {
        job->FreeAll();
        delete job;
    };
    InlineTranslatePopup* p = gPopup;
    if (!p || p->generation != job->generation) {
        return;
    }
    p->asking = false;
    ChatTurn turn;
    turn.role = str::Dup("assistant");
    if (job->result) {
        turn.content = job->result;
        job->result = nullptr;
    } else {
        turn.content = str::Format("%s %s", _TRA("The AI request failed."), job->error ? job->error : "");
    }
    p->turns.Append(turn);
    RebuildChatView(p);
    LayoutPopup(p);
}

static void AskJobWorker(AskJob* job) {
    AiTocApiChat(job->cfg, job->msgs.LendData(), job->msgs.Size(), kChatTimeoutMs, &job->result, &job->error);
    uitask::Post(MkFunc0(AskJobFinished, job), "InlineAskAiDone");
}

// content goes to the API, display (optional) is what the user sees
static void StartAsk(InlineTranslatePopup* p, const char* content, const char* display) {
    if (!p || p->asking || str::IsEmptyOrWhiteSpace(content)) {
        return;
    }
    ChatTurn userTurn;
    userTurn.role = str::Dup("user");
    userTurn.content = str::Dup(content);
    userTurn.display = display ? str::Dup(display) : nullptr;
    p->turns.Append(userTurn);
    p->asking = true;

    auto* job = new AskJob();
    job->generation = p->generation;
    job->cfg = SnapshotAiCfg();
    if (p->mode == PopupMode::Translate) {
        // in Ask AI mode the first user turn already carries the selection
        StrBuilder sys;
        sys.Append(
            "You help the user understand text selected in a document. Answer clearly and concisely, "
            "in the language the user writes in.\n\nSelected text:\n");
        sys.Append(p->selection ? p->selection : "");
        if (p->translation) {
            sys.Append("\n\nTranslation shown to the user:\n");
            sys.Append(p->translation);
        }
        AskJobAppend(job, "system", sys.Get());
    }
    for (auto& t : p->turns) {
        AskJobAppend(job, t.role, t.content);
    }
    RunAsync(MkFunc0(AskJobWorker, job), "InlineAskAi");
    RebuildChatView(p);
    LayoutPopup(p);
}

static void OnSendClicked(InlineTranslatePopup* p) {
    if (!p || p->asking) {
        return;
    }
    char* q = GetEditTextUtf8(p->hwndInput);
    str::TrimWSInPlace(q, str::TrimOpt::Both);
    if (!str::IsEmptyOrWhiteSpace(q)) {
        SetEditTextUtf8(p->hwndInput, "");
        StartAsk(p, q, nullptr);
    }
    str::Free(q);
}

static void CopyTranslation(InlineTranslatePopup* p) {
    if (!p->translation || !CopyTextToClipboard(p->translation)) {
        return;
    }
    p->copied = true;
    SetTimer(p->hwnd, kCopiedTimerId, kCopiedFlashMs, nullptr);
    InvalidateRect(p->hwnd, nullptr, FALSE);
}

static void OpenAiSettingsFromPopup() {
    InlineTranslatePopup* p = gPopup;
    MainWindow* win = p ? p->win : nullptr;
    CloseInlineTranslatePopup(true);
    if (win && IsMainWindowValid(win)) {
        ShowOptionsDialogAtPage(win, kSettingsPageAi);
    }
}

static PopupHot HitTestHot(InlineTranslatePopup* p, Point pt) {
    if (p->closeRc.Contains(pt)) {
        return PopupHot::Close;
    }
    if (!p->copyRc.IsEmpty() && p->copyRc.Contains(pt)) {
        return PopupHot::Copy;
    }
    if (!p->hintRc.IsEmpty() && p->hintRc.Contains(pt)) {
        return PopupHot::Hint;
    }
    if (!p->sendRc.IsEmpty() && p->sendRc.Contains(pt)) {
        return PopupHot::Send;
    }
    return PopupHot::None;
}

static LRESULT PopupNcHitTest(InlineTranslatePopup* p, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(p->hwnd, &pt);
    Rect rc = ClientRect(p->hwnd);
    int grip = DpiScale(p->hwnd, kResizeGrip);
    bool right = pt.x >= rc.dx - grip;
    bool bottom = pt.y >= rc.dy - grip;
    if (right && bottom) {
        return HTBOTTOMRIGHT;
    }
    if (bottom) {
        return HTBOTTOM;
    }
    if (right) {
        return HTRIGHT;
    }
    Point cpt{pt.x, pt.y};
    if (HitTestHot(p, cpt) != PopupHot::None) {
        return HTCLIENT;
    }
    // the header and the original-text preview drag the popup
    if (pt.y < p->headerRc.y + p->headerRc.dy || (!p->origRc.IsEmpty() && p->origRc.Contains(cpt))) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

static LRESULT CALLBACK InlineTranslateWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        auto* created = (InlineTranslatePopup*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)created);
        if (created) {
            created->hwnd = hwnd;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* p = (InlineTranslatePopup*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!p) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (!p->deferPaint) {
                PaintPopup(p, hdc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wp;
            bool dim = (HWND)lp == p->hwndTrans && (p->translating || p->transError);
            SetTextColor(hdc, dim ? FloatingPopupMutedTextColor() : FloatingPopupTextColor());
            SetBkColor(hdc, FloatingPopupFieldBg());
            if (!p->fieldBrush) {
                p->fieldBrush = CreateSolidBrush(FloatingPopupFieldBg());
            }
            return (LRESULT)p->fieldBrush;
        }
        case WM_NCHITTEST:
            return PopupNcHitTest(p, lp);
        case WM_NCLBUTTONDBLCLK:
            // no maximize on header double-click
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && p->hot == PopupHot::Hint) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_SIZING:
            p->userSized = true;
            break;
        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = DpiScale(hwnd, kPopupMinW);
            mmi->ptMinTrackSize.y = DpiScale(hwnd, kPopupMinH);
            return 0;
        }
        case WM_SIZE:
            UpdateFloatingPopupWindowRgn(hwnd, kFloatingPopupCornerRadius);
            LayoutPopup(p);
            return 0;
        case WM_MOUSEMOVE: {
            PopupHot hot = HitTestHot(p, Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot != p->hot) {
                p->hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (!p->trackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                p->trackingMouse = TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            p->trackingMouse = false;
            if (p->hot != PopupHot::None) {
                p->hot = PopupHot::None;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            PopupHot hot = HitTestHot(p, Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot == PopupHot::Close) {
                PostClosePopup();
            } else if (hot == PopupHot::Copy) {
                CopyTranslation(p);
            } else if (hot == PopupHot::Send) {
                OnSendClicked(p);
            } else if (hot == PopupHot::Hint) {
                uitask::Post(MkFunc0Void(OpenAiSettingsFromPopup), "InlineTranslateOpenSettings");
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kCopiedTimerId) {
                KillTimer(hwnd, kCopiedTimerId);
                p->copied = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CLOSE:
            PostClosePopup();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            p->hwnd = nullptr;
            if (!p->closing) {
                // destroyed together with the owner frame
                if (gPopup == p) {
                    gPopup = nullptr;
                }
                FreePopupData(p);
                delete p;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool InlineTranslatePopupPreTranslate(MSG& msg) {
    InlineTranslatePopup* p = gPopup;
    if (!p || !p->hwnd || !IsWindowVisible(p->hwnd)) {
        return false;
    }
    if (msg.message < WM_KEYFIRST || msg.message > WM_KEYLAST) {
        return false;
    }
    bool inPopup = msg.hwnd == p->hwnd || IsChild(p->hwnd, msg.hwnd);
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
        bool fromOwner = !inPopup && p->win && FindMainWindowByHwnd(msg.hwnd) == p->win;
        if (!inPopup && !fromOwner) {
            return false;
        }
        MainWindow* win = p->win;
        CloseInlineTranslatePopup();
        if (inPopup && win && IsMainWindowValid(win)) {
            HwndSetFocus(win->hwndCanvas);
        }
        return true;
    }
    if (!inPopup) {
        return false;
    }
    // keep the app's single-key accelerators (and Ctrl+C on the document
    // selection) away from text typed or copied in the popup
    TranslateMessage(&msg);
    DispatchMessage(&msg);
    return true;
}

static TempStr GetSelectionOrNotify(WindowTab* tab) {
    if (!tab || !tab->win || !HasPermission(Perm::CopySelection)) {
        return nullptr;
    }
    bool isTextOnly = false;
    TempStr sel = GetSelectedTextTemp(tab, "\n", isTextOnly);
    if (str::IsEmptyOrWhiteSpace(sel)) {
        return nullptr;
    }
    DisplayModel* dm = tab->AsFixed();
    if (dm && !gDisableDocumentRestrictions && !dm->GetEngine()->AllowsCopyingText()) {
        NotificationCreateArgs args;
        args.hwndParent = tab->win->hwndCanvas;
        args.msg = _TRA("Copying text was denied");
        args.timeoutMs = 3000;
        ShowNotification(args);
        return nullptr;
    }
    return sel;
}

void TranslateSelectionInTab(MainWindow* win, WindowTab* tab) {
    if (!win || !tab || !gGlobalPrefs || !gGlobalPrefs->enableInlineTranslate) {
        return;
    }
    if (!HasPermission(Perm::InternetAccess)) {
        return;
    }
    TempStr sel = GetSelectionOrNotify(tab);
    if (!sel) {
        return;
    }
    HideSelectionToolbar(win);
    InlineTranslatePopup* p = BeginPopupSession(win, sel, PopupMode::Translate);
    if (!p) {
        return;
    }
    p->srcCode = RoughLangCode(DetectSourceLangRough(p->selection));
    p->dstCode =
        str::Dup(ResolveTranslateTarget(p->selection, gGlobalPrefs->translateTargetMode, trans::GetCurrentLangCode()));
    if (!InlineTranslateVolcIsConfigured() && !InlineTranslateAiApiIsConfigured()) {
        str::ReplaceWithCopy(&p->transError,
                             _TRA("No translation service is set up yet. Add Volcengine Translate keys or an AI API "
                                  "in Settings → AI. Google / DeepL web translation is still in the selection menu."));
        RefreshTranslationView(p);
        ShowPopup(p);
        return;
    }
    StartTranslate(p);
    ShowPopup(p);
}

void AskAiSelectionInTab(MainWindow* win, WindowTab* tab, const char* selection, const char* initialPrompt) {
    if (!win || !tab || str::IsEmptyOrWhiteSpace(selection) || str::IsEmptyOrWhiteSpace(initialPrompt)) {
        return;
    }
    HideSelectionToolbar(win);
    InlineTranslatePopup* p = BeginPopupSession(win, selection, PopupMode::AskAi);
    if (!p) {
        return;
    }
    StartAsk(p, initialPrompt, _TRA("Explain the selected text"));
    ShowPopup(p);
}
