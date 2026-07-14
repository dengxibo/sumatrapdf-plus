/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/CryptoUtil.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/JsonParser.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "AppTools.h"
#include "Settings.h"
#include "GlobalPrefs.h"
#include "Notifications.h"
#include "Annotation.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "EbookAnnotations.h"
#include "PdfDarkMode.h"
#include "Theme.h"
#include "Translations.h"
#include "MainWindow.h"
#include "Selection.h"
#include "SumatraPDF.h"
#include "TextSelection.h"
#include "WindowTab.h"

#include "EditAnnotations.h"

#include "utils/Log.h"

struct EbookAnnotation {
    AnnotationType type = AnnotationType::Highlight;
    int chapter = -1;
    int sourceStart = -1;
    int sourceEnd = -1;
    COLORREF color = 0;
    char* exact = nullptr;
    char* prefix = nullptr;
    char* suffix = nullptr;
    char* note = nullptr;
    char* author = nullptr;
    time_t created = 0;
    time_t modified = 0;

    ~EbookAnnotation() {
        str::Free(exact);
        str::Free(prefix);
        str::Free(suffix);
        str::Free(note);
        str::Free(author);
    }
};

struct EbookChapterTextCache {
    int chapter = -1;
    int startPage = 0;
    // Chapter-relative anchor offset at the start of each formatted page.
    Vec<int> pageStarts;
};

struct EbookAnnotations {
    char* sourcePath = nullptr;
    char* storagePath = nullptr;
    i64 sourceSize = -1;
    Vec<EbookAnnotation*> items;
    Vec<EbookChapterTextCache*> chapterCaches;

    ~EbookAnnotations() {
        DeleteVecMembers(items);
        DeleteVecMembers(chapterCaches);
        str::Free(sourcePath);
        str::Free(storagePath);
    }
};

void EbookAnnotationsFree(EbookAnnotations* annotations) {
    delete annotations;
}

void EbookAnnotationsInvalidateLayoutCaches(WindowTab* tab) {
    if (!tab) {
        return;
    }
    DisplayModel* dm = tab->AsFixed();
    if (dm && dm->GetEngine()) {
        dm->GetEngine()->ClearTextCache();
    }
    if (tab->ebookAnnotations) {
        DeleteVecMembers(tab->ebookAnnotations->chapterCaches);
    }
}

static TempStr GetEbookAnnotationsDirTemp() {
    return GetPathInAppDataDirTemp("Annotations");
}

static TempStr GetEbookAnnotationsPathTemp(const char* filePath) {
    if (!filePath) {
        return nullptr;
    }
    TempStr normalized = path::NormalizeTemp(filePath);
    char* key = str::DupTemp(normalized ? normalized : filePath);
    str::ToLowerInPlace(key);
    if (path::HasVariableDriveLetter(key)) {
        key[0] = '?';
    }

    u8 digest[16]{};
    CalcMD5Digest(key, str::Leni(key), digest);
    AutoFreeStr hex(str::MemToHex(digest, dimof(digest)));
    return path::JoinTemp(GetEbookAnnotationsDirTemp(), str::JoinTemp(hex, ".json"));
}

static EbookAnnotation* EnsureAnnotationAt(EbookAnnotations* annotations, int idx) {
    if (idx < 0 || idx > 100000) {
        return nullptr;
    }
    while ((int)annotations->items.size() <= idx) {
        annotations->items.Append(new EbookAnnotation());
    }
    return annotations->items.at(idx);
}

static const char* GetEbookAnnotationAuthorTemp() {
    char* defAuthor = gGlobalPrefs->annotations.defaultAuthor;
    if (str::Eq(defAuthor, "(none)")) {
        return nullptr;
    }
    if (!str::IsEmptyOrWhiteSpace(defAuthor)) {
        return defAuthor;
    }
    const char* u = getenv("USER");
    if (!u) {
        u = getenv("USERNAME");
    }
    return u;
}

static void InitEbookAnnotationMetadata(EbookAnnotation* annotation) {
    if (!annotation) {
        return;
    }
    time_t now = time(nullptr);
    annotation->created = now;
    annotation->modified = now;
    const char* author = GetEbookAnnotationAuthorTemp();
    if (!str::IsEmpty(author)) {
        annotation->author = str::Dup(author);
    }
}

static void TouchEbookAnnotationModified(EbookAnnotation* annotation) {
    if (annotation) {
        annotation->modified = time(nullptr);
    }
}

static void AppendUtcDateTime(StrBuilder& s, time_t secs) {
    if (secs <= 0) {
        return;
    }
    struct tm tm;
    gmtime_s(&tm, &secs);
    char buf[100];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
    s.Append(buf);
}

static bool ParseAnnotationPath(const char* path, int* idxOut, const char** propertyOut) {
    const char* prefix = "/annotations[";
    if (!str::StartsWith(path, prefix)) {
        return false;
    }
    const char* p = path + str::Len(prefix);
    int idx = 0;
    bool hasDigit = false;
    while (str::IsDigit(*p)) {
        hasDigit = true;
        idx = idx * 10 + (*p - '0');
        p++;
    }
    if (!hasDigit || *p != ']' || p[1] != '/') {
        return false;
    }
    *idxOut = idx;
    *propertyOut = p + 2;
    return true;
}

struct EbookAnnotationsJsonVisitor : json::ValueVisitor {
    EbookAnnotations* annotations = nullptr;
    i64 savedSourceSize = -1;

    explicit EbookAnnotationsJsonVisitor(EbookAnnotations* annotations) : annotations(annotations) {}

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (str::Eq(path, "/sourceSize") && type == json::Type::Number) {
            str::Parse(value, "%lld", &savedSourceSize);
            return true;
        }

