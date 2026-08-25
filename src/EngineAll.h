/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct Annotation;
enum class AnnotationType;
struct PasswordUI;
struct FileArgs;
struct AnnotCreateArgs;

#include "EbookTypography.h"
#include "PdfTocEditModel.h"

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
int EngineGetProgressivePageCount(EngineBase* engine);
void EngineMupdfAckPdfDeferredUi(EngineBase* engine);
bool EngineMupdfIsFollowThemeProbePending(EngineBase* engine);
void EngineMupdfScheduleFollowThemeProbe(EngineBase* engine);
// Runs the deferred content probe synchronously (for -render diagnostics / tests).
void EngineMupdfEnsureFollowThemeProbeDone(EngineBase* engine);
// 0=unknown/pending, 1=bitmap-recolor doc, 2=layout/wrap doc
int EngineMupdfGetFollowThemeDocClass(EngineBase* engine);
// Cached page probe: 0=unset, 1=PureScan, 2=BitmapRecolor, 3=Mixed (see FollowThemeScanProbe)
int EngineMupdfGetFollowThemePageProbe(EngineBase* engine, int pageNo);
void NotifyPdfFollowThemeProbeComplete(const char* filePath);
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
Annotation* EngineMupdfCreateAnnotationInRect(EngineBase*, int pageNo, RectF rect, AnnotCreateArgs* args);
Annotation* EngineMupdfCreateAnnotationInkStroke(EngineBase*, int pageNo, PointF* pts, int count,
                                                 AnnotCreateArgs* args);
void EngineMupdfGetAnnotations(EngineBase*, Vec<Annotation*>&);
bool EngineMupdfHasUnsavedAnnotations(EngineBase*);
bool EngineMupdfHasUnsavedPdfChanges(EngineBase*);
bool EngineMupdfSupportsAnnotations(EngineBase*);
bool EngineMupdfIsEncrypted(EngineBase* engine);
bool EngineMupdfIsReflowableLoadingInProgress(EngineBase* engine);
bool EngineMupdfIsReflowWarmActive(EngineBase* engine);
bool EngineMupdfGetReflowPageChapter(EngineBase* engine, int pageNo, int* chapterOut, int* chapterStartPageOut);
bool EngineMupdfGetReflowChapterPageRange(EngineBase* engine, int chapter, int* startPageOut, int* endPageOut);
bool EngineIsProgressiveEbookLoading(EngineBase* engine);
int EngineMupdfFastOutlinePageNo(EngineBase* engine, IPageDestination* dest);
int EngineMupdfTocItemPageNoForSync(EngineBase* engine, IPageDestination* dest, int bakedPageNo);
int EngineMupdfResolveLinkPageNo(EngineBase* engine, IPageDestination* dest);
bool EngineMupdfIsOutlineDestReachable(EngineBase* engine, IPageDestination* dest);
bool EngineMupdfSnapshotOutlineLink(IPageDestination* dest, char** uriOut, int* reflowChOut, float* xOut, float* yOut);
bool EngineMupdfUpdatePageDest(EngineBase* engine, IPageDestination* dest, int pageNo, float x, float y);
void EngineMupdfNavigateUri(EngineBase* engine, const char* uri, int reflowOutlineChapter, float destX, float destY,
                            ILinkHandler* lh);
bool EngineMupdfTryCompletePendingReflowNav(EngineBase* engine, ILinkHandler* lh);
bool EngineMupdfHasPendingReflowNav(EngineBase* engine);
bool EngineMupdfHasOutline(EngineBase* engine);
bool EngineMupdfFirstPageLooksFixedLayout(EngineBase* engine);
bool EngineMupdfCanEditPdfToc(EngineBase* engine);
bool EngineMupdfPdfHasSignatures(EngineBase* engine);
char* EngineMupdfFormatPdfTocTarget(EngineBase* engine, int pageNo, float x, float y);
bool EngineMupdfEditPdfToc(EngineBase* engine, PdfTocEditAction action, const Vec<int>& path, const char* title,
                           const char* uri, Vec<int>* resultPathOut, char** errorOut);
bool EngineMupdfEditPdfTocMany(EngineBase* engine, PdfTocEditAction action, const Vec<PdfTocPath>& paths,
                               Vec<PdfTocPath>* resultPathsOut, char** errorOut);
