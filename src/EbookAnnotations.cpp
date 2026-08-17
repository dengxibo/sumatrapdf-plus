/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include <algorithm>
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
    COLORREF color = kColorUnset;
    int opacity = 100;
    float offsetX = 0;
    float offsetY = 0;
    float width = 0;
    float height = 0;
    int textAlignment = 0;
    int textSize = 12;
    int borderWidth = 1;
    int lineStart = 0;
    int lineEnd = 0;
    // true: top-left to bottom-right of bounds. false: top-right to bottom-left.
    // Default false matches the previous hardcoded '/' diagonal.
    bool lineTLBR = false;
    bool backgroundTransparent = true;
    COLORREF backgroundColor = 0;
    bool interiorTransparent = true;
    COLORREF interiorColor = 0;
    char* exact = nullptr;
    char* prefix = nullptr;
    char* suffix = nullptr;
    char* note = nullptr;
    char* icon = nullptr;
    char* textFont = nullptr;
    char* author = nullptr;
    time_t created = 0;
    time_t modified = 0;
    Vec<PointF> inkPoints;

    ~EbookAnnotation() {
        str::Free(exact);
        str::Free(prefix);
        str::Free(suffix);
        str::Free(note);
        str::Free(icon);
        str::Free(textFont);
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

static bool IsEbookPointAnnotationType(AnnotationType type) {
    return type == AnnotationType::Text || type == AnnotationType::FreeText || type == AnnotationType::Stamp ||
           type == AnnotationType::Caret || type == AnnotationType::Line || type == AnnotationType::Square ||
           type == AnnotationType::Circle || type == AnnotationType::Ink;
}

static SizeF GetDefaultEbookPointAnnotationSize(AnnotationType type) {
    switch (type) {
        case AnnotationType::Text:
            return {16, 16};
        case AnnotationType::FreeText:
            return {200, 100};
        case AnnotationType::Stamp:
            return {190, 50};
        case AnnotationType::Caret:
            return {18, 15};
        case AnnotationType::Line:
        case AnnotationType::Square:
        case AnnotationType::Circle:
        case AnnotationType::Ink:
            return {100, 50};
        default:
            return {};
    }
}

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

static const char* GetDefaultEbookTextIconTemp() {
    char* icon = str::DupTemp(gGlobalPrefs->annotations.textIconType);
    str::RemoveCharsInPlace(icon, " ");
    int idx = seqstrings::StrToIdxIS(gAnnotationTextIcons, icon);
    if (idx < 0) {
        return "Note";
    }
    return seqstrings::IdxToStr(gAnnotationTextIcons, idx);
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
            } else if (str::Eq(property, "opacity")) {
                str::Parse(value, "%d", &annotation->opacity);
            } else if (str::Eq(property, "created")) {
                i64 secs = 0;
                str::Parse(value, "%lld", &secs);
                annotation->created = (time_t)secs;
            } else if (str::Eq(property, "modified")) {
                i64 secs = 0;
                str::Parse(value, "%lld", &secs);
                annotation->modified = (time_t)secs;
            } else if (str::Eq(property, "offsetX")) {
                str::Parse(value, "%f", &annotation->offsetX);
            } else if (str::Eq(property, "offsetY")) {
                str::Parse(value, "%f", &annotation->offsetY);
            } else if (str::Eq(property, "width")) {
                str::Parse(value, "%f", &annotation->width);
            } else if (str::Eq(property, "height")) {
                str::Parse(value, "%f", &annotation->height);
            } else if (str::Eq(property, "textAlignment")) {
                str::Parse(value, "%d", &annotation->textAlignment);
            } else if (str::Eq(property, "textSize")) {
                str::Parse(value, "%d", &annotation->textSize);
            } else if (str::Eq(property, "borderWidth")) {
                str::Parse(value, "%d", &annotation->borderWidth);
            } else if (str::Eq(property, "lineStart")) {
                str::Parse(value, "%d", &annotation->lineStart);
            } else if (str::Eq(property, "lineEnd")) {
                str::Parse(value, "%d", &annotation->lineEnd);
            } else if (str::Eq(property, "lineTLBR")) {
                int v = 0;
                str::Parse(value, "%d", &v);
                annotation->lineTLBR = v != 0;
            } else if (str::Eq(property, "backgroundTransparent")) {
                int transparent = 1;
                str::Parse(value, "%d", &transparent);
                annotation->backgroundTransparent = transparent != 0;
            } else if (str::Eq(property, "backgroundColor")) {
                u32 color = 0;
                str::Parse(value, "%u", &color);
                annotation->backgroundColor = (COLORREF)color;
            } else if (str::Eq(property, "interiorTransparent")) {
                int transparent = 1;
                str::Parse(value, "%d", &transparent);
                annotation->interiorTransparent = transparent != 0;
            } else if (str::Eq(property, "interiorColor")) {
                u32 color = 0;
                str::Parse(value, "%u", &color);
                annotation->interiorColor = (COLORREF)color;
            } else if (str::StartsWith(property, "inkPoints/[")) {
                const char* idxStart = property + str::Len("inkPoints/[");
                int coordIdx = 0;
                str::Parse(idxStart, "%d", &coordIdx);
                float coord = 0;
                str::Parse(value, "%f", &coord);
                int pointIdx = coordIdx / 2;
                while ((int)annotation->inkPoints.len <= pointIdx) {
                    annotation->inkPoints.Append(PointF{});
                }
                if (coordIdx % 2 == 0) {
                    annotation->inkPoints.at(pointIdx).x = coord;
                } else {
                    annotation->inkPoints.at(pointIdx).y = coord;
                }
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
                } else if (str::EqI(value, "freeText")) {
                    annotation->type = AnnotationType::FreeText;
                } else if (str::EqI(value, "stamp")) {
                    annotation->type = AnnotationType::Stamp;
                } else if (str::EqI(value, "caret")) {
                    annotation->type = AnnotationType::Caret;
                } else if (str::EqI(value, "line")) {
                    annotation->type = AnnotationType::Line;
                } else if (str::EqI(value, "square")) {
                    annotation->type = AnnotationType::Square;
                } else if (str::EqI(value, "circle")) {
                    annotation->type = AnnotationType::Circle;
                } else if (str::EqI(value, "ink")) {
                    annotation->type = AnnotationType::Ink;
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
            } else if (str::Eq(property, "icon")) {
                str::ReplaceWithCopy(&annotation->icon, value);
            } else if (str::Eq(property, "textFont")) {
                str::ReplaceWithCopy(&annotation->textFont, value);
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
        } else if (annotation->type == AnnotationType::FreeText) {
            type = "freeText";
        } else if (annotation->type == AnnotationType::Stamp) {
            type = "stamp";
        } else if (annotation->type == AnnotationType::Caret) {
            type = "caret";
        } else if (annotation->type == AnnotationType::Line) {
            type = "line";
        } else if (annotation->type == AnnotationType::Square) {
            type = "square";
        } else if (annotation->type == AnnotationType::Circle) {
            type = "circle";
        } else if (annotation->type == AnnotationType::Ink) {
            type = "ink";
        }
        out.Append("\n      \"type\": ");
        AppendJsonString(out, type);
        out.AppendFmt(",\n      \"chapter\": %d,\n      \"start\": %d,\n      \"end\": %d,\n      \"color\": %u",
                      annotation->chapter, annotation->sourceStart, annotation->sourceEnd, (uint)annotation->color);
        if (IsEbookPointAnnotationType(annotation->type)) {
            out.AppendFmt(
                ",\n      \"offsetX\": %.3f,\n      \"offsetY\": %.3f,\n      \"width\": %.3f,\n      "
                "\"height\": %.3f",
                annotation->offsetX, annotation->offsetY, annotation->width, annotation->height);
        }
        out.Append(",\n      \"exact\": ");
        AppendJsonString(out, annotation->exact);
        out.Append(",\n      \"prefix\": ");
        AppendJsonString(out, annotation->prefix);
        out.Append(",\n      \"suffix\": ");
        AppendJsonString(out, annotation->suffix);
        out.Append(",\n      \"note\": ");
        AppendJsonString(out, annotation->note);
        if (!str::IsEmpty(annotation->icon)) {
            out.Append(",\n      \"icon\": ");
            AppendJsonString(out, annotation->icon);
        }
        if (annotation->type == AnnotationType::FreeText) {
            out.AppendFmt(
                ",\n      \"textAlignment\": %d,\n      \"textSize\": %d,\n      \"borderWidth\": %d,\n      "
                "\"backgroundTransparent\": %d,\n      \"backgroundColor\": %u",
                annotation->textAlignment, annotation->textSize, annotation->borderWidth,
                annotation->backgroundTransparent ? 1 : 0, (uint)annotation->backgroundColor);
            if (!str::IsEmpty(annotation->textFont)) {
                out.Append(",\n      \"textFont\": ");
                AppendJsonString(out, annotation->textFont);
            }
        }
        if (annotation->type == AnnotationType::Highlight) {
            out.AppendFmt(",\n      \"opacity\": %d", annotation->opacity);
        }
        if (annotation->type == AnnotationType::Line || annotation->type == AnnotationType::Square ||
            annotation->type == AnnotationType::Circle) {
            out.AppendFmt(
                ",\n      \"borderWidth\": %d,\n      \"interiorTransparent\": %d,\n      \"interiorColor\": %u",
                annotation->borderWidth, annotation->interiorTransparent ? 1 : 0, (uint)annotation->interiorColor);
            if (annotation->type == AnnotationType::Line) {
                out.AppendFmt(",\n      \"lineStart\": %d,\n      \"lineEnd\": %d,\n      \"lineTLBR\": %d",
                              annotation->lineStart, annotation->lineEnd, annotation->lineTLBR ? 1 : 0);
            }
        }
        if (annotation->type == AnnotationType::Ink && annotation->inkPoints.len > 0) {
            out.AppendFmt(",\n      \"borderWidth\": %d", annotation->borderWidth);
            out.Append(",\n      \"inkPoints\": [");
            for (size_t idx = 0; idx < annotation->inkPoints.len; idx++) {
                PointF pt = annotation->inkPoints.els[idx];
                if (idx != 0) {
                    out.Append(", ");
                }
                out.AppendFmt("%.3f", (double)pt.x);
                out.Append(", ");
                out.AppendFmt("%.3f", (double)pt.y);
            }
            out.AppendChar(']');
        }
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

EbookAnnotation* EbookAnnotationsCreateAt(WindowTab* tab, DisplayModel* dm, Point canvasPoint, AnnotationType type,
                                          COLORREF color) {
    if (!IsEbookPointAnnotationType(type)) {
        return nullptr;
    }
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
    Rect* coords = nullptr;
    engine->GetTextForPage(pageNo, &textLen, &coords);
    auto annotation = new EbookAnnotation();
    annotation->type = type;
    annotation->chapter = chapter;
    annotation->sourceStart = sourceStart;
    annotation->sourceEnd = sourceEnd;
    annotation->color = color;
    if (coords && glyph < textLen) {
        annotation->offsetX = pagePoint.x - (float)coords[glyph].x;
        annotation->offsetY = pagePoint.y - (float)coords[glyph].y;
    }
    SizeF defaultSize = GetDefaultEbookPointAnnotationSize(type);
    annotation->width = defaultSize.dx;
    annotation->height = defaultSize.dy;
    if (type == AnnotationType::FreeText) {
        annotation->note = str::Dup("This is a text...");
        annotation->textFont = str::Dup("Helv");
        annotation->textSize = std::max(5, gGlobalPrefs->annotations.freeTextSize);
        annotation->borderWidth = std::max(0, gGlobalPrefs->annotations.freeTextBorderWidth);
        auto& background = gGlobalPrefs->annotations.freeTextBackgroundColorParsed;
        if (background.parsedOk) {
            annotation->backgroundTransparent = false;
            annotation->backgroundColor = background.col;
        }
    } else if (type == AnnotationType::Text) {
        annotation->icon = str::Dup(GetDefaultEbookTextIconTemp());
    } else if (type == AnnotationType::Stamp) {
        annotation->icon = str::Dup("Draft");
    }
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

EbookAnnotation* EbookAnnotationsCreateDragShape(WindowTab* tab, DisplayModel* dm, Point canvasStart, Point canvasEnd,
                                                 AnnotationType type) {
    if (!tab || !dm || !IsEbookPointAnnotationType(type) || type == AnnotationType::Ink) {
        return nullptr;
    }
    COLORREF color = GetDefaultEbookPointAnnotationColor(type);
    EbookAnnotation* annotation = EbookAnnotationsCreateAt(tab, dm, canvasStart, type, color);
    if (!annotation) {
        return nullptr;
    }
    int pageNo = dm->GetPageNoByPoint(canvasStart);
    if (!dm->ValidPageNo(pageNo)) {
        return nullptr;
    }
    RectF pageBounds;
    if (type == AnnotationType::Line) {
        PointF a = dm->CvtFromScreen(canvasStart, pageNo);
        PointF b = dm->CvtFromScreen(canvasEnd, pageNo);
        float x0 = std::min(a.x, b.x);
        float y0 = std::min(a.y, b.y);
        float x1 = std::max(a.x, b.x);
        float y1 = std::max(a.y, b.y);
        pageBounds = {x0, y0, std::max(1.f, x1 - x0), std::max(1.f, y1 - y0)};
        annotation->lineTLBR = (b.x - a.x) * (b.y - a.y) >= 0;
    } else {
        int x0 = std::min(canvasStart.x, canvasEnd.x);
        int y0 = std::min(canvasStart.y, canvasEnd.y);
        int x1 = std::max(canvasStart.x, canvasEnd.x);
        int y1 = std::max(canvasStart.y, canvasEnd.y);
        Rect normalized(x0, y0, x1 - x0, y1 - y0);
        pageBounds = dm->CvtFromScreen(normalized, pageNo);
    }
    if (pageBounds.IsEmpty()) {
        return nullptr;
    }
    if (!EbookAnnotationSetPageBounds(tab, dm, annotation, pageNo, pageBounds, false)) {
        return nullptr;
    }
    if (type == AnnotationType::Line || type == AnnotationType::Square || type == AnnotationType::Circle) {
        annotation->borderWidth = 1;
    }
    TouchEbookAnnotationModified(annotation);
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !SaveEbookAnnotations(annotations)) {
        return nullptr;
    }
    return annotation;
}

EbookAnnotation* EbookAnnotationsCreateInkStroke(WindowTab* tab, DisplayModel* dm, int pageNo, PointF* points,
                                                 int nPoints, COLORREF color) {
    if (!tab || !dm || !points || nPoints < 2 || !dm->ValidPageNo(pageNo)) {
        return nullptr;
    }
    Point canvasStart = dm->CvtToScreen(pageNo, points[0]);
    EbookAnnotation* annotation = EbookAnnotationsCreateAt(tab, dm, canvasStart, AnnotationType::Ink, color);
    if (!annotation) {
        return nullptr;
    }
    annotation->inkPoints.Reset();
    for (int i = 0; i < nPoints; i++) {
        annotation->inkPoints.Append(points[i]);
    }
    annotation->borderWidth = 1;
    TouchEbookAnnotationModified(annotation);
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !SaveEbookAnnotations(annotations)) {
        return nullptr;
    }
    return annotation;
}

