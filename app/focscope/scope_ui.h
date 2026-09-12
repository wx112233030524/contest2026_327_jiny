/****************************************************************************
 * apps/examples/focscope/scope_ui.h
 *
 * FOCPilot oscilloscope UI. Tuning constants live here; the UI logic is in
 * scope_ui.c.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_FOCSCOPE_SCOPE_UI_H
#define __APPS_EXAMPLES_FOCSCOPE_SCOPE_UI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "scope_data.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SCOPE_POINT_CNT          600   /* ring history length per channel */
#define SCOPE_TICK_MS            10    /* data timer period, ms */
#define SCOPE_SAMPLES_PER_TICK   1     /* samples pushed per tick -> 100 SPS, 6 s history */
#define SCOPE_PRE_FILL           200   /* samples pre-filled so the screen starts alive (2 s) */

/* Effective sample rate (100 SPS); used to render the time span in seconds */

#define SCOPE_SPS (SCOPE_SAMPLES_PER_TICK * 1000 / SCOPE_TICK_MS)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Build the whole UI on the active screen and start the data timer.
 * The screen must be initialized (lv_nuttx_init) before calling this.
 */

void scope_ui_create(FAR scope_ds_t *ds);

#endif /* __APPS_EXAMPLES_FOCSCOPE_SCOPE_UI_H */
