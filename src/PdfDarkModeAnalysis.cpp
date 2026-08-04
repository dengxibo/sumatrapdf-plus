/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"
#include "wingui/UIModels.h"
#include "DocController.h"
#include "EngineBase.h"
#include "Annotation.h"
#include "EngineMupdf.h"
#include "PdfDarkModeInternal.h"

#define DM_ANALYSIS_STACK_SIZE 96

typedef struct {
    fz_device super;
    DarkModePageAnalysis* analysis;
    DarkModeOptions options;
    DarkModeEngineCache* engineCache;
    int top;
    fz_rect stack[DM_ANALYSIS_STACK_SIZE];
    int textOps;
    int vectorOps;
    float maxImageCoverage;
} pdf_dark_mode_analysis_device;

static fz_rect dm_analysis_clip_rect(pdf_dark_mode_analysis_device* d, fz_rect rect) {
    if (d->top > 0 && d->top <= DM_ANALYSIS_STACK_SIZE) {
        return fz_intersect_rect(rect, d->stack[d->top - 1]);
    }
    return rect;
}

static void dm_analysis_push_clip(pdf_dark_mode_analysis_device* d, fz_rect rect, bool clip) {
    rect = dm_analysis_clip_rect(d, rect);
    if (clip && ++d->top <= DM_ANALYSIS_STACK_SIZE) {
        d->stack[d->top - 1] = rect;
    }
}

static void dm_analysis_pop_clip(fz_context* ctx, fz_device* dev) {
    (void)ctx;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    if (d->top > 0) {
        d->top--;
    }
}

static float dm_rect_area(fz_rect r) {
    if (fz_is_empty_rect(r) || fz_is_infinite_rect(r)) {
        return 0.f;
    }
    return (r.x1 - r.x0) * (r.y1 - r.y0);
}

static float dm_rectf_area(RectF r) {
    if (r.IsEmpty()) {
        return 0.f;
    }
    return r.dx * r.dy;
}

static void dm_analysis_record_image(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, bool isMask) {
    (void)ctx;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    fz_rect bbox = fz_transform_rect(fz_unit_rect, ctm);
    bbox = dm_analysis_clip_rect(d, bbox);
    if (fz_is_empty_rect(bbox)) {
        return;
    }

    float pageArea = dm_rectf_area(d->analysis->pageBounds);
    float coverage = pageArea > 0.f ? dm_rect_area(bbox) / pageArea : 0.f;
    if (coverage > d->maxImageCoverage) {
        d->maxImageCoverage = coverage;
    }

    ImageOccurrenceInfo info;
    info.occurrenceIndex = d->analysis->images.Size();
    info.pageBounds = ToRectF(bbox);
    info.isImageMask = isMask;
    info.hasAlpha = image && image->mask;
    info.pageCoverage = coverage;
    if (image) {
        if (PdfFollowThemePreservesEmbeddedImageColors()) {
            info.analysis = PdfDarkModeAnalyzeImageCached(ctx, image, coverage, false, d->engineCache);
            info.looksLikePhoto =
                info.analysis.kind == DarkImageKind::Photo || info.analysis.kind == DarkImageKind::Unknown;
        } else {
            info.analysis =
                PdfDarkModeAnalyzeImageCached(ctx, image, coverage, d->analysis->isScannedPage, d->engineCache);
            info.looksLikePhoto =
                info.analysis.kind == DarkImageKind::Photo || info.analysis.kind == DarkImageKind::Unknown;
        }
    } else {
        info.looksLikePhoto = false;
    }
    d->analysis->images.Append(info);
}

static void dm_analysis_fill_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm,
                                  fz_colorspace* colorspace, const float* color, float alpha,
                                  fz_color_params color_params) {
    (void)path;
    (void)even_odd;
    (void)ctm;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    d->vectorOps++;
}

static void dm_analysis_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                                    fz_matrix ctm, fz_colorspace* colorspace, const float* color, float alpha,
                                    fz_color_params color_params) {
    (void)path;
    (void)stroke;
    (void)ctm;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    d->vectorOps++;
}

