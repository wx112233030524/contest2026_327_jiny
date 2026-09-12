/****************************************************************************
 * apps/examples/focscope/ai_tuner_test.c
 *
 * FOCPilot AI Tuner - 命令行测试程序
 *
 * 验证 MiMo API 调用和 PI 参数计算
 * 用法: ai_tuner_test [Rs] [Ld] [Ke] [poles] [usage]
 * 示例: ai_tuner_test 0.5 0.001 0.01 4
 *
 * AI 逻辑在 foc_ai_tuner.c 共享模块。
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "foc_ai_tuner.h"

int main(int argc, FAR char *argv[])
{
    motor_params_t motor;
    pi_params_t pi;
    int usage = FOC_USAGE_ROBOT;
    int ret;

    motor.Rs = 0.5f;
    motor.Ld = 0.001f;
    motor.Lq = 0.001f;
    motor.Ke = 0.01f;
    motor.poles = 4;

    if (argc > 1) motor.Rs = strtof(argv[1], NULL);
    if (argc > 2) motor.Ld = strtof(argv[2], NULL);
    if (argc > 3) motor.Ke = strtof(argv[3], NULL);
    if (argc > 4) motor.poles = atoi(argv[4]);
    if (argc > 5) usage = atoi(argv[5]);

    motor.Lq = motor.Ld;

    printf("========================================\n");
    printf("FOCPilot AI Tuner 测试程序\n");
    printf("========================================\n");
    printf("电机参数:\n");
    printf("  Rs = %.6f Ω\n", motor.Rs);
    printf("  Ld = %.6f H\n", motor.Ld);
    printf("  Lq = %.6f H\n", motor.Lq);
    printf("  Ke = %.6f V·s/rad\n", motor.Ke);
    printf("  极对数 = %d\n", motor.poles);
    printf("用途: %d\n", usage);
    printf("========================================\n");

    curl_global_init(CURL_GLOBAL_DEFAULT);

    memset(&pi, 0, sizeof(pi));
    ret = foc_ai_tune(&motor, usage, &pi);

    if (ret == FOC_AI_OK)
    {
        printf("\n========================================\n");
        printf("[AI] 计算结果:\n");
        printf("  Kp_Id    = %.6f\n", pi.Kp_Id);
        printf("  Ki_Id    = %.6f\n", pi.Ki_Id);
        printf("  Kp_Iq    = %.6f\n", pi.Kp_Iq);
        printf("  Ki_Iq    = %.6f\n", pi.Ki_Iq);
        printf("  Kp_Speed = %.6f\n", pi.Kp_Speed);
        printf("  Ki_Speed = %.6f\n", pi.Ki_Speed);
        printf("========================================\n");
        printf("[白名单] 校验通过\n");
    }
    else
    {
        printf("\n[ERROR] AI 计算失败: %d\n", ret);
        printf("[白名单] 校验失败\n");
    }

    curl_global_cleanup();

    return ret == FOC_AI_OK ? 0 : 1;
}
