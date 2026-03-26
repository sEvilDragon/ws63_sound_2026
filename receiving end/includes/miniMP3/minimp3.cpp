#define MINIMP3_IMPLEMENTATION
#include "minimp3.hpp"

// 初始化静态成员变量
std::array<char, 512> minimp3::current_url = {0};
bool minimp3::is_playing = false;
bool minimp3::is_url_ready = false;
minimp3::iis_set_rate minimp3::iis_set_rate_func = nullptr;
minimp3::mp3_get_into_iis minimp3::mp3_get_into_iis_func = nullptr;

void minimp3::iis_set_rate_set(iis_set_rate set_rate_func)
{
    iis_set_rate_func = set_rate_func;
}

void minimp3::mp3_get_into_iis_set(mp3_get_into_iis get_into_iis_func)
{
    mp3_get_into_iis_func = get_into_iis_func;
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
        // 等待URL准备好
        if (!is_url_ready) {
            osal_msleep(100);
            continue;
        }

        // 重置状态，防止重复处理
        is_url_ready = false;
        int current_hz = 0;
        int bytes_in_buf = 0;
        mp3dec_init(&mp3d);

        simple_http_url parsed_url;
        if (!parse_http_url(current_url.data(), parsed_url)) {
            osal_printk("URL解析失败: %s\n", current_url.data());
            continue;
        }

        // 创建socket连接到服务器
        int32_t sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            osal_printk("创建socket失败\n");
            continue;
        }

        // 设置超时，防止连接卡死
        timeval timeout = {5, 0}; // 5秒连接超时
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // 配置服务器地址
        sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = lwip_htons(parsed_url.port);
        if (!resolve_ipv4_addr(parsed_url.host.data(), &addr.sin_addr)) {
            osal_printk("无法解析主机地址: %s\n", parsed_url.host.data());
            lwip_close(sock);
            continue;
        }

        if (lwip_connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
            osal_printk("连接服务器失败: %s\n", parsed_url.host.data());
            lwip_close(sock);
            continue;
        }

        std::array<char, 512> request = {0};
        snprintf(request.data(), request.size(),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Connection: keep-alive\r\n"
                 "Icy-MetaData: 0\r\n\r\n",
                 parsed_url.path.data(), parsed_url.host.data());

        if (lwip_send(sock, request.data(), strlen(request.data()), 0) <= 0) {
            osal_printk("发送HTTP请求失败\n");
            lwip_close(sock);
            continue;
        }

        // 跳过HTTP响应头
        std::array<char, 1024> resp_header = {0};
        int32_t header_len = 0;
        bool header_ended = false;

        while (is_playing && !header_ended && header_len < (resp_header.size() - 1)) {
            int32_t ret = lwip_recv(sock, resp_header.data() + header_len, 1, 0);
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

        int no_progress_count = 0;

        while (is_playing) {
            // 如果缓冲区中没有数据了，就继续接收数据
            if (bytes_in_buf < mp3_buffer_size) {
                int32_t ret = lwip_recv(sock, mp3_buffer + bytes_in_buf, mp3_buffer_size - bytes_in_buf, 0);
                if (ret < 0) {
                    osal_printk("接收MP3数据失败\n");
                    break;
                } else if (ret == 0) {
                    osal_printk("服务器关闭了连接\n");
                    break;
                }
                bytes_in_buf += ret;
            }

            // 解码MP3数据并送入IIS
            int samples = mp3dec_decode_frame(&mp3d, mp3_buffer, bytes_in_buf, pcm_buffer, &info);

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
                bytes_in_buf -= info.frame_bytes;
                memmove(mp3_buffer, mp3_buffer + info.frame_bytes, bytes_in_buf);
                frame_consumed = true;
                no_progress_count = 0;
            }

            // 当缓冲区已满且解码器不前进时，丢弃1字节避免死循环空转。
            if (!frame_consumed) {
                if (bytes_in_buf >= mp3_buffer_size) {
                    bytes_in_buf -= 1;
                    memmove(mp3_buffer, mp3_buffer + 1, bytes_in_buf);
                    ++no_progress_count;
                    if ((no_progress_count % 256) == 0) {
                        osal_printk("MP3解码无进展，已丢弃字节尝试自恢复\n");
                    }
                } else {
                    osal_msleep(1);
                }
            }
        }

        lwip_close(sock);
        is_playing = false;
    }
}