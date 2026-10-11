/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#pragma once

#include <windows.h>
#include <commctrl.h>

// Measure the native control rather than guessing its thumb/channel margins.
// Query without repainting and restore the current user selection.
inline void PdfAdjustmentTrackbarCenters(HWND slider, int centers[3]) {
    int position = (int)SendMessageW(slider, TBM_GETPOS, 0, 0);
    int values[] = {(int)SendMessageW(slider, TBM_GETRANGEMIN, 0, 0), 0,
                    (int)SendMessageW(slider, TBM_GETRANGEMAX, 0, 0)};
    SendMessageW(slider, WM_SETREDRAW, FALSE, 0);
    // Force a position change before measuring the first endpoint: a repeated
    // TBM_SETPOS may leave the thumb rectangle cached after a range/size change.
    SendMessageW(slider, TBM_SETPOS, TRUE, values[2]);
    for (int i = 0; i < 3; i++) {
        SendMessageW(slider, TBM_SETPOS, TRUE, values[i]);
        RECT thumb{};
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        centers[i] = (thumb.left + thumb.right) / 2;
    }
    SendMessageW(slider, TBM_SETPOS, TRUE, position);
    SendMessageW(slider, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(slider, nullptr, FALSE);
}
