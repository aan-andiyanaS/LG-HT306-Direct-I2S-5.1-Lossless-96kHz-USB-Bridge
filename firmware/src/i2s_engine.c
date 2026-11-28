#include "i2s_engine.h"
#include <string.h>

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "i2s_multi.pio.h"
#define HT306_I2S_RAM_FUNC(fn) __not_in_flash_func(fn)
#define HT306_I2S_LUT_ATTR     __not_in_flash("ht306_lut")
#define HT306_MEMORY_BARRIER() __dmb()
#else
#define HT306_I2S_RAM_FUNC(fn) fn
#define HT306_I2S_LUT_ATTR
#define HT306_MEMORY_BARRIER() ((void)0)
#endif

#define HT306_DMA_BLOCK_WORDS (HT306_DMA_BLOCK_FRAMES * HT306_I2S_WORDS_PER_FRAME)

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert((HT306_RING_BUFFER_BLOCKS & (HT306_RING_BUFFER_BLOCKS - 1U)) == 0U,
               "HT306_RING_BUFFER_BLOCKS must be a power of two");
_Static_assert(HT306_RING_BUFFER_BLOCKS >= 8U,
               "HT306_RING_BUFFER_BLOCKS must hold pre-fill watermark and in-flight DMA blocks");
#endif

/**
 * Block-Indexed Ring Buffer SPSC Wait-Free di SRAM (32 blok x 48 frame x 8 uint32_t).
 *
 * Menggunakan 3 counter blok uint32_t monotonik + masking power-of-2 (& 31U):
 * - g_write_blocks      : dinaikkan Core 1 setelah selesai menulis blok & __dmb()
 * - g_dma_queued_blocks : dinaikkan DMA ISR saat kanal DMA diarahkan ke blok ring
 * - g_read_blocks       : dinaikkan DMA ISR HANYA ketika kanal DMA yang memegang blok ring SELESAI
 *
 * Dengan invariant (g_write_blocks - g_read_blocks) <= HT306_RING_BUFFER_BLOCKS,
 * Core 1 dijamin 100% tidak pernah menimpa 2 blok yang sedang in-flight di DMA dan
 * bebas wraparound 2^32.
 */
static uint32_t g_pio_ring_buffer[HT306_RING_BUFFER_BLOCKS][HT306_DMA_BLOCK_WORDS];
static int32_t  g_ring_block_last_pcm[HT306_RING_BUFFER_BLOCKS][6];
static uint32_t g_ring_block_rate[HT306_RING_BUFFER_BLOCKS];
static uint32_t g_silence_block[HT306_DMA_BLOCK_WORDS];

static volatile uint32_t g_write_blocks      = 0U;
static volatile uint32_t g_dma_queued_blocks = 0U;
static volatile uint32_t g_read_blocks       = 0U;

static volatile uint32_t g_active_rate               = HT306_SAMPLE_RATE_48000;
static volatile uint32_t g_requested_rate            = HT306_SAMPLE_RATE_48000;
static volatile uint32_t g_stat_underrun_count       = 0U;
static volatile uint32_t g_stat_overrun_count        = 0U;
static volatile uint32_t g_stat_blocks_transmitted   = 0U;
static volatile uint32_t g_stat_total_written_frames = 0U;
static volatile bool     g_stop_requested            = false;

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
typedef struct {
    uint16_t div_int;
    uint8_t  div_frac8;
} HT306_ClkDiv_t;

typedef enum {
    HT306_PLAY_IDLE = 0,
    HT306_PLAY_ACTIVE,
    HT306_PLAY_RATE_SWITCH
} HT306_PlayState_t;

static uint32_t          g_ramp_block[HT306_DMA_BLOCK_WORDS];
static int32_t           g_last_pcm32[6] = {0};
static bool              g_chan_holds_ring[2] = { false, false };
static bool              g_chan_holds_ramp[2] = { false, false };
static HT306_PlayState_t g_play_state = HT306_PLAY_IDLE;
static uint8_t           g_rate_silence_remaining = 0U;
static uint32_t          g_pending_switch_rate = HT306_SAMPLE_RATE_48000;

