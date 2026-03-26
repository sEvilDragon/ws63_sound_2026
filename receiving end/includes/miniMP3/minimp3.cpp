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

void minimp3::http_get_url(const char *url)
{
    if (url == nullptr || url[0] == '\0') {
        return;
    }
    copy_string_safe(current_url.data(), current_url.size(), url);

    // 更新状态
    is_playing = true;
    is_url_ready = true;
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

        std::array<char, 128> content_type = {0};
        std::array<char, 64> transfer_encoding = {0};

        bool has_content_type =
            extract_http_header_value(resp_header.data(), "Content-Type", content_type.data(), content_type.size());
        bool has_transfer_encoding = extract_http_header_value(resp_header.data(), "Transfer-Encoding",
                                                               transfer_encoding.data(), transfer_encoding.size());
        bool is_chunked_transfer = false;

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
        static constexpr int k_queue_soft_high = 46;
        static constexpr int k_queue_hard_high = 49;
        static constexpr int k_queue_recover_low = 8;
        static constexpr int k_decode_loops_recover = 16;
        static constexpr int k_recv_block_avoid_threshold = 1024;
        static constexpr int k_recv_chunk_bytes = 1024;
        bool need_rebuffer = (bytes_in_buf == 0);
        int stat_loop_count = 0;
        int stat_recv_bytes = 0;
        int stat_decode_frames = 0;
        int stat_timeouts = 0;
        int stat_rebuffer_waits = 0;
        int stat_backpressure_waits = 0;

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
                }
            }
        }

        while (is_playing) {
            // 使用滑动窗口，避免每帧都对整段数据 memmove。
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
                if (tail_free > 0) {
                    recv_called = true;
                    int recv_want = tail_free;
                    if (recv_want > k_recv_chunk_bytes) {
                        recv_want = k_recv_chunk_bytes;
                    }
                    ret = lwip_recv(sock, mp3_buffer + buf_start + bytes_in_buf, recv_want, 0);
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
                    bytes_in_buf += ret;
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
                if (queue_level <= k_queue_recover_low) {
                    // 低水位快速追赶，尽量避免IIS掉到停播区间造成长空白。
                    decode_loops_budget = k_decode_loops_recover;
                }
                if (queue_level >= k_queue_hard_high) {
                    // 不再硬暂停，避免形成“等待-突发-等待”的可闻卡顿。
                    decode_loops_budget = 1;
                    stat_backpressure_waits++;
                } else if (queue_level >= k_queue_soft_high) {
                    decode_loops_budget = 2;
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
                // 解码MP3数据并送入IIS
                int samples = mp3dec_decode_frame(&mp3d, mp3_buffer + buf_start, bytes_in_buf, pcm_buffer, &info);

                if (samples > 0) {
                    // 如果采样率变化了，调用iis_set_rate_func设置新的采样率
                    if (info.hz != current_hz && iis_set_rate_func) {
                        iis_set_rate_func(info.hz);
                        current_hz = info.hz;
                    }

                    // 将解码得到的PCM数据送入IIS
                    if (mp3_get_into_iis_func) {
                        mp3_get_into_iis_func(pcm_buffer, samples * info.channels);
                    }
                }

                // 网络半帧数据尾巴保护算法 (滑动窗口前移)
                bool frame_consumed = false;
                if (info.frame_bytes > 0 && info.frame_bytes <= bytes_in_buf) {
                    buf_start += info.frame_bytes;
                    bytes_in_buf -= info.frame_bytes;
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
                }

                // 当缓冲区已满且解码器不前进时，丢弃1字节避免死循环空转。
                if (!frame_consumed) {
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
                    }
                    break;
                }

                decode_loops++;
                if (bytes_in_buf < 1024) {
                    break;
                }
            }

            if (decode_loops == 0 && bytes_in_buf == 0) {
                osal_msleep(1);
            }

            stat_loop_count++;
            // 近似按秒统计（该循环在空闲路径有msleep，数量级足够观察网络波动）。
            if (stat_loop_count >= 1000) {
                osal_printk("MP3统计(窗口): recv=%dB frames=%d timeout=%d rebuf=%d bp=%d in_buf=%d\n", stat_recv_bytes,
                            stat_decode_frames, stat_timeouts, stat_rebuffer_waits, stat_backpressure_waits,
                            bytes_in_buf);
                stat_loop_count = 0;
                stat_recv_bytes = 0;
                stat_decode_frames = 0;
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