EbookAnnotation* EbookAnnotationsCreateText(WindowTab* tab, DisplayModel* dm, Point canvasPoint, COLORREF color) {
    return EbookAnnotationsCreateAt(tab, dm, canvasPoint, AnnotationType::Text, color);
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

static bool GetPointAnnotationPageBounds(EbookAnnotations* annotations, EngineBase* engine, EbookAnnotation* annotation,
                                         int pageNo, RectF* boundsOut) {
    if (annotation->type == AnnotationType::Ink && annotation->inkPoints.len > 0) {
        Vec<RectF> anchorRects;
        if (!boundsOut || !GetAnnotationPageRects(annotations, engine, annotation, pageNo, anchorRects) ||
            anchorRects.empty()) {
            return false;
        }
        float minX = annotation->inkPoints.at(0).x;
        float minY = annotation->inkPoints.at(0).y;
        float maxX = minX;
        float maxY = minY;
        for (size_t i = 1; i < annotation->inkPoints.len; i++) {
            PointF pt = annotation->inkPoints.at(i);
            minX = std::min(minX, pt.x);
            minY = std::min(minY, pt.y);
            maxX = std::max(maxX, pt.x);
            maxY = std::max(maxY, pt.y);
        }
        constexpr float pad = 4.f;
        *boundsOut = {minX - pad, minY - pad, std::max(1.f, maxX - minX + pad * 2),
                      std::max(1.f, maxY - minY + pad * 2)};
        return true;
    }
    Vec<RectF> anchorRects;
    if (!boundsOut || !GetAnnotationPageRects(annotations, engine, annotation, pageNo, anchorRects) ||
        anchorRects.empty()) {
        return false;
    }
    RectF anchor = anchorRects.at(0);
    SizeF size = GetDefaultEbookPointAnnotationSize(annotation->type);
    if (annotation->width > 0) {
        size.dx = annotation->width;
    }
    if (annotation->height > 0) {
        size.dy = annotation->height;
    }
    *boundsOut = {anchor.x + annotation->offsetX, anchor.y + annotation->offsetY, size.dx, size.dy};
    return true;
}

bool EbookAnnotationGetPageBounds(WindowTab* tab, DisplayModel* dm, EbookAnnotation* annotation, int* pageNoOut,
                                  RectF* boundsOut) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !dm || !annotation || !IsEbookPointAnnotationType(annotation->type)) {
        return false;
    }
    int pageNo = EbookAnnotationGetPageNo(tab, annotation);
    if (!dm->ValidPageNo(pageNo) ||
        !GetPointAnnotationPageBounds(annotations, dm->GetEngine(), annotation, pageNo, boundsOut)) {
        return false;
    }
    if (pageNoOut) {
        *pageNoOut = pageNo;
    }
    return true;
}

