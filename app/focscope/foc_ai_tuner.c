/****************************************************************************
 * foc_ai_tuner.c
 *
 * FOCPilot AI Tuner 共享模块实现
 *
 * MiMo API 调用 + PI 参数计算 + 白名单校验。
 * 供 ai_tuner_test 与 ai_tuner_can 复用。
 *
 * MiMo API: OpenAI 兼容协议
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <curl/curl.h>

#include "foc_ai_tuner.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MIMO_API_BASE   "https://token-plan-cn.xiaomimimo.com/v1"
#define MIMO_API_URL    MIMO_API_BASE "/chat/completions"
#define MIMO_MODEL      "mimo-v2.5"

/* 填入你自己的 MiMo API key 后再编译 (https://platform.xiaomimimo.com/console).
 * 注意: 本仓库是公开的 — 不要把真实 key 提交上来, 会被爬虫立即抓走. */

#define MIMO_API_KEY    "YOUR_MIMO_API_KEY"

#define FOC_AI_HTTP_TIMEOUT   300L
#define FOC_AI_RESP_SIZE      131072

/****************************************************************************
 * Private Data
 ****************************************************************************/

static char g_response[FOC_AI_RESP_SIZE];
static size_t g_response_len = 0;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static size_t http_response_cb(void *contents, size_t size,
                               size_t nmemb, void *userp)
{
    size_t total = size * nmemb;

    if (g_response_len + total >= sizeof(g_response) - 1)
    {
        printf("[AI] 响应缓冲区溢出\n");
        return 0;
    }

    memcpy(g_response + g_response_len, contents, total);
    g_response_len += total;
    g_response[g_response_len] = '\0';

    return total;
}

static int build_prompt(char *buf, size_t buf_size,
                        FAR motor_params_t *motor, int usage)
{
    const char *usage_desc;

    switch (usage)
    {
        case FOC_USAGE_GIMBAL:
            usage_desc = "云台，低带宽，高稳定性";
            break;
        case FOC_USAGE_AERO:
            usage_desc = "航模，高响应";
            break;
        case FOC_USAGE_ROBOT:
            usage_desc = "机器人关节，平衡";
            break;
        default:
            usage_desc = "通用";
            break;
    }

    return snprintf(buf, buf_size,
        "{"
        "\"model\": \"%s\","
        "\"messages\": ["
        "  {"
        "    \"role\": \"system\","
        "    \"content\": \"你是电机控制专家。输出一个JSON对象，包含Kp_Id,Ki_Id,Kp_Iq,Ki_Iq,Kp_Speed,Ki_Speed六个浮点数。公式：omega_c=2*PI*开关频率/15, 电流环Kp=L*omega_c, Ki=R*omega_c, 速度环带宽=电流环/10，速度环Kp=电流环Kp/10, Ki=电流环Ki/10。只输出JSON，不要其他。\""
        "  },"
        "  {"
        "    \"role\": \"user\","
        "    \"content\": \"Rs=%.6f, Ld=%.6f, Lq=%.6f, Ke=%.6f, poles=%d, 用途=%s, 开关频率=16000Hz\""
        "  }"
        "],"
        "\"max_tokens\": 16384,"
        "\"temperature\": 0.0"
        "}",
        MIMO_MODEL,
        motor->Rs, motor->Ld, motor->Lq, motor->Ke, motor->poles,
        usage_desc);
}

/* 从 JSON 中提取 key 对应的 float 值 */
static float extract_json_float(const char *content, const char *key)
{
    const char *p = strstr(content, key);
    if (!p)
    {
        return 0.0f;
    }

    p = strchr(p, ':');
    if (!p)
    {
        return 0.0f;
    }

    return strtof(p + 1, NULL);
}

