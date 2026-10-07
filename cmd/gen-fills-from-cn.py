#!/usr/bin/env python3
"""
Generate major-lang fills for all gap keys using:
- existing cn (preferred) / English key
- hand rules + optional MyMemory with short timeout
Writes cmd/_fills_part_auto.json incrementally.
"""
from __future__ import annotations

import json
import re
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "cmd" / "_i18n-gap-report.json"
OUT = ROOT / "cmd" / "_fills_part_auto.json"
MAJORS = ["cn", "tw", "ja", "kr", "de", "fr", "es", "it", "pt", "br", "ru", "uk", "pl", "nl", "tr", "vn"]

# Single-char OpenCC-ish map (maketrans requires length-1 keys)
S2T = str.maketrans(
    {
        "档": "檔",
        "国": "國",
        "删": "刪",
        "默": "預",  # incomplete alone; multi-char reps handle 默认
        "设": "設",
        "选": "選",
        "择": "擇",
        "页": "頁",
        "号": "號",
        "码": "碼",
        "识": "識",
        "认": "認",
        "倾": "傾",
        "软": "軟",
        "对": "對",
        "启": "啟",
        "动": "動",
        "复": "復",
        "时": "時",
        "间": "間",
        "长": "長",
        "宽": "寬",
        "显": "顯",
        "侧": "側",
        "栏": "欄",
        "标": "標",
        "题": "題",
        "链": "鏈",
        "错": "錯",
        "误": "誤",
        "败": "敗",
        "请": "請",
        "输": "輸",
        "确": "確",
        "导": "導",
        "进": "進",
        "检": "檢",
        "语": "語",
        "汉": "漢",
        "简": "簡",
        "体": "體",
        "无": "無",
        "与": "與",
        "并": "並",
        "从": "從",
        "这": "這",
        "个": "個",
        "为": "為",
        "会": "會",
        "经": "經",
        "过": "過",
        "还": "還",
        "没": "沒",
        "该": "該",
        "说": "說",
        "话": "話",
        "发": "發",
        "现": "現",
        "开": "開",
        "关": "關",
        "图": "圖",
        "载": "載",
        "处": "處",
        "结": "結",
        "书": "書",
        "签": "籤",
        "注": "註",
        "颜": "顏",
        "滚": "捲",
        "扫": "掃",
        "描": "描",
        "窗": "視",
        "帮": "幫",
        "助": "助",
        "复": "複",
        "制": "製",
        "查": "查",
        "找": "找",
        "信": "訊",
        "息": "息",
        "网": "網",
        "络": "路",
        "数": "數",
        "据": "據",
        "密": "密",
        "码": "碼",
    }
)


def cn_to_tw(s: str) -> str:
    # Multi-char replacements first
    reps = [
        ("打开", "開啟"),
        ("关闭", "關閉"),
        ("删除", "刪除"),
        ("默认", "預設"),
        ("设置", "設定"),
        ("选择", "選擇"),
        ("文档", "文件"),
        ("文件夹", "資料夾"),
        ("目录", "目錄"),
        ("书签", "書籤"),
        ("标注", "註解"),
        ("颜色", "顏色"),
        ("字体", "字型"),
        ("滚动", "捲動"),
        ("扫描", "掃描"),
        ("保存", "儲存"),
        ("复原", "還原"),
        ("恢复", "還原"),
        ("识别", "辨識"),
        ("链接", "連結"),
        ("窗口", "視窗"),
        ("菜单", "功能表"),
        ("工具栏", "工具列"),
        ("标题栏", "標題列"),
        ("文件", "檔案"),
        ("关于", "關於"),
        ("帮助", "說明"),
        ("打印", "列印"),
        ("复制", "複製"),
        ("粘贴", "貼上"),
        ("剪切", "剪下"),
        ("查找", "尋找"),
        ("替换", "取代"),
        ("信息", "資訊"),
        ("错误", "錯誤"),
        ("警告", "警告"),
        ("网络", "網路"),
        ("数据", "資料"),
        ("用户", "使用者"),
        ("密码", "密碼"),
        ("账号", "帳號"),
        ("视频", "影片"),
        ("音频", "音訊"),
        ("软件", "軟體"),
        ("硬件", "硬體"),
        ("鼠标", "滑鼠"),
        ("键盘", "鍵盤"),
        ("单击", "單擊"),
        ("双击", "按兩下"),
        ("右键", "右鍵"),
        ("本地", "本機"),
        ("服务器", "伺服器"),
        ("质量", "品質"),
        ("分辨率", "解析度"),
        ("打印机", "印表機"),
        ("电子邮件", "電子郵件"),
        ("回收站", "資源回收筒"),
    ]
    out = s
    for a, b in reps:
        out = out.replace(a, b)
    return out.translate(S2T)


