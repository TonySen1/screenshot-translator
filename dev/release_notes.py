# -*- coding: utf-8 -*-
"""从 CHANGELOG.md 里抽出某个版本的说明，给 GitHub Release 当正文用。

用法：
    python dev/release_notes.py v1.2.0            # 打到标准输出
    python dev/release_notes.py v1.2.0 out.md     # 写到文件

CI 在 tag 推送后调用它。抽不到对应版本时退出码 1，调用方可以退回
gh release 自带的 --generate-notes。

为什么需要这个：本仓库一直是直接往 main 提交、不走 PR，而
--generate-notes 是按 PR 标题生成的，所以它只会吐出一行
"Full Changelog" 链接。v1.1.0 的说明是当时手工贴进去的。
"""

import io
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHANGELOG = os.path.join(HERE, "CHANGELOG.md")

# 形如 "## v1.2.0 — 2026-10-16"
_HEADING = r"^##\s+{tag}(?:\s|$).*$"
# 下一节的起点，用来确定当前节的结尾
_NEXT = re.compile(r"^##\s+v\d", re.MULTILINE)


def extract(text: str, tag: str) -> str:
    """取出 `## <tag> ...` 那一节，到下一个 `## v...` 之前为止。"""
    match = re.compile(_HEADING.format(tag=re.escape(tag)), re.MULTILINE).search(text)
    if match is None:
        return ""
    nxt = _NEXT.search(text, match.end())
    section = text[match.start():nxt.start() if nxt else len(text)]
    # 去掉节尾那根 --- 分隔线
    section = re.sub(r"\n+-{3,}\s*$", "", section.rstrip())
    return section.strip()


def main(argv) -> int:
    tag = argv[1] if len(argv) > 1 else os.environ.get("GITHUB_REF_NAME", "")
    if not tag:
        sys.stderr.write("用法：release_notes.py <tag> [输出文件]\n")
        return 2
    with io.open(CHANGELOG, encoding="utf-8") as handle:
        text = handle.read()
    notes = extract(text, tag)
    if not notes:
        sys.stderr.write("CHANGELOG.md 里没有 %s 这一节\n" % tag)
        return 1
    if len(argv) > 2:
        with io.open(argv[2], "w", encoding="utf-8", newline="\n") as handle:
            handle.write(notes + "\n")
    else:
        sys.stdout.write(notes + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
