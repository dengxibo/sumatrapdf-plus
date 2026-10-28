/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/WinDynCalls.h"
#include "utils/Dpi.h"
#include "utils/GdiPlusUtil.h"
#include "utils/GuessFileType.h"
#include "utils/ThreadUtil.h"
#include "FzImgReader.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "AppDialogTheme.h"
#include "GlobalPrefs.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DocProperties.h"
#include "EngineAll.h"
#include "PdfCreator.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "SumatraConfig.h"
#include "Theme.h"
#include "Translations.h"
#include "ImageSaveCropResize.h"
#include "SumatraDialogs.h"
#include "resource.h"

#include "utils/Log.h"

#include <wincodec.h>

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::Ok;
using Gdiplus::Status;

constexpr const WCHAR* kImageEditWinClassName = L"SUMATRA_PDF_IMAGE_EDIT";

constexpr int kMinWindowWidth = 640;
constexpr int kImagePadding = 16;
constexpr int kResizeEdgeThreshold = 6;
constexpr int kDragHandleSize = 6;
constexpr int kRowPadding = 6;
constexpr int kButtonPadding = 8;

struct ImageFormat {
    const char* label;
    const GUID* containerFormat; // WIC container format GUID (null for PDF)
    const char* ext;
    bool needsProbe; // if true, check if encoder is available before offering
    bool available;  // set after probing
    bool isPdf;      // PDF is created via PdfCreator, not WIC
};

// clang-format off
static ImageFormat gImageFormats[] = {
    {"PNG",  &GUID_ContainerFormatPng,  ".png",  false, true, false},
    {"JPEG", &GUID_ContainerFormatJpeg, ".jpg",  false, true, false},
    {"BMP",  &GUID_ContainerFormatBmp,  ".bmp",  false, true, false},
    {"GIF",  &GUID_ContainerFormatGif,  ".gif",  false, true, false},
    {"TIFF", &GUID_ContainerFormatTiff, ".tif",  false, true, false},
    {"WebP", &GUID_ContainerFormatWebp, ".webp", true,  false, false},
    {"PDF",  nullptr,                   ".pdf",  false, true, true},
};
// clang-format on

constexpr int kPdfFormatIdx = 6;

constexpr int kDefaultFormatIdx = 0; // PNG

static bool gFormatsProbed = false;

static void ProbeImageFormats() {
    if (gFormatsProbed) {
        return;
    }
    gFormatsProbed = true;

    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                  (void**)&pFactory);
    if (FAILED(hr) || !pFactory) {
        return;
    }

    for (auto& fmt : gImageFormats) {
        if (!fmt.needsProbe) {
            continue;
        }
        IWICBitmapEncoder* pEncoder = nullptr;
        hr = pFactory->CreateEncoder(*fmt.containerFormat, nullptr, &pEncoder);
        if (SUCCEEDED(hr) && pEncoder) {
            fmt.available = true;
            pEncoder->Release();
        }
    }

    pFactory->Release();
}

enum class DragEdge {
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Draw,
    Move // only used in crop mode
};

struct ImageEditWindow {
    ImageEditMode mode = ImageEditMode::Crop;
    bool fromRenderedBitmap = false;

    HWND hwnd = nullptr;
    HWND hwndParent = nullptr;

    // child controls
    HWND hwndPathLabel = nullptr;
    HWND hwndDestEdit = nullptr;
    HWND hwndBrowseBtn = nullptr;
    HWND hwndOpenAfterSave = nullptr;
    Rect hintRc;
    // Size line is not a control. A STATIC (even a hidden one) plus the window
    // background brush paints a second plate under the glyphs.
    char* infoText = nullptr;
    Rect infoRc;
    HBRUSH chromeBrush = nullptr;
    COLORREF chromeColor = 0;
    Button* btnSave = nullptr;
    Button* btnCrop = nullptr;   // "Crop" or "Apply Crop"
    Button* btnResize = nullptr; // "Resize" or "Apply Resize"
    Button* btnRotateLeft = nullptr;
    Button* btnRotateRight = nullptr;
    DropDown* dropFormat = nullptr;
    Vec<int> formatIndices; // maps dropdown index to gImageFormats index

    // source image
    char* filePath = nullptr;
    Bitmap* srcBitmap = nullptr;
    int imgW = 0;
    int imgH = 0;

    // display
    int imgDisplayX = 0;
    int imgDisplayY = 0;
    int imgDisplayW = 0;
    int imgDisplayH = 0;
    int imgAreaH = 0; // height of the image area (window height - control area)

    // crop rectangle in image coordinates (crop mode)
    int cropX = 0;
    int cropY = 0;
    int cropW = 0;
    int cropH = 0;

    // new size in image coordinates (resize mode)
    int newW = 0;
    int newH = 0;

    // drag state
    bool isDragging = false;
    bool dragWithMiddleButton = false;
    DragEdge dragEdge = DragEdge::None;
    DragEdge hoverEdge = DragEdge::None; // edge under mouse, for arrow key nudging
    POINT dragStart{};
    // crop mode drag state
    int dragCropX = 0;
    int dragCropY = 0;
    int dragCropW = 0;
    int dragCropH = 0;
    // resize mode drag state
    int dragNewW = 0;
    int dragNewH = 0;

    HFONT hFont = nullptr;
    int dpi = 96;
    bool fontOwned = false;
    AppDialogBrushes brushes;

    // Signature photos: Apply Crop returns the bitmap and closes this window.
    ImageCropDoneFn cropDone = nullptr;
    void* cropDoneUser = nullptr;

    ImageEditWindow() = default;
    ~ImageEditWindow() {
        delete srcBitmap;
        free(filePath);
        str::Free(infoText);
        if (chromeBrush) {
            DeleteObject(chromeBrush);
        }
        delete btnSave;
        delete btnCrop;
        delete btnResize;
        delete btnRotateLeft;
        delete btnRotateRight;
        delete dropFormat;
        if (fontOwned && hFont) {
            DeleteObject(hFont);
        }
        brushes.Destroy();
    }
};

static int ImgPx(const ImageEditWindow* ew, int x) {
    int dpi = (ew && ew->dpi >= 72) ? ew->dpi : 96;
    return MulDiv(x, dpi, 96);
}

static void ApplyImageFont(ImageEditWindow* ew, int dpi) {
    if (!ew || dpi < 72 || (ew->dpi == dpi && ew->hFont)) {
        return;
    }
    AppDialogFonts fonts;
    fonts.CreateForDpi(dpi);
    if (!fonts.body) {
        return;
    }
    HFONT prev = ew->fontOwned ? ew->hFont : nullptr;
    ew->hFont = fonts.body;
    fonts.body = nullptr;
    ew->fontOwned = true;
    ew->dpi = dpi;
    if (ew->hwndPathLabel) {
        SendMessageW(ew->hwndPathLabel, WM_SETFONT, (WPARAM)ew->hFont, FALSE);
    }
    if (ew->hwndDestEdit) {
        SendMessageW(ew->hwndDestEdit, WM_SETFONT, (WPARAM)ew->hFont, FALSE);
    }
    if (ew->hwndBrowseBtn) {
        SendMessageW(ew->hwndBrowseBtn, WM_SETFONT, (WPARAM)ew->hFont, FALSE);
    }
    if (ew->hwndOpenAfterSave) {
        SendMessageW(ew->hwndOpenAfterSave, WM_SETFONT, (WPARAM)ew->hFont, TRUE);
    }
    if (ew->btnSave && ew->btnSave->hwnd) {
        SendMessageW(ew->btnSave->hwnd, WM_SETFONT, (WPARAM)ew->hFont, FALSE);
    }
    if (ew->btnCrop && ew->btnCrop->hwnd) {
        SendMessageW(ew->btnCrop->hwnd, WM_SETFONT, (WPARAM)ew->hFont, FALSE);
    }
    if (ew->btnResize && ew->btnResize->hwnd) {
        SendMessageW(ew->btnResize->hwnd, WM_SETFONT, (WPARAM)ew->hFont, FALSE);
    }
    if (ew->btnRotateLeft && ew->btnRotateLeft->hwnd) {
        SendMessageW(ew->btnRotateLeft->hwnd, WM_SETFONT, (WPARAM)ew->hFont, FALSE);
    }
    if (ew->btnRotateRight && ew->btnRotateRight->hwnd) {
        SendMessageW(ew->btnRotateRight->hwnd, WM_SETFONT, (WPARAM)ew->hFont, FALSE);
    }
    if (ew->dropFormat && ew->dropFormat->hwnd) {
        SendMessageW(ew->dropFormat->hwnd, WM_SETFONT, (WPARAM)ew->hFont, FALSE);
    }
    if (prev) {
        DeleteObject(prev);
    }
}

static Vec<ImageEditWindow*> gImageEditWindows;

static ImageEditWindow* FindImageEditWindowByHwnd(HWND hwnd) {
    for (auto* ew : gImageEditWindows) {
        if (ew->hwnd == hwnd) {
            return ew;
        }
    }
    return nullptr;
}

// Convert display coordinates to image coordinates (crop mode)
static int DisplayToImageX(ImageEditWindow* ew, int dx) {
    if (ew->imgDisplayW <= 0) {
        return 0;
    }
    int v = (int)((float)(dx - ew->imgDisplayX) * ew->imgW / ew->imgDisplayW);
    return std::clamp(v, 0, ew->imgW);
}

static int DisplayToImageY(ImageEditWindow* ew, int dy) {
    if (ew->imgDisplayH <= 0) {
        return 0;
    }
    int v = (int)((float)(dy - ew->imgDisplayY) * ew->imgH / ew->imgDisplayH);
    return std::clamp(v, 0, ew->imgH);
}

// Convert image coordinates to display coordinates (crop mode)
static int ImageToDisplayX(ImageEditWindow* ew, int ix) {
    if (ew->imgW <= 0) {
        return ew->imgDisplayX;
    }
    return ew->imgDisplayX + (int)((float)ix * ew->imgDisplayW / ew->imgW);
}

static int ImageToDisplayY(ImageEditWindow* ew, int iy) {
    if (ew->imgH <= 0) {
        return ew->imgDisplayY;
    }
    return ew->imgDisplayY + (int)((float)iy * ew->imgDisplayH / ew->imgH);
}

// Convert display-scale sizes to image-scale sizes (resize mode)
static int DisplayToImageW(ImageEditWindow* ew, int dispW) {
    if (ew->imgDisplayW <= 0) {
        return 0;
    }
    return (int)((float)dispW * ew->imgW / ew->imgDisplayW);
}

static int DisplayToImageH(ImageEditWindow* ew, int dispH) {
    if (ew->imgDisplayH <= 0) {
        return 0;
    }
    return (int)((float)dispH * ew->imgH / ew->imgDisplayH);
}

// Convert image size to display size (resize mode)
static int ImageToDisplayW(ImageEditWindow* ew, int iw) {
    if (ew->imgW <= 0) {
        return 0;
    }
    return (int)((float)iw * ew->imgDisplayW / ew->imgW);
}

static int ImageToDisplayH(ImageEditWindow* ew, int ih) {
    if (ew->imgH <= 0) {
        return 0;
    }
    return (int)((float)ih * ew->imgDisplayH / ew->imgH);
}

static void LayoutControls(ImageEditWindow* ew);

static int GetSelectedFormatIdx(ImageEditWindow* ew) {
    if (!ew->dropFormat) {
        return kDefaultFormatIdx;
    }
    int ddIdx = ew->dropFormat->GetCurrentSelection();
    if (ddIdx < 0 || ddIdx >= ew->formatIndices.Size()) {
        return kDefaultFormatIdx;
    }
    return ew->formatIndices.at(ddIdx);
}

static void OnFormatChanged(ImageEditWindow* ew) {
    int idx = GetSelectedFormatIdx(ew);
    const char* newExt = gImageFormats[idx].ext;
    // update the extension in the dest path
    WCHAR destW[MAX_PATH + 1]{};
    GetWindowTextW(ew->hwndDestEdit, destW, MAX_PATH);
    TempStr dest = ToUtf8Temp(destW);
    if (!str::IsEmpty(dest)) {
        TempStr oldExt = path::GetExtTemp(dest);
        int baseLen = str::Leni(dest) - str::Leni(oldExt);
        TempStr base = str::DupTemp(dest, baseLen);
        TempStr newDest = str::FormatTemp("%s%s", base, newExt);
        SetWindowTextW(ew->hwndDestEdit, ToWStrTemp(newDest));
    }
    SetFocus(ew->hwnd);
}

static void UpdateSaveButtonText(ImageEditWindow* ew) {
    WCHAR destW[MAX_PATH + 1]{};
    GetWindowTextW(ew->hwndDestEdit, destW, MAX_PATH);
    TempStr dest = ToUtf8Temp(destW);
    const char* text = file::Exists(dest) ? _TRA("Overwrite") : _TRA("Save");
    ew->btnSave->SetText(text);
    // re-layout since button width may have changed
    LayoutControls(ew);
}

static TempStr FormatCropInfoTemp(int srcW, int srcH, int cropW, int cropH, int cropX, int cropY) {
    return str::FormatTemp("%d x %d => %d x %d @ %d , %d", srcW, srcH, cropW, cropH, cropX, cropY);
}

static TempStr FormatResizeInfoTemp(int srcW, int srcH, int newW, int newH) {
    float pctW = (srcW > 0) ? (float)newW * 100.0f / srcW : 0.0f;
    float pctH = (srcH > 0) ? (float)newH * 100.0f / srcH : 0.0f;
    return str::FormatTemp("%d x %d => %d x %d (%.2f%% x %.2f%%)", srcW, srcH, newW, newH, pctW, pctH);
}

// Title-bar / dialog chrome. The window background (#ebe6da on Light-Warm) is a
// darker plate; filling the size line with it looks like a label chip.
static HBRUSH ImageEditChromeBrush(ImageEditWindow* ew) {
    COLORREF col = ThemeChromeBackgroundColor();
    if (!ew->chromeBrush || ew->chromeColor != col) {
        if (ew->chromeBrush) {
            DeleteObject(ew->chromeBrush);
        }
        ew->chromeBrush = CreateSolidBrush(col);
        ew->chromeColor = col;
    }
    return ew->chromeBrush;
}

static void FillControlStrip(ImageEditWindow* ew, HDC hdc) {
    if (!ew || !hdc || ew->imgAreaH <= 0) {
        return;
    }
    RECT crc{};
    GetClientRect(ew->hwnd, &crc);
    RECT ctrlRc{0, ew->imgAreaH, crc.right, crc.bottom};
    HBRUSH bg = ImageEditChromeBrush(ew);
    if (!bg) {
        bg = GetSysColorBrush(COLOR_BTNFACE);
    }
    FillRect(hdc, &ctrlRc, bg);
}

static void UpdateInfoLabel(ImageEditWindow* ew) {
    TempStr s;
    if (ew->mode == ImageEditMode::Save) {
        s = str::FormatTemp("%d x %d", ew->imgW, ew->imgH);
    } else if (ew->mode == ImageEditMode::Crop) {
        s = FormatCropInfoTemp(ew->imgW, ew->imgH, ew->cropW, ew->cropH, ew->cropX, ew->cropY);
    } else {
        s = FormatResizeInfoTemp(ew->imgW, ew->imgH, ew->newW, ew->newH);
    }
    str::ReplaceWithCopy(&ew->infoText, s);
    if (!ew->hwnd) {
        return;
    }
    // Dragging only changes the numbers, not the control layout. Moving all
    // child windows here forces synchronous paints for every mouse move.
    if (!ew->isDragging) {
        LayoutControls(ew);
    }
    RECT rc{ew->infoRc.x, ew->infoRc.y, ew->infoRc.x + ew->infoRc.dx, ew->infoRc.y + ew->infoRc.dy};
    RECT client{};
    GetClientRect(ew->hwnd, &client);
    rc.right = client.right;
    if (rc.bottom < rc.top + 1) {
        rc.bottom = rc.top + 1;
    }
    InvalidateRect(ew->hwnd, &rc, TRUE);
}