def extract_mnemonic(key: str):
    m = re.search(r"&([A-Za-z0-9])", key)
    if not m:
        return key, None
    return key.replace("&", "", 1), m.group(1).upper()


def apply_mnemonic(text: str, letter: str | None, lang: str) -> str:
    if not letter or "&" in text:
        return text
    if lang in {"cn", "tw", "ja", "kr", "vn"}:
        return f"{text}(&{letter})"
    low = letter.lower()
    for i, ch in enumerate(text):
        if ch.lower() == low:
            return text[:i] + "&" + text[i:]
    return f"{text} (&{letter})"


def is_useless(key: str, val: str | None) -> bool:
    if not val:
        return True
    return key.replace("&", "") == val.replace("&", "")


def mymemory(text: str, lang: str, timeout=8.0) -> str | None:
    langmap = {
        "cn": "zh-CN",
        "tw": "zh-TW",
        "ja": "ja-JP",
        "kr": "ko-KR",
        "de": "de-DE",
        "fr": "fr-FR",
        "es": "es-ES",
        "it": "it-IT",
        "pt": "pt-PT",
        "br": "pt-BR",
        "ru": "ru-RU",
        "uk": "uk-UA",
        "pl": "pl-PL",
        "nl": "nl-NL",
        "tr": "tr-TR",
        "vn": "vi-VN",
    }
    q = urllib.parse.urlencode({"q": text, "langpair": f"en|{langmap[lang]}"})
    url = f"https://api.mymemory.translated.net/get?{q}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        return data.get("responseData", {}).get("translatedText")
    except Exception:
        return None


BRAND = {"ChatGPT", "DeepSeek", "Doubao", "OCR", "AI", "PDF", "KB", "MB", "GB", "OK", "(dbg)", "You", "You:", "AI:", "none", "DPI:", "On", "Off", "Set", "Play", "Copy", "Send", "Menu", "Settings", "Source", "Page", "Chinese", "Conservative", "Detailed", "GB"}


def main():
    rows = json.loads(REPORT.read_text(encoding="utf-8"))
    fills = {}
    if OUT.exists():
        fills = json.loads(OUT.read_text(encoding="utf-8"))
        print(f"resume {len(fills)}")

    for idx, row in enumerate(rows):
        key = row["key"]
        entry = dict(fills.get(key, {}))
        plain, letter = extract_mnemonic(key)

        # cn
        if is_useless(key, entry.get("cn")):
            if row.get("cn") and not is_useless(key, row["cn"]):
                entry["cn"] = row["cn"]
            elif key in BRAND or plain in BRAND:
                entry["cn"] = plain
            else:
                t = mymemory(plain, "cn")
                entry["cn"] = t or plain
                time.sleep(0.2)

        # tw from cn
        if is_useless(key, entry.get("tw")):
            entry["tw"] = cn_to_tw(entry["cn"]) if entry.get("cn") else plain

        for lang in MAJORS:
            if lang in {"cn", "tw"}:
                continue
            if not is_useless(key, entry.get(lang)):
                continue
            if key in BRAND or plain in BRAND:
                entry[lang] = plain
                continue
            # Prefer ja from report if good
            if lang == "ja" and row.get("ja") and not is_useless(key, row["ja"]):
                entry["ja"] = row["ja"]
                continue
            t = mymemory(plain, lang)
            if t and not is_useless(plain, t):
                entry[lang] = apply_mnemonic(t, letter, lang)
            else:
                # last resort: keep English plain with mnemonic for European; for CJK use cn
                if lang in {"ja", "kr"} and entry.get("cn"):
                    # temporary bridge — better than English for CJK UI
                    entry[lang] = apply_mnemonic(entry["cn"], letter, lang) if lang == "kr" else apply_mnemonic(plain, letter, lang)
                    if lang == "ja":
                        # try once more slowly
                        time.sleep(0.5)
                        t2 = mymemory(plain, "ja")
                        if t2 and not is_useless(plain, t2):
                            entry["ja"] = apply_mnemonic(t2, letter, "ja")
                else:
                    entry[lang] = apply_mnemonic(plain, letter, lang)
            time.sleep(0.15)

        # apply mnemonic on cn/tw if needed
        if letter:
            for lang in ("cn", "tw"):
                if entry.get(lang) and "&" not in entry[lang] and "(&" not in entry[lang]:
                    # only if English key had mnemonic
                    if "&" in key:
                        entry[lang] = apply_mnemonic(entry[lang], letter, lang)

        fills[key] = entry
        if (idx + 1) % 10 == 0 or idx + 1 == len(rows):
            OUT.write_text(json.dumps(fills, ensure_ascii=False, indent=2), encoding="utf-8")
            print(f"[{idx+1}/{len(rows)}] saved, last={key[:50]!r}", flush=True)

    OUT.write_text(json.dumps(fills, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"done {len(fills)} -> {OUT}")


if __name__ == "__main__":
    main()
