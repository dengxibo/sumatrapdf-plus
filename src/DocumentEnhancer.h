/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

// Display-only document enhancement (Reading / Scanned). Does not modify PDF,
// layout, OCR, selection, annotations, or hit testing. Runs on already-
// rasterized BGRA bitmaps after theme/document recolor has been baked in by
// Engine::RenderPage.

enum class DocumentEnhancementMode : int {
    Off = 0,
    Legacy = 1,  // old RGB LUT path (env / migrated FileState only)
    Reading = 2, // Mild — light contrast/sharpen
    Scanned = 3, // Strong — wash gray ink / uneven paper
    Auto = 4,    // pick Mild or Strong per tile from page stats
};

struct DocumentEnhancementParams {
    DocumentEnhancementMode mode = DocumentEnhancementMode::Off;

    // UI adjustment relative to preset baseline (-100..100 / 0..100).
    float brightness = 0.f;
    float contrast = 0.f;
    float sharpness = 0.f;

    // Internal preset knobs (filled by Get*Preset / BuildEnhancementParams).
    float backgroundStrength = 0.f;
    float backgroundTarget = 200.f;

    float blackPointStrength = 0.f;
    float whitePointStrength = 0.f;

    float gamma = 1.f;
    float sCurveStrength = 0.f;

    float sharpenRadius = 1.f;
    float sharpenAmount = 0.f;
    float sharpenThreshold = 4.f;

    float imageProtection = 1.f;
    bool preserveColor = true;

    // When true, the bitmap is already dark-theme / match-theme recolored
    // (dark paper, light ink). Enhance in inverted luminance so Scanned/Reading
    // light-paper logic still applies, then invert back before recombine.
    bool disablePaperNormalize = false;
};

struct DocumentEnhancementStats {
    float mean = 0.f;
    float median = 0.f;
    float p1 = 0.f;
    float p5 = 0.f;
    float p95 = 0.f;
    float p99 = 0.f;
    float bright220 = 0.f;
    float bright235 = 0.f;
    float bright245 = 0.f;
    float variance = 0.f;
    float meanSaturation = 0.f;
    float documentConfidence = 0.f;
    float blackPoint = 0.f;
    float whitePoint = 255.f;
    float gamma = 1.f;
    float backgroundStrengthUsed = 0.f;
    double msTotal = 0.0;
    double msHistogram = 0.0;
    double msBackground = 0.0;
    double msTone = 0.0;
    double msSharpen = 0.0;
};

constexpr uint32_t kDocumentEnhancerVersion = 9;

DocumentEnhancementParams GetReadingPreset();
DocumentEnhancementParams GetScannedPreset();

// Combine mode preset with UI slider deltas (brightness/contrast/sharpness).
DocumentEnhancementParams BuildEnhancementParams(DocumentEnhancementMode mode, int brightness, int contrast,
                                                 int sharpness);

// While the filter panel sliders are being dragged, use a cheaper preview path
// (skip background map / denoise; lighter sharpen) so the UI stays responsive.
void SetDocumentEnhancerPreviewLite(bool lite);
bool DocumentEnhancerPreviewLite();

// Apply Reading/Scanned pipeline to a top-down 32bpp BGRA DIB. Leaves alpha
// unchanged. No-op when mode is Off or Legacy (Legacy is handled by the UI
// layer's existing LUT path).
void ApplyDocumentEnhancement(uint8_t* bgra, int width, int height, int stride, const DocumentEnhancementParams& params,
                              DocumentEnhancementStats* outStats = nullptr);

// Optional env: SUMATRA_DUMP_DOCUMENT_ENHANCER=<dir> writes stage dumps.
// Optional env: SUMATRA_DOC_ENHANCER=off|legacy|reading|scanned|auto forces mode.
// Returns true when the env var is set (including forced Off).
bool TryParseDocumentEnhancerEnvOverride(DocumentEnhancementMode* outMode);
bool DocumentEnhancerEnvForceDisablePaperNormalize();
