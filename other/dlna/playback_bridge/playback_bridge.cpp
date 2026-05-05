#include "playback_bridge.hpp"

namespace sed_ws63 {

void playback_bridge::set_uri_handler_set(set_uri_handler handler)
{
    set_uri_handler_ = handler;
}

void playback_bridge::play_handler_set(simple_handler handler)
{
    play_handler_ = handler;
}

void playback_bridge::pause_handler_set(simple_handler handler)
{
    pause_handler_ = handler;
}

void playback_bridge::stop_handler_set(simple_handler handler)
{
    stop_handler_ = handler;
}

errcode_t playback_bridge::dispatch(const dlna_playback_command &cmd)
{
    switch (cmd.type) {
        case dlna_playback_command_type::set_uri:
            return set_uri(cmd.uri, cmd.metadata);
        case dlna_playback_command_type::play:
            return play();
        case dlna_playback_command_type::pause:
            return pause();
        case dlna_playback_command_type::stop:
            return stop();
        default:
            return ERRCODE_FAIL;
    }
}

errcode_t playback_bridge::set_uri(const char *uri, const char *metadata)
{
    if (set_uri_handler_ == nullptr) {
        return ERRCODE_FAIL;
    }
    return set_uri_handler_(uri, metadata);
}

errcode_t playback_bridge::play()
{
    return (play_handler_ == nullptr) ? ERRCODE_FAIL : play_handler_();
}

errcode_t playback_bridge::pause()
{
    return (pause_handler_ == nullptr) ? ERRCODE_FAIL : pause_handler_();
}

errcode_t playback_bridge::stop()
{
    return (stop_handler_ == nullptr) ? ERRCODE_FAIL : stop_handler_();
}

} // namespace sed_ws63