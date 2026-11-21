/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Log.h"
#include "utils/CryptoUtil.h"
#include "utils/WinUtil.h"
#include "utils/JsonParser.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include "EdgeSpeechResponse.h"
#include "EdgeSpeechTransport.h"
#include "../ext/edge-gec/EdgeGec.h"

// Declare the WinHTTP WebSocket extensions while preserving the Windows 7 executable target.
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#include <winhttp.h>
#pragma pop_macro("_WIN32_WINNT")

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

struct EdgeWebSocketApi {
    decltype(&WinHttpWebSocketCompleteUpgrade) upgrade = nullptr;
    decltype(&WinHttpWebSocketSend) send = nullptr;
    decltype(&WinHttpWebSocketReceive) receive = nullptr;
};

static EdgeWebSocketApi GetEdgeWebSocketApi() {
    HMODULE winHttp = GetModuleHandleW(L"winhttp.dll");
    return {
        (decltype(EdgeWebSocketApi::upgrade))GetProcAddress(winHttp, "WinHttpWebSocketCompleteUpgrade"),
        (decltype(EdgeWebSocketApi::send))GetProcAddress(winHttp, "WinHttpWebSocketSend"),
        (decltype(EdgeWebSocketApi::receive))GetProcAddress(winHttp, "WinHttpWebSocketReceive"),
    };
}

static bool EdgeTurnCancelled(HANDLE cancelEvent) {
    return cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0;
}

static DWORD EdgeReadWebSocket(const EdgeWebSocketApi& api, HINTERNET socket, HANDLE cancelEvent,
                               EdgeSpeechResponse& response) {
    constexpr size_t maxMessageSize = 1024 * 1024;
    constexpr ULONGLONG maxReceiveMs = 120000;
    ULONGLONG start = GetTickCount64();
    u8 buffer[8192];
    StrBuilder message;
    while (!EdgeTurnCancelled(cancelEvent)) {
        if (GetTickCount64() - start >= maxReceiveMs) {
            return ERROR_TIMEOUT;
        }
        DWORD count = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD error = api.receive(socket, buffer, sizeof(buffer), &count, &type);
        if (error != ERROR_SUCCESS) {
            return EdgeTurnCancelled(cancelEvent) ? ERROR_CANCELLED : error;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            return ERROR_HANDLE_EOF;
        }
        if (count > maxMessageSize - message.size() || !message.Append(buffer, count)) {
            return ERROR_INVALID_DATA;
        }
        if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            continue;
        }
        if (EdgeTurnCancelled(cancelEvent)) {
            return ERROR_CANCELLED;
        }
        auto parsed =
            response.Receive(type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, message.Get(), message.size());
        message.Reset();
        if (parsed == EdgeSpeechMessage::Invalid) {
            return ERROR_INVALID_DATA;
        }
        if (parsed == EdgeSpeechMessage::End) {
            return response.mp3.IsEmpty() ? ERROR_NO_DATA : ERROR_SUCCESS;
        }
    }
    return ERROR_CANCELLED;
}

static HINTERNET EdgeHttpSession() {
    static INIT_ONCE sessionOnce = INIT_ONCE_STATIC_INIT;
    static HINTERNET sharedSession = nullptr;
    InitOnceExecuteOnce(
        &sessionOnce,
        [](PINIT_ONCE, PVOID context, PVOID*) -> BOOL {
            auto* handle = (HINTERNET*)context;
            *handle = WinHttpOpen(L"SumatraPDF Plus", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
            if (!*handle) return FALSE;
            if (!WinHttpSetTimeouts(*handle, 5000, 5000, 5000, 5000)) {
                WinHttpCloseHandle(*handle);
                *handle = nullptr;
                return FALSE;
            }
            return TRUE;
        },
        &sharedSession, nullptr);
    return sharedSession;
}

struct EdgeConnection {
    HINTERNET connection = nullptr;
    HINTERNET socket = nullptr;
    void Reset() {
        if (socket) WinHttpCloseHandle(socket);
        if (connection) WinHttpCloseHandle(connection);
        socket = connection = nullptr;
    }
    void Take(EdgeConnection& source) {
        connection = source.connection;
        socket = source.socket;
        source.connection = source.socket = nullptr;
    }
};

static EdgeSpeechTransportResult OpenEdgeConnection(const EdgeSpeechTurn& turn, HANDLE cancel, EdgeConnection& out,
                                                    const WCHAR* host = L"speech.platform.bing.com") {
    ULONGLONG started = GetTickCount64();
    const auto api = GetEdgeWebSocketApi();
    if (!api.upgrade || !api.send || !api.receive) return {ERROR_NOT_SUPPORTED};
    HINTERNET session = EdgeHttpSession();
    if (!session) return {GetLastError()};
    out.connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!out.connection) return {GetLastError()};
    HINTERNET request = WinHttpOpenRequest(out.connection, L"GET", turn.resource, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) return {GetLastError()};
    defer {
        WinHttpCloseHandle(request);
    };
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        return {GetLastError()};
    if (EdgeTurnCancelled(cancel)) return {ERROR_CANCELLED};
    if (!WinHttpSendRequest(request, turn.headers, turn.headers ? (DWORD)-1 : 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
        return {EdgeTurnCancelled(cancel) ? ERROR_CANCELLED : GetLastError()};
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                             nullptr))
        return {GetLastError()};
    if (status != 101) return {ERROR_WINHTTP_INVALID_SERVER_RESPONSE, status};
    out.socket = api.upgrade(request, 0);
    if (out.socket) logf("Edge speech: WebSocket ready after %llu ms\n", GetTickCount64() - started);
    return {out.socket ? ERROR_SUCCESS : GetLastError(), status};
}

static SRWLOCK gEdgeConnectionLock = SRWLOCK_INIT;
static EdgeConnection gEdgePreparedConnection;
static ULONGLONG gEdgePreparedAt = 0;
static bool gEdgePreparing = false;
static constexpr ULONGLONG kEdgeConnectionIdleMs = 20000;
static bool BuildEdgeRequest(WStrBuilder& target, WStrBuilder& headers);

