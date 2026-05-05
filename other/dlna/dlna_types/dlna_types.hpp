#pragma once

/*
    sed_ws63:dlna_types.hpp
    dlna的一些类型定义
*/

extern "C" {
#include "common_def.h"
}

namespace sed_ws63 {

enum class dlna_service_kind : uint8_t {
    avtransport = 0,
    renderingcontrol = 1,
    connectionmanager = 2,
    unknown = 255,
};

enum class dlna_transport_state : uint8_t {
    stopped = 0,
    transitioning = 1,
    playing = 2,
    paused_playback = 3,
    no_media_present = 4,
};

enum class dlna_playback_command_type : uint8_t {
    set_uri = 0,
    play = 1,
    pause = 2,
    stop = 3,
};

class dlna_playback_command {
public:
    dlna_playback_command_type type = dlna_playback_command_type::set_uri;
    char uri[512] = {0};
    char metadata[512] = {0};
};

class dlna_subscribe_slot {
public:
    char sid[128] = {0};
    char callback_url[256] = {0};
    uint32_t seq = 0;
    uint32_t timeout_sec = 1800;
    bool is_valid = false;
};

class dlna_renderer_state {
public:
    char current_uri[512] = {0};
    char current_metadata[512] = {0};
    dlna_transport_state state = dlna_transport_state::stopped;
    uint8_t volume = 50;
    bool mute = false;
    uint32_t elapsed_base_sec = 0;
    // 节拍数
    unsigned long long playback_started_jiffies = 0;

public:
    const char *transport_state_text() const;
};

class dlna_control_response {
public:
    int http_status = 200;
    char status_text[32] = {"OK"};
    char content_type[64] = {"text/xml; charset=\"utf-8\""};
    char body[2048] = {0};
    bool send_initial_avtransport_notify = false;
    bool send_initial_rendering_notify = false;
};

} // namespace sed_ws63