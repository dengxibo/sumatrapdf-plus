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

#include <commctrl.h>
#include <richedit.h>
#include <gdiplus.h>

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
#include "Commands.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "Menu.h"
#include "SumatraConfig.h"
#include "Selection.h"
#include "SelectionToolbar.h"
#include "WindowTab.h"
#include "SidebarThumbs.h"
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
constexpr int kCloseHit = 26;
constexpr int kCloseGlyph = 14;
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

static AiChatService ActiveWebAiService() {
    const char* p = gGlobalPrefs ? gGlobalPrefs->aiChatProvider : nullptr;
    if (str::EqI(p, "deepseek")) {
        return AiChatService::DeepSeek;
    }
    if (str::EqI(p, "chatgpt")) {
        return AiChatService::ChatGPT;
    }
    return AiChatService::Doubao;
}

static bool WebAiTranslateEnabled() {
    return gGlobalPrefs && gGlobalPrefs->enableAskAI && HasPermission(Perm::InternetAccess);
}

// A missing key and a rejected Authorization header are the same for the reader:
// there is no translation API to call, so the web chat should do it.
static bool TranslateErrorMeansUnusableApi(const char* err) {
    if (str::IsEmpty(err)) {
        return false;
    }
    return str::FindI(err, "Authorization") || str::FindI(err, "not configured") ||
           str::FindI(err, "InvalidAccessKey") || str::FindI(err, "SignatureDoesNotMatch") ||
           str::FindI(err, "InvalidCredential");
}

static void SendSelectionToWebAi(MainWindow* win, const char* text, const char* targetLang) {
    if (!WebAiTranslateEnabled() || str::IsEmptyOrWhiteSpace(text)) {
        return;
    }
    const char* langName = TargetLangDisplayName(targetLang);
    TempStr prompt = str::FormatTemp("%s %s:\n%s", _TRA("Please translate the following text into"),
                                     langName ? langName : "the target language", text);
    LaunchAiChatWithPromptAsync(ActiveWebAiService(), prompt);
    if (win && win->hwndCanvas) {
        NotificationCreateArgs args;
        args.hwndParent = win->hwndCanvas;
        args.msg = _TRA("Question sent to AI.");
        args.timeoutMs = 5000;
        ShowNotification(args);
    }
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
constexpr UINT_PTR kChatSubclassId = 0x51A8;
constexpr UINT_PTR kTransSubclassId = 0x51A9;
constexpr UINT_PTR kCopiedTimerId = 1;
constexpr int kCopiedFlashMs = 1200;

constexpr int kHeaderH = 30;
constexpr int kFieldPadX = 8;
constexpr int kFieldPadY = 6;
constexpr int kFieldRadius = 6;
constexpr int kComposerH = 38;
constexpr int kComposerRadius = 8;
constexpr int kSendHit = 28;
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
    Model,
    Close,
    Copy,
    Hint,
    Send,
    Context,
};

struct InlineTranslatePopup {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    HWND hwndTip = nullptr;
    HWND hwndTrans = nullptr;
    HWND hwndChat = nullptr;
    HWND hwndInput = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr; // owned
    HFONT bodyFont = nullptr;  // owned; translation and AI answers, two points above the UI font
    HBRUSH fieldBrush = nullptr;
    HBRUSH chatBrush = nullptr;
    bool inputHasText = false;
    PopupMode mode = PopupMode::Translate;
    char* selection = nullptr;  // normalized
    int contextPage = 0;        // page the selection was taken from; 0 if unknown
    bool contextOpen = false;   // docked: the context line is expanded
    bool chatFollowTail = true; // docked: stay at the bottom unless the user scrolled up
    bool chatAdjustingScroll = false;
    bool chatScrollHot = false;
    bool chatScrollDrag = false;
    bool composerScrollHot = false;
    bool composerScrollDrag = false;
    bool sendDown = false;
    int composerLines = 1;       // docked composer grows from 1 to 5 lines
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
    bool docked = false; // AI sidebar page, not the floating popup
    WindowTab* boundTab = nullptr;
    bool inLayout = false;
    bool deferPaint = false; // theme switch: keep the old pixels until one final paint
    bool placedAbove = false;
    bool copied = false;
    bool trackingMouse = false;
    bool chatIsRich = false;
    bool chatScrollPending = false;
    int chatScrollCp = 0;
    int dpi = 0; // monitor the popup is laid out for; 0 until the first placement
    int generation = 0;
    Vec<SelectionOnPage>* sourceSelection = nullptr;
    PopupHot hot = PopupHot::None;
    Rect closeRc{};
    Rect copyRc{};
    Rect headerRc{};
    Rect modelRc{};
    Rect contextRc{};
    Rect origRc{};
    Rect transRc{};
    Rect chatRc{};
    Rect inputRc{};
    Rect hintRc{};
    Rect sendRc{};
};

static InlineTranslatePopup* gPopup = nullptr;
// Ask AI sidebar. Separate from the translate popup so a translation does not
// wipe the conversation, and so the normal Ask AI path is not the floating window.
static InlineTranslatePopup* gAsk = nullptr;
// Session-only override. Never write it back to the shared TOC/API settings.
static char* gAskModel = nullptr;
static char* gAskModelProvider = nullptr;
static bool gAskModelsLoading = false;
static int gNextPopupGeneration = 1;

struct AiChatState {
    Vec<ChatTurn> turns;
    char* draft = nullptr;
    char* selection = nullptr;
    int contextPage = 0;
    int generation = 0;
    bool asking = false;
    ~AiChatState() {
        for (auto& t : turns) {
            t.Free();
        }
        str::Free(draft);
        str::Free(selection);
    }
};

static AiChatState* AskState(WindowTab* tab, bool create) {
    if (!tab) {
        return nullptr;
    }
    auto* st = (AiChatState*)tab->askAiState;
    if (!st && create) {
        st = new AiChatState();
        tab->askAiState = st;
    }
    return st;
}

static void MoveTurns(Vec<ChatTurn>& dst, Vec<ChatTurn>& src) {
    for (auto& t : dst) {
        t.Free();
    }
    dst.Reset();
    for (auto& t : src) {
        dst.Append(t);
    }
    src.Reset();
}

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
    if (p->chatBrush) {
        DeleteObject(p->chatBrush);
        p->chatBrush = nullptr;
    }
    if (p->titleFont) {
        DeleteObject(p->titleFont);
        p->titleFont = nullptr;
    }
    if (p->bodyFont) {
        DeleteObject(p->bodyFont);
        p->bodyFont = nullptr;
    }
    if (p->hwndTip) {
        DestroyWindow(p->hwndTip);
        p->hwndTip = nullptr;
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
    kRtfUserBg = 5,
};

enum class MdBlock {
    None,
    Para,
    List,
    Head,
    Other
};

struct RtfDoc {
    StrBuilder sb;
    int halfPt = 18;
    bool hasPara = false;
    bool pendingGap = false;
    bool compact = false; // docked sidebar: tighter headings and lists
    MdBlock last = MdBlock::None;

    int GapTw() const { return halfPt * 6; }

    // RichEdit twips track the DC, so 15 twips stays 1 DIP at any scale.
    static int Tw(int dip) { return dip * 15; }

    int BlockGap(MdBlock next) const {
        if (!hasPara) {
            return 0;
        }
        // Sidebar answers are read in a narrow column, so the steps stay small.
        int head = compact ? 6 : 16;
        int headRun = compact ? 2 : 10;
        int list = compact ? 1 : 6;
        int listBreak = compact ? 4 : 10;
        int afterHead = compact ? 2 : 8;
        int para = compact ? 4 : 11;
        if (next == MdBlock::Head) {
            return Tw(last == MdBlock::Head ? headRun : head);
        }
        if (next == MdBlock::List) {
            return Tw(last == MdBlock::List ? list : listBreak);
        }
        if (next == MdBlock::Para) {
            if (last == MdBlock::Head) {
                return Tw(afterHead);
            }
            if (last == MdBlock::Para && !pendingGap) {
                return 0;
            }
            return Tw(para);
        }
        if (last == MdBlock::Other && !pendingGap) {
            return 0;
        }
        return Tw(pendingGap ? (compact ? 6 : 8) : 2);
    }

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

