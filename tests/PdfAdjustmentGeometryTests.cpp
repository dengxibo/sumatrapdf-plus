#include "../src/PdfAdjustmentGeometry.h"
#include <assert.h>
#include <stdio.h>

int main() {
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
    printf("Shared saved/preview crop geometry: %d angle/size cases passed\n", cases);
}
