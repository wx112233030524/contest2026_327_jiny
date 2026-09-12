/****************************************************************************
 * focpilot_can_proto.h
 *
 * FOCPilot AI 整定 CAN 通信协议 v1.0
 * ---------------------------------------------------------------
 * 物理层:   CAN 2.0 标准帧 (11-bit ID), 500 kbps, 每帧 8 字节
 * 字节序:   小端 (IEEE 754 单精度 float; STM32 与 R528 同为 ARM 小端)
 * 设备:     R528 = /dev/can0, /dev/can1; STM32 侧按驱动板实际 CAN 外设
 *
 * 双端共用此头文件, 保证结构体/宏/ID 完全一致。
 ****************************************************************************/

#ifndef __FOCPILOT_CAN_PROTO_H
#define __FOCPILOT_CAN_PROTO_H

#include <stdint.h>

/* ====================== 帧 ID 分配 ====================== */
/* STM32 -> R528 (电机参数 + 请求) */
#define FOC_CAN_ID_PARAM_FETCH    0x301   /* 板子→STM32: 请求上传电机参数 (data[0]=usage) */
#define FOC_CAN_ID_PARAM_REQ      0x101   /* 请求整定 + 用途 */
#define FOC_CAN_ID_PARAM_RS_LD    0x102   /* Rs, Ld  (float x2) */
#define FOC_CAN_ID_PARAM_LQ_KE    0x103   /* Lq, Ke  (float x2) */
#define FOC_CAN_ID_PARAM_POLES    0x104   /* poles, usage (u8 x2) */

/* R528 -> STM32 (整定结果) */
#define FOC_CAN_ID_TUNE_STATUS    0x201   /* 整定状态 */
#define FOC_CAN_ID_PI_ID          0x202   /* Kp_Id, Ki_Id  (float x2) */
#define FOC_CAN_ID_PI_IQ          0x203   /* Kp_Iq, Ki_Iq  (float x2) */
#define FOC_CAN_ID_PI_SPEED       0x204   /* Kp_Speed, Ki_Speed (float x2) */

/* ====================== 状态码 ====================== */
#define FOC_TUNE_IDLE             0x00    /* 空闲, 可接收新请求 */
#define FOC_TUNE_STARTED          0x01    /* 已收到参数, 开始计算 */
#define FOC_TUNE_OK               0x02    /* 计算完成, PI 参数有效 */
#define FOC_TUNE_PARAM_INVALID    0x03    /* 电机参数不合法 */
#define FOC_TUNE_WHITELIST_FAIL   0x04    /* PI 越限, 白名单校验失败 */
#define FOC_TUNE_AI_FAIL          0x05    /* AI 调用失败 */
#define FOC_TUNE_BUSY             0x06    /* 正在计算, 拒绝新请求 */

/* ====================== 电机用途 ====================== */
#define FOC_USAGE_GIMBAL          0       /* 云台 */
#define FOC_USAGE_AERO            1       /* 航模 */
#define FOC_USAGE_ROBOT           2       /* 机器人关节 */

/* ====================== 帧数据布局 ====================== */
/* 0x101 PARAM_REQ: data[0]=cmd, data[1]=usage, data[2]=reserved */
#define FOC_REQ_CMD_TUNE          0x01    /* 请求 AI 整定 */
#define FOC_PARAMREQ_OFF_CMD      0
#define FOC_PARAMREQ_OFF_USAGE    1

/* 0x102 PARAM_RS_LD: data[0..3]=Rs(float), data[4..7]=Ld(float) */
/* 0x103 PARAM_LQ_KE: data[0..3]=Lq(float), data[4..7]=Ke(float) */
/* 0x104 PARAM_POLES: data[0]=poles(u8), data[1]=reserved */
#define FOC_PARAMPOLES_OFF_POLES  0

/* 0x201 TUNE_STATUS: data[0]=status */
#define FOC_STATUS_OFF_STATUS     0

/* 0x202 PI_ID:      data[0..3]=Kp_Id(float), data[4..7]=Ki_Id(float) */
/* 0x203 PI_IQ:      data[0..3]=Kp_Iq(float), data[4..7]=Ki_Iq(float) */
/* 0x204 PI_SPEED:   data[0..3]=Kp_Speed(float), data[4..7]=Ki_Speed(float) */
#define FOC_PI_OFF_KP             0
#define FOC_PI_OFF_KI             4

/* ====================== 辅助函数 (内联, 双端通用) ====================== */

/* 打包两个 float 到 CAN 数据 (小端) */
static inline void foc_pack_f32(uint8_t *dst, float a, float b)
{
    uint32_t ua, ub;
    union { float f; uint32_t u; } conv;

    conv.f = a; ua = conv.u;
    conv.f = b; ub = conv.u;

    dst[0] = (uint8_t)(ua & 0xff);
    dst[1] = (uint8_t)((ua >> 8) & 0xff);
    dst[2] = (uint8_t)((ua >> 16) & 0xff);
    dst[3] = (uint8_t)((ua >> 24) & 0xff);
    dst[4] = (uint8_t)(ub & 0xff);
    dst[5] = (uint8_t)((ub >> 8) & 0xff);
    dst[6] = (uint8_t)((ub >> 16) & 0xff);
    dst[7] = (uint8_t)((ub >> 24) & 0xff);
}

/* 从 CAN 数据解出 float (小端) */
static inline float foc_unpack_f32(const uint8_t *src)
{
    union { float f; uint32_t u; } conv;

    conv.u = (uint32_t)src[0] |
             ((uint32_t)src[1] << 8) |
             ((uint32_t)src[2] << 16) |
             ((uint32_t)src[3] << 24);
    return conv.f;
}

#endif /* __FOCPILOT_CAN_PROTO_H */
