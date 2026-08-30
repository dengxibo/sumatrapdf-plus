/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/ThreadUtil.h"
#include "utils/Timer.h"
#include "utils/WinUtil.h"

#include "OcrOnnx.h"
#include "Settings.h"
#include "GlobalPrefs.h"

#include "utils/Log.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include "../ext/onnxruntime/onnxruntime_c_api.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

void FreeOcrBoxes(Vec<OcrBox>& boxes) {
    for (auto& b : boxes) {
        str::Free(b.text);
        b.text = nullptr;
        free(b.charX);
        b.charX = nullptr;
        b.nChar = 0;
    }
    boxes.Reset();
}

static Mutex gOcrLock;
static HMODULE gOrtMod = nullptr;
static const OrtApi* gApi = nullptr;
static const OrtApiBase* gApiBase = nullptr;
static OrtEnv* gEnv = nullptr;
static OrtSessionOptions* gOpts = nullptr;
static OrtMemoryInfo* gMem = nullptr;
static OrtAllocator* gAlloc = nullptr;
static bool gRuntimeTried = false;
static bool gRuntimeOk = false;
static bool gForceProfile = false;
static OcrProfile gForcedProfile = OcrProfile::Balanced;
static bool gLoggedGenericRec = false;
static char gMissingHint[512] = {};
static char gLastError[512] = {};
static char* gOrtVersion = nullptr;

static constexpr int kOcrMaxSlots = 8;
static constexpr int kOcrProfileCount = 3;

struct OcrOrtBundle {
    OrtSession* det = nullptr;
    OrtSession* rec = nullptr;
    OrtSession* cls = nullptr;
    char* detIn = nullptr;
    char* detOut = nullptr;
    char* recIn = nullptr;
    char* recOut = nullptr;
    char* clsIn = nullptr;
    char* clsOut = nullptr;
};

struct OcrDetParams {
    int limitSideLen = 960;
    bool limitMin = false;
    int maxSideLen = 0;
    float thresh = 0.3f;
    float boxThresh = 0.5f;
    float unclipRatio = 1.6f;
    bool dilation = false;
};

struct OcrProfileState {
    bool resolveTried = false;
    bool resolved = false;
    bool loadTried = false;
    bool loadOk = false;
    OcrProfile requested = OcrProfile::Fast;
    OcrProfile actual = OcrProfile::Fast;
    char* detPath = nullptr;
    char* recPath = nullptr;
    char* dictPath = nullptr;
    char* clsPath = nullptr;
    StrVec keys;
    int recClassCount = 0;
    OcrOrtBundle bundles[kOcrMaxSlots];
    bool slotBusy[kOcrMaxSlots]{};
    int slotCount = 0;
    HANDLE slotSem = nullptr;
    double loadMs = 0;
};

static OcrProfileState gProfiles[kOcrProfileCount];
static char* gSharedClsPath = nullptr;

static const char* kClsNames[] = {"cls.onnx", "ch_ppocr_mobile_v2.0_cls_infer.onnx",
                                  "ch_ppocr_mobile_v2.0_cls_mobile.onnx", nullptr};

char* OcrSidecarDirTemp() {
    TempStr exeDir = GetSelfExeDirTemp();
    TempStr nextToExe = path::JoinTemp(exeDir, "ocr");
    if (dir::Exists(nextToExe)) {
        return nextToExe;
    }
    TempStr up = path::JoinTemp(exeDir, "..", "..");
    TempStr repoOcr = path::JoinTemp(up, "ocr");
    if (dir::Exists(repoOcr)) {
        return path::NormalizeTemp(repoOcr);
    }
    return nextToExe;
}

static bool FileInDir(const char* dir, const char* name) {
    if (!dir || !name) {
        return false;
    }
    TempStr p = path::JoinTemp(dir, name);
    return file::Exists(p);
}

static char* FindNamedFileTemp(const char* dir, const char** names) {
    for (int i = 0; names[i]; i++) {
        TempStr p = path::JoinTemp(dir, names[i]);
        if (file::Exists(p)) {
            return p;
        }
    }
    return nullptr;
}

static void SetMissingHint(const char* dir) {
    str::BufSet(gMissingHint, dimof(gMissingHint),
                "OCR models not found. Put onnxruntime.dll and PP-OCRv6 models in: ");
    str::BufAppend(gMissingHint, dimof(gMissingHint), dir ? dir : "ocr");
}

static void SetOcrError(const char* msg) {
    str::BufSet(gLastError, dimof(gLastError), msg ? msg : "");
}

const char* OcrLastError() {
    return gLastError[0] ? gLastError : nullptr;
}

const char* OcrModelsMissingHint() {
    if (!gMissingHint[0]) {
        SetMissingHint(OcrSidecarDirTemp());
    }
    return gMissingHint;
}

const char* OcrProfileName(OcrProfile profile) {
    switch (profile) {
        case OcrProfile::Fast:
            return "Fast";
        case OcrProfile::Balanced:
            return "Balanced";
        case OcrProfile::Hybrid:
            return "Hybrid";
    }
    return "Unknown";
}

void OcrSetForcedProfile(OcrProfile profile, bool enable) {
    gForceProfile = enable;
    gForcedProfile = profile;
}

static OcrProfile ParseProfileEnv(const char* s) {
    if (!s || !s[0]) {
        return OcrProfile::Fast;
    }
    if (str::EqI(s, "fast") || str::EqI(s, "tiny")) {
        return OcrProfile::Fast;
    }
    if (str::EqI(s, "balanced") || str::EqI(s, "small")) {
        return OcrProfile::Balanced;
    }
    if (str::EqI(s, "hybrid")) {
        return OcrProfile::Hybrid;
    }
    return OcrProfile::Fast;
}

OcrProfile GetOcrProfileForOperation(OcrOperation op) {
    if (gForceProfile) {
        return gForcedProfile;
    }
    char env[64]{};
    DWORD n = GetEnvironmentVariableA("SUMATRA_OCR_PROFILE", env, dimof(env));
    if (n > 0 && n < dimof(env)) {
        return ParseProfileEnv(env);
    }
    switch (op) {
        case OcrOperation::CurrentPage:
            return OcrProfile::Balanced;
        case OcrOperation::AllPages:
        case OcrOperation::SaveSearchable:
            if (gGlobalPrefs && gGlobalPrefs->ocrFullDocumentMode &&
                str::EqI(gGlobalPrefs->ocrFullDocumentMode, "accurate")) {
                return OcrProfile::Balanced;
            }
            return OcrProfile::Fast;
    }
    return OcrProfile::Balanced;
}

static OcrDetParams DetParamsFor(OcrProfile) {
    OcrDetParams p;
    // RapidOCR >= 3.9 PP-OCRv6: limit_type=min, 736; thresh/unclip/dilation
    // from config.yaml. Global.max_side_len is 2000 for camera photos.
    // PDF pages are already rasterized to ~1920; running det at that size makes
    // Small ~3.5s/page. Cap at 960 so Tiny/Hybrid stay interactive.
    // Min-side 736 still upscales small clips.
    p.limitSideLen = 736;
    p.limitMin = true;
    p.maxSideLen = 960;
    p.thresh = 0.3f;
    p.boxThresh = 0.5f;
    p.unclipRatio = 1.6f;
    p.dilation = true;
    return p;
}