// Glyphs only. The dialog chrome is already in the DC; do not fill behind the text.
static void PaintInfoLabel(ImageEditWindow* ew, HDC hdc) {
    if (!ew || !ew->infoText || !ew->infoText[0] || !hdc) {
        return;
    }
    if (ew->infoRc.dx <= 0 || ew->infoRc.dy <= 0) {
        return;
    }
    WCHAR* text = ToWStrTemp(ew->infoText);
    if (!text || !text[0]) {
        return;
    }
    RECT rc{ew->infoRc.x, ew->infoRc.y, ew->infoRc.x + ew->infoRc.dx, ew->infoRc.y + ew->infoRc.dy};
    HFONT old = (HFONT)SelectObject(hdc, ew->hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeWindowTextColor());
    DrawTextW(hdc, text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, old);
}

// invalidate only the image area, not the control area below
static void InvalidateImageArea(ImageEditWindow* ew) {
    RECT rc = {0, 0, 0, 0};
    GetClientRect(ew->hwnd, &rc);
    rc.bottom = ew->imgAreaH;
    InvalidateRect(ew->hwnd, &rc, FALSE);
}

static const char* ImageCropHint() {
    return _TRA("Crop: drag to draw a selection; drag handles to resize; hold Space and drag to move.");
}

static int LayoutImageActions(ImageEditWindow* ew, int x, int y, int width, bool place) {
    int left = x, rowH = 0;
    int gap = ImgPx(ew, 6);
    bool rtl = trans::IsCurrLangRtl();
    auto add = [&](HWND hwnd, Size sz) {
        if (!hwnd) return;
        if (left > x && left + sz.dx > x + width) {
            y += rowH + gap;
            left = x;
            rowH = 0;
        }
        if (place) MoveWindow(hwnd, rtl ? x + width - (left - x) - sz.dx : left, y, sz.dx, sz.dy, TRUE);
        left += sz.dx + gap;
        rowH = std::max(rowH, sz.dy);
    };
    Button* buttons[] = {ew->btnRotateLeft, ew->btnRotateRight, ew->btnCrop, ew->btnResize};
    for (Button* button : buttons) {
        if (button) add(button->hwnd, button->GetIdealSize());
    }
    if (ew->dropFormat) add(ew->dropFormat->hwnd, ew->dropFormat->GetIdealSize());
    return y + std::max(rowH, ImgPx(ew, 26));
}

