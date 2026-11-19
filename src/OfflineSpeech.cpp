#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/Log.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "AppTools.h"
#include "OfflineSpeech.h"
#include "../ext/tts-hd/InProcessSpeech.h"
#include <mmreg.h>

const OfflineVoice gOfflineVoices[] = {
    {"offline:yunyi-hd", "yunyi-hd", "Yunyi HD", "zh-CN", "Microsoft Yunyi (Natural HD) - Chinese (Simplified, China)",
     293583275, "AA01993F1FEE3B51931B2FCD1A23286C00DCC8CC3A7D6D0A1D7A638F7DDA01D6", "9N2GLLQFPHSG",
     "MicrosoftWindows.Voice.zh-CN.YunyiHD.1_1.0.1.0_x64__cw5n1h2txyewy_7d95064e-e335-4125-a7f9-256094d5b392.msix"},
    {"offline:andrew-hd", "andrew-hd", "Andrew HD", "en-US", "Microsoft Andrew (Natural HD) - English (United States)",
     251845442, "659BDDCEBCE6AFC9D6CE356072BC86FA9DEBE32775BE1214B9C77FAE69FD6A1F", "9NC8KRJL426K",
     "MicrosoftWindows.Voice.en-US.AndrewHD.1_1.0.4.0_x64__cw5n1h2txyewy_17a53d3d-2bfd-4646-8ca7-0017289fa411.msix"},
    {"offline:ava-hd", "ava-hd", "Ava HD", "en-US", "Microsoft Ava (Natural HD) - English (United States)", 251494062,
     "1800E88F9F621B21E6EB82B71D76D52A94D7788D61AFD224E595A3E4DE74AC7F", "9NLWD5DK4Q6X",
     "MicrosoftWindows.Voice.en-US.AvaHD.1_1.0.3.0_x64__cw5n1h2txyewy_334b0bd1-48bf-4d71-81ee-6a0cdf6f049a.msix"},
    {"offline:xiaoxiao", "xiaoxiao", "Xiaoxiao", "zh-CN", "Microsoft Xiaoxiao (Natural) - Chinese (Simplified, China)",
     53584926, "1987D94005B64FBBDF334365AC84B5C45F4BE751AA39B41EC3356BB42A9F3B8F", "9NLCMWX9ZZJZ",
     "MicrosoftWindows.Voice.zh-CN.Xiaoxiao.1_1.0.10.0_x64__cw5n1h2txyewy_3c42aed5-e913-4370-9284-737b089b1548.msix"},
    {"offline:xiaoxiao-hd", "xiaoxiao-hd", "Xiaoxiao HD", "zh-CN",
     "Microsoft Xiaoxiao (Natural HD) - Chinese (Simplified, China)", 288279487,
     "A0066079E086E922BBF86C0D811690CB51072EAD9CB50DB84F539821B2AED85D", "9MX56K82J9CM",
     "MicrosoftWindows.Voice.zh-CN.XiaoxiaoHD.1_1.0.1.0_x64__cw5n1h2txyewy_9ebe05d6-b288-48c7-b634-480c8e7035d5.msix"},
    {"offline:yunxi", "yunxi", "Yunxi", "zh-CN", "Microsoft Yunxi (Natural) - Chinese (Simplified, China)", 53615020,
     "626EAAE27BD5D6F2477165FE9AE7FDC8D9766F31CBFC125CFA4A8B92B4B89F92", "9NTHZKRRFGHD",
     "MicrosoftWindows.Voice.zh-CN.Yunxi.1_1.0.5.0_x64__cw5n1h2txyewy_e2d28517-151f-4767-b94f-8f968211c76d.msix"},
    {"offline:neerja", "neerja", "Neerja", "en-IN", "Microsoft Neerja (Natural) - English (India)", 33340051,
     "895600ED3344D63F8AC0781AE115A9D1488A50A96561F6A31E6D4FC7D68BCA2E", "9PCJ06J1J311",
     "MicrosoftWindows.Voice.en-IN.Neerja.1_1.0.6.0_x64__cw5n1h2txyewy_4fb2403b-a469-43d7-afc9-ff9e725dfce3.msix"},
    {"offline:prabhat", "prabhat", "Prabhat", "en-IN", "Microsoft Prabhat (Natural) - English (India)", 33305419,
     "2E8A389B8C6A7B230220D29FC7FD640EF5868B9FAF47CD76BDF8078ED6CA1CE3", "9PMB6HGFBWDD",
     "MicrosoftWindows.Voice.en-IN.Prabhat.1_1.0.3.0_x64__cw5n1h2txyewy_d609668a-ae45-4aa4-8af5-45a5d264e86a.msix"},
    {"offline:ryan", "ryan", "Ryan", "en-GB", "Microsoft Ryan (Natural) - English (United Kingdom)", 20561728,
     "A9AC7762FE609027C70B103541D2586DB96E61A0166948DD816026E397270519", "9MT2574707FH",
     "MicrosoftWindows.Voice.en-GB.Ryan.1_1.0.4.0_x64__cw5n1h2txyewy_d99baf15-6c64-4d13-8843-40ed716c4654.msix"},
    {"offline:sonia", "sonia", "Sonia", "en-GB", "Microsoft Sonia (Natural) - English (United Kingdom)", 20585815,
     "CB959805CFFDE11E9B54DDDDE8370158370EF22578A86E8F11B8B842D98B860F", "9NGZ44VVJ0T8",
     "MicrosoftWindows.Voice.en-GB.Sonia.1_1.0.4.0_x64__cw5n1h2txyewy_5d3fb457-88de-4733-b486-9c80b22ea233.msix"},
    {"offline:aria", "aria", "Aria (v2)", "en-US", "Microsoft Aria (Natural) - English (United States)", 15507189,
     "40459F29AB5A74642A024012B3144A021D0EE4FEF7081881232652AB103C443A", "9MT3MDCCXWRT",
     "MicrosoftWindows.Voice.en-US.Aria.2_1.0.2.0_x64__cw5n1h2txyewy_597c3c7f-2ca6-421c-9a32-9cb2a5893a11.msix"},
    {"offline:aria-v1", "aria-v1", "Aria (v1)", "en-US", "Microsoft Aria (Natural) - English (United States)", 12758504,
     "802F8C17E64FACED202789BDD9441AB4E82631F83FC3773C87464C8C7BBDF376", "9NL69Z8MT4WT",
     "MicrosoftWindows.Voice.en-US.Aria.1_1.0.8.0_x64__cw5n1h2txyewy_cd7fd2ef-8e30-4120-b3f3-76ad5cb0284e.msix"},
    {"offline:guy", "guy", "Guy (v2)", "en-US", "Microsoft Guy(Natural) - English (United States)", 15499218,
     "582946F415EF375E754E8C11C486A0CAC4B805F517EC64B35E8B9088F7299709", "9PC9NR8V4BFG",
     "MicrosoftWindows.Voice.en-US.Guy.2_1.0.2.0_x64__cw5n1h2txyewy_e510efbc-731c-465c-960d-780bbdc7d723.msix"},
    {"offline:guy-v1", "guy-v1", "Guy (v1)", "en-US", "Microsoft Guy (Natural) - English (United States)", 12565513,
     "E9CB0CA9132DFA08EDB7975CCC1C29A31836673CC7279579DAC496085E17077B", "9NKKZTPST9PC",
     "MicrosoftWindows.Voice.en-US.Guy.1_1.0.5.0_x64__cw5n1h2txyewy_1ed8882d-f360-4e2c-9133-6878db4a06f0.msix"},
    {"offline:jenny", "jenny", "Jenny (v2)", "en-US", "Microsoft Jenny(Natural) - English (United States)", 15488935,
     "60E58067EF346EA916A4BA163EC53221AEC1B93863B5ADC95050A2E8E21691F0", "9PFS84F4W50P",
     "MicrosoftWindows.Voice.en-US.Jenny.2_1.0.2.0_x64__cw5n1h2txyewy_83752d5a-9f84-428f-a2e5-3855e3f61de2.msix"},
    {"offline:jenny-v1", "jenny-v1", "Jenny (v1)", "en-US", "Microsoft Jenny (Natural) - English (United States)",
     12013412, "78EC1EE7D73059EAF7595B83D7AB3E32669558766ABF3D502FD576C9EBF974EC", "9NFLWLX5G2HQ",
     "MicrosoftWindows.Voice.en-US.Jenny.1_1.0.8.0_x64__cw5n1h2txyewy_8507f217-7b12-4c5a-bd08-1615315d3137.msix"},
    {"offline:denise", "denise", "Denise", "fr-FR", "Microsoft Denise (Natural) - French (France)", 11394143,
     "4A398D7C3839F1D741DFEA1781A376E2B7088153136B2031B183EEC8E8F89194", "9PC6SZHM7JXN",
     "MicrosoftWindows.Voice.fr-FR.Denise.1_1.0.3.0_x64__cw5n1h2txyewy_bca5b671-02b2-46dc-bc69-75d9d89a133d.msix"},
    {"offline:henri", "henri", "Henri", "fr-FR", "Microsoft Henri (Natural) - French (France)", 11347278,
     "6F16555255F7F8368826E0669582D273C7731E4B2DDF6279524D4990F13249B5", "9N1PBWRGLP1L",
     "MicrosoftWindows.Voice.fr-FR.Henri.1_1.0.3.0_x64__cw5n1h2txyewy_02aa7880-f9a0-4601-b656-0d71fad78dd0.msix"},
    {"offline:remy-hd", "remy-hd", "Rémy HD", "fr-FR", "Microsoft Remy (Natural HD) - French (France)", 244368319,
     "1869B2073AF6F047635982053183A6B1D93A873BD6CB7C8E42A1B34E5453B108", "9P72TBJZS7W7",
     "MicrosoftWindows.Voice.fr-FR.RemyHD.1_1.0.1.0_x64__cw5n1h2txyewy_63929ed3-84c0-4fb4-afd0-c0afe4b6e769.msix"},
    {"offline:vivienne-hd", "vivienne-hd", "Vivienne HD", "fr-FR", "Microsoft Vivienne (Natural HD) - French (France)",
     244395958, "5D605F77815F33CC97B2BF13DBCBECBCD5140AC1AA3BFEB689CB1B96DD6C8E7D", "9N0N4LFGS57L",
     "MicrosoftWindows.Voice.fr-FR.VivienneHD.1_1.0.1.0_x64__cw5n1h2txyewy_fc48b2c9-78cd-47ec-876f-923c2def8636.msix"},
    {"offline:conrad", "conrad", "Conrad", "de-DE", "Microsoft Conrad (Natural) - German (Germany)", 13423609,
     "5791E9D23C7737AC4D16E2618C44F1CB770C90D8F1D09D281439DCC01B8EEF44", "9N8KX9CTV81V",
     "MicrosoftWindows.Voice.de-DE.Conrad.1_1.0.4.0_x64__cw5n1h2txyewy_1a6043e1-153d-4778-93e2-4b812c33ab64.msix"},
    {"offline:katja", "katja", "Katja", "de-DE", "Microsoft Katja (Natural) - German (Germany)", 13475365,
     "EA770966034D92C094F6F3EECD1E881F6A660BE9F5AAB6905BE4652139C65A5A", "9PM0J5R14Z1M",
     "MicrosoftWindows.Voice.de-DE.Katja.1_1.0.4.0_x64__cw5n1h2txyewy_53b78bfc-8c61-4e43-934e-17298f0e5a1c.msix"},
    {"offline:keita", "keita", "Keita", "ja-JP", "Microsoft Keita (Natural) - Japanese (Japan)", 22664001,
     "0F6B736AD7DF9D6504CBDD68A2651915B1E50659598EC47AF065A925BC9CF91A", "9NHK23L2Z30D",
     "MicrosoftWindows.Voice.ja-JP.Keita.1_1.0.5.0_x64__cw5n1h2txyewy_31a7daed-2986-4aea-b2b0-9f63a55cdfc0.msix"},
    {"offline:nanami", "nanami", "Nanami", "ja-JP", "Microsoft Nanami (Natural) - Japanese (Japan)", 22799424,
     "ED2BFFD888E280DA030686DD19490544121052C782C5F9868843D09687B42823", "9N6LVRVCXSXF",
     "MicrosoftWindows.Voice.ja-JP.Nanami.1_1.0.5.0_x64__cw5n1h2txyewy_62412160-baef-4400-8c73-c5004a07b13b.msix"},
    {"offline:injoon", "injoon", "InJoon", "ko-KR", "Microsoft InJoon (Natural) - Korean (Korea)", 12683818,
     "E49E0F0B4AFC93BEA39FB288847AB08BD89FF1B37891ECFA8BF634E4ADB3D419", "9PLQGVDCQZJS",
     "MicrosoftWindows.Voice.ko-KR.InJoon.1_1.0.3.0_x64__cw5n1h2txyewy_5b72169e-e96d-43eb-b0a3-8116513d4402.msix"},
    {"offline:sunhi", "sunhi", "SunHi", "ko-KR", "Microsoft SunHi (Natural) - Korean (Korea)", 12668887,
     "2FA0FDCB2D19B1EFC79151829371459B7F2B70569BC17E058B64C9921B3D4592", "9P8064X7M9GB",
     "MicrosoftWindows.Voice.ko-KR.SunHi.1_1.0.3.0_x64__cw5n1h2txyewy_dcd18b6b-b338-43de-98b9-916875ac39db.msix"},
    {"offline:antonio", "antonio", "Antonio", "pt-BR", "Microsoft Antonio (Natural) - Portuguese (Brazil)", 12194511,
     "C1FA8D928AA6984E94F29FF075960F2CC894B19D6DA4F13465F6B7341BD99D9C", "9NG7FQSCKH1N",
     "MicrosoftWindows.Voice.pt-BR.Antonio.1_1.0.3.0_x64__cw5n1h2txyewy_d071def5-1e9d-4155-8065-5e52c50f159e.msix"},
    {"offline:francisca", "francisca", "Francisca", "pt-BR", "Microsoft Francisca (Natural) - Portuguese (Brazil)",
     12347374, "68BCEAFCA1C1C37921D66D2DB1AD3D5B750B153DB045BD5FA4FAAF696FC37707", "9P48F19GVB2K",
     "MicrosoftWindows.Voice.pt-BR.Francisca.1_1.0.3.0_x64__cw5n1h2txyewy_c23bc4e5-36a2-4794-aa24-bff1345846db.msix"},
    {"offline:dalia", "dalia", "Dalia", "es-MX", "Microsoft Dalia (Natural) - Spanish (Mexico)", 11085135,
     "BE346F9177B0942FA4BB995D4C2BE0CBFA1DF7C5E91DF8DC9F9902D36D93C42D", "9PJB6PTLJPXZ",
     "MicrosoftWindows.Voice.es-MX.Dalia.1_1.0.4.0_x64__cw5n1h2txyewy_295b210c-2bf5-41ea-bff7-5071d50470a5.msix"},
    {"offline:jorge", "jorge", "Jorge", "es-MX", "Microsoft Jorge (Natural) - Spanish (Mexico)", 11116400,
     "C6DF28B16374ECF5E06D99253FED50B32BE081C563585BA12F989EFBC6E77150", "9PBMW06CDXK3",
     "MicrosoftWindows.Voice.es-MX.Jorge.1_1.0.3.0_x64__cw5n1h2txyewy_27b61afb-0a29-41ab-af4a-5c63a293e6f3.msix"},
    {"offline:alvaro", "alvaro", "Alvaro", "es-ES", "Microsoft Alvaro (Natural) - Spanish (Spain)", 11468010,
     "5DD741F1651844C067F1CD0BC8276D73B9F1FEBBC0ED9AC4D6C661EE41DCCFED", "9PDFS4QS9C7X",
     "MicrosoftWindows.Voice.es-ES.Alvaro.1_1.0.4.0_x64__cw5n1h2txyewy_822bb950-19a0-49c6-92b9-70857c71b285.msix"},
    {"offline:elvira", "elvira", "Elvira", "es-ES", "Microsoft Elvira (Natural) - Spanish (Spain)", 11466094,
     "0E98A6ECD465B4ADC80E36724BD2CA88B046E226F7FB99CD3C0CAC218CD559C3", "9P185BSJ28NG",
     "MicrosoftWindows.Voice.es-ES.Elvira.1_1.0.4.0_x64__cw5n1h2txyewy_915035e4-4fbe-4b47-a127-9404805c73bb.msix"},
};