static void dm_analysis_fill_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                                  fz_colorspace* colorspace, const float* color, float alpha,
                                  fz_color_params color_params) {
    (void)text;
    (void)ctm;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    d->textOps++;
}

static void dm_analysis_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                                    fz_matrix ctm, fz_colorspace* colorspace, const float* color, float alpha,
                                    fz_color_params color_params) {
    (void)text;
    (void)stroke;
    (void)ctm;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    d->textOps++;
}

static void dm_analysis_fill_shade(fz_context* ctx, fz_device* dev, fz_shade* shd, fz_matrix ctm, float alpha,
                                   fz_color_params color_params) {
    (void)shd;
    (void)ctm;
    (void)alpha;
    (void)color_params;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    d->vectorOps++;
}

static void dm_analysis_fill_image(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha,
                                   fz_color_params color_params) {
    (void)alpha;
    (void)color_params;
    dm_analysis_record_image(ctx, dev, image, ctm, false);
}

static void dm_analysis_fill_image_mask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm,
                                        fz_colorspace* colorspace, const float* color, float alpha,
                                        fz_color_params color_params) {
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    dm_analysis_record_image(ctx, dev, image, ctm, true);
}

static void dm_analysis_clip_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm,
                                  fz_rect scissor) {
    (void)scissor;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    dm_analysis_push_clip(d, fz_bound_path(ctx, path, nullptr, ctm), true);
}

static void dm_analysis_clip_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path,
                                         const fz_stroke_state* stroke, fz_matrix ctm, fz_rect scissor) {
    (void)scissor;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    dm_analysis_push_clip(d, fz_bound_path(ctx, path, stroke, ctm), true);
}

static void dm_analysis_clip_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                                  fz_rect scissor) {
    (void)scissor;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    dm_analysis_push_clip(d, fz_bound_text(ctx, text, nullptr, ctm), true);
}

static void dm_analysis_clip_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text,
                                         const fz_stroke_state* stroke, fz_matrix ctm, fz_rect scissor) {
    (void)scissor;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    dm_analysis_push_clip(d, fz_bound_text(ctx, text, stroke, ctm), true);
}

static void dm_analysis_clip_image_mask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm,
                                        fz_rect scissor) {
    (void)scissor;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    dm_analysis_push_clip(d, fz_transform_rect(fz_unit_rect, ctm), true);
}

static void dm_analysis_begin_mask(fz_context* ctx, fz_device* dev, fz_rect area, int luminosity,
                                   fz_colorspace* colorspace, const float* color, fz_color_params color_params) {
    (void)luminosity;
    (void)colorspace;
    (void)color;
    (void)color_params;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    dm_analysis_push_clip(d, area, true);
}

static void dm_analysis_end_mask(fz_context* ctx, fz_device* dev, fz_function* tr) {
    (void)tr;
    dm_analysis_pop_clip(ctx, dev);
}

static void dm_analysis_begin_group(fz_context* ctx, fz_device* dev, fz_rect area, fz_colorspace* cs, int isolated,
                                    int knockout, int blendmode, float alpha) {
    (void)cs;
    (void)isolated;
    (void)knockout;
    (void)blendmode;
    (void)alpha;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    dm_analysis_push_clip(d, area, true);
}

static void dm_analysis_end_group(fz_context* ctx, fz_device* dev) {
    dm_analysis_pop_clip(ctx, dev);
}

static int dm_analysis_begin_tile(fz_context* ctx, fz_device* dev, fz_rect area, fz_rect view, float xstep, float ystep,
                                  fz_matrix ctm, int id, int doc_id) {
    (void)view;
    (void)xstep;
    (void)ystep;
    (void)id;
    (void)doc_id;
    pdf_dark_mode_analysis_device* d = (pdf_dark_mode_analysis_device*)dev;
    dm_analysis_push_clip(d, fz_transform_rect(area, ctm), false);
    return 0;
}

static void dm_analysis_end_tile(fz_context* ctx, fz_device* dev) {
    (void)ctx;
    (void)dev;
}

