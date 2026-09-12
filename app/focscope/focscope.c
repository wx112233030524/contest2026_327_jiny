/****************************************************************************
 * apps/examples/focscope/focscope.c
 *
 * FOCPilot oscilloscope UI entry point.
 * Initializes LVGL on /dev/fb0 with /dev/input0 touch, creates the scope
 * UI, then runs the standard lv_timer_handler loop.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/boardctl.h>

#include <lvgl/lvgl.h>

#include "scope_data.h"
#include "scope_ui.h"
#include "ai_tuner_ui.h"
#include "wifi_ui.h"
#include "foc_agent_skill.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Should we perform board-specific driver initialization? There are two
 * ways that board initialization can occur:  1) automatically via
 * board_late_initialize() during bootup if CONFIG_BOARD_LATE_INITIALIZE
 * or 2).
 * via a call to boardctl() if the interface is enabled
 * (CONFIG_BOARDCTL=y).
 * If this task is running as an NSH built-in application, then that
 * initialization has probably already been performed otherwise we do it
 * here.
 */

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: scope_instance_lock
 *
 * Description:
 *   Grab a boot-lifetime single-instance lock on /tmp (tmpfs, cleared on
 *   reboot). On openvela, LVGL's global state lives in per-task TLS, so
 *   lv_is_initialized() cannot see another task's LVGL instance: two
 *   focscope tasks would both take over /dev/fb0 and /dev/input0 and
 *   fight for the screen. A plain O_EXCL file is the reliable guard.
 *
 ****************************************************************************/