static void SaveEdgeConnection(EdgeConnection& connection) {
    AcquireSRWLockExclusive(&gEdgeConnectionLock);
    gEdgePreparedConnection.Reset();
    gEdgePreparedConnection.Take(connection);
    gEdgePreparedAt = GetTickCount64();
    ReleaseSRWLockExclusive(&gEdgeConnectionLock);
}

static bool TakeEdgeConnection(EdgeConnection& connection) {
    AcquireSRWLockExclusive(&gEdgeConnectionLock);
    ULONGLONG age = GetTickCount64() - gEdgePreparedAt;
    if (gEdgePreparedConnection.socket && age > kEdgeConnectionIdleMs) {
        gEdgePreparedConnection.Reset();
    }
    bool ready = gEdgePreparedConnection.socket != nullptr;
    if (ready) connection.Take(gEdgePreparedConnection);
    ReleaseSRWLockExclusive(&gEdgeConnectionLock);
    if (ready) logf("Edge speech: prepared connection age %llu ms\n", age);
    return ready;
}

static DWORD WINAPI PrepareEdgeConnection(void*) {
    EdgeConnection connection;
    WStrBuilder target, headers;
    if (BuildEdgeRequest(target, headers)) {
        EdgeSpeechTurn turn{};
        turn.resource = target.Get();
        turn.headers = headers.Get();
        if (OpenEdgeConnection(turn, nullptr, connection).error == ERROR_SUCCESS) SaveEdgeConnection(connection);
    }
    connection.Reset();
    AcquireSRWLockExclusive(&gEdgeConnectionLock);
    gEdgePreparing = false;
    ReleaseSRWLockExclusive(&gEdgeConnectionLock);
    DestroyTempAllocator();
    return 0;
}

static void PrepareEdgeSpeech(ULONGLONG maxAge) {
    AcquireSRWLockExclusive(&gEdgeConnectionLock);
    bool ready = gEdgePreparedConnection.socket && GetTickCount64() - gEdgePreparedAt <= maxAge;
    if (ready || gEdgePreparing) {
        ReleaseSRWLockExclusive(&gEdgeConnectionLock);
        return;
    }
    gEdgePreparing = true;
    ReleaseSRWLockExclusive(&gEdgeConnectionLock);
    HANDLE worker = CreateThread(nullptr, 0, PrepareEdgeConnection, nullptr, 0, nullptr);
    if (worker)
        CloseHandle(worker);
    else {
        AcquireSRWLockExclusive(&gEdgeConnectionLock);
        gEdgePreparing = false;
        ReleaseSRWLockExclusive(&gEdgeConnectionLock);
    }
}

void EdgeSpeechPrepare() {
    PrepareEdgeSpeech(kEdgeConnectionIdleMs);
}

void EdgeSpeechRefreshPreparation() {
    // Leave time to establish a replacement before the current socket expires.
    PrepareEdgeSpeech(12000);
}

EdgeSpeechTransportResult EdgeSpeechReceiveTurn(const EdgeSpeechTurn& turn, HANDLE cancelEvent,
                                                EdgeSpeechResponse& response) {
    constexpr WCHAR route[] = L"/consumer/speech/synthesize/readaloud/edge/v1";
    constexpr size_t routeLen = dimof(route) - 1;
    if (!turn.resource || wcsncmp(turn.resource, route, routeLen) != 0 ||
        (turn.resource[routeLen] != 0 && turn.resource[routeLen] != L'?') || str::IsEmpty(turn.configMessage) ||
        str::IsEmpty(turn.ssmlMessage)) {
        return {ERROR_INVALID_PARAMETER};
    }
    if (EdgeTurnCancelled(cancelEvent)) {
        return {ERROR_CANCELLED};
    }
    static const EdgeWebSocketApi api = GetEdgeWebSocketApi();
    if (!api.upgrade || !api.send || !api.receive) {
        return {ERROR_NOT_SUPPORTED};
    }

    EdgeConnection connection;
    defer {
        connection.Reset();
    };
    bool reused = turn.allowPreparedConnection && TakeEdgeConnection(connection);
    if (reused) log("Edge speech: using prepared WebSocket\n");
    for (int attempt = 0; attempt < 2; attempt++) {
        if (EdgeTurnCancelled(cancelEvent)) return {ERROR_CANCELLED};
        EdgeSpeechTransportResult result{ERROR_SUCCESS, 101};
        if (!connection.socket) result = OpenEdgeConnection(turn, cancelEvent, connection);
        if (result.error == ERROR_SUCCESS) {
            const char* messages[] = {turn.configMessage, turn.ssmlMessage};
            for (const char* message : messages) {
                if (EdgeTurnCancelled(cancelEvent)) return {ERROR_CANCELLED, 101};
                result.error = api.send(connection.socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)message,
                                        (DWORD)strlen(message));
                if (result.error != ERROR_SUCCESS) break;
            }
            if (result.error == ERROR_SUCCESS)
                result.error = EdgeReadWebSocket(api, connection.socket, cancelEvent, response);
        }
        if (result.error == ERROR_SUCCESS) {
            if (turn.allowPreparedConnection && !EdgeTurnCancelled(cancelEvent)) SaveEdgeConnection(connection);
            return result;
        }
        connection.Reset();
        // The server can expire an idle socket. Retry only before publishing any
        // audio or word data, so a stale connection never repeats spoken text.
        if (!reused || attempt || EdgeTurnCancelled(cancelEvent) || !response.mp3.IsEmpty() ||
            !response.words.IsEmpty())
            return result;
        reused = false;
        log("Edge speech: idle WebSocket expired; reconnecting before audio\n");
    }
    return {ERROR_GEN_FAILURE};
}

static bool IsEdgeVoiceName(const char* voice) {
    if (str::IsEmpty(voice) || strlen(voice) > 120) {
        return false;
    }
    for (const char* p = voice; *p; p++) {
        char c = *p;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '-') {
            return false;
        }
    }
    return true;
}

