#pragma once

// ============================================================
// WS63 经典蓝牙 (BR/EDR) A2DP Sink 接收端
// 目标：手机蓝牙设置直连，无需 App，作为蓝牙音箱使用
//
// 协议栈：
//   GAP (BR/EDR)  — 设备发现、ACL 连接、配对
//   A2DP Sink     — 音频流接收与解码 (SBC/AAC)
//   AVRCP Target  — 响应手机播放/暂停/音量控制
// ============================================================

extern "C" {
#include "bts_br_gap.h"
#include "bt_audio_hal_interface.h"
#include "bts_avrcp_target.h"
#include "bt_audio.h"
#include "errcode.h"
#include "soc_osal.h"
}
#include <array>

class bt {
public:
    bt(); // 构造函数：注册所有回调 + enable_bt_stack()

    // 外部数据注入接口（与 sle 保持相同风格，供手动共享内存路径使用）
    using data_process_t = void (*)(const int16_t *data, uint32_t length);
    using data_clear_t = void (*)();
    static void set_data_process_function(data_process_t callback);
    static void set_data_clear_function(data_clear_t callback);
    // 解码格式设置（必须在 STREAM_CREATE 之前调用，即 BT stack TURN_ON 之前）
    // 支持：BT_AUDIO_CODEC_SBC(0x00) / BT_AUDIO_CODEC_MPEG24(0x02) AAC
    static void set_codec_preference(uint8_t codec_type);
    static uint8_t get_codec_preference();
private:
    // ---- 初始化步骤（在 BT stack TURN_ON 回调中按序调用） ----
    static void setup_local_device();
    static void setup_scan_mode();
    static void setup_audio_listener();
    static void setup_avrcp();

    // ---- GAP 回调 ----
    static void bt_stack_state_cb(const int transport, const int status);
    static void acl_state_changed_cb(const bd_addr_t *bd_addr, gap_acl_state_t state, unsigned int reason);
    static void pair_requested_cb(const bd_addr_t *bd_addr);
    static void pair_confirmed_cb(const bd_addr_t *bd_addr, int req_type, int number);
    static void pair_status_changed_cb(const bd_addr_t *bd_addr, int status);

    // ---- A2DP 音频流事件 ----
    static void audio_event_cb(bt_audio_event_type type, const td_void *data, int32_t size, td_void *context);

    // ---- AVRCP 媒体按键 ----
    static void avrcp_passthrough_cb(td_u32 key_operation, td_u32 key_value);

    // ---- 数据回调指针（手动共享内存路径） ----
    static data_process_t data_process;
    static data_clear_t data_clear;

    // ---- 运行时状态 ----
    static td_pvoid current_stream_hdl;
    static bool    is_acl_connected;
    static bool    is_audio_streaming;
    static uint8_t preferred_codec;  // 用户设置的首选解码格式

    // ---- 配置常量 ----
    static constexpr const char *device_name = "WS63-Speaker";
    static constexpr uint8_t device_name_len = 12;
    static constexpr std::array<uint8_t, 6> local_addr = {0xA1, 0x11, 0x00, 0x03, 0x26, 0x20};
    static constexpr td_u32 audio_share_mem_id = 0; // 0 = I2S 硬件直通，非零 = 共享内存手动路径
    static constexpr int scan_duration = 0;         // 0 = 无限时可发现
};
