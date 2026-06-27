#include "wifi_task.hpp"
#include "spi_task.h"
#include "udp.hpp"
#include "discover_broadcast.hpp"

extern "C" {
#include "cJSON.h"
}

namespace {
static constexpr int k_queue_hard_high = static_cast<int>(iis::buffer_num) - 6;
static constexpr int k_queue_soft_high = static_cast<int>(iis::buffer_num) - 10;
static constexpr int k_queue_target_low = 6;
static constexpr int k_queue_emergency_low = 2;
static constexpr int k_wait_slice_ms = 2;
static constexpr int k_max_wait_loops = 160;

void push_pcm_with_closed_loop(const int16_t *data, uint32_t size)
{
    if (data == nullptr || size == 0)
        return;

    int queue_level = static_cast<int>(iis::pending_frames);

    int wait_loops = 0;
    while (queue_level >= k_queue_hard_high && wait_loops < k_max_wait_loops) {
        osal_msleep(k_wait_slice_ms);
        ++wait_loops;
        queue_level = static_cast<int>(iis::pending_frames);
    }

    if (queue_level >= k_queue_soft_high) {
        osal_msleep(1);
    }

    iis::data_write(data, size, get_spi_settings()->volume, get_spi_settings()->bass);

    queue_level = static_cast<int>(iis::pending_frames);
    if (queue_level <= k_queue_emergency_low) {
        iis::fill_buffer_if_needed();
    } else if (queue_level <= k_queue_target_low) {
        iis::fill_buffer_if_needed();
    }
}

} // namespace

// ======================== 跨 TU 共享的 WiFi 状态 ========================
// 由 wifi_task 主循环维护，供 http_control / discover_broadcast 读取

static char provisioned_ssid[64] = "OPPO Find X8 972E";
static char provisioned_password[64] = "mytc4386";
static bool has_credentials = true;

// 本机 IP（STA 连接后由 dhcp 分配），供外部查询
static char g_sta_ip[16] = "0.0.0.0";
static bool g_sta_connected_flag = false;

const char *wifi_get_current_ssid(void)
{
    return provisioned_ssid;
}

const char *wifi_get_current_ip(void)
{
    return g_sta_ip;
}

bool wifi_is_sta_connected(void)
{
    return g_sta_connected_flag;
}

static void update_sta_ip(void)
{
    netif *iface = netifapi_netif_find_by_name("wlan0");
    if (iface) {
        uint32_t ip_host = lwip_ntohl(iface->ip_addr.u_addr.ip4.addr);
        snprintf(g_sta_ip, sizeof(g_sta_ip), "%u.%u.%u.%u",
                 (ip_host >> 24) & 0xFF, (ip_host >> 16) & 0xFF,
                 (ip_host >> 8) & 0xFF, ip_host & 0xFF);
    }
}

void *dlna_task(void *arg)
{
    (void)arg;
    static dlan dlan_;
    osal_printk("[DLNA] task started\r\n");

    dlan_.register_media_set_uri_handler(minimp3::prepare_url);
    dlan_.register_media_play_handler([](const char *uri) -> bool {
        if (minimp3::get_is_paused()) {
            minimp3::resume_playback();
            return true;
        }
        if (uri == nullptr || uri[0] == '\0')
            return false;
        minimp3::play_url(uri);
        return true;
    });
    dlan_.register_media_pause_handler([]() { minimp3::pause_playback(); });
    dlan_.register_media_stop_handler([]() { minimp3::stop_playback(); });
    dlan_.register_media_seek_handler([](uint32_t seconds) { minimp3::seek_to_seconds(seconds); });

    dlan_.is_ready_set(true);
    dlan_.ssdp_and_http_scan();

    osal_printk("[DLNA] task exited\r\n");
    return nullptr;
}

