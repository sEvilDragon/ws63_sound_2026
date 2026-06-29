#include "spi_task.h"
#include "dws_slave.hpp"

extern "C" {
#include "nv.h"
#include "soc_osal.h"
#include "systick.h"
}

/* 控制端 SPI 配置在 NV 中使用的 Key ID (KEY_ID_REGION5: 0x5000~0x5FFF) */
#define NV_KEY_SPI_SETTINGS 0x5001 /* 已在 app.json 注册 */
#define NV_FLUSH_DELAY_MS 5000     /* 5 秒无操作后才写入 Flash */

static spi_settings_t g_settings = {
    SPI_CMD_QUERY, (uint8_t)((SPI_HOTSPOT_OFF << 4) | SPI_NETWORK_CONN), SPI_MODE_WIREED, 25, 50, 0};

const spi_settings_t *get_spi_settings()
{
    return &g_settings;
}

static audio_result_t g_audio = {};

const audio_result_t *get_audio_result()
{
    return &g_audio;
}

/**
 * @brief 上电时从 NV 恢复上次保存的 SPI 配置。
 *        若 NV 中无数据（首次开机），保持硬编码默认值。
 */
void nv_load_settings(void)
{
    uint16_t len = 0;
    spi_settings_t saved;
    errcode_t ret = uapi_nv_read(NV_KEY_SPI_SETTINGS, sizeof(saved), &len, (uint8_t *)&saved);
    if (ret == ERRCODE_SUCC && len == sizeof(saved)) {
        g_settings = saved;
        g_settings.cmd = SPI_CMD_QUERY; // cmd 永远从 QUERY 开始
        osal_printk("[DWS_S] loaded settings from NV: mode=%u vol=%u bri=%u bass=%u\r\n", (unsigned)g_settings.mode,
                    (unsigned)g_settings.volume, (unsigned)g_settings.brightness, (unsigned)g_settings.bass);
    } else {
        osal_printk("[DWS_S] no NV data: ret=%d len=%u (expected %u), using defaults\r\n", (int)ret, (unsigned)len,
                    (unsigned)sizeof(saved));
    }
}

static bool g_nv_dirty = false;
static uint64_t g_nv_last_change_tick = 0;

/**
 * @brief 将当前 SPI 配置写入 NV 持久化。
 *        由 ui_task 在每次值变更后调用。
 */
void nv_mark_dirty(void)
{
    g_nv_dirty = true;
    g_nv_last_change_tick = uapi_systick_get_ms();
}

/**
 * @brief 若脏标志已置位且距上次变更超过 2 秒，执行 NV 写入。
 *        由 ui_task 主循环周期性调用。
 */
void nv_flush_if_idle(void)
{
    if (!g_nv_dirty) {
        return;
    }

    uint64_t now = uapi_systick_get_ms();
    if (now - g_nv_last_change_tick < NV_FLUSH_DELAY_MS) {
        return; // 还在频繁操作，不写
    }

    errcode_t ret = uapi_nv_write(NV_KEY_SPI_SETTINGS, (const uint8_t *)&g_settings, sizeof(g_settings));
    if (ret != ERRCODE_SUCC) {
        osal_printk("[DWS_S] NV write failed: %d\r\n", (int)ret);
    } else {
        osal_printk("[DWS_S] NV write OK\r\n");
    }
    g_nv_dirty = false;
    osal_printk("[DWS_S] settings flushed to NV\r\n");
}

/**
 * @brief 立即写入 NV，不等防抖。供关机/测试使用。
 */
void nv_flush_now(void)
{
    g_nv_dirty = true;
    g_nv_last_change_tick = 0; // 让防抖检查立即通过
    nv_flush_if_idle();
}

void *spi_slave_task(void *arg)
{
    (void)arg;
    osal_printk("[DWS_S] >>> task started, constructing dws_slave...\r\n");
    static sed_ws63::dws_slave spi;
    osal_printk("[DWS_S] >>> dws_slave object constructed, entering main loop\r\n");

    uint8_t rx_buf[sed_ws63::dws_slave::TRANSFER_LEN];
    int diag_cnt = 0;
    int succ_cnt = 0;
    int fail_cnt = 0;
    int loop_cnt = 0;

    while (true) {
        loop_cnt++;
        spi_settings_t response = g_settings;
        response.cmd = SPI_CMD_QUERY;

        // 前 10 次循环每次都打印, 帮助确认初始状态
        if (loop_cnt <= 10) {
            osal_printk("[DWS_S] loop=%d (first 10) entering transfer...\r\n", loop_cnt);
        } else if (loop_cnt % 100 == 1) {
            osal_printk("[DWS_S] loop=%d succ=%d fail=%d entering transfer...\r\n", loop_cnt, succ_cnt, fail_cnt);
        }

        int ret = spi.transfer(rx_buf, sed_ws63::dws_slave::TRANSFER_LEN, (const uint8_t *)&response, SPI_SETTINGS_LEN);
        if (ret != 0) {
            fail_cnt++;
            // 失败时立即打印(不抑制), 显示累计失败次数
            osal_printk("[DWS_S] *** transfer FAIL #%d at loop=%d (total fail=%d, succ=%d) ***\r\n", fail_cnt, loop_cnt,
                        fail_cnt, succ_cnt);
            osal_msleep(10);
            continue;
        }

        // 首次成功时特别标记
        if (succ_cnt == 0) {
            osal_printk("[DWS_S] >>> FIRST successful transfer at loop=%d <<<\r\n", loop_cnt);
        }
        succ_cnt++;

        audio_result_t tmp;
        tmp.bands[0] = rx_buf[SPI_AUDIO_OFFSET + 0];
        tmp.bands[1] = rx_buf[SPI_AUDIO_OFFSET + 1];
        tmp.bands[2] = rx_buf[SPI_AUDIO_OFFSET + 2];
        tmp.bands[3] = rx_buf[SPI_AUDIO_OFFSET + 3];
        tmp.bands[4] = rx_buf[SPI_AUDIO_OFFSET + 4];
        tmp.overall = rx_buf[SPI_AUDIO_OFFSET + 5];
        tmp.beat = rx_buf[SPI_AUDIO_OFFSET + 6];

        unsigned long flags = osal_irq_lock();
        g_audio = tmp;
        osal_irq_restore(flags);

        if (++diag_cnt % 20 == 1) {
            osal_printk("[DWS_S] recv OK #%d: [%u %u %u %u %u] ov=%u bt=%u (succ=%d fail=%d)\r\n", diag_cnt,
                        (unsigned)tmp.bands[0], (unsigned)tmp.bands[1], (unsigned)tmp.bands[2], (unsigned)tmp.bands[3],
                        (unsigned)tmp.bands[4], (unsigned)tmp.overall, (unsigned)tmp.beat, succ_cnt, fail_cnt);
        }

        osal_msleep(5);
    }
    return nullptr;
}
