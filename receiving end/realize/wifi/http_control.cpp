#include "http_control.hpp"
#include "spi_task.h"
#include "wifi_task.hpp"
#include "nv_recv.hpp"
#include "dlan.hpp"
#include "sle.hpp"

extern "C" {
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/inet.h"
#include "cJSON.h"
#include "securec.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>

// ======================== 文件作用域常量 ========================
static constexpr uint16_t k_http_port = 8080;
static constexpr int k_max_backlog = 4;
static constexpr size_t k_recv_buf_size = 1024;
static constexpr size_t k_resp_body_size = 512;
static constexpr int k_client_timeout_sec = 3;
// The relay server publishes the currently selected file at this stable URL.
static constexpr char k_relay_play_url[] = "http://124.222.12.152:18080/ws63-test.mp3";

static char s_http_send_buf[512];
static char s_http_body_buf[k_resp_body_size];
static char s_http_recv_buf[k_recv_buf_size];

// 停止标志（由 wifi_task 通过 http_control_request_stop() 设置）
static volatile bool s_stop_requested = false;

// ======================== 辅助函数 ========================
static const char *mode_name_str(uint8_t mode)
{
    switch (mode) {
        case SPI_MODE_WIREED:
            return "WIRED";
        case SPI_MODE_SLE:
            return "SLE";
        case SPI_MODE_DLNA:
            return "DLNA";
        case SPI_MODE_SLE_MIC:
            return "SLE_MIC";
        case SPI_MODE_DLNA_NET:
            return "DLNA_NET";
        default:
            return "UNKNOWN";
    }
}

static uint8_t tone_from_name(const char *tone)
{
    if (tone == nullptr) {
        return 0xFF;
    }
    if (strcmp(tone, "FLAT") == 0 || strcmp(tone, "flat") == 0) {
        return SPI_TONE_FLAT;
    }
    if (strcmp(tone, "VOCAL") == 0 || strcmp(tone, "vocal") == 0) {
        return SPI_TONE_VOCAL;
    }
    if (strcmp(tone, "BASS_BOOST") == 0 || strcmp(tone, "bass_boost") == 0 ||
        strcmp(tone, "BASS") == 0 || strcmp(tone, "bass") == 0) {
        return SPI_TONE_BASS_BOOST;
    }
    if (strcmp(tone, "POP") == 0 || strcmp(tone, "pop") == 0) {
        return SPI_TONE_POP;
    }
    if (strcmp(tone, "ROCK") == 0 || strcmp(tone, "rock") == 0) {
        return SPI_TONE_ROCK;
    }
    return 0xFF;
}

// 发送 HTTP JSON 响应并关闭连接
static void send_json(int sock, int http_code, int biz_code, const char *msg, const char *data_json)
{
    const char *body = data_json ? data_json : "{}";
    int len = snprintf(s_http_send_buf, sizeof(s_http_send_buf),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: application/json\r\n"
                       "Connection: close\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "\r\n"
                       "{\"code\":%d,\"msg\":\"%s\",\"data\":%s}",
                       http_code, (http_code == 200) ? "OK" : "Error", biz_code, msg, body);
    if (len > 0) {
        size_t send_len = (len < (int)sizeof(s_http_send_buf)) ? (size_t)len : (sizeof(s_http_send_buf) - 1);
        lwip_send(sock, s_http_send_buf, send_len, 0);
    }
}

// 从 HTTP 首行解析 METHOD 和 PATH
static bool parse_first_line(const char *buf, char *method, size_t method_sz, char *path, size_t path_sz)
{
    const char *sp1 = strchr(buf, ' ');
    if (!sp1)
        return false;

    size_t mlen = (size_t)(sp1 - buf);
    if (mlen >= method_sz)
        return false;
    memcpy(method, buf, mlen);
    method[mlen] = '\0';

    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2)
        return false;

    size_t plen = (size_t)(sp2 - (sp1 + 1));
    if (plen >= path_sz)
        return false;
    memcpy(path, sp1 + 1, plen);
    path[plen] = '\0';

    return true;
}

// 获取 HTTP body（\r\n\r\n 之后的内容）
static const char *get_body(const char *http_req)
{
    const char *p = strstr(http_req, "\r\n\r\n");
    return p ? (p + 4) : nullptr;
}

