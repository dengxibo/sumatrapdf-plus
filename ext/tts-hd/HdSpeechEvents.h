#pragma once
#include <cstdint>

namespace HdSpeech {
enum class Type : uint32_t {
    Word,
    Sentence,
    Viseme,
    Bookmark
};
struct Event {
    uint64_t ticks = 0, duration = 0;
    uint32_t textOffset = 0, textLength = 0, value = 0, reserved = 0;
};
} // namespace HdSpeech
