# In-process offline speech

The runtime and PCM event integration originated in the user's TtsApplication project. See LICENSE for the source license.

The reader compiles InProcessSpeech.cpp directly into the application. A narrow, exception-safe interface connects the reader's own strings and containers to the Windows runtime integration. There is no helper executable, executable resource, child process or pipe protocol.

One persistent MTA worker owns the selected model and synthesizer. Requests deliver PCM and word timing callbacks directly to the reader's streaming player. The model remains loaded between requests. Cancellation calls the synthesizer's Stop method, drains the request and keeps the model reusable. Voice changes and removal release it on its owning worker.

Voice construction temporarily instruments the extension's import slot and restores it immediately. Only the matching Windows speech SDK may be loaded; a conflicting SDK is rejected. DLL directories are scoped to the runtime and the reader's default DLL search policy is not changed. Callback contexts carry unique identities; disconnecting a voice drains active callbacks and rejects queued callbacks for retired identities.

The Windows speech extension versions 2608.11000.0.0 and 2609.28000.0.0 are supported. Runtime DLLs and voice models are not distributed with the reader. The extension is loaded from the Windows SystemApps directory and models are downloaded or imported into `tts` only on user request.

OfflineSpeechInstall.cpp queries Microsoft's Store catalog using WinHTTP and MSXML, verifies the pinned package size and SHA-256, and extracts the selected model. SOAP templates and their license are in ext/tts-store. Downloading and synthesis do not use PowerShell scripts.
