/****************************************************************************
 * foc_agent_skill_data.c
 *
 * 自动生成, 请勿手工编辑 —— 改 agent_skill 目录下的 Markdown 后重新运行
 *   python3 agent_skill/gen_c_string.py
 *
 * 内容来源:
  motor-tuning.md
 ****************************************************************************/

#include "foc_agent_skill.h"

static const char g_skill_motor_tuning[] =
    "# FOC 电机整定\n"
    "整定 FOC 电流环/速度环 PI 参数。触发词：整定、调参、PI、电流环、速度环、电机调不好。\n"
    "\n"
    "<!-- 上面那行描述必须紧贴标题，中间不能有空行 —— ai_agent 的\n"
    "     extract_description() 从标题行之后开始读、遇到第一个空行就停，中间空一行\n"
    "     整段描述就变成空字符串，触发词永远进不了模型上下文。原因见\n"
    "     ../AI_TUNER_README.md 的「Skill 描述为什么紧贴标题」。 -->\n"
    "\n"
    "## When to use\n"
    "\n"
    "用户要求整定或调整电机参数时，例如：\n"
    "\n"
    "- \"帮我整定一下这台电机\"\n"
    "- \"电流环参数调一下\"\n"
    "- \"电机抖得厉害，重新整定一下\"\n"
    "\n"
    "不适用：电机不转/报错等硬件故障排查（那应该先看 `focscope` 的波形和 CAN 链路状态）。\n"
    "\n"
    "## How to use\n"
    "\n"
    "1. **确认电机参数。** 用户没说就按默认值，并在回复里说明用了默认值：\n"
    "   `Rs=0.5` `Ld=0.001` `Ke=0.01` `poles=4`\n"
    "2. **判断用途。** `0`=云台（低带宽、高稳定）；`1`=航模（高响应）；`2`=机器人关节（平衡）。\n"
    "   用户没说就用 `2`。\n"
    "3. **用 `run_shell` 跑一次整定**（参数顺序固定，`Lq` 自动等于 `Ld`）：\n"
    "\n"
    "   ```text\n"
    "   ai_tuner_test <Rs> <Ld> <Ke> <poles> <usage>\n"
    "   ```\n"
    "\n"
    "   例如默认值 + 机器人关节：\n"
    "\n"
    "   ```text\n"
    "   ai_tuner_test 0.5 0.001 0.01 4 2\n"
    "   ```\n"
    "\n"
    "4. **用 `read_file` 读结果**：`/data/agent/tune_result.json`。\n"
    "\n"
    "5. **按 `status` 字段决定怎么回复：**\n"
    "\n"
    "   - `ok` → 汇报 6 个 PI 参数（电流环 `Kp_Id`/`Ki_Id`/`Kp_Iq`/`Ki_Iq`，速度环 `Kp_Speed`/`Ki_Speed`）\n"
    "   - `whitelist_fail` → 说明 AI 给出的参数超出理论值 ±50% 区间、已被安全机制拒绝，\n"
    "     **电机保持原参数未被改动**\n"
    "   - `parse_fail` → AI 返回内容无法解析，建议重试\n"
    "   - `http_fail` / `http_code_fail` → AI 调用失败，检查网络连接\n"
    "   - `no_key` → 未配置 MiMo API key，提示用户在 `foc_ai_tuner.c` 填入后重新编译\n"
    "\n"
    "6. **回复必须包含**：6 个参数值、用的用途模式、以及是否通过白名单校验。\n"
    "\n"
    "## Example\n"
    "\n"
    "```text\n"
    "User: \"帮我整定这台电机，是个云台用的\"\n"
    "→ run_shell: ai_tuner_test 0.5 0.001 0.01 4 0\n"
    "→ read_file: /data/agent/tune_result.json\n"
    "→ \"整定完成（云台模式）。电流环 Kp_Id=0.006702 Ki_Id=3351.03，\n"
    "   速度环 Kp_Speed=0.000670 Ki_Speed=335.10，已通过白名单校验。\"\n"
    "```\n"
    "\n"
    "```text\n"
    "User: \"重新整定一下\"\n"
    "→ run_shell: ai_tuner_test 0.5 0.001 0.01 4 2\n"
    "→ status=whitelist_fail\n"
    "→ \"AI 给出的 Kp_Id 超出理论值 ±50% 区间，已被白名单拒绝，电机参数保持原样。\n"
    "   可以再试一次。\"\n"
    "```\n"
    "\n"
    "## 安全说明\n"
    "\n"
    "白名单校验是本能力**最重要的设计**：AI 只输出建议值，任何超出理论值 ±50% 区间的结果都会被拒绝，\n"
    "电机保持原参数。这是为了不让模型的错误直接变成电机的危险参数。\n"
    "\n"
    "因此：**被拒绝的参数绝不能描述成\"已生效\"**。宁可告诉用户失败了，也不要让它看起来成功。\n"
    "\n";


const foc_agent_skill_t g_foc_agent_skills[] =
{
    { "motor-tuning.md", g_skill_motor_tuning },
};

const int g_foc_agent_skills_count =
    sizeof(g_foc_agent_skills) / sizeof(g_foc_agent_skills[0]);