static bool AppendEdgeSsmlText(StrBuilder& ssml, const char* text) {
    for (const char* p = text; *p; p++) {
        switch (*p) {
            case '&':
                ssml.Append("&amp;");
                break;
            case '<':
                ssml.Append("&lt;");
                break;
            case '>':
                ssml.Append("&gt;");
                break;
            default:
                if ((u8)*p < 0x20 && *p != '\t' && *p != '\r' && *p != '\n') {
                    return false;
                }
                ssml.AppendChar(*p);
                break;
        }
    }
    return true;
}

EdgeSpeechTransportResult EdgeSpeechSynthesize(const WCHAR* resource, const WCHAR* headers, const char* text,
                                               const char* voice, float rate, HANDLE cancelEvent,
                                               EdgeSpeechResponse& response, bool allowPreparedConnection) {
    if (str::IsEmpty(text) || strlen(text) > 4096 || !IsEdgeVoiceName(voice) || !(rate >= 0.25f && rate <= 2.0f)) {
        return {ERROR_INVALID_PARAMETER};
    }
    if (EdgeTurnCancelled(cancelEvent)) {
        return {ERROR_CANCELLED};
    }

    SYSTEMTIME utc;
    GetSystemTime(&utc);
    WCHAR httpTime[WINHTTP_TIME_FORMAT_BUFSIZE];
    if (!WinHttpTimeFromSystemTime(&utc, httpTime)) {
        return {GetLastError()};
    }
    GUID guid;
    if (FAILED(CoCreateGuid(&guid))) {
        return {ERROR_GEN_FAILURE};
    }
    WCHAR guidText[40];
    if (!StringFromGUID2(guid, guidText, dimof(guidText))) {
        return {ERROR_GEN_FAILURE};
    }
    StrBuilder requestId;
    for (const WCHAR* p = guidText; *p; p++) {
        if (*p != L'{' && *p != L'}' && *p != L'-') {
            requestId.AppendChar((char)*p);
        }
    }
    const char* timestamp = ToUtf8Temp(httpTime);
    StrBuilder config;
    config.AppendFmt(
        "X-Timestamp:%s\r\nContent-Type:application/json; charset=utf-8\r\n"
        "Path:speech.config\r\n\r\n",
        timestamp);
    config.Append(R"({"context":{"synthesis":{"audio":{"metadataoptions":{"wordBoundaryEnabled":"true",)"
                  R"("sentenceBoundaryEnabled":"false"},"outputFormat":"audio-24khz-48kbitrate-mono-mp3"}}}})");

    StrBuilder ssml;
    ssml.AppendFmt("X-Timestamp:%s\r\nX-RequestId:%s\r\nContent-Type:application/ssml+xml\r\nPath:ssml\r\n\r\n",
                   timestamp, requestId.Get());
    int ratePercent = (int)(rate * 100.0f + 0.5f) - 100;
    const char* firstDash = strchr(voice, '-');
    const char* secondDash = firstDash ? strchr(firstDash + 1, '-') : nullptr;
    const char* locale = secondDash ? str::DupTemp(voice, secondDash - voice) : "en-US";
    ssml.AppendFmt(
        "<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"%s\">"
        "<voice name=\"%s\"><prosody rate=\"%+d%%\">",
        locale, voice, ratePercent);
    if (!AppendEdgeSsmlText(ssml, text)) {
        return {ERROR_INVALID_PARAMETER};
    }
    ssml.Append("</prosody></voice></speak>");

    EdgeSpeechTurn turn{resource, headers, config.Get(), ssml.Get(), allowPreparedConnection};
    return EdgeSpeechReceiveTurn(turn, cancelEvent, response);
}

const EdgeReadAloudVoice gEdgeReadAloudVoices[] = {
    {"edge:emma-multilingual", "Emma", "en-US-EmmaMultilingualNeural", EdgeVoiceLanguage::Multilingual},
    {"edge:ava-multilingual", "Ava", "en-US-AvaMultilingualNeural", EdgeVoiceLanguage::Multilingual},
    {"edge:andrew-multilingual", "Andrew", "en-US-AndrewMultilingualNeural", EdgeVoiceLanguage::Multilingual},
    {"edge:brian-multilingual", "Brian", "en-US-BrianMultilingualNeural", EdgeVoiceLanguage::Multilingual},
    {"edge:zh-cn-xiaoxiao", "Xiaoxiao (zh-CN)", "zh-CN-XiaoxiaoNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-cn-xiaoyi", "Xiaoyi (zh-CN)", "zh-CN-XiaoyiNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-cn-yunjian", "Yunjian (zh-CN)", "zh-CN-YunjianNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-cn-yunxi", "Yunxi (zh-CN)", "zh-CN-YunxiNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-cn-yunxia", "Yunxia (zh-CN)", "zh-CN-YunxiaNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-cn-yunyang", "Yunyang (zh-CN)", "zh-CN-YunyangNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-tw-hsiaochen", "HsiaoChen (zh-TW)", "zh-TW-HsiaoChenNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-tw-hsiaoyu", "HsiaoYu (zh-TW)", "zh-TW-HsiaoYuNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-hk-hiugaai", "HiuGaai (zh-HK)", "zh-HK-HiuGaaiNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-hk-hiumaan", "HiuMaan (zh-HK)", "zh-HK-HiuMaanNeural", EdgeVoiceLanguage::Chinese},
    {"edge:zh-hk-wanlung", "WanLung (zh-HK)", "zh-HK-WanLungNeural", EdgeVoiceLanguage::Chinese},
    {"edge:en-us-aria", "Aria (en-US)", "en-US-AriaNeural", EdgeVoiceLanguage::English},
    {"edge:en-us-guy", "Guy (en-US)", "en-US-GuyNeural", EdgeVoiceLanguage::English},
    {"edge:en-us-jenny", "Jenny (en-US)", "en-US-JennyNeural", EdgeVoiceLanguage::English},
    {"edge:en-us-christopher", "Christopher (en-US)", "en-US-ChristopherNeural", EdgeVoiceLanguage::English},
    {"edge:en-us-eric", "Eric (en-US)", "en-US-EricNeural", EdgeVoiceLanguage::English},
    {"edge:en-us-michelle", "Michelle (en-US)", "en-US-MichelleNeural", EdgeVoiceLanguage::English},
    {"edge:en-us-steffan", "Steffan (en-US)", "en-US-SteffanNeural", EdgeVoiceLanguage::English},
    {"edge:en-us-roger", "Roger (en-US)", "en-US-RogerNeural", EdgeVoiceLanguage::English},
    {"edge:en-gb-sonia", "Sonia (en-GB)", "en-GB-SoniaNeural", EdgeVoiceLanguage::English},
    {"edge:en-gb-ryan", "Ryan (en-GB)", "en-GB-RyanNeural", EdgeVoiceLanguage::English},
    {"edge:en-gb-libby", "Libby (en-GB)", "en-GB-LibbyNeural", EdgeVoiceLanguage::English},
    {"edge:en-gb-maisie", "Maisie (en-GB)", "en-GB-MaisieNeural", EdgeVoiceLanguage::English},
    {"edge:en-gb-thomas", "Thomas (en-GB)", "en-GB-ThomasNeural", EdgeVoiceLanguage::English},
    {"edge:en-au-natasha", "Natasha (en-AU)", "en-AU-NatashaNeural", EdgeVoiceLanguage::English},
    {"edge:en-au-william", "William (en-AU)", "en-AU-WilliamNeural", EdgeVoiceLanguage::English},
    {"edge:en-ca-clara", "Clara (en-CA)", "en-CA-ClaraNeural", EdgeVoiceLanguage::English},
    {"edge:en-ca-liam", "Liam (en-CA)", "en-CA-LiamNeural", EdgeVoiceLanguage::English},
    {"edge:en-in-neerja", "Neerja (en-IN)", "en-IN-NeerjaNeural", EdgeVoiceLanguage::English},
    {"edge:en-in-prabhat", "Prabhat (en-IN)", "en-IN-PrabhatNeural", EdgeVoiceLanguage::English},
};
const int gEdgeReadAloudVoiceCount = (int)dimof(gEdgeReadAloudVoices);

