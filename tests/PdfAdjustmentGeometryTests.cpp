#include "../src/PdfAdjustmentGeometry.h"
#include <assert.h>
#include <stdio.h>

int main() {
    // All presets, all eight handles, four drawing directions, edge pressure.
    const float ratios[] = {0, 210.f / 297.f, 297.f / 210.f, 1, 4.f / 3.f, 3.f / 4.f};
    const int handles[] = {1, 2, 4, 8, 5, 6, 9, 10};
    for (float ratio : ratios) {
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                auto c = PdfAdjustmentRatioRect(300, 400, 300 + sx * 1000.f, 400 + sy * 1000.f, ratio, 600, 800);
                assert(PdfAdjustmentCropFits(c, 600, 800));
                if (ratio > 0) assert(fabsf(c.w / c.h - ratio) < .0001f);
            }

        if (ratio > 0) {
            PdfAdjustmentCropBounds inputs[] = {{100, 200, 200, 300}, {500, 700, 500, 700}};
            for (auto c : inputs) {
                auto fit = PdfAdjustmentFitRatio(c, ratio, 600, 800);
                assert(PdfAdjustmentCropFits(fit, 600, 800));
                assert(fabsf(fit.w / fit.h - ratio) < .0001f);
            }
        }
        PdfAdjustmentCropBounds base{200, 250, 120, ratio > 0 ? 120 / ratio : 160};
        for (int hit : handles)
            for (int delta = -900; delta <= 900; delta += 30) {
                auto c = PdfAdjustmentDragCrop(base, hit, (float)delta, (float)delta, 0, 0, 0, 0, ratio, 600, 800);
                assert(c.x >= -.001f && c.y >= -.001f && c.x + c.w <= 600.001f && c.y + c.h <= 800.001f);
                if (ratio > 0 && c.h > 0) assert(fabsf(c.w / c.h - ratio) < .0001f);
            }
        auto moved = PdfAdjustmentDragCrop(base, 16, 1000, -1000, 0, 0, 0, 0, ratio, 600, 800);
        assert(moved.w == base.w && moved.h == base.h);
        assert(PdfAdjustmentCropFits(moved, 600, 800));
    }
    PdfRotateCropState shared;
    shared.initialized = true;
    shared.box = {20, 30, 500, 700};
    shared.ratioMode = 1;
    shared.ratio = 5.f / 7.f;
    PdfRotateCropState overrides[2] = {shared, shared};
    overrides[0].pageNo = 2;
    overrides[1].pageNo = 5;
    overrides[0].box.x = 40;
    shared.box.x = 50;
    assert(PdfAdjustmentCropForPage(&shared, overrides, 2, 1)->box.x == 50);
    assert(PdfAdjustmentCropForPage(&shared, overrides, 2, 2)->box.x == 40);
    assert(PdfAdjustmentCropForPage(&shared, overrides, 2, 5)->box.x == 20);
    assert(PdfAdjustmentCropForPage(&shared, overrides, 0, 2) == &shared);
    assert(!PdfAdjustmentCropFits(shared.box, 400, 800));
    float ow, oh;
    PdfAdjustmentOutputSize(600, 800, 90, 0, false, false, false, &ow, &oh);
    assert(fabsf(ow - 800) < .001f && fabsf(oh - 600) < .001f);
    PdfAdjustmentOutputSize(600, 800, 0, 0, false, false, false, &ow, &oh);
    assert(PdfAdjustmentCropFits(shared.box, ow, oh));
    PdfAdjustmentCropBounds crop{};
    assert(PdfAdjustmentManualCrop(-100, -200, 600, 800, .25f, .125f, .5f, .75f, &crop));
    assert(crop.x == 50 && crop.y == -100 && crop.w == 300 && crop.h == 600);
    assert(PdfAdjustmentManualCrop(20, 30, 800, 600, 0, 0, 1, 1, &crop));
    assert(crop.x == 20 && crop.y == 30 && crop.w == 800 && crop.h == 600);
    assert(!PdfAdjustmentManualCrop(0, 0, 600, 800, -.1f, 0, 1, 1, &crop));
    assert(!PdfAdjustmentManualCrop(0, 0, 600, 800, .5f, 0, .6f, 1, &crop));
    assert(!PdfAdjustmentManualCrop(0, 0, 600, 800, 0, 0, 0, 1, &crop));
    assert(!PdfAdjustmentManualCrop(0, 0, 600, 800, NAN, 0, 1, 1, &crop));
    assert(!PdfAdjustmentManualCrop(0, 0, 600, 800, 0, 0, INFINITY, 1, &crop));
    assert(!PdfAdjustmentManualCrop(0, 0, 600, 800, 0, 0, 1, 1, nullptr));
    // Pixel and point mappings must select the same physical content.
    for (int i = 1; i <= 99; i++) {
        float f = i / 100.f;
        PdfAdjustmentCropBounds points{}, pixels{};
        assert(PdfAdjustmentManualCrop(-80, -20, 600, 800, 0, 0, f, f, &points));
        assert(PdfAdjustmentManualCrop(-160, -40, 1200, 1600, 0, 0, f, f, &pixels));
        assert(fabsf(pixels.x - points.x * 2) < .001f);
        assert(fabsf(pixels.y - points.y * 2) < .001f);
        assert(fabsf(pixels.w - points.w * 2) < .001f);
        assert(fabsf(pixels.h - points.h * 2) < .001f);
    }

    // Same physical box on different pages maps to distinct normalized rectangles,
    // and maps back to identical coordinates in preview pixels and saved points.
    const float pages[][2] = {{600, 800}, {612, 792}, {800, 1000}};
    for (auto& page : pages) {
        auto c = shared.box;
        PdfAdjustmentCropBounds saved{}, preview{};
        assert(PdfAdjustmentManualCrop(0, 0, page[0], page[1], c.x / page[0], c.y / page[1], c.w / page[0],
                                       c.h / page[1], &saved));
        assert(PdfAdjustmentManualCrop(40, 60, page[0] * 2, page[1] * 2, c.x / page[0], c.y / page[1], c.w / page[0],
                                       c.h / page[1], &preview));
        assert(fabsf(saved.x - c.x) < .001f && fabsf(saved.w - c.w) < .001f);
        assert(fabsf(preview.x - (40 + c.x * 2)) < .001f && fabsf(preview.h - c.h * 2) < .001f);
    }
    for (int angle = -180; angle <= 180; angle++)
        for (int flip = 0; flip < 4; flip++) {
            PdfAdjustmentOutputSize(600, 800, (float)angle, 2.5f, flip & 1, flip & 2, false, &ow, &oh);
            float rad = (angle + ((flip == 1 || flip == 2) ? -2.5f : 2.5f)) * .0174532925f;
            assert(fabsf(ow - (fabsf(600 * cosf(rad)) + fabsf(800 * sinf(rad)))) < .001f);
            assert(fabsf(oh - (fabsf(600 * sinf(rad)) + fabsf(800 * cosf(rad)))) < .001f);
        }
    float w = 123, h = 456;
    assert(!PdfAdjustmentInscribedSize(600, 800, 0, &w, &h));
    assert(w == 123 && h == 456);
    assert(!PdfAdjustmentInscribedSize(600, 800, 90, &w, &h));
    assert(!PdfAdjustmentInscribedSize(600, 800, 180, &w, &h));
    assert(!PdfAdjustmentInscribedSize(.5f, 800, 2, &w, &h));
    assert(!PdfAdjustmentInscribedSize(600, 800, .01f, &w, &h));
    float sizes[][2] = {{600, 800}, {800, 600}, {1000, 1000}, {200, 1600}, {36, 72}};
    int cases = 0;
    for (auto& size : sizes) {
        for (int tenth = -1799; tenth <= 1799; tenth++) {
            float angle = tenth / 10.f;
            float width = 0, height = 0;
            if (!PdfAdjustmentInscribedSize(size[0], size[1], angle, &width, &height)) continue;
            assert(width >= 1 && height >= 1);
            float radians = angle * .0174532925f;
            float c = cosf(radians), s = sinf(radians);
            // All crop corners must lie within the actual turned source page.
            for (int x = -1; x <= 1; x += 2) {
                for (int y = -1; y <= 1; y += 2) {
                    float sourceX = x * width * .5f * c + y * height * .5f * s;
                    float sourceY = -x * width * .5f * s + y * height * .5f * c;
                    assert(fabsf(sourceX) <= size[0] * .5f + .02f);
                    assert(fabsf(sourceY) <= size[1] * .5f + .02f);
                }
            }
            float mirroredW = 0, mirroredH = 0;
            assert(PdfAdjustmentInscribedSize(size[0], size[1], -angle, &mirroredW, &mirroredH));
            assert(fabsf(width - mirroredW) < .001f && fabsf(height - mirroredH) < .001f);
            cases++;
        }
    }
    printf("Crop ratios, eight handles, batch overrides, mapping and %d angle/size cases passed\n", cases);
}