static int parse_ai_response(const char *response, FAR pi_params_t *pi)
{
    const char *p;
    const char *r;
    char *w;
    char content[8192];
    size_t clen = 0;

    /* 只从 "content" 字段提取 - 跳过 reasoning_content */
    p = strstr(response, "\"message\":{\"content\":\"");
    if (!p)
    {
        p = strstr(response, "\"content\":\"");
        if (p)
        {
            p += 11;
        }
    }
    else
    {
        p += 22; /* 跳过 "message":{"content":" (22 字符) */
    }

    if (!p)
    {
        printf("[AI] 未找到 content 字段\n");
        return -1;
    }

    /* 复制 content, 处理 JSON 转义 */
    r = p;
    w = content;
    while (*r && *r != '"' && clen < sizeof(content) - 1)
    {
        if (*r == '\\')
        {
            switch (r[1])
            {
                case 'n':  *w++ = '\n'; break;
                case 't':  *w++ = '\t'; break;
                case '"':  *w++ = '"';  break;
                case '\\': *w++ = '\\'; break;
                default:   *w++ = r[1]; break;
            }

            clen++;
            r += 2;
        }
        else
        {
            *w++ = *r++;
            clen++;
        }
    }

    *w = '\0';

    if (clen == 0)
    {
        printf("[AI] content 为空 (推理未完成)\n");
        return -1;
    }

    pi->Kp_Id    = extract_json_float(content, "Kp_Id");
    pi->Ki_Id    = extract_json_float(content, "Ki_Id");
    pi->Kp_Iq    = extract_json_float(content, "Kp_Iq");
    pi->Ki_Iq    = extract_json_float(content, "Ki_Iq");
    pi->Kp_Speed = extract_json_float(content, "Kp_Speed");
    pi->Ki_Speed = extract_json_float(content, "Ki_Speed");

    return (pi->Kp_Id > 0.0f) ? 0 : -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int foc_ai_tune(FAR motor_params_t *motor, int usage, FAR pi_params_t *pi)
{
    CURL *curl;
    CURLcode res;
    char prompt[2048];
    struct curl_slist *headers = NULL;
    long http_code = 0;
    int ret;

    g_response_len = 0;
    g_response[0] = '\0';

    build_prompt(prompt, sizeof(prompt), motor, usage);

    printf("[AI] 发送请求到 MiMo API...\n");
    printf("[AI] Rs=%.6f Ld=%.6f Lq=%.6f Ke=%.6f poles=%d usage=%d\n",
           motor->Rs, motor->Ld, motor->Lq, motor->Ke, motor->poles, usage);

    /* Temp diagnostic: resolve the API host directly through the NuttX
     * resolver (getaddrinfo is exactly what cURL uses).  This separates
     * "resolver / DNS / route" from "cURL / TLS". */
    {
      struct addrinfo hints;
      struct addrinfo *res = NULL;
      struct sockaddr_in *sa4;
      char ipbuf[INET_ADDRSTRLEN] = "?";
      char rl[128];
      FILE *rf;
      int g;

      rf = fopen("/tmp/resolv.conf", "r");
      if (rf)
        {
          while (fgets(rl, sizeof(rl), rf))
            {
              rl[strcspn(rl, "\n")] = '\0';
              printf("[AI-DNS] resolv: %s\n", rl);
            }
          fclose(rf);
        }
      else
        {
          printf("[AI-DNS] no /tmp/resolv.conf\n");
        }

      memset(&hints, 0, sizeof(hints));
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      g = getaddrinfo("token-plan-cn.xiaomimimo.com", NULL, &hints, &res);
      if (g == 0 && res)
        {
          sa4 = (struct sockaddr_in *)res->ai_addr;
          inet_ntop(AF_INET, &sa4->sin_addr, ipbuf, sizeof(ipbuf));
          printf("[AI-DNS] getaddrinfo OK -> %s\n", ipbuf);
          freeaddrinfo(res);
        }
      else
        {
          printf("[AI-DNS] getaddrinfo FAILED gai=%d errno=%d\n", g, errno);
        }
    }

    curl = curl_easy_init();
    if (!curl)
    {
        printf("[AI] curl_easy_init() 失败\n");
        return FOC_AI_ERR_HTTP;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers,
                                "Authorization: Bearer " MIMO_API_KEY);

    curl_easy_setopt(curl, CURLOPT_URL, MIMO_API_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, prompt);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_response_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, FOC_AI_HTTP_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, NULL);
    curl_easy_setopt(curl, CURLOPT_CAPATH, NULL);

    res = curl_easy_perform(curl);

    if (res != CURLE_OK)
    {
        printf("[AI] HTTP 请求失败: %s\n", curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return FOC_AI_ERR_HTTP;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    printf("[AI] HTTP 状态码: %ld\n", http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (http_code != 200)
    {
        printf("[AI] HTTP 错误: %ld\n", http_code);
        return FOC_AI_ERR_HTTP_CODE;
    }

    printf("[AI] 收到响应 (%zu 字节)\n", g_response_len);

    /* Persist the whole response to a file so the result survives the
     * serial console (the WiFi firmware floods it with RTL871x/ICMP logs).
     * Read it on the board with: cat /data/ai_result.txt */
    {
      FILE *rf = fopen("/data/ai_result.txt", "w");
      if (rf)
        {
          fprintf(rf, "http=%ld resp_len=%zu\n", http_code, g_response_len);
          fprintf(rf, "%s\n", g_response);
          fclose(rf);
          printf("[AI] wrote /data/ai_result.txt (%zu bytes)\n",
                 g_response_len);
        }
      else
        {
          printf("[AI] cannot open /data/ai_result.txt\n");
        }
    }

    ret = parse_ai_response(g_response, pi);
    if (ret != 0)
    {
        printf("==== AI PARSE FAILED ret=%d (see /data/ai_result.txt) ====\n",
               ret);
        return FOC_AI_ERR_PARSE;
    }

    /* 白名单校验 */
    if (foc_ai_validate(pi, motor) != 0)
    {
        printf("==== AI WHITELIST REJECTED ====\n");
        return FOC_AI_ERR_WHITELIST;
    }

    /* Append the parsed PID + OK status so the result is visible even when
     * the serial console is flooded by the WiFi firmware. */
    {
        FILE *rf = fopen("/data/ai_result.txt", "a");
        if (rf)
        {
            fprintf(rf,
                    "\nPARSE_OK\n"
                    "Kp_Id=%.6f Ki_Id=%.6f\n"
                    "Kp_Iq=%.6f Ki_Iq=%.6f\n"
                    "Kp_Speed=%.6f Ki_Speed=%.6f\n",
                    pi->Kp_Id, pi->Ki_Id, pi->Kp_Iq, pi->Ki_Iq,
                    pi->Kp_Speed, pi->Ki_Speed);
            fclose(rf);
        }
    }

    printf("====>> AI TUNE OK <<====\n");
    return FOC_AI_OK;
}

int foc_ai_validate(FAR pi_params_t *pi, FAR motor_params_t *motor)
{
    float wc = 2.0f * 3.14159f * 16000.0f / 15.0f;
    float kp_id_theory = motor->Ld * wc;
    float ki_id_theory = motor->Rs * wc;
    float kp_iq_theory = motor->Lq * wc;
    float ki_iq_theory = motor->Rs * wc;

    int ok = 1;

    if (pi->Kp_Id < kp_id_theory * 0.5f || pi->Kp_Id > kp_id_theory * 1.5f)
    {
        printf("[白名单] Kp_Id 越限: %.6f (理论值 %.6f)\n",
               pi->Kp_Id, kp_id_theory);
        ok = 0;
    }

    if (pi->Ki_Id < ki_id_theory * 0.5f || pi->Ki_Id > ki_id_theory * 1.5f)
    {
        printf("[白名单] Ki_Id 越限: %.6f (理论值 %.6f)\n",
               pi->Ki_Id, ki_id_theory);
        ok = 0;
    }

    if (pi->Kp_Iq < kp_iq_theory * 0.5f || pi->Kp_Iq > kp_iq_theory * 1.5f)
    {
        printf("[白名单] Kp_Iq 越限: %.6f (理论值 %.6f)\n",
               pi->Kp_Iq, kp_iq_theory);
        ok = 0;
    }

    if (pi->Ki_Iq < ki_iq_theory * 0.5f || pi->Ki_Iq > ki_iq_theory * 1.5f)
    {
        printf("[白名单] Ki_Iq 越限: %.6f (理论值 %.6f)\n",
               pi->Ki_Iq, ki_iq_theory);
        ok = 0;
    }

    return ok ? 0 : -1;
}
