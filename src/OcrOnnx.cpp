/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/ThreadUtil.h"
#include "utils/WinUtil.h"

#include "OcrOnnx.h"

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
static OrtEnv* gEnv = nullptr;
static OrtSessionOptions* gOpts = nullptr;
static OrtMemoryInfo* gMem = nullptr;
static OrtAllocator* gAlloc = nullptr;
static StrVec gKeys;
static bool gInitTried = false;
static bool gInitOk = false;
static char gMissingHint[512] = {};
static char gLastError[512] = {};

static constexpr int kOcrMaxSlots = 8;
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
static OcrOrtBundle gBundles[kOcrMaxSlots];
static bool gSlotBusy[kOcrMaxSlots];
static int gSlotCount = 1;
static HANDLE gSlotSem = nullptr;
static char* gDetPath = nullptr;
static char* gRecPath = nullptr;
static char* gClsPath = nullptr;

static const char* kDetNames[] = {"det.onnx",
                                  "ch_PP-OCRv4_det_mobile.onnx",
                                  "ch_PP-OCRv4_det_infer.onnx",
                                  "ch_PP-OCRv5_det_mobile.onnx",
                                  "ch_PP-OCRv5_det_infer.onnx",
                                  nullptr};
static const char* kRecNames[] = {"rec.onnx",
                                  "ch_PP-OCRv4_rec_mobile.onnx",
                                  "ch_PP-OCRv4_rec_infer.onnx",
                                  "ch_PP-OCRv5_rec_mobile.onnx",
                                  "ch_PP-OCRv5_rec_infer.onnx",
                                  nullptr};
static const char* kClsNames[] = {"cls.onnx", "ch_ppocr_mobile_v2.0_cls_infer.onnx",
                                  "ch_ppocr_mobile_v2.0_cls_mobile.onnx", nullptr};
static const char* kKeyNames[] = {"keys.txt", "ppocr_keys_v1.txt", "ppocr_keys.txt", nullptr};

char* OcrSidecarDirTemp() {
    TempStr exeDir = GetSelfExeDirTemp();
    TempStr nextToExe = path::JoinTemp(exeDir, "ocr");
    if (dir::Exists(nextToExe)) {
        return nextToExe;
    }
    // debug build: repo-root/ocr when exe is out/dbg64
    TempStr up = path::JoinTemp(exeDir, "..", "..");
    TempStr repoOcr = path::JoinTemp(up, "ocr");
    if (dir::Exists(repoOcr)) {
        return path::NormalizeTemp(repoOcr);
    }
    return nextToExe;
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
                "OCR models not found. Put onnxruntime.dll, det.onnx, rec.onnx and keys.txt in: ");
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

// One ONNX session bundle per in-flight page. Leave 1–2 logical processors for
// the UI thread; cap so we do not load too many copies of det/rec/cls.
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
    if (gInitOk && gSlotCount >= 1) {
        return gSlotCount;
    }
    return DesiredOcrSlotCount();
}

static char* SessionFirstName(OrtSession* session, bool input);

static bool CacheSessionNames(OcrOrtBundle* b) {
    if (!b || !b->det || !b->rec) {
        return false;
    }
    b->detIn = SessionFirstName(b->det, true);
    b->detOut = SessionFirstName(b->det, false);
    b->recIn = SessionFirstName(b->rec, true);
    b->recOut = SessionFirstName(b->rec, false);
    if (b->cls) {
        b->clsIn = SessionFirstName(b->cls, true);
        b->clsOut = SessionFirstName(b->cls, false);
    }
    return b->detIn && b->detOut && b->recIn && b->recOut;
}

static bool EnsureBundleLocked(int i) {
    if (i < 0 || i >= gSlotCount) {
        return false;
    }
    OcrOrtBundle* b = &gBundles[i];
    if (b->det && b->rec) {
        return true;
    }
    if (!gDetPath || !gRecPath) {
        return false;
    }
    b->det = LoadSession(gDetPath);
    b->rec = LoadSession(gRecPath);
    if (gClsPath) {
        b->cls = LoadSession(gClsPath);
    }
    if (!b->det || !b->rec) {
        return false;
    }
    return CacheSessionNames(b);
}

static int AcquireOcrSlot() {
    if (!gSlotSem) {
        return -1;
    }
    WaitForSingleObject(gSlotSem, INFINITE);
    gOcrLock.Lock();
    int slot = -1;
    for (int i = 0; i < gSlotCount; i++) {
        if (gSlotBusy[i] || !gBundles[i].det || !gBundles[i].rec) {
            continue;
        }
        gSlotBusy[i] = true;
        slot = i;
        break;
    }
    gOcrLock.Unlock();
    if (slot < 0) {
        ReleaseSemaphore(gSlotSem, 1, nullptr);
    }
    return slot;
}

