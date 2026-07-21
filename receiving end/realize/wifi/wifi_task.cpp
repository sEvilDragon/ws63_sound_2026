#include "wifi_task.hpp"
#include "spi_task.h"
#include "udp.hpp"
#include "discover_broadcast.hpp"
#include "nv_recv.hpp"
#include "relay_client.hpp"
#include "audio_play.hpp"

extern "C" {
#include "cJSON.h"
#include "wifi_device.h"
}

namespace {
static constexpr int k_queue_hard_high = static_cast<int>(iis::buffer_num) - 6;
static constexpr int k_queue_soft_high = static_cast<int>(iis::buffer_num) - 10;
static constexpr int k_queue_target_low = 6;
static constexpr int k_queue_emergency_low = 2;
static constexpr int k_wait_slice_ms = 2;
static constexpr int k_max_wait_loops = 160;
static constexpr uint32_t k_minimp3_task_stack_size = 0xA000;
static constexpr uint8_t k_wifi_mac_len = 6;
static constexpr uint32_t k_dlna_stop_wait_ms = 2500;
static constexpr uint32_t k_discover_stop_wait_ms = 4000;
static constexpr uint32_t k_http_stop_wait_ms = 2000;
// Explicit provisioning reconnect: one initial attempt plus three retries.
static constexpr int k_sta_reconnect_retry_count = 3;
static constexpr int k_sta_reconnect_attempts = 1 + k_sta_reconnect_retry_count;
static constexpr uint32_t k_sta_reconnect_retry_delay_ms = 1000;
// Temporary network-failure test switch. While enabled, every boot overwrites
// the saved STA credentials and starts one bounded connection cycle with a
// deliberately nonexistent network.
// Disable this after the provisioning/retry tests, otherwise a reboot will
// replace credentials submitted by the mini program again.
static constexpr bool k_force_invalid_sta_credentials_for_test = false;
static constexpr char k_invalid_test_ssid[] = "WS63_TEST_INVALID_AP";
static constexpr char k_invalid_test_password[] = "wrong_password_2026";

// Fixed locally administered unicast MAC addresses for the receiving end.
// Keep STA and SoftAP different to avoid conflicts if both interface addresses are observed.
static constexpr int8_t k_fixed_sta_mac[k_wifi_mac_len] = {0x02, 0x63, 0x26, 0x20, 0x00, 0x01};
static constexpr int8_t k_fixed_softap_mac[k_wifi_mac_len] = {0x02, 0x63, 0x26, 0x20, 0x00, 0x02};

void print_wifi_mac(const char *label, const int8_t *mac)
{
    osal_printk("[WiFi] %s MAC=%02x:%02x:%02x:%02x:%02x:%02x\r\n", label,
                static_cast<unsigned int>(static_cast<uint8_t>(mac[0])),
                static_cast<unsigned int>(static_cast<uint8_t>(mac[1])),
                static_cast<unsigned int>(static_cast<uint8_t>(mac[2])),
                static_cast<unsigned int>(static_cast<uint8_t>(mac[3])),
                static_cast<unsigned int>(static_cast<uint8_t>(mac[4])),
                static_cast<unsigned int>(static_cast<uint8_t>(mac[5])));
}

void apply_fixed_wifi_macs()
{
    errcode_t ret = wifi_set_base_mac_addr(k_fixed_sta_mac, k_wifi_mac_len);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[WiFi] set fixed STA MAC failed: %u\r\n", ret);
    } else {
        int8_t sta_mac[k_wifi_mac_len] = {0};
        if (wifi_get_base_mac_addr(sta_mac, k_wifi_mac_len) == ERRCODE_SUCC) {
            print_wifi_mac("fixed STA", sta_mac);
        } else {
            print_wifi_mac("fixed STA", k_fixed_sta_mac);
        }
    }

