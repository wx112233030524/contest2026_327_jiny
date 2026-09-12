/****************************************************************************
 * wifi_ui.h
 *
 * FOCPilot WiFi 页面后台模块
 *
 * 独立线程执行 WiFi 扫描 / 连接 (wapi ioctl 会阻塞, 不能放 LVGL 主循环)。
 * UI (scope_ui) 通过 wifi_ui_* 接口轮询状态和扫描结果。
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_FOCSCOPE_WIFI_UI_H
#define __APPS_EXAMPLES_FOCSCOPE_WIFI_UI_H

#include <nuttx/config.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_UI_STATE_IDLE      0   /* 空闲 */
#define WIFI_UI_STATE_SCANNING  1   /* 扫描中 */
#define WIFI_UI_STATE_SCAN_DONE 2   /* 扫描完成 */
#define WIFI_UI_STATE_CONNECTING 3  /* 连接中 */
#define WIFI_UI_STATE_CONNECTED 4   /* 已连接 */
#define WIFI_UI_STATE_ERROR     5   /* 错误 */

#define WIFI_UI_MAX_AP          12  /* 最多显示的 AP 数 */
#define WIFI_UI_SSID_LEN        33

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* 启动后台 WiFi 线程 (进程内只调一次) */
void wifi_ui_start(void);

/* 触发扫描. 线程内执行, 完成后状态 SCAN_DONE */
void wifi_ui_scan(void);

/* 连接指定 SSID (密码为空则读 /data/wifi.cfg). 异步, 状态 CONNECTING */
void wifi_ui_connect(FAR const char *ssid, FAR const char *password);

/* 断开当前连接 */
void wifi_ui_disconnect(void);

/* 当前状态 (WIFI_UI_STATE_*) */
int wifi_ui_state(void);

/* 扫描结果 */
int  wifi_ui_scan_count(void);
FAR const char *wifi_ui_scan_ssid(int idx);
int wifi_ui_scan_rssi(int idx);
int wifi_ui_scan_channel(int idx);
int wifi_ui_scan_has_security(int idx);   /* 1 = 加密 */

/* 当前连接信息 (SSID 为空 = 未连接; IP 为空 = 未拿到地址) */
FAR const char *wifi_ui_connected_ssid(void);
FAR const char *wifi_ui_connected_ip(void);

#endif /* __APPS_EXAMPLES_FOCSCOPE_WIFI_UI_H */
