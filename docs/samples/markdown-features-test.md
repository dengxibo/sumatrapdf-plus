# Markdown Features Test

Standard GFM feature checklist for SumatraPDF.

## Headings

### Heading 3

#### Heading 4

## Emphasis

*italic* **bold** ***bold italic*** ~~strikethrough~~

## Links

[SumatraPDF](https://www.sumatrapdfreader.org/free-pdf-reader.html)

## Lists

1. First ordered
2. Second ordered
   1. Nested ordered
3. Third ordered

- Unordered one
- Unordered two
  - Nested unordered

- [x] Task done
- [ ] Task todo

## Blockquote

> This is a blockquote.
> Second line in blockquote.

## Horizontal rule

Above the rule.

---

Below the rule.

## Inline code

Use `printf("hello")` in a sentence.

## Fenced code block

```python
def greet(name):
    print(f"Hello, {name}!")

greet("SumatraPDF")
```

## Table

| Feature | Supported | Notes |
| --- | --- | --- |
| Tables | yes | GFM pipe tables |
| Code blocks | yes | Fenced blocks |
| HR | yes | `---` |
| 中文 | yes | UTF-8 text |

## Table alignment

| Left | Center | Right |
| :--- | :---: | ---: |
| L1 | C1 | R1 |
| 左 | 中 | 右 |

## Chinese text

这是一段中文正文，用于测试 UTF-8 与 CJK 字体。