static HT306_ClkDiv_t    g_clkdiv_44k1 = { 26U, 147U };
static HT306_ClkDiv_t    g_clkdiv_48k  = { 24U, 106U };
static HT306_ClkDiv_t    g_clkdiv_96k  = { 12U, 53U  };
static HT306_ClkDiv_t    g_active_clkdiv = { 12U, 53U };
static int8_t            g_pll_trim_state = 0; /* -1: lambat (+1 frac8), 0: nominal, +1: cepat (-1 frac8) */

static int               g_dma_chan_a = -1;
static int               g_dma_chan_b = -1;
static PIO               g_pio_instance = pio0;
static uint              g_pio_sm = 0U;
static bool              g_pio_sm_claimed = false;
static uint              g_pio_offset = 0U;
static bool              g_pio_program_loaded = false;
#endif

/* LUT 256-entry di SRAM: memetakan byte (MSB..LSB) ke posisi bit-0 tiap 8 nibble 4-lane PIO (~10x speedup) */
#define HT306_SPREAD_BYTE(x) ( \
    ((((uint32_t)(x) >> 7U) & 1U) <<  0U) | \
    ((((uint32_t)(x) >> 6U) & 1U) <<  4U) | \
    ((((uint32_t)(x) >> 5U) & 1U) <<  8U) | \
    ((((uint32_t)(x) >> 4U) & 1U) << 12U) | \
    ((((uint32_t)(x) >> 3U) & 1U) << 16U) | \
    ((((uint32_t)(x) >> 2U) & 1U) << 20U) | \
    ((((uint32_t)(x) >> 1U) & 1U) << 24U) | \
    ((((uint32_t)(x) >> 0U) & 1U) << 28U) )

#define HT306_SPREAD4(b) \
    HT306_SPREAD_BYTE((b) + 0U), HT306_SPREAD_BYTE((b) + 1U), \
    HT306_SPREAD_BYTE((b) + 2U), HT306_SPREAD_BYTE((b) + 3U)
#define HT306_SPREAD16(b) \
    HT306_SPREAD4((b) + 0U), HT306_SPREAD4((b) + 4U), \
    HT306_SPREAD4((b) + 8U), HT306_SPREAD4((b) + 12U)
#define HT306_SPREAD64(b) \
    HT306_SPREAD16((b) + 0U), HT306_SPREAD16((b) + 16U), \
    HT306_SPREAD16((b) + 32U), HT306_SPREAD16((b) + 48U)

static const uint32_t HT306_I2S_LUT_ATTR k_spread_rev8[256] = {
    HT306_SPREAD64(0U), HT306_SPREAD64(64U), HT306_SPREAD64(128U), HT306_SPREAD64(192U)
};

#undef HT306_SPREAD64
#undef HT306_SPREAD16
#undef HT306_SPREAD4
#undef HT306_SPREAD_BYTE

void HT306_I2S_RAM_FUNC(ht306_interleave_4lanes_to_pio_words)(
    uint32_t lane0_sample32,
    uint32_t lane1_sample32,
    uint32_t lane2_sample32,
    uint32_t lane3_sample32,
    uint32_t pio_words_out[4])
{
    pio_words_out[0] =  k_spread_rev8[(lane0_sample32 >> 24U) & 0xFFU]
                     | (k_spread_rev8[(lane1_sample32 >> 24U) & 0xFFU] << 1U)
                     | (k_spread_rev8[(lane2_sample32 >> 24U) & 0xFFU] << 2U)
                     | (k_spread_rev8[(lane3_sample32 >> 24U) & 0xFFU] << 3U);
    pio_words_out[1] =  k_spread_rev8[(lane0_sample32 >> 16U) & 0xFFU]
                     | (k_spread_rev8[(lane1_sample32 >> 16U) & 0xFFU] << 1U)
                     | (k_spread_rev8[(lane2_sample32 >> 16U) & 0xFFU] << 2U)
                     | (k_spread_rev8[(lane3_sample32 >> 16U) & 0xFFU] << 3U);
    pio_words_out[2] =  k_spread_rev8[(lane0_sample32 >>  8U) & 0xFFU]
                     | (k_spread_rev8[(lane1_sample32 >>  8U) & 0xFFU] << 1U)
                     | (k_spread_rev8[(lane2_sample32 >>  8U) & 0xFFU] << 2U)
                     | (k_spread_rev8[(lane3_sample32 >>  8U) & 0xFFU] << 3U);
    pio_words_out[3] =  k_spread_rev8[ lane0_sample32         & 0xFFU]
                     | (k_spread_rev8[ lane1_sample32         & 0xFFU] << 1U)
                     | (k_spread_rev8[ lane2_sample32         & 0xFFU] << 2U)
                     | (k_spread_rev8[ lane3_sample32         & 0xFFU] << 3U);
}

