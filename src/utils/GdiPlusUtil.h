/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct RenderedBitmap;

Gdiplus::RectF RectToRectF(Gdiplus::Rect r);

typedef RectF (*TextMeasureAlgorithm)(Gdiplus::Graphics* g, Gdiplus::Font* f, const WCHAR* s, int len);

RectF MeasureTextAccurate(Gdiplus::Graphics* g, Gdiplus::Font* f, const WCHAR* s, int len);
RectF MeasureTextStandard(Gdiplus::Graphics* g, Gdiplus::Font* f, const WCHAR* s, int len);
RectF MeasureTextQuick(Gdiplus::Graphics* g, Gdiplus::Font* f, const WCHAR* s, int len);
RectF MeasureText(Gdiplus::Graphics* g, Gdiplus::Font* f, const WCHAR* s, size_t len = -1,
                  TextMeasureAlgorithm algo = nullptr);
// float     GetSpaceDx(Graphics *g, Font *f, TextMeasureAlgorithm algo=nullptr);
// size_t   StringLenForWidth(Graphics *g, Font *f, const WCHAR *s, size_t len, float dx, TextMeasureAlgorithm
// algo=nullptr);

void GetBaseTransform(Gdiplus::Matrix& m, Gdiplus::RectF pageRect, float zoom, int rotation);

Gdiplus::Bitmap* BitmapFromDataWin(const ByteSlice& bmpData);
Size ImageSizeFromData(const ByteSlice&);
Size ImageSizeFromHeader(const ByteSlice&);
// Register reader fonts from <exe>\fonts\ for GDI+ (MOBI/AZW/CHM path).
void ConfigureBundledReaderLatinFont(const char* familyName);
void ConfigureBundledReaderCjkFont(const char* familyName, const char* fileName);
void ResetBundledReaderFonts();
void InstallBundledReaderFonts();
void ClearMeasureTextQuickFontCache();
Gdiplus::Font* TryCreateBundledFont(const WCHAR* familyName, float sizePt, Gdiplus::FontStyle style);
// Load Literata/Source Serif static TTF from exe\fonts or Windows\Fonts (same files MuPDF uses).
Gdiplus::Font* TryCreateReaderLatinFromSystemFiles(float sizePt, Gdiplus::FontStyle style);
void CollectBundledFontFamilyNames(Vec<char*>* families);
CLSID GetGdiPlusEncoderClsid(const WCHAR* format);
RenderedBitmap* LoadRenderedBitmapWin(const char* path);
