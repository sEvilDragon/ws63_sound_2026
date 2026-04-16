#define MINIMP3_IMPLEMENTATION
#include "minimp3.hpp"

namespace {
bool has_known_non_mp3_signature(const uint8_t *data, int len)
{
    if (data == nullptr || len < 4) {
        return false;
    }

    if (len >= 4 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
        return true;
    }
    if (len >= 4 && data[0] == 'O' && data[1] == 'g' && data[2] == 'g' && data[3] == 'S') {
        return true;
    }
    if (len >= 4 && data[0] == 'f' && data[1] == 'L' && data[2] == 'a' && data[3] == 'C') {
        return true;
    }
    if (len >= 8 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p') {
        return true;
    }

    // M3U/HLS 文本清单也会被误当成音频体，这里快速识别。
    if (len >= 7 && data[0] == '#' && data[1] == 'E' && data[2] == 'X' && data[3] == 'T' && data[4] == 'M' &&
        data[5] == '3' && data[6] == 'U') {
        return true;
    }

    // ADTS AAC 同步字，minimp3 无法解码。
    if (len >= 2 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0) {
        return true;
    }

    return false;
}

bool probe_mp3_frame_from_buffer(const uint8_t *data, int len, int *first_frame_offset)
{
    if (data == nullptr || len < 64) {
        return false;
    }

    int probe_start = 0;
    if (len >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        int tag_size = ((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) | ((data[8] & 0x7F) << 7) | (data[9] & 0x7F);
        probe_start = 10 + tag_size;
        if (probe_start >= len) {
            return false;
        }
    }

    const int probe_limit = (len < 3072) ? len : 3072;
    mp3dec_t probe_dec;
    mp3dec_init(&probe_dec);
    std::array<int16_t, MINIMP3_MAX_SAMPLES_PER_FRAME> probe_pcm = {0};
    mp3dec_frame_info_t info = {0};

    for (int off = probe_start; off < probe_limit - 4; ++off) {
        memset(&info, 0, sizeof(info));
        int samples = mp3dec_decode_frame(&probe_dec, data + off, len - off, probe_pcm.data(), &info);
        if (samples <= 0 || info.frame_bytes <= 0 || info.hz <= 0) {
            continue;
        }

        const int next_off = off + info.frame_bytes;
        if (next_off < len - 4) {
            mp3dec_frame_info_t info2 = {0};
            int samples2 = mp3dec_decode_frame(&probe_dec, data + next_off, len - next_off, probe_pcm.data(), &info2);
            if (samples2 > 0 && info2.frame_bytes > 0 && info2.hz > 0) {
                if (first_frame_offset != nullptr) {
                    *first_frame_offset = off;
                }
                return true;
            }
        }

        if (first_frame_offset != nullptr) {
            *first_frame_offset = off;
        }
        return true;
    }

    return false;
}

int parse_id3v2_tag_total_size_if_present(const uint8_t *data, int len)
{
    if (data == nullptr || len < 10) {
        return 0;
    }
    if (!(data[0] == 'I' && data[1] == 'D' && data[2] == '3')) {
        return 0;
    }
    // 仅接受常见ID3v2版本，降低误判概率。
    if (!((data[3] == 2) || (data[3] == 3) || (data[3] == 4))) {
        return 0;
    }

    // ID3v2 size is syncsafe integer in bytes 6..9, excludes 10-byte header.
    int payload_size = ((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) | ((data[8] & 0x7F) << 7) | (data[9] & 0x7F);
    int total = 10 + payload_size;

    // Footer present flag (bit4 of flags byte) adds 10 bytes.
    if ((data[5] & 0x10) != 0) {
        total += 10;
    }

    if (total <= 10 || total > len) {
        return 0;
    }
    return total;
}

int parse_http_status_code_from_header(const char *header)
{
    if (header == nullptr) {
        return -1;
    }

    // 期望首行为: HTTP/1.1 200 OK
    const char *p = strchr(header, ' ');
    if (p == nullptr) {
        return -1;
    }
    while (*p == ' ') {
        ++p;
    }
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9') {
        return -1;
    }
    return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
}

const char *classify_payload_prefix(const uint8_t *data, int len)
{
    if (data == nullptr || len <= 0) {
        return "empty";
    }
    if (len >= 3 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        return "id3";
    }
    if (len >= 4 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
        return "riff";
    }
    if (len >= 4 && data[0] == 'O' && data[1] == 'g' && data[2] == 'g' && data[3] == 'S') {
        return "ogg";
    }
    if (len >= 4 && data[0] == 'f' && data[1] == 'L' && data[2] == 'a' && data[3] == 'C') {
        return "flac";
    }
    if (len >= 8 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p') {
        return "mp4/iso-bmff";
    }
    if (len >= 7 && data[0] == '#' && data[1] == 'E' && data[2] == 'X' && data[3] == 'T' && data[4] == 'M' &&
        data[5] == '3' && data[6] == 'U') {
        return "m3u";
    }
    if (len >= 2 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0) {
        return "aac/adts";
    }
    if (len >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) {
        return "mp3-sync";
    }
    if (len >= 5 && data[0] == '<' && data[1] == 'h' && data[2] == 't' && data[3] == 'm' && data[4] == 'l') {
        return "html";
    }
    if (len >= 9 && data[0] == '<' && data[1] == '!' && data[2] == 'D' && data[3] == 'O' && data[4] == 'C') {
        return "html-doctype";
    }
    if (len >= 5 && data[0] == '{' && data[1] == '"') {
        return "json";
    }
    return "unknown";
}

void log_payload_preview(const char *tag, const uint8_t *data, int len, int max_bytes)
{
    if (tag == nullptr || data == nullptr || len <= 0 || max_bytes <= 0) {
        return;
    }

    int n = (len < max_bytes) ? len : max_bytes;
    std::array<char, 256> hex = {0};
    std::array<char, 96> asc = {0};
    int hex_pos = 0;
    int asc_pos = 0;
    for (int i = 0; i < n; ++i) {
        if (hex_pos + 4 < static_cast<int>(hex.size())) {
            hex_pos += snprintf(hex.data() + hex_pos, hex.size() - hex_pos, "%02X ", data[i]);
        }
        if (asc_pos + 2 < static_cast<int>(asc.size())) {
            unsigned char c = data[i];
            asc[asc_pos++] = (c >= 32 && c <= 126) ? static_cast<char>(c) : '.';
            asc[asc_pos] = '\0';
        }
    }
    osal_printk("%s: len=%d guess=%s hex=%s ascii=%s\n", tag, len, classify_payload_prefix(data, len), hex.data(),
                asc.data());
}

int find_mp3_sync_offset(const uint8_t *data, int len)
{
    if (data == nullptr || len < 4) {
        return -1;
    }

    for (int i = 0; i + 3 < len; ++i) {
        if (data[i] != 0xFF || (data[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        // 过滤保留值，降低误判：layer不能为00，bitrate/samplerate索引不能是保留值
        int layer_bits = (data[i + 1] >> 1) & 0x3;
        int bitrate_idx = (data[i + 2] >> 4) & 0xF;
        int sample_idx = (data[i + 2] >> 2) & 0x3;
        if (layer_bits == 0 || bitrate_idx == 0xF || sample_idx == 0x3) {
            continue;
        }
        return i;
    }
    return -1;
}
} // namespace

// 初始化静态成员变量
std::array<char, 512> minimp3::current_url = {0};
bool minimp3::is_playing = false;
bool minimp3::is_url_ready = false;
minimp3::iis_set_rate minimp3::iis_set_rate_func = nullptr;
minimp3::mp3_get_into_iis minimp3::mp3_get_into_iis_func = nullptr;
minimp3::playback_queue_level_getter minimp3::playback_queue_level_getter_func = nullptr;

void minimp3::iis_set_rate_set(iis_set_rate set_rate_func)
{
    iis_set_rate_func = set_rate_func;
}

void minimp3::mp3_get_into_iis_set(mp3_get_into_iis get_into_iis_func)
{
    mp3_get_into_iis_func = get_into_iis_func;
}

void minimp3::playback_queue_level_getter_set(playback_queue_level_getter getter_func)
{
    playback_queue_level_getter_func = getter_func;
}

void minimp3::prepare_url(const char *url)
{
    http_set_url(url, false);
}

void minimp3::play_url(const char *url)
{
    http_get_url(url);
}

void minimp3::stop_playback()
{
    http_stop();
}

void minimp3::clear_playback_url()
{
    http_clear_url();
}

void minimp3::http_set_url(const char *url, bool start_playback)
{
    if (url == nullptr || url[0] == '\0') {
        if (start_playback) {
            // 允许Play命令在已有URL场景下仅拉起播放开关。
            if (current_url[0] != '\0') {
                is_playing = true;
                if (!is_url_ready) {
                    is_url_ready = true;
                }
            }
        }
        return;
    }

    const bool is_same_url = (strncmp(current_url.data(), url, current_url.size()) == 0);
    if (!is_same_url) {
        copy_string_safe(current_url.data(), current_url.size(), url);
        is_url_ready = true;
    } else if (!is_playing && !is_url_ready) {
        // 同URL从暂停/停止恢复时，确保能重启拉流。
        is_url_ready = true;
    }

    if (start_playback) {
        is_playing = true;
    }
}

void minimp3::http_get_url(const char *url)
{
    http_set_url(url, true);
}

void minimp3::http_stop()
{
    // 更新状态
    is_playing = false;
    is_url_ready = false;
}

void minimp3::http_clear_url()
{
    // 清空URL数据
    memset(current_url.data(), 0, current_url.size());
    // 更新状态
    is_playing = false;
    is_url_ready = false;
}

void minimp3::stream_mp3_to_iis()
{
    // 网络播放任务
    mp3dec_t mp3d;
    mp3dec_frame_info_t info;
    static constexpr size_t k_mp3_buffer_chunk_size = 1024;
    static constexpr size_t k_mp3_buffer_chunk_count = 20;
    static_assert(k_mp3_buffer_chunk_size * k_mp3_buffer_chunk_count == mp3_buffer_size,
                  "mp3_buffer_size must equal 20 x 1KB");
    static std::array<std::array<uint8_t, k_mp3_buffer_chunk_size>, k_mp3_buffer_chunk_count> mp3_buffer_chunks = {0};
    uint8_t *mp3_buffer = mp3_buffer_chunks[0].data();

    int16_t *pcm_buffer = (int16_t *)osal_kmalloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                                  OSAL_GFP_KERNEL); // PCM缓冲区，预留足够空间
    if (pcm_buffer == nullptr) {
        osal_printk("pcm_buffer内存分配失败\n");
        return;
    }

    // 开启任务循环
    while (true) {
        // 未处于播放态时等待
        if (!is_playing) {
            osal_msleep(100);
            continue;
        }

        // 接管一次新的URL请求
        if (is_url_ready) {
            is_url_ready = false;
        }

        std::array<char, 512> working_url = {0};
        copy_string_safe(working_url.data(), working_url.size(), current_url.data());
        if (working_url[0] == '\0') {
            osal_msleep(100);
            continue;
        }

        int current_hz = 0;
        int hz_candidate = 0;
        int hz_candidate_count = 0;
        int rate_mismatch_streak = 0;
        int buf_start = 0;
        int bytes_in_buf = 0;
        mp3dec_init(&mp3d);

        simple_http_url parsed_url;
        if (!parse_http_url(working_url.data(), parsed_url)) {
            osal_printk("URL解析失败: %s\n", working_url.data());
            osal_msleep(300);
            continue;
        }

        // 创建socket连接到服务器
        int32_t sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            osal_printk("创建socket失败\n");
            osal_msleep(500);
            continue;
        }

        // 给首包和响应头更充足时间，避免网络抖动下误判失败。
        timeval timeout = {5, 0};
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        // 配置服务器地址
        sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = lwip_htons(parsed_url.port);
        if (!resolve_ipv4_addr(parsed_url.host.data(), &addr.sin_addr)) {
            osal_printk("无法解析主机地址: %s\n", parsed_url.host.data());
            lwip_close(sock);
            osal_msleep(300);
            continue;
        }

        if (lwip_connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
            osal_printk("连接服务器失败: %s\n", parsed_url.host.data());
            lwip_close(sock);
            osal_msleep(300);
            continue;
        }

        std::array<char, 640> request = {0};
        snprintf(request.data(), request.size(),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Accept: audio/mpeg, audio/mp3, audio/x-mpeg, application/octet-stream\r\n"
                 "Accept-Encoding: identity\r\n"
                 "Connection: close\r\n"
                 "Icy-MetaData: 0\r\n\r\n",
                 parsed_url.path.data(), parsed_url.host.data());

        if (lwip_send(sock, request.data(), strlen(request.data()), 0) <= 0) {
            osal_printk("发送HTTP请求失败\n");
            lwip_close(sock);
            osal_msleep(200);
            continue;
        }

        // 跳过HTTP响应头
        std::array<char, 1024> resp_header = {0};
        int32_t header_len = 0;
        bool header_ended = false;

        while (is_playing && !header_ended && header_len < (resp_header.size() - 1)) {
            int32_t want = static_cast<int32_t>(resp_header.size() - 1 - header_len);
            if (want > 64) {
                want = 64;
            }

            int32_t ret = lwip_recv(sock, resp_header.data() + header_len, want, 0);
            if (ret <= 0) {
                osal_printk("接收HTTP响应头失败\n");
                break;
            }
            header_len += ret;
            resp_header[header_len] = '\0';

            if (header_len >= 4 && strstr(resp_header.data(), "\r\n\r\n") != nullptr) {
                header_ended = true;
                break;
            }
        }
        if (!header_ended) {
            lwip_close(sock);
            osal_msleep(500);
            continue;
        }

        {
            char *status_end = strstr(resp_header.data(), "\r\n");
            if (status_end != nullptr) {
                char saved = *status_end;
                *status_end = '\0';
                osal_printk("HTTP状态行: %s\n", resp_header.data());
                *status_end = saved;
            }
        }
        std::array<char, 128> content_type = {0};
        std::array<char, 64> content_encoding = {0};
        std::array<char, 64> transfer_encoding = {0};
        std::array<char, 32> icy_metaint_text = {0};

        bool has_content_type =
            extract_http_header_value(resp_header.data(), "Content-Type", content_type.data(), content_type.size());
        bool has_content_encoding = extract_http_header_value(resp_header.data(), "Content-Encoding",
                                                              content_encoding.data(), content_encoding.size());
        bool has_transfer_encoding = extract_http_header_value(resp_header.data(), "Transfer-Encoding",
                                                               transfer_encoding.data(), transfer_encoding.size());
        bool has_icy_metaint = extract_http_header_value(resp_header.data(), "icy-metaint", icy_metaint_text.data(),
                                                         icy_metaint_text.size());
        bool is_chunked_transfer = false;
        int icy_metaint = 0;
        int icy_audio_remaining = 0;
        int icy_metadata_remaining = 0;

        // SED : 串口输出，打印HTTP响应头中的Content-Type和Transfer-Encoding，方便调试验证服务器响应的格式是否正确。
        if (has_content_type) {
            trim_ascii_whitespace(content_type.data());
            // SED : 串口输出，打印Content-Type，方便调试验证服务器响应的内容类型是否正确。
            osal_printk("HTTP Content-Type: %s\n", content_type.data());
        }

        if (has_transfer_encoding) {
            trim_ascii_whitespace(transfer_encoding.data());
            is_chunked_transfer = ascii_icontains(transfer_encoding.data(), "chunked");
            // SED : 串口输出，打印Transfer-Encoding，方便调试验证服务器响应的传输编码是否正确。
            osal_printk("HTTP Transfer-Encoding: %s\n", transfer_encoding.data());
        }

        if (has_content_encoding) {
            trim_ascii_whitespace(content_encoding.data());
            osal_printk("HTTP Content-Encoding: %s\n", content_encoding.data());
            if (ascii_icontains(content_encoding.data(), "gzip") ||
                ascii_icontains(content_encoding.data(), "deflate") || ascii_icontains(content_encoding.data(), "br")) {
                osal_printk("检测到压缩内容编码，当前版本不支持，停止本次播放\n");
                lwip_close(sock);
                is_playing = false;
                continue;
            }
        }

        if (has_icy_metaint) {
            trim_ascii_whitespace(icy_metaint_text.data());
            icy_metaint = atoi(icy_metaint_text.data());
            if (icy_metaint > 0) {
                icy_audio_remaining = icy_metaint;
                osal_printk("检测到ICY元数据: metaint=%d，已启用过滤\n", icy_metaint);
            }
        }

        // 当前接收逻辑仅支持连续字节流，不支持chunked分块体。
        if (is_chunked_transfer) {
            osal_printk("检测到chunked传输，当前版本不支持，停止本次播放\n");
            lwip_close(sock);
            is_playing = false;
            continue;
        }

        // 平衡超时与阻塞：避免timeout风暴，同时不过度拉长可闻空白。
        timeval stream_timeout = {0, 180000};
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &stream_timeout, sizeof(stream_timeout));

        int no_progress_count = 0;
        int recv_fail_count = 0;
        int recv_timeout_count = 0;
        static constexpr int k_max_recv_fail_count = 16;
        static constexpr int k_max_recv_timeout_count = 20;
        static constexpr int k_compact_threshold = 512;
        static constexpr int k_max_decode_loops_per_round = 6;
        static constexpr int k_recv_block_avoid_threshold = 4096;
        static constexpr int k_recv_chunk_bytes = 2048;
        static constexpr int k_max_sync_skip_bytes = 96;
        static constexpr int k_min_keep_tail_bytes = 4;
        static constexpr int k_queue_soft_high = 30;
        static constexpr int k_queue_hard_high = 34;
        static constexpr int k_queue_recover_low = 6;
        static constexpr int k_decode_loops_recover = 10;
        int stat_loop_count = 0;
        int stat_recv_bytes = 0;
        int stat_decode_frames = 0;
        int stat_pcm_frames = 0;
        int stat_pcm_samples = 0;
        int stat_no_pcm_parsed_frames = 0;
        int stat_last_queue_level = -1;
        int stat_peak_queue_level = -1;
        int stat_icy_meta_bytes = 0;
        int stat_icy_meta_blocks = 0;
        int stat_timeouts = 0;
        int stat_sync_realigns = 0;
        int stat_rate_switches = 0;
        int stat_rate_rejects = 0;
        int non_mp3_window_count = 0;
        int invalid_header_streak = 0;

        auto compact_window = [&]() {
            if (bytes_in_buf <= 0) {
                bytes_in_buf = 0;
                buf_start = 0;
                return;
            }
            if (buf_start <= 0) {
                return;
            }

            memmove(mp3_buffer, mp3_buffer + buf_start, bytes_in_buf);
            buf_start = 0;
        };

        {
            char *header_end = strstr(resp_header.data(), "\r\n\r\n");
            if (header_end != nullptr) {
                const uint8_t *body_start = reinterpret_cast<const uint8_t *>(header_end + 4);
                int body_len =
                    static_cast<int>(header_len - (body_start - reinterpret_cast<const uint8_t *>(resp_header.data())));
                if (body_len > 0) {
                    if (body_len > static_cast<int>(mp3_buffer_size)) {
                        body_len = static_cast<int>(mp3_buffer_size);
                    }
                    memcpy(mp3_buffer, body_start, body_len);
                    bytes_in_buf = body_len;

                    osal_printk("首包已接收: bytes=%d\n", body_len);
                }
            }
        }

        while (is_playing) {
            // 使用滑动窗口，避免每帧都对整段数据 memmove。
            // 当缓冲区内的字节不足最小解码帧长（这里假设为最少需要2000字节触发优先解码）时，强制接收
            if (bytes_in_buf < static_cast<int>(mp3_buffer_size)) {
                // 本地缓冲足够时优先解码，避免被阻塞式 recv 打断造成可闻卡顿。
                if (bytes_in_buf >= k_recv_block_avoid_threshold) {
                    goto decode_stage;
                }

                int tail_free = static_cast<int>(mp3_buffer_size) - (buf_start + bytes_in_buf);
                if (tail_free < k_compact_threshold && buf_start > 0) {
                    compact_window();
                    tail_free = static_cast<int>(mp3_buffer_size) - bytes_in_buf;
                }

                int32_t ret = -1;
                bool recv_called = false;
                std::array<uint8_t, k_recv_chunk_bytes> recv_temp = {0};
                if (tail_free > 0) {
                    recv_called = true;
                    int recv_want = tail_free;
                    if (recv_want > k_recv_chunk_bytes) {
                        recv_want = k_recv_chunk_bytes;
                    }
                    ret = lwip_recv(sock, recv_temp.data(), recv_want, 0);
                }

                if (!recv_called) {
                    goto decode_stage;
                }

                if (ret < 0) {
                    const int socket_errno = errno;
                    const bool is_timeout = (socket_errno == EWOULDBLOCK) || (socket_errno == EAGAIN);

                    if (is_timeout) {
                        if (bytes_in_buf > 0) {
                            // 关键：超时但缓冲区仍有数据时，不能跳过解码，否则会人为放大卡顿。
                            recv_fail_count = 0;
                            recv_timeout_count = 0;
                        } else {
                            recv_timeout_count++;
                            stat_timeouts++;
                            if (recv_timeout_count >= k_max_recv_timeout_count) {
                                osal_printk("接收MP3数据空缓冲超时，准备重连\n");
                                break;
                            }
                            osal_msleep(10);
                            continue;
                        }
                    }

                    recv_timeout_count = 0;
                    // 缓冲区里还有可解码数据时，优先继续解码，避免因短暂网络抖动产生卡顿。
                    if (bytes_in_buf == 0) {
                        recv_fail_count++;
                        if (recv_fail_count >= k_max_recv_fail_count) {
                            osal_printk("接收MP3数据持续失败，准备重连\n");
                            break;
                        }
                        osal_msleep(10);
                        continue;
                    }
                } else if (ret == 0) {
                    osal_printk("服务器关闭了连接\n");
                    break;
                } else {
                    int appended = 0;
                    if (icy_metaint > 0) {
                        for (int i = 0; i < ret; ++i) {
                            uint8_t b = recv_temp[i];

                            if (icy_metadata_remaining > 0) {
                                icy_metadata_remaining--;
                                stat_icy_meta_bytes++;
                                continue;
                            }

                            if (icy_audio_remaining == 0) {
                                icy_metadata_remaining = static_cast<int>(b) * 16;
                                if (icy_metadata_remaining > 0) {
                                    stat_icy_meta_blocks++;
                                }
                                icy_audio_remaining = icy_metaint;
                                continue;
                            }

                            if (bytes_in_buf + appended < static_cast<int>(mp3_buffer_size)) {
                                mp3_buffer[buf_start + bytes_in_buf + appended] = b;
                                appended++;
                            }
                            icy_audio_remaining--;
                        }
                    } else {
                        appended = ret;
                        memcpy(mp3_buffer + buf_start + bytes_in_buf, recv_temp.data(), appended);
                    }

                    bytes_in_buf += appended;
                    stat_recv_bytes += ret;
                    recv_fail_count = 0;
                    recv_timeout_count = 0;
                }
            }

        decode_stage:
            int queue_level = -1;
            int decode_loops_budget = k_max_decode_loops_per_round;
            if (playback_queue_level_getter_func != nullptr) {
                queue_level = playback_queue_level_getter_func();
                stat_last_queue_level = queue_level;
                if (queue_level > stat_peak_queue_level) {
                    stat_peak_queue_level = queue_level;
                }
                if (queue_level >= k_queue_hard_high) {
                    // 队列接近满时暂停解码推进，避免IIS端触发主动丢样造成“快进感”。
                    osal_msleep(2);
                    continue;
                }
                if (queue_level >= k_queue_soft_high) {
                    decode_loops_budget = 1;
                } else if (queue_level <= k_queue_recover_low) {
                    decode_loops_budget = k_decode_loops_recover;
                }
            }

            int decode_loops = 0;
            while (is_playing && bytes_in_buf > 0 && decode_loops < decode_loops_budget) {
                memset(&info, 0, sizeof(info));
                // 解码MP3数据并送入IIS
                int samples = mp3dec_decode_frame(&mp3d, mp3_buffer + buf_start, bytes_in_buf, pcm_buffer, &info);

                if (samples > 0) {
                    // 如果采样率变化了，调用iis_set_rate_func设置新的采样率
                    // 仅接受常见采样率并做防抖，避免伪帧导致播放速率被错误切换。
                    const bool hz_allowed = (info.hz == 48000 || info.hz == 44100 || info.hz == 32000 ||
                                             info.hz == 24000 || info.hz == 22050 || info.hz == 16000 ||
                                             info.hz == 12000 || info.hz == 11025 || info.hz == 8000);
                    if (hz_allowed && info.layer == 3) {
                        if (current_hz == 0) {
                            // 启动阶段快速锁定，避免长时间用默认采样率播放导致整体变速。
                            if (info.hz != hz_candidate) {
                                hz_candidate = info.hz;
                                hz_candidate_count = 1;
                            } else {
                                hz_candidate_count++;
                            }
                            if (hz_candidate_count >= 2 && iis_set_rate_func) {
                                iis_set_rate_func(hz_candidate);
                                current_hz = hz_candidate;
                                stat_rate_switches++;
                                osal_printk("采样率初次锁定: %d\n", current_hz);
                                hz_candidate = 0;
                                hz_candidate_count = 0;
                                rate_mismatch_streak = 0;
                            }
                        } else if (info.hz == current_hz) {
                            hz_candidate = 0;
                            hz_candidate_count = 0;
                            rate_mismatch_streak = 0;
                        } else {
                            rate_mismatch_streak++;
                            if (info.hz != hz_candidate) {
                                hz_candidate = info.hz;
                                hz_candidate_count = 1;
                            } else {
                                hz_candidate_count++;
                            }

                            // 运行中切速必须更保守：持续一致且队列接近空，避免边播边切产生快进/爆点。
                            const bool queue_safe_to_switch = (queue_level >= 0 && queue_level <= 2);
                            if (hz_candidate_count >= 24) {
                                if (queue_safe_to_switch && iis_set_rate_func) {
                                    int old_hz = current_hz;
                                    iis_set_rate_func(hz_candidate);
                                    current_hz = hz_candidate;
                                    stat_rate_switches++;
                                    osal_printk("采样率运行切换: %d -> %d (q=%d)\n", old_hz, current_hz,
                                                queue_level);
                                } else {
                                    stat_rate_rejects++;
                                }
                                hz_candidate = 0;
                                hz_candidate_count = 0;
                                rate_mismatch_streak = 0;
                            }
                        }
                    }

                    uint32_t output_samples = static_cast<uint32_t>(samples * info.channels);
                    if (info.channels == 1) {
                        // 单声道扩展为双声道，避免仅左声道有声。
                        for (int i = samples - 1; i >= 0; --i) {
                            const int16_t s = pcm_buffer[i];
                            pcm_buffer[2 * i] = s;
                            pcm_buffer[2 * i + 1] = s;
                        }
                        output_samples = static_cast<uint32_t>(samples * 2);
                    }

                    // 将解码得到的PCM数据送入IIS
                    if (mp3_get_into_iis_func) {
                        mp3_get_into_iis_func(pcm_buffer, output_samples);
                    }
                    stat_pcm_frames++;
                    stat_pcm_samples += static_cast<int>(output_samples);
                    invalid_header_streak = 0;
                }

                // 网络半帧数据尾巴保护算法 (滑动窗口前移)
                bool frame_consumed = false;
                bool need_more_bytes = false;
                if (samples > 0 && info.frame_bytes > 0) {
                    int consume = (info.frame_bytes <= bytes_in_buf) ? info.frame_bytes : bytes_in_buf;
                    buf_start += consume;
                    bytes_in_buf -= consume;
                    frame_consumed = true;
                    stat_decode_frames++;
                    no_progress_count = 0;

                    if (bytes_in_buf == 0) {
                        buf_start = 0;
                    } else if (buf_start > static_cast<int>(mp3_buffer_size / 2)) {
                        compact_window();
                    }
                } else if (info.frame_bytes > 0) {
                    // 契约优先：minimp3 的 frame_bytes 就是本轮建议推进量。
                    stat_no_pcm_parsed_frames++;
                    int consume = 0;
                    const bool header_valid = (info.layer > 0 && info.hz > 0 && info.channels > 0);
                    if (!header_valid) {
                        invalid_header_streak++;
                    } else {
                        invalid_header_streak = 0;
                    }

                    // 当frame_bytes == in_buf且层/采样率信息无效时，通常是“窗口内全是非MP3块”。
                    if (samples == 0 && info.frame_bytes == bytes_in_buf && info.hz == 0 && info.layer == 0) {
                        const uint8_t *cur = mp3_buffer + buf_start;
                        int id3_skip = parse_id3v2_tag_total_size_if_present(cur, bytes_in_buf);
                        if (id3_skip > 0 && id3_skip <= bytes_in_buf) {
                            consume = id3_skip;
                            non_mp3_window_count = 0;
                            osal_printk("检测到ID3块，快速跳过: %dB\n", id3_skip);
                        } else {
                            int sync_off = find_mp3_sync_offset(cur, bytes_in_buf);
                            if (sync_off > 0 && sync_off <= k_max_sync_skip_bytes) {
                                consume = sync_off;
                                stat_sync_realigns++;
                                non_mp3_window_count = 0;
                                osal_printk("无PCM窗口内快速对齐: skip=%d\n", sync_off);
                            } else if (sync_off > k_max_sync_skip_bytes) {
                                // 偏移过大通常意味着当前窗口同步不可靠，继续收包比大跨度跳过更稳。
                                consume = 0;
                                need_more_bytes = true;
                                osal_printk("重同步候选偏移过大: skip=%d，继续收包\n", sync_off);
                            } else if (has_known_non_mp3_signature(cur, bytes_in_buf)) {
                                // 对疑似非MP3窗口优先扩窗验证，避免误丢真实音频片段。
                                need_more_bytes = true;
                                non_mp3_window_count++;
                                if (non_mp3_window_count <= 3) {
                                    osal_printk("疑似非MP3窗口(%s)，继续收包验证\n",
                                                classify_payload_prefix(cur, bytes_in_buf));
                                }
                            }
                        }
                    }

                    // 关键：只有在头信息有效时，才按minimp3契约推进frame_bytes。
                    if (consume == 0 && !need_more_bytes) {
                        if (header_valid) {
                            if (samples == 0) {
                                // 关键收敛：有合法头但未产PCM时优先扩窗，避免误吞一整帧导致时间轴压缩。
                                if (bytes_in_buf < static_cast<int>(mp3_buffer_size - 128)) {
                                    need_more_bytes = true;
                                } else {
                                    consume = 1;
                                }
                            } else {
                                if (info.frame_bytes < bytes_in_buf) {
                                    consume = info.frame_bytes;
                                } else if (info.frame_bytes == bytes_in_buf) {
                                    // 合法头但窗口刚好卡边界时，优先扩窗，避免吞掉潜在可解码边界数据。
                                    if (bytes_in_buf < static_cast<int>(mp3_buffer_size - 128)) {
                                        need_more_bytes = true;
                                    } else {
                                        consume = (bytes_in_buf > k_min_keep_tail_bytes)
                                                      ? (bytes_in_buf - k_min_keep_tail_bytes)
                                                      : bytes_in_buf;
                                    }
                                } else {
                                    consume = bytes_in_buf;
                                }
                            }
                        } else {
                            // 无效头不按frame_bytes推进，先扩窗；仅满窗时最小推进防止死锁。
                            if (bytes_in_buf < static_cast<int>(mp3_buffer_size - 64)) {
                                need_more_bytes = true;
                            } else {
                                consume = 1;
                            }
                        }
                    }

                    if (invalid_header_streak >= 64) {
                        // 连续无效头说明解码状态可能被污染，重置状态机但保留窗口数据。
                        mp3dec_init(&mp3d);
                        invalid_header_streak = 0;
                        osal_printk("连续无效帧头，重置解码器状态\n");
                    }

                    if (consume > 0) {
                        buf_start += consume;
                        bytes_in_buf -= consume;
                        frame_consumed = true;
                        stat_decode_frames++;
                        if (stat_no_pcm_parsed_frames <= 6) {
                            osal_printk("无PCM恢复推进: consume=%d frame_bytes=%d in_buf=%d\n", consume,
                                        info.frame_bytes, bytes_in_buf);
                        }
                    }

                    if (stat_no_pcm_parsed_frames <= 3) {
                        osal_printk("无PCM帧: layer=%d hz=%d ch=%d kbps=%d frame_bytes=%d\n", info.layer, info.hz,
                                    info.channels, info.bitrate_kbps, info.frame_bytes);
                    }

                    // 激进简化：移除诊断分支，保持主路径纯解码。

                    // 当前阶段不做自动熔断，先保留原始流观察。

                    if (bytes_in_buf <= 0) {
                        bytes_in_buf = 0;
                        buf_start = 0;
                    } else if (buf_start > static_cast<int>(mp3_buffer_size / 2)) {
                        compact_window();
                    }

                    if (need_more_bytes) {
                        break;
                    }
                }

                // 保留少量尾部字节即可，避免对低码率短帧过度保守导致解码推进不足。
                if (bytes_in_buf < 128) {
                    break;
                }

                if (!frame_consumed) {
                    // frame_bytes==0时优先重同步；失败则退出本轮继续收包，不做激进丢弃。
                    int sync_off = find_mp3_sync_offset(mp3_buffer + buf_start, bytes_in_buf);
                    if (sync_off > 0 && sync_off <= k_max_sync_skip_bytes) {
                        buf_start += sync_off;
                        bytes_in_buf -= sync_off;
                        frame_consumed = true;
                        stat_decode_frames++;
                        stat_sync_realigns++;
                        no_progress_count = 0;
                        if (stat_no_pcm_parsed_frames <= 6) {
                            osal_printk("快速重同步: 跳过=%d in_buf=%d\n", sync_off, bytes_in_buf);
                        }
                    } else if (sync_off > k_max_sync_skip_bytes) {
                        osal_printk("快速重同步候选偏移过大: skip=%d，继续收包\n", sync_off);
                        break;
                    }

                    // 如果仍未前进，才走最小丢弃路径。
                    if (frame_consumed) {
                        if (bytes_in_buf == 0) {
                            buf_start = 0;
                        } else if (buf_start > static_cast<int>(mp3_buffer_size / 2)) {
                            compact_window();
                        }
                        continue;
                    }

                    ++no_progress_count;
                    if (bytes_in_buf >= static_cast<int>(mp3_buffer_size - 1)) {
                        buf_start += 1;
                        bytes_in_buf -= 1;
                        if (bytes_in_buf == 0) {
                            buf_start = 0;
                        } else if (buf_start > static_cast<int>(mp3_buffer_size / 2)) {
                            compact_window();
                        }
                        osal_printk("MP3缓冲满且无法推进，丢弃1字节自恢复\n");
                        no_progress_count = 0;
                    }
                    break;
                }

                decode_loops++;
                if (bytes_in_buf < 512) {
                    break;
                }
            }

            if (decode_loops == 0) {
                // 如果是因为没有数据而暂停，稍微休眠即可；如果是因为背压，稍微休眠。
                // 确保休眠不会因为死锁导致网络不再接收数据
                osal_msleep(5);
            } else if (bytes_in_buf == 0) {
                osal_msleep(1);
            }

            stat_loop_count++;
            // 近似按秒统计（该循环在空闲路径有msleep，数量级足够观察网络波动）。
            if (stat_loop_count >= 1000) {
                osal_printk(
                    "MP3统计(窗口): recv=%dB parsed=%d pcm_frames=%d pcm_samples=%d icy_blocks=%d icy_bytes=%d "
                    "timeout=%d sync=%d rate_sw=%d rate_rej=%d in_buf=%d q_last=%d q_peak=%d\n",
                    stat_recv_bytes, stat_decode_frames, stat_pcm_frames, stat_pcm_samples, stat_icy_meta_blocks,
                    stat_icy_meta_bytes, stat_timeouts, stat_sync_realigns, stat_rate_switches, stat_rate_rejects,
                    bytes_in_buf, stat_last_queue_level,
                    stat_peak_queue_level);
                if (stat_decode_frames > 0 && stat_pcm_frames == 0 && stat_no_pcm_parsed_frames > 128) {
                    osal_printk("告警: 连续解析到帧头但始终无PCM输出，流很可能不是MP3音频体\n");
                }
                stat_loop_count = 0;
                stat_recv_bytes = 0;
                stat_decode_frames = 0;
                stat_pcm_frames = 0;
                stat_pcm_samples = 0;
                stat_no_pcm_parsed_frames = 0;
                stat_last_queue_level = -1;
                stat_peak_queue_level = -1;
                stat_icy_meta_blocks = 0;
                stat_icy_meta_bytes = 0;
                stat_timeouts = 0;
                stat_sync_realigns = 0;
                stat_rate_switches = 0;
                stat_rate_rejects = 0;
            }
        }

        lwip_close(sock);

        if (!is_playing) {
            continue;
        }

        // 若URL发生变化或收到新URL，立即进入下一轮连接。
        if (is_url_ready || strcmp(working_url.data(), current_url.data()) != 0) {
            continue;
        }

        // 适度退避，避免在弱网环境下形成连续重连风暴。
        osal_msleep(500);
    }
}