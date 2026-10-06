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
SizeF GetDefaultStampSize();
