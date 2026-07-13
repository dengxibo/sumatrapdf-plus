/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

namespace updatecheck {

constexpr int kSecondsInHour = 60 * 60;
constexpr int kSecondsInDay = 24 * kSecondsInHour;
constexpr int kSecondsInWeek = 7 * kSecondsInDay;

inline bool CanStartUpdateCheck(bool userInitiated, bool checkInProgress, bool isDebugBuild, bool isStoreBuild,
                                bool hasInternetPermission, bool canSavePreferences, bool automaticChecksEnabled) {
    if (checkInProgress) {
        return false;
    }
    if (userInitiated) {
        return true;
    }
    return !isDebugBuild && !isStoreBuild && hasInternetPermission && canSavePreferences && automaticChecksEnabled;
}

inline bool IsZeroFileTime(const FILETIME& ft) {
    return ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0;
}

inline i64 FileTimeDiffInSecs64(const FILETIME& newer, const FILETIME& older) {
    ULARGE_INTEGER t1{};
    t1.LowPart = newer.dwLowDateTime;
    t1.HighPart = newer.dwHighDateTime;
    ULARGE_INTEGER t2{};
    t2.LowPart = older.dwLowDateTime;
    t2.HighPart = older.dwHighDateTime;
    return ((i64)t1.QuadPart - (i64)t2.QuadPart) / 10000000;
}

inline int GetAutomaticCheckDelaySecs(const FILETIME& now, const FILETIME& lastCheck, int intervalSecs) {
    if (IsZeroFileTime(lastCheck)) {
        return 0;
    }
    i64 elapsed = FileTimeDiffInSecs64(now, lastCheck);
    if (elapsed < 0) {
        return intervalSecs;
    }
    if (elapsed >= intervalSecs) {
        return 0;
    }
    return intervalSecs - (int)elapsed;
}

inline int GetRetryDelaySecs(int failureCount) {
    constexpr int delays[] = {kSecondsInHour, 2 * kSecondsInHour, 4 * kSecondsInHour, 6 * kSecondsInHour};
    int idx = limitValue(failureCount, 0, (int)dimof(delays) - 1);
    return delays[idx];
}

inline bool ShouldSnoozeVersion(const char* latestVer, const char* snoozedVer, const FILETIME& snoozedAt,
                                const FILETIME& now) {
    if (!latestVer || !snoozedVer || !str::EqI(latestVer, snoozedVer) || IsZeroFileTime(snoozedAt)) {
        return false;
    }
    i64 elapsed = FileTimeDiffInSecs64(now, snoozedAt);
    return elapsed >= 0 && elapsed < kSecondsInWeek;
}

} // namespace updatecheck
