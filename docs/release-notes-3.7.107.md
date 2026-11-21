# SumatraPDF Plus 3.7.107

- JoyInside 仅保留「沉稳男声」（原唐僧）和「温情女声」（原 F25 青年-温情范），无需等待企业音色目录加载。
- 修复取消 JoyInside 合成后，旧请求仍可能阻塞网络接收、让新音色排队等待的问题。正常切换音色和语速时复用连接。
- 增加鉴权、建连和首段音频的耗时日志，便于排查冷启动等待。冷启动仍可能需要重新鉴权和建连。
- 已通过真实接口的男女声切换、取消后重播、同音色连续合成及空闲后重连测试。

## English

- Keep two JoyInside voices: 沉稳男声 (formerly 唐僧) and 温情女声 (formerly F25 青年-温情范). The voice list no longer waits for the enterprise catalog.
- Interrupt blocked network receives when cancelling JoyInside synthesis so an old request does not hold up the next voice. Reuse the connection when changing voices or speed.
- Log authentication, connection and first audio latency to help diagnose cold starts. Cold starts may still require authentication and a new connection.
- Pass live service tests for voice switching, playback after cancellation, consecutive requests and reconnection after inactivity.
