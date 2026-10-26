/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#pragma once

static LRESULT CALLBACK ToolbarPageTextSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int dx = rc.right, dy = rc.bottom;
        if (dx > 0 && dy > 0) {
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP bitmap = buffer ? CreateCompatibleBitmap(dc, dx, dy) : nullptr;
            if (bitmap) {
                HGDIOBJ old = SelectObject(buffer, bitmap);
                // Keep the visible text until background and glyphs are ready.
                DefSubclassProc(hwnd, WM_PRINTCLIENT, (WPARAM)buffer, PRF_CLIENT | PRF_ERASEBKGND);
                BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                       ps.rcPaint.bottom - ps.rcPaint.top, buffer, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                SelectObject(buffer, old);
                DeleteObject(bitmap);
            } else {
                DefSubclassProc(hwnd, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
            }
            if (buffer) DeleteDC(buffer);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ToolbarPageTextSubclassProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