const OfflineVoice* FindOfflineVoice(const char* id) {
    for (const auto& voice : gOfflineVoices) {
        if (voice.id && str::Eq(voice.id, id)) {
            return &voice;
        }
    }
    return nullptr;
}

static TempStr OfflineSpeechModelDir(const OfflineVoice& voice) {
    TempStr model = path::JoinTemp(GetSelfExeDirTemp(), "tts", voice.directory);
    if (file::Exists(path::JoinTemp(model, "Tokens.xml"))) {
        return model;
    }
    model = path::JoinTemp(GetPathInAppDataDirTemp("tts"), voice.directory);
    return file::Exists(path::JoinTemp(model, "Tokens.xml")) ? model : nullptr;
}

bool OfflineSpeechInstalled(const OfflineVoice& voice) {
    return voice.directory && OfflineSpeechModelDir(voice);
}

bool OfflineSpeechAvailable(const OfflineVoice& voice) {
    return OfflineSpeechInstalled(voice);
}

static void ReleaseOfflineSpeechSession(const OfflineVoice& voice);

bool RemoveOfflineSpeech(const OfflineVoice& voice) {
    // Only the catalog's model folder is removed. Both install roots may contain a copy.
    if (!voice.directory || FindOfflineVoice(voice.id) != &voice) {
        return false;
    }
    ReleaseOfflineSpeechSession(voice);
    for (int i = 0; i < 2; i++) {
        TempStr model = OfflineSpeechModelDir(voice);
        if (!model) {
            return true;
        }
        DWORD attributes = GetFileAttributesW(ToWStrTemp(model));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !file::DeleteFileToTrash(model) || dir::Exists(model)) {
            return false;
        }
    }
    return !OfflineSpeechInstalled(voice);
}

