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
#include "AnnotationNotesExport.h"
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
    bool borderColorExplicit = false;
    COLORREF borderColor = RGB(0, 0, 0);
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
    // Stroke lengths. Empty means inkPoints is one stroke (older files).
    Vec<int> inkCounts;
    // File name only, under the annotations directory. Not a path.
    char* imageFile = nullptr;
    Gdiplus::Bitmap* imageBmp = nullptr;
    bool imageLoadFailed = false;

    ~EbookAnnotation() {
        str::Free(exact);
        str::Free(prefix);
        str::Free(suffix);
        str::Free(note);
        str::Free(icon);
        str::Free(textFont);
        str::Free(author);
        str::Free(imageFile);
        delete imageBmp;
    }
};

struct EbookChapterTextCache {
    int chapter = -1;
    int startPage = 0;
    // Chapter-relative anchor offset at the start of each formatted page.
    Vec<int> pageStarts;
};

struct EbookAnnotations {
    bool lastSaveFailed = false;
    bool dirty = false;
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
            return {22, 22};
        case AnnotationType::FreeText: {
            int w = gGlobalPrefs->annotations.freeTextWidth;
            int h = gGlobalPrefs->annotations.freeTextHeight;
            if (w >= 8 && h >= 8) {
                return {(float)w, (float)h};
            }
            return {200, 100};
        }
        case AnnotationType::Stamp:
            return GetDefaultStampSize();
        case AnnotationType::Caret:
            return {22, 22};
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
        return "Comment";
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

static bool EbookImageFileNameOk(const char* name);

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
            } else if (str::Eq(property, "borderColor")) {
                u32 color = 0;
                str::Parse(value, "%u", &color);
                annotation->borderColor = (COLORREF)color;
                annotation->borderColorExplicit = true;
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
            } else if (str::StartsWith(property, "inkCounts/[")) {
                const char* idxStart = property + str::Len("inkCounts/[");
                int strokeIdx = 0;
                str::Parse(idxStart, "%d", &strokeIdx);
                int count = 0;
                str::Parse(value, "%d", &count);
                if (strokeIdx >= 0 && count > 0) {
                    while ((int)annotation->inkCounts.len <= strokeIdx) {
                        annotation->inkCounts.Append(0);
                    }
                    annotation->inkCounts.at(strokeIdx) = count;
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
            } else if (str::Eq(property, "image")) {
                if (EbookImageFileNameOk(value)) {
                    str::ReplaceWithCopy(&annotation->imageFile, value);
                }
            } else if (str::Eq(property, "textFont")) {
                str::ReplaceWithCopy(&annotation->textFont, value);
            } else if (str::Eq(property, "author")) {
                str::ReplaceWithCopy(&annotation->author, value);
            }
        }
        return true;
    }
};