static int ImageHintHeight(ImageEditWindow* ew, int width) {
    HDC dc = GetDC(ew->hwnd);
    HFONT old = ew->hFont ? (HFONT)SelectObject(dc, ew->hFont) : nullptr;
    RECT rc{0, 0, std::max(width, ImgPx(ew, 100)), 0};
    DrawTextW(dc, ToWStrTemp(ImageCropHint()), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (old) SelectObject(dc, old);
    ReleaseDC(ew->hwnd, dc);
    return std::max((int)rc.bottom, ImgPx(ew, 18));
}

static int ImageFooterHeight(ImageEditWindow* ew) {
    return std::max(ew->btnSave ? ew->btnSave->GetIdealSize().dy : 0, ImgPx(ew, 26));
}

static int GetControlAreaDy(ImageEditWindow* ew) {
    int pad = ImgPx(ew, kRowPadding);
    int width = ew->hwnd ? ClientRect(ew->hwnd).dx - ImgPx(ew, kButtonPadding * 2) : ImgPx(ew, kMinWindowWidth);
    int height = ew->fromRenderedBitmap ? 0 : ImgPx(ew, 18) + pad;
    height += ImgPx(ew, 24) + pad;
    height += LayoutImageActions(ew, 0, 0, width, false) + pad;
    height += ImgPx(ew, 20) + pad;
    return height + ImageHintHeight(ew, width) + ImageFooterHeight(ew) + pad * 3;
}

static void CalcImageLayout(ImageEditWindow* ew) {
    Rect cRc = ClientRect(ew->hwnd);
    ew->imgAreaH = cRc.dy - GetControlAreaDy(ew);
    if (ew->imgAreaH < ImgPx(ew, 10)) {
        ew->imgAreaH = ImgPx(ew, 10);
    }

    // fit image within image area with padding
    int pad = ImgPx(ew, kImagePadding);
    int availW = cRc.dx - 2 * pad;
    int availH = ew->imgAreaH - 2 * pad;
    if (availW <= 0 || availH <= 0 || ew->imgW <= 0 || ew->imgH <= 0) {
        ew->imgDisplayX = pad;
        ew->imgDisplayY = pad;
        ew->imgDisplayW = 0;
        ew->imgDisplayH = 0;
        return;
    }

    float scaleX = (float)availW / ew->imgW;
    float scaleY = (float)availH / ew->imgH;
    float scale = std::min(scaleX, scaleY);
    // don't upscale beyond 100%
    if (scale > 1.0f) {
        scale = 1.0f;
    }

    ew->imgDisplayW = (int)(ew->imgW * scale);
    ew->imgDisplayH = (int)(ew->imgH * scale);
    // center in available area
    ew->imgDisplayX = pad + (availW - ew->imgDisplayW) / 2;
    ew->imgDisplayY = pad + (availH - ew->imgDisplayH) / 2;
}

// Grow the window if the new-size rectangle exceeds the image display area (resize mode only).
// Tries to grow in the direction of the drag, moving the window if needed,
// but stops at screen edges.
static void GrowWindowIfNeeded(ImageEditWindow* ew, DragEdge edge) {
    // calculate how much display space the new size needs
    int neededDispW = ImageToDisplayW(ew, ew->newW) + 2 * ImgPx(ew, kImagePadding);
    int neededDispH = ImageToDisplayH(ew, ew->newH) + 2 * ImgPx(ew, kImagePadding);

    Rect cRc = ClientRect(ew->hwnd);
    int availW = cRc.dx;
    int availH = ew->imgAreaH;

    int extraW = neededDispW - availW;
    int extraH = neededDispH - availH;
    if (extraW <= 0 && extraH <= 0) {
        return;
    }
    if (extraW < 0) {
        extraW = 0;
    }
    if (extraH < 0) {
        extraH = 0;
    }

    // get screen work area
    HMONITOR hMon = MonitorFromWindow(ew->hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfo(hMon, &mi);
    int screenL = mi.rcWork.left;
    int screenT = mi.rcWork.top;
    int screenR = mi.rcWork.right;
    int screenB = mi.rcWork.bottom;

    RECT winRc;
    GetWindowRect(ew->hwnd, &winRc);
    int winX = winRc.left;
    int winY = winRc.top;
    int winW = winRc.right - winRc.left;
    int winH = winRc.bottom - winRc.top;

    int newWinW = winW + extraW;
    int newWinH = winH + extraH;

    // don't exceed screen size
    int maxW = screenR - screenL;
    int maxH = screenB - screenT;
    if (newWinW > maxW) {
        newWinW = maxW;
    }
    if (newWinH > maxH) {
        newWinH = maxH;
    }

    int deltaW = newWinW - winW;
    int deltaH = newWinH - winH;
    if (deltaW <= 0 && deltaH <= 0) {
        return;
    }

    int newX = winX;
    int newY = winY;

    // determine grow direction based on which edge is being dragged
    bool growLeft = (edge == DragEdge::Left || edge == DragEdge::TopLeft || edge == DragEdge::BottomLeft);
    bool growUp = (edge == DragEdge::Top || edge == DragEdge::TopLeft || edge == DragEdge::TopRight);

    if (growLeft && deltaW > 0) {
        newX = winX - deltaW;
        if (newX < screenL) {
            newX = screenL;
        }
    } else if (deltaW > 0) {
        // grow right - check we don't go past screen edge
        if (newX + newWinW > screenR) {
            newX = screenR - newWinW;
            if (newX < screenL) {
                newX = screenL;
            }
        }
    }

    if (growUp && deltaH > 0) {
        newY = winY - deltaH;
        if (newY < screenT) {
            newY = screenT;
        }
    } else if (deltaH > 0) {
        // grow down
        if (newY + newWinH > screenB) {
            newY = screenB - newWinH;
            if (newY < screenT) {
                newY = screenT;
            }
        }
    }

    SetWindowPos(ew->hwnd, nullptr, newX, newY, newWinW, newWinH, SWP_NOZORDER);
    // recalc layout after window resize
    CalcImageLayout(ew);
}

static DragEdge HitTestCropEdge(ImageEditWindow* ew, int mx, int my) {
    int left = ImageToDisplayX(ew, ew->cropX);
    int right = ImageToDisplayX(ew, ew->cropX + ew->cropW);
    int top = ImageToDisplayY(ew, ew->cropY);
    int bottom = ImageToDisplayY(ew, ew->cropY + ew->cropH);

    int t = ImgPx(ew, kResizeEdgeThreshold);

    bool onLeft = (mx >= left - t && mx <= left + t);
    bool onRight = (mx >= right - t && mx <= right + t);
    bool onTop = (my >= top - t && my <= top + t);
    bool onBottom = (my >= bottom - t && my <= bottom + t);

    bool inVertRange = (my >= top - t && my <= bottom + t);
    bool inHorzRange = (mx >= left - t && mx <= right + t);

    if (onLeft && onTop) {
        return DragEdge::TopLeft;
    }
    if (onRight && onTop) {
        return DragEdge::TopRight;
    }
    if (onLeft && onBottom) {
        return DragEdge::BottomLeft;
    }
    if (onRight && onBottom) {
        return DragEdge::BottomRight;
    }
    if (onLeft && inVertRange) {
        return DragEdge::Left;
    }
    if (onRight && inVertRange) {
        return DragEdge::Right;
    }
    if (onTop && inHorzRange) {
        return DragEdge::Top;
    }
    if (onBottom && inHorzRange) {
        return DragEdge::Bottom;
    }
    // Space explicitly moves the selection; otherwise draw a replacement.
    if ((GetKeyState(VK_SPACE) & 0x8000) && mx >= left && mx <= right && my >= top && my <= bottom) {
        return DragEdge::Move;
    }
    if (mx >= ew->imgDisplayX && mx < ew->imgDisplayX + ew->imgDisplayW && my >= ew->imgDisplayY &&
        my < ew->imgDisplayY + ew->imgDisplayH)
        return DragEdge::Draw;
    return DragEdge::None;
}

static DragEdge HitTestResizeEdge(ImageEditWindow* ew, int mx, int my) {
    // the "new size" rectangle, centered in the display area
    int dispNewW = ImageToDisplayW(ew, ew->newW);
    int dispNewH = ImageToDisplayH(ew, ew->newH);
    int cx = ew->imgDisplayX + ew->imgDisplayW / 2;
    int cy = ew->imgDisplayY + ew->imgDisplayH / 2;
    int left = cx - dispNewW / 2;
    int right = left + dispNewW;
    int top = cy - dispNewH / 2;
    int bottom = top + dispNewH;

    int t = ImgPx(ew, kResizeEdgeThreshold);

    bool onLeft = (mx >= left - t && mx <= left + t);
    bool onRight = (mx >= right - t && mx <= right + t);
    bool onTop = (my >= top - t && my <= top + t);
    bool onBottom = (my >= bottom - t && my <= bottom + t);

    bool inVertRange = (my >= top - t && my <= bottom + t);
    bool inHorzRange = (mx >= left - t && mx <= right + t);

    if (onLeft && onTop) {
        return DragEdge::TopLeft;
    }
    if (onRight && onTop) {
        return DragEdge::TopRight;
    }
    if (onLeft && onBottom) {
        return DragEdge::BottomLeft;
    }
    if (onRight && onBottom) {
        return DragEdge::BottomRight;
    }
    if (onLeft && inVertRange) {
        return DragEdge::Left;
    }
    if (onRight && inVertRange) {
        return DragEdge::Right;
    }
    if (onTop && inHorzRange) {
        return DragEdge::Top;
    }
    if (onBottom && inHorzRange) {
        return DragEdge::Bottom;
    }
    return DragEdge::None;
}

static HCURSOR GetCursorForEdge(DragEdge edge) {
    switch (edge) {
        case DragEdge::Left:
        case DragEdge::Right:
            return GetCachedCursor(IDC_SIZEWE);
        case DragEdge::Top:
        case DragEdge::Bottom:
            return GetCachedCursor(IDC_SIZENS);
        case DragEdge::TopLeft:
        case DragEdge::BottomRight:
            return GetCachedCursor(IDC_SIZENWSE);
        case DragEdge::TopRight:
        case DragEdge::BottomLeft:
            return GetCachedCursor(IDC_SIZENESW);
        case DragEdge::Move:
            return GetCachedCursor(IDC_SIZEALL);
        case DragEdge::Draw:
            return GetCachedCursor(IDC_CROSS);
        default:
            return GetCachedCursor(IDC_ARROW);
    }
}

static void PaintSaveImage(ImageEditWindow* ew, HDC hdc) {
    Rect cRc = ClientRect(ew->hwnd);

    PaintCheckerboard(hdc, 0, 0, cRc.dx, ew->imgAreaH);

    if (!ew->srcBitmap || ew->imgDisplayW <= 0 || ew->imgDisplayH <= 0) {
        return;
    }

    Graphics g(hdc);
    g.DrawImage(ew->srcBitmap, ew->imgDisplayX, ew->imgDisplayY, ew->imgDisplayW, ew->imgDisplayH);
}

static void PaintCropImage(ImageEditWindow* ew, HDC hdc) {
    Rect cRc = ClientRect(ew->hwnd);

    PaintCheckerboard(hdc, 0, 0, cRc.dx, ew->imgAreaH);

    if (!ew->srcBitmap || ew->imgDisplayW <= 0 || ew->imgDisplayH <= 0) {
        return;
    }

    Graphics g(hdc);

    // draw the image
    g.DrawImage(ew->srcBitmap, ew->imgDisplayX, ew->imgDisplayY, ew->imgDisplayW, ew->imgDisplayH);

    // draw semi-transparent overlay over non-cropped areas
    int cropDispX = ImageToDisplayX(ew, ew->cropX);
    int cropDispY = ImageToDisplayY(ew, ew->cropY);
    int cropDispR = ImageToDisplayX(ew, ew->cropX + ew->cropW);
    int cropDispB = ImageToDisplayY(ew, ew->cropY + ew->cropH);

    int ix = ew->imgDisplayX;
    int iy = ew->imgDisplayY;
    int iw = ew->imgDisplayW;
    int ih = ew->imgDisplayH;
    int ir = ix + iw;
    int ib = iy + ih;

    // draw semi-transparent overlay using GDI+ with explicit Gdiplus::Brush pointer
    Gdiplus::SolidBrush overlayBrush(Gdiplus::Color(128, 0, 0, 0));
    Gdiplus::Brush* pBrush = &overlayBrush;

    // top strip
    if (cropDispY > iy) {
        g.FillRectangle(pBrush, ix, iy, iw, cropDispY - iy);
    }
    // bottom strip
    if (cropDispB < ib) {
        g.FillRectangle(pBrush, ix, cropDispB, iw, ib - cropDispB);
    }
    // left strip (between top and bottom crop)
    if (cropDispX > ix) {
        g.FillRectangle(pBrush, ix, cropDispY, cropDispX - ix, cropDispB - cropDispY);
    }
    // right strip
    if (cropDispR < ir) {
        g.FillRectangle(pBrush, cropDispR, cropDispY, ir - cropDispR, cropDispB - cropDispY);
    }

    // draw crop border
    float stroke = (float)ImgPx(ew, 1);
    Gdiplus::Pen outline(Color(255, 0, 0, 0), stroke * 3);
    g.DrawRectangle(&outline, cropDispX, cropDispY, cropDispR - cropDispX, cropDispB - cropDispY);
    Gdiplus::Pen pen(Color(255, 255, 255), stroke);
    pen.SetDashStyle(Gdiplus::DashStyleDash);
    g.DrawRectangle(&pen, cropDispX, cropDispY, cropDispR - cropDispX, cropDispB - cropDispY);

    // draw drag handles at corners and edge midpoints
    int hs = ImgPx(ew, kDragHandleSize);
    int hh = hs / 2;
    int midX = (cropDispX + cropDispR) / 2;
    int midY = (cropDispY + cropDispB) / 2;

    Gdiplus::SolidBrush handleBrush(Color(255, 255, 255, 255));
    Gdiplus::Pen handlePen(Color(255, 0, 0, 0), (float)ImgPx(ew, 1));

    auto drawHandle = [&](int cx, int cy) {
        g.FillRectangle(&handleBrush, cx - hh, cy - hh, hs, hs);
        g.DrawRectangle(&handlePen, cx - hh, cy - hh, hs, hs);
    };

    // corners
    drawHandle(cropDispX, cropDispY);
    drawHandle(cropDispR, cropDispY);
    drawHandle(cropDispX, cropDispB);
    drawHandle(cropDispR, cropDispB);
    // edge midpoints
    drawHandle(midX, cropDispY);
    drawHandle(midX, cropDispB);
    drawHandle(cropDispX, midY);
    drawHandle(cropDispR, midY);
}

static void PaintResizeImage(ImageEditWindow* ew, HDC hdc) {
    Rect cRc = ClientRect(ew->hwnd);

    PaintCheckerboard(hdc, 0, 0, cRc.dx, ew->imgAreaH);

    if (!ew->srcBitmap || ew->imgDisplayW <= 0 || ew->imgDisplayH <= 0) {
        return;
    }

    Graphics g(hdc);

    // draw the full image
    g.DrawImage(ew->srcBitmap, ew->imgDisplayX, ew->imgDisplayY, ew->imgDisplayW, ew->imgDisplayH);

    // draw semi-transparent overlay over the entire image
    Gdiplus::SolidBrush overlayBrush(Gdiplus::Color(128, 0, 0, 0));
    Gdiplus::Brush* pBrush = &overlayBrush;
    g.FillRectangle(pBrush, ew->imgDisplayX, ew->imgDisplayY, ew->imgDisplayW, ew->imgDisplayH);

    // draw the "new size" rectangle showing the resized portion, centered
    int dispNewW = ImageToDisplayW(ew, ew->newW);
    int dispNewH = ImageToDisplayH(ew, ew->newH);
    int cx = ew->imgDisplayX + ew->imgDisplayW / 2;
    int cy = ew->imgDisplayY + ew->imgDisplayH / 2;
    int newLeft = cx - dispNewW / 2;
    int newTop = cy - dispNewH / 2;

    // redraw the image portion at the new size area (clear overlay there)
    // clip source to full image, draw scaled into the new rect
    g.DrawImage(ew->srcBitmap, newLeft, newTop, dispNewW, dispNewH);

    // draw border around the new size rectangle
    Gdiplus::Pen underlay(Color(255, 0, 0, 0), (float)ImgPx(ew, 3));
    g.DrawRectangle(&underlay, newLeft, newTop, dispNewW, dispNewH);
    Gdiplus::Pen pen(Color(255, 255, 255), (float)ImgPx(ew, 1));
    pen.SetDashStyle(Gdiplus::DashStyleDash);
    g.DrawRectangle(&pen, newLeft, newTop, dispNewW, dispNewH);

    // draw drag handles
    int hs = ImgPx(ew, kDragHandleSize);
    int hh = hs / 2;
    int midX = newLeft + dispNewW / 2;
    int midY = newTop + dispNewH / 2;
    int right = newLeft + dispNewW;
    int bottom = newTop + dispNewH;

    Gdiplus::SolidBrush handleBrush(Color(255, 255, 255, 255));
    Gdiplus::Pen handlePen(Color(255, 0, 0, 0), (float)ImgPx(ew, 1));

    auto drawHandle = [&](int hx, int hy) {
        g.FillRectangle(&handleBrush, hx - hh, hy - hh, hs, hs);
        g.DrawRectangle(&handlePen, hx - hh, hy - hh, hs, hs);
    };

    drawHandle(newLeft, newTop);
    drawHandle(right, newTop);
    drawHandle(newLeft, bottom);
    drawHandle(right, bottom);
    drawHandle(midX, newTop);
    drawHandle(midX, bottom);
    drawHandle(newLeft, midY);
    drawHandle(right, midY);
}

static void LayoutControls(ImageEditWindow* ew) {
    // Button captions can change between Save/Overwrite and Crop/Apply Crop.
    // Recompute the image area when translated controls wrap onto another row.
    CalcImageLayout(ew);
    Rect cRc = ClientRect(ew->hwnd);
    int rowPad = ImgPx(ew, kRowPadding);
    int btnPad = ImgPx(ew, kButtonPadding);
    int labelH = ImgPx(ew, 18);
    int editH = ImgPx(ew, 24);
    int y = ew->imgAreaH + rowPad;
    int x = btnPad;
    int w = cRc.dx - 2 * btnPad;

    // row 1: file path label — skip if from RenderedBitmap
    if (!ew->fromRenderedBitmap) {
        int editBorder = GetSystemMetrics(SM_CXEDGE);
        LRESULT margins = SendMessageW(ew->hwndDestEdit, EM_GETMARGINS, 0, 0);
        int editLeftMargin = LOWORD(margins);
        int labelShift = editBorder + editLeftMargin;
        MoveWindow(ew->hwndPathLabel, x + labelShift, y, w - labelShift, labelH, TRUE);
        y += labelH + rowPad;
    }

    // row 2: dest edit + browse button
    int browseW = ImgPx(ew, 30);
    int gap = ImgPx(ew, 4);
    MoveWindow(ew->hwndDestEdit, x, y, w - browseW - gap, editH, TRUE);
    MoveWindow(ew->hwndBrowseBtn, x + w - browseW, y, browseW, editH, TRUE);
    y += editH + rowPad;

    y = LayoutImageActions(ew, x, y, w, true) + rowPad;
    ew->infoRc = {x, y, w, ImgPx(ew, 20)};
    y += ew->infoRc.dy + rowPad;
    ew->hintRc = {x, y, w, ImageHintHeight(ew, w)};
    y += ew->hintRc.dy + rowPad;
    int footerH = ImageFooterHeight(ew);
    int saveW = std::max(ew->btnSave->GetIdealSize().dx, ImgPx(ew, 90));
    bool rtl = trans::IsCurrLangRtl();
    MoveWindow(ew->btnSave->hwnd, rtl ? x : x + w - saveW, y, saveW, footerH, TRUE);
    if (ew->hwndOpenAfterSave) {
        MoveWindow(ew->hwndOpenAfterSave, rtl ? x + saveW + rowPad : x, y, w - saveW - rowPad, footerH, TRUE);
    }
}

static bool IsCropChanged(ImageEditWindow* ew);
static void UpdateModeButtons(ImageEditWindow* ew);

static void ClampCropToImage(ImageEditWindow* ew) {
    if (ew->imgW < 1 || ew->imgH < 1) {
        return;
    }
    if (ew->cropX < 0) {
        ew->cropX = 0;
    }
    if (ew->cropY < 0) {
        ew->cropY = 0;
    }
    if (ew->cropX >= ew->imgW) {
        ew->cropX = 0;
    }
    if (ew->cropY >= ew->imgH) {
        ew->cropY = 0;
    }
    if (ew->cropW < 1 || ew->cropX + ew->cropW > ew->imgW) {
        ew->cropW = ew->imgW - ew->cropX;
    }
    if (ew->cropH < 1 || ew->cropY + ew->cropH > ew->imgH) {
        ew->cropH = ew->imgH - ew->cropY;
    }
}

// clockwise is Rotate Right. The crop rectangle turns with the picture.
static void RotateSource(ImageEditWindow* ew, bool clockwise) {
    if (!ew->srcBitmap) {
        return;
    }
    int oldW = ew->imgW;
    int oldH = ew->imgH;
    if (oldW < 1 || oldH < 1) {
        return;
    }
    bool fullCrop = !IsCropChanged(ew);
    int cx = ew->cropX;
    int cy = ew->cropY;
    int cw = ew->cropW;
    int ch = ew->cropH;
    bool sizeMatched = ew->newW == oldW && ew->newH == oldH;
    ew->srcBitmap->RotateFlip(clockwise ? Gdiplus::Rotate90FlipNone : Gdiplus::Rotate270FlipNone);
    ew->imgW = (int)ew->srcBitmap->GetWidth();
    ew->imgH = (int)ew->srcBitmap->GetHeight();
    if (fullCrop) {
        ew->cropX = 0;
        ew->cropY = 0;
        ew->cropW = ew->imgW;
        ew->cropH = ew->imgH;
    } else if (clockwise) {
        ew->cropX = oldH - cy - ch;
        ew->cropY = cx;
        ew->cropW = ch;
        ew->cropH = cw;
    } else {
        ew->cropX = cy;
        ew->cropY = oldW - cx - cw;
        ew->cropW = ch;
        ew->cropH = cw;
    }
    ClampCropToImage(ew);
    if (sizeMatched) {
        ew->newW = ew->imgW;
        ew->newH = ew->imgH;
    } else {
        int t = ew->newW;
        ew->newW = ew->newH;
        ew->newH = t;
    }
    ew->isDragging = false;
    ew->dragEdge = DragEdge::None;
    ew->hoverEdge = DragEdge::None;
    CalcImageLayout(ew);
    UpdateModeButtons(ew);
    UpdateInfoLabel(ew);
    LayoutControls(ew);
    InvalidateImageArea(ew);
}

static void OnRotateLeft(ImageEditWindow* ew) {
    RotateSource(ew, false);
}

static void OnRotateRight(ImageEditWindow* ew) {
    RotateSource(ew, true);
}

static void OnBrowse(ImageEditWindow* ew) {
    WCHAR dstFileName[MAX_PATH + 1]{};
    // pre-populate with current dest path
    int len = GetWindowTextW(ew->hwndDestEdit, dstFileName, MAX_PATH);
    (void)len;

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ew->hwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter =
        L"All Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tiff;*.tif;*.webp\0"
        L"PNG Files\0*.png\0"
        L"JPEG Files\0*.jpg;*.jpeg\0"
        L"BMP Files\0*.bmp\0"
        L"All Files\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(ew->hwndDestEdit, dstFileName);
    }
}

// Save a GDI+ Bitmap using WIC. Supports all formats that have a WIC encoder installed,
// including WebP on Windows 10+.
static bool SaveBitmapWithWIC(Bitmap* bmp, const WCHAR* destPath, const GUID* containerFormat, int dpi = 0) {
    if (!bmp || !destPath || !containerFormat) {
        return false;
    }

    // get HBITMAP from GDI+ bitmap
    HBITMAP hbmp = nullptr;
    Gdiplus::Color bgColor(255, 255, 255, 255);
    if (bmp->GetHBITMAP(bgColor, &hbmp) != Ok || !hbmp) {
        return false;
    }

    bool ok = false;
    IWICImagingFactory* pFactory = nullptr;
    IWICBitmapEncoder* pEncoder = nullptr;
    IWICBitmapFrameEncode* pFrame = nullptr;
    IWICBitmap* pWicBitmap = nullptr;
    IStream* pStream = nullptr;
    IPropertyBag2* pProps = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                  (void**)&pFactory);
    if (FAILED(hr)) {
        goto Done;
    }

    hr = pFactory->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUsePremultipliedAlpha, &pWicBitmap);
    if (FAILED(hr)) {
        goto Done;
    }

    hr = SHCreateStreamOnFileEx(destPath, STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, FILE_ATTRIBUTE_NORMAL, TRUE,
                                nullptr, &pStream);
    if (FAILED(hr)) {
        goto Done;
    }

    hr = pFactory->CreateEncoder(*containerFormat, nullptr, &pEncoder);
    if (FAILED(hr)) {
        goto Done;
    }

    hr = pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        goto Done;
    }

    hr = pEncoder->CreateNewFrame(&pFrame, &pProps);
    if (FAILED(hr)) {
        goto Done;
    }

    hr = pFrame->Initialize(pProps);
    if (FAILED(hr)) {
        goto Done;
    }

    if (dpi > 0 && FAILED(pFrame->SetResolution(dpi, dpi))) {
        goto Done;
    }

    {
        UINT w = 0, h = 0;
        pWicBitmap->GetSize(&w, &h);
        hr = pFrame->SetSize(w, h);
        if (FAILED(hr)) {
            goto Done;
        }

        WICPixelFormatGUID pixFmt = GUID_WICPixelFormat32bppBGRA;
        hr = pFrame->SetPixelFormat(&pixFmt);
        if (FAILED(hr)) {
            goto Done;
        }

        hr = pFrame->WriteSource(pWicBitmap, nullptr);
        if (FAILED(hr)) {
            goto Done;
        }
    }

    hr = pFrame->Commit();
    if (FAILED(hr)) {
        goto Done;
    }

    hr = pEncoder->Commit();
    ok = SUCCEEDED(hr);

Done:
    if (pProps) {
        pProps->Release();
    }
    if (pFrame) {
        pFrame->Release();
    }
    if (pEncoder) {
        pEncoder->Release();
    }
    if (pStream) {
        pStream->Release();
    }
    if (pWicBitmap) {
        pWicBitmap->Release();
    }
    if (pFactory) {
        pFactory->Release();
    }
    DeleteObject(hbmp);
    return ok;
}

bool SaveBitmapAsPng(HBITMAP hbmp, const char* destPath) {
    if (!hbmp || str::IsEmpty(destPath)) {
        return false;
    }
    Bitmap bmp(hbmp, nullptr);
    if (bmp.GetLastStatus() != Ok) {
        return false;
    }
    TempWStr destW = ToWStrTemp(destPath);
    return SaveBitmapWithWIC(&bmp, destW, &GUID_ContainerFormatPng);
}

// The modal owner keeps document edits out of the export pass. Holding a
// reference to the live engine preserves unsaved annotations and page edits;
// reopening the file would silently export the older, saved document instead.
struct PageImageExport {
    EngineBase* engine = nullptr;
    AutoFreeStr baseName;
    AutoFreeStr folder;
    AutoFreeStr outputFolder;
    HANDLE thread = nullptr;
    LONG cancel = 0;
    LONG completed = 0;
    int shownCompleted = -1;
    int firstPage = 1;
    int lastPage = 1;
    int currentPage = 1;
    int rotation = 0;
    int dpi = 300;
    bool jpeg = false;
    bool failed = false;
    bool tooLarge = false;
    int failedPage = 0;

