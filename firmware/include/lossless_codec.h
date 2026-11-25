#ifndef HT306_LOSSLESS_CODEC_H
#define HT306_LOSSLESS_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Konfigurasi Protokol Audio Lossless 96 kHz (LG HT306 - Waveshare RP2350-Zero)
 */
#define HT306_SYNC_WORD               0xA55AU
#define HT306_NUM_CHANNELS            6U      /* FL, FR, C, LFE, SL, SR (5.1) */
#define HT306_MAX_CHANNELS            8U      /* Ekspansi 7.1 / Atmos */
#define HT306_SUBFRAME_SAMPLES        48U     /* 0.5 ms pada 96 kHz */
#define HT306_MAX_SUBFRAME_SAMPLES    96U     /* 1.0 ms pada 96 kHz */
#define HT306_RICE_ESCAPE_QUOTIENT    15U     /* Batas kuosien unary sebelum escape raw bits */
#define HT306_ESCAPE_RAW_BITS         25U     /* Lebar bit zigzag penuh untuk 24-bit residual */
#define HT306_ESCAPE_RAW_BITS_32      32U     /* Lebar bit zigzag penuh untuk 32-bit residual */
#define HT306_MAX_PACKET_BYTES        1200U   /* Batas aman USB Bulk (muat 32-bit verbatim 1166B & 24-bit verbatim 878B) */

/**
 * Kode status hasil operasi codec
 */
typedef enum {
    HT306_CODEC_OK = 0,
    HT306_CODEC_ERR_INVALID_ARG = -1,
    HT306_CODEC_ERR_BAD_SYNC = -2,
    HT306_CODEC_ERR_CRC_MISMATCH = -3,
    HT306_CODEC_ERR_BUFFER_TOO_SMALL = -4,
    HT306_CODEC_ERR_CORRUPT_STREAM = -5
} HT306_CodecStatus_t;

/**
 * Flag konfigurasi di dalam header paket USB Lossless
 */
#define HT306_FLAG_RATE_96KHZ         (1U << 0)
#define HT306_FLAG_BITDEPTH_24        (1U << 1)
#define HT306_FLAG_CHANNELS_8         (1U << 2)
#define HT306_FLAG_BITDEPTH_32        (1U << 3)  /* Mode 32-bit Studio Master */
#define HT306_FLAG_VERBATIM_RAW       (1U << 4)  /* Mode payload PCM mentah anti-overflow */
#define HT306_FLAG_SUPPORTED_MASK     (HT306_FLAG_RATE_96KHZ | HT306_FLAG_BITDEPTH_24 | \
                                       HT306_FLAG_BITDEPTH_32 | HT306_FLAG_VERBATIM_RAW)

/**
 * Struktur Header Paket USB Lossless (14 Byte)
 */
#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct {
#else
typedef struct __attribute__((packed)) {
#endif
    uint16_t sync_word;                       /* Offset 0..1 : Wajib bernilai HT306_SYNC_WORD (0xA55A) */
    uint16_t packet_len;                      /* Offset 2..3 : Panjang total paket (header + payload) dalam byte */
    uint8_t  sequence;                        /* Offset 4    : Nomor urut paket (0..255) untuk deteksi drop */
    uint8_t  flags;                           /* Offset 5    : Kombinasi HT306_FLAG_* */
    uint8_t  num_frames;                      /* Offset 6    : Jumlah sampel per channel dalam paket (default: 48) */
    uint8_t  rice_k[HT306_NUM_CHANNELS];      /* Offset 7..12: Parameter Golomb-Rice k (0..30) per channel */
    uint8_t  payload_crc8;                    /* Offset 13   : CRC-8 (poly 0x07) atas header[2..12] + seluruh payload */
#if defined(_MSC_VER)
} HT306_LosslessHeader_t;
#pragma pack(pop)
#else
} HT306_LosslessHeader_t;
#endif

#if defined(__cplusplus)
static_assert(sizeof(HT306_LosslessHeader_t) == 14U, "HT306_LosslessHeader_t must be 14 bytes");
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(HT306_LosslessHeader_t) == 14U, "HT306_LosslessHeader_t must be 14 bytes");
#endif

#if !defined(PICO_BUILD) || defined(HT306_ENABLE_EMBEDDED_ENCODER)
/**
 * Mengemas (Encode) array PCM 24-bit/32-bit interleaved (6 channel) dengan flag kustom.
 */
HT306_CodecStatus_t ht306_lossless_encode_frame_flags(
    const int32_t *pcm_in,
    uint8_t num_frames,
    uint8_t sequence,
    uint8_t flags,
    uint8_t *packet_out,
    size_t packet_out_cap,
    size_t *packet_len_out
);

/**
 * Mengemas (Encode) array PCM 24-bit interleaved (6 channel) menjadi paket USB Lossless Delta-Rice.
 * Wrapper default untuk mode 24-bit 96 kHz.
 */
HT306_CodecStatus_t ht306_lossless_encode_frame(
    const int32_t *pcm_in,
    uint8_t num_frames,
    uint8_t sequence,
    uint8_t *packet_out,
    size_t packet_out_cap,
    size_t *packet_len_out
);
#endif

/**
 * Membuka (Decode) paket USB Lossless Delta-Rice atau Verbatim Raw dengan validasi kapasitas output.
 *
 * @param packet_in           Buffer paket USB yang diterima.
 * @param packet_len          Panjang byte paket yang diterima.
 * @param pcm24_out           Output sampel 24-bit signed (int32_t) untuk verifikasi bit-exact (boleh NULL).
 * @param i2s32_msb_out       Output sampel 32-bit MSB-aligned / native 32-bit siap kirim ke DMA PIO (boleh NULL).
 * @param out_frames_cap      Kapasitas maksimal frame (masing-masing 6 channel) pada buffer output.
 * @param frames_decoded_out  Output jumlah frame per channel yang berhasil didekode.
 * @return                    HT306_CODEC_OK jika paket valid dan berhasil dibuka.
 */
HT306_CodecStatus_t ht306_lossless_decode_frame_ex(
    const uint8_t *packet_in,
    size_t packet_len,
    int32_t *pcm24_out,
    int32_t *i2s32_msb_out,
    size_t out_frames_cap,
    uint8_t *frames_decoded_out
);

/**
 * Wrapper kompatibilitas ht306_lossless_decode_frame_ex() dengan kapasitas default HT306_MAX_SUBFRAME_SAMPLES.
 */
HT306_CodecStatus_t ht306_lossless_decode_frame(
    const uint8_t *packet_in,
    size_t packet_len,
    int32_t *pcm24_out,
    int32_t *i2s32_msb_out,
    uint8_t *frames_decoded_out
);

/**
 * Memperbarui checksum CRC-8 (Polinomial 0x07) secara inkremental dari nilai awal `init`.
 */
uint8_t ht306_crc8_update(uint8_t init, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HT306_LOSSLESS_CODEC_H */
