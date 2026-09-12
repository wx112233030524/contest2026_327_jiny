# FOCPilot CAN 遥测协议 v1.2

R528 示波器(接收端)与 FOC 驱动板(发送端)之间的遥测协议。
v1.1(2026-08-19):新增 0x502 母线电流帧,0x501 布局不变。
v1.2(2026-08-19):0x502 数值字段由 uint16 扩为 uint32(4 字节小端)。

## 总线参数

| 参数 | 值 |
|---|---|
| 波特率 | 500 kbit/s |
| 帧类型 | 标准帧(11-bit ID) |
| 数据长度 | 8 字节(固定) |
| 字节序 | 小端(little-endian) |
| 发送方 | 驱动板(STM32G4 + 板载 CAN 收发器 TJA1044 等) |
| 周期 | 10 ms(100 Hz)广播 |

## 帧布局

**ID = 0x501**(遥测广播;0x50x 段保留给本协议扩展)

| 字节 | 类型 | 单位 | 范围 | 说明 |
|---|---|---|---|---|
| 0-1 | int16 | mA | ±32767 | d 轴电流 id |
| 2-3 | int16 | mA | ±32767 | q 轴电流 iq |
| 4-5 | int16 | rpm | ±32767 | 转子转速 |
| 6-7 | uint16 | mV | 0..65535 | 母线电压 Vbus |

定点定标:电流 mA(A × 1000)、电压 mV(V × 1000)、转速整数 rpm。

**ID = 0x502**(母线电流遥测;与 0x501 同一 10 ms 节拍发送)

| 字节 | 类型 | 单位 | 范围 | 说明 |
|---|---|---|---|---|
| 0-3 | uint32 | mA | 0..4294967295 | 母线电流 Ibus(小端) |
| 4-7 | — | — | 0 | 预留(温度/故障码候选位) |

> 母线电流无符号。无专用母线电流传感器时,用平均电功率估算:
> `Ibus = MC_GetAveragePowerMotor1_F() / Vbus`,负值(再生)按 0 发送。

## 驱动板发送端实现(STM32G4 + MCSDK 工程参考)

### 1. FDCAN 配置(CubeMX)

G4 的 CAN 外设是 FDCAN,工作于**经典 CAN 模式**即可:

- FDCAN1 激活,Mode = `FDCAN_MODE_NORMAL`(CAN Classic),不用 FD/BRS
- Nominal 时序 500 kbit/s:让 CubeMX 按外设时钟自动算(如 170 MHz 时钟下
  常见 Prescaler=17、TimeSeg1=15、TimeSeg2=4,采样点 ~80%;以 CubeMX 生成值为准)
- 不启用 FIFO 接收中断即可(遥测只发不收;若 FDCAN 要求配 RX 则接受但忽略)

### 2. 发送函数(拷贝即用)

```c
/* foc_telemetry.c -- FOCPilot 协议 v1 遥测发送,STM32 HAL */

#include <string.h>
#include "main.h"

extern FDCAN_HandleTypeDef hfdcan1;

/* float A -> int16 mA,夹取到协议范围 */
static int16_t foc_tel_clamp_i16(float v)
{
    float mA = v * 1000.0f;
    if (mA >  32767.0f) return  32767;
    if (mA < -32768.0f) return -32768;
    return (int16_t)mA;
}

/* 发一帧 0x501(小端):FIFO 满或总线忙则丢帧,绝不阻塞控制环路 */
void foc_telemetry_send(int16_t id_mA, int16_t iq_mA,
                        int16_t speed_rpm, uint16_t vbus_mV)
{
    FDCAN_TxHeaderTypeDef hdr;
    uint8_t d[8];
    union { int16_t  s; uint8_t b[2]; } s16;   /* 小端拆包 */
    union { uint16_t u; uint8_t b[2]; } u16;

    s16.s = id_mA;     d[0] = s16.b[0]; d[1] = s16.b[1];
    s16.s = iq_mA;     d[2] = s16.b[0]; d[3] = s16.b[1];
    s16.s = speed_rpm; d[4] = s16.b[0]; d[5] = s16.b[1];
    u16.u = vbus_mV;   d[6] = u16.b[0]; d[7] = u16.b[1];

    hdr.Identifier       = 0x501;
    hdr.IdType           = FDCAN_STANDARD_ID;
    hdr.TxFrameType      = FDCAN_DATA_FRAME;
    hdr.DataLength       = FDCAN_DLC_BYTES_8;
    hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hdr.BitRateSwitch    = FDCAN_BRS_OFF;
    hdr.FDFormat         = FDCAN_CLASSIC_CAN;
    hdr.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    hdr.MessageMarker    = 0;

    (void)HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &hdr, d);
}

/* 发一帧 0x502(母线电流,小端);与 0x501 同一 10 ms 节拍调用 */
void foc_telemetry_send_ibus(uint32_t ibus_mA)
{
    FDCAN_TxHeaderTypeDef hdr;
    uint8_t d[8] = { 0 };
    union { uint32_t u; uint8_t b[4]; } u32;

    u32.u = ibus_mA;
    d[0] = u32.b[0]; d[1] = u32.b[1]; d[2] = u32.b[2]; d[3] = u32.b[3];

    hdr.Identifier       = 0x502;
    hdr.IdType           = FDCAN_STANDARD_ID;
    hdr.TxFrameType      = FDCAN_DATA_FRAME;
    hdr.DataLength       = FDCAN_DLC_BYTES_8;
    hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hdr.BitRateSwitch    = FDCAN_BRS_OFF;
    hdr.FDFormat         = FDCAN_CLASSIC_CAN;
    hdr.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    hdr.MessageMarker    = 0;

    (void)HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &hdr, d);
}
```

