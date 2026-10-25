# HD SAPI 客户端工作线程接入（2026-10-24）

## 实现

`src/SapiSpeechWorker.h` 是实际生产后端，`TextToSpeech.cpp` 的 UI 入口只复制命令参数和读取纯数据事件。播放 ISpVoice、准备服务及音频 lease 的 COM 生命周期均归工作线程；枚举、验证语音 token 仍在 UI。

- 停止、重启、跳转、换声、变速切换代次。工作线程在收到 SAPI 事件时保存 generation + 客户端逻辑 chunk，Windows 消息只负责唤醒，不能赋予旧事件新的代次。
- 每一代先 purge、等待 abort 确认并 drain，再提交新流。确认失败便放弃旧 voice，不复用。等待发生在工作线程；普通控制动作不阻塞 UI。程序退出会等待工作线程清理。
- 新流收到 START 后才接受其 WORD/END。UI 读完 A 的 END 后，更新段落及发音词典映射，再接管 B 的缓冲事件；跨段词偏移不累计。UI 忙时只合并相邻同段 WORD，保留最新位置和实际词长。
- 当前输出实例首次 Speak 前尝试设为 5000ms 缓冲，HRESULT 记录在 START 日志中。失败仍使用原输出，不修改系统全局设备设置。
- 选定 TtsApplicationHD token 时，使用公开生产 CLSID 服务，探测版本/能力并设置有限缓存。一旦下一段可用就提交 Prepare；用词典改写后的实际文本、完整 token ID、SAPI rate 和代次作为身份。
- READY 后 Acquire + Validate + SpeakStream 只入队一次。持有 ticket 和音频 lease，直到该段结束或 purge 已确认。注意 Cancel(ticket) 会使已有流的 Read 失败，成功入队后不可立即 Cancel。
- 未安装/旧服务、准备失败、资源超限，或当前段 END 时准备尚未完成：先取消 ticket，再普通 Speak 回退一次。迟到 READY 不会重复入队。
- 智能双语仍不提前排队；跨页/OCR 没有拿到文本时无法预合成。首段冷启动和超过缓冲预算的合成仍可能等待。
- 保存朗读位置后再应用变速/多语种换声，避免取消代次清除原位置后从段首重读。
- 连续停止每次都留下最新代次的取消屏障，即使 UI 已清空当前流编号，也不能丢弃尚未执行的 abort。

本项目目前没有单独的 ISpVoice 音量调节入口；队列预合成时由 HdQueuePrepared 同步当前 voice 音量。如果以后新增此入口，需要同时给当前和已排队的 IHdPreparedAudio 调用 SetPlaybackVolume。

## 验证

`tools/test_sapi_worker.cpp` 直接使用同一个生产 worker，不是重新实现一个模拟后端。需要安装 HD SAPI 服务和真实输出设备，会播放测试语音。在 VS Developer PowerShell 中：

```powershell
cl /nologo /EHsc /W4 /std:c++17 /Fe:out/test_sapi_worker.exe /Fo:out/test_sapi_worker.obj tools/test_sapi_worker.cpp
$token = 'HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\TtsApplicationHD-Xiaoxiao'
out/test_sapi_worker.exe $token abc verbose
# 分别运行：stop early-jump jump rate voice prepared-stop prepared-jump unavailable repeated-stop
```

真实正式安装的 x64 Xiaoxiao/Yunyi 测试结果：

- A/B/C：3 START、3 END、37 个逐词事件，B/C 均 prepared=1，各段第一个 WORD 从 0 开始。
- 停止、START 前跳转、普通跳转、rate=4、Xiaoxiao→Yunyi、预合成播放中停止/跳转：通过代次过滤、事件数量和逐段偏移检查。
- 预合成流还检查播放持续时间与多个词事件，防止仅 START/END 数量正确却实际音频被提前取消。
- unavailable 模式通过注入未注册 COM 类测试真正的激活失败路径，A/B/C 普通回退：3 START、3 END、37 WORD、prepared=0。
- 内置 Huihui 普通 SAPI：A/B/C 检查通过，prepared=0。

日志位于 `out/hd-worker-abc-tests.txt`、`out/hd-worker-control-tests.txt`、`out/hd-worker-repeated-stop.txt`，完整构建日志 `out/hd-worker-build.log`。这些后端集成测试验证生产调度和用于高亮的事件数据；不等于在 PDF/EPUB 界面上逐一操作过所有高亮、OCR、跨页和文档切换场景。本轮没有重新捕获读者的实际回环音频，不声称所有负载均无段间空档。

公开协议源文件来自用户同机 TtsApplication/sapi/HdPrepare.h，复制至 src/HdPrepare.h 并格式化；无需修改适配器项目。