// 从 headers 中解析 Content-Length 值
static int parse_content_length(const char *buf)
{
    // 大小写不敏感搜索
    const char *cl = strstr(buf, "Content-Length:");
    if (!cl)
        cl = strstr(buf, "content-length:");
    if (!cl)
        return 0;

    cl += 15; // 跳过 "Content-Length:"
    while (*cl == ' ' || *cl == '\t')
        cl++;
    return atoi(cl);
}

// 循环读取直到收完完整的 HTTP 请求（headers + body）
// 返回 true=成功, false=超时/错误/缓冲区满
static bool recv_http_request(int sock, char *buf, size_t buf_size, size_t *out_len)
{
    size_t total = 0;

    // 设置客户端 socket 超时
    timeval tv = {k_client_timeout_sec, 0};
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 阶段 1: 循环读取直到收到 \r\n\r\n（headers 结束标志）
    while (total < buf_size - 1) {
        int n = lwip_recv(sock, buf + total, (int)(buf_size - 1 - total), 0);
        if (n <= 0) {
            osal_printk("[HTTP] recv headers failed: %d (got %u bytes)\r\n", n, (unsigned)total);
            return false;
        }
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") != nullptr)
            break;
    }

    if (total >= buf_size - 1) {
        osal_printk("[HTTP] headers too large\r\n");
        return false;
    }

    // 阶段 2: 解析 Content-Length，读取 body
    int content_length = parse_content_length(buf);
    if (content_length <= 0) {
        *out_len = total;
        return true; // GET 请求无 body，直接完成
    }

    const char *body_start = strstr(buf, "\r\n\r\n");
    if (!body_start) {
        return false;
    }
    body_start += 4;

    size_t body_received = total - (size_t)(body_start - buf);

    // 阶段 3: 循环读取剩余 body 字节
    while (body_received < (size_t)content_length && total < buf_size - 1) {
        int n = lwip_recv(sock, buf + total, (int)(buf_size - 1 - total), 0);
        if (n <= 0) {
            osal_printk("[HTTP] recv body failed: %d (got %u/%d bytes)\r\n", n, (unsigned)body_received,
                        content_length);
            return false;
        }
        total += (size_t)n;
        body_received += (size_t)n;
    }

    buf[total] = '\0';
    *out_len = total;
    return true;
}

// 从 JSON body 提取 int 字段
static bool json_get_int(const char *body, const char *key, int *out)
{
    if (!body)
        return false;
    cJSON *json = cJSON_Parse(body);
    if (!json)
        return false;
    cJSON *item = cJSON_GetObjectItem(json, key);
    bool ok = (item != nullptr) && cJSON_IsNumber(item);
    if (ok)
        *out = item->valueint;
    cJSON_Delete(json);
    return ok;
}

// 从 JSON body 提取 string 字段
static bool json_get_str(const char *body, const char *key, char *out, size_t out_sz)
{
    if (!body)
        return false;
    cJSON *json = cJSON_Parse(body);
    if (!json)
        return false;
    cJSON *item = cJSON_GetObjectItem(json, key);
    bool ok = (item != nullptr) && (item->valuestring != nullptr);
    if (ok) {
        size_t slen = strlen(item->valuestring);
        if (slen >= out_sz)
            slen = out_sz - 1;
        memcpy(out, item->valuestring, slen);
        out[slen] = '\0';
    }
    cJSON_Delete(json);
    return ok;
}

static bool json_get_bool(const char *body, const char *key, bool *out)
{
    if (!body)
        return false;
    cJSON *json = cJSON_Parse(body);
    if (!json)
        return false;
    cJSON *item = cJSON_GetObjectItem(json, key);
    bool ok = false;
    if (item != nullptr) {
        if (cJSON_IsBool(item)) {
            *out = cJSON_IsTrue(item);
            ok = true;
        } else if (cJSON_IsNumber(item)) {
            *out = (item->valueint != 0);
            ok = true;
        }
    }
    cJSON_Delete(json);
    return ok;
}

// ======================== 路由处理 ========================

