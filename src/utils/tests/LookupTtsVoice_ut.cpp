/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "LookupTtsVoice.h"
#include "utils/UtAssert.h"

void LookupTtsVoice_UnitTests() {
    LookupTtsVoicePrefs p;
    p.multilingual = "brian-multi";
    p.localZh = "xiaoxiao";
    p.localEn = "zira";
    p.onlineZh = "online-xiaoxiao";
    p.onlineEn = "online-aria";

    p.mode = "online:multilingual";
    utassert(str::Eq(LookupPickVoiceId(p, true), "brian-multi"));
    utassert(str::Eq(LookupPickVoiceId(p, false), "brian-multi"));

    p.mode = "smart:zh-en";
    utassert(str::Eq(LookupPickVoiceId(p, true), "xiaoxiao"));
    utassert(str::Eq(LookupPickVoiceId(p, false), "zira"));

    p.mode = "smart-online:zh-en";
    utassert(str::Eq(LookupPickVoiceId(p, true), "online-xiaoxiao"));
    utassert(str::Eq(LookupPickVoiceId(p, false), "online-aria"));

    p.mode = "";
    utassert(LookupPickVoiceId(p, true) && LookupPickVoiceId(p, true)[0] == 0);
    utassert(LookupPickVoiceId(p, false) && LookupPickVoiceId(p, false)[0] == 0);

    p.mode = nullptr;
    utassert(LookupPickVoiceId(p, false) && LookupPickVoiceId(p, false)[0] == 0);

    p.mode = "huihui";
    utassert(str::Eq(LookupPickVoiceId(p, true), "huihui"));
    utassert(str::Eq(LookupPickVoiceId(p, false), "huihui"));

    p.mode = "online:multilingual";
    p.multilingual = nullptr;
    utassert(LookupPickVoiceId(p, true) == nullptr);
    utassert(LookupPickVoiceId(p, false) == nullptr);

    p.mode = "smart:zh-en";
    p.localEn = nullptr;
    utassert(str::Eq(LookupPickVoiceId(p, true), "xiaoxiao"));
    utassert(LookupPickVoiceId(p, false) == nullptr);

    p.mode = "smart-online:zh-en";
    p.onlineZh = "";
    utassert(LookupPickVoiceId(p, true) == nullptr);
    utassert(str::Eq(LookupPickVoiceId(p, false), "online-aria"));
}