static const EdgeReadAloudVoice kCloudVoices[] = {
    {"azure:configured", "Microsoft Azure Speech", "azure:configured", EdgeVoiceLanguage::Multilingual},
    {"joyinside:40486", "JoyInside · Mature uncle", "joyinside:40486", EdgeVoiceLanguage::Chinese},
};

static constexpr const char* kSpeechRegistry = "Software\\SumatraPDF-Plus\\OnlineSpeech";
static volatile LONG gJoySettingsVersion = 0;
static constexpr const char* kSpeechValues[] = {"AzureRegion", "AzureKey", "AzureVoice", "JoyAccessKey",
                                                "JoySecret",   "JoyBotId", "JoyVendorId"};
static constexpr const WCHAR* kSpeechEnvironment[] = {
    L"SUMATRA_AZURE_REGION", L"SUMATRA_AZURE_KEY",  L"SUMATRA_AZURE_VOICE",   L"SUMATRA_JOY_ACCESS_KEY",
    L"SUMATRA_JOY_SECRET",   L"SUMATRA_JOY_BOT_ID", L"SUMATRA_JOY_VENDOR_ID",
};
static SRWLOCK gJoyCatalogLock = SRWLOCK_INIT;
static Vec<const EdgeReadAloudVoice*> gJoyCatalog, gJoyKnownVoices;
static LONG gJoyCatalogVersion = -1, gJoyFetchingVersion = -1;

bool ReadCloudSpeechSetting(int index, WCHAR* value, DWORD capacity) {
    value[0] = 0;
    if (const char* stored = ReadRegStrTemp(HKEY_CURRENT_USER, kSpeechRegistry, kSpeechValues[index])) {
        bool encrypted = index == 1 || index == 4;
        AutoFree plain(encrypted ? UnprotectStringDpapi(stored) : nullptr);
        const char* data = encrypted ? plain.Get() : stored;
        if (str::IsEmpty(data)) return false;
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, -1, value, capacity) > 0;
    }
    DWORD length = GetEnvironmentVariableW(kSpeechEnvironment[index], value, capacity);
    return length > 0 && length < capacity;
}

bool SaveCloudSpeechSettings(const WCHAR* region, const WCHAR* key, const WCHAR* voice, const WCHAR* accessKey,
                             const WCHAR* secret, const WCHAR* botId, const WCHAR* vendorId) {
    AutoFree protectedKey(ProtectStringDpapi(ToUtf8Temp(key)));
    AutoFree protectedSecret(ProtectStringDpapi(ToUtf8Temp(secret)));
    if (!protectedKey.Get() || !protectedSecret.Get()) return false;
    const char* values[] = {ToUtf8Temp(region),    protectedKey.Get(), ToUtf8Temp(voice),   ToUtf8Temp(accessKey),
                            protectedSecret.Get(), ToUtf8Temp(botId),  ToUtf8Temp(vendorId)};
    for (int i = 0; i < (int)dimof(values); i++) {
        if (!WriteRegStr(HKEY_CURRENT_USER, kSpeechRegistry, kSpeechValues[i], values[i])) return false;
    }
    InterlockedIncrement(&gJoySettingsVersion);
    return true;
}

const EdgeReadAloudVoice* ConfiguredCloudSpeechVoice(int index) {
    WCHAR region[64], voice[128], key[256];
    if (index == 0 && ReadCloudSpeechSetting(0, region, dimof(region)) && ReadCloudSpeechSetting(1, key, dimof(key)) &&
        ReadCloudSpeechSetting(2, voice, dimof(voice))) {
        return &kCloudVoices[0];
    }
    WCHAR accessKey[128], secret[256], botId[128];
    if (index == 1 && ReadCloudSpeechSetting(3, accessKey, dimof(accessKey)) &&
        ReadCloudSpeechSetting(4, secret, dimof(secret)) && ReadCloudSpeechSetting(5, botId, dimof(botId))) {
        return &kCloudVoices[1];
    }
    return nullptr;
}

static bool JoySafeId(const char* text, bool bearer = false);

