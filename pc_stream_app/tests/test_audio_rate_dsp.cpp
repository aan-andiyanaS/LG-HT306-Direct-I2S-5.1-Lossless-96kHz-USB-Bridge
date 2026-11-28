#include "audio_rate_dsp.h"
#include "lossless_codec.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using namespace ht306::audio;

static int g_failures = 0;

#define EXPECT_TRUE(cond, msg)                                                 \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static double MeasureToneGainDb(
    const std::vector<float>& interleaved_6ch,
    uint32_t sample_rate,
    double freq_hz,
    size_t skip_frames
) {
    const size_t total_frames = interleaved_6ch.size() / 6u;
    if (total_frames <= skip_frames + 64u) {
        return -200.0;
    }
    double cos_sum = 0.0;
    double sin_sum = 0.0;
    const size_t count = total_frames - skip_frames;
    const double two_pi = 2.0 * 3.14159265358979323846;
    for (size_t i = skip_frames; i < total_frames; ++i) {
        double phase = two_pi * freq_hz * (static_cast<double>(i) / static_cast<double>(sample_rate));
        double sample = static_cast<double>(interleaved_6ch[i * 6u]);
        cos_sum += sample * std::cos(phase);
        sin_sum += sample * std::sin(phase);
    }
    double mag = (2.0 / static_cast<double>(count)) * std::sqrt(cos_sum * cos_sum + sin_sum * sin_sum);
    if (mag < 1e-10) {
        return -200.0;
    }
    return 20.0 * std::log10(mag);
}

static void TestResolveTargetSampleRate() {
    auto r0 = ResolveTargetSampleRate(SampleRateMode::AUTO, 0u);
    EXPECT_TRUE(!r0.valid_input && r0.target_rate == 96000u && !r0.resampled, "rate 0 invalid");

    auto r_bad = ResolveTargetSampleRate(SampleRateMode::AUTO, 999999u);
    EXPECT_TRUE(!r_bad.valid_input, "rate 999999 invalid");

    auto r44 = ResolveTargetSampleRate(SampleRateMode::AUTO, 44100u);
    EXPECT_TRUE(r44.valid_input && r44.target_rate == 48000u && !r44.clamped_from_high && r44.resampled, "auto 44.1k -> 48k");

    auto r48 = ResolveTargetSampleRate(SampleRateMode::AUTO, 48000u);
    EXPECT_TRUE(r48.valid_input && r48.target_rate == 48000u && !r48.clamped_from_high && !r48.resampled, "auto 48k native");

    auto r88 = ResolveTargetSampleRate(SampleRateMode::AUTO, 88200u);
    EXPECT_TRUE(r88.valid_input && r88.target_rate == 96000u && !r88.clamped_from_high && r88.resampled, "auto 88.2k -> 96k");

    auto r96 = ResolveTargetSampleRate(SampleRateMode::AUTO, 96000u);
    EXPECT_TRUE(r96.valid_input && r96.target_rate == 96000u && !r96.clamped_from_high && !r96.resampled, "auto 96k native");

    auto r176 = ResolveTargetSampleRate(SampleRateMode::AUTO, 176400u);
    EXPECT_TRUE(r176.valid_input && r176.target_rate == 96000u && r176.clamped_from_high && r176.resampled, "auto 176.4k -> 96k clamped");

    auto r192 = ResolveTargetSampleRate(SampleRateMode::AUTO, 192000u);
    EXPECT_TRUE(r192.valid_input && r192.target_rate == 96000u && r192.clamped_from_high && r192.resampled, "auto 192k -> 96k clamped");

    auto l48 = ResolveTargetSampleRate(SampleRateMode::LOCK_48K, 96000u);
    EXPECT_TRUE(l48.valid_input && l48.target_rate == 48000u && l48.clamped_from_high && l48.resampled, "lock48k 96k -> 48k");

    auto l96 = ResolveTargetSampleRate(SampleRateMode::LOCK_96K, 48000u);
    EXPECT_TRUE(l96.valid_input && l96.target_rate == 96000u && !l96.clamped_from_high && l96.resampled, "lock96k 48k -> 96k");

    std::printf("[PASS] TestResolveTargetSampleRate\n");
}

