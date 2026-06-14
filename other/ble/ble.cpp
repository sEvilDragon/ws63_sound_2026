#include "ble.hpp"

// ============================================================
// 静态成员初始化
// ============================================================
bt::data_process_t bt::data_process = nullptr;
bt::data_clear_t bt::data_clear = nullptr;
td_pvoid bt::current_stream_hdl = nullptr;
bool bt::is_acl_connected = false;
bool bt::is_audio_streaming = false;
uint8_t bt::preferred_codec = BT_AUDIO_CODEC_SBC; // 默认 SBC

// ============================================================
// 外部数据注入接口
// ============================================================
void bt::set_data_process_function(data_process_t callback)
{
    data_process = callback;
}

void bt::set_data_clear_function(data_clear_t callback)
{
    data_clear = callback;
}

// ============================================================
// 解码格式设置
// ============================================================
void bt::set_codec_preference(uint8_t codec_type)
{
    preferred_codec = codec_type;
    const char *name = (codec_type == BT_AUDIO_CODEC_SBC)      ? "SBC"
                       : (codec_type == BT_AUDIO_CODEC_MPEG24) ? "AAC"
                                                               : "UNKNOWN";
    osal_printk("[BT] codec preference set to: %s (0x%02X)\n", name, codec_type);
}

uint8_t bt::get_codec_preference()
{
    return preferred_codec;
}

// 根据解码类型构建 bt_a2dp_codec_param
static void build_codec_param(uint8_t codec_type, bt_a2dp_codec_param *param)
{
    (void)param;
    param->codec_type = codec_type;
    param->payload = 0; // 由协议栈自动协商

    if (codec_type == BT_AUDIO_CODEC_SBC) {
        // SBC 能力集：48k/44.1k 采样率，立体声，所有块长，子带 4/8，Loudness
        param->cap_len = 4;
        param->codec_caps[0] = BT_AUDIO_A2DP_SBC_SF_48000 | BT_AUDIO_A2DP_SBC_SF_44100;
        param->codec_caps[1] = BT_AUDIO_A2DP_SBC_CHMODE_STEREO | BT_AUDIO_A2DP_SBC_CHMODE_JOINT_STEREO;
        param->codec_caps[2] = BT_AUDIO_A2DP_SBC_BLOCK_4 | BT_AUDIO_A2DP_SBC_BLOCK_8 | BT_AUDIO_A2DP_SBC_BLOCK_12 |
                               BT_AUDIO_A2DP_SBC_BLOCK_16 | BT_AUDIO_A2DP_SBC_SUBBAND_4 | BT_AUDIO_A2DP_SBC_SUBBAND_8 |
                               BT_AUDIO_A2DP_SBC_LOUDNESS;
        param->codec_caps[3] = BT_AUDIO_A2DP_SBC_MINBITPOOL; // min_bitpool = 2
        param->codec_caps[4] = BT_AUDIO_A2DP_SBC_MAXBITPOOL; // max_bitpool = 68
        param->cap_len = 5;
    } else if (codec_type == BT_AUDIO_CODEC_MPEG24) {
        // AAC 能力集：48k/44.1k，立体声，VBR，MPEG4_LC + MPEG2_LC
        param->cap_len = 6;
        // Object Type byte: MPEG4_LC | MPEG2_LC
        param->codec_caps[0] = BT_AUDIO_A2DP_AAC_MPEG4_LC | BT_AUDIO_A2DP_AAC_MPEG2_LC;
        // Sample Frequency (2 bytes, big-endian): 48000 | 44100
        uint16_t sf = BT_AUDIO_A2DP_AAC_SF48000 | BT_AUDIO_A2DP_AAC_SF44100;
        param->codec_caps[1] = (sf >> 8) & 0xFF;
        param->codec_caps[2] = sf & 0xFF;
        // Channels: stereo (2)
        param->codec_caps[3] = BT_AUDIO_A2DP_AAC_CH_2;
        // VBR
        param->codec_caps[4] = BT_AUDIO_A2DP_AAC_VBR;
        // Bitrate (4 bytes): 320000 bps
        uint32_t br = 320000;
        param->codec_caps[5] = (br >> 24) & 0xFF;
        param->codec_caps[6] = (br >> 16) & 0xFF;
        param->codec_caps[7] = (br >> 8) & 0xFF;
        param->codec_caps[8] = br & 0xFF;
        param->cap_len = 9;
    }
}