static char* MakeOfflineWave(const StrBuilder& pcm, DWORD* waveSize) {
    if (pcm.IsEmpty() || pcm.Size() % 2) {
        return nullptr;
    }
    struct Header {
        char riff[4] = {'R', 'I', 'F', 'F'};
        DWORD riffSize = 0;
        char waveFmt[8] = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
        DWORD fmtSize = 16;
        WORD format = WAVE_FORMAT_PCM;
        WORD channels = 1;
        DWORD sampleRate = 24000;
        DWORD byteRate = 48000;
        WORD blockAlign = 2;
        WORD bits = 16;
        char data[4] = {'d', 'a', 't', 'a'};
        DWORD dataSize = 0;
    };
    static_assert(sizeof(Header) == 44);
    Header header;
    header.dataSize = (DWORD)pcm.Size();
    header.riffSize = header.dataSize + sizeof(header) - 8;
    *waveSize = header.riffSize + 8;
    char* wav = (char*)malloc(*waveSize);
    if (wav) {
        memcpy(wav, &header, sizeof(header));
        memcpy(wav + sizeof(header), pcm.Get(), pcm.size());
    }
    return wav;
}

static TempStr OfflineSpeechSsml(const OfflineVoice& voice, const char* text, float rate) {
    StrBuilder ssml;
    int ratePercent = (int)(rate * 100 + 0.5f) - 100;
    ssml.AppendFmt(
        "<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"%s\">"
        "<voice name=\"%s\"><prosody rate=\"%+d%%\">",
        voice.locale, voice.runtimeName, ratePercent);
    for (const char* p = text; *p; p++) {
        switch (*p) {
            case '&':
                ssml.Append("&amp;");
                break;
            case '<':
                ssml.Append("&lt;");
                break;
            case '>':
                ssml.Append("&gt;");
                break;
            default:
                ssml.AppendChar(*p);
                break;
        }
    }
    ssml.Append("</prosody></voice></speak>");
    return str::DupTemp(ssml.Get(), ssml.size());
}