bool EbookAnnotationSetPageBounds(WindowTab* tab, DisplayModel* dm, EbookAnnotation* annotation, int pageNo,
                                  RectF bounds, bool save) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !dm || !annotation || !IsEbookPointAnnotationType(annotation->type)) {
        return false;
    }
    Vec<RectF> anchorRects;
    if (!GetAnnotationPageRects(annotations, dm->GetEngine(), annotation, pageNo, anchorRects) || anchorRects.empty()) {
        return false;
    }
    RectF anchor = anchorRects.at(0);
    annotation->offsetX = bounds.x - anchor.x;
    annotation->offsetY = bounds.y - anchor.y;
    annotation->width = bounds.dx;
    annotation->height = bounds.dy;
    if (!save) {
        return true;
    }
    TouchEbookAnnotationModified(annotation);
    return SaveEbookAnnotations(annotations);
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
        if (IsEbookPointAnnotationType(annotation->type)) {
            RectF bounds;
            if (GetPointAnnotationPageBounds(annotations, engine, annotation, pageNo, &bounds) &&
                dm->CvtToScreen(pageNo, bounds).Contains(canvasPoint)) {
                return i;
            }
            continue;
        }
        Vec<RectF> rects;
        if (!GetAnnotationPageRects(annotations, engine, annotation, pageNo, rects)) {
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
    if (tab->selectedEbookAnnotation == annotation) {
        tab->selectedEbookAnnotation = nullptr;
    }
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
    if (tab->selectedEbookAnnotation == annotation) {
        tab->selectedEbookAnnotation = nullptr;
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

const char* EbookAnnotationGetIcon(EbookAnnotation* annotation) {
    if (!annotation) {
        return nullptr;
    }
    if (annotation->type != AnnotationType::Text && annotation->type != AnnotationType::Stamp) {
        return nullptr;
    }
    if (annotation->icon) {
        return annotation->icon;
    }
    return annotation->type == AnnotationType::Stamp ? "Draft" : "Note";
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

COLORREF GetDefaultEbookPointAnnotationColor(AnnotationType type) {
    if (type == AnnotationType::FreeText) {
        return RGB(0, 0, 0);
    }
    if (type == AnnotationType::Caret) {
        return RGB(0, 0, 255);
    }
    if (type == AnnotationType::Stamp || type == AnnotationType::Line || type == AnnotationType::Square ||
        type == AnnotationType::Circle || type == AnnotationType::Ink) {
        return RGB(255, 0, 0);
    }
    if (type == AnnotationType::Text) {
        return GetDefaultAnnotationColor(AnnotationType::Text);
    }
    return RGB(0, 0, 0);
}

COLORREF EbookAnnotationGetColor(EbookAnnotation* annotation) {
    if (!annotation) {
        return GetDefaultAnnotationColor(AnnotationType::Highlight);
    }
    if (IsEbookPointAnnotationType(annotation->type)) {
        if (IsSpecialColor(annotation->color)) {
            return GetDefaultEbookPointAnnotationColor(annotation->type);
        }
        return annotation->color;
    }
    if (IsSpecialColor(annotation->color)) {
        return GetDefaultAnnotationColor(annotation->type);
    }
    return annotation->color;
}

int EbookAnnotationGetOpacity(EbookAnnotation* annotation) {
    return annotation ? std::clamp(annotation->opacity, 0, 100) : 100;
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
    if (tab->win && annotation->type == AnnotationType::FreeText) {
        MainWindowRerender(tab->win);
    }
    return true;
}

bool EbookAnnotationSetIcon(WindowTab* tab, EbookAnnotation* annotation, const char* icon) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !annotation ||
        (annotation->type != AnnotationType::Text && annotation->type != AnnotationType::Stamp) ||
        annotations->items.Find(annotation) < 0) {
        return false;
    }
    const char* icons = annotation->type == AnnotationType::Stamp ? gStampIcons : gAnnotationTextIcons;
    int idx = seqstrings::StrToIdxIS(icons, icon);
    if (idx < 0) {
        return false;
    }
    const char* normalized = seqstrings::IdxToStr(icons, idx);
    if (str::Eq(annotation->icon, normalized)) {
        return true;
    }
    AutoFreeStr previous(annotation->icon);
    annotation->icon = str::Dup(normalized);
    TouchEbookAnnotationModified(annotation);
    if (!SaveEbookAnnotations(annotations)) {
        str::Free(annotation->icon);
        annotation->icon = previous.StealData();
        return false;
    }
    if (tab->win && (annotation->type == AnnotationType::Stamp || annotation->type == AnnotationType::Text)) {
        MainWindowRerender(tab->win);
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

static bool SaveEbookPointProperties(WindowTab* tab, EbookAnnotation* annotation);

bool EbookAnnotationSetOpacity(WindowTab* tab, EbookAnnotation* annotation, int opacity) {
    if (!annotation || annotation->type != AnnotationType::Highlight) return false;
    annotation->opacity = std::clamp(opacity, 0, 100);
    return SaveEbookPointProperties(tab, annotation);
}

static bool SaveEbookFreeTextProperties(WindowTab* tab, EbookAnnotation* annotation) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !annotation || annotation->type != AnnotationType::FreeText ||
        annotations->items.Find(annotation) < 0) {
        return false;
    }
    TouchEbookAnnotationModified(annotation);
    return SaveEbookAnnotations(annotations);
}

int EbookAnnotationGetFreeTextAlignment(EbookAnnotation* annotation) {
    return annotation ? annotation->textAlignment : 0;
}

const char* EbookAnnotationGetFreeTextFont(EbookAnnotation* annotation) {
    return annotation && !str::IsEmpty(annotation->textFont) ? annotation->textFont : "Helv";
}

int EbookAnnotationGetFreeTextSize(EbookAnnotation* annotation) {
    return annotation ? std::max(5, annotation->textSize) : 12;
}

int EbookAnnotationGetFreeTextBorderWidth(EbookAnnotation* annotation) {
    return annotation ? std::max(0, annotation->borderWidth) : 1;
}

bool EbookAnnotationGetFreeTextBackground(EbookAnnotation* annotation, COLORREF* colorOut) {
    if (!annotation || annotation->backgroundTransparent) {
        return false;
    }
    if (colorOut) {
        *colorOut = annotation->backgroundColor;
    }
    return true;
}

bool EbookAnnotationSetFreeTextAlignment(WindowTab* tab, EbookAnnotation* annotation, int alignment) {
    if (!annotation || alignment < 0 || alignment > 2) return false;
    annotation->textAlignment = alignment;
    return SaveEbookFreeTextProperties(tab, annotation);
}

bool EbookAnnotationSetFreeTextFont(WindowTab* tab, EbookAnnotation* annotation, const char* font) {
    if (!annotation || !font || !*font) return false;
    str::ReplaceWithCopy(&annotation->textFont, font);
    return SaveEbookFreeTextProperties(tab, annotation);
}

bool EbookAnnotationSetFreeTextSize(WindowTab* tab, EbookAnnotation* annotation, int size) {
    if (!annotation) return false;
    annotation->textSize = std::clamp(size, 5, 128);
    return SaveEbookFreeTextProperties(tab, annotation);
}

bool EbookAnnotationSetFreeTextBorderWidth(WindowTab* tab, EbookAnnotation* annotation, int width) {
    if (!annotation) return false;
    annotation->borderWidth = std::clamp(width, 0, 12);
    return SaveEbookFreeTextProperties(tab, annotation);
}

bool EbookAnnotationSetFreeTextBackground(WindowTab* tab, EbookAnnotation* annotation, bool transparent,
                                          COLORREF color) {
    if (!annotation) return false;
    annotation->backgroundTransparent = transparent;
    annotation->backgroundColor = color;
    return SaveEbookFreeTextProperties(tab, annotation);
}

int EbookAnnotationGetBorderWidth(EbookAnnotation* annotation) {
    return annotation ? std::clamp(annotation->borderWidth, 0, 12) : 1;
}

int EbookAnnotationGetLineStart(EbookAnnotation* annotation) {
    return annotation ? std::clamp(annotation->lineStart, 0, 9) : 0;
}

int EbookAnnotationGetLineEnd(EbookAnnotation* annotation) {
    return annotation ? std::clamp(annotation->lineEnd, 0, 9) : 0;
}

bool EbookAnnotationGetInteriorColor(EbookAnnotation* annotation, COLORREF* colorOut) {
    if (!annotation || annotation->interiorTransparent) return false;
    if (colorOut) *colorOut = annotation->interiorColor;
    return true;
}

static bool SaveEbookPointProperties(WindowTab* tab, EbookAnnotation* annotation) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !annotation || annotations->items.Find(annotation) < 0) return false;
    TouchEbookAnnotationModified(annotation);
    return SaveEbookAnnotations(annotations);
}