const EdgeReadAloudVoice* FindEdgeReadAloudVoice(const char* id) {
    if (str::StartsWith(id, "joyinside:") && !str::Eq(id, kCloudVoices[1].id) && strlen(id) < 128 &&
        JoySafeId(id + 10) && ConfiguredCloudSpeechVoice(1)) {
        AcquireSRWLockExclusive(&gJoyCatalogLock);
        defer {
            ReleaseSRWLockExclusive(&gJoyCatalogLock);
        };
        for (const auto* voice : gJoyCatalog) {
            if (str::Eq(id, voice->id)) return voice;
        }
        for (const auto* voice : gJoyKnownVoices) {
            if (str::Eq(id, voice->id)) return voice;
        }
        char* storedId = str::Dup(id);
        auto* voice = new EdgeReadAloudVoice{storedId, storedId + 10, storedId, EdgeVoiceLanguage::Multilingual};
        gJoyKnownVoices.Append(voice);
        return voice;
    }
    for (const auto& voice : kCloudVoices) {
        if (str::Eq(id, voice.id)) return &voice;
    }
    for (const auto& voice : gEdgeReadAloudVoices) {
        if (str::Eq(id, voice.id)) {
            return &voice;
        }
    }
    return nullptr;
}

static bool BuildEdgeRequest(WStrBuilder& target, WStrBuilder& headers) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks;
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    char validation[82];
    EdgeGec::Generate(ticks.QuadPart, validation);
    TempStr requestTarget = str::FormatTemp(
        "/consumer/speech/synthesize/readaloud/edge/v1?TrustedClientToken=6A5AA1D4EAFF4E9FB37E23D68491D6F4"
        "&Sec-MS-GEC=%s&Sec-MS-GEC-Version=2-143.0.3650.75",
        validation);
    GUID id;
    if (FAILED(CoCreateGuid(&id))) {
        return false;
    }
    char muid[33];
    constexpr char hex[] = "0123456789ABCDEF";
    const u8* bytes = (const u8*)&id;
    for (size_t i = 0; i < sizeof(id); i++) {
        muid[2 * i] = hex[bytes[i] >> 4];
        muid[2 * i + 1] = hex[bytes[i] & 15];
    }
    muid[32] = 0;
    TempStr requestHeaders = str::FormatTemp(
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0\r\n"
        "Origin: chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold\r\n"
        "Pragma: no-cache\r\nCache-Control: no-cache\r\nAccept-Language: en-US,en;q=0.9\r\nCookie: muid=%s;",
        muid);
    target.Set(ToWStrTemp(requestTarget));
    headers.Set(ToWStrTemp(requestHeaders));
    return true;
}

static EdgeSpeechTransportResult PostSpeech(const WCHAR* host, const WCHAR* path, const WCHAR* headers,
                                            StrBuilder& body, HANDLE cancel, StrBuilder& output) {
    if (EdgeTurnCancelled(cancel)) return {ERROR_CANCELLED};
    HINTERNET session = EdgeHttpSession();
    if (!session) return {GetLastError()};
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) return {GetLastError()};
    defer {
        WinHttpCloseHandle(connection);
    };
    HINTERNET request = WinHttpOpenRequest(connection, L"POST", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!request) return {GetLastError()};
    defer {
        WinHttpCloseHandle(request);
    };
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD length = (DWORD)body.Size();
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSendRequest(request, headers, (DWORD)-1, (void*)body.Get(), length, length, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        return {GetLastError()};
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                             nullptr)) {
        return {GetLastError()};
    }
    if (status != 200) return {ERROR_WINHTTP_INVALID_SERVER_RESPONSE, status};
    char buffer[8192];
    while (!EdgeTurnCancelled(cancel)) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read)) return {GetLastError(), status};
        if (read == 0) {
            return {output.Size() ? DWORD(ERROR_SUCCESS) : DWORD(ERROR_INVALID_DATA), status};
        }
        if (output.Size() + read > 8 * 1024 * 1024 || !output.Append(buffer, read)) {
            return {ERROR_INVALID_DATA, status};
        }
    }
    return {ERROR_CANCELLED, status};
}

static EdgeSpeechTransportResult SynthesizeAzure(const char* text, float rate, HANDLE cancel,
                                                 EdgeSpeechResponse& response) {
    WCHAR region[64], voice[128], key[256];
    if (!ReadCloudSpeechSetting(0, region, dimof(region)) || !ReadCloudSpeechSetting(1, key, dimof(key)) ||
        !ReadCloudSpeechSetting(2, voice, dimof(voice))) {
        return {ERROR_INVALID_PARAMETER};
    }
    for (const WCHAR* p = region; *p; p++) {
        if (!(*p >= L'a' && *p <= L'z') && !(*p >= L'0' && *p <= L'9')) return {ERROR_INVALID_PARAMETER};
    }
    for (const WCHAR* p = key; *p; p++) {
        if (*p <= L' ' || *p > L'~') return {ERROR_INVALID_PARAMETER};
    }
    const char* name = ToUtf8Temp(voice);
    if (!IsEdgeVoiceName(name)) return {ERROR_INVALID_PARAMETER};
    const char* dash = strchr(name, '-');
    const char* end = dash ? strchr(dash + 1, '-') : nullptr;
    const char* locale = end ? str::DupTemp(name, end - name) : "en-US";
    StrBuilder body;
    body.AppendFmt(
        "<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"%s\">"
        "<voice name=\"%s\"><prosody rate=\"%+d%%\">",
        locale, name, (int)(rate * 100 + 0.5f) - 100);
    if (!AppendEdgeSsmlText(body, text)) return {ERROR_INVALID_PARAMETER};
    body.Append("</prosody></voice></speak>");
    WStrBuilder host, headers;
    host.Append(region);
    host.Append(L".tts.speech.microsoft.com");
    headers.Append(
        L"Content-Type: application/ssml+xml\r\n"
        L"X-Microsoft-OutputFormat: audio-24khz-48kbitrate-mono-mp3\r\n"
        L"Ocp-Apim-Subscription-Key: ");
    headers.Append(key);
    return PostSpeech(host.Get(), L"/cognitiveservices/v1", headers.Get(), body, cancel, response.mp3);
}

