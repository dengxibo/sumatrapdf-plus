═══════════════════════════════════════
  Sumatra PDF Plus 测试版
  基于 SumatraPDF 修改 · 非官方版本
═══════════════════════════════════════

【这是什么】
  Windows 下的 PDF / EPUB / MOBI 等电子书阅读器。
  在 SumatraPDF 基础上针对国内常用电子书做了优化，
  侧重：中文 EPUB/MOBI 排版、打开速度、离线查词、暗色主题等。

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

【双击查词（离线词典）】
  1. 在 exe 同目录下建立 dict 文件夹，例如：
       YourFolder\dict\
  2. 将词典文件放入 dict，例如：
       SumatraDict.idx / SumatraDict.dat      （英汉）
       SumatraDictZh.idx / SumatraDictZh.dat  （汉语词典，如有）
  3. 工具栏点「查词」图标，确保已开启
  4. 在正文里双击词语即可查词

  若无词典文件，阅读功能仍可用，只是无法查词。

【界面提示】
  · 工具栏可切换亮/暗主题
  · EPUB 多数优先用内置排版引擎，部分书籍会回退到备用引擎
  · 字体说明见 fonts\README.txt

【反馈】
  试用 EPUB / MOBI / PDF，如有问题请说明书名与现象。

【许可】
  基于 SumatraPDF，遵循 GPLv3。
  分发本程序二进制文件时，请同时提供对应的源代码。

【上游项目】
  https://www.sumatrapdfreader.org/
