/****************************************************************************
 * foc_agent_skill.h
 *
 * FOCPilot 运行时 Skill 安装
 *
 * 把 FOCPilot 的 Skill 装到 openvela 板载 ai_agent 的技能目录
 * (/data/agent/skills/) 下, 让用户可以用自然语言驱动电机整定。
 *
 * 背景: ai_agent 的 skill_loader 只在启动时安装它自己硬编码的 10 个内置
 * Skill (见 packages/ai_agent/src/tools/skill_loader.c), 并不认识我们的
 * 技能文件。所以必须由我们自己的应用在运行时写进去。
 *
 ****************************************************************************/

#ifndef __FOC_AGENT_SKILL_H
#define __FOC_AGENT_SKILL_H

#include <nuttx/config.h>
#include <nuttx/compiler.h>   /* FAR */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 一个待安装的 Skill: 目标文件名 + Markdown 正文 */
typedef struct
{
    FAR const char *name;       /* 目标文件名, 如 "motor-tuning.md" */
    FAR const char *content;    /* Markdown 正文 */
} foc_agent_skill_t;

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* 由 agent_skill/gen_c_string.py 从 agent_skill 目录下的 Markdown 生成 */
extern const foc_agent_skill_t g_foc_agent_skills[];
extern const int g_foc_agent_skills_count;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* 把内置 Skill 安装到 /data/agent/skills/。
 *
 * 已存在且内容一致的文件不会被重写 (避免每次都擦写 flash); 内容有变化
 * 则覆盖, 这样升级固件后 Skill 会跟着更新。
 *
 * 返回实际写入的文件数; 目录不可用时返回负值。 */
int foc_agent_skill_install(void);

#endif /* __FOC_AGENT_SKILL_H */