    ~PageImageExport() {
        if (thread) {
            InterlockedExchange(&cancel, 1);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        if (engine) engine->Release();
    }
};

static void ExportPageImagesWorker(PageImageExport* data) {
    ScopedCom com;
    float zoom = data->dpi / data->engine->GetFileDPI();
    for (int page = data->firstPage; page <= data->lastPage; page++) {
        if (InterlockedCompareExchange(&data->cancel, 0, 0)) break;
        RectF box = data->engine->PageMediabox(page);
        double width = ceil(box.dx * zoom);
        double height = ceil(box.dy * zoom);
        // Bound per-page memory, including the renderer and encoder copies.
        // An A4 page at 600 DPI fits; oversized drawings fail without allocating.
        if (!(width > 0 && height > 0 && width <= 32767 && height <= 32767 && width * height <= 64000000)) {
            data->tooLarge = data->failed = true;
            data->failedPage = page;
            break;
        }
        RenderPageArgs args(page, zoom, data->rotation, nullptr, RenderTarget::Export);
        RenderedBitmap* rendered = data->engine->RenderPage(args);
        if (!rendered || !rendered->IsValid()) {
            delete rendered;
            data->failed = true;
            data->failedPage = page;
            break;
        }
        if (InterlockedCompareExchange(&data->cancel, 0, 0)) {
            delete rendered;
            break;
        }
        TempStr name = str::FormatTemp("%s-%03d.%s", data->baseName.Get(), page, data->jpeg ? "jpg" : "png");
        TempStr path = path::JoinTemp(data->outputFolder, name);
        bool ok = false;
        {
            Bitmap bitmap(rendered->GetBitmap(), nullptr);
            ok = bitmap.GetLastStatus() == Ok &&
                 SaveBitmapWithWIC(&bitmap, ToWStrTemp(path),
                                   data->jpeg ? &GUID_ContainerFormatJpeg : &GUID_ContainerFormatPng, data->dpi);
        }
        delete rendered;
        if (!ok) {
            // Only this worker writes into its newly created output directory.
            file::Delete(path);
            data->failed = true;
            data->failedPage = page;
            break;
        }
        InterlockedIncrement(&data->completed);
    }
}

static void BrowsePageImageFolder(HWND hwnd) {
    ScopedComPtr<IFileOpenDialog> dialog;
    if (!dialog.Create(CLSID_FileOpenDialog)) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    ScopedComPtr<IShellItem> initial;
    TempStr current = HwndGetTextTemp(GetDlgItem(hwnd, IDC_EXPORT_FOLDER));
    if (SUCCEEDED(SHCreateItemFromParsingName(ToWStrTemp(current), nullptr, IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial);
    }
    if (FAILED(dialog->Show(hwnd))) return;
    ScopedComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return;
    WCHAR* path = nullptr;
    if (SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        SetDlgItemTextW(hwnd, IDC_EXPORT_FOLDER, path);
        CoTaskMemFree(path);
    }
}

static void EnablePageImageExportInputs(HWND hwnd, bool enable) {
    int ids[] = {IDC_EXPORT_CURRENT, IDC_EXPORT_RANGE,       IDC_EXPORT_ALL,
                 IDC_EXPORT_FORMAT,  IDC_EXPORT_DPI,         IDC_EXPORT_FOLDER,
                 IDC_EXPORT_BROWSE,  IDC_EXPORT_OPEN_FOLDER, IDOK};
    for (int id : ids) EnableWindow(GetDlgItem(hwnd, id), enable);
    EnableWindow(GetDlgItem(hwnd, IDC_EXPORT_PAGE_RANGE),
                 enable && IsDlgButtonChecked(hwnd, IDC_EXPORT_RANGE) == BST_CHECKED);
    // Native themed controls repaint their client on WM_ENABLE, but their
    // rounded frames also need invalidation when returning from disabled state.
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

static bool CreatePageImageExportFolder(PageImageExport* data) {
    for (int i = 1; i <= 10000; i++) {
        TempStr name = i == 1 ? str::FormatTemp("%s-images", data->baseName.Get())
                              : str::FormatTemp("%s-images-%d", data->baseName.Get(), i);
        TempStr path = path::JoinTemp(data->folder, name);
        if (CreateDirectoryW(ToWStrTemp(path), nullptr)) {
            data->outputFolder.Set(str::Dup(path));
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    return false;
}

static LRESULT CALLBACK PageImageExportLabelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        Rect rc = ClientRect(hwnd);
        DoubleBuffer buffer(hwnd, rc);
        HDC paint = buffer.GetDC();
        ScopedGdiObj<HBRUSH> bg(CreateSolidBrush(AppDialogPanelBackgroundColor()));
        RECT rect = ToRECT(rc);
        FillRect(paint, &rect, bg);
        ScopedSelectObject font(paint, GetWindowFont(hwnd));
        SetBkMode(paint, TRANSPARENT);
        SetTextColor(paint, ThemeWindowTextColor());
        UINT flags = DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (HwndIsRtl(hwnd)) flags |= DT_RTLREADING | DT_RIGHT;
        DrawTextW(paint, ToWStrTemp(HwndGetTextTemp(hwnd)), -1, &rect, flags);
        buffer.Flush(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, PageImageExportLabelProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Accept one inclusive interval and bound each number before accumulating it.
static bool ParsePageImageExportRange(const char* text, int pageCount, int& first, int& last) {
    int pages[2]{};
    for (int i = 0; i < 2; i++) {
        while (*text == ' ' || *text == '\t') text++;
        if (*text < '0' || *text > '9') return false;
        while (*text >= '0' && *text <= '9') {
            int digit = *text++ - '0';
            if (pages[i] > pageCount / 10 || (pages[i] == pageCount / 10 && digit > pageCount % 10)) return false;
            pages[i] = pages[i] * 10 + digit;
        }
        while (*text == ' ' || *text == '\t') text++;
        if (i == 0 && *text++ != '-') return false;
    }
    if (*text || pages[0] < 1 || pages[1] < pages[0]) return false;
    first = pages[0];
    last = pages[1];
    return true;
}

static INT_PTR CALLBACK ExportPageImagesProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto data = (PageImageExport*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_INITDIALOG) {
        data = (PageImageExport*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        HwndSetText(hwnd, _TRA("Export pages as images"));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_RANGE_LABEL, _TRA("Pages:"));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_FORMAT_LABEL, _TRA("Image format:"));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_DPI_LABEL, _TRA("Resolution (DPI):"));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_FOLDER_LABEL, _TRA("Save in folder:"));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_BROWSE, _TRA("Browse..."));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_HINT, _TRA("A new subfolder is created for each export."));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_OPEN_FOLDER, _TRA("Open folder after successful export"));
        CheckDlgButton(hwnd, IDC_EXPORT_OPEN_FOLDER,
                       gGlobalPrefs->pageImagesOpenFolderAfterExport ? BST_CHECKED : BST_UNCHECKED);
        HwndSetDlgItemText(hwnd, IDOK, _TRA("Export"));
        HwndSetDlgItemText(hwnd, IDCANCEL, _TRA("Cancel"));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_CURRENT, str::FormatTemp(_TRA("Current page (%d)"), data->currentPage));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_RANGE, _TRA("Page range"));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_ALL,
                           str::FormatTemp(_TRA("All pages (%d pages)"), data->engine->PageCount()));
        HwndSetDlgItemText(hwnd, IDC_EXPORT_RANGE_HINT,
                           str::FormatTemp(_TRA("Pages 1-%d. Example: 30-45 (inclusive)."), data->engine->PageCount()));
        CheckDlgButton(hwnd, IDC_EXPORT_RANGE, BST_CHECKED);
        SendDlgItemMessageW(hwnd, IDC_EXPORT_PAGE_RANGE, EM_SETCUEBANNER, TRUE,
                            (LPARAM)ToWStrTemp(_TRA("Example: 30-45")));
        HWND format = GetDlgItem(hwnd, IDC_EXPORT_FORMAT);
        ComboBox_AddString(format, L"PNG");
        ComboBox_AddString(format, L"JPEG");
        ComboBox_SetCurSel(format, 0);
        HWND dpi = GetDlgItem(hwnd, IDC_EXPORT_DPI);
        const WCHAR* resolutions[] = {L"96", L"150", L"200", L"300", L"600"};
        for (const WCHAR* value : resolutions) ComboBox_AddString(dpi, value);
        ComboBox_SetCurSel(dpi, 3);
        HwndSetDlgItemText(hwnd, IDC_EXPORT_FOLDER, data->folder);
        AppDialogStyleEdit(GetDlgItem(hwnd, IDC_EXPORT_FOLDER), true);
        AppDialogStyleEdit(GetDlgItem(hwnd, IDC_EXPORT_PAGE_RANGE), true);
        int labels[] = {IDC_EXPORT_RANGE_LABEL, IDC_EXPORT_FORMAT_LABEL, IDC_EXPORT_DPI_LABEL, IDC_EXPORT_FOLDER_LABEL,
                        IDC_EXPORT_HINT,        IDC_EXPORT_STATUS,       IDC_EXPORT_RANGE_HINT};
        for (int id : labels) SetWindowSubclass(GetDlgItem(hwnd, id), PageImageExportLabelProc, 1, 0);
        SendDlgItemMessageW(hwnd, IDC_EXPORT_PROGRESS, PBM_SETRANGE32, 0, data->engine->PageCount());
        CenterDialog(hwnd);
        HwndSetFocus(GetDlgItem(hwnd, IDC_EXPORT_PAGE_RANGE));
        return FALSE;
    }
    if (!data) return FALSE;
    switch (msg) {
        // Per-monitor-v2's dialog manager scales the template, children and
        // fonts together. Do not also scale them here (that doubles the change).
        case WM_DPICHANGED:
            return FALSE;
        case WM_TIMER: {
            if (!data->thread || wp != 1) break;
            int completed = InterlockedCompareExchange(&data->completed, 0, 0);
            int total = data->lastPage - data->firstPage + 1;
            if (completed != data->shownCompleted) {
                data->shownCompleted = completed;
                SendDlgItemMessageW(hwnd, IDC_EXPORT_PROGRESS, PBM_SETPOS, completed, 0);
                if (!InterlockedCompareExchange(&data->cancel, 0, 0)) {
                    HwndSetDlgItemText(hwnd, IDC_EXPORT_STATUS,
                                       str::FormatTemp(_TRA("Exported %d of %d pages."), completed, total));
                }
            }
            if (WaitForSingleObject(data->thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(data->thread);
                data->thread = nullptr;
                KillTimer(hwnd, 1);
                EnablePageImageExportInputs(hwnd, true);
                EnableWindow(GetDlgItem(hwnd, IDCANCEL), TRUE);
                HwndSetDlgItemText(hwnd, IDCANCEL, _TRA("Close"));
                AppDialogSyncWarmPushButtons(hwnd);
                RedrawWindow(GetDlgItem(hwnd, IDCANCEL), nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
                if (data->failed) {
                    HwndSetDlgItemText(hwnd, IDC_EXPORT_STATUS,
                                       str::FormatTemp(_TRA("Could not export page %d."), data->failedPage));
                    HwndSetDlgItemText(hwnd, IDC_EXPORT_HINT,
                                       data->tooLarge
                                           ? _TRA("Page is too large. Choose a lower DPI.")
                                           : _TRA("Export failed. Check the output folder and free disk space."));
                } else {
                    HwndSetDlgItemText(hwnd, IDC_EXPORT_HINT, data->outputFolder);
                    if (InterlockedCompareExchange(&data->cancel, 0, 0)) {
                        HwndSetDlgItemText(hwnd, IDC_EXPORT_STATUS,
                                           str::FormatTemp(_TRA("Cancelled. %d pages exported."), completed));
                    } else if (completed == total && IsDlgButtonChecked(hwnd, IDC_EXPORT_OPEN_FOLDER) == BST_CHECKED) {
                        LaunchFileShell(data->outputFolder);
                    }
                }
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            }
            return TRUE;
        }
        case WM_CLOSE:
            SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_EXPORT_OPEN_FOLDER:
                    if (!data->thread && HIWORD(wp) == BN_CLICKED) {
                        gGlobalPrefs->pageImagesOpenFolderAfterExport =
                            IsDlgButtonChecked(hwnd, IDC_EXPORT_OPEN_FOLDER) == BST_CHECKED;
                        SaveSettings();
                    }
                    return TRUE;
                case IDC_EXPORT_CURRENT:
                case IDC_EXPORT_ALL:
                case IDC_EXPORT_RANGE:
                    if (!data->thread && HIWORD(wp) == BN_CLICKED) {
                        bool range = IsDlgButtonChecked(hwnd, IDC_EXPORT_RANGE) == BST_CHECKED;
                        EnableWindow(GetDlgItem(hwnd, IDC_EXPORT_PAGE_RANGE), range);
                        if (range) {
                            HwndSetFocus(GetDlgItem(hwnd, IDC_EXPORT_PAGE_RANGE));
                            SendDlgItemMessageW(hwnd, IDC_EXPORT_PAGE_RANGE, EM_SETSEL, 0, -1);
                        }
                    }
                    return TRUE;
                case IDC_EXPORT_BROWSE:
                    if (!data->thread) BrowsePageImageFolder(hwnd);
                    return TRUE;
                case IDCANCEL:
                    if (data->thread) {
                        InterlockedExchange(&data->cancel, 1);
                        HwndSetDlgItemText(hwnd, IDC_EXPORT_STATUS, _TRA("Cancelling after the current page..."));
                        EnableWindow(GetDlgItem(hwnd, IDCANCEL), FALSE);
                    } else {
                        EndDialog(hwnd, IDCANCEL);
                    }
                    return TRUE;
                case IDOK: {
                    if (data->thread) return TRUE;
                    int selection = IsDlgButtonChecked(hwnd, IDC_EXPORT_CURRENT) == BST_CHECKED ? 0
                                    : IsDlgButtonChecked(hwnd, IDC_EXPORT_RANGE) == BST_CHECKED ? 1
                                                                                                : 2;
                    data->firstPage = selection == 0 ? data->currentPage : 1;
                    data->lastPage = selection == 0 ? data->currentPage : data->engine->PageCount();
                    if (selection == 1) {
                        TempStr range = HwndGetTextTemp(GetDlgItem(hwnd, IDC_EXPORT_PAGE_RANGE));
                        if (!ParsePageImageExportRange(range, data->engine->PageCount(), data->firstPage,
                                                       data->lastPage)) {
                            HwndSetDlgItemText(hwnd, IDC_EXPORT_STATUS,
                                               str::FormatTemp(_TRA("Enter a page range such as 30-45 (1-%d)."),
                                                               data->engine->PageCount()));
                            HwndSetFocus(GetDlgItem(hwnd, IDC_EXPORT_PAGE_RANGE));
                            SendDlgItemMessageW(hwnd, IDC_EXPORT_PAGE_RANGE, EM_SETSEL, 0, -1);
                            return TRUE;
                        }
                    }
                    data->folder.Set(str::Dup(HwndGetTextTemp(GetDlgItem(hwnd, IDC_EXPORT_FOLDER))));
                    if (!path::IsDirectory(data->folder)) {
                        HwndSetDlgItemText(hwnd, IDC_EXPORT_STATUS, _TRA("Choose an existing output folder."));
                        HwndSetFocus(GetDlgItem(hwnd, IDC_EXPORT_FOLDER));
                        return TRUE;
                    }
                    if (!CreatePageImageExportFolder(data)) {
                        HwndSetDlgItemText(hwnd, IDC_EXPORT_STATUS, _TRA("Could not create the output folder."));
                        return TRUE;
                    }
                    constexpr int resolutions[] = {96, 150, 200, 300, 600};
                    int idx = (int)ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_EXPORT_DPI));
                    data->dpi = resolutions[idx >= 0 && idx < dimof(resolutions) ? idx : 3];
                    data->jpeg = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_EXPORT_FORMAT)) == 1;
                    data->cancel = data->completed = 0;
                    data->shownCompleted = -1;
                    data->failed = data->tooLarge = false;
                    if (!SetTimer(hwnd, 1, 100, nullptr)) {
                        HwndSetDlgItemText(hwnd, IDC_EXPORT_STATUS, _TRA("Could not start image export."));
                        return TRUE;
                    }
                    EnablePageImageExportInputs(hwnd, false);
                    SendDlgItemMessageW(hwnd, IDC_EXPORT_PROGRESS, PBM_SETRANGE32, 0,
                                        data->lastPage - data->firstPage + 1);
                    HwndSetDlgItemText(hwnd, IDC_EXPORT_HINT, data->outputFolder);
                    HwndSetDlgItemText(hwnd, IDCANCEL, _TRA("Cancel"));
                    AppDialogSyncWarmPushButtons(hwnd);
                    InvalidateRect(GetDlgItem(hwnd, IDCANCEL), nullptr, FALSE);
                    data->thread = StartThread(MkFunc0(ExportPageImagesWorker, data), "ExportPageImages");
                    if (!data->thread) {
                        KillTimer(hwnd, 1);
                        EnablePageImageExportInputs(hwnd, true);
                        HwndSetDlgItemText(hwnd, IDC_EXPORT_STATUS, _TRA("Could not start image export."));
                    }
                    return TRUE;
                }
            }
            break;
        case WM_DESTROY:
            InterlockedExchange(&data->cancel, 1);
            KillTimer(hwnd, 1);
            break;
    }
    return FALSE;
}

