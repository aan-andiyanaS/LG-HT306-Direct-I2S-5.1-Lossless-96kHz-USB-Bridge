#include "lossless_codec.h"
#include "i2s_engine.h"
#include "ht306_usb.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint32_t g_prng_state = 0x13579BDFU;

static uint32_t next_xorshift32(void) {
    uint32_t x = g_prng_state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    g_prng_state = x;
    return x;
}

static void generate_multichannel_96k_signal_24bit(
    int32_t *pcm_out,
    size_t num_frames,
    size_t frame_offset,
    bool inject_explosion_transient)
{
    const double fs = 96000.0;
    const double freqs[HT306_NUM_CHANNELS] = {
        440.0,   /* FL: Nada Utama */
        880.0,   /* FR: Harmonik */
        300.0,   /* C : Vokal Dialog */
        55.0,    /* LFE: Bass Subwoofer */
        1200.0,  /* SL: Efek Surround Kiri */
        1600.0   /* SR: Efek Surround Kanan */
    };

    for (size_t n = 0; n < num_frames; ++n) {
        double t = (double)(frame_offset + n) / fs;
        for (size_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
            double wave = 0.65 * sin(2.0 * M_PI * freqs[ch] * t)
                        + 0.20 * cos(2.0 * M_PI * (freqs[ch] * 2.5) * t);
            int32_t sample24 = (int32_t)(wave * 4194303.0);

            if (inject_explosion_transient && (n == 17U || n == 18U)) {
                sample24 = (n & 1U) ? 8388600 : -8388600;
            }

            if (sample24 > 8388607) sample24 = 8388607;
            if (sample24 < -8388608) sample24 = -8388608;

            pcm_out[n * HT306_NUM_CHANNELS + ch] = sample24;
        }
    }
}

static void generate_multichannel_96k_signal_32bit(
    int32_t *pcm_out,
    size_t num_frames,
    size_t frame_offset,
    bool inject_explosion_transient)
{
    const double fs = 96000.0;
    const double freqs[HT306_NUM_CHANNELS] = {
        440.0, 880.0, 300.0, 55.0, 1200.0, 1600.0
    };

    for (size_t n = 0; n < num_frames; ++n) {
        double t = (double)(frame_offset + n) / fs;
        for (size_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
            double wave = 0.65 * sin(2.0 * M_PI * freqs[ch] * t)
                        + 0.20 * cos(2.0 * M_PI * (freqs[ch] * 2.5) * t);
            int64_t sample32 = (int64_t)(wave * 1073741823.0);

            if (inject_explosion_transient && (n == 17U || n == 18U)) {
                sample32 = (n & 1U) ? 2147483640LL : -2147483640LL;
            }

            if (sample32 > 2147483647LL) sample32 = 2147483647LL;
            if (sample32 < -2147483648LL) sample32 = -2147483648LL;

            pcm_out[n * HT306_NUM_CHANNELS + ch] = (int32_t)sample32;
        }
    }
}

static void deinterleave_pio_words_4lanes(const uint32_t words[4], uint32_t lanes_out[4]) {
    lanes_out[0] = 0U;
    lanes_out[1] = 0U;
    lanes_out[2] = 0U;
    lanes_out[3] = 0U;

    for (uint32_t w = 0U; w < 4U; ++w) {
        uint32_t osr = words[w];
        for (uint32_t step = 0U; step < 8U; ++step) {
            uint32_t pins = osr & 0xFU;
            osr >>= 4U;
            lanes_out[0] = (lanes_out[0] << 1U) | ((pins >> 0U) & 1U);
            lanes_out[1] = (lanes_out[1] << 1U) | ((pins >> 1U) & 1U);
            lanes_out[2] = (lanes_out[2] << 1U) | ((pins >> 2U) & 1U);
            lanes_out[3] = (lanes_out[3] << 1U) | ((pins >> 3U) & 1U);
        }
    }
}