static void OrtFail(OrtStatus* st, const char* what) {
    if (!st) {
        return;
    }
    const char* msg = gApi ? gApi->GetErrorMessage(st) : "";
    str::BufSet(gLastError, dimof(gLastError), what);
    str::BufAppend(gLastError, dimof(gLastError), ": ");
    str::BufAppend(gLastError, dimof(gLastError), msg ? msg : "");
    gApi->ReleaseStatus(st);
}

static int OnnxIrVersion(const char* path) {
    if (!path) {
        return 0;
    }
    ByteSlice d = file::ReadFile(path);
    if (!d || d.size() < 3) {
        if (d) {
            d.Free();
        }
        return 0;
    }
    const u8* p = (const u8*)d.data();
    int ver = 0;
    if (p[0] == 0x08) {
        ver = p[1] & 0x7f;
        if (p[1] & 0x80 && d.size() > 2) {
            ver |= (p[2] & 0x7f) << 7;
        }
    }
    d.Free();
    return ver;
}

static OrtSession* LoadSession(const char* pathUtf8) {
    if (!pathUtf8 || !gApi || !gEnv || !gOpts) {
        return nullptr;
    }
    WCHAR wpath[MAX_PATH * 2]{};
    int n = MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1, wpath, dimof(wpath));
    if (n <= 0) {
        return nullptr;
    }
    OrtSession* session = nullptr;
    OrtStatus* st = gApi->CreateSession(gEnv, wpath, gOpts, &session);
    if (st) {
        OrtFail(st, pathUtf8);
        return nullptr;
    }
    return session;
}

static int CpuLogicalCount() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    if (n < 1) {
        n = 1;
    }
    return n;
}

static int DesiredOcrSlotCount() {
    int n = CpuLogicalCount();
    if (n <= 2) {
        return 1;
    }
    int leave = (n >= 8) ? 2 : 1;
    int w = n - leave;
    if (w < 2) {
        w = 2;
    }
    if (w > kOcrMaxSlots) {
        w = kOcrMaxSlots;
    }
    return w;
}

int OcrInferenceSlotCount() {
    return DesiredOcrSlotCount();
}

static char* OrtDupName(OrtSession* session, bool input, size_t index) {
    if (!session || !gApi || !gAlloc) {
        return nullptr;
    }
    char* name = nullptr;
    OrtStatus* st = nullptr;
    if (input) {
        st = gApi->SessionGetInputName(session, index, gAlloc, &name);
    } else {
        st = gApi->SessionGetOutputName(session, index, gAlloc, &name);
    }
    if (st) {
        OrtFail(st, "SessionGetName");
        return nullptr;
    }
    char* copy = str::Dup(name);
    gApi->AllocatorFree(gAlloc, name);
    return copy;
}

static bool LogAndCacheIo(OrtSession* session, const char* tag, char** firstIn, char** firstOut, int* lastOutDim) {
    if (!session || !gApi) {
        return false;
    }
    size_t nIn = 0, nOut = 0;
    OrtStatus* st = gApi->SessionGetInputCount(session, &nIn);
    if (st) {
        OrtFail(st, "SessionGetInputCount");
        return false;
    }
    st = gApi->SessionGetOutputCount(session, &nOut);
    if (st) {
        OrtFail(st, "SessionGetOutputCount");
        return false;
    }
    logf("OcrOnnx: %s inputs=%zu outputs=%zu\n", tag, nIn, nOut);
    if (nIn < 1 || nOut < 1) {
        SetOcrError("ONNX session has no input/output");
        return false;
    }
    *firstIn = OrtDupName(session, true, 0);
    *firstOut = OrtDupName(session, false, 0);
    if (!*firstIn || !*firstOut) {
        return false;
    }

    auto logOne = [&](bool input, size_t i) {
        char* name = OrtDupName(session, input, i);
        OrtTypeInfo* ti = nullptr;
        if (input) {
            st = gApi->SessionGetInputTypeInfo(session, i, &ti);
        } else {
            st = gApi->SessionGetOutputTypeInfo(session, i, &ti);
        }
        if (st) {
            OrtFail(st, "SessionGetTypeInfo");
            str::Free(name);
            return;
        }
        const OrtTensorTypeAndShapeInfo* info = nullptr;
        st = gApi->CastTypeInfoToTensorInfo(ti, &info);
        if (st) {
            OrtFail(st, "CastTypeInfoToTensorInfo");
            gApi->ReleaseTypeInfo(ti);
            str::Free(name);
            return;
        }
        size_t rank = 0;
        gApi->GetDimensionsCount(info, &rank);
        int64_t dims[8]{};
        if (rank > dimof(dims)) {
            rank = dimof(dims);
        }
        gApi->GetDimensions(info, dims, rank);
        char buf[128]{};
        str::BufSet(buf, dimof(buf), "[");
        for (size_t d = 0; d < rank; d++) {
            char one[32]{};
            snprintf(one, dimof(one), "%s%lld", d ? "," : "", (long long)dims[d]);
            str::BufAppend(buf, dimof(buf), one);
        }
        str::BufAppend(buf, dimof(buf), "]");
        logf("OcrOnnx:   %s[%zu] name=%s shape=%s\n", input ? "in" : "out", i, name ? name : "?", buf);
        if (!input && lastOutDim && rank > 0) {
            *lastOutDim = (int)dims[rank - 1];
        }
        gApi->ReleaseTypeInfo(ti);
        str::Free(name);
    };

    for (size_t i = 0; i < nIn && i < 4; i++) {
        logOne(true, i);
    }
    for (size_t i = 0; i < nOut && i < 4; i++) {
        logOne(false, i);
    }
    return true;
}

static bool CopyBundleIoNames(const OcrOrtBundle* src, OcrOrtBundle* dst) {
    if (!src || !dst || !src->detIn || !src->detOut || !src->recIn || !src->recOut) {
        return false;
    }
    str::ReplacePtr(&dst->detIn, src->detIn);
    str::ReplacePtr(&dst->detOut, src->detOut);
    str::ReplacePtr(&dst->recIn, src->recIn);
    str::ReplacePtr(&dst->recOut, src->recOut);
    str::ReplacePtr(&dst->clsIn, src->clsIn);
    str::ReplacePtr(&dst->clsOut, src->clsOut);
    return dst->detIn && dst->detOut && dst->recIn && dst->recOut;
}

static bool CacheSessionNames(OcrOrtBundle* b, int* recClassCount) {
    if (!b || !b->det || !b->rec) {
        return false;
    }
    int detLast = 0;
    int recLast = 0;
    if (!LogAndCacheIo(b->det, "det", &b->detIn, &b->detOut, &detLast)) {
        return false;
    }
    if (!LogAndCacheIo(b->rec, "rec", &b->recIn, &b->recOut, &recLast)) {
        return false;
    }
    if (b->cls) {
        int clsLast = 0;
        LogAndCacheIo(b->cls, "cls", &b->clsIn, &b->clsOut, &clsLast);
    }
    if (recClassCount) {
        *recClassCount = recLast;
    }
    return b->detIn && b->detOut && b->recIn && b->recOut;
}

