/****************************************************************************
 * wifi_ui.c
 *
 * FOCPilot WiFi 页面后台模块实现
 *
 * 独立线程执行 WiFi 扫描 / 连接, 复用 wapi 库 (apps/wireless/wapi):
 *   - 扫描: wapi_scan_init -> wapi_scan_stat 轮询 -> wapi_scan_coll
 *   - 连接: wpa_driver_wext_associate (wpa_wconfig_s)
 *
 * wapi 需要很大的栈 (CONFIG_WIRELESS_WAPI_STACKSIZE=204800), 因此本线程
 * 也配置 256KB 栈, 避免 ioctl/TLS 栈溢出。
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <nuttx/wireless/wireless.h>  /* struct iwreq, SIOCGIWESSID */
#include <netutils/netlib.h>          /* netlib_ifup, netlib_obtain_ipv4addr */

#include "wifi_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_IFNAME         "wlan0"
#define WIFI_UI_THREAD_STACK 262144   /* 256KB: wapi needs a big stack */
#define WIFI_UI_POLL_US     50000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wifi_ap_s
{
  char ssid[WIFI_UI_SSID_LEN];
  int rssi;
  int channel;
  int has_security;
};

struct wifi_ui_s
{
  pthread_t tid;
  bool running;

  /* 共享状态 (mutex-guarded) */

  pthread_mutex_t lock;
  int state;                    /* WIFI_UI_STATE_* */
  char connected_ssid[WIFI_UI_SSID_LEN];
  char connected_ip[16];
  int scan_count;
  struct wifi_ap_s aps[WIFI_UI_MAX_AP];

  /* 命令 (mutex-guarded) */

