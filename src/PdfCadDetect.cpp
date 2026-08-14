/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"

#include "Settings.h"
#include "GlobalPrefs.h"

#include "PdfCadDetect.h"

EngineeringDrawingEnhanceMode GetEngineeringDrawingEnhanceMode() {
    const char* v = gGlobalPrefs->engineeringDrawingEnhance;
    if (!v || !*v || str::EqI(v, "auto")) {
        return EngineeringDrawingEnhanceMode::Auto;
    }
    if (str::EqI(v, "on")) {
        return EngineeringDrawingEnhanceMode::On;
    }
    return EngineeringDrawingEnhanceMode::Off;
}

const char* CadEnhanceReasonName(CadEnhanceReason reason) {
    switch (reason) {
        case CadEnhanceReason::Pdfe:
            return "PDF/E";
        case CadEnhanceReason::Metadata:
            return "metadata";
        case CadEnhanceReason::Heuristic:
            return "heuristic";
        case CadEnhanceReason::RasterImage:
            return "raster-image";
        case CadEnhanceReason::Manual:
            return "manual";
        default:
            return "none";
    }
}

bool CadEnhanceEnabledForEngine(const CadDetectResult& detect, CadEnhanceOverride overrideState) {
    switch (overrideState) {
        case CadEnhanceOverride::ForceOn:
            return true;
        case CadEnhanceOverride::ForceOff:
            return false;
        default:
            break;
    }
    switch (GetEngineeringDrawingEnhanceMode()) {
        case EngineeringDrawingEnhanceMode::On:
            return true;
        case EngineeringDrawingEnhanceMode::Off:
            return false;
        case EngineeringDrawingEnhanceMode::Auto:
        default:
            return detect.enable;
    }
}

static bool MetadataContainsAnyI(const char* haystack, const char* const* needles, int count) {
    if (!haystack || !*haystack) {
        return false;
    }
    AutoFreeStr lower = str::Dup(haystack);
    str::ToLowerInPlace(lower);
    for (int i = 0; i < count; i++) {
        if (str::Find(lower, needles[i])) {
            return true;
        }
    }
    return false;
}

static const char* kMetadataStrong[] = {
    "autocad",    "dwg to pdf", "dwg trueview", "revit",    "microstation", "solidworks", "catia",
    " creo",      " nx ",       "zwcad",        "gstarcad", "浩辰",         "中望",       "bluebeam",
    "pdffactory", "tekla",      "sketchup",     "archicad", "vectorworks",  "bentley",
};

static const char* kMetadataWeak[] = {
    "cad",       "dwg",        "plot",           "engineering", "layout", "draft", "mechanical",
    "architect", "screenshot", "screen capture", "snipaste",    "截图",   "wps",
};

static const char* kMetadataBlacklist[] = {
    "microsoft word", "libreoffice", "openoffice", "indesign", "itext",     "pdflatex", "xelatex",
    "lualatex",       "latex",       " prince",    "chrome",   "skia/pdf",  "mozilla",  "calibre",
    "epub",           "powerpoint",  "excel",      "onenote",  "doctotext",
};