void *wifi_task(void *arg)
{
    unused(arg);
    osal_printk("[WiFi] task started\r\n");

    static sed_ws63::sta sta_;

    while (wifi_is_wifi_inited() == 0) {
        osal_msleep(100);
    }
    sta_.enable_auto_reconnect();
    osal_printk("[WiFi] subsystem ready\r\n");

    bool softap_active = false;
    bool dlna_running = false;
    bool sta_connected = false;
    bool http_ctrl_running = false;
    bool discover_running = false;
    osal_task *dlna_handle = nullptr;
    osal_task *mp3_handle = nullptr;
    osal_task *http_ctrl_handle = nullptr;
    osal_task *discover_handle = nullptr;

    while (true) {
        const spi_settings_t *s = get_spi_settings();
        bool want_hotspot = (spi_get_hotspot(s->hotspot_network) == SPI_HOTSPOT_ON);
        uint8_t mode = s->mode;

        // ===== SOFTAP (hotspot) MODE =====
        if (want_hotspot && !softap_active) {
            if (dlna_running) {
                dlan::request_stop();
                minimp3::request_exit();
                minimp3::stop_playback();
                osal_msleep(2000);
                dlna_handle = nullptr;
                mp3_handle = nullptr;
                dlna_running = false;
            }
            if (discover_running) {
                discover_broadcast_request_stop();
                osal_msleep(3500);
                discover_handle = nullptr;
                discover_running = false;
            }
            if (sta_connected) {
                sta_.sta_disconnect();
                sta_connected = false;
                g_sta_connected_flag = false;
            }

            osal_printk("[WiFi] starting SoftAP for provisioning\r\n");
            sed_ws63::softapconfig ap_config;
            sed_ws63::softap ap(ap_config);
            errcode_t ret = ap.init();
            if (ret != ERRCODE_SUCC) {
                osal_printk("[WiFi] SoftAP failed: %u, retry...\r\n", ret);
                osal_msleep(2000);
                continue;
            }
            softap_active = true;

            char bind_ip[16];
            snprintf(bind_ip, sizeof(bind_ip), "%u.%u.%u.%u", ap_config.ip[0], ap_config.ip[1], ap_config.ip[2],
                     ap_config.ip[3]);

            while (want_hotspot) {
                sed_ws63::udp udp_server;
                ret = udp_server.bind_udp(20261, bind_ip);
                if (ret != ERRCODE_SUCC) {
                    osal_printk("[WiFi] UDP bind failed\r\n");
                    osal_msleep(500);
                    want_hotspot = (spi_get_hotspot(get_spi_settings()->hotspot_network) == SPI_HOTSPOT_ON);
                    continue;
                }

                osal_printk("[WiFi] SoftAP ready, listening on %s:20261\r\n", bind_ip);

                bool got_cred = false;
                while (want_hotspot && !got_cred) {
                    uint8_t buf[512] = {0};
                    int32_t len = udp_server.receive_udp(buf, sizeof(buf));
                    want_hotspot = (spi_get_hotspot(get_spi_settings()->hotspot_network) == SPI_HOTSPOT_ON);
                    if (!want_hotspot)
                        break;
                    if (len <= 0) {
                        osal_msleep(100);
                        continue;
                    }

                    cJSON *json = cJSON_Parse((const char *)buf);
                    if (!json)
                        continue;

                    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
                    cJSON *pwd_item = cJSON_GetObjectItem(json, "password");
                    if (ssid_item && pwd_item && ssid_item->valuestring && pwd_item->valuestring) {
                        size_t sl = strlen(ssid_item->valuestring);
                        size_t pl = strlen(pwd_item->valuestring);
                        if (sl > 0 && sl < sizeof(provisioned_ssid) && pl > 0 && pl < sizeof(provisioned_password)) {
                            memcpy(provisioned_ssid, ssid_item->valuestring, sl + 1);
                            memcpy(provisioned_password, pwd_item->valuestring, pl + 1);
                            has_credentials = true;
                            got_cred = true;
                            osal_printk("[WiFi] provisioned: SSID=%s\r\n", provisioned_ssid);
                        }
                    }
                    cJSON_Delete(json);
                }

                udp_server.close_udp();
                break;
            }

            ap.deinit();
            softap_active = false;
            osal_printk("[WiFi] SoftAP stopped\r\n");

            if (has_credentials) {
                osal_printk("[WiFi] connecting STA to %s...\r\n", provisioned_ssid);
                sed_ws63::stacredential cred;
                memset(&cred, 0, sizeof(cred));
                snprintf(cred.ssid, sizeof(cred.ssid), "%s", provisioned_ssid);
                snprintf(cred.password, sizeof(cred.password), "%s", provisioned_password);
                errcode_t r = sta_.sta_connect(cred);
                if (r == ERRCODE_SUCC) {
                    sta_connected = true;
                    g_sta_connected_flag = true;
                    update_sta_ip();
                    osal_printk("[WiFi] STA connected, IP=%s\r\n", g_sta_ip);
                } else {
                    osal_printk("[WiFi] STA connect failed: %u\r\n", r);
                }
            }
            continue;
        }

        // ===== STA MODE =====
        if (!sta_connected && has_credentials) {
            osal_printk("[WiFi] connecting to %s...\r\n", provisioned_ssid);
            sed_ws63::stacredential cred;
            memset(&cred, 0, sizeof(cred));
            snprintf(cred.ssid, sizeof(cred.ssid), "%s", provisioned_ssid);
            snprintf(cred.password, sizeof(cred.password), "%s", provisioned_password);
            errcode_t r = sta_.sta_connect(cred);
            if (r == ERRCODE_SUCC) {
                sta_connected = true;
                g_sta_connected_flag = true;
                update_sta_ip();
                osal_printk("[WiFi] STA connected, IP=%s\r\n", g_sta_ip);
            } else {
                osal_printk("[WiFi] STA failed: %u, retry in 3s\r\n", r);
                osal_msleep(3000);
                continue;
            }
        }

        if (sta_connected && !sta_.is_connected()) {
            osal_printk("[WiFi] STA disconnected\r\n");
            sta_connected = false;
            g_sta_connected_flag = false;
        }

        // ===== UDP DISCOVER BROADCAST LIFECYCLE =====
        // STA 连接后广播设备信息到 :20262，供小程序自动发现
        if (sta_connected && !discover_running) {
            discover_broadcast_reset_stop();
            discover_handle = osal_kthread_create((osal_kthread_handler)discover_broadcast_task,
                                                  NULL, "discover", 4096);
            discover_running = true;
            osal_printk("[WiFi] discover broadcast started on port 20262\r\n");
        }

        if (!sta_connected && discover_running) {
            osal_printk("[WiFi] stopping discover broadcast\r\n");
            discover_broadcast_request_stop();
            osal_msleep(3500); // 等待 sleep 分段超时 + 任务退出
            discover_handle = nullptr;
            discover_running = false;
        }

        // ===== HTTP CONTROL SERVER LIFECYCLE =====
        // STA 连接后启动 HTTP 控制服务器，断开后停止
        // 独立于 DLNA：任何模式下只要 STA 在线，小程序均可控制
        if (sta_connected && !http_ctrl_running) {
            http_control_reset_stop();
            http_ctrl_handle = osal_kthread_create((osal_kthread_handler)http_control_task, NULL, "http_ctrl", 4096);
            http_ctrl_running = true;
            osal_printk("[WiFi] HTTP control server started on port 8080\r\n");
        }

        if (!sta_connected && http_ctrl_running) {
            osal_printk("[WiFi] stopping HTTP control server\r\n");
            http_control_request_stop();
            osal_msleep(1500); // 等待 accept 超时 + 任务退出
            http_ctrl_handle = nullptr;
            http_ctrl_running = false;
        }

        // ===== DLNA LIFECYCLE =====
        bool want_dlna = (mode == SPI_MODE_DLNA || mode == SPI_MODE_DLNA_NET);

        if (want_dlna && sta_connected && !dlna_running) {
            dlan::reset_stop();
            minimp3::reset_exit();
            dlna_handle = osal_kthread_create((osal_kthread_handler)dlna_task, NULL, "dlna_task", 8192);
            mp3_handle = osal_kthread_create((osal_kthread_handler)minimp3_task, NULL, "minimp3_task", 8192 * 4);
            dlna_running = true;
            osal_printk("[WiFi] DLNA started\r\n");
        }

        if (!want_dlna && dlna_running) {
            osal_printk("[WiFi] stopping DLNA\r\n");
            dlan::request_stop();
            minimp3::request_exit();
            minimp3::stop_playback();
            osal_msleep(2000);
            dlna_handle = nullptr;
            mp3_handle = nullptr;
            dlna_running = false;
            osal_printk("[WiFi] DLNA stopped\r\n");
        }

        if (dlna_running && !sta_connected) {
            osal_printk("[WiFi] WiFi lost, stopping DLNA\r\n");
            dlan::request_stop();
            minimp3::request_exit();
            minimp3::stop_playback();
            osal_msleep(2000);
            dlna_handle = nullptr;
            mp3_handle = nullptr;
            dlna_running = false;
        }

        if (sta_connected) {
            update_sta_ip(); // DHCP 租约可能变更 IP
        }

        osal_msleep(500);
    }

    return nullptr;
}

void *minimp3_task(void *arg)
{
    unused(arg);

    minimp3::iis_set_rate_set([](int rate) {
        if (rate == 48000)
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_48K);
        else if (rate == 44100)
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_44K);
        else if (rate == 32000)
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_32K);
        else if (rate == 24000)
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_24K);
        else if (rate == 22050)
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_22K);
        else if (rate == 16000)
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_16K);
        else if (rate == 12000)
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_12K);
        else if (rate == 11025)
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_11K);
        else if (rate == 8000)
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_8K);
        else
            osal_printk("[minimp3] unsupported rate: %d\r\n", rate);
    });
    minimp3::mp3_get_into_iis_set([](const int16_t *data, uint32_t size) { push_pcm_with_closed_loop(data, size); });
    minimp3::playback_queue_level_getter_set([]() -> int { return static_cast<int>(iis::pending_frames); });

    minimp3::stream_mp3_to_iis();
    osal_printk("[minimp3] task exited\r\n");
    return nullptr;
}