// GET /api/v1/status
static void handle_status(int sock)
{
    const spi_settings_t *s = get_spi_settings();

    const char *ip = wifi_get_current_ip();
    const char *sta_ssid = wifi_get_current_ssid();
    const char *ap_ssid = wifi_get_current_ap_name();

    uint8_t hotspot = spi_get_hotspot(s->hotspot_network);
    uint8_t network = spi_get_network(s->hotspot_network);

    snprintf(s_http_body_buf, sizeof(s_http_body_buf),
             "{\"mode\":%u,\"mode_name\":\"%s\","
             "\"volume\":%u,\"bass\":%u,\"brightness\":%u,"
             "\"tone\":%u,\"tone_name\":\"%s\",\"night\":%s,\"sle_adpcm\":%s,\"sle_mono\":%s,"
             "\"hotspot\":\"%s\",\"network\":\"%s\","
             "\"wifi_ssid\":\"%s\",\"softap_ssid\":\"%s\","
             "\"dlna_name\":\"%s\",\"device_ip\":\"%s\"}",
             s->mode, mode_name_str(s->mode), s->volume, s->bass, s->brightness,
             s->tone, spi_tone_name(s->tone), spi_settings_is_night(s) ? "true" : "false",
              sle::adpcm_enabled() ? "true" : "false", sle::mono_enabled() ? "true" : "false",
             (hotspot == SPI_HOTSPOT_ON) ? "ON" : "OFF", (network == SPI_NETWORK_CONN) ? "CONNECTED" : "DISCONNECTED",
             sta_ssid, ap_ssid, dlan::friendly_name(), ip);
    send_json(sock, 200, 0, "ok", s_http_body_buf);
}

// POST /api/v1/mode  { "mode": 127 }
static void handle_set_mode(int sock, const char *body)
{
    int mode = -1;
    if (!json_get_int(body, "mode", &mode) || !spi_validate_mode((uint8_t)mode)) {
        send_json(sock, 400, 400, "invalid mode, allowed: 0,63,127,85,170", nullptr);
        return;
    }
    spi_settings_update_mode((uint8_t)mode);
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"mode\":%d,\"mode_name\":\"%s\"}", mode, mode_name_str((uint8_t)mode));
    send_json(sock, 200, 0, "ok", resp);
}

// POST /api/v1/play { "url": "source URL (optional, for logging)" }
// The source URL is not fetched by WS63. Playback always uses the relay file.
static void handle_play(int sock, const char *body)
{
    char source_url[256] = {0};
    const bool has_source_url = json_get_str(body, "url", source_url, sizeof(source_url));
    (void)has_source_url;

    // Ensure wifi_task starts the DLNA/minimp3 lifecycle before the request is
    // consumed. The URL is safe to set before minimp3_task itself runs.
    spi_settings_update_mode(SPI_MODE_DLNA);
    minimp3::play_url(k_relay_play_url);
    send_json(sock, 200, 0, "ok", "{\"playing\":true,\"url\":\"http://124.222.12.152:18080/ws63-test.mp3\"}");
}

// POST /api/v1/pause
static void handle_pause(int sock)
{
    minimp3::pause_playback();
    send_json(sock, 200, 0, "ok", "{\"paused\":true}");
}

// POST /api/v1/resume
static void handle_resume(int sock)
{
    minimp3::resume_playback();
    send_json(sock, 200, 0, "ok", "{\"playing\":true}");
}

// POST /api/v1/stop
static void handle_stop(int sock)
{
    minimp3::stop_playback();
    send_json(sock, 200, 0, "ok", "{\"stopped\":true}");
}

// POST /api/v1/seek { "seconds": 30 }
static void handle_seek(int sock, const char *body)
{
    int seconds = -1;
    if (!json_get_int(body, "seconds", &seconds) || seconds < 0) {
        send_json(sock, 400, 400, "invalid seconds", nullptr);
        return;
    }
    minimp3::seek_to_seconds(static_cast<uint32_t>(seconds));
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"seconds\":%d}", seconds);
    send_json(sock, 200, 0, "ok", resp);
}

// POST /api/v1/volume  { "volume": 75 }
static void handle_set_volume(int sock, const char *body)
{
    int vol = -1;
    if (!json_get_int(body, "volume", &vol) || !spi_validate_percent((uint8_t)vol)) {
        send_json(sock, 400, 400, "invalid volume, range: 0~100", nullptr);
        return;
    }
    spi_settings_update_volume((uint8_t)vol);
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"volume\":%d}", vol);
    send_json(sock, 200, 0, "ok", resp);
}

