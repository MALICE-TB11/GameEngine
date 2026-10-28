"""生成 engine/debug/UiGlyphs.inc：界面里用到、但不在 ImGui「常用 2500 汉字」里的汉字。

为什么需要：ImGui 1.91 的字体图集是启动时一次性烘焙的，只包含指定范围的字形。
「全部汉字」（2 万多字）会让图集膨胀到几十 MB；「常用 2500 字」又缺少「渲、帧、辑、拟」等技术用字，
缺的字会显示成「?」。这个脚本扫描 engine/ 和 sandbox/ 源码里字符串字面量中的汉字，
减去 ImGui 常用字表，把剩下的写进 UiGlyphs.inc，由 ImGuiLayer 追加进字形范围。

用法（在工程根目录）：python tools/update_ui_glyphs.py
新增界面文字后如果出现「?」，重新运行一次并重新构建即可。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGUI_DRAW = os.path.join(ROOT, "build", "_deps", "imgui-src", "imgui_draw.cpp")
OUT = os.path.join(ROOT, "engine", "debug", "UiGlyphs.inc")
SCAN_DIRS = ["engine", "sandbox"]
STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def imgui_common_chinese():
    """解析 imgui_draw.cpp 里 GetGlyphRangesChineseSimplifiedCommon 的累加偏移表。"""
    with open(IMGUI_DRAW, encoding="utf-8") as f:
        src = f.read()
    m = re.search(r"GetGlyphRangesChineseSimplifiedCommon\(\).*?accumulative_offsets_from_0x4E00\[\d*\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit("在 imgui_draw.cpp 中找不到常用汉字表（ImGui 版本变了？）")
    offsets = [int(x) for x in re.findall(r"\d+", m.group(1))]
    chars, code = set(), 0x4E00
    for off in offsets:
        code += off
        chars.add(code)
    return chars


def used_cjk():
    used = set()
    for d in SCAN_DIRS:
        for dirpath, _, files in os.walk(os.path.join(ROOT, d)):
            for name in files:
                if not name.endswith((".cpp", ".h")):
                    continue
                with open(os.path.join(dirpath, name), encoding="utf-8", errors="ignore") as f:
                    for line in f:
                        code = line.split("//")[0] if line.lstrip().startswith("//") else line
                        for lit in STRING_RE.findall(code):
                            used.update(ord(ch) for ch in lit if 0x4E00 <= ord(ch) <= 0x9FFF)
    return used


def main():
    if not os.path.exists(IMGUI_DRAW):
        sys.exit("先构建一次（需要 build/_deps/imgui-src）")
    missing = sorted(used_cjk() - imgui_common_chinese())
    text = "".join(chr(c) for c in missing)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("// 自动生成，请勿手改：python tools/update_ui_glyphs.py\n")
        f.write("// 界面字符串中用到、但不在 ImGui 常用 2500 汉字表里的字（%d 个）。\n" % len(missing))
        f.write('static const char* kUiExtraGlyphs = "%s";\n' % text)
    print("wrote %s (%d glyphs): %s" % (OUT, len(missing), text))


if __name__ == "__main__":
    main()