static void scope_instance_lock(void)
{
  char buf[16];
  pid_t pid;
  ssize_t n;
  int fd;
  int ofd;

  /* Try to take the lock and record our pid in it */

  fd = open("/tmp/focscope.lock", O_CREAT | O_EXCL | O_WRONLY, 0666);
  if (fd >= 0)
    {
      dprintf(fd, "%d\n", getpid());
      close(fd);
      return;
    }

  if (errno != EEXIST)
    {
      /* No tmpfs (or some other error): degrade to running unlocked rather
       * than refusing to start.  Plain printf: LV_LOG may be lost if the
       * task dies before the syslog drain (SYSLOG_BUFFER is small and
       * async).
       */

      printf("focscope: lock unavailable (%d), running unlocked\n", errno);
      return;
    }

  /* Another focscope holds the lock (typically the sim instance started
   * by xiaozhi.sh at boot).  Identify and terminate it, then take the
   * lock.  On openvela LVGL state lives in per-task TLS, so the new
   * instance starts a clean LVGL world once the old task is gone.
   */

  ofd = open("/tmp/focscope.lock", O_RDONLY);
  pid = 0;
  if (ofd >= 0)
    {
      n = read(ofd, buf, sizeof(buf) - 1);
      close(ofd);
      if (n > 0)
        {
          buf[n] = '\0';
          pid = (pid_t)atoi(buf);
        }
    }

  if (pid > 0)
    {
      printf("focscope: replacing existing instance (pid %d)\n", pid);
      kill(pid, SIGKILL);
      usleep(100000);
    }
  else
    {
      printf("focscope: stale lock without owner, removing\n");
    }

  unlink("/tmp/focscope.lock");

  fd = open("/tmp/focscope.lock", O_CREAT | O_EXCL | O_WRONLY, 0666);
  if (fd < 0)
    {
      printf("focscope: lock still held (%d) - reboot to clear\n", errno);
      exit(EXIT_FAILURE);
    }

  dprintf(fd, "%d\n", getpid());
  close(fd);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main
 *
 * Description:
 *   FOCPilot oscilloscope main entry. Creates the data source (simulated
 *   for now; swap scope_ds_sim_get() for a CAN-backed source later), then
 *   builds the UI and runs the LVGL event loop.
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  scope_ds_t ds;
  bool use_can = false;
  int can_port = 0;

  /* Install our runtime Skill for the on-board ai_agent before doing anything
   * else. This has to happen at boot from a program that always runs, not
   * from ai_tuner_can: the agent can only ask for a tune once it has already
   * read the Skill telling it how, so installing it from the tuner itself
   * would be a chicken-and-egg problem.
   *
   * Cheap on every subsequent boot: identical content is not rewritten. */

  foc_agent_skill_install();

  /* Data source selection:
   *   focscope          - simulated waveforms (default)
   *   focscope can      - CAN telemetry from the drive board (/dev/can0)
   *   focscope can 0|1  - same, explicit CAN port (/dev/can0 or /dev/can1)
   */

  if (argc > 1 && strcmp(argv[1], "can") == 0)
    {
      use_can = true;
      if (argc > 2)
        {
          can_port = atoi(argv[2]);
          if (can_port != 0 && can_port != 1)
            {
              printf("focscope: bad CAN port '%s', defaulting to 0\n",
                     argv[2]);
              can_port = 0;
            }
        }
    }

  /* Single instance only: a second focscope would fight over the
   * framebuffer. Take the lock before touching LVGL. */

  scope_instance_lock();

#ifdef NEED_BOARDINIT
  /* Perform board-specific driver initialization */

  boardctl(BOARDIOC_INIT, 0);

#endif

  lv_init();

  lv_nuttx_dsc_init(&info);

#ifdef CONFIG_INPUT_TOUCHSCREEN
  info.input_path = CONFIG_EXAMPLES_FOCSCOPE_INPUT_DEVPATH;
#endif

  lv_nuttx_init(&info, &result);

  if (result.disp == NULL)
    {
      LV_LOG_ERROR("focscope display initialization failure!");
      return 1;
    }

  /* Data source: simulated FOC waveforms by default, CAN telemetry with
   * "focscope can [0|1]" (FOCPILOT CAN PROTOCOL v1).
   */

  if (use_can)
    {
      ds = scope_ds_can_get(can_port);
    }
  else
    {
      ds = scope_ds_sim_get();
    }

  /* Start the AI tuner thread before the data source registers its CAN
   * send callback (ai_tuner_ui_set_sender) inside ds.init(). */

  ai_tuner_ui_start();

  /* Start the WiFi background thread (wapi ioctl is blocking; must not
   * run on the LVGL thread). */

  wifi_ui_start();

  /* focscope owns the WiFi now (wifi_manager removed from rcS.nsh) — no
   * second process competing for the adapter.  Auto-connect from
   * /data/wifi.cfg, asynchronously through the background thread. */

  /* Let the WiFi module's SDIO rail settle before the first wifi_on()
   * firmware download.  Firing it immediately after boot has been seen to
   * raise host->int_err=0x8000 (SDIO cmd53 I/O fail) with a flood of
   * "halmac_sdio_reg_write_32 I/O FAIL" — an un-stable power-up timing
   * issue.  A short settle delay avoids it. */

  usleep(5000000);

  {
    FILE *fp;
    char line[128];
    char ssid[33] = { 0 };
    char pwd[64]  = { 0 };

    fp = fopen("/data/wifi.cfg", "r");
    if (fp)
      {
        while (fgets(line, sizeof(line), fp))
          {
            line[strcspn(line, "\r\n")] = '\0';
            if (strncmp(line, "SSID=", 5) == 0)
              {
                strlcpy(ssid, line + 5, sizeof(ssid));
              }
            else if (strncmp(line, "PASSWORD=", 9) == 0)
              {
                strlcpy(pwd, line + 9, sizeof(pwd));
              }
          }

        fclose(fp);
      }

    if (ssid[0])
      {
        printf("[WiFi] auto-connect %s (from /data/wifi.cfg)\n", ssid);
        wifi_ui_connect(ssid, pwd);
      }
  }

  if (ds.init != NULL && ds.init(&ds) < 0)
    {
      LV_LOG_ERROR("focscope data source init failure!");
      return 1;
    }

  scope_ui_create(&ds);
  LV_LOG_WARN("focscope UI created (%s), entering loop", ds.name);

  while (1)
    {
      uint32_t idle;
      idle = lv_timer_handler();

      /* Minimum sleep of 1ms */

      idle = idle ? idle : 1;
      usleep(idle * 1000);
    }

  return 0;
}
