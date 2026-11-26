#include "lossless_codec.h"
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(PICO_BUILD) || defined(PICO_RP2350) || defined(PICO_RP2040)
#include "pico.h"
#define HT306_RAM_FUNC(fn) __not_in_flash_func(fn)
#define HT306_LUT_ATTR     __not_in_flash("ht306_lut")
#else
#define HT306_RAM_FUNC(fn) fn
#define HT306_LUT_ATTR
#endif

#if !defined(PICO_BUILD) || defined(HT306_ENABLE_EMBEDDED_ENCODER)
/* Buffer statis di SRAM untuk ZigZag encoding (hanya dikompilasi pada PC host / unit test) */
static uint32_t s_zz_buf[HT306_MAX_SUBFRAME_SAMPLES][HT306_NUM_CHANNELS];
static uint32_t s_zz_cand[4][HT306_MAX_SUBFRAME_SAMPLES];

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    uint64_t acc;
    uint8_t  avail;
    bool     overflow;
} BitWriter_t;

static inline void bw_put(BitWriter_t *bw, uint32_t bits, uint8_t nbits) {
    if (bw->overflow || nbits == 0U) {
        return;
    }
    uint64_t mask = (nbits >= 32U) ? 0xFFFFFFFFULL : ((1ULL << nbits) - 1ULL);
    bw->acc |= ((uint64_t)(bits & mask)) << bw->avail;
    bw->avail = (uint8_t)(bw->avail + nbits);

    while (bw->avail >= 8U) {
        if (bw->pos < bw->cap) {
            bw->buf[bw->pos++] = (uint8_t)(bw->acc & 0xFFU);
            bw->acc >>= 8U;
            bw->avail = (uint8_t)(bw->avail - 8U);
        } else {
            bw->overflow = true;
            return;
        }
    }
}

static inline size_t bw_flush(BitWriter_t *bw) {
    if (bw->avail > 0U && !bw->overflow) {
        if (bw->pos < bw->cap) {
            bw->buf[bw->pos++] = (uint8_t)(bw->acc & 0xFFU);
            bw->acc = 0ULL;
            bw->avail = 0U;
        } else {
            bw->overflow = true;
        }
    }
    return bw->pos;
}
#endif

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    uint64_t       acc;
    uint8_t        avail;
    bool           underflow;
} BitReader_t;

static inline uint32_t br_get(BitReader_t *br, uint8_t nbits) {
    if (nbits == 0U) {
        return 0U;
    }
    while (br->avail < nbits) {
        if (br->pos < br->len) {
            br->acc |= ((uint64_t)br->buf[br->pos++]) << br->avail;
            br->avail = (uint8_t)(br->avail + 8U);
        } else {
            br->underflow = true;
            return 0U;
        }
    }
    uint32_t mask = (nbits >= 32U) ? 0xFFFFFFFFU : ((1U << nbits) - 1U);
    uint32_t val = (uint32_t)br->acc & mask;
    br->acc >>= nbits;
    br->avail = (uint8_t)(br->avail - nbits);
    return val;
}

// ponytail: branchless 24-bit sign extension (UB-free & portable)
static inline int32_t sext24(int32_t v) {
    int32_t u = (int32_t)((uint32_t)v & 0x00FFFFFFU);
    return (u ^ 0x00800000) - 0x00800000;
}

static inline int32_t shift_sample_24(int32_t raw, uint8_t shift_bits) {
    int32_t s = sext24(raw);
    if (shift_bits == 0U) {
        return s;
    }
    int32_t half = 1 << (shift_bits - 1U);
    int32_t max_v = (1 << (23U - shift_bits)) - 1;
    int32_t min_v = -(1 << (23U - shift_bits));
    int32_t q = (s >= 0) ? ((s + half) >> shift_bits) : -(((-s) + half) >> shift_bits);
    if (q > max_v) {
        q = max_v;
    }
    if (q < min_v) {
        q = min_v;
    }
    return q;
}

static inline uint32_t zz_enc(int32_t r) {
    uint32_t u = (uint32_t)r;
    return (u << 1U) ^ (0U - (u >> 31U));
}

static inline int32_t zz_dec(uint32_t z) {
    return (int32_t)((z >> 1U) ^ (0U - (z & 1U)));
}