struct JoySpeechJson : json::ValueVisitor {
    StrBuilder token, type, event, audio, codec;
    bool failed = false;

    bool Visit(const char* path, const char* value, json::Type kind) override {
        if (kind == json::Type::Number && str::Eq(path, "/code")) {
            failed = !str::Eq(value, "200") && !str::Eq(value, "0");
        }
        if (kind != json::Type::String) return true;
        if (str::Eq(path, "/data/accessToken") || str::Eq(path, "/result/accessToken") || str::Eq(path, "/accessToken"))
            token.Set(value);
        if (str::Eq(path, "/contentType")) type.Set(value);
        if (str::Eq(path, "/content/eventType")) event.Set(value);
        if (str::Eq(path, "/content/audioBase64")) audio.Set(value);
        if (str::Eq(path, "/content/audioAue")) codec.Set(value);
        return true;
    }
};

static bool JoySafeId(const char* text, bool bearer) {
    if (str::IsEmpty(text)) return false;
    for (; *text; text++) {
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.~", *text) &&
            !(bearer && strchr("+/=", *text)))
            return false;
    }
    return true;
}

static TempStr JoyRequestId() {
    GUID id;
    WCHAR text[40];
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, dimof(text))) return nullptr;
    text[37] = 0;
    return ToUtf8Temp(text + 1);
}

static bool JoyRequestToken(const char* accessKey, const char* secret, const char* identity, HANDLE cancel,
                            JoySpeechJson& reply, bool vendor = false) {
    auto nonce = JoyRequestId();
    if (!nonce) return false;
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks;
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    auto stamp = str::FormatTemp("%llu", ticks.QuadPart / 10000 - 11644473600000ULL);
    auto input =
        str::FormatTemp("accesskeyid=%s&accessnonce=%s&accesstimestamp=%s&accessversion=V2", accessKey, nonce, stamp);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        return false;
    defer {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, (PUCHAR)secret, (ULONG)strlen(secret), 0) < 0) return false;
    defer {
        BCryptDestroyHash(hash);
    };
    BYTE digest[16];
    if (BCryptHashData(hash, (PUCHAR)input, (ULONG)strlen(input), 0) < 0 ||
        BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
        return false;
    char signature[33] = {};
    constexpr char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        signature[i * 2] = hex[digest[i] >> 4];
        signature[i * 2 + 1] = hex[digest[i] & 15];
    }
    StrBuilder body, output;
    body.AppendFmt(R"({"accessVersion":"V2","accessTimestamp":"%s","accessNonce":"%s",)"
                   R"("accessKeyId":"%s","accessSign":"%s","%s":"%s"})",
                   stamp, nonce, accessKey, signature, vendor ? "vendorId" : "botId", identity);
    auto result =
        PostSpeech(L"api.joyinside.com", L"/auth/getToken", L"Content-Type: application/json", body, cancel, output);
    return result.error == ERROR_SUCCESS && json::Parse(output.Get(), &reply) && !reply.failed &&
           JoySafeId(reply.token.Get(), true);
}

struct JoyVoiceList : json::ValueVisitor {
    struct Item {
        char* id = nullptr;
        char* name = nullptr;
    };
    Vec<Item> items;
    ~JoyVoiceList() override {
        for (auto& item : items) {
            free(item.id);
            free(item.name);
        }
    }
    bool Visit(const char* path, const char* value, json::Type type) override {
        if (!str::StartsWith(path, "/data[")) return true;
        const char* index = path + 6;
        char* end = nullptr;
        unsigned long i = strtoul(index, &end, 10);
        if (index == end || i >= 4096 || *end != ']') return false;
        bool id = str::Eq(end + 1, "/timbreId") && (type == json::Type::String || type == json::Type::Number);
        bool name = str::Eq(end + 1, "/voiceName") && type == json::Type::String;
        if (!id && !name) return true;
        while ((unsigned long)items.Size() <= i) items.Append(Item{});
        str::ReplaceWithCopy(id ? &items[i].id : &items[i].name, value);
        return true;
    }
};

static bool IsJoyReadingVoice(const char* id) {
    // Character and novelty voices, plus voices excluded by editorial preference.
    // Use catalog IDs so renamed voices remain filtered. Keep 40486 for long-form reading.
    static constexpr const char* excluded[] = {
        "40186", "40231", "40230", "40283", "40280", "40351", "40252", "40408", "40033", "40030", "40094", "40027",
        "40407", "40409", "40309", "40210", "40212", "40418", "40421", "40422", "40424", "40427", "40469", "40484",
        "40488", "40489", "40490", "40491", "40261", "40565", "40577", "40578", "40594", "40229", "40596", "40614",
        "40258", "40665", "32094", "40677", "40680", "40682", "40683", "40684", "40685", "40686", "40687", "40228",
        "40621", "40620", "40618", "40566", "40198", "40366", "40420", "40619", "40678", "40246", "40503", "40200",
    };
    for (const char* candidate : excluded) {
        if (str::Eq(id, candidate)) return false;
    }
    return true;
}

void GetJoySpeechVoices(Vec<const EdgeReadAloudVoice*>& voices) {
    AcquireSRWLockShared(&gJoyCatalogLock);
    defer {
        ReleaseSRWLockShared(&gJoyCatalogLock);
    };
    if (gJoyCatalogVersion == InterlockedCompareExchange(&gJoySettingsVersion, 0, 0))
        for (const auto* voice : gJoyCatalog) voices.Append(voice);
}

struct JoyCatalogFetch {
    LONG version;
    void (*onChanged)();
};