static bool EnsureBundleLocked(OcrProfileState* st, int i) {
    if (!st || i < 0 || i >= st->slotCount) {
        return false;
    }
    OcrOrtBundle* b = &st->bundles[i];
    if (b->det && b->rec) {
        return true;
    }
    if (!st->detPath || !st->recPath) {
        return false;
    }
    b->det = LoadSession(st->detPath);
    b->rec = LoadSession(st->recPath);
    if (st->clsPath) {
        b->cls = LoadSession(st->clsPath);
    }
    if (!b->det || !b->rec) {
        return false;
    }
    if (i > 0 && st->bundles[0].detIn) {
        return CopyBundleIoNames(&st->bundles[0], b);
    }
    int recClass = 0;
    if (!CacheSessionNames(b, &recClass)) {
        return false;
    }
    st->recClassCount = recClass;
    return true;
}

static int AcquireOcrSlot(OcrProfileState* st) {
    if (!st || !st->slotSem) {
        return -1;
    }
    WaitForSingleObject(st->slotSem, INFINITE);
    gOcrLock.Lock();
    int slot = -1;
    for (int i = 0; i < st->slotCount; i++) {
        if (st->slotBusy[i]) {
            continue;
        }
        if (!EnsureBundleLocked(st, i)) {
            continue;
        }
        st->slotBusy[i] = true;
        slot = i;
        break;
    }
    gOcrLock.Unlock();
    if (slot < 0) {
        ReleaseSemaphore(st->slotSem, 1, nullptr);
    }
    return slot;
}

static int TryAcquireOcrSlot(OcrProfileState* st) {
    if (!st || !st->slotSem) {
        return -1;
    }
    if (WaitForSingleObject(st->slotSem, 0) != WAIT_OBJECT_0) {
        return -1;
    }
    gOcrLock.Lock();
    int slot = -1;
    for (int i = 0; i < st->slotCount; i++) {
        if (st->slotBusy[i]) {
            continue;
        }
        if (!EnsureBundleLocked(st, i)) {
            continue;
        }
        st->slotBusy[i] = true;
        slot = i;
        break;
    }
    gOcrLock.Unlock();
    if (slot < 0) {
        ReleaseSemaphore(st->slotSem, 1, nullptr);
    }
    return slot;
}

static void ReleaseOcrSlot(OcrProfileState* st, int slot) {
    if (!st || slot < 0 || slot >= st->slotCount) {
        return;
    }
    gOcrLock.Lock();
    st->slotBusy[slot] = false;
    gOcrLock.Unlock();
    if (st->slotSem) {
        ReleaseSemaphore(st->slotSem, 1, nullptr);
    }
}

static bool LoadKeysInto(StrVec& keys, const char* path) {
    keys.Reset();
    ByteSlice d = file::ReadFile(path);
    if (!d) {
        return false;
    }
    keys.Append("");
    const char* p = (const char*)d.data();
    size_t n = d.size();
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && p[i] != '\n' && p[i] != '\r') {
            i++;
        }
        size_t len = i - start;
        if (len > 0) {
            keys.Append(p + start, len);
        }
        while (i < n && (p[i] == '\n' || p[i] == '\r')) {
            i++;
        }
    }
    d.Free();
    return keys.Size() > 8;
}

static bool InitFail(const char*) {
    return false;
}

static bool GenericPairPresent(const char* dir) {
    return FileInDir(dir, "rec.onnx") && FileInDir(dir, "keys.txt") &&
           (FileInDir(dir, "det.onnx") || FileInDir(dir, "PP-OCRv6_det_tiny.onnx") ||
            FileInDir(dir, "PP-OCRv6_det_small.onnx"));
}

static bool FastExactPresent(const char* dir) {
    return FileInDir(dir, "PP-OCRv6_det_tiny.onnx") && FileInDir(dir, "PP-OCRv6_rec_tiny.onnx") &&
           FileInDir(dir, "ppocrv6_tiny_dict.txt");
}

static bool BalancedExactPresent(const char* dir) {
    return FileInDir(dir, "PP-OCRv6_det_small.onnx") && FileInDir(dir, "PP-OCRv6_rec_small.onnx") &&
           FileInDir(dir, "ppocrv6_dict.txt");
}

static bool HybridExactPresent(const char* dir) {
    return FileInDir(dir, "PP-OCRv6_det_small.onnx") && FileInDir(dir, "PP-OCRv6_rec_tiny.onnx") &&
           FileInDir(dir, "ppocrv6_tiny_dict.txt");
}

static bool AnyOcrPairPresent(const char* dir) {
    if (FileInDir(dir, "rec.onnx") && !FileInDir(dir, "keys.txt") && !gLoggedGenericRec) {
        gLoggedGenericRec = true;
        logf("OcrOnnx: rec.onnx present but keys.txt missing; not guessing a dictionary\n");
    }
    return FastExactPresent(dir) || BalancedExactPresent(dir) || HybridExactPresent(dir) || GenericPairPresent(dir);
}

static bool FillExact(OcrProfileState* st, const char* dir, const char* det, const char* rec, const char* dict,
                      OcrProfile actual) {
    TempStr detPath = path::JoinTemp(dir, det);
    TempStr recPath = path::JoinTemp(dir, rec);
    TempStr dictPath = path::JoinTemp(dir, dict);
    str::ReplacePtr(&st->detPath, detPath);
    str::ReplacePtr(&st->recPath, recPath);
    str::ReplacePtr(&st->dictPath, dictPath);
    st->actual = actual;
    st->resolved = true;
    return true;
}

static bool FillGeneric(OcrProfileState* st, const char* dir) {
    if (!FileInDir(dir, "rec.onnx")) {
        return false;
    }
    if (!FileInDir(dir, "keys.txt")) {
        if (!gLoggedGenericRec) {
            gLoggedGenericRec = true;
            logf("OcrOnnx: rec.onnx present but keys.txt missing; not guessing a dictionary\n");
        }
        return false;
    }
    TempStr detPath = nullptr;
    if (FileInDir(dir, "det.onnx")) {
        detPath = path::JoinTemp(dir, "det.onnx");
    } else if (FileInDir(dir, "PP-OCRv6_det_small.onnx")) {
        detPath = path::JoinTemp(dir, "PP-OCRv6_det_small.onnx");
    } else if (FileInDir(dir, "PP-OCRv6_det_tiny.onnx")) {
        detPath = path::JoinTemp(dir, "PP-OCRv6_det_tiny.onnx");
    }
    if (!detPath) {
        return false;
    }
    str::ReplacePtr(&st->detPath, detPath);
    str::ReplacePtr(&st->recPath, path::JoinTemp(dir, "rec.onnx"));
    str::ReplacePtr(&st->dictPath, path::JoinTemp(dir, "keys.txt"));
    st->actual = st->requested;
    st->resolved = true;
    logf("OcrOnnx: using generic rec.onnx + keys.txt (will not guess a v6 dict)\n");
    return true;
}