// ponytail: native 32-bit modular arithmetic FLAC Order-4 predictor (UB-free & single-cycle ALU)
static inline int32_t predict_order4_32(uint8_t n, int32_t p1, int32_t p2, int32_t p3, int32_t p4) {
    uint32_t u1 = (uint32_t)p1;
    uint32_t u2 = (uint32_t)p2;
    uint32_t u3 = (uint32_t)p3;
    uint32_t u4 = (uint32_t)p4;
    uint32_t upred = (n == 1U) ? u1
                   : (n == 2U) ? (2U * u1 - u2)
                   : (n == 3U) ? (3U * u1 - 3U * u2 + u3)
                               : (4U * u1 - 6U * u2 + 4U * u3 - u4);
    return (int32_t)upred;
}

// ponytail: 24-bit signed inputs bounded to |15 * 2^23| << INT32_MAX, safely reuses 32-bit ALU
static inline int32_t predict_order4_24(uint8_t n, int32_t p1, int32_t p2, int32_t p3, int32_t p4) {
    return sext24(predict_order4_32(n, p1, p2, p3, p4));
}

static inline uint32_t predict_order4_steady_u32(int32_t p1, int32_t p2, int32_t p3, int32_t p4) {
    return (4U * (uint32_t)p1) - (6U * (uint32_t)p2) + (4U * (uint32_t)p3) - (uint32_t)p4;
}

// ponytail: single-cycle CLZ replaces while-loop log2 estimation (GCC/Clang & MSVC portable)
static inline uint8_t optimal_rice_k(uint64_t sum_zz, uint8_t count, uint8_t max_k) {
    uint32_t mean = (count > 0U) ? (uint32_t)(sum_zz / count) : 0U;
    if (mean <= 1U) {
        return 0U;
    }
    uint8_t k;
#if defined(_MSC_VER)
    unsigned long msb_idx = 0UL;
    (void)_BitScanReverse(&msb_idx, (unsigned long)mean);
    k = (uint8_t)msb_idx;
#else
    k = (uint8_t)(31U - (uint32_t)__builtin_clz(mean));
#endif
    return (k > max_k) ? max_k : k;
}

/* LUT CRC-8 polinomial 0x07 ditempatkan di SRAM (__not_in_flash) untuk eksekusi ~8-10x lebih cepat */
static const uint8_t HT306_LUT_ATTR k_crc8_lut[256] = {
    0x00U, 0x07U, 0x0EU, 0x09U, 0x1CU, 0x1BU, 0x12U, 0x15U, 0x38U, 0x3FU, 0x36U, 0x31U, 0x24U, 0x23U, 0x2AU, 0x2DU,
    0x70U, 0x77U, 0x7EU, 0x79U, 0x6CU, 0x6BU, 0x62U, 0x65U, 0x48U, 0x4FU, 0x46U, 0x41U, 0x54U, 0x53U, 0x5AU, 0x5DU,
    0xE0U, 0xE7U, 0xEEU, 0xE9U, 0xFCU, 0xFBU, 0xF2U, 0xF5U, 0xD8U, 0xDFU, 0xD6U, 0xD1U, 0xC4U, 0xC3U, 0xCAU, 0xCDU,
    0x90U, 0x97U, 0x9EU, 0x99U, 0x8CU, 0x8BU, 0x82U, 0x85U, 0xA8U, 0xAFU, 0xA6U, 0xA1U, 0xB4U, 0xB3U, 0xBAU, 0xBDU,
    0xC7U, 0xC0U, 0xC9U, 0xCEU, 0xDBU, 0xDCU, 0xD5U, 0xD2U, 0xFFU, 0xF8U, 0xF1U, 0xF6U, 0xE3U, 0xE4U, 0xEDU, 0xEAU,
    0xB7U, 0xB0U, 0xB9U, 0xBEU, 0xABU, 0xACU, 0xA5U, 0xA2U, 0x8FU, 0x88U, 0x81U, 0x86U, 0x93U, 0x94U, 0x9DU, 0x9AU,
    0x27U, 0x20U, 0x29U, 0x2EU, 0x3BU, 0x3CU, 0x35U, 0x32U, 0x1FU, 0x18U, 0x11U, 0x16U, 0x03U, 0x04U, 0x0DU, 0x0AU,
    0x57U, 0x50U, 0x59U, 0x5EU, 0x4BU, 0x4CU, 0x45U, 0x42U, 0x6FU, 0x68U, 0x61U, 0x66U, 0x73U, 0x74U, 0x7DU, 0x7AU,
    0x89U, 0x8EU, 0x87U, 0x80U, 0x95U, 0x92U, 0x9BU, 0x9CU, 0xB1U, 0xB6U, 0xBFU, 0xB8U, 0xADU, 0xAAU, 0xA3U, 0xA4U,
    0xF9U, 0xFEU, 0xF7U, 0xF0U, 0xE5U, 0xE2U, 0xEBU, 0xECU, 0xC1U, 0xC6U, 0xCFU, 0xC8U, 0xDDU, 0xDAU, 0xD3U, 0xD4U,
    0x69U, 0x6EU, 0x67U, 0x60U, 0x75U, 0x72U, 0x7BU, 0x7CU, 0x51U, 0x56U, 0x5FU, 0x58U, 0x4DU, 0x4AU, 0x43U, 0x44U,
    0x19U, 0x1EU, 0x17U, 0x10U, 0x05U, 0x02U, 0x0BU, 0x0CU, 0x21U, 0x26U, 0x2FU, 0x28U, 0x3DU, 0x3AU, 0x33U, 0x34U,
    0x4EU, 0x49U, 0x40U, 0x47U, 0x52U, 0x55U, 0x5CU, 0x5BU, 0x76U, 0x71U, 0x78U, 0x7FU, 0x6AU, 0x6DU, 0x64U, 0x63U,
    0x3EU, 0x39U, 0x30U, 0x37U, 0x22U, 0x25U, 0x2CU, 0x2BU, 0x06U, 0x01U, 0x08U, 0x0FU, 0x1AU, 0x1DU, 0x14U, 0x13U,
    0xAEU, 0xA9U, 0xA0U, 0xA7U, 0xB2U, 0xB5U, 0xBCU, 0xBBU, 0x96U, 0x91U, 0x98U, 0x9FU, 0x8AU, 0x8DU, 0x84U, 0x83U,
    0xDEU, 0xD9U, 0xD0U, 0xD7U, 0xC2U, 0xC5U, 0xCCU, 0xCBU, 0xE6U, 0xE1U, 0xE8U, 0xEFU, 0xFAU, 0xFDU, 0xF4U, 0xF3U
};