static DWORD WINAPI FetchJoyCatalog(void* arg) {
    auto* work = (JoyCatalogFetch*)arg;
    defer {
        delete work;
        DestroyTempAllocator();
    };
    JoyVoiceList list;
    bool success = false;
    WCHAR accessKey[128], secret[256], vendorId[128];
    defer {
        SecureZeroMemory(secret, sizeof(secret));
    };
    if (ReadCloudSpeechSetting(3, accessKey, dimof(accessKey)) && ReadCloudSpeechSetting(4, secret, dimof(secret)) &&
        ReadCloudSpeechSetting(6, vendorId, dimof(vendorId))) {
        const char* access = ToUtf8Temp(accessKey);
        const char* vendor = ToUtf8Temp(vendorId);
        auto id = JoyRequestId();
        JoySpeechJson token;
        if (id && JoySafeId(access) && JoySafeId(vendor) &&
            JoyRequestToken(access, ToUtf8Temp(secret), vendor, nullptr, token, true)) {
            StrBuilder request, output;
            request.AppendFmt(R"({"requestId":"%s","vendorId":"%s"})", id, vendor);
            auto headers = ToWStrTemp(
                str::FormatTemp("Content-Type: application/json\r\nAuthorization: Bearer %s", token.token.Get()));
            auto http = PostSpeech(L"api.joyinside.com", L"/soulmate/timbre/findAllByVendorId", headers, request,
                                   nullptr, output);
            success = http.error == ERROR_SUCCESS && json::Parse(output.Get(), &list) && !list.items.IsEmpty();
        }
    }
    AcquireSRWLockExclusive(&gJoyCatalogLock);
    if (work->version == gJoyFetchingVersion) gJoyFetchingVersion = -1;
    success = success && work->version == InterlockedCompareExchange(&gJoySettingsVersion, 0, 0);
    if (success) {
        gJoyCatalog.Reset();
        for (const auto& item : list.items) {
            if (!JoySafeId(item.id) || str::IsEmpty(item.name) || !IsJoyReadingVoice(item.id)) continue;
            const char* id = str::FormatTemp("joyinside:%s", item.id);
            const EdgeReadAloudVoice* voice = nullptr;
            for (const auto* known : gJoyKnownVoices) {
                if (str::Eq(known->id, id) && str::Eq(known->name, item.name)) {
                    voice = known;
                    break;
                }
            }
            if (!voice) {
                // Synthesis jobs retain these pointers across configuration changes.
                char* storedId = str::Dup(id);
                voice =
                    new EdgeReadAloudVoice{storedId, str::Dup(item.name), storedId, EdgeVoiceLanguage::Multilingual};
                gJoyKnownVoices.Append(voice);
            }
            if (!gJoyCatalog.Contains(voice)) gJoyCatalog.Append(voice);
        }
        gJoyCatalogVersion = work->version;
    }
    ReleaseSRWLockExclusive(&gJoyCatalogLock);
    if (success && work->onChanged) work->onChanged();
    return 0;
}

void RefreshJoySpeechVoices(void (*onChanged)()) {
    WCHAR vendorId[128];
    if (!ConfiguredCloudSpeechVoice(1) || !ReadCloudSpeechSetting(6, vendorId, dimof(vendorId))) return;
    LONG version = InterlockedCompareExchange(&gJoySettingsVersion, 0, 0);
    AcquireSRWLockExclusive(&gJoyCatalogLock);
    if (gJoyCatalogVersion != version && gJoyFetchingVersion != version) {
        auto* work = new JoyCatalogFetch{version, onChanged};
        if (HANDLE thread = CreateThread(nullptr, 0, FetchJoyCatalog, work, 0, nullptr)) {
            gJoyFetchingVersion = version;
            CloseHandle(thread);
        } else {
            delete work;
        }
    }
    ReleaseSRWLockExclusive(&gJoyCatalogLock);
}

static DWORD SendJoyFrame(const EdgeWebSocketApi& api, HINTERNET socket, StrBuilder& frame) {
    return api.send(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)frame.Get(), (DWORD)frame.Size());
}

static DWORD ReceiveJoySpeech(const EdgeWebSocketApi& api, HINTERNET socket, HANDLE cancel, StrBuilder& request,
                              EdgeSpeechResponse& response, bool submitted) {
    auto& mp3 = response.mp3;
    StrBuilder message;
    char buffer[8192];
    ULONGLONG start = GetTickCount64();
    while (!EdgeTurnCancelled(cancel) && GetTickCount64() - start < 30000) {
        DWORD count = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD error = api.receive(socket, buffer, sizeof(buffer), &count, &type);
        if (error != ERROR_SUCCESS) return error;
        if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            return ERROR_INVALID_DATA;
        if (message.Size() + count > 2 * 1024 * 1024 || !message.Append(buffer, count)) return ERROR_INVALID_DATA;
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
        JoySpeechJson reply;
        if (!json::Parse(message.Get(), &reply) || reply.failed) return ERROR_INVALID_DATA;
        message.Reset();
        if (!submitted) {
            if (str::Eq(reply.event.Get(), "SERVER_VOICE_CHAT_UPDATED")) {
                error = SendJoyFrame(api, socket, request);
                if (error != ERROR_SUCCESS) return error;
                submitted = true;
            }
            continue;
        }
        if (str::Eq(reply.type.Get(), "TTS") && !reply.audio.IsEmpty()) {
            DWORD bytes = 0;
            if (!str::Eq(reply.codec.Get(), "mp3") ||
                !CryptStringToBinaryA(reply.audio.Get(), 0, CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr) ||
                mp3.Size() + bytes > 8 * 1024 * 1024)
                return ERROR_INVALID_DATA;
            AutoFree data((char*)malloc(bytes));
            if (!data.Get() ||
                !CryptStringToBinaryA(reply.audio.Get(), 0, CRYPT_STRING_BASE64, (BYTE*)data.Get(), &bytes, nullptr,
                                      nullptr) ||
                !mp3.Append(data.Get(), bytes))
                return ERROR_INVALID_DATA;
            if (response.audioReceived) response.audioReceived(response.audioContext, response);
        }
        if (str::Eq(reply.event.Get(), "TTS_COMPLETE") || str::Eq(reply.type.Get(), "TTS_COMPLETE")) {
            return mp3.IsEmpty() ? ERROR_NO_DATA : ERROR_SUCCESS;
        }
    }
    return EdgeTurnCancelled(cancel) ? ERROR_CANCELLED : ERROR_TIMEOUT;
}

