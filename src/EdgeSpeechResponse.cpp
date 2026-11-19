/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/JsonParser.h"
#include "EdgeSpeechResponse.h"

static const size_t kMaxEdgeMessage = 1024 * 1024;
static const size_t kMaxEdgeAudio = 4 * 1024 * 1024;

static bool HasEdgePath(const char* headers, size_t size, const char* path) {
    size_t pathSize = strlen(path);
    while (size) {
        const char* end = (const char*)memchr(headers, '\n', size);
        size_t len = end ? (size_t)(end - headers) : size;
        size_t contentLen = len && headers[len - 1] == '\r' ? len - 1 : len;
        if (contentLen == pathSize && memcmp(headers, path, pathSize) == 0) {
            return true;
        }
        if (!end) {
            break;
        }
        headers = end + 1;
        size -= len + 1;
    }
    return false;
}

struct EdgeMetadataItem {
    i64 offset = -1;
    const char* text = nullptr;
    bool word = false;
};

struct EdgeMetadataVisitor : json::ValueVisitor {
    Vec<EdgeMetadataItem> items;

    bool Visit(const char* path, const char* value, json::Type type) override {
        constexpr char prefix[] = "/Metadata[";
        if (!str::StartsWith(path, prefix)) {
            return true;
        }
        int index = 0;
        const char* field = str::Parse(path + sizeof(prefix) - 1, "%d]", &index);
        if (!field || index < 0 || index >= 4096) {
            return false;
        }
        while (items.Size() <= index) {
            items.Append(EdgeMetadataItem{});
        }
        auto& item = items.At(index);
        if (type == json::Type::String && str::Eq(field, "/Type")) {
            item.word = str::Eq(value, "WordBoundary");
        } else if (type == json::Type::Number && str::Eq(field, "/Data/Offset")) {
            char* end = nullptr;
            errno = 0;
            i64 offset = _strtoi64(value, &end, 10);
            if (errno == 0 && end != value && *end == '\0' && offset >= 0) {
                item.offset = offset;
            }
        } else if (type == json::Type::String && str::Eq(field, "/Data/text/Text")) {
            item.text = str::DupTemp(value);
        }
        return true;
    }
};

EdgeSpeechMessage EdgeSpeechResponse::Receive(bool binary, const void* message, size_t size) {
    if (!message || !size || size > kMaxEdgeMessage) {
        return EdgeSpeechMessage::Invalid;
    }
    const char* data = (const char*)message;
    if (binary) {
        if (size < 2) {
            return EdgeSpeechMessage::Invalid;
        }
        size_t headerSize = ((size_t)(u8)data[0] << 8) | (u8)data[1];
        if (headerSize > size - 2 || !HasEdgePath(data + 2, headerSize, "Path:audio")) {
            return EdgeSpeechMessage::Invalid;
        }
        size_t payloadSize = size - 2 - headerSize;
        if (payloadSize > kMaxEdgeAudio - mp3.Size()) {
            return EdgeSpeechMessage::Invalid;
        }
        if (!mp3.Append(data + 2 + headerSize, payloadSize)) {
            return EdgeSpeechMessage::Invalid;
        }
        if (audioReceived && payloadSize) {
            audioReceived(audioContext, *this);
        }
        return EdgeSpeechMessage::Audio;
    }

    if (memchr(data, '\0', size)) {
        return EdgeSpeechMessage::Invalid;
    }
    const char* frame = str::DupTemp(data, size);
    const char* body = strstr(frame, "\r\n\r\n");
    if (!body) {
        return EdgeSpeechMessage::Invalid;
    }
    size_t headerSize = body - frame;
    if (HasEdgePath(frame, headerSize, "Path:turn.end")) {
        return EdgeSpeechMessage::End;
    }
    if (!HasEdgePath(frame, headerSize, "Path:audio.metadata")) {
        return EdgeSpeechMessage::Other;
    }

    EdgeMetadataVisitor visitor;
    if (!json::Parse(body + 4, &visitor)) {
        return EdgeSpeechMessage::Invalid;
    }
    for (const auto& item : visitor.items) {
        if (!text || !item.word || item.offset < 0 || str::IsEmpty(item.text)) {
            continue;
        }
        const char* match = strstr(text + textOffset, item.text);
        if (match) {
            int start = (int)(match - text);
            textOffset = start + str::Leni(item.text);
            words.Append({item.offset, start, textOffset});
        }
    }
    return EdgeSpeechMessage::Metadata;
}
