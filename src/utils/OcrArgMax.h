/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#pragma once

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__)
#include <emmintrin.h>
#endif

// CTC chooses the first maximum, including ties. Preserve the scalar comparison
// semantics for NaNs as well: a NaN in row[0] keeps class zero, others are ignored.
inline int OcrArgMax(const float* row, int count) {
    int best = 0;
    float bestValue = row[0];
    int c = 1;
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__)
    __m128 values = _mm_set1_ps(bestValue);
    __m128i indices = _mm_setzero_si128();
    __m128i positions = _mm_setr_epi32(1, 2, 3, 4);
    for (; c + 3 < count; c += 4) {
        __m128 next = _mm_loadu_ps(row + c);
        __m128 greater = _mm_cmpgt_ps(next, values);
        values = _mm_or_ps(_mm_and_ps(greater, next), _mm_andnot_ps(greater, values));
        __m128i mask = _mm_castps_si128(greater);
        indices = _mm_or_si128(_mm_and_si128(mask, positions), _mm_andnot_si128(mask, indices));
        positions = _mm_add_epi32(positions, _mm_set1_epi32(4));
    }
    float laneValues[4];
    int laneIndices[4];
    _mm_storeu_ps(laneValues, values);
    _mm_storeu_si128((__m128i*)laneIndices, indices);
    for (int lane = 0; lane < 4; lane++) {
        if (laneValues[lane] > bestValue || (laneValues[lane] == bestValue && laneIndices[lane] < best)) {
            bestValue = laneValues[lane];
            best = laneIndices[lane];
        }
    }
#endif
    for (; c < count; c++) {
        if (row[c] > bestValue) {
            bestValue = row[c];
            best = c;
        }
    }
    return best;
}