static void TestFloatToInt32Safe() {
    int32_t max_pos = FloatToInt32Safe(1.0f);
    int32_t over_pos = FloatToInt32Safe(1.5f);
    int32_t min_neg = FloatToInt32Safe(-1.0f);
    int32_t over_neg = FloatToInt32Safe(-1.5f);
    int32_t zero_val = FloatToInt32Safe(0.0f);
    int32_t nan_val = FloatToInt32Safe(0.0f / 0.0f);

    EXPECT_TRUE(max_pos == 2147483647, "+1.0f must convert to INT32_MAX without wrap-around");
    EXPECT_TRUE(over_pos == 2147483647, "+1.5f must clamp cleanly to INT32_MAX");
    EXPECT_TRUE(min_neg == -2147483647 || min_neg == INT32_MIN, "-1.0f valid negative full scale");
    EXPECT_TRUE(over_neg == min_neg, "-1.5f clamped cleanly");
    EXPECT_TRUE(zero_val == 0, "0.0f -> 0");
    EXPECT_TRUE(nan_val == 0, "NaN -> 0");

    std::printf("[PASS] TestFloatToInt32Safe\n");
}

static void TestDecodeInterleavedFormats() {
    float out[6] = {};

    // 1. IEEE Float 2ch
    float f2[2] = { 0.5f, -0.25f };
    AudioStreamFormat fmt_f{ SampleFormat::IEEE_FLOAT32, 2, 96000, 32 };
    EXPECT_TRUE(DecodeInterleavedFrameToFloat6(reinterpret_cast<const uint8_t*>(f2), 0, fmt_f, out), "float decode");
    EXPECT_TRUE(std::fabs(out[0] - 0.5f) < 1e-6f && std::fabs(out[1] + 0.25f) < 1e-6f && out[2] == 0.0f, "float values");

    // 2. PCM 16-bit
    int16_t p16[2] = { 16384, -32768 };
    AudioStreamFormat fmt_16{ SampleFormat::PCM16, 2, 48000, 16 };
    EXPECT_TRUE(DecodeInterleavedFrameToFloat6(reinterpret_cast<const uint8_t*>(p16), 0, fmt_16, out), "pcm16 decode");
    EXPECT_TRUE(std::fabs(out[0] - 0.5f) < 1e-4f && std::fabs(out[1] + 1.0f) < 1e-4f, "pcm16 values");

    // 3. PCM 24-bit packed (3 bytes per sample)
    uint8_t p24[6] = {
        0xFF, 0xFF, 0x7F, // +max (8388607)
        0x00, 0x00, 0x80  // -1.0 (-8388608)
    };
    AudioStreamFormat fmt_24{ SampleFormat::PCM24_PACKED, 2, 96000, 24 };
    EXPECT_TRUE(DecodeInterleavedFrameToFloat6(p24, 0, fmt_24, out), "pcm24 decode");
    EXPECT_TRUE(out[0] > 0.9999f && std::fabs(out[1] + 1.0f) < 1e-6f, "pcm24 values");

    // 4. PCM 24-in-32 (4-byte container -> SampleFormat::PCM32)
    int32_t p32[2] = { 1073741824, INT32_MIN };
    AudioStreamFormat fmt_32{ SampleFormat::PCM32, 2, 96000, 24 };
    EXPECT_TRUE(DecodeInterleavedFrameToFloat6(reinterpret_cast<const uint8_t*>(p32), 0, fmt_32, out), "pcm32 decode");
    EXPECT_TRUE(std::fabs(out[0] - 0.5f) < 1e-6f && std::fabs(out[1] + 1.0f) < 1e-6f, "pcm32 values");

    // 5. Unsupported
    AudioStreamFormat fmt_bad{ SampleFormat::UNSUPPORTED, 2, 48000, 16 };
    EXPECT_TRUE(!DecodeInterleavedFrameToFloat6(p24, 0, fmt_bad, out), "unsupported must return false");

    std::printf("[PASS] TestDecodeInterleavedFormats\n");
}

