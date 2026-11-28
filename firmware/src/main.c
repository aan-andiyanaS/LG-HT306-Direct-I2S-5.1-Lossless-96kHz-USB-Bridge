#include "lossless_codec.h"
#include "i2s_engine.h"
#include "ht306_control.h"
#include "ht306_usb.h"
#include <string.h>

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "hardware/sync.h"
#include "tusb.h"
#define HT306_MAIN_RAM_FUNC(fn) __not_in_flash_func(fn)
#define HT306_MAIN_DMB() __dmb()
#else
#define HT306_MAIN_RAM_FUNC(fn) fn
#define HT306_MAIN_DMB() ((void)0)
#endif

/**
 * Antrian Paket 96 kHz Lossless Wait-Free SPSC antara Core 0 (USB) dan Core 1 (Unpacker).
 */
#define HT306_PACKET_QUEUE_DEPTH  64U
#define HT306_PACKET_QUEUE_MASK   (HT306_PACKET_QUEUE_DEPTH - 1U)

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert((HT306_PACKET_QUEUE_DEPTH & HT306_PACKET_QUEUE_MASK) == 0U,
               "HT306_PACKET_QUEUE_DEPTH must be a power of two");
#endif

typedef struct {
    uint8_t  data[HT306_MAX_PACKET_BYTES];
    uint16_t length;
} HT306_PacketSlot_t;

static HT306_PacketSlot_t g_packet_queue[HT306_PACKET_QUEUE_DEPTH];
static volatile uint32_t  g_pkt_write_count = 0U;
static volatile uint32_t  g_pkt_read_count  = 0U;

/* Buffer statis di SRAM (mencegah Stack Overflow pada Core 1 yang hanya memiliki 2 KB stack) */
static int32_t g_core1_decoded_i2s32[HT306_MAX_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];

/* Array peak level 6-kanal wait-free/atomic antara Core 1 dan Core 0 */
static volatile uint32_t g_channel_peaks[HT306_NUM_CHANNELS] = {0};

/* Permintaan status LED dari Core 1 -> dieksekusi eksklusif oleh Core 0 via PIO1 */
static volatile HT306_StatusLed_t g_core1_led_req = HT306_STATUS_STANDBY_BLUE;

/* Status CDC Serial Monitor (diperbarui Core 0): skip scan VU Core 1 saat headless */
static volatile bool g_cdc_connected = false;

uint32_t HT306_MAIN_RAM_FUNC(ht306_usb_packet_queue_free_slots)(void) {
    uint32_t used = (uint32_t)(g_pkt_write_count - g_pkt_read_count);
    if (used >= HT306_PACKET_QUEUE_DEPTH) {
        return 0U;
    }
    return HT306_PACKET_QUEUE_DEPTH - used;
}

