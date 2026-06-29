#pragma once

extern "C" {
#include "spi.h"
#include "spi_porting.h"
#include "dma.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "app_init.h"
}

namespace sed_ws63 {

class sk9822_led {
public:
    sk9822_led();
    ~sk9822_led() = default;

    void set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 15);
    void update();
    void clear();

    static constexpr uint8_t NUM_LEDS = 18;

private:
    void pin_init();
    void spi_init();
    void spi_dma_init();
    void spi_send(const uint8_t *data, uint32_t len);

    static constexpr spi_bus_t BUS = SPI_BUS_1;
    static constexpr uint32_t FREQ_MHZ = 2;
    static constexpr uint32_t TIMEOUT = 0xFFFFFFFF;

    // 4(start) + 18*4(led) + 4(end) = 80 bytes
    static constexpr uint32_t FRAME_SIZE = 4 + NUM_LEDS * 4 + 4;

    static uint8_t frame_buf[FRAME_SIZE];
};

} // namespace sed_ws63
