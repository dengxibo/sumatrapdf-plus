# SumatraPDF Plus 3.7.105

- 在线朗读新增 Azure Speech 和 JoyInside，可在「设置 → 朗读 → 在线配置」中填写自己的服务凭据。在线语音配置用 Windows DPAPI 加密，随当前 `SumatraPDF-settings.txt` 保存。旧版注册表中的在线语音配置需要重新填写一次。
- JoyInside 音色排在 Edge 前面，保留「成熟大叔」并筛去不适合图书朗读的音色。连续朗读复用连接，收到首批音频后即可开始解码。
- 保存在线配置前检查 JoyInside 设备 ID 的格式。合成失败时，提示会显示实际使用的语音服务。
- 朗读中更换音色后从当前位置继续，修复全屏退出按钮在朗读高亮刷新时闪动的问题。

## English

- Add Azure Speech and JoyInside to online read aloud. The app uses Windows DPAPI to encrypt the online speech configuration in the active `SumatraPDF-settings.txt`. Re-enter values previously saved in the Windows registry.
- List JoyInside voices before Edge voices, with a selection suited to book reading. Reuse the connection between segments and start decoding before the full response arrives.
- Validate the JoyInside device ID before saving. Synthesis errors name the selected speech service.
- Resume from the current position after a voice change and fix fullscreen exit button flicker during read-aloud highlighting.
