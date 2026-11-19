#include "utils/BaseUtil.h"
#include "utils/CryptoUtil.h"
#include "utils/FileUtil.h"
#include "utils/HttpUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "AppTools.h"
#include "OfflineSpeechInstall.h"
#include "OfflineSpeech.h"
#include "OfflineVoiceCatalog.h"
#include "../ext/libarchive/libarchive/archive.h"
#include "../ext/libarchive/libarchive/archive_entry.h"

static TempStr ReadVoiceUrlTemp(const char* path) {
    i64 size = file::GetSize(path);
    if (size <= 0 || size > 4096) {
        return nullptr;
    }
    ByteSlice data = file::ReadFile(path);
    defer {
        data.Free();
    };
    if (data.empty() || memchr(data.data(), 0, data.size())) {
        return nullptr;
    }
    auto url = str::DupTemp((const char*)data.data(), data.size());
    str::TrimWSInPlace(url, str::TrimOpt::Both);
    return !strpbrk(url, " \t\r\n") ? url : nullptr;
}

static TempStr OfflineVoiceOverrideUrlTemp(const OfflineVoice& voice) {
    if (!voice.directory) {
        return nullptr;
    }
    TempStr path = path::JoinTemp(GetSelfExeDirTemp(), "tts", str::JoinTemp(voice.directory, "-url.txt"));
    TempStr url = ReadVoiceUrlTemp(path);
    return str::StartsWithI(url, "https://") ? url : nullptr;
}

bool OfflineVoiceCanDownload(const OfflineVoice& voice) {
    return OfflineVoiceOverrideUrlTemp(voice) || (voice.storeId && voice.storeFile);
}

static bool IsMicrosoftVoiceCdn(const char* url) {
    return str::StartsWithI(url, "http://tlu.dl.delivery.mp.microsoft.com/") ||
           str::StartsWithI(url, "https://tlu.dl.delivery.mp.microsoft.com/");
}

static TempStr ResolveOfflineVoiceUrl(const OfflineVoice& voice, HANDLE cancel) {
    if (TempStr overrideUrl = OfflineVoiceOverrideUrlTemp(voice)) {
        return overrideUrl;
    }
    return ResolveVoiceStorePackage(voice.storeId, voice.storeFile, cancel);
}

struct OfflineVoiceTransfer {
    HANDLE cancel;
    volatile LONG* percent;
    i64 size;
};

static bool OfflineVoiceCancelled(HANDLE cancel) {
    return WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0;
}

static void OfflineVoiceDownloadProgress(OfflineVoiceTransfer* job, HttpProgress* progress) {
    progress->cancel = OfflineVoiceCancelled(job->cancel) || progress->nDownloaded > job->size;
    if (!progress->cancel) {
        InterlockedExchange(job->percent, (LONG)(90 * progress->nDownloaded / job->size));
    }
}

static bool VerifyOfflineVoicePackage(const OfflineVoice& voice, const char* path) {
    AutoCloseHandle file(
        CreateFileW(ToWStrTemp(path), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file.IsValid() || file::GetSize(file) != voice.packageSize) {
        return false;
    }
    AutoCloseHandle mapping(CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping.IsValid()) {
        return false;
    }
    const void* bytes = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!bytes) {
        return false;
    }
    defer {
        UnmapViewOfFile(bytes);
    };
    u8 hash[32];
    CalcSHA2Digest(bytes, (int)voice.packageSize, hash);
    const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < sizeof(hash); i++) {
        if (voice.sha256[2 * i] != hex[hash[i] >> 4] || voice.sha256[2 * i + 1] != hex[hash[i] & 15]) {
            return false;
        }
    }
    return true;
}

static bool ExtractOfflineVoice(const char* package, const char* output, HANDLE cancel) {
    auto* reader = archive_read_new();
    if (!reader) {
        return false;
    }
    defer {
        archive_read_free(reader);
    };
    if (archive_read_support_format_zip(reader) != ARCHIVE_OK ||
        archive_read_open_filename_w(reader, ToWStrTemp(package), 65536) != ARCHIVE_OK) {
        return false;
    }
    archive_entry* entry = nullptr;
    int result;
    while ((result = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const char* name = archive_entry_pathname_utf8(entry);
        if (!name || !*name || *name == '/' || strpbrk(name, "\\:") || str::Contains(name, "..") ||
            OfflineVoiceCancelled(cancel)) {
            return false;
        }
        TempStr dest = path::JoinTemp(output, name);
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            if (!dir::CreateAll(dest)) {
                return false;
            }
            continue;
        }
        if (archive_entry_filetype(entry) != AE_IFREG || !dir::CreateForFile(dest)) {
            return false;
        }
        AutoCloseHandle file(CreateFileW(ToWStrTemp(dest), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));
        if (!file.IsValid()) {
            return false;
        }
        u8 buffer[65536];
        for (;;) {
            auto read = archive_read_data(reader, buffer, sizeof(buffer));
            if (read == 0) {
                break;
            }
            DWORD wrote = 0;
            if (read < 0 || OfflineVoiceCancelled(cancel) || !WriteFile(file, buffer, (DWORD)read, &wrote, nullptr) ||
                wrote != (DWORD)read) {
                return false;
            }
        }
    }
    return result == ARCHIVE_EOF && file::Exists(path::JoinTemp(output, "Tokens.xml")) &&
           file::Exists(path::JoinTemp(output, "AppxManifest.xml"));
}

bool InstallOfflineVoice(const OfflineVoice& voice, const char* source, bool download, HANDLE cancel,
                         volatile LONG* percent) {
    if (!voice.directory || !voice.sha256 || (!download && str::IsEmpty(source))) {
        return false;
    }
    TempStr root = path::JoinTemp(GetSelfExeDirTemp(), "tts");
    GUID id;
    WCHAR suffix[40];
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, suffix, dimof(suffix))) {
        return false;
    }
    TempStr name = str::JoinTemp(".install-", ToUtf8Temp(suffix));
    TempStr stage = path::JoinTemp(root, name);
    if (!dir::CreateAll(root) || !CreateDirectoryW(ToWStrTemp(stage), nullptr)) {
        root = GetPathInAppDataDirTemp("tts");
        stage = path::JoinTemp(root, name);
        if (!dir::CreateAll(root) || !CreateDirectoryW(ToWStrTemp(stage), nullptr)) {
            return false;
        }
    }
    defer {
        dir::RemoveAll(stage);
    };
    if (download) {
        source = ResolveOfflineVoiceUrl(voice, cancel);
        if (!source || OfflineVoiceCancelled(cancel)) {
            return false;
        }
        TempStr file = path::JoinTemp(stage, "voice.msix");
        OfflineVoiceTransfer transfer{cancel, percent, voice.packageSize};
        if (!HttpGetToFile(source, file, MkFunc1(OfflineVoiceDownloadProgress, &transfer),
                           IsMicrosoftVoiceCdn(source))) {
            return false;
        }
        source = file;
    }
    AutoCloseHandle packageLock(
        CreateFileW(ToWStrTemp(source), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!packageLock.IsValid() || OfflineVoiceCancelled(cancel) || !VerifyOfflineVoicePackage(voice, source)) {
        return false;
    }
    InterlockedExchange(percent, 95);
    TempStr model = path::JoinTemp(stage, "model");
    TempStr dest = path::JoinTemp(root, voice.directory);
    return dir::CreateAll(model) && ExtractOfflineVoice(source, model, cancel) && !OfflineVoiceCancelled(cancel) &&
           MoveFileW(ToWStrTemp(model), ToWStrTemp(dest));
}