struct OfflineSpeechSession {
    const OfflineVoice* voice = nullptr;
    void* engine = nullptr;
    void Reset() {
        if (engine) HdInProcessDestroy(engine);
        engine = nullptr;
        voice = nullptr;
    }
};
static SRWLOCK gOfflineSpeechSessionLock = SRWLOCK_INIT;
static OfflineSpeechSession gOfflineSpeechSession;

static void ReleaseOfflineSpeechSession(const OfflineVoice& voice) {
    AcquireSRWLockExclusive(&gOfflineSpeechSessionLock);
    if (gOfflineSpeechSession.voice == &voice) gOfflineSpeechSession.Reset();
    ReleaseSRWLockExclusive(&gOfflineSpeechSessionLock);
}

static bool StartOfflineSpeechSession(const OfflineVoice& voice, OfflineSpeechSession& session) {
    const char* model = OfflineSpeechModelDir(voice);
    if (!model) return false;
    WCHAR windows[MAX_PATH];
    UINT length = GetSystemWindowsDirectoryW(windows, dimof(windows));
    if (!length || length >= dimof(windows)) return false;
    const char* runtime =
        path::JoinTemp(ToUtf8Temp(windows), "SystemApps", "MicrosoftWindows.Client.Core_cw5n1h2txyewy");
    if (!file::Exists(path::JoinTemp(runtime, "SpeechSynthesizerExtension.dll"))) return false;
    HRESULT error = E_FAIL;
    ULONGLONG started = GetTickCount64();
    session.engine = HdInProcessCreate(ToWStrTemp(runtime), ToWStrTemp(model), ToWStrTemp(voice.runtimeName),
                                       ToWStrTemp(voice.locale), &error);
    if (!session.engine) {
        logf("Offline speech: in-process initialization failed, HRESULT 0x%08lx\n", (DWORD)error);
        return false;
    }
    session.voice = &voice;
    logf("Offline speech: in-process model ready after %llu ms\n", GetTickCount64() - started);
    return true;
}

