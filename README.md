# FOCPilot — AI 整定的 FOC 电机调试示波器

> 队伍编号 `327` ｜ 队名 `jiny` ｜ 赛道：**AI 硬件产品创新**

---

## 一、作品简介

**FOCPilot 是一台跑在 openvela 上的「FOC 电机调试示波器 + AI 自整定器」。**

调 FOC（磁场定向控制）电机的工程师都有同一个痛点：**PI 参数整定靠试凑**。改一次参数、烧一次板、看一次波形，一轮十几分钟，一个电流环能磨掉一整天。而那些能自动整定的商用工具，动辄绑定特定厂商的驱动板与上位机，一套几万块。

FOCPilot 把这件事拆成两块，用一块 100ask DShanPi R528 开发板全包了：

| | 做什么 | 怎么做的 |
|---|---|---|
| **看得见** | 实时显示电机的 id / iq / 转速波形 | R528 通过 CAN 总线以 100 Hz 接收驱动板遥测，LVGL 绘制滚动示波器 |
| **算得出** | 自动算出电流环 / 速度环的 6 个 PI 参数 | 把电机参数（Rs / Ld / Lq / Ke / 极对数）交给大模型，由 MiMo 推理出参数，再经**白名单校验**后从 CAN 下发回驱动板 |

**核心亮点：AI 不直接控制电机，只输出「建议值」，且必须过数学白名单。**

这是本项目最重要的一条设计决策。大模型会算错——这是事实，不值得赌。所以整定链路里加了一道闸门：AI 给出的 6 个 PI 参数，必须落在**经典控制理论解算出的理论值 ±50% 区间内**才允许下发：

```
ωc       = 2π × f_sw / 15          电流环带宽
Kp_理论  = L × ωc                   Ki_理论 = R × ωc
速度环   = 电流环带宽 / 10
```

超出区间的结果被直接拒绝，返回 `FOC_TUNE_WHITELIST_FAIL`，电机保持原参数。**AI 负责在合理范围内给出好参数，白名单负责保证它永远不会给出致命参数**——这才是 AI 落到硬件上该有的形态：让模型做它擅长的（在约束内搜索、权衡用途偏好），把安全边界交给确定性代码。

另一个亮点是**全链路闭环，不是玩具 demo**：从 CAN 采集 → 屏幕波形 → 用户点击「AI 调试」→ 参数上传 → 大模型推理 → 白名单校验 → 结果下发 → 驱动板生效，这条链路在真机上跑通了，连的是真实的 STM32G4 驱动板 + 真实电机。

---

## 二、选题方向

**AI 硬件产品创新**。

理由：本项目满足赛道对「AI 落地」的硬要求——**至少落地图形 / AI / 多媒体之一**，这里图形（LVGL 示波器）与 AI（大模型整定）两条都占了，且二者构成一个完整的产品闭环，而非各自独立的两个演示。

同时它解决了真实场景里的真实痛点（电机调试费时、商用整定工具昂贵且封闭），具备商品化潜力：这套代码可以低成本移植到任何带 CAN 的 openvela 板子上，作为电机厂商的配套调试工具。

---

## 三、目录结构

```text
contest2026_327_jiny/
├── app/focscope/            # 作品全部代码（唯一需要开发的地方）
│   ├── focscope.c           #   主程序入口：数据源选择 + LVGL 初始化
│   ├── scope_ui.c/.h        #   示波器界面（LVGL，2000 行）
│   ├── scope_data_can.c     #   CAN 采集线程：解析 0x501/0x502 遥测帧
│   ├── scope_data_sim.c     #   仿真数据源（无硬件时也能跑起来看 UI）
│   ├── scope_data.h         #   数据源抽象层接口
│   ├── wifi_ui.c/.h         #   WiFi 连接界面（板载 RTL8733BS）
│   ├── foc_ai_tuner.c/.h    #   ★ AI 整定核心：MiMo API 调用 + 白名单校验
│   ├── ai_tuner_test.c      #   AI 整定命令行测试程序
│   ├── ai_tuner_can.c       #   ★ AI 整定 CAN 服务：监听请求 → 算 → 回发
│   ├── ai_tuner_ui.c/.h     #   AI 整定界面
│   ├── focpilot_can_proto.h #   ★ 双端共用 CAN 协议头（R528 ↔ STM32 完全一致）
│   ├── FOCPILOT_CAN_PROTOCOL.md  # 遥测协议规格书（0x501/0x502，含 STM32 参考实现）
│   ├── AI_TUNER_README.md        # AI 整定模块说明
│   ├── font_puhui_20_4.c    #   中文字库（普黑 20px，LVGL 格式）
│   ├── Kconfig / Make.defs / Makefile
├── board/r528s3_config/
│   └── defconfig            # 板级配置：开好 CAN / LVGL / WiFi / libcurl / 本应用
├── logs/wx112233030524/     # AI Coding 日志（官方采集器导出，含 manifest.json）
├── contest2026_327_jiny.xml # repo manifest：把 app/focscope 软链进编译树
└── README.md                # 本文件
```