    void Start(MdBlock kind, int li = 0, int fi = 0) {
        Para(li, fi, BlockGap(kind), 0);
        last = kind;
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
            // Long URLs have no spaces. A break after these marks wraps them in the sidebar.
            if (wc == '/' || wc == '?' || wc == '&' || wc == '=') {
                sb.Append("\\u8203?");
            }
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

static void RtfAppendInline(StrBuilder& sb, const char* s, const char* e, bool quiet) {
    while (s < e) {
        char c = *s;
        if (c == '`') {
            const char* close = FindSeq(s + 1, e, "`");
            if (close && close > s + 1) {
                // Sidebar: same face as the answer, and a faint wash. A monospace
                // chip reads as a tag, not as a word in the sentence.
                sb.AppendFmt(quiet ? "{\\highlight%d " : "{\\f1\\highlight%d ", kRtfCodeBg);
                RtfAppendEscaped(sb, s + 1, close);
                sb.Append("}");
                s = close + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && s + 1 < e && s[1] == c) {
            const char* close = FindSeq(s + 2, e, c == '*' ? "**" : "__");
            if (close && close > s + 2) {
                // Models bold whole sentences. In the sidebar only a short span stays bold.
                bool bold = !quiet || (int)(close - (s + 2)) <= 36;
                if (bold) {
                    sb.Append("{\\b ");
                }
                RtfAppendInline(sb, s + 2, close, quiet);
                if (bold) {
                    sb.Append("}");
                }
                s = close + 2;
                continue;
            }
        }
        if (c == '~' && s + 1 < e && s[1] == '~') {
            const char* close = FindSeq(s + 2, e, "~~");
            if (close && close > s + 2) {
                sb.Append("{\\strike ");
                RtfAppendInline(sb, s + 2, close, quiet);
                sb.Append("}");
                s = close + 2;
                continue;
            }
        }
        if (c == '*' && s + 1 < e && s[1] != ' ' && s[1] != '*') {
            const char* close = FindSeq(s + 1, e, "*");
            if (close && close[-1] != ' ') {
                sb.Append("{\\i ");
                RtfAppendInline(sb, s + 1, close, quiet);
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
                RtfAppendInline(sb, s + 1, mid, quiet);
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
    const int indentTw = doc.compact ? doc.halfPt * 4 : doc.halfPt * 10 * 3 / 2;
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
            doc.Start(MdBlock::Other, indentTw / 2, 0);
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
            const int scaleFull[] = {130, 118, 108, 100, 100, 100};
            const int scaleCompact[] = {112, 106, 102, 100, 100, 100};
            const int* scale = doc.compact ? scaleCompact : scaleFull;
            int fs = doc.halfPt * scale[hashes - 1] / 100;
            doc.Start(MdBlock::Head);
            // In the sidebar only the first two heading levels stay bold.
            bool bold = !doc.compact || hashes <= 2;
            sb.AppendFmt(bold ? "{\\b\\fs%d " : "{\\fs%d ", fs);
            RtfAppendInline(sb, t + hashes + 1, te, doc.compact);
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
            doc.Start(MdBlock::Other);
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
                RtfAppendInline(sb, cs, ce, doc.compact);
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
            doc.Start(MdBlock::Other, indentTw * std::min(depth, doc.compact ? 2 : 3), 0);
            sb.AppendFmt("{\\cf%d ", kRtfMuted);
            RtfAppendInline(sb, t, te, doc.compact);
            sb.Append("}");
            s = next;
            continue;
        }
        int level = std::min(indent / 2, doc.compact ? 1 : 3);
        bool bullet = te - t >= 2 && (*t == '-' || *t == '*' || *t == '+') && t[1] == ' ';
        int olen = bullet ? 0 : MdOrderedMarkerLen(t, te);
        if (bullet || olen > 0) {
            // Sidebar column is narrow. A 14 DIP step pushed nested items into a sliver.
            const int listStep = RtfDoc::Tw(doc.compact ? 10 : 18);
            const int listHang = RtfDoc::Tw(doc.compact ? 8 : 12);
            int li = listStep * (level + 1);
            doc.Start(MdBlock::List, li, -listHang);
            if (bullet) {
                sb.AppendFmt("{\\cf%d%s}\\tab ", kRtfMuted, level == 0 ? "\\u8226?" : "\\u9702?");
                t += 2;
            } else {
                RtfAppendEscaped(sb, t, t + olen - 1);
                sb.Append("\\tab ");
                t += olen;
            }
            while (t < te && *t == ' ') {
                t++;
            }
            RtfAppendInline(sb, t, te, doc.compact);
            s = next;
            continue;
        }
        doc.Start(MdBlock::Para, level > 0 ? indentTw * level : 0, 0);
        RtfAppendInline(sb, t, te, doc.compact);
        s = next;
    }
}

static void RtfAppendColorEntry(StrBuilder& sb, COLORREF c) {
    sb.AppendFmt("\\red%d\\green%d\\blue%d;", GetRValue(c), GetGValue(c), GetBValue(c));
}

static int FontHalfPoints(HFONT font, int dpi) {
    LOGFONTW lf{};
    if (!font || !GetObjectW(font, sizeof(lf), &lf) || lf.lfHeight == 0) {
        return 18;
    }
    if (dpi < 72) {
        dpi = 96;
    }
    int px = lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight * 4 / 5;
    return std::max(MulDiv(px, 144, dpi), 12);
}

// Ask AI answers sit on the popup surface. Translate keeps the field surface.
static COLORREF ChatContentBg(const InlineTranslatePopup* p) {
    if (p && p->docked) {
        return ThemeSidebarBackgroundColor();
    }
    if (p && p->mode == PopupMode::AskAi) {
        return FloatingPopupBg();
    }
    return FloatingPopupFieldBg();
}

// Builds the chat as RTF. With stopAtTurn >= 0 only the turns before it are emitted,
// which lets the caller find where that turn starts.
static void BuildChatRtf(InlineTranslatePopup* p, RtfDoc& doc, int stopAtTurn) {
    LOGFONTW lf{};
    HFONT reading = p->bodyFont ? p->bodyFont : p->font;
    GetObjectW(reading, sizeof(lf), &lf);
    int dpi = p->dpi >= 72 ? p->dpi : DpiGet(p->hwnd);
    doc.halfPt = FontHalfPoints(reading, dpi);
    doc.compact = p->docked;
    float codeMix = p->docked ? 0.045f : 0.10f;
    COLORREF codeBg = BlendFloatingPopupColors(ChatContentBg(p), FloatingPopupTextColor(), codeMix);
    float userLift = p->docked ? 0.04f : (ThemeUsesDarkChrome() ? 0.08f : 0.06f);
    COLORREF userBg = BlendFloatingPopupColors(ChatContentBg(p), FloatingPopupTextColor(), userLift);

    StrBuilder& sb = doc.sb;
    sb.Append("{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0\\fnil ");
    TempStr face = ToUtf8Temp(lf.lfFaceName[0] ? lf.lfFaceName : L"Segoe UI");
    RtfAppendEscaped(sb, face, face + str::Len(face));
    sb.Append(";}{\\f1\\fmodern Consolas;}}{\\colortbl ;");
    RtfAppendColorEntry(sb, FloatingPopupTextColor());
    RtfAppendColorEntry(sb, FloatingPopupMutedTextColor());
    RtfAppendColorEntry(sb, FloatingPopupAccentColor());
    RtfAppendColorEntry(sb, codeBg);
    RtfAppendColorEntry(sb, userBg);
    sb.AppendFmt("}\\f0\\fs%d\\cf%d ", doc.halfPt, kRtfText);

    int labelFs = std::max(doc.halfPt - (p->docked ? 2 : 0), 14);
    int n = stopAtTurn >= 0 ? stopAtTurn : p->turns.Size();
    for (int i = 0; i < n; i++) {
        ChatTurn& t = p->turns[i];
        bool user = str::Eq(t.role, "user");
        int before = 0;
        if (doc.hasPara) {
            before = p->docked ? RtfDoc::Tw(12) : doc.GapTw() * 2;
        }
        doc.Para(0, 0, before, RtfDoc::Tw(2));
        sb.AppendFmt("{\\fs%d\\cf%d ", labelFs, kRtfMuted);
        const char* label = user ? _TRA("You") : _TRA("AI");
        RtfAppendEscaped(sb, label, label + str::Len(label));
        sb.Append("}");
        const char* text = t.display ? t.display : t.content;
        if (user) {
            doc.Para(RtfDoc::Tw(p->docked ? 2 : 0), 0, RtfDoc::Tw(3), RtfDoc::Tw(4));
            if (p->docked) {
                sb.AppendFmt("\\highlight%d ", kRtfUserBg);
            }
            RtfAppendEscaped(sb, text, text + str::Len(text));
            if (p->docked) {
                sb.Append("\\highlight0 ");
            }
        } else {
            doc.pendingGap = false;
            RtfAppendMarkdown(doc, text);
        }
    }
    if (stopAtTurn < 0 && p->asking) {
        int before = doc.hasPara ? (p->docked ? RtfDoc::Tw(12) : doc.GapTw() * 2) : 0;
        doc.Para(0, 0, before, RtfDoc::Tw(2));
        sb.AppendFmt("{\\fs%d\\cf%d ", labelFs, kRtfMuted);
        const char* label = _TRA("AI");
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

static bool ChatIsNearBottom(HWND hwnd) {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (!GetScrollInfo(hwnd, SB_VERT, &si)) {
        return true;
    }
    if (si.nMax <= (int)si.nPage) {
        return true;
    }
    return si.nPos + (int)si.nPage >= si.nMax - 8;
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
    POINT saved{};
    bool keepPlace = p->docked && !p->chatFollowTail;
    if (keepPlace) {
        SendMessageW(p->hwndChat, EM_GETSCROLLPOS, 0, (LPARAM)&saved);
    }
    SendMessageW(p->hwndChat, EM_SETBKGNDCOLOR, 0, (LPARAM)ChatContentBg(p));
    int lastUser = -1;
    for (int i = 0; i < p->turns.Size(); i++) {
        if (str::Eq(p->turns[i].role, "user")) {
            lastUser = i;
        }
    }
    // Floating popup: the latest question sits at the top of the view.
    // Docked sidebar: follow the tail, unless the reader has scrolled up.
    p->chatScrollCp = 0;
    if (!p->docked && lastUser > 0) {
        RtfDoc head;
        BuildChatRtf(p, head, lastUser);
        SetRichTextRtf(p->hwndChat, head.sb.Get());
        p->chatScrollCp = RichTextLen(p->hwndChat) + 1;
    }
    RtfDoc doc;
    BuildChatRtf(p, doc, -1);
    p->chatAdjustingScroll = true;
    SetRichTextRtf(p->hwndChat, doc.sb.Get());
    if (p->docked && p->chatFollowTail) {
        int len = RichTextLen(p->hwndChat);
        SendMessageW(p->hwndChat, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(p->hwndChat, EM_SCROLLCARET, 0, 0);
        p->chatScrollPending = false;
    } else if (keepPlace) {
        SendMessageW(p->hwndChat, EM_SETSCROLLPOS, 0, (LPARAM)&saved);
        p->chatScrollPending = false;
    } else {
        p->chatScrollPending = true;
    }
    p->chatAdjustingScroll = false;
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
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (p->deferPaint) {
        flags |= SWP_NOREDRAW;
    }
    SetWindowPos(p->hwnd, nullptr, wr.x, y, wr.dx, dy, flags);
}

static void SyncEditScrollbar(HWND edit, int contentH, int visibleH) {
    ShowScrollBar(edit, SB_VERT, contentH > visibleH);
}

// Prefer the DPI we chose for this monitor. GetDpiForWindow still reports the
// primary monitor until the popup is shown or WM_DPICHANGED arrives.
static int Px(HWND hwnd, int x) {
    auto* p = hwnd ? (InlineTranslatePopup*)GetWindowLongPtrW(hwnd, GWLP_USERDATA) : nullptr;
    int dpi = (p && p->dpi >= 72) ? p->dpi : DpiGet(hwnd);
    return MulDiv(x, dpi, 96);
}

// One muted quoted line, so the selection reads as context rather than the first sentence of the answer.
static TempStr AskContextLineTemp(const char* selection) {
    StrBuilder sb;
    sb.Append("\xE2\x80\x9C");
    sb.Append(selection ? selection : "");
    sb.Append("\xE2\x80\x9D");
    return str::DupTemp(sb.Get());
}

// Language direction only. The engine name belongs on the larger popup.
static TempStr TranslateLangLabelTemp(InlineTranslatePopup* p) {
    const char* src = LangCodeUiName(p->srcCode);
    const char* dst = LangCodeUiName(p->dstCode);
    if (src && dst && !str::EqI(p->srcCode, p->dstCode)) {
        return str::FormatTemp("%s → %s", src, dst);
    }
    if (dst) {
        return str::DupTemp(dst);
    }
    return nullptr;
}

static void SyncCopyTip(InlineTranslatePopup* p) {
    if (!p || !p->hwndTip || !p->hwnd) {
        return;
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = p->hwnd;
    ti.uId = 1;
    SendMessageW(p->hwndTip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    if (p->copyRc.IsEmpty()) {
        return;
    }
    ti.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
    ti.rect = ToRECT(p->copyRc);
    static WCHAR tip[96];
    lstrcpynW(tip, ToWStrTemp(_TRA("Copy translation")), dimof(tip));
    ti.lpszText = tip;
    SendMessageW(p->hwndTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void LayoutTranslateMinimal(InlineTranslatePopup* p) {
    HWND h = p->hwnd;
    ShowWindow(p->hwndChat, SW_HIDE);
    ShowWindow(p->hwndInput, SW_HIDE);
    p->chatRc = {};
    p->inputRc = {};
    p->sendRc = {};
    p->hintRc = {};
    p->origRc = {};

    int padX = Px(h, 12);
    int padY = Px(h, 4);
    int padBottom = Px(h, 18);
    int headerH = Px(h, 36);
    // Same box as the word-lookup close button.
    int closeSz = Px(h, 16);
    int copySz = Px(h, 16);
    int iconGap = Px(h, 6);
    HFONT reading = p->bodyFont ? p->bodyFont : p->font;
    const char* text = TransDisplayText(p);
    if (!text) {
        text = "";
    }
    Rect work = MonitorWorkArea(h);
    int minW = Px(h, 320);
    int maxW = std::min(Px(h, 380), std::max(1, work.dx - Px(h, 16)));
    if (maxW < minW) {
        minW = maxW;
    }
    bool showCopy = p->translation && !p->transError && !p->translating;
    TempStr lang = TranslateLangLabelTemp(p);
    int chromeW = padX + MeasureTextWidth(h, p->titleFont ? p->titleFont : p->font, _TRA("Translate"));
    if (!str::IsEmptyOrWhiteSpace(lang)) {
        chromeW += Px(h, 10) + MeasureTextWidth(h, p->font, lang);
    }
    chromeW += Px(h, 8) + closeSz + padX;
    if (showCopy) {
        chromeW += iconGap + copySz;
    }
    int textW = MeasureTextWidth(h, reading, text) + Px(h, 8);
    int wantW = std::max(chromeW, textW + padX * 2);
    wantW = limitValue(wantW, minW, maxW);
    int inner = std::max(1, wantW - padX * 2);
    int lineH = FontLineHeight(h, reading);
    int textH = MeasureTextHeight(h, reading, text, inner);
    if (textH > lineH) {
        textH += std::max(1, lineH / 5);
    }
    int maxH = std::max(headerH + padY + padBottom + lineH * 2, work.dy * 2 / 5);
    int wantH = headerH + padY + textH + padBottom;
    wantH = std::max(wantH, headerH + padY + lineH + padBottom);
    if (wantH > maxH) {
        wantH = maxH;
    }

    auto placeWindow = [&](int w, int ht) {
        Rect wr = WindowRect(h);
        int x = wr.x;
        int y = p->placedAbove ? wr.y + wr.dy - ht : wr.y;
        if (y + ht > work.y + work.dy) {
            y = work.y + work.dy - ht;
        }
        if (y < work.y) {
            y = work.y;
        }
        if (x + w > work.x + work.dx) {
            x = work.x + work.dx - w;
        }
        if (x < work.x) {
            x = work.x;
        }
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
        if (p->deferPaint) {
            flags |= SWP_NOREDRAW;
        }
        SetWindowPos(h, nullptr, x, y, w, ht, flags);
        UpdateFloatingPopupWindowRgn(h, kFloatingPopupCornerRadius, !p->deferPaint);
    };
    if (!p->userSized) {
        placeWindow(wantW, wantH);
    }

    auto placeFields = [&]() {
        Rect rc = ClientRect(h);
        p->headerRc = Rect(0, 0, rc.dx, headerH);
        int iconY = (headerH - closeSz) / 2;
        p->closeRc = Rect(rc.dx - padX - closeSz, iconY, closeSz, closeSz);
        if (showCopy) {
            p->copyRc = Rect(p->closeRc.x - iconGap - copySz, iconY, copySz, copySz);
        } else {
            p->copyRc = {};
        }
        int textTop = headerH + padY;
        int editRight = rc.dx - padX;
        int editW = std::max(1, editRight - padX);
        int fieldH = std::max(1, rc.dy - textTop - padBottom);
        p->transRc = Rect(padX, textTop, editW, fieldH);
        MoveWindow(p->hwndTrans, padX, textTop, editW, fieldH, TRUE);
        RECT fr{0, 0, std::max(1, editW - Px(h, 4)), std::max(1, fieldH)};
        SendMessageW(p->hwndTrans, EM_SETRECT, 0, (LPARAM)&fr);
    };
    placeFields();
    int editH = textH;
    int nChars = GetWindowTextLengthW(p->hwndTrans);
    if (nChars > 0) {
        LRESULT pos = SendMessageW(p->hwndTrans, EM_POSFROMCHAR, (WPARAM)(nChars - 1), 0);
        if (pos != (LRESULT)-1) {
            int charY = (short)HIWORD(pos);
            editH = std::max(editH, charY + lineH + Px(h, 4));
        }
    }
    if (!p->userSized && editH + headerH + padY + padBottom > wantH && wantH < maxH) {
        wantH = std::min(maxH, editH + headerH + padY + padBottom);
        Rect wr = WindowRect(h);
        placeWindow(wr.dx, wantH);
        placeFields();
    }
    int fieldH = std::max(1, p->transRc.dy);
    SyncEditScrollbar(p->hwndTrans, editH, fieldH);
    SyncCopyTip(p);
    ShowWindow(p->hwndTrans, SW_SHOWNA);
    InvalidateRect(h, nullptr, FALSE);
}

static COLORREF AskComposerFill() {
    COLORREF bg = ThemeSidebarBackgroundColor();
    // One small step, so the field is a footer on the sidebar and not a second panel.
    if (ThemeUsesBlackChrome()) {
        return AccentColor(bg, 8);
    }
    if (ThemeUsesDarkChrome()) {
        return AccentColor(bg, 5);
    }
    return AccentColor(bg, 4);
}

static COLORREF AskComposerBorder(bool focused) {
    COLORREF bg = ThemeSidebarBackgroundColor();
    int step = focused ? (ThemeUsesBlackChrome() ? 16 : (ThemeUsesDarkChrome() ? 14 : 12))
                       : (ThemeUsesBlackChrome() ? 8 : (ThemeUsesDarkChrome() ? 7 : 6));
    return AccentColor(bg, step);
}

static const char* AskCueText(InlineTranslatePopup* p) {
    if (!p) {
        return "";
    }
    return p->turns.Size() == 0 ? _TRA("Ask about this document...") : _TRA("Ask a follow-up...");
}

static void UpdateAskCue(InlineTranslatePopup* p) {
    if (!p || !p->docked || !p->hwndInput) {
        return;
    }
    // Multiline edits ignore EM_SETCUEBANNER. The input subclass paints this.
    InvalidateRect(p->hwndInput, nullptr, FALSE);
}

// Docked AI page: header, conversation, sticky composer. No frame around the conversation.
static void LayoutAskSidebar(InlineTranslatePopup* p) {
    HWND h = p->hwnd;
    Rect rc = ClientRect(h);
    int padX = Px(h, 12);
    int padY = Px(h, 8);
    int lineH = FontLineHeight(h, p->font);
    HFONT reading = p->bodyFont ? p->bodyFont : p->font;
    int readLine = FontLineHeight(h, reading);
    int scrollW = GetSystemMetrics(SM_CXVSCROLL);
    bool aiOk = InlineTranslateAiApiIsConfigured();
    bool showChat = p->turns.Size() > 0 || p->asking;

    int headerH = lineH + Px(h, 2);
    p->headerRc = Rect(padX, padY, std::max(1, rc.dx - padX * 2), headerH);
    // The sidebar shell already has the close. A second × next to the model name
    // reads as another command (clear chat, dismiss the model, close the page).
    p->closeRc = Rect();
    p->copyRc = Rect();

    int y = padY + headerH + Px(h, 2);
    // The user turn carries the request and quoted selection; no redundant
    // document/selection subtitle above the conversation.
    p->contextRc = Rect();
    p->origRc = Rect();
    y += Px(h, 10);

    int lines = std::max(1, std::min(p->composerLines, 5));
    int rowPad = Px(h, 8);
    int rowH = lineH * lines + rowPad;
    int bottomPad = Px(h, 10);
    int hintH = aiOk ? 0 : std::min(MeasureTextHeight(h, p->font, HintText(p), rc.dx - padX * 2), lineH * 2);
    int bottomH = aiOk ? rowH : hintH;
    int composerTop = rc.dy - bottomPad - bottomH;
    if (composerTop < y + readLine) {
        composerTop = y + readLine;
    }
    int chatH = std::max(0, composerTop - Px(h, 8) - y);

    if (!showChat) {
        p->chatRc = Rect(padX, y, std::max(1, rc.dx - padX * 2), chatH);
        ShowWindow(p->hwndChat, SW_HIDE);
    } else {
        int inset = padX;
        int chatW = std::max(1, rc.dx - inset);
        p->chatRc = Rect(0, y, rc.dx, chatH);
        int textW = std::max(1, chatW - scrollW - Px(h, 4));
        int textH = p->chatIsRich ? MeasureRichHeight(p->hwndChat, textW)
                                  : MeasureTextHeight(h, reading, ChatDisplayTextTemp(p), textW);
        MoveWindow(p->hwndChat, inset, y, chatW, std::max(1, chatH), TRUE);
        SyncEditScrollbar(p->hwndChat, textH, chatH);
        ShowWindow(p->hwndChat, SW_SHOWNA);
        ApplyChatScroll(p);
    }
    ShowWindow(p->hwndTrans, SW_HIDE);
    p->transRc = Rect();

    if (aiOk) {
        p->inputRc = Rect(padX, composerTop, std::max(1, rc.dx - padX * 2), rowH);
        int sendHit = Px(h, 22);
        int inset = Px(h, 8);
        int sendY = lines <= 1 ? composerTop + (rowH - sendHit) / 2 : composerTop + rowH - inset - sendHit;
        p->sendRc = Rect(p->inputRc.x + p->inputRc.dx - inset - sendHit, sendY, sendHit, sendHit);
        int editH = lineH * lines;
        int editX = p->inputRc.x + inset;
        int editW = std::max(1, p->sendRc.x - Px(h, 4) - editX);
        MoveWindow(p->hwndInput, editX, composerTop + (rowH - editH) / 2, editW, editH, TRUE);
        int rawLines = (int)SendMessageW(p->hwndInput, EM_GETLINECOUNT, 0, 0);
        ShowScrollBar(p->hwndInput, SB_VERT, rawLines > 5);
        ShowWindow(p->hwndInput, SW_SHOWNA);
        p->hintRc = Rect();
    } else {
        ShowWindow(p->hwndInput, SW_HIDE);
        p->inputRc = Rect();
        p->sendRc = Rect();
        p->hintRc = Rect(padX, composerTop, std::max(1, rc.dx - padX * 2), hintH);
    }
    UpdateAskCue(p);
    InvalidateRect(h, nullptr, FALSE);
}

static void LayoutPopup(InlineTranslatePopup* p) {
    if (!p || !p->hwnd || p->inLayout) {
        return;
    }
    p->inLayout = true;
    defer {
        p->inLayout = false;
    };
    if (p->docked) {
        LayoutAskSidebar(p);
        return;
    }
    Rect rc0 = ClientRect(p->hwnd);
    if (p->docked && (rc0.dx < 8 || rc0.dy < 8)) {
        return;
    }
    if (p->mode == PopupMode::Translate && !p->docked) {
        LayoutTranslateMinimal(p);
        return;
    }
    HWND h = p->hwnd;
    Rect rc = ClientRect(h);
    int pad = Px(h, kPopupPad);
    int gap = Px(h, kPopupGap);
    int fx = Px(h, kFieldPadX);
    int fy = Px(h, kFieldPadY);
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

    int headerH = Px(h, kHeaderH);
    p->headerRc = Rect(pad, pad, contentW, headerH);
    int closeSz = Px(h, kCloseHit);
    if (p->docked) {
        p->closeRc = Rect();
    } else {
        p->closeRc = Rect(rc.dx - pad - closeSz, pad + (headerH - closeSz) / 2, closeSz, closeSz);
    }
    if (showCopy) {
        int btnH = Px(h, 22);
        int copyW = std::max(MeasureTextWidth(h, p->font, _TRA("Copied")), MeasureTextWidth(h, p->font, _TRA("Copy")));
        copyW += Px(h, 2 * kBtnPadX);
        p->copyRc = Rect(p->closeRc.x - Px(h, 6) - copyW, pad + (headerH - btnH) / 2, copyW, btnH);
    } else {
        p->copyRc = Rect();
    }
    bool flatChat = p->mode == PopupMode::AskAi;
    int chatInset = flatChat ? Px(h, 2) : fx;
    int chatWindowW = std::max(1, contentW - chatInset * 2);
    int chatMeasureW = std::max(1, chatWindowW - scrollW);

    int y = pad + headerH + Px(h, flatChat ? 9 : 4);

    p->origRc = Rect();
    // Docked AI puts each selection into the conversation as a short quote.
    if (!p->docked && !str::IsEmptyOrWhiteSpace(p->selection)) {
        int maxLines = flatChat ? 1 : 2;
        const char* preview = flatChat ? AskContextLineTemp(p->selection) : p->selection;
        int origH = std::min(MeasureTextHeight(h, p->font, preview, contentW), lineH * maxLines);
        p->origRc = Rect(pad, y, contentW, origH);
        y += origH + (flatChat ? Px(h, 13) : gap);
    }

    int rowH = Px(h, kComposerH);
    int hintH = aiOk ? 0 : std::min(MeasureTextHeight(h, p->font, HintText(p), contentW), lineH * 2);
    int bottomH = aiOk ? rowH : hintH;

    int minField = readLine * 2 + fy * 2;
    int transNat = 0;
    int chatNat = 0;
    if (showTrans) {
        transNat = std::max(MeasureTextHeight(h, reading, TransDisplayText(p), innerW) + fy * 2 + 2, minField);
    }
    if (showChat) {
        int textH = p->chatIsRich ? MeasureRichHeight(p->hwndChat, chatMeasureW)
                                  : MeasureTextHeight(h, reading, ChatDisplayTextTemp(p), chatMeasureW);
        int chatPad = flatChat ? Px(h, 2) : fy * 2 + 2;
        chatNat = std::max(textH + chatPad, readLine * 3);
    }
    int fieldsGap = (showTrans ? gap : 0) + (showChat ? gap : 0);
    int fixedH = y + fieldsGap + bottomH + pad;

    if (!p->userSized && !p->docked) {
        Rect work = MonitorWorkArea(h);
        int maxH = std::min(Px(h, kPopupMaxH), work.dy);
        int want = limitValue(fixedH + transNat + chatNat, Px(h, kPopupMinH), maxH);
        if (want != rc.dy) {
            ResizePopupHeight(p, want);
            UpdateFloatingPopupWindowRgn(h, kFloatingPopupCornerRadius, !p->deferPaint);
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
    if (!showChat) {
        p->chatRc = Rect();
        ShowWindow(p->hwndChat, SW_HIDE);
    } else if (flatChat) {
        p->chatRc = Rect(pad, y, contentW, chatH);
        MoveWindow(p->hwndChat, p->chatRc.x + chatInset, p->chatRc.y, chatWindowW, p->chatRc.dy, TRUE);
        SyncEditScrollbar(p->hwndChat, chatNat, chatH);
        ShowWindow(p->hwndChat, SW_SHOWNA);
        y += chatH + gap;
    } else {
        placeField(p->hwndChat, p->chatRc, true, chatH, chatNat);
    }
    if (showChat) {
        ApplyChatScroll(p);
    }

    y = rc.dy - pad - bottomH;
    if (aiOk) {
        p->inputRc = Rect(pad, y, contentW, rowH);
        int sendHit = Px(h, kSendHit);
        int inset = Px(h, 10);
        int gapToSend = Px(h, 4);
        p->sendRc = Rect(pad + contentW - inset - sendHit, y + (rowH - sendHit) / 2, sendHit, sendHit);
        int editH = lineH + Px(h, 2);
        int editX = pad + inset;
        int editW = std::max(1, p->sendRc.x - gapToSend - editX);
        MoveWindow(p->hwndInput, editX, y + (rowH - editH) / 2, editW, editH, TRUE);
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

static void DrawFieldBox(HDC hdc, HWND hwnd, const Rect& r, bool focused, int radiusDip = kFieldRadius) {
    if (r.IsEmpty()) {
        return;
    }
    int radius = Px(hwnd, radiusDip);
    FillFloatingPopupRoundedRect(hdc, r, radius, FloatingPopupFieldBg());
    COLORREF border = focused ? FloatingPopupAccentColor() : FloatingPopupSeparatorColor();
    StrokeFloatingPopupRoundedRect(hdc, r, radius, border);
}

// Paper plane, one outline. An up arrow reads as Previous next to Find's arrows.
static void DrawComposerSend(HDC hdc, HWND hwnd, const Rect& r, bool hot, bool pressed, bool enabled,
                             COLORREF hoverFill = kColorUnset) {
    if (r.IsEmpty()) {
        return;
    }
    if ((hot || pressed) && enabled) {
        COLORREF fill = hoverFill == kColorUnset ? FloatingPopupHoverBg(FloatingPopupFieldBg()) : hoverFill;
        if (pressed) {
            fill = AccentColor(fill, ThemeUsesDarkChrome() ? 8 : 6);
        }
        FillFloatingPopupRoundedRect(hdc, r, Px(hwnd, 4), fill);
    }
    COLORREF col = enabled ? FloatingPopupTextColor() : FloatingPopupMutedTextColor();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Color c;
    c.SetFromCOLORREF(col);
    // Glyph stays ~12 DIP inside the hit target, same optical size as a toolbar icon.
    float penW = std::max(1.15f, (float)Px(hwnd, 1));
    Gdiplus::Pen pen(c, penW);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    float u = (float)Px(hwnd, 12);
    float ox = (float)r.x + ((float)r.dx - u) / 2.f;
    float oy = (float)r.y + ((float)r.dy - u) / 2.f;
    auto at = [&](float x, float y) { return Gdiplus::PointF(ox + x * u, oy + y * u); };
    // One kite pointing up-right. No fold line: a second stroke turns to noise at 16px.
    Gdiplus::PointF pts[4] = {at(0.08f, 0.42f), at(0.92f, 0.08f), at(0.58f, 0.92f), at(0.42f, 0.50f)};
    g.DrawPolygon(&pen, pts, 4);
}

// Same look as the selection toolbar: text only, rounded hover fill.
// outlined adds a field-style frame for a button that sits next to a text field.
static void DrawPopupButton(HDC hdc, HWND hwnd, const Rect& r, const char* label, bool hot, bool enabled, bool outlined,
                            COLORREF textOverride = kColorUnset) {
    if (r.IsEmpty()) {
        return;
    }
    COLORREF bg = FloatingPopupBg();
    int radius = Px(hwnd, kBtnRadius);
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
        bool sameProvider = gGlobalPrefs && str::Eq(gAskModelProvider, gGlobalPrefs->aiTocApiBaseUrl);
        const char* model =
            gAskModel && sameProvider ? gAskModel : (gGlobalPrefs ? gGlobalPrefs->aiTocApiModel : nullptr);
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

static void PaintAskSidebar(InlineTranslatePopup* p, HDC hdcWnd) {
    HWND h = p->hwnd;
    Rect rc = ClientRect(h);
    DoubleBuffer buffer(h, rc);
    HDC hdc = buffer.GetDC();
    COLORREF bg = ThemeSidebarBackgroundColor();
    COLORREF txt = FloatingPopupTextColor();
    COLORREF muted = FloatingPopupMutedTextColor();
    RECT full = ToRECT(rc);
    ScopedGdiObj<HBRUSH> bgBrush(CreateSolidBrush(bg));
    FillRect(hdc, &full, bgBrush);
    SetBkMode(hdc, TRANSPARENT);

    HFONT old = (HFONT)SelectObject(hdc, p->titleFont ? p->titleFont : p->font);
    RECT titleRc{p->headerRc.x, p->headerRc.y, p->headerRc.x + p->headerRc.dx, p->headerRc.y + p->headerRc.dy};
    SetTextColor(hdc, txt);
    const char* title = _TRA("Ask AI");
    DrawTextW(hdc, ToWStrTemp(title), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT measured = titleRc;
    DrawTextW(hdc, ToWStrTemp(title), -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, p->font);
    TempStr model = HeaderSubtitleTemp(p);
    p->modelRc = Rect();
    if (!str::IsEmptyOrWhiteSpace(model)) {
        RECT modelRc = titleRc;
        modelRc.left = measured.right + Px(h, 12);
        if (modelRc.left < modelRc.right) {
            int modelWidth = HwndMeasureText(h, model, p->font).dx + Px(h, 22);
            modelRc.left = std::max(modelRc.left, modelRc.right - modelWidth);
            p->modelRc = Rect(modelRc.left, modelRc.top, RectDx(modelRc), RectDy(modelRc));
            modelRc.right -= Px(h, 18);
            SetTextColor(hdc, p->hot == PopupHot::Model ? txt : muted);
            DrawTextW(hdc, ToWStrTemp(model), -1, &modelRc,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            RECT arrow = ToRECT(p->modelRc);
            arrow.left = arrow.right - Px(h, 16);
            DrawTextW(hdc, L"⌄", -1, &arrow, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    if (p->turns.Size() == 0 && !p->asking && !p->chatRc.IsEmpty()) {
        RECT hint = ToRECT(p->chatRc);
        hint.left += Px(h, 12);
        hint.right -= Px(h, 12);
        hint.top += Px(h, 8);
        SelectObject(hdc, p->font);
        SetTextColor(hdc, muted);
        const char* empty = _TRA("Select text in the document and choose Ask AI, or ask a question below.");
        DrawTextW(hdc, ToWStrTemp(empty), -1, &hint, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    SelectClipRgn(hdc, nullptr);

    if (!p->inputRc.IsEmpty()) {
        // One field on the sidebar. The idle edge is only a step above the page;
        // focus strengthens that edge. It is not a link-blue Windows text box.
        bool focus = GetFocus() == p->hwndInput;
        COLORREF fill = AskComposerFill();
        int radius = Px(h, kFieldRadius);
        FillFloatingPopupRoundedRect(hdc, p->inputRc, radius, fill);
        StrokeFloatingPopupRoundedRect(hdc, p->inputRc, radius, AskComposerBorder(focus));
        COLORREF sendHover = AccentColor(fill, ThemeUsesDarkChrome() ? 10 : 8);
        bool sendOn = p->inputHasText && !p->asking;
        DrawComposerSend(hdc, h, p->sendRc, p->hot == PopupHot::Send, p->sendDown && sendOn, sendOn, sendHover);
    }
    if (!p->hintRc.IsEmpty()) {
        RECT r = ToRECT(p->hintRc);
        bool hot = p->hot == PopupHot::Hint;
        SelectObject(hdc, p->font);
        SetTextColor(hdc, hot ? txt : muted);
        DrawTextW(hdc, ToWStrTemp(HintText(p)), -1, &r, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    }

    SelectObject(hdc, old);
    buffer.Flush(hdcWnd);
}

static bool PopupIconPressed(InlineTranslatePopup* p, PopupHot which) {
    return p && p->hot == which && (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
}

static void DrawPopupIconHover(HDC hdc, const Rect& r, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPageUnit(Gdiplus::UnitPixel);
    Gdiplus::Color c;
    c.SetFromCOLORREF(col);
    Gdiplus::SolidBrush b(c);
    g.FillEllipse(&b, r.x, r.y, r.dx - 2, r.dy - 2);
}

static void AddRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius) {
    float d = std::min(radius * 2.f, std::min(w, h));
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.AddArc(x, y, d, d, 180, 90);
    path.CloseFigure();
}

static void DrawCopyGlyph(HDC hdc, const Rect& r, COLORREF col, COLORREF cover) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::Color c;
    c.SetFromCOLORREF(col);
    float side = (float)std::max(1, std::min(r.dx, r.dy));
    // ChatGPT copy: two equal rounded sheets. The rear one sits up-right.
    float penW = std::max(1.f, side * 1.25f / 16.f);
    float sheet = side * 0.50f;
    float shift = side * 0.20f;
    float margin = (side - sheet - shift) * 0.5f;
    float rad = sheet * 0.22f;
    Gdiplus::Pen pen(c, penW);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    float backX = (float)r.x + margin + shift;
    float backY = (float)r.y + margin;
    float frontX = (float)r.x + margin;
    float frontY = (float)r.y + margin + shift;
    Gdiplus::GraphicsPath back;
    AddRoundRect(back, backX, backY, sheet, sheet, rad);
    g.DrawPath(&pen, &back);
    Gdiplus::GraphicsPath front;
    AddRoundRect(front, frontX, frontY, sheet, sheet, rad);
    Gdiplus::Color coverCol;
    coverCol.SetFromCOLORREF(cover);
    Gdiplus::SolidBrush coverBr(coverCol);
    g.FillPath(&coverBr, &front);
    g.DrawPath(&pen, &front);
}

static void DrawCheckGlyph(HDC hdc, const Rect& r, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::Color c;
    c.SetFromCOLORREF(col);
    float side = (float)std::max(1, std::min(r.dx, r.dy));
    float penW = std::max(1.f, side * 1.35f / 16.f);
    Gdiplus::Pen pen(c, penW);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::PointF pts[3] = {
        {r.x + side * 0.20f, r.y + side * 0.54f},
        {r.x + side * 0.40f, r.y + side * 0.74f},
        {r.x + side * 0.80f, r.y + side * 0.28f},
    };
    g.DrawLines(&pen, pts, 3);
}

static void PaintPopup(InlineTranslatePopup* p, HDC hdcWnd) {
    if (p->docked) {
        PaintAskSidebar(p, hdcWnd);
        return;
    }
    HWND h = p->hwnd;
    Rect rc = ClientRect(h);
    DoubleBuffer buffer(h, rc);
    HDC hdc = buffer.GetDC();

    COLORREF bg = p->docked ? ThemeSidebarBackgroundColor() : FloatingPopupBg();
    COLORREF txt = FloatingPopupTextColor();
    COLORREF muted = FloatingPopupMutedTextColor();
    RECT full = ToRECT(rc);
    ScopedGdiObj<HBRUSH> bgBrush(CreateSolidBrush(bg));
    FillRect(hdc, &full, bgBrush);
    if (!p->docked) {
        int radius = Px(h, kFloatingPopupCornerRadius);
        FillFloatingPopupRoundedRect(hdc, rc, radius, bg);
        StrokeFloatingPopupRoundedRect(hdc, rc, radius, FloatingPopupBorderColor());
    }

    SetBkMode(hdc, TRANSPARENT);
    HFONT old = (HFONT)SelectObject(hdc, p->titleFont ? p->titleFont : p->font);

    bool minimalTrans = p->mode == PopupMode::Translate && !p->docked;
    if (minimalTrans) {
        SelectObject(hdc, p->titleFont ? p->titleFont : p->font);
        int rightLimit = rc.dx - Px(h, 8);
        if (!p->copyRc.IsEmpty()) {
            rightLimit = std::min(rightLimit, p->copyRc.x - Px(h, 6));
        }
        if (!p->closeRc.IsEmpty()) {
            rightLimit = std::min(rightLimit, p->closeRc.x - Px(h, 6));
        }
        RECT titleRc{Px(h, 12), p->headerRc.y, rightLimit, p->headerRc.y + p->headerRc.dy};
        SetTextColor(hdc, txt);
        const char* title = _TRA("Translate");
        DrawTextW(hdc, ToWStrTemp(title), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT measured = titleRc;
        DrawTextW(hdc, ToWStrTemp(title), -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, p->font);
        TempStr lang = TranslateLangLabelTemp(p);
        if (!str::IsEmptyOrWhiteSpace(lang)) {
            RECT langRc = titleRc;
            langRc.left = measured.right + Px(h, 10);
            if (langRc.left < langRc.right) {
                SetTextColor(hdc, muted);
                DrawTextW(hdc, ToWStrTemp(lang), -1, &langRc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
        int ruleY = p->headerRc.y + p->headerRc.dy;
        {
            Gdiplus::Graphics rule(hdc);
            Gdiplus::Color ruleCol;
            ruleCol.SetFromCOLORREF(FloatingPopupSeparatorColor());
            Gdiplus::Pen rulePen(ruleCol, 1.f);
            float x0 = (float)Px(h, 12);
            float x1 = (float)std::max(Px(h, 12), rc.dx - Px(h, 12));
            rule.DrawLine(&rulePen, x0, (float)ruleY + 0.5f, x1, (float)ruleY + 0.5f);
        }
        if (!p->closeRc.IsEmpty()) {
            bool closeHot = p->hot == PopupHot::Close || PopupIconPressed(p, PopupHot::Close);
            DrawCloseButtonArgs cb;
            cb.hdc = hdc;
            cb.r = p->closeRc;
            cb.isHover = closeHot;
            cb.colX = muted;
            cb.colXHover = txt;
            cb.colHoverBg = FloatingPopupCloseHoverBg(bg);
            DrawCloseButton(cb);
        }
        if (!p->copyRc.IsEmpty()) {
            bool copyHot = p->hot == PopupHot::Copy || PopupIconPressed(p, PopupHot::Copy);
            COLORREF mark = (copyHot || p->copied) ? txt : muted;
            if (copyHot) {
                DrawPopupIconHover(hdc, p->copyRc, FloatingPopupCloseHoverBg(bg));
            }
            // The overlapping sheets occupy only part of the glyph box, so use
            // a larger box to match the close mark visually.
            int glyph = Px(h, 14);
            int insetX = std::max(0, (p->copyRc.dx - glyph) / 2);
            int insetY = std::max(0, (p->copyRc.dy - glyph) / 2);
            Rect ink(p->copyRc.x + insetX, p->copyRc.y + insetY, glyph, glyph);
            if (p->copied) {
                DrawCheckGlyph(hdc, ink, mark);
            } else {
                DrawCopyGlyph(hdc, ink, mark, copyHot ? FloatingPopupCloseHoverBg(bg) : bg);
            }
        }
        SelectObject(hdc, old);
        buffer.Flush(hdcWnd);
        return;
    }

    if (p->docked && p->turns.Size() == 0 && !p->asking) {
        RECT hint = ToRECT(rc);
        hint.left += Px(h, 16);
        hint.right -= Px(h, 16);
        hint.top = p->headerRc.y + p->headerRc.dy + Px(h, 18);
        hint.bottom = p->inputRc.IsEmpty() ? rc.y + rc.dy - Px(h, 16) : p->inputRc.y - Px(h, 12);
        SelectObject(hdc, p->font);
        SetTextColor(hdc, muted);
        const char* empty = _TRA("Select text in the document and choose Ask AI, or ask a question below.");
        DrawTextW(hdc, ToWStrTemp(empty), -1, &hint, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(hdc, p->titleFont ? p->titleFont : p->font);
    }

    // header: title, subtitle, copy, close
    const char* title = p->mode == PopupMode::AskAi ? _TRA("Ask AI") : _TRA("Translate");
    int rightLimit = rc.x + rc.dx - Px(h, 8);
    if (!p->copyRc.IsEmpty()) {
        rightLimit = p->copyRc.x - Px(h, 8);
    } else if (!p->closeRc.IsEmpty()) {
        rightLimit = p->closeRc.x - Px(h, 8);
    }
    RECT titleRc{p->headerRc.x, p->headerRc.y, rightLimit, p->headerRc.y + p->headerRc.dy};
    SetTextColor(hdc, txt);
    DrawTextW(hdc, ToWStrTemp(title), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT measured = titleRc;
    DrawTextW(hdc, ToWStrTemp(title), -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, p->font);
    TempStr subtitle = HeaderSubtitleTemp(p);
    if (!str::IsEmptyOrWhiteSpace(subtitle)) {
        RECT subRc = titleRc;
        subRc.left = measured.right + Px(h, 10);
        if (subRc.left < subRc.right) {
            SetTextColor(hdc, muted);
            DrawTextW(hdc, ToWStrTemp(subtitle), -1, &subRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    DrawPopupButton(hdc, h, p->copyRc, CopyLabel(p), p->hot == PopupHot::Copy, true, false,
                    p->copied ? FloatingPopupAccentColor() : kColorUnset);

    if (!p->closeRc.IsEmpty()) {
        bool closeHot = p->hot == PopupHot::Close;
        if (closeHot) {
            FillFloatingPopupRoundedRect(hdc, p->closeRc, Px(h, 6), FloatingPopupCloseHoverBg(bg));
        }
        int glyph = Px(h, kCloseGlyph);
        Rect closeGlyph(p->closeRc.x + (p->closeRc.dx - glyph) / 2, p->closeRc.y + (p->closeRc.dy - glyph) / 2, glyph,
                        glyph);
        DrawCloseButtonArgs cb;
        cb.hdc = hdc;
        cb.r = closeGlyph;
        cb.isHover = false;
        cb.colX = closeHot ? txt : muted;
        cb.colXHover = txt;
        DrawCloseButton(cb);
    }

    // original text preview. Ask AI is one muted line of context; translate stays two lines.
    if (!p->origRc.IsEmpty()) {
        RECT r = ToRECT(p->origRc);
        SetTextColor(hdc, muted);
        UINT fmt = DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (p->mode == PopupMode::AskAi) {
            fmt |= DT_SINGLELINE | DT_VCENTER;
        } else {
            fmt |= DT_WORDBREAK | DT_EDITCONTROL;
        }
        const char* preview = p->mode == PopupMode::AskAi ? AskContextLineTemp(p->selection) : p->selection;
        DrawTextW(hdc, ToWStrTemp(preview), -1, &r, fmt);
    }

    HWND focus = GetFocus();
    DrawFieldBox(hdc, h, p->transRc, false);
    if (p->mode != PopupMode::AskAi) {
        DrawFieldBox(hdc, h, p->chatRc, false);
    }
    DrawFieldBox(hdc, h, p->inputRc, focus && focus == p->hwndInput, kComposerRadius);
    bool sendEnabled = p->inputHasText && !p->asking;
    DrawComposerSend(hdc, h, p->sendRc, p->hot == PopupHot::Send, p->sendDown && sendEnabled, sendEnabled);

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
            // ScrollBar only. The full Explorer theme loads text services and freezes.
            // Gating this on the experimental flag left a classic white track on a dark page.
            const WCHAR* scroll = ThemeUsesDarkChrome() ? L"DarkMode_Explorer::ScrollBar" : L"Explorer::ScrollBar";
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

static void StripEditFrame(HWND hwnd);

static void ApplyPopupTheme(InlineTranslatePopup* p) {
    if (!p || !p->hwnd) {
        return;
    }
    if (p->fieldBrush) {
        DeleteObject(p->fieldBrush);
    }
    if (p->chatBrush) {
        DeleteObject(p->chatBrush);
    }
    p->fieldBrush = CreateSolidBrush(FloatingPopupFieldBg());
    p->chatBrush = CreateSolidBrush(ChatContentBg(p));
    ApplyEditTheme(p->hwndTrans, false);
    if (!p->docked && p->hwndTrans) {
        StripEditFrame(p->hwndTrans);
        if (DynSetWindowTheme) {
            DynSetWindowTheme(p->hwndTrans, L"", L"");
        }
        if (UseDarkModeLib() && ThemeUsesDarkChrome()) {
            DarkMode::setDarkScrollBar(p->hwndTrans);
        }
    }
    ApplyEditTheme(p->hwndChat, p->chatIsRich);
    ApplyEditTheme(p->hwndInput, false);
    if (p->docked && p->hwndInput && DynSetWindowTheme) {
        // The composer draws its own border. The themed edit frame would be a second, brighter box.
        DynSetWindowTheme(p->hwndInput, L"", L"");
    }
    if (p->chatIsRich) {
        // colors live in the RTF color table
        RebuildChatView(p);
        if (IsWindowVisible(p->hwnd) || p->deferPaint) {
            LayoutPopup(p);
        }
    }
    if (p->docked) {
        SetWindowRgn(p->hwnd, nullptr, FALSE);
    } else {
        UpdateFloatingPopupWindowRgn(p->hwnd, kFloatingPopupCornerRadius);
    }
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
    if (gPopup) {
        bool live = gPopup->hwnd && (GetWindowLongPtrW(gPopup->hwnd, GWL_STYLE) & WS_VISIBLE);
        if (live) {
            FreezePopupRedraw(gPopup);
        }
        ApplyPopupTheme(gPopup);
    }
    if (gAsk) {
        ApplyPopupTheme(gAsk);
    }
}

void FinishInlineTranslatePopupTheme() {
    if (gThemeFreezeCount == 0) {
        if (gPopup) {
            gPopup->deferPaint = false;
        }
        if (gAsk) {
            gAsk->deferPaint = false;
        }
        return;
    }
    ThawPopupRedraw(gPopup);
}

static void ApplyPopupFonts(InlineTranslatePopup* p, int dpi);
static void StripEditFrame(HWND hwnd);

static Rect SelectionAnchorScreen(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    if (!dm || !tab->selectionOnPage || !win->hwndCanvas) {
        return Rect();
    }
    Rect u;
    bool any = false;
    for (SelectionOnPage& s : *tab->selectionOnPage) {
        Rect r = s.GetRect(dm);
        if (r.IsEmpty()) {
            continue;
        }
        POINT tl{r.x, r.y};
        POINT br{r.x + r.dx, r.y + r.dy};
        ClientToScreen(win->hwndCanvas, &tl);
        ClientToScreen(win->hwndCanvas, &br);
        Rect sr = Rect::FromXY((int)tl.x, (int)tl.y, (int)br.x, (int)br.y);
        if (!any) {
            u = sr;
            any = true;
        } else {
            u = Rect::FromXY(std::min(u.x, sr.x), std::min(u.y, sr.y), std::max(u.Right(), sr.Right()),
                             std::max(u.Bottom(), sr.Bottom()));
        }
    }
    return u;
}

static void PositionPopupNear(InlineTranslatePopup* p, Rect anchor, Point fallback) {
    HWND h = p->hwnd;
    bool has = !anchor.IsEmpty();
    int ax = has ? anchor.x + anchor.dx / 2 : fallback.x;
    int ay = has ? anchor.y + anchor.dy / 2 : fallback.y;
    HMONITOR mon = MonitorFromPoint({ax, ay}, MONITOR_DEFAULTTONEAREST);
    int dpi = DpiGetForMonitor(mon);
    if (dpi < 72) {
        dpi = DpiGet(h);
    }
    dpi = RoundUp(dpi, 4);
    ApplyPopupFonts(p, dpi);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    Rect work = ToRect(mi.rcWork);
    int prefW = p->mode == PopupMode::Translate ? 380 : kPopupMaxW;
    int w = std::min(Px(h, prefW), std::max(1, work.dx - Px(h, 16)));
    if (p->mode == PopupMode::Translate) {
        int minW = std::min(Px(h, 320), w);
        w = std::max(w, minW);
    } else {
        w = std::max(w, std::min(Px(h, kPopupMinW), work.dx));
    }
    int hgt = p->mode == PopupMode::Translate ? Px(h, 88) : Px(h, kPopupMinH);
    int gap = Px(h, 8);
    int expected = p->mode == PopupMode::Translate ? std::min(work.dy * 2 / 5, Px(h, 220)) : Px(h, 320);
    int x = 0;
    int y = 0;
    if (has) {
        int spaceAbove = anchor.y - work.y;
        p->placedAbove = spaceAbove >= std::min(expected, Px(h, 120)) + gap;
        x = anchor.x;
        y = p->placedAbove ? anchor.y - gap - hgt : anchor.y + anchor.dy + gap;
    } else {
        x = fallback.x - w / 2;
        y = fallback.y + gap;
        p->placedAbove = y + expected > work.y + work.dy && fallback.y - gap - expected >= work.y;
        if (p->placedAbove) {
            y = fallback.y - gap - hgt;
        }
    }
    x = limitValue(x, work.x, std::max(work.x, work.x + work.dx - w));
    y = limitValue(y, work.y, std::max(work.y, work.y + work.dy - hgt));
    SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateFloatingPopupWindowRgn(h, kFloatingPopupCornerRadius);
}

static void OnSendClicked(InlineTranslatePopup* p);
static void SyncInputHasText(InlineTranslatePopup* p);
static void PaintQuietScrollbar(HWND hwnd, COLORREF trackCol, bool hot, bool dragging);

// Enter confirms an IME composition. It must not also send the question.
static bool InputImeComposing(HWND hwnd) {
    using GetCtxFn = void*(WINAPI*)(HWND);
    using ReleaseFn = BOOL(WINAPI*)(HWND, void*);
    using GetStrFn = LONG(WINAPI*)(void*, DWORD, LPVOID, DWORD);
    static GetCtxFn getCtx = nullptr;
    static ReleaseFn releaseCtx = nullptr;
    static GetStrFn getStr = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE imm = LoadLibraryW(L"imm32.dll");
        if (imm) {
            getCtx = (GetCtxFn)GetProcAddress(imm, "ImmGetContext");
            releaseCtx = (ReleaseFn)GetProcAddress(imm, "ImmReleaseContext");
            getStr = (GetStrFn)GetProcAddress(imm, "ImmGetCompositionStringW");
        }
    }
    if (!getCtx || !releaseCtx || !getStr) {
        return false;
    }
    void* himc = getCtx(hwnd);
    if (!himc) {
        return false;
    }
    // GCS_COMPSTR
    LONG n = getStr(himc, 0x0008, nullptr, 0);
    releaseCtx(hwnd, himc);
    return n > 0;
}

static LRESULT CALLBACK InputSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* p = (InlineTranslatePopup*)data;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_RETURN && !InputImeComposing(hwnd)) {
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (p->docked && shift) {
                    SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)L"\r\n");
                    return 0;
                }
                // Enter sends. Shift+Enter is the newline, above.
                if (p->docked || p == gPopup) {
                    OnSendClicked(p);
                }
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (p->docked) {
                    if (p->win && IsMainWindowValid(p->win)) {
                        HwndSetFocus(p->win->hwndCanvas ? p->win->hwndCanvas : p->win->hwndFrame);
                    }
                    return 0;
                }
                PostClosePopup();
                return 0;
            }
            break;
        case WM_CHAR:
            // swallow Enter / Esc so the edit doesn't beep. A composing IME still needs Enter.
            if (wp == 27 || (wp == '\r' && !InputImeComposing(hwnd))) {
                return 0;
            }
            break;
        case WM_PAINT:
            if (p && p->docked && GetWindowTextLengthW(hwnd) == 0) {
                LRESULT painted = DefSubclassProc(hwnd, msg, wp, lp);
                HDC hdc = GetDC(hwnd);
                if (hdc) {
                    RECT fr{};
                    SendMessageW(hwnd, EM_GETRECT, 0, (LPARAM)&fr);
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, FloatingPopupMutedTextColor());
                    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
                    HFONT old = font ? (HFONT)SelectObject(hdc, font) : nullptr;
                    DrawTextW(hdc, ToWStrTemp(AskCueText(p)), -1, &fr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    if (old) {
                        SelectObject(hdc, old);
                    }
                    ReleaseDC(hwnd, hdc);
                }
                return painted;
            }
            break;
        case WM_NCPAINT:
            if (p && p->docked) {
                LRESULT painted = DefSubclassProc(hwnd, msg, wp, lp);
                PaintQuietScrollbar(hwnd, AskComposerFill(), p->composerScrollHot, p->composerScrollDrag);
                return painted;
            }
            break;
        case WM_NCMOUSEMOVE:
            if (p && p->docked && wp == HTVSCROLL) {
                p->composerScrollHot = true;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                TrackMouseEvent(&tme);
                PaintQuietScrollbar(hwnd, AskComposerFill(), true, p->composerScrollDrag);
            }
            break;
        case WM_NCMOUSELEAVE:
            if (p && p->docked && p->composerScrollHot) {
                p->composerScrollHot = false;
                PaintQuietScrollbar(hwnd, AskComposerFill(), false, p->composerScrollDrag);
            }
            break;
        case WM_NCLBUTTONDOWN:
            if (p && p->docked && wp == HTVSCROLL) {
                p->composerScrollDrag = true;
            }
            break;
        case WM_NCLBUTTONUP:
            if (p && p->composerScrollDrag) {
                p->composerScrollDrag = false;
                if (p->docked) {
                    PaintQuietScrollbar(hwnd, AskComposerFill(), p->composerScrollHot, false);
                }
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(GetParent(hwnd), nullptr, FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);
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
static HFONT CreateReadingFont(HFONT base, int dpi) {
    LOGFONTW lf{};
    if (!base || !GetObjectW(base, sizeof(lf), &lf)) {
        return nullptr;
    }
    if (dpi < 72) {
        dpi = 96;
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

static void ApplyPopupFonts(InlineTranslatePopup* p, int dpi) {
    if (!p || !p->hwnd || dpi < 72 || (p->dpi == dpi && p->font && p->bodyFont)) {
        return;
    }
    p->dpi = dpi;
    if (p->titleFont) {
        DeleteObject(p->titleFont);
        p->titleFont = nullptr;
    }
    if (p->bodyFont) {
        DeleteObject(p->bodyFont);
        p->bodyFont = nullptr;
    }
    p->font = GetAppFontForDpi(dpi);
    p->titleFont = CreateTitleFont(p->font);
    p->bodyFont = CreateReadingFont(p->font, dpi);
    HFONT reading = p->bodyFont ? p->bodyFont : p->font;
    if (p->hwndTrans) {
        SendMessageW(p->hwndTrans, WM_SETFONT, (WPARAM)reading, FALSE);
    }
    if (p->hwndChat) {
        SendMessageW(p->hwndChat, WM_SETFONT, (WPARAM)reading, FALSE);
    }
    if (p->hwndInput) {
        SendMessageW(p->hwndInput, WM_SETFONT, (WPARAM)p->font, FALSE);
    }
    if (p->chatIsRich && (p->turns.Size() > 0 || p->asking)) {
        RebuildChatView(p);
    }
}

static void PaintAwayEditFrame(HWND hwnd, COLORREF bg) {
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) {
        return;
    }
    RECT cr{};
    GetClientRect(hwnd, &cr);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    int left = origin.x - wr.left;
    int top = origin.y - wr.top;
    int ww = wr.right - wr.left;
    int wh = wr.bottom - wr.top;
    int right = wr.right - (origin.x + cr.right);
    int bottom = wr.bottom - (origin.y + cr.bottom);
    if (left <= 0 && top <= 0 && bottom <= 0) {
        return;
    }
    HDC hdc = GetWindowDC(hwnd);
    if (!hdc) {
        return;
    }
    HBRUSH br = CreateSolidBrush(bg);
    if (top > 0) {
        RECT r{0, 0, ww, top};
        FillRect(hdc, &r, br);
    }
    if (left > 0) {
        RECT r{0, top > 0 ? top : 0, left, wh - (bottom > 0 ? bottom : 0)};
        FillRect(hdc, &r, br);
    }
    if (bottom > 0) {
        RECT r{0, wh - bottom, ww - (right > 0 ? right : 0), wh};
        FillRect(hdc, &r, br);
    }
    DeleteObject(br);
    ReleaseDC(hwnd, hdc);
}

static void StripEditFrame(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    style &= ~(WS_BORDER | WS_DLGFRAME);
    ex &= ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    SetWindowLongW(hwnd, GWL_STYLE, style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// Cover the system track with the sidebar color, then a short thumb.
// DarkMode_Explorer still paints a light gutter; that reads as a white frame.
static void PaintQuietScrollbar(HWND hwnd, COLORREF trackCol, bool hot, bool dragging) {
    SCROLLBARINFO sbi{};
    sbi.cbSize = sizeof(sbi);
    if (!GetScrollBarInfo(hwnd, OBJID_VSCROLL, &sbi)) {
        return;
    }
    if (sbi.rgstate[0] & STATE_SYSTEM_INVISIBLE) {
        return;
    }
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) {
        return;
    }
    RECT track = sbi.rcScrollBar;
    OffsetRect(&track, -wr.left, -wr.top);
    if (track.right <= track.left || track.bottom <= track.top) {
        return;
    }
    int thumbTop = sbi.xyThumbTop;
    int thumbBot = sbi.xyThumbBottom;
    if (thumbBot > sbi.rcScrollBar.bottom + 4 || thumbTop < sbi.rcScrollBar.top - 4) {
        thumbTop = sbi.rcScrollBar.top + sbi.xyThumbTop;
        thumbBot = sbi.rcScrollBar.top + sbi.xyThumbBottom;
    }
    thumbTop -= wr.top;
    thumbBot -= wr.top;

    HDC hdc = GetWindowDC(hwnd);
    if (!hdc) {
        return;
    }
    HBRUSH bgBr = CreateSolidBrush(trackCol);
    FillRect(hdc, &track, bgBr);
    DeleteObject(bgBr);
    if (thumbBot - thumbTop >= 8 && thumbTop >= track.top - 2 && thumbBot <= track.bottom + 2) {
        int inset = std::max(2, (int)(track.right - track.left) / 4);
        RECT thumb{track.left + inset, thumbTop + 1, track.right - inset, thumbBot - 1};
        if (thumb.bottom > thumb.top + 4) {
            int lift = dragging ? 32 : (hot ? 22 : 14);
            if (!ThemeUsesDarkChrome()) {
                lift = dragging ? 24 : (hot ? 16 : 10);
            }
            HBRUSH br = CreateSolidBrush(AccentColor(trackCol, lift));
            FillRect(hdc, &thumb, br);
            DeleteObject(br);
        }
    }
    ReleaseDC(hwnd, hdc);
}

static void NoteChatUserScroll(InlineTranslatePopup* p, HWND hwnd) {
    if (!p || !p->docked || p->chatAdjustingScroll) {
        return;
    }
    p->chatFollowTail = ChatIsNearBottom(hwnd);
}

static LRESULT CALLBACK ChatSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* p = (InlineTranslatePopup*)data;
    if (msg == WM_NCPAINT && p && p->docked) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        PaintAwayEditFrame(hwnd, ThemeSidebarBackgroundColor());
        PaintQuietScrollbar(hwnd, ThemeSidebarBackgroundColor(), p->chatScrollHot, p->chatScrollDrag);
        return r;
    }
    if (p && p->docked && msg == WM_NCMOUSEMOVE && wp == HTVSCROLL) {
        if (!p->chatScrollHot) {
            p->chatScrollHot = true;
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
        TrackMouseEvent(&tme);
        PaintQuietScrollbar(hwnd, ThemeSidebarBackgroundColor(), true, p->chatScrollDrag);
    } else if (p && p->docked && msg == WM_NCMOUSELEAVE) {
        p->chatScrollHot = false;
        p->chatScrollDrag = false;
        PaintQuietScrollbar(hwnd, ThemeSidebarBackgroundColor(), false, false);
    } else if (p && p->docked && msg == WM_NCLBUTTONDOWN && wp == HTVSCROLL) {
        p->chatScrollDrag = true;
    } else if (p && p->docked && (msg == WM_NCLBUTTONUP || msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED)) {
        if (p->chatScrollDrag) {
            p->chatScrollDrag = false;
            PaintQuietScrollbar(hwnd, ThemeSidebarBackgroundColor(), p->chatScrollHot, false);
        }
    }
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || (msg == WM_KEYDOWN && (wp == VK_PRIOR || wp == VK_NEXT))) {
        NoteChatUserScroll(p, hwnd);
        if (p && p->docked) {
            PaintQuietScrollbar(hwnd, ThemeSidebarBackgroundColor(), p->chatScrollHot, p->chatScrollDrag);
        }
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ChatSubclassProc, id);
    }
    return r;
}

// The translation is read-only output. Keep the edit for selection, but do not
// show a caret or an I-beam that implies the text can be edited.
static LRESULT CALLBACK TransSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    }
    if (msg == WM_SETFOCUS || msg == WM_PAINT) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        HideCaret(hwnd);
        return r;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static InlineTranslatePopup* CreatePopup(MainWindow* win, bool docked) {
    RegisterPopupClass();
    auto* p = new InlineTranslatePopup();
    p->win = win;
    p->docked = docked;
    p->mode = docked ? PopupMode::AskAi : PopupMode::Translate;
    int dpi = DpiGet(win->hwndFrame);
    p->dpi = dpi;
    p->font = GetAppFontForDpi(dpi);
    p->titleFont = CreateTitleFont(p->font);
    p->bodyFont = CreateReadingFont(p->font, dpi);
    // Floating: owned by the frame, stays above it, hides with it.
    // Docked: a child of the sidebar column, the AI page.
    DWORD ex = docked ? 0 : WS_EX_TOOLWINDOW;
    DWORD style = docked ? (WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS) : (WS_POPUP | WS_CLIPCHILDREN);
    HWND parent = docked ? win->hwndTocBox : win->hwndFrame;
    p->hwnd = CreateWindowExW(ex, kInlineTranslateClassName, L"", style, 0, 0, 0, 0, parent, nullptr,
                              GetModuleHandle(nullptr), p);
    if (!p->hwnd) {
        FreePopupData(p);
        delete p;
        return nullptr;
    }
    HINSTANCE inst = GetModuleHandle(nullptr);
    DWORD roStyle = WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL;
    p->hwndTrans = CreateWindowExW(0, WC_EDITW, L"", roStyle, 0, 0, 0, 0, p->hwnd, nullptr, inst, nullptr);
    if (!docked && p->hwndTrans) {
        SetWindowSubclass(p->hwndTrans, TransSubclassProc, kTransSubclassId, (DWORD_PTR)p);
        p->hwndTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, p->hwnd, nullptr, inst,
                                     nullptr);
        if (p->hwndTip) {
            int maxTip = Px(p->hwnd, 220);
            SendMessageW(p->hwndTip, TTM_SETMAXTIPWIDTH, 0, maxTip);
        }
    }
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
    // Docked composer is multiline: Enter sends, Shift+Enter breaks a line, and the
    // field grows. The floating popup stays a single line.
    DWORD inputStyle = WS_CHILD | WS_TABSTOP | (docked ? (ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL) : ES_AUTOHSCROLL);
    p->hwndInput = CreateWindowExW(0, WC_EDITW, L"", inputStyle, 0, 0, 0, 0, p->hwnd, nullptr, inst, nullptr);
    HFONT reading = p->bodyFont ? p->bodyFont : p->font;
    SendMessageW(p->hwndTrans, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SendMessageW(p->hwndTrans, WM_SETFONT, (WPARAM)reading, FALSE);
    SendMessageW(p->hwndChat, WM_SETFONT, (WPARAM)reading, FALSE);
    SendMessageW(p->hwndInput, WM_SETFONT, (WPARAM)p->font, FALSE);
    const char* cue = docked ? _TRA("Ask a follow-up...") : _TRA("Ask about the selected text…");
    SendMessageW(p->hwndInput, EM_SETCUEBANNER, TRUE, (LPARAM)ToWStrTemp(cue));
    SendMessageW(p->hwndInput, EM_SETLIMITTEXT, 2000, 0);
    SetWindowSubclass(p->hwndInput, InputSubclassProc, kInputSubclassId, (DWORD_PTR)p);
    if (docked) {
        StripEditFrame(p->hwndChat);
        StripEditFrame(p->hwndInput);
        ShowScrollBar(p->hwndInput, SB_VERT, FALSE);
        int margin = Px(p->hwnd, 2);
        SendMessageW(p->hwndInput, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(margin, margin));
        SetWindowSubclass(p->hwndChat, ChatSubclassProc, kChatSubclassId, (DWORD_PTR)p);
        if (DynSetWindowTheme && p->hwndInput) {
            DynSetWindowTheme(p->hwndInput, L"", L"");
        }
        p->userSized = true;
        p->generation = ++gNextPopupGeneration;
        win->hwndAiSidebar = p->hwnd;
    }
    ApplyPopupTheme(p);
    return p;
}

// Reuses the open popup for the same window; a new session always starts clean.
static InlineTranslatePopup* BeginPopupSession(MainWindow* win, const char* selection, PopupMode mode) {
    if (gPopup && gPopup->win != win) {
        CloseInlineTranslatePopup();
    }
    if (!gPopup) {
        gPopup = CreatePopup(win, false);
        if (!gPopup) {
            return nullptr;
        }
    }
    InlineTranslatePopup* p = gPopup;
    WindowTab* tab = win->CurrentTab();
    p->sourceSelection = tab ? tab->selectionOnPage : nullptr;
    // global so a late result from a closed popup can't match a new one
    p->generation = ++gNextPopupGeneration;
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
    SyncInputHasText(p);

    POINT cur{};
    GetCursorPos(&cur);
    PositionPopupNear(p, SelectionAnchorScreen(win), Point{cur.x, cur.y});
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
    bool fallbackWeb = false;
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
    if (job->fallbackWeb) {
        MainWindow* win = p->win;
        char* text = str::Dup(job->text);
        char* target = str::Dup(job->target);
        CloseInlineTranslatePopup(false);
        SendSelectionToWebAi(win, text, target);
        str::Free(text);
        str::Free(target);
        return;
    }
    if (job->result) {
        str::ReplacePtr(&p->translation, job->result);
        job->result = nullptr;
        p->engine = job->engine;
    } else {
        TempStr err = str::FormatTemp("%s\n%s", _TRA("Translation failed."), job->error ? job->error : "");
        str::ReplaceWithCopy(&p->transError, err);
    }
    RefreshTranslationView(p);
    // Stay hidden until there is something to show. A missing API falls back
    // to the browser above and must not flash this window first.
    if (!IsWindowVisible(p->hwnd)) {
        ShowPopup(p);
    }
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
        } else if (WebAiTranslateEnabled() &&
                   ((!haveVolc && !AiTocApiIsConfigured(job->cfg)) || TranslateErrorMeansUnusableApi(job->error))) {
            job->fallbackWeb = true;
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
    WindowTab* tab = nullptr;
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

// Replace the chat and resize in one paint. Otherwise the answer is drawn into
// the small "Thinking…" window, then the window jumps to the new height.
static void PresentChatUpdate(InlineTranslatePopup* p) {
    if (!p || !p->hwnd) {
        return;
    }
    bool live = (GetWindowLongPtrW(p->hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
    if (live) {
        FreezePopupRedraw(p);
    }
    RebuildChatView(p);
    LayoutPopup(p);
    if (live) {
        ThawPopupRedraw(p);
    }
}

static void AskJobFinished(AskJob* job) {
    defer {
        job->FreeAll();
        delete job;
    };
    WindowTab* tab = job->tab;
    ChatTurn turn;
    turn.role = str::Dup("assistant");
    if (job->result) {
        turn.content = job->result;
        job->result = nullptr;
    } else {
        turn.content = str::Format("%s %s", _TRA("The AI request failed."), job->error ? job->error : "");
    }
    // The visible sidebar owns the turns. A parked tab keeps them on itself,
    // so a late reply cannot land on another document.
    if (gAsk && tab && gAsk->boundTab == tab && gAsk->generation == job->generation && IsWindowTabValid(tab)) {
        gAsk->asking = false;
        gAsk->turns.Append(turn);
        PresentChatUpdate(gAsk);
        return;
    }
    // Legacy floating Ask AI popup. The normal path does not open it.
    if (!tab && gPopup && gPopup->mode == PopupMode::AskAi && gPopup->generation == job->generation) {
        gPopup->asking = false;
        gPopup->turns.Append(turn);
        PresentChatUpdate(gPopup);
        return;
    }
    if (!tab || !IsWindowTabValid(tab)) {
        turn.Free();
        return;
    }
    AiChatState* st = (AiChatState*)tab->askAiState;
    if (!st || st->generation != job->generation) {
        turn.Free();
        return;
    }
    st->asking = false;
    st->turns.Append(turn);
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
    p->chatFollowTail = true;

    auto* job = new AskJob();
    job->generation = p->generation;
    job->tab = p->boundTab;
    job->cfg = SnapshotAiCfg();
    if (p->docked && gAskModel && str::Eq(gAskModelProvider, job->cfg.baseUrl)) {
        free((void*)job->cfg.model);
        job->cfg.model = str::Dup(gAskModel);
    }
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
    PresentChatUpdate(p);
}

// Grow the docked composer with the line count, up to five, then scroll inside.
static void SyncComposerLines(InlineTranslatePopup* p) {
    if (!p || !p->docked || !p->hwndInput || p->inLayout) {
        return;
    }
    int n = (int)SendMessageW(p->hwndInput, EM_GETLINECOUNT, 0, 0);
    if (n < 1) {
        n = 1;
    }
    ShowScrollBar(p->hwndInput, SB_VERT, n > 5);
    int shown = std::min(n, 5);
    if (shown == p->composerLines) {
        return;
    }
    p->composerLines = shown;
    LayoutPopup(p);
}

static void SyncInputHasText(InlineTranslatePopup* p) {
    if (!p || !p->hwndInput) {
        return;
    }
    char* q = GetEditTextUtf8(p->hwndInput);
    str::TrimWSInPlace(q, str::TrimOpt::Both);
    bool has = !str::IsEmptyOrWhiteSpace(q);
    str::Free(q);
    if (has == p->inputHasText) {
        return;
    }
    p->inputHasText = has;
    if (p->hwnd) {
        InvalidateRect(p->hwnd, nullptr, FALSE);
    }
}

static void OnSendClicked(InlineTranslatePopup* p) {
    if (!p || p->asking) {
        return;
    }
    char* q = GetEditTextUtf8(p->hwndInput);
    str::TrimWSInPlace(q, str::TrimOpt::Both);
    if (!str::IsEmptyOrWhiteSpace(q)) {
        SetEditTextUtf8(p->hwndInput, "");
        SyncInputHasText(p);
        SyncComposerLines(p);
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

struct AskModelsRequest {
    AiTocApiConfig cfg;
    HWND hwnd = nullptr;
    int generation = 0;
    Vec<char*> models;
    char* error = nullptr;
    ~AskModelsRequest() {
        FreeAiCfg(cfg);
        for (auto model : models) str::Free(model);
        str::Free(error);
    }
};

static void AskModelsReady(AskModelsRequest* request) {
    gAskModelsLoading = false;
    InlineTranslatePopup* p = gAsk;
    if (!p || p->hwnd != request->hwnd || p->generation != request->generation || p->asking) {
        delete request;
        return;
    }
    // Ignore a stale provider response if Settings changed during the fetch.
    if (!str::Eq(request->cfg.baseUrl, gGlobalPrefs->aiTocApiBaseUrl) ||
        !str::Eq(request->cfg.key, gGlobalPrefs->aiTocApiKey)) {
        delete request;
        return;
    }
    if (request->error) MessageBoxWarning(p->hwnd, request->error, _TRA("Ask AI"));
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (!gAskModel ? MF_CHECKED : 0), 1,
                ToWStrTemp(str::FormatTemp("%s: %s", _TRA("Default"), request->cfg.model ? request->cfg.model : "")));
    for (int i = 0; i < request->models.Size(); i++) {
        AppendMenuW(menu, MF_STRING | (str::Eq(gAskModel, request->models[i]) ? MF_CHECKED : 0), i + 2,
                    ToWStrTemp(request->models[i]));
    }
    POINT origin{p->modelRc.x + p->modelRc.dx, p->modelRc.y + p->modelRc.dy};
    ClientToScreen(p->hwnd, &origin);
    // Match the main/context menus: one owner-drawn coordinate system in
    // dark themes, rather than native hover text over the themed overlay.
    MarkMenuOwnerDraw(menu);
    UINT choice =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN, origin.x, origin.y, 0, p->hwnd, nullptr);
    FreeMenuOwnerDrawInfoData(menu);
    DestroyMenu(menu);
    if (gAsk == p && p->hwnd == request->hwnd && p->generation == request->generation && choice) {
        str::FreePtr(&gAskModel);
        str::FreePtr(&gAskModelProvider);
        if (choice >= 2 && choice - 2 < (UINT)request->models.Size()) {
            gAskModel = str::Dup(request->models[(int)choice - 2]);
            gAskModelProvider = str::Dup(request->cfg.baseUrl);
        }
        InvalidateRect(p->hwnd, nullptr, FALSE);
    }
    delete request;
}

static void AskModelsWorker(AskModelsRequest* request) {
    AiTocApiFetchModels(request->cfg.baseUrl, request->cfg.key, request->models, &request->error);
    uitask::Post(MkFunc0(AskModelsReady, request), "AskModelsReady");
}

static void ShowAskModels(InlineTranslatePopup* p) {
    if (!p->docked || p->asking || gAskModelsLoading || !InlineTranslateAiApiIsConfigured() ||
        !HasPermission(Perm::InternetAccess))
        return;
    auto request = new AskModelsRequest();
    request->cfg = SnapshotAiCfg();
    request->hwnd = p->hwnd;
    request->generation = p->generation;
    gAskModelsLoading = true;
    RunAsync(MkFunc0(AskModelsWorker, request), "AskModels");
}

static PopupHot HitTestHot(InlineTranslatePopup* p, Point pt) {
    if (p->docked && !p->modelRc.IsEmpty() && p->modelRc.Contains(pt)) return PopupHot::Model;
    if (p->closeRc.Contains(pt)) {
        return PopupHot::Close;
    }
    if (!p->copyRc.IsEmpty() && p->copyRc.Contains(pt)) {
        return PopupHot::Copy;
    }
    if (!p->hintRc.IsEmpty() && p->hintRc.Contains(pt)) {
        return PopupHot::Hint;
    }
    // The docked context line is status, not a control that expands the quote.
    if (!p->docked && !p->contextRc.IsEmpty() && !str::IsEmptyOrWhiteSpace(p->selection) && p->contextRc.Contains(pt)) {
        return PopupHot::Context;
    }
    if (!p->sendRc.IsEmpty() && p->sendRc.Contains(pt)) {
        return PopupHot::Send;
    }
    return PopupHot::None;
}

static LRESULT PopupNcHitTest(InlineTranslatePopup* p, LPARAM lp) {
    if (p->docked) {
        return HTCLIENT;
    }
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(p->hwnd, &pt);
    Rect rc = ClientRect(p->hwnd);
    Point cpt{pt.x, pt.y};
    if (HitTestHot(p, cpt) != PopupHot::None) {
        return HTCLIENT;
    }
    if (p->mode != PopupMode::Translate) {
        int grip = Px(p->hwnd, kResizeGrip);
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
    }
    // Header blank space drags. The translation body does not.
    bool inHeader = pt.y >= p->headerRc.y && pt.y < p->headerRc.y + p->headerRc.dy;
    if (inHeader || (p->mode != PopupMode::Translate && !p->origRc.IsEmpty() && p->origRc.Contains(cpt))) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

static LRESULT CALLBACK InlineTranslateWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MEASUREITEM && lp && ((MEASUREITEMSTRUCT*)lp)->CtlType == ODT_MENU) {
        MenuCustomDrawMesureItem(hwnd, (MEASUREITEMSTRUCT*)lp);
        return TRUE;
    }
    if (msg == WM_DRAWITEM && lp && ((DRAWITEMSTRUCT*)lp)->CtlType == ODT_MENU) {
        MenuCustomDrawItem(hwnd, (DRAWITEMSTRUCT*)lp);
        return TRUE;
    }
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
            HWND ctl = (HWND)lp;
            bool chat = ctl == p->hwndChat;
            bool dim = ctl == p->hwndTrans && (p->translating || p->transError);
            bool plainTrans = p->mode == PopupMode::Translate && ctl == p->hwndTrans;
            bool askInput = p->docked && ctl == p->hwndInput;
            COLORREF bg =
                chat ? ChatContentBg(p)
                     : (askInput ? AskComposerFill() : (plainTrans ? FloatingPopupBg() : FloatingPopupFieldBg()));
            SetTextColor(hdc, dim ? FloatingPopupMutedTextColor() : FloatingPopupTextColor());
            SetBkColor(hdc, bg);
            HBRUSH& brush = chat ? p->chatBrush : p->fieldBrush;
            if (brush) {
                DeleteObject(brush);
            }
            brush = CreateSolidBrush(bg);
            return (LRESULT)brush;
        }
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE && (HWND)lp == p->hwndInput) {
                SyncInputHasText(p);
                SyncComposerLines(p);
            }
            return 0;
        case WM_NCHITTEST:
            return PopupNcHitTest(p, lp);
        case WM_NCLBUTTONDBLCLK:
            // no maximize on header double-click
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (p->hot == PopupHot::Hint || p->hot == PopupHot::Context)) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_SIZING:
            p->userSized = true;
            break;
        case WM_GETMINMAXINFO: {
            if (p->docked) {
                return 0;
            }
            auto* mmi = (MINMAXINFO*)lp;
            if (p->mode == PopupMode::Translate) {
                mmi->ptMinTrackSize.x = Px(hwnd, 320);
                mmi->ptMinTrackSize.y = Px(hwnd, 72);
            } else {
                mmi->ptMinTrackSize.x = Px(hwnd, kPopupMinW);
                mmi->ptMinTrackSize.y = Px(hwnd, kPopupMinH);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            int dpi = (int)LOWORD(wp);
            if (dpi < 72) {
                dpi = 96;
            }
            dpi = RoundUp(dpi, 4);
            ApplyPopupFonts(p, dpi);
            auto* prc = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (IsWindowVisible(hwnd)) {
                LayoutPopup(p);
            }
            return 0;
        }
        case WM_SIZE:
            if (!p->docked) {
                UpdateFloatingPopupWindowRgn(hwnd, kFloatingPopupCornerRadius, !p->deferPaint);
            }
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
        case WM_LBUTTONDOWN: {
            PopupHot hot = HitTestHot(p, Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot == PopupHot::Send && p->inputHasText && !p->asking) {
                p->sendDown = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (hot == PopupHot::Close || hot == PopupHot::Copy) {
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            bool sendWasDown = p->sendDown;
            p->sendDown = false;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            PopupHot hot = HitTestHot(p, Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot == PopupHot::Close) {
                if (p->docked) {
                    if (p->win && IsMainWindowValid(p->win) && p->win->tocVisible) {
                        HwndSendCommand(p->win->hwndFrame, CmdToggleBookmarks);
                    }
                } else {
                    PostClosePopup();
                }
            } else if (hot == PopupHot::Context && !p->docked) {
                p->contextOpen = !p->contextOpen;
                LayoutPopup(p);
            } else if (hot == PopupHot::Copy) {
                CopyTranslation(p);
            } else if (hot == PopupHot::Send && sendWasDown) {
                OnSendClicked(p);
            } else if (hot == PopupHot::Model) {
                ShowAskModels(p);
            } else if (hot == PopupHot::Hint) {
                uitask::Post(MkFunc0Void(OpenAiSettingsFromPopup), "InlineTranslateOpenSettings");
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (p->sendDown) {
                p->sendDown = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_TIMER:
            if (wp == kCopiedTimerId) {
                KillTimer(hwnd, kCopiedTimerId);
                p->copied = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_ACTIVATE:
            // Clicking another app deactivates a focused translation popup.
            if (LOWORD(wp) == WA_INACTIVE && p->mode == PopupMode::Translate && !p->closing) {
                PostClosePopup();
                return 0;
            }
            break;
        case WM_CLOSE:
            if (p->docked) {
                return 0;
            }
            PostClosePopup();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            p->hwnd = nullptr;
            if (p->win && p->win->hwndAiSidebar == hwnd) {
                p->win->hwndAiSidebar = nullptr;
            }
            if (!p->closing) {
                // destroyed together with the owner frame
                if (gPopup == p) {
                    gPopup = nullptr;
                }
                if (gAsk == p) {
                    gAsk = nullptr;
                }
                FreePopupData(p);
                delete p;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// A click or scroll outside the translation popup dismisses it, as word lookup does.
static bool TranslateDismissOutside(InlineTranslatePopup* p, const MSG& msg) {
    if (p->mode != PopupMode::Translate) {
        return false;
    }
    bool clearSelection = false;
    switch (msg.message) {
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_NCMBUTTONDOWN:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            clearSelection = true;
            break;
        case WM_RBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
            break;
        default:
            return false;
    }
    if (msg.hwnd == p->hwnd || IsChild(p->hwnd, msg.hwnd)) {
        return false;
    }
    CloseInlineTranslatePopup(clearSelection);
    return true;
}

static bool AskComposerKeepsKey(WPARAM vk) {
    switch (vk) {
        case 'A':
        case 'C':
        case 'V':
        case 'X':
        case 'Z':
        case 'Y':
        case VK_INSERT:
            return true;
        default:
            return false;
    }
}

bool InlineTranslatePopupPreTranslate(MSG& msg) {
    if (gAsk && gAsk->hwnd && (msg.hwnd == gAsk->hwnd || IsChild(gAsk->hwnd, msg.hwnd))) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            if (gAsk->win && IsMainWindowValid(gAsk->win)) {
                HwndSetFocus(gAsk->win->hwndCanvas ? gAsk->win->hwndCanvas : gAsk->win->hwndFrame);
            }
            return true;
        }
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            // Ctrl+F, Ctrl+L, and the other app shortcuts stay with the document.
            // Copy / paste in the composer still stay here.
            if (alt || (ctrl && !AskComposerKeepsKey(msg.wParam))) {
                return false;
            }
            // Page Up / Page Down in the composer scroll the document.
            // In the conversation they scroll the conversation.
            if ((msg.wParam == VK_PRIOR || msg.wParam == VK_NEXT) && msg.hwnd == gAsk->hwndInput) {
                return false;
            }
        }
        if (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            return true;
        }
    }
    InlineTranslatePopup* p = gPopup;
    if (!p || !p->hwnd || !IsWindowVisible(p->hwnd)) {
        return false;
    }
    if (TranslateDismissOutside(p, msg)) {
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
    if (!InlineTranslateVolcIsConfigured() && !InlineTranslateAiApiIsConfigured() && WebAiTranslateEnabled()) {
        char* text = NormalizeSelectionText(sel);
        const char* dst = ResolveTranslateTarget(text, gGlobalPrefs->translateTargetMode, trans::GetCurrentLangCode());
        SendSelectionToWebAi(win, text, dst);
        str::Free(text);
        return;
    }
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
}

void ParkAskAiSidebar(WindowTab* tab) {
    if (!gAsk || !tab || gAsk->boundTab != tab) {
        return;
    }
    AiChatState* st = AskState(tab, true);
    MoveTurns(st->turns, gAsk->turns);
    char* draft = GetEditTextUtf8(gAsk->hwndInput);
    str::ReplacePtr(&st->draft, draft);
    str::ReplacePtr(&st->selection, gAsk->selection);
    gAsk->selection = nullptr;
    st->contextPage = gAsk->contextPage;
    gAsk->contextPage = 0;
    gAsk->contextOpen = false;
    st->generation = gAsk->generation;
    st->asking = gAsk->asking;
    gAsk->asking = false;
    gAsk->boundTab = nullptr;
    SetEditTextUtf8(gAsk->hwndChat, "");
    SetEditTextUtf8(gAsk->hwndInput, "");
    SyncInputHasText(gAsk);
}

void BindAskAiSidebar(WindowTab* tab) {
    if (!gAsk || !tab) {
        return;
    }
    if (gAsk->boundTab == tab) {
        return;
    }
    if (gAsk->boundTab) {
        ParkAskAiSidebar(gAsk->boundTab);
    }
    gAsk->boundTab = tab;
    gAsk->win = tab->win;
    AiChatState* st = AskState(tab, false);
    if (!st) {
        ClearTurns(gAsk);
        gAsk->asking = false;
        str::FreePtr(&gAsk->selection);
        gAsk->contextPage = 0;
        gAsk->contextOpen = false;
        SetEditTextUtf8(gAsk->hwndInput, "");
        SyncInputHasText(gAsk);
        PresentChatUpdate(gAsk);
        return;
    }
    MoveTurns(gAsk->turns, st->turns);
    str::ReplacePtr(&gAsk->selection, st->selection);
    st->selection = nullptr;
    gAsk->contextPage = st->contextPage;
    gAsk->contextOpen = false;
    if (st->generation) {
        gAsk->generation = st->generation;
    }
    gAsk->asking = st->asking;
    SetEditTextUtf8(gAsk->hwndInput, st->draft ? st->draft : "");
    str::FreePtr(&st->draft);
    SyncInputHasText(gAsk);
    PresentChatUpdate(gAsk);
}

void FreeTabAskAiState(WindowTab* tab) {
    if (!tab) {
        return;
    }
    if (gAsk && gAsk->boundTab == tab) {
        ClearTurns(gAsk);
        gAsk->asking = false;
        gAsk->generation++;
        gAsk->boundTab = nullptr;
        str::FreePtr(&gAsk->selection);
        gAsk->contextPage = 0;
        gAsk->contextOpen = false;
        if (gAsk->hwndInput) {
            SetEditTextUtf8(gAsk->hwndInput, "");
        }
        if (gAsk->hwnd) {
            PresentChatUpdate(gAsk);
        }
    }
    auto* st = (AiChatState*)tab->askAiState;
    tab->askAiState = nullptr;
    delete st;
}

void LayoutAskAiSidebar(MainWindow* win) {
    if (!gAsk || !gAsk->hwnd || !win || gAsk->win != win) {
        return;
    }
    LayoutPopup(gAsk);
    InvalidateRect(gAsk->hwnd, nullptr, FALSE);
}

void EnsureAskAiSidebar(MainWindow* win) {
    if (!win || !win->hwndTocBox || !gGlobalPrefs || !gGlobalPrefs->enableAskAI) {
        return;
    }
    if (!gAsk || gAsk->win != win) {
        if (gAsk) {
            if (gAsk->boundTab) {
                ParkAskAiSidebar(gAsk->boundTab);
            }
            if (gAsk->hwnd && IsWindow(gAsk->hwnd)) {
                gAsk->closing = true;
                DestroyWindow(gAsk->hwnd);
            }
            FreePopupData(gAsk);
            delete gAsk;
            gAsk = nullptr;
        }
        gAsk = CreatePopup(win, true);
        if (!gAsk) {
            return;
        }
    }
    WindowTab* tab = win->CurrentTab();
    if (tab) {
        BindAskAiSidebar(tab);
    }
}

static void FocusAskInput(InlineTranslatePopup* p) {
    if (!p || !p->hwndInput) {
        return;
    }
    ShowWindow(p->hwnd, SW_SHOW);
    SetFocus(p->hwndInput);
}

void AskAiSelectionInTab(MainWindow* win, WindowTab* tab, const char* selection, const char* initialPrompt) {
    if (!win || !tab || !gGlobalPrefs || !gGlobalPrefs->enableAskAI) {
        return;
    }
    if (str::IsEmptyOrWhiteSpace(selection) || str::IsEmptyOrWhiteSpace(initialPrompt)) {
        return;
    }
    HideSelectionToolbar(win);
    ShowSidebarPage(win, SidebarView::Ai);
    EnsureAskAiSidebar(win);
    InlineTranslatePopup* p = gAsk;
    if (!p) {
        return;
    }
    BindAskAiSidebar(tab);
    p->sourceSelection = tab->selectionOnPage;
    str::ReplacePtr(&p->selection, NormalizeSelectionText(selection));
    p->contextOpen = false;
    p->contextPage = (win->ctrl) ? win->ctrl->CurrentPageNo() : 0;
    // Show the complete request, including the selected passage, as a user
    // message. The API payload stays unchanged.
    StartAsk(p, initialPrompt, nullptr);
}

void OpenEmptyAskAi(MainWindow* win) {
    if (!win || !gGlobalPrefs || !gGlobalPrefs->enableAskAI || !HasPermission(Perm::InternetAccess)) {
        return;
    }
    HideSelectionToolbar(win);
    ShowSidebarPage(win, SidebarView::Ai);
    EnsureAskAiSidebar(win);
    FocusAskInput(gAsk);
}