static bool EbookImageFileNameOk(const char* name) {
    if (!name || !name[0] || str::Len(name) > 64 || !str::EndsWithI(name, ".png")) {
        return false;
    }
    for (const char* p = name; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                  c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static char* SaveEbookSignaturePng(const u8* png, int pngLen) {
    if (!png || pngLen < 8 || !dir::CreateAll(GetEbookAnnotationsDirTemp())) {
        return nullptr;
    }
    u8 digest[16]{};
    CalcMD5Digest(png, pngLen, digest);
    AutoFreeStr hex(str::MemToHex(digest, dimof(digest)));
    char* name = str::Join("sig-", hex, ".png");
    TempStr path = path::JoinTemp(GetEbookAnnotationsDirTemp(), name);
    if (!file::Exists(path) && !file::WriteFile(path, ByteSlice(png, (size_t)pngLen))) {
        str::Free(name);
        return nullptr;
    }
    return name;
}

static void ForgetEbookSignatureImage(EbookAnnotations* annotations, EbookAnnotation* annotation) {
    if (!annotations || !annotation || !EbookImageFileNameOk(annotation->imageFile)) {
        return;
    }
    for (size_t i = 0; i < annotations->items.size(); i++) {
        EbookAnnotation* other = annotations->items.at(i);
        if (other != annotation && other->imageFile && str::Eq(other->imageFile, annotation->imageFile)) {
            return;
        }
    }
    file::Delete(path::JoinTemp(GetEbookAnnotationsDirTemp(), annotation->imageFile));
}

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

// Mutations stay in memory until the user explicitly saves.
static bool SaveEbookAnnotations(EbookAnnotations* annotations) {
    if (!annotations) return false;
    annotations->dirty = true;
    return true;
}

static bool PersistEbookAnnotations(EbookAnnotations* annotations) {
    if (annotations) annotations->lastSaveFailed = true;
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
            if (annotation->borderColorExplicit) {
                out.AppendFmt(",\n      \"borderColor\": %u", (uint)annotation->borderColor);
            }
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
            if (annotation->inkCounts.len > 0) {
                out.Append(",\n      \"inkCounts\": [");
                for (size_t idx = 0; idx < annotation->inkCounts.len; idx++) {
                    if (idx != 0) {
                        out.Append(", ");
                    }
                    out.AppendFmt("%d", annotation->inkCounts.at(idx));
                }
                out.AppendChar(']');
            }
        }
        if (!str::IsEmpty(annotation->imageFile)) {
            out.Append(",\n      \"image\": ");
            AppendJsonString(out, annotation->imageFile);
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
    annotations->lastSaveFailed = false;
    annotations->dirty = false;
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

static int DrawStyleBorderPx(float points) {
    if (points < 0.5f) {
        return 0;
    }
    return std::clamp((int)(points + 0.5f), 0, 12);
}

void AdoptEbookDrawStyle(EbookAnnotation* annotation) {
    AnnotDrawStyle style;
    if (!annotation || !FindAnnotDrawStyle(annotation->type, &style)) {
        return;
    }
    AnnotationType type = annotation->type;
    if (style.flags & kDrawStyleColor) {
        annotation->color = style.color;
    }
    if (style.flags & kDrawStyleBorder) {
        annotation->borderWidth = DrawStyleBorderPx(style.border);
    }
    if (style.flags & kDrawStyleOpacity) {
        annotation->opacity = std::clamp(style.opacityPercent, 0, 100);
    }
    if ((style.flags & kDrawStyleInterior) &&
        (type == AnnotationType::Line || type == AnnotationType::Square || type == AnnotationType::Circle)) {
        annotation->interiorTransparent = style.interiorTransparent;
        if (!style.interiorTransparent) {
            annotation->interiorColor = style.interior;
        }
    }
    if ((style.flags & kDrawStyleLineEnds) && type == AnnotationType::Line) {
        annotation->lineStart = style.lineStart;
        annotation->lineEnd = style.lineEnd;
    }
    if ((style.flags & kDrawStyleIcon) && style.icon[0] &&
        (type == AnnotationType::Text || type == AnnotationType::Stamp)) {
        str::ReplaceWithCopy(&annotation->icon, style.icon);
    }
    if (type == AnnotationType::FreeText) {
        if ((style.flags & kDrawStyleFont) && style.font[0]) {
            str::ReplaceWithCopy(&annotation->textFont, style.font);
        }
        if (style.flags & kDrawStyleTextSize) {
            annotation->textSize = std::clamp(style.textSize, 5, 128);
        }
        if (style.flags & kDrawStyleAlign) {
            annotation->textAlignment = std::clamp(style.align, 0, 2);
        }
        if (style.flags & kDrawStyleBorderColor) {
            annotation->borderColorExplicit = true;
            annotation->borderColor = style.borderColor;
        }
        if (style.flags & kDrawStyleBackground) {
            annotation->backgroundTransparent = style.backgroundTransparent;
            if (!style.backgroundTransparent) {
                annotation->backgroundColor = style.background;
            }
        }
    }
}

void RememberEbookDrawStyle(EbookAnnotation* annotation) {
    if (!annotation) {
        return;
    }
    AnnotDrawStyle style{};
    style.type = annotation->type;
    if (!IsSpecialColor(annotation->color)) {
        style.color = annotation->color;
        style.flags |= kDrawStyleColor;
    }
    if (annotation->type == AnnotationType::FreeText || annotation->type == AnnotationType::Line ||
        annotation->type == AnnotationType::Square || annotation->type == AnnotationType::Circle ||
        annotation->type == AnnotationType::Ink) {
        style.border = (float)annotation->borderWidth;
        style.flags |= kDrawStyleBorder;
    }
    if (annotation->type == AnnotationType::Highlight || annotation->type == AnnotationType::FreeText) {
        style.opacityPercent = std::clamp(annotation->opacity, 0, 100);
        style.flags |= kDrawStyleOpacity;
    }
    if (annotation->type == AnnotationType::Line || annotation->type == AnnotationType::Square ||
        annotation->type == AnnotationType::Circle) {
        style.interiorTransparent = annotation->interiorTransparent;
        style.interior = annotation->interiorColor;
        style.flags |= kDrawStyleInterior;
    }
    if (annotation->type == AnnotationType::Line) {
        style.lineStart = annotation->lineStart;
        style.lineEnd = annotation->lineEnd;
        style.flags |= kDrawStyleLineEnds;
    }
    if ((annotation->type == AnnotationType::Text || annotation->type == AnnotationType::Stamp) &&
        !str::IsEmpty(annotation->icon)) {
        str::BufSet(style.icon, dimof(style.icon), annotation->icon);
        style.flags |= kDrawStyleIcon;
    }
    if (annotation->type == AnnotationType::FreeText) {
        if (!str::IsEmpty(annotation->textFont)) {
            str::BufSet(style.font, dimof(style.font), annotation->textFont);
            style.flags |= kDrawStyleFont;
        }
        style.textSize = annotation->textSize;
        style.flags |= kDrawStyleTextSize;
        style.align = annotation->textAlignment;
        style.flags |= kDrawStyleAlign;
        style.borderColor = annotation->borderColor;
        style.flags |= kDrawStyleBorderColor;
        style.backgroundTransparent = annotation->backgroundTransparent;
        style.background = annotation->backgroundColor;
        style.flags |= kDrawStyleBackground;
    }
    if (style.flags) {
        SaveAnnotDrawStyle(style);
        if (annotation->type == AnnotationType::Stamp && style.icon[0]) {
            RememberStampIconName(style.icon);
        }
    }
}

static void TakeEbookDrawStyle(EbookAnnotation* annotation) {
    AdoptEbookDrawStyle(annotation);
    RememberEbookDrawStyle(annotation);
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
    TakeEbookDrawStyle(annotation);
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
        annotation->textFont = str::Dup(FreeTextPresetFont());
        annotation->textSize = std::max(5, gGlobalPrefs->annotations.freeTextSize);
        annotation->borderWidth = std::max(0, gGlobalPrefs->annotations.freeTextBorderWidth);
        annotation->borderColorExplicit = true;
        annotation->borderColor = RGB(0, 0, 0);
        auto& background = gGlobalPrefs->annotations.freeTextBackgroundColorParsed;
        if (background.parsedOk) {
            annotation->backgroundTransparent = false;
            annotation->backgroundColor = background.col;
        }
    } else if (type == AnnotationType::Text) {
        annotation->icon = str::Dup(GetDefaultEbookTextIconTemp());
    } else if (type == AnnotationType::Stamp) {
        annotation->icon = str::Dup(DefaultStampIconName());
    }
    annotation->exact = str::Dup(ExtractContextTemp(engine, pageNo, glyph, std::min(glyph + 1, textLen)));
    constexpr int kContextChars = 32;
    annotation->prefix = str::Dup(ExtractContextTemp(engine, pageNo, glyph - kContextChars, glyph));
    annotation->suffix = str::Dup(ExtractContextTemp(engine, pageNo, glyph + 1, glyph + 1 + kContextChars));
    InitEbookAnnotationMetadata(annotation);
    TakeEbookDrawStyle(annotation);
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
    TakeEbookDrawStyle(annotation);
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
    TakeEbookDrawStyle(annotation);
    TouchEbookAnnotationModified(annotation);
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !SaveEbookAnnotations(annotations)) {
        return nullptr;
    }
    return annotation;
}

static EbookAnnotation* CreateEbookAnnotationOnPage(WindowTab* tab, DisplayModel* dm, int pageNo, RectF pageRect,
                                                    AnnotationType type, COLORREF color) {
    PointF candidates[5] = {
        {pageRect.x + pageRect.dx * 0.5f, pageRect.y + pageRect.dy * 0.5f},
        {pageRect.x + 1.f, pageRect.y + 1.f},
        {pageRect.x + std::max(1.f, pageRect.dx - 1.f), pageRect.y + 1.f},
        {pageRect.x + 1.f, pageRect.y + std::max(1.f, pageRect.dy - 1.f)},
        {pageRect.x + std::max(1.f, pageRect.dx - 1.f), pageRect.y + std::max(1.f, pageRect.dy - 1.f)},
    };
    for (PointF pagePoint : candidates) {
        Point screen = dm->CvtToScreen(pageNo, pagePoint);
        EbookAnnotation* annotation = EbookAnnotationsCreateAt(tab, dm, screen, type, color);
        if (annotation) {
            return annotation;
        }
    }
    return nullptr;
}

EbookAnnotation* EbookAnnotationsCreateInkStrokes(WindowTab* tab, DisplayModel* dm, int pageNo, PointF* points,
                                                  const int* counts, int nStrokes, COLORREF color, int borderWidth) {
    if (!tab || !dm || !points || !counts || nStrokes < 1 || !dm->ValidPageNo(pageNo)) {
        return nullptr;
    }
    int total = 0;
    float minX = 0;
    float minY = 0;
    float maxX = 0;
    float maxY = 0;
    bool any = false;
    int src = 0;
    for (int i = 0; i < nStrokes; i++) {
        int n = counts[i];
        if (n >= 2) {
            for (int k = 0; k < n; k++) {
                PointF pt = points[src + k];
                if (!any) {
                    minX = maxX = pt.x;
                    minY = maxY = pt.y;
                    any = true;
                } else {
                    minX = std::min(minX, pt.x);
                    minY = std::min(minY, pt.y);
                    maxX = std::max(maxX, pt.x);
                    maxY = std::max(maxY, pt.y);
                }
            }
            total += n;
        }
        if (n > 0) {
            src += n;
        }
    }
    if (!any || total < 2) {
        return nullptr;
    }
    RectF box{minX, minY, std::max(1.f, maxX - minX), std::max(1.f, maxY - minY)};
    EbookAnnotation* annotation = CreateEbookAnnotationOnPage(tab, dm, pageNo, box, AnnotationType::Ink, color);
    if (!annotation) {
        return nullptr;
    }
    annotation->inkPoints.Reset();
    annotation->inkCounts.Reset();
    src = 0;
    for (int i = 0; i < nStrokes; i++) {
        int n = counts[i];
        if (n >= 2) {
            for (int k = 0; k < n; k++) {
                annotation->inkPoints.Append(points[src + k]);
            }
            annotation->inkCounts.Append(n);
        }
        if (n > 0) {
            src += n;
        }
    }
    annotation->borderWidth = borderWidth > 0 ? borderWidth : 1;
    TouchEbookAnnotationModified(annotation);
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !SaveEbookAnnotations(annotations)) {
        EbookAnnotationsDelete(tab, annotation);
        return nullptr;
    }
    return annotation;
}

