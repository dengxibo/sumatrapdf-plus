// Copyright 2026 Authors of SumatraPDF
// License: GPLv3
// Classic .doc → .docx via Microsoft Word Automation (read-only viewing).

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"
#include "OfficeConvert.h"

#include <ole2.h>

#ifndef wdFormatXMLDocument
#define wdFormatXMLDocument 12
#endif

// Keep conversions for all documents opened in this process, not only the last
// one. Serialize conversion so thumbnail/main-window requests cannot launch
// duplicate Office instances for the same source.
static SRWLOCK gOfficeConvertLock = SRWLOCK_INIT;
struct OfficeConvertGuard {
    OfficeConvertGuard() { AcquireSRWLockExclusive(&gOfficeConvertLock); }
    ~OfficeConvertGuard() { ReleaseSRWLockExclusive(&gOfficeConvertLock); }
};
struct OfficeConversion {
    char* source = nullptr;
    char* output = nullptr;
    FILETIME modified{};
    i64 size = 0;
};
static Vec<OfficeConversion> gOfficeConversions;

bool IsCachedOleOfficeDocx(const char* path) {
    OfficeConvertGuard guard;
    for (auto& entry : gOfficeConversions) {
        if (path && str::EqI(path, entry.output) && file::Exists(entry.output)) return true;
    }
    return false;
}

void ForgetCachedOleOfficeDocx(const char* srcPath) {
    OfficeConvertGuard guard;
    for (int i = 0; i < gOfficeConversions.Size(); i++) {
        auto& entry = gOfficeConversions[i];
        if (!srcPath || !str::EqI(srcPath, entry.source)) continue;
        str::Free(entry.source);
        str::Free(entry.output);
        gOfficeConversions.RemoveAt(i);
        return;
    }
}

static char* CachedOleDocx(const char* srcPath) {
    FILETIME modified = file::GetModificationTime(srcPath);
    i64 size = file::GetSize(srcPath);
    for (auto& entry : gOfficeConversions) {
        if (str::EqI(srcPath, entry.source) && entry.size == size && CompareFileTime(&modified, &entry.modified) == 0 &&
            file::Exists(entry.output)) {
            return str::Dup(entry.output);
        }
    }
    return nullptr;
}

static void RememberOleDocx(const char* srcPath, const char* dstPath) {
    OfficeConversion entry;
    entry.source = str::Dup(srcPath);
    entry.output = str::Dup(dstPath);
    entry.modified = file::GetModificationTime(srcPath);
    entry.size = file::GetSize(srcPath);
    gOfficeConversions.Append(entry);
}
static void SetErr(char* errOut, int errOutLen, const char* msg) {
    if (errOut && errOutLen > 0 && msg) {
        str::BufSet(errOut, errOutLen, msg);
    }
}

static bool InvokePutBool(IDispatch* obj, const WCHAR* name, bool v) {
    DISPID id = 0;
    LPOLESTR n = (LPOLESTR)name;
    if (FAILED(obj->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
        return false;
    }
    VARIANT val;
    VariantInit(&val);
    val.vt = VT_BOOL;
    val.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    DISPPARAMS dp{};
    dp.cArgs = 1;
    dp.rgvarg = &val;
    DISPID named = DISPID_PROPERTYPUT;
    dp.cNamedArgs = 1;
    dp.rgdispidNamedArgs = &named;
    HRESULT hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr, nullptr);
    VariantClear(&val);
    return SUCCEEDED(hr);
}

static bool InvokePutI4(IDispatch* obj, const WCHAR* name, LONG v) {
    DISPID id = 0;
    LPOLESTR n = (LPOLESTR)name;
    if (FAILED(obj->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
        return false;
    }
    VARIANT val;
    VariantInit(&val);
    val.vt = VT_I4;
    val.lVal = v;
    DISPPARAMS dp{};
    dp.cArgs = 1;
    dp.rgvarg = &val;
    DISPID named = DISPID_PROPERTYPUT;
    dp.cNamedArgs = 1;
    dp.rgdispidNamedArgs = &named;
    HRESULT hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr, nullptr);
    VariantClear(&val);
    return SUCCEEDED(hr);
}

static IDispatch* InvokeGetDispatch(IDispatch* obj, const WCHAR* name) {
    DISPID id = 0;
    LPOLESTR n = (LPOLESTR)name;
    if (FAILED(obj->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
        return nullptr;
    }
    VARIANT resultV;
    VariantInit(&resultV);
    DISPPARAMS dp{};
    HRESULT hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &dp, &resultV, nullptr, nullptr);
    IDispatch* out = nullptr;
    if (SUCCEEDED(hr) && resultV.vt == VT_DISPATCH && resultV.pdispVal) {
        out = resultV.pdispVal;
        resultV.pdispVal = nullptr;
    }
    VariantClear(&resultV);
    return out;
}

