/****************************************************************************
 * foc_agent_skill.c
 *
 * FOCPilot 运行时 Skill 安装实现
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "foc_agent_skill.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ai_agent 的数据目录是可配置的 (CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR),
 * 所以这里不能写死。本板的 Kconfig 默认跟随 ai_agent 的同名选项, 两边不会
 * 走岔 —— 写错目录的后果是 Skill 被装进一个 agent 根本不扫的文件夹, 而
 * 表面上看一切正常。下面的 #ifndef 只是给不经过 Kconfig 的构建兜底。 */

#ifndef CONFIG_EXAMPLES_FOCSCOPE_AGENT_DATA_DIR
#  define CONFIG_EXAMPLES_FOCSCOPE_AGENT_DATA_DIR "/data/agent"
#endif

#define FOC_AGENT_DIR        CONFIG_EXAMPLES_FOCSCOPE_AGENT_DATA_DIR
#define FOC_AGENT_SKILLS_DIR FOC_AGENT_DIR "/skills"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: read_file_into
 *
 * Description:
 *   把 path 的全部内容读进 buf (最多 buf_size-1 字节, 保证 NUL 结尾)。
 *   文件不存在或读失败返回 -1, 否则返回读到的字节数。
 *
 ****************************************************************************/

static int read_file_into(FAR const char *path, FAR char *buf, size_t buf_size)
{
    FILE *fp;
    size_t n;

    fp = fopen(path, "r");
    if (fp == NULL)
    {
        return -1;
    }

    n = fread(buf, 1, buf_size - 1, fp);
    fclose(fp);

    buf[n] = '\0';
    return (int)n;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: foc_agent_skill_install
 *
 * Description:
 *   把 g_foc_agent_skills[] 里的 Skill 安装到 /data/agent/skills/。
 *
 *   写入策略: 先读现有文件比对内容, 一致就跳过。这样每次开机不会重复擦写
 *   flash, 而固件升级带来内容变化时又会自动更新。
 *
 ****************************************************************************/

int foc_agent_skill_install(void)
{
    int installed = 0;
    int i;

    /* ai_agent 正常启动时也会建这两个目录, 但我们的应用可能先跑,
     * 所以这里自己确保存在 (已存在返回 EEXIST, 不算错误)。 */

    if (mkdir(FOC_AGENT_DIR, 0755) != 0 && errno != EEXIST)
    {
        printf("[skill] 无法创建 %s: %d\n", FOC_AGENT_DIR, errno);
        return -1;
    }

    if (mkdir(FOC_AGENT_SKILLS_DIR, 0755) != 0 && errno != EEXIST)
    {
        printf("[skill] 无法创建 %s: %d\n", FOC_AGENT_SKILLS_DIR, errno);
        return -1;
    }

    for (i = 0; i < g_foc_agent_skills_count; i++)
    {
        FAR const foc_agent_skill_t *skill = &g_foc_agent_skills[i];
        char path[128];
        size_t len = strlen(skill->content);
        FAR char *cur;
        bool same;
        FILE *fp;

        snprintf(path, sizeof(path), "%s/%s", FOC_AGENT_SKILLS_DIR,
                 skill->name);

        /* 已经装过同样内容就跳过。
         * 缓冲区多给一个字节: read_file_into() 最多读 buf_size-1 = len+1 字节,
         * 盘上文件比新内容长时读回 len+1 != len, 于是判为不同、走覆盖分支 ——
         * 否则"新内容恰好是旧文件前缀"会让缩短过的 Skill 永远更新不掉。 */

        cur = malloc(len + 2);
        if (cur == NULL)
        {
            printf("[skill] 内存不足, 跳过 %s\n", skill->name);
            continue;
        }

        same = (read_file_into(path, cur, len + 2) == (int)len &&
                memcmp(cur, skill->content, len) == 0);
        free(cur);

        if (same)
        {
            continue;
        }

        fp = fopen(path, "w");
        if (fp == NULL)
        {
            printf("[skill] 无法写入 %s: %d\n", path, errno);
            continue;
        }

        if (fwrite(skill->content, 1, len, fp) != len)
        {
            printf("[skill] 写入 %s 不完整\n", path);
            fclose(fp);
            continue;
        }

        fclose(fp);
        installed++;

        printf("[skill] 已安装 %s (%zu 字节)\n", path, len);
    }

    if (installed > 0)
    {
        printf("[skill] 共安装 %d 个 Skill 到 %s\n", installed,
               FOC_AGENT_SKILLS_DIR);
    }

    return installed;
}