void ExportPageImages(MainWindow* win) {
    if (!win || !CanAccessDisk() || !win->AsFixed()) return;
    EngineBase* engine = win->AsFixed()->GetEngine();
    if (!engine || !str::EqI(engine->defaultExt, ".pdf") || engine->PageCount() < 1) return;
    PageImageExport data;
    data.engine = engine;
    engine->AddRef();
    data.currentPage = win->ctrl->CurrentPageNo();
    data.rotation = win->AsFixed()->GetRotation();
    TempStr name = path::GetBaseNameTemp(win->CurrentTab()->filePath);
    data.baseName.Set(str::Dup(path::GetPathNoExtTemp(name)));
    data.folder.Set(str::Dup(path::GetDirTemp(win->CurrentTab()->filePath)));
    CreateAppDialogBox(IDD_EXPORT_PAGE_IMAGES, win->hwndFrame, ExportPageImagesProc, (LPARAM)&data);
}

static TempStr FormatPdfDateTemp() {
    SYSTEMTIME lt{};
    GetLocalTime(&lt);
    TIME_ZONE_INFORMATION tzi{};
    DWORD tz = GetTimeZoneInformation(&tzi);
    int off = 0;
    if (tz == TIME_ZONE_ID_STANDARD) {
        off = -tzi.StandardBias;
    } else if (tz == TIME_ZONE_ID_DAYLIGHT) {
        off = -tzi.DaylightBias;
    }
    char sign = '+';
    if (off < 0) {
        sign = '-';
        off = -off;
    }
    int offH = off / 60;
    int offM = off % 60;
    return str::FormatTemp("D:%04d%02d%02d%02d%02d%02d%c%02d'%02d'", (int)lt.wYear, (int)lt.wMonth, (int)lt.wDay,
                           (int)lt.wHour, (int)lt.wMinute, (int)lt.wSecond, sign, offH, offM);
}

static bool SaveBitmapAsPdf(Bitmap* bmp, const char* destPath) {
    if (!bmp || !destPath) {
        return false;
    }
    PdfCreator* c = new PdfCreator();
    bool ok = c->AddPageFromGdiplusBitmap(bmp, 0);
    if (ok) {
        TempStr now = FormatPdfDateTemp();
        c->SetProperty(kPropCreationDate, now);
        c->SetProperty(kPropModificationDate, now);
        c->SetProperty(kPropCreatorApp, "SumatraPDF");
        ok = c->SaveToFile(destPath);
    }
    delete c;
    return ok;
}

static void SaveImageEdit(ImageEditWindow* ew, bool showErrors) {
    if (!ew->srcBitmap) {
        return;
    }
    if (ew->mode == ImageEditMode::Crop && (ew->cropW <= 0 || ew->cropH <= 0)) {
        return;
    }
    if (ew->mode == ImageEditMode::Resize && (ew->newW <= 0 || ew->newH <= 0)) {
        return;
    }

    WCHAR rawDestW[MAX_PATH + 1]{};
    GetWindowTextW(ew->hwndDestEdit, rawDestW, MAX_PATH);
    TempStr rawDest = ToUtf8Temp(rawDestW);
    if (str::IsEmpty(rawDest)) {
        OnBrowse(ew);
        GetWindowTextW(ew->hwndDestEdit, rawDestW, MAX_PATH);
        rawDest = ToUtf8Temp(rawDestW);
        if (str::IsEmpty(rawDest)) {
            return;
        }
    }

    // ensure extension matches selected format
    int fmtIdx = GetSelectedFormatIdx(ew);
    const char* fmtExt = gImageFormats[fmtIdx].ext;
    TempStr destExt = path::GetExtTemp(rawDest);
    TempStr dest;
    if (str::EqI(destExt, fmtExt)) {
        dest = str::DupTemp(rawDest);
    } else {
        int baseLen = str::Leni(rawDest) - str::Leni(destExt);
        TempStr base = str::DupTemp(rawDest, baseLen);
        dest = str::FormatTemp("%s%s", base, fmtExt);
    }

    Bitmap* result = nullptr;
    if (ew->mode == ImageEditMode::Save) {
        // save as-is
        result = ew->srcBitmap->Clone(0, 0, ew->imgW, ew->imgH, ew->srcBitmap->GetPixelFormat());
        if (!result) {
            if (showErrors) MessageBoxWarning(ew->hwnd, _TRA("Failed to save image"), _TRA("Save Image"));
            return;
        }
    } else if (ew->mode == ImageEditMode::Crop) {
        // create cropped bitmap
        Gdiplus::Rect srcRect(ew->cropX, ew->cropY, ew->cropW, ew->cropH);
        result = ew->srcBitmap->Clone(srcRect, ew->srcBitmap->GetPixelFormat());
        if (!result) {
            if (showErrors) MessageBoxWarning(ew->hwnd, _TRA("Failed to create cropped image"), _TRA("Crop Image"));
            return;
        }
    } else {
        // create resized bitmap
        result = new Bitmap(ew->newW, ew->newH, ew->srcBitmap->GetPixelFormat());
        if (!result) {
            if (showErrors) MessageBoxWarning(ew->hwnd, _TRA("Failed to create resized image"), _TRA("Resize Image"));
            return;
        }
        Graphics g(result);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(ew->srcBitmap, 0, 0, ew->newW, ew->newH);
    }

    TempWStr destW = ToWStrTemp(dest);
    bool saved = false;
    if (gImageFormats[fmtIdx].isPdf) {
        saved = SaveBitmapAsPdf(result, dest);
    } else {
        saved = SaveBitmapWithWIC(result, destW, gImageFormats[fmtIdx].containerFormat);
    }
    delete result;

    if (!saved) {
        if (showErrors) MessageBoxWarning(ew->hwnd, _TRA("Failed to save image"), _TRA("Save Image"));
        return;
    }
    UpdateSaveButtonText(ew);

    // A signature photo still has to come back through Apply Crop. Saving only writes the file.
    if (ew->cropDone) {
        return;
    }

    bool openAfterSave = ew->hwndOpenAfterSave && SendMessageW(ew->hwndOpenAfterSave, BM_GETCHECK, 0, 0) == BST_CHECKED;
    str::ReplaceWithCopy(&gGlobalPrefs->imageSaveFolder, path::GetDirTemp(dest));
    gGlobalPrefs->imageOpenAfterSave = openAfterSave;
    SaveSettings();
    if (!openAfterSave) {
        DestroyWindow(ew->hwnd);
        return;
    }

    // load the saved image
    HWND hwndParent = ew->hwndParent;
    char* savedPath = str::Dup(dest);
    DestroyWindow(ew->hwnd);

    MainWindow* win = FindMainWindowByHwnd(hwndParent);
    if (!win && !gWindows.IsEmpty()) {
        win = gWindows.at(0);
    }
    if (win) {
        LoadArgs args(savedPath, win);
        StartLoadDocument(&args);
    }
    free(savedPath);
}

static void OnSave(ImageEditWindow* ew) {
    SaveImageEdit(ew, true);
}

static void UpdateInfoLabel(ImageEditWindow* ew);
static void UpdateModeButtons(ImageEditWindow* ew);

static void SwitchToSaveMode(ImageEditWindow* ew) {
    ew->mode = ImageEditMode::Save;
    ew->isDragging = false;
    ew->dragEdge = DragEdge::None;
    ew->hoverEdge = DragEdge::None;
    ew->cropX = 0;
    ew->cropY = 0;
    ew->cropW = ew->imgW;
    ew->cropH = ew->imgH;
    ew->newW = ew->imgW;
    ew->newH = ew->imgH;
    SetWindowTextW(ew->hwnd, _TRW("Save Image"));
    UpdateModeButtons(ew);
    UpdateSaveButtonText(ew);
    UpdateInfoLabel(ew);
    LayoutControls(ew);
    InvalidateImageArea(ew);
    SetFocus(ew->hwnd);
}

static void ReplaceSrcBitmap(ImageEditWindow* ew, Bitmap* newBmp) {
    delete ew->srcBitmap;
    ew->srcBitmap = newBmp;
    ew->imgW = (int)newBmp->GetWidth();
    ew->imgH = (int)newBmp->GetHeight();
    CalcImageLayout(ew);
}

static bool IsCropChanged(ImageEditWindow* ew) {
    return ew->cropX != 0 || ew->cropY != 0 || ew->cropW != ew->imgW || ew->cropH != ew->imgH;
}

static bool IsResizeChanged(ImageEditWindow* ew) {
    return ew->newW != ew->imgW || ew->newH != ew->imgH;
}

static void UpdateModeButtons(ImageEditWindow* ew) {
    if (ew->mode == ImageEditMode::Crop) {
        ew->btnCrop->SetText(_TRA("Apply Crop"));
        // A signature crop can accept the whole picture, so Apply stays enabled.
        ew->btnCrop->SetIsEnabled(IsCropChanged(ew) || ew->cropDone != nullptr);
        if (ew->btnResize) {
            ew->btnResize->SetText(_TRA("Resize"));
            ew->btnResize->SetIsEnabled(true);
        }
    } else if (ew->mode == ImageEditMode::Resize) {
        ew->btnCrop->SetText(_TRA("Crop"));
        ew->btnCrop->SetIsEnabled(true);
        ew->btnResize->SetText(_TRA("Apply Resize"));
        ew->btnResize->SetIsEnabled(IsResizeChanged(ew));
    } else {
        // Save mode
        ew->btnCrop->SetText(_TRA("Crop"));
        ew->btnCrop->SetIsEnabled(true);
        ew->btnResize->SetText(_TRA("Resize"));
        ew->btnResize->SetIsEnabled(true);
    }
}

static void ApplyCrop(ImageEditWindow* ew) {
    if (!IsCropChanged(ew)) {
        return;
    }
    if (ew->cropW <= 0 || ew->cropH <= 0) {
        return;
    }
    Gdiplus::Rect srcRect(ew->cropX, ew->cropY, ew->cropW, ew->cropH);
    Bitmap* cropped = ew->srcBitmap->Clone(srcRect, ew->srcBitmap->GetPixelFormat());
    if (cropped) {
        ReplaceSrcBitmap(ew, cropped);
        ew->cropX = 0;
        ew->cropY = 0;
        ew->cropW = ew->imgW;
        ew->cropH = ew->imgH;
        UpdateModeButtons(ew);
        UpdateInfoLabel(ew);
        LayoutControls(ew);
        InvalidateImageArea(ew);
    }
}

static void ApplyResize(ImageEditWindow* ew) {
    if (!IsResizeChanged(ew)) {
        return;
    }
    if (ew->newW <= 0 || ew->newH <= 0) {
        return;
    }
    Bitmap* resized = new Bitmap(ew->newW, ew->newH, ew->srcBitmap->GetPixelFormat());
    if (resized) {
        Graphics g(resized);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(ew->srcBitmap, 0, 0, ew->newW, ew->newH);
        ReplaceSrcBitmap(ew, resized);
        ew->newW = ew->imgW;
        ew->newH = ew->imgH;
        UpdateModeButtons(ew);
        UpdateInfoLabel(ew);
        LayoutControls(ew);
        InvalidateImageArea(ew);
    }
}

static void SwitchToCropMode(ImageEditWindow* ew) {
    // apply pending resize before switching
    if (ew->mode == ImageEditMode::Resize && IsResizeChanged(ew)) {
        ApplyResize(ew);
    }
    ew->mode = ImageEditMode::Crop;
    ew->cropX = 0;
    ew->cropY = 0;
    ew->cropW = ew->imgW;
    ew->cropH = ew->imgH;
    SetWindowTextW(ew->hwnd, _TRW("Crop Image"));
    UpdateModeButtons(ew);
    UpdateSaveButtonText(ew);
    UpdateInfoLabel(ew);
    LayoutControls(ew);
    InvalidateImageArea(ew);
    SetFocus(ew->hwnd);
}

static void SwitchToResizeMode(ImageEditWindow* ew) {
    // apply pending crop before switching
    if (ew->mode == ImageEditMode::Crop && IsCropChanged(ew)) {
        ApplyCrop(ew);
    }
    ew->mode = ImageEditMode::Resize;
    ew->newW = ew->imgW;
    ew->newH = ew->imgH;
    SetWindowTextW(ew->hwnd, _TRW("Resize Image"));
    UpdateModeButtons(ew);
    UpdateSaveButtonText(ew);
    UpdateInfoLabel(ew);
    LayoutControls(ew);
    InvalidateImageArea(ew);
    SetFocus(ew->hwnd);
}

static Bitmap* CloneCropResult(ImageEditWindow* ew) {
    if (!ew || !ew->srcBitmap || ew->cropW <= 0 || ew->cropH <= 0) {
        return nullptr;
    }
    if (!IsCropChanged(ew)) {
        return ew->srcBitmap->Clone(0, 0, ew->imgW, ew->imgH, PixelFormat32bppARGB);
    }
    Gdiplus::Rect srcRect(ew->cropX, ew->cropY, ew->cropW, ew->cropH);
    return ew->srcBitmap->Clone(srcRect, PixelFormat32bppARGB);
}

static void OnCropButton(ImageEditWindow* ew) {
    if (ew->mode == ImageEditMode::Crop) {
        if (ew->cropDone) {
            Bitmap* cropped = CloneCropResult(ew);
            ImageCropDoneFn fn = ew->cropDone;
            void* user = ew->cropDoneUser;
            ew->cropDone = nullptr;
            ew->cropDoneUser = nullptr;
            if (cropped && fn) {
                fn(cropped, user);
            } else {
                delete cropped;
            }
            if (ew->hwnd) {
                DestroyWindow(ew->hwnd);
            }
            return;
        }
        ApplyCrop(ew);
    } else {
        SwitchToCropMode(ew);
    }
}

static void OnResizeButton(ImageEditWindow* ew) {
    if (ew->mode == ImageEditMode::Resize) {
        ApplyResize(ew);
    } else {
        SwitchToResizeMode(ew);
    }
}