bool HT306_I2S_RAM_FUNC(ht306_i2s_ring_has_space)(void) {
    uint32_t used_blocks = (uint32_t)(g_write_blocks - g_read_blocks);
    return used_blocks < HT306_RING_BUFFER_BLOCKS;
}

size_t HT306_I2S_RAM_FUNC(ht306_i2s_write_frames_6ch)(
    const int32_t *samples_6ch_i2s32,
    size_t num_frames)
{
    if (samples_6ch_i2s32 == NULL || num_frames == 0U ||
        (num_frames % HT306_DMA_BLOCK_FRAMES) != 0U) {
        return 0U;
    }

    uint32_t blocks_needed = (uint32_t)(num_frames / HT306_DMA_BLOCK_FRAMES);
    uint32_t local_write = g_write_blocks;
    uint32_t used_blocks = (uint32_t)(local_write - g_read_blocks);
    uint32_t block_rate = g_requested_rate;

    /* Proteksi penuh: jangan pernah menimpa blok yang masih antre atau in-flight di DMA */
    if ((used_blocks + blocks_needed) > HT306_RING_BUFFER_BLOCKS) {
        g_stat_overrun_count = g_stat_overrun_count + 1U;
        return 0U;
    }

    for (uint32_t b = 0U; b < blocks_needed; ++b) {
        uint32_t block_idx = local_write & HT306_RING_BUFFER_MASK;
        uint32_t *block_dst = g_pio_ring_buffer[block_idx];
        const int32_t *block_src = &samples_6ch_i2s32[(size_t)b * HT306_DMA_BLOCK_FRAMES * 6U];

        for (size_t n = 0U; n < HT306_DMA_BLOCK_FRAMES; ++n) {
            const int32_t *f = &block_src[n * 6U];
            uint32_t *dst = &block_dst[n * HT306_I2S_WORDS_PER_FRAME];

            /* Mapping 4-lane fisik HT306 ([0]=FL,[1]=FR,[2]=C,[3]=LFE,[4]=SL,[5]=SR):
             * GP4(L)=SL, GP5(L)=FL, GP6(L)=C | GP4(R)=FR, GP5(R)=SR, GP6(R)=LFE */
            ht306_interleave_4lanes_to_pio_words((uint32_t)f[4], (uint32_t)f[0], (uint32_t)f[2], 0U, &dst[0]);
            ht306_interleave_4lanes_to_pio_words((uint32_t)f[1], (uint32_t)f[5], (uint32_t)f[3], 0U, &dst[4]);
        }

        const int32_t *last_f = &block_src[(HT306_DMA_BLOCK_FRAMES - 1U) * 6U];
        for (size_t ch = 0U; ch < 6U; ++ch) {
            g_ring_block_last_pcm[block_idx][ch] = last_f[ch];
        }
        g_ring_block_rate[block_idx] = block_rate;

        local_write++;
    }

    HT306_MEMORY_BARRIER();
    g_write_blocks = local_write;
    g_stat_total_written_frames = g_stat_total_written_frames + (uint32_t)num_frames;
    return num_frames;
}

void HT306_I2S_RAM_FUNC(ht306_i2s_request_sample_rate)(HT306_SampleRate_t new_rate) {
    if (new_rate != HT306_SAMPLE_RATE_44100 &&
        new_rate != HT306_SAMPLE_RATE_48000 &&
        new_rate != HT306_SAMPLE_RATE_96000) {
        return;
    }

    if (g_requested_rate == (uint32_t)new_rate) {
        return;
    }

    g_requested_rate = (uint32_t)new_rate;
    HT306_MEMORY_BARRIER();

#if !defined(PICO_BUILD) && !defined(PICO_RP2350) && !defined(PICO_RP2040)
    g_active_rate = (uint32_t)new_rate;
#endif
}

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
static HT306_ClkDiv_t ht306_compute_clkdiv_16_8(uint32_t sys_hz, uint32_t sample_rate) {
    /* 128 siklus PIO per frame stereo; scaled 256x => (sys_hz * 256) / (rate * 128) = (sys_hz * 2) / rate */
    uint64_t scaled = (((uint64_t)sys_hz * 2ULL) + ((uint64_t)sample_rate / 2ULL)) / (uint64_t)sample_rate;
    HT306_ClkDiv_t res;
    res.div_int   = (uint16_t)(scaled >> 8U);
    res.div_frac8 = (uint8_t)(scaled & 0xFFU);
    return res;
}

