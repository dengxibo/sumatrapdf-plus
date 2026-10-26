/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Debug-build integration test: Canvas.cpp intentionally disables the custom
// document UIA provider in release builds. Only drives the child it starts, with
// an isolated -appdata directory. Two MTA clients retain document/text providers
// while the child's STA switches tabs, closes documents and returns to Home.
// Usage: test_uia_lifecycle.exe exe appdata pdf1 pdf2 nextCmd prevCmd closeCmd rounds
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <stdio.h>
#include <stdlib.h>

template <typename T>
static void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

struct Probe {
    HWND frame = nullptr;
    HANDLE stop = nullptr;
    IUIAutomation* uia = nullptr;
    IUIAutomationCondition* condition = nullptr;
    volatile LONG calls = 0;
    volatile LONG ranges = 0;
    volatile LONG disconnected = 0;
    volatile LONG stage = 0;
    volatile LONG documents = 0;
    volatile LONG patterns = 0;
    volatile LONG lastResult = 0;
};

static DWORD WINAPI ReadDocument(void* arg) {
    auto* p = (Probe*)arg;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        fwprintf(stderr, L"CoInitializeEx failed: %08lx\n", (unsigned long)initialized);
        return 1;
    }
    // All readers belong to the same MTA. Initialize the shared UIA client
    // once, before starting concurrent provider queries.
    IUIAutomation* uia = p->uia;
    IUIAutomationCondition* condition = p->condition;
    uia->AddRef();
    condition->AddRef();
    HRESULT hr = S_OK;
    InterlockedExchange(&p->stage, 1);
    while (SUCCEEDED(hr) && WaitForSingleObject(p->stop, 0) == WAIT_TIMEOUT) {
        IUIAutomationElement* root = nullptr;
        IUIAutomationElement* document = nullptr;
        IUIAutomationTextPattern* text = nullptr;
        if (SUCCEEDED(uia->ElementFromHandle(p->frame, &root)) && root) {
            InterlockedExchange(&p->stage, 2);
            InterlockedExchange(&p->lastResult, root->FindFirst(TreeScope_Descendants, condition, &document));
            InterlockedExchange(&p->stage, 3);
        }
        if (document) {
            InterlockedIncrement(&p->documents);
            InterlockedExchange(&p->lastResult, document->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text)));
            if (text) InterlockedIncrement(&p->patterns);
        }
        // Keep these interfaces after a tab switch; an unavailable-element
        // response is expected. A use-after-free or a hung STA is not.
        for (int i = 0; text && i < 12 && WaitForSingleObject(p->stop, 0) == WAIT_TIMEOUT; i++) {
            IUIAutomationTextRange* range = nullptr;
            HRESULT result = text->get_DocumentRange(&range);
            InterlockedExchange(&p->stage, 4);
            InterlockedIncrement(&p->calls);
            if (SUCCEEDED(result) && range) {
                BSTR value = nullptr;
                result = range->GetText(256, &value);
                InterlockedExchange(&p->stage, 5);
                SysFreeString(value);
                if (SUCCEEDED(result)) InterlockedIncrement(&p->ranges);
            }
            if (FAILED(result)) InterlockedIncrement(&p->disconnected);
            Release(range);
            WaitForSingleObject(p->stop, 2);
        }
        Release(text);
        Release(document);
        Release(root);
        WaitForSingleObject(p->stop, 2);
    }
    Release(condition);
    Release(uia);
    CoUninitialize();
    return FAILED(hr) ? 1 : 0;
}

struct ChildWindows {
    DWORD pid;
    HWND frame = nullptr;
    bool crashed = false;
};

static BOOL CALLBACK FindChildWindow(HWND hwnd, LPARAM arg) {
    auto* child = (ChildWindows*)arg;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != child->pid) return TRUE;
    WCHAR title[128]{};
    WCHAR cls[64]{};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"SUMATRA_PDF_FRAME") == 0) child->frame = hwnd;
    if (wcscmp(title, L"SumatraPDF crashed") == 0) child->crashed = true;
    return TRUE;
}