uint8_t HT306_RAM_FUNC(ht306_crc8_update)(uint8_t init, const uint8_t *data, size_t len) {
    uint8_t crc = init;
    for (size_t i = 0U; i < len; ++i) {
        crc = k_crc8_lut[crc ^ data[i]];
    }
    return crc;
}

#if !defined(PICO_BUILD) || defined(HT306_ENABLE_EMBEDDED_ENCODER)
static inline int32_t predict_by_code(
    uint8_t pred_code,
    uint8_t n,
    bool is_32bit,
    int32_t p1,
    int32_t p2,
    int32_t p3,
    int32_t p4)
{
    if (pred_code == 1U) {
        return p1;
    }
    if (pred_code == 2U) {
        if (n == 1U) {
            return p1;
        }
        uint32_t u = (2U * (uint32_t)p1) - (uint32_t)p2;
        return is_32bit ? (int32_t)u : sext24((int32_t)u);
    }
    if (pred_code == 3U) {
        return 0;
    }
    return is_32bit ? predict_order4_32(n, p1, p2, p3, p4) : predict_order4_24(n, p1, p2, p3, p4);
}

HT306_CodecStatus_t ht306_lossless_encode_frame_flags(
    const int32_t *pcm_in,
    uint8_t num_frames,
    uint8_t sequence,
    uint8_t flags,
    uint8_t *packet_out,
    size_t packet_out_cap,
    size_t *packet_len_out)
{
    const size_t hdr_sz = sizeof(HT306_LosslessHeader_t);
    const bool is_32bit = (flags & HT306_FLAG_BITDEPTH_32) != 0U;
    const size_t bytes_per_sample = is_32bit ? 4U : 3U;
    const size_t anc_sz = HT306_NUM_CHANNELS * bytes_per_sample;
    const uint8_t escape_bits = is_32bit ? HT306_ESCAPE_RAW_BITS_32 : HT306_ESCAPE_RAW_BITS;
    const uint8_t max_k = is_32bit ? 30U : 22U;

    if (!pcm_in || !packet_out || !packet_len_out || num_frames == 0U ||
        num_frames > HT306_MAX_SUBFRAME_SAMPLES ||
        (flags & ~HT306_FLAG_SUPPORTED_MASK) != 0U ||
        (is_32bit && (flags & HT306_FLAG_SHIFT_MASK) != 0U)) {
        return HT306_CODEC_ERR_INVALID_ARG;
    }
    if (packet_out_cap < (hdr_sz + anc_sz)) {
        return HT306_CODEC_ERR_BUFFER_TOO_SMALL;
    }

    uint8_t shift_code = (uint8_t)((flags & HT306_FLAG_SHIFT_MASK) >> HT306_FLAG_SHIFT_SHIFT);
    bool use_verbatim = (flags & HT306_FLAG_VERBATIM_RAW) != 0U;

    if (!is_32bit && shift_code == 0U && !use_verbatim) {
        uint32_t all_or = 0U;
        const size_t total_samples = (size_t)num_frames * HT306_NUM_CHANNELS;
        for (size_t i = 0U; i < total_samples; ++i) {
            all_or |= ((uint32_t)sext24(pcm_in[i]) & 0x00FFFFFFU);
        }
        if (all_or != 0U && (all_or & 0xFFU) == 0U) {
            shift_code = 4U; /* 16-in-24 bit-exact wasted-bits removal */
        }
    }

    const uint8_t shift_bits = (uint8_t)(shift_code * 2U);
    const size_t bytes_per_verbatim = is_32bit ? 4U : ((shift_bits >= 8U && num_frames >= 2U) ? 2U : 3U);
    const size_t raw_payload_len = (size_t)num_frames * HT306_NUM_CHANNELS * bytes_per_verbatim;

    HT306_LosslessHeader_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.sync_word  = HT306_SYNC_WORD;
    hdr.sequence   = sequence;
    hdr.flags      = (uint8_t)((flags & ~(HT306_FLAG_VERBATIM_RAW | HT306_FLAG_SHIFT_MASK)) |
                               HT306_FLAG_ENCODE_SHIFT(shift_code));
    hdr.num_frames = num_frames;

    uint8_t *payload = packet_out + hdr_sz;
    size_t total_payload = 0U;

    if (!use_verbatim) {
        for (uint8_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
            int32_t s0 = is_32bit ? pcm_in[ch] : shift_sample_24(pcm_in[ch], shift_bits);
            uint32_t u0 = (uint32_t)s0;
            if (is_32bit) {
                payload[ch * 4U + 0U] = (uint8_t)(u0 & 0xFFU);
                payload[ch * 4U + 1U] = (uint8_t)((u0 >> 8U) & 0xFFU);
                payload[ch * 4U + 2U] = (uint8_t)((u0 >> 16U) & 0xFFU);
                payload[ch * 4U + 3U] = (uint8_t)((u0 >> 24U) & 0xFFU);
            } else {
                payload[ch * 3U + 0U] = (uint8_t)(u0 & 0xFFU);
                payload[ch * 3U + 1U] = (uint8_t)((u0 >> 8U) & 0xFFU);
                payload[ch * 3U + 2U] = (uint8_t)((u0 >> 16U) & 0xFFU);
            }

            uint8_t  best_pred = 0U;
            uint8_t  best_k    = 0U;
            uint32_t best_bits = 0xFFFFFFFFU;

            for (uint8_t pred_code = 0U; pred_code < 4U; ++pred_code) {
                int32_t p1 = s0, p2 = s0, p3 = s0, p4 = s0;
                uint64_t sum_zz = 0ULL;

                for (uint8_t n = 1U; n < num_frames; ++n) {
                    int32_t raw_s = pcm_in[(size_t)n * HT306_NUM_CHANNELS + ch];
                    int32_t s = is_32bit ? raw_s : shift_sample_24(raw_s, shift_bits);
                    int32_t pred = predict_by_code(pred_code, n, is_32bit, p1, p2, p3, p4);
                    int32_t res = is_32bit ? (int32_t)((uint32_t)s - (uint32_t)pred) : sext24(s - pred);
                    uint32_t zz = zz_enc(res);
                    s_zz_cand[pred_code][n] = zz;
                    if (pred_code != 0U || n >= 3U || num_frames <= 3U) {
                        sum_zz += zz;
                    }
                    p4 = p3;
                    p3 = p2;
                    p2 = p1;
                    p1 = s;
                }

                uint8_t steady_cnt = (pred_code == 0U && num_frames > 3U)
                                   ? (uint8_t)(num_frames - 3U)
                                   : ((num_frames > 1U) ? (uint8_t)(num_frames - 1U) : 1U);
                uint8_t cand_k = optimal_rice_k(sum_zz, steady_cnt, max_k);
                uint32_t cand_bits = 0U;

                for (uint8_t n = 1U; n < num_frames; ++n) {
                    uint8_t k = (pred_code == 0U && n < 3U && cand_k <= 16U)
                              ? (uint8_t)(cand_k + 6U)
                              : cand_k;
                    uint32_t q = s_zz_cand[pred_code][n] >> k;
                    if (q < HT306_RICE_ESCAPE_QUOTIENT) {
                        cand_bits += q + 1U + k;
                    } else {
                        cand_bits += (uint32_t)(HT306_RICE_ESCAPE_QUOTIENT + 1U + escape_bits);
                    }
                }

                if (cand_bits < best_bits) {
                    best_bits = cand_bits;
                    best_pred = pred_code;
                    best_k    = cand_k;
                }
            }

            for (uint8_t n = 1U; n < num_frames; ++n) {
                s_zz_buf[n][ch] = s_zz_cand[best_pred][n];
            }
            hdr.rice_k[ch] = (uint8_t)((best_pred << HT306_PRED_ORDER_SHIFT) | (best_k & HT306_RICE_K_MASK));
        }

        size_t avail_cap = packet_out_cap - hdr_sz - anc_sz;
        size_t max_rice_payload = (raw_payload_len > anc_sz) ? (raw_payload_len - anc_sz) : 0U;
        BitWriter_t bw = {
            .buf = payload + anc_sz,
            .cap = (avail_cap < max_rice_payload) ? avail_cap : max_rice_payload,
            .pos = 0U,
            .acc = 0ULL,
            .avail = 0U,
            .overflow = false
        };
        for (uint8_t n = 1U; n < num_frames && !bw.overflow; ++n) {
            for (uint8_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
                uint8_t base_k = hdr.rice_k[ch] & HT306_RICE_K_MASK;
                uint8_t pred_code = (hdr.rice_k[ch] & HT306_PRED_ORDER_MASK) >> HT306_PRED_ORDER_SHIFT;
                uint8_t k = (pred_code == 0U && n < 3U && base_k <= 16U) ? (uint8_t)(base_k + 6U) : base_k;
                uint32_t zz = s_zz_buf[n][ch];
                uint32_t q = zz >> k;

                if (q < HT306_RICE_ESCAPE_QUOTIENT) {
                    bw_put(&bw, (1U << q) - 1U, (uint8_t)(q + 1U));
                    if (k > 0U) {
                        bw_put(&bw, zz & ((1U << k) - 1U), k);
                    }
                } else {
                    bw_put(&bw, (1U << HT306_RICE_ESCAPE_QUOTIENT) - 1U, HT306_RICE_ESCAPE_QUOTIENT + 1U);
                    bw_put(&bw, zz, escape_bits);
                }
                if (bw.overflow) {
                    break;
                }
            }
        }

        size_t compressed_payload = anc_sz + bw_flush(&bw);
        if (bw.overflow || compressed_payload > raw_payload_len) {
            use_verbatim = true;
        } else {
            total_payload = compressed_payload;
        }
    }

    if (use_verbatim) {
        if ((hdr_sz + raw_payload_len) > packet_out_cap) {
            return HT306_CODEC_ERR_BUFFER_TOO_SMALL;
        }
        hdr.flags = (uint8_t)((flags & ~HT306_FLAG_SHIFT_MASK) |
                              HT306_FLAG_ENCODE_SHIFT(shift_code) |
                              HT306_FLAG_VERBATIM_RAW);
        memset(hdr.rice_k, 0, sizeof(hdr.rice_k));

        size_t off = 0U;
        for (uint8_t n = 0U; n < num_frames; ++n) {
            const int32_t *row = &pcm_in[(size_t)n * HT306_NUM_CHANNELS];
            for (uint8_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
                uint32_t u = is_32bit ? (uint32_t)row[ch] : (uint32_t)shift_sample_24(row[ch], shift_bits);
                payload[off++] = (uint8_t)(u & 0xFFU);
                payload[off++] = (uint8_t)((u >> 8U) & 0xFFU);
                if (bytes_per_verbatim >= 3U) {
                    payload[off++] = (uint8_t)((u >> 16U) & 0xFFU);
                }
                if (bytes_per_verbatim == 4U) {
                    payload[off++] = (uint8_t)((u >> 24U) & 0xFFU);
                }
            }
        }
        total_payload = raw_payload_len;
    }

    hdr.packet_len   = (uint16_t)(hdr_sz + total_payload);
    hdr.payload_crc8 = 0U;
    memcpy(packet_out, &hdr, hdr_sz);

    /* CRC-8 melindungi header bytes [2..12] (packet_len..rice_k[5]) dilanjutkan payload */
    uint8_t crc = ht306_crc8_update(0x00U, packet_out + 2U, hdr_sz - 3U);
    crc = ht306_crc8_update(crc, payload, total_payload);
    packet_out[hdr_sz - 1U] = crc;

    *packet_len_out = hdr.packet_len;
    return HT306_CODEC_OK;
}

