# Read Aloud (TTS)

**Available in [pre-release 3.7](https://www.sumatrapdfreader.org/prerelease)**

SumatraPDF can read documents aloud with **word-by-word highlighting**: the current spoken word is highlighted in the same style as text selection.

Supported for fixed-layout and reflowable documents (PDF, EPUB, MOBI, etc.) that expose extractable text.

## Quick start

1. Install SumatraPDF 3.7 (or newer pre-release build).
2. **Configure a natural voice** — see [Voice setup](#voice-setup) below. Without this step, Windows default voices (e.g. legacy SAPI voices) sound robotic.
3. Open a document.
4. Use **Read Aloud (TTS)** from the toolbar, menu, or command palette (`Ctrl + K` → `Read Aloud`).
5. Choose **Start Reading From Top**, **Start Reading From Cursor Position**, or **Start Reading Selection**.
6. Pick a voice under **Read Aloud (TTS) → Voice**.

While reading, the current word is highlighted. Use **Pause Reading**, **Continue Reading**, and **Stop Reading** from the same menu.

## Commands

| Command | Description |
| --- | --- |
| `CmdReadAloud` | Read Aloud (opens read-aloud menu / toolbar) |
| `CmdReadAloudFromTopPage` | Start Reading From Top |
| `CmdReadAloudSelection` | Start Reading Selection |
| `CmdPauseReadAloud` | Pause Reading |
| `CmdContinueReadAloud` | Continue Reading |
| `CmdStopReadAloud` | Stop Reading |

Voice selection is available from **Read Aloud (TTS) → Voice** (system default plus installed voices).

Speaking rate presets are available from **Read Aloud (TTS) → Speed** (0.5× through 2.0×).

Advanced setting `ReadAloudVoiceId` stores the chosen voice id (WinRT voice id or SAPI token id). Advanced setting `ReadAloudSpeakingRate` stores the speaking rate multiplier (1.0 is normal). See [Advanced options / settings](Advanced-options-settings.md).

## Voice setup

**Important:** SumatraPDF uses Windows speech APIs (WinRT `SpeechSynthesis`, with SAPI 5 fallback). Out of the box, many systems only expose **legacy SAPI voices** (e.g. Microsoft Huihui, Kangkang, Yaoyao on Chinese Windows). Those sound mechanical.

**Narrator “Natural HD” voices installed under Accessibility → Narrator are not available to third-party apps by default.** To use natural-sounding speech in SumatraPDF, install **[NaturalVoiceSAPIAdapter](https://github.com/gexgd0419/NaturalVoiceSAPIAdapter)** and enable one of the two modes below.

Download the installer from:  
https://github.com/gexgd0419/NaturalVoiceSAPIAdapter/releases

Run the **64-bit installer as administrator** on 64-bit Windows. After installation, open **NaturalVoiceSAPIAdapter Installer** from the Start menu to configure voices.

Windows Defender / SmartScreen may warn about this tool because it registers a custom SAPI engine and modifies registry settings. It is a third-party community project — install only if you accept that.

### Method 1: Local Narrator voices (recommended, offline)

Best quality; works **without network** after setup.

1. Install NaturalVoiceSAPIAdapter (64-bit).
2. Open **NaturalVoiceSAPIAdapter Installer**.
3. Check **Enable Narrator natural voices**.
4. Download **MSIX** voice packages from the project wiki (use the **last working version** links; current Microsoft Store versions may not work with this adapter):  
   https://github.com/gexgd0419/NaturalVoiceSAPIAdapter/wiki/Narrator-natural-voice-download-links  
   For Chinese: **Microsoft Xiaoxiao** (晓晓), **Microsoft Yunxi** (云希).
5. Create a folder with an **ASCII-only path**, e.g. `C:\TTS` (no non-ASCII characters in the path).
6. Unzip each MSIX file (treat it as a ZIP) into its **own subfolder** under `C:\TTS`.
7. In the installer, set **Local voice path** to the parent folder (`C:\TTS`), not the subfolder.
8. Click **Close**. Restart SumatraPDF completely.
9. **Read Aloud (TTS) → Voice** — select **Microsoft Xiaoxiao**, **Microsoft Yunxi**, or another installed natural voice.

Notes:

- Put **only** voice subfolders inside the local voice path; do not mix other files there.
- Subfolder names should also use ASCII-only paths.
- If voices do not appear, open the installer again and confirm 64-bit is installed and **Enable Narrator natural voices** is checked.

### Method 2: Microsoft Edge online voices (easier, requires internet)

No MSIX download; voices are fetched online when reading.

1. Install NaturalVoiceSAPIAdapter.
2. Open **NaturalVoiceSAPIAdapter Installer**.
3. Check **Enable Microsoft Edge online voices**.
4. Optionally adjust **Included languages** (default: follow user's preferred languages).
5. Click **Close**. Restart SumatraPDF.
6. **Read Aloud (TTS) → Voice** — choose an Edge online voice.

**Azure online voices** require a subscription key (`Enable Azure online voices` + **Set Azure key...**); most users can ignore this.

## Comparison

| | Local MSIX voices | Edge online voices |
| --- | --- | --- |
| Installer option | Enable Narrator natural voices | Enable Microsoft Edge online voices |
| MSIX download | Yes (from project wiki) | No |
| Offline | Yes | No (needs network while reading) |
| Quality | Natural HD (best) | Good |
| Path setup | `C:\TTS` or other ASCII path | None |
| Best for | Long reading sessions, books | Quick try-out |

## Chinese pronunciation dictionary

WinRT SpeechSynthesis and SAPI 5 speak **plain text only** (no SSML/`<phoneme>`). By default the engine picks readings from context. To force names, places, or polyphones, place `tts-pronunciation.json` next to `SumatraPDF.exe` or in the SumatraPDF AppData folder (exe directory wins). Copy and edit [`tts-pronunciation.sample.json`](../../tts-pronunciation.sample.json) as a starting point.

```json
{
  "entries": [
    { "from": "重庆", "to": "崇庆", "wholeWord": true },
    { "from": "银行", "to": "银杭", "wholeWord": true },
    { "from": "单老师", "to": "善老师", "wholeWord": true }
  ]
}
```

- Matching is **longest `from` first**, so a rule for `重庆` wins over a shorter rule for `重`.
- `to` should usually be **homophone Hanzi** (same intended reading). Latin pinyin in `to` often sounds wrong on Chinese voices.
- Only the spoken stream is rewritten; on-screen text and word highlight stay on the original document text.
- Restart reading (or restart the app) after editing the file. `wholeWord` mainly protects Latin tokens (e.g. `row` inside `crowd`); CJK relies on longest-match.

## FAQ

**Voice menu only shows Huihui / Kangkang / Yaoyao**

The SAPI bridge is not active or no natural voices are configured. Reinstall 64-bit NaturalVoiceSAPIAdapter, enable the correct option, restart SumatraPDF, and verify local MSIX folders or Edge online setting.

**I installed Xiaoxiao (Natural HD) in Narrator but SumatraPDF cannot see it**

Expected. Narrator natural voices are separate from third-party SAPI/WinRT lists until exposed via NaturalVoiceSAPIAdapter (method 1 or 2 above).

**Highlight does not follow the spoken word**

Ensure you are on a build with Read Aloud highlighting enabled, the document has a text layer, and reading has started (highlight appears only while TTS is active).

**Antivirus blocked the download or installer**

Add an exclusion for the download folder, or use a browser/tool that can complete the download; the adapter is commonly flagged as a hacktool due to registry and engine registration behavior.

## See also

- [Commands](Commands.md)
- [Advanced options / settings](Advanced-options-settings.md) — `ReadAloudVoiceId`
- [Version history](Version-history.md)