bool EbookAnnotationSetBorderWidth(WindowTab* tab, EbookAnnotation* annotation, int width) {
    if (!annotation) return false;
    annotation->borderWidth = std::clamp(width, 0, 12);
    return SaveEbookPointProperties(tab, annotation);
}

bool EbookAnnotationSetLineEnds(WindowTab* tab, EbookAnnotation* annotation, int start, int end) {
    if (!annotation || annotation->type != AnnotationType::Line) return false;
    annotation->lineStart = std::clamp(start, 0, 9);
    annotation->lineEnd = std::clamp(end, 0, 9);
    return SaveEbookPointProperties(tab, annotation);
}

bool EbookAnnotationSetInteriorColor(WindowTab* tab, EbookAnnotation* annotation, bool transparent, COLORREF color) {
    if (!annotation) return false;
    annotation->interiorTransparent = transparent;
    annotation->interiorColor = color;
    return SaveEbookPointProperties(tab, annotation);
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

// PDF annotations are rendered into the page bitmap. EPUB annotations are an
// overlay, so near-gray colors still follow the page black↔text / white↔bg map.
// Saturated markup (PDF default red, yellow highlight) is left unchanged,
// matching object-level PDF dark mode which already keeps chroma.
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

    int chroma = std::max({r, g, b}) - std::min({r, g, b});
    if (chroma >= 40) {
        return color;
    }

    auto mapChannel = [](u8 value, u8 text, u8 background) -> u8 {
        int x = value * ((int)background - (int)text) + 128;
        x += x >> 8;
        return (u8)(text + (x >> 8));
    };
    return RGB(mapChannel(r, rt, rb), mapChannel(g, gt, gb), mapChannel(b, bt, bb));
}

