#include "../src/PdfAdjustmentTrackbar.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    assert(InitCommonControlsEx(&controls));
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 900, 900, nullptr, nullptr, nullptr, nullptr);
    assert(parent);
    int scales[] = {100, 125, 150, 200};
    for (int scale : scales) {
        HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ, 0, 0, 350 * scale / 100,
                                      32 * scale / 100, parent, nullptr, nullptr, nullptr);
        assert(slider);
        SendMessageW(slider, TBM_SETRANGEMIN, FALSE, -500);
        SendMessageW(slider, TBM_SETRANGEMAX, FALSE, 500);
        SendMessageW(slider, TBM_CLEARTICS, FALSE, 0);
        SendMessageW(slider, TBM_SETTIC, 0, 0);
        int selections[] = {-500, -123, 0, 123, 500};
        for (int selection : selections) {
            SendMessageW(slider, TBM_SETPOS, FALSE, selection);
            int centers[3]{};
            PdfAdjustmentTrackbarCenters(slider, centers);
            assert(SendMessageW(slider, TBM_GETPOS, 0, 0) == selection);
            assert(centers[0] < centers[1] && centers[1] < centers[2]);
            assert(abs(centers[0] + centers[2] - 2 * centers[1]) <= 1);
            // Native tick coordinates may truncate an odd-sized thumb by one
            // pixel; the text uses the measured thumb center itself.
            assert(SendMessageW(slider, TBM_GETTIC, 0, 0) == 0);
            assert(abs(centers[1] - (int)SendMessageW(slider, TBM_GETTICPOS, 0, 0)) <= 1);
            SendMessageW(slider, TBM_SETPOS, TRUE, 500);
            SendMessageW(slider, TBM_SETPOS, TRUE, 0);
            RECT zeroThumb{};
            SendMessageW(slider, TBM_GETTHUMBRECT, 0, (LPARAM)&zeroThumb);
            assert(centers[1] == (zeroThumb.left + zeroThumb.right) / 2);
        }
        DestroyWindow(slider);
    }
    DestroyWindow(parent);
    puts("Native trackbar: endpoint/zero centers and preserved selection passed at four layout scales");
}