static int test_verbatim_white_noise(void) {
    printf("\n[3/6] Menjalankan Uji White Noise & Full-Scale Clipping Verbatim Fallback (24-bit & 32-bit)...\n");

    int32_t pcm_in[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t pcm_out[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t i2s_out[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    uint8_t packet[HT306_MAX_PACKET_BYTES];

    /* 3A. 24-bit full-range white noise + clipping */
    for (size_t i = 0; i < HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS; ++i) {
        if ((i % 23U) == 0U) {
            pcm_in[i] = (i & 1U) ? 8388607 : -8388608;
        } else {
            int32_t raw24 = (int32_t)(next_xorshift32() & 0x00FFFFFFU);
            pcm_in[i] = (raw24 ^ 0x00800000) - 0x00800000;
        }
    }

    size_t packet_len = 0U;
    HT306_CodecStatus_t st = ht306_lossless_encode_frame(
        pcm_in, HT306_SUBFRAME_SAMPLES, 0x42U, packet, sizeof(packet), &packet_len
    );
    const size_t expected_24_verbatim = sizeof(HT306_LosslessHeader_t) + (HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS * 3U);
    if (st != HT306_CODEC_OK || packet_len != expected_24_verbatim) {
        printf("[GAGAL] 24-bit noise verbatim encode gagal (st=%d, len=%zu, exp=%zu)\n", st, packet_len, expected_24_verbatim);
        return 1;
    }
    if ((packet[5] & HT306_FLAG_VERBATIM_RAW) == 0U) {
        printf("[GAGAL] 24-bit noise tidak memicu HT306_FLAG_VERBATIM_RAW!\n");
        return 1;
    }
    for (size_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
        if (packet[7U + ch] != 0U) {
            printf("[GAGAL] rice_k[%zu] tidak 0 pada mode verbatim!\n", ch);
            return 1;
        }
    }

    uint8_t decoded_frames = 0U;
    memset(pcm_out, 0, sizeof(pcm_out));
    memset(i2s_out, 0, sizeof(i2s_out));
    st = ht306_lossless_decode_frame(packet, packet_len, pcm_out, i2s_out, &decoded_frames);
    if (st != HT306_CODEC_OK || decoded_frames != HT306_SUBFRAME_SAMPLES ||
        memcmp(pcm_in, pcm_out, sizeof(pcm_in)) != 0) {
        printf("[GAGAL] 24-bit verbatim decode tidak bit-exact (st=%d)!\n", st);
        return 1;
    }
    for (size_t i = 0; i < HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS; ++i) {
        if (i2s_out[i] != (int32_t)((uint32_t)pcm_in[i] << 8U)) {
            printf("[GAGAL] 24-bit verbatim I2S slot salah pada indeks %zu!\n", i);
            return 1;
        }
    }

    /* 3B. 32-bit full-range white noise + clipping */
    for (size_t i = 0; i < HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS; ++i) {
        if ((i % 19U) == 0U) {
            pcm_in[i] = (i & 1U) ? INT32_MAX : INT32_MIN;
        } else {
            pcm_in[i] = (int32_t)next_xorshift32();
        }
    }

    st = ht306_lossless_encode_frame_flags(
        pcm_in, HT306_SUBFRAME_SAMPLES, 0x43U,
        HT306_FLAG_RATE_96KHZ | HT306_FLAG_BITDEPTH_32,
        packet, sizeof(packet), &packet_len
    );
    const size_t expected_32_verbatim = sizeof(HT306_LosslessHeader_t) + (HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS * 4U);
    if (st != HT306_CODEC_OK || packet_len != expected_32_verbatim) {
        printf("[GAGAL] 32-bit noise verbatim encode gagal (st=%d, len=%zu, exp=%zu)\n", st, packet_len, expected_32_verbatim);
        return 1;
    }
    if ((packet[5] & HT306_FLAG_VERBATIM_RAW) == 0U) {
        printf("[GAGAL] 32-bit noise tidak memicu HT306_FLAG_VERBATIM_RAW!\n");
        return 1;
    }

    memset(i2s_out, 0, sizeof(i2s_out));
    st = ht306_lossless_decode_frame(packet, packet_len, NULL, i2s_out, &decoded_frames);
    if (st != HT306_CODEC_OK || decoded_frames != HT306_SUBFRAME_SAMPLES ||
        memcmp(pcm_in, i2s_out, sizeof(pcm_in)) != 0) {
        printf("[GAGAL] 32-bit verbatim decode tidak bit-exact (st=%d)!\n", st);
        return 1;
    }

    /* 3C. Buffer terlalu kecil bahkan untuk verbatim -> wajib mengembalikan BUFFER_TOO_SMALL */
    uint8_t small_buf[256];
    st = ht306_lossless_encode_frame_flags(
        pcm_in, HT306_SUBFRAME_SAMPLES, 0x44U,
        HT306_FLAG_RATE_96KHZ | HT306_FLAG_BITDEPTH_32,
        small_buf, sizeof(small_buf), &packet_len
    );
    if (st != HT306_CODEC_ERR_BUFFER_TOO_SMALL) {
        printf("[GAGAL] Kapasitas kecil seharusnya mengembalikan BUFFER_TOO_SMALL (got=%d)!\n", st);
        return 1;
    }

    printf("  [LULUS] Verbatim Fallback 24-bit (%zu B) & 32-bit (%zu B): 100%% Bit-Exact\n",
           expected_24_verbatim, expected_32_verbatim);
    return 0;
}

static int test_extreme_boundaries(void) {
    printf("\n[4/6] Menjalankan Uji Nilai Batas Ekstrem (INT24_MIN/MAX & INT32_MIN/MAX)...\n");

    int32_t pcm_in[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t pcm_out[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t i2s_out[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    uint8_t packet[HT306_MAX_PACKET_BYTES];

    const int32_t vals24[5] = { -8388608, 8388607, 0, -1, 1 };
    const int32_t vals32[5] = { INT32_MIN, INT32_MAX, 0, -1, 1 };

    /* Pola konstan (uji jalur Rice terkompresi) & pola ekstrem bergantian */
    for (int mode = 0; mode < 6; ++mode) {
        for (size_t n = 0; n < HT306_SUBFRAME_SAMPLES; ++n) {
            for (size_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
                if (mode < 5) {
                    pcm_in[n * HT306_NUM_CHANNELS + ch] = vals24[mode];
                } else {
                    pcm_in[n * HT306_NUM_CHANNELS + ch] = vals24[(n + ch) % 5U];
                }
            }
        }
        size_t pkt_len = 0U;
        uint8_t frames = 0U;
        HT306_CodecStatus_t enc = ht306_lossless_encode_frame(
            pcm_in, HT306_SUBFRAME_SAMPLES, (uint8_t)mode, packet, sizeof(packet), &pkt_len
        );
        HT306_CodecStatus_t dec = ht306_lossless_decode_frame(
            packet, pkt_len, pcm_out, i2s_out, &frames
        );
        if (enc != HT306_CODEC_OK || dec != HT306_CODEC_OK ||
            frames != HT306_SUBFRAME_SAMPLES || memcmp(pcm_in, pcm_out, sizeof(pcm_in)) != 0) {
            printf("[GAGAL] Uji ekstrem 24-bit mode %d gagal (enc=%d, dec=%d)!\n", mode, enc, dec);
            return 1;
        }
    }

    for (int mode = 0; mode < 6; ++mode) {
        for (size_t n = 0; n < HT306_SUBFRAME_SAMPLES; ++n) {
            for (size_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
                if (mode < 5) {
                    pcm_in[n * HT306_NUM_CHANNELS + ch] = vals32[mode];
                } else {
                    pcm_in[n * HT306_NUM_CHANNELS + ch] = vals32[(n + ch) % 5U];
                }
            }
        }
        size_t pkt_len = 0U;
        uint8_t frames = 0U;
        HT306_CodecStatus_t enc = ht306_lossless_encode_frame_flags(
            pcm_in, HT306_SUBFRAME_SAMPLES, (uint8_t)(0x80U + mode),
            HT306_FLAG_RATE_96KHZ | HT306_FLAG_BITDEPTH_32,
            packet, sizeof(packet), &pkt_len
        );
        HT306_CodecStatus_t dec = ht306_lossless_decode_frame(
            packet, pkt_len, NULL, i2s_out, &frames
        );
        if (enc != HT306_CODEC_OK || dec != HT306_CODEC_OK ||
            frames != HT306_SUBFRAME_SAMPLES || memcmp(pcm_in, i2s_out, sizeof(pcm_in)) != 0) {
            printf("[GAGAL] Uji ekstrem 32-bit mode %d gagal (enc=%d, dec=%d)!\n", mode, enc, dec);
            return 1;
        }
    }

    printf("  [LULUS] Nilai Ekstrem INT24_MIN/MAX & INT32_MIN/MAX: 100%% Bit-Exact (Rice & Verbatim)\n");
    return 0;
}

static int test_header_crc_and_guards(void) {
    printf("\n[5/6] Menjalankan Uji Korupsi Header (CRC-8), Flag Tak Didukung, & Guard out_frames_cap...\n");

    int32_t pcm_in[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t pcm_out[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t i2s_out[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    uint8_t packet[HT306_MAX_PACKET_BYTES];

    generate_multichannel_96k_signal_24bit(pcm_in, HT306_SUBFRAME_SAMPLES, 0U, false);
    size_t pkt_len = 0U;
    if (ht306_lossless_encode_frame(pcm_in, HT306_SUBFRAME_SAMPLES, 7U, packet, sizeof(packet), &pkt_len) != HT306_CODEC_OK) {
        printf("[GAGAL] Encode baseline gagal!\n");
        return 1;
    }

    uint8_t dummy_frames = 0U;

    /* 5A. Flip bit di rice_k[2] (offset 9) -> wajib CRC_MISMATCH */
    packet[9] ^= 0x01U;
    HT306_CodecStatus_t st = ht306_lossless_decode_frame(packet, pkt_len, pcm_out, i2s_out, &dummy_frames);
    packet[9] ^= 0x01U;
    if (st != HT306_CODEC_ERR_CRC_MISMATCH) {
        printf("[GAGAL] Korupsi rice_k[2] tidak mengembalikan CRC_MISMATCH (got=%d)!\n", st);
        return 1;
    }

    /* 5B. Flip bit di flags (offset 5 - bit valid & bit unsupported) -> wajib CRC_MISMATCH */
    packet[5] ^= HT306_FLAG_RATE_96KHZ;
    st = ht306_lossless_decode_frame(packet, pkt_len, pcm_out, i2s_out, &dummy_frames);
    packet[5] ^= HT306_FLAG_RATE_96KHZ;
    if (st != HT306_CODEC_ERR_CRC_MISMATCH) {
        printf("[GAGAL] Korupsi flags (RATE_96KHZ) tidak mengembalikan CRC_MISMATCH (got=%d)!\n", st);
        return 1;
    }

    packet[5] ^= (1U << 5);
    st = ht306_lossless_decode_frame(packet, pkt_len, pcm_out, i2s_out, &dummy_frames);
    packet[5] ^= (1U << 5);
    if (st != HT306_CODEC_ERR_CRC_MISMATCH) {
        printf("[GAGAL] Korupsi flags (bit 5) tidak mengembalikan CRC_MISMATCH (got=%d)!\n", st);
        return 1;
    }

    /* 5C. Paket dengan CRC valid namun membawa flag unsupported -> wajib CORRUPT_STREAM */
    uint8_t bad_flag_pkt[HT306_MAX_PACKET_BYTES];
    memcpy(bad_flag_pkt, packet, pkt_len);
    bad_flag_pkt[5] |= HT306_FLAG_CHANNELS_8;
    uint8_t crc = ht306_crc8_update(0x00U, bad_flag_pkt + 2U, sizeof(HT306_LosslessHeader_t) - 3U);
    crc = ht306_crc8_update(crc, bad_flag_pkt + sizeof(HT306_LosslessHeader_t), pkt_len - sizeof(HT306_LosslessHeader_t));
    bad_flag_pkt[sizeof(HT306_LosslessHeader_t) - 1U] = crc;
    st = ht306_lossless_decode_frame(bad_flag_pkt, pkt_len, pcm_out, i2s_out, &dummy_frames);
    if (st != HT306_CODEC_ERR_CORRUPT_STREAM) {
        printf("[GAGAL] Flag unsupported tidak ditolak sebagai CORRUPT_STREAM (got=%d)!\n", st);
        return 1;
    }

    /* 5D. Guard kapasitas output out_frames_cap pada decode_frame_ex */
    st = ht306_lossless_decode_frame_ex(packet, pkt_len, pcm_out, i2s_out, 16U, &dummy_frames);
    if (st != HT306_CODEC_ERR_BUFFER_TOO_SMALL) {
        printf("[GAGAL] out_frames_cap=16 untuk paket 48 frame seharusnya BUFFER_TOO_SMALL (got=%d)!\n", st);
        return 1;
    }

    printf("  [LULUS] Proteksi Header CRC-8 (rice_k & flags), Reject Unsupported Flag, & Guard out_frames_cap\n");
    return 0;
}

static int test_i2s_ring_wraparound_and_mapping(void) {
    printf("\n[6/6] Menjalankan Uji i2s_engine Wraparound (0xFFFFFFFE -> 0x00000002), In-Flight Guard, & 4-Lane Mapping...\n");

    if (!ht306_i2s_engine_init(HT306_SAMPLE_RATE_96000)) {
        printf("[GAGAL] ht306_i2s_engine_init gagal!\n");
        return 1;
    }

    /* 6A. Simulasi wraparound uint32_t 0xFFFFFFFE -> 0x00000002 (4 blok) */
    ht306_i2s_test_set_block_counters(0xFFFFFFFEU, 0xFFFFFFFEU, 0xFFFFFFFEU);

    int32_t blocks_in[4][HT306_DMA_BLOCK_FRAMES * 6U];
    for (uint32_t b = 0U; b < 4U; ++b) {
        for (size_t n = 0U; n < HT306_DMA_BLOCK_FRAMES; ++n) {
            int32_t *row = &blocks_in[b][n * 6U];
            uint32_t tag = (b << 24U) | ((uint32_t)n << 16U);
            row[0] = (int32_t)(tag | 0x0101U); /* FL -> ADATA0 Left */
            row[1] = (int32_t)(tag | 0x0202U); /* FR -> ADATA0 Right */
            row[2] = (int32_t)(tag | 0x0303U); /* C  -> ADATA2 Left */
            row[3] = (int32_t)(tag | 0x0404U); /* LFE-> ADATA2 Right */
            row[4] = (int32_t)(tag | 0x0505U); /* SL -> ADATA1 Left */
            row[5] = (int32_t)(tag | 0x0606U); /* SR -> ADATA1 Right */
        }

        if (!ht306_i2s_ring_has_space()) {
            printf("[GAGAL] Ring seharusnya masih memiliki ruang pada blok %u!\n", b);
            return 1;
        }
        size_t written = ht306_i2s_write_frames_6ch(blocks_in[b], HT306_DMA_BLOCK_FRAMES);
        if (written != HT306_DMA_BLOCK_FRAMES) {
            printf("[GAGAL] ht306_i2s_write_frames_6ch gagal pada blok %u (written=%zu)!\n", b, written);
            return 1;
        }
    }

    uint32_t w = 0U, q = 0U, r = 0U;
    ht306_i2s_test_get_block_counters(&w, &q, &r);
    if (w != 0x00000002U || q != 0xFFFFFFFEU || r != 0xFFFFFFFEU) {
        printf("[GAGAL] Counter pasca-wraparound salah (w=0x%08X, q=0x%08X, r=0x%08X)!\n", w, q, r);
        return 1;
    }

    for (uint32_t b = 0U; b < 4U; ++b) {
        const uint32_t *pio_block = ht306_i2s_test_consume_block();
        if (pio_block == NULL) {
            printf("[GAGAL] Consume blok %u mengembalikan NULL!\n", b);
            return 1;
        }

        for (size_t n = 0U; n < HT306_DMA_BLOCK_FRAMES; ++n) {
            const uint32_t *frame_words = &pio_block[n * HT306_I2S_WORDS_PER_FRAME];
            const int32_t *exp = &blocks_in[b][n * 6U];

            uint32_t left_lanes[4];
            uint32_t right_lanes[4];
            deinterleave_pio_words_4lanes(&frame_words[0], left_lanes);
            deinterleave_pio_words_4lanes(&frame_words[4], right_lanes);

            if (left_lanes[0] != (uint32_t)exp[4] ||  /* ADATA0 = SL */
                left_lanes[1] != (uint32_t)exp[0] ||  /* ADATA1 = FL */
                left_lanes[2] != (uint32_t)exp[2] ||  /* ADATA2 = C  */
                left_lanes[3] != 0U ||                /* ADATA3 = 0  */
                right_lanes[0] != (uint32_t)exp[1] || /* ADATA0 = FR */
                right_lanes[1] != (uint32_t)exp[5] || /* ADATA1 = SR */
                right_lanes[2] != (uint32_t)exp[3] || /* ADATA2 = LFE*/
                right_lanes[3] != 0U) {               /* ADATA3 = 0  */
                printf("[GAGAL] Pemetaan 4-lane PIO salah pada blok %u frame %zu!\n", b, n);
                return 1;
            }
        }
    }

    if (ht306_i2s_test_consume_block() != NULL) {
        printf("[GAGAL] Ring seharusnya kosong setelah 4 blok dikonsumsi!\n");
        return 1;
    }
    ht306_i2s_test_get_block_counters(&w, &q, &r);
    if (w != 0x00000002U || q != 0x00000002U || r != 0x00000002U) {
        printf("[GAGAL] Counter akhir wraparound tidak sinkron (w=0x%08X, q=0x%08X, r=0x%08X)!\n", w, q, r);
        return 1;
    }

    /* 6B. Uji proteksi 2 blok in-flight DMA melewati wraparound */
    ht306_i2s_test_set_block_counters(0xFFFFFFFEU, 0xFFFFFFFEU, 0xFFFFFFFCU);
    for (uint32_t i = 0U; i < (HT306_RING_BUFFER_BLOCKS - 2U); ++i) {
        if (!ht306_i2s_ring_has_space() ||
            ht306_i2s_write_frames_6ch(blocks_in[0], HT306_DMA_BLOCK_FRAMES) != HT306_DMA_BLOCK_FRAMES) {
            printf("[GAGAL] Penulisan blok ke-%u gagal saat ruang seharusnya tersedia!\n", i);
            return 1;
        }
    }
    if (ht306_i2s_ring_has_space()) {
        printf("[GAGAL] Ring harus penuh untuk melindungi 2 blok in-flight DMA!\n");
        return 1;
    }
    if (ht306_i2s_write_frames_6ch(blocks_in[0], HT306_DMA_BLOCK_FRAMES) != 0U) {
        printf("[GAGAL] Write harus ditolak saat 2 blok DMA sedang in-flight!\n");
        return 1;
    }

    printf("  [LULUS] Wraparound 0xFFFFFFFE->0x00000002, 4-Lane PIO Mapping, & Proteksi 2 Blok In-Flight: 100%% OK\n");
    return 0;
}

static uint8_t  g_mock_usb_pkts[8][HT306_MAX_PACKET_BYTES];
static uint16_t g_mock_usb_lens[8];
static uint32_t g_mock_usb_count = 0U;

bool ht306_usb_enqueue_lossless_packet(const uint8_t *usb_data, uint16_t len) {
    if (g_mock_usb_count < 8U && usb_data != NULL && len <= HT306_MAX_PACKET_BYTES) {
        memcpy(g_mock_usb_pkts[g_mock_usb_count], usb_data, len);
        g_mock_usb_lens[g_mock_usb_count] = len;
        g_mock_usb_count++;
    }
    return true;
}

uint32_t ht306_usb_packet_queue_free_slots(void) {
    return 32U;
}

static uint8_t crc8_bitexact_ref(uint8_t init, const uint8_t *data, size_t len) {
    uint8_t crc = init;
    for (size_t i = 0U; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t b = 0U; b < 8U; ++b) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1U) ^ 0x07U) : (uint8_t)(crc << 1U);
        }
    }
    return crc;
}

static void interleave_bitexact_ref(
    uint32_t lane0, uint32_t lane1, uint32_t lane2, uint32_t lane3, uint32_t out[4])
{
    for (uint32_t w = 0U; w < 4U; ++w) {
        uint32_t acc = 0U;
        uint32_t base_bit = 31U - (w * 8U);
        for (uint32_t step = 0U; step < 8U; ++step) {
            uint32_t bit_pos = base_bit - step;
            uint32_t nibble = (((lane0 >> bit_pos) & 1U) << 0U)
                            | (((lane1 >> bit_pos) & 1U) << 1U)
                            | (((lane2 >> bit_pos) & 1U) << 2U)
                            | (((lane3 >> bit_pos) & 1U) << 3U);
            acc |= (nibble << (step * 4U));
        }
        out[w] = acc;
    }
}

static int test_crc8_lut_bitexact(void) {
    printf("\n[7/9] Menjalankan Uji CRC-8 LUT 256-Entry vs Referensi Bit-by-Bit...\n");
    for (uint32_t init = 0U; init < 256U; init += 17U) {
        for (uint32_t v = 0U; v < 256U; ++v) {
            uint8_t byte = (uint8_t)v;
            uint8_t got = ht306_crc8_update((uint8_t)init, &byte, 1U);
            uint8_t ref = crc8_bitexact_ref((uint8_t)init, &byte, 1U);
            if (got != ref) {
                printf("[GAGAL] CRC-8 LUT mismatch (init=0x%02X, byte=0x%02X, got=0x%02X, ref=0x%02X)\n",
                       init, byte, got, ref);
                return 1;
            }
        }
    }
    uint8_t buf[HT306_MAX_PACKET_BYTES];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = (uint8_t)(next_xorshift32() & 0xFFU);
    }
    if (ht306_crc8_update(0x00U, buf, sizeof(buf)) != crc8_bitexact_ref(0x00U, buf, sizeof(buf))) {
        printf("[GAGAL] CRC-8 LUT mismatch pada buffer acak 1200 B!\n");
        return 1;
    }
    printf("  [LULUS] CRC-8 256-Entry LUT: 100%% Identik dengan Referensi Bit-by-Bit\n");
    return 0;
}

static int test_i2s_lut_4lanes_bitexact(void) {
    printf("\n[8/9] Menjalankan Uji I2S 4-Lane LUT vs Referensi Bit-by-Bit (Termasuk Non-Zero Lane 3)...\n");
    const uint32_t edges[6] = {
        0x00000000U, 0xFFFFFFFFU, 0x80000000U, 0x00000001U, 0x55555555U, 0xAAAAAAAAU
    };
    for (size_t e = 0; e < 6U; ++e) {
        uint32_t got[4], ref[4];
        ht306_interleave_4lanes_to_pio_words(edges[e], edges[(e + 1U) % 6U], edges[(e + 2U) % 6U], edges[(e + 3U) % 6U], got);
        interleave_bitexact_ref(edges[e], edges[(e + 1U) % 6U], edges[(e + 2U) % 6U], edges[(e + 3U) % 6U], ref);
        if (memcmp(got, ref, sizeof(got)) != 0) {
            printf("[GAGAL] I2S LUT mismatch pada edge pattern %zu!\n", e);
            return 1;
        }
    }
    for (int i = 0; i < 256; ++i) {
        uint32_t l0 = next_xorshift32();
        uint32_t l1 = next_xorshift32();
        uint32_t l2 = next_xorshift32();
        uint32_t l3 = next_xorshift32() | 0x80000001U; /* Wajib non-zero lane3 */
        uint32_t got[4], ref[4];
        ht306_interleave_4lanes_to_pio_words(l0, l1, l2, l3, got);
        interleave_bitexact_ref(l0, l1, l2, l3, ref);
        if (memcmp(got, ref, sizeof(got)) != 0) {
            printf("[GAGAL] I2S LUT mismatch pada random quad #%d (lane3 non-zero)!\n", i);
            return 1;
        }
    }
    printf("  [LULUS] I2S 4-Lane LUT (256-entry + non-zero lane3): 100%% Bit-Exact\n");
    return 0;
}

static int test_usb_reassembler_bulk_memcpy(void) {
    printf("\n[9/9] Menjalankan Uji USB Bulk Reassembler (Bulk memcpy + Chunking 64-Byte + Sync Recovery)...\n");

    int32_t pcm[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    generate_multichannel_96k_signal_24bit(pcm, HT306_SUBFRAME_SAMPLES, 0U, false);

    uint8_t pkt_a[HT306_MAX_PACKET_BYTES];
    size_t len_a = 0U;
    if (ht306_lossless_encode_frame(pcm, HT306_SUBFRAME_SAMPLES, 0x11U, pkt_a, sizeof(pkt_a), &len_a) != HT306_CODEC_OK) {
        printf("[GAGAL] Encode pkt_a gagal!\n");
        return 1;
    }

    int32_t pcm_min[4 * HT306_NUM_CHANNELS] = {0};
    uint8_t pkt_b[HT306_MAX_PACKET_BYTES];
    size_t len_b = 0U;
    /* 1 frame (6 ch x 3 B = 18 B) -> tepat min_pkt_len 32 B */
    if (ht306_lossless_encode_frame(pcm_min, 1U, 0x22U, pkt_b, sizeof(pkt_b), &len_b) != HT306_CODEC_OK || len_b != 32U) {
        printf("[GAGAL] Encode pkt_b min_pkt_len gagal (len=%zu)!\n", len_b);
        return 1;
    }

    /* Stream gabungan: [junk + false partial 0x5A] + pkt_a (dipecah tiap 64B) + [false sync w/ bad len containing 0x5A 0xA5] + 2 x pkt_b */
    uint8_t stream[2048];
    size_t slen = 0U;
    stream[slen++] = 0x12U;
    stream[slen++] = 0x5AU; /* false sync byte 0 followed by non-0xA5 */
    stream[slen++] = 0x99U;
    stream[slen++] = 0x5AU; /* double 0x5A before real 0xA5 */
    memcpy(stream + slen, pkt_a, len_a);
    slen += len_a;

    /* Nested 0x5A 0xA5 in packet_len field [2..3] of an aborted false sync */
    stream[slen++] = 0x5AU;
    stream[slen++] = 0xA5U;
    stream[slen++] = 0x5AU;
    stream[slen++] = 0xA5U;
    memcpy(stream + slen, pkt_b + 2U, len_b - 2U); /* completes using nested 0x5A 0xA5! */
    slen += (len_b - 2U);

    /* Normal pkt_b immediately following */
    memcpy(stream + slen, pkt_b, len_b);
    slen += len_b;

    ht306_usb_reset_reassembler();
    g_mock_usb_count = 0U;
    for (size_t off = 0U; off < slen; off += 64U) {
        uint32_t chunk = (uint32_t)((slen - off < 64U) ? (slen - off) : 64U);
        ht306_usb_process_vendor_stream(stream + off, chunk);
    }

    if (g_mock_usb_count != 3U ||
        g_mock_usb_lens[0] != len_a || memcmp(g_mock_usb_pkts[0], pkt_a, len_a) != 0 ||
        g_mock_usb_lens[1] != len_b || memcmp(g_mock_usb_pkts[1], pkt_b, len_b) != 0 ||
        g_mock_usb_lens[2] != len_b || memcmp(g_mock_usb_pkts[2], pkt_b, len_b) != 0) {
        printf("[GAGAL] USB Bulk Reassembler gagal (count=%u, lens=%u/%u/%u)\n",
               g_mock_usb_count, g_mock_usb_lens[0], g_mock_usb_lens[1], g_mock_usb_lens[2]);
        return 1;
    }

    printf("  [LULUS] USB Bulk Reassembler (chunking 64B + sync recovery + nested 0xA55A): 100%% Bit-Exact\n");
    return 0;
}

static int test_i2s_per_block_rate_tagging(void) {
    printf("\n[10/10] Menjalankan Uji Per-Block Rate Tagging (48 kHz -> 96 kHz -> 48 kHz)...\n");

    if (!ht306_i2s_engine_init(HT306_SAMPLE_RATE_48000)) {
        printf("[GAGAL] Init I2S 48 kHz gagal!\n");
        return 1;
    }

    int32_t blk[HT306_DMA_BLOCK_FRAMES * HT306_NUM_CHANNELS];
    memset(blk, 0, sizeof(blk));

    if (ht306_i2s_write_frames_6ch(blk, HT306_DMA_BLOCK_FRAMES) != HT306_DMA_BLOCK_FRAMES) {
        printf("[GAGAL] Write blok 0 (48 kHz) gagal!\n");
        return 1;
    }
    ht306_i2s_request_sample_rate(HT306_SAMPLE_RATE_96000);
    if (ht306_i2s_write_frames_6ch(blk, HT306_DMA_BLOCK_FRAMES) != HT306_DMA_BLOCK_FRAMES) {
        printf("[GAGAL] Write blok 1 (96 kHz) gagal!\n");
        return 1;
    }
    ht306_i2s_request_sample_rate(HT306_SAMPLE_RATE_48000);
    if (ht306_i2s_write_frames_6ch(blk, HT306_DMA_BLOCK_FRAMES) != HT306_DMA_BLOCK_FRAMES) {
        printf("[GAGAL] Write blok 2 (48 kHz) gagal!\n");
        return 1;
    }

    if (ht306_i2s_test_get_block_rate(0U) != HT306_SAMPLE_RATE_48000 ||
        ht306_i2s_test_get_block_rate(1U) != HT306_SAMPLE_RATE_96000 ||
        ht306_i2s_test_get_block_rate(2U) != HT306_SAMPLE_RATE_48000) {
        printf("[GAGAL] Per-block rate tag salah: b0=%lu b1=%lu b2=%lu\n",
               (unsigned long)ht306_i2s_test_get_block_rate(0U),
               (unsigned long)ht306_i2s_test_get_block_rate(1U),
               (unsigned long)ht306_i2s_test_get_block_rate(2U));
        return 1;
    }

    for (int i = 0; i < 3; ++i) {
        if (ht306_i2s_test_consume_block() == NULL) {
            printf("[GAGAL] Consume blok %d gagal!\n", i);
            return 1;
        }
    }

    printf("  [LULUS] Per-Block Rate Tagging (48k -> 96k -> 48k): Akurat Per Blok DMA\n");
    return 0;
}

static int test_adaptive_pred_and_shift_codec(void) {
    printf("\n[11/11] Menjalankan Uji Adaptive Predictor (O0/1/2/4) & Wasted-Bits/Dynamic Shift...\n");

    int32_t pcm_in[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t pcm_out[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    uint8_t packet[HT306_MAX_PACKET_BYTES];
    size_t pkt_len = 0U;
    uint8_t frames = 0U;

    /* 11A. 16-in-24 auto wasted-bits: lower 8 bits = 0 -> auto shift_code=4, 100% bit-exact */
    for (size_t n = 0; n < HT306_SUBFRAME_SAMPLES; ++n) {
        double t = (double)n / 48000.0;
        for (size_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
            double v = 0.6 * sin(2.0 * M_PI * (440.0 + ch * 190.0) * t);
            int32_t s16 = (int32_t)(v * 32767.0);
            pcm_in[n * HT306_NUM_CHANNELS + ch] = (int32_t)((uint32_t)s16 << 8U);
        }
    }
    if (ht306_lossless_encode_frame_flags(pcm_in, HT306_SUBFRAME_SAMPLES, 0x10U,
            HT306_FLAG_BITDEPTH_24, packet, sizeof(packet), &pkt_len) != HT306_CODEC_OK) {
        printf("[GAGAL] 16-in-24 encode gagal!\n");
        return 1;
    }
    if (((packet[5] & HT306_FLAG_SHIFT_MASK) >> HT306_FLAG_SHIFT_SHIFT) != 4U) {
        printf("[GAGAL] 16-in-24 tidak memicu auto shift_code=4 (flags=0x%02X)!\n", packet[5]);
        return 1;
    }
    if (ht306_lossless_decode_frame(packet, pkt_len, pcm_out, NULL, &frames) != HT306_CODEC_OK ||
        frames != HT306_SUBFRAME_SAMPLES || memcmp(pcm_in, pcm_out, sizeof(pcm_in)) != 0) {
        printf("[GAGAL] 16-in-24 round-trip tidak bit-exact!\n");
        return 1;
    }

    /* 11B. Loud wideband 48 kHz music -> 100% bit-exact with adaptive predictor */
    for (size_t n = 0; n < HT306_SUBFRAME_SAMPLES; ++n) {
        double t = (double)n / 48000.0;
        for (size_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
            double v = 0.35 * sin(2.0 * M_PI * 80.0 * t)
                     + 0.25 * sin(2.0 * M_PI * 2800.0 * t + ch * 0.4)
                     + 0.20 * sin(2.0 * M_PI * 12500.0 * t + ch * 0.9);
            int32_t s24 = (int32_t)(v * 8388607.0);
            pcm_in[n * HT306_NUM_CHANNELS + ch] = s24;
        }
    }
    if (ht306_lossless_encode_frame_flags(pcm_in, HT306_SUBFRAME_SAMPLES, 0x11U,
            HT306_FLAG_BITDEPTH_24, packet, sizeof(packet), &pkt_len) != HT306_CODEC_OK ||
        ht306_lossless_decode_frame(packet, pkt_len, pcm_out, NULL, &frames) != HT306_CODEC_OK ||
        memcmp(pcm_in, pcm_out, sizeof(pcm_in)) != 0) {
        printf("[GAGAL] Adaptive predictor 48kHz wideband tidak bit-exact!\n");
        return 1;
    }

    /* 11C. Explicit shift_code=2 (20-bit) & shift_code=4 (16-bit 2-byte verbatim white noise <= 590B) */
    for (size_t i = 0; i < HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS; ++i) {
        int32_t raw24 = (int32_t)(next_xorshift32() & 0x00FFFFFFU);
        pcm_in[i] = (raw24 ^ 0x00800000) - 0x00800000;
    }
    if (ht306_lossless_encode_frame_flags(pcm_in, HT306_SUBFRAME_SAMPLES, 0x12U,
            HT306_FLAG_RATE_96KHZ | HT306_FLAG_BITDEPTH_24 | HT306_FLAG_ENCODE_SHIFT(4U),
            packet, sizeof(packet), &pkt_len) != HT306_CODEC_OK ||
        pkt_len != (sizeof(HT306_LosslessHeader_t) + HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS * 2U) ||
        ht306_lossless_decode_frame(packet, pkt_len, pcm_out, NULL, &frames) != HT306_CODEC_OK) {
        printf("[GAGAL] 16-bit 2-byte verbatim fallback gagal (len=%zu)!\n", pkt_len);
        return 1;
    }
    for (size_t i = 0; i < HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS; ++i) {
        int32_t diff = pcm_in[i] - pcm_out[i];
        if (diff < -128 || diff > 128) {
            printf("[GAGAL] 16-bit shift rounding error terlalu besar (%d) pada idx %zu!\n", diff, i);
            return 1;
        }
    }

    printf("  [LULUS] Adaptive Predictor & Wasted-Bits/Shift Codec: 100%% Akurat\n");
    return 0;
}

int main(void) {
    printf("=== Uji Verifikasi Bit-Perfect Codec Lossless 96 kHz & I2S Engine (RP2350-Zero HT306) ===\n");

    const uint32_t total_subframes = 2000U; /* 2000 x 0.5 ms = 1.0 detik @ 96 kHz (96.000 frame x 6 ch) */
    size_t total_raw_bytes = 0;
    size_t total_packed_bytes = 0;
    size_t max_packet_size = 0;
    size_t last_packet_len = 0;

    int32_t pcm_in[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t pcm_out[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t i2s_out[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    uint8_t packet[HT306_MAX_PACKET_BYTES];

    /* --- Bagian 1: Pengujian 24-bit 96 kHz --- */
    printf("\n[1/6] Menjalankan Uji 24-bit 96 kHz 6-Channel (576.000 sampel)...\n");
    for (uint32_t sf = 0; sf < total_subframes; ++sf) {
        bool is_transient_test = (sf % 250U == 125U);
        generate_multichannel_96k_signal_24bit(
            pcm_in,
            HT306_SUBFRAME_SAMPLES,
            (size_t)sf * HT306_SUBFRAME_SAMPLES,
            is_transient_test
        );

        size_t packet_len = 0;
        HT306_CodecStatus_t enc_status = ht306_lossless_encode_frame(
            pcm_in,
            HT306_SUBFRAME_SAMPLES,
            (uint8_t)(sf & 0xFFU),
            packet,
            sizeof(packet),
            &packet_len
        );

        if (enc_status != HT306_CODEC_OK) {
            printf("[GAGAL] 24-bit Encode gagal pada subframe %u (status=%d)\n", sf, enc_status);
            return 1;
        }

        uint8_t decoded_frames = 0;
        memset(pcm_out, 0, sizeof(pcm_out));
        memset(i2s_out, 0, sizeof(i2s_out));

        HT306_CodecStatus_t dec_status = ht306_lossless_decode_frame(
            packet,
            packet_len,
            pcm_out,
            i2s_out,
            &decoded_frames
        );

        if (dec_status != HT306_CODEC_OK || decoded_frames != HT306_SUBFRAME_SAMPLES) {
            printf("[GAGAL] 24-bit Decode gagal pada subframe %u (status=%d, frames=%u)\n",
                   sf, dec_status, decoded_frames);
            return 1;
        }

        if (memcmp(pcm_in, pcm_out, sizeof(pcm_in)) != 0) {
            printf("[GAGAL] 24-bit Bit mismatch pada subframe %u!\n", sf);
            return 1;
        }

        for (size_t i = 0; i < HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS; ++i) {
            int32_t expected_i2s = (int32_t)((uint32_t)pcm_in[i] << 8U);
            if (i2s_out[i] != expected_i2s) {
                printf("[GAGAL] Format slot I2S 32-bit salah pada indeks %zu!\n", i);
                return 1;
            }
        }

        total_raw_bytes += HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS * 3U;
        total_packed_bytes += packet_len;
        last_packet_len = packet_len;
        if (packet_len > max_packet_size) {
            max_packet_size = packet_len;
        }
    }

    /* Uji Deteksi Korupsi Payload Paket (CRC-8 Guard) memakai last_packet_len yang benar */
    packet[sizeof(HT306_LosslessHeader_t) + 5U] ^= 0x40U;
    uint8_t dummy_frames = 0;
    HT306_CodecStatus_t corrupt_status = ht306_lossless_decode_frame(
        packet, last_packet_len, pcm_out, i2s_out, &dummy_frames
    );
    if (corrupt_status != HT306_CODEC_ERR_CRC_MISMATCH) {
        printf("[GAGAL] Deteksi CRC-8 tidak menangkap kerusakan payload (status=%d)!\n", corrupt_status);
        return 1;
    }

    double ratio_pct = ((double)total_packed_bytes / (double)total_raw_bytes) * 100.0;
    double kb_per_sec = (double)total_packed_bytes / 1000.0;
    printf("  [LULUS] Bit-Perfect 24-bit: 100%% Identik (0 bit error pada 576.000 sampel)\n");
    printf("  [LULUS] Uji Proteksi CRC-8 : Korupsi payload terdeteksi akurat\n");
    printf("  [INFO]  Bandwidth Mentah   : %.2f KB/s | Lossless USB: %.2f KB/s (%.1f%%) | Maks: %zu B\n",
           (double)total_raw_bytes / 1000.0, kb_per_sec, ratio_pct, max_packet_size);

    /* --- Bagian 2: Pengujian 32-bit True Studio Master 96 kHz --- */
    printf("\n[2/6] Menjalankan Uji 32-bit 96 kHz 6-Channel True Studio Master (576.000 sampel)...\n");
    size_t total_32_raw = 0;
    size_t total_32_packed = 0;
    size_t max_32_packet = 0;

    for (uint32_t sf = 0; sf < total_subframes; ++sf) {
        bool is_transient_test = (sf % 250U == 125U);
        generate_multichannel_96k_signal_32bit(
            pcm_in,
            HT306_SUBFRAME_SAMPLES,
            (size_t)sf * HT306_SUBFRAME_SAMPLES,
            is_transient_test
        );

        size_t packet_len = 0;
        HT306_CodecStatus_t enc_status = ht306_lossless_encode_frame_flags(
            pcm_in,
            HT306_SUBFRAME_SAMPLES,
            (uint8_t)(sf & 0xFFU),
            HT306_FLAG_RATE_96KHZ | HT306_FLAG_BITDEPTH_32,
            packet,
            sizeof(packet),
            &packet_len
        );

        if (enc_status != HT306_CODEC_OK) {
            printf("[GAGAL] 32-bit Encode gagal pada subframe %u (status=%d)\n", sf, enc_status);
            return 1;
        }

        uint8_t decoded_frames = 0;
        memset(i2s_out, 0, sizeof(i2s_out));

        HT306_CodecStatus_t dec_status = ht306_lossless_decode_frame(
            packet,
            packet_len,
            NULL,
            i2s_out,
            &decoded_frames
        );

        if (dec_status != HT306_CODEC_OK || decoded_frames != HT306_SUBFRAME_SAMPLES) {
            printf("[GAGAL] 32-bit Decode gagal pada subframe %u (status=%d)\n", sf, dec_status);
            return 1;
        }

        /* 32-bit: slot i2s_out harus 100% bit-exact dengan pcm_in 32-bit */
        if (memcmp(pcm_in, i2s_out, sizeof(pcm_in)) != 0) {
            printf("[GAGAL] 32-bit Bit mismatch pada subframe %u!\n", sf);
            return 1;
        }

        total_32_raw += HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS * 4U;
        total_32_packed += packet_len;
        if (packet_len > max_32_packet) {
            max_32_packet = packet_len;
        }
    }

    double ratio_32_pct = ((double)total_32_packed / (double)total_32_raw) * 100.0;
    double kb_32_per_sec = (double)total_32_packed / 1000.0;
    printf("  [LULUS] Bit-Perfect 32-bit: 100%% Identik (0 bit error pada 576.000 sampel)\n");
    printf("  [INFO]  Bandwidth Mentah   : %.2f KB/s | Lossless USB: %.2f KB/s (%.1f%%)\n",
           (double)total_32_raw / 1000.0, kb_32_per_sec, ratio_32_pct);
    printf("  [INFO]  Ukuran Maks Paket  : %zu Byte / 0.5 ms\n", max_32_packet);

    if (test_verbatim_white_noise() != 0) {
        return 1;
    }
    if (test_extreme_boundaries() != 0) {
        return 1;
    }
    if (test_header_crc_and_guards() != 0) {
        return 1;
    }
    if (test_i2s_ring_wraparound_and_mapping() != 0) {
        return 1;
    }
    if (test_crc8_lut_bitexact() != 0) {
        return 1;
    }
    if (test_i2s_lut_4lanes_bitexact() != 0) {
        return 1;
    }
    if (test_usb_reassembler_bulk_memcpy() != 0) {
        return 1;
    }
    if (test_i2s_per_block_rate_tagging() != 0) {
        return 1;
    }
    if (test_adaptive_pred_and_shift_codec() != 0) {
        return 1;
    }

    printf("\n=== SEMUA 11 SUITE PENGUJIAN CODEC, I2S ENGINE & USB REASSEMBLER LULUS 100%% BIT-PERFECT ===\n");
    return 0;
}