static int GetEbookStampIconIndex(EbookAnnotation* annotation) {
    if (!annotation || annotation->type != AnnotationType::Stamp) {
        return -1;
    }
    const char* name = annotation->icon;
    if (str::IsEmpty(name)) {
        name = "Draft";
    }
    int idx = seqstrings::StrToIdxIS(gStampIcons, name);
    if (idx < 0) {
        idx = seqstrings::StrToIdxIS(gStampIcons, "Draft");
    }
    return idx;
}

static void PaintEbookTextMarker(WindowTab* tab, HDC hdc, Rect anchor, COLORREF color, const char* icon) {
    int size = std::max(8, std::min(anchor.dx, anchor.dy));
    Rect marker(anchor.x, anchor.y, size, size);
    u8 r, g, b;
    UnpackColor(color, r, g, b);
    COLORREF borderColor = MapEbookAnnotationColor(RGB(45, 45, 45));
    u8 borderR, borderG, borderB;
    UnpackColor(borderColor, borderR, borderG, borderB);
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // Match MuPDF's Text-annotation appearance: an opaque colored square
    // with a compact black pictogram, rather than a generic toolbar glyph.
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, r, g, b));
    Gdiplus::SolidBrush glyph(Gdiplus::Color(255, borderR, borderG, borderB));
    // Keep the EPUB outline lighter than the PDF appearance's stroked frame.
    Gdiplus::Pen pen(Gdiplus::Color(170, borderR, borderG, borderB), .75f);
    Gdiplus::Rect bounds(marker.x, marker.y, marker.dx - 1, marker.dy - 1);
    graphics.FillRectangle(&fill, bounds);
    graphics.DrawRectangle(&pen, bounds);

    float centerX = (float)marker.x + marker.dx / 2.f;
    float centerY = (float)marker.y + marker.dy / 2.f;
    // These are MuPDF's PDF Text-annotation pictograms from
    // source/pdf/annotation-icons.h, mapped from its 8x8 coordinate system
    // into the centered inner square of this GDI+ overlay marker.
    float iconSize = (float)std::max(8, size / 2);
    float iconX = centerX - iconSize / 2.f;
    float iconY = centerY - iconSize / 2.f;
    auto p = [&](float x, float y) { return Gdiplus::PointF(iconX + x * iconSize / 8.f, iconY + y * iconSize / 8.f); };
    if (str::EqI(icon, "Comment")) {
        Gdiplus::PointF bubble[] = {p(.09f, 0), p(0, .09f), p(0, 6),         p(6, 6),
                                    p(8, 8),    p(8, .08f), p(7.91f, -.01f), p(.1f, -.01f)};
        graphics.FillPolygon(&glyph, bubble, dimof(bubble));
    } else if (str::EqI(icon, "Help")) {
        Gdiplus::GraphicsPath helpPath;
        helpPath.StartFigure();
        helpPath.AddBezier(p(2.47f, 0), p(1.62f, 0), p(.99f, .26f), p(.59f, .66f));
        helpPath.AddBezier(p(.59f, .66f), p(.19f, 1.06f), p(.05f, 1.56f), p(0, 1.94f));
        helpPath.AddLine(p(0, 1.94f), p(1, 2.07f));
        helpPath.AddBezier(p(1, 2.07f), p(1.04f, 1.82f), p(1.12f, 1.57f), p(1.31f, 1.38f));
        helpPath.AddBezier(p(1.31f, 1.38f), p(1.5f, 1.19f), p(1.8f, 1), p(2.47f, 1));
        helpPath.AddBezier(p(2.47f, 1), p(3.13f, 1), p(3.49f, 1.16f), p(3.69f, 1.34f));
        helpPath.AddBezier(p(3.69f, 1.34f), p(3.89f, 1.52f), p(3.97f, 1.74f), p(3.97f, 2));
        helpPath.AddBezier(p(3.97f, 2), p(3.97f, 2.83f), p(3.63f, 3.06f), p(3.13f, 3.5f));
        helpPath.AddBezier(p(3.13f, 3.5f), p(2.63f, 3.94f), p(1.97f, 4.58f), p(1.97f, 5.75f));
        helpPath.AddLine(p(1.97f, 5.75f), p(1.97f, 6));
        helpPath.AddLine(p(1.97f, 6), p(2.97f, 6));
        helpPath.AddLine(p(2.97f, 6), p(2.97f, 5.75f));
        helpPath.AddBezier(p(2.97f, 5.75f), p(2.97f, 4.92f), p(3.28f, 4.69f), p(3.78f, 4.25f));
        helpPath.AddBezier(p(3.78f, 4.25f), p(4.28f, 3.81f), p(4.97f, 3.17f), p(4.97f, 2));
        helpPath.AddBezier(p(4.97f, 2), p(4.97f, 1.52f), p(4.8f, .98f), p(4.38f, .59f));
        helpPath.AddBezier(p(4.38f, .59f), p(3.95f, .2f), p(3.31f, 0), p(2.47f, 0));
        helpPath.CloseFigure();
        helpPath.AddRectangle(Gdiplus::RectF(p(1.97f, 7).X, p(1.97f, 7).Y, iconSize / 8.f, iconSize / 8.f));
        graphics.FillPath(&glyph, &helpPath);
    } else if (str::EqI(icon, "Key")) {
        Gdiplus::GraphicsPath keyPath;
        keyPath.SetFillMode(Gdiplus::FillModeAlternate);
        keyPath.StartFigure();
        keyPath.AddBezier(p(5.5f, 0), p(4.12f, 0), p(3, 1.12f), p(3, 2.5f));
        keyPath.AddBezier(p(3, 2.5f), p(3, 2.66f), p(3, 2.82f), p(3.03f, 2.97f));
        keyPath.AddLine(p(3.03f, 2.97f), p(0, 6));
        keyPath.AddLine(p(0, 6), p(0, 8));
        keyPath.AddLine(p(0, 8), p(3, 8));
        keyPath.AddLine(p(3, 8), p(3, 6));
        keyPath.AddLine(p(3, 6), p(5, 6));
        keyPath.AddLine(p(5, 6), p(5, 5));
        keyPath.AddLine(p(5, 5), p(5.03f, 4.97f));
        keyPath.AddBezier(p(5.03f, 4.97f), p(5.18f, 5), p(5.34f, 5), p(5.5f, 5));
        keyPath.AddBezier(p(5.5f, 5), p(6.88f, 5), p(8, 3.88f), p(8, 2.5f));
        keyPath.AddBezier(p(8, 2.5f), p(8, 1.12f), p(6.88f, 0), p(5.5f, 0));
        keyPath.CloseFigure();
        keyPath.StartFigure();
        keyPath.AddBezier(p(6, 1), p(6.55f, 1), p(7, 1.45f), p(7, 2));
        keyPath.AddBezier(p(7, 2), p(7, 2.55f), p(6.55f, 3), p(6, 3));
        keyPath.AddBezier(p(6, 3), p(5.45f, 3), p(5, 2.55f), p(5, 2));
        keyPath.AddBezier(p(5, 2), p(5, 1.45f), p(5.45f, 1), p(6, 1));
        keyPath.CloseFigure();
        graphics.FillPath(&glyph, &keyPath);
    } else if (str::EqI(icon, "Insert")) {
        Gdiplus::PointF triangle[] = {p(8, 5), p(4, 0), p(0, 5)};
        graphics.FillPolygon(&glyph, triangle, dimof(triangle));
    } else if (str::EqI(icon, "NewParagraph")) {
        Gdiplus::PointF triangle[] = {p(8, 8), p(4, 0), p(0, 8)};
        graphics.FillPolygon(&glyph, triangle, dimof(triangle));
    } else if (str::EqI(icon, "Paragraph")) {
        Gdiplus::GraphicsPath paragraphPath;
        paragraphPath.StartFigure();
        paragraphPath.AddLine(p(7, 0), p(2, 0));
        paragraphPath.AddBezier(p(2, 0), p(1, 0), p(0, 1), p(0, 2));
        paragraphPath.AddBezier(p(0, 2), p(0, 3), p(1, 4), p(2, 4));
        paragraphPath.AddLine(p(2, 4), p(3, 4));
        paragraphPath.AddLine(p(3, 4), p(3, 8));
        paragraphPath.AddLine(p(3, 8), p(4, 8));
        paragraphPath.AddLine(p(4, 8), p(4, 1));
        paragraphPath.AddLine(p(4, 1), p(5, 1));
        paragraphPath.AddLine(p(5, 1), p(5, 8));
        paragraphPath.AddLine(p(5, 8), p(6, 8));
        paragraphPath.AddLine(p(6, 8), p(6, 1));
        paragraphPath.AddLine(p(6, 1), p(7, 1));
        paragraphPath.CloseFigure();
        graphics.FillPath(&glyph, &paragraphPath);
    } else {
        // Note: four one-unit bars, matching MuPDF's icon_note path.
        for (int y = 0; y < 8; y += 2) {
            graphics.FillRectangle(&glyph, Gdiplus::RectF(iconX, iconY + y * iconSize / 8.f, iconSize, iconSize / 8.f));
        }
    }
}

