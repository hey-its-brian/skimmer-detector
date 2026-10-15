// Board definition: ESP32-2432S028R "Cheap Yellow Display", 2-USB revision.
#pragma once
#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---- pins (fixed by the board) ----
#define PIN_TFT_SCLK   14
#define PIN_TFT_MOSI   13
#define PIN_TFT_MISO   12
#define PIN_TFT_CS     15
#define PIN_TFT_DC      2
#define PIN_TFT_BL     21

#define PIN_TP_CLK     25
#define PIN_TP_MOSI    32
#define PIN_TP_MISO    39
#define PIN_TP_CS      33
#define PIN_TP_IRQ     36   // low while the panel is pressed; deep-sleep wake source

#define PIN_SD_CS       5
#define PIN_SD_SCK     18
#define PIN_SD_MISO    19
#define PIN_SD_MOSI    23

#define PIN_LED_R       4   // active LOW
#define PIN_LED_G      16
#define PIN_LED_B      17
#define PIN_SPK        26   // on-board amp, drive with LEDC tone
#define PIN_LDR        34   // light sensor, ADC

#define SCREEN_W      320
#define SCREEN_H      240

#ifndef PANEL_ILI9342
#define PANEL_ILI9342 0
#endif

class LGFX : public lgfx::LGFX_Device {
#if PANEL_ILI9342
    lgfx::Panel_ILI9342 _panel;
#else
    lgfx::Panel_ST7789  _panel;
#endif
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = HSPI_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = PIN_TFT_SCLK;
            cfg.pin_mosi    = PIN_TFT_MOSI;
            cfg.pin_miso    = PIN_TFT_MISO;
            cfg.pin_dc      = PIN_TFT_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs           = PIN_TFT_CS;
            cfg.pin_rst          = -1;
            cfg.pin_busy         = -1;
            cfg.panel_width      = 240;
            cfg.panel_height     = 320;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = false;   // runtime-adjustable from Setup
            cfg.rgb_order        = false;   // runtime-adjustable from Setup
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = PIN_TFT_BL;
            cfg.invert      = false;
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};
