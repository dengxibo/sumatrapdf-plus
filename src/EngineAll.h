/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct Annotation;
enum class AnnotationType;
struct PasswordUI;
struct FileArgs;
struct AnnotCreateArgs;

#include "EbookTypography.h"

/* EngineDjVu.cpp */
void CleanupEngineDjVu();
bool IsEngineDjVuSupportedFileType(Kind kind);
EngineBase* CreateEngineDjVuFromFile(const char* path);
EngineBase* CreateEngineDjVuFromStream(IStream* stream);

/* EngineEbook.cpp */
EngineBase* CreateEngineEpubFromFile(const char* fileName);
EngineBase* CreateEngineEpubFromStream(IStream* stream);
EngineBase* CreateEngineFb2FromFile(const char* fileName);
EngineBase* CreateEngineFb2FromStream(IStream* stream);
EngineBase* CreateEngineMobiFromFile(const char* fileName);
EngineBase* CreateEngineMobiFromStream(IStream* stream);
EngineBase* CreateEnginePdbFromFile(const char* fileName);
EngineBase* CreateEngineChmFromFile(const char* fileName);
EngineBase* CreateEngineHtmlFromFile(const char* fileName);
EngineBase* CreateEngineTxtFromFile(const char* fileName);

void SetDefaultEbookFont(const char* name, float size);
bool EngineIsFixedLayoutEbook(EngineBase* engine);
bool EngineEbookHitTestText(EngineBase* engine, int pageNo, PointF pagePt, EbookTextHit* hitOut);
TempWStr EngineEbookGetRunTextTemp(EngineBase* engine, int pageNo, int instrIndex);
bool EngineEbookGetCharRangeBbox(EngineBase* engine, int pageNo, int instrIndex, int charStart, int charEnd,
                                 RectF* out);
bool EngineEbookGetSourceOffset(EngineBase* engine, int pageNo, int glyphIndex, bool endBoundary, int* offsetOut);
bool EngineEbookGetSourceRangeRects(EngineBase* engine, int pageNo, int sourceStart, int sourceEnd,
                                    Vec<RectF>& rectsOut);
int EngineEbookGetSourcePageNo(EngineBase* engine, int sourceOffset);
void EngineEbookCleanup();
bool EngineEbookIsProgressiveLoadingInProgress(EngineBase* engine);
int EngineEbookGetFormattedPageCount(EngineBase* engine);
int EngineEbookParseTocLinkFilePos(EngineBase* engine, IPageDestination* dest);
bool EngineEbookIsTocFilePosReachable(EngineBase* engine, int filePos);
int EngineEbookPageNoForTocFilePos(EngineBase* engine, int filePos);

struct DocController;
bool IsInternalPageLinkReachable(DocController* ctrl, IPageDestination* dest);
bool IsPageElementLinkReachable(DocController* ctrl, IPageElement* el);

void SetCreateEngineForThumbnail(bool value);
bool IsCreateEngineForThumbnail();

constexpr int kEbookInitialPages = 1;

/* EngineImages.cpp */

bool IsEngineImageSupportedFileType(Kind);
EngineBase* CreateEngineImageFromFile(const char* fileName);
EngineBase* CreateEngineImageFromStream(IStream* stream);

bool IsEngineImageDirSupportedFile(const char* fileName, bool sniff = false);
EngineBase* CreateEngineImageDirFromFile(const char* fileName);

bool IsEngineCbxSupportedFileType(Kind kind);
EngineBase* CreateEngineCbxFromFile(const char* path, PasswordUI* pwdUI = nullptr, Kind hintKind = nullptr,
                                    const char* realPath = nullptr);
EngineBase* CreateEngineCbxFromStream(IStream* stream);

bool IsEngineImages(EngineBase*);
void EngineImagesGetImageProperties(EngineBase*, int pageNo, StrVec& keyValOut);

/* EngineMupdf.cpp */

using ShowErrorCb = Func1<const char*>;

