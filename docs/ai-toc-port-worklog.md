# AI 目录识别移植与优化工作日志

## 2026-09-28：移植基础与设置

- 将 autoContents 的印刷目录页检测、逐页标题与页码提取、层级判定和扫描书页码偏移流程移入 SumatraPDF Plus 的 C++ 模块 `AiTocApi.cpp`，保留 autoContents 原仓库及其使用方式。
- 设置页支持保存多套平台配置、切换平台、读取模型列表、在模型输入框内筛选、配置 1–8 路并发及连接测试。旧版单配置可迁移为 Default。
- 修复 API 配置自动填充、错误显示与翻译资源链路；API 全部失败时展示实际错误，而非误报“未检测到印刷目录”。
- 真书端到端验证曾提取出 31 条目录项，层级判定后交给目录校准界面；书签最终确认由用户在应用中完成。此前的详细过程见本地 `autoContents-main/.trae/documents/ai-toc-port-worklog.md`。

## 2026-09-28：交互、速度与关闭思考

- 修复 AI 检测目录页的拼接图像生命周期问题：所测 `An Invitation to Reflexiv.pdf` 的印刷目录页可被识别。无独立目录时，在回退提示下加入进度条，并排于两个操作按钮左侧。
- 去除“AI 提取目录书签”窗口的全局置顶行为。运行时核对窗口不带 `WS_EX_TOPMOST`。
- 检测、逐页提取和层级判定分别记录请求耗时、尝试次数与成功状态到临时运行目录的 `*.meta.txt`，便于区分平台响应与重试耗时。
- 所有 OpenAI 兼容平台的聊天请求现在默认发送 `enable_thinking: false`。若服务端以 HTTP 400/422 明确拒绝该参数，当前请求自动去掉参数重试一次，并按平台地址和模型在本进程缓存这一兼容结果。部分平台可能忽略该字段，实际速度仍取决于模型、图像大小和服务端负载。
- 使用已配置的 Step `step-5-preview` 发起短文本请求，携带 `enable_thinking: false` 返回 HTTP 200，耗时约 2.2 秒。未打印或记录密钥。
- `bun ./cmd/build.ts --out-dir out/aitoc-preview3` 编译成功：0 个错误、5 个既有依赖代码警告；输出为 `out/aitoc-preview3/SumatraPDF-Plus.exe`。`git diff --check` 通过。
- `out/aitoc-preview3/test_util.exe` 通过全部 102052 项单元测试；已将便携平台配置复制到新预览目录，便于继续试用（配置文件被 Git 忽略）。
- 编辑期间，补丁工具曾使未跟踪源文件短暂读为全零；已从磁盘恢复原文，并在再次写入前保存独立备份。最终编译所用源文件内容完整。

## 后续验证

- 在应用内用扫描 PDF 再次走完 AI 检测、目录提取、层级判定和手工确认书签；短文本 API 探针与构建不能证明完整 PDF 工作流。
- 对不接受 `enable_thinking` 的平台，以实际连接测试验证兼容重试与缓存；当前已编译检查，未找到可安全调用的真实拒绝该参数的平台。

## 2026-09-28：同步 dengxibo/sumatrapdf-plus 最新上游

- 基于上游 `2c1ce45`（3.7.33 后续修复）融合 8 个提交，保留 AI 目录模块与原有配置。隔离分支为 `codex/upstream-sync-20260928`，原 `main` 未改动。
- 合并时有 3 处文本冲突：`src/Settings.h` 和两份翻译文件。依据合并后的 `cmd/gen-settings.ts` 重新生成设置头文件；两份翻译均以最新上游为基础，补入 AI 目录的 30 组中/繁译文。
- 首次构建暴露上游生成脚本遗漏 `FileState.AutoOcrOn`：上游已生成头文件有该字段，脚本却没有。现已将字段写入生成脚本，重新生成头文件及设置文档，避免后续再次生成时丢失。
- `bun ./cmd/build.ts --out-dir out/aitoc-upstream-preview` 构建通过：0 警告、0 错误；`test_util.exe` 通过全部 102290 项单元测试。融合版完整扫描 PDF 流程尚未再次实测。
- 用户已确认自己是 autoContents 原版权人或已取得授权，允许这部分移植代码按 GPLv3 提交上游；源文件和使用文档已补充来源声明。PR 将如实说明融合版尚未重新走完整扫描 PDF 流程。
- 上游草稿 PR [#93](https://github.com/dengxibo/sumatrapdf-plus/pull/93) 已创建：4965898:codex/upstream-sync-20260928 → dengxibo:main，初始融合提交 34bc027。GitHub 显示 22 个改动文件、可自动合并；创建时自动检查尚未返回。保留草稿状态，等待融合版完整 PDF 实测与上游评审。