// ============================================================
// 构造函数 — 注册所有回调 + 使能 BT 协议栈
// ============================================================
bt::bt()
{
    // GAP 回调
    gap_call_backs_t gap_cbs = {0};
    gap_cbs.state_change_callback = bt_stack_state_cb;
    gap_cbs.acl_state_changed_callbak = acl_state_changed_cb;
    gap_cbs.pair_requested_callback = pair_requested_cb;
    gap_cbs.pair_confiremed_callback = pair_confirmed_cb;
    gap_cbs.pair_status_changed_callback = pair_status_changed_cb;
    // 注意：is_accept_conn_on_safe_mode_callback 留空，SDK 默认自动接受配对

    gap_register_callbacks(&gap_cbs);

    // AVRCP Target 连接状态回调（avrcp_tg_callbacks_t 仅含 conn_state_changed_cb）
    avrcp_tg_callbacks_t avrcp_cbs = {0};
    avrcp_cbs.conn_state_changed_cb = nullptr; // AVRCP 连接状态变化暂时不处理
    avrcp_tg_register_callbacks(&avrcp_cbs);

    // 使能 BT 协议栈
    errcode_t ret = enable_bt_stack();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[BT] enable_bt_stack failed, err=%u\n", ret);
    } else {
        osal_printk("[BT] enable_bt_stack success\n");
    }
}

// ============================================================
// 初始化步骤
// ============================================================
void bt::setup_local_device()
{
    // 设置本地设备名称
    errcode_t ret = bluetooth_set_local_name(reinterpret_cast<const unsigned char *>(device_name), device_name_len);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[BT] set_local_name failed, err=%u\n", ret);
    }

    // 设置本地设备地址
    uint8_t mac[6];
    mac[0] = local_addr[0];
    mac[1] = local_addr[1];
    mac[2] = local_addr[2];
    mac[3] = local_addr[3];
    mac[4] = local_addr[4];
    mac[5] = local_addr[5];
    ret = bluetooth_set_local_addr(mac, 6);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[BT] set_local_addr failed, err=%u\n", ret);
    }

    osal_printk("[BT] local device: %s, addr %02X:%02X:%02X:%02X:%02X:%02X\n", device_name, mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
}

void bt::setup_scan_mode()
{
    // 设置为可连接 + 通用可发现模式，手机蓝牙设置中可直接搜索到
    bool ret = gap_br_set_bt_scan_mode(GAP_SCAN_MODE_CONNECTABLE_GENERAL_DISCOVERABLE, scan_duration);
    if (!ret) {
        osal_printk("[BT] set_scan_mode failed\n");
    } else {
        osal_printk("[BT] scan mode: connectable + general discoverable\n");
    }
}

void bt::setup_audio_listener()
{
    // 注册音频监听器 — 这是启动 A2DP Sink 角色的关键
    // 注册后协议栈自动：注册 SDP record、设置 CoD、准备好接受 A2DP Source 连接
    td_u32 ret = bt_register_audio_listener(audio_event_cb, nullptr);
    if (ret != 0) {
        osal_printk("[BT] register_audio_listener failed, ret=%u\n", ret);
    } else {
        osal_printk("[BT] audio listener registered (A2DP Sink ready)\n");
    }
}