### 关于 manifest

本仓通过 `contest2026_327_jiny.xml` 里的 `<linkfile>` 把 `app/focscope/` 软链到 openvela 编译树的 `apps/examples/focscope`：

```xml
<project path="contest2026_327_jiny" name="contest2026_327_jiny">
  <linkfile src="app/focscope" dest="apps/examples/focscope"/>
</project>
```

**生产仓库零改动。** `apps/examples/Make.defs` 本身是 `include $(wildcard $(APPDIR)/examples/*/Make.defs)`，`apps/examples/Kconfig` 由 `apps/tools/mkkconfig.sh` 自动生成——所以新目录放进去就被自动发现，不需要动 `apps/` 里任何一行代码。

`<linkfile>` 只在下一次 `repo sync` 时生效。如果你改过 manifest，记得重跑一次 `repo sync`，否则 `apps/examples/focscope` 不会出现。

板级配置 `board/r528s3_config/defconfig` **不做软链**，见下面的编译说明。

---

## 四、运行方式

### 1. 拉取工程

```bash
repo init -u https://github.com/open-vela/contest2026_327_jiny \
  -b dev-ai-contest-2026 -m contest2026_327_jiny.xml
repo sync -c -j8
```

### 2. 编译

在 openvela 工作区**根目录**（即本仓的上一级）：

```bash
cp contest2026_327_jiny/board/r528s3_config/defconfig nuttx/.config
make -C nuttx olddefconfig
make -C nuttx -j8
```

产物：`nuttx/vela_nsh.bin`（以及 `vendor/allwinnertech/lichee/board/r528s3/velaevb1_nand/configs/nsh.fex`）。

> **为什么不是 `./build.sh <路径>`？**
> `nuttx/tools/configure.sh` 要求 board config 目录旁边能找到一份 `Make.defs`
> （`<configdir>/Make.defs`、`<configdir>/../../scripts/Make.defs` 或
> `<configdir>/../../../common/scripts/Make.defs` 三者之一）。本仓的
> `board/r528s3_config/` 是仓外目录，三者都没有，于是 configure.sh 直接以
> `File Make.defs could not be found` 退出——`build.sh` 的后半段根本不会执行。
> 上面三条命令就是 `configure.sh` 实际做的配置动作（把 defconfig 放进
> `nuttx/.config` 再 olddefconfig）；`Make.defs` 与链接脚本由上一次
> `lunch_nuttx` 配置留在原处，仍然有效。
>
> 想走厂商那条路也可以：把 `board/r528s3_config/defconfig` 覆盖到
> `vendor/allwinnertech/boards/r528/r528s3-velaevb1/configs/nsh/defconfig`
> （该目录有 `../../scripts/Make.defs`，configure.sh 能用），然后
> ```bash
> source vendor/allwinnertech/lichee/envsetup.sh
> lunch_nuttx r528s3-velaevb1
> make -C nuttx -j8
> ```
> 这条路径会改动 vendor 仓里的一个文件，介意的话用上面那三条命令。

> 本仓的 `defconfig` 已开好全部所需选项，**无需再跑 menuconfig**：
> `CONFIG_EXAMPLES_FOCSCOPE=y`、`CONFIG_EXAMPLES_FOCSCOPE_AI_TEST=y`、
> `CONFIG_EXAMPLES_FOCSCOPE_AI_TUNER_CAN=y`、`CONFIG_R528_CAN=y`、
> `CONFIG_GRAPHICS_LVGL=y`、`CONFIG_LIB_CURL=y`、`CONFIG_IEEE80211_REALTEK_WIFI_RTL8733BS=y`，
> 以及运行时 Skill 需要的 `CONFIG_EXAMPLES_AI_AGENT_VELA=y`、
> `CONFIG_EXAMPLES_AI_AGENT_VELA_SHELL_FULL=y`、`CONFIG_SYSTEM_POPEN=y`。

### 3. 烧录与运行

烧录到 100ask DShanPi R528 后，NSH 中：

```bash
# ① 无硬件也能看 UI —— 仿真数据源
nsh> focscope

# ② 接上驱动板 —— CAN 实时遥测（/dev/can0，500 kbps）
nsh> focscope can 0

# ③ AI 整定（命令行测试，直接传电机参数）
nsh> ai_tuner_test 0.5 0.001 0.01 4 2
#   参数依次是：Rs(Ω) Ld(H) Ke(V·s/rad) 极对数 用途(0云台/1航模/2机器人关节)

# ④ AI 整定 CAN 服务（接收驱动板请求 → 计算 → 回发）
nsh> ai_tuner_can 0
```

