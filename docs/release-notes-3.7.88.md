# SumatraPDF Plus 3.7.88

## 中文

- 修复 EPUB 插图尺寸，保留原书的小、中、大图比例，避免普通插图被按页面高度放大或退回原始图片大小。
- 修正块级图片百分比宽度重复计算，以及侧栏、并排图片的尺寸参照；保留图片宽高比及边框。
- 普通插图不再强制另起一页；能放入一页的图片与图注尽量保持在一起，过长图注自然分页，超大图片等比缩小以适应页面。
- 本次针对 #121 先修正现有排版，尚未新增手动图片缩放选项。

下载 Windows x64 EXE。退出程序后替换安装目录中的 EXE，保留配置及配套文件。

## English

- Preserve publisher-defined EPUB illustration sizes instead of forcing ordinary figures to a page-derived height or their intrinsic pixel size.
- Fix percentage widths being applied twice to block images, and correct sizing relative to sidebar containers and table cells. Preserve image aspect ratios and borders.
- Remove forced page breaks for ordinary illustrations. Keep a recognized image and caption together when they fit on one page, allow long captions to paginate, and proportionally shrink oversized images to fit.
- This release addresses the layout problems discussed in #121; it does not add manual image zoom controls.

Download the Windows x64 EXE. Close the application before replacing the EXE; retain settings and supporting files.