static void dm_analysis_drop_device(fz_context* ctx, fz_device* dev) {
    (void)ctx;
    (void)dev;
}

static fz_device* PdfDarkModeNewAnalysisDevice(fz_context* ctx, DarkModePageAnalysis* analysis,
                                               const DarkModeOptions& options, DarkModeEngineCache* engineCache) {
    pdf_dark_mode_analysis_device* d = fz_new_derived_device(ctx, pdf_dark_mode_analysis_device);
    d->super.fill_path = dm_analysis_fill_path;
    d->super.stroke_path = dm_analysis_stroke_path;
    d->super.fill_text = dm_analysis_fill_text;
    d->super.stroke_text = dm_analysis_stroke_text;
    d->super.fill_shade = dm_analysis_fill_shade;
    d->super.fill_image = dm_analysis_fill_image;
    d->super.fill_image_mask = dm_analysis_fill_image_mask;
    d->super.clip_path = dm_analysis_clip_path;
    d->super.clip_stroke_path = dm_analysis_clip_stroke_path;
    d->super.clip_text = dm_analysis_clip_text;
    d->super.clip_stroke_text = dm_analysis_clip_stroke_text;
    d->super.clip_image_mask = dm_analysis_clip_image_mask;
    d->super.pop_clip = dm_analysis_pop_clip;
    d->super.begin_mask = dm_analysis_begin_mask;
    d->super.end_mask = dm_analysis_end_mask;
    d->super.begin_group = dm_analysis_begin_group;
    d->super.end_group = dm_analysis_end_group;
    d->super.begin_tile = dm_analysis_begin_tile;
    d->super.end_tile = dm_analysis_end_tile;
    d->super.drop_device = dm_analysis_drop_device;
    d->analysis = analysis;
    d->options = options;
    d->engineCache = engineCache;
    d->top = 0;
    d->textOps = 0;
    d->vectorOps = 0;
    d->maxImageCoverage = 0.f;
    return &d->super;
}

static bool PdfDarkModeImageMeetsPreserveMinSize(const ImageOccurrenceInfo& img) {
    int minPx = GetPreservePdfImagesMinSize();
    if (minPx <= 0) {
        return true;
    }
    // pageBounds are in page space; at 100% zoom device px ~= page pt for typical PDFs
    return img.pageBounds.dx >= (float)minPx && img.pageBounds.dy >= (float)minPx;
}

static bool PdfDarkModeIsPureScanPage(bool isScanned, int textOps, int vectorOps) {
    if (!isScanned) {
        return false;
    }
    // Full-bleed photo covers can match scan heuristics; require almost no vector/text work.
    return textOps <= 2 && vectorOps <= 2;
}

static bool PdfDarkModeDetectScanPage(DarkModePageAnalysis* analysis, int textOps, int vectorOps, float maxCoverage,
                                      const DarkModeOptions& options) {
    if (maxCoverage < options.scanImageCoverageThreshold) {
        return false;
    }
    if (textOps > options.maxTextOpsForScanPage || vectorOps > options.maxVectorOpsForScanPage) {
        return false;
    }

    float dominantCoverage = 0.f;
    RectF dominantBounds;
    int imageOps = 0;
    for (const ImageOccurrenceInfo& img : analysis->images) {
        if (img.isImageMask) {
            continue;
        }
        imageOps++;
        if (img.pageCoverage > dominantCoverage) {
            dominantCoverage = img.pageCoverage;
            dominantBounds = img.pageBounds;
        }
    }
    if (imageOps <= 0) {
        return false;
    }
    if (dominantCoverage < options.minScanDominantCoverage && maxCoverage < 0.92f) {
        return false;
    }
    if (!analysis->pageBounds.IsEmpty() && !dominantBounds.IsEmpty()) {
        float pageAsp = analysis->pageBounds.dx / analysis->pageBounds.dy;
        float imgAsp = dominantBounds.dx / dominantBounds.dy;
        if (pageAsp > 0.f && imgAsp > 0.f) {
            float skew = pageAsp > imgAsp ? pageAsp / imgAsp : imgAsp / pageAsp;
            if (skew > options.maxScanAspectSkew) {
                return false;
            }
        }
    }
    return true;
}

