#include "ht306_control.h"

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "ws2812.pio.h"
#endif

// Inisialisasi sentinel (0xFF) agar pemanggilan pertama saat boot selalu menyalakan LED fisik
static HT306_StatusLed_t g_current_led = (HT306_StatusLed_t)0xFF;

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
static uint     g_ws2812_sm = 0U;
static bool     g_ws2812_ready = false;
static bool     g_ws2812_has_sent = false;
static uint64_t g_ws2812_last_tx_us = 0ULL;

/**
 * Pengirim sinyal 800 kHz protokol WS2812 (GRB format) via PIO1 pada GP16 RP2350-Zero.
 * 100% bebas pemblokiran interupsi & menjaga jeda reset-latch >= 300 us (30 us TX + >= 320 us LOW).
 * Dipanggil eksklusif dari Core 0.
 */
static bool ws2812_write_grb(uint8_t g, uint8_t r, uint8_t b) {
    if (!g_ws2812_ready) {
        return false;
    }

    uint64_t now_us = time_us_64();
    if (g_ws2812_has_sent && (now_us - g_ws2812_last_tx_us) < 350ULL) {
        return false;
    }
    if (pio_sm_is_tx_fifo_full(pio1, g_ws2812_sm)) {
        return false;
    }

    uint32_t grb = ((uint32_t)g << 16U) | ((uint32_t)r << 8U) | (uint32_t)b;
    pio_sm_put(pio1, g_ws2812_sm, grb << 8U);
    g_ws2812_last_tx_us = now_us;
    g_ws2812_has_sent = true;
    return true;
}
#endif

void ht306_control_init(void) {
#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
    gpio_init(HT306_PIN_MUX_SELECT);
    gpio_set_dir(HT306_PIN_MUX_SELECT, GPIO_OUT);
    gpio_put(HT306_PIN_MUX_SELECT, 1);

    // ponytail: open-drain emulation (idle = input high-Z so stock IR remote still works)
    gpio_init(HT306_PIN_REMOCON_TX);
    gpio_put(HT306_PIN_REMOCON_TX, 0);
    gpio_set_dir(HT306_PIN_REMOCON_TX, GPIO_IN);

    if (!g_ws2812_ready && pio_can_add_program(pio1, &ws2812_program)) {
        uint ws2812_offset = pio_add_program(pio1, &ws2812_program);
        g_ws2812_sm = (uint)pio_claim_unused_sm(pio1, true);
        ws2812_program_init(pio1, g_ws2812_sm, ws2812_offset, HT306_PIN_WS2812_LED, 800000.0f, false);
        g_ws2812_ready = true;
    }
#endif
    ht306_control_set_status_led(HT306_STATUS_STANDBY_BLUE);
}

void ht306_control_set_pc_mode(bool pc_mode_active) {
#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
    gpio_put(HT306_PIN_MUX_SELECT, pc_mode_active ? 1 : 0);
#else
    (void)pc_mode_active;
#endif
}

void ht306_control_set_status_led(HT306_StatusLed_t status) {
    if (g_current_led == status) {
        return;
    }
#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
    bool sent = false;
    switch (status) {
        case HT306_STATUS_STANDBY_BLUE:
            sent = ws2812_write_grb(0U, 0U, 32U);   // Biru
            break;
        case HT306_STATUS_MODE_48K_GREEN:
            sent = ws2812_write_grb(32U, 0U, 0U);   // Hijau
            break;
        case HT306_STATUS_MODE_96K_PURPLE:
            sent = ws2812_write_grb(0U, 32U, 32U);  // Ungu / Magenta
            break;
        case HT306_STATUS_ERROR_RED:
            sent = ws2812_write_grb(0U, 32U, 0U);   // Merah
            break;
        default:
            return;
    }
    if (sent) {
        g_current_led = status;
    }
#else
    g_current_led = status;
#endif
}

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
// ponytail: single pulse helper replaces separate pull_low / release_high functions
static inline void remocon_pulse(uint32_t low_us, uint32_t high_us) {
    gpio_set_dir(HT306_PIN_REMOCON_TX, GPIO_OUT);
    sleep_us(low_us);
    gpio_set_dir(HT306_PIN_REMOCON_TX, GPIO_IN);
    sleep_us(high_us);
}
#endif

void ht306_control_send_remote_cmd(HT306_RemoteCmd_t cmd_code) {
#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
    uint8_t addr = (uint8_t)(cmd_code >> 16U);
    uint8_t cmd  = (uint8_t)(cmd_code >> 8U);
    uint32_t frame = (uint32_t)addr | ((uint32_t)(uint8_t)~addr << 8U)
                   | ((uint32_t)cmd << 16U) | ((uint32_t)(uint8_t)~cmd << 24U);

    remocon_pulse(9000U, 4500U);
    for (uint8_t b = 0U; b < 32U; ++b) {
        remocon_pulse(560U, ((frame >> b) & 1U) ? 1690U : 560U);
    }
    remocon_pulse(560U, 1000U);
#else
    (void)cmd_code;
#endif
}