static void RunResampleRatioCheck(uint32_t in_rate, uint32_t out_rate, double pass_freq, double alias_freq) {
    RateResampler6Ch rs;
    rs.Configure(in_rate, out_rate);

    const size_t chunk = in_rate / 100u; // 10 ms chunks for 1 full second
    std::vector<float> in_chunk(chunk * 6u, 0.0f);
    std::vector<float> out_chunk;
    std::vector<float> all_out;
    all_out.reserve(out_rate * 6u);

    const double two_pi = 2.0 * 3.14159265358979323846;
    size_t sample_idx = 0;

    for (int step = 0; step < 100; ++step) {
        for (size_t i = 0; i < chunk; ++i) {
            double t = static_cast<double>(sample_idx++) / static_cast<double>(in_rate);
            float sig = static_cast<float>(0.5 * std::sin(two_pi * pass_freq * t));
            if (alias_freq > 0.0) {
                sig += static_cast<float>(0.5 * std::sin(two_pi * alias_freq * t));
            }
            for (int c = 0; c < 6; ++c) {
                in_chunk[i * 6u + c] = sig;
            }
        }
        rs.Process(in_chunk.data(), chunk, out_chunk);
        all_out.insert(all_out.end(), out_chunk.begin(), out_chunk.end());
    }

    const size_t total_out_frames = all_out.size() / 6u;
    int64_t diff = static_cast<int64_t>(total_out_frames) - static_cast<int64_t>(out_rate);
    EXPECT_TRUE(std::llabs(diff) <= 32, "1s resampled frame count within tolerance");

    double gain_pass = MeasureToneGainDb(all_out, out_rate, pass_freq, out_rate / 10u);
    double expected_db = -6.02; // amplitude 0.5
    EXPECT_TRUE(gain_pass > expected_db - 1.5 && gain_pass < expected_db + 1.0, "passband 1kHz preserved");

    if (alias_freq > 0.0) {
        double folded = std::fabs(std::fmod(alias_freq, static_cast<double>(out_rate)));
        if (folded > static_cast<double>(out_rate) * 0.5) {
            folded = static_cast<double>(out_rate) - folded;
        }
        double alias_db = MeasureToneGainDb(all_out, out_rate, folded, out_rate / 10u);
        EXPECT_TRUE(alias_db < -40.0, "ultrasonic alias suppressed >40dB");
    }
}

static void TestResampler() {
    // 1. Bit-exact bypass check
    {
        RateResampler6Ch rs;
        rs.Configure(96000u, 96000u);
        EXPECT_TRUE(rs.IsBypass(), "96k->96k is bypass");
        std::vector<float> in(48 * 6, 0.1234567f);
        in[7] = -0.9876543f;
        std::vector<float> out;
        size_t n = rs.Process(in.data(), 48, out);
        EXPECT_TRUE(n == 48 && std::memcmp(in.data(), out.data(), in.size() * sizeof(float)) == 0, "bitwise exact bypass");
    }

    // 2. Ratios & anti-aliasing
    RunResampleRatioCheck(192000u, 96000u, 1000.0, 70000.0);
    RunResampleRatioCheck(192000u, 48000u, 1000.0, 35000.0);
    RunResampleRatioCheck(176400u, 96000u, 1000.0, 70000.0);
    RunResampleRatioCheck(96000u, 48000u, 1000.0, 35000.0);
    RunResampleRatioCheck(44100u, 48000u, 1000.0, 0.0);
    RunResampleRatioCheck(48000u, 96000u, 1000.0, 0.0);

    // 3. Guard input_rate == 0 and nullptr flush
    {
        RateResampler6Ch rs;
        rs.Configure(0u, 48000u);
        std::vector<float> in(48 * 6, 0.5f), out;
        EXPECT_TRUE(rs.Process(in.data(), 48, out) == 0, "invalid rate safely produces 0 frames");

        rs.Configure(192000u, 96000u);
        EXPECT_TRUE(rs.Process(nullptr, 96, out) > 0, "nullptr zero flush succeeds");
    }

    std::printf("[PASS] TestResampler\n");
}