static void PdfDarkModeAssignFollowThemeImagePolicy(ImageOccurrenceInfo& img, const RectF& pageBounds) {
    if (img.pageCoverage >= kMaxPreserveImagePageCoverage && img.analysis.confidence > 0.f) {
        img.policy = PdfDarkModePolicyForImageKind(img.analysis.kind, img.isImageMask);
        return;
    }
    img.policy = PdfDarkModePolicyForFollowThemeImage(img.pageBounds, img.isImageMask, pageBounds);
}

static void PdfDarkModeAssignPolicies(DarkModePageAnalysis* analysis, const DarkModeOptions& options, int textOps,
                                      int vectorOps, float maxCoverage) {
    const bool followThemeFast = PdfFollowThemePreservesEmbeddedImageColors();

    bool isScanned =
        followThemeFast ? false : PdfDarkModeDetectScanPage(analysis, textOps, vectorOps, maxCoverage, options);
    analysis->isScannedPage = isScanned;
    bool isPureScan = followThemeFast ? false : PdfDarkModeIsPureScanPage(isScanned, textOps, vectorOps);

    for (ImageOccurrenceInfo& img : analysis->images) {
        if (followThemeFast) {
            PdfDarkModeAssignFollowThemeImagePolicy(img, analysis->pageBounds);
            continue;
        }
        if (!img.isImageMask) {
            float conf = img.analysis.confidence;
            img.analysis.kind =
                PdfDarkModeClassifyImageFeatures(img.analysis.features, img.pageCoverage, isScanned, &conf);
            img.analysis.confidence = conf;
        }
        img.policy = PdfDarkModePolicyForImageKind(img.analysis.kind, img.isImageMask);
        if (!img.isImageMask) {
            bool preserveArt = PdfDarkModeShouldPreserveImageFeatures(img.analysis.features, img.pageCoverage);
            if (preserveArt) {
                img.analysis.kind = DarkImageKind::Photo;
                img.policy = DarkImagePolicy::Preserve;
            }
            if (!PdfDarkModeImageMeetsPreserveMinSize(img) ||
                PdfDarkModeIsDecorativeStripImage(img.pageBounds, analysis->pageBounds)) {
                img.policy = DarkImagePolicy::AdaptiveDocument;
            } else if (img.pageCoverage >= kMaxPreserveImagePageCoverage && !preserveArt) {
                img.policy = DarkImagePolicy::AdaptiveDocument;
            }
            if (isPureScan && img.pageCoverage >= options.minScanDominantCoverage && !preserveArt) {
                img.analysis.kind = DarkImageKind::FullPageScan;
                img.analysis.confidence = 0.88f;
                img.policy = DarkImagePolicy::AdaptiveDocument;
            }
        }
    }
}

void PdfDarkModeFreeAnalysis(fz_context* ctx, DarkModePageAnalysis* analysis) {
    if (!analysis) {
        return;
    }
    PdfDarkModeFreeProcessCache(ctx, analysis);
    delete analysis;
}

void PdfDarkModeInvalidatePage(fz_context* ctx, FzPageInfo* pageInfo) {
    if (!pageInfo) {
        return;
    }
    if (pageInfo->darkModeAnalysis) {
        PdfDarkModeFreeAnalysis(ctx, pageInfo->darkModeAnalysis);
        pageInfo->darkModeAnalysis = nullptr;
    }
    pageInfo->darkModeAnalysisHash = 0;
    pageInfo->darkLegacySkipHash = 0;
    pageInfo->darkLegacyArtworkPageBottom = 0.f;
    pageInfo->darkLegacySkipDevAbs.Clear();
    pageInfo->followThemeScanProbe = 0;
}

