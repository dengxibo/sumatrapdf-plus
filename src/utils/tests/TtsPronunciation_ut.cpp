/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"

#include "TtsPronunciation.h"

#include "utils/UtAssert.h"

static const char* kSamplePronJson = R"JSON(
{
  "entries": [
    { "from": "重庆", "to": "崇庆", "wholeWord": true },
    { "from": "重要", "to": "重要", "wholeWord": true },
    { "from": "银行", "to": "银杭", "wholeWord": true },
    { "from": "行走", "to": "形走", "wholeWord": true },
    { "from": "音乐", "to": "阴乐", "wholeWord": true },
    { "from": "快乐", "to": "快乐", "wholeWord": true },
    { "from": "单老师", "to": "善老师", "wholeWord": true },
    { "from": "重", "to": "众", "wholeWord": true }
  ]
}
)JSON";

void TtsPronunciation_UnitTests() {
    TtsPronunciationClear();
    utassert(TtsPronunciationEntryCount() == 0);

    utassert(TtsPronunciationLoadFromJson(kSamplePronJson));
    utassert(TtsPronunciationEntryCount() == 8);
    // Longest-first: "重庆" before "重"
    utassert(str::Len(TtsPronunciationEntriesForTest()[0].from) >= str::Len(TtsPronunciationEntriesForTest()[1].from));

    Vec<int> map;
    char* spoken = TtsPronunciationApply("去重庆看重要的银行，边行走边听音乐真快乐。单老师来了。", &map);
    defer {
        str::Free(spoken);
    };
    utassert(spoken);
    // 重庆 → 崇庆 (not 众庆 from single-char 重)
    utassert(str::Find(spoken, "崇庆"));
    utassert(!str::Find(spoken, "众庆"));
    utassert(str::Find(spoken, "重要"));
    utassert(str::Find(spoken, "银杭"));
    utassert(str::Find(spoken, "形走"));
    utassert(str::Find(spoken, "阴乐"));
    utassert(str::Find(spoken, "快乐"));
    utassert(str::Find(spoken, "善老师"));

    // Identity when empty dict
    TtsPronunciationClear();
    char* same = TtsPronunciationApply("重庆银行", &map);
    defer {
        str::Free(same);
    };
    utassert(str::Eq(same, "重庆银行"));
    utassert(map.Size() == (int)str::Len("重庆银行") + 1);

    // wholeWord Latin: "row" inside "crowd" should not match if wholeWord
    TtsPronunciationClear();
    utassert(TtsPronunciationLoadFromJson("{\"entries\":[{\"from\":\"row\",\"to\":\"XXX\",\"wholeWord\":true}]}"));
    char* crowd = TtsPronunciationApply("crowd", nullptr);
    defer {
        str::Free(crowd);
    };
    utassert(str::Eq(crowd, "crowd"));
    char* row = TtsPronunciationApply("a row here", nullptr);
    defer {
        str::Free(row);
    };
    utassert(str::Find(row, "XXX"));

    TtsPronunciationClear();

    // Pinyin → SAPI phoneme (dictionary polyphone speaker)
    char* ph1 = TtsPinyinToSapiPh("qiào");
    defer {
        str::Free(ph1);
    };
    utassert(ph1 && str::Eq(ph1, "qiao 4"));
    char* ph2 = TtsPinyinToSapiPh("shāo");
    defer {
        str::Free(ph2);
    };
    utassert(ph2 && str::Eq(ph2, "shao 1"));
    char* ph3 = TtsPinyinToSapiPh("qiao4");
    defer {
        str::Free(ph3);
    };
    utassert(ph3 && str::Eq(ph3, "qiao 4"));
    char* ph4 = TtsPinyinToSapiPh("yín háng");
    defer {
        str::Free(ph4);
    };
    utassert(ph4 && str::Eq(ph4, "yin 2 hang 2"));
    char* ph5 = TtsPinyinToSapiPh(".de");
    defer {
        str::Free(ph5);
    };
    utassert(ph5 && str::Eq(ph5, "de 5"));
    char* ph6 = TtsPinyinToSapiPh("dì");
    defer {
        str::Free(ph6);
    };
    utassert(ph6 && str::Eq(ph6, "di 4"));
}