static int TryAcquireOcrSlot() {
    if (!gSlotSem) {
        return -1;
    }
    if (WaitForSingleObject(gSlotSem, 0) != WAIT_OBJECT_0) {
        return -1;
    }
    gOcrLock.Lock();
    int slot = -1;
    for (int i = 0; i < gSlotCount; i++) {
        if (gSlotBusy[i] || !gBundles[i].det || !gBundles[i].rec) {
            continue;
        }
        gSlotBusy[i] = true;
        slot = i;
        break;
    }
    gOcrLock.Unlock();
    if (slot < 0) {
        ReleaseSemaphore(gSlotSem, 1, nullptr);
    }
    return slot;
}

static void ReleaseOcrSlot(int slot) {
    if (slot < 0 || slot >= gSlotCount) {
        return;
    }
    gOcrLock.Lock();
    gSlotBusy[slot] = false;
    gOcrLock.Unlock();
    if (gSlotSem) {
        ReleaseSemaphore(gSlotSem, 1, nullptr);
    }
}

static bool LoadKeys(const char* path) {
    gKeys.Reset();
    ByteSlice d = file::ReadFile(path);
    if (!d) {
        return false;
    }
    // CTC blank is index 0; file lines are classes 1..N
    gKeys.Append("");
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
            gKeys.Append(p + start, len);
        }
        while (i < n && (p[i] == '\n' || p[i] == '\r')) {
            i++;
        }
    }
    d.Free();
    if (gKeys.Size() == 6624) {
        // rec.onnx has 6625 classes: CTC blank + 6623 dict lines + space
        gKeys.Append(" ");
    }
    return gKeys.Size() > 8;
}

static bool InitFail(const char* reason) {
    return false;
}

