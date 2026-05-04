#pragma once

/*
    sed_ws63: playback_bridge.hpp
    控制面板和播放端的桥梁，负责接收控制面板的命令，并将命令转发给播放端
*/

#include "wifi_tool.hpp"
#include "dlna_types/dlna_types.hpp"

namespace sed_ws63 {

class playback_bridge {
public:
    using set_uri_handler = errcode_t (*)(const char *uri, const char *metadata);
    using simple_handler = errcode_t (*)();

private:
    set_uri_handler set_uri_handler_ = nullptr;
    simple_handler play_handler_ = nullptr;
    simple_handler pause_handler_ = nullptr;
    simple_handler stop_handler_ = nullptr;

public:
    // 功能：设置URI处理函数
    void set_uri_handler_set(set_uri_handler handler);
    void play_handler_set(simple_handler handler);
    void pause_handler_set(simple_handler handler);
    void stop_handler_set(simple_handler handler);

    errcode_t dispatch(const dlna_playback_command &cmd);
    errcode_t set_uri(const char *uri, const char *metadata);
    errcode_t play();
    errcode_t pause();
    errcode_t stop();

private:
};
} // namespace sed_ws63