DarkModePageAnalysis* PdfDarkModeGetOrBuildAnalysis(fz_context* ctx, FzPageInfo* pageInfo, fz_display_list* list,
                                                    u32 optionsHash, DarkModeEngineCache* engineCache) {
    if (pageInfo->darkModeAnalysis && pageInfo->darkModeAnalysisHash == optionsHash) {
        return pageInfo->darkModeAnalysis;
    }
    PdfDarkModeInvalidatePage(ctx, pageInfo);

    DarkModeOptions options = PdfDarkModeCurrentOptions();
    auto* analysis = new DarkModePageAnalysis();
    analysis->pageNumber = pageInfo->pageNo;
    analysis->optionsHash = optionsHash;

    if (pageInfo->page) {
        analysis->pageBounds = ToRectF(fz_bound_page(ctx, pageInfo->page));
    } else if (!pageInfo->mediabox.IsEmpty()) {
        analysis->pageBounds = pageInfo->mediabox;
    }

    fz_rect pageRect = ToFzRect(analysis->pageBounds);
    fz_device* dev = nullptr;
    pdf_dark_mode_analysis_device* ad = nullptr;
    fz_var(dev);
    fz_try(ctx) {
        dev = PdfDarkModeNewAnalysisDevice(ctx, analysis, options, engineCache);
        ad = (pdf_dark_mode_analysis_device*)dev;
        fz_run_display_list(ctx, list, dev, fz_identity, pageRect, nullptr);
        fz_close_device(ctx, dev);
        PdfDarkModeAssignPolicies(analysis, options, ad->textOps, ad->vectorOps, ad->maxImageCoverage);
    }
    fz_always(ctx) {
        if (dev) {
            fz_drop_device(ctx, dev);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        delete analysis;
        return nullptr;
    }

    pageInfo->darkModeAnalysis = analysis;
    pageInfo->darkModeAnalysisHash = optionsHash;
    return analysis;
}

typedef struct {
    fz_device super;
    float maxImageCoverage;
    int textOps;
    int vectorOps;
    int imageOps;
    float pageArea;
} pdf_scan_probe_device;

static void probe_drop_device(fz_context* ctx, fz_device* dev) {
    (void)ctx;
    (void)dev;
}

static void probe_fill_image(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha,
                             fz_color_params color_params) {
    (void)ctx;
    (void)image;
    (void)alpha;
    (void)color_params;
    pdf_scan_probe_device* d = (pdf_scan_probe_device*)dev;
    d->imageOps++;
    if (d->pageArea <= 0.f) {
        return;
    }
    fz_rect bbox = fz_transform_rect(fz_unit_rect, ctm);
    if (fz_is_empty_rect(bbox) || fz_is_infinite_rect(bbox)) {
        return;
    }
    float cov = (bbox.x1 - bbox.x0) * (bbox.y1 - bbox.y0) / d->pageArea;
    if (cov > d->maxImageCoverage) {
        d->maxImageCoverage = cov;
    }
}

static void probe_fill_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                            fz_colorspace* colorspace, const float* color, float alpha, fz_color_params color_params) {
    (void)ctx;
    (void)text;
    (void)ctm;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    ((pdf_scan_probe_device*)dev)->textOps++;
}

static void probe_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                              fz_matrix ctm, fz_colorspace* colorspace, const float* color, float alpha,
                              fz_color_params color_params) {
    (void)ctx;
    (void)text;
    (void)stroke;
    (void)ctm;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    ((pdf_scan_probe_device*)dev)->textOps++;
}

static void probe_fill_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm,
                            fz_colorspace* colorspace, const float* color, float alpha, fz_color_params color_params) {
    (void)ctx;
    (void)path;
    (void)even_odd;
    (void)ctm;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    ((pdf_scan_probe_device*)dev)->vectorOps++;
}

static void probe_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                              fz_matrix ctm, fz_colorspace* colorspace, const float* color, float alpha,
                              fz_color_params color_params) {
    (void)ctx;
    (void)path;
    (void)stroke;
    (void)ctm;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)color_params;
    ((pdf_scan_probe_device*)dev)->vectorOps++;
}

static void probe_fill_shade(fz_context* ctx, fz_device* dev, fz_shade* shd, fz_matrix ctm, float alpha,
                             fz_color_params color_params) {
    (void)ctx;
    (void)shd;
    (void)ctm;
    (void)alpha;
    (void)color_params;
    ((pdf_scan_probe_device*)dev)->vectorOps++;
}