        int idx = -1;
        const char* property = nullptr;
        if (!ParseAnnotationPath(path, &idx, &property)) {
            return true;
        }
        EbookAnnotation* annotation = EnsureAnnotationAt(annotations, idx);
        if (!annotation) {
            return false;
        }
        if (type == json::Type::Number) {
            if (str::Eq(property, "chapter")) {
                str::Parse(value, "%d", &annotation->chapter);
            } else if (str::Eq(property, "start")) {
                str::Parse(value, "%d", &annotation->sourceStart);
            } else if (str::Eq(property, "end")) {
                str::Parse(value, "%d", &annotation->sourceEnd);
            } else if (str::Eq(property, "color")) {
                uint color = 0;
                str::Parse(value, "%u", &color);
                annotation->color = (COLORREF)color;
            } else if (str::Eq(property, "created")) {
                i64 secs = 0;
                str::Parse(value, "%lld", &secs);
                annotation->created = (time_t)secs;
            } else if (str::Eq(property, "modified")) {
                i64 secs = 0;
                str::Parse(value, "%lld", &secs);
                annotation->modified = (time_t)secs;
            }
        } else if (type == json::Type::String) {
            if (str::Eq(property, "type")) {
                if (str::EqI(value, "underline")) {
                    annotation->type = AnnotationType::Underline;
                } else if (str::EqI(value, "squiggly")) {
                    annotation->type = AnnotationType::Squiggly;
                } else if (str::EqI(value, "strikeout")) {
                    annotation->type = AnnotationType::StrikeOut;
                } else if (str::EqI(value, "text")) {
                    annotation->type = AnnotationType::Text;
                } else {
                    annotation->type = AnnotationType::Highlight;
                }
            } else if (str::Eq(property, "exact")) {
                str::ReplaceWithCopy(&annotation->exact, value);
            } else if (str::Eq(property, "prefix")) {
                str::ReplaceWithCopy(&annotation->prefix, value);
            } else if (str::Eq(property, "suffix")) {
                str::ReplaceWithCopy(&annotation->suffix, value);
            } else if (str::Eq(property, "note")) {
                str::ReplaceWithCopy(&annotation->note, value);
            } else if (str::Eq(property, "author")) {
                str::ReplaceWithCopy(&annotation->author, value);
            }
        }
        return true;
    }
};

static void RemoveInvalidAnnotations(EbookAnnotations* annotations) {
    for (int i = (int)annotations->items.size() - 1; i >= 0; i--) {
        EbookAnnotation* annotation = annotations->items.at(i);
        if (annotation->sourceStart >= 0 && annotation->sourceEnd > annotation->sourceStart) {
            continue;
        }
        annotations->items.RemoveAt(i);
        delete annotation;
    }
}

static EbookAnnotations* LoadEbookAnnotations(const char* filePath) {
    auto annotations = new EbookAnnotations();
    annotations->sourcePath = str::Dup(filePath);
    annotations->sourceSize = file::GetSize(filePath);
    annotations->storagePath = str::Dup(GetEbookAnnotationsPathTemp(filePath));

    if (!file::Exists(annotations->storagePath)) {
        return annotations;
    }
    ByteSlice data = file::ReadFile(annotations->storagePath);
    if (data.empty()) {
        return annotations;
    }
    defer {
        data.Free();
    };
    EbookAnnotationsJsonVisitor visitor(annotations);
    if (!json::Parse((const char*)data.data(), &visitor)) {
        logf("LoadEbookAnnotations: invalid JSON in '%s'\n", annotations->storagePath);
        DeleteVecMembers(annotations->items);
    } else if (visitor.savedSourceSize >= 0 && visitor.savedSourceSize != annotations->sourceSize) {
        logf("LoadEbookAnnotations: source size changed for '%s', ignoring annotations\n", filePath);
        DeleteVecMembers(annotations->items);
    }
    RemoveInvalidAnnotations(annotations);
    return annotations;
}

static void AppendJsonString(StrBuilder& out, const char* value) {
    out.AppendChar('"');
    if (value) {
        const u8* p = (const u8*)value;
        while (*p) {
            u8 c = *p++;
            switch (c) {
                case '"':
                    out.Append("\\\"");
                    break;
                case '\\':
                    out.Append("\\\\");
                    break;
                case '\b':
                    out.Append("\\b");
                    break;
                case '\f':
                    out.Append("\\f");
                    break;
                case '\n':
                    out.Append("\\n");
                    break;
                case '\r':
                    out.Append("\\r");
                    break;
                case '\t':
                    out.Append("\\t");
                    break;
                default:
                    if (c < 0x20) {
                        out.AppendFmt("\\u%04x", (uint)c);
                    } else {
                        out.AppendChar((char)c);
                    }
                    break;
            }
        }
    }
    out.AppendChar('"');
}