static inline HT306_ClkDiv_t __not_in_flash_func(ht306_clkdiv_for_rate)(HT306_SampleRate_t rate) {
    if (rate == HT306_SAMPLE_RATE_96000) {
        return g_clkdiv_96k;
    }
    if (rate == HT306_SAMPLE_RATE_44100) {
        return g_clkdiv_44k1;
    }
    return g_clkdiv_48k;
}

static void __not_in_flash_func(ht306_pll_adjust_symmetrical)(uint32_t queued_blocks) {
    int8_t next_state = g_pll_trim_state;
    if (g_pll_trim_state == 0) {
        if (queued_blocks < 16U) {
            next_state = -1;
        } else if (queued_blocks > 48U) {
            next_state = 1;
        }
    } else if (g_pll_trim_state < 0) {
        if (queued_blocks >= 32U) {
            next_state = 0;
        }
    } else {
        if (queued_blocks <= 32U) {
            next_state = 0;
        }
    }

    if (next_state == g_pll_trim_state) {
        return;
    }

    g_pll_trim_state = next_state;
    uint32_t d_int  = g_active_clkdiv.div_int;
    uint8_t  d_frac = g_active_clkdiv.div_frac8;

    if (next_state > 0) {
        /* Host sedikit lebih cepat (+ppm): percepat PIO -1 LSB frac8 (dengan borrow) */
        if (d_frac == 0U) {
            if (d_int > 1U) {
                d_int -= 1U;
                d_frac = 255U;
            }
        } else {
            d_frac = (uint8_t)(d_frac - 1U);
        }
    } else if (next_state < 0) {
        /* Host sedikit lebih lambat (-ppm): perlambat PIO +1 LSB frac8 (dengan carry) */
        if (d_frac == 255U) {
            d_int += 1U;
            d_frac = 0U;
        } else {
            d_frac = (uint8_t)(d_frac + 1U);
        }
    }

    pio_sm_set_clkdiv_int_frac8(g_pio_instance, g_pio_sm, d_int, d_frac);
}

static bool __not_in_flash_func(ht306_build_fade_out_ramp_block)(void) {
    /* Lewati ramp saat 6 sampel terakhir sudah hening 0 (menghemat 96x interleave di DMA_IRQ_0) */
    uint32_t nz = (uint32_t)g_last_pcm32[0] | (uint32_t)g_last_pcm32[1]
                | (uint32_t)g_last_pcm32[2] | (uint32_t)g_last_pcm32[3]
                | (uint32_t)g_last_pcm32[4] | (uint32_t)g_last_pcm32[5];
    if (nz == 0U) {
        return false;
    }

    /* Linear fade-out 48 frame menuju 0 menggunakan fixed-point Q16 murni (0 libgcc Flash / 0 float) */
    for (uint32_t n = 0U; n < HT306_DMA_BLOCK_FRAMES; ++n) {
        int64_t scale_q16 = (int64_t)((HT306_DMA_BLOCK_FRAMES - 1U - n) * 1365U);
        uint32_t s[6];
        for (uint32_t ch = 0U; ch < 6U; ++ch) {
            int32_t faded = (int32_t)(((int64_t)g_last_pcm32[ch] * scale_q16) >> 16);
            s[ch] = (uint32_t)faded;
        }
        uint32_t *dst = &g_ramp_block[n * HT306_I2S_WORDS_PER_FRAME];
        ht306_interleave_4lanes_to_pio_words(s[4], s[0], s[2], 0U, &dst[0]);
        ht306_interleave_4lanes_to_pio_words(s[1], s[5], s[3], 0U, &dst[4]);
    }

    for (uint32_t ch = 0U; ch < 6U; ++ch) {
        g_last_pcm32[ch] = 0;
    }
    return true;
}