static bool PdfDarkModeInfoFieldContainsI(fz_context* ctx, pdf_obj* info, pdf_obj* key, const char* needle) {
    if (!info || !needle || !*needle) {
        return false;
    }
    const char* val = pdf_dict_get_text_string(ctx, info, key);
    if (!val || !*val) {
        return false;
    }
    return str::FindI(val, needle) != nullptr;
}

static bool PdfDarkModeInfoFieldsMatchAnyI(fz_context* ctx, pdf_obj* info, const char* const* needles, int count) {
    for (int i = 0; i < count; i++) {
        if (PdfDarkModeInfoFieldContainsI(ctx, info, PDF_NAME(Creator), needles[i]) ||
            PdfDarkModeInfoFieldContainsI(ctx, info, PDF_NAME(Producer), needles[i])) {
            return true;
        }
    }
    return false;
}

bool PdfDarkModePdfMetadataSuggestsBitmapRecolorDoc(fz_context* ctx, pdf_document* doc) {
    if (!ctx || !doc) {
        return false;
    }
    pdf_obj* info = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Info));
    if (!info) {
        return false;
    }
    static const char* kLatexNeedles[] = {
        "latex",  "pdflatex", "xelatex",   "lualatex", "tex live", "texlive", "hyperref",
        "pdfTeX", "pdfTex",   "xdvipdfmx", "xetex",    "luatex",   "context", "puppeteer",
        "typst",  "tectonic", "groff",     "ps2pdf",   "dvips",
    };
    return PdfDarkModeInfoFieldsMatchAnyI(ctx, info, kLatexNeedles, dimof(kLatexNeedles));
}

// Scanned books packaged as one image per page (DuXiu / Pdg2Pic / CAJ). Prefer the
// cheap whole-tile bitmap recolor path — not FollowTheme wrap / ProcessScanPixmap.
bool PdfDarkModePdfMetadataSuggestsFullPageScanDoc(fz_context* ctx, pdf_document* doc) {
    if (!ctx || !doc) {
        return false;
    }
    pdf_obj* info = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Info));
    if (!info) {
        return false;
    }
    static const char* kScanNeedles[] = {
        "duxiu",    "superstar", "pdg2pic",      "cajviewer",     "caj ",
        "ssreader", "ss reader", "annasarchive", "annas archive", "chaoxing",
    };
    return PdfDarkModeInfoFieldsMatchAnyI(ctx, info, kScanNeedles, dimof(kScanNeedles));
}

bool PdfDarkModePdfMetadataSuggestsLayoutPhotoDoc(fz_context* ctx, pdf_document* doc) {
    if (!ctx || !doc) {
        return false;
    }
    pdf_obj* info = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Info));
    if (!info) {
        return false;
    }
    static const char* kLayoutNeedles[] = {
        "adobe",      "indesign", "illustrator", "quark",       "scribus",    "affinity",  "publisher",
        "framemaker", "word",     "powerpoint",  "libreoffice", "openoffice", "microsoft", "acrobat",
        "foxit",      "nitro",    "finereader",  "abbyy",       "indesign",   "prince",    "antenna house",
    };
    return PdfDarkModeInfoFieldsMatchAnyI(ctx, info, kLayoutNeedles, dimof(kLayoutNeedles));
}

bool PdfDarkModePdfMetadataSuggestsPaperCaptureDoc(fz_context* ctx, pdf_document* doc) {
    if (!ctx || !doc) {
        return false;
    }
    pdf_obj* info = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Info));
    if (!info) {
        return false;
    }
    static const char* kPaperCaptureNeedles[] = {
        "paper capture", "papercapture", "abbyy", "finereader", "tesseract", "ocr ",
    };
    return PdfDarkModeInfoFieldsMatchAnyI(ctx, info, kPaperCaptureNeedles, dimof(kPaperCaptureNeedles));
}

