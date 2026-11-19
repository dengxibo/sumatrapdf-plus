# SumatraPDF Plus 3.7.100

- 朗读按「系统语音、离线语音、在线语音」选择来源，每种来源分别记住音色。
- 可在阅读器中下载和删除离线自然语音，支持 HD 音色，无需另外安装 SAPI 适配器。
- 在线列表保留 34 个经过实际合成及音频解码验证的音色，包含多语种、中文和英文。
- 改善首声等待和段落衔接：流式播放、提前合成下一段、离线模型预热和复用。
- 改善朗读设置排版，以及确认对话框的多语言和主题显示。

离线语音的下载和合成均在主程序内完成，无需脚本、辅助 EXE 或另外安装适配器。只有首次下载或导入离线音色时才创建 `tts` 目录，存放语音模型。模型按需下载，不包含在发布包中。在线朗读不会创建该目录，需要联网并将朗读文本发送给微软。

Read aloud now supports separate system, offline and online voice selections, downloadable offline natural/HD voices without a SAPI adapter, and 34 synthesis-tested online voices. Streaming playback, prefetch and reusable offline models improve startup and continuity.

Offline voice downloading and synthesis run inside the application. No script, helper executable or adapter installation is needed. The `tts` folder is created only when installing an offline voice and stores voice models; online reading does not create it. Models are downloaded on demand. Online reading sends the text to Microsoft.