void bt::setup_avrcp()
{
    // 注册 AVRCP Target 媒体按键回调
    bt_avrcp_tg_bts_cbk avrcp_audio_cbs = {0};
    avrcp_audio_cbs.notify_pass_through_status_cbk = avrcp_passthrough_cb;
    // get_media_status_cbk 暂不注册
    bt_avrcp_tg_register_audio_cbk(&avrcp_audio_cbs);
    osal_printk("[BT] AVRCP target callbacks registered\n");
}

// ============================================================
// GAP 回调
// ============================================================
void bt::bt_stack_state_cb(const int transport, const int status)
{
    osal_printk("[BT] stack state changed: transport=%d, status=%d\n", transport, status);

    if (transport != BT_TRANSPORT_BR_EDR) {
        return;
    }

    if (status == BT_STACK_STATE_TURN_ON) {
        osal_printk("[BT] BR/EDR stack TURN_ON, initializing...\n");

        setup_local_device();
        setup_audio_listener();
        setup_avrcp();
        setup_scan_mode(); // 最后开启可发现，手机可搜索到
    } else if (status == BT_STACK_STATE_TURN_OFF) {
        osal_printk("[BT] BR/EDR stack TURN_OFF\n");
        is_acl_connected = false;
        is_audio_streaming = false;
        current_stream_hdl = nullptr;
    }
}

void bt::acl_state_changed_cb(const bd_addr_t *bd_addr, gap_acl_state_t state, unsigned int reason)
{
    osal_printk("[BT] ACL state: %d, addr %02X:%02X:%02X:%02X:%02X:%02X, reason=%u\n", state, bd_addr->addr[0],
                bd_addr->addr[1], bd_addr->addr[2], bd_addr->addr[3], bd_addr->addr[4], bd_addr->addr[5], reason);

    if (state == GAP_ACL_STATE_CONNECTED) {
        is_acl_connected = true;
        osal_printk("[BT] ACL connected\n");
    } else if (state == GAP_ACL_STATE_DISCONNECTED) {
        is_acl_connected = false;
        is_audio_streaming = false;
        current_stream_hdl = nullptr;
        osal_printk("[BT] ACL disconnected\n");

        // 断连后重新开启可发现模式，等待下一个手机连接
        setup_scan_mode();
    }
}

void bt::pair_requested_cb(const bd_addr_t *bd_addr)
{
    // ⛔ SDK 无 gap_br_confirm_pair() 函数，协议栈默认自动接受配对
    // 此回调仅为通知日志
    osal_printk("[BT] pair requested from %02X:%02X:%02X:%02X:%02X:%02X (auto-accept)\n", bd_addr->addr[0],
                bd_addr->addr[1], bd_addr->addr[2], bd_addr->addr[3], bd_addr->addr[4], bd_addr->addr[5]);
}

void bt::pair_confirmed_cb(const bd_addr_t *bd_addr, int req_type, int number)
{
    (void)bd_addr;
    osal_printk("[BT] pair confirmed: req_type=%d, passkey=%d\n", req_type, number);
}

void bt::pair_status_changed_cb(const bd_addr_t *bd_addr, int status)
{
    osal_printk("[BT] pair status: %d (0x%02X:%02X:%02X:%02X:%02X:%02X)\n", status, bd_addr->addr[0], bd_addr->addr[1],
                bd_addr->addr[2], bd_addr->addr[3], bd_addr->addr[4], bd_addr->addr[5]);
}