static bool SaveEbookAnnotations(EbookAnnotations* annotations) {
    if (!annotations || !annotations->storagePath || !CanAccessDisk()) {
        return false;
    }
    if (!dir::CreateAll(GetEbookAnnotationsDirTemp())) {
        return false;
    }

    StrBuilder out;
    out.Append("{\n  \"version\": 1,\n  \"sourcePath\": ");
    AppendJsonString(out, annotations->sourcePath);
    out.AppendFmt(",\n  \"sourceSize\": %lld,\n  \"annotations\": [", annotations->sourceSize);
    for (size_t i = 0; i < annotations->items.size(); i++) {
        EbookAnnotation* annotation = annotations->items.at(i);
        out.Append(i == 0 ? "\n    {" : ",\n    {");
        const char* type = "highlight";
        if (annotation->type == AnnotationType::Underline) {
            type = "underline";
        } else if (annotation->type == AnnotationType::Squiggly) {
            type = "squiggly";
        } else if (annotation->type == AnnotationType::StrikeOut) {
            type = "strikeout";
        } else if (annotation->type == AnnotationType::Text) {
            type = "text";
        }
        out.Append("\n      \"type\": ");
        AppendJsonString(out, type);
        out.AppendFmt(
            ",\n      \"chapter\": %d,\n      \"start\": %d,\n      \"end\": %d,\n      \"color\": %u,\n      "
            "\"exact\": ",
            annotation->chapter, annotation->sourceStart, annotation->sourceEnd, (uint)annotation->color);
        AppendJsonString(out, annotation->exact);
        out.Append(",\n      \"prefix\": ");
        AppendJsonString(out, annotation->prefix);
        out.Append(",\n      \"suffix\": ");
        AppendJsonString(out, annotation->suffix);
        out.Append(",\n      \"note\": ");
        AppendJsonString(out, annotation->note);
        if (!str::IsEmpty(annotation->author)) {
            out.Append(",\n      \"author\": ");
            AppendJsonString(out, annotation->author);
        }
        if (annotation->created > 0) {
            out.AppendFmt(",\n      \"created\": %lld", (i64)annotation->created);
        }
        if (annotation->modified > 0) {
            out.AppendFmt(",\n      \"modified\": %lld", (i64)annotation->modified);
        }
        out.Append("\n    }");
    }
    if (annotations->items.size() > 0) {
        out.AppendChar('\n');
    }
    out.Append("  ]\n}\n");
    TempStr tempPath = str::JoinTemp(annotations->storagePath, ".tmp");
    if (!file::WriteFile(tempPath, ByteSlice((const u8*)out.Get(), out.size()))) {
        return false;
    }
    TempWStr tempPathW = ToWStrTemp(tempPath);
    TempWStr storagePathW = ToWStrTemp(annotations->storagePath);
    BOOL ok = MoveFileExW(tempPathW, storagePathW, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        LogLastError();
        file::Delete(tempPath);
        return false;
    }
    return true;
}

static EbookAnnotations* EnsureEbookAnnotations(WindowTab* tab) {
    if (!tab || !EbookAnnotationsSupported(tab)) {
        return nullptr;
    }
    if (!tab->ebookAnnotations) {
        tab->ebookAnnotations = LoadEbookAnnotations(tab->filePath);
    }
    return tab->ebookAnnotations;
}

bool EbookAnnotationsSupported(WindowTab* tab) {
    if (!tab || !tab->filePath) {
        return false;
    }
    EngineBase* engine = tab->GetEngine();
    bool isFixedEbook = engine && (engine->kind == kindEngineEpub || engine->kind == kindEngineMobi);
    bool isMupdfEpub = engine && engine->kind == kindEngineMupdf && str::EqI(engine->defaultExt, ".epub");
    return (isFixedEbook || isMupdfEpub) && CanAccessDisk();
}

static bool IsEbookAnchorChar(WCHAR c) {
    return c != L'\xad' && !str::IsWs(c);
}

static int CountEbookAnchorChars(const WCHAR* text, int len) {
    int count = 0;
    for (int i = 0; text && i < len; i++) {
        count += IsEbookAnchorChar(text[i]);
    }
    return count;
}

static EbookChapterTextCache* GetChapterTextCache(EbookAnnotations* annotations, int chapter, int startPage) {
    for (EbookChapterTextCache* cache : annotations->chapterCaches) {
        if (cache->chapter == chapter && cache->startPage == startPage) {
            return cache;
        }
    }
    auto cache = new EbookChapterTextCache();
    cache->chapter = chapter;
    cache->startPage = startPage;
    cache->pageStarts.Append(0);
    annotations->chapterCaches.Append(cache);
    return cache;
}

static bool GetMupdfPageAnchorStart(EbookAnnotations* annotations, EngineBase* engine, int chapter, int startPage,
                                    int pageNo, int* offsetOut) {
    if (pageNo < startPage || !offsetOut) {
        return false;
    }
    EbookChapterTextCache* cache = GetChapterTextCache(annotations, chapter, startPage);
    int pageIndex = pageNo - startPage;
    while ((int)cache->pageStarts.size() <= pageIndex) {
        int sourcePage = startPage + (int)cache->pageStarts.size() - 1;
        int textLen = 0;
        const WCHAR* text = engine->GetTextForPage(sourcePage, &textLen);
        if (!text || textLen < 0) {
            return false;
        }
        cache->pageStarts.Append(cache->pageStarts.Last() + CountEbookAnchorChars(text, textLen));
    }
    *offsetOut = cache->pageStarts[pageIndex];
    return true;
}

static bool GetMupdfSelectionAnchor(EbookAnnotations* annotations, EngineBase* engine, int pageNo, int glyph,
                                    int* chapterOut, int* offsetOut) {
    int chapter = -1;
    int chapterStartPage = 0;
    if (!EngineMupdfGetReflowPageChapter(engine, pageNo, &chapter, &chapterStartPage)) {
        return false;
    }
    int pageStart = 0;
    if (!GetMupdfPageAnchorStart(annotations, engine, chapter, chapterStartPage, pageNo, &pageStart)) {
        return false;
    }
    int textLen = 0;
    const WCHAR* text = engine->GetTextForPage(pageNo, &textLen);
    if (!text) {
        return false;
    }
    if (glyph < 0) {
        glyph = 0;
    }
    if (glyph > textLen) {
        glyph = textLen;
    }
    *chapterOut = chapter;
    *offsetOut = pageStart + CountEbookAnchorChars(text, glyph);
    return true;
}

static char* ExtractContextTemp(EngineBase* engine, int pageNo, int fromGlyph, int toGlyph) {
    int textLen = 0;
    const WCHAR* text = engine->GetTextForPage(pageNo, &textLen);
    if (!text || textLen <= 0) {
        return nullptr;
    }
    if (fromGlyph < 0) {
        fromGlyph = 0;
    }
    if (toGlyph > textLen) {
        toGlyph = textLen;
    }
    if (toGlyph <= fromGlyph) {
        return nullptr;
    }
    return ToUtf8Temp(text + fromGlyph, toGlyph - fromGlyph);
}