// POST /api/v1/bass  { "bass": 50 }
static void handle_set_bass(int sock, const char *body)
{
    int bass = -1;
    if (!json_get_int(body, "bass", &bass) || !spi_validate_percent((uint8_t)bass)) {
        send_json(sock, 400, 400, "invalid bass, range: 0~100", nullptr);
        return;
    }
    spi_settings_update_bass((uint8_t)bass);
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"bass\":%d}", bass);
    send_json(sock, 200, 0, "ok", resp);
}

// POST /api/v1/brightness  { "brightness": 80 }
static void handle_set_brightness(int sock, const char *body)
{
    int bri = -1;
    if (!json_get_int(body, "brightness", &bri) || !spi_validate_percent((uint8_t)bri)) {
        send_json(sock, 400, 400, "invalid brightness, range: 0~100", nullptr);
        return;
    }
    spi_settings_update_brightness((uint8_t)bri);
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"brightness\":%d}", bri);
    send_json(sock, 200, 0, "ok", resp);
}

// POST /api/v1/tone  { "tone": 2 } or { "tone": "BASS_BOOST" }
static void handle_set_tone(int sock, const char *body)
{
    int tone = -1;
    char tone_name[24] = {0};
    bool ok = false;
    if (json_get_int(body, "tone", &tone) && spi_validate_tone((uint8_t)tone)) {
        ok = true;
    } else if (json_get_str(body, "tone", tone_name, sizeof(tone_name))) {
        uint8_t parsed = tone_from_name(tone_name);
        if (spi_validate_tone(parsed)) {
            tone = parsed;
            ok = true;
        }
    }

    if (!ok) {
        send_json(sock, 400, 400, "invalid tone, allowed: 0~4 or FLAT/VOCAL/BASS_BOOST/POP/ROCK", nullptr);
        return;
    }

    spi_settings_update_tone((uint8_t)tone);
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"tone\":%d,\"tone_name\":\"%s\"}", tone, spi_tone_name((uint8_t)tone));
    send_json(sock, 200, 0, "ok", resp);
}

// POST /api/v1/night  { "night": true }
static void handle_set_night(int sock, const char *body)
{
    bool night = false;
    if (!json_get_bool(body, "night", &night)) {
        send_json(sock, 400, 400, "missing or invalid night boolean", nullptr);
        return;
    }

    spi_settings_update_night(night ? 1 : 0);
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"night\":%s}", night ? "true" : "false");
    send_json(sock, 200, 0, "ok", resp);
}

// POST /api/v1/hotspot  { "action": "on" | "off" }
static void handle_hotspot(int sock, const char *body)
{
    char action[8] = {0};
    if (!json_get_str(body, "action", action, sizeof(action))) {
        send_json(sock, 400, 400, "missing 'action' field (on|off)", nullptr);
        return;
    }
    if (strcmp(action, "on") == 0) {
        spi_settings_update_hotspot_network(SPI_HOTSPOT_ON, SPI_NETWORK_DISC);
        send_json(sock, 200, 0, "ok", "{\"hotspot\":\"ON\"}");
    } else if (strcmp(action, "off") == 0) {
        spi_settings_update_hotspot_network(SPI_HOTSPOT_OFF, SPI_NETWORK_CONN);
        send_json(sock, 200, 0, "ok", "{\"hotspot\":\"OFF\"}");
    } else {
        send_json(sock, 400, 400, "action must be 'on' or 'off'", nullptr);
    }
}

// POST /api/v1/network  { "action": "connect" | "disconnect" }
static void handle_network(int sock, const char *body)
{
    char action[16] = {0};
    if (!json_get_str(body, "action", action, sizeof(action))) {
        send_json(sock, 400, 400, "missing 'action' field (connect|disconnect)", nullptr);
        return;
    }
    if (strcmp(action, "connect") == 0) {
        // Connecting STA is also the explicit way to leave SoftAP mode.
        spi_settings_update_hotspot_network(SPI_HOTSPOT_OFF, SPI_NETWORK_CONN);
        send_json(sock, 200, 0, "ok", "{\"network\":\"CONNECTING\"}");
    } else if (strcmp(action, "disconnect") == 0) {
        spi_settings_update_hotspot_network(spi_get_hotspot(get_spi_settings()->hotspot_network), SPI_NETWORK_DISC);
        send_json(sock, 200, 0, "ok", "{\"network\":\"DISCONNECTED\"}");
    } else {
        send_json(sock, 400, 400, "action must be 'connect' or 'disconnect'", nullptr);
    }
}

