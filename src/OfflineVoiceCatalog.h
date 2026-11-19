/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#pragma once

#include "utils/JsonParser.h"
#include <winhttp.h>
#include <msxml6.h>
#include "../ext/tts-store/VoiceStoreRequests.h"

#pragma comment(lib, "winhttp.lib")

static bool VoiceStoreCancelled(HANDLE cancel) {
    return cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0;
}

static void VoiceStoreReplace(StrBuilder& value, const char* token, const char* replacement) {
    StrBuilder result;
    const char* current = value.Get();
    const char* match;
    while ((match = strstr(current, token)) != nullptr) {
        result.Append(current, match - current);
        result.Append(replacement);
        current = match + str::Len(token);
    }
    result.Append(current);
    value.Set(result.Get());
}

static bool VoiceStoreRequest(const WCHAR* host, const WCHAR* path, const char* body, HANDLE cancel,
                              StrBuilder& response) {
    if (VoiceStoreCancelled(cancel)) return false;
    HINTERNET session = WinHttpOpen(L"SumatraPDF-Plus/VoiceDownload", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    defer {
        WinHttpCloseHandle(session);
    };
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) return false;
    defer {
        WinHttpCloseHandle(connection);
    };
    HINTERNET request = WinHttpOpenRequest(connection, body ? L"POST" : L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) return false;
    defer {
        WinHttpCloseHandle(request);
    };
    DWORD redirects = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &redirects, sizeof(redirects))) return false;
    const WCHAR* headers = body ? L"Content-Type: application/soap+xml; charset=utf-8\r\n" : nullptr;
    DWORD size = body ? (DWORD)str::Len(body) : 0;
    if (!WinHttpSendRequest(request, headers, headers ? (DWORD)-1 : 0, (void*)body, size, size, 0) ||
        VoiceStoreCancelled(cancel) || !WinHttpReceiveResponse(request, nullptr))
        return false;
    DWORD status = 0, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        status != 200)
        return false;
    char buffer[16384];
    for (;;) {
        DWORD received = 0;
        if (VoiceStoreCancelled(cancel) || !WinHttpReadData(request, buffer, sizeof(buffer), &received)) return false;
        if (!received) return true;
        if (response.size() + received > 16 * 1024 * 1024) return false;
        response.Append(buffer, received);
    }
}

struct VoiceStoreJsonValue : json::ValueVisitor {
    const char* wanted;
    StrBuilder value;
    explicit VoiceStoreJsonValue(const char* wanted) : wanted(wanted) {}
    bool Visit(const char* path, const char* text, json::Type type) override {
        if (type == json::Type::String && str::Eq(path, wanted)) value.Set(text);
        return true;
    }
};

static bool VoiceStoreLoadXml(ScopedComPtr<IXMLDOMDocument2>& doc, const char* text) {
    if (!doc.Create(__uuidof(DOMDocument60))) return false;
    doc->put_async(VARIANT_FALSE);
    doc->put_validateOnParse(VARIANT_FALSE);
    doc->put_resolveExternals(VARIANT_FALSE);
    BSTR xml = SysAllocString(ToWStrTemp(text));
    if (!xml) return false;
    defer {
        SysFreeString(xml);
    };
    VARIANT_BOOL loaded = VARIANT_FALSE;
    return SUCCEEDED(doc->loadXML(xml, &loaded)) && loaded == VARIANT_TRUE;
}

static TempStr VoiceStoreXmlText(IXMLDOMNode* parent, const WCHAR* xpath) {
    BSTR query = SysAllocString(xpath);
    if (!query) return nullptr;
    defer {
        SysFreeString(query);
    };
    ScopedComPtr<IXMLDOMNode> node;
    if (FAILED(parent->selectSingleNode(query, &node)) || !node) return nullptr;
    BSTR text = nullptr;
    if (FAILED(node->get_text(&text)) || !text) return nullptr;
    defer {
        SysFreeString(text);
    };
    return ToUtf8Temp(text);
}