    ret = wifi_softap_set_mac_addr(k_fixed_softap_mac, k_wifi_mac_len);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[WiFi] set fixed SoftAP MAC failed: %u\r\n", ret);
    } else {
        int8_t softap_mac[k_wifi_mac_len] = {0};
        if (wifi_softap_get_mac_addr(softap_mac, k_wifi_mac_len) == ERRCODE_SUCC) {
            print_wifi_mac("fixed SoftAP", softap_mac);
        } else {
            print_wifi_mac("fixed SoftAP", k_fixed_softap_mac);
        }
    }
}

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

    const spi_settings_t *s = get_spi_settings();
    iis::data_write(data, size, spi_settings_effective_volume(s), spi_settings_effective_bass(s));

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

static char provisioned_ssid[64] = "WS63_TEST_INVALID_AP";
static char provisioned_password[64] = "wrong_password_2026";
static bool has_credentials = true;

// 标志：本次 SoftAP 会话中是否已通过任意方式获得了凭据
static volatile bool g_cred_updated_this_session = false;
static volatile bool g_sta_reconnect_requested = false;

void wifi_notify_cred_updated(void)
{
    g_cred_updated_this_session = true;
    g_sta_reconnect_requested = true;
}

// 本机 IP（STA 连接后由 dhcp 分配），供外部查询
static char g_sta_ip[16] = "0.0.0.0";
static bool g_sta_connected_flag = false;
static volatile bool g_relay_playback_ready = false;
static volatile bool g_dlna_tasks_running_flag = false;

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

bool wifi_dlna_tasks_running(void)
{
    return g_dlna_tasks_running_flag;
}

const char *wifi_get_current_ap_name(void)
{
    // 从 NV 读取 SoftAP 名称（高频调用，直接读 NV）
    static softap_config_nv_t ap_cfg;
    nv_recv_read_ap(&ap_cfg);
    // 若 NV 为空，返回默认值
    if (ap_cfg.ap_name[0] == '\0') {
        return "ws63_softap";
    }
    return (const char *)ap_cfg.ap_name;
}

bool wifi_update_sta_credentials(const char *ssid, const char *password)
{
    // 支持部分更新：任一参数为 NULL 表示不更新该字段
    if (ssid == NULL && password == NULL)
        return false;

    if (ssid != NULL) {
        size_t sl = strlen(ssid);
        if (sl == 0 || sl >= sizeof(provisioned_ssid))
            return false;
        memcpy(provisioned_ssid, ssid, sl + 1);
    }
    if (password != NULL) {
        size_t pl = strlen(password);
        if (pl == 0 || pl >= sizeof(provisioned_password))
            return false;
        memcpy(provisioned_password, password, pl + 1);
    }
    // 向 NV 写入当前完整凭据
    if (provisioned_ssid[0] == '\0' || provisioned_password[0] == '\0') {
        osal_printk("[WiFi] credentials incomplete, reconnect not scheduled\r\n");
        return false;
    }
    has_credentials = true;
    nv_recv_write_sta(provisioned_ssid, provisioned_password);
    wifi_notify_cred_updated();
    osal_printk("[WiFi] credentials updated via API: ssid=%s\r\n", provisioned_ssid);
    return true;
}

void wifi_update_ap_config(const char *name, const char *password)
{
    if (name == NULL || password == NULL)
        return;
    nv_recv_write_ap(name, password);
    osal_printk("[WiFi] AP config updated via API: %s\r\n", name);
}

static void update_sta_ip(void)
{
    netif *iface = netifapi_netif_find_by_name("wlan0");
    if (iface) {
        uint32_t ip_host = lwip_ntohl(iface->ip_addr.u_addr.ip4.addr);
        snprintf(g_sta_ip, sizeof(g_sta_ip), "%u.%u.%u.%u", (ip_host >> 24) & 0xFF, (ip_host >> 16) & 0xFF,
                 (ip_host >> 8) & 0xFF, ip_host & 0xFF);
    }
}

static void clear_sta_ip(void)
{
    (void)snprintf(g_sta_ip, sizeof(g_sta_ip), "0.0.0.0");
}