EbookAnnotation* EbookAnnotationsCreateSignatureImage(WindowTab* tab, DisplayModel* dm, int pageNo, RectF pageRect,
                                                      const u8* png, int pngLen) {
    if (!tab || !dm || !png || pngLen < 8 || !dm->ValidPageNo(pageNo) || pageRect.dx < 1.f || pageRect.dy < 1.f) {
        return nullptr;
    }
    EbookAnnotation* annotation =
        CreateEbookAnnotationOnPage(tab, dm, pageNo, pageRect, AnnotationType::Stamp, RGB(0, 0, 0));
    if (!annotation) {
        return nullptr;
    }
    char* name = SaveEbookSignaturePng(png, pngLen);
    if (!name) {
        EbookAnnotationsDelete(tab, annotation);
        return nullptr;
    }
    str::ReplaceWithCopy(&annotation->imageFile, name);
    str::Free(name);
    if (!EbookAnnotationSetPageBounds(tab, dm, annotation, pageNo, pageRect, true)) {
        EbookAnnotationsDelete(tab, annotation);
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
    // Joined stext lines (a wrapped CJK paragraph) have no zero-width break.
    // One box for the whole run makes underline, strikeout, and squiggly use
    // the paragraph height: one thick stroke on the last line only.
    bool geoSplit = !PageHasVerticalGlyphLayout(engine, pageNo);
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
        Rect band = *c;
        while (c < end && (c->x || c->dx)) {
            if (geoSplit && c != lineStart && GlyphJumpsToNextBandLine(band, *c)) {
                break;
            }
            band = band.Union(*c);
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
    // Ink is stored as page points. A move used to write offset/width, which
    // the stroke does not read, so the line snapped back on mouse-up.
    if (save && annotation->type == AnnotationType::Ink && annotation->inkPoints.len > 0) {
        RectF cur;
        if (!GetPointAnnotationPageBounds(annotations, dm->GetEngine(), annotation, pageNo, &cur)) {
            return false;
        }
        float dx = bounds.x - cur.x;
        float dy = bounds.y - cur.y;
        for (size_t i = 0; i < annotation->inkPoints.len; i++) {
            annotation->inkPoints.at(i).x += dx;
            annotation->inkPoints.at(i).y += dy;
        }
        TouchEbookAnnotationModified(annotation);
        return SaveEbookAnnotations(annotations);
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
    ForgetEbookSignatureImage(annotations, annotation);
    delete annotation;
    return true;
}

bool EbookAnnotationsDelete(WindowTab* tab, EbookAnnotation* annotation) {
    EndFreeTextInPlaceEditForTab(tab, false);
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
    ForgetEbookSignatureImage(annotations, annotation);
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

int EbookAnnotationGetChapter(EbookAnnotation* annotation) {
    return annotation ? annotation->chapter : -1;
}

bool EbookAnnotationsLastSaveFailed(WindowTab* tab) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    return annotations && annotations->lastSaveFailed;
}

bool EbookAnnotationsRetrySave(WindowTab* tab) {
    return PersistEbookAnnotations(EnsureEbookAnnotations(tab));
}

bool EbookAnnotationsHasUnsavedChanges(WindowTab* tab) {
    auto annotations = tab ? tab->ebookAnnotations : nullptr;
    return annotations && annotations->dirty;
}

bool EbookAnnotationsSaveCopy(WindowTab* tab, HWND parent) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !PersistEbookAnnotations(annotations)) return false;
    auto data = file::ReadFile(annotations->storagePath);
    if (data.empty()) return false;
    TempStr destination = path::JoinTemp(
        path::GetDirTemp(tab->filePath),
        str::JoinTemp(path::GetBaseNameTemp(path::GetPathNoExtTemp(tab->filePath)), "-annotations.json"));
    bool saved = SaveDataToFile(parent, destination, data);
    str::Free(data.data());
    return saved;
}

const char* EbookAnnotationGetText(EbookAnnotation* annotation) {
    return annotation ? annotation->exact : nullptr;
}

TempStr EbookAnnotationExcerptTemp(WindowTab* tab, EbookAnnotation* annotation) {
    if (!annotation) return nullptr;
    AnnotationType type = EbookAnnotationGetType(annotation);
    TempStr excerpt = str::DupTemp(EbookAnnotationGetText(annotation));
    if (type == AnnotationType::FreeText) {
        excerpt = str::DupTemp(EbookAnnotationGetNote(annotation));
    }
    if (type == AnnotationType::Square || type == AnnotationType::Circle || type == AnnotationType::Line ||
        type == AnnotationType::Ink) {
        // Shape exact text is a reflow anchor, never the displayed excerpt.
        excerpt = nullptr;
        int pageNo = 0;
        RectF bounds{};
        auto dm = tab ? tab->AsFixed() : nullptr;
        if (dm && EbookAnnotationGetPageBounds(tab, dm, annotation, &pageNo, &bounds)) {
            if (type == AnnotationType::Square || type == AnnotationType::Circle) {
                char* regionText = dm->GetTextInRegion(pageNo, bounds, true);
                excerpt = str::DupTemp(regionText);
                str::Free(regionText);
            }
            if (str::IsEmptyOrWhiteSpace(excerpt)) {
                excerpt = str::FormatTemp("x=%d y=%d dx=%d dy=%d", (int)bounds.x, (int)bounds.y, (int)bounds.dx,
                                          (int)bounds.dy);
            }
        }
    }
    return excerpt;
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
    return annotation->type == AnnotationType::Stamp ? "Final" : "Comment";
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
        return GetDefaultAnnotationColor(AnnotationType::Text);
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

bool EbookAnnotationSetAuthor(WindowTab* tab, EbookAnnotation* annotation, const char* author) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !annotation || annotations->items.Find(annotation) < 0) {
        return false;
    }
    const char* next = str::IsEmptyOrWhiteSpace(author) ? nullptr : author;
    if (str::Eq(annotation->author, next) || (str::IsEmpty(annotation->author) && !next)) {
        return true;
    }
    AutoFreeStr previous(annotation->author);
    annotation->author = next ? str::Dup(next) : nullptr;
    TouchEbookAnnotationModified(annotation);
    if (!SaveEbookAnnotations(annotations)) {
        annotation->author = previous.StealData();
        return false;
    }
    return true;
}