static bool ResolveProfileLocked(OcrProfile requested, OcrProfileState* st, const char* dir) {
    st->requested = requested;
    st->resolveTried = true;
    st->resolved = false;

    auto tryFast = [&]() -> bool {
        return FastExactPresent(dir) && FillExact(st, dir, "PP-OCRv6_det_tiny.onnx", "PP-OCRv6_rec_tiny.onnx",
                                                  "ppocrv6_tiny_dict.txt", OcrProfile::Fast);
    };
    auto tryBalanced = [&]() -> bool {
        return BalancedExactPresent(dir) && FillExact(st, dir, "PP-OCRv6_det_small.onnx", "PP-OCRv6_rec_small.onnx",
                                                      "ppocrv6_dict.txt", OcrProfile::Balanced);
    };
    auto tryHybrid = [&]() -> bool {
        return HybridExactPresent(dir) && FillExact(st, dir, "PP-OCRv6_det_small.onnx", "PP-OCRv6_rec_tiny.onnx",
                                                    "ppocrv6_tiny_dict.txt", OcrProfile::Hybrid);
    };
    auto tryGeneric = [&]() -> bool { return FillGeneric(st, dir); };

    bool ok = false;
    switch (requested) {
        case OcrProfile::Fast:
            ok = tryFast() || tryBalanced() || tryGeneric();
            break;
        case OcrProfile::Balanced:
            ok = tryBalanced() || tryFast() || tryGeneric();
            break;
        case OcrProfile::Hybrid:
            ok = tryHybrid() || tryFast() || tryBalanced() || tryGeneric();
            break;
    }
    if (!ok) {
        logf("OcrOnnx: no model pair for profile %s in %s\n", OcrProfileName(requested), dir);
        return false;
    }
    if (st->actual != requested) {
        logf("OcrOnnx: requested %s, falling back to %s (det=%s rec=%s dict=%s)\n", OcrProfileName(requested),
             OcrProfileName(st->actual), path::GetBaseNameTemp(st->detPath), path::GetBaseNameTemp(st->recPath),
             path::GetBaseNameTemp(st->dictPath));
    }
    TempStr clsPath = FindNamedFileTemp(dir, kClsNames);
    str::ReplacePtr(&st->clsPath, clsPath);
    if (clsPath) {
        str::ReplacePtr(&gSharedClsPath, clsPath);
    }
    return true;
}

static bool InitRuntimeLocked() {
    if (gRuntimeTried) {
        return gRuntimeOk;
    }
    gRuntimeTried = true;
    TempStr dir = OcrSidecarDirTemp();
    SetMissingHint(dir);

    TempStr dllPath = path::JoinTemp(dir, "onnxruntime.dll");
    if (!file::Exists(dllPath)) {
        logf("OcrOnnx: missing %s\n", dllPath);
        return InitFail("missing dll");
    }
    if (!AnyOcrPairPresent(dir)) {
        logf("OcrOnnx: no usable det/rec/dict pair in %s\n", dir);
        return InitFail("missing det/rec/keys");
    }

    gOrtMod = LoadLibraryExW(ToWStrTemp(dllPath), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!gOrtMod) {
        SetOcrError("LoadLibrary onnxruntime.dll failed");
        logf("OcrOnnx: LoadLibrary onnxruntime.dll failed %#x\n", GetLastError());
        return InitFail("LoadLibrary");
    }
    using FnGetApiBase = const OrtApiBase* (*)(void);
    auto getBase = (FnGetApiBase)GetProcAddress(gOrtMod, "OrtGetApiBase");
    if (!getBase) {
        logf("OcrOnnx: OrtGetApiBase missing\n");
        return InitFail("OrtGetApiBase");
    }
    gApiBase = getBase();
    if (!gApiBase || !gApiBase->GetApi) {
        return InitFail("GetApi missing");
    }
    if (gApiBase->GetVersionString) {
        str::ReplacePtr(&gOrtVersion, gApiBase->GetVersionString());
    }
    gApi = gApiBase->GetApi(ORT_API_VERSION);
    if (!gApi) {
        gApi = gApiBase->GetApi(17);
    }
    if (!gApi) {
        gApi = gApiBase->GetApi(16);
    }
    if (!gApi) {
        gApi = gApiBase->GetApi(14);
    }
    if (!gApi) {
        logf("OcrOnnx: OrtApi version not supported\n");
        return InitFail("OrtApi version");
    }

    OrtStatus* st = gApi->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "SumatraOCR", &gEnv);
    if (st) {
        OrtFail(st, "CreateEnv");
        return InitFail("CreateEnv");
    }
    st = gApi->CreateSessionOptions(&gOpts);
    if (st) {
        OrtFail(st, "CreateSessionOptions");
        return InitFail("CreateSessionOptions");
    }
    st = gApi->SetIntraOpNumThreads(gOpts, DesiredOcrSlotCount() > 1 ? 1 : 2);
    if (st) {
        OrtFail(st, "SetIntraOpNumThreads");
    }
    st = gApi->SetSessionGraphOptimizationLevel(gOpts, ORT_ENABLE_EXTENDED);
    if (st) {
        OrtFail(st, "SetSessionGraphOptimizationLevel");
    }
    st = gApi->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &gMem);
    if (st) {
        OrtFail(st, "CreateCpuMemoryInfo");
        return InitFail("CreateCpuMemoryInfo");
    }
    st = gApi->GetAllocatorWithDefaultOptions(&gAlloc);
    if (st) {
        OrtFail(st, "GetAllocatorWithDefaultOptions");
        return InitFail("GetAllocatorWithDefaultOptions");
    }
    gRuntimeOk = true;
    logf("OcrOnnx: ORT %s CPUExecutionProvider (API %d)\n", gOrtVersion ? gOrtVersion : "unknown", ORT_API_VERSION);
    return true;
}

static bool LoadProfileLocked(OcrProfileState* st) {
    if (st->loadTried) {
        return st->loadOk;
    }
    st->loadTried = true;
    LARGE_INTEGER t0 = TimeGet();
    if (!LoadKeysInto(st->keys, st->dictPath)) {
        logf("OcrOnnx: failed to read keys %s\n", st->dictPath);
        SetOcrError("failed to read dictionary");
        return false;
    }
    st->slotCount = DesiredOcrSlotCount();
    if (st->slotCount < 1) {
        st->slotCount = 1;
    }
    if (!EnsureBundleLocked(st, 0)) {
        logf("OcrOnnx: session create failed det=%s rec=%s ir_det=%d ir_rec=%d err=%s\n", st->detPath, st->recPath,
             OnnxIrVersion(st->detPath), OnnxIrVersion(st->recPath), gLastError[0] ? gLastError : "(none)");
        return false;
    }
    int nKeys = st->keys.Size();
    int nCls = st->recClassCount;
    if (nCls > 0) {
        if (nCls == nKeys + 1) {
            st->keys.Append(" ");
            nKeys = st->keys.Size();
            logf("OcrOnnx: appended space token so dict %d matches rec classes %d\n", nKeys, nCls);
        }
        if (nCls != nKeys) {
            char msg[256]{};
            snprintf(msg, dimof(msg), "recognition class count %d does not match dictionary tokens %d (%s / %s)", nCls,
                     nKeys, path::GetBaseNameTemp(st->recPath), path::GetBaseNameTemp(st->dictPath));
            SetOcrError(msg);
            logf("OcrOnnx: %s\n", msg);
            return false;
        }
    } else {
        logf("OcrOnnx: rec output class dim is dynamic; will check on first inference (dict tokens=%d)\n", nKeys);
    }
    if (!st->slotSem) {
        st->slotSem = CreateSemaphoreW(nullptr, st->slotCount, st->slotCount, nullptr);
    }
    st->loadMs = TimeSinceInMs(t0);
    st->loadOk = true;
    logf("OCR profile: %s%s\n", OcrProfileName(st->actual),
         st->actual != st->requested ? str::FormatTemp(" (requested %s)", OcrProfileName(st->requested)) : "");
    logf("  Detection model:    %s (IR %d)\n", path::GetBaseNameTemp(st->detPath), OnnxIrVersion(st->detPath));
    logf("  Recognition model:  %s (IR %d)\n", path::GetBaseNameTemp(st->recPath), OnnxIrVersion(st->recPath));
    logf("  Dictionary:         %s (%d tokens incl. CTC blank)\n", path::GetBaseNameTemp(st->dictPath),
         st->keys.Size());
    logf("  ONNX Runtime:       %s\n", gOrtVersion ? gOrtVersion : "unknown");
    logf("                      CPUExecutionProvider\n");
    {
        OcrDetParams dp = DetParamsFor(st->actual);
        logf("  det params:         limit=%d %s max=%d thresh=%.2f box=%.2f unclip=%.1f dilation=%d\n", dp.limitSideLen,
             dp.limitMin ? "min" : "max", dp.maxSideLen, dp.thresh, dp.boxThresh, dp.unclipRatio, dp.dilation ? 1 : 0);
    }
    logf("  load_ms:            %.1f  slots=%d\n", st->loadMs, st->slotCount);
    return true;
}