static EdgeSpeechTransportResult SynthesizeJoySpeech(const char* text, const char* voiceId, float rate, HANDLE cancel,
                                                     EdgeSpeechResponse& response) {
    static SRWLOCK lock = SRWLOCK_INIT;
    while (!TryAcquireSRWLockExclusive(&lock)) {
        if (cancel && WaitForSingleObject(cancel, 25) == WAIT_OBJECT_0) return {ERROR_CANCELLED};
        if (!cancel) Sleep(25);
    }
    defer {
        ReleaseSRWLockExclusive(&lock);
    };
    WCHAR accessKey[128], secret[256], botId[128];
    defer {
        SecureZeroMemory(secret, sizeof(secret));
    };
    if (str::IsEmpty(text) || strlen(text) >= 1000 || !ReadCloudSpeechSetting(3, accessKey, dimof(accessKey)) ||
        !ReadCloudSpeechSetting(4, secret, dimof(secret)) || !ReadCloudSpeechSetting(5, botId, dimof(botId))) {
        return {ERROR_INVALID_PARAMETER};
    }
    const char* access = ToUtf8Temp(accessKey);
    const char* bot = ToUtf8Temp(botId);
    if (!JoySafeId(access) || !JoySafeId(bot)) return {ERROR_INVALID_PARAMETER};
    auto id = JoyRequestId();
    auto speechId = JoyRequestId();
    if (!id || !speechId) return {ERROR_GEN_FAILURE};
    auto target =
        str::FormatTemp("/soulmate/voiceChat/v1?botId=%s&sessionId=%s-%s&requestId=%s&uid=%s", bot, bot, id, id, bot);
    StrBuilder config, request;
    int speed = (int)(rate * 100.0f + 0.5f);
    speed = speed < 80 ? 80 : speed > 120 ? 120 : speed;
    config.AppendFmt(
        R"({"mid":"%s","contentType":"EVENT","content":{"eventType":"CLIENT_VOICE_CHAT_UPDATE",)"
        R"("eventData":{"audio":{"binary":false,"output":{"codec":"mp3","sampleRate":"24000","stream":true},)"
        R"("timbre":{"voiceId":"%s","voiceSpeed":%d.%02d}}}}})",
        id, voiceId, speed / 100, speed % 100);
    request.AppendFmt(R"({"mid":"%s","contentType":"EVENT","content":{"eventType":"CLIENT_INPUT_TEXT_TO_SPEECH",)"
                      R"("eventData":{"text":")",
                      speechId);
    for (const char* p = text; *p; p++) {
        if (*p == '"' || *p == '\\') request.AppendChar('\\');
        if ((u8)*p < 32)
            request.AppendFmt("\\u%04x", (unsigned)(u8)*p);
        else
            request.AppendChar(*p);
    }
    request.Append("\"}}}");
    static EdgeConnection connection;
    static StrBuilder activeVoice, cachedToken;
    static int activeSpeed = 0;
    static LONG version = -1;
    static ULONGLONG lastUsed = 0, tokenAt = 0;
    LONG currentVersion = InterlockedCompareExchange(&gJoySettingsVersion, 0, 0);
    ULONGLONG now = GetTickCount64();
    if (version != currentVersion) {
        connection.Reset();
        cachedToken.Reset();
        version = currentVersion;
    }
    if (now - lastUsed > 15000 || !str::Eq(activeVoice.Get(), voiceId) || speed != activeSpeed) connection.Reset();
    auto api = GetEdgeWebSocketApi();
    EdgeSpeechTransportResult result{ERROR_GEN_FAILURE};
    for (int attempt = 0; attempt < 2 && !EdgeTurnCancelled(cancel); attempt++) {
        bool reused = connection.socket != nullptr;
        if (!reused) {
            if (cachedToken.IsEmpty() || now - tokenAt > 60000) {
                JoySpeechJson token;
                if (!JoyRequestToken(access, ToUtf8Temp(secret), bot, cancel, token)) {
                    return {EdgeTurnCancelled(cancel) ? DWORD(ERROR_CANCELLED) : DWORD(ERROR_ACCESS_DENIED)};
                }
                cachedToken.Set(token.token.Get());
                tokenAt = GetTickCount64();
            }
            auto headers = str::FormatTemp("Authorization: Bearer %s", cachedToken.Get());
            EdgeSpeechTurn turn{ToWStrTemp(target), ToWStrTemp(headers), nullptr, nullptr, false};
            result = OpenEdgeConnection(turn, cancel, connection, L"ws.joyinside.com");
            if (result.error != ERROR_SUCCESS) {
                connection.Reset();
                cachedToken.Reset();
                return result;
            }
        }
        result.httpStatus = 101;
        result.error = SendJoyFrame(api, connection.socket, reused ? request : config);
        if (result.error == ERROR_SUCCESS) {
            result.error = ReceiveJoySpeech(api, connection.socket, cancel, request, response, reused);
        }
        if (result.error == ERROR_SUCCESS) {
            activeVoice.Set(voiceId);
            activeSpeed = speed;
            lastUsed = GetTickCount64();
            return result;
        }
        connection.Reset();
        if (!reused || !response.mp3.IsEmpty()) break;
    }
    if (EdgeTurnCancelled(cancel)) result.error = ERROR_CANCELLED;
    return result;
}

EdgeSpeechTransportResult EdgeSpeechSynthesizeOnline(const char* text, const char* voice, float rate,
                                                     HANDLE cancelEvent, EdgeSpeechResponse& response,
                                                     bool usePrepared) {
    if (str::Eq(voice, kCloudVoices[0].shortName)) return SynthesizeAzure(text, rate, cancelEvent, response);
    if (str::StartsWith(voice, "joyinside:") && JoySafeId(voice + 10))
        return SynthesizeJoySpeech(text, voice + 10, rate, cancelEvent, response);
    WStrBuilder target, headers;
    if (!BuildEdgeRequest(target, headers)) return {ERROR_GEN_FAILURE};
    return EdgeSpeechSynthesize(target.Get(), headers.Get(), text, voice, rate, cancelEvent, response, usePrepared);
}