static Bitmap* CreateBitmapForClipboard(ImageEditWindow* ew) {
    if (!ew || !ew->srcBitmap) {
        return nullptr;
    }
    if (ew->mode == ImageEditMode::Crop && ew->cropW > 0 && ew->cropH > 0) {
        Gdiplus::Rect srcRect(ew->cropX, ew->cropY, ew->cropW, ew->cropH);
        return ew->srcBitmap->Clone(srcRect, ew->srcBitmap->GetPixelFormat());
    }
    if (ew->mode == ImageEditMode::Resize && ew->newW > 0 && ew->newH > 0 &&
        (ew->newW != ew->imgW || ew->newH != ew->imgH)) {
        Bitmap* resized = new Bitmap(ew->newW, ew->newH, ew->srcBitmap->GetPixelFormat());
        if (!resized) {
            return nullptr;
        }
        Graphics g(resized);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(ew->srcBitmap, 0, 0, ew->newW, ew->newH);
        return resized;
    }
    return ew->srcBitmap->Clone(0, 0, ew->imgW, ew->imgH, ew->srcBitmap->GetPixelFormat());
}

static bool CopyEditedImageToClipboard(ImageEditWindow* ew) {
    Bitmap* bmp = CreateBitmapForClipboard(ew);
    if (!bmp) {
        return false;
    }
    HBITMAP tmp = nullptr;
    Status status = bmp->GetHBITMAP((Gdiplus::ARGB)0xffffffff, &tmp);
    delete bmp;
    if (status != Ok || !tmp) {
        return false;
    }
    ScopedGdiObj<HBITMAP> hbmp(tmp);
    return CopyImageToClipboard(tmp, false);
}

LRESULT CALLBACK WndProcImageEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static void CancelImageEditDrag(ImageEditWindow* ew) {
    if (!ew || !ew->isDragging) return;
    ew->isDragging = false;
    if (ew->mode == ImageEditMode::Crop) {
        ew->cropX = ew->dragCropX;
        ew->cropY = ew->dragCropY;
        ew->cropW = ew->dragCropW;
        ew->cropH = ew->dragCropH;
    } else {
        ew->newW = ew->dragNewW;
        ew->newH = ew->dragNewH;
    }
    if (GetCapture() == ew->hwnd) ReleaseCapture();
    UpdateInfoLabel(ew);
    UpdateModeButtons(ew);
    InvalidateImageArea(ew);
}

