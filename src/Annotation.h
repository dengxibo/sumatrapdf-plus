/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// TODO: not quite happy how those functions are split among
// Annotation.cpp, EngineMupdf.cpp and EditAnnotations.cpp

// for fast conversions, must match the order of pdf_annot_type enum in annot.h
enum class AnnotationType {
    Text,
    Link,
    FreeText,
    Line,
    Square,
    Circle,
    Polygon,
    PolyLine,
    Highlight,
    Underline,
    Squiggly,
    StrikeOut,
    Redact,
    Stamp,
    Caret,
    Ink,
    Popup,
    FileAttachment,
    Sound,
    Movie,
    RichMedia,
    Widget,
    Screen,
    PrinterMark,
    TrapNet,
    Watermark,
    ThreeD,
    Projection,
    Last = Projection,
    Unknown = -1
};

enum class AnnotationChange {
    Add,
    Remove,
    Modify,
};

class EngineMupdf;
extern "C" struct pdf_annot;

extern const char* gAnnotationTextIcons;
extern const char* gStampIcons;

// an user annotation on page
// It abstracts over pdf_annot so that we don't have to
// inlude mupdf to include Annotation
struct Annotation {
    // common to both smx and pdf
    AnnotationType type = AnnotationType::Unknown;
    int pageNo = -1;

    // in page coordinates
    RectF bounds = {};

    EngineMupdf* engine = nullptr;
    pdf_annot* pdfannot = nullptr; // not owned

    Annotation() = default;
    ~Annotation();
};

struct AnnotCreateArgs {
    AnnotationType annotType = AnnotationType::Unknown;
    // the following are set depending on type of the annotation
    ParsedColor col;
    // bgCol for free text
    ParsedColor bgCol;
    // interior color (fill) for shapes like Square, Circle, Line
    ParsedColor interiorCol;
    // opacity for free text, 0-100, 0-fully transparent (invisible), 100-fully opaque
    // if 100 we don't actually set it (it's the default)
    int opacity = 100;
    bool copyToClipboard = false;
    // for free text, < 0 means not given
    int textSize = -1;
    // for free text, < 0 means not given
    int borderWidth = -1;
    bool setContentToSelection = false;
    TempStr content = nullptr;
};

int PageNo(Annotation*);
RectF GetBounds(Annotation*);
RectF GetRect(Annotation*);
void SetRect(Annotation*, RectF);
// The annotation by itself, at the page zoom. 32bpp top-down BGRA, premultiplied
// alpha. Caller DeleteObject. nullptr if it cannot be drawn quickly.
HBITMAP RenderAnnotationPreviewBitmap(Annotation* annot, float zoom, int rotation);
// Page pixels for pageRect with this annotation left out. 32bpp top-down.
// Caller DeleteObject. nullptr if it cannot be rendered.
HBITMAP RenderPagePatchHidingAnnotation(Annotation* annot, float zoom, int rotation, RectF pageRect);
HBITMAP RenderAnnotationIconPreviewBitmap(EngineMupdf* engine, int pageNo, AnnotationType type, COLORREF color,
                                          const char* iconName, int size, int lineEnding = -1, bool lineStart = false);
HBITMAP RenderAnnotationIconPreviewBitmap(Annotation* annot, const char* iconName, int size, int lineEnding = -1,
                                          bool lineStart = false);
void SetLine(Annotation*, PointF a, PointF b);
void SetQuadPointsAsRect(Annotation*, const Vec<RectF>&);
// Vec<Annotation*> FilterAnnotationsForPage(Vec<Annotation*>* annots, int pageNo);

// EditAnnotations.cpp
const char* Author(Annotation*);
bool AnnotationHasAuthor(Annotation*);
bool SetAuthor(Annotation*, const char*);
time_t ModificationDate(Annotation*);
int PopupId(Annotation*); // -1 if not exist
TempStr AnnotationReadableNameTemp(AnnotationType tp);
AnnotationType Type(Annotation*);

const char* DefaultAppearanceTextFont(Annotation*);
PdfColor DefaultAppearanceTextColor(Annotation*);
int DefaultAppearanceTextSize(Annotation*);
TempStr Contents(Annotation*);
PdfColor GetColor(Annotation*);      // kColorUnset if no color
PdfColor InteriorColor(Annotation*); // kColorUnset if no color
int Quadding(Annotation*);
int BorderWidth(Annotation*);
float BorderWidthF(Annotation*);
// Older builds stored a free-text border of 0 as a 0.15 hairline. Rewrite that to 0.
bool ClearFreeTextHairlineBorder(Annotation*);
bool GetLinePoints(Annotation*, PointF& a, PointF& b);
void GetInkStrokes(Annotation*, Vec<PointF>& points, Vec<int>& counts);
const char* IconName(Annotation*); // empty() if no icon
int Opacity(Annotation*);
void GetLineEndingStyles(Annotation*, int* start, int* end);