EbookAnnotation* EbookAnnotationsCreateFromSelection(WindowTab* tab, AnnotationType type, COLORREF color) {
    if (type != AnnotationType::Highlight && type != AnnotationType::Underline && type != AnnotationType::Squiggly &&
        type != AnnotationType::StrikeOut) {
        return nullptr;
    }
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    if (!annotations || !dm || !dm->textSelection || !tab->win->showSelection) {
        return nullptr;
    }
    // Pause background reflow loading so this stays responsive during progressive
    // EPUB load (no-op once loading has finished).
    ReflowLoadingPauseScope reflowPause(dm->GetEngine());

    TextSelection* selection = dm->textSelection;
    int fromPage = 0, fromGlyph = 0, toPage = 0, toGlyph = 0;
    selection->GetGlyphRange(&fromPage, &fromGlyph, &toPage, &toGlyph);
    int sourceStart = -1;
    int sourceEnd = -1;
    int chapter = -1;
    EngineBase* engine = dm->GetEngine();
    if (engine->kind == kindEngineMupdf) {
        int endChapter = -1;
        if (!GetMupdfSelectionAnchor(annotations, engine, fromPage, fromGlyph, &chapter, &sourceStart) ||
            !GetMupdfSelectionAnchor(annotations, engine, toPage, toGlyph, &endChapter, &sourceEnd) ||
            chapter != endChapter || sourceEnd <= sourceStart) {
            return nullptr;
        }
    } else {
        if (!EngineEbookGetSourceOffset(engine, fromPage, fromGlyph, false, &sourceStart) ||
            !EngineEbookGetSourceOffset(engine, toPage, toGlyph, true, &sourceEnd) || sourceEnd <= sourceStart) {
            return nullptr;
        }
    }

    // Selecting the same range and highlighting it again deletes the existing
    // annotation (toggle), which is handy for quick undo without opening the list.
    for (size_t i = 0; i < annotations->items.size(); i++) {
        EbookAnnotation* existing = annotations->items.at(i);
        if (existing->type == type && existing->chapter == chapter && existing->sourceStart == sourceStart &&
            existing->sourceEnd == sourceEnd) {
            annotations->items.RemoveAt(i);
            delete existing;
            SaveEbookAnnotations(annotations);
            return nullptr;
        }
    }

    bool isTextOnlySelection = false;
    TempStr exact = GetSelectedTextTemp(tab, " ", isTextOnlySelection);
    if (!isTextOnlySelection || str::IsEmpty(exact)) {
        return nullptr;
    }

    constexpr int kContextChars = 32;
    auto annotation = new EbookAnnotation();
    annotation->type = type;
    annotation->chapter = chapter;
    annotation->sourceStart = sourceStart;
    annotation->sourceEnd = sourceEnd;
    annotation->color = color;
    annotation->exact = str::Dup(exact);
    annotation->prefix = str::Dup(ExtractContextTemp(engine, fromPage, fromGlyph - kContextChars, fromGlyph));
    annotation->suffix = str::Dup(ExtractContextTemp(engine, toPage, toGlyph, toGlyph + kContextChars));
    InitEbookAnnotationMetadata(annotation);
    annotations->items.Append(annotation);
    if (!SaveEbookAnnotations(annotations)) {
        annotations->items.RemoveAt(annotations->items.size() - 1);
        delete annotation;
        return nullptr;
    }
    return annotation;
}

static int FindGlyphAtPagePoint(EngineBase* engine, int pageNo, PointF pagePoint) {
    int textLen = 0;
    Rect* coords = nullptr;
    const WCHAR* text = engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!text || !coords || textLen <= 0) {
        return -1;
    }

    int nearest = -1;
    float nearestDistance = FLT_MAX;
    for (int i = 0; i < textLen; i++) {
        Rect& rect = coords[i];
        if (rect.IsEmpty() || !IsEbookAnchorChar(text[i])) {
            continue;
        }
        if (pagePoint.x >= rect.x && pagePoint.x <= rect.BR().x && pagePoint.y >= rect.y &&
            pagePoint.y <= rect.BR().y) {
            return i;
        }
        float dx = pagePoint.x - ((float)rect.x + (float)rect.dx / 2.f);
        float dy = pagePoint.y - ((float)rect.y + (float)rect.dy / 2.f);
        float distance = dx * dx + dy * dy;
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = i;
        }
    }
    return nearest;
}

static bool GetAnchorRangeAtGlyph(EbookAnnotations* annotations, EngineBase* engine, int pageNo, int glyph,
                                  int* chapterOut, int* sourceStartOut, int* sourceEndOut) {
    int textLen = 0;
    engine->GetTextForPage(pageNo, &textLen);
    while (glyph >= 0 && glyph < textLen) {
        int chapter = -1;
        int sourceStart = -1;
        int sourceEnd = -1;
        bool ok = false;
        if (engine->kind == kindEngineMupdf) {
            int endChapter = -1;
            ok = GetMupdfSelectionAnchor(annotations, engine, pageNo, glyph, &chapter, &sourceStart) &&
                 GetMupdfSelectionAnchor(annotations, engine, pageNo, glyph + 1, &endChapter, &sourceEnd) &&
                 chapter == endChapter;
        } else {
            ok = EngineEbookGetSourceOffset(engine, pageNo, glyph, false, &sourceStart) &&
                 EngineEbookGetSourceOffset(engine, pageNo, glyph + 1, true, &sourceEnd);
        }
        if (ok && sourceEnd > sourceStart) {
            *chapterOut = chapter;
            *sourceStartOut = sourceStart;
            *sourceEndOut = sourceEnd;
            return true;
        }
        glyph++;
    }
    return false;
}

