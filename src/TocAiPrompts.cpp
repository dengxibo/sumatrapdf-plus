/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "TocAiPrompts.h"

static const char* kPrintedTocCoreRules =
    "你正在帮助 PDF 阅读器恢复一本扫描书籍中的印刷目录。图片按原书顺序排列。请根据视觉布局、缩进、"
    "字体、粗细、居中位置、上下留白、点线、页码和双栏关系恢复目录结构。"
    "忠于原文，保留语言、编号、选学星号和有语义的标点，不翻译、不把圈号擅自改为其他编号。排除页眉页脚，去掉标题与页码之"
    "间的引导点。"
    "不要猜测缺失页码，保留实际目录标题，无页码时用 null；页码推断由软件后续校准处理。"
    "页码是目录中印刷的页码：阿拉伯数字保留数字（如 12）；罗马数字或附录页码保留文字（如 xiv、R1）；"
    "没有页码时用 null。"
    "level 从 1 "
    "开始。特别注意：目录中可能存在没有‘第X章’编号、但通过居中、加粗、字号更大、上下留白或单独成行来划分大分段的标题。"
    "这类分段标题也是一级目录项，必须单独输出，不能丢弃，也不能挂到后面的章节下面。例如‘地球和地图’、‘中国地理’、‘自然"
    "地理’等，"
    "即使没有章节编号，也应输出为 level:1，并使用该标题右侧对应的页码。"
    "层级必须按目录的视觉分组关系判断，而不是只看‘第X章’文字：如果一个无编号的大标题（如‘地球和地图’）位于若干章之前并"
    "作为总分段标题，"
    "则它是父级 level:1；属于该分段的‘第一章 地球’、‘第二章 地图’必须改为 "
    "level:2；这些章下面的‘第一节’、‘第二节’必须改为 level:3。"
    "同理，‘中国地理’是 level:1，它下面的‘第一章 疆域和行政区划’、‘第二章 人口和民族’、‘第三章 地形’等必须是 level:2，"
    "这些章下面的‘第一节’、‘课堂练习’必须是 "
    "level:3。不要因为章节文字本身通常是一级，就忽略目录中更高层的无编号分段标题。"
    "例如本目录应形成：地球和地图(1) → 第一章 地球(2) → 第一节 地球的形状和经纬网(3)；"
    "地球和地图(1) → 第二章 地图(2) → 课堂练习(3)；中国地理(1) → 第一章 疆域和行政区划(2)。"
    "如果无编号分段标题右侧没有印刷页码，可以暂时输出 "
    "page:null，但仍必须保留其父级层次；不要删除它，也不要把后续章节提升为同级。"
    "不要把无编号分段标题误当成普通说明文字，也不要把它与相邻章节合并。‘第一节’、‘第二节’等明确属于所在章节的下一级；"
    "‘课堂练习’若在章节条目缩进下也属于所在章节的下一级。"
    "同一行里若有多个带括号页码的小条目，必须逐条拆开，禁止合并成一个标题。"
    "例如一行「一、多元函数概念(1) 二、二元函数的极限(5) 三、二元函数的连续性(8) 习题8-1(11)」要输出四条，"
    "而不是一条 title 里连写全部。"
    "每个「一、」「二、」「三、」等条目单独一条 item：title 保留编号，写成「一、多元函数概念」，"
    "不要把页码括号留在 title 里；page 取括号内的数字。半角 (1) 与全角（1）同样处理。"
    "「习题8-1(11)」「习题 8-1（11）」也单独一条：title 为「习题8-1」（保留习题编号，去掉括号页码，题号前的空格去掉），"
    "page 为括号内数字。"
    "行首 * 表示选学，留在 title 里，例如「*二、全微分在近似计算中的应用」。"
    "当目录顶层就是「第X章」时，层级固定为：章 level=1，节 level=2，节下面的「一、」「二、」和习题 level=3。"
    "节名右侧点线后的页码是这一节自己的 page，不要用它替换节内第一条的页码。"
    "只有章的上面还有无编号大分段标题时，才按前面的规则把章、节、节内条目整体下移一层。"
    "书签只能显示一行普通文字，标题里的公式必须写成同一行 Unicode，整段留在同一个 title 字符串里，禁止换行把公式拆出 "
    "输出。"
    "禁止 LaTeX：不要 $...$、不要反斜杠命令（如 \\lambda、\\frac、\\sqrt、\\sin、\\cos、\\int、\\sum、\\partial）。"
    "希腊字母直接写字符（α β γ δ ε θ λ μ π σ φ ω Δ Σ Ω）。"
    "函数名写成 sin、cos、tan、ln、log，不要加反斜杠。"
    "单字符上标用 Unicode 上标（x²、xⁿ、y⁽ⁿ⁾）；多字符上标写成 ^( )，例如 e^(λx)、e^(n+1)。"
    "单字符下标用 Unicode 下标（aₙ、x₁、x₂）；没有对应字符时写成 _ ，例如 P_m(x)。"
    "导数的撇用 ′ ″ ‴，不要用英文单引号：y′、y″，不要写成 y' 或 y''。"
    "分式写成 a/b，根号写成 √(x)，积分写成 ∫，求和写成 Σ，偏导写成 ∂，无穷写成 ∞，不等号写成 ≤ ≥ ≠。"
    "例如「$y''=f(x,y')$型」写成「y″=f(x,y′) 型」；"
    "「$f(x)=e^{\\lambda x}P_m(x)$型」写成「f(x)=e^(λx)P_m(x) 型」；"
    "「$f(x)=e^{\\lambda x}[P_l(x)\\cos\\omega x+P_n(x)\\sin\\omega x]$型」写成"
    "「f(x)=e^(λx)[P_l(x) cos ωx+P_n(x) sin ωx] 型」。"
    "只输出目录中实际印刷的条目，按页面从上到下、从左到右的顺序输出，不要补写图片中不存在的标题。"
    "同一本书若同时印刷了‘按单元目录’和‘按体裁/专题索引’，只输出按阅读顺序的主目录（单元/章节），"
    "不要把体裁索引、作者名行、专题对照表再重复导入一遍。作者名若单独成行且无页码，不要输出为独立条目。";