HT306_CodecStatus_t ht306_lossless_encode_frame(
    const int32_t *pcm_in,
    uint8_t num_frames,
    uint8_t sequence,
    uint8_t *packet_out,
    size_t packet_out_cap,
    size_t *packet_len_out)
{
    return ht306_lossless_encode_frame_flags(
        pcm_in,
        num_frames,
        sequence,
        HT306_FLAG_RATE_96KHZ | HT306_FLAG_BITDEPTH_24,
        packet_out,
        packet_out_cap,
        packet_len_out
    );
}
#endif

static inline bool decode_rice_sample(BitReader_t *br, uint8_t k, uint8_t escape_bits, uint32_t *zz_out) {
    uint32_t q = 0U;
    while (br_get(br, 1U) == 1U) {
        if (++q > HT306_RICE_ESCAPE_QUOTIENT) {
            return false;
        }
    }
    uint32_t zz = (q == HT306_RICE_ESCAPE_QUOTIENT)
                ? br_get(br, escape_bits)
                : ((q << k) | br_get(br, k));
    if (br->underflow) {
        return false;
    }
    *zz_out = zz;
    return true;
}

HT306_CodecStatus_t HT306_RAM_FUNC(ht306_lossless_decode_frame_ex)(
    const uint8_t *packet_in,
    size_t packet_len,
    int32_t *pcm24_out,
    int32_t *i2s32_msb_out,
    size_t out_frames_cap,
    uint8_t *frames_decoded_out)
{
    const size_t hdr_sz = sizeof(HT306_LosslessHeader_t);
    if (!packet_in || packet_len < hdr_sz) {
        return HT306_CODEC_ERR_INVALID_ARG;
    }

    HT306_LosslessHeader_t hdr;
    memcpy(&hdr, packet_in, hdr_sz);
    if (hdr.sync_word != HT306_SYNC_WORD) {
        return HT306_CODEC_ERR_BAD_SYNC;
    }
    if (hdr.packet_len != packet_len) {
        return HT306_CODEC_ERR_CORRUPT_STREAM;
    }

    const uint8_t *payload = packet_in + hdr_sz;
    const size_t payload_len = packet_len - hdr_sz;
    uint8_t calc_crc = ht306_crc8_update(0x00U, packet_in + 2U, hdr_sz - 3U);
    calc_crc = ht306_crc8_update(calc_crc, payload, payload_len);
    if (calc_crc != hdr.payload_crc8) {
        return HT306_CODEC_ERR_CRC_MISMATCH;
    }

    if ((hdr.flags & ~HT306_FLAG_SUPPORTED_MASK) != 0U) {
        return HT306_CODEC_ERR_CORRUPT_STREAM;
    }
    if (hdr.num_frames == 0U || hdr.num_frames > HT306_MAX_SUBFRAME_SAMPLES) {
        return HT306_CODEC_ERR_CORRUPT_STREAM;
    }
    if ((size_t)hdr.num_frames > out_frames_cap) {
        return HT306_CODEC_ERR_BUFFER_TOO_SMALL;
    }

    const bool is_32bit = (hdr.flags & HT306_FLAG_BITDEPTH_32) != 0U;
    if (is_32bit && (hdr.flags & HT306_FLAG_SHIFT_MASK) != 0U) {
        return HT306_CODEC_ERR_CORRUPT_STREAM;
    }

    const uint8_t shift_code = (uint8_t)((hdr.flags & HT306_FLAG_SHIFT_MASK) >> HT306_FLAG_SHIFT_SHIFT);
    const uint8_t shift_bits = (uint8_t)(shift_code * 2U);
    const bool is_verbatim = (hdr.flags & HT306_FLAG_VERBATIM_RAW) != 0U;
    const size_t bytes_per_sample = is_32bit ? 4U : 3U;

    if (is_verbatim) {
        const size_t bytes_per_verbatim = is_32bit ? 4U : ((shift_bits >= 8U && hdr.num_frames >= 2U) ? 2U : 3U);
        const size_t expected_payload = (size_t)hdr.num_frames * HT306_NUM_CHANNELS * bytes_per_verbatim;
        if (payload_len != expected_payload) {
            return HT306_CODEC_ERR_CORRUPT_STREAM;
        }

        size_t off = 0U;
        for (uint8_t n = 0U; n < hdr.num_frames; ++n) {
            size_t base = (size_t)n * HT306_NUM_CHANNELS;
            for (uint8_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
                int32_t s;
                if (is_32bit) {
                    uint32_t u32 = (uint32_t)payload[off]
                                 | ((uint32_t)payload[off + 1U] << 8U)
                                 | ((uint32_t)payload[off + 2U] << 16U)
                                 | ((uint32_t)payload[off + 3U] << 24U);
                    off += 4U;
                    s = (int32_t)u32;
                } else if (bytes_per_verbatim == 2U) {
                    int16_t v16 = (int16_t)(uint16_t)((uint32_t)payload[off] | ((uint32_t)payload[off + 1U] << 8U));
                    off += 2U;
                    s = sext24((int32_t)((uint32_t)(int32_t)v16 << shift_bits));
                } else {
                    uint32_t u24 = (uint32_t)payload[off]
                                 | ((uint32_t)payload[off + 1U] << 8U)
                                 | ((uint32_t)payload[off + 2U] << 16U);
                    off += 3U;
                    s = sext24((int32_t)u24);
                    if (shift_bits > 0U) {
                        s = sext24((int32_t)((uint32_t)s << shift_bits));
                    }
                }

                if (pcm24_out) {
                    pcm24_out[base + ch] = is_32bit ? (int32_t)(s >> 8) : s;
                }
                if (i2s32_msb_out) {
                    i2s32_msb_out[base + ch] = is_32bit ? s : (int32_t)((uint32_t)s << 8U);
                }
            }
        }

        if (frames_decoded_out) {
            *frames_decoded_out = hdr.num_frames;
        }
        return HT306_CODEC_OK;
    }

    const size_t anc_sz = HT306_NUM_CHANNELS * bytes_per_sample;
    const uint8_t escape_bits = is_32bit ? HT306_ESCAPE_RAW_BITS_32 : HT306_ESCAPE_RAW_BITS;
    const uint8_t max_k = is_32bit ? 30U : 22U;

    if (payload_len < anc_sz) {
        return HT306_CODEC_ERR_CORRUPT_STREAM;
    }

    uint8_t base_k[HT306_NUM_CHANNELS];
    uint8_t pred_code[HT306_NUM_CHANNELS];
    for (uint8_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
        if ((hdr.rice_k[ch] & 0x80U) != 0U) {
            return HT306_CODEC_ERR_CORRUPT_STREAM;
        }
        base_k[ch] = hdr.rice_k[ch] & HT306_RICE_K_MASK;
        pred_code[ch] = (hdr.rice_k[ch] & HT306_PRED_ORDER_MASK) >> HT306_PRED_ORDER_SHIFT;
        if (base_k[ch] > max_k) {
            return HT306_CODEC_ERR_CORRUPT_STREAM;
        }
    }

    int32_t p1[HT306_NUM_CHANNELS], p2[HT306_NUM_CHANNELS], p3[HT306_NUM_CHANNELS], p4[HT306_NUM_CHANNELS];
    for (uint8_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
        int32_t s0;
        if (is_32bit) {
            uint32_t u32 = (uint32_t)payload[ch * 4U]
                         | ((uint32_t)payload[ch * 4U + 1U] << 8U)
                         | ((uint32_t)payload[ch * 4U + 2U] << 16U)
                         | ((uint32_t)payload[ch * 4U + 3U] << 24U);
            s0 = (int32_t)u32;
        } else {
            uint32_t u24 = (uint32_t)payload[ch * 3U]
                         | ((uint32_t)payload[ch * 3U + 1U] << 8U)
                         | ((uint32_t)payload[ch * 3U + 2U] << 16U);
            s0 = sext24((int32_t)u24);
        }
        p1[ch] = p2[ch] = p3[ch] = p4[ch] = s0;
        int32_t s_out = (!is_32bit && shift_bits > 0U) ? sext24((int32_t)((uint32_t)s0 << shift_bits)) : s0;
        if (pcm24_out) {
            pcm24_out[ch] = is_32bit ? (int32_t)(s_out >> 8) : s_out;
        }
        if (i2s32_msb_out) {
            i2s32_msb_out[ch] = is_32bit ? s_out : (int32_t)((uint32_t)s_out << 8U);
        }
    }

    BitReader_t br = {
        .buf = payload + anc_sz,
        .len = payload_len - anc_sz,
        .pos = 0U,
        .acc = 0ULL,
        .avail = 0U,
        .underflow = false
    };
    const uint8_t warmup_end = (hdr.num_frames < 4U) ? hdr.num_frames : 4U;
    for (uint8_t n = 1U; n < warmup_end; ++n) {
        size_t base = (size_t)n * HT306_NUM_CHANNELS;
        for (uint8_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
            uint8_t k = (pred_code[ch] == 0U && n < 3U && base_k[ch] <= 16U)
                      ? (uint8_t)(base_k[ch] + 6U)
                      : base_k[ch];
            uint32_t zz = 0U;
            if (!decode_rice_sample(&br, k, escape_bits, &zz)) {
                return HT306_CODEC_ERR_CORRUPT_STREAM;
            }

            uint32_t upred;
            if (pred_code[ch] == 1U) {
                upred = (uint32_t)p1[ch];
            } else if (pred_code[ch] == 2U) {
                upred = (n == 1U) ? (uint32_t)p1[ch] : ((2U * (uint32_t)p1[ch]) - (uint32_t)p2[ch]);
            } else if (pred_code[ch] == 3U) {
                upred = 0U;
            } else {
                upred = (uint32_t)predict_order4_32(n, p1[ch], p2[ch], p3[ch], p4[ch]);
            }

            uint32_t sum = upred + (uint32_t)zz_dec(zz);
            int32_t s = is_32bit ? (int32_t)sum : sext24((int32_t)sum);
            p4[ch] = p3[ch];
            p3[ch] = p2[ch];
            p2[ch] = p1[ch];
            p1[ch] = s;

            int32_t s_out = (!is_32bit && shift_bits > 0U) ? sext24((int32_t)((uint32_t)s << shift_bits)) : s;
            if (pcm24_out) {
                pcm24_out[base + ch] = is_32bit ? (int32_t)(s_out >> 8) : s_out;
            }
            if (i2s32_msb_out) {
                i2s32_msb_out[base + ch] = is_32bit ? s_out : (int32_t)((uint32_t)s_out << 8U);
            }
        }
    }

    for (uint8_t n = 4U; n < hdr.num_frames; ++n) {
        size_t base = (size_t)n * HT306_NUM_CHANNELS;
        for (uint8_t ch = 0; ch < HT306_NUM_CHANNELS; ++ch) {
            uint32_t zz = 0U;
            if (!decode_rice_sample(&br, base_k[ch], escape_bits, &zz)) {
                return HT306_CODEC_ERR_CORRUPT_STREAM;
            }

            uint32_t upred;
            if (pred_code[ch] == 1U) {
                upred = (uint32_t)p1[ch];
            } else if (pred_code[ch] == 2U) {
                upred = (2U * (uint32_t)p1[ch]) - (uint32_t)p2[ch];
            } else if (pred_code[ch] == 3U) {
                upred = 0U;
            } else {
                upred = predict_order4_steady_u32(p1[ch], p2[ch], p3[ch], p4[ch]);
            }

            uint32_t sum = upred + (uint32_t)zz_dec(zz);
            int32_t s = is_32bit ? (int32_t)sum : sext24((int32_t)sum);
            p4[ch] = p3[ch];
            p3[ch] = p2[ch];
            p2[ch] = p1[ch];
            p1[ch] = s;

            int32_t s_out = (!is_32bit && shift_bits > 0U) ? sext24((int32_t)((uint32_t)s << shift_bits)) : s;
            if (pcm24_out) {
                pcm24_out[base + ch] = is_32bit ? (int32_t)(s_out >> 8) : s_out;
            }
            if (i2s32_msb_out) {
                i2s32_msb_out[base + ch] = is_32bit ? s_out : (int32_t)((uint32_t)s_out << 8U);
            }
        }
    }

    if (frames_decoded_out) {
        *frames_decoded_out = hdr.num_frames;
    }
    return HT306_CODEC_OK;
}

HT306_CodecStatus_t HT306_RAM_FUNC(ht306_lossless_decode_frame)(
    const uint8_t *packet_in,
    size_t packet_len,
    int32_t *pcm24_out,
    int32_t *i2s32_msb_out,
    uint8_t *frames_decoded_out)
{
    return ht306_lossless_decode_frame_ex(
        packet_in,
        packet_len,
        pcm24_out,
        i2s32_msb_out,
        HT306_MAX_SUBFRAME_SAMPLES,
        frames_decoded_out
    );
}
