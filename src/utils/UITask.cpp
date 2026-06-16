/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/UITask.h"

namespace uitask {

static HWND gTaskDispatchHwnd = nullptr;

UINT gExecuteTaskMessage = 0;

static DWORD gMainUIThreadId = 0;

static LRESULT CALLBACK WndProcTaskDispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (gExecuteTaskMessage == msg) {
        Kind kind = (Kind)wp;
        auto func = (Func0*)lp;
        func->Call();
        delete func;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

constexpr const WCHAR* UITASK_CLASS_NAME = L"UITask_Wnd_Class";

void Initialize() {
    gMainUIThreadId = GetCurrentThreadId();

    ReportIf(gExecuteTaskMessage != 0);
    gExecuteTaskMessage = RegisterWindowMessageA("UITask_Msg_StdFunction");
    WNDCLASSEX wcex;
    FillWndClassEx(wcex, UITASK_CLASS_NAME, WndProcTaskDispatch);
    RegisterClassEx(&wcex);

    ReportIf(gTaskDispatchHwnd);
    auto cls = UITASK_CLASS_NAME;
    auto title = L"UITask Dispatch Window";
    auto m = GetModuleHandleW(nullptr);
    DWORD style = WS_OVERLAPPED;
    gTaskDispatchHwnd = CreateWindowExW(0, cls, title, style, 0, 0, 0, 0, HWND_MESSAGE, nullptr, m, nullptr);
}

static void DrainQueueAll() {
    ReportIf(!gTaskDispatchHwnd);
    MSG msg;
    UINT wmExecTask = gExecuteTaskMessage;
    while (PeekMessage(&msg, gTaskDispatchHwnd, wmExecTask, wmExecTask, PM_REMOVE)) {
        DispatchMessage(&msg);
    }
}

void DrainQueue() {
    ReportIf(!gTaskDispatchHwnd);
    MSG msg;
    UINT wmExecTask = gExecuteTaskMessage;
    // Process one task per message-loop turn so long chains (e.g. progressive
    // page-layout batches) yield back to user input and painting between steps.
    if (PeekMessage(&msg, gTaskDispatchHwnd, wmExecTask, wmExecTask, PM_REMOVE)) {
        DispatchMessage(&msg);
    }
}

void Destroy() {
    DrainQueueAll();
    DestroyWindow(gTaskDispatchHwnd);
    gTaskDispatchHwnd = nullptr;
}

void Post(const Func0& f, Kind kind) {
    auto func = new Func0(f);
    PostMessageW(gTaskDispatchHwnd, gExecuteTaskMessage, (WPARAM)kind, (LPARAM)func);
} // NOLINT

bool IsMainUIThread() {
    return GetCurrentThreadId() == gMainUIThreadId;
}

void PostOptimized(const Func0& f, Kind kind) {
    if (IsMainUIThread()) {
        // if we're already on ui thread, execute immediately
        // faster and easier to debug
        f.Call();
        return;
    }
    Post(f, kind);
} // NOLINT

void Invoke(const Func0& f, Kind kind) {
    if (IsMainUIThread()) {
        f.Call();
        return;
    }
    auto func = new Func0(f);
    SendMessageW(gTaskDispatchHwnd, gExecuteTaskMessage, (WPARAM)kind, (LPARAM)func);
} // NOLINT

} // namespace uitask
