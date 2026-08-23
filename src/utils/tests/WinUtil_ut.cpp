/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

// must be last due to assert() over-write
#include "utils/UtAssert.h"

void WinUtilTest() {
    ScopedCom comScope;

    {
        const char* string = "abcde";
        size_t stringSize = str::Len(string);
        auto strm = CreateStreamFromData({(u8*)string, stringSize});
        ScopedComPtr<IStream> stream(strm);
        utassert(stream);
        ByteSlice data = GetDataFromStream(stream, nullptr);
        utassert(data.Get());
        utassert(stringSize == data.size());
        const char* s = data;
        utassert(str::Eq(s, string));
        data.Free();
    }

    {
        const WCHAR* string = L"abcde";
        size_t stringSize = str::Len(string) * sizeof(WCHAR);
        auto strm = CreateStreamFromData({(u8*)string, stringSize});
        ScopedComPtr<IStream> stream(strm);
        utassert(stream);
        ByteSlice dataTmp = GetDataFromStream(stream, nullptr);
        WCHAR* data = (WCHAR*)dataTmp.Get();
        utassert(data && stringSize == dataTmp.size() && str::Eq(data, string));
        dataTmp.Free();
    }

    {
        Rect oneScreen = GetFullscreenRect(nullptr);
        Rect allScreens = GetVirtualScreenRect();
        utassert(allScreens.Intersect(oneScreen) == oneScreen);
    }

    {
        // White paper remapped to Dracula must stay #282A36, not flatten to gray/black.
        HBITMAP hbmp = CreateMemoryBitmap(Size(2, 1));
        utassert(hbmp);
        BitmapPixels* px = GetBitmapPixels(hbmp);
        utassert(px && px->pixels && px->nBytesPerPixel == 4);
        px->pixels[0] = 255;
        px->pixels[1] = 255;
        px->pixels[2] = 255;
        px->pixels[3] = 255;
        px->pixels[4] = 0;
        px->pixels[5] = 0;
        px->pixels[6] = 0;
        px->pixels[7] = 255;
        FinalizeBitmapPixels(px);

        COLORREF text = RGB(0xF8, 0xF8, 0xF2);
        COLORREF bg = RGB(0x28, 0x2A, 0x36);
        UpdateBitmapColors(hbmp, text, bg, 0, nullptr);

        px = GetBitmapPixels(hbmp);
        utassert(px);
        COLORREF paper = GetPixel(px, 0, 0);
        COLORREF ink = GetPixel(px, 1, 0);
        FinalizeBitmapPixels(px);
        DeleteObject(hbmp);

        utassert(GetRValue(paper) == 0x28 && GetGValue(paper) == 0x2A && GetBValue(paper) == 0x36);
        utassert(GetRValue(ink) == 0xF8 && GetGValue(ink) == 0xF8 && GetBValue(ink) == 0xF2);
    }

    // TODO: moved AdjustLigthness() to Colors.[h|cpp] which is outside of utils directory
#if 0
    {
        COLORREF c = AdjustLightness(RGB(255, 0, 0), 1.0f);
        utassert(c == RGB(255, 0, 0));
        c = AdjustLightness(RGB(255, 0, 0), 2.0f);
        utassert(c == RGB(255, 255, 255));
        c = AdjustLightness(RGB(255, 0, 0), 0.25f);
        utassert(c == RGB(64, 0, 0));
        c = AdjustLightness(RGB(226, 196, 226), 95 / 255.0f);
        utassert(c == RGB(105, 52, 105));
        c = AdjustLightness(RGB(255, 255, 255), 0.5f);
        utassert(c == RGB(128, 128, 128));
    }
#endif
}