static void TestFloatToInt24Safe() {
    int32_t max_pos = FloatToInt24Safe(1.0f);
    int32_t over_pos = FloatToInt24Safe(1.5f);
    int32_t min_neg = FloatToInt24Safe(-1.0f);
    int32_t over_neg = FloatToInt24Safe(-1.5f);
    int32_t zero_val = FloatToInt24Safe(0.0f);
    int32_t nan_val = FloatToInt24Safe(std::numeric_limits<float>::quiet_NaN());
    int32_t inf_val = FloatToInt24Safe(std::numeric_limits<float>::infinity());
    int32_t ninf_val = FloatToInt24Safe(-std::numeric_limits<float>::infinity());

    EXPECT_TRUE(max_pos == 8388607, "+1.0f must convert to +8388607");
    EXPECT_TRUE(over_pos == 8388607, "+1.5f must clamp cleanly to +8388607");
    EXPECT_TRUE(min_neg == -8388607 || min_neg == -8388608, "-1.0f valid 24-bit negative full scale");
    EXPECT_TRUE(over_neg == min_neg, "-1.5f must clamp cleanly");
    EXPECT_TRUE(zero_val == 0, "0.0f -> 0");
    EXPECT_TRUE(nan_val == 0, "NaN -> 0");
    EXPECT_TRUE(inf_val == 0 && ninf_val == 0, "+/-Inf -> 0");

    std::printf("[PASS] TestFloatToInt24Safe\n");
}

static void TestDecode8ChAndDownmix() {
    // 1. 8-ch (7.1: FL, FR, FC, LFE, BL, BR, SL, SR)
    float src71[8] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f };
    float dec8[8] = {};
    AudioStreamFormat fmt71{ SampleFormat::IEEE_FLOAT32, 8, 96000, 32, 0x63Fu };
    EXPECT_TRUE(DecodeInterleavedFrameToFloat8(reinterpret_cast<const uint8_t*>(src71), 0, fmt71, dec8), "8ch float decode");
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(std::fabs(dec8[i] - src71[i]) < 1e-6f, "8ch channel value preserved");
    }

    float out6[6] = {};
    const float atten_db = -3.0f;
    const float downmix_lin = std::pow(10.0f, atten_db / 20.0f);
    NormalizeChannelsTo6ChEx(dec8, out6, 8, 0x63Fu, atten_db, ChannelRoutingMode::DISCRETE_1TO1, false);
    EXPECT_TRUE(std::fabs(out6[0] - 0.1f) < 1e-6f, "7.1 FL preserved");
    EXPECT_TRUE(std::fabs(out6[1] - 0.2f) < 1e-6f, "7.1 FR preserved");
    EXPECT_TRUE(std::fabs(out6[2] - 0.3f) < 1e-6f, "7.1 FC preserved");
    EXPECT_TRUE(std::fabs(out6[3] - 0.4f) < 1e-6f, "7.1 LFE preserved");
    EXPECT_TRUE(std::fabs(out6[4] - (0.7f + 0.5f) * downmix_lin) < 1e-5f, "7.1 BL+SL folded into SL");
    EXPECT_TRUE(std::fabs(out6[5] - (0.8f + 0.6f) * downmix_lin) < 1e-5f, "7.1 BR+SR folded into SR");

    // Verify SL/SR-only signal is NOT dropped
    float sl_only[8] = { 0, 0, 0, 0, 0, 0, 0.5f, -0.5f };
    NormalizeChannelsTo6ChEx(sl_only, out6, 8, 0x63Fu, atten_db, ChannelRoutingMode::DISCRETE_1TO1, true);
    EXPECT_TRUE(std::fabs(out6[4] - 0.5f * downmix_lin) < 1e-5f, "7.1 SL-only not dropped");
    EXPECT_TRUE(std::fabs(out6[5] + 0.5f * downmix_lin) < 1e-5f, "7.1 SR-only not dropped");

    // 2. 4-ch Quad (FL, FR, BL, BR -> BL->SL, BR->SR, C=0, LFE=0)
    float quad[8] = { 0.25f, -0.25f, 0.6f, -0.7f, 0, 0, 0, 0 };
    NormalizeChannelsTo6ChEx(quad, out6, 4, 0x33u, atten_db, ChannelRoutingMode::DISCRETE_1TO1, false);
    EXPECT_TRUE(std::fabs(out6[0] - 0.25f) < 1e-6f, "Quad FL");
    EXPECT_TRUE(std::fabs(out6[1] + 0.25f) < 1e-6f, "Quad FR");
    EXPECT_TRUE(out6[2] == 0.0f && out6[3] == 0.0f, "Quad C and LFE must be 0");
    EXPECT_TRUE(std::fabs(out6[4] - 0.6f) < 1e-6f, "Quad BL mapped to SL");
    EXPECT_TRUE(std::fabs(out6[5] + 0.7f) < 1e-6f, "Quad BR mapped to SR");

    // 4-ch with channel_mask == 0 defaults to Quad as well
    NormalizeChannelsTo6ChEx(quad, out6, 4, 0u, atten_db, ChannelRoutingMode::DISCRETE_1TO1, false);
    EXPECT_TRUE(out6[2] == 0.0f && out6[3] == 0.0f && std::fabs(out6[4] - 0.6f) < 1e-6f && std::fabs(out6[5] + 0.7f) < 1e-6f,
                "4ch mask=0 defaults to Quad mapping");

    // 4-ch explicit 3.1 (FL, FR, FC, LFE, mask 0x0F)
    NormalizeChannelsTo6ChEx(quad, out6, 4, 0x0Fu, atten_db, ChannelRoutingMode::DISCRETE_1TO1, false);
    EXPECT_TRUE(std::fabs(out6[2] - 0.6f) < 1e-6f && std::fabs(out6[3] + 0.7f) < 1e-6f && out6[4] == 0.0f && out6[5] == 0.0f,
                "4ch mask=0x0F maps to 3.1");

    std::printf("[PASS] TestDecode8ChAndDownmix\n");
}