static void PaintEbookPointAnnotation(WindowTab* tab, HDC hdc, Rect marker, EbookAnnotation* annotation) {
    AnnotationType type = annotation->type;
    COLORREF color = EbookAnnotationGetColor(annotation);
    if (type == AnnotationType::Text) {
        PaintEbookTextMarker(tab, hdc, marker, MapEbookAnnotationColor(color), EbookAnnotationGetIcon(annotation));
        return;
    }

    color = MapEbookAnnotationColor(color);
    u8 r, g, b;
    UnpackColor(color, r, g, b);
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float lineWidth = (float)std::max(1, DpiScale(tab->win->hwndFrame, 1));
    Gdiplus::Pen pen(Gdiplus::Color(255, r, g, b), lineWidth);
    pen.SetLineCap(Gdiplus::LineCapFlat, Gdiplus::LineCapFlat, Gdiplus::DashCapFlat);
    Gdiplus::Rect bounds(marker.x, marker.y, marker.dx - 1, marker.dy - 1);

    if (type == AnnotationType::FreeText) {
        // Match pdf_write_free_text_appearance(): its text is black by
        // default, and the fill exists only when a background color is set.
        // Free Text stores its text color on the annotation itself, just as
        // PDF's default appearance does. Do not replace it with the global
        // creation default while painting existing annotations.
        COLORREF textColor = annotation->color;
        textColor = MapEbookAnnotationColor(textColor);
        u8 tr, tg, tb;
        UnpackColor(textColor, tr, tg, tb);
        int opacity = gGlobalPrefs->annotations.freeTextOpacity;
        setMinMax(opacity, 0, 100);
        u8 alpha = (u8)(255 * opacity / 100);
        COLORREF background = 0;
        if (EbookAnnotationGetFreeTextBackground(annotation, &background)) {
            background = MapEbookAnnotationColor(background);
            u8 br, bgc, bb;
            UnpackColor(background, br, bgc, bb);
            Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, br, bgc, bb));
            graphics.FillRectangle(&brush, bounds);
        }
        float borderWidth = (float)EbookAnnotationGetFreeTextBorderWidth(annotation);
        borderWidth = (float)DpiScale(tab->win->hwndFrame, (int)borderWidth);
        if (borderWidth > 0) {
            Gdiplus::Pen border(Gdiplus::Color(alpha, tr, tg, tb), borderWidth);
            float half = borderWidth / 2.f;
            graphics.DrawRectangle(&border, (float)marker.x + half, (float)marker.y + half,
                                   (float)marker.dx - borderWidth, (float)marker.dy - borderWidth);
        }
        TempWStr text = ToWStrTemp(str::IsEmpty(annotation->note) ? "This is a text..." : annotation->note);
        int fontSize = EbookAnnotationGetFreeTextSize(annotation);
        const char* fontName = EbookAnnotationGetFreeTextFont(annotation);
        const WCHAR* family = str::Eq(fontName, "Cour")   ? L"Courier New"
                              : str::Eq(fontName, "TiRo") ? L"Times New Roman"
                                                          : L"Arial";
        Gdiplus::Font font(family, (float)DpiScale(tab->win->hwndFrame, fontSize), Gdiplus::FontStyleRegular,
                           Gdiplus::UnitPixel);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(alpha, tr, tg, tb));
        Gdiplus::RectF textBounds((float)marker.x + borderWidth, (float)marker.y + borderWidth,
                                  (float)marker.dx - borderWidth * 2, (float)marker.dy - borderWidth * 2);
        Gdiplus::StringFormat format;
        format.SetAlignment(EbookAnnotationGetFreeTextAlignment(annotation) == 1   ? Gdiplus::StringAlignmentCenter
                            : EbookAnnotationGetFreeTextAlignment(annotation) == 2 ? Gdiplus::StringAlignmentFar
                                                                                   : Gdiplus::StringAlignmentNear);
        graphics.DrawString(text, -1, &font, textBounds, &format, &textBrush);
        return;
    }
    if (type == AnnotationType::Stamp) {
        // Direct port of pdf_write_stamp_appearance_rubber().
        Gdiplus::GraphicsState state = graphics.Save();
        float cx = (float)marker.x + marker.dx / 2.f;
        float cy = (float)marker.y + marker.dy / 2.f;
        graphics.TranslateTransform(cx, cy);
        graphics.RotateTransform(.6f);
        graphics.TranslateTransform(-cx, -cy);
        float inset = std::max(2.f, lineWidth * 2.f);
        Gdiplus::RectF stamp((float)marker.x + inset, (float)marker.y + inset, (float)marker.dx - inset * 2,
                             (float)marker.dy - inset * 2);
        Gdiplus::Pen stampPen(Gdiplus::Color(255, r, g, b), lineWidth * 2.f);
        graphics.DrawRectangle(&stampPen, stamp);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, r, g, b));
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        auto drawStampLine = [&](const WCHAR* text, float y, float height) {
            // MuPDF measures against the 190x50 stamp appearance; y is from
            // its PDF bottom edge, hence the vertical conversion here.
            float top = (float)marker.y + (50.f - y - height) * marker.dy / 50.f;
            float h = height * marker.dy / 50.f;
            Gdiplus::Font font(L"Times New Roman", h, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::RectF line((float)marker.x, top, (float)marker.dx, h);
            graphics.DrawString(text, -1, &font, line, &format, &textBrush);
        };
        int stampIdx = GetEbookStampIconIndex(annotation);
        switch (stampIdx) {
            case 0:
                drawStampLine(L"APPROVED", 13, 30);
                break;
            case 1:
                drawStampLine(L"AS IS", 13, 30);
                break;
            case 2:
                drawStampLine(L"CONFIDENTIAL", 17, 20);
                break;
            case 3:
                drawStampLine(L"DEPARTMENTAL", 17, 20);
                break;
            case 4:
                drawStampLine(L"DRAFT", 13, 30);
                break;
            case 5:
                drawStampLine(L"EXPERIMENTAL", 17, 20);
                break;
            case 6:
                drawStampLine(L"EXPIRED", 13, 30);
                break;
            case 7:
                drawStampLine(L"FINAL", 13, 30);
                break;
            case 8:
                drawStampLine(L"FOR COMMENT", 17, 20);
                break;
            case 9:
                drawStampLine(L"FOR PUBLIC", 26, 18);
                drawStampLine(L"RELEASE", 8.5f, 18);
                break;
            case 10:
                drawStampLine(L"NOT APPROVED", 17, 20);
                break;
            case 11:
                drawStampLine(L"NOT FOR", 26, 18);
                drawStampLine(L"PUBLIC RELEASE", 8.5f, 18);
                break;
            case 12:
                drawStampLine(L"SOLD", 13, 30);
                break;
            case 13:
                drawStampLine(L"TOP SECRET", 14, 26);
                break;
            default:
                drawStampLine(L"DRAFT", 13, 30);
                break;
        }
        graphics.Restore(state);
        return;
    }
    if (type == AnnotationType::Caret) {
        // The PDF caret is a filled, curved 20x14 appearance rather than
        // two stroked segments (pdf_write_caret_appearance).
        float x = (float)marker.x + marker.dx / 2.f;
        float bottom = (float)marker.BR().y;
        float top = (float)marker.y;
        Gdiplus::GraphicsPath caret;
        caret.StartFigure();
        caret.AddBezier((float)marker.x, bottom, x, bottom, x, (top + bottom) / 2.f, x, top);
        caret.AddBezier(x, top, x, (top + bottom) / 2.f, x, bottom, (float)marker.BR().x, bottom);
        caret.CloseFigure();
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, r, g, b));
        graphics.FillPath(&brush, &caret);
        return;
    }
    if (type == AnnotationType::Line) {
        float width = (float)DpiScale(tab->win->hwndFrame, EbookAnnotationGetBorderWidth(annotation));
        Gdiplus::Pen linePen(Gdiplus::Color(255, r, g, b), std::max(1.f, width));
        Gdiplus::PointF a, z;
        if (annotation->lineTLBR) {
            a = Gdiplus::PointF((float)marker.x + 2, (float)marker.y + 2);
            z = Gdiplus::PointF((float)marker.BR().x - 2, (float)marker.BR().y - 2);
        } else {
            a = Gdiplus::PointF((float)marker.x + 2, (float)marker.BR().y - 2);
            z = Gdiplus::PointF((float)marker.BR().x - 2, (float)marker.y + 2);
        }
        graphics.DrawLine(&linePen, a, z);
        COLORREF interiorColor = 0;
        bool hasInterior = EbookAnnotationGetInteriorColor(annotation, &interiorColor);
        BYTE ir = GetRValue(interiorColor), ig = GetGValue(interiorColor), ib = GetBValue(interiorColor);
        Gdiplus::SolidBrush strokeBrush(Gdiplus::Color(255, r, g, b));
        Gdiplus::SolidBrush interiorBrush(Gdiplus::Color(255, ir, ig, ib));
        auto drawCap = [&](Gdiplus::PointF p, Gdiplus::PointF toward, int style) {
            if (style == 0) return;
            float dx = toward.X - p.X, dy = toward.Y - p.Y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len < .1f) return;
            dx /= len;
            dy /= len;
            float nx = -dy, ny = dx, capRadius = std::max(3.f, linePen.GetWidth() * 3.f);
            Gdiplus::PointF q1(p.X + dx * capRadius + nx * capRadius * .65f,
                               p.Y + dy * capRadius + ny * capRadius * .65f);
            Gdiplus::PointF q2(p.X + dx * capRadius - nx * capRadius * .65f,
                               p.Y + dy * capRadius - ny * capRadius * .65f);
            if (style == 1) {
                Gdiplus::RectF rect(p.X - capRadius, p.Y - capRadius, capRadius * 2, capRadius * 2);
                if (hasInterior) graphics.FillRectangle(&interiorBrush, rect);
                graphics.DrawRectangle(&linePen, rect);
            } else if (style == 2) {
                Gdiplus::RectF rect(p.X - capRadius, p.Y - capRadius, capRadius * 2, capRadius * 2);
                if (hasInterior) graphics.FillEllipse(&interiorBrush, rect);
                graphics.DrawEllipse(&linePen, rect);
            } else if (style == 3) {
                Gdiplus::PointF d[] = {
                    {p.X, p.Y - capRadius}, {p.X + capRadius, p.Y}, {p.X, p.Y + capRadius}, {p.X - capRadius, p.Y}};
                if (hasInterior) graphics.FillPolygon(&interiorBrush, d, dimof(d));
                graphics.DrawPolygon(&linePen, d, dimof(d));
            } else if (style == 4 || style == 5 || style == 7 || style == 8) {
                if (style == 7 || style == 8) {
                    dx = -dx;
                    dy = -dy;
                    q1 = {p.X + dx * capRadius + nx * capRadius * .65f, p.Y + dy * capRadius + ny * capRadius * .65f};
                    q2 = {p.X + dx * capRadius - nx * capRadius * .65f, p.Y + dy * capRadius - ny * capRadius * .65f};
                }
                Gdiplus::PointF arrow[] = {q1, p, q2};
                if (style == 5 || style == 8) {
                    if (hasInterior) graphics.FillPolygon(&interiorBrush, arrow, dimof(arrow));
                    graphics.DrawPolygon(&linePen, arrow, dimof(arrow));
                } else {
                    graphics.DrawLines(&linePen, arrow, dimof(arrow));
                }
            } else if (style == 6) {
                graphics.DrawLine(&linePen, p.X + nx * capRadius, p.Y + ny * capRadius, p.X - nx * capRadius,
                                  p.Y - ny * capRadius);
            } else if (style == 9) {
                float slashRadius = std::max(5.f, linePen.GetWidth() * 5.f);
                float angle = atan2f(dy, dx) - 30.f * (3.14159265358979323846f / 180.f);
                float sx = cosf(angle) * slashRadius, sy = sinf(angle) * slashRadius;
                graphics.DrawLine(&linePen, p.X + sx, p.Y + sy, p.X - sx, p.Y - sy);
            }
        };
        drawCap(a, z, EbookAnnotationGetLineStart(annotation));
        drawCap(z, a, EbookAnnotationGetLineEnd(annotation));
        return;
    }
    if (type == AnnotationType::Square) {
        // Match pdf_write_square_appearance(): the rectangle is filled only
        // when an interior color exists, and the border's center stays inside
        // the annotation rectangle by half of its stroke width.
        float width = (float)DpiScale(tab->win->hwndFrame, EbookAnnotationGetBorderWidth(annotation));
        if (width > 0.f) {
            Gdiplus::Pen squarePen(Gdiplus::Color(255, r, g, b), width);
            float half = width / 2.f;
            Gdiplus::RectF square((float)marker.x + half, (float)marker.y + half,
                                  std::max(1.f, (float)marker.dx - width), std::max(1.f, (float)marker.dy - width));
            COLORREF interior = 0;
            if (EbookAnnotationGetInteriorColor(annotation, &interior)) {
                interior = MapEbookAnnotationColor(interior);
                u8 ir, ig, ib;
                UnpackColor(interior, ir, ig, ib);
                Gdiplus::SolidBrush fill(Gdiplus::Color(255, ir, ig, ib));
                graphics.FillRectangle(&fill, square);
            }
            graphics.DrawRectangle(&squarePen, square);
        }
        return;
    }
    if (type == AnnotationType::Circle) {
        // Match pdf_write_circle_appearance(): center the stroke inside the
        // annotation rectangle and paint only if an interior color is set.
        float width = (float)DpiScale(tab->win->hwndFrame, EbookAnnotationGetBorderWidth(annotation));
        if (width > 0.f) {
            Gdiplus::Pen circlePen(Gdiplus::Color(255, r, g, b), width);
            float half = width / 2.f;
            Gdiplus::RectF circle((float)marker.x + half, (float)marker.y + half,
                                  std::max(1.f, (float)marker.dx - width), std::max(1.f, (float)marker.dy - width));
            COLORREF interior = 0;
            if (EbookAnnotationGetInteriorColor(annotation, &interior)) {
                interior = MapEbookAnnotationColor(interior);
                u8 ir, ig, ib;
                UnpackColor(interior, ir, ig, ib);
                Gdiplus::SolidBrush fill(Gdiplus::Color(255, ir, ig, ib));
                graphics.FillEllipse(&fill, circle);
            }
            graphics.DrawEllipse(&circlePen, circle);
        }
    }
}