static void __not_in_flash_func(ht306_dma_reload_channel)(int chan_idx, uint dma_chan) {
    const uint32_t transfer_words = HT306_DMA_BLOCK_WORDS;
    const uint32_t *src = g_silence_block;
    bool holds_ring = false;
    bool holds_ramp = false;
    bool ramp_in_flight = g_chan_holds_ramp[0] || g_chan_holds_ramp[1];

    g_stat_blocks_transmitted = g_stat_blocks_transmitted + 1U;

    /* 1. Permintaan mute & stop aman (sebelum BOOTSEL) */
    if (g_stop_requested) {
        if (g_play_state == HT306_PLAY_ACTIVE && !ramp_in_flight && ht306_build_fade_out_ramp_block()) {
            src = g_ramp_block;
            holds_ramp = true;
        }
        g_play_state = HT306_PLAY_IDLE;
        goto commit_dma;
    }

    /* 2. Sekuens pergantian sample rate bebas pop: ramp -> 2 blok silence -> set CLKDIV */
    if (g_play_state == HT306_PLAY_RATE_SWITCH) {
        if (g_rate_silence_remaining > 0U) {
            g_rate_silence_remaining--;
        } else {
            HT306_SampleRate_t target = (HT306_SampleRate_t)g_pending_switch_rate;
            g_active_clkdiv = ht306_clkdiv_for_rate(target);
            pio_sm_set_clkdiv_int_frac8(g_pio_instance, g_pio_sm,
                                        g_active_clkdiv.div_int, g_active_clkdiv.div_frac8);
            g_pll_trim_state = 0;
            g_active_rate = (uint32_t)target;
            g_play_state = HT306_PLAY_IDLE;
        }
        goto commit_dma;
    }

    /* 3. Snapshot satu kali untuk cek pergantian frekuensi & alur playback (bebas double-__dmb & TOCTOU) */
    {
        uint32_t w = g_write_blocks;
        uint32_t q = g_dma_queued_blocks;
        uint32_t queued = (uint32_t)(w - q);
        if (queued > 0U) {
            HT306_MEMORY_BARRIER();
        }

        uint32_t target_rate = (queued > 0U) ? g_ring_block_rate[q & HT306_RING_BUFFER_MASK] : g_requested_rate;
        if (target_rate != g_active_rate &&
            (target_rate == HT306_SAMPLE_RATE_44100 ||
             target_rate == HT306_SAMPLE_RATE_48000 ||
             target_rate == HT306_SAMPLE_RATE_96000)) {
            if (g_play_state == HT306_PLAY_ACTIVE && !ramp_in_flight && ht306_build_fade_out_ramp_block()) {
                src = g_ramp_block;
                holds_ramp = true;
                g_pending_switch_rate = target_rate;
                g_rate_silence_remaining = 2U;
                g_play_state = HT306_PLAY_RATE_SWITCH;
            } else if (!ramp_in_flight) {
                /* Sudah hening: kirim 2 blok silence lalu ganti CLKDIV */
                g_pending_switch_rate = target_rate;
                g_rate_silence_remaining = 1U;
                g_play_state = HT306_PLAY_RATE_SWITCH;
            }
            goto commit_dma;
        }

        /* 4. Alur playback normal & pre-fill watermark */
        if (g_play_state == HT306_PLAY_IDLE) {
            if (!ramp_in_flight && queued >= HT306_PREFILL_WATERMARK_BLOCKS) {
                g_play_state = HT306_PLAY_ACTIVE;
            } else {
                goto commit_dma;
            }
        }

        if (queued > 0U) {
            uint32_t ring_idx = q & HT306_RING_BUFFER_MASK;
            src = g_pio_ring_buffer[ring_idx];
            for (uint32_t ch = 0U; ch < 6U; ++ch) {
                g_last_pcm32[ch] = g_ring_block_last_pcm[ring_idx][ch];
            }
            holds_ring = true;
            g_dma_queued_blocks = q + 1U;

            /* Symmetrical 16.8 PLL tracking tiap 64 blok (~32 ms @ 96 kHz) */
            if ((g_stat_blocks_transmitted & 0x3FU) == 0U) {
                ht306_pll_adjust_symmetrical(queued - 1U);
            }
        } else {
            /* Underrun: reset PLL ke nominal + kirim soft-ramp fade-out 48 frame bila non-zero */
            g_stat_underrun_count = g_stat_underrun_count + 1U;
            if (g_pll_trim_state != 0) {
                g_pll_trim_state = 0;
                pio_sm_set_clkdiv_int_frac8(g_pio_instance, g_pio_sm,
                                            g_active_clkdiv.div_int, g_active_clkdiv.div_frac8);
            }
            if (!ramp_in_flight && ht306_build_fade_out_ramp_block()) {
                src = g_ramp_block;
                holds_ramp = true;
            }
            g_play_state = HT306_PLAY_IDLE;
        }
    }

commit_dma:
    dma_channel_set_trans_count(dma_chan, transfer_words, false);
    dma_channel_set_read_addr(dma_chan, src, false);
    g_chan_holds_ring[chan_idx] = holds_ring;
    g_chan_holds_ramp[chan_idx] = holds_ramp;
}

