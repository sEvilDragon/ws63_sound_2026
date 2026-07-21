#pragma once

namespace ws63_relay {

// The relay publishes every accepted source URL at this stable playback URL.
const char *fixed_play_url();

// Submit a source URL to the relay server.  A true result means the relay
// accepted the request (HTTP 202); the download itself continues in the
// server background worker.
bool submit_source_url(const char *source_url);

} // namespace ws63_relay