static void PaintEbookInkStroke(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo, EbookAnnotation* annotation) {
    if (!tab || !dm || !annotation || annotation->inkPoints.len < 2) {
        return;
    }
    COLORREF color = MapEbookAnnotationColor(EbookAnnotationGetColor(annotation));
    u8 r, g, b;
    UnpackColor(color, r, g, b);
    float width = (float)DpiScale(tab->win->hwndFrame, EbookAnnotationGetBorderWidth(annotation));
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(255, r, g, b), std::max(1.f, width));
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    Point prevScreen = dm->CvtToScreen(pageNo, annotation->inkPoints.at(0));
    for (size_t i = 1; i < annotation->inkPoints.len; i++) {
        Point curScreen = dm->CvtToScreen(pageNo, annotation->inkPoints.at(i));
        graphics.DrawLine(&pen, prevScreen.x, prevScreen.y, curScreen.x, curScreen.y);
        prevScreen = curScreen;
    }
}

static void PaintEbookPointAnnotationSelection(HDC hdc, Rect marker, AnnotationType type) {
    marker.Inflate(type == AnnotationType::Text ? 1 : 4, type == AnnotationType::Text ? 1 : 4);
    Gdiplus::Graphics graphics(hdc);
    Gdiplus::Pen pen(Gdiplus::Color(255, 0, 80, 200), 2.f);
    pen.SetDashStyle(Gdiplus::DashStyleDot);
    graphics.DrawRectangle(&pen, marker.x, marker.y, marker.dx, marker.dy);
    if (type == AnnotationType::Text || type == AnnotationType::Caret) {
        return;
    }
    constexpr int handleSize = 6;
    constexpr int half = handleSize / 2;
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::Pen handlePen(Gdiplus::Color(255, 0, 0, 0), 1.f);
    int xs[] = {marker.x - half, marker.x + marker.dx / 2 - half, marker.BR().x - half};
    int ys[] = {marker.y - half, marker.y + marker.dy / 2 - half, marker.BR().y - half};
    for (int x : xs) {
        for (int y : ys) {
            bool isCenter = x == xs[1] && y == ys[1];
            if (isCenter) {
                continue;
            }
            graphics.FillRectangle(&brush, x, y, handleSize, handleSize);
            graphics.DrawRectangle(&handlePen, x, y, handleSize, handleSize);
        }
    }
}