TempStr BuildPrintedTocPromptTemp(PrintedTocPromptFormat format) {
    const char* wrapper = nullptr;
    switch (format) {
        case PrintedTocPromptFormat::Json:
            wrapper =
                "只返回合法 JSON，不要解释或 Markdown。格式必须是 "
                "{\"items\":[{\"title\":\"章节标题\",\"page\":12,\"level\":1}]}。";
            break;
        case PrintedTocPromptFormat::ExtractCsv:
            wrapper =
                "本阶段只提取标题和印刷页码，不输出层级。只输出 CSV，表头 "
                "title,page_number；半角逗号分隔，字段含逗号或引号时按 CSV 规则双引号转义。缺页码写 null。不要 "
                "Markdown 或解释。";
            break;
        case PrintedTocPromptFormat::LevelsCsv:
            wrapper =
                "本阶段根据图片为输入 CSV 的每个标题判断 level。只输出 CSV，表头 "
                "title,page_number,level；半角逗号分隔，字段含逗号或引号时按 CSV "
                "规则双引号转义。保持输入标题数量、顺序和页码（包括 null），不要猜页码或删除条目。level 为从 1 "
                "开始的整数。参考页只供判断层级，不要重复输出参考页条目；当前页首项未必是一级。不要 Markdown 或解释。";
            break;
    }
    return str::FormatTemp("%s\n\n%s", kPrintedTocCoreRules, wrapper);
}