static OcrProfileState* EnsureProfileReadyLocked(OcrProfile requested) {
    if (!InitRuntimeLocked()) {
        return nullptr;
    }
    OcrProfileState* st = &gProfiles[(int)requested];
    TempStr dir = OcrSidecarDirTemp();
    if (!st->resolved && !ResolveProfileLocked(requested, st, dir)) {
        return nullptr;
    }
    if (!LoadProfileLocked(st)) {
        return nullptr;
    }
    return st;
}

bool OcrSidecarLooksPresent() {
    TempStr dir = OcrSidecarDirTemp();
    TempStr dllPath = path::JoinTemp(dir, "onnxruntime.dll");
    if (!file::Exists(dllPath)) {
        return false;
    }
    return AnyOcrPairPresent(dir);
}

bool OcrModelsAvailable() {
    gOcrLock.Lock();
    bool ok = InitRuntimeLocked();
    gOcrLock.Unlock();
    return ok;
}

static void BilinearRgb(const u8* src, int sw, int sh, int sstride, u8* dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int y0 = (int)floorf(fy);
        int y1 = y0 + 1;
        float wy = fy - (float)y0;
        if (y0 < 0) {
            y0 = 0;
            wy = 0;
        }
        if (y1 >= sh) {
            y1 = sh - 1;
        }
        u8* row = dst + (size_t)y * (size_t)dw * 3;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int x0 = (int)floorf(fx);
            int x1 = x0 + 1;
            float wx = fx - (float)x0;
            if (x0 < 0) {
                x0 = 0;
                wx = 0;
            }
            if (x1 >= sw) {
                x1 = sw - 1;
            }
            const u8* p00 = src + (size_t)y0 * (size_t)sstride + (size_t)x0 * 3;
            const u8* p01 = src + (size_t)y0 * (size_t)sstride + (size_t)x1 * 3;
            const u8* p10 = src + (size_t)y1 * (size_t)sstride + (size_t)x0 * 3;
            const u8* p11 = src + (size_t)y1 * (size_t)sstride + (size_t)x1 * 3;
            for (int c = 0; c < 3; c++) {
                float v = (1 - wy) * ((1 - wx) * p00[c] + wx * p01[c]) + wy * ((1 - wx) * p10[c] + wx * p11[c]);
                row[x * 3 + c] = (u8)(v + 0.5f);
            }
        }
    }
}

static void RgbToNchwNorm(const u8* rgb, int w, int h, int stride, float* out) {
    size_t plane = (size_t)w * (size_t)h;
    for (int y = 0; y < h; y++) {
        const u8* row = rgb + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            for (int c = 0; c < 3; c++) {
                float v = (float)row[x * 3 + c] / 255.f;
                out[c * plane + i] = (v - 0.5f) / 0.5f;
            }
        }
    }
}

static bool RunTensor(OrtSession* session, const char* inName, const char* outName, float* in, const int64_t* inShape,
                      size_t inDim, float** out, size_t* outCount) {
    *out = nullptr;
    *outCount = 0;
    OrtValue* inVal = nullptr;
    size_t inElems = 1;
    for (size_t i = 0; i < inDim; i++) {
        inElems *= (size_t)inShape[i];
    }
    OrtStatus* st = gApi->CreateTensorWithDataAsOrtValue(gMem, in, inElems * sizeof(float), inShape, inDim,
                                                         ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal);
    if (st) {
        OrtFail(st, "CreateTensor");
        return false;
    }
    OrtValue* outVal = nullptr;
    const char* ins[] = {inName};
    const char* outs[] = {outName};
    st = gApi->Run(session, nullptr, ins, (const OrtValue* const*)&inVal, 1, outs, 1, &outVal);
    gApi->ReleaseValue(inVal);
    if (st) {
        OrtFail(st, "Run");
        return false;
    }
    float* data = nullptr;
    st = gApi->GetTensorMutableData(outVal, (void**)&data);
    if (st) {
        OrtFail(st, "GetTensorMutableData");
        gApi->ReleaseValue(outVal);
        return false;
    }
    OrtTensorTypeAndShapeInfo* info = nullptr;
    st = gApi->GetTensorTypeAndShape(outVal, &info);
    if (st) {
        OrtFail(st, "GetTensorTypeAndShape");
        gApi->ReleaseValue(outVal);
        return false;
    }
    size_t count = 0;
    gApi->GetTensorShapeElementCount(info, &count);
    gApi->ReleaseTensorTypeAndShapeInfo(info);
    *out = AllocArray<float>(count);
    memcpy(*out, data, count * sizeof(float));
    *outCount = count;
    gApi->ReleaseValue(outVal);
    return true;
}

struct DetBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float score = 0;
};

static void FloodBox(const u8* bin, const float* pred, int w, int h, int x, int y, u8* seen, DetBox* box) {
    Vec<int> st;
    st.Append(x);
    st.Append(y);
    seen[(size_t)y * (size_t)w + (size_t)x] = 1;
    int x0 = x, y0 = y, x1 = x, y1 = y;
    double sum = 0;
    int n = 0;
    while (st.Size() >= 2) {
        int cy = st.Pop();
        int cx = st.Pop();
        sum += pred[(size_t)cy * (size_t)w + (size_t)cx];
        n++;
        if (cx < x0) {
            x0 = cx;
        }
        if (cy < y0) {
            y0 = cy;
        }
        if (cx > x1) {
            x1 = cx;
        }
        if (cy > y1) {
            y1 = cy;
        }
        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            int nx = cx + dx[k];
            int ny = cy + dy[k];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                continue;
            }
            size_t i = (size_t)ny * (size_t)w + (size_t)nx;
            if (seen[i] || !bin[i]) {
                continue;
            }
            seen[i] = 1;
            st.Append(nx);
            st.Append(ny);
        }
    }
    box->x0 = x0;
    box->y0 = y0;
    box->x1 = x1;
    box->y1 = y1;
    box->score = n > 0 ? (float)(sum / n) : 0;
}