// ============================================================
// A2DP 音频流事件
// ============================================================
void bt::audio_event_cb(bt_audio_event_type type, const td_void *data, int32_t size, td_void *context)
{
    (void)context;
    (void)size;

    switch (type) {
        case BT_AUDIO_A2DP_STREAM_CREATE: {
            // data = stream handle (td_pvoid)
            current_stream_hdl = *(td_pvoid *)data;
            osal_printk("[BT] A2DP stream CREATE, hdl=%p\n", current_stream_hdl);

            // 流创建后立即设置解码格式偏好
            bt_a2dp_codec_param codec_param = {0};
            build_codec_param(preferred_codec, &codec_param);
            td_u32 ret = bt_set_audio_parameter(current_stream_hdl, BT_AUDIO_PARAM_A2DP_CODEC, &codec_param,
                                                sizeof(codec_param));
            if (ret != 0) {
                osal_printk("[BT] set codec param failed, ret=%u\n", ret);
            } else {
                osal_printk("[BT] codec preference applied: %s\n", (preferred_codec == BT_AUDIO_CODEC_SBC)      ? "SBC"
                                                                   : (preferred_codec == BT_AUDIO_CODEC_MPEG24) ? "AAC"
                                                                                                                : "?");
            }
            break;
        }

        case BT_AUDIO_A2DP_STREAM_OPENED: {
            // data = bt_audio_a2dp_stream_open_data *
            const bt_audio_a2dp_stream_open_data *open_data = static_cast<const bt_audio_a2dp_stream_open_data *>(data);
            current_stream_hdl = open_data->stream_hdl;
            osal_printk("[BT] A2DP stream OPENED, hdl=%p, mtu=%u, frame=%u, num=%u\n", current_stream_hdl,
                        open_data->stream_mtu, open_data->frame_size, open_data->num_frame);

            // 绑定音频端口（A2DP → I2S 硬件直通或共享内存）
            bt_audio_port_params port = {0};
            port.port_type = A2DP;
            port.share_mem_id = audio_share_mem_id; // 0 = I2S hardware direct

            td_u32 ret = bt_attach_audio_port(current_stream_hdl, &port);
            if (ret != 0) {
                osal_printk("[BT] attach_audio_port failed, ret=%u\n", ret);
            } else {
                osal_printk("[BT] audio port attached (share_mem_id=%u)\n", audio_share_mem_id);
            }
            break;
        }

        case BT_AUDIO_A2DP_STREAM_STRAT: {
            // data = stream handle
            current_stream_hdl = *(td_pvoid *)data;
            is_audio_streaming = true;
            osal_printk("[BT] A2DP stream START, hdl=%p\n", current_stream_hdl);
            break;
        }

        case BT_AUDIO_A2DP_STREAM_SUSPENDED: {
            // data = stream handle
            current_stream_hdl = *(td_pvoid *)data;
            is_audio_streaming = false;
            osal_printk("[BT] A2DP stream SUSPENDED, hdl=%p\n", current_stream_hdl);
            break;
        }

        case BT_AUDIO_A2DP_STREAM_CLOSED: {
            // data = stream handle
            td_pvoid stream = *(td_pvoid *)data;

            // 先解绑音频端口
            bt_audio_port_params port = {0};
            port.port_type = A2DP;
            port.share_mem_id = audio_share_mem_id;
            bt_detach_audio_port(stream, &port);

            // 再停止流
            bt_stop_audio_stream(stream);

            is_audio_streaming = false;
            current_stream_hdl = nullptr;
            osal_printk("[BT] A2DP stream CLOSED, hdl=%p\n", stream);
            break;
        }

        case BT_AUDIO_A2DP_STREAM_CONFIG_CHANGE: {
            // data = bt_audio_a2dp_config_chg_data *
            const bt_audio_a2dp_config_chg_data *cfg = static_cast<const bt_audio_a2dp_config_chg_data *>(data);
            osal_printk("[BT] A2DP config change: codec_type=%u\n", cfg->codec.codec_type);
            break;
        }

        default:
            osal_printk("[BT] A2DP unknown event: %d\n", type);
            break;
    }
}

// ============================================================
// AVRCP 媒体按键
// ============================================================
void bt::avrcp_passthrough_cb(td_u32 key_operation, td_u32 key_value)
{
    // 常见按键值：
    // 0x44 = PLAY, 0x46 = PAUSE, 0x45 = STOP
    // 0x4B = FORWARD, 0x4C = BACKWARD
    // 0x41 = VOLUME_UP, 0x42 = VOLUME_DOWN
    osal_printk("[BT] AVRCP passthrough: operation=0x%02X, value=%u\n", key_operation, key_value);
}
