// WS63 LiteOS: 无 mmap/munmap/stdio，只用回调 API
#define MINIMP3_NO_STDIO
// IO 缓冲区 18KB，防止嵌入式堆分配失败
// 系统总堆约 340KB，大部分被 TLS/套接字/HTTP 头部所占用
#define MINIMP3_IO_SIZE (18 * 1024)
#define MINIMP3_IMPLEMENTATION

// 提供 munmap 桩实现，因为 RISCV musl 预处理器未正确排除 mmap/munmap
// 路径：编译器提供桩函数，链接到 RISCV musl
// 背景：由于预处理器
// __linux__ 导致 minimp3_ex.h 中的 #if defined(__linux__) 分支被编译——尽管 MINIMP3_NO_STDIO 已定义。
// 这些桩函数不会被实际调用，因为使用回调 IO
// 但链接器仍需要（移除文件映射代码后）符号，否则链接阶段报 undefined reference：
extern "C" {
int munmap(void *addr, unsigned long length)
{
    (void)addr;
    (void)length;
    return -1;
}
}

#include "minimp3.hpp"

// 【静态预分配 IO 缓冲区】替代动态分配，防止堆碎片导致分配失败
// mp3dec_ex_open_cb 内部调用 malloc(MINIMP3_IO_SIZE)，极易碎片化失败。
// 堆已满（340KB 已耗尽，剩余 3~10KB 在池中）。
// 通过重载 MINIMP3_ALLOC_IO / MINIMP3_FREE_IO 宏，将动态堆分配替换为
// 运行时单次使用的静态缓冲区，消除了这个故障点。
static uint8_t g_mp3_io_buf[MINIMP3_IO_SIZE];
static bool g_mp3_io_buf_in_use = false;

#define MINIMP3_ALLOC_IO(size) \
    ((void)(size), g_mp3_io_buf_in_use ? (void *)NULL : (g_mp3_io_buf_in_use = true, (void *)g_mp3_io_buf))
#define MINIMP3_FREE_IO(ptr)                         \
    do {                                             \
        if ((void *)(ptr) == (void *)g_mp3_io_buf) { \
            g_mp3_io_buf_in_use = false;             \
        }                                            \
    } while (0)

#include "miniMP3/minimp3_ex.h"

#include "systick.h"
#include "trng.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include "iis.hpp"

