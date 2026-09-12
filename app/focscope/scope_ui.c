/****************************************************************************
 * apps/examples/focscope/scope_ui.c
 *
 * FOCPilot oscilloscope UI.
 *
 * Layout (320x480 portrait):
 *   title bar  (y=6)      title + [EN/中文 toggle][state]
 *   main chart (4,34)     312x202  id/iq current, mA
 *   rpm chart  (4,238)    312x94   speed, rpm
 *   readouts   (y=336)    id / iq / rpm / Vb / Ib live values
 *   buttons r1 (y=376)    PAUSE  ZOOM+  ZOOM-  SCALE
 *   buttons r2 (y=428)    CLEAR  <      >      CH
 *
 * Language: all user-visible strings live in one table (g_lang, zh + en).
 * The top-right button toggles the active language and re-texts every
 * label from that table. The full-Unicode 20px PuHui font (from
 * lvgldemo's font_puhui_20_4.c) covers both scripts.
 *
 * Behavior:
 *   - Data is polled from the scope_ds_t source in a 50 ms lv_timer
 *     callback (single-threaded; all LVGL calls happen inside
 *     lv_timer_handler()).
 *   - RUN: values scroll in (SHIFT mode). ZOOM+/ZOOM- (horizontal zoom /
 *     time base) and SCALE (vertical range) work live; a small label over
 *     the main chart shows the current time span and range, so the
 *     adjustments are visible while running.
 *   - PAUSED: the write cursor is frozen; ZOOM+/- and SCALE keep working,
 *     and < / > (pan) plus CLEAR become available.
 *   - CH cycles id/iq visibility.
 *
 * NOTE on v9.1 chart semantics (verified against lv_chart.c):
 *   - lv_chart_get_x_start_point() only returns ser->start_point in
 *     SHIFT mode; CIRCULAR mode ignores panning.
 *   - lv_chart_set_x_start_point() is per-series - set every series.
 *   - set_next_value() advances ser->start_point; never touch
 *     x_start_point while running.
 *   - Zoomed drawing maps point window [start, start+span) onto the
 *     content width, span = point_cnt * 256 / zoom_x.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "scope_ui.h"
#include "ai_tuner_ui.h"
#include "wifi_ui.h"

/* Full-Unicode 20px PuHui font, copied from lvgldemo's font_puhui_20_4.c
 * (Chinese + Latin, range 0x0-0xfffff). lvgldemo itself is removed from
 * the build: all apps link into one binary, so both apps' copies of the
 * symbol would collide. Only labels that need Chinese use this font;
 * chart tick labels keep the small theme font. */

LV_FONT_DECLARE(font_puhui_20_4);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Layout */

#define SCOPE_CHART_X       4
#define SCOPE_CHART_W       312
#define SCOPE_CHART_Y       34
#define SCOPE_CHART_H       202
#define SCOPE_RPM_Y         238
#define SCOPE_RPM_H         94
#define SCOPE_RO_Y          336
#define SCOPE_BTN_Y1        376
#define SCOPE_BTN_Y2        428
#define SCOPE_BTN_W         72
#define SCOPE_BTN_H         44
#define SCOPE_BTN_X(i)      (8 + (i) * 78)

/* Colors (dark oscilloscope theme) */

#define CLR_BG             lv_color_hex(0x101418)
#define CLR_CHART_BG       lv_color_hex(0x161b22)
#define CLR_CHART_BD       lv_color_hex(0x303841)
#define CLR_GRID           lv_color_hex(0x232a34)
#define CLR_TEXT           lv_color_hex(0xd8dee9)
#define CLR_DIM            lv_color_hex(0x6a737d)
#define CLR_ID             lv_color_hex(0x33cc88)
#define CLR_IQ             lv_color_hex(0xe8c840)
#define CLR_RPM            lv_color_hex(0x38a3ff)
#define CLR_IBUS           lv_color_hex(0xff9f43)
#define CLR_RUN            lv_color_hex(0x33cc88)
#define CLR_PAUSED         lv_color_hex(0xff6b6b)
#define CLR_BTN_BG         lv_color_hex(0x222a35)
#define CLR_BTN_BG_PRS     lv_color_hex(0x2f3b49)
#define CLR_BTN_BG_DIS     lv_color_hex(0x171c23)
#define CLR_BTN_BD         lv_color_hex(0x3a4653)
#define CLR_BTN_TXT_DIS    lv_color_hex(0x7a8494)

/* Link considered lost after this many ms without a 0x501 frame
 * (must match SCOPE_CAN_STALE_MS in scope_data_can.c) */

#define SCOPE_CAN_STALE_MS 500

/* Horizontal zoom table: 256 = 1x. Visible span = POINT_CNT*256/zoom:
 * 600, 480, 400, 300, 240, 192, 150, 120, 100, 80, 60, 50 samples
 * (6.0 .. 0.5 s at 100 SPS). */

static const uint32_t g_zoom_table[] =
{
  256, 320, 384, 512, 640, 800, 1024,
  1280, 1536, 1920, 2560, 3072
};

#define ZOOM_TABLE_LEN (sizeof(g_zoom_table) / sizeof(g_zoom_table[0]))

/* Vertical ranges for the main chart, mA */

static const int32_t g_scale_main[][2] =
{
  { -5000,  5000 },   /* default */
  { -2500,  2500 },
  { -1000,  1000 },   /* 1 A: zoom in on id / small currents */
  { -10000, 10000 },
  { -20000, 20000 },  /* real drive board: up to +-20 A */
  { -40000, 40000 }   /* real drive board: up to +-40 A */
};

#define SCALE_MAIN_LEN (sizeof(g_scale_main) / sizeof(g_scale_main[0]))

/* Vertical ranges for the RPM chart (cycles in lockstep with SCALE) */

static const int32_t g_scale_rpm[][2] =
{
  { 0,    3000 },     /* default */
  { 0,    1500 },
  { 0,    600 },
  { 0,    6000 },
  { 0,    15000 },
  { 0,    30000 }
};

#define SCALE_RPM_LEN (sizeof(g_scale_rpm) / sizeof(g_scale_rpm[0]))

/* Button actions */

enum
{
  ACT_PAUSE = 0,
  ACT_ZOOM_IN,
  ACT_ZOOM_OUT,
  ACT_SCALE,
  ACT_CLEAR,
  ACT_LEFT,
  ACT_RIGHT,
  ACT_CH,
  ACT_CAN,
  ACT_AI,
  ACT_AI_START,
  ACT_AI_SRC,
  ACT_WIFI,
  ACT_WIFI_SCAN,
  ACT_WIFI_CONN,
  ACT_WIFI_DISC,
  ACT_WIFI_SEL_PREV,
  ACT_WIFI_SEL_NEXT,
  ACT_LANG
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef enum
{
  SCOPE_STATE_RUN = 0,
  SCOPE_STATE_PAUSED
} scope_state_t;

typedef enum
{
  LANG_ZH = 0,
  LANG_EN
} scope_lang_t;

/* One row of this table per language; every user-visible string lives
 * here so a language switch is one re-text pass. btn[] is indexed by
 * ACT_* (ACT_PAUSE = 0 ... ACT_CAN = 8). */

struct scope_lang_s
{
  const char *title;
  const char *state_run;
  const char *state_paused;
  const char *lang_name;   /* label of the language toggle */
  const char *btn[10];
  const char *can_title;      /* CAN page header */
  const char *can_back;       /* CAN button label while on the CAN page */
  const char *can_link_ok;
  const char *can_link_lost;
  const char *can_frames;     /* frames counter prefix */
  const char *can_rate;       /* frame rate prefix */
  const char *can_bad;        /* bad frames prefix */
  const char *can_raw;        /* 0x502 raw-bytes line prefix */
  const char *can_hw;         /* controller section header */
  const char *can_no;         /* no CAN source (sim) */
  const char *ai_title;       /* AI page header */
  const char *ai_back;        /* AI button label while on the AI page */
  const char *ai_start;       /* AI start button */
  const char *ai_idle;
  const char *ai_wait;
  const char *ai_calc;
  const char *ai_ok;
  const char *ai_fail;
  const char *wifi_title;     /* WiFi page header */
  const char *wifi_back;      /* WiFi button label while on the WiFi page */
  const char *wifi_scan;
  const char *wifi_conn;
  const char *wifi_disc;
  const char *wifi_prev;
  const char *wifi_next;
  const char *wifi_idle;
  const char *wifi_scanning;
  const char *wifi_connecting;
  const char *wifi_connected;
  const char *wifi_error;
  const char *wifi_none;
};

struct scope_ui_s
{
  scope_ds_t *ds;

  scope_state_t state;
  scope_lang_t lang;

  /* Widgets */

  lv_obj_t *chart_cur;
  lv_obj_t *chart_rpm;
  lv_chart_series_t *ser_id;
  lv_chart_series_t *ser_iq;
  lv_chart_series_t *ser_rpm;
  lv_obj_t *lb_title;
  lv_obj_t *lb_state;
  lv_obj_t *lb_lang;
  lv_obj_t *btn_lang;
  lv_obj_t *lab_btn[10];   /* button labels, indexed by ACT_* */
  lv_obj_t *lb_id;
  lv_obj_t *lb_iq;
  lv_obj_t *lb_rpm;
  lv_obj_t *lb_vbus;
  lv_obj_t *lb_ibus;
  lv_obj_t *lb_view;    /* time-span / range indicator over the main chart */
  lv_obj_t *btn_pause;
  lv_obj_t *btn_zoom_in;
  lv_obj_t *btn_zoom_out;
  lv_obj_t *btn_scale;
  lv_obj_t *btn_left;
  lv_obj_t *btn_right;
  lv_obj_t *btn_clear;
  lv_obj_t *btn_ch;
  lv_obj_t *btn_can;    /* toggles the CAN link-info page */
  lv_obj_t *btn_ai;     /* toggles the AI tune page */
  lv_obj_t *btn_wifi;   /* toggles the WiFi page (top bar) */
  lv_obj_t *lb_wifi_btn;/* WiFi top-bar button label */

  /* CAN link-info page (covers charts + readouts when shown) */

  lv_obj_t *can_page;      /* container */
  lv_obj_t *lb_can_link;   /* OK / LOST banner */
  lv_obj_t *lb_can_frames; /* frames / rate counters */
  lv_obj_t *lb_can_bad;    /* bad-frame counter */
  lv_obj_t *lb_can_val;    /* latest id/iq/rpm/vb/ib values */
  lv_obj_t *lb_can_hw;     /* controller registers */