static void DilateBin2x2(u8* bin, int w, int h) {
    u8* src = AllocArray<u8>((size_t)w * (size_t)h);
    memcpy(src, bin, (size_t)w * (size_t)h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u8 v = 0;
            for (int dy = 0; dy < 2 && !v; dy++) {
                for (int dx = 0; dx < 2 && !v; dx++) {
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx < w && ny < h && src[(size_t)ny * (size_t)w + (size_t)nx]) {
                        v = 1;
                    }
                }
            }
            bin[(size_t)y * (size_t)w + (size_t)x] = v;
        }
    }
    free(src);
}

static void CollectDetBoxes(const float* pred, int pw, int ph, float sx, float sy, int imgW, int imgH, Vec<DetBox>& out,
                            const OcrDetParams& dp) {
    u8* bin = AllocArray<u8>((size_t)pw * (size_t)ph);
    u8* seen = AllocArray<u8>((size_t)pw * (size_t)ph);
    for (int i = 0; i < pw * ph; i++) {
        bin[i] = pred[i] > dp.thresh ? 1 : 0;
    }
    if (dp.dilation) {
        DilateBin2x2(bin, pw, ph);
    }
    int nCand = 0;
    for (int y = 0; y < ph; y++) {
        for (int x = 0; x < pw; x++) {
            size_t i = (size_t)y * (size_t)pw + (size_t)x;
            if (!bin[i] || seen[i]) {
                continue;
            }
            if (nCand >= 1000) {
                break;
            }
            DetBox b{};
            FloodBox(bin, pred, pw, ph, x, y, seen, &b);
            nCand++;
            int bw = b.x1 - b.x0 + 1;
            int bh = b.y1 - b.y0 + 1;
            if (bw < 3 || bh < 3 || b.score < dp.boxThresh) {
                continue;
            }
            float area = (float)bw * (float)bh;
            float peri = 2.f * (float)(bw + bh);
            float d = peri > 1 ? (area * dp.unclipRatio / peri) : 2.f;
            int x0 = (int)((float)b.x0 * sx - d * sx);
            int y0 = (int)((float)b.y0 * sy - d * sy);
            int x1 = (int)((float)(b.x1 + 1) * sx + d * sx);
            int y1 = (int)((float)(b.y1 + 1) * sy + d * sy);
            if (x0 < 0) {
                x0 = 0;
            }
            if (y0 < 0) {
                y0 = 0;
            }
            if (x1 > imgW) {
                x1 = imgW;
            }
            if (y1 > imgH) {
                y1 = imgH;
            }
            if (x1 - x0 < 4 || y1 - y0 < 4) {
                continue;
            }
            DetBox mapped{x0, y0, x1, y1, b.score};
            out.Append(mapped);
        }
    }
    free(bin);
    free(seen);

    int n = out.Size();
    for (int i = 0; i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            int midI = (out[best].y0 + out[best].y1) / 2;
            int midJ = (out[j].y0 + out[j].y1) / 2;
            int h = (out[best].y1 - out[best].y0 + out[j].y1 - out[j].y0) / 2;
            bool sameRow = abs(midI - midJ) < (h > 8 ? h / 2 : 8);
            if (sameRow) {
                if (out[j].x0 < out[best].x0) {
                    best = j;
                }
            } else if (midJ < midI) {
                best = j;
            }
        }
        if (best != i) {
            DetBox tmp = out[i];
            out[i] = out[best];
            out[best] = tmp;
        }
    }
}

static void CropRgb(const u8* rgb, int w, int h, int stride, const DetBox& b, Vec<u8>& crop, int* cw, int* ch) {
    int x0 = b.x0, y0 = b.y0, x1 = b.x1, y1 = b.y1;
    *cw = x1 - x0;
    *ch = y1 - y0;
    crop.Reset();
    u8* dst = crop.AppendBlanks((size_t)(*cw) * (size_t)(*ch) * 3);
    for (int y = 0; y < *ch; y++) {
        const u8* src = rgb + (size_t)(y0 + y) * (size_t)stride + (size_t)x0 * 3;
        memcpy(dst + (size_t)y * (size_t)(*cw) * 3, src, (size_t)(*cw) * 3);
    }
}

// Packed RGB. (x,y) -> (y, w-1-x). Dest is h x w so a tall column becomes a wide line.
static void RotateRgbPacked90Ccw(const u8* src, int w, int h, Vec<u8>& dst) {
    dst.Reset();
    if (!src || w < 1 || h < 1) {
        return;
    }
    int nw = h;
    int nh = w;
    u8* d = dst.AppendBlanks((size_t)nw * (size_t)nh * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = y;
            int dy = w - 1 - x;
            const u8* s = src + ((size_t)y * (size_t)w + (size_t)x) * 3;
            u8* p = d + ((size_t)dy * (size_t)nw + (size_t)dx) * 3;
            p[0] = s[0];
            p[1] = s[1];
            p[2] = s[2];
        }
    }
}

static void Rotate180(Vec<u8>& img, int w, int h) {
    int n = w * h;
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        for (int c = 0; c < 3; c++) {
            u8 t = img[i * 3 + c];
            img[i * 3 + c] = img[j * 3 + c];
            img[j * 3 + c] = t;
        }
    }
}

static void CtcEmit(StrBuilder& sb, Vec<int>& xs, int clsId, int s, int e, int cropW, int t, const StrVec& keys) {
    int nKeys = keys.Size();
    if (clsId <= 0 || clsId >= nKeys || t < 1) {
        return;
    }
    const char* ch = keys.At(clsId);
    if (!ch || !ch[0]) {
        return;
    }
    sb.Append(ch);
    int x0 = (int)((float)s * (float)cropW / (float)t + 0.5f);
    int x1 = (int)((float)(e + 1) * (float)cropW / (float)t + 0.5f);
    if (x1 <= x0) {
        x1 = x0 + 1;
    }
    xs.Append(x0);
    xs.Append(x1);
}

static char* CtcDecode(const float* out, int t, int cls, int cropW, const StrVec& keys, int** charXOut, int* nCharOut) {
    if (charXOut) {
        *charXOut = nullptr;
    }
    if (nCharOut) {
        *nCharOut = 0;
    }
    StrBuilder sb;
    Vec<int> xs;
    int cur = -1, s = 0, e = 0;
    for (int i = 0; i < t; i++) {
        const float* row = out + (size_t)i * (size_t)cls;
        int best = 0;
        float bestV = row[0];
        for (int c = 1; c < cls; c++) {
            if (row[c] > bestV) {
                bestV = row[c];
                best = c;
            }
        }
        if (best == 0) {
            if (cur > 0) {
                CtcEmit(sb, xs, cur, s, e, cropW, t, keys);
                cur = -1;
            }
            continue;
        }
        if (best == cur) {
            e = i;
            continue;
        }
        if (cur > 0) {
            CtcEmit(sb, xs, cur, s, e, cropW, t, keys);
        }
        cur = best;
        s = e = i;
    }
    if (cur > 0) {
        CtcEmit(sb, xs, cur, s, e, cropW, t, keys);
    }
    if (charXOut && nCharOut && xs.Size() >= 2) {
        int n = xs.Size();
        int* p = AllocArray<int>(n);
        for (int i = 0; i < n; i++) {
            p[i] = xs[i];
        }
        *charXOut = p;
        *nCharOut = n / 2;
    }
    if (sb.Size() == 0) {
        return nullptr;
    }
    return sb.StealData();
}

