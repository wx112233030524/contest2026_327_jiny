/****************************************************************************
 * ai_tuner_ui.c
 *
 * FOCPilot AI Tuner 后台模块实现
 *
 * 独立线程执行 AI 整定 (curl 调用不阻塞 LVGL):
 *   - 参数来源: scope_data_can.c 通过 ai_tuner_ui_submit() 提交 CAN 帧,
 *     或 UI 通过 ai_tuner_ui_trigger() 手动触发
 *   - 完成/失败后通过 ai_tuner_ui_set_sender() 注册的回调回发 CAN 帧
 *     (0x201 状态 + 0x202~0x204 PI 参数), 复用 focscope 的 CAN fd
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "ai_tuner_ui.h"
#include "focpilot_can_proto.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ai_tuner_s
{
  pthread_t tid;
  bool running;

  /* 共享状态 (mutex-guarded) */

  pthread_mutex_t lock;
  int state;                    /* AI_TUNER_* */
  int last_error;               /* FOC_AI_ERR_* */
  motor_params_t motor;         /* 最近一次整定参数 */
  pi_params_t pi;               /* 最近一次结果 */

  /* 待处理触发 (mutex-guarded): 一次最多挂一个 */

  bool work_pending;
  bool work_from_can;           /* 0 = 手动, 1 = CAN 提交 */
  motor_params_t work_motor;
  int work_usage;

  /* CAN 发送回调 (scope_data_can 注册) */

  ai_tuner_send_t sender;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ai_tuner_s g_tuner;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 入队一个整定任务 (若空闲). 返回 1 = 已入队 */
static int tuner_queue(FAR motor_params_t *motor, int usage, bool from_can)
{
  int queued;

  pthread_mutex_lock(&g_tuner.lock);
  if (g_tuner.state == AI_TUNER_CALC || g_tuner.work_pending)
    {
      queued = 0;   /* 忙, 忽略 */
    }
  else
    {
      g_tuner.work_motor = *motor;
      g_tuner.work_usage = usage;
      g_tuner.work_from_can = from_can;
      g_tuner.work_pending = true;
      queued = 1;
    }
  pthread_mutex_unlock(&g_tuner.lock);
  return queued;
}

/* 取走一个任务. 返回 1 = 有 */
static int tuner_take(FAR motor_params_t *motor, FAR int *usage)
{
  int taken;

  pthread_mutex_lock(&g_tuner.lock);
  taken = g_tuner.work_pending;
  if (taken)
    {
      g_tuner.work_pending = false;
      *motor = g_tuner.work_motor;
      *usage = g_tuner.work_usage;
    }
  pthread_mutex_unlock(&g_tuner.lock);
  return taken;
}

/* 回发结果帧 */
static void tuner_send_result(int status, FAR pi_params_t *pi)
{
  uint8_t data[8];
  ai_tuner_send_t send;

  pthread_mutex_lock(&g_tuner.lock);
  send = g_tuner.sender;
  pthread_mutex_unlock(&g_tuner.lock);

  if (send == NULL)
    {
      return;   /* 无 CAN 发送通道 (仿真数据源等) */
    }

  data[0] = status;
  send(FOC_CAN_ID_TUNE_STATUS, data, 1);

  if (status == FOC_TUNE_OK && pi != NULL)
    {
      foc_pack_f32(data, pi->Kp_Id, pi->Ki_Id);
      send(FOC_CAN_ID_PI_ID, data, 8);

      foc_pack_f32(data, pi->Kp_Iq, pi->Ki_Iq);
      send(FOC_CAN_ID_PI_IQ, data, 8);

      foc_pack_f32(data, pi->Kp_Speed, pi->Ki_Speed);
      send(FOC_CAN_ID_PI_SPEED, data, 8);
    }
}

/* 执行一次整定 */
static void tuner_run(FAR motor_params_t *motor, int usage)
{
  int ret;

  pthread_mutex_lock(&g_tuner.lock);
  g_tuner.motor = *motor;
  g_tuner.state = AI_TUNER_CALC;
  pthread_mutex_unlock(&g_tuner.lock);

  ret = foc_ai_tune(motor, usage, &g_tuner.pi);

  pthread_mutex_lock(&g_tuner.lock);
  if (ret == FOC_AI_OK)
    {
      g_tuner.state = AI_TUNER_OK;
      g_tuner.last_error = 0;
    }
  else
    {
      g_tuner.state = AI_TUNER_FAIL;
      g_tuner.last_error = ret;
    }
  pthread_mutex_unlock(&g_tuner.lock);

  /* 回发 CAN 状态 */
  if (ret == FOC_AI_OK)
    {
      tuner_send_result(FOC_TUNE_OK, &g_tuner.pi);
    }
  else if (ret == FOC_AI_ERR_WHITELIST)
    {
      tuner_send_result(FOC_TUNE_WHITELIST_FAIL, NULL);
    }
  else
    {
      tuner_send_result(FOC_TUNE_AI_FAIL, NULL);
    }
}

