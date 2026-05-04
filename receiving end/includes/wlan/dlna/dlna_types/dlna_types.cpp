#include "dlna_types.hpp"

namespace sed_ws63 {

    const char * dlna_renderer_state::transport_state_text()
    {
        switch (state) {
        case dlna_transport_state::stopped:
            return "STOPPED";
        case dlna_transport_state::transitioning:
            return "TRANSITIONING";
        case dlna_transport_state::playing:
            return "PLAYING";
        case dlna_transport_state::paused_playback:
            return "PAUSED_PLAYBACK";
        case dlna_transport_state::no_media_present:
            return "NO_MEDIA_PRESENT";
        default:
            return "STOPPED";
        }
    }

}