/****************************************************************************
 * apps/examples/focscope/scope_data_can.c
 *
 * FOCPilot CAN telemetry data source (FOCPILOT CAN PROTOCOL v1).
 *
 * A reader thread opens the CAN device and consumes every broadcast frame:
 *
 *   ID 0x501, standard frame, 8 data bytes, little-endian, ~100 Hz
 *     [0:1] id   int16  mA   (d-axis current,  +/-32767 mA)
 *     [2:3] iq   int16  mA   (q-axis current,  +/-32767 mA)
 *     [4:5] rpm  int16  rpm  (rotor speed,     +/-32767 rpm)
 *     [6:7] vbus uint16 mV   (bus voltage,     0..65535 mV)
 *
 *   ID 0x502, standard frame, 8 data bytes, little-endian, ~100 Hz
 *     [0:1] ibus uint16 mA   (bus current,     0..65535 mA)
 *     [2:7] reserved (0)
 *
 * Both frames are sent from the same 10 ms drive-board tick. Either one
 * refreshes the link-alive timestamp; 0x501 stays the waveform heartbeat.
 *
 * The UI polls get_sample() from the LVGL timer callback (single thread).
 * The reader thread only touches CAN + a mutex-guarded latest-sample slot,
 * never LVGL, so the single-threaded UI constraint is preserved.
 *
 * If no frame arrives within SCOPE_CAN_STALE_MS the source reports an
 * error and the UI stops advancing the traces (link lost); once frames
 * resume the traces continue automatically.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <nuttx/can/can.h>

#include "scope_data.h"
#include "ai_tuner_ui.h"
#include "focpilot_can_proto.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* FOCPilot CAN telemetry protocol v1 */

#define SCOPE_CAN_ID           0x501 /* Broadcast telemetry frame ID */
#define SCOPE_CAN_ID_IBUS      0x502 /* Bus-current telemetry frame ID */
#define SCOPE_CAN_MAX_DLC      8

/* Driver-private debug ioctl (R528 driver, same as cantest). The driver
 * only reads registers/counters here, so calling it from the UI thread
 * while the reader thread is blocked in read() is safe. */

#define CANIOC_GET_STATS       _CANIOC(20)

struct r528_can_stats_s
{
  uint32_t isr_cnt;
  uint32_t rx_cnt;
  uint32_t txdone_cnt;
  uint32_t err_printed;
  uint32_t isrc_hist[8];
  uint32_t isr_last_isrc;
  uint32_t isr_last_status;
  uint32_t msel;
  uint32_t btime;
  uint32_t cfg0;
};

/* Reader thread parameters */

#define SCOPE_CAN_STALE_MS     500   /* Link considered lost after this */
#define SCOPE_CAN_INIT_TIMEOUT_MS 2000 /* No frame yet since open */

/* Main chart is mA: UI multiplies A by 1000, so mA maps 1:1 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct scope_can_s
{
  scope_ds_t   ds;
  pthread_t    tid;
  int          fd;
  int          port;        /* 0 = /dev/can0, 1 = /dev/can1 */
  bool         running;

  pthread_mutex_t lock;
  struct timespec t_open;   /* Reader start time */
  struct timespec t_last;   /* Last frame arrival */
  bool           fresh;     /* A frame arrived since the last get_sample() */
  scope_sample_t latest;

  /* Frame counters (mutex-guarded, consumed by the link-info page) */

  uint32_t nframes_all;     /* every frame received */
  uint32_t nframes_501;     /* valid 0x501 frames */
  uint32_t nframes_502;     /* valid 0x502 frames */
  uint32_t nframes_bad;     /* wrong ID or short DLC */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* focscope is single-instance, so one static source is fine */

static struct scope_can_s g_can;
static char g_can_name[16];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: scope_can_reader
 *
 * Description:
 *   Blocking CAN reader thread.  Consumes every frame, keeps the latest
 *   telemetry sample.
 *
 ****************************************************************************/