### 4. 让 AI 整定真正跑起来：填 API Key

`foc_ai_tuner.c` 中预留了占位符（**本仓库公开，故不提交真实密钥**）：

```c
#define MIMO_API_KEY    "YOUR_MIMO_API_KEY"
```

把它换成你自己的 MiMo API key（[控制台](https://platform.xiaomimimo.com/console)）后重新编译即可。
整定链路其余部分（CAN 收发、白名单、下发）不依赖网络，只有这一个字符串需要填。

### 5. 驱动板侧

驱动板（STM32G4）需要按 `app/focscope/FOCPILOT_CAN_PROTOCOL.md` 广播遥测帧，并按
`app/focscope/focpilot_can_proto.h` 响应整定请求。两份文档都给了**拷贝即用的 HAL 参考实现**
（FDCAN 配置、发送函数、10 ms 周期触发位置）。

---

## 五、AI Coding 使用说明

本作品从零到真机跑通，全程使用 **Claude Code** 辅助开发，完整对话日志见 `logs/wx112233030524/`。

### AI 参与到了哪些环节

| 环节 | AI 做了什么 |
|---|---|
| **需求拆解与选型** | 讨论「示波器 + 整定」这个组合的产品形态，确定 AI 只出建议值、白名单兜底的安全架构 |
| **方案设计** | 设计 CAN 遥测帧格式（0x501/0x502）、整定协议（0x101–0x104 / 0x201–0x204），双端共用同一头文件保证一致性 |
| **编码** | 2000 行的 LVGL 示波器界面、CAN 采集线程、MiMo API 客户端、白名单校验器 |
| **调试验证** | 大量真机日志分析。例如 CAN 通信一直不通，AI 逐层排查（时钟树 → 时序参数 → 收发器），最终用示波器实测确认 **R528 CAN 模块时钟是 24 MHz 而非预期的 40 MHz**，据此重算 500 kbps 时序（brp=2 / tseg1=15 / tseg2=8），问题解决 |
| **死锁排查** | 定位并修复 100ask 版本 LVGL 的 `lvgl_lock()` 死锁（多次加锁导致自锁），修复后 UI 稳定运行 |
| **文档** | 协议规格书、模块 README、代码注释 |

### AI 带来的实际帮助

最直接的是**排障效率**：CAN 不通这类问题，人工排查通常要在「硬件 / 驱动 / 时序 / 应用」之间反复试错，一轮就是一次编译烧录。AI 在这里的价值是能同时读懂驱动源码、示波器波形记录和串口日志，快速排除掉不可能的层级，把问题收敛到「模块时钟频率」这一个点上。

其次是**代码量**：LVGL 界面样板代码密度极高且高度重复，这部分交给 AI 后，人的精力集中在真正的设计决策上（安全架构、协议设计、整定策略）。

> 需要说明的是：AI 生成的代码并非照单全收。协议字段、时钟频率、时序参数这类**必须与硬件一致**的地方，全部经过真机验证与示波器实测；CAN 时序就是 AI 最初按错误时钟算出来、再由实测纠正的。日志里完整保留了这些往返过程。

### 关于日志目录

`logs/` 由官方 AI Coding 采集器（`claude-code-collector`）导出，含 `manifest.json`（schema 1.0）。
其中 API Key、WiFi 密码等敏感信息已全部替换为占位符。

---

## 六、当前状态与已知限制

**已在真机验证：**

- ✅ openvela 编译通过，生成可烧录镜像
- ✅ LVGL 示波器 UI 在 R528 上稳定运行（触屏交互、滚动波形）
- ✅ CAN 通信打通，500 kbps 下稳定接收遥测帧（时序经示波器实测校准）
- ✅ WiFi 连接（板载 RTL8733BS）
- ✅ MiMo API 调用成功返回 PI 参数，白名单校验生效

> 上面最后一条是**填入真实 key 后**在真机上验证的。本仓公开，源码里只保留占位符
> `YOUR_MIMO_API_KEY`，所以直接编译出来的固件跑到这一步会返回 `no_key` 状态并给出
> 提示，而不是静默失败——这是有意设计的默认行为，不是故障。

**已知限制：**

- 驱动板侧代码（STM32G4 工程）不在本仓，本仓只提供协议规格与参考实现；完整复现需自备驱动板
- AI 整定需要联网，无网络时命令行会明确报错退出，不影响示波器功能
- 当前白名单是固定 ±50% 区间，尚未按电机用途（云台/航模/机器人）做差异化收紧——这是下一步计划

---

## 附：AI Coding 日志格式

```
logs/wx112233030524/
├── manifest.json                                 # schema 1.0, team_id, sessions[]
└── 2026-09-12/
    └── claude-code__<session-id>.jsonl           # 逐事件记录（ts/role/text/tool）
```

---

*本作品采用 Apache License 2.0 许可。*