namespace {
enum class stream_url_scheme : uint8_t { http, https };

struct stream_url_desc {
    std::array<char, 128> host = {0};
    std::array<char, 512> path = {0};
    uint16_t port = 80;
    stream_url_scheme scheme = stream_url_scheme::http;
};

struct stream_transport {
    bool use_tls = false;
    bool tls_inited = false;
    int32_t plain_sock = -1;
    mbedtls_net_context tls_net;
    mbedtls_ssl_context tls_ssl;
    mbedtls_ssl_config tls_conf;
    mbedtls_ctr_drbg_context tls_ctr_drbg;
    mbedtls_entropy_context tls_entropy;
};

static constexpr int k_stream_retry_later = -2;
static constexpr uint32_t k_tls_initial_read_timeout_ms = 10;
static constexpr uint32_t k_tls_handshake_timeout_ms = 4000;
static constexpr uint32_t k_http_header_timeout_ms = 5000;
static constexpr size_t k_tls_entropy_min_hardclock = 4;
static constexpr uint64_t k_range_preroll_bytes = 8ULL * 1024ULL; // MP3 bit reservoir < 512B, 8KB 足够
static constexpr uint32_t k_range_recovery_read_timeout_ms = 400;
// 单次 drain 的最大字节数，超过该值则说明 Range 请求不可靠（丢数据），
// 需要重连而非长时间丢弃数据。
static constexpr uint64_t k_max_drain_bytes = 64ULL * 1024ULL;

uint64_t compute_range_request_offset(uint64_t target_byte)
{
    return (target_byte > k_range_preroll_bytes) ? (target_byte - k_range_preroll_bytes) : 0ULL;
}

uint64_t stream_now_ms()
{
    return uapi_systick_get_ms();
}

uint64_t stream_elapsed_ms(uint64_t start_ms)
{
    const uint64_t now_ms = stream_now_ms();
    if (now_ms >= start_ms) {
        return now_ms - start_ms;
    }
    return (UINT64_MAX - start_ms) + now_ms + 1;
}

int tls_entropy_source_callback(void *context, unsigned char *output, size_t len, size_t *out_len)
{
    (void)context;

    if (output == nullptr || out_len == nullptr || len == 0 || len > UINT32_MAX) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    if (uapi_drv_cipher_trng_get_random_bytes(output, static_cast<uint32_t>(len)) != ERRCODE_SUCC) {
        *out_len = 0;
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    *out_len = len;
    return 0;
}

bool seed_tls_random(stream_transport &transport)
{
    int ret = mbedtls_entropy_add_source(&transport.tls_entropy, tls_entropy_source_callback, nullptr,
                                         k_tls_entropy_min_hardclock, MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (ret != 0) {
        osal_printk("HTTPS熵源注册失败: ret=-0x%04X\n", -ret);
        return false;
    }

    static const char *k_tls_personalization = "ws63-minimp3";
    ret = mbedtls_ctr_drbg_seed(&transport.tls_ctr_drbg, mbedtls_entropy_func, &transport.tls_entropy,
                                reinterpret_cast<const unsigned char *>(k_tls_personalization),
                                strlen(k_tls_personalization));
    if (ret != 0) {
        osal_printk("HTTPS随机数初始化失败: ret=-0x%04X\n", -ret);
        return false;
    }

    mbedtls_ctr_drbg_set_prediction_resistance(&transport.tls_ctr_drbg, MBEDTLS_CTR_DRBG_PR_OFF);
    return true;
}

char ascii_to_lower_local(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

bool starts_with_ascii_ignore_case_local(const char *text, const char *prefix)
{
    if (text == nullptr || prefix == nullptr) {
        return false;
    }

    for (size_t index = 0; prefix[index] != '\0'; ++index) {
        if (text[index] == '\0') {
            return false;
        }
        if (ascii_to_lower_local(text[index]) != ascii_to_lower_local(prefix[index])) {
            return false;
        }
    }
    return true;
}

bool copy_text_range(char *dst, size_t dst_size, const char *begin, const char *end)
{
    if (dst == nullptr || dst_size == 0 || begin == nullptr || end == nullptr || end < begin) {
        return false;
    }

    const size_t text_len = static_cast<size_t>(end - begin);
    if (text_len == 0 || text_len >= dst_size) {
        return false;
    }

    memcpy(dst, begin, text_len);
    dst[text_len] = '\0';
    return true;
}

bool parse_stream_url(const char *url, stream_url_desc &out)
{
    if (url == nullptr || url[0] == '\0') {
        return false;
    }

    if (starts_with_ascii_ignore_case_local(url, "http://")) {
        simple_http_url parsed = {};
        if (!parse_http_url(url, parsed)) {
            return false;
        }
        copy_string_safe(out.host.data(), out.host.size(), parsed.host.data());
        copy_string_safe(out.path.data(), out.path.size(), parsed.path.data());
        out.port = parsed.port;
        out.scheme = stream_url_scheme::http;
        return true;
    }

    if (!starts_with_ascii_ignore_case_local(url, "https://")) {
        return false;
    }

    const char *authority = url + 8;
    const char *authority_end = authority;
    while (*authority_end != '\0' && *authority_end != '/' && *authority_end != '?' && *authority_end != '#') {
        ++authority_end;
    }

    const char *port_sep = nullptr;
    for (const char *cursor = authority; cursor < authority_end; ++cursor) {
        if (*cursor == ':') {
            port_sep = cursor;
        }
    }

    const char *host_end = (port_sep != nullptr) ? port_sep : authority_end;
    if (!copy_text_range(out.host.data(), out.host.size(), authority, host_end)) {
        return false;
    }

    if (port_sep != nullptr) {
        unsigned long parsed_port = 0;
        for (const char *cursor = port_sep + 1; cursor < authority_end; ++cursor) {
            if (*cursor < '0' || *cursor > '9') {
                return false;
            }
            parsed_port = parsed_port * 10UL + static_cast<unsigned long>(*cursor - '0');
            if (parsed_port > 65535UL) {
                return false;
            }
        }
        if (parsed_port == 0) {
            return false;
        }
        out.port = static_cast<uint16_t>(parsed_port);
    } else {
        out.port = 443;
    }

    if (*authority_end == '\0') {
        copy_string_safe(out.path.data(), out.path.size(), "/");
    } else if (!copy_text_range(out.path.data(), out.path.size(), authority_end, url + strlen(url))) {
        return false;
    }

    out.scheme = stream_url_scheme::https;
    return true;
}

void free_stream_transport(stream_transport &transport)
{
    if (transport.tls_inited) {
        (void)mbedtls_ssl_close_notify(&transport.tls_ssl);
        mbedtls_ssl_free(&transport.tls_ssl);
        mbedtls_ssl_config_free(&transport.tls_conf);
        mbedtls_ctr_drbg_free(&transport.tls_ctr_drbg);
        mbedtls_entropy_free(&transport.tls_entropy);
        mbedtls_net_free(&transport.tls_net);
        transport.tls_inited = false;
    }

    if (transport.plain_sock >= 0) {
        lwip_close(transport.plain_sock);
        transport.plain_sock = -1;
    }

    transport.use_tls = false;
}

bool open_plain_stream_transport(stream_transport &transport, const stream_url_desc &url)
{
    transport.use_tls = false;
    transport.plain_sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (transport.plain_sock < 0) {
        osal_printk("创建socket失败\n");
        return false;
    }

    timeval timeout = {5, 0};
    lwip_setsockopt(transport.plain_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    lwip_setsockopt(transport.plain_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(url.port);
    if (!resolve_ipv4_addr(url.host.data(), &addr.sin_addr)) {
        osal_printk("无法解析服务器地址: %s\n", url.host.data());
        return false;
    }

    if (lwip_connect(transport.plain_sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
        osal_printk("连接服务器失败: %s\n", url.host.data());
        return false;
    }

    return true;
}

bool open_tls_stream_transport(stream_transport &transport, const stream_url_desc &url)
{
    transport.use_tls = true;
    mbedtls_net_init(&transport.tls_net);
    mbedtls_ssl_init(&transport.tls_ssl);
    mbedtls_ssl_config_init(&transport.tls_conf);
    mbedtls_ctr_drbg_init(&transport.tls_ctr_drbg);
    mbedtls_entropy_init(&transport.tls_entropy);
    transport.tls_inited = true;

    if (!seed_tls_random(transport)) {
        return false;
    }

    int ret = mbedtls_ssl_config_defaults(&transport.tls_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        osal_printk("HTTPS配置失败: ret=-0x%04X\n", -ret);
        return false;
    }

    mbedtls_ssl_conf_authmode(&transport.tls_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&transport.tls_conf, mbedtls_ctr_drbg_random, &transport.tls_ctr_drbg);
    mbedtls_ssl_conf_min_version(&transport.tls_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&transport.tls_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_read_timeout(&transport.tls_conf, k_tls_initial_read_timeout_ms);

    ret = mbedtls_ssl_setup(&transport.tls_ssl, &transport.tls_conf);
    if (ret != 0) {
        osal_printk("HTTPS SSL setup失败: ret=-0x%04X\n", -ret);
        return false;
    }

    ret = mbedtls_ssl_set_hostname(&transport.tls_ssl, url.host.data());
    if (ret != 0) {
        osal_printk("HTTPS设置SNI失败: host=%s ret=-0x%04X\n", url.host.data(), -ret);
        return false;
    }

    std::array<char, 8> port_text = {0};
    snprintf(port_text.data(), port_text.size(), "%u", static_cast<unsigned int>(url.port));
    ret = mbedtls_net_connect(&transport.tls_net, url.host.data(), port_text.data(), MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        osal_printk("HTTPS连接服务器失败: host=%s ret=-0x%04X\n", url.host.data(), -ret);
        return false;
    }

    timeval send_timeout = {5, 0};
    lwip_setsockopt(transport.tls_net.fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    mbedtls_ssl_set_bio(&transport.tls_ssl, &transport.tls_net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);

    const uint64_t handshake_start_ms = stream_now_ms();
    do {
        ret = mbedtls_ssl_handshake(&transport.tls_ssl);
        if (ret == 0) {
            osal_printk("HTTPS握手成功: host=%s\n", url.host.data());
            return true;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE && ret != MBEDTLS_ERR_SSL_TIMEOUT) {
            break;
        }
    } while (stream_elapsed_ms(handshake_start_ms) < k_tls_handshake_timeout_ms);

    osal_printk("HTTPS握手失败: host=%s ret=-0x%04X elapsed=%llu ms\n", url.host.data(), -ret,
                static_cast<unsigned long long>(stream_elapsed_ms(handshake_start_ms)));
    return false;
}

bool open_stream_transport(stream_transport &transport, const stream_url_desc &url)
{
    free_stream_transport(transport);
    if (url.scheme == stream_url_scheme::https) {
        if (open_tls_stream_transport(transport, url)) {
            return true;
        }
        free_stream_transport(transport);
        return false;
    }
    if (open_plain_stream_transport(transport, url)) {
        return true;
    }
    free_stream_transport(transport);
    return false;
}

void set_stream_transport_read_timeout(stream_transport &transport, uint32_t timeout_ms)
{
    if (transport.use_tls && transport.tls_inited) {
        mbedtls_ssl_conf_read_timeout(&transport.tls_conf, timeout_ms);
        return;
    }

    if (transport.plain_sock >= 0) {
        timeval timeout = {static_cast<long>(timeout_ms / 1000U), static_cast<long>((timeout_ms % 1000U) * 1000U)};
        lwip_setsockopt(transport.plain_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
}

int stream_send_all(stream_transport &transport, const uint8_t *data, int len)
{
    if (data == nullptr || len <= 0) {
        return -1;
    }

    if (!transport.use_tls) {
        return lwip_send(transport.plain_sock, data, len, 0);
    }

    int total_written = 0;
    while (total_written < len) {
        int ret = mbedtls_ssl_write(&transport.tls_ssl, data + total_written, static_cast<size_t>(len - total_written));
        if (ret > 0) {
            total_written += ret;
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            osal_msleep(1);
            continue;
        }
        osal_printk("HTTPS发送失败: ret=-0x%04X\n", -ret);
        return -1;
    }

    return total_written;
}

int stream_recv_some(stream_transport &transport, uint8_t *data, int len, bool *timed_out)
{
    if (timed_out != nullptr) {
        *timed_out = false;
    }

    if (!transport.use_tls) {
        int ret = lwip_recv(transport.plain_sock, data, len, 0);
        if (ret < 0 && timed_out != nullptr) {
            const int socket_errno = errno;
            if (socket_errno == EWOULDBLOCK || socket_errno == EAGAIN) {
                *timed_out = true;
            }
        }
        return ret;
    }

    int ret = mbedtls_ssl_read(&transport.tls_ssl, data, static_cast<size_t>(len));
    if (ret > 0) {
        return ret;
    }
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return k_stream_retry_later;
    }
    if (timed_out != nullptr && ret == MBEDTLS_ERR_SSL_TIMEOUT) {
        *timed_out = true;
        return -1;
    }

    osal_printk("HTTPS接收失败: ret=-0x%04X\n", -ret);
    return -1;
}

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

    // M3U/HLS 文本清单也会被误当成音频流，需要提前识别
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

// 轻量级 MPEG 帧边界定位：通过帧头解析+下一帧校验，不依赖 PCM 解码。
// 即使数据中包含非 MP3 字节也能可靠跳过，适合在 Range 恢复后排除杂音使用。
// 返回 true 表示找到有效帧边界，*first_frame_offset 为第一个同步字偏移。
static bool find_mp3_frame_boundary(const uint8_t *data, int len, int *first_frame_offset)
{
    if (data == nullptr || len < 8 || first_frame_offset == nullptr) {
        return false;
    }

    // MPEG1 采样率表 (idx: 0=44100,1=48000,2=32000,3=reserved)
    static constexpr uint16_t k_sample_rates_mpeg1[4] = {44100, 48000, 32000, 0};
    // MPEG2/2.5 采样率表
    static constexpr uint16_t k_sample_rates_mpeg2[4] = {22050, 24000, 16000, 0};
    static constexpr uint16_t k_sample_rates_mpeg25[4] = {11025, 12000, 8000, 0};
    // 比特率表 (idx: 1..14, 0=free/invalid), MPEG1 Layer3
    static constexpr uint16_t k_bitrates_mpeg1_l3[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                                         112, 128, 160, 192, 224, 256, 320, 0};

    const int scan_limit = (len < 4096) ? len : 4096;
    for (int off = 0; off < scan_limit - 4; ++off) {
        // 检查 MPEG 同步字: 0xFFE0 (11个1 + 3bit)
        if (data[off] != 0xFF || (data[off + 1] & 0xE0) != 0xE0) {
            continue;
        }

        const uint8_t b2 = data[off + 1];
        const uint8_t b3 = data[off + 2];
        const uint8_t ver = (b2 >> 3) & 0x03; // MPEG version
        const uint8_t lyr = (b2 >> 1) & 0x03; // Layer
        const uint8_t brx = (b3 >> 4) & 0x0F; // Bitrate index
        const uint8_t srx = (b3 >> 2) & 0x03; // Sample rate index
        const uint8_t pad = (b3 >> 1) & 0x01; // Padding bit

        // 仅处理 Layer 3
        if (lyr != 1) {
            continue;
        }
        // 无效版本/比特率/采样率
        if (ver == 1 || brx == 0 || brx == 15 || srx == 3) {
            continue;
        }

        uint16_t sample_rate = 0;
        uint16_t bitrate = k_bitrates_mpeg1_l3[brx];
        if (bitrate == 0) {
            continue;
        }

        if (ver == 3) { // MPEG1
            sample_rate = k_sample_rates_mpeg1[srx];
        } else if (ver == 2) { // MPEG2
            sample_rate = k_sample_rates_mpeg2[srx];
        } else { // MPEG2.5
            sample_rate = k_sample_rates_mpeg25[srx];
        }
        if (sample_rate == 0) {
            continue;
        }

        // 计算帧长: MPEG1 Layer3 = 144*bitrate*1000/samplerate + pad
        //            MPEG2/2.5 Layer3 = 72*bitrate*1000/samplerate + pad
        // 注意：bitrate 单位是 kbps，乘 1000 转换为 bps。
        int frame_size = (ver == 3) ? (144 * (int)bitrate * 1000 / (int)sample_rate + (int)pad)
                                    : (72 * (int)bitrate * 1000 / (int)sample_rate + (int)pad);
        if (frame_size < 16 || frame_size > 2880) {
            continue;
        }

        // 验证下一个同步字
        const int next_off = off + frame_size;
        if (next_off + 2 > len) {
            continue; // 数据不足以验证，但仍可能找到
        }
        if (data[next_off] == 0xFF && (data[next_off + 1] & 0xE0) == 0xE0) {
            *first_frame_offset = off;
            return true;
        }
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
    // 检查是否可能是 ID3v2 版本，降低误报概率。
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

    // 首行格式为: HTTP/1.1 200 OK
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
        // 过滤保留值：检查连续帧，layer 不能为 00，bitrate/samplerate 不能是保留值
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

// ============================================================
// HTTP Chunked 传输解码器
// 状态机解析 "size\r\n data\r\n ... 0\r\n\r\n" 格式
// 参考 RFC 7230 §4.1，只解析基本格式，不支持 chunk-extension
// ============================================================
struct chunked_decoder {
    // 解析状态
    enum class st : uint8_t {
        rd_size,  // 读取 chunk 大小（十六进制字符串）
        skip_ext, // 跳过 chunk-extension（分号后直接到 \r）
        rd_lf,    // 等待 \n（size 行 CRLF 的第二字节）
        rd_data,  // 读取 chunk 数据体
        data_cr,  // 等待 chunk 数据后的 \r
        data_lf,  // 等待 chunk 数据后的 \n
        trailer,  // 读取 trailing headers（0-chunk 后）
        done,     // 收到 0-chunk + CRLF 结束标记
        err       // 格式异常
    };

    st state = st::rd_size;
    int32_t chunk_remaining = 0;
    std::array<char, 20> hex_buf = {0};
    int hex_len = 0;
    // trailer 状态需要检测 \r\n 结尾的空行
    bool trailer_prev_cr = false;

    void reset()
    {
        state = st::rd_size;
        chunk_remaining = 0;
        hex_buf[0] = '\0';
        hex_len = 0;
        trailer_prev_cr = false;
    }

    bool is_finished() const
    {
        return state == st::done;
    }
    bool has_error() const
    {
        return state == st::err;
    }

    // 逐字节喂入原始数据字节，提取解块后的数据写入 out[0..out_cap)。
    // 返回写入 out 的字节数 >=0；-1 表示格式错误。通过 is_done 标志检测 0-chunk。
    int feed(const uint8_t *in, int in_len, uint8_t *out, int out_cap, bool &is_done)
    {
        is_done = false;
        if (in == nullptr || in_len <= 0 || out == nullptr || out_cap <= 0) {
            return 0;
        }
        int out_pos = 0;
        for (int i = 0; i < in_len; ++i) {
            if (state == st::done) {
                is_done = true;
                break;
            }
            if (state == st::err) {
                return -1;
            }
            const uint8_t b = in[i];
            switch (state) {
                case st::rd_size:
                    if (b == '\r') {
                        hex_buf[hex_len] = '\0';
                        state = st::rd_lf;
                    } else if (b == ';') {
                        hex_buf[hex_len] = '\0';
                        state = st::skip_ext;
                    } else if ((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F')) {
                        if (hex_len < static_cast<int>(hex_buf.size()) - 1) {
                            hex_buf[hex_len++] = static_cast<char>(b);
                        } else {
                            osal_printk("chunked: chunk-size 字段过长\n");
                            state = st::err;
                            return -1;
                        }
                    } else {
                        osal_printk("chunked: chunk-size 包含非法字符 0x%02X\n", b);
                        state = st::err;
                        return -1;
                    }
                    break;

                case st::skip_ext:
                    if (b == '\r') {
                        state = st::rd_lf;
                    }
                    break;

                case st::rd_lf:
                    if (b != '\n') {
                        osal_printk("chunked: 期望 '\\n'，得到 0x%02X\n", b);
                        state = st::err;
                        return -1;
                    }
                    {
                        // 解析十六进制 chunk 大小，除非 hex_len==0 只有 rd_size
                        // 阶段标记
                        // \r 时提交
                        if (hex_len == 0) {
                            osal_printk("chunked: 缺少 chunk-size 字段\n");
                            state = st::err;
                            return -1;
                        }
                        char *end_ptr = nullptr;
                        unsigned long sz = strtoul(hex_buf.data(), &end_ptr, 16);
                        hex_buf[0] = '\0';
                        hex_len = 0;
                        chunk_remaining = static_cast<int32_t>(sz);
                        if (chunk_remaining == 0) {
                            // 收到 0-chunk，进入 trailer 尾阶段
                            state = st::trailer;
                            trailer_prev_cr = false;
                        } else {
                            state = st::rd_data;
                        }
                    }
                    break;

                case st::rd_data:
                    if (out_pos < out_cap) {
                        out[out_pos++] = b;
                    }
                    if (--chunk_remaining == 0) {
                        state = st::data_cr;
                    }
                    break;

                case st::data_cr:
                    if (b != '\r') {
                        osal_printk("chunked: chunk 尾部期望 '\\r'，得到 0x%02X\n", b);
                        state = st::err;
                        return -1;
                    }
                    state = st::data_lf;
                    break;

                case st::data_lf:
                    if (b != '\n') {
                        osal_printk("chunked: chunk 尾部期望 '\\n'，得到 0x%02X\n", b);
                        state = st::err;
                        return -1;
                    }
                    // 准备读取下一个 chunk
                    state = st::rd_size;
                    break;

                case st::trailer:
                    // 读取 trailer headers，等待空行 (\r\n) 表示结束
                    if (b == '\r') {
                        trailer_prev_cr = true;
                    } else if (b == '\n' && trailer_prev_cr) {
                        // 收到 \r\n，若是空行则完成，下一个 \r\n
                        // 则终止
                        state = st::done;
                        is_done = true;
                        trailer_prev_cr = false;
                    } else {
                        trailer_prev_cr = false;
                    }
                    break;

                case st::done:
                    is_done = true;
                    break;
                case st::err:
                    return -1;
            }
        }
        return out_pos;
    }
};

// ========= minimp3_ex IO callbacks 共享上下文 ============
// Context shared between read and seek callbacks.
struct stream_ex_io_ctx {
    stream_transport transport = {};
    stream_url_desc url = {};
    bool connected = false;
    uint64_t stream_pos = 0;       // 当前 HTTP 流已读取的文件字节偏移
    uint64_t mp3_start_offset = 0; // 文件中第一个 MP3 帧的偏移（= dec->start_offset）
    uint64_t content_length = 0;   // HTTP Content-Length，0=未知/chunked
    uint64_t seek_on_open = 0;     // 非 0 时，下次 open 时 seek(0) 将 Range 跳转到该字节
    bool seek_on_open_active = false;
    // Range 重定向后，mp3dec_ex_open_cb 会再次 seek(dec->start_offset)。
    // start_offset 是相对于 Range 起始位置的偏移，seek_base 记录 Range 起始
    // 绝对字节位置，用于将第二次 seek 的偏移转换为绝对位置。
    uint64_t seek_base = 0;
};

// 从 HTTP 响应头中提取 Content-Length，返回 0 表示未找到或解析失败。
static uint64_t parse_content_length_from_header(const char *header)
{
    if (header == nullptr) {
        return 0;
    }
    const char *p = header;
    while (*p) {
        // 跳过一行开头
        while (*p == '\r' || *p == '\n')
            ++p;
        if (*p == '\0')
            break;
        // 不区分大小写比较 "content-length:"
        if (!starts_with_ascii_ignore_case_local(p, "content-length:")) {
            // 跳到下一行
            const char *eol = strchr(p, '\r');
            if (eol == nullptr)
                eol = strchr(p, '\n');
            if (eol == nullptr)
                break;
            p = eol;
            continue;
        }
        p += 14; // strlen("content-length")
        // 跳过 ':' 和空格
        while (*p == ':' || *p == ' ' || *p == '\t')
            ++p;
        if (*p < '0' || *p > '9')
            return 0;
        uint64_t val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10ULL + (uint64_t)(*p - '0');
            ++p;
        }
        return val;
    }
    return 0;
}

// 从 HTTP 响应头中提取 Content-Range 的总长度（bytes X-Y/Z 中的 Z）。
// 返回 0 表示未找到或解析失败。
static uint64_t parse_content_range_total(const char *header)
{
    if (header == nullptr) {
        return 0;
    }
    const char *p = header;
    while (*p) {
        while (*p == '\r' || *p == '\n')
            ++p;
        if (*p == '\0')
            break;
        if (!starts_with_ascii_ignore_case_local(p, "content-range:")) {
            const char *eol = strchr(p, '\r');
            if (eol == nullptr)
                eol = strchr(p, '\n');
            if (eol == nullptr)
                break;
            p = eol;
            continue;
        }
        p += 13; // strlen("content-range")
        while (*p == ':' || *p == ' ' || *p == '\t')
            ++p;
        // 预期格式: bytes X-Y/Z 或 bytes */Z
        if (!starts_with_ascii_ignore_case_local(p, "bytes"))
            return 0;
        p += 5; // strlen("bytes")
        while (*p == ' ')
            ++p;
        // 跳过 X-Y 或 *
        while (*p >= '0' && *p <= '9')
            ++p;
        if (*p == '-') {
            ++p;
            while (*p >= '0' && *p <= '9')
                ++p;
        } else if (*p == '*') {
            ++p;
        }
        if (*p != '/')
            return 0;
        ++p; // 跳过 '/'
        while (*p == ' ')
            ++p;
        if (*p < '0' || *p > '9')
            return 0;
        uint64_t val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10ULL + (uint64_t)(*p - '0');
            ++p;
        }
        return val;
    }
    return 0;
}

// Blocking / retrying read for minimp3_ex.  Returns 0 only on EOF or
// unrecoverable error so that mp3dec_ex_read knows to stop.
static size_t stream_ex_read_cb(void *buf, size_t size, void *user_data)
{
    auto *ctx = static_cast<stream_ex_io_ctx *>(user_data);
    if (!ctx->connected || size == 0 || size > (size_t)INT_MAX) {
        return 0;
    }

    size_t total = 0;
    uint64_t start_ms = stream_now_ms();
    static constexpr uint32_t k_read_timeout_ms = 8000;

    while (total < size) {
        bool timed_out = false;
        int ret = stream_recv_some(ctx->transport, static_cast<uint8_t *>(buf) + total, static_cast<int>(size - total),
                                   &timed_out);
        if (ret > 0) {
            total += static_cast<size_t>(ret);
            ctx->stream_pos += static_cast<uint64_t>(ret);
            start_ms = stream_now_ms(); // reset timer on progress
            continue;
        }
        if (ret == k_stream_retry_later) {
            osal_msleep(10);
            if (stream_elapsed_ms(start_ms) > k_read_timeout_ms)
                break;
            continue;
        }
        if (timed_out) {
            if (stream_elapsed_ms(start_ms) > k_read_timeout_ms)
                break;
            osal_msleep(10);
            continue;
        }
        // ret <= 0, not retry-later, not timeout => hard error or remote close
        break;
    }
    return total;
}

// Seek callback: 支持向前跳跃字节偏移，避免 TCP 全局重连。超过 drain 阈值
// 或未连接时，重建 HTTP 连接。
static int stream_ex_seek_cb(uint64_t position, void *user_data)
{
    auto *ctx = static_cast<stream_ex_io_ctx *>(user_data);

    // 处理 seek_on_open：mp3dec_ex_open_cb 调用 seek(0) 时，
    // 若 seek_on_open_active 为真，则用 Range 请求直接跳转目标位置，
    // 避免 GET 再 drain 的浪费。同时记录 seek_base 用于后续第二次 seek。
    if (ctx->seek_on_open_active && position == 0) {
        ctx->seek_on_open_active = false;
        ctx->seek_base = ctx->seek_on_open; // 记录 Range 起始绝对位置，用于二次 seek 偏移转换
        position = ctx->seek_on_open;
        ctx->seek_on_open = 0;
        if (position > 0) {
            osal_printk("ex-seek: seek_on_open redirect to Range bytes=%llu-\n",
                        static_cast<unsigned long long>(position));
            goto do_reconnect;
        }
    }

    // 处理 post-Range 第二次 seek：
    // mp3dec_ex_open_cb 在 Range 重定向+扫描后，会用 start_offset 再次 seek。
    // start_offset 是相对于 Range 起始位置的偏移（通常 0 或 ID3 标签大小），
    // 借助 seek_base 转换为绝对文件偏移，避免错误跳到文件头部 GET。
    // 使用 position < seek_base 作为判断：start_offset 永远远小于 seek_base。
    // 而 mp3dec_iterate_cb 内部的 ID3 seek 会被正确转换为绝对位置而不触发 seek_base。
    if (ctx->seek_base > 0 && position < ctx->seek_base) {
        osal_printk("ex-seek: post-Range seek adjust: %llu + base %llu = %llu\n",
                    static_cast<unsigned long long>(position), static_cast<unsigned long long>(ctx->seek_base),
                    static_cast<unsigned long long>(position + ctx->seek_base));
        position += ctx->seek_base;
        ctx->seek_base = 0; // 单次有效，仅对第一次 post-Range seek 生效
    }

    // 快速路径：连接正常且目标位置 >= 当前位置，且跳跃字节数较少时
    if (ctx->connected && position >= ctx->stream_pos) {
        uint64_t to_skip = position - ctx->stream_pos;
        if (to_skip == 0) {
            return 0; // 已在目标位置
        }
        // 超过阈值说明距离太远，Range 请求更可靠
        if (to_skip > k_max_drain_bytes) {
            osal_printk("ex-seek: drain %llu exceeds limit %llu, reconnecting\n",
                        static_cast<unsigned long long>(to_skip), static_cast<unsigned long long>(k_max_drain_bytes));
            goto do_reconnect;
        }
        // 消费 to_skip 字节（小量读取并丢弃）
        uint8_t drain_buf[256];
        const uint64_t drain_start = stream_now_ms();
        static constexpr uint32_t k_drain_timeout_ms = 5000;
        while (to_skip > 0) {
            size_t chunk = (to_skip > sizeof(drain_buf)) ? sizeof(drain_buf) : static_cast<size_t>(to_skip);
            size_t got = stream_ex_read_cb(drain_buf, chunk, user_data);
            if (got == 0) {
                // 连接断开则走重连路径
                osal_printk("ex-seek: drain lost connection at %llu, reconnecting\n",
                            static_cast<unsigned long long>(ctx->stream_pos));
                goto do_reconnect;
            }
            to_skip -= got;
            // stream_ex_read_cb 已更新 ctx->stream_pos
            if (stream_elapsed_ms(drain_start) > k_drain_timeout_ms) {
                osal_printk("ex-seek: drain timeout, reconnecting\n");
                goto do_reconnect;
            }
        }
        return 0; // 消费完成
    }

do_reconnect:
    // 重连路径：断开旧连接，用 HTTP Range 请求重建
    free_stream_transport(ctx->transport);
    ctx->connected = false;
    ctx->stream_pos = 0;

    if (!open_stream_transport(ctx->transport, ctx->url)) {
        osal_printk("ex-seek: reconnect failed\n");
        return -1;
    }

    std::array<char, 640> request = {0};
    if (position > 0) {
        snprintf(request.data(), request.size(),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Range: bytes=%llu-\r\n"
                 "Accept: audio/mpeg, */*\r\n"
                 "Accept-Encoding: identity\r\n"
                 "Connection: close\r\n"
                 "Icy-MetaData: 0\r\n\r\n",
                 ctx->url.path.data(), ctx->url.host.data(), static_cast<unsigned long long>(position));
        osal_printk("ex-seek: Range bytes=%llu-\n", static_cast<unsigned long long>(position));
    } else {
        snprintf(request.data(), request.size(),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Accept: audio/mpeg, */*\r\n"
                 "Accept-Encoding: identity\r\n"
                 "Connection: close\r\n"
                 "Icy-MetaData: 0\r\n\r\n",
                 ctx->url.path.data(), ctx->url.host.data());
    }

    if (stream_send_all(ctx->transport, reinterpret_cast<const uint8_t *>(request.data()),
                        static_cast<int>(strlen(request.data()))) <= 0) {
        osal_printk("ex-seek: send request failed\n");
        free_stream_transport(ctx->transport);
        return -1;
    }

    // Drain HTTP response header
    std::array<char, 1024> header = {0};
    int header_len = 0;
    bool header_ended = false;
    const uint64_t hdr_start = stream_now_ms();

    while (!header_ended && header_len < static_cast<int>(header.size() - 1)) {
        bool timed_out = false;
        int ret = stream_recv_some(ctx->transport, reinterpret_cast<uint8_t *>(header.data()) + header_len,
                                   static_cast<int>(header.size() - 1 - header_len), &timed_out);
        if (ret == k_stream_retry_later) {
            osal_msleep(5);
            if (stream_elapsed_ms(hdr_start) > k_http_header_timeout_ms)
                break;
            continue;
        }
        if (ret < 0 && timed_out) {
            if (stream_elapsed_ms(hdr_start) < k_http_header_timeout_ms) {
                osal_msleep(5);
                continue;
            }
            break;
        }
        if (ret <= 0)
            break;

        header_len += ret;
        header[header_len] = '\0';
        if (strstr(header.data(), "\r\n\r\n") != nullptr) {
            header_ended = true;
            break;
        }
    }

    if (!header_ended) {
        osal_printk("ex-seek: header timeout\n");
        free_stream_transport(ctx->transport);
        return -1;
    }

    // 更新 Content-Length 与 Content-Range，供 seek 统计和进度追踪使用
    {
        // Content-Range 总长度：Range 请求的 206 响应携带完整文件大小(bytes X-Y/Z)
        uint64_t cr_total = parse_content_range_total(header.data());
        if (cr_total > 0) {
            ctx->content_length = cr_total;
            osal_printk("ex-seek: Content-Range total=%llu\n", static_cast<unsigned long long>(cr_total));
        } else if (position == 0) {
            // 仅在非 Range 请求(从头 GET)时使用 Content-Length。
            // Range 请求的 Content-Length 只是 range 块大小，不能作为文件总长度
            uint64_t cl = parse_content_length_from_header(header.data());
            if (cl > 0) {
                ctx->content_length = cl;
                osal_printk("ex-seek: Content-Length=%llu\n", static_cast<unsigned long long>(cl));
            }
        }
    }

    // Log status line
    char *status_end = strstr(header.data(), "\r\n");
    if (status_end != nullptr) {
        char saved = *status_end;
        *status_end = '\0';
        osal_printk("ex-seek HTTP: %s\n", header.data());
        *status_end = saved;
    }

    ctx->connected = true;
    ctx->stream_pos = position; // HTTP Range 响应从 position 开始
    return 0;
}

} // namespace

// 初始化静态成员变量
std::array<char, 512> minimp3::current_url = {0};
bool minimp3::is_playing = false;
bool minimp3::is_url_ready = false;
bool minimp3::is_paused = false;
bool minimp3::s_has_range = false;
bool minimp3::s_interrupt_stream = false;
volatile uint32_t minimp3::s_stream_epoch = 1;
uint64_t minimp3::s_range_start_byte = 0;
uint64_t minimp3::s_resume_target_byte = 0;
uint64_t minimp3::s_content_length = 0;
uint32_t minimp3::s_duration_seconds = 0;
uint64_t minimp3::s_bytes_streamed = 0;
uint32_t minimp3::s_avg_bitrate_bps = 0;
minimp3::iis_set_rate minimp3::iis_set_rate_func = nullptr;
volatile bool minimp3::s_exit_requested = false;
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

void minimp3::bump_stream_epoch()
{
    uint32_t irq = osal_irq_lock();
    if (s_stream_epoch == UINT32_MAX) {
        s_stream_epoch = 1;
    } else {
        s_stream_epoch++;
    }
    osal_irq_restore(irq);
}

void minimp3::prepare_url(const char *url)
{
    http_set_url(url, false);
}

void minimp3::play_url(const char *url)
{
    http_get_url(url);
}

void minimp3::request_exit()
{
    s_exit_requested = true;
    is_playing = false;
}

void minimp3::reset_exit()
{
    s_exit_requested = false;
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
            // 仅 Play 且无 URL，则仅启动播放开关。
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
        // 新 URL 时，重置所有位移追踪状态
        is_paused = false;
        s_has_range = false;
        s_range_start_byte = 0;
        s_resume_target_byte = 0;
        s_interrupt_stream = false;
        s_content_length = 0;
        s_duration_seconds = 0;
        s_bytes_streamed = 0;
        s_avg_bitrate_bps = 0;
        if (is_playing) {
            s_interrupt_stream = true;
            minimp3::bump_stream_epoch();
        }
    } else if (!is_playing && !is_url_ready) {
        // 同 URL 但暂停/停止恢复时，确保允许重新启动
        is_url_ready = true;
    }

    if (start_playback) {
        const bool should_restart_stream = (!is_playing) || is_paused || !is_same_url;
        is_playing = true;
        if (should_restart_stream) {
            s_interrupt_stream = true;
            minimp3::bump_stream_epoch();
        }
    }
}

void minimp3::http_get_url(const char *url)
{
    http_set_url(url, true);
}

void minimp3::http_stop()
{
    // 重置状态
    is_playing = false;
    is_url_ready = false;
    is_paused = false;
    s_has_range = false;
    s_range_start_byte = 0;
    s_resume_target_byte = 0;
    s_interrupt_stream = false;
    minimp3::bump_stream_epoch();
}

void minimp3::http_clear_url()
{
    // 清除 URL 内容
    memset(current_url.data(), 0, current_url.size());
    // 重置状态
    is_playing = false;
    is_url_ready = false;
    is_paused = false;
    s_has_range = false;
    s_range_start_byte = 0;
    s_resume_target_byte = 0;
    s_interrupt_stream = false;
    s_content_length = 0;
    s_duration_seconds = 0;
    s_bytes_streamed = 0;
    s_avg_bitrate_bps = 0;
    minimp3::bump_stream_epoch();
}

void minimp3::pause_playback()
{
    if (!is_playing) {
        return;
    }
    // 记录位置，用于 Range 恢复播放。
    // 暂停时，循环会关闭 TCP 连接并释放 LWIP 内存，
    // 否则累积的缓冲数据撑爆 LWIP 导致 DLNA NOTIFY 的 lwip_send 发送内存失败。
    s_resume_target_byte = s_bytes_streamed;
    s_range_start_byte = compute_range_request_offset(s_resume_target_byte);
    s_has_range = true;
    is_paused = true;
    is_playing = false;
    s_interrupt_stream = true;
    osal_printk("pause_playback: paused at byte %llu, Range start=%llu\n", (unsigned long long)s_resume_target_byte,
                (unsigned long long)s_range_start_byte);
    minimp3::bump_stream_epoch();
}

void minimp3::resume_playback()
{
    if (!is_paused) {
        osal_printk("resume_playback: not paused, ignored\n");
        return;
    }
    if (current_url[0] == '\0') {
        osal_printk("resume_playback: no URL set, cannot resume\n");
        is_paused = false;
        return;
    }
    osal_printk("resume_playback: resuming from byte %llu\n", static_cast<unsigned long long>(s_resume_target_byte));
    // 暂停时 TCP 连接已关闭。s_has_range 仍设置，主循环通过 REOPEN
    // 走 Range 请求恢复。IO 缓冲区为静态预分配，不会失败。
    is_paused = false;
    is_playing = true;
    minimp3::bump_stream_epoch();
}

bool minimp3::get_is_paused()
{
    return is_paused;
}

void minimp3::seek_to_seconds(uint32_t seconds)
{
    uint64_t byte_offset = 0;
    if (s_content_length > 0 && s_duration_seconds > 0) {
        if (seconds >= s_duration_seconds) {
            byte_offset = (s_content_length > 16U) ? (s_content_length - 16U) : 0U;
        } else {
            byte_offset = (uint64_t)seconds * s_content_length / (uint64_t)s_duration_seconds;
        }
    } else if (s_avg_bitrate_bps > 0) {
        // 方案2: 按帧/平均比特率估算 (CBR 准确, VBR 近似)
        byte_offset = (uint64_t)seconds * (s_avg_bitrate_bps / 8U);
    } else {
        // 兜底: 假设 128kbps CBR，在 s_content_length/s_avg_bitrate 均未就绪时用于粗跳
        static constexpr uint32_t k_fallback_bitrate_bps = 128000U;
        byte_offset = (uint64_t)seconds * (k_fallback_bitrate_bps / 8U);
        osal_printk("seek_to_seconds: using fallback 128kbps (cl=%llu dur=%u bps=%u)\n",
                    static_cast<unsigned long long>(s_content_length), static_cast<unsigned>(s_duration_seconds),
                    static_cast<unsigned>(s_avg_bitrate_bps));
    }

    s_resume_target_byte = byte_offset;
    s_range_start_byte = compute_range_request_offset(s_resume_target_byte);
    s_has_range = true;
    is_paused = false;
    is_playing = true;
    minimp3::bump_stream_epoch();
    osal_printk("seek_to_seconds: %us -> target_byte=%llu range_start=%llu (cl=%llu dur=%us bps=%u)\n",
                (unsigned)seconds, (unsigned long long)byte_offset, (unsigned long long)s_range_start_byte,
                (unsigned long long)s_content_length, (unsigned)s_duration_seconds, (unsigned)s_avg_bitrate_bps);
}

uint32_t minimp3::get_duration_seconds()
{
    return s_duration_seconds;
}

void minimp3::stream_mp3_to_iis()
{
    // ===== IO context shared between the read / seek callbacks =====
    static stream_ex_io_ctx io_ctx = {};

    mp3dec_io_t io = {};
    io.read = stream_ex_read_cb;
    io.read_data = &io_ctx;
    io.seek = stream_ex_seek_cb;
    io.seek_data = &io_ctx;

    mp3dec_ex_t dec;
    memset(&dec, 0, sizeof(dec));
    bool dec_open = false;

    // 重试计数器，防止失败时无限重试耗光 CPU 和日志
    int open_retry_count = 0;
    static constexpr int k_max_open_retries = 5;
    static constexpr uint32_t k_open_retry_base_ms = 200;

    // PCM buffer for IIS output — 静态分配，避免堆碎片化导致分配失败
    static constexpr size_t k_pcm_batch_samples = 1152 * 2;
    static int16_t s_pcm_buf[k_pcm_batch_samples];
    int16_t *pcm_buf = s_pcm_buf;

    while (true) {
        if (s_exit_requested) {
            osal_printk("[minimp3] exit requested\r\n");
            break;
        }
        // ===== Wait while paused / stopped =====
        if (!is_playing) {
            if (dec_open && is_paused) {
                // 暂停：清空 IIS 防止缓冲区溢出 data_write
                // 同线程，无竞争。 记录当前位置后关闭解码器和 TCP 连接，释放
                // LWIP 内存。 否则累积的缓冲数据撑爆 LWIP 导致 DLNA NOTIFY 的 lwip_send 发送内存失败。
                iis::data_clear();
                s_resume_target_byte = s_bytes_streamed;
                s_range_start_byte = compute_range_request_offset(s_resume_target_byte);
                s_has_range = true;
                mp3dec_ex_close(&dec);
                dec_open = false;
                free_stream_transport(io_ctx.transport);
                io_ctx.connected = false;
                io_ctx.stream_pos = 0;
            } else if (dec_open && !is_paused) {
                // Stopped: 清空 IIS，释放解码器和传输资源以回收可控内存。
                iis::data_clear();
                mp3dec_ex_close(&dec);
                dec_open = false;
                free_stream_transport(io_ctx.transport);
                io_ctx.connected = false;
                io_ctx.stream_pos = 0;
            }
            // 停止状态时，重置重试计数器以便下次播放重新开始
            open_retry_count = 0;
            osal_msleep(100);
            continue;
        }

        // ===== Open / reopen decoder when URL changes =====
        if (is_url_ready || !dec_open) {
            if (dec_open) {
                mp3dec_ex_close(&dec);
                dec_open = false;
            }
            free_stream_transport(io_ctx.transport);
            io_ctx.connected = false;
            io_ctx.stream_pos = 0;
            // 等 LWIP / mbedTLS 超时释放 TCP PCB 内部资源，
            // 避免下一次 malloc(18KB) 堆碎片失败。
            if (open_retry_count == 0) {
                osal_msleep(300);
            }

            // 内存压力检查：连续多次打开失败，等待更长时间以回收系统资源
            if (open_retry_count >= k_max_open_retries) {
                osal_printk("minimp3_ex: too many open failures (%d), stopping playback\n", open_retry_count);
                is_playing = false;
                is_url_ready = false;
                open_retry_count = 0;
                osal_msleep(500);
                continue;
            }

            if (!parse_stream_url(current_url.data(), io_ctx.url)) {
                osal_printk("minimp3_ex: URL parse fail: %s\n", current_url.data());
                open_retry_count++;
                uint32_t delay = k_open_retry_base_ms * (1U << (open_retry_count > 4 ? 4 : open_retry_count));
                osal_msleep(delay);
                continue;
            }

            // 重置 io_ctx
            // 尽可能干净的初始状态，防止上一次残留的参数值污染新连接。
            io_ctx.content_length = 0;
            io_ctx.mp3_start_offset = 0;
            io_ctx.seek_on_open = 0;
            io_ctx.seek_on_open_active = false;
            io_ctx.seek_base = 0;

            // 处理 pending seek（来自 seek_to_seconds 或 resume）
            // 通过 seek_on_open，使 stream_ex_seek_cb 在 seek(0) 时使用 Range 请求
            if (s_has_range && s_resume_target_byte > 0) {
                io_ctx.seek_on_open = s_range_start_byte;
                io_ctx.seek_on_open_active = true;
                osal_printk("minimp3_ex: seek_on_open Range bytes=%llu-\n",
                            static_cast<unsigned long long>(s_range_start_byte));
            }

            // Fast open – byte-level seeking only, NO sample index (avoids full-file scan)
            int ret = mp3dec_ex_open_cb(&dec, &io, MP3D_DO_NOT_SCAN);
            io_ctx.seek_on_open_active = false; // 清除标志
            if (ret != 0) {
                osal_printk("minimp3_ex: open_cb failed ret=%d (attempt %d)\n", ret, open_retry_count + 1);
                // mp3dec_ex_open_cb 在返回 IO 错误前内部可能已分配缓冲区，
                // 需要调用 mp3dec_ex_close 释放已分配的内存/标志静态缓冲区已占用标记。
                mp3dec_ex_close(&dec);
                free_stream_transport(io_ctx.transport);
                io_ctx.connected = false;
                io_ctx.stream_pos = 0;
                open_retry_count++;
                uint32_t delay = k_open_retry_base_ms * (1U << (open_retry_count > 4 ? 4 : open_retry_count));
                if (ret == MP3D_E_MEMORY) {
                    // 内存不足，等更长时间让 LWIP / TLS 释放资源
                    delay += 800;
                    osal_printk("minimp3_ex: memory exhausted, waiting %u ms\n", delay);
                }
                osal_msleep(delay);
                continue;
            }
            // 打开成功，重置重试计数
            open_retry_count = 0;
            dec_open = true;
            is_url_ready = false;
            io_ctx.mp3_start_offset = dec.start_offset; // 记录文件中 MP3 音频起始偏移

            // 从 open_cb 已解码的第一帧头获取初始比特率（精确到帧），确保 seek 时 byte_offset 可用
            if (dec.info.bitrate_kbps > 0 && s_avg_bitrate_bps == 0) {
                s_avg_bitrate_bps = static_cast<uint32_t>(dec.info.bitrate_kbps) * 1000U;
            }

            // Sync Content-Length from HTTP response header (parsed in stream_ex_seek_cb).
            // Content-Range 提供完整大小优先，非 Range 请求的 Content-Length 也可接受。
            if (io_ctx.content_length > 0) {
                s_content_length = io_ctx.content_length;
                osal_printk("minimp3_ex: Content-Length=%llu\n", static_cast<unsigned long long>(s_content_length));
            }

            // 处理 pending seek（来自 seek_to_seconds 等操作）
            // Range 请求已定位到预读位置，从 Range 起始字节开始追踪位置，
            // 逐帧累计 frame_bytes 来逐步逼近目标位置。
            // 注意：无论 s_resume_target_byte 是否为 0，都重置 s_has_range，
            // 避免下一循环重复触发 mid-playback seek 的重新连接逻辑。
            if (s_has_range) {
                osal_printk("minimp3_ex: seeked, Range start=%llu target=%llu\n",
                            static_cast<unsigned long long>(s_range_start_byte),
                            static_cast<unsigned long long>(s_resume_target_byte));
                s_bytes_streamed = s_range_start_byte;
                s_has_range = false;
                s_resume_target_byte = 0;
                s_range_start_byte = 0;
            } else {
                s_bytes_streamed = 0; // 从头开始计数
            }

            // Apply detected sampling rate
            if (dec.info.hz > 0 && iis_set_rate_func) {
                iis_set_rate_func(dec.info.hz);
                osal_printk("minimp3_ex: rate locked %d Hz, layer=%d ch=%d start_off=%llu\n", dec.info.hz,
                            dec.info.layer, dec.info.channels,
                            static_cast<unsigned long long>(io_ctx.mp3_start_offset));
            }
            if (dec.detected_samples > 0 && dec.info.hz > 0) {
                s_duration_seconds = static_cast<uint32_t>(dec.detected_samples / dec.info.hz);
                osal_printk("minimp3_ex: duration %u s (from VBR tag)\n", static_cast<unsigned>(s_duration_seconds));
            }
            if (dec.vbr_tag_found) {
                osal_printk("minimp3_ex: VBR tag detected\n");
            }
        }

        // ===== Pending seek during playback (e.g. DLNA seek / resume) =====
        if (s_has_range && dec_open) {
            osal_printk("minimp3_ex: mid-playback seek to byte %llu\n",
                        static_cast<unsigned long long>(s_resume_target_byte));
            // 清空 IIS，防止旧数据在切换到新位置时残留；单 data_write
            // 同线程无竞争。
            iis::data_clear();
            // 通过 seek_on_open 在 reopen 时使用 Range 请求，避免 GET 再
            // drain。
            io_ctx.seek_on_open = s_range_start_byte;
            io_ctx.seek_on_open_active = true;
            mp3dec_ex_close(&dec);
            dec_open = false;
            free_stream_transport(io_ctx.transport);
            io_ctx.connected = false;
            io_ctx.stream_pos = 0;
            is_url_ready = true; // 触发 reopen 路径
            continue;
        }

        // ===== Read one frame of decoded PCM =====
        mp3dec_frame_info_t frame_info;
        memset(&frame_info, 0, sizeof(frame_info));
        mp3d_sample_t *frame_samples = nullptr;
        size_t n = mp3dec_ex_read_frame(&dec, &frame_samples, &frame_info, k_pcm_batch_samples);

        // ===== Update position / bitrate trackers =====
        // 即使 n==0（跳过了非音频帧/to_skip），n=0 但 frame_bytes>0。
        // 始终更新位置追踪和比特率。
        if (frame_info.bitrate_kbps > 0) {
            s_avg_bitrate_bps = static_cast<uint32_t>(frame_info.bitrate_kbps) * 1000U;
        }
        if (frame_info.frame_bytes > 0) {
            s_bytes_streamed += static_cast<uint64_t>(frame_info.frame_bytes);
        }

        if (n == 0) {
            // 帧跳（跳过了非音频，可能是 bit reservoir 或 encoder delay），
            // frame_bytes > 0 表示消耗了流数据但未输出 PCM 采样。
            // 继续循环，不停止播放。
            if (dec.last_error == 0 && frame_info.frame_bytes > 0) {
                continue;
            }
            if (dec.last_error != 0) {
                osal_printk("minimp3_ex: decode error %d, stopping\n", dec.last_error);
            } else {
                osal_printk("minimp3_ex: stream EOF at byte %llu, stopping playback\n",
                            static_cast<unsigned long long>(io_ctx.stream_pos));
            }
            mp3dec_ex_close(&dec);
            dec_open = false;
            free_stream_transport(io_ctx.transport);
            io_ctx.connected = false;
            io_ctx.stream_pos = 0;
            // 流结束停止播放，不是错误，不自动重试（保持循环和内存）。
            // DLNA 控制端会通过 SetAVTransportURI+Play 发起下一首歌。
            s_range_start_byte = s_bytes_streamed;
            s_has_range = false;
            s_resume_target_byte = 0;
            is_playing = false;
            is_url_ready = false;
            open_retry_count = 0;
            osal_msleep(500);
            continue;
        }

        // ===== Rate-change detection =====
        if (frame_info.hz > 0 && frame_info.hz != dec.info.hz && iis_set_rate_func) {
            const int old_hz = dec.info.hz;
            iis_set_rate_func(frame_info.hz);
            osal_printk("minimp3_ex: rate switch %d -> %d Hz\n", old_hz, frame_info.hz);
        }

        // ===== Push PCM to IIS =====
        if (mp3_get_into_iis_func) {
            if (frame_info.channels == 1) {
                // Mono → stereo expansion
                for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
                    pcm_buf[2 * i] = frame_samples[i];
                    pcm_buf[2 * i + 1] = frame_samples[i];
                }
                mp3_get_into_iis_func(pcm_buf, static_cast<uint32_t>(n * 2));
            } else {
                mp3_get_into_iis_func(frame_samples, static_cast<uint32_t>(n));
            }
        }

        // ===== Duration from VBR tag =====
        if (dec.detected_samples > 0 && dec.info.hz > 0 && s_duration_seconds == 0) {
            s_duration_seconds = static_cast<uint32_t>(dec.detected_samples / dec.info.hz);
        }
        // Content-Length is synced from io_ctx.content_length during REOPEN.
        // dec.end_offset is only the scan window boundary, NOT the file size.

        // ===== Back-pressure from IIS queue =====
        if (playback_queue_level_getter_func) {
            int q = playback_queue_level_getter_func();
            if (q >= 34) {
                osal_msleep(2); // hard high-water: slow down decode
            }
        }
    }

    if (dec_open) {
        mp3dec_ex_close(&dec);
    }
    free_stream_transport(io_ctx.transport);
    io_ctx.connected = false;
}