#include "../src/utils/OcrArgMax.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int Reference(const float* row, int count) {
    int best = 0;
    for (int c = 1; c < count; c++) {
        if (row[c] > row[best]) best = c;
    }
    return best;
}

int main() {
    float row[20000];
    unsigned state = 12345;
    // Dictionary sizes, SIMD tails, repeated maxima, and non-finite scores.
    for (int n = 1; n <= 20000; n += n < 40 ? 1 : 137) {
        for (int repeat = 0; repeat < 12; repeat++) {
            for (int c = 0; c < n; c++) {
                state = state * 1664525 + 1013904223;
                row[c] = (float)((int)(state % 71) - 35);
            }
            if (repeat == 1) row[0] = NAN;
            if (repeat == 2) row[n / 2] = NAN;
            if (repeat == 3) row[n / 2] = INFINITY;
            if (repeat == 4) row[0] = -INFINITY;
            assert(OcrArgMax(row, n) == Reference(row, n));
        }
    }
    puts("PASS: SIMD CTC argmax matches scalar, including ties and NaNs");
}
