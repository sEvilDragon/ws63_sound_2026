// WS63 LiteOS: �� mmap/munmap/stdio��ֻ�ûص� API
#define MINIMP3_NO_STDIO
// ��С IO ������������ malloc(128KB) ��Ƕ��ʽ����ʧ��
// ϵͳ���ö�Լ 340KB����Ϊ TLS/�׽���/HTTP ͷ����ռ�
#define MINIMP3_IO_SIZE (18 * 1024)
#define MINIMP3_IMPLEMENTATION

// ���ף����Ԥ����δ��ȷ�ų� mmap/munmap ·�����ṩ��׮������������
// RISCV musl ����������Ԥ���� __linux__������ minimp3_ex.h �е�
// #if defined(__linux__) ��֧�����롪����ʹ��� MINIMP3_NO_STDIO �Ѷ��塣
// ��׮�������ᱻʵ�ʵ��ã���Ϊ�ص� IO ·���������ļ�ӳ����룩��
// �������������ӽ׶ε� undefined reference��
extern "C" {
int munmap(void *addr, unsigned long length)
{
    (void)addr;
    (void)length;
    return -1;
}
}

#include "minimp3.hpp"

// ���� ��̬Ԥ���� IO ������ ������������������������������������������������������������������������������������
// mp3dec_ex_open_cb �ڲ��� malloc(MINIMP3_IO_SIZE)������Ƭ������
// ����ʧ�ܣ�340KB �Ѳ��ź��ʣ 3~10KB ���У���
// ͨ������ MINIMP3_ALLOC_IO / MINIMP3_FREE_IO �꣬����̬�����滻Ϊ
// ����ʱ������õľ�̬�����������������˹��ϵ㡣
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
static constexpr uint64_t k_range_preroll_bytes = 8ULL * 1024ULL; // MP3 bit reservoir < 512B, 8KB �㹻
static constexpr uint32_t k_range_recovery_read_timeout_ms = 400;
// ������� drain ���ֽ�������������ֵ����˵� Range ������������������·��
// ��ʱ�䶪�����ݡ�
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
        osal_printk("HTTPS��Դע��ʧ��: ret=-0x%04X\n", -ret);
        return false;
    }

    static const char *k_tls_personalization = "ws63-minimp3";
    ret = mbedtls_ctr_drbg_seed(&transport.tls_ctr_drbg, mbedtls_entropy_func, &transport.tls_entropy,
                                reinterpret_cast<const unsigned char *>(k_tls_personalization),
                                strlen(k_tls_personalization));
    if (ret != 0) {
        osal_printk("HTTPS�������ʼ��ʧ��: ret=-0x%04X\n", -ret);
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
        osal_printk("����socketʧ��\n");
        return false;
    }

    timeval timeout = {5, 0};
    lwip_setsockopt(transport.plain_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    lwip_setsockopt(transport.plain_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(url.port);
    if (!resolve_ipv4_addr(url.host.data(), &addr.sin_addr)) {
        osal_printk("�޷�����������ַ: %s\n", url.host.data());
        return false;
    }

    if (lwip_connect(transport.plain_sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
        osal_printk("���ӷ�����ʧ��: %s\n", url.host.data());
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
        osal_printk("HTTPS����ʧ��: ret=-0x%04X\n", -ret);
        return false;
    }

    mbedtls_ssl_conf_authmode(&transport.tls_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&transport.tls_conf, mbedtls_ctr_drbg_random, &transport.tls_ctr_drbg);
    mbedtls_ssl_conf_min_version(&transport.tls_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&transport.tls_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_read_timeout(&transport.tls_conf, k_tls_initial_read_timeout_ms);

    ret = mbedtls_ssl_setup(&transport.tls_ssl, &transport.tls_conf);
    if (ret != 0) {
        osal_printk("HTTPS SSL setupʧ��: ret=-0x%04X\n", -ret);
        return false;
    }

    ret = mbedtls_ssl_set_hostname(&transport.tls_ssl, url.host.data());
    if (ret != 0) {
        osal_printk("HTTPS����SNIʧ��: host=%s ret=-0x%04X\n", url.host.data(), -ret);
        return false;
    }

    std::array<char, 8> port_text = {0};
    snprintf(port_text.data(), port_text.size(), "%u", static_cast<unsigned int>(url.port));
    ret = mbedtls_net_connect(&transport.tls_net, url.host.data(), port_text.data(), MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        osal_printk("HTTPS���ӷ�����ʧ��: host=%s ret=-0x%04X\n", url.host.data(), -ret);
        return false;
    }

    timeval send_timeout = {5, 0};
    lwip_setsockopt(transport.tls_net.fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    mbedtls_ssl_set_bio(&transport.tls_ssl, &transport.tls_net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);

    const uint64_t handshake_start_ms = stream_now_ms();
    do {
        ret = mbedtls_ssl_handshake(&transport.tls_ssl);
        if (ret == 0) {
            osal_printk("HTTPS���ֳɹ�: host=%s\n", url.host.data());
            return true;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE && ret != MBEDTLS_ERR_SSL_TIMEOUT) {
            break;
        }
    } while (stream_elapsed_ms(handshake_start_ms) < k_tls_handshake_timeout_ms);

    osal_printk("HTTPS����ʧ��: host=%s ret=-0x%04X elapsed=%llu ms\n", url.host.data(), -ret,
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
        osal_printk("HTTPS����ʧ��: ret=-0x%04X\n", -ret);
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

    osal_printk("HTTPS����ʧ��: ret=-0x%04X\n", -ret);
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

    // M3U/HLS �ı��嵥Ҳ�ᱻ�󵱳���Ƶ�壬�������ʶ��
    if (len >= 7 && data[0] == '#' && data[1] == 'E' && data[2] == 'X' && data[3] == 'T' && data[4] == 'M' &&
        data[5] == '3' && data[6] == 'U') {
        return true;
    }

    // ADTS AAC ͬ���֣�minimp3 �޷����롣
    if (len >= 2 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0) {
        return true;
    }

    return false;
}

// ������ MPEG ֡�߽綨λ����ͨ��֡ͷ��������֡������֤��������ͬ���֡�
// ������������Ҳ������ PCM ���壬�ʺ��� Range �ָ���ջ���ų���ʹ�á�
// ���� true ��ʾ�ҵ���Ч֡�߽磬*first_frame_offset Ϊ��һ��ͬ����ƫ�ơ�
static bool find_mp3_frame_boundary(const uint8_t *data, int len, int *first_frame_offset)
{
    if (data == nullptr || len < 8 || first_frame_offset == nullptr) {
        return false;
    }

    // MPEG1 �����ʱ� (idx: 0=44100,1=48000,2=32000,3=reserved)
    static constexpr uint16_t k_sample_rates_mpeg1[4] = {44100, 48000, 32000, 0};
    // MPEG2/2.5 �����ʱ�
    static constexpr uint16_t k_sample_rates_mpeg2[4] = {22050, 24000, 16000, 0};
    static constexpr uint16_t k_sample_rates_mpeg25[4] = {11025, 12000, 8000, 0};
    // �����ʱ� (idx: 1..14, 0=free/invalid), MPEG1 Layer3
    static constexpr uint16_t k_bitrates_mpeg1_l3[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                                         112, 128, 160, 192, 224, 256, 320, 0};

    const int scan_limit = (len < 4096) ? len : 4096;
    for (int off = 0; off < scan_limit - 4; ++off) {
        // ��� MPEG ͬ����: 0xFFE0 (11��1 + 3bit)
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

        // ������ Layer 3
        if (lyr != 1) {
            continue;
        }
        // ��Ч�汾/������/������
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

        // ����֡��: MPEG1 Layer3 = 144*bitrate*1000/samplerate + pad
        //            MPEG2/2.5 Layer3 = 72*bitrate*1000/samplerate + pad
        // ע�⣺bitrate ��λ�� kbps����� 1000 ת��Ϊ bps��
        int frame_size = (ver == 3) ? (144 * (int)bitrate * 1000 / (int)sample_rate + (int)pad)
                                    : (72 * (int)bitrate * 1000 / (int)sample_rate + (int)pad);
        if (frame_size < 16 || frame_size > 2880) {
            continue;
        }

        // ��֤��һ��ͬ����
        const int next_off = off + frame_size;
        if (next_off + 2 > len) {
            continue; // ���ݲ�������֤�������ҵ�
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
    // �����ܳ���ID3v2�汾���������и��ʡ�
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

    // ��������Ϊ: HTTP/1.1 200 OK
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
        // ���˱���ֵ���������У�layer����Ϊ00��bitrate/samplerate���������Ǳ���ֵ
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
// HTTP Chunked ���������
// ״̬������ "size\r\n data\r\n ... 0\r\n\r\n" ��ʽ
// �ο� RFC 7230 ��4.1����������ʽ����������Ӽ���
// ============================================================
struct chunked_decoder {
    // ����״̬
    enum class st : uint8_t {
        rd_size,  // ��ȡ chunk ��С��ʮ�������ַ���
        skip_ext, // ���� chunk-extension���ֺź�ֱ��\r��
        rd_lf,    // �ȴ� \n��size �е� CRLF �ĵڶ��ֽڣ�
        rd_data,  // ��ȡ chunk ������
        data_cr,  // �ȴ� chunk ���ݺ�� \r
        data_lf,  // �ȴ� chunk ���ݺ�� \n
        trailer,  // ��ȡ trailing headers��0-chunk ��
        done,     // ���� 0-chunk + CRLF ������
        err       // ��������
    };

    st state = st::rd_size;
    int32_t chunk_remaining = 0;
    std::array<char, 20> hex_buf = {0};
    int hex_len = 0;
    // trailer ״̬����Ҫ���� \r\n ��β�Ŀ���
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

    // �������ι��ԭʼ�����ֽڣ�����������Ƶ��д�� out[0..out_cap)��
    // ����д�� out ���ֽ�����>=0����-1 ��ʾ��������ͨ�� is_done �������� 0-chunk��
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
                            osal_printk("chunked: chunk-size �ֶι���\n");
                            state = st::err;
                            return -1;
                        }
                    } else {
                        osal_printk("chunked: chunk-size ���Ƿ��ַ� 0x%02X\n", b);
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
                        osal_printk("chunked: ���� '\\n'���õ� 0x%02X\n", b);
                        state = st::err;
                        return -1;
                    }
                    {
                        // ����ʮ������ chunk ��С������ hex_len==0 ֻ�� rd_size �׶α� \r ����ʱ��
                        if (hex_len == 0) {
                            osal_printk("chunked: �� chunk-size �ֶ�\n");
                            state = st::err;
                            return -1;
                        }
                        char *end_ptr = nullptr;
                        unsigned long sz = strtoul(hex_buf.data(), &end_ptr, 16);
                        hex_buf[0] = '\0';
                        hex_len = 0;
                        chunk_remaining = static_cast<int32_t>(sz);
                        if (chunk_remaining == 0) {
                            // ���� 0-chunk������ trailer ���ѽ׶�
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
                        osal_printk("chunked: chunk β������ '\\r'���õ� 0x%02X\n", b);
                        state = st::err;
                        return -1;
                    }
                    state = st::data_lf;
                    break;

                case st::data_lf:
                    if (b != '\n') {
                        osal_printk("chunked: chunk β������ '\\n'���õ� 0x%02X\n", b);
                        state = st::err;
                        return -1;
                    }
                    // ׼����ȡ��һ�� chunk
                    state = st::rd_size;
                    break;

                case st::trailer:
                    // ���� trailer headers���ȴ����� (\r\n) ��ʾ����
                    if (b == '\r') {
                        trailer_prev_cr = true;
                    } else if (b == '\n' && trailer_prev_cr) {
                        // ���� \r\n�����ǿ��У��򻯣���һ�� \r\n ����ֹ��
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

// ������ minimp3_ex IO callbacks ������������������������������������������������������������������������������
// Context shared between read and seek callbacks.
struct stream_ex_io_ctx {
    stream_transport transport = {};
    stream_url_desc url = {};
    bool connected = false;
    uint64_t stream_pos = 0;       // ��ǰ HTTP ���Ѷ�ȡ�����ļ��ֽ�ƫ��
    uint64_t mp3_start_offset = 0; // �ļ��е�һ�� MP3 ֡��ƫ�ƣ�= dec->start_offset��
    uint64_t content_length = 0;   // HTTP Content-Length��0=δ֪/chunked��
    uint64_t seek_on_open = 0;     // ��0ʱ���´� open �� seek(0) ���� Range ����λ�����ֽ�
    bool seek_on_open_active = false;
    // Range �ض����mp3dec_ex_open_cb ���ٴ� seek(dec->start_offset)��
    // start_offset ������� Range ��ʼλ�õ�ƫ�ƣ�seek_base ��¼ Range ��ʼ
    // �����ֽ�λ�ã����ڽ��ڶ��� seek �����ƫ��ת��Ϊ����λ�á�
    uint64_t seek_base = 0;
};

// �� HTTP ��Ӧͷ����ȡ Content-Length������ 0 ��ʾδ�ҵ������ʧ�ܡ�
static uint64_t parse_content_length_from_header(const char *header)
{
    if (header == nullptr) {
        return 0;
    }
    const char *p = header;
    while (*p) {
        // ����һ�п�ͷ
        while (*p == '\r' || *p == '\n')
            ++p;
        if (*p == '\0')
            break;
        // �����ִ�Сд�Ƚ� "content-length:"
        if (!starts_with_ascii_ignore_case_local(p, "content-length:")) {
            // ������һ��
            const char *eol = strchr(p, '\r');
            if (eol == nullptr)
                eol = strchr(p, '\n');
            if (eol == nullptr)
                break;
            p = eol;
            continue;
        }
        p += 14; // strlen("content-length")
        // ���� ':' �Ϳո�
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

// �� HTTP ��Ӧͷ����ȡ Content-Range ���������ȣ�bytes X-Y/Z �е� Z����
// ���� 0 ��ʾδ�ҵ������ʧ�ܡ�
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
        // ������ʽ: bytes X-Y/Z �� bytes */Z
        if (!starts_with_ascii_ignore_case_local(p, "bytes"))
            return 0;
        p += 5; // strlen("bytes")
        while (*p == ' ')
            ++p;
        // ���� X-Y �� *
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
        ++p; // ���� '/'
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
        // ret <= 0, not retry-later, not timeout �� hard error or remote close
        break;
    }
    return total;
}

// Seek callback: �����ſ��ֽ�ǰ�ƣ����� TCP ��������������Ҫ���ˡ����� drain ��ֵ
// ��δ����ʱ������ HTTP��
static int stream_ex_seek_cb(uint64_t position, void *user_data)
{
    auto *ctx = static_cast<stream_ex_io_ctx *>(user_data);

    // ���� ���� seek_on_open���� minimp3_ex_open_cb ���� seek(0) ʱ��
    // ��� seek_on_open_active Ϊ�棬���� Range ����ֱ������Ŀ��λ�ã�
    // ������ GET �� drain ���˷ѡ�ͬʱ��¼ seek_base ���ں����ڶ��� seek�� ����
    if (ctx->seek_on_open_active && position == 0) {
        ctx->seek_on_open_active = false;
        ctx->seek_base = ctx->seek_on_open; // ��¼ Range ��ʼ����λ�ã����ڶ��� seek ƫ��ת��
        position = ctx->seek_on_open;
        ctx->seek_on_open = 0;
        if (position > 0) {
            osal_printk("ex-seek: seek_on_open redirect to Range bytes=%llu-\n",
                        static_cast<unsigned long long>(position));
            goto do_reconnect;
        }
    }

    // ���� ���� post-Range �ڶ��� seek ����
    // mp3dec_ex_open_cb �� Range �ض���+ɨ��󣬻��� start_offset �ٴ� seek��
    // start_offset ������� Range ��ʼλ�õ�ƫ�ƣ�ͨ�� 0 �� ID3 ��ǩ��С����
    // ������� seek_base ת��Ϊ�����ļ�ƫ�ƣ��������˵��ļ�ͷ���� GET��
    // ʹ�� position < seek_base ��Ϊ�ж�������start_offset ����ԶС�� seek_base��
    // �� mp3dec_iterate_cb �ڲ��� ID3 seek �ᱻ��ȷת��Ϊ����λ���Ҳ����� seek_base��
    if (ctx->seek_base > 0 && position < ctx->seek_base) {
        osal_printk("ex-seek: post-Range seek adjust: %llu + base %llu = %llu\n",
                    static_cast<unsigned long long>(position), static_cast<unsigned long long>(ctx->seek_base),
                    static_cast<unsigned long long>(position + ctx->seek_base));
        position += ctx->seek_base;
        ctx->seek_base = 0; // ���ѣ����Ե�һ�� post-Range seek ��Ч
    }

    // ���� ����·������������Ŀ��λ�� >= ��ǰλ�� �� �ſ��ֽڼ��� ����
    if (ctx->connected && position >= ctx->stream_pos) {
        uint64_t to_skip = position - ctx->stream_pos;
        if (to_skip == 0) {
            return 0; // ����Ŀ��λ��
        }
        // ������ֵ��������·����Range ������ɿ���
        if (to_skip > k_max_drain_bytes) {
            osal_printk("ex-seek: drain %llu exceeds limit %llu, reconnecting\n",
                        static_cast<unsigned long long>(to_skip), static_cast<unsigned long long>(k_max_drain_bytes));
            goto do_reconnect;
        }
        // �ſ� to_skip �ֽڣ�С���ȡ������
        uint8_t drain_buf[256];
        const uint64_t drain_start = stream_now_ms();
        static constexpr uint32_t k_drain_timeout_ms = 5000;
        while (to_skip > 0) {
            size_t chunk = (to_skip > sizeof(drain_buf)) ? sizeof(drain_buf) : static_cast<size_t>(to_skip);
            size_t got = stream_ex_read_cb(drain_buf, chunk, user_data);
            if (got == 0) {
                // ���ӶϿ������˵�����·��
                osal_printk("ex-seek: drain lost connection at %llu, reconnecting\n",
                            static_cast<unsigned long long>(ctx->stream_pos));
                goto do_reconnect;
            }
            to_skip -= got;
            // stream_ex_read_cb �Ѹ��� ctx->stream_pos
            if (stream_elapsed_ms(drain_start) > k_drain_timeout_ms) {
                osal_printk("ex-seek: drain timeout, reconnecting\n");
                goto do_reconnect;
            }
        }
        return 0; // �ſ����
    }

do_reconnect:
    // ���� ����·�����Ͽ������ӣ������� HTTP ���� ����
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

    // ���� Content-Length �� Content-Range������ seek ����ͽ���׷�٣�
    {
        // Content-Range ���ȣ�Range ����� 206 ��ӦЯ�������ļ���С(bytes X-Y/Z)
        uint64_t cr_total = parse_content_range_total(header.data());
        if (cr_total > 0) {
            ctx->content_length = cr_total;
            osal_printk("ex-seek: Content-Range total=%llu\n", static_cast<unsigned long long>(cr_total));
        } else if (position == 0) {
            // ���ڷ� Range ����(��ͷ GET)ʱʹ�� Content-Length��
            // Range ����� Content-Length ֻ�� range ���С��������Ϊ�ļ��ܳ���
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
    ctx->stream_pos = position; // HTTP Range ��Ӧ��� position ��ʼ
    return 0;
}

} // namespace

// ��ʼ����̬��Ա����
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
            // ����Play����������URL�����½����𲥷ſ��ء�
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
        // ��URLʱ��������λ��׷��״̬
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
        // ͬURL����ͣ/ֹͣ�ָ�ʱ��ȷ��������������
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
    // ����״̬
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
    // ���URL����
    memset(current_url.data(), 0, current_url.size());
    // ����״̬
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
    // ��¼λ�ã����� Range �ָ�������
    // ��ͣʱ��ѭ����ر� TCP �����ͷ� LWIP �ڴ棬
    // ��������������������ݻ�ľ� LWIP ������������ NOTIFY ʧ�ܡ�
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
    // ��ͣʱ TCP �����ѹرա�s_has_range �����ã���ѭ��ͨ�� REOPEN
    // �� Range ����������IO ������Ϊ��̬Ԥ���䣬����ʧ�ܡ�
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
        // ����2: ��֡/ƽ�������ʹ��� (CBR׼ȷ, VBR����)
        byte_offset = (uint64_t)seconds * (s_avg_bitrate_bps / 8U);
    } else {
        // ����: ���� 128kbps CBR������ s_content_length/s_avg_bitrate ��δ����ʱ������ͷ
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
    // ���� IO context shared between the read / seek callbacks ��������������������
    static stream_ex_io_ctx io_ctx = {};

    mp3dec_io_t io = {};
    io.read = stream_ex_read_cb;
    io.read_data = &io_ctx;
    io.seek = stream_ex_seek_cb;
    io.seek_data = &io_ctx;

    mp3dec_ex_t dec;
    memset(&dec, 0, sizeof(dec));
    bool dec_open = false;

    // ���Լ���������ֹ��ʧ��ʱ�������Ժľ� CPU ����־
    int open_retry_count = 0;
    static constexpr int k_max_open_retries = 5;
    static constexpr uint32_t k_open_retry_base_ms = 200;

    // ���� PCM buffer for IIS output ����������������������������������������������������������������
    static constexpr size_t k_pcm_batch_samples = 1152 * 2;
    int16_t *pcm_buf = static_cast<int16_t *>(osal_kmalloc(k_pcm_batch_samples * sizeof(int16_t), OSAL_GFP_KERNEL));
    if (pcm_buf == nullptr) {
        osal_printk("minimp3_ex: pcm buffer alloc failed\n");
        return;
    }

    while (true) {
        if (s_exit_requested) {
            osal_printk("[minimp3] exit requested\r\n");
            break;
        }
        // ���� Wait while paused / stopped ����������������������������������������������������
        if (!is_playing) {
            if (dec_open && is_paused) {
                // ��ͣ�������� IIS ���������� data_write ͬ�̣߳��޾�������
                // �ٿ���λ�ú�رս������� TCP ���ӣ��ͷ� LWIP �ڴ档
                // ��������������������ݻ�ľ� LWIP ��������
                // ���� DLNA NOTIFY �� lwip_send ���䲻���ڴ��ʧ�ܡ�
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
                // Stopped: ���� IIS���ͷŽ������ʹ�����Դ�Ի��տ����ڴ档
                iis::data_clear();
                mp3dec_ex_close(&dec);
                dec_open = false;
                free_stream_transport(io_ctx.transport);
                io_ctx.connected = false;
                io_ctx.stream_pos = 0;
            }
            // ֹͣ״̬ʱ�������Լ��������´β��ſ����³���
            open_retry_count = 0;
            osal_msleep(100);
            continue;
        }

        // ���� Open / reopen decoder when URL changes ������������������������������
        if (is_url_ready || !dec_open) {
            if (dec_open) {
                mp3dec_ex_close(&dec);
                dec_open = false;
            }
            free_stream_transport(io_ctx.transport);
            io_ctx.connected = false;
            io_ctx.stream_pos = 0;
            // �� LWIP / mbedTLS ��ʱ����� TCP PCB ���ڲ���������
            // �����´� malloc(18KB) ����Ƭ��ʧ�ܡ�
            if (open_retry_count == 0) {
                osal_msleep(300);
            }

            // �ڴ�ѹ����飺���������ʧ�ܣ��ȴ�����ʱ����ϵͳ������Դ
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

            // ���� io_ctx �п����ӵ�״̬����ֹ��һ�׸�Ĳ���ֵ��Ⱦ�����ӡ�
            io_ctx.content_length = 0;
            io_ctx.mp3_start_offset = 0;
            io_ctx.seek_on_open = 0;
            io_ctx.seek_on_open_active = false;
            io_ctx.seek_base = 0;

            // �� pending seek���� seek_to_seconds������ resume����
            // ���� seek_on_open���� stream_ex_seek_cb �� seek(0) ʱʹ�� Range ����
            if (s_has_range && s_resume_target_byte > 0) {
                io_ctx.seek_on_open = s_range_start_byte;
                io_ctx.seek_on_open_active = true;
                osal_printk("minimp3_ex: seek_on_open Range bytes=%llu-\n",
                            static_cast<unsigned long long>(s_range_start_byte));
            }

            // Fast open �C byte-level seeking only, NO sample index (avoids full-file scan)
            int ret = mp3dec_ex_open_cb(&dec, &io, MP3D_DO_NOT_SCAN);
            io_ctx.seek_on_open_active = false; // ���ѱ�־
            if (ret != 0) {
                osal_printk("minimp3_ex: open_cb failed ret=%d (attempt %d)\n", ret, open_retry_count + 1);
                // mp3dec_ex_open_cb �ڷ��� IO ���������Կ������������ʧ�ܣ�
                // ������� mp3dec_ex_close �ͷ��ѷ���Ļ�����/���þ�̬��������־��
                mp3dec_ex_close(&dec);
                free_stream_transport(io_ctx.transport);
                io_ctx.connected = false;
                io_ctx.stream_pos = 0;
                open_retry_count++;
                uint32_t delay = k_open_retry_base_ms * (1U << (open_retry_count > 4 ? 4 : open_retry_count));
                if (ret == MP3D_E_MEMORY) {
                    // �ڴ治�㣬�ȸ����� LWIP / TLS �ͷ���Դ
                    delay += 800;
                    osal_printk("minimp3_ex: memory exhausted, waiting %u ms\n", delay);
                }
                osal_msleep(delay);
                continue;
            }
            // �򿪳ɹ����������Լ���
            open_retry_count = 0;
            dec_open = true;
            is_url_ready = false;
            io_ctx.mp3_start_offset = dec.start_offset; // ��¼�ļ��� MP3 ������ʼƫ��

            // �� open_cb �ѽ�������֡ͷ��ʼ�������ʣ�������֡���룩��ȷ�� seek ʱ byte_offset ����
            if (dec.info.bitrate_kbps > 0 && s_avg_bitrate_bps == 0) {
                s_avg_bitrate_bps = static_cast<uint32_t>(dec.info.bitrate_kbps) * 1000U;
            }

            // Sync Content-Length from HTTP response header (parsed in stream_ex_seek_cb).
            // Content-Range �������ܴ�С���ȣ��� Range ����� Content-Length ��ɽ��ܡ�
            if (io_ctx.content_length > 0) {
                s_content_length = io_ctx.content_length;
                osal_printk("minimp3_ex: Content-Length=%llu\n", static_cast<unsigned long long>(s_content_length));
            }

            // �� pending seek���� seek_to_seconds ��������
            // Range �����Ѷ�λ��Ԥ��λ�ã��� Range ��ʼ�ֽڿ�ʼ׷��λ�ã�
            // ������֡�ۼ� frame_bytes ��Ȼ�ƽ�Ŀ��λ�á�
            // ע�⣺���� s_resume_target_byte �Ƿ�Ϊ 0����������� s_has_range��
            // ������һ��ѭ�����ظ����� mid-playback seek �� ��ѭ�����١�
            if (s_has_range) {
                osal_printk("minimp3_ex: seeked, Range start=%llu target=%llu\n",
                            static_cast<unsigned long long>(s_range_start_byte),
                            static_cast<unsigned long long>(s_resume_target_byte));
                s_bytes_streamed = s_range_start_byte;
                s_has_range = false;
                s_resume_target_byte = 0;
                s_range_start_byte = 0;
            } else {
                s_bytes_streamed = 0; // ������ 0 ��ʼ����
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

        // ���� Pending seek during playback (e.g. DLNA seek / resume) ����
        if (s_has_range && dec_open) {
            osal_printk("minimp3_ex: mid-playback seek to byte %llu\n",
                        static_cast<unsigned long long>(s_resume_target_byte));
            // ������ IIS���������Ƶ��������������λ�ô��ң��� data_write ͬ�߳��޾�����
            iis::data_clear();
            // ���� seek_on_open �� reopen ʱʹ�� Range ���󣬱����� GET �� drain��
            io_ctx.seek_on_open = s_range_start_byte;
            io_ctx.seek_on_open_active = true;
            mp3dec_ex_close(&dec);
            dec_open = false;
            free_stream_transport(io_ctx.transport);
            io_ctx.connected = false;
            io_ctx.stream_pos = 0;
            is_url_ready = true; // ���� reopen ·��
            continue;
        }

        // ���� Read one frame of decoded PCM ��������������������������������������������������
        mp3dec_frame_info_t frame_info;
        memset(&frame_info, 0, sizeof(frame_info));
        mp3d_sample_t *frame_samples = nullptr;
        size_t n = mp3dec_ex_read_frame(&dec, &frame_samples, &frame_info, k_pcm_batch_samples);

        // ���� Update position / bitrate trackers ����������������������������������������
        // ������ n==0 ���֮ǰ����֡��to_skip��ʱ n=0 �� frame_bytes>0��
        // �������λ��׷�ٺͱ����ʡ�
        if (frame_info.bitrate_kbps > 0) {
            s_avg_bitrate_bps = static_cast<uint32_t>(frame_info.bitrate_kbps) * 1000U;
        }
        if (frame_info.frame_bytes > 0) {
            s_bytes_streamed += static_cast<uint64_t>(frame_info.frame_bytes);
        }

        if (n == 0) {
            // ��֡��������������� bit reservoir ������ encoder delay����
            // frame_bytes > 0 ��ʾ�������������ݵ�δ���� PCM ������
            // ����ѭ������ֹͣ���š�
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
            // ��������ֹͣ���ţ������Զ���������������ѭ�������ڴ棩
            // DLNA ���ƶ˻�ͨ�� SetAVTransportURI+Play ������һ�׸�
            s_range_start_byte = s_bytes_streamed;
            s_has_range = false;
            s_resume_target_byte = 0;
            is_playing = false;
            is_url_ready = false;
            open_retry_count = 0;
            osal_msleep(500);
            continue;
        }

        // ���� Rate-change detection ������������������������������������������������������������������
        if (frame_info.hz > 0 && frame_info.hz != dec.info.hz && iis_set_rate_func) {
            const int old_hz = dec.info.hz;
            iis_set_rate_func(frame_info.hz);
            osal_printk("minimp3_ex: rate switch %d -> %d Hz\n", old_hz, frame_info.hz);
        }

        // ���� Push PCM to IIS ������������������������������������������������������������������������������
        if (mp3_get_into_iis_func) {
            if (frame_info.channels == 1) {
                // Mono �� stereo expansion
                for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
                    pcm_buf[2 * i] = frame_samples[i];
                    pcm_buf[2 * i + 1] = frame_samples[i];
                }
                mp3_get_into_iis_func(pcm_buf, static_cast<uint32_t>(n * 2));
            } else {
                mp3_get_into_iis_func(frame_samples, static_cast<uint32_t>(n));
            }
        }

        // ���� Duration from VBR tag ������������������������������������������������������������������
        if (dec.detected_samples > 0 && dec.info.hz > 0 && s_duration_seconds == 0) {
            s_duration_seconds = static_cast<uint32_t>(dec.detected_samples / dec.info.hz);
        }
        // Content-Length is synced from io_ctx.content_length during REOPEN.
        // dec.end_offset is only the scan window boundary, NOT the file size.

        // ���� Back-pressure from IIS queue ����������������������������������������������������
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
    osal_kfree(pcm_buf);
}