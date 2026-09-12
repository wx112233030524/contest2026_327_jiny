#!/usr/bin/env python3
"""把本目录下的 Skill Markdown 源文件转成 C 字符串源文件。

板子的固件里没法直接读仓库里的 .md, 所以技能正文必须以 C 字符串形式编进固件,
由 foc_agent_skill_install() 在应用启动时写到 /data/agent/skills/。

源文件是本目录下的 *.md; 生成物是 ../foc_agent_skill_data.c。
改了 .md 之后重新跑一次这个脚本。

用法:
    python3 gen_c_string.py
"""

import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE.parent / "foc_agent_skill_data.c"

HEADER = '''/****************************************************************************
 * foc_agent_skill_data.c
 *
 * 自动生成, 请勿手工编辑 —— 改 agent_skill 目录下的 Markdown 后重新运行
 *   python3 agent_skill/gen_c_string.py
 *
 * 内容来源:
%s
 ****************************************************************************/

#include "foc_agent_skill.h"

'''

FOOTER = '''
const foc_agent_skill_t g_foc_agent_skills[] =
{
%s};

const int g_foc_agent_skills_count =
    sizeof(g_foc_agent_skills) / sizeof(g_foc_agent_skills[0]);
'''


def c_escape(text: str) -> str:
    """把文本转义成 C 字符串字面量, 按行拆分以便阅读。"""
    out = []
    for line in text.split("\n"):
        esc = (
            line.replace("\\", "\\\\")
                .replace('"', '\\"')
                .replace("\t", "\\t")
        )
        out.append('    "%s\\n"' % esc)
    return "\n".join(out)


def main() -> int:
    mds = sorted(HERE.glob("*.md"))
    if not mds:
        print("没有找到 .md 源文件", file=sys.stderr)
        return 1

    names = ", ".join(m.name for m in mds)
    body = HEADER % ("  " + names)

    for i, md in enumerate(mds):
        text = md.read_text(encoding="utf-8")
        const = "g_skill_%s" % md.stem.replace("-", "_")
        body += "static const char %s[] =\n%s;\n\n" % (const, c_escape(text))

    entries = "".join(
        '    { "%s", g_skill_%s },\n' % (md.name, md.stem.replace("-", "_"))
        for md in mds
    )
    body += FOOTER % entries

    OUT.write_text(body, encoding="utf-8")
    print("已生成 %s (%d 个 Skill, %d 字节)" % (OUT.name, len(mds), len(body)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