  bool scan_pending;
  bool connect_pending;
  char connect_ssid[WIFI_UI_SSID_LEN];
  char connect_password[64];
  bool disconnect_pending;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct wifi_ui_s g_wifi;

/* latched after the first `wapi mode` fallback; shared by wifi_do_scan()
 * and wifi_do_connect() so the driver stack is not restarted repeatedly */
static bool g_drv_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void wifi_set_state(int state)
{
  pthread_mutex_lock(&g_wifi.lock);
  g_wifi.state = state;
  pthread_mutex_unlock(&g_wifi.lock);
}

/* 查询当前连接状态: 已连接则填 ssid_out, 未连接则保持为空。
 *
 * 直接 socket ioctl (SIOCGIWESSID), 与 wapi show 同一条驱动路径, 但不起
 * nsh 子进程、不写临时文件: 之前用 system("wapi show ...") 在断开与
 * wifi_manager 重连并发时子进程会卡死, 导致本线程永久阻塞, UI 状态
 * 停在旧值 ("断开了还显示连接")。 */
static void wifi_query_connected(FAR char *ssid_out, size_t ssid_len)
{
  struct iwreq wrq;
  int fd;
  int ret;

  ssid_out[0] = '\0';

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    {
      return;
    }

  memset(&wrq, 0, sizeof(wrq));
  wrq.u.essid.pointer = ssid_out;
  wrq.u.essid.length  = ssid_len;
  strlcpy(wrq.ifr_name, WIFI_IFNAME, IFNAMSIZ);

  ret = ioctl(fd, SIOCGIWESSID, (unsigned long)&wrq);
  close(fd);

  if (ret < 0 || !wrq.u.essid.flags || ssid_out[0] == '\0')
    {
      ssid_out[0] = '\0';   /* not connected */
    }
}

/* 查询 wlan0 的 IPv4 地址 (SIOCGIFADDR, 同 netutils/netlib 的实现) */
static int wifi_query_ip(FAR char *ip_out, size_t len)
{
  int fd;
  struct ifreq req;
  FAR struct sockaddr_in *addr;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    {
      return -1;
    }

  strlcpy(req.ifr_name, WIFI_IFNAME, IFNAMSIZ);
  if (ioctl(fd, SIOCGIFADDR, (unsigned long)&req) < 0)
    {
      close(fd);
      return -1;
    }

  addr = (FAR struct sockaddr_in *)&req.ifr_addr;
  if (addr->sin_addr.s_addr == 0)
    {
      close(fd);
      return -1;
    }

  inet_ntop(AF_INET, &addr->sin_addr, ip_out, len);
  close(fd);
  return 0;
}

/* 刷新已连接信息 (SSID + IP)。覆盖两种来源: 本页面的连接和
 * wifi_manager 开机自动连接 (rcS.nsh -> wifi_manager)。
 * 只打印状态变化 (连接/断开), 平时 2s 一轮静默轮询。 */
static void wifi_refresh_conn(void)
{
  char ssid[WIFI_UI_SSID_LEN];
  char ip[16];
  static int  last_connected = -1;   /* force a print on the 1st poll */
  static char last_ssid[WIFI_UI_SSID_LEN];

  ssid[0] = '\0';
  ip[0] = '\0';

  wifi_query_connected(ssid, sizeof(ssid));

  if (ssid[0] != '\0')
    {
      /* 已关联就算已连接 (IP 是附加信息, DHCP 未完成时显示 "--") */
      wifi_query_ip(ip, sizeof(ip));

      pthread_mutex_lock(&g_wifi.lock);
      strncpy(g_wifi.connected_ssid, ssid, WIFI_UI_SSID_LEN - 1);
      g_wifi.connected_ssid[WIFI_UI_SSID_LEN - 1] = '\0';
      strncpy(g_wifi.connected_ip, ip, sizeof(g_wifi.connected_ip) - 1);
      g_wifi.connected_ip[sizeof(g_wifi.connected_ip) - 1] = '\0';
      g_wifi.state = WIFI_UI_STATE_CONNECTED;
      pthread_mutex_unlock(&g_wifi.lock);

      if (last_connected != 1 || strcmp(last_ssid, ssid) != 0)
        {
          printf("[WiFi] connected: %s (%s)\n", ssid, ip);
        }
    }
  else if (wifi_ui_state() == WIFI_UI_STATE_CONNECTED)
    {
      /* 之前连着, 现在断了 */
      pthread_mutex_lock(&g_wifi.lock);
      g_wifi.connected_ssid[0] = '\0';
      g_wifi.connected_ip[0] = '\0';
      g_wifi.state = WIFI_UI_STATE_IDLE;
      pthread_mutex_unlock(&g_wifi.lock);

      if (last_connected != 0)
        {
          printf("[WiFi] disconnected\n");
        }
    }

  if (ssid[0] != '\0')
    {
      strncpy(last_ssid, ssid, sizeof(last_ssid) - 1);
      last_ssid[sizeof(last_ssid) - 1] = '\0';
    }

  last_connected = (ssid[0] != '\0') ? 1 : 0;
}

/* 扫描已弃用: 这块固件的无目标主动扫描返回 0 AP (已实测), UI 改为手动输入
 * SSID 连接。 保留本函数仅为兼容公开接口 wifi_ui_scan(); 直接空结果 ——
 * 纯 NuttX, 无 system()/nsh。 */
static void wifi_do_scan(void)
{
  netlib_ifup(WIFI_IFNAME);

  pthread_mutex_lock(&g_wifi.lock);
  g_wifi.scan_count = 0;
  g_wifi.state = WIFI_UI_STATE_SCAN_DONE;
  pthread_mutex_unlock(&g_wifi.lock);

  printf("[WiFi] scan disabled (fw has no AP discovery); use manual SSID\n");
}

/* --- ioctl helpers: pure NuttX, no system()/nsh child processes --- */

static int wifi_ioctl_mode(int fd, int mode)
{
  struct iwreq wrq;

  memset(&wrq, 0, sizeof(wrq));
  strlcpy(wrq.ifr_name, WIFI_IFNAME, IFNAMSIZ);
  wrq.u.mode = mode;
  return ioctl(fd, SIOCSIWMODE, (unsigned long)&wrq);
}

/* WPA2 passphrase -> SIOCSIWENCODEEXT (same as `wapi psk ... 3 2`) */
static int wifi_ioctl_psk(int fd, FAR const char *passphrase)
{
  struct iwreq wrq;
  FAR struct iw_encode_ext *ext;
  uint8_t buf[sizeof(struct iw_encode_ext) + 64];
  size_t plen = strlen(passphrase);

  if (plen == 0 || plen > 63)
    {
      return -1;
    }

  memset(buf, 0, sizeof(buf));
  ext = (FAR struct iw_encode_ext *)buf;
  ext->alg     = IW_ENCODE_ALG_CCMP;
  ext->key_len = plen;
  memcpy(ext + 1, passphrase, plen);   /* key lives right after the header */

  memset(&wrq,  0, sizeof(wrq));
  strlcpy(wrq.ifr_name, WIFI_IFNAME, IFNAMSIZ);
  wrq.u.encoding.pointer = buf;
  wrq.u.encoding.length  = sizeof(struct iw_encode_ext) + plen;
  return ioctl(fd, SIOCSIWENCODEEXT, (unsigned long)&wrq);
}

static int wifi_ioctl_essid(int fd, FAR const char *ssid)
{
  struct iwreq wrq;

  memset(&wrq, 0, sizeof(wrq));
  strlcpy(wrq.ifr_name, WIFI_IFNAME, IFNAMSIZ);
  wrq.u.essid.pointer = (FAR void *)ssid;
  wrq.u.essid.length  = strlen(ssid);
  wrq.u.essid.flags   = 1;
  return ioctl(fd, SIOCSIWESSID, (unsigned long)&wrq);
}

static void wifi_ioctl_disconnect(void)
{
  struct iwreq wrq;
  int fd;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    {
      return;
    }

  memset(&wrq, 0, sizeof(wrq));
  strlcpy(wrq.ifr_name, WIFI_IFNAME, IFNAMSIZ);
  wrq.u.essid.pointer = NULL;
  wrq.u.essid.length  = 0;
  wrq.u.essid.flags   = 0;   /* driver: flag==0 -> wifi_disconnect() */
  ioctl(fd, SIOCSIWESSID, (unsigned long)&wrq);
  close(fd);
}

/* 连接一个 AP — 全部走 NuttX API (netlib + ioctl), 不再起 nsh 子进程 */
static void wifi_do_connect(FAR const char *ssid, FAR const char *password)
{
  FILE *fp;
  int fd;

  wifi_set_state(WIFI_UI_STATE_CONNECTING);

  netlib_ifup(WIFI_IFNAME);

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    {
      wifi_set_state(WIFI_UI_STATE_ERROR);
      return;
    }

  /* Bring the driver protocol stack up once (STA mode triggers wifi_on()).
   * Do not repeat mode on every connect: it restarts the stack and can
   * interrupt a concurrent wifi_manager join. */

  if (!g_drv_ready)
    {
      wifi_ioctl_mode(fd, IW_MODE_INFRA);
      g_drv_ready = true;
      usleep(1000000);
    }

  /* WPA2 passphrase + target SSID, direct ioctl (same as `wapi psk`/`wapi essid`) */

  if (password[0])
    {
      wifi_ioctl_psk(fd, password);
    }

  wifi_ioctl_essid(fd, ssid);
  close(fd);

  /* DHCP: obtains IP + installs the DHCP gateway as the default route and
   * writes the DNS nameserver into resolver (the fixed dhcpc does both) */
  netlib_obtain_ipv4addr(WIFI_IFNAME);

  /* Persist the config: wifi_manager re-reads /data/wifi.cfg every 5s
   * and would otherwise force its own saved SSID back; writing it here
   * keeps both in sync and makes the new AP stick after reboot. */

  fp = fopen("/data/wifi.cfg", "w");
  if (fp)
    {
      fprintf(fp, "SSID=%s\n", ssid);
      fprintf(fp, "PASSWORD=%s\n", password);
      fclose(fp);
    }

  /* Remember the SSID we targeted, and grab the IP (DHCP is synchronous,
   * so the address should be in place already; wifi_refresh_conn() will
   * keep it up to date afterwards) */

  {
    char ip[16];

    ip[0] = '\0';
    wifi_query_ip(ip, sizeof(ip));

    pthread_mutex_lock(&g_wifi.lock);
    strncpy(g_wifi.connected_ssid, ssid, WIFI_UI_SSID_LEN - 1);
    g_wifi.connected_ssid[WIFI_UI_SSID_LEN - 1] = '\0';
    strncpy(g_wifi.connected_ip, ip, sizeof(g_wifi.connected_ip) - 1);
    g_wifi.connected_ip[sizeof(g_wifi.connected_ip) - 1] = '\0';
    g_wifi.state = WIFI_UI_STATE_CONNECTED;
    pthread_mutex_unlock(&g_wifi.lock);

    printf("[WiFi] connected to %s (%s)\n", ssid, ip);
  }
}

static FAR void *wifi_thread(FAR void *arg)
{
  bool scan_pending, connect_pending, disconnect_pending;
  char ssid[WIFI_UI_SSID_LEN];
  char password[64];
  int refresh_tick = 0;

  for (;;)
    {
      scan_pending = connect_pending = disconnect_pending = false;

      pthread_mutex_lock(&g_wifi.lock);
      scan_pending       = g_wifi.scan_pending;
      connect_pending    = g_wifi.connect_pending;
      disconnect_pending = g_wifi.disconnect_pending;
      g_wifi.scan_pending = g_wifi.connect_pending = false;
      g_wifi.disconnect_pending = false;

      if (connect_pending)
        {
          strncpy(ssid, g_wifi.connect_ssid, WIFI_UI_SSID_LEN - 1);
          ssid[WIFI_UI_SSID_LEN - 1] = '\0';
          strncpy(password, g_wifi.connect_password, sizeof(password) - 1);
          password[sizeof(password) - 1] = '\0';
        }
      pthread_mutex_unlock(&g_wifi.lock);

      if (disconnect_pending)
        {
          wifi_ioctl_disconnect();

          /* Keep /data/wifi.cfg: wifi_manager auto-reconnect is desired.
           * The UI state is then re-synced by the 2s wifi_refresh_conn()
           * poll, so the screen shows the real connect/disconnect state. */

          pthread_mutex_lock(&g_wifi.lock);
          g_wifi.connected_ssid[0] = '\0';
          g_wifi.connected_ip[0] = '\0';
          g_wifi.state = WIFI_UI_STATE_IDLE;
          pthread_mutex_unlock(&g_wifi.lock);
        }
      else if (scan_pending)
        {
          wifi_do_scan();
        }
      else if (connect_pending)
        {
          wifi_do_connect(ssid, password);
        }
      else
        {
          /* Periodically refresh the connected info so the UI tracks
           * connect/disconnect in near-real-time (passive drops, AP gone,
           * etc.). 2s = 40 x 50ms. Each poll is ~10 wapi ioctls but the
           * driver no longer prints them. */

          if (++refresh_tick >= 40)
            {
              refresh_tick = 0;
              wifi_refresh_conn();
            }

          usleep(WIFI_UI_POLL_US);
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void wifi_ui_start(void)
{
  pthread_attr_t attr;

  if (g_wifi.running)
    {
      return;
    }

  g_wifi.state = WIFI_UI_STATE_IDLE;
  pthread_mutex_init(&g_wifi.lock, NULL);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, WIFI_UI_THREAD_STACK);

  if (pthread_create(&g_wifi.tid, &attr, wifi_thread, NULL) == 0)
    {
      g_wifi.running = true;
    }

  pthread_attr_destroy(&attr);
}

void wifi_ui_scan(void)
{
  pthread_mutex_lock(&g_wifi.lock);
  g_wifi.scan_pending = true;
  pthread_mutex_unlock(&g_wifi.lock);
}

void wifi_ui_connect(FAR const char *ssid, FAR const char *password)
{
  pthread_mutex_lock(&g_wifi.lock);
  strncpy(g_wifi.connect_ssid, ssid, WIFI_UI_SSID_LEN - 1);
  g_wifi.connect_ssid[WIFI_UI_SSID_LEN - 1] = '\0';
  strncpy(g_wifi.connect_password, password, sizeof(g_wifi.connect_password) - 1);
  g_wifi.connect_password[sizeof(g_wifi.connect_password) - 1] = '\0';
  g_wifi.connect_pending = true;
  pthread_mutex_unlock(&g_wifi.lock);
}

void wifi_ui_disconnect(void)
{
  pthread_mutex_lock(&g_wifi.lock);
  g_wifi.disconnect_pending = true;
  pthread_mutex_unlock(&g_wifi.lock);
}

int wifi_ui_state(void)
{
  int s;

  pthread_mutex_lock(&g_wifi.lock);
  s = g_wifi.state;
  pthread_mutex_unlock(&g_wifi.lock);
  return s;
}

int wifi_ui_scan_count(void)
{
  int n;

  pthread_mutex_lock(&g_wifi.lock);
  n = g_wifi.scan_count;
  pthread_mutex_unlock(&g_wifi.lock);
  return n;
}

FAR const char *wifi_ui_scan_ssid(int idx)
{
  FAR const char *s = "";

  pthread_mutex_lock(&g_wifi.lock);
  if (idx >= 0 && idx < g_wifi.scan_count)
    {
      s = g_wifi.aps[idx].ssid;
    }
  pthread_mutex_unlock(&g_wifi.lock);
  return s;
}

int wifi_ui_scan_rssi(int idx)
{
  int v = 0;

  pthread_mutex_lock(&g_wifi.lock);
  if (idx >= 0 && idx < g_wifi.scan_count)
    {
      v = g_wifi.aps[idx].rssi;
    }
  pthread_mutex_unlock(&g_wifi.lock);
  return v;
}

int wifi_ui_scan_channel(int idx)
{
  int v = 0;

  pthread_mutex_lock(&g_wifi.lock);
  if (idx >= 0 && idx < g_wifi.scan_count)
    {
      v = g_wifi.aps[idx].channel;
    }
  pthread_mutex_unlock(&g_wifi.lock);
  return v;
}

int wifi_ui_scan_has_security(int idx)
{
  int v = 0;

  pthread_mutex_lock(&g_wifi.lock);
  if (idx >= 0 && idx < g_wifi.scan_count)
    {
      v = g_wifi.aps[idx].has_security;
    }
  pthread_mutex_unlock(&g_wifi.lock);
  return v;
}

FAR const char *wifi_ui_connected_ssid(void)
{
  pthread_mutex_lock(&g_wifi.lock);
  return g_wifi.connected_ssid;
}

FAR const char *wifi_ui_connected_ip(void)
{
  pthread_mutex_lock(&g_wifi.lock);
  return g_wifi.connected_ip;
}