bool IsEngineMupdfSupportedFileType(Kind);
EngineBase* CreateEngineMupdfFromFile(const char* path, Kind kind, int displayDPI, PasswordUI* pwdUI = nullptr);
EngineBase* CreateEngineMupdfFromStream(IStream* stream, const char* nameHint, PasswordUI* pwdUI = nullptr);
EngineBase* CreateEngineMupdfFromData(const ByteSlice& data, const char* nameHint, PasswordUI* pwdUI);
ByteSlice LoadEmbeddedPDFFile(const char* path);
const char* ParseEmbeddedStreamNumber(const char* path, int* streamNoOut);
Annotation* EngineMupdfCreateAnnotation(EngineBase*, int pageNo, PointF pos, AnnotCreateArgs* args);
void EngineMupdfGetAnnotations(EngineBase*, Vec<Annotation*>&);
bool EngineMupdfHasUnsavedAnnotations(EngineBase*);
bool EngineMupdfSupportsAnnotations(EngineBase*);
bool EngineMupdfIsEncrypted(EngineBase* engine);
bool EngineMupdfIsReflowableLoadingInProgress(EngineBase* engine);
bool EngineMupdfGetReflowPageChapter(EngineBase* engine, int pageNo, int* chapterOut, int* chapterStartPageOut);
bool EngineMupdfGetReflowChapterPageRange(EngineBase* engine, int chapter, int* startPageOut, int* endPageOut);
bool EngineIsProgressiveEbookLoading(EngineBase* engine);
int EngineMupdfFastOutlinePageNo(EngineBase* engine, IPageDestination* dest);
int EngineMupdfResolveLinkPageNo(EngineBase* engine, IPageDestination* dest);
bool EngineMupdfIsOutlineDestReachable(EngineBase* engine, IPageDestination* dest);
bool EngineMupdfSnapshotOutlineLink(IPageDestination* dest, char** uriOut, int* reflowChOut, float* xOut, float* yOut);
void EngineMupdfNavigateUri(EngineBase* engine, const char* uri, int reflowOutlineChapter, float destX, float destY,
                            ILinkHandler* lh);
bool EngineMupdfHasOutline(EngineBase* engine);
const char* EngineMupdfGetPassword(EngineBase* engine);
bool EngineMupdfSaveUpdated(EngineBase* engine, const char* path, const ShowErrorCb& showErrorFunc);
Annotation* EngineMupdfGetAnnotationAtPos(EngineBase*, int pageNo, PointF pos, Annotation*);
ByteSlice EngineMupdfLoadAttachment(EngineBase*, int attachmentNo);
ByteSlice EngineMupdfLoadAnnotAttachment(EngineBase*, int objNum);
TempStr EngineMupdfGetPdfInfo(const char* path);
TempStr EngineMupdfGetPdfOutline(const char* path);
void EngineMupdfInvalidateDarkMode(EngineBase* engine);
bool EngineMupdfRelayoutForThemeChange(EngineBase* engine);
bool EngineMupdfReflowTocNeedsUiReload(EngineBase* engine);
void EngineMupdfClearReflowTocNeedsUiReload(EngineBase* engine);
bool EngineSupportsSmartDarkMode(EngineBase* engine);
void EngineMupdfToggleCadEnhance(EngineBase* engine);

/* EnginePs.cpp */

bool IsEnginePsAvailable();
bool IsEnginePsSupportedFileType(Kind);
EngineBase* CreateEnginePsFromFile(const char* fileName);

/* EngineCreate.cpp */

bool IsSupportedFileType(Kind kind, bool enableEngineEbooks);

EngineBase* CreateEngineFromFile(const char* filePath, PasswordUI* pwdUI, bool enableChmEngine);

bool EngineSupportsAnnotations(EngineBase*);
bool EngineGetAnnotations(EngineBase*, Vec<Annotation*>&);
bool EngineHasUnsavedAnnotations(EngineBase*);
Annotation* EngineGetAnnotationAtPos(EngineBase*, int pageNo, PointF pos, Annotation*);