void *dlna_task(void *arg)
{
    (void)arg;
    static dlan dlan_;
    osal_printk("[DLNA] task started\r\n");

    g_relay_playback_ready = false;
    dlan_.register_media_set_uri_handler([](const char *source_url) -> bool {
        // The stable URL is reused for every song, so explicitly discard the
        // old session before submitting the new source URL to the relay.
        g_relay_playback_ready = false;
        minimp3::stop_playback();
        minimp3::clear_playback_url();

        if (!ws63_relay::submit_source_url(source_url)) {
            osal_printk("[DLNA] relay request failed\r\n");
            return false;
        }

        minimp3::prepare_url(ws63_relay::fixed_play_url());
        g_relay_playback_ready = true;
        osal_printk("[DLNA] relay source accepted; waiting on fixed URL\r\n");
        return true;
    });
    dlan_.register_media_play_handler([](const char *uri) -> bool {
        if (minimp3::get_is_paused()) {
            if (!g_relay_playback_ready) {
                return false;
            }
            minimp3::resume_playback();
            return true;
        }
        if (!g_relay_playback_ready || uri == nullptr || uri[0] == '\0')
            return false;
        (void)uri;
        minimp3::play_url(ws63_relay::fixed_play_url());
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
    apply_fixed_wifi_macs();
    // 由 wifi_task 统一执行有限重试；不启用 SDK 的后台无限/额外重连。
    sta_.disable_auto_reconnect();
    osal_printk("[WiFi] subsystem ready\r\n");

    if (k_force_invalid_sta_credentials_for_test) {
        osal_printk("[WiFi] TEST: overwriting STA NV with invalid credentials\r\n");
        nv_recv_write_sta(k_invalid_test_ssid, k_invalid_test_password);
        (void)snprintf(provisioned_ssid, sizeof(provisioned_ssid), "%s", k_invalid_test_ssid);
        (void)snprintf(provisioned_password, sizeof(provisioned_password), "%s", k_invalid_test_password);
        has_credentials = true;
        // A failure test must actually enter the bounded STA policy even if a
        // previous failed run persisted network=OFF in NV.
        spi_settings_update_hotspot_network(SPI_HOTSPOT_OFF, SPI_NETWORK_CONN);
        osal_printk("[WiFi] TEST credentials active: ssid=%s (password hidden)\r\n", provisioned_ssid);
        osal_printk("[WiFi] TEST: forcing STA connection attempt\r\n");
    } else {
        // Use the most recently submitted NV credentials; retain the compiled
        // preset only when the NV item is empty or invalid.
        wifi_sta_config_nv_t saved_sta = {};
        osal_printk("[WiFi] reading STA credentials from NV\r\n");
        nv_recv_read_sta(&saved_sta);
        if (saved_sta.ssid[0] != '\0' && saved_sta.password[0] != '\0') {
            (void)snprintf(provisioned_ssid, sizeof(provisioned_ssid), "%s", (const char *)saved_sta.ssid);
            (void)snprintf(provisioned_password, sizeof(provisioned_password), "%s", (const char *)saved_sta.password);
            has_credentials = true;
            osal_printk("[WiFi] loaded STA credentials from NV: ssid=%s\r\n", provisioned_ssid);
        } else {
            osal_printk("[WiFi] no valid STA credentials in NV; using preset ssid=%s\r\n", provisioned_ssid);
        }
    }

    bool softap_active = false;
    bool dlna_running = false;
    bool sta_connected = false;
    bool http_ctrl_running = false;
    bool discover_running = false;
    osal_task *dlna_handle = nullptr;
    osal_task *mp3_handle = nullptr;
    osal_task *http_ctrl_handle = nullptr;
    osal_task *discover_handle = nullptr;
    uint8_t last_hotspot_network = 0xFF;
    uint8_t last_mode = 0xFF;

    auto stop_dlna = [&]() {
        if (!dlna_running) {
            return;
        }
        dlan::request_stop();
        minimp3::request_exit();
        minimp3::stop_playback();
        osal_msleep(k_dlna_stop_wait_ms);
        dlna_handle = nullptr;
        mp3_handle = nullptr;
        dlna_running = false;
        g_dlna_tasks_running_flag = false;
        osal_printk("[WiFi] DLNA stopped\r\n");
    };

    auto stop_discover = [&]() {
        if (!discover_running) {
            return;
        }
        discover_broadcast_request_stop();
        osal_msleep(k_discover_stop_wait_ms);
        discover_handle = nullptr;
        discover_running = false;
    };

    auto stop_http = [&]() {
        if (!http_ctrl_running) {
            return;
        }
        http_control_request_stop();
        osal_msleep(k_http_stop_wait_ms);
        http_ctrl_handle = nullptr;
        http_ctrl_running = false;
    };

    auto disconnect_sta = [&]() {
        sta_.sta_disconnect();
        sta_connected = false;
        g_sta_connected_flag = false;
        clear_sta_ip();
    };

    auto connect_sta_with_retries = [&]() -> bool {
        sed_ws63::stacredential cred;
        memset(&cred, 0, sizeof(cred));
        (void)snprintf(cred.ssid, sizeof(cred.ssid), "%s", provisioned_ssid);
        (void)snprintf(cred.password, sizeof(cred.password), "%s", provisioned_password);

        // 所有 STA 建链都使用这条有限重试策略，避免 SDK 自动重连与本策略叠加。
        sta_.disable_auto_reconnect();
        for (int attempt = 1; attempt <= k_sta_reconnect_attempts; ++attempt) {
            sta_.sta_disconnect();
            osal_printk("[WiFi] reconnect attempt %d/%d, ssid=%s\r\n", attempt, k_sta_reconnect_attempts,
                        provisioned_ssid);
            errcode_t ret = sta_.sta_connect(cred);
            if (ret == ERRCODE_SUCC && sta_.is_connected()) {
                sta_connected = true;
                g_sta_connected_flag = true;
                update_sta_ip();
                osal_printk("[WiFi] STA connected, IP=%s\r\n", g_sta_ip);
                return true;
            }

            sta_connected = false;
            g_sta_connected_flag = false;
            clear_sta_ip();
            osal_printk("[WiFi] reconnect attempt %d failed: %u\r\n", attempt, ret);
            if (attempt < k_sta_reconnect_attempts) {
                osal_msleep(k_sta_reconnect_retry_delay_ms);
            }
        }

        // 三次重试仍失败：彻底关闭 STA，并由调用方把 network 状态置为 DISC。
        sta_.sta_disconnect();
        sta_.disable_auto_reconnect();
        sta_connected = false;
        g_sta_connected_flag = false;
        clear_sta_ip();
        return false;
    };

    while (true) {
        const spi_settings_t *s = get_spi_settings();
        bool want_hotspot = (spi_get_hotspot(s->hotspot_network) == SPI_HOTSPOT_ON);
        bool want_network = (spi_get_network(s->hotspot_network) == SPI_NETWORK_CONN);
        uint8_t mode = s->mode;

        if (s->hotspot_network != last_hotspot_network || mode != last_mode) {
            osal_printk("[WiFi] intent: hotspot=%s network=%s mode=%u credentials=%s\r\n",
                        want_hotspot ? "ON" : "OFF", want_network ? "ON" : "OFF",
                        (unsigned)mode, has_credentials ? provisioned_ssid : "NONE");
            last_hotspot_network = s->hotspot_network;
            last_mode = mode;
        }

        // 小程序提交新凭据后，无论原先是 STA、SoftAP 还是断网状态，都走同一
        // 条显式重连路径。SoftAP 分支会先退出并在下一轮来到这里。
        if (g_sta_reconnect_requested && !softap_active) {
            g_sta_reconnect_requested = false;
            osal_printk("[WiFi] provisioning credentials received, restarting network\r\n");

            // 配网动作本身应当重新开启联网意图，同时保证 STA 与 SoftAP 不并存。
            spi_settings_update_hotspot_network(SPI_HOTSPOT_OFF, SPI_NETWORK_CONN);
            stop_dlna();
            stop_discover();
            stop_http();
            disconnect_sta();

            if (!connect_sta_with_retries()) {
                spi_settings_update_hotspot_network(SPI_HOTSPOT_ON, SPI_NETWORK_DISC);
                osal_printk("[WiFi] provisioning reconnect failed after initial try + %d retries; enabling SoftAP\r\n",
                            k_sta_reconnect_retry_count);
            }
            g_cred_updated_this_session = false;
            continue;
        }

        // ===== SOFTAP (hotspot) MODE =====
        if (want_hotspot && !softap_active) {
            // SoftAP 和 STA 互斥：一旦进入热点态，立即把网络意图改为断开。
            spi_settings_update_hotspot_network(SPI_HOTSPOT_ON, SPI_NETWORK_DISC);
            if (dlna_running) {
                dlan::request_stop();
                minimp3::request_exit();
                minimp3::stop_playback();
                osal_msleep(k_dlna_stop_wait_ms);
                dlna_handle = nullptr;
                mp3_handle = nullptr;
                dlna_running = false;
                g_dlna_tasks_running_flag = false;
            }
            if (discover_running) {
                discover_broadcast_request_stop();
                osal_msleep(k_discover_stop_wait_ms);
                discover_handle = nullptr;
                discover_running = false;
            }
            if (sta_connected) {
                sta_.sta_disconnect();
                sta_connected = false;
                g_sta_connected_flag = false;
                clear_sta_ip();
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
            g_cred_updated_this_session = false; // 新 SoftAP 会话，重置标志

            // 启动 HTTP 服务器，供小程序握手 + 配置 WiFi（与 UDP 共存作为备用）
            if (!http_ctrl_running) {
                http_control_reset_stop();
                http_ctrl_handle =
                    osal_kthread_create((osal_kthread_handler)http_control_task, NULL, "http_ctrl", 4096);
                if (http_ctrl_handle) {
                    http_ctrl_running = true;
                    osal_printk("[WiFi] HTTP started for SoftAP mode\r\n");
                }
            }

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

                osal_printk("[WiFi] SoftAP ready, HTTP:8080 + UDP:20261\r\n");

                bool got_cred = false;
                while (want_hotspot && !got_cred) {
                    // 检查是否已通过 HTTP 在本会话中配置了凭据
                    if (g_cred_updated_this_session || g_sta_reconnect_requested) {
                        osal_printk("[WiFi] credentials set via HTTP, leaving SoftAP for reconnect\r\n");
                        got_cred = true;
                        break;
                    }

                    uint8_t buf[512] = {0};
                    sockaddr_in sender = {};
                    int32_t len = udp_server.receive_udp(buf, sizeof(buf) - 1, &sender);
                    want_hotspot = (spi_get_hotspot(get_spi_settings()->hotspot_network) == SPI_HOTSPOT_ON);
                    if (len < 100) {
                        if (!want_hotspot)
                            break;
                        osal_msleep(100);
                        continue;
                    }

                    cJSON *json = cJSON_Parse((const char *)buf);
                    bool accepted = false;
                    if (json != nullptr) {
                        cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
                        cJSON *pwd_item = cJSON_GetObjectItem(json, "password");
                        if (cJSON_IsString(ssid_item) && cJSON_IsString(pwd_item)) {
                            accepted = wifi_update_sta_credentials(ssid_item->valuestring, pwd_item->valuestring);
                        }
                        cJSON_Delete(json);
                    }

                    const char *ack = accepted
                                          ? "{\"code\":0,\"msg\":\"credentials_received\",\"reconnect\":true}"
                                          : "{\"code\":400,\"msg\":\"invalid_credentials\"}";
                    (void)udp_server.send_udp((const uint8_t *)ack, (uint32_t)strlen(ack), sender);
                    if (accepted) {
                        got_cred = true;
                        osal_printk("[WiFi] provisioned: SSID=%s, reconnect scheduled\r\n", provisioned_ssid);
                    }
                }

                udp_server.close_udp();
                break;
            }

            ap.deinit();
            softap_active = false;
            osal_printk("[WiFi] SoftAP stopped\r\n");

            // 新凭据统一由下一轮的显式重连状态机处理，避免 SoftAP 路径只尝试一次。
            continue;
        }

        // ===== STA MODE =====
        if (!want_network) {
            // network=DISC 后不再使用凭据自动重试；只有下一次配网或显式打开网络
            // 才允许重新建立 STA。
            stop_dlna();
            stop_discover();
            stop_http();
            if (sta_connected || sta_.is_connected()) {
                disconnect_sta();
            }
            osal_msleep(500);
            continue;
        }

        if (!sta_connected && has_credentials) {
            osal_printk("[WiFi] network enabled, starting unified STA retry policy\r\n");
            if (!connect_sta_with_retries()) {
                spi_settings_update_hotspot_network(SPI_HOTSPOT_ON, SPI_NETWORK_DISC);
                osal_printk("[WiFi] STA failed after initial try + %d retries; enabling SoftAP\r\n",
                            k_sta_reconnect_retry_count);
            }
            continue;
        }

        if (sta_connected && !sta_.is_connected()) {
            osal_printk("[WiFi] STA disconnected\r\n");
            sta_connected = false;
            g_sta_connected_flag = false;
            clear_sta_ip();
        }

        // ===== UDP DISCOVER BROADCAST LIFECYCLE =====
        // STA 连接后广播设备信息到 :20262，供小程序自动发现
        if (sta_connected && !discover_running) {
            discover_broadcast_reset_stop();
            discover_handle =
                osal_kthread_create((osal_kthread_handler)discover_broadcast_task, NULL, "discover", 4096);
            discover_running = true;
            osal_printk("[WiFi] discover broadcast started on port 20262\r\n");
        }

        if (!sta_connected && discover_running) {
            osal_printk("[WiFi] stopping discover broadcast\r\n");
            discover_broadcast_request_stop();
            osal_msleep(k_discover_stop_wait_ms);
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
            osal_msleep(k_http_stop_wait_ms);
            http_ctrl_handle = nullptr;
            http_ctrl_running = false;
        }

        // ===== DLNA LIFECYCLE =====
        bool want_dlna = (mode == SPI_MODE_DLNA || mode == SPI_MODE_DLNA_NET);

        if (want_dlna && sta_connected && !dlna_running) {
            if (audio_sle_task_running()) {
                // audio_play_task clears this only after SLE teardown and its
                // release grace period have both completed.
                osal_msleep(100);
                continue;
            }
            dlan::reset_stop();
            minimp3::reset_exit();
            g_dlna_tasks_running_flag = true;
            dlna_handle = osal_kthread_create((osal_kthread_handler)dlna_task, NULL, "dlna_task", 8192);
            mp3_handle = osal_kthread_create((osal_kthread_handler)minimp3_task, NULL, "minimp3_task", k_minimp3_task_stack_size);
            if (dlna_handle != nullptr && mp3_handle != nullptr) {
                dlna_running = true;
                osal_printk("[WiFi] DLNA started\r\n");
            } else {
                dlan::request_stop();
                minimp3::request_exit();
                minimp3::stop_playback();
                osal_msleep(k_dlna_stop_wait_ms);
                dlna_handle = nullptr;
                mp3_handle = nullptr;
                g_dlna_tasks_running_flag = false;
                osal_printk("[WiFi] DLNA task creation failed\r\n");
            }
        }

        if (!want_dlna && dlna_running) {
            osal_printk("[WiFi] stopping DLNA\r\n");
            dlan::request_stop();
            minimp3::request_exit();
            minimp3::stop_playback();
            osal_msleep(k_dlna_stop_wait_ms);
            dlna_handle = nullptr;
            mp3_handle = nullptr;
            dlna_running = false;
            g_dlna_tasks_running_flag = false;
            osal_printk("[WiFi] DLNA stopped\r\n");
        }

        if (dlna_running && !sta_connected) {
            osal_printk("[WiFi] WiFi lost, stopping DLNA\r\n");
            dlan::request_stop();
            minimp3::request_exit();
            minimp3::stop_playback();
            osal_msleep(k_dlna_stop_wait_ms);
            dlna_handle = nullptr;
            mp3_handle = nullptr;
            dlna_running = false;
            g_dlna_tasks_running_flag = false;
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
