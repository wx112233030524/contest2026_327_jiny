# FOCPilot AI Tuner 测试程序

## 概述

这是 FOCPilot AI 整定闭环的测试程序，用于验证 MiMo API 调用和 PI 参数计算。

## MiMo API 配置

在 `foc_ai_tuner.c` 中找到：

```c
#define MIMO_API_KEY    "YOUR_MIMO_API_KEY"
```

把它替换成你自己的 MiMo API key（在 https://platform.xiaomimimo.com/console 获取）。

> **注意**：本仓库是公开的，**不要把真实 key 提交上来**——公网爬虫会在几秒内抓走。
> 这里保留占位符，评测时填入自己的 key 即可编译通过。

可选：修改 `foc_ai_tuner.c` 中的 `MIMO_MODEL` 选择模型（默认 `mimo-v2.5`）。

可用模型：
- `mimo-v2.5` - 标准版（¥1/MTok 输入，¥2/MTok 输出）
- `mimo-v2.5-pro` - 旗舰版（¥3/MTok 输入，¥6/MTok 输出）

## 编译配置

在 menuconfig 中启用：

```
Application Configuration → Examples → FOC Scope (LVGL oscilloscope)
  [*] FOC Scope (LVGL oscilloscope)
  [*] AI Tuner test program
```

注意：需要先启用 libcurl：

```
Networking Support → Network Utilities
  [*] libcurl
```

## 使用方法

### 基本用法

```bash
# 使用默认电机参数
nsh> ai_tuner_test

# 指定电机参数
nsh> ai_tuner_test 0.5 0.001 0.01 4

# 参数说明：
#   Rs (Ω)   - 定子电阻，默认 0.5
#   Ld (H)   - d轴电感，默认 0.001
#   Ke (V·s/rad) - 反电动势常数，默认 0.01
#   poles    - 极对数，默认 4
```

### 电机用途

```bash
# 云台（低带宽，高稳定性）
nsh> ai_tuner_test 0.5 0.001 0.01 4 0

# 航模（高响应）
nsh> ai_tuner_test 0.5 0.001 0.01 4 1

# 机器人关节（平衡，默认）
nsh> ai_tuner_test 0.5 0.001 0.01 4 2
```

## 输出示例

```
========================================
FOCPilot AI Tuner 测试程序
========================================
电机参数:
  Rs = 0.500000 Ω
  Ld = 0.001000 H
  Lq = 0.001000 H
  Ke = 0.010000 V·s/rad
  极对数 = 4
用途: 2
========================================

========================================
[AI] 发送请求到 MiMo API...
[AI] 电机参数: Rs=0.500000, Ld=0.001000, Lq=0.001000, Ke=0.010000, poles=4
[AI] 用途: 2, 迭代: 0
========================================
[AI] HTTP 状态码: 200
[AI] 收到响应 (1234 字节)
[AI] 响应内容:
{...}

========================================
[AI] 计算结果:
  Kp_Id    = 0.006702
  Ki_Id    = 3351.032227
  Kp_Iq    = 0.006702
  Ki_Iq    = 3351.032227
  Kp_Speed = 0.000670
  Ki_Speed = 335.103210
========================================
[白名单] 校验通过 ✓
```

## 白名单校验规则

**电流环的 4 个参数**（`Kp_Id` / `Ki_Id` / `Kp_Iq` / `Ki_Iq`）必须在理论值的
50% ~ 150% 范围内，任一项越限则整条结果作废：

- 理论 Kp = L × ωc，其中 ωc = 2π × 16000 / 15
- 理论 Ki = R × ωc

**速度环的 2 个参数（`Kp_Speed` / `Ki_Speed`）不在校验范围内。** 它们靠提示词里的
降阶关系引导（`foc_ai_tuner.c` 的 prompt：速度环带宽 = 电流环带宽 / 10），
属于"事前引导"而非"事后否决"——模型不照做时没有代码拒绝。这是已知限制，
补齐方式是把速度环改为由已通过校验的电流环参数直接推算。

## 注意事项

1. 需要有效的 MiMo API Key（配置方式见上文「MiMo API 配置」）
2. 需要网络连接（WiFi 或以太网）
3. 首次运行可能需要等待 SSL 证书验证

## 后续集成

测试通过后，将 AI 整定引擎集成到 focscope 主程序中：
1. 添加 "AI 调试" 按钮到 UI
2. 实现 Profiler 触发和结果接收
3. 实现 PI 参数下发到驱动板

## 运行时 Skill（板载 ai_agent）

`agent_skill/motor-tuning.md` 是给板载 `ai_agent` 用的运行时 Skill：装到
`/data/agent/skills/` 之后，用户可以用自然语言（"帮我整定这台电机"）驱动整定，
agent 自己会去调 `ai_tuner_test` 并读回结果。

### 装配链路

```text
agent_skill/motor-tuning.md     ← 人写的源文件
        │  python3 agent_skill/gen_c_string.py
        ▼
foc_agent_skill_data.c          ← 自动生成, 编进固件
        │  focscope 启动时调用 foc_agent_skill_install()
        ▼
/data/agent/skills/motor-tuning.md
        │  ai_agent 启动时扫描该目录并读标题
        ▼
系统提示词的 Skills 摘要条目 → 模型据此决定要不要读全文
```

`ai_agent` 自带 10 个内置 Skill，但它的 `skill_loader.c` 只在启动时安装自己
硬编码的那 10 个字符串，**不会**扫描仓库——所以外部 Skill 必须由我们自己的
应用在运行时写进去。

### 为什么 Skill 描述紧贴标题

`skill_loader.c` 的 `extract_description()` 从标题行之后开始读，遇到第一个空行
就停。而它自己内置的 Skill、以及一般人写 Markdown 的习惯，都是标题下面空一行，
于是描述恒为空字符串——那个"跳过前导空行"的分支写在 break 判断之后，永远执行
不到（见 `skill_loader.c` 中 `if (off == 0 && line[0] == '\n') continue;`，
被上一行的 break 挡住了）。

所以这里的写法是**标题行下面直接接描述行，中间不空行**，并在描述之后用 HTML
注释留了一份说明。这是绕过上游缺陷的写法；上游修好之后可以改回常规排版。

### 摘要预算只有 1023 字节

`context_builder.c` 用 1024 字节的栈缓冲装全部 Skill 的摘要条目
（`skill_loader_build_summary`），装不下就**静默丢弃**后面的条目。10 个内置
Skill 占 784 字节，本 Skill 的条目 203 字节，合计 987/1023——能装下，但余量
只有 36 字节，再加一个 Skill 就会被挤掉。

### 触发链路依赖 FULL shell 模式

Skill 让 agent 用 `run_shell` 跑 `ai_tuner_test`，而 `ai_tuner_test` 不在
`run_shell` 默认 ALLOWLIST 的 58 个命令里。板级 defconfig 因此开了
`CONFIG_EXAMPLES_AI_AGENT_VELA_SHELL_FULL=y`，并且必须同时开
`CONFIG_SYSTEM_POPEN=y`——FULL 模式走 `popen()`，没开这个选项时
`run_shell` 会编译成 "popen not available"，Skill 看起来装好了但每条命令都失败。