static void __not_in_flash_func(ht306_dma_irq_handler)(void) {
    bool chan_done[2] = { false, false };
    int chans[2] = { g_dma_chan_a, g_dma_chan_b };

    /* Cek KEDUA kanal DMA secara independen (bukan else-if) guna menutup jalur OOB Dual-IRQ */
    if (dma_channel_get_irq0_status((uint)g_dma_chan_a)) {
        dma_channel_acknowledge_irq0((uint)g_dma_chan_a);
        chan_done[0] = true;
    }
    if (dma_channel_get_irq0_status((uint)g_dma_chan_b)) {
        dma_channel_acknowledge_irq0((uint)g_dma_chan_b);
        chan_done[1] = true;
    }

    if (!chan_done[0] && !chan_done[1]) {
        return;
    }

    /* Jika kedua kanal selesai bersamaan akibat stall, hentikan kanal yang terpicu ulang sebelum reload agar bebas OOB SRAM */
    if (chan_done[0] && chan_done[1]) {
        if (dma_channel_is_busy((uint)g_dma_chan_a)) {
            dma_channel_abort((uint)g_dma_chan_a);
            dma_channel_acknowledge_irq0((uint)g_dma_chan_a);
        }
        if (dma_channel_is_busy((uint)g_dma_chan_b)) {
            dma_channel_abort((uint)g_dma_chan_b);
            dma_channel_acknowledge_irq0((uint)g_dma_chan_b);
        }
    }

    bool read_advanced = false;
    int first_reloaded_chan = -1;

    /* 1. Lepas kepemilikan blok ring/ramp HANYA setelah kanal DMA benar-benar selesai */
    for (int idx = 0; idx < 2; ++idx) {
        if (!chan_done[idx]) {
            continue;
        }
        if (g_chan_holds_ring[idx]) {
            g_chan_holds_ring[idx] = false;
            g_read_blocks = g_read_blocks + 1U;
            read_advanced = true;
        }
        if (g_chan_holds_ramp[idx]) {
            g_chan_holds_ramp[idx] = false;
        }
    }

    /* 2. Reload setiap kanal yang selesai dengan READ_ADDR & trans_count valid */
    for (int idx = 0; idx < 2; ++idx) {
        if (!chan_done[idx]) {
            continue;
        }
        ht306_dma_reload_channel(idx, (uint)chans[idx]);
        if (first_reloaded_chan < 0) {
            first_reloaded_chan = chans[idx];
        }
    }

    if (read_advanced) {
        __dsb();
        __sev();
    }

    /* 3. Pemulihan stall aman: mulai dari kanal PERTAMA yang di-reload agar urutan blok tetap kronologis */
    if (first_reloaded_chan >= 0 &&
        !dma_channel_is_busy((uint)g_dma_chan_a) &&
        !dma_channel_is_busy((uint)g_dma_chan_b)) {
        dma_channel_start((uint)first_reloaded_chan);
    }
}
#endif

void ht306_i2s_mute_and_stop(void) {
    g_stop_requested = true;
    HT306_MEMORY_BARRIER();

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
    if (g_pio_program_loaded && g_dma_chan_a >= 0 && g_dma_chan_b >= 0) {
        /* Beri waktu 8 ms agar fade-out ramp + beberapa blok silence terkirim penuh di 44.1/48/96 kHz */
        sleep_ms(8);

        irq_set_enabled(DMA_IRQ_0, false);
        dma_channel_set_irq0_enabled((uint)g_dma_chan_a, false);
        dma_channel_set_irq0_enabled((uint)g_dma_chan_b, false);

        pio_sm_set_enabled(g_pio_instance, g_pio_sm, false);
        dma_channel_abort((uint)g_dma_chan_a);
        dma_channel_abort((uint)g_dma_chan_b);
        pio_sm_clear_fifos(g_pio_instance, g_pio_sm);
    }
#endif
}

