/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
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

static DWORD WINAPI PrepareEdgeConnection(void*) {
    HINTERNET session = EdgeHttpSession();
    if (!session) return 0;
    HINTERNET connection = WinHttpConnect(session, L"speech.platform.bing.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) return 0;
    defer {
        WinHttpCloseHandle(connection);
    };
    HINTERNET request = WinHttpOpenRequest(connection, L"HEAD", L"/", nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) return 0;
    defer {
        WinHttpCloseHandle(request);
    };
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    WinHttpSetTimeouts(request, 2000, 2000, 2000, 2000);
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) {
        WinHttpReceiveResponse(request, nullptr);
    }
    DestroyTempAllocator();
    return 0;
}

void EdgeSpeechPrepare() {
    static LONG prepared = 0;
    if (InterlockedCompareExchange(&prepared, 1, 0) != 0) return;
    HANDLE worker = CreateThread(nullptr, 0, PrepareEdgeConnection, nullptr, 0, nullptr);
    if (worker) {
        CloseHandle(worker);
    } else {
        InterlockedExchange(&prepared, 0);
    }
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

    HINTERNET session = EdgeHttpSession();
    if (!session) {
        return {GetLastError()};
    }
    HINTERNET connection = WinHttpConnect(session, L"speech.platform.bing.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        return {GetLastError()};
    }
    defer {
        WinHttpCloseHandle(connection);
    };
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", turn.resource, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        return {GetLastError()};
    }
    defer {
        WinHttpCloseHandle(request);
    };
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)) ||
        !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        return {GetLastError()};
    }
    if (EdgeTurnCancelled(cancelEvent)) {
        return {ERROR_CANCELLED};
    }
    if (!WinHttpSendRequest(request, turn.headers, turn.headers ? (DWORD)-1 : 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        return {EdgeTurnCancelled(cancelEvent) ? ERROR_CANCELLED : GetLastError()};
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status,
                             &statusSize, nullptr)) {
        return {GetLastError()};
    }
    if (status != 101) {
        return {ERROR_WINHTTP_INVALID_SERVER_RESPONSE, status};
    }
    HINTERNET socket = api.upgrade(request, 0);
    if (!socket) {
        return {GetLastError(), status};
    }
    defer {
        WinHttpCloseHandle(socket);
    };
    const char* messages[] = {turn.configMessage, turn.ssmlMessage};
    for (const char* msg : messages) {
        if (EdgeTurnCancelled(cancelEvent)) {
            return {ERROR_CANCELLED, status};
        }
        DWORD error = api.send(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)msg, (DWORD)strlen(msg));
        if (error != ERROR_SUCCESS) {
            return {error, status};
        }
    }
    return {EdgeReadWebSocket(api, socket, cancelEvent, response), status};
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
                                               EdgeSpeechResponse& response) {
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

    EdgeSpeechTurn turn{resource, headers, config.Get(), ssml.Get()};
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

const EdgeReadAloudVoice* FindEdgeReadAloudVoice(const char* id) {
    for (const auto& voice : gEdgeReadAloudVoices) {
        if (str::Eq(id, voice.id)) {
            return &voice;
        }
    }
    return nullptr;
}

EdgeSpeechTransportResult EdgeSpeechSynthesizeOnline(const char* text, const char* voice, float rate,
                                                     HANDLE cancelEvent, EdgeSpeechResponse& response) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks;
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    char validation[82];
    EdgeGec::Generate(ticks.QuadPart, validation);
    TempStr target = str::FormatTemp(
        "/consumer/speech/synthesize/readaloud/edge/v1?TrustedClientToken=6A5AA1D4EAFF4E9FB37E23D68491D6F4"
        "&Sec-MS-GEC=%s&Sec-MS-GEC-Version=2-143.0.3650.75",
        validation);
    GUID id;
    if (FAILED(CoCreateGuid(&id))) {
        return {ERROR_GEN_FAILURE};
    }
    char muid[33];
    constexpr char hex[] = "0123456789ABCDEF";
    const u8* bytes = (const u8*)&id;
    for (size_t i = 0; i < sizeof(id); i++) {
        muid[2 * i] = hex[bytes[i] >> 4];
        muid[2 * i + 1] = hex[bytes[i] & 15];
    }
    muid[32] = 0;
    TempStr headers = str::FormatTemp(
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0\r\n"
        "Origin: chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold\r\n"
        "Pragma: no-cache\r\nCache-Control: no-cache\r\nAccept-Language: en-US,en;q=0.9\r\nCookie: muid=%s;",
        muid);
    return EdgeSpeechSynthesize(ToWStrTemp(target), ToWStrTemp(headers), text, voice, rate, cancelEvent, response);
}
