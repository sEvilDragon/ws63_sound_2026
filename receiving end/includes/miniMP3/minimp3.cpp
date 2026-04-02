#define MINIMP3_IMPLEMENTATION
#include "minimp3.hpp"

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

    uint8_t *mp3_buffer = (uint8_t *)osal_kmalloc(mp3_buffer_size, OSAL_GFP_KERNEL);
    if (mp3_buffer == nullptr) {
        osal_printk("mp3_buffer内存分配失败\n");
        return;
    }
    int16_t *pcm_buffer = (int16_t *)osal_kmalloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                                  OSAL_GFP_KERNEL); // PCM缓冲区，预留足够空间
    if (pcm_buffer == nullptr) {
        osal_printk("pcm_buffer内存分配失败\n");
        osal_kfree(mp3_buffer);
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

        std::array<char, 512> request = {0};
        snprintf(request.data(), request.size(),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
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
        std::array<char, 64> transfer_encoding = {0};
        std::array<char, 32> icy_metaint_text = {0};

        bool has_content_type =
            extract_http_header_value(resp_header.data(), "Content-Type", content_type.data(), content_type.size());
        bool has_transfer_encoding = extract_http_header_value(resp_header.data(), "Transfer-Encoding",
                                                               transfer_encoding.data(), transfer_encoding.size());
        bool has_icy_metaint = extract_http_header_value(resp_header.data(), "icy-metaint", icy_metaint_text.data(),
                                                         icy_metaint_text.size());
        bool is_chunked_transfer = false;
        bool likely_mp3_content = true;
        int icy_metaint = 0;
        int icy_audio_remaining = 0;
        int icy_metadata_remaining = 0;

        // SED : 串口输出，打印HTTP响应头中的Content-Type和Transfer-Encoding，方便调试验证服务器响应的格式是否正确。
        if (has_content_type) {
            trim_ascii_whitespace(content_type.data());
            // SED : 串口输出，打印Content-Type，方便调试验证服务器响应的内容类型是否正确。
            osal_printk("HTTP Content-Type: %s\n", content_type.data());

            likely_mp3_content = ascii_icontains(content_type.data(), "audio/mpeg") ||
                                 ascii_icontains(content_type.data(), "audio/mp3") ||
                                 ascii_icontains(content_type.data(), "audio/x-mpeg") ||
                                 ascii_icontains(content_type.data(), "application/octet-stream");
            if (!likely_mp3_content) {
                osal_printk("警告: 当前Content-Type可能非MP3，解码可能无PCM输出\n");
            }
        }

        if (has_transfer_encoding) {
            trim_ascii_whitespace(transfer_encoding.data());
            is_chunked_transfer = ascii_icontains(transfer_encoding.data(), "chunked");
            // SED : 串口输出，打印Transfer-Encoding，方便调试验证服务器响应的传输编码是否正确。
            osal_printk("HTTP Transfer-Encoding: %s\n", transfer_encoding.data());
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
        static constexpr int k_stall_drop_threshold = 64;
        static constexpr int k_rebuffer_threshold_bytes = 192;
        // 与IIS侧预缓冲门限(prebuffer_num=30)对齐，避免启动期过早背压导致长静音。
        static constexpr int k_queue_soft_high = 34;
        static constexpr int k_queue_hard_high = 46;
        static constexpr int k_queue_recover_low = 6;
        static constexpr int k_decode_loops_recover = 16;
        static constexpr int k_recv_block_avoid_threshold = 1024;
        static constexpr int k_recv_chunk_bytes = 1024;
        bool need_rebuffer = (bytes_in_buf == 0);
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
        int stat_rebuffer_waits = 0;
        int stat_backpressure_waits = 0;
        int no_pcm_streak = 0;

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

                    // 首包特征识别：快速判断是否为MP3/ID3/AAC/RIFF，辅助定位“有流量无声音”。
                    if (body_len >= 3) {
                        const uint8_t b0 = mp3_buffer[0];
                        const uint8_t b1 = mp3_buffer[1];
                        const uint8_t b2 = mp3_buffer[2];
                        if (b0 == 'I' && b1 == 'D' && b2 == '3') {
                            osal_printk("音频体前导: ID3 (MP3标签头)\n");
                        } else if (body_len >= 2 && b0 == 0xFF && (b1 & 0xF6) == 0xF0) {
                            osal_printk("音频体前导: ADTS/AAC，同步字命中，minimp3无法解码\n");
                        } else if (body_len >= 12 && mp3_buffer[0] == 'R' && mp3_buffer[1] == 'I' &&
                                   mp3_buffer[2] == 'F' && mp3_buffer[3] == 'F') {
                            osal_printk("音频体前导: RIFF容器，当前链路未实现容器解析\n");
                        } else if (body_len >= 12 && mp3_buffer[4] == 'f' && mp3_buffer[5] == 't' &&
                                   mp3_buffer[6] == 'y' && mp3_buffer[7] == 'p') {
                            osal_printk("音频体前导: MP4/ISO-BMFF(ftyp)，minimp3无法解码\n");
                        } else {
                            osal_printk("音频体前导HEX: %02X %02X %02X %02X\n", mp3_buffer[0], mp3_buffer[1],
                                        mp3_buffer[2], body_len >= 4 ? mp3_buffer[3] : 0);
                        }
                    }
                }
            }
        }

        while (is_playing) {
            // 使用滑动窗口，避免每帧都对整段数据 memmove。
            // 当缓冲区内的字节不足最小解码帧长（这里假设为最少需要2000字节触发优先解码）时，强制接收
            if (bytes_in_buf < static_cast<int>(mp3_buffer_size)) {
                // 本地缓冲足够时优先解码，避免被阻塞式 recv 打断造成可闻卡顿。
                if (bytes_in_buf >= k_recv_block_avoid_threshold && !need_rebuffer) {
                    goto decode_stage;
                }

                int tail_free = static_cast<int>(mp3_buffer_size) - (buf_start + bytes_in_buf);
                if (tail_free < k_compact_threshold && buf_start > 0) {
                    memmove(mp3_buffer, mp3_buffer + buf_start, bytes_in_buf);
                    buf_start = 0;
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
            // 根据IIS待播队列做软背压：高水位时降低解码推进速度，而非完全暂停。
            int queue_level = -1;
            int decode_loops_budget = k_max_decode_loops_per_round;
            if (playback_queue_level_getter_func != nullptr) {
                queue_level = playback_queue_level_getter_func();
                stat_last_queue_level = queue_level;
                if (queue_level > stat_peak_queue_level) {
                    stat_peak_queue_level = queue_level;
                }
                if (queue_level <= k_queue_recover_low) {
                    // 低水位快速追赶，尽量避免IIS掉到停播区间造成长空白。
                    decode_loops_budget = k_decode_loops_recover;
                }
                if (queue_level >= k_queue_hard_high) {
                    // 避免完全停推进造成网络接收饥饿，改为最小推进。
                    decode_loops_budget = 1;
                    stat_backpressure_waits++;
                } else if (queue_level >= k_queue_soft_high) {
                    decode_loops_budget = 1;
                }
            }

            // 再缓冲仅在见底阶段触发，并根据输出队列状态动态放宽阈值，避免长静音。
            if (need_rebuffer) {
                int threshold = k_rebuffer_threshold_bytes;
                if (queue_level > k_queue_soft_high) {
                    threshold = 64;
                }
                if (bytes_in_buf < threshold) {
                    stat_rebuffer_waits++;
                    osal_msleep(1);
                    continue;
                }
                need_rebuffer = false;
            }

            int decode_loops = 0;
            while (is_playing && bytes_in_buf > 0 && decode_loops < decode_loops_budget) {
                memset(&info, 0, sizeof(info));
                // 解码MP3数据并送入IIS
                int samples = mp3dec_decode_frame(&mp3d, mp3_buffer + buf_start, bytes_in_buf, pcm_buffer, &info);

                if (samples > 0) {
                    // 如果采样率变化了，调用iis_set_rate_func设置新的采样率
                    if (info.hz != current_hz && iis_set_rate_func) {
                        iis_set_rate_func(info.hz);
                        current_hz = info.hz;
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
                    no_pcm_streak = 0;
                }

                // 网络半帧数据尾巴保护算法 (滑动窗口前移)
                bool frame_consumed = false;
                if (samples > 0 && info.frame_bytes > 0) {
                    int consume = (info.frame_bytes <= bytes_in_buf) ? info.frame_bytes : bytes_in_buf;
                    buf_start += consume;
                    bytes_in_buf -= consume;
                    frame_consumed = true;
                    stat_decode_frames++;
                    no_progress_count = 0;

                    // 仅在缓冲真正耗尽时才再缓冲，避免频繁触发导致长空白。
                    if (bytes_in_buf == 0) {
                        need_rebuffer = true;
                    }

                    if (bytes_in_buf == 0) {
                        buf_start = 0;
                    } else if (buf_start > static_cast<int>(mp3_buffer_size / 2)) {
                        memmove(mp3_buffer, mp3_buffer + buf_start, bytes_in_buf);
                        buf_start = 0;
                    }
                } else if (info.frame_bytes > 0) {
                    // 网络抖动时可能先命中帧头但数据尚未完整，先给几次机会继续收包，避免误丢有效帧。
                    ++no_pcm_streak;
                    if (no_pcm_streak <= 2 && bytes_in_buf < (info.frame_bytes + 256)) {
                        break;
                    }

                    // 连续无PCM再做单字节重同步，避免整帧跳过跨过真实音频。
                    if (bytes_in_buf > 0) {
                        buf_start += 1;
                        bytes_in_buf -= 1;
                        frame_consumed = true;
                        stat_decode_frames++;
                        stat_no_pcm_parsed_frames++;
                    }

                    if (stat_no_pcm_parsed_frames <= 3) {
                        osal_printk("无PCM帧: layer=%d hz=%d ch=%d kbps=%d frame_bytes=%d\n", info.layer, info.hz,
                                    info.channels, info.bitrate_kbps, info.frame_bytes);
                    }

                    if (bytes_in_buf <= 0) {
                        bytes_in_buf = 0;
                        buf_start = 0;
                        need_rebuffer = true;
                    } else if (buf_start > static_cast<int>(mp3_buffer_size / 2)) {
                        memmove(mp3_buffer, mp3_buffer + buf_start, bytes_in_buf);
                        buf_start = 0;
                    }
                }

                // 保留少量尾部字节即可，避免对低码率短帧过度保守导致解码推进不足。
                if (bytes_in_buf < 128) {
                    break;
                }

                // 当缓冲区已满且解码器不前进时，丢弃1字节避免死循环空转。
                if (!frame_consumed) {
                    // 如果单帧太大，由于当前没有足够的数据，解码器也可能返回 frame_bytes == 0。
                    // 只有在接收缓冲真的达到上限，且真的无法前进时才丢弃数据。
                    ++no_progress_count;
                    if (bytes_in_buf >= static_cast<int>(mp3_buffer_size - 1) &&
                        no_progress_count >= k_stall_drop_threshold) {
                        buf_start += 1;
                        bytes_in_buf -= 1;
                        if (bytes_in_buf == 0) {
                            buf_start = 0;
                        } else if (buf_start > static_cast<int>(mp3_buffer_size / 2)) {
                            memmove(mp3_buffer, mp3_buffer + buf_start, bytes_in_buf);
                            buf_start = 0;
                        }
                        osal_printk("MP3解码长时间无进展，已丢弃1字节尝试自恢复\n");
                        no_progress_count = 0;
                    } else if (bytes_in_buf < static_cast<int>(mp3_buffer_size)) {
                        // 如果缓冲区没满，而且又没有消费，说明帧不完整，需要退出 decode 循环去继续 recv。
                        break;
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
                    "timeout=%d rebuf=%d bp=%d in_buf=%d q_last=%d q_peak=%d\n",
                    stat_recv_bytes, stat_decode_frames, stat_pcm_frames, stat_pcm_samples, stat_icy_meta_blocks,
                    stat_icy_meta_bytes, stat_timeouts, stat_rebuffer_waits, stat_backpressure_waits, bytes_in_buf,
                    stat_last_queue_level, stat_peak_queue_level);
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
                stat_rebuffer_waits = 0;
                stat_backpressure_waits = 0;
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