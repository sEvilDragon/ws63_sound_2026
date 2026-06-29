#include "dws_slave.hpp"

namespace sed_ws63 {

dws_slave::dws_slave()
{
    osal_printk("[DWS_S] ctor: pin_init...\r\n");
    pin_init();
    osal_printk("[DWS_S] DWS Slave 初始化完成 (CLK=GPIO_%02u DAT=GPIO_%02u)\r\n", (unsigned)CLK_PIN, (unsigned)DAT_PIN);
}

void dws_slave::pin_init()
{
    /* CLK: 输入, 由 Master 驱动 */
    uapi_pin_set_mode(CLK_PIN, PIN_MODE_0);
    uapi_gpio_set_dir(CLK_PIN, GPIO_DIRECTION_INPUT);

    /* DAT: 初始为输入 (释放态), 外部上拉 */
    uapi_pin_set_mode(DAT_PIN, PIN_MODE_0);
    uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_INPUT);
}

uint8_t dws_slave::recv_bit()
{
    /* 等待 CLK 上升沿 */
    while (uapi_gpio_get_val(CLK_PIN) == GPIO_LEVEL_LOW) {
    }
    /* 在 CLK 高电平期间采样 DAT */
    uint8_t bit = uapi_gpio_get_val(DAT_PIN) & 0x01;
    /* 等待 CLK 下降沿 */
    while (uapi_gpio_get_val(CLK_PIN) == GPIO_LEVEL_HIGH) {
    }
    return bit;
}

void dws_slave::send_bit(uint8_t bit_val)
{
    /* 在 CLK 上升沿之前设置 DAT, 确保 Master 采样到正确的值 */
    if (bit_val) {
        uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_INPUT);
    } else {
        uapi_gpio_set_val(DAT_PIN, GPIO_LEVEL_LOW);
        uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_OUTPUT);
    }

    /* 等待 Master 产生 CLK 上升沿 (采样) */
    while (uapi_gpio_get_val(CLK_PIN) == GPIO_LEVEL_LOW) {
    }
    /* 等待 CLK 下降沿 (准备下一位) */
    while (uapi_gpio_get_val(CLK_PIN) == GPIO_LEVEL_HIGH) {
    }
}

int dws_slave::transfer(uint8_t *rx_data, uint32_t rx_len, const uint8_t *tx_data, uint32_t tx_len)
{
    if (rx_len > TRANSFER_LEN || tx_len > TRANSFER_LEN) {
        osal_printk("[DWS_S] transfer: invalid len rx=%u tx=%u\r\n", (unsigned)rx_len, (unsigned)tx_len);
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

    /* ===== 等待 START: CLK 空闲 HIGH → LOW ===== */
    /* 先确保 CLK 回到空闲 HIGH (上一帧结束时 Master 会拉高) */
    while (uapi_gpio_get_val(CLK_PIN) == GPIO_LEVEL_LOW) {
    }
    /* 等待 Master 拉低 CLK (START 条件) */
    while (uapi_gpio_get_val(CLK_PIN) == GPIO_LEVEL_HIGH) {
    }
    /* CLK 已变 LOW, START 已触发 */

    /* ===== Phase 1: Master → Slave, 接收 16 字节 (128 bits) MSB-first ===== */
    uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_INPUT);
    for (uint32_t byte_idx = 0; byte_idx < TRANSFER_LEN; byte_idx++) {
        uint8_t byte = 0;
        for (int bit = 7; bit >= 0; bit--) {
            if (recv_bit()) {
                byte |= (uint8_t)(1 << bit);
            }
        }
        rx_buffer[byte_idx] = byte;
    }

    /* ===== Phase 2: Slave → Master, 发送 6 字节 (48 bits) MSB-first ===== */
    for (uint32_t byte_idx = 0; byte_idx < 6; byte_idx++) {
        uint8_t byte = tx_buffer[byte_idx];
        for (int bit = 7; bit >= 0; bit--) {
            send_bit((byte >> bit) & 1);
        }
    }

    /* 释放 DAT */
    uapi_gpio_set_dir(DAT_PIN, GPIO_DIRECTION_INPUT);

    /* 拷贝接收数据到用户缓冲 */
    uint32_t copy_len = (rx_len < TRANSFER_LEN) ? rx_len : TRANSFER_LEN;
    for (uint32_t i = 0; i < copy_len; i++) {
        rx_data[i] = rx_buffer[i];
    }

    return 0;
}

} // namespace sed_ws63