EbookAnnotation* EbookAnnotationsCreateText(WindowTab* tab, DisplayModel* dm, Point canvasPoint, COLORREF color) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !dm) {
        return nullptr;
    }
    int pageNo = dm->GetPageNoByPoint(canvasPoint);
    if (!dm->ValidPageNo(pageNo)) {
        return nullptr;
    }
    EngineBase* engine = dm->GetEngine();
    // Pause background reflow loading so this stays responsive during progressive load.
    ReflowLoadingPauseScope reflowPause(engine);
    PointF pagePoint = dm->CvtFromScreen(canvasPoint, pageNo);
    int glyph = FindGlyphAtPagePoint(engine, pageNo, pagePoint);
    if (glyph < 0) {
        return nullptr;
    }

    int chapter = -1;
    int sourceStart = -1;
    int sourceEnd = -1;
    if (!GetAnchorRangeAtGlyph(annotations, engine, pageNo, glyph, &chapter, &sourceStart, &sourceEnd)) {
        return nullptr;
    }

    int textLen = 0;
    engine->GetTextForPage(pageNo, &textLen);
    auto annotation = new EbookAnnotation();
    annotation->type = AnnotationType::Text;
    annotation->chapter = chapter;
    annotation->sourceStart = sourceStart;
    annotation->sourceEnd = sourceEnd;
    annotation->color = color;
    annotation->exact = str::Dup(ExtractContextTemp(engine, pageNo, glyph, std::min(glyph + 1, textLen)));
    constexpr int kContextChars = 32;
    annotation->prefix = str::Dup(ExtractContextTemp(engine, pageNo, glyph - kContextChars, glyph));
    annotation->suffix = str::Dup(ExtractContextTemp(engine, pageNo, glyph + 1, glyph + 1 + kContextChars));
    InitEbookAnnotationMetadata(annotation);
    annotations->items.Append(annotation);
    if (!SaveEbookAnnotations(annotations)) {
        annotations->items.RemoveAt(annotations->items.size() - 1);
        delete annotation;
        return nullptr;
    }
    return annotation;
}

static int GlyphAtAnchorStart(const WCHAR* text, int textLen, int anchorOffset) {
    int offset = 0;
    for (int i = 0; i < textLen; i++) {
        if (!IsEbookAnchorChar(text[i])) {
            continue;
        }
        if (offset == anchorOffset) {
            return i;
        }
        offset++;
    }
    return textLen;
}

static int GlyphAtAnchorEnd(const WCHAR* text, int textLen, int anchorOffset) {
    if (anchorOffset <= 0) {
        return 0;
    }
    int offset = 0;
    for (int i = 0; i < textLen; i++) {
        if (!IsEbookAnchorChar(text[i])) {
            continue;
        }
        offset++;
        if (offset == anchorOffset) {
            return i + 1;
        }
    }
    return textLen;
}

static bool GetMupdfAnnotationPageRects(EbookAnnotations* annotations, EngineBase* engine, EbookAnnotation* annotation,
                                        int pageNo, Vec<RectF>& rectsOut) {
    int chapter = -1;
    int chapterStartPage = 0;
    if (annotation->chapter < 0 || !EngineMupdfGetReflowPageChapter(engine, pageNo, &chapter, &chapterStartPage) ||
        chapter != annotation->chapter) {
        return false;
    }
    int pageStart = 0;
    if (!GetMupdfPageAnchorStart(annotations, engine, chapter, chapterStartPage, pageNo, &pageStart)) {
        return false;
    }

    int textLen = 0;
    Rect* coords = nullptr;
    const WCHAR* text = engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!text || !coords || textLen <= 0) {
        return false;
    }
    int pageEnd = pageStart + CountEbookAnchorChars(text, textLen);
    if (annotation->sourceEnd <= pageStart || annotation->sourceStart >= pageEnd) {
        return false;
    }
    int localStart = annotation->sourceStart > pageStart ? annotation->sourceStart - pageStart : 0;
    int localEnd = annotation->sourceEnd < pageEnd ? annotation->sourceEnd - pageStart : pageEnd - pageStart;
    int fromGlyph = GlyphAtAnchorStart(text, textLen, localStart);
    int toGlyph = GlyphAtAnchorEnd(text, textLen, localEnd);
    if (toGlyph <= fromGlyph) {
        return false;
    }

    Rect mediabox = engine->PageMediabox(pageNo).Round();
    Rect* c = coords + fromGlyph;
    Rect* end = coords + toGlyph;
    while (c < end) {
        while (c < end && !c->x && !c->dx) {
            c++;
        }
        if (c >= end) {
            break;
        }
        Rect* lineStart = c;
        while (c < end && (c->x || c->dx)) {
            c++;
        }
        Rect rect = BuildHighlightLineRect(lineStart, c).Intersect(mediabox);
        if (!rect.IsEmpty()) {
            rectsOut.Append(ToRectF(rect));
        }
    }
    return !rectsOut.empty();
}

static bool GetAnnotationPageRects(EbookAnnotations* annotations, EngineBase* engine, EbookAnnotation* annotation,
                                   int pageNo, Vec<RectF>& rectsOut) {
    if (annotation->chapter >= 0) {
        return engine->kind == kindEngineMupdf &&
               GetMupdfAnnotationPageRects(annotations, engine, annotation, pageNo, rectsOut);
    }
    return EngineEbookGetSourceRangeRects(engine, pageNo, annotation->sourceStart, annotation->sourceEnd, rectsOut);
}