static char* RecognizeCrop(OcrOrtBundle* b, OcrProfileState* st, const u8* rgb, int w, int h, int** charXOut,
                           int* nCharOut, OcrPageTiming* timing) {
    if (w < 4 || h < 4) {
        return nullptr;
    }
    LARGE_INTEGER tPre = TimeGet();
    const int recH = 48;
    int recW = (int)((float)w * (float)recH / (float)h + 0.5f);
    if (recW < 8) {
        recW = 8;
    }
    if (recW > 960) {
        recW = 960;
    }
    recW = (recW + 7) & ~7;
    u8* resized = AllocArray<u8>((size_t)recW * recH * 3);
    BilinearRgb(rgb, w, h, w * 3, resized, recW, recH);
    float* nchw = AllocArray<float>((size_t)3 * recW * recH);
    RgbToNchwNorm(resized, recW, recH, recW * 3, nchw);
    free(resized);
    if (timing) {
        timing->recPreprocessMs += TimeSinceInMs(tPre);
    }

    if (!b || !b->rec || !b->recIn || !b->recOut) {
        free(nchw);
        return nullptr;
    }
    int64_t shape[4] = {1, 3, recH, recW};
    float* out = nullptr;
    size_t outN = 0;
    LARGE_INTEGER tInf = TimeGet();
    bool ok = RunTensor(b->rec, b->recIn, b->recOut, nchw, shape, 4, &out, &outN);
    if (timing) {
        timing->recInferenceMs += TimeSinceInMs(tInf);
    }
    free(nchw);
    if (!ok || !out || outN < 2) {
        free(out);
        return nullptr;
    }
    LARGE_INTEGER tPost = TimeGet();
    int cls = st->recClassCount > 0 ? st->recClassCount : st->keys.Size();
    int t = 0;
    if (cls > 0 && outN % (size_t)cls == 0) {
        t = (int)(outN / (size_t)cls);
    } else {
        SetOcrError("recognition output size does not match dictionary class count");
        logf("OcrOnnx: rec outN=%zu classCount=%d dict=%d — refusing CTC decode\n", outN, cls, st->keys.Size());
        free(out);
        return nullptr;
    }
    if (st->recClassCount <= 0) {
        st->recClassCount = cls;
        if (cls != st->keys.Size()) {
            SetOcrError("recognition class count does not match dictionary");
            logf("OcrOnnx: inferred rec classes %d != dict %d\n", cls, st->keys.Size());
            free(out);
            return nullptr;
        }
    }
    char* text = CtcDecode(out, t, cls, w, st->keys, charXOut, nCharOut);
    free(out);
    if (timing) {
        timing->recPostprocessMs += TimeSinceInMs(tPost);
    }
    return text;
}

static bool ShouldRotate180(OcrOrtBundle* b, const u8* rgb, int w, int h) {
    if (!b || !b->cls || !b->clsIn || !b->clsOut || w < 4 || h < 4) {
        return false;
    }
    const int cw = 192, ch = 48;
    u8* resized = AllocArray<u8>((size_t)cw * ch * 3);
    BilinearRgb(rgb, w, h, w * 3, resized, cw, ch);
    float* nchw = AllocArray<float>((size_t)3 * cw * ch);
    RgbToNchwNorm(resized, cw, ch, cw * 3, nchw);
    free(resized);
    int64_t shape[4] = {1, 3, ch, cw};
    float* out = nullptr;
    size_t outN = 0;
    bool ok = RunTensor(b->cls, b->clsIn, b->clsOut, nchw, shape, 4, &out, &outN);
    free(nchw);
    bool rot = false;
    if (ok && out && outN >= 2) {
        rot = out[1] > out[0] && out[1] > 0.9f;
    }
    free(out);
    return rot;
}

struct RecParShare {
    const u8* rgb = nullptr;
    int w = 0;
    int h = 0;
    int stride = 0;
    DetBox* dets = nullptr;
    int nDets = 0;
    OcrBox* out = nullptr;
    OcrProfileState* st = nullptr;
    volatile LONG next = 0;
};

struct RecParCtx {
    RecParShare* share = nullptr;
    OcrOrtBundle* b = nullptr;
    OcrPageTiming local{};
};

static void RecParLoop(RecParCtx* ctx) {
    if (!ctx || !ctx->share || !ctx->b || !ctx->share->st) {
        return;
    }
    RecParShare* s = ctx->share;
    for (;;) {
        LONG i = InterlockedIncrement(&s->next) - 1;
        if (i >= s->nDets) {
            return;
        }
        DetBox& box = s->dets[i];
        Vec<u8> crop;
        int cw = 0, ch = 0;
        LARGE_INTEGER tCrop = TimeGet();
        CropRgb(s->rgb, s->w, s->h, s->stride, box, crop, &cw, &ch);
        ctx->local.cropMs += TimeSinceInMs(tCrop);
        if (cw < 4 || ch < 4) {
            continue;
        }
        // Vertical book columns are tall. Rec expects a wide strip (height ~48).
        bool vertical = ch > cw * 3 / 2 && ch > 20;
        if (vertical) {
            Vec<u8> rot;
            RotateRgbPacked90Ccw(crop.LendData(), cw, ch, rot);
            if (rot.Size() > 0) {
                crop.Reset();
                u8* p = crop.AppendBlanks((size_t)rot.Size());
                memcpy(p, rot.LendData(), (size_t)rot.Size());
                int t = cw;
                cw = ch;
                ch = t;
            } else {
                vertical = false;
            }
        }
        if (ShouldRotate180(ctx->b, crop.LendData(), cw, ch)) {
            Rotate180(crop, cw, ch);
        }
        int* charX = nullptr;
        int nChar = 0;
        char* text = RecognizeCrop(ctx->b, s->st, crop.LendData(), cw, ch, &charX, &nChar, &ctx->local);
        if (!text) {
            free(charX);
            continue;
        }
        s->out[i].rect = Rect(box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0);
        s->out[i].text = text;
        s->out[i].charX = charX;
        s->out[i].nChar = nChar;
        s->out[i].vertical = vertical;
    }
}

