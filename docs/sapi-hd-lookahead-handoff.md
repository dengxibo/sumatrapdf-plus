# SAPI HD 语音段落衔接：转交适配器项目的提示词

SumatraPDF 已改为在当前段开始播放后，立即用同一个 ISpVoice 提交下一段：

- 当前段：Speak(text, SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, &streamA)。
- 下一段：Speak(nextText, SPF_ASYNC | SPF_IS_NOT_XML, &streamB)，不 purge。
- 收到 streamA 的 SPEI_END_INPUT_STREAM 后，应用把高亮切到下一段，直接接管 streamB，不重复 Speak，不 purge，然后排入第三段。
- 每次最多提前一段；停止、跳转和重新开始通过 purge 丢弃队列。智能双语切换目前不提前排队。
- 下一段使用发音词典改写后的实际文本；各流的 WORD_BOUNDARY 必须相对于各自传入文本，不能跨流累计。

请先检查并优化以下路径：

1. 连续异步 Speak A/B 时，确认 B 的引擎调用何时发生；记录合成开始、首块 PCM、合成完成、Write 返回时间。不要假定 SAPI 排队等于并行预合成。
2. 尽可能让 A 的 PCM 写入 SAPI 后及时返回，让 SAPI 在 A 音频尚在设备缓冲播放时处理 B。不要在引擎 Speak 返回前额外等待设备播放完毕；必须遵守 SAPI 的流和事件协议。
3. 复用本地 HD 模型和 synthesizer，避免每段重建、取消后等待、冷启动；缓存和异步任务必须隔离语音、语速及取消代次。
4. 正确处理 purge/abort：立即停止旧流，旧任务不能在新流中写音频或上报事件。WORD_BOUNDARY 和 END_INPUT_STREAM 必须按实际播放时序交给 SAPI，不能按预合成完成时刻提前触发。
5. 如果标准 SAPI 队列只能串行调用且不足以覆盖合成延迟，请提出可由客户端真正调用的可选预合成协议及头文件，再交给 SumatraPDF 项目接入。协议需支持：能力探测、异步 Prepare(text, voice, rate, generation)、缓存命中复用、取消、内存上限；无扩展时回退普通 SAPI。明确如何从客户端取得服务，不能仅在引擎私有对象添加一个客户端拿不到的 COM 接口。预合成不得播放，也不得发送播放边界事件。
6. 给出 A/B/C 连续朗读、停止、跳转、变速、换声的测试与耗时日志，尤其统计 A 最后一个音频采样到 B 第一个采样的间隔。

客户端当前没有调用任何私有预合成接口。标准排队是否足以改善，需要实际测量；跨页文本尚未提取/OCR 尚未完成时也可能没有下一段可提前提交。

SAPI 队列说明：https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms720149(v=vs.85)

## 2026-10-24 适配器交接后的客户端核对

已读取 TtsApplication/sapi/HdPrepareIntegration.zh-CN.md 和 HdPrepare.h，并核对安装成功状态及 queue-release 原始 sample-gaps.json。默认 500ms 缓冲样例的额外 PCM 间隔为 690/1230ms；Win32 5000ms 样例为 0/0。这是适配器探针证据，不能当作 SumatraPDF 实机回归已通过。

客户端新增 SapiConfigureQueueBuffer：创建并选声之后、首次 Speak 之前，将该输出设备 SPAUDIOBUFFERINFO.ulMsBufferSize 设为 5000，保留通知间隔和事件 bias，检查并记录 HRESULT；不支持则继续普通 SAPI。不修改全局设备设置。下一段仍只排一次，当前 END 后接管既有流。客户端目前在提交 A 后立即提交 B，比等待 A START 更早。

公开 Prepare 服务尚未接入。当前 SAPI 对象由 UI 线程拥有，停止是异步 purge，事件仅用 stream number 识别。适配器实测 purge 后流号重用，故旧 WORD/END 可能被误认成新流。不能把 HdPurgeAndDrain 直接放到 UI 线程，也不能只加一个在派发时读取的 generation。完整接入还需：

- 单个串行 SAPI 工作线程拥有 COM 对象，串行执行 Speak、GetEvents、purge 和准备调度。
- UI 命令和工作线程产出的事件带代次；停止立即废弃旧 UI 代次，工作线程确认 abort 并 drain 后才能复用播放 voice；新 START 建立流身份。
- 队列段状态保存实际改写文本及 source map、完整 voice token、rate、generation、ticket/lease；准备和普通回退只能二选一排入，避免重复。
- 停止、跳转（包括 START 前）、变速、换声以及逐字高亮必须在读者内复测；适配器探针通过不代表这些客户端路径通过。
- 未提取/OCR 的段落仍无法提前排队；智能双语仍禁用提前排队。

5 秒是可提前写入设备的容量，不是每次播放固定等待 5 秒；它允许串行合成覆盖剩余播放时间，不会使合成引擎变成并行。首次启动和超过缓冲预算的计算延迟仍存在。

本次在 SumatraPDF 工作区另行运行正式安装的 x64 Xiaoxiao token + QueueProbe（5000ms、queue 模式），无需临时 COM 注册。探针 PASS ABC streams=1,2,3，回环波形分析额外空档为 0/0ms、对齐漂移 0，证据在 out/hd-queue-installed-check/sample-gaps.json。此探针验证安装适配器与标准缓冲路径，不覆盖读者的停止/跳转/高亮状态机。新增 C++ 编译成功；完整 build.ts 链接报 LNK1168，运行中的 SumatraPDF-Plus.exe 无法覆盖，未关闭用户进程。

## 生产接入已更新

先前“公开服务尚未接入、需要工作线程”的结论已由后续实现取代。当前已在本项目加入工作线程、代次隔离、公开 Prepare + SpeakStream 和单次普通回退。实现与实测见 [HD SAPI 工作线程接入记录](sapi-hd-worker-2026-10-24.md)。原有历史探针证据仍保留，不能替代读者界面测试。