static int FindEbookAnnotationAt(WindowTab* tab, DisplayModel* dm, Point canvasPoint) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !dm) {
        return -1;
    }
    int pageNo = dm->GetPageNoByPoint(canvasPoint);
    if (!dm->ValidPageNo(pageNo)) {
        return -1;
    }
    PointF pagePoint = dm->CvtFromScreen(canvasPoint, pageNo);
    EngineBase* engine = dm->GetEngine();
    for (int i = (int)annotations->items.size() - 1; i >= 0; i--) {
        EbookAnnotation* annotation = annotations->items.at(i);
        Vec<RectF> rects;
        if (!GetAnnotationPageRects(annotations, engine, annotation, pageNo, rects)) {
            continue;
        }
        if (annotation->type == AnnotationType::Text && !rects.empty()) {
            Rect anchor = dm->CvtToScreen(pageNo, rects.at(0));
            int size = DpiScale(tab->win->hwndFrame, 14);
            Rect marker(anchor.x - size / 2, anchor.y - size / 2, size, size);
            if (marker.Contains(canvasPoint)) {
                return i;
            }
            continue;
        }
        for (RectF rect : rects) {
            rect = ScaleHighlightBandRect(rect, kSelectionHighlightBandRatio);
            if (rect.Contains(pagePoint)) {
                return i;
            }
        }
    }
    return -1;
}

EbookAnnotation* EbookAnnotationsGetAt(WindowTab* tab, DisplayModel* dm, Point canvasPoint) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    int idx = FindEbookAnnotationAt(tab, dm, canvasPoint);
    return annotations && idx >= 0 ? annotations->items.at(idx) : nullptr;
}

bool EbookAnnotationsHitTest(WindowTab* tab, DisplayModel* dm, Point canvasPoint) {
    return FindEbookAnnotationAt(tab, dm, canvasPoint) >= 0;
}

bool EbookAnnotationsDeleteAt(WindowTab* tab, DisplayModel* dm, Point canvasPoint) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    // Pause background reflow loading so this stays responsive during progressive load.
    ReflowLoadingPauseScope reflowPause(dm ? dm->GetEngine() : nullptr);
    int idx = FindEbookAnnotationAt(tab, dm, canvasPoint);
    if (!annotations || idx < 0) {
        return false;
    }
    EbookAnnotation* annotation = annotations->items.at(idx);
    annotations->items.RemoveAt(idx);
    if (!SaveEbookAnnotations(annotations)) {
        annotations->items.InsertAt(idx, annotation);
        return false;
    }
    delete annotation;
    return true;
}

bool EbookAnnotationsDelete(WindowTab* tab, EbookAnnotation* annotation) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    int idx = annotations ? annotations->items.Find(annotation) : -1;
    if (idx < 0) {
        return false;
    }
    annotations->items.RemoveAt(idx);
    if (!SaveEbookAnnotations(annotations)) {
        annotations->items.InsertAt(idx, annotation);
        return false;
    }
    delete annotation;
    return true;
}

void EbookAnnotationsGetAll(WindowTab* tab, Vec<EbookAnnotation*>& annotationsOut) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations) {
        return;
    }
    for (EbookAnnotation* annotation : annotations->items) {
        annotationsOut.Append(annotation);
    }
}

AnnotationType EbookAnnotationGetType(EbookAnnotation* annotation) {
    return annotation ? annotation->type : AnnotationType::Unknown;
}

const char* EbookAnnotationGetText(EbookAnnotation* annotation) {
    return annotation ? annotation->exact : nullptr;
}

const char* EbookAnnotationGetNote(EbookAnnotation* annotation) {
    return annotation ? annotation->note : nullptr;
}

const char* EbookAnnotationGetAuthor(EbookAnnotation* annotation) {
    return annotation ? annotation->author : nullptr;
}

time_t EbookAnnotationGetCreated(EbookAnnotation* annotation) {
    return annotation ? annotation->created : 0;
}

time_t EbookAnnotationGetModified(EbookAnnotation* annotation) {
    return annotation ? annotation->modified : 0;
}

COLORREF EbookAnnotationGetColor(EbookAnnotation* annotation) {
    if (!annotation) {
        return GetDefaultAnnotationColor(AnnotationType::Highlight);
    }
    if (annotation->color == 0) {
        return GetDefaultAnnotationColor(annotation->type);
    }
    return annotation->color;
}

bool EbookAnnotationSetNote(WindowTab* tab, EbookAnnotation* annotation, const char* note) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || annotations->items.Find(annotation) < 0) {
        return false;
    }
    AutoFreeStr previous(annotation->note);
    annotation->note = str::Dup(note);
    TouchEbookAnnotationModified(annotation);
    if (!SaveEbookAnnotations(annotations)) {
        str::Free(annotation->note);
        annotation->note = previous.StealData();
        return false;
    }
    return true;
}

bool EbookAnnotationSetColor(WindowTab* tab, EbookAnnotation* annotation, COLORREF color) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || annotations->items.Find(annotation) < 0) {
        return false;
    }
    COLORREF previous = annotation->color;
    annotation->color = color;
    TouchEbookAnnotationModified(annotation);
    if (!SaveEbookAnnotations(annotations)) {
        annotation->color = previous;
        return false;
    }
    return true;
}

int EbookAnnotationGetPageNo(WindowTab* tab, EbookAnnotation* annotation) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    EngineBase* engine = tab ? tab->GetEngine() : nullptr;
    if (!annotations || !engine || !annotation || annotations->items.Find(annotation) < 0) {
        return 0;
    }
    if (annotation->chapter < 0) {
        return EngineEbookGetSourcePageNo(engine, annotation->sourceStart);
    }

    int startPage = 0;
    int endPage = 0;
    if (!EngineMupdfGetReflowChapterPageRange(engine, annotation->chapter, &startPage, &endPage)) {
        return 0;
    }
    for (int pageNo = startPage; pageNo <= endPage; pageNo++) {
        int pageStart = 0;
        if (!GetMupdfPageAnchorStart(annotations, engine, annotation->chapter, startPage, pageNo, &pageStart)) {
            return 0;
        }
        int textLen = 0;
        const WCHAR* text = engine->GetTextForPage(pageNo, &textLen);
        int pageEnd = pageStart + CountEbookAnchorChars(text, textLen);
        if (annotation->sourceStart >= pageStart && annotation->sourceStart < pageEnd) {
            return pageNo;
        }
    }
    return 0;
}

