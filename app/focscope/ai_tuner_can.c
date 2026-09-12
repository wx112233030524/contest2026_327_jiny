/****************************************************************************
 * ai_tuner_can.c
 *
 * FOCPilot AI Tuner - CAN 整定程序
 *
 * 监听 /dev/can0 上的电机参数请求帧 (0x101~0x104), 调用 MiMo API
 * 计算 PI 参数, 通过 0x201~0x204 回发结果。
 *
 * 协议见 focpilot_can_proto.h (双端共用)。
 *
 * 用法: ai_tuner_can [0|1]   (默认 0 = /dev/can0)
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <nuttx/can/can.h>
#include <curl/curl.h>

#include "foc_ai_tuner.h"
#include "focpilot_can_proto.h"
#include "foc_agent_skill.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AI_TUNER_MAX_DLC   8

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 发送一个标准帧 */
static int can_send_frame(int fd, uint16_t id, FAR const uint8_t *data, int dlc)
{
    struct can_msg_s msg;

    memset(&msg, 0, sizeof(msg));
    msg.cm_hdr.ch_id  = id;
    msg.cm_hdr.ch_dlc = dlc;
    if (data)
    {
        memcpy(msg.cm_data, data, dlc);
    }

    if (write(fd, &msg, CAN_MSGLEN(dlc)) < 0)
    {
        fprintf(stderr, "ai_tuner_can: write 0x%x failed: %d\n", id, errno);
        return -1;
    }

    return 0;
}

/* 发送整定状态帧 0x201 */
static int send_status(int fd, uint8_t status)
{
    uint8_t data[AI_TUNER_MAX_DLC];

    memset(data, 0, sizeof(data));
    data[FOC_STATUS_OFF_STATUS] = status;
    return can_send_frame(fd, FOC_CAN_ID_TUNE_STATUS, data, 1);
}

/* 发送 PI 参数帧 0x202~0x204 */
static void send_pi_params(int fd, FAR pi_params_t *pi)
{
    uint8_t data[AI_TUNER_MAX_DLC];

    foc_pack_f32(data, pi->Kp_Id, pi->Ki_Id);
    can_send_frame(fd, FOC_CAN_ID_PI_ID, data, 8);

    foc_pack_f32(data, pi->Kp_Iq, pi->Ki_Iq);
    can_send_frame(fd, FOC_CAN_ID_PI_IQ, data, 8);

    foc_pack_f32(data, pi->Kp_Speed, pi->Ki_Speed);
    can_send_frame(fd, FOC_CAN_ID_PI_SPEED, data, 8);
}

/* 单次整定流程: 等 0x101 请求 + 0x102/0x103/0x104 参数, 调 AI, 回发结果 */
static void tune_once(int fd)
{
    struct can_msg_s msg;
    motor_params_t motor;
    pi_params_t pi;
    int have_req = 0;
    int have_rs_ld = 0;
    int have_lq_ke = 0;
    int have_poles = 0;
    int usage = FOC_USAGE_ROBOT;
    int ret;

    memset(&motor, 0, sizeof(motor));
    memset(&pi, 0, sizeof(pi));

    /* 等待请求 + 三帧参数 (最多 50 帧超时) */
    while (!(have_req && have_rs_ld && have_lq_ke && have_poles))
    {
        ssize_t nbytes;

        memset(&msg, 0, sizeof(msg));
        nbytes = read(fd, &msg, CAN_MSGLEN(AI_TUNER_MAX_DLC));
        if (nbytes < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            fprintf(stderr, "ai_tuner_can: read failed: %d\n", errno);
            return;
        }

        switch (msg.cm_hdr.ch_id)
        {
            case FOC_CAN_ID_PARAM_REQ:
                usage = msg.cm_data[FOC_PARAMREQ_OFF_USAGE];
                have_req = 1;
                printf("[CAN] 收到整定请求, usage=%d\n", usage);
                break;

            case FOC_CAN_ID_PARAM_RS_LD:
                motor.Rs = foc_unpack_f32(&msg.cm_data[0]);
                motor.Ld = foc_unpack_f32(&msg.cm_data[4]);
                have_rs_ld = 1;
                break;

            case FOC_CAN_ID_PARAM_LQ_KE:
                motor.Lq = foc_unpack_f32(&msg.cm_data[0]);
                motor.Ke = foc_unpack_f32(&msg.cm_data[4]);
                have_lq_ke = 1;
                break;

            case FOC_CAN_ID_PARAM_POLES:
                motor.poles = msg.cm_data[FOC_PARAMPOLES_OFF_POLES];
                have_poles = 1;
                break;

            default:
                /* 遥测帧 0x501/0x502 等忽略 */
                break;
        }

        /* 打印已收集的参数 */
        if (have_rs_ld && have_lq_ke && have_poles)
        {
            printf("[CAN] Rs=%.6f Ld=%.6f Lq=%.6f Ke=%.6f poles=%d\n",
                   motor.Rs, motor.Ld, motor.Lq, motor.Ke, motor.poles);
        }
    }

    /* 通知开始计算 */
    send_status(fd, FOC_TUNE_STARTED);

    /* 调用 AI (内含白名单校验) */
    ret = foc_ai_tune(&motor, usage, &pi);

    if (ret == FOC_AI_OK)
    {
        printf("[AI] 整定成功: Kp_Id=%.6f Ki_Id=%.6f Kp_Speed=%.6f\n",
               pi.Kp_Id, pi.Ki_Id, pi.Kp_Speed);

        send_status(fd, FOC_TUNE_OK);
        send_pi_params(fd, &pi);
    }
    else if (ret == FOC_AI_ERR_WHITELIST)
    {
        printf("[AI] 白名单校验失败\n");
        send_status(fd, FOC_TUNE_WHITELIST_FAIL);
    }
    else
    {
        printf("[AI] 计算失败: %d\n", ret);
        send_status(fd, FOC_TUNE_AI_FAIL);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
    int port = 0;
    char path[16];
    int fd;

    if (argc > 1)
    {
        port = atoi(argv[1]);
        if (port != 0 && port != 1)
        {
            fprintf(stderr, "usage: ai_tuner_can [0|1]\n");
            return 1;
        }
    }

    snprintf(path, sizeof(path), "/dev/can%d", port);
    printf("FOCPilot AI Tuner CAN 整定\n");
    printf("监听 %s (500kbps, 标准帧)\n", path);

    fd = open(path, O_RDWR);
    if (fd < 0)
    {
        fprintf(stderr, "ai_tuner_can: open %s failed: %d\n", path, errno);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* focscope 开机时已经装过一遍, 这里是兜底: 万一 agent 直接拉起本程序而
     * focscope 没跑过, Skill 也得在位。内容一致时不做任何写入。 */

    foc_agent_skill_install();

    /* 循环处理整定请求 */
    while (1)
    {
        tune_once(fd);
    }

    curl_global_cleanup();
    close(fd);

    return 0;
}