  /* AI tune page */

  lv_obj_t *ai_page;       /* container */
  lv_obj_t *lb_ai_state;   /* state banner */
  lv_obj_t *lb_ai_motor;   /* motor params */
  lv_obj_t *lb_ai_result;  /* PI results / error */
  lv_obj_t *btn_ai_start;  /* manual start button */
  lv_obj_t *lb_ai_start;   /* manual start button label */
  motor_params_t ai_motor; /* last-run motor params (CAN echo) */
  motor_params_t ai_manual;/* MANUAL-mode params (typed in the AI page) */
  lv_obj_t *btn_ai_src;   /* source switch: CAN / MANUAL */
  lv_obj_t *lb_ai_src;    /* source button label */
  lv_obj_t *lb_ai_pm;     /* "MOTOR Rs Ld ..." caption */
  lv_obj_t *ai_ta;        /* manual params textarea */
  lv_obj_t *ai_kb;        /* AI-page keyboard overlay */
  char ai_params[80];     /* manual params text buffer */
  int ai_src;             /* 0=MANUAL, 1=CAN */
  uint8_t ai_shown;        /* 1: AI page visible, 0: not */

  /* WiFi page */

  lv_obj_t *wifi_page;     /* container */
  lv_obj_t *lb_wifi_state; /* state banner */
  lv_obj_t *lb_wifi_sel;   /* current connection line */
  lv_obj_t *wifi_ta_ssid;  /* SSID textarea */
  lv_obj_t *wifi_ta;       /* password textarea */
  lv_obj_t *wifi_kb;       /* virtual keyboard (screen-level overlay) */
  lv_obj_t *lb_wifi_pwd;   /* "pwd" caption */
  lv_obj_t *lb_wifi_sid;   /* "SSID" caption */
  lv_obj_t *ta_active;     /* textarea currently attached to the keyboard */
  uint8_t wifi_shown;      /* 1: WiFi page visible */
  uint8_t wifi_kb_open;    /* 1: keyboard open */
  char wifi_ssid[33];
  char wifi_password[64];
  lv_obj_t *wifi_btn[2];   /* CONN DISC */

  /* View state. zoom is live in both states; pan and cursor are the
   * paused-view window (frozen cursor anchor + history offset). */

  uint32_t zoom;      /* current horizontal zoom, 256 = 1x */
  uint32_t pan;       /* samples panned back from the frozen cursor */
  uint32_t cursor;    /* frozen write cursor (ser->start_point) */

  uint8_t ch_mode;    /* 0: id+iq, 1: id only, 2: iq only */
  uint8_t scale_idx;  /* main chart vertical range index */
  uint8_t can_shown;  /* 1: CAN page visible, 0: scope page */

  /* Frame-rate estimator (CAN page) */

  uint32_t fr_nframes;   /* nframes_501 at last rate update */
  uint32_t fr_tick;      /* lv_tick_get() at last rate update */
  uint32_t fr_hz;        /* smoothed frame rate */