static FollowThemeScanProbe PdfDarkModeClassifyFollowThemeProbe(const pdf_scan_probe_device* probe,
                                                                FollowThemePageProbeStats* stats) {
    DarkModeOptions opts = PdfDarkModeCurrentOptions();
    FollowThemePageProbeStats st{.textOps = probe->textOps,
                                 .imageOps = probe->imageOps,
                                 .vectorOps = probe->vectorOps,
                                 .maxImageCoverage = probe->maxImageCoverage};
    if (stats) {
        *stats = st;
    }
    if (probe->maxImageCoverage >= opts.scanImageCoverageThreshold && probe->textOps <= opts.maxTextOpsForScanPage &&
        probe->vectorOps <= opts.maxVectorOpsForScanPage && probe->textOps <= 2 && probe->vectorOps <= 2) {
        return FollowThemeScanProbe::PureScan;
    }
    if (::FollowThemePageStatsMatchBitmapRecolor(st, opts)) {
        return FollowThemeScanProbe::BitmapRecolor;
    }
    return FollowThemeScanProbe::Mixed;
}

static FollowThemeScanProbe PdfDarkModeRunFollowThemeScanProbe(fz_context* ctx, const RectF& pageBounds,
                                                               void (*run)(fz_context*, fz_device*, void*), void* arg,
                                                               FollowThemePageProbeStats* stats) {
    if (!ctx || pageBounds.IsEmpty() || !run) {
        return FollowThemeScanProbe::Mixed;
    }
    float pageArea = pageBounds.dx * pageBounds.dy;
    if (pageArea <= 0.f) {
        return FollowThemeScanProbe::Mixed;
    }

    pdf_scan_probe_device* probe = nullptr;
    fz_device* dev = nullptr;
    fz_var(dev);
    FollowThemeScanProbe result = FollowThemeScanProbe::Mixed;
    fz_try(ctx) {
        probe = fz_new_derived_device(ctx, pdf_scan_probe_device);
        dev = &probe->super;
        probe->pageArea = pageArea;
        dev->fill_image = probe_fill_image;
        dev->fill_text = probe_fill_text;
        dev->stroke_text = probe_stroke_text;
        dev->fill_path = probe_fill_path;
        dev->stroke_path = probe_stroke_path;
        dev->fill_shade = probe_fill_shade;
        dev->drop_device = probe_drop_device;
        run(ctx, dev, arg);
        fz_close_device(ctx, dev);
        result = PdfDarkModeClassifyFollowThemeProbe(probe, stats);
    }
    fz_always(ctx) {
        if (dev) {
            fz_drop_device(ctx, dev);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        result = FollowThemeScanProbe::Mixed;
    }
    return result;
}

static void probe_run_page(fz_context* ctx, fz_device* dev, void* arg) {
    fz_page* page = (fz_page*)arg;
    fz_run_page(ctx, page, dev, fz_identity, nullptr);
}

static void probe_run_list(fz_context* ctx, fz_device* dev, void* arg) {
    struct ProbeListArg {
        fz_display_list* list;
        fz_rect rect;
    };
    ProbeListArg* a = (ProbeListArg*)arg;
    fz_run_display_list(ctx, a->list, dev, fz_identity, a->rect, nullptr);
}

FollowThemeScanProbe PdfDarkModeProbeFollowThemeScanPage(fz_context* ctx, fz_page* page, const RectF& pageBounds,
                                                         FollowThemePageProbeStats* stats) {
    if (!page) {
        return FollowThemeScanProbe::Mixed;
    }
    return PdfDarkModeRunFollowThemeScanProbe(ctx, pageBounds, probe_run_page, page, stats);
}

FollowThemeScanProbe PdfDarkModeProbeFollowThemeScanList(fz_context* ctx, fz_display_list* list,
                                                         const RectF& pageBounds, FollowThemePageProbeStats* stats) {
    if (!list) {
        return FollowThemeScanProbe::Mixed;
    }
    struct ProbeListArg {
        fz_display_list* list;
        fz_rect rect;
    } arg{list, ToFzRect(pageBounds)};
    return PdfDarkModeRunFollowThemeScanProbe(ctx, pageBounds, probe_run_list, &arg, stats);
}