static bool BufferContainsAscii(fz_buffer* buf, const char* needle) {
    if (!buf || !needle) {
        return false;
    }
    unsigned char* data = nullptr;
    size_t len = fz_buffer_storage((fz_context*)nullptr, buf, &data);
    if (!data || len == 0) {
        return false;
    }
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(data + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static bool HasPdfEMarker(fz_context* ctx, pdf_document* doc) {
    pdf_obj* trailer = pdf_trailer(ctx, doc);
    pdf_obj* info = pdf_dict_get(ctx, trailer, PDF_NAME(Info));
    if (info) {
        pdf_obj* v = pdf_dict_gets(ctx, info, "ISO_PDFEVersion");
        if (pdf_is_string(ctx, v)) {
            return true;
        }
    }

    pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
    pdf_obj* meta = pdf_dict_get(ctx, root, PDF_NAME(Metadata));
    if (!meta) {
        return false;
    }

    fz_buffer* buf = nullptr;
    bool found = false;
    fz_var(buf);
    fz_try(ctx) {
        buf = pdf_load_stream(ctx, meta);
        if (BufferContainsAscii(buf, "pdfe:ISO_PDFEVersion") || BufferContainsAscii(buf, "PDF/E-1") ||
            BufferContainsAscii(buf, "PDF/E-2")) {
            found = true;
        }
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return found;
}

static void AppendMetadataField(fz_context* ctx, pdf_obj* info, const char* key, StrVec& out) {
    if (!info) {
        return;
    }
    pdf_obj* val = pdf_dict_gets(ctx, info, key);
    if (pdf_is_string(ctx, val)) {
        const char* s = pdf_to_text_string(ctx, val);
        if (s && *s) {
            out.Append(s);
        }
    }
}

static int ScoreMetadata(fz_context* ctx, pdf_document* doc, bool* strongMatchOut) {
    *strongMatchOut = false;
    StrVec fields;
    pdf_obj* trailer = pdf_trailer(ctx, doc);
    pdf_obj* info = pdf_dict_get(ctx, trailer, PDF_NAME(Info));
    AppendMetadataField(ctx, info, "Creator", fields);
    AppendMetadataField(ctx, info, "Producer", fields);

    pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
    pdf_obj* meta = pdf_dict_get(ctx, root, PDF_NAME(Metadata));
    if (meta) {
        fz_buffer* buf = nullptr;
        fz_var(buf);
        fz_try(ctx) {
            buf = pdf_load_stream(ctx, meta);
            unsigned char* data = nullptr;
            size_t len = fz_buffer_storage(ctx, buf, &data);
            if (data && len > 0) {
                TempStr s = str::DupTemp((const char*)data, len);
                fields.Append(s);
            }
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }

    int score = 0;
    for (const char* field : fields) {
        if (MetadataContainsAnyI(field, kMetadataBlacklist, dimof(kMetadataBlacklist))) {
            return -100;
        }
        if (MetadataContainsAnyI(field, kMetadataStrong, dimof(kMetadataStrong))) {
            *strongMatchOut = true;
            score += 40;
        } else if (MetadataContainsAnyI(field, kMetadataWeak, dimof(kMetadataWeak))) {
            score += 15;
        }
    }
    return score;
}

typedef struct {
    fz_device super;
    int strokes;
    int fills;
    int textOps;
    int grayStrokes;
    int thinStrokes;
    float maxImageCoverage;
    float pageArea;
} cad_analysis_device;

static float CadRectArea(fz_rect r) {
    if (fz_is_empty_rect(r) || fz_is_infinite_rect(r)) {
        return 0.f;
    }
    return (r.x1 - r.x0) * (r.y1 - r.y0);
}

static bool CadIsGrayRgb(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float chroma = maxC - minC;
    if (chroma > 0.12f) {
        return false;
    }
    return lum >= 0.38f && lum <= 0.88f;
}

static void cad_analysis_note_stroke(cad_analysis_device* d, const fz_stroke_state* stroke, float r, float g, float b) {
    d->strokes++;
    if (CadIsGrayRgb(r, g, b)) {
        d->grayStrokes++;
    }
    if (stroke && stroke->linewidth <= 0.25f) {
        d->thinStrokes++;
    }
}

static void cad_analysis_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path,
                                     const fz_stroke_state* stroke, fz_matrix ctm, fz_colorspace* colorspace,
                                     const float* color, float alpha, fz_color_params color_params) {
    (void)path;
    (void)ctm;
    (void)alpha;
    cad_analysis_device* d = (cad_analysis_device*)dev;
    float rgb[FZ_MAX_COLORS] = {};
    fz_colorspace* ds = fz_device_rgb(ctx);
    fz_convert_color(ctx, colorspace, color, ds, rgb, colorspace, color_params);
    cad_analysis_note_stroke(d, stroke, rgb[0], rgb[1], rgb[2]);
}

static void cad_analysis_fill_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm,
                                   fz_colorspace* colorspace, const float* color, float alpha,
                                   fz_color_params color_params) {
    (void)ctx;
    (void)path;
    (void)even_odd;
    (void)ctm;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    cad_analysis_device* d = (cad_analysis_device*)dev;
    d->fills++;
}

static void cad_analysis_fill_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                                   fz_colorspace* colorspace, const float* color, float alpha,
                                   fz_color_params color_params) {
    (void)text;
    (void)ctm;
    (void)alpha;
    cad_analysis_device* d = (cad_analysis_device*)dev;
    d->textOps++;
    float rgb[FZ_MAX_COLORS] = {};
    fz_colorspace* ds = fz_device_rgb(ctx);
    fz_convert_color(ctx, colorspace, color, ds, rgb, colorspace, color_params);
    cad_analysis_note_stroke(d, nullptr, rgb[0], rgb[1], rgb[2]);
}

static void cad_analysis_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text,
                                     const fz_stroke_state* stroke, fz_matrix ctm, fz_colorspace* colorspace,
                                     const float* color, float alpha, fz_color_params color_params) {
    cad_analysis_fill_text(ctx, dev, text, ctm, colorspace, color, alpha, color_params);
    (void)stroke;
}