void SetDefaultAppearanceTextFont(Annotation*, const char*);
void SetDefaultAppearanceTextSize(Annotation*, int);
void SetDefaultAppearanceTextColor(Annotation*, PdfColor);
bool SetContents(Annotation*, const char*);
bool SetColor(Annotation*, PdfColor);
bool SetInteriorColor(Annotation*, PdfColor);
bool SetQuadding(Annotation*, int);
void SetBorderWidth(Annotation*, int);
void SetBorderWidthFloat(Annotation*, float);
// Independent of text color. Missing key keeps the legacy text-colored stroke.
bool FreeTextBorderColorIsExplicit(Annotation*);
PdfColor FreeTextBorderColor(Annotation*);
void SetFreeTextBorderColor(Annotation*, PdfColor);
// User-chosen font only. Opening a file or syncing a control must not call this.
void NoteFreeTextFontUsed(const char* family);
int GetFreeTextRecentFonts(const char** out, int cap);
void SetOpacity(Annotation*, int);
void SetIconName(Annotation*, const char*);
void SetLineEndStyles(Annotation*, int end);
void SetLineStartStyles(Annotation*, int start);

void DeleteAnnotation(Annotation*);
bool AnnotationCanBeMoved(AnnotationType);
bool AnnotationCanBeResized(AnnotationType);
bool AnnotationContentsEqual(Annotation*, const char* text);
bool AnnotationSupportsColor(AnnotationType);
bool IsPdfTextMarkupAnnotation(AnnotationType);
bool IsPdfTextMarkupAnnotation(Annotation* annot);
TempStr MarkupTextTemp(Annotation* annot);
Vec<RectF> GetQuadPointsAsRect(Annotation*);
bool AnnotationSupportsBorder(AnnotationType);
bool AnnotationSupportsInteriorColor(AnnotationType);

AnnotationType CmdIdToAnnotationType(int cmdId);

// Play embedded audio from a PDF Sound/RichMedia/Screen annotation. Returns false if no playable sound.
bool PlaySoundAnnotation(Annotation* annot);

// Copies the annotation's audio. Caller frees *dataOut with free().
bool AnnotationCopyEmbeddedAudio(Annotation* annot, u8** dataOut, size_t* sizeOut);
// Stable id for the clip, or 0 when the annotation has no PDF object.
u64 AnnotationEmbeddedAudioToken(Annotation* annot);

bool AnnotationSupportsMediaPlayback(AnnotationType tp);

const char* DefaultStampIconName();
void RememberStampIconName(const char* name);

// Color of a new annotation after the settings file is deleted.
COLORREF FactoryAnnotationColor(AnnotationType type);
// PDF stroke width, in points, that lands on a 2px screen stroke at this zoom.
float NewStrokeWidthPoints(float zoom);

// Last drawn annotation of each type. The next draw of that type uses it.
constexpr u32 kDrawStyleColor = 1u << 0;
constexpr u32 kDrawStyleBorder = 1u << 1;
constexpr u32 kDrawStyleOpacity = 1u << 2;
constexpr u32 kDrawStyleInterior = 1u << 3;
constexpr u32 kDrawStyleLineEnds = 1u << 4;
constexpr u32 kDrawStyleIcon = 1u << 5;
constexpr u32 kDrawStyleFont = 1u << 6;
constexpr u32 kDrawStyleTextSize = 1u << 7;
constexpr u32 kDrawStyleAlign = 1u << 8;
constexpr u32 kDrawStyleBorderColor = 1u << 9;
constexpr u32 kDrawStyleBackground = 1u << 10;

struct AnnotDrawStyle {
    AnnotationType type = AnnotationType::Unknown;
    u32 flags = 0;
    COLORREF color = 0;
    float border = 1.f;
    int opacityPercent = 100;
    bool interiorTransparent = true;
    COLORREF interior = 0;
    int lineStart = 0;
    int lineEnd = 0;
    char icon[64]{};
    char font[96]{};
    int textSize = 21;
    int align = 0;
    COLORREF borderColor = 0;
    bool backgroundTransparent = true;
    COLORREF background = 0;
};

bool FindAnnotDrawStyle(AnnotationType type, AnnotDrawStyle* out);
void SaveAnnotDrawStyle(const AnnotDrawStyle& style);
void RememberPdfDrawStyle(Annotation* annot);
void ApplyRememberedPdfDrawStyle(Annotation* annot);
// font is Helv/Cour/TiRo; size 0 leaves size; width and height below 8 leave the box size
const char* FreeTextPresetFont();
const char* FreeTextFontLabel(const char* font);
const WCHAR* FreeTextWindowsFace(const char* font);
void RememberFreeTextPreset(const char* font, int size, float width, float height);
void ResetFreeTextPreset();
SizeF GetDefaultStampSize();