static DWORD WINAPI PrepareOfflineSpeechWorker(void* context) {
    auto* voice = (const OfflineVoice*)context;
    AcquireSRWLockExclusive(&gOfflineSpeechSessionLock);
    if (gOfflineSpeechSession.voice != voice || !gOfflineSpeechSession.engine) {
        gOfflineSpeechSession.Reset();
        StartOfflineSpeechSession(*voice, gOfflineSpeechSession);
    }
    ReleaseSRWLockExclusive(&gOfflineSpeechSessionLock);
    DestroyTempAllocator();
    return 0;
}

void OfflineSpeechPrepare(const OfflineVoice& voice) {
    if (!OfflineSpeechAvailable(voice)) return;
    HANDLE worker = CreateThread(nullptr, 0, PrepareOfflineSpeechWorker, (void*)&voice, 0, nullptr);
    if (worker) CloseHandle(worker);
}

struct OfflineSpeechOutput {
    StrBuilder pcm;
    const char* text;
    const char* cursor;
    Vec<OfflineSpeechCue>* cues;
    HANDLE firstAudio;
    const OfflineSpeechStream* stream;
    bool failed = false;
};

static void ReceiveOfflinePcm(void* context, const BYTE* data, size_t size) {
    auto& output = *(OfflineSpeechOutput*)context;
    if (output.failed || !size) return;
    bool first = output.pcm.IsEmpty();
    if (output.pcm.size() + size > 16 * 1024 * 1024 || !output.pcm.Append(data, size)) {
        output.failed = true;
        return;
    }
    if (first && output.firstAudio) SetEvent(output.firstAudio);
    if (output.stream && output.stream->audio)
        output.stream->audio(output.stream->context, (const char*)data, (DWORD)size);
}