// First-round body-TOC prompt. The AI receives ONLY compressed heading
// candidates (id / PDF page / original text), never the body text or page
// images. It may select candidates and assign levels; it may not invent
// headings, rewrite titles or guess pages.
static const char* kBodyTocPromptPreamble =
    "你正在帮助一个 PDF 阅读器恢复文档的电子目录（TOC）。\n"
    "\n"
    "下面的数据不是完整正文，而是软件对【整份 PDF】进行本地快速扫描后得到的“疑似标题候选”。\n"
    "每个候选都有唯一 candidate_id。\n"
    "\n"
    "软件本地已经保存：\n"
    "- 原始标题文字\n"
    "- PDF 页码\n"
    "- 页面位置\n"
    "- 跳转目标\n"
    "\n"
    "因此：\n"
    "你不需要猜页码；\n"
    "你不需要生成跳转位置；\n"
    "你不需要重新改写标题。\n"
    "\n"
    "你的任务只有：\n"
    "1. 判断哪些候选是真正的章节 / 小节标题；\n"
    "2. 删除正文短句、页眉页脚、表格内容、图题、表题等噪声；\n"
    "3. 判断真正标题之间的层级；\n"
    "4. 检查文档结构是否可能存在遗漏。\n"
    "\n"
    "请结合：\n"
    "- 标题编号\n"
    "- 编号连续性\n"
    "- 页面顺序\n"
    "- 标题措辞\n"
    "- 全局层级关系\n"
    "进行判断。\n"
    "\n"
    "常见中文结构包括但不限于：\n"
    "第一章 / 第二章\n"
    "第一节 / 第二节\n"
    "一、\n"
    "二、\n"
    "（一）\n"
    "（二）\n"
    "1.\n"
    "1.1\n"
    "1.1.1\n"
    "1、\n"
    "（1）\n"
    "前言\n"
    "引言\n"
    "结论\n"
    "附录\n"
    "附件\n"
    "参考文献\n"
    "\n"
    "但是：\n"
    "编号符合格式不代表一定是标题。\n"
    "没有编号的独立短标题也可能是真标题。\n"
    "\n"
    "<rules>\n"
    "\n"
    "1. 正式 TOC 只能从提供的 candidate 中选择。\n"
    "2. 不得编造 candidate_id。\n"
    "3. 不得编造不存在的标题。\n"
    "4. 不得自己填写或修改 PDF 页码。\n"
    "5. 不得润色、重写或简化标题。\n"
    "6. 如果怀疑存在缺失标题，但候选中没有：不要猜标题，只在 suspected_gaps 中返回需要软件重新检查的 PDF 页码范围。\n"
    "7. level 使用整数：1 = 一级，2 = 二级，3 = 三级，4 = 四级。\n"
    "8. 同一编号体系的层级应尽量保持一致。例如：第一章 → 一、 → （一）；或者 1 → 1.1 → 1.1.1。\n"
    "9. 公文层级规则：一、二、三、为一级；其下是（一）（二）为二级；再下是 1. 2. 为三级；"
    "同一父标题下不应混用（一）与 1. 作为同一级。\n"
    "\n"
    "</rules>\n"
    "\n"
    "<gap_check>\n"
    "\n"
    "完成目录以后检查：\n"
    "\n"
    "A. 编号是否断裂。\n"
    "例如第一章、第二章、第四章可能缺第三章；3.1、3.2、3.4 可能缺 3.3；（一）（二）（四）可能缺（三）。\n"
    "\n"
    "B. 是否存在异常长的无标题 PDF 页区间。\n"
    "如果认为某一范围很可疑，加入 suspected_gaps。suspected_gaps 只是让软件重新检查，不得自行创建缺失标题。\n"
    "\n"
    "</gap_check>\n"
    "\n"
    "<output_format>\n"
    "\n"
    "严格只输出一个 JSON 对象。\n"
    "不要输出 Markdown、代码围栏、解释、前言、总结或注释。\n"
    "\n"
    "格式：\n"
    "{\n"
    "  \"toc\": [\n"
    "    { \"candidate_id\": \"C00012\", \"level\": 1 },\n"
    "    { \"candidate_id\": \"C00018\", \"level\": 2 }\n"
    "  ],\n"
    "  \"suspected_gaps\": [\n"
    "    { \"start_pdf_page\": 63, \"end_pdf_page\": 78, \"reason\": \"章节编号从第二章直接进入第四章，可能存在遗漏\" "
    "}\n"
    "  ]\n"
    "}\n"
    "如果没有可疑遗漏：\"suspected_gaps\": []。\n"
    "\n"
    "</output_format>\n";

static const char* kBodyTocPromptEpilogue =
    "\n</candidates>\n"
    "\n"
    "请恢复文档真实的 TOC。\n"
    "再次强调：只能引用已有 candidate_id；不要编造标题；不要编造页码；只输出 JSON。\n";

char* BuildBodyTocPrompt(int totalPages, const char* candidateDigest) {
    StrBuilder out;
    out.Append(kBodyTocPromptPreamble);
    out.AppendFmt("<document>\n\nPDF_TOTAL_PAGES=%d\n\n</document>\n\n<candidates>\n", totalPages);
    if (candidateDigest) {
        out.Append(candidateDigest);
    }
    out.Append(kBodyTocPromptEpilogue);
    return out.StealData();
}