char* ConvertOleOfficeToDocx(const char* srcPath, char* errOut, int errOutLen) {
    if (errOut && errOutLen > 0) {
        errOut[0] = 0;
    }
    if (!srcPath || !srcPath[0] || !file::Exists(srcPath)) {
        SetErr(errOut, errOutLen, "source missing");
        return nullptr;
    }

    OfficeConvertGuard guard;
    ULONGLONG started = GetTickCount64();
    if (char* cached = CachedOleDocx(srcPath)) {
        logf("OfficeConvert: cache hit %s -> %s\n", srcPath, cached);
        return cached;
    }

    const char* ext = path::GetExtTemp(srcPath);
    bool presentation = str::EqI(ext, ".ppt") || str::EqI(ext, ".pptx") || str::EqI(ext, ".dps");
    // Preserve WPS's page layout. Its fixed line spacing and positioned text
    // do not survive the DOCX-to-HTML reflow path reliably.
    bool fixedLayout = presentation || str::EqI(ext, ".wps");
    bool comOwned = false;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hrInit)) {
        comOwned = true;
    } else if (hrInit != RPC_E_CHANGED_MODE && hrInit != S_FALSE) {
        SetErr(errOut, errOutLen, "COM init failed");
        return nullptr;
    }

    WCHAR srcW[MAX_PATH]{};
    if (!MultiByteToWideChar(CP_UTF8, 0, srcPath, -1, srcW, dimof(srcW))) {
        SetErr(errOut, errOutLen, "path utf8");
        if (comOwned) {
            CoUninitialize();
        }
        return nullptr;
    }

    TempStr tmpDir = GetTempDirTemp();
    if (!tmpDir) {
        SetErr(errOut, errOutLen, "no temp");
        if (comOwned) {
            CoUninitialize();
        }
        return nullptr;
    }
    TempStr base = path::GetBaseNameTemp(srcPath);
    // Avoid "foo.doc.docx" looking odd: strip trailing .doc from basename for dest name.
    TempStr stem = base;
    if (str::EndsWithI(stem, ".doc")) {
        stem = str::DupTemp(stem);
        stem[str::Len(stem) - 4] = 0;
    }
    TempStr destA = path::JoinTemp(tmpDir, str::FormatTemp("smp-office-%u-%s.%s", (unsigned)GetTickCount(),
                                                           stem ? stem : "office", fixedLayout ? "pdf" : "docx"));
    WCHAR destW[MAX_PATH]{};
    if (!MultiByteToWideChar(CP_UTF8, 0, destA, -1, destW, dimof(destW))) {
        SetErr(errOut, errOutLen, "dest utf8");
        if (comOwned) {
            CoUninitialize();
        }
        return nullptr;
    }

    IDispatch* app = nullptr;
    IDispatch* docs = nullptr;
    IDispatch* doc = nullptr;
    char* result = nullptr;

    CLSID clsid{};
    HRESULT hr = E_FAIL;
    const WCHAR* progIds[] = {presentation ? L"PowerPoint.Application" : L"Word.Application",
                              presentation ? L"Kwpp.Application" : L"Kwps.Application",
                              presentation ? L"Wpp.Application" : L"Wps.Application"};
    // Native WPS formats should not first cold-start Microsoft Office and
    // run its foreign-format importer when WPS automation is available.
    if (str::EqI(ext, ".wps") || str::EqI(ext, ".dps")) {
        const WCHAR* microsoft = progIds[0];
        progIds[0] = progIds[1];
        progIds[1] = progIds[2];
        progIds[2] = microsoft;
    }
    for (const WCHAR* progId : progIds) {
        if (SUCCEEDED(CLSIDFromProgID(progId, &clsid))) {
            hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch, (void**)&app);
            if (SUCCEEDED(hr) && app) break;
        }
    }
    if (FAILED(hr) || !app) {
        SetErr(errOut, errOutLen, "Microsoft Word or WPS Writer automation unavailable");
        goto cleanup;
    }

    InvokePutBool(app, L"Visible", false);
    InvokePutBool(app, L"ScreenUpdating", false);
    InvokePutI4(app, L"DisplayAlerts", 0);
    InvokePutI4(app, L"AutomationSecurity", 3);
    logf("OfficeConvert: startup %llu ms\n", GetTickCount64() - started);

    docs = InvokeGetDispatch(app, presentation ? L"Presentations" : L"Documents");
    if (!docs) {
        SetErr(errOut, errOutLen, "Documents missing");
        goto cleanup;
    }

    // Documents.Open(FileName) — one arg avoids ConfirmConversions/ReadOnly slot mistakes.
    {
        DISPID id = 0;
        LPOLESTR n = (LPOLESTR)L"Open";
        if (FAILED(docs->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
            SetErr(errOut, errOutLen, "Open missing");
            goto cleanup;
        }
        VARIANT arg;
        VariantInit(&arg);
        arg.vt = VT_BSTR;
        arg.bstrVal = SysAllocString(srcW);
        VARIANT openArgs[4]{};
        int count = presentation ? 4 : 3;
        openArgs[count - 1] = arg;
        for (int i = 0; i < count - 1; i++) {
            openArgs[i].vt = presentation ? VT_I4 : VT_BOOL;
        }
        // PowerPoint: FileName, ReadOnly, Untitled, WithWindow.
        // Word/WPS Writer: FileName, ConfirmConversions, ReadOnly.
        if (presentation)
            openArgs[2].lVal = -1;
        else
            openArgs[0].boolVal = VARIANT_TRUE;
        DISPPARAMS dp{};
        dp.cArgs = count;
        dp.rgvarg = openArgs;
        VARIANT resultV;
        VariantInit(&resultV);
        hr = docs->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &resultV, nullptr, nullptr);
        if (arg.bstrVal) {
            SysFreeString(arg.bstrVal);
        }
        if (FAILED(hr) || resultV.vt != VT_DISPATCH || !resultV.pdispVal) {
            VariantClear(&resultV);
            SetErr(errOut, errOutLen, "Word Open failed");
            goto cleanup;
        }
        doc = resultV.pdispVal;
        logf("OfficeConvert: opened at %llu ms\n", GetTickCount64() - started);
    }

    // Document.SaveAs2(FileName, FileFormat:=12) — args reverse order for Invoke.
    {
        DISPID id = 0;
        LPOLESTR n = (LPOLESTR)(presentation ? L"SaveAs" : L"SaveAs2");
        if (FAILED(doc->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
            n = (LPOLESTR)L"SaveAs";
            if (FAILED(doc->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
                SetErr(errOut, errOutLen, "SaveAs missing");
                goto cleanup;
            }
        }
        VARIANT args[2];
        VariantInit(&args[0]);
        VariantInit(&args[1]);
        args[1].vt = VT_BSTR;
        args[1].bstrVal = SysAllocString(destW);
        args[0].vt = VT_I4;
        args[0].lVal = presentation ? 32 : fixedLayout ? 17 : wdFormatXMLDocument;
        DISPPARAMS dp{};
        dp.cArgs = 2;
        dp.rgvarg = args;
        hr = doc->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, nullptr, nullptr, nullptr);
        if (args[1].bstrVal) {
            SysFreeString(args[1].bstrVal);
        }
        VariantClear(&args[0]);
        logf("OfficeConvert: exported at %llu ms\n", GetTickCount64() - started);
        if (FAILED(hr)) {
            SetErr(errOut, errOutLen, "SaveAs docx failed");
            goto cleanup;
        }
    }

    {
        DISPID id = 0;
        LPOLESTR n = (LPOLESTR)L"Close";
        if (SUCCEEDED(doc->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
            VARIANT arg;
            VariantInit(&arg);
            arg.vt = VT_BOOL;
            arg.boolVal = VARIANT_FALSE;
            DISPPARAMS dp{};
            dp.cArgs = presentation ? 0 : 1;
            dp.rgvarg = presentation ? nullptr : &arg;
            doc->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, nullptr, nullptr, nullptr);
            VariantClear(&arg);
        }
    }

    if (file::Exists(destA)) {
        result = str::Dup(destA);
        RememberOleDocx(srcPath, result);
    } else {
        SetErr(errOut, errOutLen, "docx not written");
    }

cleanup:
    if (doc) {
        doc->Release();
    }
    if (docs) {
        docs->Release();
    }
    if (app) {
        DISPID id = 0;
        LPOLESTR n = (LPOLESTR)L"Quit";
        if (SUCCEEDED(app->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
            DISPPARAMS dp{};
            app->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, nullptr, nullptr, nullptr);
        }
        app->Release();
    }
    if (comOwned) {
        CoUninitialize();
    }
    if (result) {
        logf("OfficeConvert: %s -> %s\n", srcPath, result);
    } else if (errOut && errOut[0]) {
        logf("OfficeConvert failed (%s): %s\n", errOut, srcPath);
    }
    return result;
}