static void cad_analysis_fill_image(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha,
                                    fz_color_params color_params) {
    (void)ctx;
    (void)image;
    (void)alpha;
    (void)color_params;
    cad_analysis_device* d = (cad_analysis_device*)dev;
    fz_rect bbox = fz_transform_rect(fz_unit_rect, ctm);
    if (d->pageArea > 0.f) {
        float coverage = CadRectArea(bbox) / d->pageArea;
        if (coverage > d->maxImageCoverage) {
            d->maxImageCoverage = coverage;
        }
    }
}

static void cad_analysis_drop_device(fz_context* ctx, fz_device* dev) {
    (void)ctx;
    cad_analysis_device* d = (cad_analysis_device*)dev;
    free(d);
}

static fz_device* NewCadAnalysisDevice(fz_context* ctx, float pageArea) {
    cad_analysis_device* d = (cad_analysis_device*)calloc(1, sizeof(cad_analysis_device));
    if (!d) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "cad analysis device: out of memory");
    }
    d->super.drop_device = cad_analysis_drop_device;
    d->super.stroke_path = cad_analysis_stroke_path;
    d->super.fill_path = cad_analysis_fill_path;
    d->super.fill_text = cad_analysis_fill_text;
    d->super.stroke_text = cad_analysis_stroke_text;
    d->super.fill_image = cad_analysis_fill_image;
    d->pageArea = pageArea;
    return &d->super;
}

static int CountOcgLayers(fz_context* ctx, pdf_document* doc) {
    pdf_obj* trailer = pdf_trailer(ctx, doc);
    pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
    pdf_obj* ocp = pdf_dict_get(ctx, root, PDF_NAME(OCProperties));
    if (!ocp) {
        return 0;
    }
    pdf_obj* ocgs = pdf_dict_get(ctx, ocp, PDF_NAME(OCGs));
    if (!pdf_is_array(ctx, ocgs)) {
        return 0;
    }
    return pdf_array_len(ctx, ocgs);
}

static int CountSquareAnnots(fz_context* ctx, pdf_document* doc, int pageCount) {
    int count = 0;
    int pages = pageCount > 3 ? 3 : pageCount;
    for (int i = 0; i < pages; i++) {
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, doc, i);
        pdf_obj* annots = pdf_dict_get(ctx, pageObj, PDF_NAME(Annots));
        if (!pdf_is_array(ctx, annots)) {
            continue;
        }
        int n = pdf_array_len(ctx, annots);
        for (int j = 0; j < n; j++) {
            pdf_obj* annot = pdf_array_get(ctx, annots, j);
            pdf_obj* subtype = pdf_dict_get(ctx, annot, PDF_NAME(Subtype));
            if (pdf_name_eq(ctx, subtype, PDF_NAME(Square))) {
                count++;
            }
        }
    }
    return count;
}

static void AnalyzePage(fz_context* ctx, pdf_document* doc, int pageNo, cad_analysis_device* stats) {
    pdf_page* page = nullptr;
    fz_device* dev = nullptr;
    fz_var(page);
    fz_var(dev);
    fz_try(ctx) {
        page = pdf_load_page(ctx, doc, pageNo);
        dev = NewCadAnalysisDevice(ctx, stats->pageArea);
        pdf_run_page_contents(ctx, page, dev, fz_identity, nullptr);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
        fz_drop_page(ctx, (fz_page*)page);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }

    cad_analysis_device* pageStats = (cad_analysis_device*)dev;
    stats->strokes += pageStats->strokes;
    stats->fills += pageStats->fills;
    stats->textOps += pageStats->textOps;
    stats->grayStrokes += pageStats->grayStrokes;
    stats->thinStrokes += pageStats->thinStrokes;
    if (pageStats->maxImageCoverage > stats->maxImageCoverage) {
        stats->maxImageCoverage = pageStats->maxImageCoverage;
    }
}