static FAR void *scope_can_reader(FAR void *arg)
{
  struct scope_can_s *priv = (FAR struct scope_can_s *)arg;
  struct can_msg_s msg;
  scope_sample_t s;
  motor_params_t ai_motor;
  int ai_usage = FOC_USAGE_ROBOT;
  int ai_have_req, ai_have_rs_ld, ai_have_lq_ke, ai_have_poles;
  uint32_t raw;
  ssize_t nbytes;

  ai_have_req = ai_have_rs_ld = ai_have_lq_ke = ai_have_poles = 0;
  memset(&ai_motor, 0, sizeof(ai_motor));

  while (priv->running)
    {
      memset(&msg, 0, sizeof(msg));
      nbytes = read(priv->fd, &msg, CAN_MSGLEN(SCOPE_CAN_MAX_DLC));
      if (nbytes < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          /* Device went away (or fatal error): stop the thread */

          break;
        }

      /* Count every frame; accept 0x501 (waveform) and 0x502 (bus
       * current), reject everything else. Either frame refreshes the
       * link-alive timestamp. */

      if (msg.cm_hdr.ch_id == SCOPE_CAN_ID_IBUS && msg.cm_hdr.ch_dlc >= 4)
        {
          pthread_mutex_lock(&priv->lock);
          priv->nframes_all++;
          priv->nframes_502++;
          pthread_mutex_unlock(&priv->lock);

          /* 0x502: [0:3] ibus uint32 mA, little-endian */

          raw = (uint32_t)msg.cm_data[0] |
                ((uint32_t)msg.cm_data[1] << 8) |
                ((uint32_t)msg.cm_data[2] << 16) |
                ((uint32_t)msg.cm_data[3] << 24);

          pthread_mutex_lock(&priv->lock);
          priv->latest.ibus = (float)raw / 1000.0f;
          priv->latest.ibus_raw = raw; /* raw bytes for the UI debug line */
          priv->fresh  = true;
          clock_gettime(CLOCK_MONOTONIC, &priv->t_last);
          pthread_mutex_unlock(&priv->lock);
          continue;
        }

      /* AI tune parameter frames: 0x101~0x104. Collect the full set,
       * then hand to the background tuner (shared CAN channel). */

      switch (msg.cm_hdr.ch_id)
        {
          case FOC_CAN_ID_PARAM_REQ:
            ai_usage = msg.cm_data[FOC_PARAMREQ_OFF_USAGE];
            ai_have_req = 1;
            printf("[CAN-REQ] usage=%d\n", ai_usage);
            break;

          case FOC_CAN_ID_PARAM_RS_LD:
            /* temp diag: show the raw bytes so we can tell whether the STM32
             * sends all-zero parameter frames (nothing set) or the unpack is
             * wrong. */
            printf("[CAN-PARAM] RS_LD raw=%02x%02x%02x%02x | %02x%02x%02x%02x"
                   " -> Rs=%.6f Ld=%.6f\n",
                   msg.cm_data[0], msg.cm_data[1], msg.cm_data[2],
                   msg.cm_data[3], msg.cm_data[4], msg.cm_data[5],
                   msg.cm_data[6], msg.cm_data[7],
                   foc_unpack_f32(&msg.cm_data[0]),
                   foc_unpack_f32(&msg.cm_data[4]));
            ai_motor.Rs = foc_unpack_f32(&msg.cm_data[0]);
            ai_motor.Ld = foc_unpack_f32(&msg.cm_data[4]);
            ai_have_rs_ld = 1;
            break;

          case FOC_CAN_ID_PARAM_LQ_KE:
            printf("[CAN-PARAM] LQ_KE raw=%02x%02x%02x%02x | %02x%02x%02x%02x"
                   " -> Lq=%.6f Ke=%.6f\n",
                   msg.cm_data[0], msg.cm_data[1], msg.cm_data[2],
                   msg.cm_data[3], msg.cm_data[4], msg.cm_data[5],
                   msg.cm_data[6], msg.cm_data[7],
                   foc_unpack_f32(&msg.cm_data[0]),
                   foc_unpack_f32(&msg.cm_data[4]));
            ai_motor.Lq = foc_unpack_f32(&msg.cm_data[0]);
            ai_motor.Ke = foc_unpack_f32(&msg.cm_data[4]);
            ai_have_lq_ke = 1;
            break;

          case FOC_CAN_ID_PARAM_POLES:
            printf("[CAN-PARAM] POLES raw=%02x -> poles=%d\n",
                   msg.cm_data[FOC_PARAMPOLES_OFF_POLES],
                   msg.cm_data[FOC_PARAMPOLES_OFF_POLES]);
            ai_motor.poles = msg.cm_data[FOC_PARAMPOLES_OFF_POLES];
            ai_have_poles = 1;
            break;

          default:
            break;
        }

      if (ai_have_req && ai_have_rs_ld && ai_have_lq_ke && ai_have_poles)
        {
          ai_tuner_ui_submit(&ai_motor, ai_usage);

          /* Reset collector for the next tune request */

          ai_have_req = ai_have_rs_ld = ai_have_lq_ke = ai_have_poles = 0;
          memset(&ai_motor, 0, sizeof(ai_motor));
        }

      /* AI parameter frames (0x101~0x104) are consumed here; don't count
       * them as bad telemetry. */

      if (msg.cm_hdr.ch_id >= FOC_CAN_ID_PARAM_REQ &&
          msg.cm_hdr.ch_id <= FOC_CAN_ID_PARAM_POLES)
        {
          continue;
        }

      /* Frames that are not 0x501/0x502 telemetry and not AI params are
       * rejected below. */

      if (msg.cm_hdr.ch_id != SCOPE_CAN_ID || msg.cm_hdr.ch_dlc < 8)
        {
          pthread_mutex_lock(&priv->lock);
          priv->nframes_all++;
          priv->nframes_bad++;
          pthread_mutex_unlock(&priv->lock);
          continue;
        }

      pthread_mutex_lock(&priv->lock);
      priv->nframes_all++;
      priv->nframes_501++;
      pthread_mutex_unlock(&priv->lock);

      /* Little-endian decode (match the drive-board implementation) */

      raw     = (uint32_t)msg.cm_data[0] | ((uint32_t)msg.cm_data[1] << 8);
      s.id    = (float)(int16_t)raw / 1000.0f;

      raw     = (uint32_t)msg.cm_data[2] | ((uint32_t)msg.cm_data[3] << 8);
      s.iq    = (float)(int16_t)raw / 1000.0f;

      raw     = (uint32_t)msg.cm_data[4] | ((uint32_t)msg.cm_data[5] << 8);
      s.speed = (float)(int16_t)raw;

      raw     = (uint32_t)msg.cm_data[6] | ((uint32_t)msg.cm_data[7] << 8);
      s.vbus  = (float)raw / 1000.0f;

      pthread_mutex_lock(&priv->lock);
      priv->latest = s;
      priv->fresh  = true;
      clock_gettime(CLOCK_MONOTONIC, &priv->t_last);
      pthread_mutex_unlock(&priv->lock);
    }

  return NULL;
}