int wmain(int argc, WCHAR** argv) {
    if (argc != 9) {
        fwprintf(stderr, L"Usage: test_uia_lifecycle.exe exe appdata pdf1 pdf2 nextCmd prevCmd closeCmd rounds\n");
        return 2;
    }
    WCHAR command[8192]{};
    swprintf_s(command, L"\"%s\" -appdata \"%s\" -new-window \"%s\" \"%s\"", argv[1], argv[2], argv[3], argv[4]);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[1], command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return 3;
    CloseHandle(process.hThread);
    ChildWindows child{process.dwProcessId};
    for (int i = 0; i < 150 && !child.frame; i++) {
        EnumWindows(FindChildWindow, (LPARAM)&child);
        if (WaitForSingleObject(process.hProcess, 100) != WAIT_TIMEOUT) break;
    }
    if (!child.frame) {
        TerminateProcess(process.hProcess, 4);
        CloseHandle(process.hProcess);
        return 4;
    }
    Sleep(1500);
    WCHAR title[256]{};
    GetWindowTextW(child.frame, title, ARRAYSIZE(title));
    wprintf(L"Testing child: %s\n", title);
    fflush(stdout);
    Probe probe{child.frame, CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = initialized;
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&probe.uia));
    }
    VARIANT type{};
    type.vt = VT_I4;
    type.lVal = UIA_DocumentControlTypeId;
    if (SUCCEEDED(hr)) hr = probe.uia->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &probe.condition);
    if (FAILED(hr)) {
        fwprintf(stderr, L"UIA initialization failed: %08lx\n", (unsigned long)hr);
        Release(probe.condition);
        Release(probe.uia);
        if (SUCCEEDED(initialized)) CoUninitialize();
        CloseHandle(probe.stop);
        TerminateProcess(process.hProcess, 6);
        CloseHandle(process.hProcess);
        return 6;
    }
    HANDLE readers[2] = {CreateThread(nullptr, 0, ReadDocument, &probe, 0, nullptr),
                         CreateThread(nullptr, 0, ReadDocument, &probe, 0, nullptr)};
    // Wait for a real document-range read before starting the race. UIA's
    // first provider discovery can be slower on a cold start.
    for (int i = 0; i < 200 && probe.ranges == 0; i++) Sleep(25);
    bool failed = !probe.stop || !readers[0] || !readers[1];
    int rounds = _wtoi(argv[8]);
    for (int i = 0; i < rounds && !failed; i++) {
        PostMessageW(child.frame, WM_COMMAND, _wtoi(argv[(i & 1) ? 5 : 6]), 0);
        if (WaitForSingleObject(process.hProcess, 80) != WAIT_TIMEOUT) {
            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            wprintf(L"Child exited during switch %d with code %lu.\n", i, code);
            failed = true;
        }
        EnumWindows(FindChildWindow, (LPARAM)&child);
        failed |= child.crashed;
    }
    if (!failed) {
        PostMessageW(child.frame, WM_COMMAND, _wtoi(argv[7]), 0);
        Sleep(500);
        // Keep the other document/window alive until the readers release their
        // retained interfaces. Process teardown can otherwise block UIA's RPC
        // cleanup and obscure the document-unload regression being tested.
        EnumWindows(FindChildWindow, (LPARAM)&child);
        failed = child.crashed;
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(process.hProcess, &exitCode);
            wprintf(L"Child exit after closing documents: %lu.\n", exitCode);
            failed |= exitCode != 0;
        }
    }
    SetEvent(probe.stop);
    for (HANDLE reader : readers) {
        if (!reader) continue;
        if (WaitForSingleObject(reader, 15000) != WAIT_OBJECT_0) {
            fwprintf(stderr, L"UIA client did not finish within 15 seconds.\n");
            fwprintf(stderr, L"stage=%ld calls=%ld ranges=%ld disconnected=%ld crashed=%d\n", probe.stage, probe.calls,
                     probe.ranges, probe.disconnected, child.crashed);
            // Exit the harness process below after stopping its child; never
            // return while a worker still owns a pointer to the stack probe.
            TerminateProcess(process.hProcess, 5);
            ExitProcess(5);
        }
        DWORD result = 0;
        GetExitCodeThread(reader, &result);
        if (result) wprintf(L"UIA reader returned %lu.\n", result);
        failed |= result != 0;
        CloseHandle(reader);
    }
    CloseHandle(probe.stop);
    Release(probe.condition);
    Release(probe.uia);
    CoUninitialize();
    failed |= probe.ranges == 0;
    wprintf(L"calls=%ld successful ranges=%ld disconnected=%ld crashed=%d result=%s\n", probe.calls, probe.ranges,
            probe.disconnected, child.crashed, failed ? L"FAIL" : L"PASS");
    wprintf(L"stage=%ld documents=%ld patterns=%ld last HRESULT=%08lx\n", probe.stage, probe.documents, probe.patterns,
            (unsigned long)probe.lastResult);
    PostMessageW(child.frame, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, failed ? 1 : 0);
    CloseHandle(process.hProcess);
    return failed ? 1 : 0;
}