struct EbookAnnotationSortItem {
    EbookAnnotation* annotation = nullptr;
    int pageNo = 0;
    int sourceStart = 0;
};

static void AppendMarkdownBlockquote(StrBuilder& out, const char* text) {
    if (str::IsEmpty(text)) {
        return;
    }
    const u8* p = (const u8*)text;
    bool lineStart = true;
    while (*p) {
        u8 c = *p++;
        if (lineStart) {
            out.Append("> ");
            lineStart = false;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            out.AppendChar('\n');
            lineStart = true;
            continue;
        }
        out.AppendChar((char)c);
    }
    if (!lineStart) {
        out.AppendChar('\n');
    }
}

static bool BuildEbookAnnotationsExport(WindowTab* tab, StrBuilder& out) {
    Vec<EbookAnnotation*> annotations;
    EbookAnnotationsGetAll(tab, annotations);
    if (annotations.empty()) {
        return false;
    }

    Vec<EbookAnnotationSortItem> items;
    for (EbookAnnotation* annotation : annotations) {
        EbookAnnotationSortItem item;
        item.annotation = annotation;
        item.pageNo = EbookAnnotationGetPageNo(tab, annotation);
        item.sourceStart = annotation->sourceStart;
        items.Append(item);
    }
    std::sort(items.begin(), items.end(), [](const EbookAnnotationSortItem& a, const EbookAnnotationSortItem& b) {
        if (a.pageNo != b.pageNo) {
            return a.pageNo < b.pageNo;
        }
        return a.sourceStart < b.sourceStart;
    });

    out.AppendFmt("# %s\n\n", tab->GetTabTitle());
    out.AppendFmt("%s: %s\n", _TRA("Source"), tab->filePath);
    out.Append(_TRA("Exported:"));
    out.Append(" ");
    AppendUtcDateTime(out, time(nullptr));
    out.Append("\n\n---\n\n");

    for (const EbookAnnotationSortItem& item : items) {
        EbookAnnotation* annotation = item.annotation;
        TempStr typeName = AnnotationReadableNameTemp(EbookAnnotationGetType(annotation));
        out.AppendFmt("## %s %d — %s\n\n", _TRA("Page"), item.pageNo, typeName);

        const char* text = EbookAnnotationGetText(annotation);
        if (!str::IsEmpty(text)) {
            AppendMarkdownBlockquote(out, text);
            out.AppendChar('\n');
        }

        const char* note = EbookAnnotationGetNote(annotation);
        if (!str::IsEmpty(note)) {
            out.AppendFmt("**%s**\n\n", _TRA("Note:"));
            out.Append(note);
            out.Append("\n\n");
        }

        const char* author = EbookAnnotationGetAuthor(annotation);
        if (!str::IsEmpty(author)) {
            out.Append(_TRA("Author:"));
            out.Append(" ");
            out.Append(author);
            out.Append("\n");
        }
        time_t date = EbookAnnotationGetModified(annotation);
        if (date <= 0) {
            date = EbookAnnotationGetCreated(annotation);
        }
        if (date > 0) {
            out.Append(_TRA("Date:"));
            out.Append(" ");
            AppendUtcDateTime(out, date);
            out.AppendChar('\n');
        }
        out.Append("\n---\n\n");
    }
    return true;
}

bool EbookAnnotationsExportNotes(WindowTab* tab, HWND hwndParent) {
    if (!tab || !EbookAnnotationsSupported(tab)) {
        return false;
    }
    StrBuilder out;
    if (!BuildEbookAnnotationsExport(tab, out)) {
        NotificationCreateArgs nargs;
        nargs.hwndParent = hwndParent;
        nargs.font = GetDefaultGuiFont();
        nargs.timeoutMs = 4000;
        nargs.msg = _TRA("No annotations to export.");
        ShowNotification(nargs);
        return false;
    }

    TempStr defaultPath =
        path::JoinTemp(path::GetDirTemp(tab->filePath),
                       str::JoinTemp(path::GetBaseNameTemp(path::GetPathNoExtTemp(tab->filePath)), "-notes.md"));
    if (!SaveDataToFile(hwndParent, defaultPath, ByteSlice((const u8*)out.Get(), out.size()))) {
        return false;
    }

    NotificationCreateArgs nargs;
    nargs.hwndParent = hwndParent;
    nargs.font = GetDefaultGuiFont();
    nargs.timeoutMs = 5000;
    nargs.msg = _TRA("Exported annotations.");
    ShowNotification(nargs);
    return true;
}

// PDF annotations are rendered into the page bitmap and therefore go through
// UpdateBitmapColors() together with the page. EPUB annotations are painted as
// an overlay, so apply the same affine color mapping before painting them.
static COLORREF MapEbookAnnotationColor(COLORREF color) {
    PdfDocumentColorMode docMode = GetPdfDocumentColorMode();
    bool pageUsesThemeColors =
        docMode != PdfDocumentColorMode::Light && (ThemeUsesDarkChrome() || !ThemeUsesOriginalPageColors());
    if (!pageUsesThemeColors) {
        return color;
    }

    COLORREF bgColor;
    COLORREF textColor = ThemePageRenderColors(bgColor, true);
    u8 r, g, b;
    u8 rt, gt, bt;
    u8 rb, gb, bb;
    UnpackColor(color, r, g, b);
    UnpackColor(textColor, rt, gt, bt);
    UnpackColor(bgColor, rb, gb, bb);

    auto mapChannel = [](u8 value, u8 text, u8 background) -> u8 {
        int x = value * ((int)background - (int)text) + 128;
        x += x >> 8;
        return (u8)(text + (x >> 8));
    };
    return RGB(mapChannel(r, rt, rb), mapChannel(g, gt, gb), mapChannel(b, bt, bb));
}