static void PaintEbookMarkup(WindowTab* tab, HDC hdc, Vec<Rect>& screenRects, EbookAnnotation* annotation) {
    if (screenRects.empty()) {
        return;
    }
    if (IsEbookPointAnnotationType(annotation->type)) {
        PaintEbookPointAnnotation(tab, hdc, screenRects.at(0), annotation);
        return;
    }
    PaintTextMarkupOverlay(hdc, tab->win->canvasRc, annotation->type, EbookAnnotationGetColor(annotation), screenRects,
                           EbookAnnotationGetOpacity(annotation));
}

// Match MuPDF/Acrobat markup appearance (pdf-appearance.c):
// highlight: Multiply blend
// underline: line at 1/7 of height from bottom, thickness h/16
// strike-out: line at 3/7 of height from bottom, thickness h/16
// squiggly: zigzag from baseline with amplitude h/7, step h/7, thickness h/16
void PaintTextMarkupOverlay(HDC hdc, Rect canvasRc, AnnotationType type, COLORREF color, Vec<Rect>& screenRects,
                            int opacity) {
    if (screenRects.empty()) {
        return;
    }
    if (type == AnnotationType::Highlight) {
        PaintMultiplyRectangles(hdc, canvasRc, screenRects, color, opacity);
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
        if (annotation->type == AnnotationType::Ink) {
            if (annotation->inkPoints.len >= 2) {
                RectF bounds;
                if (!GetPointAnnotationPageBounds(annotations, engine, annotation, pageNo, &bounds)) {
                    continue;
                }
                PaintEbookInkStroke(tab, hdc, dm, pageNo, annotation);
                if (tab->editEbookAnnotsWindow && tab->selectedEbookAnnotation == annotation) {
                    Rect screenRect = dm->CvtToScreen(pageNo, bounds);
                    PaintEbookPointAnnotationSelection(hdc, screenRect, annotation->type);
                }
            }
            continue;
        }
        if (IsEbookPointAnnotationType(annotation->type)) {
            RectF bounds;
            if (GetPointAnnotationPageBounds(annotations, engine, annotation, pageNo, &bounds)) {
                Vec<Rect> screenRects;
                Rect screenRect = dm->CvtToScreen(pageNo, bounds);
                if (!screenRect.IsEmpty()) {
                    screenRects.Append(screenRect);
                    PaintEbookMarkup(tab, hdc, screenRects, annotation);
                    if (tab->editEbookAnnotsWindow && tab->selectedEbookAnnotation == annotation) {
                        PaintEbookPointAnnotationSelection(hdc, screenRect, annotation->type);
                    }
                }
            }
            continue;
        }
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
