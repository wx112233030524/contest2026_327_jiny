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

PI 参数必须在理论值的 50% ~ 150% 范围内：

- 理论 Kp = L × ωc，其中 ωc = 2π × 16000 / 15
- 理论 Ki = R × ωc

## 注意事项

1. 需要有效的 MiMo API Key（配置方式见上文「MiMo API 配置」）
2. 需要网络连接（WiFi 或以太网）
3. 首次运行可能需要等待 SSL 证书验证

## 后续集成

测试通过后，将 AI 整定引擎集成到 focscope 主程序中：
1. 添加 "AI 调试" 按钮到 UI
2. 实现 Profiler 触发和结果接收
3. 实现 PI 参数下发到驱动板