// OPTIONS (CORS 预检)
static void handle_cors(int sock)
{
    const char *resp =
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n\r\n";
    lwip_send(sock, resp, strlen(resp), 0);
}

// ======================== WiFi 配置接口 ========================

// GET /api/v1/wifi — 返回 STA + SoftAP 配置（密码脱敏）
static void handle_get_wifi(int sock)
{
    wifi_sta_config_nv_t sta_cfg;
    softap_config_nv_t ap_cfg;
    nv_recv_read_sta(&sta_cfg);
    nv_recv_read_ap(&ap_cfg);

    snprintf(s_http_body_buf, sizeof(s_http_body_buf),
             "{\"sta\":{\"ssid\":\"%s\",\"has_pwd\":%s},"
             "\"ap\":{\"ssid\":\"%s\",\"has_pwd\":%s}}",
             (const char *)sta_cfg.ssid, (sta_cfg.password[0] != '\0') ? "true" : "false", (const char *)ap_cfg.ap_name,
             (ap_cfg.ap_password[0] != '\0') ? "true" : "false");
    send_json(sock, 200, 0, "ok", s_http_body_buf);
}

// POST /api/v1/wifi/sta  — 支持部分更新：{ "ssid":"..." } 或 { "password":"..." } 或两者
static void handle_set_sta(int sock, const char *body)
{
    char ssid[WIFI_NV_SSID_MAX_LEN] = {0};
    char pwd[WIFI_NV_PWD_MAX_LEN] = {0};
    bool has_ssid = json_get_str(body, "ssid", ssid, sizeof(ssid));
    bool has_pwd = json_get_str(body, "password", pwd, sizeof(pwd));

    if (!has_ssid && !has_pwd) {
        send_json(sock, 400, -1, "no field to update (ssid or password required)", nullptr);
        return;
    }

    if (has_ssid && (ssid[0] == '\0' || strlen(ssid) > (WIFI_NV_SSID_MAX_LEN - 1))) {
        send_json(sock, 400, 400, "ssid too long or empty (max 32 chars)", nullptr);
        return;
    }
    if (has_pwd && (pwd[0] == '\0' || strlen(pwd) > (WIFI_NV_PWD_MAX_LEN - 1))) {
        send_json(sock, 400, 400, "password too long or empty (max 64 chars)", nullptr);
        return;
    }

    // 部分更新：从 NV 读出现有值，只覆盖传了的字段，然后写回
    wifi_sta_config_nv_t cur;
    nv_recv_read_sta(&cur);
    if (has_ssid) {
        (void)strncpy_s((char *)cur.ssid, WIFI_NV_SSID_MAX_LEN, ssid, WIFI_NV_SSID_MAX_LEN - 1);
    }
    if (has_pwd) {
        (void)strncpy_s((char *)cur.password, WIFI_NV_PWD_MAX_LEN, pwd, WIFI_NV_PWD_MAX_LEN - 1);
    }
    nv_recv_write_sta((const char *)cur.ssid, (const char *)cur.password);

    // 同步更新内存凭据（供主循环下次连接使用）
    if (!wifi_update_sta_credentials((const char *)cur.ssid, (const char *)cur.password)) {
        send_json(sock, 400, 400, "incomplete or invalid STA credentials", nullptr);
        return;
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"sta\":{\"ssid\":\"%s\"},\"reconnect\":true}", (const char *)cur.ssid);
    send_json(sock, 200, 0, "ok, reconnect scheduled", resp);
}

// POST /api/v1/sle/adpcm  { "enabled": true }
static void handle_set_sle_adpcm(int sock, const char *body)
{
    const uint8_t mode = get_spi_settings()->mode;
    if (mode != SPI_MODE_SLE && mode != SPI_MODE_SLE_MIC) {
        send_json(sock, 409, 409, "SLE ADPCM can only be changed in SLE mode", nullptr);
        return;
    }

    bool enabled = false;
    if (!json_get_bool(body, "enabled", &enabled)) {
        send_json(sock, 400, 400, "missing or invalid enabled boolean", nullptr);
        return;
    }
    if (!sle::set_adpcm_enabled(enabled)) {
        send_json(sock, 500, 500, "failed to persist SLE ADPCM setting", nullptr);
        return;
    }

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"enabled\":%s}", enabled ? "true" : "false");
    send_json(sock, 200, 0, "ok", resp);
}

