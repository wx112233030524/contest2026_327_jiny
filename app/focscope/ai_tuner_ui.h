/****************************************************************************
 * ai_tuner_ui.h
 *
 * FOCPilot AI Tuner 后台模块 (供 UI 集成)
 *
 * 复用 focscope 的 CAN 通道 (scope_data_can.c):
 *   - scope_data_can reader 收到 STM32 电机参数帧 (0x101~0x104) 后,
 *     调用 ai_tuner_ui_submit() 提交参数
 *   - 或 UI 手动 ai_tuner_ui_trigger()
 *   - 后台线程调用 MiMo API 计算, 完成后经注册的 sender 回发
 *     0x201(状态) + 0x202~0x204 (PI 参数)
 *
 * UI (scope_ui) 通过 ai_tuner_ui_state()/ai_tuner_ui_get_result() 轮询,
 * 不阻塞 LVGL。AI 调用 (curl, 最长 300s) 只在这个线程里执行。
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_FOCSCOPE_AI_TUNER_UI_H
#define __APPS_EXAMPLES_FOCSCOPE_AI_TUNER_UI_H

#include <nuttx/config.h>
#include <stdint.h>

#include "foc_ai_tuner.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* AI 整定状态机 (UI 显示用) */

#define AI_TUNER_IDLE        0   /* 空闲, 等待触发 */
#define AI_TUNER_CALC        1   /* AI 计算中 */
#define AI_TUNER_OK          2   /* 完成, 结果有效 */
#define AI_TUNER_FAIL        3   /* 失败 (HTTP/解析/白名单) */

/* 发送回调: 回发 CAN 帧. 由 scope_data_can.c 注册.
 * 返回 0 成功, 非 0 失败. */
typedef int (*ai_tuner_send_t)(uint16_t id, FAR const uint8_t *data, int dlc);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* 启动后台整定线程 (进程内只调一次). */
void ai_tuner_ui_start(void);

/* 注册 CAN 发送回调 (scope_data_can.c 在 reader 启动时调用) */
void ai_tuner_ui_set_sender(ai_tuner_send_t send);

/* 提交 CAN 收到的电机参数, 触发一次整定 (scope_data_can reader 调用).
 * 若正在计算则忽略. */
void ai_tuner_ui_submit(FAR motor_params_t *motor, int usage);

/* 手动触发一次整定 (UI 开始按钮). 若正在计算则忽略. */
void ai_tuner_ui_trigger(FAR motor_params_t *motor, int usage);

/* 板子→STM32: 请求 STM32 上传电机参数 (发 0x301). 收到 0x101~0x104 后自动整定 */
void ai_tuner_ui_request_params(int usage);

/* 当前状态 (AI_TUNER_*) */
int ai_tuner_ui_state(void);

/* 最近一次结果. 返回 1 = 有效 (状态为 OK), 0 = 无效. */
int ai_tuner_ui_get_result(FAR pi_params_t *pi);

/* 最近一次整定用的电机参数 (供 UI 显示). */
void ai_tuner_ui_get_motor(FAR motor_params_t *motor);

/* 最近一次错误码 (FOC_AI_ERR_*, 仅 FAIL 时有效). */
int ai_tuner_ui_last_error(void);

#endif /* __APPS_EXAMPLES_FOCSCOPE_AI_TUNER_UI_H */