/****************************************************************************
 * Name: tuner_thread
 *
 *  等任务 -> 调 AI -> 回发结果, 循环.
 *
 ****************************************************************************/

static FAR void *tuner_thread(FAR void *arg)
{
  motor_params_t motor;
  int usage;

  for (;;)
    {
      while (!tuner_take(&motor, &usage))
        {
          usleep(50000);
        }

      tuner_run(&motor, usage);

      /* 完成后短暂停顿, 避免忙轮询 */

      usleep(100000);
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ai_tuner_ui_start(void)
{
  /* Must be called before any sender is registered (focscope.c does this
   * before ds.init).  Idempotent: does not clear a registered sender. */

  pthread_attr_t attr;

  if (g_tuner.running)
    {
      return;
    }

  g_tuner.state = AI_TUNER_IDLE;
  pthread_mutex_init(&g_tuner.lock, NULL);

  /* The tuner thread calls curl + TLS (mbedTLS handshake needs a lot of
   * stack).  NuttX's default pthread stack is far too small and would
   * overflow inside curl_easy_perform() -> silent memory corruption /
   * data abort.  Give it a generous dedicated stack. */

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 65536);

  if (pthread_create(&g_tuner.tid, &attr, tuner_thread, NULL) == 0)
    {
      g_tuner.running = true;
    }

  pthread_attr_destroy(&attr);
}

void ai_tuner_ui_set_sender(ai_tuner_send_t send)
{
  pthread_mutex_lock(&g_tuner.lock);
  g_tuner.sender = send;
  pthread_mutex_unlock(&g_tuner.lock);
}

void ai_tuner_ui_submit(FAR motor_params_t *motor, int usage)
{
  /* temp diag: which path fed the tuner and with what params */
  printf("[AI-IN] source=CAN  Rs=%.4f Ld=%.4f Lq=%.4f Ke=%.4f poles=%d usage=%d\n",
         motor->Rs, motor->Ld, motor->Lq, motor->Ke, motor->poles, usage);
  tuner_queue(motor, usage, true);
}

void ai_tuner_ui_trigger(FAR motor_params_t *motor, int usage)
{
  /* temp diag: which path fed the tuner and with what params */
  printf("[AI-IN] source=UI   Rs=%.4f Ld=%.4f Lq=%.4f Ke=%.4f poles=%d usage=%d\n",
         motor->Rs, motor->Ld, motor->Lq, motor->Ke, motor->poles, usage);
  tuner_queue(motor, usage, false);
}

void ai_tuner_ui_request_params(int usage)
{
  /* Board->STM32 handshake: send 0x301 telling the STM32 to push its
   * motor params (0x101..0x104).  Those frames auto-trigger the tune
   * (scope_data_can -> ai_tuner_ui_submit). */
  uint8_t data[8] = { 0 };
  ai_tuner_send_t send;

  pthread_mutex_lock(&g_tuner.lock);
  send = g_tuner.sender;
  pthread_mutex_unlock(&g_tuner.lock);

  if (send == NULL)
    {
      printf("[AI] no CAN send path (run 'focscope can 0')\n");
      return;
    }

  data[0] = usage;
  send(FOC_CAN_ID_PARAM_FETCH, data, 1);
  printf("[AI] requested params from STM32 (0x301, usage=%d)\n", usage);
}

int ai_tuner_ui_state(void)
{
  int s;

  pthread_mutex_lock(&g_tuner.lock);
  s = g_tuner.state;
  pthread_mutex_unlock(&g_tuner.lock);
  return s;
}

int ai_tuner_ui_get_result(FAR pi_params_t *pi)
{
  int ok;

  pthread_mutex_lock(&g_tuner.lock);
  ok = (g_tuner.state == AI_TUNER_OK);
  if (ok)
    {
      *pi = g_tuner.pi;
    }
  pthread_mutex_unlock(&g_tuner.lock);
  return ok;
}

void ai_tuner_ui_get_motor(FAR motor_params_t *motor)
{
  pthread_mutex_lock(&g_tuner.lock);
  *motor = g_tuner.motor;
  pthread_mutex_unlock(&g_tuner.lock);
}

int ai_tuner_ui_last_error(void)
{
  int e;

  pthread_mutex_lock(&g_tuner.lock);
  e = g_tuner.last_error;
  pthread_mutex_unlock(&g_tuner.lock);
  return e;
}