// POST /api/v1/sle/mono  { "enabled": true }
static void handle_set_sle_mono(int sock, const char *body)
{
    const uint8_t mode = get_spi_settings()->mode;
    if (mode != SPI_MODE_SLE && mode != SPI_MODE_SLE_MIC) {
        send_json(sock, 409, 409, "SLE mono can only be changed in SLE mode", nullptr);
        return;
    }

    bool enabled = false;
    if (!json_get_bool(body, "enabled", &enabled)) {
        send_json(sock, 400, 400, "missing or invalid enabled boolean", nullptr);
        return;
    }
    if (!sle::set_mono_enabled(enabled)) {
        send_json(sock, 500, 500, "failed to persist SLE mono setting", nullptr);
        return;
    }

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"enabled\":%s}", enabled ? "true" : "false");
    send_json(sock, 200, 0, "ok", resp);
}

// POST /api/v1/wifi/ap  — 支持部分更新：{ "ssid":"..." } 或 { "password":"..." } 或两者
static void handle_set_ap(int sock, const char *body)
{
    char name[WIFI_NV_SSID_MAX_LEN] = {0};
    char pwd[WIFI_NV_PWD_MAX_LEN] = {0};
    bool has_ssid = json_get_str(body, "ssid", name, sizeof(name));
    bool has_pwd = json_get_str(body, "password", pwd, sizeof(pwd));

    if (!has_ssid && !has_pwd) {
        send_json(sock, 400, -1, "no field to update (ssid or password required)", nullptr);
        return;
    }

    if (has_ssid && (name[0] == '\0' || strlen(name) > (WIFI_NV_SSID_MAX_LEN - 1))) {
        send_json(sock, 400, 400, "ssid too long or empty (max 32 chars)", nullptr);
        return;
    }
    // 密码校验：仅当传了 password 字段时才检查 ≥8 位
    if (has_pwd && strlen(pwd) < 8) {
        send_json(sock, 400, 400, "password must be at least 8 chars", nullptr);
        return;
    }
    if (has_pwd && strlen(pwd) > (WIFI_NV_PWD_MAX_LEN - 1)) {
        send_json(sock, 400, 400, "password too long (max 64 chars)", nullptr);
        return;
    }

    // 部分更新：从 NV 读出现有值，只覆盖传了的字段，然后写回
    softap_config_nv_t cur;
    nv_recv_read_ap(&cur);
    if (has_ssid) {
        (void)strncpy_s((char *)cur.ap_name, WIFI_NV_SSID_MAX_LEN, name, WIFI_NV_SSID_MAX_LEN - 1);
    }
    if (has_pwd) {
        (void)strncpy_s((char *)cur.ap_password, WIFI_NV_PWD_MAX_LEN, pwd, WIFI_NV_PWD_MAX_LEN - 1);
    }
    nv_recv_write_ap((const char *)cur.ap_name, (const char *)cur.ap_password);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ap\":{\"ssid\":\"%s\"}}", (const char *)cur.ap_name);
    send_json(sock, 200, 0, "ok, restart hotspot for new AP", resp);
}

// POST /api/v1/dlna/name  { "name":"Living Room Speaker" }
static void handle_set_dlna_name(int sock, const char *body)
{
    char name[DLNA_NV_NAME_MAX_LEN] = {0};
    if (!json_get_str(body, "name", name, sizeof(name)) || name[0] == '\0') {
        send_json(sock, 400, 400, "name required", nullptr);
        return;
    }
    if (strlen(name) > (DLNA_NV_NAME_MAX_LEN - 1)) {
        send_json(sock, 400, 400, "name too long (max 63 bytes)", nullptr);
        return;
    }
    // friendlyName 位于 XML 文本和 JSON 响应中，限制会破坏两种格式的字符。
    if (strpbrk(name, "<>&\"\\") != nullptr) {
        send_json(sock, 400, 400, "name contains unsupported characters", nullptr);
        return;
    }
    if (!nv_recv_write_dlna_name(name) || !dlan::set_friendly_name(name)) {
        send_json(sock, 500, 500, "failed to save DLNA name", nullptr);
        return;
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"dlna\":{\"name\":\"%s\"},\"rescan\":true}", name);
    send_json(sock, 200, 0, "ok, rescan DLNA devices", resp);
}

