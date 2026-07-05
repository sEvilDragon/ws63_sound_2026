#include "dws_master.hpp"

namespace sed_ws63 {

dws_master::dws_master()
{
    pin_init();
}

void dws_master::pin_init()
{
    /* CLK: 推挽输出, 初始 HIGH (空闲态) */
    uapi_pin_set_mode(CLK_PIN, PIN_MODE_0);
    uapi_gpio_set_dir(CLK_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(CLK_PIN, GPIO_LEVEL_HIGH);

    /* DAT: 初始为输入 (释放态), 外部上拉拉高 */
    uapi_pin_set_mode(DAT_PIN, PIN_MODE_0);
    uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_INPUT);
}

void dws_master::send_bit(uint8_t bit_val)
{
    if (bit_val) {
        /* 释放总线 (外部上拉拉高) */
        uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_INPUT);
    } else {
        /* 拉低 */
        uapi_gpio_set_val(DAT_PIN, GPIO_LEVEL_LOW);
        uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_OUTPUT);
    }

    osal_udelay(HALF_BIT_US);

    /* CLK 上升沿: Slave 在此采样 */
    uapi_gpio_set_val(CLK_PIN, GPIO_LEVEL_HIGH);
    osal_udelay(HALF_BIT_US);

    /* CLK 下降沿 */
    uapi_gpio_set_val(CLK_PIN, GPIO_LEVEL_LOW);
}

uint8_t dws_master::recv_bit()
{
    /* CLK 上升沿 */
    uapi_gpio_set_val(CLK_PIN, GPIO_LEVEL_HIGH);
    osal_udelay(HALF_BIT_US);

    /* 在 CLK 高电平期间采样 DAT */
    uint8_t bit = uapi_gpio_get_val(DAT_PIN) & 0x01;

    /* CLK 下降沿 */
    uapi_gpio_set_val(CLK_PIN, GPIO_LEVEL_LOW);
    osal_udelay(HALF_BIT_US);

    return bit;
}

int dws_master::transfer(const uint8_t *tx_data, uint32_t tx_len, uint8_t *rx_data, uint32_t rx_len)
{
    if (tx_len > TRANSFER_LEN || rx_len > TRANSFER_LEN) {
        osal_printk("[DWS_M] transfer: invalid len tx=%u rx=%u\r\n", (unsigned)tx_len, (unsigned)rx_len);
        return -1;
    }

    /* 填充发送缓冲 (不足补零) */
    for (uint32_t i = 0; i < TRANSFER_LEN; i++) {
        tx_buffer[i] = (i < tx_len) ? tx_data[i] : 0;
    }
    /* 清零接收缓冲 */
    for (uint32_t i = 0; i < TRANSFER_LEN; i++) {
        rx_buffer[i] = 0;
    }

    /* ===== START: CLK LOW 100μs ===== */
    uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_INPUT); /* 先释放 DAT */
    uapi_gpio_set_val(CLK_PIN, GPIO_LEVEL_LOW);
    osal_udelay(START_US);

    /* ===== Phase 1: Master → Slave, 发送 16 字节 (128 bits) MSB-first ===== */
    uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_OUTPUT);
    for (uint32_t byte_idx = 0; byte_idx < TRANSFER_LEN; byte_idx++) {
        uint8_t byte = tx_buffer[byte_idx];
        for (int bit = 7; bit >= 0; bit--) {
            send_bit((byte >> bit) & 1);
        }
    }

    /* ===== Turnaround: 释放 DAT, 等 Slave 准备好发送 ===== */
    uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_INPUT);
    uapi_gpio_set_val(CLK_PIN, GPIO_LEVEL_LOW);
    osal_udelay(TURNAROUND_US);

    /* ===== Phase 2: Slave → Master, 接收 6 字节 (48 bits) MSB-first ===== */
    for (uint32_t byte_idx = 0; byte_idx < REPLY_LEN; byte_idx++) {
        uint8_t byte = 0;
        for (int bit = 7; bit >= 0; bit--) {
            if (recv_bit()) {
                byte |= (uint8_t)(1 << bit);
            }
        }
        rx_buffer[byte_idx] = byte;
    }

    /* ===== STOP: CLK HIGH, DAT 保持输入 ===== */
    uapi_gpio_set_val(CLK_PIN, GPIO_LEVEL_HIGH);

    /* 拷贝接收数据到用户缓冲 */
    uint32_t copy_len = (rx_len < TRANSFER_LEN) ? rx_len : TRANSFER_LEN;
    for (uint32_t i = 0; i < copy_len; i++) {
        rx_data[i] = rx_buffer[i];
    }

    return 0;
}

} // namespace sed_ws63