void HT306_MAIN_RAM_FUNC(ht306_core1_lossless_worker)(void) {
    while (true) {
        uint32_t w = g_pkt_write_count;
        uint32_t r = g_pkt_read_count;

        if ((uint32_t)(w - r) > 0U) {
            HT306_MAIN_DMB();
            uint32_t slot_idx = r & HT306_PACKET_QUEUE_MASK;
            const HT306_PacketSlot_t *slot = &g_packet_queue[slot_idx];

            /* Baca flags SEBELUM slot dilepas ke Core 0 (menutup bug use-after-release) */
            uint8_t pkt_flags = (slot->length >= sizeof(HT306_LosslessHeader_t)) ? slot->data[5] : 0U;
            uint8_t frames_decoded = 0U;

            HT306_CodecStatus_t status = ht306_lossless_decode_frame_ex(
                slot->data,
                slot->length,
                NULL,
                g_core1_decoded_i2s32,
                HT306_MAX_SUBFRAME_SAMPLES,
                &frames_decoded
            );

            HT306_MAIN_DMB();
            g_pkt_read_count = r + 1U;

            if (status == HT306_CODEC_OK && frames_decoded > 0U) {
                /* Perbarui peak level 6-kanal hanya saat CDC Serial terhubung (hemat ~3.6M siklus/detik saat headless) */
                if (g_cdc_connected) {
                    uint32_t local_peaks[HT306_NUM_CHANNELS] = {0};
                    const int32_t *p_sample = g_core1_decoded_i2s32;
                    for (uint32_t i = 0U; i < frames_decoded; ++i) {
                        for (uint32_t ch = 0U; ch < HT306_NUM_CHANNELS; ++ch) {
                            int32_t s = *p_sample++;
                            uint32_t mag = (s < 0) ? (uint32_t)(0U - (uint32_t)s) : (uint32_t)s;
                            if (mag > local_peaks[ch]) {
                                local_peaks[ch] = mag;
                            }
                        }
                    }
                    for (uint32_t ch = 0U; ch < HT306_NUM_CHANNELS; ++ch) {
                        uint32_t cur = __atomic_load_n(&g_channel_peaks[ch], __ATOMIC_RELAXED);
                        while (local_peaks[ch] > cur) {
                            if (__atomic_compare_exchange_n(&g_channel_peaks[ch], &cur, local_peaks[ch],
                                                            true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                                break;
                            }
                        }
                    }
                }

                bool is_96k = (pkt_flags & HT306_FLAG_RATE_96KHZ) != 0U;
                ht306_i2s_request_sample_rate(is_96k ? HT306_SAMPLE_RATE_96000 : HT306_SAMPLE_RATE_48000);

                /* Backpressure: tunggu slot Ring Buffer DMA tersedia daripada membuang blok audio */
                size_t offset = 0U;
                while ((offset + HT306_DMA_BLOCK_FRAMES) <= frames_decoded) {
#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
                    while (!ht306_i2s_ring_has_space()) {
                        __wfe();
                    }
#endif
                    (void)ht306_i2s_write_frames_6ch(
                        &g_core1_decoded_i2s32[offset * HT306_NUM_CHANNELS],
                        HT306_DMA_BLOCK_FRAMES
                    );
                    offset += HT306_DMA_BLOCK_FRAMES;
                }

                g_core1_led_req = is_96k ? HT306_STATUS_MODE_96K_PURPLE : HT306_STATUS_MODE_48K_GREEN;
            } else {
                g_core1_led_req = HT306_STATUS_ERROR_RED;
            }
        }
#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
        else {
            /* Cortex-M Wait-For-Event: Nol latency saat bangun via __sev(), menghemat daya dan bus SRAM */
            __wfe();
        }
#else
        else {
            break;
        }
#endif
    }
}

bool HT306_MAIN_RAM_FUNC(ht306_usb_enqueue_lossless_packet)(const uint8_t *usb_data, uint16_t len) {
    if (usb_data == NULL || len == 0U || len > HT306_MAX_PACKET_BYTES) {
        return false;
    }

    uint32_t w = g_pkt_write_count;
    uint32_t r = g_pkt_read_count;
    if ((uint32_t)(w - r) >= HT306_PACKET_QUEUE_DEPTH) {
        return false;
    }

    uint32_t slot_idx = w & HT306_PACKET_QUEUE_MASK;
    HT306_PacketSlot_t *slot = &g_packet_queue[slot_idx];
    memcpy(slot->data, usb_data, len);
    slot->length = len;

    HT306_MAIN_DMB();
    g_pkt_write_count = w + 1U;

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
    /* Pastikan store g_pkt_write_count selesai sebelum membangunkan Core 1 dari __wfe() */
    __dsb();
    __sev();
#endif

    return true;
}

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
static const char *ht306_sample_rate_label(uint32_t rate_hz) {
    if (rate_hz >= 96000U) {
        return "96kHz";
    }
    if (rate_hz == HT306_SAMPLE_RATE_44100) {
        return "44.1kHz";
    }
    return "48kHz";
}

static const char *ht306_vu_meter_bar(uint32_t peak) {
    /* Skala representasi visual 6 segmen non-linear (dB-scaled):
     * Segment 1 (>= -42 dBFS): 16777216U (audible ambient/background)
     * Segment 2 (>= -30 dBFS): 67108864U (soft dialogue/music)
     * Segment 3 (>= -18 dBFS): 268435456U (nominal music/dialogue)
     * Segment 4 (>= -10 dBFS): 671088640U (strong dynamic content)
     * Segment 5 (>= -4 dBFS) : 1342177280U (loud climax/bass)
     * Segment 6 (>= -1 dBFS) : 1932735283U (peak / near 0 dBFS clipping)
     */
    if (peak >= 1932735283U) return "██████";
    if (peak >= 1342177280U) return "█████░";
    if (peak >=  671088640U) return "████░░";
    if (peak >=  268435456U) return "███░░░";
    if (peak >=   67108864U) return "██░░░░";
    if (peak >=   16777216U) return "█░░░░░";
    return "░░░░░░";
}

static void ht306_enter_bootsel_safe(void) {
    ht306_usb_log("\r\n[BOOT] Muting I2S & rebooting into BOOTSEL mode...\r\n");
    ht306_i2s_mute_and_stop();
    for (int i = 0; i < 25; ++i) {
        tud_task();
        sleep_ms(1);
    }
    reset_usb_boot(0, 0);
}

int main(void) {
    /* 1. Inisialisasi pin kontrol HT306 (MUX GP8, REMOCON GP9, PIO1 WS2812 GP16) */
    ht306_control_init();
    ht306_control_set_pc_mode(true);

    /* 2. Inisialisasi mesin PIO0 4-Lane I2S & DMA Ping-Pong pada 48 kHz (auto-switch ke 96 kHz saat paket 96k tiba) */
    bool i2s_ok = ht306_i2s_engine_init(HT306_SAMPLE_RATE_48000);
    if (!i2s_ok) {
        ht306_control_set_status_led(HT306_STATUS_ERROR_RED);
    }

    /* 3. Inisialisasi TinyUSB Device Stack (CDC Serial + Vendor Bulk) */
    tusb_init();

    /* 4. Jalankan Core 1 khusus Lossless Unpacker HANYA jika engine I2S berhasil diinisialisasi */
    if (i2s_ok) {
        multicore_launch_core1(ht306_core1_lossless_worker);
    }

    /* 5. Loop utama Core 0: Melayani USB, LED single-owner, & Diagnostik CDC */
    bool banner_sent = false;
    bool is_streaming = false;
    uint32_t last_heartbeat_ms = 0U;
    uint32_t last_vu_ms = 0U;
    uint32_t last_pkt_seen = 0U;
    uint32_t last_audio_activity_ms = 0U;

    char   cdc_cmd_buf[32];
    size_t cdc_cmd_len = 0U;
    bool   cdc_cmd_overflow = false;
    bool   cdc_last_was_cr = false;

    while (true) {
        ht306_usb_task();
        if (ht306_usb_bootsel_requested()) {
            ht306_enter_bootsel_safe();
        }

        /* Pelacakan aktivitas stream & kontrol LED single-owner berjalan walaupun Serial Monitor tertutup */
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        uint32_t current_pkt = g_pkt_read_count;

        if (current_pkt != last_pkt_seen) {
            last_pkt_seen = current_pkt;
            last_audio_activity_ms = now_ms;
            is_streaming = true;
        } else if (is_streaming && (uint32_t)(now_ms - last_audio_activity_ms) >= 500U) {
            is_streaming = false;
        }

        if (!i2s_ok) {
            ht306_control_set_status_led(HT306_STATUS_ERROR_RED);
        } else if (is_streaming) {
            ht306_control_set_status_led(g_core1_led_req);
        } else {
            ht306_control_set_status_led(HT306_STATUS_STANDBY_BLUE);
        }

        bool cdc_ok = tud_cdc_connected();
        g_cdc_connected = cdc_ok;
        if (cdc_ok) {
            /* Tampilkan banner selamat datang saat Serial Monitor dibuka */
            if (!banner_sent) {
                ht306_usb_log("\r\n======================================================\r\n");
                ht306_usb_log("  LG HT306 Direct-I2S 5.1/7.1 Lossless USB Bridge\r\n");
                ht306_usb_log("  Target: Waveshare RP2350-Zero (Arm Cortex-M33 @ 150MHz)\r\n");
                ht306_usb_log("======================================================\r\n");
                ht306_usb_log("[BOOT] Sistem Siap! Core 0 & Core 1 Berjalan.\r\n");
                if (i2s_ok) {
                    ht306_usb_log("[INIT] PIO0 4-Lane I2S Engine: BERHASIL (48/96 kHz Auto-Switch Ready)\r\n");
                    ht306_usb_log("       -> GP2: LRCK (Word Select)\r\n");
                    ht306_usb_log("       -> GP3: BCK  (Bit Clock)\r\n");
                    ht306_usb_log("       -> GP4: ADATA0 (Front L/R)\r\n");
                    ht306_usb_log("       -> GP5: ADATA1 (Surround L/R)\r\n");
                    ht306_usb_log("       -> GP6: ADATA2 (Center / Subwoofer)\r\n");
                } else {
                    ht306_usb_log("[ERROR] Gagal Inisialisasi PIO0 I2S Engine!\r\n");
                }
                ht306_usb_log("[MUX]  GP8 disetel HIGH (+3.3V) -> Mode PC USB Aktif\r\n");
                ht306_usb_log("[WAIT] Standby (LED Biru) - Menunggu Aliran Audio dari PC...\r\n");
                ht306_usb_log("[INFO] Ketik HT306_BOOTSEL + ENTER untuk mode BOOTSEL, atau ENTER untuk cek status.\r\n");
                ht306_usb_log("======================================================\r\n\r\n");
                banner_sent = true;
            }

            /* Parser perintah baris CDC (BOOTSEL hanya via baris eksplisit "HT306_BOOTSEL") */
            if (tud_cdc_available() > 0U) {
                char rx_buf[64];
                uint32_t n = tud_cdc_read(rx_buf, sizeof(rx_buf));
                for (uint32_t i = 0U; i < n; ++i) {
                    char c = rx_buf[i];
                    if (c == '\r' || c == '\n') {
                        if (c == '\n' && cdc_last_was_cr) {
                            cdc_last_was_cr = false;
                            continue;
                        }
                        cdc_last_was_cr = (c == '\r');

                        if (!cdc_cmd_overflow && cdc_cmd_len == 13U &&
                            memcmp(cdc_cmd_buf, "HT306_BOOTSEL", 13U) == 0) {
                            ht306_enter_bootsel_safe();
                        } else {
                            HT306_I2SStats_t stats;
                            ht306_i2s_get_stats(&stats);
                            ht306_usb_log_printf(
                                "\r\n>>> [HT306 STATUS] Core 0 (USB) & Core 1 (DSP) %s | I2S %s %s | MUX GP8: HIGH <<<\r\n\r\n",
                                i2s_ok ? "AKTIF" : "STANDBY",
                                ht306_sample_rate_label(stats.current_sample_rate),
                                i2s_ok ? "SIAP" : "ERROR"
                            );
                        }
                        cdc_cmd_len = 0U;
                        cdc_cmd_overflow = false;
                    } else {
                        cdc_last_was_cr = false;
                        if (cdc_cmd_len < (sizeof(cdc_cmd_buf) - 1U)) {
                            cdc_cmd_buf[cdc_cmd_len++] = c;
                        } else {
                            cdc_cmd_overflow = true;
                        }
                    }
                }
            }

            /* Visualisasi VU meter 6-kanal periodik setiap ~250 ms (4 Hz) saat streaming aktif */
            if (is_streaming) {
                if (now_ms - last_vu_ms >= 250U) {
                    last_vu_ms = now_ms;

                    uint32_t peaks[HT306_NUM_CHANNELS];
                    for (uint32_t ch = 0U; ch < HT306_NUM_CHANNELS; ++ch) {
                        peaks[ch] = __atomic_exchange_n(&g_channel_peaks[ch], 0U, __ATOMIC_RELAXED);
                    }

                    HT306_I2SStats_t stats;
                    ht306_i2s_get_stats(&stats);

                    ht306_usb_log_printf(
                        "[VU 5.1] FL:[%s] FR:[%s] C:[%s] LFE:[%s] SL:[%s] SR:[%s] | Rate: %s | Pkts: %lu | U:%lu O:%lu Q:%lu\r\n",
                        ht306_vu_meter_bar(peaks[0]),
                        ht306_vu_meter_bar(peaks[1]),
                        ht306_vu_meter_bar(peaks[2]),
                        ht306_vu_meter_bar(peaks[3]),
                        ht306_vu_meter_bar(peaks[4]),
                        ht306_vu_meter_bar(peaks[5]),
                        ht306_sample_rate_label(stats.current_sample_rate),
                        (unsigned long)current_pkt,
                        (unsigned long)stats.underrun_count,
                        (unsigned long)stats.overrun_count,
                        (unsigned long)(stats.buffered_frames / HT306_DMA_BLOCK_FRAMES)
                    );
                }
                last_heartbeat_ms = now_ms;
            } else {
                /* Detak Jantung berkala setiap 1 detik saat Serial Monitor terbuka & standby */
                if (now_ms - last_heartbeat_ms >= 1000U) {
                    last_heartbeat_ms = now_ms;
                    uint32_t sec = now_ms / 1000U;
                    HT306_I2SStats_t stats;
                    ht306_i2s_get_stats(&stats);
                    ht306_usb_log_printf(
                        "[HT306] Detak Jantung #%lu | Uptime: %lu detik | I2S Engine: %s (%s) | GP8 MUX: HIGH\r\n",
                        (unsigned long)sec,
                        (unsigned long)sec,
                        i2s_ok ? "SIAP" : "ERROR",
                        ht306_sample_rate_label(stats.current_sample_rate)
                    );
                }
            }
        } else {
            banner_sent = false;
            cdc_cmd_len = 0U;
            cdc_cmd_overflow = false;
            cdc_last_was_cr = false;
        }
    }

    return 0;
}
#endif