// ======================== 请求分发 ========================
static void dispatch(int sock, const char *method, const char *path, const char *body)
{

    if (strcmp(method, "OPTIONS") == 0) {
        handle_cors(sock);
        return;
    }

    // GET
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/api/v1/status") == 0) {
            handle_status(sock);
            return;
        }
        if (strcmp(path, "/api/v1/wifi") == 0) {
            handle_get_wifi(sock);
            return;
        }
        send_json(sock, 404, 404, "not found", nullptr);
        return;
    }

    // POST
    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/v1/play") == 0) {
            handle_play(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/pause") == 0) {
            handle_pause(sock);
            return;
        }
        if (strcmp(path, "/api/v1/resume") == 0) {
            handle_resume(sock);
            return;
        }
        if (strcmp(path, "/api/v1/stop") == 0) {
            handle_stop(sock);
            return;
        }
        if (strcmp(path, "/api/v1/seek") == 0) {
            handle_seek(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/mode") == 0) {
            handle_set_mode(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/volume") == 0) {
            handle_set_volume(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/bass") == 0) {
            handle_set_bass(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/brightness") == 0) {
            handle_set_brightness(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/tone") == 0) {
            handle_set_tone(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/night") == 0) {
            handle_set_night(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/sle/adpcm") == 0) {
            handle_set_sle_adpcm(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/sle/mono") == 0) {
            handle_set_sle_mono(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/hotspot") == 0) {
            handle_hotspot(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/network") == 0) {
            handle_network(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/wifi/sta") == 0) {
            handle_set_sta(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/wifi/ap") == 0) {
            handle_set_ap(sock, body);
            return;
        }
        if (strcmp(path, "/api/v1/dlna/name") == 0) {
            handle_set_dlna_name(sock, body);
            return;
        }
        send_json(sock, 404, 404, "not found", nullptr);
        return;
    }

    // 不支持的 METHOD
    send_json(sock, 405, 405, "method not allowed", nullptr);
}

// ======================== 公开接口 ========================
void http_control_request_stop(void)
{
    s_stop_requested = true;
}

void http_control_reset_stop(void)
{
    s_stop_requested = false;
}

void *http_control_task(void *arg)
{
    (void)arg;
    s_stop_requested = false;
    osal_printk("[HTTP] task started, port=%u\r\n", k_http_port);

    int32_t listen_sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        osal_printk("[HTTP] socket() failed\r\n");
        return nullptr;
    }

    int opt = 1;
    lwip_setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(k_http_port);
    addr.sin_addr.s_addr = IPADDR_ANY;

    if (lwip_bind(listen_sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
        osal_printk("[HTTP] bind() failed\r\n");
        lwip_close(listen_sock);
        return nullptr;
    }

    if (lwip_listen(listen_sock, k_max_backlog) < 0) {
        osal_printk("[HTTP] listen() failed\r\n");
        lwip_close(listen_sock);
        return nullptr;
    }

    // 设置 accept 超时 1 秒，以便检查 s_stop_requested
    timeval tv = {1, 0};
    lwip_setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!s_stop_requested) {
        sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        int32_t client_sock = lwip_accept(listen_sock, (sockaddr *)&client_addr, &client_len);

        if (client_sock < 0) {
            continue; // 超时或临时错误，继续循环
        }

        memset(s_http_recv_buf, 0, sizeof(s_http_recv_buf));
        size_t recv_len = 0;

        if (recv_http_request(client_sock, s_http_recv_buf, sizeof(s_http_recv_buf), &recv_len)) {
            char method[8] = {0};
            char path[64] = {0};
            if (parse_first_line(s_http_recv_buf, method, sizeof(method), path, sizeof(path))) {
                dispatch(client_sock, method, path, get_body(s_http_recv_buf));
            } else {
                const char *err = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
                lwip_send(client_sock, err, strlen(err), 0);
            }
        } else {
            // recv 超时或错误
            const char *err = "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n";
            lwip_send(client_sock, err, strlen(err), 0);
        }

        lwip_close(client_sock);
    }

    lwip_close(listen_sock);
    osal_printk("[HTTP] task exited\r\n");
    return nullptr;
}
