/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/FreeTextMru.h"
#include "utils/UtAssert.h"

static void ExpectMru(const char* stored, const char* font, const char* expected) {
    char* got = FreeTextMruPush(stored, font);
    utassert(str::Eq(got ? got : "", expected));
    str::Free(got);
}

static void FreeTextMruTestPush() {
    ExpectMru(nullptr, "Arial", "Arial");
    ExpectMru("Arial", "Times New Roman", "Times New Roman|Arial");
    ExpectMru("Times New Roman|Arial", "Courier New", "Courier New|Times New Roman|Arial");
    ExpectMru("Courier New|Times New Roman|Arial", "Georgia", "Georgia|Courier New|Times New Roman");
    ExpectMru("Georgia|Courier New|Times New Roman", "Courier New", "Courier New|Georgia|Times New Roman");
    ExpectMru("Courier New|Georgia|Times New Roman", "courier new", "courier new|Georgia|Times New Roman");
    ExpectMru("Arial", "", "Arial");
    ExpectMru("Arial", "Bad|Name", "Arial");
}

void FreeTextMru_UnitTests() {
    FreeTextMruTestPush();
}
