═══════════════════════════════════════
  Sumatra PDF Plus
  基于 SumatraPDF 修改
═══════════════════════════════════════

【这是什么】
  Windows 下的 PDF / EPUB / MOBI 等电子书阅读器。
  在 SumatraPDF 基础上针对国内常用电子书做了优化，
  侧重：中文 EPUB/MOBI 排版、打开速度、离线查词、繁体查词、暗色主题等。

  ※ 非 sumatrapdfreader.org 官方版本，基于 SumatraPDF (GPLv3) 的社区修改版。

【双屏 DPI 适配】
  原版 Sumatra PDF 在双屏、不同缩放比例之间拖动窗口时，书签侧栏、
  工具栏、菜单栏、标签栏等字号长期容易错乱。本版本已修复：跨屏拖动
  过程中即时按各显示器 DPI 刷新 UI，并修复标签拖出新窗口后原窗口
  工具栏字体异常变大的问题。

【支持的格式（常见）】
  PDF、EPUB、MOBI、AZW / AZW3、FB2、CHM、CBZ/CBR、DjVu 等

【使用方法】
  1. 解压整个文件夹到任意位置（路径尽量不要有特殊符号）
  2. 双击 SumatraPDF.exe 运行
  3. 不要只复制 exe，请保留同目录下的：
     - libmupdf.dll（如有）
     - fonts 文件夹
     - opencc 文件夹（繁体 PDF 查词需要，见下文）
     - dict 文件夹（离线查词需要，见下文）

【配置文件】
  程序会把用户设置保存在 SumatraPDF-settings.txt：

  · 便携版：与 SumatraPDF.exe 同目录
  · 安装版：%LOCALAPPDATA%\SumatraPDF\SumatraPDF-settings.txt

  本发行包附带一份带中文说明的参考模板：
    SumatraPDF-settings-annotated.txt

  用法：
  1. 程序保存设置时会自动在每个配置项后写入中文注释
  2. 删除 SumatraPDF-settings.txt 后重启，会按默认值重建带注释的配置文件
  3. 也可参考 SumatraPDF-settings-annotated.txt（与程序生成格式一致）

  注意：
  · 注释写在配置值同一行后面，以 # 开头，例如：CheckForUpdates = true # 是否每天自动检测新版本
  · 文件末尾 FileStates、SessionData 等由程序自动写入；其中动态数据项通常不带注释

【双击查词（离线词典）】
  1. 在 exe 同目录下建立 dict 文件夹，例如：
       YourFolder\dict\
  2. 将词典文件放入 dict，例如：
       SumatraDict.idx / SumatraDict.dat      （英汉）
       SumatraDictZh.idx / SumatraDictZh.dat  （汉语词典，如有）
  3. 工具栏点「查词」图标，或设置 EnableDoubleClickWordLookup = true
  4. 在正文里双击词语即可查词

  也可用高级设置 OfflineDictionaryPath 指定其他词典目录。
  若无词典文件，阅读功能仍可用，只是无法查词。

【繁体中文 PDF 查词】
  查繁体 PDF 时，程序会用 OpenCC 将文字转为简体后再查词典。
  请保留 exe 同目录下的 opencc 文件夹（内含 t2s.json 等数据文件）。
  仅升级 exe、不复制 opencc，繁体查词将不可用。

【Ask AI（在线）】
  · EnableAskAI = true       显示 Ask AI 入口
  · AiChatProvider = doubao  可选 doubao / deepseek / chatgpt
  出现在划词工具栏、右键菜单、命令面板中，需要网络权限。

  注意：EnableDoubleClickWordLookup 是「离线词典查词」，不是 AI。

【常用设置速查】
  Theme = Light-Warm              主题（Light-Warm / Light-White / Dark-Dracula / Dark-Black）
  DocumentColorMode = smart       文档颜色模式：smart=自动 / original=原稿 / theme=强制主题
  SearchUIFloating = false        false=顶部搜索栏；true=悬浮搜索窗
  Scrollbars = windows            windows / smart / overlay / hidden
  UseTabs = true                  标签页模式
  ShowMenubarWithTabs = false     标签模式下菜单栏（UseTabs=true 时以此为准）
  RestoreSession = true           启动恢复上次会话
  Annotations [ SelectionToolbar ]  划词后浮动工具栏

  完整说明见 SumatraPDF-settings-annotated.txt

【界面提示】
  · 工具栏可切换亮/暗主题
  · EPUB 多数优先用内置排版引擎，部分书籍会回退到备用引擎
  · 字体说明见 fonts\README.txt
  · 朗读（TTS）与自然语音配置见 docs\md\Read-Aloud-TTS.md

【反馈】
  试用 EPUB / MOBI / PDF，如有问题请说明书名与现象。
  GitHub: https://github.com/dengxibo/sumatrapdf-plus/issues

【许可】
  基于 SumatraPDF，遵循 GPLv3。
  分发本程序二进制文件时，请同时提供对应的源代码。

【上游项目】
  https://www.sumatrapdfreader.org/