static bool VoiceStoreXmlNodes(IXMLDOMNode* parent, const WCHAR* xpath, ScopedComPtr<IXMLDOMNodeList>& nodes) {
    BSTR query = SysAllocString(xpath);
    if (!query) return false;
    defer {
        SysFreeString(query);
    };
    return SUCCEEDED(parent->selectNodes(query, &nodes)) && nodes;
}

static TempStr VoiceStoreTime(i64 seconds) {
    FILETIME fileTime;
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER ticks;
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    ticks.QuadPart += seconds * 10000000;
    fileTime.dwLowDateTime = ticks.LowPart;
    fileTime.dwHighDateTime = ticks.HighPart;
    SYSTEMTIME time;
    if (!FileTimeToSystemTime(&fileTime, &time)) return nullptr;
    return str::FormatTemp("%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth, time.wDay, time.wHour,
                           time.wMinute, time.wSecond, time.wMilliseconds);
}

static bool VoiceStoreIsCdn(const char* url) {
    return str::StartsWithI(url, "http://tlu.dl.delivery.mp.microsoft.com/") ||
           str::StartsWithI(url, "https://tlu.dl.delivery.mp.microsoft.com/");
}

// Only resolves the exact catalog package. Installation still checks its pinned size and SHA-256.
static TempStr ResolveVoiceStorePackage(const char* productId, const char* packageFile, HANDLE cancel) {
    if (str::IsEmpty(productId) || str::IsEmpty(packageFile) || VoiceStoreCancelled(cancel)) return nullptr;
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return nullptr;
    defer {
        if (SUCCEEDED(com)) CoUninitialize();
    };
    StrBuilder response;
    const WCHAR* productPath =
        ToWStrTemp(str::FormatTemp("/v9.0/products/%s?market=US&locale=en-us&deviceFamily=Windows.Desktop", productId));
    if (!VoiceStoreRequest(L"storeedgefd.dsx.mp.microsoft.com", productPath, nullptr, cancel, response)) return nullptr;
    VoiceStoreJsonValue fulfillment("/Payload/Skus[0]/FulfillmentData");
    if (!json::Parse(response.Get(), &fulfillment) || fulfillment.value.IsEmpty()) return nullptr;
    VoiceStoreJsonValue category("/WuCategoryId");
    if (!json::Parse(fulfillment.value.Get(), &category) || category.value.IsEmpty()) return nullptr;
    StrBuilder cookie(kVoiceStoreCookie), sync(kVoiceStoreSync), files(kVoiceStoreFiles);
    const char* created = VoiceStoreTime(0);
    const char* expires = VoiceStoreTime(300);
    StrBuilder* requests[] = {&cookie, &sync, &files};
    for (StrBuilder* request : requests) {
        VoiceStoreReplace(*request, "{Created}", created);
        VoiceStoreReplace(*request, "{ExpiresSync}", expires);
        VoiceStoreReplace(*request, "{ReleaseType}", "Retail");
        VoiceStoreReplace(*request, "{Locale}", "en-US");
        VoiceStoreReplace(*request, "{OSSkuId}", "48");
        VoiceStoreReplace(*request, "{OSVersion}", "10.0.16184.1001");
    }
    VoiceStoreReplace(cookie, "{ExpiresCookie}", VoiceStoreTime(27 * 86400));
    VoiceStoreReplace(cookie, "{LastChange}", VoiceStoreTime(-2 * 365 * 86400));
    VoiceStoreReplace(cookie, "{CurrentTime}", created);
    const WCHAR* host = L"fe3.delivery.mp.microsoft.com";
    const WCHAR* service = L"/ClientWebService/client.asmx";
    response.Reset();
    if (!VoiceStoreRequest(host, service, cookie.Get(), cancel, response)) return nullptr;
    ScopedComPtr<IXMLDOMDocument2> cookieDoc;
    if (!VoiceStoreLoadXml(cookieDoc, response.Get())) return nullptr;
    const char* cookieValue = VoiceStoreXmlText(cookieDoc, L"//*[local-name()='EncryptedData']");
    if (str::IsEmpty(cookieValue)) return nullptr;
    VoiceStoreReplace(sync, "{CookieValue}", cookieValue);
    VoiceStoreReplace(sync, "{WuCategoryId}", category.value.Get());
    response.Reset();
    if (!VoiceStoreRequest(host, service, sync.Get(), cancel, response)) return nullptr;
    // SyncUpdates contains escaped update fragments. Expand those fragments as in the Store protocol.
    VoiceStoreReplace(response, "&lt;", "<");
    VoiceStoreReplace(response, "&gt;", ">");
    ScopedComPtr<IXMLDOMDocument2> syncDoc;
    if (!VoiceStoreLoadXml(syncDoc, response.Get())) return nullptr;
    ScopedComPtr<IXMLDOMNodeList> fileNodes;
    if (!VoiceStoreXmlNodes(syncDoc, L"//*[local-name()='Files']", fileNodes)) return nullptr;
    StrBuilder selectedId;
    long count = 0;
    fileNodes->get_length(&count);
    for (long i = 0; i < count; i++) {
        ScopedComPtr<IXMLDOMNode> node;
        if (FAILED(fileNodes->get_item(i, &node)) || !node) continue;
        const char* installer = VoiceStoreXmlText(node, L"*[1]/@InstallerSpecificIdentifier");
        const char* filename = VoiceStoreXmlText(node, L"*[1]/@FileName");
        if (!installer || !filename || !str::Eq(str::FormatTemp("%s_%s", installer, filename), packageFile)) continue;
        const char* id = VoiceStoreXmlText(node, L"../../*[local-name()='ID']");
        if (id) selectedId.Set(id);
        break;
    }
    if (selectedId.IsEmpty()) return nullptr;
    ScopedComPtr<IXMLDOMNodeList> fragments;
    if (!VoiceStoreXmlNodes(syncDoc, L"//*[local-name()='SecuredFragment']", fragments)) return nullptr;
    fragments->get_length(&count);
    bool found = false;
    for (long i = 0; i < count; i++) {
        ScopedComPtr<IXMLDOMNode> node;
        if (FAILED(fragments->get_item(i, &node)) || !node) continue;
        const char* id = VoiceStoreXmlText(node, L"../../../*[local-name()='ID']");
        if (!str::Eq(id, selectedId.Get())) continue;
        const char* update = VoiceStoreXmlText(node, L"../../*[1]/@UpdateID");
        const char* revision = VoiceStoreXmlText(node, L"../../*[1]/@RevisionNumber");
        if (str::IsEmpty(update) || str::IsEmpty(revision)) continue;
        VoiceStoreReplace(files, "{UpdateID}", update);
        VoiceStoreReplace(files, "{RevisionNumber}", revision);
        found = true;
        break;
    }
    if (!found) return nullptr;
    response.Reset();
    if (!VoiceStoreRequest(host, L"/ClientWebService/client.asmx/secured", files.Get(), cancel, response))
        return nullptr;
    ScopedComPtr<IXMLDOMDocument2> urlsDoc;
    if (!VoiceStoreLoadXml(urlsDoc, response.Get())) return nullptr;
    ScopedComPtr<IXMLDOMNodeList> urls;
    if (!VoiceStoreXmlNodes(urlsDoc, L"//*[local-name()='FileLocation']/*[local-name()='Url']", urls)) return nullptr;
    urls->get_length(&count);
    for (long i = 0; i < count; i++) {
        ScopedComPtr<IXMLDOMNode> node;
        if (FAILED(urls->get_item(i, &node)) || !node) continue;
        const char* url = VoiceStoreXmlText(node, L".");
        if (VoiceStoreIsCdn(url)) return str::DupTemp(url);
    }
    return nullptr;
}