bool EbookAnnotationSetNote(WindowTab* tab, EbookAnnotation* annotation, const char* note) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || !annotation || annotations->items.Find(annotation) < 0) {
        return false;
    }
    // Saving and closing both flush the editor. An unchanged flush must not
    // advance the modification time or make a saved document dirty again.
    const char* oldNote = annotation->note ? annotation->note : "";
    const char* newNote = note ? note : "";
    TempStr normalizedOld = str::ReplaceTemp(oldNote, "\r\n", "\n");
    TempStr normalizedNew = str::ReplaceTemp(newNote, "\r\n", "\n");
    if (str::Eq(normalizedOld, normalizedNew)) return true;
    AutoFreeStr previous(annotation->note);
    annotation->note = str::Dup(normalizedNew);
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
    if (annotation->type == AnnotationType::Stamp) {
        RememberStampIconName(normalized);
    }
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

static bool SaveEbookFreeTextProperties(WindowTab* tab, EbookAnnotation* annotation);

COLORREF EbookAnnotationGetFreeTextBorderColor(EbookAnnotation* annotation) {
    if (!annotation) {
        return RGB(0, 0, 0);
    }
    if (annotation->borderColorExplicit) {
        return annotation->borderColor;
    }
    return annotation->color == kColorUnset ? RGB(0, 0, 0) : annotation->color;
}

bool EbookAnnotationSetFreeTextBorderColor(WindowTab* tab, EbookAnnotation* annotation, COLORREF color) {
    if (!annotation || annotation->type != AnnotationType::FreeText) {
        return false;
    }
    annotation->borderColorExplicit = true;
    annotation->borderColor = color;
    return SaveEbookFreeTextProperties(tab, annotation);
}

bool EbookAnnotationSetColor(WindowTab* tab, EbookAnnotation* annotation, COLORREF color) {
    EbookAnnotations* annotations = EnsureEbookAnnotations(tab);
    if (!annotations || annotations->items.Find(annotation) < 0) {
        return false;
    }
    if (annotation->type == AnnotationType::FreeText && !annotation->borderColorExplicit &&
        annotation->color != color) {
        annotation->borderColor = annotation->color == kColorUnset ? RGB(0, 0, 0) : annotation->color;
        annotation->borderColorExplicit = true;
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
        int chapterA = EbookAnnotationGetChapter(a.annotation);
        int chapterB = EbookAnnotationGetChapter(b.annotation);
        if (chapterA != chapterB) return chapterA < chapterB;
        if (a.pageNo != b.pageNo) {
            return a.pageNo < b.pageNo;
        }
        return a.sourceStart < b.sourceStart;
    });

    out.Append(UTF8_BOM);
    out.AppendFmt("# %s\n\n", tab->GetTabTitle());
    out.Append("<details>\n");
    out.AppendFmt("<summary>%s</summary>\n\n", _TRA("Source"));
    out.AppendFmt("%s: %s\n\n", _TRA("Source"), tab->filePath);
    out.Append(_TRA("Exported:"));
    out.Append(" ");
    AppendUtcDateTime(out, time(nullptr));
    out.Append("\n\n</details>\n\n");

    int group = -1;
    int number = 0;
    for (const EbookAnnotationSortItem& item : items) {
        EbookAnnotation* annotation = item.annotation;
        int chapter = EbookAnnotationGetChapter(annotation) + 1;
        if (chapter != group) {
            group = chapter;
            number = 0;
            out.AppendFmt("## §%d\n\n", chapter);
        }
        const char* typeName = trans::GetTranslation(AnnotationReadableNameTemp(EbookAnnotationGetType(annotation)));
        const char* excerpt = EbookAnnotationExcerptTemp(tab, annotation);
        const char* note = EbookAnnotationGetNote(annotation);
        time_t date = EbookAnnotationGetModified(annotation);
        if (date <= 0) date = EbookAnnotationGetCreated(annotation);
        bool geometry = excerpt && str::StartsWith(excerpt, "x=") &&
                        (annotation->type == AnnotationType::Square || annotation->type == AnnotationType::Circle ||
                         annotation->type == AnnotationType::Line || annotation->type == AnnotationType::Ink);
        AppendReadingNoteMarkdown(out, ++number, typeName, excerpt, note, EbookAnnotationGetAuthor(annotation), date,
                                  geometry, item.pageNo);
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

// A handwritten-signature PNG is black ink on transparency. On a dark page it
// uses the same text↔paper map as the ink strokes.
static bool EbookSignatureInkShouldFollowPage(COLORREF& textColor, COLORREF& bgColor) {
    PdfDocumentColorMode docMode = GetPdfDocumentColorMode();
    bool pageUsesThemeColors =
        docMode != PdfDocumentColorMode::Light && (ThemeUsesDarkChrome() || !ThemeUsesOriginalPageColors());
    if (!pageUsesThemeColors) {
        return false;
    }
    textColor = ThemePageRenderColors(bgColor, true);
    u8 tr, tg, tb, br, bgg, bb;
    UnpackColor(textColor, tr, tg, tb);
    UnpackColor(bgColor, br, bgg, bb);
    return (int)tr + (int)tg + (int)tb > (int)br + (int)bgg + (int)bb;
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
        name = "Final";
    }
    int idx = seqstrings::StrToIdxIS(gStampIcons, name);
    if (idx < 0) {
        idx = seqstrings::StrToIdxIS(gStampIcons, "Final");
    }
    return idx;
}