static void ReceiveOfflineWord(void* context, ULONGLONG ticks, const char* word) {
    auto& output = *(OfflineSpeechOutput*)context;
    const char* match = *word ? strstr(output.cursor, word) : nullptr;
    if (!match || ticks / 10000 > INT_MAX || output.failed) return;
    int position =
        match == output.text ? 0 : MultiByteToWideChar(CP_UTF8, 0, output.text, (int)(match - output.text), nullptr, 0);
    int timeMs = (int)(ticks / 10000);
    output.cues->Append({position, timeMs});
    if (output.stream && output.stream->word) output.stream->word(output.stream->context, position, timeMs);
    output.cursor = match + strlen(word);
}

char* OfflineSpeechSynthesize(const OfflineVoice& voice, const char* text, float rate, HANDLE cancel, DWORD* waveSize,
                              Vec<OfflineSpeechCue>& cues, HANDLE firstAudio, const OfflineSpeechStream* stream) {
    while (!TryAcquireSRWLockExclusive(&gOfflineSpeechSessionLock)) {
        if (WaitForSingleObject(cancel, 5) == WAIT_OBJECT_0) return nullptr;
    }
    defer {
        ReleaseSRWLockExclusive(&gOfflineSpeechSessionLock);
    };
    if (str::IsEmpty(text) || WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return nullptr;
    auto& session = gOfflineSpeechSession;
    if (session.voice != &voice) session.Reset();
    if (!session.engine && !StartOfflineSpeechSession(voice, session)) return nullptr;
    if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return nullptr;
    OfflineSpeechOutput output;
    output.text = output.cursor = text;
    output.cues = &cues;
    output.firstAudio = firstAudio;
    output.stream = stream;
    HdInProcessCallbacks callbacks{&output, ReceiveOfflinePcm, ReceiveOfflineWord};
    HRESULT error =
        HdInProcessSpeak(session.engine, ToWStrTemp(OfflineSpeechSsml(voice, text, rate)), cancel, &callbacks);
    if (FAILED(error) || output.failed) {
        if (error != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            logf("Offline speech: in-process synthesis failed, HRESULT 0x%08lx\n", (DWORD)error);
            session.Reset();
        }
        return nullptr;
    }
    return WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0 ? nullptr : MakeOfflineWave(output.pcm, waveSize);
}