LRESULT CALLBACK WndProcImageEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ImageEditWindow* ew;

    LRESULT res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res != 0) {
        return res;
    }

    switch (msg) {
        case WM_CREATE:
            break;

        case WM_DPICHANGED: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (ew) {
                CancelImageEditDrag(ew);
                int dpi = RoundUp((int)LOWORD(wp), 4);
                if (dpi < 72) {
                    dpi = 96;
                }
                ApplyImageFont(ew, dpi);
                RECT* prc = (RECT*)lp;
                if (prc) {
                    SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                CalcImageLayout(ew);
                LayoutControls(ew);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (ew) {
                auto* limits = (MINMAXINFO*)lp;
                limits->ptMinTrackSize.x = ImgPx(ew, kMinWindowWidth);
                limits->ptMinTrackSize.y = GetControlAreaDy(ew) + ImgPx(ew, 140);
                return 0;
            }
            return DefWindowProc(hwnd, msg, wp, lp);
        }

        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            CancelImageEditDrag(FindImageEditWindowByHwnd(hwnd));
            return 0;

        case WM_SIZE: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (ew) {
                CalcImageLayout(ew);
                LayoutControls(ew);
                if (ew->mode == ImageEditMode::Crop) {
                    InvalidateImageArea(ew);
                    RECT crc{};
                    GetClientRect(hwnd, &crc);
                    RECT ctrl{0, ew->imgAreaH, crc.right, crc.bottom};
                    InvalidateRect(hwnd, &ctrl, TRUE);
                } else {
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        }

        case WM_PAINT: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (!ew) return 0;
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            // double-buffer only the image area to avoid flicker
            Rect cRc = ClientRect(hwnd);
            int paintH = ew->imgAreaH;
            if (paintH > cRc.dy) {
                paintH = cRc.dy;
            }
            if (cRc.dx > 0 && paintH > 0 && ps.rcPaint.top < paintH) {
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP memBmp = CreateCompatibleBitmap(hdc, cRc.dx, paintH);
                HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
                if (ew->mode == ImageEditMode::Save) {
                    PaintSaveImage(ew, memDC);
                } else if (ew->mode == ImageEditMode::Crop) {
                    PaintCropImage(ew, memDC);
                } else {
                    PaintResizeImage(ew, memDC);
                }
                BitBlt(hdc, 0, 0, cRc.dx, paintH, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, oldBmp);
                DeleteObject(memBmp);
                DeleteDC(memDC);
            }
            FillControlStrip(ew, hdc);
            PaintInfoLabel(ew, hdc);
            HFONT oldFont = (HFONT)SelectObject(hdc, ew->hFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, ThemeWindowTextColor());
            RECT hint = ToRECT(ew->hintRc);
            UINT flags = DT_WORDBREAK | DT_NOPREFIX;
            if (trans::IsCurrLangRtl()) flags |= DT_RIGHT | DT_RTLREADING;
            DrawTextW(hdc, ToWStrTemp(ImageCropHint()), -1, &hint, flags);
            SelectObject(hdc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (!ew) return 0;
            // paint control area background, skip image area (double-buffered)
            HDC hdc = (HDC)wp;
            FillControlStrip(ew, hdc);
            PaintInfoLabel(ew, hdc);
            HFONT oldFont = (HFONT)SelectObject(hdc, ew->hFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, ThemeWindowTextColor());
            RECT hint = ToRECT(ew->hintRc);
            UINT flags = DT_WORDBREAK | DT_NOPREFIX;
            if (trans::IsCurrLangRtl()) flags |= DT_RIGHT | DT_RTLREADING;
            DrawTextW(hdc, ToWStrTemp(ImageCropHint()), -1, &hint, flags);
            SelectObject(hdc, oldFont);
            return 1;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORDLG: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (!ew) {
                break;
            }
            HBRUSH chrome = ImageEditChromeBrush(ew);
            HBRUSH br = AppDialogCtlColorBrush(msg, wp, lp, chrome ? chrome : ew->brushes.background,
                                               ew->brushes.control, ew->hwndDestEdit);
            if (br) {
                return (LRESULT)br;
            }
            return DefWindowProc(hwnd, msg, wp, lp);
        }

        case WM_MOUSEMOVE: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (!ew || ew->mode == ImageEditMode::Save) {
                break;
            }
            int mx = GET_X_LPARAM(lp);
            int my = GET_Y_LPARAM(lp);
            if (ew->isDragging) {
                if (ew->mode == ImageEditMode::Crop) {
                    int imgDx = DisplayToImageX(ew, mx) - DisplayToImageX(ew, ew->dragStart.x);
                    int imgDy = DisplayToImageY(ew, my) - DisplayToImageY(ew, ew->dragStart.y);

                    auto edge = ew->dragEdge;
                    int nx = ew->dragCropX;
                    int ny = ew->dragCropY;
                    int nw = ew->dragCropW;
                    int nh = ew->dragCropH;

                    if (edge == DragEdge::Draw) {
                        int sx = DisplayToImageX(ew, ew->dragStart.x);
                        int sy = DisplayToImageY(ew, ew->dragStart.y);
                        int ex = DisplayToImageX(ew, mx);
                        int ey = DisplayToImageY(ew, my);
                        if (abs(mx - ew->dragStart.x) <= ImgPx(ew, 4) && abs(my - ew->dragStart.y) <= ImgPx(ew, 4))
                            return 0;
                        nx = std::min(sx, ex);
                        ny = std::min(sy, ey);
                        nw = abs(ex - sx);
                        nh = abs(ey - sy);
                        if (nw < 1 || nh < 1) return 0;
                    }
                    if (edge == DragEdge::Left || edge == DragEdge::TopLeft || edge == DragEdge::BottomLeft) {
                        nx = ew->dragCropX + imgDx;
                        nw = ew->dragCropW - imgDx;
                    }
                    if (edge == DragEdge::Right || edge == DragEdge::TopRight || edge == DragEdge::BottomRight) {
                        nw = ew->dragCropW + imgDx;
                    }
                    if (edge == DragEdge::Top || edge == DragEdge::TopLeft || edge == DragEdge::TopRight) {
                        ny = ew->dragCropY + imgDy;
                        nh = ew->dragCropH - imgDy;
                    }
                    if (edge == DragEdge::Bottom || edge == DragEdge::BottomLeft || edge == DragEdge::BottomRight) {
                        nh = ew->dragCropH + imgDy;
                    }
                    if (edge == DragEdge::Move) {
                        nx = ew->dragCropX + imgDx;
                        ny = ew->dragCropY + imgDy;
                        // clamp to image bounds
                        if (nx < 0) {
                            nx = 0;
                        }
                        if (ny < 0) {
                            ny = 0;
                        }
                        if (nx + nw > ew->imgW) {
                            nx = ew->imgW - nw;
                        }
                        if (ny + nh > ew->imgH) {
                            ny = ew->imgH - nh;
                        }
                    }

                    // enforce minimum size and bounds
                    if (nw < 1) {
                        nw = 1;
                        nx = ew->cropX;
                    }
                    if (nh < 1) {
                        nh = 1;
                        ny = ew->cropY;
                    }
                    if (nx < 0) {
                        nw += nx;
                        nx = 0;
                    }
                    if (ny < 0) {
                        nh += ny;
                        ny = 0;
                    }
                    if (nx + nw > ew->imgW) {
                        nw = ew->imgW - nx;
                    }
                    if (ny + nh > ew->imgH) {
                        nh = ew->imgH - ny;
                    }

                    ew->cropX = nx;
                    ew->cropY = ny;
                    ew->cropW = nw;
                    ew->cropH = nh;
                    UpdateInfoLabel(ew);
                    InvalidateImageArea(ew);
                } else {
                    // resize mode
                    int dx = mx - ew->dragStart.x;
                    int dy = my - ew->dragStart.y;
                    // convert pixel deltas to image-space deltas
                    int imgDx = DisplayToImageW(ew, dx);
                    int imgDy = DisplayToImageH(ew, dy);

                    auto edge = ew->dragEdge;
                    int nw = ew->dragNewW;
                    int nh = ew->dragNewH;

                    // left/right edges change width
                    if (edge == DragEdge::Left || edge == DragEdge::TopLeft || edge == DragEdge::BottomLeft) {
                        nw = ew->dragNewW - imgDx * 2; // symmetric resize
                    }
                    if (edge == DragEdge::Right || edge == DragEdge::TopRight || edge == DragEdge::BottomRight) {
                        nw = ew->dragNewW + imgDx * 2;
                    }
                    // top/bottom edges change height
                    if (edge == DragEdge::Top || edge == DragEdge::TopLeft || edge == DragEdge::TopRight) {
                        nh = ew->dragNewH - imgDy * 2;
                    }
                    if (edge == DragEdge::Bottom || edge == DragEdge::BottomLeft || edge == DragEdge::BottomRight) {
                        nh = ew->dragNewH + imgDy * 2;
                    }

                    if (nw < 1) {
                        nw = 1;
                    }
                    if (nh < 1) {
                        nh = 1;
                    }

                    ew->newW = nw;
                    ew->newH = nh;
                    GrowWindowIfNeeded(ew, edge);
                    UpdateInfoLabel(ew);
                    InvalidateImageArea(ew);
                }
            } else {
                DragEdge edge;
                if (ew->mode == ImageEditMode::Crop) {
                    edge = HitTestCropEdge(ew, mx, my);
                } else {
                    edge = HitTestResizeEdge(ew, mx, my);
                }
                ew->hoverEdge = edge;
                SetCursor(GetCursorForEdge(edge));
            }
            return 0;
        }

        case WM_MBUTTONDOWN:
        case WM_LBUTTONDOWN: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (!ew || ew->mode == ImageEditMode::Save || ew->isDragging) {
                break;
            }
            int mx = GET_X_LPARAM(lp);
            int my = GET_Y_LPARAM(lp);
            DragEdge edge;
            if (msg == WM_MBUTTONDOWN) {
                if (ew->mode != ImageEditMode::Crop || mx < ImageToDisplayX(ew, ew->cropX) ||
                    mx > ImageToDisplayX(ew, ew->cropX + ew->cropW) || my < ImageToDisplayY(ew, ew->cropY) ||
                    my > ImageToDisplayY(ew, ew->cropY + ew->cropH)) {
                    return 0;
                }
                edge = DragEdge::Move;
            } else if (ew->mode == ImageEditMode::Crop) {
                edge = HitTestCropEdge(ew, mx, my);
            } else {
                edge = HitTestResizeEdge(ew, mx, my);
            }
            if (edge != DragEdge::None) {
                SetFocus(hwnd);
                ew->isDragging = true;
                ew->dragWithMiddleButton = msg == WM_MBUTTONDOWN;
                ew->dragEdge = edge;
                SetCursor(GetCursorForEdge(edge));
                ew->dragStart = {mx, my};
                if (ew->mode == ImageEditMode::Crop) {
                    ew->dragCropX = ew->cropX;
                    ew->dragCropY = ew->cropY;
                    ew->dragCropW = ew->cropW;
                    ew->dragCropH = ew->cropH;
                } else {
                    ew->dragNewW = ew->newW;
                    ew->dragNewH = ew->newH;
                }
                SetCapture(hwnd);
            }
            return 0;
        }

        case WM_MBUTTONUP:
        case WM_LBUTTONUP: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (ew && ew->isDragging && ew->dragWithMiddleButton == (msg == WM_MBUTTONUP)) {
                // Include the release position even when no final mouse move
                // was delivered before the button-up message.
                SendMessageW(hwnd, WM_MOUSEMOVE, wp, lp);
                ew->isDragging = false;
                ReleaseCapture();
                UpdateModeButtons(ew);
            }
            return 0;
        }

        case WM_SETCURSOR: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (ew && ew->mode != ImageEditMode::Save && LOWORD(lp) == HTCLIENT) {
                if (ew->isDragging) {
                    SetCursor(GetCursorForEdge(ew->dragEdge));
                    return TRUE;
                }
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                DragEdge edge;
                if (ew->mode == ImageEditMode::Crop) {
                    edge = HitTestCropEdge(ew, pt.x, pt.y);
                } else {
                    edge = HitTestResizeEdge(ew, pt.x, pt.y);
                }
                if (edge != DragEdge::None) {
                    SetCursor(GetCursorForEdge(edge));
                    return TRUE;
                }
            }
            return DefWindowProc(hwnd, msg, wp, lp);
        }

        case WM_CHAR:
            if (VK_ESCAPE == wp) {
                return 0;
            }
            break;

        case WM_KEYDOWN: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (!ew) {
                break;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wp == 'C') {
                CopyEditedImageToClipboard(ew);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (ew->isDragging) {
                    CancelImageEditDrag(ew);
                    return 0;
                }
                if (ew->mode != ImageEditMode::Save) {
                    SwitchToSaveMode(ew);
                } else {
                    DestroyWindow(hwnd);
                }
                return 0;
            }
            if (ew->mode == ImageEditMode::Resize) {
                // left/right change width, up/down change height
                if (wp == VK_LEFT) {
                    ew->newW -= 1;
                } else if (wp == VK_RIGHT) {
                    ew->newW += 1;
                } else if (wp == VK_UP) {
                    ew->newH += 1;
                } else if (wp == VK_DOWN) {
                    ew->newH -= 1;
                } else {
                    break;
                }
                if (ew->newW < 1) {
                    ew->newW = 1;
                }
                if (ew->newH < 1) {
                    ew->newH = 1;
                }
                UpdateInfoLabel(ew);
                UpdateModeButtons(ew);
                InvalidateImageArea(ew);
                return 0;
            }
            // crop mode: need hoverEdge and directional deltas
            {
                auto edge = ew->hoverEdge;
                if (edge == DragEdge::None) {
                    break;
                }
                int dx = 0, dy = 0;
                if (wp == VK_LEFT) {
                    dx = -1;
                } else if (wp == VK_RIGHT) {
                    dx = 1;
                } else if (wp == VK_UP) {
                    dy = -1;
                } else if (wp == VK_DOWN) {
                    dy = 1;
                }
                if (edge == DragEdge::Move) {
                    ew->cropX += dx;
                    ew->cropY += dy;
                    if (ew->cropX < 0) {
                        ew->cropX = 0;
                    }
                    if (ew->cropY < 0) {
                        ew->cropY = 0;
                    }
                    if (ew->cropX + ew->cropW > ew->imgW) {
                        ew->cropX = ew->imgW - ew->cropW;
                    }
                    if (ew->cropY + ew->cropH > ew->imgH) {
                        ew->cropY = ew->imgH - ew->cropH;
                    }
                } else {
                    // nudge individual edge
                    if (dx != 0 &&
                        (edge == DragEdge::Left || edge == DragEdge::TopLeft || edge == DragEdge::BottomLeft)) {
                        ew->cropX += dx;
                        ew->cropW -= dx;
                    }
                    if (dx != 0 &&
                        (edge == DragEdge::Right || edge == DragEdge::TopRight || edge == DragEdge::BottomRight)) {
                        ew->cropW += dx;
                    }
                    if (dy != 0 && (edge == DragEdge::Top || edge == DragEdge::TopLeft || edge == DragEdge::TopRight)) {
                        ew->cropY += dy;
                        ew->cropH -= dy;
                    }
                    if (dy != 0 &&
                        (edge == DragEdge::Bottom || edge == DragEdge::BottomLeft || edge == DragEdge::BottomRight)) {
                        ew->cropH += dy;
                    }
                    // clamp
                    if (ew->cropX < 0) {
                        ew->cropW += ew->cropX;
                        ew->cropX = 0;
                    }
                    if (ew->cropY < 0) {
                        ew->cropH += ew->cropY;
                        ew->cropY = 0;
                    }
                    if (ew->cropW < 1) {
                        ew->cropW = 1;
                    }
                    if (ew->cropH < 1) {
                        ew->cropH = 1;
                    }
                    if (ew->cropX + ew->cropW > ew->imgW) {
                        ew->cropW = ew->imgW - ew->cropX;
                    }
                    if (ew->cropY + ew->cropH > ew->imgH) {
                        ew->cropH = ew->imgH - ew->cropY;
                    }
                }
            } // end crop mode block
            UpdateInfoLabel(ew);
            UpdateModeButtons(ew);
            InvalidateImageArea(ew);
            return 0;
        }

        case WM_COMMAND: {
            ew = FindImageEditWindowByHwnd(hwnd);
            if (!ew) {
                break;
            }
            int code = HIWORD(wp);
            if ((HWND)lp == ew->hwndOpenAfterSave && code == BN_CLICKED) {
                gGlobalPrefs->imageOpenAfterSave =
                    SendMessageW(ew->hwndOpenAfterSave, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SaveSettings();
                return 0;
            }
            // browse button
            if ((HWND)lp == ew->hwndBrowseBtn && code == BN_CLICKED) {
                OnBrowse(ew);
                UpdateSaveButtonText(ew);
                return 0;
            }
            // dest edit changed
            if ((HWND)lp == ew->hwndDestEdit && code == EN_CHANGE) {
                UpdateSaveButtonText(ew);
                return 0;
            }
            break;
        }

        case WM_DESTROY:
            ew = FindImageEditWindowByHwnd(hwnd);
            if (ew) {
                UnregisterAppDialogForTheme(hwnd);
                gImageEditWindows.Remove(ew);
                delete ew;
            }
            break;

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

static void ImageEditApplyFieldTheme(ImageEditWindow* ew) {
    if (!ew || !ew->hwndDestEdit || !DynSetWindowTheme) {
        return;
    }
    // A themed EDIT ignores WM_CTLCOLOREDIT and stays COLOR_WINDOW, which is
    // white on Warm. An empty theme lets the field brush show through.
    if (ThemeUsesDarkChrome()) {
        DynSetWindowTheme(ew->hwndDestEdit, nullptr, nullptr);
    } else {
        DynSetWindowTheme(ew->hwndDestEdit, L"", L"");
    }
}

static void ImageEditThemeRefreshCb(HWND hwnd, void* ctx) {
    auto* ew = (ImageEditWindow*)ctx;
    if (!ew) {
        return;
    }
    ew->brushes.Recreate();
    AppDialogApplyChrome(hwnd);
    ImageEditApplyFieldTheme(ew);
    CalcImageLayout(ew);
    LayoutControls(ew);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static HWND CreateImageEditWindow(HWND owner, MainWindow* win, ImageEditMode mode, const char* filePath,
                                  RenderedBitmap* rbmp, bool selectPdf, ImageCropDoneFn cropDone, void* cropDoneUser,
                                  bool show) {
    if (!owner) {
        return nullptr;
    }

    ProbeImageFormats();

    Bitmap* bmp = nullptr;
    bool fromRenderedBitmap = (rbmp != nullptr);

    if (fromRenderedBitmap) {
        // create GDI+ bitmap from RenderedBitmap
        HBITMAP hbmp = rbmp->GetBitmap();
        if (!hbmp) {
            return nullptr;
        }
        bmp = new Bitmap(hbmp, nullptr);
        if (!bmp || bmp->GetWidth() == 0) {
            delete bmp;
            return nullptr;
        }
    } else {
        // load from current tab's file
        if (!filePath) {
            WindowTab* tab = win ? win->CurrentTab() : nullptr;
            if (!tab || !tab->filePath) {
                return nullptr;
            }
            Kind engineType = tab->GetEngineType();
            if (engineType != kindEngineImage) {
                return nullptr;
            }
            filePath = tab->filePath;
        }
        ByteSlice data = file::ReadFile(filePath);
        if (data.empty()) {
            return nullptr;
        }
        bmp = BitmapFromData(data);
        data.Free();
        if (!bmp) {
            return nullptr;
        }
    }

    int imgW = (int)bmp->GetWidth();
    int imgH = (int)bmp->GetHeight();
    if (imgW <= 0 || imgH <= 0) {
        delete bmp;
        return nullptr;
    }

    auto* ew = new ImageEditWindow();
    ew->mode = mode;
    ew->cropDone = cropDone;
    ew->cropDoneUser = cropDoneUser;
    ew->fromRenderedBitmap = fromRenderedBitmap;
    ew->filePath = filePath ? str::Dup(filePath) : nullptr;
    ew->srcBitmap = bmp;
    ew->imgW = imgW;
    ew->imgH = imgH;

    if (mode == ImageEditMode::Crop) {
        // initial crop = full image
        ew->cropX = 0;
        ew->cropY = 0;
        ew->cropW = imgW;
        ew->cropH = imgH;
    } else {
        ew->newW = imgW;
        ew->newH = imgH;
    }

    gImageEditWindows.Append(ew);

    HMODULE h = GetModuleHandleW(nullptr);
    WNDCLASSEX wcex = {};
    FillWndClassEx(wcex, kImageEditWinClassName, WndProcImageEdit);
    wcex.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    WCHAR* iconName = MAKEINTRESOURCEW(GetAppIconID());
    wcex.hIcon = LoadIconW(h, iconName);
    RegisterClassEx(&wcex);

    int dpi = DpiGet(owner);
    if (dpi < 72) {
        dpi = 96;
    }
    ew->dpi = dpi;
    // calculate window size: image at 100% + padding + control area, clamped to screen
    int pad = ImgPx(ew, kImagePadding);
    int wantW = imgW + 2 * pad;
    int controlDy = GetControlAreaDy(ew);
    int wantH = imgH + 2 * pad + controlDy;
    // add window chrome
    RECT rc = {0, 0, wantW, wantH};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    int winW = rc.right - rc.left;
    int minW = ImgPx(ew, kMinWindowWidth);
    if (winW < minW) {
        winW = minW;
    }
    int winH = rc.bottom - rc.top;
    // clamp to screen
    HMONITOR hMon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfo(hMon, &mi);
    int screenW = mi.rcWork.right - mi.rcWork.left;
    int screenH = mi.rcWork.bottom - mi.rcWork.top;
    if (winW > screenW) {
        winW = screenW;
    }
    // A phone photo is taller than the screen. Open at two thirds so the
    // window does not sit edge to edge; the picture scales to fit.
    int maxH = screenH * 2 / 3;
    if (maxH < ImgPx(ew, 360)) {
        maxH = screenH;
    }
    if (winH > maxH) {
        winH = maxH;
    }

    const WCHAR* title = _TRW("Save Image");
    if (mode == ImageEditMode::Crop) {
        title = _TRW("Crop Image");
    } else if (mode == ImageEditMode::Resize) {
        title = _TRW("Resize Image");
    }
    HWND hwnd = CreateWindowExW(0, kImageEditWinClassName, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, winW, winH, nullptr, nullptr, h, nullptr);
    if (!hwnd) {
        gImageEditWindows.Remove(ew);
        delete ew;
        return nullptr;
    }

    ew->hwnd = hwnd;
    ew->hwndParent = owner;

    ApplyImageFont(ew, ew->dpi);

    // create child controls
    // row 1: file path label (read-only) — hidden when from RenderedBitmap
    DWORD pathLabelStyle = WS_CHILD | SS_LEFT | SS_PATHELLIPSIS;
    if (!fromRenderedBitmap) {
        pathLabelStyle |= WS_VISIBLE;
    }
    ew->hwndPathLabel = CreateWindowExW(0, L"STATIC", filePath ? ToWStrTemp(filePath) : L"", pathLabelStyle, 0, 0, 0, 0,
                                        hwnd, nullptr, h, nullptr);
    SendMessageW(ew->hwndPathLabel, WM_SETFONT, (WPARAM)ew->hFont, TRUE);

    // row 2: dest edit + browse
    TempStr suggested = filePath ? str::DupTemp(filePath) : nullptr;
    if (!cropDone && !str::IsEmpty(gGlobalPrefs->imageSaveFolder) && dir::Exists(gGlobalPrefs->imageSaveFolder)) {
        suggested =
            path::JoinTemp(gGlobalPrefs->imageSaveFolder, filePath ? path::GetBaseNameTemp(filePath) : "image.png");
    }
    TempStr destPath = suggested ? MakeUniqueFilePathTemp(suggested) : str::DupTemp("");
    ew->hwndDestEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, ToWStrTemp(destPath),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, nullptr, h, nullptr);
    SendMessageW(ew->hwndDestEdit, WM_SETFONT, (WPARAM)ew->hFont, TRUE);

    ew->hwndBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                        0, 0, hwnd, nullptr, h, nullptr);
    SendMessageW(ew->hwndBrowseBtn, WM_SETFONT, (WPARAM)ew->hFont, TRUE);

    // row 3: info label
    TempStr infoStr;
    if (mode == ImageEditMode::Save) {
        infoStr = str::FormatTemp("%d x %d", imgW, imgH);
    } else if (mode == ImageEditMode::Crop) {
        infoStr = FormatCropInfoTemp(imgW, imgH, imgW, imgH, 0, 0);
    } else {
        infoStr = FormatResizeInfoTemp(imgW, imgH, imgW, imgH);
    }
    // No STATIC: its background is the window color, a plate under the glyphs.
    str::ReplaceWithCopy(&ew->infoText, infoStr);

    // buttons
    {
        auto* btn = new Button();
        Button::CreateArgs args;
        args.parent = hwnd;
        args.font = ew->hFont;
        args.text = _TRA("Save");
        btn->Create(args);
        btn->onClick = MkFunc0<ImageEditWindow>(OnSave, ew);
        ew->btnSave = btn;
    }
    {
        auto* btn = new Button();
        Button::CreateArgs args;
        args.parent = hwnd;
        args.font = ew->hFont;
        args.text = _TRA("Crop");
        btn->Create(args);
        btn->onClick = MkFunc0<ImageEditWindow>(OnCropButton, ew);
        ew->btnCrop = btn;
    }
    // A signature photo is sized when it is placed. Resize only confuses that step.
    if (!cropDone) {
        auto* btn = new Button();
        Button::CreateArgs args;
        args.parent = hwnd;
        args.font = ew->hFont;
        args.text = _TRA("Resize");
        btn->Create(args);
        btn->onClick = MkFunc0<ImageEditWindow>(OnResizeButton, ew);
        ew->btnResize = btn;
    }
    {
        auto* btn = new Button();
        Button::CreateArgs args;
        args.parent = hwnd;
        args.font = ew->hFont;
        args.text = _TRA("Rotate Left");
        btn->Create(args);
        btn->onClick = MkFunc0<ImageEditWindow>(OnRotateLeft, ew);
        ew->btnRotateLeft = btn;
    }
    {
        auto* btn = new Button();
        Button::CreateArgs args;
        args.parent = hwnd;
        args.font = ew->hFont;
        args.text = _TRA("Rotate Right");
        btn->Create(args);
        btn->onClick = MkFunc0<ImageEditWindow>(OnRotateRight, ew);
        ew->btnRotateRight = btn;
    }

    // format dropdown
    {
        auto* dd = new DropDown();
        DropDown::CreateArgs args;
        args.parent = hwnd;
        args.font = ew->hFont;
        dd->Create(args);
        StrVec items;
        int wantFmtIdx = selectPdf ? kPdfFormatIdx : kDefaultFormatIdx;
        int defaultDdIdx = 0;
        for (int i = 0; i < (int)dimof(gImageFormats); i++) {
            if (!gImageFormats[i].available) {
                continue;
            }
            if (i == wantFmtIdx) {
                defaultDdIdx = ew->formatIndices.Size();
            }
            ew->formatIndices.Append(i);
            items.Append(gImageFormats[i].label);
        }
        dd->SetItems(items);
        dd->SetCurrentSelection(defaultDdIdx);
        dd->onSelectionChanged = MkFunc0<ImageEditWindow>(OnFormatChanged, ew);
        ew->dropFormat = dd;
    }

    if (selectPdf) {
        OnFormatChanged(ew);
    }

    if (!cropDone) {
        DWORD ex = trans::IsCurrLangRtl() ? WS_EX_RTLREADING | WS_EX_RIGHT : 0;
        ew->hwndOpenAfterSave = CreateWindowExW(ex, WC_BUTTONW, _TRW("Open image after saving"),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE, 0,
                                                0, 0, 0, hwnd, nullptr, h, nullptr);
        SendMessageW(ew->hwndOpenAfterSave, WM_SETFONT, (WPARAM)ew->hFont, TRUE);
        SendMessageW(ew->hwndOpenAfterSave, BM_SETCHECK, gGlobalPrefs->imageOpenAfterSave ? BST_CHECKED : BST_UNCHECKED,
                     0);
    }
    // Measure translated controls after they exist. Long labels can wrap the
    // action row and need more height than the initial estimate.
    Rect client = ClientRect(hwnd);
    int minClientH = GetControlAreaDy(ew) + ImgPx(ew, 140);
    if (client.dy < minClientH) {
        RECT outer{};
        GetWindowRect(hwnd, &outer);
        SetWindowPos(hwnd, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top + minClientH - client.dy,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    CalcImageLayout(ew);
    UpdateModeButtons(ew);
    LayoutControls(ew);
    UpdateSaveButtonText(ew);

    CenterDialog(hwnd, owner);
    HwndEnsureVisible(hwnd);
    ew->brushes.Create();
    AppDialogApplyChrome(hwnd);
    ImageEditApplyFieldTheme(ew);
    RegisterAppDialogForTheme(hwnd, ImageEditThemeRefreshCb, ew);
    if (show) ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

HWND ShowImageEditWindow(MainWindow* win, ImageEditMode mode, const char* filePath, RenderedBitmap* rbmp,
                         bool selectPdf, ImageCropDoneFn cropDone, void* cropDoneUser) {
    return win ? CreateImageEditWindow(win->hwndFrame, win, mode, filePath, rbmp, selectPdf, cropDone, cropDoneUser,
                                       true)
               : nullptr;
}

bool TestImageEditWindow() {
    const char* outputDir = getenv("SUMATRA_TEST_IMAGE_EDIT_DIR");
    if (!outputDir || !dir::Exists(outputDir)) return false;
    FILE* report = fopen(path::JoinTemp(outputDir, "results.txt"), "w");
    if (!report) return false;
    defer {
        fclose(report);
    };
    HWND owner = CreateWindowExW(0, WC_STATICW, L"Image tests", WS_POPUP, 0, 0, 800, 900, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    if (!owner) return false;
    defer {
        DestroyWindow(owner);
    };
    Bitmap source(240, 160, PixelFormat32bppARGB);
    {
        Graphics g(&source);
        g.Clear(Color(255, 255, 0, 0));
        Gdiplus::SolidBrush blue(Color(255, 0, 0, 255));
        g.FillRectangle(&blue, 120, 0, 120, 160);
    }
    HBITMAP hbmp = nullptr;
    source.GetHBITMAP(Color(255, 255, 255, 255), &hbmp);
    if (!hbmp) return false;
    RenderedBitmap rendered(hbmp, Size(240, 160));
    bool ok = true;
    const char* themes[] = {"Light-Warm", "Light-White", "Dark-Dracula", "Dark-Black"};
    bool oldEscToExit = gGlobalPrefs->escToExit;
    defer {
        gGlobalPrefs->escToExit = oldEscToExit;
    };
    for (const char* theme : themes) {
        SetTheme(theme);
        for (bool escToExit : {false, true}) {
            gGlobalPrefs->escToExit = escToExit;
            for (ImageEditMode mode : {ImageEditMode::Crop, ImageEditMode::Resize, ImageEditMode::Save}) {
                HWND hwnd =
                    CreateImageEditWindow(owner, nullptr, mode, nullptr, &rendered, false, nullptr, nullptr, false);
                ImageEditWindow* ew = FindImageEditWindowByHwnd(hwnd);
                if (!ew) return false;
                SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
                bool valid =
                    mode == ImageEditMode::Save ? !IsWindow(hwnd) : IsWindow(hwnd) && ew->mode == ImageEditMode::Save;
                if (mode != ImageEditMode::Save) {
                    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
                    valid = valid && !IsWindow(hwnd);
                }
                fprintf(report, "Esc layers theme=%s pref=%d mode=%d: %s\n", theme, escToExit, (int)mode,
                        valid ? "PASS" : "FAIL");
                ok = valid && ok;
            }
            HWND hwnd = CreateImageEditWindow(owner, nullptr, ImageEditMode::Crop, nullptr, &rendered, false, nullptr,
                                              nullptr, false);
            ImageEditWindow* ew = FindImageEditWindowByHwnd(hwnd);
            if (!ew) return false;
            ew->dragCropX = 40;
            ew->dragCropY = 30;
            ew->dragCropW = 160;
            ew->dragCropH = 100;
            ew->isDragging = true;
            SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            bool valid = IsWindow(hwnd) && !ew->isDragging && ew->mode == ImageEditMode::Crop && ew->cropX == 40 &&
                         ew->cropY == 30 && ew->cropW == 160 && ew->cropH == 100;
            SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            valid = valid && IsWindow(hwnd) && ew->mode == ImageEditMode::Save;
            SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            valid = valid && !IsWindow(hwnd);
            fprintf(report, "Esc cancels drag first theme=%s pref=%d: %s\n", theme, escToExit, valid ? "PASS" : "FAIL");
            ok = valid && ok;
        }
    }
    gGlobalPrefs->escToExit = oldEscToExit;
    if (getenv("SUMATRA_TEST_IMAGE_EDIT_ESC_ONLY")) {
        fprintf(report, "Overall: %s\n", ok ? "PASS" : "FAIL");
        return ok;
    }
    const char* langs[] = {"cn", "tw", "en", "de", "ar"};
    int dpis[] = {96, 120, 144, 192, 96};
    for (const char* theme : themes) {
        SetTheme(theme);
        for (const char* lang : langs) {
            trans::SetCurrentLangByCode(lang);
            TempStr dest = path::JoinTemp(outputDir, str::FormatTemp("crop-%s-%s.png", theme, lang));
            str::ReplaceWithCopy(&gGlobalPrefs->imageSaveFolder, outputDir);
            gGlobalPrefs->imageOpenAfterSave = false;
            HWND hwnd = CreateImageEditWindow(owner, nullptr, ImageEditMode::Crop, dest, &rendered, false, nullptr,
                                              nullptr, false);
            ImageEditWindow* ew = FindImageEditWindowByHwnd(hwnd);
            if (!ew) return false;
            bool valid = ew->hintRc.Bottom() <= ClientRect(hwnd).dy;
            for (int dpi : dpis) {
                RECT suggested{0, 0, MulDiv(800, dpi, 96), MulDiv(900, dpi, 96)};
                SendMessageW(hwnd, WM_DPICHANGED, MAKELONG(dpi, dpi), (LPARAM)&suggested);
                valid = valid && ew->dpi == dpi && ew->hintRc.Bottom() <= ClientRect(hwnd).dy;
                HWND controls[] = {ew->hwndDestEdit,        ew->hwndBrowseBtn,        ew->hwndOpenAfterSave,
                                   ew->btnSave->hwnd,       ew->btnCrop->hwnd,        ew->btnResize->hwnd,
                                   ew->btnRotateLeft->hwnd, ew->btnRotateRight->hwnd, ew->dropFormat->hwnd};
                for (HWND ctl : controls) {
                    RECT r{};
                    GetWindowRect(ctl, &r);
                    MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&r, 2);
                    Rect client = ClientRect(hwnd);
                    valid =
                        valid && r.left >= 0 && r.right <= client.dx && r.top >= ew->imgAreaH && r.bottom <= client.dy;
                }
                for (int direction = 0; direction < 4; direction++) {
                    ew->cropX = ew->cropY = 0;
                    ew->cropW = ew->imgW;
                    ew->cropH = ew->imgH;
                    int x0 = ImageToDisplayX(ew, direction & 1 ? 200 : 40);
                    int y0 = ImageToDisplayY(ew, direction & 2 ? 130 : 30);
                    int x1 = ImageToDisplayX(ew, direction & 1 ? 40 : 200);
                    int y1 = ImageToDisplayY(ew, direction & 2 ? 30 : 130);
                    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
                    SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x1, y1));
                    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x1, y1));
                    valid = valid && abs(ew->cropX - 40) <= 2 && abs(ew->cropY - 30) <= 2 &&
                            abs(ew->cropW - 160) <= 2 && abs(ew->cropH - 100) <= 2;
                }
                ew->cropX = 40;
                ew->cropY = 30;
                ew->cropW = 160;
                ew->cropH = 100;
                int x = ImageToDisplayX(ew, 80), y = ImageToDisplayY(ew, 60);
                int moveX = ImageToDisplayX(ew, 100), moveY = ImageToDisplayY(ew, 70);
                SendMessageW(hwnd, WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(x, y));
                SendMessageW(hwnd, WM_MOUSEMOVE, MK_MBUTTON, MAKELPARAM(moveX, moveY));
                SendMessageW(hwnd, WM_LBUTTONUP, MK_MBUTTON, MAKELPARAM(moveX, moveY));
                valid = valid && ew->isDragging && abs(ew->cropX - 60) <= 2 && abs(ew->cropY - 40) <= 2 &&
                        ew->cropW == 160 && ew->cropH == 100;
                SendMessageW(hwnd, WM_MBUTTONUP, 0, MAKELPARAM(moveX, moveY));
                valid = valid && !ew->isDragging && GetCapture() != hwnd;
                ew->cropX = 40;
                ew->cropY = 30;
                SendMessageW(hwnd, WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(x, y));
                SendMessageW(hwnd, WM_MOUSEMOVE, MK_MBUTTON, MAKELPARAM(moveX, moveY));
                SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
                valid = valid && !ew->isDragging && ew->cropX == 40 && ew->cropY == 30;
                SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
                SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + ImgPx(ew, 30), y + ImgPx(ew, 30)));
                SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
                valid = valid && !ew->isDragging && ew->cropX == 40 && ew->cropY == 30 && ew->cropW == 160 &&
                        ew->cropH == 100;
                SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
                SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + ImgPx(ew, 30), y + ImgPx(ew, 30)));
                SendMessageW(hwnd, WM_CANCELMODE, 0, 0);
                valid = valid && !ew->isDragging && ew->cropX == 40 && ew->cropY == 30 && ew->cropW == 160 &&
                        ew->cropH == 100;
                RotateSource(ew, true);
                valid = valid && ew->imgW == 160 && ew->imgH == 240 && ew->cropX == 30 && ew->cropY == 40 &&
                        ew->cropW == 100 && ew->cropH == 160;
                RotateSource(ew, false);
                valid = valid && ew->imgW == 240 && ew->imgH == 160 && ew->cropX == 40 && ew->cropY == 30 &&
                        ew->cropW == 160 && ew->cropH == 100;
            }
            // Saving must keep original pixels regardless of theme and close
            // the editor without opening a document by default.
            WCHAR filename[MAX_PATH + 1]{};
            GetWindowTextW(ew->hwndDestEdit, filename, dimof(filename));
            OnSave(ew);
            Bitmap saved(filename);
            Color red, blue;
            saved.GetPixel(0, 0, &red);
            saved.GetPixel(159, 99, &blue);
            valid = valid && !IsWindow(hwnd) && saved.GetWidth() == 160 && saved.GetHeight() == 100 &&
                    red.GetR() == 255 && red.GetB() == 0 && blue.GetB() == 255 && blue.GetR() == 0 &&
                    !gGlobalPrefs->imageOpenAfterSave && str::EqI(gGlobalPrefs->imageSaveFolder, outputDir);
            printf("Image crop/export theme=%s lang=%s DPI round trip: %s\n", theme, lang, valid ? "PASS" : "FAIL");
            fprintf(report,
                    "Image crop/export theme=%s lang=%s DPI round trip, cancellation, rotation, original pixels: %s\n",
                    theme, lang, valid ? "PASS" : "FAIL");
            ok = ok && valid;
        }
    }
    int callbackCount = 0;
    auto done = [](Bitmap* bmp, void* data) {
        int* count = (int*)data;
        *count += bmp && bmp->GetWidth() == 80 && bmp->GetHeight() == 60 ? 1 : 100;
        delete bmp;
    };
    TempStr dest = path::JoinTemp(outputDir, "signature.png");
    HWND hwnd =
        CreateImageEditWindow(owner, nullptr, ImageEditMode::Crop, dest, &rendered, false, done, &callbackCount, false);
    ImageEditWindow* ew = FindImageEditWindowByHwnd(hwnd);
    if (!ew) return false;
    ok = ok && ew->hwndOpenAfterSave == nullptr;
    ew->cropW = 80;
    ew->cropH = 60;
    OnSave(ew);
    ok = ok && IsWindow(hwnd) && callbackCount == 0;
    OnCropButton(ew);
    ok = ok && !IsWindow(hwnd) && callbackCount == 1;
    printf("Signature save/apply callback: %s\n", callbackCount == 1 ? "PASS" : "FAIL");
    fprintf(report, "Signature save/apply callback: %s\n", callbackCount == 1 ? "PASS" : "FAIL");
    // A missing remembered directory must fall back to the supplied default.
    // Reusing an existing path must suggest a different filename.
    TempStr missingDir = path::JoinTemp(outputDir, "directory-that-does-not-exist");
    str::ReplaceWithCopy(&gGlobalPrefs->imageSaveFolder, missingDir);
    hwnd = CreateImageEditWindow(owner, nullptr, ImageEditMode::Crop, dest, &rendered, false, nullptr, nullptr, false);
    ew = FindImageEditWindowByHwnd(hwnd);
    if (!ew) return false;
    WCHAR suggested[MAX_PATH + 1]{};
    GetWindowTextW(ew->hwndDestEdit, suggested, dimof(suggested));
    TempStr suggestedPath = ToUtf8Temp(suggested);
    bool pathsOk = str::EqI(path::GetDirTemp(suggestedPath), outputDir) && !file::Exists(suggestedPath);
    TempStr invalidPath = path::JoinTemp(missingDir, "failure.png");
    SetWindowTextW(ew->hwndDestEdit, ToWStrTemp(invalidPath));
    ew->cropX = 40;
    ew->cropY = 30;
    ew->cropW = 160;
    ew->cropH = 100;
    SendMessageW(ew->hwndOpenAfterSave, BM_SETCHECK, BST_CHECKED, 0);
    SaveImageEdit(ew, false);
    pathsOk = pathsOk && IsWindow(hwnd) && ew->cropX == 40 && ew->cropY == 30 && ew->cropW == 160 && ew->cropH == 100 &&
              str::EqI(gGlobalPrefs->imageSaveFolder, missingDir) && !gGlobalPrefs->imageOpenAfterSave &&
              !file::Exists(invalidPath);
    TempStr chinesePath = path::JoinTemp(outputDir, "裁剪保存测试.png");
    SetWindowTextW(ew->hwndDestEdit, ToWStrTemp(chinesePath));
    SaveImageEdit(ew, false);
    pathsOk = pathsOk && !IsWindow(hwnd) && file::Exists(chinesePath) && gGlobalPrefs->imageOpenAfterSave &&
              str::EqI(gGlobalPrefs->imageSaveFolder, outputDir);
    ok = ok && pathsOk;
    fprintf(report,
            "Unique filename, invalid remembered folder, failed save preservation, Chinese path, open preference: %s\n",
            pathsOk ? "PASS" : "FAIL");
    fprintf(report, "Overall: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool ImageEditWindowPreTranslate(MSG& msg) {
    for (ImageEditWindow* ew : gImageEditWindows) {
        if (msg.hwnd != ew->hwnd && !IsChild(ew->hwnd, msg.hwnd)) continue;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(ew->hwnd, WM_KEYDOWN, VK_ESCAPE, msg.lParam);
            return true;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            (!ew->dropFormat || msg.hwnd != ew->dropFormat->hwnd)) {
            WCHAR cls[32]{};
            GetClassNameW(msg.hwnd, cls, dimof(cls));
            if (str::EqI(cls, WC_BUTTONW))
                SendMessageW(msg.hwnd, BM_CLICK, 0, 0);
            else
                OnSave(ew);
            return true;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) return IsDialogMessageW(ew->hwnd, &msg) != FALSE;
        // Keep document accelerators out of the path editor and checkbox.
        if (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            return true;
        }
        return false;
    }
    return false;
}

HWND ShowImageCropForBitmap(MainWindow* win, Gdiplus::Bitmap* src, ImageCropDoneFn fn, void* user) {
    if (!win || !src || !fn) {
        return nullptr;
    }
    int w = (int)src->GetWidth();
    int h = (int)src->GetHeight();
    if (w <= 0 || h <= 0) {
        return nullptr;
    }
    HBITMAP hbmp = nullptr;
    if (src->GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &hbmp) != Gdiplus::Ok || !hbmp) {
        if (hbmp) {
            DeleteObject(hbmp);
        }
        return nullptr;
    }
    auto* rb = new RenderedBitmap(hbmp, Size(w, h));
    HWND hwnd = ShowImageEditWindow(win, ImageEditMode::Crop, nullptr, rb, false, fn, user);
    delete rb;
    return hwnd;
}