static void PaintEbookTextMarker(WindowTab* tab, HDC hdc, Rect anchor, COLORREF color, const char* icon) {
    int size = std::max(8, std::min(anchor.dx, anchor.dy));
    Rect marker(anchor.x, anchor.y, size, size);
    u8 r, g, b;
    UnpackColor(color, r, g, b);
    int luma = (30 * (int)r + 59 * (int)g + 11 * (int)b) / 100;
    COLORREF glyphColor = luma < 140 ? RGB(255, 255, 255) : RGB(40, 40, 40);
    u8 gr, gg, gb;
    UnpackColor(glyphColor, gr, gg, gb);
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float side = (float)size;
    float radius = side * 0.2f;
    Gdiplus::GraphicsPath plate;
    float x0 = (float)marker.x;
    float y0 = (float)marker.y;
    float d = radius * 2.f;
    plate.AddArc(x0, y0, d, d, 180, 90);
    plate.AddArc(x0 + side - d, y0, d, d, 270, 90);
    plate.AddArc(x0 + side - d, y0 + side - d, d, d, 0, 90);
    plate.AddArc(x0, y0 + side - d, d, d, 90, 90);
    plate.CloseFigure();
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, r, g, b));
    graphics.FillPath(&fill, &plate);

    // Note border and pilcrow stems are about 0.85 in the 16-box. Line icons use that width.
    float glyphW = 0.85f;
    float sw = std::max(0.9f, side * (glyphW * 1.28f / 16.f));
    Gdiplus::Pen pen(Gdiplus::Color(255, gr, gg, gb), sw);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    // Same 1.28 scale as the PDF note appearance, about the center of the 16 box.
    constexpr float kGlyph = 1.28f;
    auto Su = [&](float v) { return 8.f + (v - 8.f) * kGlyph; };
    auto P = [&](float u, float v) {
        return Gdiplus::PointF(x0 + Su(u) * side / 16.f, y0 + (16.f - Su(v)) * side / 16.f);
    };
    // Same fit as pdf_begin_fitted_glyph: note page is height 10 on (8,8).
    // Stroked marks use 9.15 so the stroke's outer edge matches that box.
    struct GlyphFit {
        float s = 1.f;
        float cx = 8.f;
        float cy = 8.f;
    } fit;
    auto G = [&](float u, float v) { return P(fit.s * (u - fit.cx) + 8.f, fit.s * (v - fit.cy) + 8.f); };
    auto LineG = [&](float x1, float y1, float x2, float y2) { graphics.DrawLine(&pen, G(x1, y1), G(x2, y2)); };
    auto FitTo = [&](float ax, float ay, float bx, float by, bool stroked) {
        float maxd = std::max(bx - ax, by - ay);
        fit.s = (stroked ? 9.15f : 10.f) / maxd;
        fit.cx = (ax + bx) * 0.5f;
        fit.cy = (ay + by) * 0.5f;
    };
    if (str::EqI(icon, "Comment")) {
        FitTo(4.1f, 3.3f, 12.0f, 11.2f, true);
        Gdiplus::GraphicsPath bubble;
        Gdiplus::PointF bubbleAt = G(4.2f, 11.2f);
        float bx = bubbleAt.X;
        float by = bubbleAt.Y;
        float bw = 7.8f * fit.s * kGlyph * side / 16.f;
        float bh = 5.6f * fit.s * kGlyph * side / 16.f;
        float br = 1.45f * fit.s * kGlyph * side / 16.f;
        float bd = br * 2.f;
        bubble.AddArc(bx, by, bd, bd, 180, 90);
        bubble.AddArc(bx + bw - bd, by, bd, bd, 270, 90);
        bubble.AddArc(bx + bw - bd, by + bh - bd, bd, bd, 0, 90);
        bubble.AddArc(bx, by + bh - bd, bd, bd, 90, 90);
        bubble.CloseFigure();
        graphics.DrawPath(&pen, &bubble);
        LineG(5.5f, 5.6f, 4.1f, 3.3f);
        LineG(4.1f, 3.3f, 7.5f, 5.6f);
    } else if (str::EqI(icon, "Help")) {
        FitTo(6.0f, 5.2f, 10.5f, 13.0f, true);
        graphics.DrawBezier(&pen, G(6.0f, 10.7f), G(6.0f, 12.2f), G(6.9f, 13.0f), G(8.1f, 13.0f));
        graphics.DrawBezier(&pen, G(8.1f, 13.0f), G(9.5f, 13.0f), G(10.5f, 12.1f), G(10.5f, 10.7f));
        graphics.DrawBezier(&pen, G(10.5f, 10.7f), G(10.5f, 9.4f), G(9.2f, 8.9f), G(8.1f, 8.1f));
        LineG(8.1f, 8.1f, 8.1f, 7.3f);
        LineG(7.6f, 5.2f, 8.6f, 5.2f);
    } else if (str::EqI(icon, "Key")) {
        FitTo(4.2f, 4.5f, 12.45f, 12.65f, true);
        Gdiplus::PointF keyAt = G(8.35f, 12.65f);
        float keyD = 4.1f * fit.s * kGlyph * side / 16.f;
        graphics.DrawEllipse(&pen, keyAt.X, keyAt.Y, keyD, keyD);
        LineG(8.9f, 9.2f, 4.2f, 4.5f);
        LineG(5.6f, 5.9f, 7.0f, 4.5f);
    } else if (str::EqI(icon, "Insert")) {
        FitTo(4.6f, 3.4f, 11.4f, 12.6f, true);
        LineG(8.f, 3.4f, 8.f, 12.6f);
        LineG(4.6f, 12.6f, 11.4f, 12.6f);
        LineG(4.6f, 3.4f, 11.4f, 3.4f);
    } else if (str::EqI(icon, "NewParagraph")) {
        FitTo(4.3f, 3.8f, 10.7f, 11.8f, true);
        LineG(10.7f, 11.8f, 10.7f, 6.0f);
        LineG(10.7f, 6.0f, 4.3f, 6.0f);
        LineG(4.3f, 6.0f, 6.5f, 8.2f);
        LineG(4.3f, 6.0f, 6.5f, 3.8f);
    } else if (str::EqI(icon, "Caret")) {
        FitTo(4.0f, 3.8f, 12.0f, 12.6f, true);
        Gdiplus::GraphicsPath chevron;
        chevron.AddLine(G(4.0f, 3.8f), G(8.f, 12.6f));
        chevron.AddLine(G(8.f, 12.6f), G(12.0f, 3.8f));
        graphics.DrawPath(&pen, &chevron);
    } else if (str::EqI(icon, "Paragraph")) {
        FitTo(3.91f, 3.f, 12.09f, 13.f, false);
        Gdiplus::GraphicsPath pilcrow(Gdiplus::FillModeAlternate);
        pilcrow.StartFigure();
        pilcrow.AddBezier(G(6.64f, 13.f), G(5.14f, 13.f), G(3.91f, 11.77f), G(3.91f, 10.27f));
        pilcrow.AddBezier(G(3.91f, 10.27f), G(3.91f, 8.77f), G(5.14f, 7.55f), G(6.64f, 7.55f));
        pilcrow.AddLine(G(6.64f, 7.55f), G(8.45f, 7.55f));
        pilcrow.AddLine(G(8.45f, 7.55f), G(8.45f, 3.f));
        pilcrow.AddLine(G(8.45f, 3.f), G(9.36f, 3.f));
        pilcrow.AddLine(G(9.36f, 3.f), G(9.36f, 12.09f));
        pilcrow.AddLine(G(9.36f, 12.09f), G(10.27f, 12.09f));
        pilcrow.AddLine(G(10.27f, 12.09f), G(10.27f, 3.f));
        pilcrow.AddLine(G(10.27f, 3.f), G(11.18f, 3.f));
        pilcrow.AddLine(G(11.18f, 3.f), G(11.18f, 12.09f));
        pilcrow.AddLine(G(11.18f, 12.09f), G(12.09f, 12.09f));
        pilcrow.AddLine(G(12.09f, 12.09f), G(12.09f, 13.f));
        pilcrow.CloseFigure();
        pilcrow.StartFigure();
        pilcrow.AddLine(G(6.64f, 12.09f), G(8.45f, 12.09f));
        pilcrow.AddLine(G(8.45f, 12.09f), G(8.45f, 8.45f));
        pilcrow.AddLine(G(8.45f, 8.45f), G(6.64f, 8.45f));
        pilcrow.AddBezier(G(6.64f, 8.45f), G(5.63f, 8.45f), G(4.82f, 9.26f), G(4.82f, 10.27f));
        pilcrow.AddBezier(G(4.82f, 10.27f), G(4.82f, 11.28f), G(5.63f, 12.09f), G(6.64f, 12.09f));
        pilcrow.CloseFigure();
        Gdiplus::SolidBrush glyphBrush(Gdiplus::Color(255, gr, gg, gb));
        graphics.FillPath(&glyphBrush, &pilcrow);
    } else {
        FitTo(3.83f, 3.f, 12.17f, 13.f, false);
        Gdiplus::GraphicsPath page(Gdiplus::FillModeAlternate);
        page.StartFigure();
        page.AddLine(G(11.33f, 13.f), G(4.67f, 13.f));
        page.AddBezier(G(4.67f, 13.f), G(4.21f, 13.f), G(3.83f, 12.63f), G(3.83f, 12.17f));
        page.AddLine(G(3.83f, 12.17f), G(3.83f, 3.83f));
        page.AddBezier(G(3.83f, 3.83f), G(3.83f, 3.37f), G(4.21f, 3.f), G(4.67f, 3.f));
        page.AddLine(G(4.67f, 3.f), G(11.33f, 3.f));
        page.AddBezier(G(11.33f, 3.f), G(11.79f, 3.f), G(12.17f, 3.37f), G(12.17f, 3.83f));
        page.AddLine(G(12.17f, 3.83f), G(12.17f, 12.17f));
        page.AddBezier(G(12.17f, 12.17f), G(12.17f, 12.63f), G(11.79f, 13.f), G(11.33f, 13.f));
        page.CloseFigure();
        page.StartFigure();
        page.AddBezier(G(11.33f, 4.25f), G(11.33f, 4.02f), G(11.15f, 3.83f), G(10.92f, 3.83f));
        page.AddLine(G(10.92f, 3.83f), G(5.08f, 3.83f));
        page.AddBezier(G(5.08f, 3.83f), G(4.85f, 3.83f), G(4.67f, 4.02f), G(4.67f, 4.25f));
        page.AddLine(G(4.67f, 4.25f), G(4.67f, 11.75f));
        page.AddBezier(G(4.67f, 11.75f), G(4.67f, 11.98f), G(4.85f, 12.17f), G(5.08f, 12.17f));
        page.AddLine(G(5.08f, 12.17f), G(10.92f, 12.17f));
        page.AddBezier(G(10.92f, 12.17f), G(11.15f, 12.17f), G(11.33f, 11.98f), G(11.33f, 11.75f));
        page.AddLine(G(11.33f, 11.75f), G(11.33f, 4.25f));
        page.CloseFigure();
        auto bar = [&](float y, float yb, float yt) {
            page.StartFigure();
            page.AddBezier(G(10.08f, y), G(10.08f, y - 0.23f), G(9.90f, yb), G(9.67f, yb));
            page.AddLine(G(9.67f, yb), G(6.33f, yb));
            page.AddBezier(G(6.33f, yb), G(6.10f, yb), G(5.92f, y - 0.23f), G(5.92f, y));
            page.AddBezier(G(5.92f, y), G(5.92f, y + 0.23f), G(6.10f, yt), G(6.33f, yt));
            page.AddLine(G(6.33f, yt), G(9.67f, yt));
            page.AddBezier(G(9.67f, yt), G(9.90f, yt), G(10.08f, y + 0.23f), G(10.08f, y));
            page.CloseFigure();
        };
        bar(10.08f, 9.67f, 10.50f);
        bar(8.f, 7.58f, 8.42f);
        bar(5.92f, 5.50f, 6.33f);
        Gdiplus::SolidBrush glyphBrush(Gdiplus::Color(255, gr, gg, gb));
        graphics.FillPath(&glyphBrush, &page);
    }
}