static void TestPolyphasePerformance() {
    RateResampler6Ch rs;
    rs.Configure(192000u, 96000u);

    const size_t chunk = 1920u; // 10 ms at 192 kHz
    std::vector<float> in_chunk(chunk * 6u, 0.25f);
    std::vector<float> out_chunk;

    auto t0 = std::chrono::steady_clock::now();
    size_t total_out = 0;
    for (int i = 0; i < 100; ++i) {
        total_out += rs.Process(in_chunk.data(), chunk, out_chunk);
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    EXPECT_TRUE(total_out >= 95900u && total_out <= 96100u, "192k->96k perf frame count");
    EXPECT_TRUE(elapsed_ms < 250.0, "Polyphase LUT 1s 192k->96k 6ch must finish well below real-time (<250 ms)");
    std::printf("[PASS] TestPolyphasePerformance (1s 192k->96k 6ch in %.2f ms)\n", elapsed_ms);
}

static void TestEncode24BitWhiteNoiseVerbatim() {
    int32_t pcm_in[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    int32_t pcm_dec[HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS];
    uint8_t packet[HT306_MAX_PACKET_BYTES];

    uint32_t seed = 0x12345678u;
    for (size_t i = 0; i < HT306_SUBFRAME_SAMPLES * HT306_NUM_CHANNELS; ++i) {
        seed = seed * 1664525u + 1013904223u;
        float f = (static_cast<float>(seed & 0x7FFFFFFFu) / 1073741824.0f) - 1.0f;
        pcm_in[i] = FloatToInt24Safe(f);
    }

    size_t enc_len = 0;
    uint8_t flags = HT306_FLAG_BITDEPTH_24 | HT306_FLAG_RATE_96KHZ;
    HT306_CodecStatus_t st = ht306_lossless_encode_frame_flags(
        pcm_in,
        HT306_SUBFRAME_SAMPLES,
        42,
        flags,
        packet,
        sizeof(packet),
        &enc_len
    );

    EXPECT_TRUE(st == HT306_CODEC_OK, "24-bit white noise encode must succeed");
    EXPECT_TRUE(enc_len <= 878u, "24-bit white noise packet must be <= 878 bytes");
    EXPECT_TRUE((packet[5] & HT306_FLAG_VERBATIM_RAW) != 0u, "24-bit white noise must auto-trigger VERBATIM_RAW");

    uint8_t decoded_frames = 0;
    HT306_CodecStatus_t dec_st = ht306_lossless_decode_frame_ex(
        packet,
        enc_len,
        pcm_dec,
        nullptr,
        HT306_SUBFRAME_SAMPLES,
        &decoded_frames
    );
    EXPECT_TRUE(dec_st == HT306_CODEC_OK && decoded_frames == HT306_SUBFRAME_SAMPLES, "24-bit verbatim decode must succeed");
    EXPECT_TRUE(std::memcmp(pcm_in, pcm_dec, sizeof(pcm_in)) == 0, "24-bit white noise decode must be 100% bit-exact");

    std::printf("[PASS] TestEncode24BitWhiteNoiseVerbatim (enc_len=%zu B, flags=0x%02X)\n", enc_len, packet[5]);
}

static void TestHoistedDecode8ChAndDownmixLinear() {
    // Verify all 4 hoisted formats on full 8 channels
    {
        float src_f[8] = { -1.0f, -0.5f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, -0.125f };
        float out8[8] = {};
        AudioStreamFormat fmt{ SampleFormat::IEEE_FLOAT32, 8, 96000, 32, 0x63Fu };
        EXPECT_TRUE(DecodeInterleavedFrameToFloat8(reinterpret_cast<const uint8_t*>(src_f), 0, fmt, out8), "8ch IEEE_FLOAT32");
        for (int c = 0; c < 8; ++c) {
            EXPECT_TRUE(std::fabs(out8[c] - src_f[c]) < 1e-6f, "8ch float exact");
        }
    }
    {
        int16_t src_16[8] = { -32768, -16384, 0, 8192, 16384, 24576, 32767, -4096 };
        float out8[8] = {};
        AudioStreamFormat fmt{ SampleFormat::PCM16, 8, 48000, 16, 0x63Fu };
        EXPECT_TRUE(DecodeInterleavedFrameToFloat8(reinterpret_cast<const uint8_t*>(src_16), 0, fmt, out8), "8ch PCM16");
        EXPECT_TRUE(std::fabs(out8[0] + 1.0f) < 1e-4f && std::fabs(out8[4] - 0.5f) < 1e-4f && out8[6] > 0.9999f, "8ch PCM16 values");
    }
    {
        uint8_t src_24[8 * 3] = {};
        for (int c = 0; c < 8; ++c) {
            int32_t v = (c == 0) ? -8388608 : (c == 7 ? 8388607 : (c * 1000000 - 3000000));
            uint32_t u = static_cast<uint32_t>(v);
            src_24[c * 3 + 0] = static_cast<uint8_t>(u & 0xFFu);
            src_24[c * 3 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
            src_24[c * 3 + 2] = static_cast<uint8_t>((u >> 16) & 0xFFu);
        }
        float out8[8] = {};
        AudioStreamFormat fmt{ SampleFormat::PCM24_PACKED, 8, 96000, 24, 0x63Fu };
        EXPECT_TRUE(DecodeInterleavedFrameToFloat8(src_24, 0, fmt, out8), "8ch PCM24_PACKED");
        EXPECT_TRUE(std::fabs(out8[0] + 1.0f) < 1e-6f && out8[7] > 0.9999f, "8ch PCM24_PACKED endpoints");
    }
    {
        int32_t src_32[8] = { INT32_MIN, -1073741824, 0, 536870912, 1073741824, 1610612736, INT32_MAX, -268435456 };
        float out8[8] = {};
        AudioStreamFormat fmt{ SampleFormat::PCM32, 8, 96000, 24, 0x63Fu };
        EXPECT_TRUE(DecodeInterleavedFrameToFloat8(reinterpret_cast<const uint8_t*>(src_32), 0, fmt, out8), "8ch PCM32");
        EXPECT_TRUE(std::fabs(out8[0] + 1.0f) < 1e-6f && std::fabs(out8[4] - 0.5f) < 1e-6f && out8[6] > 0.9999f, "8ch PCM32 values");
    }

    // Verify NormalizeChannelsTo6ChLinear matches NormalizeChannelsTo6ChEx bit-for-bit
    const float src71[8] = { 0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f };
    float out_ex[6] = {};
    float out_lin[6] = {};
    const float atten_db = -4.5f;
    NormalizeChannelsTo6ChEx(src71, out_ex, 8, 0x63Fu, atten_db, ChannelRoutingMode::DISCRETE_1TO1, false);
    NormalizeChannelsTo6ChLinear(src71, out_lin, 8, 0x63Fu, ComputeDownmixLinear(atten_db), ChannelRoutingMode::DISCRETE_1TO1, false);
    EXPECT_TRUE(std::memcmp(out_ex, out_lin, sizeof(out_ex)) == 0, "NormalizeChannelsTo6ChLinear bit-exact with Ex");

    std::printf("[PASS] TestHoistedDecode8ChAndDownmixLinear\n");
}

static void Test24BitExactRoundTripAnd61Surround() {
    // 1. Exact 24-bit round-trip (PC-C4: 2^23 scale symmetry, 0-LSB error across extreme & strided values)
    const int32_t boundaries[] = { -8388608, -8388607, -4194304, -1, 0, 1, 4194304, 8388606, 8388607 };
    AudioStreamFormat fmt24{ SampleFormat::PCM24_PACKED, 1, 96000, 24, 0x04u };
    uint8_t packed[3] = {};
    float dec8[8] = {};

    for (int32_t sample : boundaries) {
        uint32_t u = static_cast<uint32_t>(sample);
        packed[0] = static_cast<uint8_t>(u & 0xFFu);
        packed[1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
        packed[2] = static_cast<uint8_t>((u >> 16) & 0xFFu);
        EXPECT_TRUE(DecodeInterleavedFrameToFloat8(packed, 0, fmt24, dec8), "boundary PCM24 decode");
        int32_t rt = FloatToInt24Safe(dec8[0]);
        EXPECT_TRUE(rt == sample, "PCM24 boundary round-trip must be 0-LSB exact");
    }

    for (int32_t s = -8388608; s <= 8388607; s += 8191) {
        uint32_t u = static_cast<uint32_t>(s);
        packed[0] = static_cast<uint8_t>(u & 0xFFu);
        packed[1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
        packed[2] = static_cast<uint8_t>((u >> 16) & 0xFFu);
        EXPECT_TRUE(DecodeInterleavedFrameToFloat8(packed, 0, fmt24, dec8), "sweep PCM24 decode");
        int32_t rt = FloatToInt24Safe(dec8[0]);
        EXPECT_TRUE(rt == s, "PCM24 sweep round-trip must be 0-LSB exact");
    }

    // 2. 6.1 Surround: standard Windows 0x70F (BC=4, SL=5, SR=6) vs legacy 0x13F (BL=4, BR=5, BC=6)
    const float src61[8] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.0f };
    float out6[6] = {};
    const float dl = ComputeDownmixLinear(-3.0f);

    NormalizeChannelsTo6ChLinear(src61, out6, 7, 0x70Fu, dl, ChannelRoutingMode::DISCRETE_1TO1, false);
    EXPECT_TRUE(std::fabs(out6[4] - (0.6f + 0.5f) * dl) < 1e-5f, "6.1 0x70F SL+BC");
    EXPECT_TRUE(std::fabs(out6[5] - (0.7f + 0.5f) * dl) < 1e-5f, "6.1 0x70F SR+BC");

    NormalizeChannelsTo6ChLinear(src61, out6, 7, 0x13Fu, dl, ChannelRoutingMode::DISCRETE_1TO1, false);
    EXPECT_TRUE(std::fabs(out6[4] - (0.5f + 0.7f) * dl) < 1e-5f, "6.1 0x13F BL+BC");
    EXPECT_TRUE(std::fabs(out6[5] - (0.6f + 0.7f) * dl) < 1e-5f, "6.1 0x13F BR+BC");

    std::printf("[PASS] Test24BitExactRoundTripAnd61Surround\n");
}

int main() {
    TestResolveTargetSampleRate();
    TestFloatToInt32Safe();
    TestFloatToInt24Safe();
    TestDecodeInterleavedFormats();
    TestDecode8ChAndDownmix();
    TestResampler();
    TestPolyphasePerformance();
    TestEncode24BitWhiteNoiseVerbatim();
    TestHoistedDecode8ChAndDownmixLinear();
    Test24BitExactRoundTripAnd61Surround();

    if (g_failures > 0) {
        std::fprintf(stderr, "FAILED: %d assertions failed\n", g_failures);
        return 1;
    }
    std::printf("ALL AUDIO_RATE_DSP TESTS PASSED\n");
    return 0;
}