static void PaintEbookTextMarker(WindowTab* tab, HDC hdc, Rect anchor, COLORREF color) {
    int size = DpiScale(tab->win->hwndFrame, 14);
    Rect marker(anchor.x - size / 2, anchor.y - size / 2, size, size);
    u8 r, g, b;
    UnpackColor(color, r, g, b);
    COLORREF borderColor = MapEbookAnnotationColor(RGB(45, 45, 45));
    u8 borderR, borderG, borderB;
    UnpackColor(borderColor, borderR, borderG, borderB);
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(Gdiplus::Color(230, r, g, b));
    Gdiplus::Pen pen(Gdiplus::Color(210, borderR, borderG, borderB), 1.f);
    Gdiplus::Rect ellipse(marker.x, marker.y, marker.dx, marker.dy - size / 4);
    graphics.FillEllipse(&brush, ellipse);
    graphics.DrawEllipse(&pen, ellipse);
    Gdiplus::Point tail[] = {{marker.x + size / 3, marker.y + marker.dy - size / 3},
                             {marker.x + size / 3, marker.y + marker.dy},
                             {marker.x + size / 2, marker.y + marker.dy - size / 3}};
    graphics.FillPolygon(&brush, tail, dimof(tail));
}

static void PaintEbookMarkup(WindowTab* tab, HDC hdc, Vec<Rect>& screenRects, EbookAnnotation* annotation) {
    if (screenRects.empty()) {
        return;
    }
    if (annotation->type == AnnotationType::Text) {
        PaintEbookTextMarker(tab, hdc, screenRects.at(0), MapEbookAnnotationColor(annotation->color));
        return;
    }
    PaintTextMarkupOverlay(hdc, tab->win->canvasRc, annotation->type, annotation->color, screenRects);
}

// Match MuPDF/Acrobat markup appearance (pdf-appearance.c):
// highlight: Multiply blend
// underline: line at 1/7 of height from bottom, thickness h/16
// strike-out: line at 3/7 of height from bottom, thickness h/16
// squiggly: zigzag from baseline with amplitude h/7, step h/7, thickness h/16
void PaintTextMarkupOverlay(HDC hdc, Rect canvasRc, AnnotationType type, COLORREF color, Vec<Rect>& screenRects) {
    if (screenRects.empty()) {
        return;
    }
    if (type == AnnotationType::Highlight) {
        PaintMultiplyRectangles(hdc, canvasRc, screenRects, color);
        return;
    }

    color = MapEbookAnnotationColor(color);

    u8 r, g, b;
    UnpackColor(color, r, g, b);
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    for (Rect rect : screenRects) {
        if (rect.dx <= 0 || rect.dy <= 0) {
            continue;
        }
        float h = (float)rect.dy;
        float lineWidth = std::max(1.f, h / 16.f);
        Gdiplus::Pen pen(Gdiplus::Color(255, r, g, b), lineWidth);
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);

        float x1 = (float)rect.x;
        float x2 = (float)rect.BR().x;
        float yBot = (float)rect.BR().y;
        float yTop = (float)rect.y;

        if (type == AnnotationType::Underline) {
            float y = yBot - h / 7.f;
            graphics.DrawLine(&pen, x1, y, x2, y);
            continue;
        }
        if (type == AnnotationType::StrikeOut) {
            float y = yBot - h * 3.f / 7.f;
            graphics.DrawLine(&pen, x1, y, x2, y);
            continue;
        }
        if (type != AnnotationType::Squiggly) {
            continue;
        }

        // Triangular wave along the baseline, peaking at h/7 toward the top.
        float width = x2 - x1;
        if (width <= 0) {
            continue;
        }
        Gdiplus::GraphicsPath path;
        float x = 0;
        bool up = true;
        float cx = x1;
        float cy = yBot;
        path.StartFigure();
        while (x < width) {
            x += h / 7.f;
            float t = std::min(x / width, 1.f);
            float ax = x1 + t * width;
            if (up) {
                float ny = yBot + (yTop - yBot) / 7.f;
                path.AddLine(cx, cy, ax, ny);
                cx = ax;
                cy = ny;
            } else {
                path.AddLine(cx, cy, ax, yBot);
                cx = ax;
                cy = yBot;
            }
            up = !up;
        }
        graphics.DrawPath(&pen, &path);
    }
}

void EbookAnnotationsPaintPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo) {
    if (!tab || tab->hideAnnotations || !dm || !dm->PageVisible(pageNo)) {
        return;
    }
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || annotations->items.empty()) {
        return;
    }

    EngineBase* engine = dm->GetEngine();
    for (EbookAnnotation* annotation : annotations->items) {
        Vec<RectF> pageRects;
        if (!GetAnnotationPageRects(annotations, engine, annotation, pageNo, pageRects)) {
            continue;
        }
        NormalizeNearbyHighlightHeights(pageRects);
        Vec<Rect> screenRects;
        for (RectF rect : pageRects) {
            // Paint with the text quad height (same as PDF QuadPoints). Hit-testing
            // still uses a slightly scaled band in FindEbookAnnotationAt.
            Rect screenRect = dm->CvtToScreen(pageNo, rect);
            if (!screenRect.IsEmpty()) {
                screenRects.Append(screenRect);
            }
        }
        PaintEbookMarkup(tab, hdc, screenRects, annotation);
    }
}
