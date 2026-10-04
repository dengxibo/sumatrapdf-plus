/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
struct RenderedBitmap;

namespace Gdiplus {
class Bitmap;
}

enum class ImageEditMode {
    Save,
    Crop,
    Resize
};

// When cropDone is set, Apply Crop hands back the cropped bitmap and closes the window.
// The callback owns the bitmap.
using ImageCropDoneFn = void (*)(Gdiplus::Bitmap* cropped, void* user);

HWND ShowImageEditWindow(MainWindow* win, ImageEditMode mode, const char* filePath = nullptr,
                         RenderedBitmap* rbmp = nullptr, bool selectPdf = false, ImageCropDoneFn cropDone = nullptr,
                         void* cropDoneUser = nullptr);

// Opens the existing crop window on a bitmap that is already upright.
HWND ShowImageCropForBitmap(MainWindow* win, Gdiplus::Bitmap* src, ImageCropDoneFn fn, void* user);

// Saves an already-rendered page as PNG without opening the image editor.
bool SaveBitmapAsPng(HBITMAP hbmp, const char* destPath);