// Use GDI's edit-control word wrapping and the same integer font height as
// the native in-place EDIT. GDI+ DrawString adds glyph padding and uses
// different advances, causing text to rewrap when entering/leaving editing.
// Blend a copy of the visible background to retain annotation opacity without
// allocating a bitmap as large as a potentially oversized annotation.
static void PaintEbookFreeText(HDC hdc, Rect content, const WCHAR* text, const WCHAR* face, int fontPx, COLORREF color,
                               int alignment, u8 alpha) {
    RECT clip;
    if (!alpha || GetClipBox(hdc, &clip) == ERROR) return;
    RECT bounds = ToRECT(content), visible;
    if (!IntersectRect(&visible, &bounds, &clip)) return;
    int width = visible.right - visible.left, height = visible.bottom - visible.top;
    HDC buffer = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
    HFONT font = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    if (!buffer || !bitmap || !font) {
        if (font) DeleteObject(font);
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap), oldFont = SelectObject(buffer, font);
    BitBlt(buffer, 0, 0, width, height, hdc, visible.left, visible.top, SRCCOPY);
    SetViewportOrgEx(buffer, -visible.left, -visible.top, nullptr);
    SetBkMode(buffer, TRANSPARENT);
    SetTextColor(buffer, color);
    UINT format = DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX;
    format |= alignment == 1 ? DT_CENTER : alignment == 2 ? DT_RIGHT : DT_LEFT;
    DrawTextW(buffer, text, -1, &bounds, format);
    SetViewportOrgEx(buffer, 0, 0, nullptr);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
    GdiAlphaBlend(hdc, visible.left, visible.top, width, height, buffer, 0, 0, width, height, blend);
    SelectObject(buffer, oldFont);
    SelectObject(buffer, oldBitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

static void PaintEbookPointAnnotation(WindowTab* tab, HDC hdc, Rect marker, EbookAnnotation* annotation) {
    AnnotationType type = annotation->type;
    COLORREF color = EbookAnnotationGetColor(annotation);
    if (type == AnnotationType::Text || type == AnnotationType::Caret) {
        const char* icon = type == AnnotationType::Caret ? "Caret" : EbookAnnotationGetIcon(annotation);
        PaintEbookTextMarker(tab, hdc, marker, MapEbookAnnotationColor(color), icon);
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
            COLORREF frame = EbookAnnotationGetFreeTextBorderColor(annotation);
            frame = MapEbookAnnotationColor(frame);
            u8 fr, fg, fb;
            UnpackColor(frame, fr, fg, fb);
            Gdiplus::Pen border(Gdiplus::Color(alpha, fr, fg, fb), borderWidth);
            float half = borderWidth / 2.f;
            graphics.DrawRectangle(&border, (float)marker.x + half, (float)marker.y + half,
                                   (float)marker.dx - borderWidth, (float)marker.dy - borderWidth);
        }
        TempWStr text = ToWStrTemp(str::IsEmpty(annotation->note) ? "This is a text..." : annotation->note);
        int fontSize = EbookAnnotationGetFreeTextSize(annotation);
        const WCHAR* family = FreeTextWindowsFace(EbookAnnotationGetFreeTextFont(annotation));
        int fontPx = std::max(6, DpiScale(tab->win->hwndFrame, fontSize));
        float pad = borderWidth + fontPx * 0.4f;
        float side = (float)std::min(marker.dx, marker.dy);
        if (pad > side * 0.45f) {
            pad = side * 0.45f;
        }
        int inset = (int)(pad + 0.5f);
        Rect textBounds(marker.x + inset, marker.y + inset, marker.dx - inset * 2, marker.dy - inset * 2);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        PaintEbookFreeText(hdc, textBounds, text, family, fontPx, textColor,
                           EbookAnnotationGetFreeTextAlignment(annotation), alpha);
        return;
    }
    if (type == AnnotationType::Stamp) {
        if (annotation->imageFile && !annotation->imageLoadFailed) {
            if (!annotation->imageBmp) {
                if (!EbookImageFileNameOk(annotation->imageFile)) {
                    annotation->imageLoadFailed = true;
                } else {
                    TempStr path = path::JoinTemp(GetEbookAnnotationsDirTemp(), annotation->imageFile);
                    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(ToWStrTemp(path));
                    if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
                        annotation->imageBmp = bmp;
                    } else {
                        delete bmp;
                        annotation->imageLoadFailed = true;
                    }
                }
            }
            if (annotation->imageBmp) {
                COLORREF textColor = 0;
                COLORREF bgColor = 0;
                if (EbookSignatureInkShouldFollowPage(textColor, bgColor)) {
                    u8 tr, tg, tb, br, bgg, bb;
                    UnpackColor(textColor, tr, tg, tb);
                    UnpackColor(bgColor, br, bgg, bb);
                    Gdiplus::ColorMatrix cm{};
                    cm.m[0][0] = ((int)br - (int)tr) / 255.f;
                    cm.m[1][1] = ((int)bgg - (int)tg) / 255.f;
                    cm.m[2][2] = ((int)bb - (int)tb) / 255.f;
                    cm.m[3][3] = 1.f;
                    cm.m[4][0] = tr / 255.f;
                    cm.m[4][1] = tg / 255.f;
                    cm.m[4][2] = tb / 255.f;
                    cm.m[4][4] = 1.f;
                    Gdiplus::ImageAttributes attr;
                    attr.SetColorMatrix(&cm);
                    graphics.DrawImage(
                        annotation->imageBmp,
                        Gdiplus::RectF((float)marker.x, (float)marker.y, (float)marker.dx, (float)marker.dy), 0.f, 0.f,
                        (float)annotation->imageBmp->GetWidth(), (float)annotation->imageBmp->GetHeight(),
                        Gdiplus::UnitPixel, &attr);
                } else {
                    graphics.DrawImage(annotation->imageBmp, (float)marker.x, (float)marker.y, (float)marker.dx,
                                       (float)marker.dy);
                }
                return;
            }
        }
        // Same art as pdf_write_stamp_appearance_rubber(): 190x50, border
        // 2,2,186x44, text on the PDF baseline. Horizontal — no 8° tilt.
        constexpr float kFitW = 190.f;
        constexpr float kFitH = 50.f;
        float scale = std::min((float)marker.dx / kFitW, (float)marker.dy / kFitH);
        if (scale < 0.05f) {
            scale = 0.05f;
        }
        float destW = kFitW * scale;
        float destH = kFitH * scale;
        float destX = (float)marker.x + ((float)marker.dx - destW) / 2.f;
        float destY = (float)marker.y + ((float)marker.dy - destH) / 2.f;
        Gdiplus::Matrix xform(scale, 0.f, 0.f, scale, destX, destY);
        struct StampLine {
            const WCHAR* text;
            float baseline;
            float size;
        };
        StampLine lines[2] = {};
        int nLines = 1;
        switch (GetEbookStampIconIndex(annotation)) {
            case 0:
                lines[0] = {L"APPROVED", 13.f, 30.f};
                break;
            case 1:
                lines[0] = {L"AS IS", 13.f, 30.f};
                break;
            case 2:
                lines[0] = {L"CONFIDENTIAL", 17.f, 20.f};
                break;
            case 3:
                lines[0] = {L"DEPARTMENTAL", 17.f, 20.f};
                break;
            case 5:
                lines[0] = {L"EXPERIMENTAL", 17.f, 20.f};
                break;
            case 6:
                lines[0] = {L"EXPIRED", 13.f, 30.f};
                break;
            case 7:
                lines[0] = {L"FINAL", 13.f, 30.f};
                break;
            case 8:
                lines[0] = {L"FOR COMMENT", 17.f, 20.f};
                break;
            case 9:
                lines[0] = {L"FOR PUBLIC", 26.f, 18.f};
                lines[1] = {L"RELEASE", 8.5f, 18.f};
                nLines = 2;
                break;
            case 10:
                lines[0] = {L"NOT APPROVED", 17.f, 20.f};
                break;
            case 11:
                lines[0] = {L"NOT FOR", 26.f, 18.f};
                lines[1] = {L"PUBLIC RELEASE", 8.5f, 18.f};
                nLines = 2;
                break;
            case 12:
                lines[0] = {L"SOLD", 13.f, 30.f};
                break;
            case 13:
                lines[0] = {L"TOP SECRET", 14.f, 26.f};
                break;
            case 4:
            default:
                lines[0] = {L"DRAFT", 13.f, 30.f};
                break;
        }
        Gdiplus::GraphicsState state = graphics.Save();
        graphics.SetPageUnit(Gdiplus::UnitPixel);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        Gdiplus::FontFamily times(L"Times New Roman");
        const Gdiplus::FontFamily* family =
            times.GetLastStatus() == Gdiplus::Ok ? &times : Gdiplus::FontFamily::GenericSerif();
        int em = family->GetEmHeight(Gdiplus::FontStyleBold);
        int cellAscent = family->GetCellAscent(Gdiplus::FontStyleBold);
        if (em <= 0) {
            em = 2048;
        }
        Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip |
                           Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
        float textX[2] = {};
        for (int i = 0; i < nLines; i++) {
            Gdiplus::Font font(family, lines[i].size, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::RectF measured;
            graphics.MeasureString(lines[i].text, -1, &font, Gdiplus::PointF(0, 0), &fmt, &measured);
            textX[i] = (190.f - measured.Width) / 2.f;
        }
        graphics.SetTransform(&xform);
        Gdiplus::Pen stampPen(Gdiplus::Color(255, r, g, b), 2.f);
        graphics.DrawRectangle(&stampPen, 2.f, 4.f, 186.f, 44.f);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, r, g, b));
        for (int i = 0; i < nLines; i++) {
            Gdiplus::Font font(family, lines[i].size, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            float ascent = lines[i].size * (float)cellAscent / (float)em;
            float top = (50.f - lines[i].baseline) - ascent;
            graphics.DrawString(lines[i].text, -1, &font, Gdiplus::PointF(textX[i], top), &fmt, &textBrush);
        }
        graphics.Restore(state);
        return;
    }
    if (type == AnnotationType::Line) {
        float width = (float)DpiScale(tab->win->hwndFrame, 2 * EbookAnnotationGetBorderWidth(annotation));
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
        float width = (float)DpiScale(tab->win->hwndFrame, 2 * EbookAnnotationGetBorderWidth(annotation));
        if (width > 0.f) {
            Gdiplus::Pen squarePen(Gdiplus::Color(255, r, g, b), width);
            float half = width / 2.f;
            Gdiplus::RectF square((float)marker.x + half, (float)marker.y + half,
                                  std::max(1.f, (float)marker.dx - width), std::max(1.f, (float)marker.dy - width));
            // Same six-unit corner radius as the PDF appearance and drag preview.
            // EPUB point annotation dimensions use DPI-scaled logical units.
            float radius =
                std::min((float)DpiScale(tab->win->hwndFrame, 6), std::min(square.Width, square.Height) / 4.f);
            float diameter = radius * 2.f;
            Gdiplus::GraphicsPath outline;
            outline.AddArc(square.X, square.Y, diameter, diameter, 180.f, 90.f);
            outline.AddArc(square.GetRight() - diameter, square.Y, diameter, diameter, 270.f, 90.f);
            outline.AddArc(square.GetRight() - diameter, square.GetBottom() - diameter, diameter, diameter, 0.f, 90.f);
            outline.AddArc(square.X, square.GetBottom() - diameter, diameter, diameter, 90.f, 90.f);
            outline.CloseFigure();
            COLORREF interior = 0;
            if (EbookAnnotationGetInteriorColor(annotation, &interior)) {
                interior = MapEbookAnnotationColor(interior);
                u8 ir, ig, ib;
                UnpackColor(interior, ir, ig, ib);
                Gdiplus::SolidBrush fill(Gdiplus::Color(255, ir, ig, ib));
                graphics.FillPath(&fill, &outline);
            }
            graphics.DrawPath(&squarePen, &outline);
        }
        return;
    }
    if (type == AnnotationType::Circle) {
        // Match pdf_write_circle_appearance(): center the stroke inside the
        // annotation rectangle and paint only if an interior color is set.
        float width = (float)DpiScale(tab->win->hwndFrame, 2 * EbookAnnotationGetBorderWidth(annotation));
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

static void PaintEbookInkStroke(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo, EbookAnnotation* annotation,
                                Point shift) {
    if (!tab || !dm || !annotation || annotation->inkPoints.len < 2) {
        return;
    }
    COLORREF color = MapEbookAnnotationColor(EbookAnnotationGetColor(annotation));
    u8 r, g, b;
    UnpackColor(color, r, g, b);
    // Same screen thickness as a line with this border value. Line, square and
    // circle already stroke at twice the stored width.
    float width = (float)DpiScale(tab->win->hwndFrame, 2 * EbookAnnotationGetBorderWidth(annotation));
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(255, r, g, b), std::max(1.f, width));
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    auto drawStroke = [&](int begin, int n) {
        if (n < 2 || begin < 0 || begin + n > (int)annotation->inkPoints.len) {
            return;
        }
        Point prevScreen = dm->CvtToScreen(pageNo, annotation->inkPoints.at(begin));
        prevScreen.x += shift.x;
        prevScreen.y += shift.y;
        for (int k = 1; k < n; k++) {
            Point curScreen = dm->CvtToScreen(pageNo, annotation->inkPoints.at(begin + k));
            curScreen.x += shift.x;
            curScreen.y += shift.y;
            graphics.DrawLine(&pen, prevScreen.x, prevScreen.y, curScreen.x, curScreen.y);
            prevScreen = curScreen;
        }
    };
    if (annotation->inkCounts.len == 0) {
        drawStroke(0, (int)annotation->inkPoints.len);
        return;
    }
    int src = 0;
    for (size_t i = 0; i < annotation->inkCounts.len; i++) {
        int n = annotation->inkCounts.at(i);
        drawStroke(src, n);
        if (n > 0) {
            src += n;
        }
    }
}

static void PaintEbookPointAnnotationSelection(HDC hdc, Rect marker, AnnotationType type) {
    marker.Inflate(type == AnnotationType::Text || type == AnnotationType::Caret ? 1 : 4,
                   type == AnnotationType::Text || type == AnnotationType::Caret ? 1 : 4);
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
    if (!str::IsEmptyOrWhiteSpace(EbookAnnotationGetNote(annotation))) {
        HWND hwndDpi = tab->win ? tab->win->hwndFrame : nullptr;
        int size = DpiScale(hwndDpi, 11);
        if (size < 9) {
            size = 9;
        }
        Rect lineRect = screenRects.Last();
        int x = lineRect.x + lineRect.dx - size / 2;
        int y = lineRect.y - size / 4;
        COLORREF color = EbookAnnotationGetColor(annotation);
        u8 r, g, b;
        UnpackColor(color, r, g, b);
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush fill(Gdiplus::Color(230, r, g, b));
        Gdiplus::Pen frame(Gdiplus::Color(200, 40, 40, 40), 1.f);
        Gdiplus::SolidBrush glyph(Gdiplus::Color(220, 30, 30, 30));
        Gdiplus::Rect bounds(x, y, size - 1, size - 1);
        graphics.FillRectangle(&fill, bounds);
        graphics.DrawRectangle(&frame, bounds);
        float iconSize = (float)std::max(6, size / 2);
        float iconX = (float)x + ((float)size - iconSize) / 2.f;
        float iconY = (float)y + ((float)size - iconSize) / 2.f;
        for (int row = 0; row < 8; row += 2) {
            graphics.FillRectangle(&glyph,
                                   Gdiplus::RectF(iconX, iconY + row * iconSize / 8.f, iconSize, iconSize / 8.f));
        }
    }
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

static Point EbookAnnotDragShift(WindowTab* tab, EbookAnnotation* annotation) {
    MainWindow* win = tab ? tab->win : nullptr;
    if (!win || !annotation || !win->annotMovePreview || win->ebookAnnotationBeingDragged != annotation) {
        return Point{0, 0};
    }
    return Point{win->annotMoveDest.x - win->annotMoveGrab.x, win->annotMoveDest.y - win->annotMoveGrab.y};
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
        Point shift = EbookAnnotDragShift(tab, annotation);
        if (annotation->type == AnnotationType::Ink) {
            if (annotation->inkPoints.len >= 2) {
                RectF bounds;
                if (!GetPointAnnotationPageBounds(annotations, engine, annotation, pageNo, &bounds)) {
                    continue;
                }
                PaintEbookInkStroke(tab, hdc, dm, pageNo, annotation, shift);
                if (tab->editEbookAnnotsWindow && tab->selectedEbookAnnotation == annotation) {
                    Rect screenRect = dm->CvtToScreen(pageNo, bounds);
                    screenRect.x += shift.x;
                    screenRect.y += shift.y;
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
                    screenRect.x += shift.x;
                    screenRect.y += shift.y;
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
                screenRect.x += shift.x;
                screenRect.y += shift.y;
                screenRects.Append(screenRect);
            }
        }
        PaintEbookMarkup(tab, hdc, screenRects, annotation);
    }
}