static bool InitOrtLocked() {
    if (gInitTried) {
        return gInitOk;
    }
    gInitTried = true;
    TempStr dir = OcrSidecarDirTemp();
    SetMissingHint(dir);

    TempStr dllPath = path::JoinTemp(dir, "onnxruntime.dll");
    if (!file::Exists(dllPath)) {
        logf("OcrOnnx: missing %s\n", dllPath);
        return InitFail("missing dll");
    }
    TempStr detPath = FindNamedFileTemp(dir, kDetNames);
    TempStr recPath = FindNamedFileTemp(dir, kRecNames);
    TempStr keyPath = FindNamedFileTemp(dir, kKeyNames);
    if (!detPath || !recPath || !keyPath) {
        logf("OcrOnnx: need det/rec/keys in %s\n", dir);
        return InitFail("missing det/rec/keys");
    }

    gOrtMod = LoadLibraryW(ToWStrTemp(dllPath));
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
    const OrtApiBase* base = getBase();
    if (!base || !base->GetApi) {
        return InitFail("GetApi missing");
    }
    gApi = base->GetApi(ORT_API_VERSION);
    if (!gApi) {
        gApi = base->GetApi(16);
    }
    if (!gApi) {
        gApi = base->GetApi(14);
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

    if (!LoadKeys(keyPath)) {
        logf("OcrOnnx: failed to read keys %s\n", keyPath);
        return InitFail("LoadKeys");
    }
    TempStr clsPath = FindNamedFileTemp(dir, kClsNames);
    str::ReplacePtr(&gDetPath, detPath);
    str::ReplacePtr(&gRecPath, recPath);
    str::ReplacePtr(&gClsPath, clsPath);
    gSlotCount = DesiredOcrSlotCount();
    if (gSlotCount < 1) {
        gSlotCount = 1;
    }
    if (gSlotCount > kOcrMaxSlots) {
        gSlotCount = kOcrMaxSlots;
    }
    int loaded = 0;
    for (int i = 0; i < gSlotCount; i++) {
        if (!EnsureBundleLocked(i)) {
            if (i == 0) {
                return InitFail("LoadSession");
            }
            break;
        }
        loaded++;
    }
    gSlotCount = loaded;
    if (!gSlotSem) {
        gSlotSem = CreateSemaphoreW(nullptr, gSlotCount, gSlotCount, nullptr);
    }
    gInitOk = true;
    logf("OcrOnnx: ready (det=%s rec=%s cls=%s keys=%d slots=%d)\n", path::GetBaseNameTemp(detPath),
         path::GetBaseNameTemp(recPath), clsPath ? path::GetBaseNameTemp(clsPath) : "none", gKeys.Size(), gSlotCount);
    return true;
}

bool OcrSidecarLooksPresent() {
    TempStr dir = OcrSidecarDirTemp();
    TempStr dllPath = path::JoinTemp(dir, "onnxruntime.dll");
    if (!file::Exists(dllPath)) {
        return false;
    }
    if (!FindNamedFileTemp(dir, kDetNames) || !FindNamedFileTemp(dir, kRecNames) ||
        !FindNamedFileTemp(dir, kKeyNames)) {
        return false;
    }
    return true;
}

bool OcrModelsAvailable() {
    gOcrLock.Lock();
    bool ok = InitOrtLocked();
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
    // RapidOCR ONNX (det/rec/cls): (x/255 - 0.5) / 0.5
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

static char* SessionFirstName(OrtSession* session, bool input) {
    char* name = nullptr;
    OrtStatus* st = nullptr;
    if (input) {
        st = gApi->SessionGetInputName(session, 0, gAlloc, &name);
    } else {
        st = gApi->SessionGetOutputName(session, 0, gAlloc, &name);
    }
    if (st) {
        OrtFail(st, "SessionGetName");
        return nullptr;
    }
    return name;
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

static void CollectDetBoxes(const float* pred, int pw, int ph, float sx, float sy, int imgW, int imgH,
                            Vec<DetBox>& out) {
    u8* bin = AllocArray<u8>((size_t)pw * (size_t)ph);
    u8* seen = AllocArray<u8>((size_t)pw * (size_t)ph);
    for (int i = 0; i < pw * ph; i++) {
        bin[i] = pred[i] > 0.3f ? 1 : 0;
    }
    for (int y = 0; y < ph; y++) {
        for (int x = 0; x < pw; x++) {
            size_t i = (size_t)y * (size_t)pw + (size_t)x;
            if (!bin[i] || seen[i]) {
                continue;
            }
            DetBox b{};
            FloodBox(bin, pred, pw, ph, x, y, seen, &b);
            int bw = b.x1 - b.x0 + 1;
            int bh = b.y1 - b.y0 + 1;
            if (bw < 3 || bh < 3 || b.score < 0.5f) {
                continue;
            }
            float area = (float)bw * (float)bh;
            float peri = 2.f * (float)(bw + bh);
            float d = peri > 1 ? (area * 1.6f / peri) : 2.f;
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

    // top-to-bottom, then left-to-right
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

static void CtcEmit(StrBuilder& sb, Vec<int>& xs, int clsId, int s, int e, int cropW, int t, int nKeys) {
    if (clsId <= 0 || clsId >= nKeys || t < 1) {
        return;
    }
    const char* ch = gKeys.At(clsId);
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

static char* CtcDecode(const float* out, int t, int cls, int cropW, int** charXOut, int* nCharOut) {
    if (charXOut) {
        *charXOut = nullptr;
    }
    if (nCharOut) {
        *nCharOut = 0;
    }
    StrBuilder sb;
    Vec<int> xs;
    int nKeys = gKeys.Size();
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
                CtcEmit(sb, xs, cur, s, e, cropW, t, nKeys);
                cur = -1;
            }
            continue;
        }
        if (best == cur) {
            e = i;
            continue;
        }
        if (cur > 0) {
            CtcEmit(sb, xs, cur, s, e, cropW, t, nKeys);
        }
        cur = best;
        s = e = i;
    }
    if (cur > 0) {
        CtcEmit(sb, xs, cur, s, e, cropW, t, nKeys);
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

static char* RecognizeCrop(OcrOrtBundle* b, const u8* rgb, int w, int h, int** charXOut, int* nCharOut) {
    if (w < 4 || h < 4) {
        return nullptr;
    }
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

    if (!b || !b->rec || !b->recIn || !b->recOut) {
        free(nchw);
        return nullptr;
    }
    int64_t shape[4] = {1, 3, recH, recW};
    float* out = nullptr;
    size_t outN = 0;
    bool ok = RunTensor(b->rec, b->recIn, b->recOut, nchw, shape, 4, &out, &outN);
    free(nchw);
    if (!ok || !out || outN < 2) {
        free(out);
        return nullptr;
    }
    int cls = gKeys.Size();
    int t = 0;
    if (cls > 0 && outN % (size_t)cls == 0) {
        t = (int)(outN / (size_t)cls);
    } else {
        // output is often [1, T, C]
        cls = (int)(outN > 0 ? outN : 1);
        t = 1;
        for (int cand = gKeys.Size(); cand <= gKeys.Size() + 4 && cand > 0; cand++) {
            if (outN % (size_t)cand == 0) {
                cls = cand;
                t = (int)(outN / (size_t)cand);
                break;
            }
        }
    }
    char* text = CtcDecode(out, t, cls, w, charXOut, nCharOut);
    free(out);
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
    volatile LONG next = 0;
};

struct RecParCtx {
    RecParShare* share = nullptr;
    OcrOrtBundle* b = nullptr;
};

static void RecParLoop(RecParCtx* ctx) {
    if (!ctx || !ctx->share || !ctx->b) {
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
        CropRgb(s->rgb, s->w, s->h, s->stride, box, crop, &cw, &ch);
        if (cw < 4 || ch < 4) {
            continue;
        }
        if (ShouldRotate180(ctx->b, crop.LendData(), cw, ch)) {
            Rotate180(crop, cw, ch);
        }
        int* charX = nullptr;
        int nChar = 0;
        char* text = RecognizeCrop(ctx->b, crop.LendData(), cw, ch, &charX, &nChar);
        if (!text) {
            free(charX);
            continue;
        }
        s->out[i].rect = Rect(box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0);
        s->out[i].text = text;
        s->out[i].charX = charX;
        s->out[i].nChar = nChar;
    }
}

static bool RecognizeRgbLocked(OcrOrtBundle* b, const u8* rgb, int w, int h, int stride, Vec<OcrBox>& boxesOut) {
    if (!b || !b->det || !b->detIn || !b->detOut) {
        return false;
    }
    int limit = 960;
    int maxSide = w > h ? w : h;
    float ratio = maxSide > limit ? (float)limit / (float)maxSide : 1.f;
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
    // letterbox: already white-padded because we resized into a white canvas of rw*rh
    // Bilinear wrote copyW x copyH at origin; if copyW!=rw we need to place into rw stride
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

    int64_t shape[4] = {1, 3, rh, rw};
    float* pred = nullptr;
    size_t predN = 0;
    bool ok = RunTensor(b->det, b->detIn, b->detOut, nchw, shape, 4, &pred, &predN);
    free(nchw);
    if (!ok || !pred) {
        free(pred);
        return false;
    }
    int pw = rw, ph = rh;
    if (predN == (size_t)rw * (size_t)rh) {
        pw = rw;
        ph = rh;
    } else if (predN > 0) {
        // [1,1,H,W]
        ph = rh;
        pw = (int)(predN / (size_t)ph);
        if (pw <= 0 || (size_t)pw * (size_t)ph != predN) {
            pw = rw;
            ph = (int)(predN / (size_t)pw);
        }
    }
    // The detector input is letterboxed into a 32-aligned canvas, so only
    // copyW x copyH of rw x rh holds the page. Scaling pred coords by the padded
    // size squeezes every x toward the left and clips the last glyph of each line.
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
    CollectDetBoxes(pred, pw, ph, sx, sy, w, h, dets);
    int nDet = dets.Size();
    free(pred);

    if (nDet > 0) {
        RecParShare share;
        share.rgb = rgb;
        share.w = w;
        share.h = h;
        share.stride = stride;
        share.dets = dets.LendData();
        share.nDets = nDet;
        share.out = AllocArray<OcrBox>((size_t)nDet);
        share.next = 0;

        HANDLE extraTh[kOcrMaxSlots]{};
        int extraSlot[kOcrMaxSlots]{};
        RecParCtx extraCtx[kOcrMaxSlots]{};
        int nExtra = 0;
        if (nDet > 1) {
            for (;;) {
                int s = TryAcquireOcrSlot();
                if (s < 0) {
                    break;
                }
                extraSlot[nExtra] = s;
                extraCtx[nExtra].share = &share;
                extraCtx[nExtra].b = &gBundles[s];
                extraTh[nExtra] = StartThread(MkFunc0(RecParLoop, &extraCtx[nExtra]), "OcrRec");
                if (!extraTh[nExtra]) {
                    ReleaseOcrSlot(s);
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
        if (nExtra > 0) {
            WaitForMultipleObjects((DWORD)nExtra, extraTh, TRUE, INFINITE);
            for (int i = 0; i < nExtra; i++) {
                CloseHandle(extraTh[i]);
                ReleaseOcrSlot(extraSlot[i]);
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
    if (boxesOut.Size() == 0) {
        SetOcrError(nDet == 0 ? "detector found no text lines" : "recognizer returned empty text");
    }
    return boxesOut.Size() > 0;
}

bool OcrRecognizeRgb(const u8* rgb, int w, int h, int stride, Vec<OcrBox>& boxesOut) {
    boxesOut.Reset();
    if (!rgb || w < 8 || h < 8 || stride < w * 3) {
        return false;
    }
    if (!OcrModelsAvailable()) {
        return false;
    }
    int slot = AcquireOcrSlot();
    if (slot < 0) {
        return false;
    }
    bool ok = RecognizeRgbLocked(&gBundles[slot], rgb, w, h, stride, boxesOut);
    ReleaseOcrSlot(slot);
    return ok;
}
