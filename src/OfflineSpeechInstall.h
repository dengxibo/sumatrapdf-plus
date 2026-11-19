#pragma once

struct OfflineVoice;
bool OfflineVoiceCanDownload(const OfflineVoice&);
bool InstallOfflineVoice(const OfflineVoice&, const char* source, bool download, HANDLE cancel, volatile LONG* percent);