bool EngineMupdfMovePdfTocItems(EngineBase* engine, const Vec<PdfTocPath>& srcPaths, const Vec<int>& destPath,
                                PdfTocDropPos pos, Vec<PdfTocPath>* resultPathsOut, char** errorOut);
const char* EngineMupdfGetPassword(EngineBase* engine);
// overwriteTempOut: if overwriting the open file and in-place save fails, a full
// rewrite is written next to dest (or %TEMP% as a .pdf) and that path is returned
// for the caller to replace-and-reload (same pattern as OCR save). Caller frees
// with str::Free.
bool EngineMupdfSaveUpdated(EngineBase* engine, const char* path, const ShowErrorCb& showErrorFunc,
                            char** overwriteTempOut = nullptr);
bool EngineMupdfSaveSearchablePdf(EngineBase* engine, const char* path, char** errOut);
Annotation* EngineMupdfGetAnnotationAtPos(EngineBase*, int pageNo, PointF pos, Annotation*);
ByteSlice EngineMupdfLoadAttachment(EngineBase*, int attachmentNo);
ByteSlice EngineMupdfLoadAnnotAttachment(EngineBase*, int objNum);
TempStr EngineMupdfGetPdfInfo(const char* path);
TempStr EngineMupdfGetPdfOutline(const char* path);
void EngineMupdfInvalidateDarkMode(EngineBase* engine);
void EngineMupdfInvalidateSearchTextCache(EngineBase* engine);
using EngineMupdfThemeRecountProgressFn = void (*)(int chaptersDone, int chapterTotal, void* user);
void EngineMupdfSetThemeRecountProgressCb(EngineMupdfThemeRecountProgressFn cb, void* user);
using EngineMupdfRelayoutProgressFn = void (*)(int percent, void* user);
void EngineMupdfSetRelayoutProgressCb(EngineMupdfRelayoutProgressFn cb, void* user);
enum class MupdfReflowChangeKind {
    Palette,
    FontFamily,
    FontSize,
    PageGeometry,
};
bool EngineMupdfApplyReflowChange(EngineBase* engine, MupdfReflowChangeKind changeKind);
bool EngineMupdfRelayoutForThemeChange(EngineBase* engine);
bool EngineMupdfRelayoutForFontChange(EngineBase* engine);
bool EngineMupdfRelayoutForFontSizeChange(EngineBase* engine);
// Pause/resume the background reflow chapter loader so a UI-critical operation
// (theme change, annotation edit) can access the document without being starved.
// Safe to call when not loading (no-op). Use ReflowLoadingPauseScope for RAII.
void EngineMupdfSetReflowLoadingPaused(EngineBase* engine, bool paused);
// Drop a pending UI docLock request so a background EPUB loader can continue
// after the user leaves a tab that was waiting to apply theme CSS.
void EngineMupdfClearReflowUiWantsDocLock(EngineBase* engine);
// Background EPUB tabs should not keep counting chapters. Call with
// foreground=false on attach-to-background and when leaving a tab; true when
// the tab becomes current so progressive load can start.
void EngineMupdfSetReflowLoadWhenForeground(EngineBase* engine, bool foreground);
// Signal background reflow/counting/warm threads to stop (e.g. on application close).
void EngineMupdfAbortBackgroundWork(EngineBase* engine);
void EngineMupdfAbortAndWaitBackgroundWork(EngineBase* engine);
// Mark recent UI reading activity so background chapter counting yields docLock.
void EngineMupdfTouchReadingActivity(EngineBase* engine);
struct ReflowLoadingPauseScope {
    EngineBase* engine = nullptr;
    explicit ReflowLoadingPauseScope(EngineBase* e) : engine(e) { EngineMupdfSetReflowLoadingPaused(engine, true); }
    ~ReflowLoadingPauseScope() { Release(); }
    // Call before ReloadDocument or any other operation that destroys the engine.
    void Release() {
        if (!engine) {
            return;
        }
        EngineMupdfSetReflowLoadingPaused(engine, false);
        engine = nullptr;
    }
};
bool EngineMupdfReflowTocNeedsUiReload(EngineBase* engine);
void EngineMupdfClearReflowTocNeedsUiReload(EngineBase* engine);
bool EngineSupportsSmartDarkMode(EngineBase* engine);
void EngineMupdfToggleCadEnhance(EngineBase* engine);
void EngineMupdfEnsurePageLinksForHitTest(EngineBase* engine, int pageNo);
void EngineMupdfEnsurePageImagesForHitTest(EngineBase* engine, int pageNo);

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