static int ScoreHeuristic(fz_context* ctx, pdf_document* doc, int pageCount, float maxPageSide, bool* rasterDominantOut,
                          bool* hairlineVectorOut) {
    cad_analysis_device stats{};
    stats.pageArea = maxPageSide * maxPageSide;
    int pages = pageCount > 2 ? 2 : pageCount;
    for (int i = 0; i < pages; i++) {
        AnalyzePage(ctx, doc, i, &stats);
    }

    int score = 0;
    int strokeFillDenom = stats.strokes + stats.fills;
    float strokeFillRatio = strokeFillDenom > 0 ? (float)stats.strokes / (float)strokeFillDenom : 0.f;
    if (strokeFillRatio > 0.85f) {
        score += 25;
    }

    float grayRatio = stats.strokes > 0 ? (float)stats.grayStrokes / (float)stats.strokes : 0.f;
    if (grayRatio > 0.30f) {
        score += 25;
    }

    float thinRatio = stats.strokes > 0 ? (float)stats.thinStrokes / (float)stats.strokes : 0.f;
    if (thinRatio > 0.20f) {
        score += 15;
    }

    if (CountOcgLayers(ctx, doc) >= 2) {
        score += 15;
    }

    if (maxPageSide >= 842.f) {
        score += 10;
    }

    float textStrokeRatio = stats.strokes > 0 ? (float)stats.textOps / (float)stats.strokes : 0.f;
    if (textStrokeRatio < 0.15f) {
        score += 10;
    }

    int squareAnnots = CountSquareAnnots(ctx, doc, pageCount);
    if (squareAnnots > 20) {
        score += 5;
    }

    if (stats.textOps > 500 && strokeFillRatio < 0.5f) {
        score -= 30;
    }

    // Illustrated books / art catalogs: prominent images with normal text (not CAD).
    if (stats.maxImageCoverage >= 0.15f && stats.textOps >= 40) {
        score -= 65;
    }
    if (pageCount >= 80 && stats.maxImageCoverage >= 0.12f && stats.textOps >= 25) {
        score -= 40;
    }

    // Hairline vector exports (e.g. WPS "print to PDF" from a CAD screenshot):
    // dense 0.05pt strokes, almost no embedded bitmap.
    bool hairlineCad =
        stats.maxImageCoverage < 0.05f && stats.strokes >= 40 && thinRatio > 0.25f && strokeFillRatio > 0.55f;
    if (hairlineCad) {
        score += 35;
        if (hairlineVectorOut) {
            *hairlineVectorOut = true;
        }
    }

    // Screenshot / raster CAD: one large image per page, almost no vector content.
    bool rasterCad = stats.maxImageCoverage >= 0.80f && stats.strokes + stats.fills < 50 && stats.textOps < 200;
    if (rasterCad) {
        if (pageCount > 30) {
            // Full-bleed cover art in books mimics raster CAD; do not treat as engineering scan.
            score -= 70;
        } else {
            score += 55;
            if (rasterDominantOut) {
                *rasterDominantOut = true;
            }
        }
    } else if (stats.maxImageCoverage > 0.5f) {
        score -= 40;
    }

    return score;
}

CadDetectResult DetectCadPdf(fz_context* ctx, pdf_document* doc) {
    CadDetectResult res;
    if (!ctx || !doc) {
        return res;
    }

    if (HasPdfEMarker(ctx, doc)) {
        res.enable = true;
        res.reason = CadEnhanceReason::Pdfe;
        res.score = 100;
        return res;
    }

    bool strongMetadata = false;
    int metadataScore = ScoreMetadata(ctx, doc, &strongMetadata);
    if (metadataScore <= -100) {
        return res;
    }
    if (strongMetadata) {
        res.enable = true;
        res.reason = CadEnhanceReason::Metadata;
        res.score = metadataScore;
        return res;
    }

    int pageCount = pdf_count_pages(ctx, doc);
    float maxPageSide = 0.f;
    int samplePages = pageCount > 3 ? 3 : pageCount;
    for (int i = 0; i < samplePages; i++) {
        pdf_page* page = nullptr;
        fz_var(page);
        fz_try(ctx) {
            page = pdf_load_page(ctx, doc, i);
            fz_rect bounds = pdf_bound_page(ctx, page, FZ_CROP_BOX);
            float side = bounds.x1 - bounds.x0;
            float sideY = bounds.y1 - bounds.y0;
            if (sideY > side) {
                side = sideY;
            }
            if (side > maxPageSide) {
                maxPageSide = side;
            }
        }
        fz_always(ctx) {
            fz_drop_page(ctx, (fz_page*)page);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }

    bool rasterDominant = false;
    bool hairlineVector = false;
    int heuristicScore = ScoreHeuristic(ctx, doc, pageCount, maxPageSide, &rasterDominant, &hairlineVector);
    res.score = heuristicScore + metadataScore;
    res.rasterDominant = rasterDominant;
    res.hairlineVector = hairlineVector;
    if (rasterDominant && res.score >= 45 && pageCount <= 30) {
        res.enable = true;
        res.reason = CadEnhanceReason::RasterImage;
        return res;
    }
    if (rasterDominant && pageCount > 30) {
        res.rasterDominant = false;
    }
    if (hairlineVector && res.score >= 45) {
        res.enable = true;
        res.reason = CadEnhanceReason::Heuristic;
        return res;
    }
    if (strongMetadata == false && metadataScore > 0 && res.score >= 45) {
        res.enable = true;
        res.reason = CadEnhanceReason::Heuristic;
        return res;
    }
    if (res.score >= 60) {
        res.enable = true;
        res.reason = CadEnhanceReason::Heuristic;
    }
    return res;
}
