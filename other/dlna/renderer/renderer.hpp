#pragma once

extern "C" {
#include "lwip/netif.h"
#include "netinet/in.h"
}

#include "dlna_types/dlna_types.hpp"
#include "event/event.hpp"
#include "playback_bridge/playback_bridge.hpp"
#include "soap/control_router.hpp"
#include "ssdp/ssdp.hpp"
#include "tcp.hpp"

namespace sed_ws63 {

class renderer {
public:
    explicit renderer(playback_bridge &bridge);
    ~renderer();

    renderer(const renderer &) = delete;
    renderer &operator=(const renderer &) = delete;

    errcode_t run();

private:
    playback_bridge &bridge_;
    ssdp ssdp_;
    tcp_listener http_listener_;
    control_router router_;
    subscription_manager subscriptions_;
    notify_sender notifier_;
    dlna_renderer_state state_;
    char local_ip_[16] = {0};

    static constexpr uint16_t http_port_ = 49152;
    static constexpr const char *udn_ = "uuid:20260321-1612-2007-0423-a1b2c3d4e5f6";

    errcode_t wait_valid_ip_(char *out_ip, size_t out_size);
    errcode_t handle_http_client_(int32_t client_fd, const sockaddr_in &peer_addr);
    errcode_t send_initial_notify_if_needed_(const dlna_control_response &resp);
    errcode_t send_state_notify_if_needed_(const dlna_renderer_state &previous_state,
                                           const char *request,
                                           int http_status);
};

} // namespace sed_ws63