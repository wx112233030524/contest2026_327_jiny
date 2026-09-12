/****************************************************************************
 * foc_ai_tuner.h
 *
 * FOCPilot AI Tuner 共享模块
 *
 * 把 MiMo API 调用 + PI 参数计算 + 白名单校验抽成公共接口,
 * 供 ai_tuner_test (命令行测试) 和 ai_tuner_can (CAN 整定) 复用。
 *
 ****************************************************************************/

#ifndef __FOC_AI_TUNER_H
#define __FOC_AI_TUNER_H

#include <nuttx/config.h>
#include <nuttx/compiler.h>   /* FAR */

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef struct
{
    float Rs;       /* 定子电阻 (Ω) */
    float Ld;       /* d轴电感 (H) */
    float Lq;       /* q轴电感 (H) */
    float Ke;       /* 反电动势常数 (V·s/rad) */
    int poles;      /* 极对数 */
} motor_params_t;

typedef struct
{
    float Kp_Id;    /* d轴电流环 Kp */
    float Ki_Id;    /* d轴电流环 Ki */
    float Kp_Iq;    /* q轴电流环 Kp */
    float Ki_Iq;    /* q轴电流环 Ki */
    float Kp_Speed; /* 速度环 Kp */
    float Ki_Speed; /* 速度环 Ki */
} pi_params_t;

/* 电机用途 */
#define FOC_USAGE_GIMBAL    0   /* 云台 */
#define FOC_USAGE_AERO      1   /* 航模 */
#define FOC_USAGE_ROBOT     2   /* 机器人关节 */

/* 返回值 */
#define FOC_AI_OK               0   /* 成功, PI 有效 */
#define FOC_AI_ERR_HTTP        -1   /* HTTP 请求失败 */
#define FOC_AI_ERR_HTTP_CODE   -2   /* HTTP 非 200 */
#define FOC_AI_ERR_PARSE       -3   /* AI 响应解析失败 (content 为空) */
#define FOC_AI_ERR_WHITELIST   -4   /* PI 越限, 白名单校验失败 */
#define FOC_AI_ERR_NO_KEY      -5   /* 源码里的 MIMO_API_KEY 还是占位符 */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* 调用 MiMo API 计算 PI 参数并做白名单校验.
 * 成功返回 FOC_AI_OK, pi 填充有效值; 否则返回负错误码. */
int foc_ai_tune(FAR motor_params_t *motor, int usage,
                FAR pi_params_t *pi);

/* 白名单校验 (AI 结果 vs 理论值 50%~150%).
 * 通过返回 0, 越限返回 -1. */
int foc_ai_validate(FAR pi_params_t *pi, FAR motor_params_t *motor);

#endif /* __FOC_AI_TUNER_H */