static bool RecognizeRgbLocked(OcrProfileState* st, OcrOrtBundle* b, const u8* rgb, int w, int h, int stride,
                               Vec<OcrBox>& boxesOut, OcrPageTiming* timing) {
    if (!st || !b || !b->det || !b->detIn || !b->detOut) {
        return false;
    }
    OcrDetParams dp = DetParamsFor(st->actual);
    LARGE_INTEGER tPre = TimeGet();
    float ratio = 1.f;
    if (dp.limitMin) {
        int minSide = w < h ? w : h;
        if (minSide < dp.limitSideLen) {
            ratio = (float)dp.limitSideLen / (float)minSide;
        }
    } else {
        int maxSide = w > h ? w : h;
        if (maxSide > dp.limitSideLen) {
            ratio = (float)dp.limitSideLen / (float)maxSide;
        }
    }
    if (dp.maxSideLen > 0) {
        int maxSide = w > h ? w : h;
        if ((float)maxSide * ratio > (float)dp.maxSideLen) {
            ratio = (float)dp.maxSideLen / (float)maxSide;
        }
    }
    int rw = (int)((float)w * ratio);
    int rh = (int)((float)h * ratio);
    rw = ((rw + 31) / 32) * 32;
    rh = ((rh + 31) / 32) * 32;
    if (rw < 32) {
        rw = 32;
    }
    if (rh < 32) {
        rh = 32;
    }
    u8* resized = AllocArray<u8>((size_t)rw * rh * 3);
    memset(resized, 255, (size_t)rw * rh * 3);
    int copyW = (int)((float)w * ratio);
    int copyH = (int)((float)h * ratio);
    if (copyW > rw) {
        copyW = rw;
    }
    if (copyH > rh) {
        copyH = rh;
    }
    BilinearRgb(rgb, w, h, stride, resized, copyW, copyH);
    if (copyW != rw || copyH != rh) {
        u8* canvas = AllocArray<u8>((size_t)rw * rh * 3);
        memset(canvas, 255, (size_t)rw * rh * 3);
        for (int y = 0; y < copyH; y++) {
            memcpy(canvas + (size_t)y * rw * 3, resized + (size_t)y * copyW * 3, (size_t)copyW * 3);
        }
        free(resized);
        resized = canvas;
    }

    float* nchw = AllocArray<float>((size_t)3 * rw * rh);
    RgbToNchwNorm(resized, rw, rh, rw * 3, nchw);
    free(resized);
    if (timing) {
        timing->detPreprocessMs += TimeSinceInMs(tPre);
    }

    int64_t shape[4] = {1, 3, rh, rw};
    float* pred = nullptr;
    size_t predN = 0;
    LARGE_INTEGER tInf = TimeGet();
    bool ok = RunTensor(b->det, b->detIn, b->detOut, nchw, shape, 4, &pred, &predN);
    if (timing) {
        timing->detInferenceMs += TimeSinceInMs(tInf);
    }
    free(nchw);
    if (!ok || !pred) {
        free(pred);
        return false;
    }
    LARGE_INTEGER tPost = TimeGet();
    int pw = rw, ph = rh;
    if (predN == (size_t)rw * (size_t)rh) {
        pw = rw;
        ph = rh;
    } else if (predN > 0) {
        ph = rh;
        pw = (int)(predN / (size_t)ph);
        if (pw <= 0 || (size_t)pw * (size_t)ph != predN) {
            pw = rw;
            ph = (int)(predN / (size_t)pw);
        }
    }
    float contentPw = (float)copyW * (float)pw / (float)rw;
    float contentPh = (float)copyH * (float)ph / (float)rh;
    if (contentPw < 1.f) {
        contentPw = (float)pw;
    }
    if (contentPh < 1.f) {
        contentPh = (float)ph;
    }
    float sx = (float)w / contentPw;
    float sy = (float)h / contentPh;
    Vec<DetBox> dets;
    CollectDetBoxes(pred, pw, ph, sx, sy, w, h, dets, dp);
    int nDet = dets.Size();
    free(pred);
    if (timing) {
        timing->detPostprocessMs += TimeSinceInMs(tPost);
        timing->nDetBoxes = nDet;
    }

    if (nDet > 0) {
        RecParShare share;
        share.rgb = rgb;
        share.w = w;
        share.h = h;
        share.stride = stride;
        share.dets = dets.LendData();
        share.nDets = nDet;
        share.out = AllocArray<OcrBox>((size_t)nDet);
        share.st = st;
        share.next = 0;

        HANDLE extraTh[kOcrMaxSlots]{};
        int extraSlot[kOcrMaxSlots]{};
        RecParCtx extraCtx[kOcrMaxSlots]{};
        int nExtra = 0;
        if (nDet > 1) {
            for (;;) {
                int s = TryAcquireOcrSlot(st);
                if (s < 0) {
                    break;
                }
                extraSlot[nExtra] = s;
                extraCtx[nExtra].share = &share;
                extraCtx[nExtra].b = &st->bundles[s];
                extraTh[nExtra] = StartThread(MkFunc0(RecParLoop, &extraCtx[nExtra]), "OcrRec");
                if (!extraTh[nExtra]) {
                    ReleaseOcrSlot(st, s);
                    break;
                }
                nExtra++;
                if (nExtra >= kOcrMaxSlots - 1) {
                    break;
                }
            }
        }
        RecParCtx mainCtx;
        mainCtx.share = &share;
        mainCtx.b = b;
        RecParLoop(&mainCtx);
        if (timing) {
            timing->cropMs += mainCtx.local.cropMs;
            timing->recPreprocessMs += mainCtx.local.recPreprocessMs;
            timing->recInferenceMs += mainCtx.local.recInferenceMs;
            timing->recPostprocessMs += mainCtx.local.recPostprocessMs;
        }
        if (nExtra > 0) {
            WaitForMultipleObjects((DWORD)nExtra, extraTh, TRUE, INFINITE);
            for (int i = 0; i < nExtra; i++) {
                CloseHandle(extraTh[i]);
                ReleaseOcrSlot(st, extraSlot[i]);
                if (timing) {
                    timing->cropMs += extraCtx[i].local.cropMs;
                    timing->recPreprocessMs += extraCtx[i].local.recPreprocessMs;
                    timing->recInferenceMs += extraCtx[i].local.recInferenceMs;
                    timing->recPostprocessMs += extraCtx[i].local.recPostprocessMs;
                }
            }
        }
        for (int i = 0; i < nDet; i++) {
            if (!share.out[i].text) {
                continue;
            }
            boxesOut.Append(share.out[i]);
            share.out[i].text = nullptr;
            share.out[i].charX = nullptr;
        }
        free(share.out);
    }
    if (timing) {
        timing->nRecBoxes = boxesOut.Size();
    }
    if (boxesOut.Size() == 0) {
        SetOcrError(nDet == 0 ? "detector found no text lines" : "recognizer returned empty text");
    }
    return boxesOut.Size() > 0;
}

bool OcrRecognizeRgb(const u8* rgb, int w, int h, int stride, Vec<OcrBox>& boxesOut, OcrProfile profile,
                     OcrPageTiming* timing) {
    boxesOut.Reset();
    LARGE_INTEGER t0 = TimeGet();
    if (!rgb || w < 8 || h < 8 || stride < w * 3) {
        return false;
    }
    gOcrLock.Lock();
    OcrProfileState* st = EnsureProfileReadyLocked(profile);
    gOcrLock.Unlock();
    if (!st) {
        return false;
    }
    int slot = AcquireOcrSlot(st);
    if (slot < 0) {
        return false;
    }
    bool ok = RecognizeRgbLocked(st, &st->bundles[slot], rgb, w, h, stride, boxesOut, timing);
    ReleaseOcrSlot(st, slot);
    if (timing) {
        timing->pageTotalMs += TimeSinceInMs(t0);
    }
    return ok;
}