  scope_sample_t last;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct scope_ui_s g_ui;

/* Language strings. Chinese labels are two characters so they fit the
 * 72px buttons; English ones keep the classic abbreviations. */

static const struct scope_lang_s g_lang[2] =
{
  [LANG_ZH] =
  {
    .title       = "FOC 示波器",
    .state_run   = "运行",
    .state_paused = "暂停",
    .lang_name   = "中文",
    .btn         = { "暂停", "放大", "缩小", "量程",
                     "清屏", "上一", "下一", "通道", "总线", "AI" },
    .can_title   = "CAN 总线",
    .can_back    = "返回",
    .can_link_ok = "链路正常",
    .can_link_lost = "链路断开",
    .can_frames  = "帧数",
    .can_rate    = "帧率",
    .can_bad     = "异常帧",
    .can_raw     = "502 原始",
    .can_hw      = "控制器",
    .can_no      = "无 CAN 数据源",
    .ai_title    = "AI 整定",
    .ai_back     = "返回",
    .ai_start    = "开始",
    .ai_idle     = "空闲",
    .ai_wait     = "等待参数",
    .ai_calc     = "计算中",
    .ai_ok       = "完成",
    .ai_fail     = "失败",
    .wifi_title  = "WiFi",
    .wifi_back   = "返回",
    .wifi_scan   = "扫描",
    .wifi_conn   = "连接",
    .wifi_disc   = "断开",
    .wifi_prev   = "<",
    .wifi_next   = ">",
    .wifi_idle   = "空闲",
    .wifi_scanning = "扫描中",
    .wifi_connecting = "连接中",
    .wifi_connected = "已连接",
    .wifi_error  = "错误",
    .wifi_none   = "无"
  },
  [LANG_EN] =
  {
    .title       = "FOC SCOPE",
    .state_run   = "RUN",
    .state_paused = "PAUSED",
    .lang_name   = "EN",
    .btn         = { "PAUSE", "ZOOM+", "ZOOM-", "SCALE",
                     "CLR", "<", ">", "CH", "CAN", "AI" },
    .can_title   = "CAN LINK",
    .can_back    = "BACK",
    .can_link_ok = "LINK OK",
    .can_link_lost = "LINK LOST",
    .can_frames  = "Frames",
    .can_rate    = "Rate",
    .can_bad     = "Bad",
    .can_raw     = "502 RAW",
    .can_hw      = "Controller",
    .can_no      = "No CAN source",
    .ai_title    = "AI TUNE",
    .ai_back     = "BACK",
    .ai_start    = "START",
    .ai_idle     = "IDLE",
    .ai_wait     = "WAIT",
    .ai_calc     = "CALC",
    .ai_ok       = "OK",
    .ai_fail     = "FAIL",
    .wifi_title  = "WiFi",
    .wifi_back   = "BACK",
    .wifi_scan   = "SCAN",
    .wifi_conn   = "CONN",
    .wifi_disc   = "DISC",
    .wifi_prev   = "<",
    .wifi_next   = ">",
    .wifi_idle   = "IDLE",
    .wifi_scanning = "SCANNING",
    .wifi_connecting = "CONNECTING",
    .wifi_connected = "CONNECTED",
    .wifi_error  = "ERR",
    .wifi_none   = "NONE"
  }
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Visible span in samples for the current zoom */

static uint32_t scope_span(void)
{
  return (SCOPE_POINT_CNT * 256 + g_ui.zoom / 2) / g_ui.zoom;
}

/* Apply the current zoom/pan window to every series of both charts.
 * Window end is anchored at the frozen cursor; pan moves it back. */

static void scope_apply_view(void)
{
  uint32_t span = scope_span();
  uint32_t maxpan = SCOPE_POINT_CNT - span;
  uint32_t pan = g_ui.pan;
  uint32_t start;

  if (pan > maxpan)
    {
      pan = maxpan;
      g_ui.pan = pan;
    }

  start = (g_ui.cursor + SCOPE_POINT_CNT - span - pan) % SCOPE_POINT_CNT;

  /* Per-series: every series of every chart sharing the time base */

  lv_chart_set_x_start_point(g_ui.chart_cur, g_ui.ser_id, start);
  lv_chart_set_x_start_point(g_ui.chart_cur, g_ui.ser_iq, start);
  lv_chart_set_x_start_point(g_ui.chart_rpm, g_ui.ser_rpm, start);
}

static void scope_set_zoom(uint32_t zoom)
{
  g_ui.zoom = zoom;
  lv_chart_set_zoom_x(g_ui.chart_cur, zoom);
  lv_chart_set_zoom_x(g_ui.chart_rpm, zoom);
}

/* Live indicator: visible time span (s) and main-chart vertical range.
 * Refreshed on every zoom/scale change so the adjustment is visible
 * while running. */

static void scope_refresh_view_label(void)
{
  uint32_t span = scope_span();
  float secs = (float)span / (float)SCOPE_SPS;

  lv_label_set_text_fmt(g_ui.lb_view, "%.1fs %.1fA",
                        (double)secs,
                        (double)(g_scale_main[g_ui.scale_idx][1] / 1000.0f));
}

/* Enable/disable the pause-only view buttons (pan and clear; zoom and
 * scale work in both states) */

static void scope_set_view_btns_enabled(bool en)
{
  lv_obj_t *btns[] =
  {
    g_ui.btn_left, g_ui.btn_right, g_ui.btn_clear
  };
  int i;

  for (i = 0; i < (int)(sizeof(btns) / sizeof(btns[0])); i++)
    {
      if (en)
        {
          lv_obj_clear_state(btns[i], LV_STATE_DISABLED);
        }
      else
        {
          lv_obj_add_state(btns[i], LV_STATE_DISABLED);
        }
    }
}

/* Re-text every label from the active language table. Cheap (8 tiny
 * labels), so it is called on every state change as well: the PAUSE
 * button label depends on state (暂停/PAUSE while running, 运行/RUN
 * while paused). */

static void scope_apply_lang(void)
{
  const struct scope_lang_s *l = &g_lang[g_ui.lang];
  int i;

  lv_label_set_text(g_ui.lb_title, l->title);
  lv_label_set_text(g_ui.lb_lang, l->lang_name);
  lv_label_set_text(g_ui.lb_state,
                    g_ui.state == SCOPE_STATE_RUN ?
                    l->state_run : l->state_paused);

  for (i = 0; i < 10; i++)
    {
      lv_label_set_text(g_ui.lab_btn[i], l->btn[i]);
    }

  /* The CAN / AI buttons double as BACK while their page is shown */

  lv_label_set_text(g_ui.lab_btn[ACT_CAN],
                    g_ui.can_shown ? l->can_back : l->btn[ACT_CAN]);
  lv_label_set_text(g_ui.lab_btn[ACT_AI],
                    g_ui.ai_shown ? l->ai_back : l->btn[ACT_AI]);
  lv_label_set_text(g_ui.lb_ai_state, l->ai_title);
  lv_label_set_text(g_ui.lb_ai_start, l->ai_start);

  /* WiFi top-bar button + page header */

  lv_label_set_text(g_ui.lb_wifi_btn,
                    g_ui.wifi_shown ? l->wifi_back : l->wifi_title);
  lv_label_set_text(g_ui.lb_wifi_state, l->wifi_title);
}

static void scope_set_state(scope_state_t state)
{
  g_ui.state = state;

  if (state == SCOPE_STATE_RUN)
    {
      /* Resume following the live cursor.  Keep the current zoom (the
       * user may have set it while running); only clear the pan - the
       * SHIFT window re-anchors itself at the newest samples.
       */

      g_ui.pan = 0;
      scope_refresh_view_label();
      lv_obj_set_style_text_color(g_ui.lb_state, CLR_RUN, 0);
    }
  else
    {
      /* Freeze the write cursor as the window anchor */

      g_ui.cursor = lv_chart_get_x_start_point(g_ui.chart_cur, g_ui.ser_id);
      g_ui.pan = 0;
      scope_apply_view();
      scope_refresh_view_label();
      lv_obj_set_style_text_color(g_ui.lb_state, CLR_PAUSED, 0);
    }

  scope_apply_lang();
  scope_set_view_btns_enabled(state == SCOPE_STATE_PAUSED);
}

/* --- button actions --- */

static void scope_act_zoom(int dir)
{
  int i;
  int idx = -1;

  for (i = 0; i < (int)ZOOM_TABLE_LEN; i++)
    {
      if (g_zoom_table[i] == g_ui.zoom)
        {
          idx = i;
          break;
        }
    }

  if (idx < 0)
    {
      idx = 0;
    }

  idx += dir;
  if (idx < 0 || idx >= (int)ZOOM_TABLE_LEN)
    {
      return;
    }

  scope_set_zoom(g_zoom_table[idx]);

  /* In RUN the SHIFT window follows the live cursor by itself; only the
   * paused view needs re-anchoring at the frozen cursor.
   */

  if (g_ui.state == SCOPE_STATE_PAUSED)
    {
      scope_apply_view();
    }

  scope_refresh_view_label();
}

static void scope_act_pan(int dir)
{
  uint32_t span = scope_span();
  uint32_t maxpan = SCOPE_POINT_CNT - span;
  uint32_t step = (maxpan + 7) / 8;

  if (step < 1)
    {
      step = 1;
    }

  if (dir > 0)
    {
      /* pan further back into history */

      if (g_ui.pan < maxpan)
        {
          g_ui.pan = g_ui.pan + step;
          if (g_ui.pan > maxpan)
            {
              g_ui.pan = maxpan;
            }

          scope_apply_view();
        }
    }
  else
    {
      /* pan back toward the cursor */

      if (g_ui.pan >= step)
        {
          g_ui.pan -= step;
        }
      else if (g_ui.pan > 0)
        {
          g_ui.pan = 0;
        }

      scope_apply_view();
    }
}

static void scope_act_clear(void)
{
  uint32_t i;

  for (i = 0; i < SCOPE_POINT_CNT; i++)
    {
      lv_chart_set_value_by_id(g_ui.chart_cur, g_ui.ser_id, i, LV_CHART_POINT_NONE);
      lv_chart_set_value_by_id(g_ui.chart_cur, g_ui.ser_iq, i, LV_CHART_POINT_NONE);
      lv_chart_set_value_by_id(g_ui.chart_rpm, g_ui.ser_rpm, i, LV_CHART_POINT_NONE);
    }
}

static void scope_act_channel(void)
{
  g_ui.ch_mode = (g_ui.ch_mode + 1) % 3;

  lv_chart_hide_series(g_ui.chart_cur, g_ui.ser_id,
                       (g_ui.ch_mode != 0 && g_ui.ch_mode != 1));
  lv_chart_hide_series(g_ui.chart_cur, g_ui.ser_iq,
                       (g_ui.ch_mode != 0 && g_ui.ch_mode != 2));
}

static void scope_act_scale(void)
{
  g_ui.scale_idx = (g_ui.scale_idx + 1) % SCALE_MAIN_LEN;

  lv_chart_set_range(g_ui.chart_cur, LV_CHART_AXIS_PRIMARY_Y,
                     g_scale_main[g_ui.scale_idx][0],
                     g_scale_main[g_ui.scale_idx][1]);
  lv_chart_set_range(g_ui.chart_rpm, LV_CHART_AXIS_PRIMARY_Y,
                     g_scale_rpm[g_ui.scale_idx][0],
                     g_scale_rpm[g_ui.scale_idx][1]);

  scope_refresh_view_label();
}

/* Forward declarations */

static void scope_btn_cb(lv_event_t *e);

/* --- CAN link-info page --- */

/* Build the CAN page container (hidden until ACT_CAN toggles it).
 * Covers the charts + readouts area; the row-2 buttons stay visible and
 * the CAN button doubles as BACK. */

static void scope_build_can_page(void)
{
  lv_obj_t *scr = lv_screen_active();

  g_ui.can_page = lv_obj_create(scr);
  lv_obj_set_pos(g_ui.can_page, SCOPE_CHART_X, SCOPE_CHART_Y);
  lv_obj_set_size(g_ui.can_page, SCOPE_CHART_W,
                  SCOPE_BTN_Y1 - SCOPE_CHART_Y - 4);
  lv_obj_set_style_bg_color(g_ui.can_page, CLR_CHART_BG, 0);
  lv_obj_set_style_border_color(g_ui.can_page, CLR_CHART_BD, 0);
  lv_obj_set_style_border_width(g_ui.can_page, 1, 0);
  lv_obj_set_style_radius(g_ui.can_page, 4, 0);
  lv_obj_set_style_pad_all(g_ui.can_page, 10, 0);
  lv_obj_remove_flag(g_ui.can_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_ui.can_page, LV_OBJ_FLAG_HIDDEN);

  /* Link-state banner (large, colored) */

  g_ui.lb_can_link = lv_label_create(g_ui.can_page);
  lv_obj_set_style_text_font(g_ui.lb_can_link, &font_puhui_20_4, 0);
  lv_label_set_text(g_ui.lb_can_link, "----");

  /* Frame counters line. Every label on this page uses the 20px PuHui
   * font so the Chinese labels render (the theme font has no CJK glyphs);
   * CJK at 20px is wide, so the counters/rate share one line and the
   * bad-frame counter gets its own. */

  g_ui.lb_can_frames = lv_label_create(g_ui.can_page);
  lv_obj_set_pos(g_ui.lb_can_frames, 0, 36);
  lv_obj_set_style_text_font(g_ui.lb_can_frames, &font_puhui_20_4, 0);
  lv_obj_set_style_text_color(g_ui.lb_can_frames, CLR_TEXT, 0);
  lv_label_set_text(g_ui.lb_can_frames, "-");

  g_ui.lb_can_bad = lv_label_create(g_ui.can_page);
  lv_obj_set_pos(g_ui.lb_can_bad, 0, 62);
  lv_obj_set_style_text_font(g_ui.lb_can_bad, &font_puhui_20_4, 0);
  lv_obj_set_style_text_color(g_ui.lb_can_bad, CLR_TEXT, 0);
  lv_label_set_text(g_ui.lb_can_bad, "-");

  /* Latest sample values (multi-line) */

  g_ui.lb_can_val = lv_label_create(g_ui.can_page);
  lv_obj_set_pos(g_ui.lb_can_val, 0, 92);
  lv_obj_set_size(g_ui.lb_can_val, 280, 96);
  lv_obj_set_style_text_font(g_ui.lb_can_val, &font_puhui_20_4, 0);
  lv_label_set_long_mode(g_ui.lb_can_val, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(g_ui.lb_can_val, CLR_TEXT, 0);
  lv_label_set_text(g_ui.lb_can_val, "-");

  /* Controller registers (multi-line, dim) */

  g_ui.lb_can_hw = lv_label_create(g_ui.can_page);
  lv_obj_set_pos(g_ui.lb_can_hw, 0, 194);
  lv_obj_set_size(g_ui.lb_can_hw, 280, 120);
  lv_obj_set_style_text_font(g_ui.lb_can_hw, &font_puhui_20_4, 0);
  lv_label_set_long_mode(g_ui.lb_can_hw, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(g_ui.lb_can_hw, CLR_DIM, 0);
  lv_label_set_text(g_ui.lb_can_hw, "-");
}

/* Refresh the CAN page contents. Called every tick while the page is
 * shown (cheap: a few label re-texts, only the page area redraws). */

static void scope_refresh_can_page(void)
{
  const struct scope_lang_s *l = &g_lang[g_ui.lang];
  scope_can_stats_t st;
  uint32_t now;
  uint32_t dt;
  uint32_t dframes;

  if (g_ui.ds->get_can_stats == NULL)
    {
      lv_label_set_text(g_ui.lb_can_link, l->can_no);
      return;
    }

  if (g_ui.ds->get_can_stats(g_ui.ds, &st) < 0)
    {
      return;
    }

  /* Link state: LOST once no frame for SCOPE_CAN_STALE_MS */

  if (st.last_age_ms >= 0 && st.last_age_ms < SCOPE_CAN_STALE_MS)
    {
      lv_obj_set_style_text_color(g_ui.lb_can_link, CLR_RUN, 0);
      lv_label_set_text_fmt(g_ui.lb_can_link, "%s  %lu",
                            l->can_link_ok,
                            (unsigned long)st.nframes_501);
    }
  else
    {
      lv_obj_set_style_text_color(g_ui.lb_can_link, CLR_PAUSED, 0);
      if (st.last_age_ms < 0)
        {
          lv_label_set_text(g_ui.lb_can_link, l->can_link_lost);
        }
      else
        {
          lv_label_set_text_fmt(g_ui.lb_can_link, "%s  %.1fs",
                                l->can_link_lost,
                                (double)(st.last_age_ms / 1000.0));
        }
    }

  /* Frame-rate estimate: 500 ms sliding window */

  now = lv_tick_get();
  dt = now - g_ui.fr_tick;
  if (dt >= 500)
    {
      dframes = st.nframes_501 - g_ui.fr_nframes;
      g_ui.fr_hz = (dframes * 1000) / dt;
      g_ui.fr_nframes = st.nframes_501;
      g_ui.fr_tick = now;
    }

  lv_label_set_text_fmt(g_ui.lb_can_frames, "%s %lu    %s %luHz",
                        l->can_frames, (unsigned long)st.nframes_501,
                        l->can_rate, (unsigned long)g_ui.fr_hz);

  lv_label_set_text_fmt(g_ui.lb_can_bad, "%s %lu    502 %lu",
                        l->can_bad, (unsigned long)st.nframes_bad,
                        (unsigned long)st.nframes_502);

  lv_label_set_text_fmt(g_ui.lb_can_val,
                        "id %+.2fA   iq %+.2fA\n"
                        "rpm %.0f   Vb %.1fV\n"
                        "Ib %.1fA\n"
                        "%s %02X %02X %02X %02X",
                        g_ui.last.id, g_ui.last.iq,
                        g_ui.last.speed, g_ui.last.vbus,
                        g_ui.last.ibus,
                        l->can_raw,
                        (unsigned int)(g_ui.last.ibus_raw & 0xffu),
                        (unsigned int)((g_ui.last.ibus_raw >> 8) & 0xffu),
                        (unsigned int)((g_ui.last.ibus_raw >> 16) & 0xffu),
                        (unsigned int)((g_ui.last.ibus_raw >> 24) & 0xffu));

  lv_label_set_text_fmt(g_ui.lb_can_hw,
                        "%s\n"
                        "ISR %lu  RX %lu\n"
                        "TX %lu  STA 0x%lx\n"
                        "ISRC 0x%lx  MSEL 0x%lx\n"
                        "BTIME 0x%lx",
                        l->can_hw,
                        (unsigned long)st.isr_cnt,
                        (unsigned long)st.rx_cnt,
                        (unsigned long)st.txdone_cnt,
                        (unsigned long)st.isr_last_status,
                        (unsigned long)st.isr_last_isrc,
                        (unsigned long)st.msel,
                        (unsigned long)st.btime);
}

/* Toggle between the scope page and the CAN link-info page */

static void scope_act_can(void)
{
  lv_obj_t *hide[] =
  {
    g_ui.chart_cur, g_ui.chart_rpm, g_ui.lb_view,
    g_ui.lb_id, g_ui.lb_iq, g_ui.lb_rpm, g_ui.lb_vbus,
    g_ui.btn_pause, g_ui.btn_zoom_in, g_ui.btn_zoom_out, g_ui.btn_scale,
    g_ui.btn_left, g_ui.btn_right, g_ui.btn_clear, g_ui.btn_ch
  };
  int i;

  /* Drop other overlay pages first (mutually exclusive) */

  if (g_ui.ai_shown)
    {
      g_ui.ai_shown = false;
      lv_obj_add_flag(g_ui.ai_page, LV_OBJ_FLAG_HIDDEN);
    }

  if (g_ui.wifi_shown)
    {
      g_ui.wifi_shown = false;
      lv_obj_add_flag(g_ui.wifi_page, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_ui.wifi_kb, LV_OBJ_FLAG_HIDDEN);
      g_ui.wifi_kb_open = 0;
    }

  g_ui.can_shown = !g_ui.can_shown;

  for (i = 0; i < (int)(sizeof(hide) / sizeof(hide[0])); i++)
    {
      if (g_ui.can_shown)
        {
          lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_clear_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (g_ui.can_shown)
    {
      lv_obj_clear_flag(g_ui.can_page, LV_OBJ_FLAG_HIDDEN);
      scope_refresh_can_page();
    }
  else
    {
      lv_obj_add_flag(g_ui.can_page, LV_OBJ_FLAG_HIDDEN);
    }

  scope_apply_lang();   /* CAN button label: 总线/CAN <-> 返回/BACK */
}

/* --- AI tune page --- */

/* Build the AI page container. Covers the charts + readouts area; row-2
 * buttons stay visible and the AI button doubles as BACK. */

/* Parse "Rs Ld Lq Ke poles" (whitespace separated) into a motor struct.
 * Returns 0 on success; rejects nonsense (<=0 mechanical values). */
static int scope_parse_motor_str(FAR const char *s, FAR motor_params_t *m)
{
  motor_params_t t;

  memset(&t, 0, sizeof(t));
  if (sscanf(s, "%f %f %f %f %d",
             &t.Rs, &t.Ld, &t.Lq, &t.Ke, &t.poles) != 5 ||
      t.Rs <= 0.0f || t.Ld <= 0.0f || t.Lq <= 0.0f || t.Ke <= 0.0f ||
      t.poles <= 0)
    {
      return -1;
    }

  *m = t;
  return 0;
}

static void scope_ai_ta_cb(lv_event_t *e)
{
  if (lv_event_get_code(e) == LV_EVENT_CLICKED ||
      lv_event_get_code(e) == LV_EVENT_FOCUSED)
    {
      lv_keyboard_set_textarea(g_ui.ai_kb, g_ui.ai_ta);
      lv_obj_clear_flag(g_ui.ai_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scope_ai_kb_cb(lv_event_t *e)
{
  if (lv_event_get_code(e) == LV_EVENT_READY)
    {
      strncpy(g_ui.ai_params, lv_textarea_get_text(g_ui.ai_ta),
              sizeof(g_ui.ai_params) - 1);
      g_ui.ai_params[sizeof(g_ui.ai_params) - 1] = '\0';
      lv_obj_add_flag(g_ui.ai_kb, LV_OBJ_FLAG_HIDDEN);
    }
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL)
    {
      lv_obj_add_flag(g_ui.ai_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scope_build_ai_page(void)
{
  lv_obj_t *scr = lv_screen_active();

  g_ui.ai_page = lv_obj_create(scr);
  lv_obj_set_pos(g_ui.ai_page, SCOPE_CHART_X, SCOPE_CHART_Y);
  lv_obj_set_size(g_ui.ai_page, SCOPE_CHART_W,
                  SCOPE_BTN_Y1 - SCOPE_CHART_Y - 4);
  lv_obj_set_style_bg_color(g_ui.ai_page, CLR_CHART_BG, 0);
  lv_obj_set_style_border_color(g_ui.ai_page, CLR_CHART_BD, 0);
  lv_obj_set_style_border_width(g_ui.ai_page, 1, 0);
  lv_obj_set_style_radius(g_ui.ai_page, 4, 0);
  lv_obj_set_style_pad_all(g_ui.ai_page, 10, 0);
  lv_obj_remove_flag(g_ui.ai_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_ui.ai_page, LV_OBJ_FLAG_HIDDEN);

  /* State banner (top, colored by state in refresh) */

  g_ui.lb_ai_state = lv_label_create(g_ui.ai_page);
  lv_obj_set_style_text_font(g_ui.lb_ai_state, &font_puhui_20_4, 0);
  lv_label_set_text(g_ui.lb_ai_state, "-");

  /* Motor params */

  g_ui.lb_ai_motor = lv_label_create(g_ui.ai_page);
  lv_obj_set_pos(g_ui.lb_ai_motor, 0, 30);
  lv_obj_set_style_text_font(g_ui.lb_ai_motor, &font_puhui_20_4, 0);
  lv_obj_set_style_text_color(g_ui.lb_ai_motor, CLR_TEXT, 0);
  lv_label_set_long_mode(g_ui.lb_ai_motor, LV_LABEL_LONG_WRAP);
  lv_obj_set_size(g_ui.lb_ai_motor, 290, 58);
  lv_label_set_text(g_ui.lb_ai_motor, "-");

  /* Result / error */

  g_ui.lb_ai_result = lv_label_create(g_ui.ai_page);
  lv_obj_set_pos(g_ui.lb_ai_result, 0, 92);
  lv_obj_set_style_text_font(g_ui.lb_ai_result, &font_puhui_20_4, 0);
  lv_obj_set_style_text_color(g_ui.lb_ai_result, CLR_TEXT, 0);
  lv_label_set_long_mode(g_ui.lb_ai_result, LV_LABEL_LONG_WRAP);
  lv_obj_set_size(g_ui.lb_ai_result, 290, 108);
  lv_label_set_text(g_ui.lb_ai_result, "-");

  /* Manual start button (bottom of the page) */

  g_ui.btn_ai_start = lv_button_create(g_ui.ai_page);
  lv_obj_align(g_ui.btn_ai_start, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
  lv_obj_set_size(g_ui.btn_ai_start, 88, 40);
  lv_obj_add_event_cb(g_ui.btn_ai_start, scope_btn_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)ACT_AI_START);
  lv_obj_set_style_bg_color(g_ui.btn_ai_start, CLR_BTN_BG, 0);
  lv_obj_set_style_bg_color(g_ui.btn_ai_start, CLR_BTN_BG_PRS,
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_color(g_ui.btn_ai_start, CLR_BTN_BD, 0);
  lv_obj_set_style_border_width(g_ui.btn_ai_start, 1, 0);
  lv_obj_set_style_radius(g_ui.btn_ai_start, 4, 0);
  lv_obj_set_style_shadow_width(g_ui.btn_ai_start, 0, 0);
  lv_obj_set_style_pad_all(g_ui.btn_ai_start, 0, 0);
  lv_obj_set_style_text_color(g_ui.btn_ai_start, CLR_TEXT, 0);

  g_ui.lb_ai_start = lv_label_create(g_ui.btn_ai_start);
  lv_obj_set_style_text_font(g_ui.lb_ai_start, &font_puhui_20_4, 0);
  lv_obj_center(g_ui.lb_ai_start);

  /* Source switch button (bottom-left): CAN <-> MANUAL */

  g_ui.btn_ai_src = lv_button_create(g_ui.ai_page);
  lv_obj_align(g_ui.btn_ai_src, LV_ALIGN_BOTTOM_LEFT, 10, -8);
  lv_obj_set_size(g_ui.btn_ai_src, 84, 40);
  lv_obj_add_event_cb(g_ui.btn_ai_src, scope_btn_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)ACT_AI_SRC);
  lv_obj_set_style_bg_color(g_ui.btn_ai_src, CLR_BTN_BG, 0);
  lv_obj_set_style_bg_color(g_ui.btn_ai_src, CLR_BTN_BG_PRS,
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_color(g_ui.btn_ai_src, CLR_BTN_BD, 0);
  lv_obj_set_style_border_width(g_ui.btn_ai_src, 1, 0);
  lv_obj_set_style_radius(g_ui.btn_ai_src, 4, 0);
  lv_obj_set_style_shadow_width(g_ui.btn_ai_src, 0, 0);
  lv_obj_set_style_pad_all(g_ui.btn_ai_src, 0, 0);
  lv_obj_set_style_text_color(g_ui.btn_ai_src, CLR_TEXT, 0);

  g_ui.lb_ai_src = lv_label_create(g_ui.btn_ai_src);
  lv_obj_set_style_text_font(g_ui.lb_ai_src, &font_puhui_20_4, 0);
  lv_obj_center(g_ui.lb_ai_src);
  lv_label_set_text(g_ui.lb_ai_src, "SRC");

  /* Manual motor params entry (shown only in MANUAL mode) */

  g_ui.lb_ai_pm = lv_label_create(g_ui.ai_page);
  lv_obj_set_pos(g_ui.lb_ai_pm, 0, 206);
  lv_obj_set_style_text_font(g_ui.lb_ai_pm, &font_puhui_20_4, 0);
  lv_obj_set_style_text_color(g_ui.lb_ai_pm, CLR_DIM, 0);
  lv_label_set_text(g_ui.lb_ai_pm, "MOTOR  Rs Ld Lq Ke poles");

  g_ui.ai_ta = lv_textarea_create(g_ui.ai_page);
  lv_obj_set_pos(g_ui.ai_ta, 0, 226);
  lv_obj_set_size(g_ui.ai_ta, 230, 32);
  lv_obj_add_event_cb(g_ui.ai_ta, scope_ai_ta_cb, LV_EVENT_ALL, NULL);
  lv_textarea_set_one_line(g_ui.ai_ta, true);
  lv_textarea_set_text(g_ui.ai_ta, g_ui.ai_params);
  lv_obj_set_style_text_color(g_ui.ai_ta, CLR_TEXT, 0);

  /* AI-page keyboard (separate overlay from the WiFi one) */

  g_ui.ai_kb = lv_keyboard_create(scr);
  lv_obj_align(g_ui.ai_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(g_ui.ai_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(g_ui.ai_kb, scope_ai_kb_cb, LV_EVENT_ALL, NULL);
  lv_keyboard_set_textarea(g_ui.ai_kb, g_ui.ai_ta);
}

/* Refresh the AI page contents (poll the background tuner). Called every
 * tick while shown; the AI HTTP call happens in the tuner thread, so this
 * only reads shared state - fast and non-blocking. */

static void scope_refresh_ai_page(void)
{
  const struct scope_lang_s *l = &g_lang[g_ui.lang];
  motor_params_t m;
  pi_params_t pi;
  int state = ai_tuner_ui_state();

  lv_label_set_text(g_ui.lb_ai_state, l->ai_title);

  switch (state)
    {
      case AI_TUNER_IDLE:
        lv_label_set_text(g_ui.lb_ai_start, l->ai_start);
        lv_obj_set_style_text_color(g_ui.lb_ai_state, CLR_DIM, 0);
        lv_label_set_text_fmt(g_ui.lb_ai_state, "%s: %s",
                              l->ai_title, l->ai_idle);
        break;

      case AI_TUNER_CALC:
        lv_obj_set_style_text_color(g_ui.lb_ai_state, CLR_RPM, 0);
        lv_label_set_text_fmt(g_ui.lb_ai_state, "%s: %s...",
                              l->ai_title, l->ai_calc);
        break;

      case AI_TUNER_OK:
        lv_obj_set_style_text_color(g_ui.lb_ai_state, CLR_RUN, 0);
        lv_label_set_text_fmt(g_ui.lb_ai_state, "%s: %s",
                              l->ai_title, l->ai_ok);
        break;

      case AI_TUNER_FAIL:
      default:
        lv_obj_set_style_text_color(g_ui.lb_ai_state, CLR_PAUSED, 0);
        lv_label_set_text_fmt(g_ui.lb_ai_state, "%s: %s",
                              l->ai_title, l->ai_fail);
        break;
    }

  /* Source button label + show/hide the manual entry */

  lv_label_set_text(g_ui.lb_ai_src, g_ui.ai_src ? "CAN" : "MANUAL");

  if (g_ui.ai_src)
    {                       /* CAN: params come from the STM32 */
      lv_obj_add_flag(g_ui.ai_ta, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_ui.lb_ai_pm, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {                       /* MANUAL: entry box visible */
      lv_obj_clear_flag(g_ui.ai_ta, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(g_ui.lb_ai_pm, LV_OBJ_FLAG_HIDDEN);
    }

  /* Motor params: the manual set when in MANUAL mode, else the last
   * received CAN set.  Also the manual-trigger source. */

  if (g_ui.ai_src)
    {
      ai_tuner_ui_get_motor(&m);
    }
  else
    {
      m = g_ui.ai_manual;
    }

  g_ui.ai_motor = m;
  lv_label_set_text_fmt(g_ui.lb_ai_motor,
                        "Rs %.3f  Ld %.4f\n"
                        "Lq %.4f  Ke %.4f\n"
                        "poles %d",
                        m.Rs, m.Ld, m.Lq, m.Ke, m.poles);

  /* Result or error */

  if (ai_tuner_ui_get_result(&pi))
    {
      lv_obj_set_style_text_color(g_ui.lb_ai_result, CLR_TEXT, 0);
      lv_label_set_text_fmt(g_ui.lb_ai_result,
                            "Kp_Id %.4f  Ki_Id %.2f\n"
                            "Kp_Iq %.4f  Ki_Iq %.2f\n"
                            "KpSpd %.4f  KiSpd %.2f",
                            pi.Kp_Id, pi.Ki_Id,
                            pi.Kp_Iq, pi.Ki_Iq,
                            pi.Kp_Speed, pi.Ki_Speed);
    }
  else if (state == AI_TUNER_FAIL)
    {
      lv_obj_set_style_text_color(g_ui.lb_ai_result, CLR_PAUSED, 0);
      lv_label_set_text_fmt(g_ui.lb_ai_result, "err %d",
                            ai_tuner_ui_last_error());
    }
  else
    {
      lv_label_set_text(g_ui.lb_ai_result, "-");
    }
}

/* Toggle between the scope page and the AI page */

static void scope_act_ai(void)
{
  lv_obj_t *hide[] =
  {
    g_ui.chart_cur, g_ui.chart_rpm, g_ui.lb_view,
    g_ui.lb_id, g_ui.lb_iq, g_ui.lb_rpm, g_ui.lb_vbus,
    g_ui.btn_pause, g_ui.btn_zoom_in, g_ui.btn_zoom_out, g_ui.btn_scale,
    g_ui.btn_left, g_ui.btn_right, g_ui.btn_clear, g_ui.btn_ch
  };
  int i;

  /* Drop other overlay pages first (mutually exclusive) */

  if (g_ui.can_shown)
    {
      g_ui.can_shown = false;
      lv_obj_add_flag(g_ui.can_page, LV_OBJ_FLAG_HIDDEN);
    }

  if (g_ui.wifi_shown)
    {
      g_ui.wifi_shown = false;
      lv_obj_add_flag(g_ui.wifi_page, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_ui.wifi_kb, LV_OBJ_FLAG_HIDDEN);
      g_ui.wifi_kb_open = 0;
    }

  g_ui.ai_shown = !g_ui.ai_shown;

  for (i = 0; i < (int)(sizeof(hide) / sizeof(hide[0])); i++)
    {
      if (g_ui.ai_shown)
        {
          lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_clear_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (g_ui.ai_shown)
    {
      lv_obj_clear_flag(g_ui.ai_page, LV_OBJ_FLAG_HIDDEN);
      scope_refresh_ai_page();
    }
  else
    {
      lv_obj_add_flag(g_ui.ai_page, LV_OBJ_FLAG_HIDDEN);
    }

  scope_apply_lang();   /* AI button label: AI <-> 返回/BACK */
}

/* --- WiFi page --- */

/* WiFi-page button: parent-relative, y=252, 50x40 */
static lv_obj_t *scope_wifi_btn(int idx, const char *txt, int act)
{
  lv_obj_t *btn = lv_button_create(g_ui.wifi_page);
  lv_obj_t *lab;
  int x = 8 + idx * 60;

  lv_obj_set_pos(btn, x, 252);
  lv_obj_set_size(btn, 56, 40);
  lv_obj_add_event_cb(btn, scope_btn_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)act);

  lv_obj_set_style_bg_color(btn, CLR_BTN_BG, 0);
  lv_obj_set_style_bg_color(btn, CLR_BTN_BG_PRS, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(btn, CLR_BTN_BD, 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 4, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_set_style_text_color(btn, CLR_TEXT, 0);

  lab = lv_label_create(btn);
  lv_label_set_text(lab, txt);
  lv_obj_set_style_text_font(lab, &font_puhui_20_4, 0);
  lv_obj_center(lab);

  return btn;
}

static void scope_wifi_kb_cb(lv_event_t *e)
{
  uint32_t code = lv_event_get_code(e);

  if (code == LV_EVENT_READY)
    {
      /* OK key: store whatever the active textarea holds into the matching
       * field, close the keyboard, and auto-connect once a password has
       * been entered for a non-empty SSID. */

      if (g_ui.ta_active == g_ui.wifi_ta_ssid)
        {
          strncpy(g_ui.wifi_ssid,
                  lv_textarea_get_text(g_ui.wifi_ta_ssid),
                  sizeof(g_ui.wifi_ssid) - 1);
          g_ui.wifi_ssid[sizeof(g_ui.wifi_ssid) - 1] = '\0';
        }
      else if (g_ui.ta_active == g_ui.wifi_ta)
        {
          strncpy(g_ui.wifi_password,
                  lv_textarea_get_text(g_ui.wifi_ta),
                  sizeof(g_ui.wifi_password) - 1);
          g_ui.wifi_password[sizeof(g_ui.wifi_password) - 1] = '\0';
        }

      lv_obj_add_flag(g_ui.wifi_kb, LV_OBJ_FLAG_HIDDEN);
      g_ui.wifi_kb_open = 0;

      if (g_ui.ta_active == g_ui.wifi_ta && g_ui.wifi_ssid[0])
        {
          wifi_ui_connect(g_ui.wifi_ssid, g_ui.wifi_password);
        }
    }
  else if (code == LV_EVENT_CANCEL)
    {
      /* Cancel key: just close the keyboard */

      lv_obj_add_flag(g_ui.wifi_kb, LV_OBJ_FLAG_HIDDEN);
      g_ui.wifi_kb_open = 0;
    }
}

static void scope_wifi_ta_cb(lv_event_t *e)
{
  uint32_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target(e);

  /* Both textareas share one keyboard: remember which one is active and
   * point the keyboard at it. */

  if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED)
    {
      g_ui.ta_active = ta;
      lv_keyboard_set_textarea(g_ui.wifi_kb, ta);

      if (!g_ui.wifi_kb_open)
        {
          lv_obj_clear_flag(g_ui.wifi_kb, LV_OBJ_FLAG_HIDDEN);
          g_ui.wifi_kb_open = 1;
        }
    }
}

static void scope_build_wifi_page(void)
{
  lv_obj_t *scr = lv_screen_active();
  const struct scope_lang_s *l = &g_lang[g_ui.lang];

  g_ui.wifi_page = lv_obj_create(scr);
  lv_obj_set_pos(g_ui.wifi_page, SCOPE_CHART_X, SCOPE_CHART_Y);
  lv_obj_set_size(g_ui.wifi_page, SCOPE_CHART_W,
                  SCOPE_BTN_Y1 - SCOPE_CHART_Y - 4);
  lv_obj_set_style_bg_color(g_ui.wifi_page, CLR_CHART_BG, 0);
  lv_obj_set_style_border_color(g_ui.wifi_page, CLR_CHART_BD, 0);
  lv_obj_set_style_border_width(g_ui.wifi_page, 1, 0);
  lv_obj_set_style_radius(g_ui.wifi_page, 4, 0);
  lv_obj_set_style_pad_all(g_ui.wifi_page, 10, 0);
  lv_obj_remove_flag(g_ui.wifi_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_ui.wifi_page, LV_OBJ_FLAG_HIDDEN);

  /* State banner */

  g_ui.lb_wifi_state = lv_label_create(g_ui.wifi_page);
  lv_obj_set_style_text_font(g_ui.lb_wifi_state, &font_puhui_20_4, 0);
  lv_label_set_text(g_ui.lb_wifi_state, "-");

  /* Current connection line */

  g_ui.lb_wifi_sel = lv_label_create(g_ui.wifi_page);
  lv_obj_set_pos(g_ui.lb_wifi_sel, 0, 28);
  lv_obj_set_style_text_font(g_ui.lb_wifi_sel, &font_puhui_20_4, 0);
  lv_obj_set_style_text_color(g_ui.lb_wifi_sel, CLR_RPM, 0);
  lv_label_set_text(g_ui.lb_wifi_sel, "-");

  /* SSID caption + textarea */

  g_ui.lb_wifi_sid = lv_label_create(g_ui.wifi_page);
  lv_obj_set_pos(g_ui.lb_wifi_sid, 8, 58);
  lv_obj_set_style_text_font(g_ui.lb_wifi_sid, &font_puhui_20_4, 0);
  lv_obj_set_style_text_color(g_ui.lb_wifi_sid, CLR_DIM, 0);
  lv_label_set_text(g_ui.lb_wifi_sid, "SSID");

  g_ui.wifi_ta_ssid = lv_textarea_create(g_ui.wifi_page);
  lv_obj_set_pos(g_ui.wifi_ta_ssid, 64, 54);
  lv_obj_set_size(g_ui.wifi_ta_ssid, 230, 32);
  lv_obj_add_event_cb(g_ui.wifi_ta_ssid, scope_wifi_ta_cb,
                      LV_EVENT_ALL, NULL);
  lv_textarea_set_text(g_ui.wifi_ta_ssid, g_ui.wifi_ssid);
  lv_textarea_set_one_line(g_ui.wifi_ta_ssid, true);
  lv_obj_set_style_text_color(g_ui.wifi_ta_ssid, CLR_TEXT, 0);

  /* Password caption + textarea */

  g_ui.lb_wifi_pwd = lv_label_create(g_ui.wifi_page);
  lv_obj_set_pos(g_ui.lb_wifi_pwd, 8, 98);
  lv_obj_set_style_text_font(g_ui.lb_wifi_pwd, &font_puhui_20_4, 0);
  lv_obj_set_style_text_color(g_ui.lb_wifi_pwd, CLR_DIM, 0);
  lv_label_set_text(g_ui.lb_wifi_pwd, "pwd");

  g_ui.wifi_ta = lv_textarea_create(g_ui.wifi_page);
  lv_obj_set_pos(g_ui.wifi_ta, 64, 94);
  lv_obj_set_size(g_ui.wifi_ta, 230, 32);
  lv_obj_add_event_cb(g_ui.wifi_ta, scope_wifi_ta_cb, LV_EVENT_ALL, NULL);
  lv_textarea_set_text(g_ui.wifi_ta, g_ui.wifi_password);
  lv_textarea_set_password_mode(g_ui.wifi_ta, true);
  lv_textarea_set_one_line(g_ui.wifi_ta, true);
  lv_obj_set_style_text_color(g_ui.wifi_ta, CLR_TEXT, 0);

  /* Control buttons: CONN DISC */

  g_ui.wifi_btn[0] = scope_wifi_btn(0, l->wifi_conn, ACT_WIFI_CONN);
  g_ui.wifi_btn[1] = scope_wifi_btn(1, l->wifi_disc, ACT_WIFI_DISC);

  /* Virtual keyboard: screen-level overlay, starts hidden */

  g_ui.wifi_kb = lv_keyboard_create(scr);
  lv_obj_align(g_ui.wifi_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(g_ui.wifi_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(g_ui.wifi_kb, scope_wifi_kb_cb, LV_EVENT_ALL, NULL);
  lv_keyboard_set_textarea(g_ui.wifi_kb, g_ui.wifi_ta);
}

static void scope_refresh_wifi_page(void)
{
  const struct scope_lang_s *l = &g_lang[g_ui.lang];
  int state = wifi_ui_state();
  FAR const char *conn;

  /* Keep control-button labels in sync with the active language */

  lv_label_set_text(lv_obj_get_child(g_ui.wifi_btn[0], 0), l->wifi_conn);
  lv_label_set_text(lv_obj_get_child(g_ui.wifi_btn[1], 0), l->wifi_disc);

  /* State banner */

  switch (state)
    {
      case WIFI_UI_STATE_CONNECTING:
        lv_obj_set_style_text_color(g_ui.lb_wifi_state, CLR_IBUS, 0);
        lv_label_set_text_fmt(g_ui.lb_wifi_state, "%s: %s %s",
                              l->wifi_title, l->wifi_connecting,
                              g_ui.wifi_ssid);
        break;

      case WIFI_UI_STATE_CONNECTED:
        lv_obj_set_style_text_color(g_ui.lb_wifi_state, CLR_RUN, 0);
        conn = wifi_ui_connected_ssid();
        lv_label_set_text_fmt(g_ui.lb_wifi_state, "%s: %s %s",
                              l->wifi_title, l->wifi_connected,
                              conn[0] ? conn : g_ui.wifi_ssid);
        break;

      case WIFI_UI_STATE_ERROR:
        lv_obj_set_style_text_color(g_ui.lb_wifi_state, CLR_PAUSED, 0);
        lv_label_set_text_fmt(g_ui.lb_wifi_state, "%s: %s",
                              l->wifi_title, l->wifi_error);
        break;

      case WIFI_UI_STATE_IDLE:
      default:
        lv_obj_set_style_text_color(g_ui.lb_wifi_state, CLR_DIM, 0);
        lv_label_set_text_fmt(g_ui.lb_wifi_state, "%s: %s",
                              l->wifi_title, l->wifi_idle);
        break;
    }

  /* Connection line: SSID + IP when connected, otherwise the target */

  if (state == WIFI_UI_STATE_CONNECTED)
    {
      FAR const char *ip = wifi_ui_connected_ip();

      lv_label_set_text_fmt(g_ui.lb_wifi_sel, "%s  IP: %s",
                            wifi_ui_connected_ssid(),
                            ip[0] ? ip : "--");
    }
  else
    {
      lv_label_set_text_fmt(g_ui.lb_wifi_sel, "to: %s",
                            g_ui.wifi_ssid[0] ? g_ui.wifi_ssid : "-");
    }
}

static void scope_act_wifi(void)
{
  lv_obj_t *hide[] =
  {
    g_ui.chart_cur, g_ui.chart_rpm, g_ui.lb_view,
    g_ui.lb_id, g_ui.lb_iq, g_ui.lb_rpm, g_ui.lb_vbus,
    g_ui.btn_pause, g_ui.btn_zoom_in, g_ui.btn_zoom_out, g_ui.btn_scale,
    g_ui.btn_left, g_ui.btn_right, g_ui.btn_clear, g_ui.btn_ch
  };
  int i;

  /* Drop the other overlay pages first (mutually exclusive) */

  if (g_ui.can_shown)
    {
      g_ui.can_shown = false;
      lv_obj_add_flag(g_ui.can_page, LV_OBJ_FLAG_HIDDEN);
    }

  if (g_ui.ai_shown)
    {
      g_ui.ai_shown = false;
      lv_obj_add_flag(g_ui.ai_page, LV_OBJ_FLAG_HIDDEN);
    }

  g_ui.wifi_shown = !g_ui.wifi_shown;

  for (i = 0; i < (int)(sizeof(hide) / sizeof(hide[0])); i++)
    {
      if (g_ui.wifi_shown)
        {
          lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_clear_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (g_ui.wifi_shown)
    {
      lv_obj_clear_flag(g_ui.wifi_page, LV_OBJ_FLAG_HIDDEN);
      scope_refresh_wifi_page();
    }
  else
    {
      lv_obj_add_flag(g_ui.wifi_page, LV_OBJ_FLAG_HIDDEN);

      /* Close the keyboard when leaving the WiFi page */

      lv_obj_add_flag(g_ui.wifi_kb, LV_OBJ_FLAG_HIDDEN);
      g_ui.wifi_kb_open = 0;
    }

  scope_apply_lang();   /* WiFi top-bar button: WiFi <-> 返回/BACK */
}

static void scope_btn_cb(lv_event_t *e)
{
  int act = (int)(intptr_t)lv_event_get_user_data(e);

  switch (act)
    {
      case ACT_PAUSE:
        scope_set_state(g_ui.state == SCOPE_STATE_RUN ?
                        SCOPE_STATE_PAUSED : SCOPE_STATE_RUN);
        break;

      case ACT_ZOOM_IN:
        scope_act_zoom(1);
        break;

      case ACT_ZOOM_OUT:
        scope_act_zoom(-1);
        break;

      case ACT_SCALE:
        scope_act_scale();
        break;

      case ACT_CLEAR:
        scope_act_clear();
        break;

      case ACT_LEFT:
        scope_act_pan(1);
        break;

      case ACT_RIGHT:
        scope_act_pan(-1);
        break;

      case ACT_CH:
        scope_act_channel();
        break;

      case ACT_CAN:
        scope_act_can();
        break;

      case ACT_AI:
        scope_act_ai();
        break;

      case ACT_AI_START:
        if (g_ui.ai_src == 0)
          {
            /* MANUAL: parse the typed params, then trigger with them */
            motor_params_t mm;

            memset(&mm, 0, sizeof(mm));
            if (scope_parse_motor_str(g_ui.ai_params, &mm) == 0)
              {
                g_ui.ai_manual = mm;
                ai_tuner_ui_trigger(&mm, FOC_USAGE_ROBOT);
              }
            else
              {
                lv_label_set_text(g_ui.lb_ai_result,
                                  "AI: fill Rs Ld Lq Ke poles");
              }
          }
        else
          {
            /* CAN: ask the STM32 for fresh motor params (0x301).  It
             * replies with 0x101..0x104, which auto-triggers the tune. */
            ai_tuner_ui_request_params(FOC_USAGE_ROBOT);
          }

        scope_refresh_ai_page();
        break;

      case ACT_AI_SRC:
        g_ui.ai_src = !g_ui.ai_src;
        scope_refresh_ai_page();
        break;

      case ACT_WIFI:
        scope_act_wifi();
        break;

      case ACT_WIFI_CONN:
        /* Connect using the SSID + password typed into the two textareas.
         * (No AP list: this firmware's un-targeted active scan is broken,
         * so the UI takes manual SSID entry instead.) */

        if (!g_ui.wifi_kb_open)
          {
            strncpy(g_ui.wifi_ssid,
                    lv_textarea_get_text(g_ui.wifi_ta_ssid),
                    sizeof(g_ui.wifi_ssid) - 1);
            g_ui.wifi_ssid[sizeof(g_ui.wifi_ssid) - 1] = '\0';
            strncpy(g_ui.wifi_password,
                    lv_textarea_get_text(g_ui.wifi_ta),
                    sizeof(g_ui.wifi_password) - 1);
            g_ui.wifi_password[sizeof(g_ui.wifi_password) - 1] = '\0';

            if (g_ui.wifi_ssid[0])
              {
                wifi_ui_connect(g_ui.wifi_ssid, g_ui.wifi_password);
              }
          }
        break;

      case ACT_WIFI_DISC:
        wifi_ui_disconnect();
        break;

      case ACT_LANG:
        g_ui.lang = (g_ui.lang == LANG_ZH) ? LANG_EN : LANG_ZH;
        scope_apply_lang();
        break;

      default:
        break;
    }
}

/* --- data pump --- */

static void scope_push_sample(FAR scope_sample_t *s)
{
  lv_chart_set_next_value(g_ui.chart_cur, g_ui.ser_id,
                          (int32_t)(s->id * 1000.0f));
  lv_chart_set_next_value(g_ui.chart_cur, g_ui.ser_iq,
                          (int32_t)(s->iq * 1000.0f));
  lv_chart_set_next_value(g_ui.chart_rpm, g_ui.ser_rpm,
                          (int32_t)s->speed);
}

static void scope_timer_cb(lv_timer_t *timer)
{
  scope_sample_t s;
  int k;

  if (g_ui.state == SCOPE_STATE_RUN)
    {
      for (k = 0; k < SCOPE_SAMPLES_PER_TICK; k++)
        {
          if (g_ui.ds->get_sample(g_ui.ds, &s) < 0)
            {
              break;
            }

          scope_push_sample(&s);
          g_ui.last = s;
        }
    }

  /* Readouts refresh every tick (cheap: only the label area redraws) */

  lv_label_set_text_fmt(g_ui.lb_id,   "id%+.2f",  g_ui.last.id);
  lv_label_set_text_fmt(g_ui.lb_iq,   "iq%+.2f",  g_ui.last.iq);
  lv_label_set_text_fmt(g_ui.lb_rpm,  "rpm%.0f",  g_ui.last.speed);
  lv_label_set_text_fmt(g_ui.lb_vbus, "Vb %.1fV", g_ui.last.vbus);
  lv_label_set_text_fmt(g_ui.lb_ibus, "Ib %.1fA", g_ui.last.ibus);

  /* CAN / AI page refresh (only while shown) */

  if (g_ui.can_shown)
    {
      scope_refresh_can_page();
    }

  if (g_ui.ai_shown)
    {
      scope_refresh_ai_page();
    }

  if (g_ui.wifi_shown)
    {
      scope_refresh_wifi_page();
    }
}

/* --- widget construction --- */

static lv_obj_t *scope_make_btn(lv_obj_t *parent, int idx, int row,
                                const char *txt, int act, lv_coord_t w)
{
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_t *lab;

  lv_obj_set_pos(btn, row == 0 ? SCOPE_BTN_X(idx) : (8 + idx * 52),
                 row == 0 ? SCOPE_BTN_Y1 : SCOPE_BTN_Y2);
  lv_obj_set_size(btn, w, SCOPE_BTN_H);
  lv_obj_add_event_cb(btn, scope_btn_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)act);

  lv_obj_set_style_bg_color(btn, CLR_BTN_BG, 0);
  lv_obj_set_style_bg_color(btn, CLR_BTN_BG_PRS, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(btn, CLR_BTN_BG_DIS, LV_STATE_DISABLED);
  lv_obj_set_style_border_color(btn, CLR_BTN_BD, 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 4, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_set_style_text_color(btn, CLR_TEXT, 0);
  lv_obj_set_style_text_color(btn, CLR_BTN_TXT_DIS, LV_STATE_DISABLED);

  lab = lv_label_create(btn);
  lv_label_set_text(lab, txt);
  lv_obj_set_style_text_font(lab, &font_puhui_20_4, 0);
  lv_obj_center(lab);

  return btn;
}

/* Common dark chart styling; caller adds series and axis specifics */

static void scope_style_chart(lv_obj_t *chart, lv_color_t grid)
{
  lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(chart, CLR_CHART_BG, LV_PART_MAIN);
  lv_obj_set_style_border_color(chart, CLR_CHART_BD, LV_PART_MAIN);
  lv_obj_set_style_border_width(chart, 1, LV_PART_MAIN);
  lv_obj_set_style_line_color(chart, grid, LV_PART_MAIN);
  lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
  lv_obj_set_style_text_color(chart, CLR_DIM, LV_PART_MAIN);
  lv_obj_set_style_line_width(chart, 1, LV_PART_ITEMS);
  lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
}

/* Chart with y-axis tick labels (integer values only in v9) */

static void scope_y_ticks(lv_obj_t *chart, uint8_t major_cnt,
                          lv_coord_t draw_size)
{
  lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 4, 1,
                         major_cnt, 0, true, draw_size);
  lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_X, 0, 0,
                         0, 0, false, 0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void scope_ui_create(FAR scope_ds_t *ds)
{
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *unit;
  scope_sample_t s;
  int k;

  memset(&g_ui, 0, sizeof(g_ui));
  g_ui.ds = ds;
  g_ui.state = SCOPE_STATE_RUN;
  g_ui.zoom = 256;
  g_ui.scale_idx = 0;

  lv_obj_set_style_bg_color(scr, CLR_BG, 0);

  /* Title bar: title left; [language toggle][state] right. */

  g_ui.lb_title = lv_label_create(scr);
  lv_obj_set_pos(g_ui.lb_title, 8, 6);
  lv_obj_set_style_text_font(g_ui.lb_title, &font_puhui_20_4, 0);
  lv_obj_set_style_text_color(g_ui.lb_title, CLR_TEXT, 0);

  g_ui.btn_lang = lv_button_create(scr);
  lv_obj_align(g_ui.btn_lang, LV_ALIGN_TOP_RIGHT, -8, 6);
  lv_obj_set_size(g_ui.btn_lang, 48, 24);
  lv_obj_add_event_cb(g_ui.btn_lang, scope_btn_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)ACT_LANG);
  lv_obj_set_style_bg_color(g_ui.btn_lang, CLR_BTN_BG, 0);
  lv_obj_set_style_bg_color(g_ui.btn_lang, CLR_BTN_BG_PRS, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(g_ui.btn_lang, CLR_BTN_BD, 0);
  lv_obj_set_style_border_width(g_ui.btn_lang, 1, 0);
  lv_obj_set_style_radius(g_ui.btn_lang, 4, 0);
  lv_obj_set_style_shadow_width(g_ui.btn_lang, 0, 0);
  lv_obj_set_style_pad_all(g_ui.btn_lang, 0, 0);
  lv_obj_set_style_text_color(g_ui.btn_lang, CLR_TEXT, 0);

  g_ui.lb_lang = lv_label_create(g_ui.btn_lang);
  lv_obj_set_style_text_font(g_ui.lb_lang, &font_puhui_20_4, 0);
  lv_obj_center(g_ui.lb_lang);

  g_ui.lb_state = lv_label_create(scr);
  lv_obj_set_style_text_font(g_ui.lb_state, &font_puhui_20_4, 0);
  lv_obj_align(g_ui.lb_state, LV_ALIGN_TOP_RIGHT, -62, 6);
  lv_obj_set_style_text_color(g_ui.lb_state, CLR_RUN, 0);

  /* Main chart: id/iq, mA */

  g_ui.chart_cur = lv_chart_create(scr);
  lv_obj_set_pos(g_ui.chart_cur, SCOPE_CHART_X, SCOPE_CHART_Y);
  lv_obj_set_size(g_ui.chart_cur, SCOPE_CHART_W, SCOPE_CHART_H);
  lv_chart_set_type(g_ui.chart_cur, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(g_ui.chart_cur, SCOPE_POINT_CNT);
  lv_chart_set_update_mode(g_ui.chart_cur, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_range(g_ui.chart_cur, LV_CHART_AXIS_PRIMARY_Y,
                     g_scale_main[0][0], g_scale_main[0][1]);
  lv_chart_set_div_line_count(g_ui.chart_cur, 4, 6);
  scope_style_chart(g_ui.chart_cur, CLR_GRID);
  scope_y_ticks(g_ui.chart_cur, 4, 40);
  lv_obj_set_style_pad_left(g_ui.chart_cur, 48, LV_PART_MAIN);
  lv_obj_set_style_pad_right(g_ui.chart_cur, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_top(g_ui.chart_cur, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(g_ui.chart_cur, 8, LV_PART_MAIN);

  g_ui.ser_id = lv_chart_add_series(g_ui.chart_cur, CLR_ID,
                                    LV_CHART_AXIS_PRIMARY_Y);
  g_ui.ser_iq = lv_chart_add_series(g_ui.chart_cur, CLR_IQ,
                                    LV_CHART_AXIS_PRIMARY_Y);

  unit = lv_label_create(scr);
  lv_label_set_text(unit, "mA");
  lv_obj_set_pos(unit, SCOPE_CHART_X + 10, SCOPE_CHART_Y + 4);
  lv_obj_set_style_text_color(unit, CLR_DIM, 0);

  /* RPM chart */

  g_ui.chart_rpm = lv_chart_create(scr);
  lv_obj_set_pos(g_ui.chart_rpm, SCOPE_CHART_X, SCOPE_RPM_Y);
  lv_obj_set_size(g_ui.chart_rpm, SCOPE_CHART_W, SCOPE_RPM_H);
  lv_chart_set_type(g_ui.chart_rpm, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(g_ui.chart_rpm, SCOPE_POINT_CNT);
  lv_chart_set_update_mode(g_ui.chart_rpm, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_range(g_ui.chart_rpm, LV_CHART_AXIS_PRIMARY_Y,
                     g_scale_rpm[0][0], g_scale_rpm[0][1]);
  lv_chart_set_div_line_count(g_ui.chart_rpm, 3, 4);
  scope_style_chart(g_ui.chart_rpm, CLR_GRID);
  scope_y_ticks(g_ui.chart_rpm, 4, 36);
  lv_obj_set_style_pad_left(g_ui.chart_rpm, 44, LV_PART_MAIN);
  lv_obj_set_style_pad_right(g_ui.chart_rpm, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_top(g_ui.chart_rpm, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(g_ui.chart_rpm, 6, LV_PART_MAIN);

  g_ui.ser_rpm = lv_chart_add_series(g_ui.chart_rpm, CLR_RPM,
                                     LV_CHART_AXIS_PRIMARY_Y);

  unit = lv_label_create(scr);
  lv_label_set_text(unit, "rpm");
  lv_obj_set_pos(unit, SCOPE_CHART_X + 10, SCOPE_RPM_Y + 4);
  lv_obj_set_style_text_color(unit, CLR_DIM, 0);

  /* Readouts */

  /* Five readouts across 320 px: 58 px each (61 px stride). Formats are
   * compact so they fit at the 14 px theme font. */

  g_ui.lb_id = lv_label_create(scr);
  lv_obj_set_pos(g_ui.lb_id, 6, SCOPE_RO_Y);
  lv_obj_set_size(g_ui.lb_id, 58, 20);
  lv_obj_set_style_text_color(g_ui.lb_id, CLR_ID, 0);

  g_ui.lb_iq = lv_label_create(scr);
  lv_obj_set_pos(g_ui.lb_iq, 67, SCOPE_RO_Y);
  lv_obj_set_size(g_ui.lb_iq, 58, 20);
  lv_obj_set_style_text_color(g_ui.lb_iq, CLR_IQ, 0);

  g_ui.lb_rpm = lv_label_create(scr);
  lv_obj_set_pos(g_ui.lb_rpm, 128, SCOPE_RO_Y);
  lv_obj_set_size(g_ui.lb_rpm, 58, 20);
  lv_obj_set_style_text_color(g_ui.lb_rpm, CLR_RPM, 0);

  g_ui.lb_vbus = lv_label_create(scr);
  lv_obj_set_pos(g_ui.lb_vbus, 189, SCOPE_RO_Y);
  lv_obj_set_size(g_ui.lb_vbus, 58, 20);
  lv_obj_set_style_text_color(g_ui.lb_vbus, CLR_DIM, 0);

  g_ui.lb_ibus = lv_label_create(scr);
  lv_obj_set_pos(g_ui.lb_ibus, 250, SCOPE_RO_Y);
  lv_obj_set_size(g_ui.lb_ibus, 58, 20);
  lv_obj_set_style_text_color(g_ui.lb_ibus, CLR_IBUS, 0);

  /* Time-span / range indicator, overlaid on the main chart top-right.
   * Shows e.g. "10.0s 5.0A" and updates live with ZOOM+/SCALE. */

  g_ui.lb_view = lv_label_create(scr);
  lv_obj_align_to(g_ui.lb_view, g_ui.chart_cur, LV_ALIGN_TOP_RIGHT, -24, 2);
  lv_obj_set_style_text_color(g_ui.lb_view, CLR_DIM, 0);
  scope_refresh_view_label();

  /* Buttons row 1: PAUSE ZOOM+ ZOOM- SCALE. Label texts are empty here;
   * scope_apply_lang() fills them from the active language table. */

  g_ui.btn_pause = scope_make_btn(scr, 0, 0, "", ACT_PAUSE, SCOPE_BTN_W);
  g_ui.lab_btn[ACT_PAUSE] = lv_obj_get_child(g_ui.btn_pause, 0);

  g_ui.btn_zoom_in = scope_make_btn(scr, 1, 0, "", ACT_ZOOM_IN, SCOPE_BTN_W);
  g_ui.lab_btn[ACT_ZOOM_IN] = lv_obj_get_child(g_ui.btn_zoom_in, 0);
  g_ui.btn_zoom_out = scope_make_btn(scr, 2, 0, "", ACT_ZOOM_OUT, SCOPE_BTN_W);
  g_ui.lab_btn[ACT_ZOOM_OUT] = lv_obj_get_child(g_ui.btn_zoom_out, 0);
  g_ui.btn_scale = scope_make_btn(scr, 3, 0, "", ACT_SCALE, SCOPE_BTN_W);
  g_ui.lab_btn[ACT_SCALE] = lv_obj_get_child(g_ui.btn_scale, 0);

  /* Buttons row 2: CLR < > CH CAN (narrower, five across) */

  g_ui.btn_clear = scope_make_btn(scr, 0, 1, "", ACT_CLEAR, 50);
  g_ui.lab_btn[ACT_CLEAR] = lv_obj_get_child(g_ui.btn_clear, 0);
  g_ui.btn_left = scope_make_btn(scr, 1, 1, "", ACT_LEFT, 50);
  g_ui.lab_btn[ACT_LEFT] = lv_obj_get_child(g_ui.btn_left, 0);
  g_ui.btn_right = scope_make_btn(scr, 2, 1, "", ACT_RIGHT, 50);
  g_ui.lab_btn[ACT_RIGHT] = lv_obj_get_child(g_ui.btn_right, 0);
  g_ui.btn_ch = scope_make_btn(scr, 3, 1, "", ACT_CH, 50);
  g_ui.lab_btn[ACT_CH] = lv_obj_get_child(g_ui.btn_ch, 0);
  g_ui.btn_can = scope_make_btn(scr, 4, 1, "", ACT_CAN, 50);
  g_ui.lab_btn[ACT_CAN] = lv_obj_get_child(g_ui.btn_can, 0);
  g_ui.btn_ai = scope_make_btn(scr, 5, 1, "", ACT_AI, 50);
  g_ui.lab_btn[ACT_AI] = lv_obj_get_child(g_ui.btn_ai, 0);

  /* CAN link-info page (hidden until the CAN button is pressed).
   * Non-CAN sources (sim) get a disabled CAN button. */

  scope_build_can_page();
  if (g_ui.ds->get_can_stats == NULL)
    {
      lv_obj_add_state(g_ui.btn_can, LV_STATE_DISABLED);
    }

  /* AI tune page (background tuner thread is started by focscope.c before
   * the data source init; manual START uses the default motor). */

  scope_build_ai_page();
  g_ui.ai_src = 0;              /* 0=MANUAL, 1=CAN */
  g_ui.ai_motor.Rs = 0.5f;
  g_ui.ai_motor.Ld = 0.001f;
  g_ui.ai_motor.Lq = 0.001f;
  g_ui.ai_motor.Ke = 0.01f;
  g_ui.ai_motor.poles = 4;
  g_ui.ai_manual = g_ui.ai_motor;
  strncpy(g_ui.ai_params, "0.5 0.001 0.001 0.01 4",
          sizeof(g_ui.ai_params) - 1);
  g_ui.ai_params[sizeof(g_ui.ai_params) - 1] = '\0';
  lv_textarea_set_text(g_ui.ai_ta, g_ui.ai_params);

  /* WiFi page + top-bar button (left of the RUN/PAUSED label).
   * The background thread is started in focscope.c. */

  scope_build_wifi_page();
  g_ui.wifi_ssid[0] = '\0';
  g_ui.ta_active = NULL;
  strncpy(g_ui.wifi_password, "1234567890", sizeof(g_ui.wifi_password) - 1);

  g_ui.btn_wifi = lv_button_create(scr);
  lv_obj_align(g_ui.btn_wifi, LV_ALIGN_TOP_RIGHT, -112, 6);
  lv_obj_set_size(g_ui.btn_wifi, 46, 24);
  lv_obj_add_event_cb(g_ui.btn_wifi, scope_btn_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)ACT_WIFI);
  lv_obj_set_style_bg_color(g_ui.btn_wifi, CLR_BTN_BG, 0);
  lv_obj_set_style_bg_color(g_ui.btn_wifi, CLR_BTN_BG_PRS, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(g_ui.btn_wifi, CLR_BTN_BD, 0);
  lv_obj_set_style_border_width(g_ui.btn_wifi, 1, 0);
  lv_obj_set_style_radius(g_ui.btn_wifi, 4, 0);
  lv_obj_set_style_shadow_width(g_ui.btn_wifi, 0, 0);
  lv_obj_set_style_pad_all(g_ui.btn_wifi, 0, 0);
  lv_obj_set_style_text_color(g_ui.btn_wifi, CLR_TEXT, 0);

  g_ui.lb_wifi_btn = lv_label_create(g_ui.btn_wifi);
  lv_obj_set_style_text_font(g_ui.lb_wifi_btn, &font_puhui_20_4, 0);
  lv_obj_center(g_ui.lb_wifi_btn);

  /* Start in RUN (Chinese by default): fill texts, view buttons disabled */

  scope_apply_lang();
  scope_set_view_btns_enabled(false);

  /* Pre-fill so the screen is alive immediately */

  for (k = 0; k < SCOPE_PRE_FILL; k++)
    {
      if (g_ui.ds->get_sample(g_ui.ds, &s) < 0)
        {
          break;
        }

      scope_push_sample(&s);
      g_ui.last = s;
    }

  /* Data timer (runs inside lv_timer_handler, single thread) */

  lv_timer_create(scope_timer_cb, SCOPE_TICK_MS, NULL);
}
