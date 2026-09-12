/****************************************************************************
 * apps/examples/focscope/scope_data.h
 *
 * FOCPilot oscilloscope data source abstraction.
 *
 * The UI (scope_ui.c) only talks to this interface. Simulated data lives
 * in scope_data_sim.c; a future CAN-backed source (scope_data_can.c) can
 * replace it with no UI changes.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_FOCSCOPE_SCOPE_DATA_H
#define __APPS_EXAMPLES_FOCSCOPE_SCOPE_DATA_H

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One scope sample point */

struct scope_sample_s
{
  float id;      /* d-axis current, A */
  float iq;      /* q-axis current, A */
  float speed;   /* rotor speed, rpm */
  float vbus;    /* bus voltage, V */
  float ibus;      /* bus current, A (frame 0x502) */
  uint32_t ibus_raw; /* raw uint32 from 0x502 [0:3], mA (LE) */
};

typedef struct scope_sample_s scope_sample_t;

/* CAN link statistics for the link-info page. Filled by the optional
 * get_can_stats() callback (UI thread, non-blocking). */

struct scope_can_stats_s
{
  /* Application-level counters (reader thread) */

  uint32_t nframes_all;    /* every frame received on the bus */
  uint32_t nframes_501;    /* valid 0x501 telemetry frames */
  uint32_t nframes_502;    /* valid 0x502 bus-current frames */
  uint32_t nframes_bad;    /* wrong ID or short DLC */
  int32_t  last_age_ms;    /* ms since last valid frame; -1 = never */

  /* Driver-level counters (read via CANIOC_GET_STATS, R528 driver) */

  uint32_t isr_cnt;        /* ISR entries */
  uint32_t rx_cnt;         /* frames delivered by the driver */
  uint32_t txdone_cnt;     /* TX-complete callbacks */
  uint32_t isr_last_isrc;  /* last INT register value (0x3 = rx+tx normal) */
  uint32_t isr_last_status;/* last STA register value (0x2c = all normal) */
  uint32_t msel;           /* mode register */
  uint32_t btime;          /* bit-timing register */
};

typedef struct scope_can_stats_s scope_can_stats_t;

/* Data source operations. get_sample() advances the source by exactly one
 * sample period and returns the new sample. It must be fast and
 * non-blocking: the UI polls it from the LVGL timer callback (single
 * thread). A CAN-backed implementation may read from its own ring buffer
 * fed by a reader thread.
 */

struct scope_ds_s
{
  const char *name;

  /* Optional; may be NULL */

  int  (*init)(FAR struct scope_ds_s *ds);
  void (*deinit)(FAR struct scope_ds_s *ds);

  /* Mandatory. Return 0 on success, negative on failure */

  int  (*get_sample)(FAR struct scope_ds_s *ds, FAR scope_sample_t *s);

  /* Optional: CAN link statistics. NULL for non-CAN sources (the UI then
   * disables the link-info page). Return 0 on success, negative on error.
   */

  int  (*get_can_stats)(FAR struct scope_ds_s *ds,
                        FAR scope_can_stats_t *st);
};

typedef struct scope_ds_s scope_ds_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Factory: simulated FOC waveform source */

scope_ds_t scope_ds_sim_get(void);

/* Factory: CAN telemetry source (FOCPILOT CAN PROTOCOL v1).
 * port: 0 = /dev/can0, 1 = /dev/can1 */

scope_ds_t scope_ds_can_get(int port);

#endif /* __APPS_EXAMPLES_FOCSCOPE_SCOPE_DATA_H */
