# Mermaid 原生渲染测试

## 中文流程图

```mermaid
flowchart TD
    A[打开 Markdown] --> B{包含 Mermaid？}
    B -->|是| C[Rust 生成 SVG]
    B -->|否| D[显示正文]
    C --> D
```

## 时序图

```mermaid
sequenceDiagram
    participant A as 阅读器
    participant B as 渲染器
    A->>B: 请求生成图表
    B-->>A: 返回 SVG
```

## 特殊字符

```mermaid
flowchart LR
    A["A & B"] --> B["x < y"]
```

## 无效语法应保留代码

```mermaid
this is not a diagram
```

## 普通代码应保持原样

```cpp
const char* diagram = "flowchart LR; A-->B";
```

图表后的正文应正常显示。

## 多行节点文字不应重叠

```mermaid
flowchart LR
    A["Settings.txt\nFontName"] --> B["AppSettings.cpp\nReloadSettings"]
    B --> C["EngineEbook.cpp\nSetDefaultEbookFont"]
    C --> D["HtmlFormatter\nSetFontName"]
```