/****************************************************************************
 * Name: scope_can_elapsed_ms
 *
 * Description:
 *   Milliseconds elapsed between a and b (a >= b).
 *
 ****************************************************************************/

static int64_t scope_can_elapsed_ms(FAR const struct timespec *a,
                                    FAR const struct timespec *b)
{
  return (int64_t)(a->tv_sec - b->tv_sec) * 1000 +
         (a->tv_nsec - b->tv_nsec) / 1000000;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: scope_can_ai_send
 *
 * Description:
 *   Send a CAN frame on the same fd used for telemetry (registered with
 *   ai_tuner_ui so AI result frames go out on the shared channel).
 *
 ****************************************************************************/

static int scope_can_ai_send(uint16_t id, FAR const uint8_t *data, int dlc)
{
  struct can_msg_s msg;
  ssize_t nbytes;

  if (g_can.fd < 0 || dlc > 8)
    {
      return -1;
    }

  memset(&msg, 0, sizeof(msg));
  msg.cm_hdr.ch_id  = id;
  msg.cm_hdr.ch_dlc = dlc;
  if (data)
    {
      memcpy(msg.cm_data, data, dlc);
    }

  nbytes = write(g_can.fd, &msg, CAN_MSGLEN(dlc));
  return (nbytes < 0) ? -1 : 0;
}

/****************************************************************************
 * Name: scope_ds_can_init
 *
 ****************************************************************************/

static int scope_ds_can_init(FAR scope_ds_t *ds)
{
  struct scope_can_s *priv = &g_can;
  char path[16];
  int ret;

  snprintf(path, sizeof(path), "/dev/can%d", priv->port);

  priv->fd = open(path, O_RDWR);   /* R/W: telemetry + AI result frames */
  if (priv->fd < 0)
    {
      fprintf(stderr, "scope_can: open %s failed: %d\n", path, errno);
      return -1;
    }

  /* Share this CAN channel with the AI tuner (submits + result frames) */

  ai_tuner_ui_set_sender(scope_can_ai_send);

  pthread_mutex_init(&priv->lock, NULL);
  clock_gettime(CLOCK_MONOTONIC, &priv->t_open);
  priv->t_last = priv->t_open;
  priv->fresh  = false;
  memset(&priv->latest, 0, sizeof(priv->latest));
  priv->running = true;

  ret = pthread_create(&priv->tid, NULL, scope_can_reader, priv);
  if (ret != 0)
    {
      fprintf(stderr, "scope_can: thread create failed: %d\n", ret);
      close(priv->fd);
      priv->fd = -1;
      priv->running = false;
      return -1;
    }

  return 0;
}

/****************************************************************************
 * Name: scope_ds_can_deinit
 *
 ****************************************************************************/

static void scope_ds_can_deinit(FAR scope_ds_t *ds)
{
  struct scope_can_s *priv = &g_can;

  priv->running = false;
  pthread_cancel(priv->tid);
  pthread_join(priv->tid, NULL);
  if (priv->fd >= 0)
    {
      close(priv->fd);
      priv->fd = -1;
    }

  pthread_mutex_destroy(&priv->lock);
}

/****************************************************************************
 * Name: scope_ds_can_get_sample
 *
 * Description:
 *   Return the latest telemetry sample.  Never blocks (the reader thread
 *   does the I/O).  Reports an error when no frame has arrived for
 *   SCOPE_CAN_STALE_MS (link lost) so the UI stops the traces; normal
 *   operation resumes automatically.
 *
 ****************************************************************************/

static int scope_ds_can_get_sample(FAR scope_ds_t *ds, FAR scope_sample_t *s)
{
  struct scope_can_s *priv = &g_can;
  struct timespec now;
  int64_t since_last;
  int64_t since_open;

  clock_gettime(CLOCK_MONOTONIC, &now);

  pthread_mutex_lock(&priv->lock);
  since_last = scope_can_elapsed_ms(&now, &priv->t_last);
  since_open = scope_can_elapsed_ms(&now, &priv->t_open);

  if ((!priv->fresh && since_last > SCOPE_CAN_STALE_MS) ||
      (!priv->fresh && since_open > SCOPE_CAN_INIT_TIMEOUT_MS))
    {
      pthread_mutex_unlock(&priv->lock);
      return -1;
    }

  *s = priv->latest;
  priv->fresh = false;
  pthread_mutex_unlock(&priv->lock);

  return 0;
}

/****************************************************************************
 * Name: scope_ds_can_get_stats
 *
 * Description:
 *   Fill the CAN link statistics for the UI link-info page.  Fast and
 *   non-blocking: app-level counters come from the mutex-guarded reader
 *   state, driver-level counters from the read-only stats ioctl.
 *
 ****************************************************************************/

static int scope_ds_can_get_stats(FAR scope_ds_t *ds,
                                  FAR scope_can_stats_t *st)
{
  struct scope_can_s *priv = &g_can;
  struct r528_can_stats_s hw;
  struct timespec now;

  memset(st, 0, sizeof(*st));

  pthread_mutex_lock(&priv->lock);
  st->nframes_all = priv->nframes_all;
  st->nframes_501 = priv->nframes_501;
  st->nframes_502 = priv->nframes_502;
  st->nframes_bad = priv->nframes_bad;
  pthread_mutex_unlock(&priv->lock);

  clock_gettime(CLOCK_MONOTONIC, &now);
  st->last_age_ms = (int32_t)scope_can_elapsed_ms(&now, &priv->t_last);
  if (priv->nframes_501 == 0 && priv->nframes_502 == 0)
    {
      st->last_age_ms = -1;   /* nothing received yet */
    }

  if (priv->fd >= 0 &&
      ioctl(priv->fd, CANIOC_GET_STATS, (unsigned long)&hw) == 0)
    {
      st->isr_cnt         = hw.isr_cnt;
      st->rx_cnt          = hw.rx_cnt;
      st->txdone_cnt      = hw.txdone_cnt;
      st->isr_last_isrc   = hw.isr_last_isrc;
      st->isr_last_status = hw.isr_last_status;
      st->msel            = hw.msel;
      st->btime           = hw.btime;
    }

  return 0;
}

/****************************************************************************
 * Name: scope_ds_can_get
 *
 * Description:
 *   Factory: CAN-backed data source.  The device path is compile-time
 *   fixed to /dev/can0 for now.
 *
 ****************************************************************************/

scope_ds_t scope_ds_can_get(int port)
{
  memset(&g_can, 0, sizeof(g_can));
  g_can.fd = -1;
  g_can.port = port;

  snprintf(g_can_name, sizeof(g_can_name), "CAN%d 0x501/2", port);

  g_can.ds.name          = g_can_name;
  g_can.ds.init          = scope_ds_can_init;
  g_can.ds.deinit        = scope_ds_can_deinit;
  g_can.ds.get_sample    = scope_ds_can_get_sample;
  g_can.ds.get_can_stats = scope_ds_can_get_stats;

  return g_can.ds;
}
