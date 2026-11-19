/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/UtAssert.h"
#include "EdgeSpeechResponse.h"
#include "EdgeSpeechTransport.h"

void EdgeSpeechResponse_UnitTests() {
    EdgeSpeechResponse response("中文 hello hello");
    const char audio[] = "\x00\x0cPath:audio\r\n\xff\x00\x01";
    utassert(response.Receive(true, audio, sizeof(audio) - 1) == EdgeSpeechMessage::Audio);
    utassert(response.mp3.Size() == 3 && (u8)response.mp3.at(0) == 0xff && response.mp3.at(1) == 0);
    utassert(response.Receive(true, audio, 4) == EdgeSpeechMessage::Invalid);
    utassert(response.mp3.Size() == 3);

    const char metadata[] =
        "Path:audio.metadata\r\n\r\n"
        R"({"Metadata":[{"Type":"WordBoundary","Data":{"Offset":100,"text":{"Text":"中文"}}},)"
        R"({"Data":{"text":{"Text":"hello"},"Offset":200},"Type":"WordBoundary"},)"
        R"({"Type":"WordBoundary","Data":{"Offset":300,"text":{"Text":"hello"}}}]})";
    utassert(response.Receive(false, metadata, sizeof(metadata) - 1) == EdgeSpeechMessage::Metadata);
    utassert(response.words.Size() == 3);
    utassert(response.words.At(0).start == 0 && response.words.At(0).end == 6);
    utassert(response.words.At(1).start == 7 && response.words.At(1).end == 12);
    utassert(response.words.At(2).start == 13 && response.words.At(2).offset100ns == 300);
    const char end[] = "X-RequestId:example\r\nPath:turn.end\r\n\r\n";
    utassert(response.Receive(false, end, sizeof(end) - 1) == EdgeSpeechMessage::End);

    EdgeSpeechTurn request;
    request.resource = L"/consumer/speech/synthesize/readaloud/edge/v1-other";
    request.configMessage = "config";
    request.ssmlMessage = "ssml";
    utassert(EdgeSpeechReceiveTurn(request, nullptr, response).error == ERROR_INVALID_PARAMETER);
    HANDLE cancelled = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    utassert(cancelled != nullptr);
    if (cancelled) {
        request.resource = L"/consumer/speech/synthesize/readaloud/edge/v1";
        utassert(EdgeSpeechReceiveTurn(request, cancelled, response).error == ERROR_CANCELLED);
        CloseHandle(cancelled);
    }
    utassert(EdgeSpeechSynthesize(request.resource, nullptr, "sample", "bad\"voice", 1.0f, nullptr, response).error ==
             ERROR_INVALID_PARAMETER);
    utassert(EdgeSpeechSynthesize(request.resource, nullptr, "invalid\x01", "en-US-EmmaMultilingualNeural", 1.0f,
                                  nullptr, response)
                 .error == ERROR_INVALID_PARAMETER);
}