### 3. 10 ms 周期触发(FOC 环路内计数)

不另开定时器:在 FOC 控制环路(ADC 采样中断末尾 / `MC_Scheduler`)里计数。
环路频率 20 kHz(50 µs)时,200 次 = 10 ms:

```c
static uint16_t g_tel_cnt;   /* 环路内静态计数 */

/* 放在控制环路末尾,每次执行一次 */
if (++g_tel_cnt >= (PWM_FREQ_HZ / 100))   /* 20 kHz -> 每 200 次 = 10 ms */
    {
      g_tel_cnt = 0;
      foc_telemetry_send(foc_tel_clamp_i16(id_A),
                         foc_tel_clamp_i16(iq_A),
                         (int16_t)speed_rpm,
                         (uint16_t)(vbus_V * 1000.0f));

      /* 母线电流:平均电功率 / 母线电压(无专用母线电流传感器时) */
      float ibus_A = MC_GetAveragePowerMotor1_F() / (float)vbus_V;
      if (ibus_A < 0.0f) ibus_A = 0.0f;         /* 再生按 0 */
      foc_telemetry_send_ibus((uint32_t)(ibus_A * 1000.0f));
    }
```

变量来源(MCSDK 工程,不同版本 API 名有差异,找到对应量即可):

- `id_A` / `iq_A`:电流环的实际(或参考)id/iq。MCSDK 通常有
  `MC_GetIqdrefMotor1()`(参考值)或 ADC 采样经 Clarke/Park 变换后的实际值
- `speed_rpm`:速度环输出/机械转速,如 `MC_GetLastRampFinalSpeedMotor1()`
  或 `MC_GetMecSpeedAverageMotor1()`(注意单位换算:电气 rad/s 需 ×60/2π/极对数)
- `vbus_V`:母线电压采样值,如 `MC_GetVbusAverageMotor1()`(MCSDK 单位伏特)

### 4. 注意事项(必须遵守)

- **无帧时(上电、停机)也照发:值为 0 即可**。接收端 500 ms 无帧会判定
  链路断开并停止绘图,所以即使电机停转也要保持广播。
- 发送失败(总线繁忙)直接丢弃本次,下一帧照发;不要阻塞控制环路。
- 若需扩展(温度、故障码、占空比),新增 ID 0x503/0x504,不要改动 0x501/0x502 的字节布局。

## 接收端(本仓库)

- 驱动:`vendor/allwinnertech/chips/r528/r528_can.c`(`/dev/can0`、`/dev/can1`,500 kbit/s 默认)
- 解析:`apps/examples/focscope/scope_data_can.c`(采集线程 + 最新帧槽;0x501 波形 + 0x502 母线电流)
- 启动:`nsh> focscope can`(无参数为仿真数据源)
- 联调前必须关闭内核 loopback:`CONFIG_R528_CAN_LOOPBACK`(defconfig)
