/****************************************************************************
 * apps/examples/focscope/scope_data_sim.c
 *
 * FOCPilot oscilloscope simulated data source.
 *
 * Generates FOC-like waveforms: id/iq in the synchronous (dq) frame with a
 * quadrature phase relationship, iq amplitude tracking speed setpoint
 * steps and load steps, first-order lag on speed, bus voltage ripple plus
 * a load dip, and deterministic LCG noise.
 *
 * NOTE on electrical frequency: at 3000 rpm the real electrical frequency
 * is 3000/60 * 4 pole pairs = 200 Hz, far above the 100 SPS UI sampling
 * rate (would alias into a blob). The waveform is therefore shown with a
 * display time compression factor of 1/400: 3000 rpm maps to 0.5 Hz on
 * screen (~3 cycles per screen at 1x), keeping the trace sparse enough
 * to read. This is a display artifact only.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <nuttx/config.h>

#include "scope_data.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SIM_DT           (1.0 / 100.0)  /* sample period, s (UI polls at 100 SPS) */
#define SIM_TIME_COMP    (1.0 / 400.0)  /* display time compression factor */
#define SIM_POLES        4              /* pole pairs for electrical freq */
#define SIM_STEP_PERIOD  4.0            /* setpoint/load step period, s */

/* Speed setpoint cycle, rpm */

#define SIM_N_CYCLE_LEN  4
static const double g_sim_n_cycle[SIM_N_CYCLE_LEN] =
{
  800.0, 2400.0, 1200.0, 3000.0
};

#define SIM_N_LAG_TC     0.4            /* speed first-order lag, s */
#define SIM_LOAD_LAG_TC  0.3            /* load dip smoothing, s */
#define SIM_IQ_NOM       1.0            /* base iq amplitude, A */
#define SIM_IQ_SPEED     2.2            /* iq gain vs speed (at 3000 rpm), A */
#define SIM_IQ_LOAD      0.6            /* extra iq from load step, A */
#define SIM_VBUS_NOM     24.0           /* nominal bus voltage, V */
#define SIM_VBUS_DIP     2.0            /* bus dip under load, V */

/* 16-bit LCG: deterministic, no libc dependency */

#define SIM_RAND_A       1103515245u
#define SIM_RAND_C       12345u

#define SIM_RAND_RANGE(seed, amp) \
  ((((seed) >> 16) & 0xffffu) / 32768.0 - 1.0) * (amp)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sim_ctx_s
{
  double t;            /* simulation time, s */
  double n_set;        /* speed setpoint, rpm */
  double n;            /* actual (lagged) speed, rpm */
  double theta;        /* displayed electrical angle, rad */
  double load_sm;      /* smoothed load factor, 0..1 */
  uint8_t step_idx;    /* current setpoint/load cycle index */
  uint32_t seed;       /* noise LCG seed */
};

typedef struct sim_ctx_s sim_ctx_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static sim_ctx_t g_sim;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* xorshift-ish noise: returns uniform in [-amp, +amp] */

static double sim_noise(FAR sim_ctx_t *ctx, double amp)
{
  ctx->seed = ctx->seed * SIM_RAND_A + SIM_RAND_C;
  return SIM_RAND_RANGE(ctx->seed, amp);
}

static int sim_init(FAR struct scope_ds_s *ds)
{
  g_sim.t       = 0.0;
  g_sim.n_set   = g_sim_n_cycle[0];
  g_sim.n       = g_sim_n_cycle[0];
  g_sim.theta   = 0.0;
  g_sim.load_sm = 0.0;
  g_sim.step_idx = 0;
  g_sim.seed    = 0x2f6e2b1u;

  return 0;
}

static int sim_get_sample(FAR struct scope_ds_s *ds,
                          FAR scope_sample_t *s)
{
  sim_ctx_t *ctx = &g_sim;
  double dt     = SIM_DT;
  double step_t;
  double iq0;
  double id;
  double iq;
  double load_target;

  /* Step profile: every SIM_STEP_PERIOD s, advance speed setpoint and
   * toggle the load.
   */

  step_t = fmod(ctx->t, SIM_STEP_PERIOD);
  if (step_t < dt && ctx->t > 0.0)
    {
      ctx->step_idx = (ctx->step_idx + 1) % SIM_N_CYCLE_LEN;
    }

  ctx->n_set = g_sim_n_cycle[ctx->step_idx];
  load_target = (ctx->step_idx % 2) ? 1.0 : 0.0;

  /* First-order lags */

  ctx->n      += (ctx->n_set - ctx->n) * (dt / SIM_N_LAG_TC);
  ctx->load_sm += (load_target - ctx->load_sm) * (dt / SIM_LOAD_LAG_TC);

  /* Display-compressed electrical angle advance */

  ctx->theta += 2.0 * M_PI * (ctx->n / 60.0) * SIM_POLES *
                SIM_TIME_COMP * dt;

  /* q-axis current: amplitude tracks speed and load, with harmonics */

  iq0 = SIM_IQ_NOM + SIM_IQ_SPEED * (ctx->n / 3000.0) +
        SIM_IQ_LOAD * ctx->load_sm;
  iq  = iq0 * sin(ctx->theta) + 0.4 * sin(2.0 * ctx->theta) +
        sim_noise(ctx, 0.06);

  /* d-axis current: small, quadrature to iq (FOC dq-frame signature) */

  id  = 0.4 * cos(ctx->theta) + 0.15 * sin(3.0 * ctx->theta) +
        sim_noise(ctx, 0.05);

  /* Bus voltage: nominal + ripple + load dip */

  s->vbus = SIM_VBUS_NOM + 0.8 * sin(2.0 * ctx->theta) -
            SIM_VBUS_DIP * ctx->load_sm;

  /* Bus current: idle draw + speed-proportional + load step */

  s->ibus = 0.35 + 0.9 * (ctx->n / 3000.0) + 2.2 * ctx->load_sm +
            sim_noise(ctx, 0.08);
  s->ibus_raw = (uint32_t)(s->ibus * 1000.0f);

  s->id    = (float)id;
  s->iq    = (float)iq;
  s->speed = (float)ctx->n;

  ctx->t += dt;

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

scope_ds_t scope_ds_sim_get(void)
{
  scope_ds_t ds;

  ds.name          = "sim";
  ds.init          = sim_init;
  ds.deinit        = NULL;
  ds.get_sample    = sim_get_sample;
  ds.get_can_stats = NULL;   /* sim: no CAN link info */

  return ds;
}