bool ht306_i2s_engine_init(HT306_SampleRate_t initial_rate) {
    memset(g_pio_ring_buffer, 0, sizeof(g_pio_ring_buffer));
    memset(g_ring_block_last_pcm, 0, sizeof(g_ring_block_last_pcm));
    for (uint32_t i = 0U; i < HT306_RING_BUFFER_BLOCKS; ++i) {
        g_ring_block_rate[i] = (uint32_t)initial_rate;
    }
    memset(g_silence_block, 0, sizeof(g_silence_block));

    g_write_blocks              = 0U;
    g_dma_queued_blocks         = 0U;
    g_read_blocks               = 0U;
    g_active_rate               = (uint32_t)initial_rate;
    g_requested_rate            = (uint32_t)initial_rate;
    g_stat_underrun_count       = 0U;
    g_stat_overrun_count        = 0U;
    g_stat_blocks_transmitted   = 0U;
    g_stat_total_written_frames = 0U;
    g_stop_requested            = false;

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
    memset(g_ramp_block, 0, sizeof(g_ramp_block));
    memset(g_last_pcm32, 0, sizeof(g_last_pcm32));
    g_chan_holds_ring[0] = false;
    g_chan_holds_ring[1] = false;
    g_chan_holds_ramp[0] = false;
    g_chan_holds_ramp[1] = false;
    g_play_state = HT306_PLAY_IDLE;
    g_rate_silence_remaining = 0U;
    g_pending_switch_rate = (uint32_t)initial_rate;
    g_pll_trim_state = 0;

    /* 1. Muat program assembly PIO `i2s_multi_4lane` ke memori instruksi PIO0 */
    if (!g_pio_program_loaded) {
        if (!pio_can_add_program(g_pio_instance, &i2s_multi_4lane_program)) {
            return false;
        }
        g_pio_offset = pio_add_program(g_pio_instance, &i2s_multi_4lane_program);
        g_pio_program_loaded = true;
    }

    if (!g_pio_sm_claimed) {
        g_pio_sm = (uint)pio_claim_unused_sm(g_pio_instance, true);
        g_pio_sm_claimed = true;
    }

    /* 2. Inisialisasi GPIO GP2..GP7 pada Waveshare RP2350-Zero */
    for (uint pin = HT306_PIN_I2S_LRCK; pin <= HT306_PIN_I2S_ADATA3; ++pin) {
        pio_gpio_init(g_pio_instance, pin);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
    }
    pio_sm_set_consecutive_pindirs(g_pio_instance, g_pio_sm, HT306_PIN_I2S_LRCK, 6U, true);

    /* 3. Prekomputasi divider 16.8 exact untuk 44.1 / 48 / 96 kHz dari clock_get_hz(clk_sys) */
    uint32_t sys_hz = clock_get_hz(clk_sys);
    g_clkdiv_44k1 = ht306_compute_clkdiv_16_8(sys_hz, HT306_SAMPLE_RATE_44100);
    g_clkdiv_48k  = ht306_compute_clkdiv_16_8(sys_hz, HT306_SAMPLE_RATE_48000);
    g_clkdiv_96k  = ht306_compute_clkdiv_16_8(sys_hz, HT306_SAMPLE_RATE_96000);
    g_active_clkdiv = ht306_clkdiv_for_rate(initial_rate);

    /* 4. Konfigurasi State Machine PIO0 (Side-set: GP2=LRCK / GP3=BCK, OUT: GP4..GP7, Autopull 32-bit) */
    pio_sm_config sm_cfg = i2s_multi_4lane_program_get_default_config(g_pio_offset);
    sm_config_set_sideset_pins(&sm_cfg, HT306_PIN_I2S_LRCK);
    sm_config_set_out_pins(&sm_cfg, HT306_PIN_I2S_ADATA0, 4U);
    sm_config_set_out_shift(&sm_cfg, true, true, 32U); /* Shift Right, Autopull threshold = 32 */
    sm_config_set_fifo_join(&sm_cfg, PIO_FIFO_JOIN_TX); /* Gabungkan FIFO RX+TX menjadi 8-word TX FIFO */
    sm_config_set_clkdiv_int_frac8(&sm_cfg, g_active_clkdiv.div_int, g_active_clkdiv.div_frac8);

    pio_sm_init(g_pio_instance, g_pio_sm, g_pio_offset, &sm_cfg);

    /* 5. Klaim & konfigurasi 2 kanal DMA untuk operasi Chained Ping-Pong */
    g_dma_chan_a = dma_claim_unused_channel(true);
    g_dma_chan_b = dma_claim_unused_channel(true);

    dma_channel_config cfg_a = dma_channel_get_default_config((uint)g_dma_chan_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_dreq(&cfg_a, pio_get_dreq(g_pio_instance, g_pio_sm, true));
    channel_config_set_chain_to(&cfg_a, (uint)g_dma_chan_b);

    dma_channel_config cfg_b = dma_channel_get_default_config((uint)g_dma_chan_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_dreq(&cfg_b, pio_get_dreq(g_pio_instance, g_pio_sm, true));
    channel_config_set_chain_to(&cfg_b, (uint)g_dma_chan_a);

    const uint32_t transfer_words = HT306_DMA_BLOCK_WORDS;

    dma_channel_configure(
        (uint)g_dma_chan_a, &cfg_a,
        &g_pio_instance->txf[g_pio_sm],
        g_silence_block, transfer_words, false
    );
    dma_channel_configure(
        (uint)g_dma_chan_b, &cfg_b,
        &g_pio_instance->txf[g_pio_sm],
        g_silence_block, transfer_words, false
    );

    dma_channel_set_irq0_enabled((uint)g_dma_chan_a, true);
    dma_channel_set_irq0_enabled((uint)g_dma_chan_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, ht306_dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);

    /* 6. Aktifkan DMA dan State Machine PIO0 */
    dma_channel_start((uint)g_dma_chan_a);
    pio_sm_set_enabled(g_pio_instance, g_pio_sm, true);
#endif

    return true;
}

void ht306_i2s_get_stats(HT306_I2SStats_t *stats_out) {
    if (stats_out != NULL) {
        uint32_t q = g_dma_queued_blocks;
        HT306_MEMORY_BARRIER();
        uint32_t w = g_write_blocks;
        stats_out->current_sample_rate  = g_active_rate;
        stats_out->buffered_frames      = (uint32_t)(w - q) * HT306_DMA_BLOCK_FRAMES;
        stats_out->underrun_count       = g_stat_underrun_count;
        stats_out->overrun_count        = g_stat_overrun_count;
        stats_out->blocks_transmitted   = g_stat_blocks_transmitted;
        stats_out->total_written_frames = g_stat_total_written_frames;
    }
}

#if !defined(PICO_BUILD) && !defined(PICO_RP2350) && !defined(PICO_RP2040)
void ht306_i2s_test_set_block_counters(uint32_t write_blocks, uint32_t dma_queued_blocks, uint32_t read_blocks) {
    g_write_blocks      = write_blocks;
    g_dma_queued_blocks = dma_queued_blocks;
    g_read_blocks       = read_blocks;
}

void ht306_i2s_test_get_block_counters(uint32_t *write_blocks, uint32_t *dma_queued_blocks, uint32_t *read_blocks) {
    if (write_blocks != NULL) {
        *write_blocks = g_write_blocks;
    }
    if (dma_queued_blocks != NULL) {
        *dma_queued_blocks = g_dma_queued_blocks;
    }
    if (read_blocks != NULL) {
        *read_blocks = g_read_blocks;
    }
}

const uint32_t *ht306_i2s_test_consume_block(void) {
    uint32_t q = g_dma_queued_blocks;
    if ((uint32_t)(g_write_blocks - q) == 0U) {
        return NULL;
    }
    uint32_t ring_idx = q & HT306_RING_BUFFER_MASK;
    g_dma_queued_blocks = q + 1U;
    g_read_blocks = g_read_blocks + 1U;
    g_stat_blocks_transmitted = g_stat_blocks_transmitted + 1U;
    return g_pio_ring_buffer[ring_idx];
}

uint32_t ht306_i2s_test_get_block_rate(uint32_t block_idx) {
    return g_ring_block_rate[block_idx & HT306_RING_BUFFER_MASK];
}
#endif
