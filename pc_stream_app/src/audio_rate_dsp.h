#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ht306::audio {

enum class SampleFormat : uint8_t {
    UNSUPPORTED = 0,
    IEEE_FLOAT32,
    PCM16,
    PCM24_PACKED,
    PCM32
};

enum class SampleRateMode : uint8_t {
    AUTO = 0,
    LOCK_96K = 1,
    LOCK_48K = 2
};

struct AudioStreamFormat {
    SampleFormat format = SampleFormat::UNSUPPORTED;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t valid_bits = 0;
    uint32_t channel_mask = 0;
};

enum class ChannelRoutingMode : uint8_t {
    DISCRETE_1TO1 = 0,
    STEREO_UPMIX = 1,
    DOWNMIX_71 = 2,
    DUAL_MONO = 3
};

struct ResolvedRate {
    uint32_t target_rate = 48000u;
    bool valid_input = false;
    bool clamped_from_high = false;
    bool resampled = false;
};

/**
 * Resolves the target RP2350 output rate (always 48000 Hz or 96000 Hz)
 * from the active UI mode and captured Windows/VB-Cable input rate.
 */
inline ResolvedRate ResolveTargetSampleRate(SampleRateMode mode, uint32_t input_rate) {
    ResolvedRate res{};
    if (input_rate < 8000u || input_rate > 384000u) {
        res.target_rate = (mode == SampleRateMode::LOCK_48K) ? 48000u : 96000u;
        res.valid_input = false;
        res.clamped_from_high = false;
        res.resampled = false;
        return res;
    }

    res.valid_input = true;
    switch (mode) {
        case SampleRateMode::LOCK_48K:
            res.target_rate = 48000u;
            res.clamped_from_high = (input_rate > 48000u);
            break;
        case SampleRateMode::LOCK_96K:
            res.target_rate = 96000u;
            res.clamped_from_high = (input_rate > 96000u);
            break;
        case SampleRateMode::AUTO:
        default:
            res.target_rate = (input_rate <= 48000u) ? 48000u : 96000u;
            res.clamped_from_high = (input_rate > 96000u);
            break;
    }
    res.resampled = (input_rate != res.target_rate);
    return res;
}

/**
 * Converts normalized float [-1.0, 1.0] to signed 32-bit PCM using double
 * arithmetic so +1.0f never rounds up to 2^31 (UB / INT32_MIN wrap-around).
 */
inline int32_t FloatToInt32Safe(float s) {
    if (!std::isfinite(s)) {
        return 0;
    }
    double clamped = (std::clamp)(static_cast<double>(s), -1.0, 1.0);
    double scaled = (std::clamp)(clamped * 2147483648.0, -2147483648.0, 2147483647.0);
    return static_cast<int32_t>(std::llround(scaled));
}

/**
 * Converts normalized float [-1.0, 1.0] to signed 24-bit PCM [-8388608, +8388607]
 * using 2^23 symmetry so decoded PCM24 values round-trip with 0 LSB error.
 */
inline int32_t FloatToInt24Safe(float s) {
    if (!std::isfinite(s)) {
        return 0;
    }
    double clamped = (std::clamp)(static_cast<double>(s), -1.0, 1.0);
    double scaled = (std::clamp)(clamped * 8388608.0, -8388608.0, 8388607.0);
    return static_cast<int32_t>(std::llround(scaled));
}

inline uint32_t BytesPerSample(SampleFormat fmt) {
    switch (fmt) {
        case SampleFormat::IEEE_FLOAT32:
        case SampleFormat::PCM32:
            return 4u;
        case SampleFormat::PCM24_PACKED:
            return 3u;
        case SampleFormat::PCM16:
            return 2u;
        case SampleFormat::UNSUPPORTED:
        default:
            return 0u;
    }
}

/**
 * Decodes up to 8 channels of a single interleaved input frame into normalized
 * float [-1.0, 1.0]. Sample stride strictly follows container SampleFormat
 * (e.g., 24-in-32 uses PCM32 with 4-byte stride).
 */
inline bool DecodeInterleavedFrameToFloat8(
    const uint8_t* src,
    uint32_t frame_idx,
    const AudioStreamFormat& fmt,
    float out_ch[8]
) {
    for (int c = 0; c < 8; ++c) {
        out_ch[c] = 0.0f;
    }
    if (src == nullptr || fmt.channels == 0u) {
        return false;
    }
    uint32_t bps = BytesPerSample(fmt.format);
    if (bps == 0u) {
        return false;
    }

    const size_t ch_count = static_cast<size_t>(fmt.channels);
    const size_t copy_ch = (std::min)(ch_count, size_t{8});
    const uint8_t* frame_ptr = src + static_cast<size_t>(frame_idx) * ch_count * bps;

    switch (fmt.format) {
        case SampleFormat::IEEE_FLOAT32:
            for (size_t c = 0; c < copy_ch; ++c) {
                float f = 0.0f;
                std::memcpy(&f, frame_ptr + c * 4u, sizeof(float));
                out_ch[c] = std::isfinite(f) ? f : 0.0f;
            }
            return true;

        case SampleFormat::PCM32:
            for (size_t c = 0; c < copy_ch; ++c) {
                int32_t v = 0;
                std::memcpy(&v, frame_ptr + c * 4u, sizeof(int32_t));
                out_ch[c] = static_cast<float>(static_cast<double>(v) / 2147483648.0);
            }
            return true;

        case SampleFormat::PCM24_PACKED:
            for (size_t c = 0; c < copy_ch; ++c) {
                const uint8_t* p = frame_ptr + c * 3u;
                uint32_t u = static_cast<uint32_t>(p[0]) |
                             (static_cast<uint32_t>(p[1]) << 8) |
                             (static_cast<uint32_t>(p[2]) << 16);
                if (u & 0x800000u) {
                    u |= 0xFF000000u;
                }
                int32_t v = static_cast<int32_t>(u);
                out_ch[c] = static_cast<float>(v) / 8388608.0f;
            }
            return true;

        case SampleFormat::PCM16:
            for (size_t c = 0; c < copy_ch; ++c) {
                int16_t v = 0;
                std::memcpy(&v, frame_ptr + c * 2u, sizeof(int16_t));
                out_ch[c] = static_cast<float>(v) / 32768.0f;
            }
            return true;

        default:
            return false;
    }
}

/**
 * Backwards-compatible wrapper decoding up to 6 channels of an interleaved frame.
 */
inline bool DecodeInterleavedFrameToFloat6(
    const uint8_t* src,
    uint32_t frame_idx,
    const AudioStreamFormat& fmt,
    float out_ch[6]
) {
    float tmp8[8];
    bool ok = DecodeInterleavedFrameToFloat8(src, frame_idx, fmt, tmp8);
    for (int c = 0; c < 6; ++c) {
        out_ch[c] = tmp8[c];
    }
    return ok;
}

inline float ComputeDownmixLinear(float downmix_attenuation_db) {
    const float atten_db = std::isfinite(downmix_attenuation_db)
        ? (std::clamp)(downmix_attenuation_db, -12.0f, 0.0f)
        : -3.0f;
    return std::pow(10.0f, atten_db / 20.0f);
}

/**
 * Normalizes 1..8 input channels into standard 5.1 layout (FL, FR, C, LFE, SL, SR)
 * using precomputed linear downmix attenuation (avoids 96k-192k pow() calls/sec):
 * - 8-ch (7.1: FL, FR, FC, LFE, BL, BR, SL, SR): folds BL/BR into SL/SR
 * - 4-ch Quad (FL, FR, BL, BR): maps BL->SL, BR->SR with C=0, LFE=0 (unless 3.1 mask 0x0F)
 * - 5-ch (5.0 / 4.1) & 3-ch (3.0 / 2.1) & Stereo (2.0) & Mono (1.0)
 */
inline void NormalizeChannelsTo6ChLinear(
    const float src8[8],
    float dst6[6],
    uint16_t in_channels,
    uint32_t channel_mask,
    float downmix_lin,
    ChannelRoutingMode routing_mode,
    bool bit_perfect
) {
    for (int c = 0; c < 6; ++c) {
        dst6[c] = 0.0f;
    }

    if (in_channels >= 8u) {
        // 7.1 Surround (FL, FR, FC, LFE, BL, BR, SL, SR) -> 5.1 (FL, FR, C, LFE, SL, SR)
        dst6[0] = src8[0];
        dst6[1] = src8[1];
        dst6[2] = src8[2];
        dst6[3] = src8[3];
        dst6[4] = (src8[6] + src8[4]) * downmix_lin;
        dst6[5] = (src8[7] + src8[5]) * downmix_lin;
    } else if (in_channels == 7u) {
        // 6.1 Surround:
        // - Standard Windows 0x70F (FL, FR, FC, LFE, BC, SL, SR): BC=4, SL=5, SR=6
        // - Legacy 0x13F or mask=0 (FL, FR, FC, LFE, BL, BR, BC): BL=4, BR=5, BC=6
        dst6[0] = src8[0];
        dst6[1] = src8[1];
        dst6[2] = src8[2];
        dst6[3] = src8[3];
        if ((channel_mask & 0x600u) == 0x600u && (channel_mask & 0x30u) == 0u) {
            dst6[4] = (src8[5] + src8[4]) * downmix_lin;
            dst6[5] = (src8[6] + src8[4]) * downmix_lin;
        } else {
            dst6[4] = (src8[4] + src8[6]) * downmix_lin;
            dst6[5] = (src8[5] + src8[6]) * downmix_lin;
        }
    } else if (in_channels == 6u) {
        for (int c = 0; c < 6; ++c) {
            dst6[c] = src8[c];
        }
    } else if (in_channels == 5u) {
        constexpr uint32_t kMaskLfe = 0x08u;
        dst6[0] = src8[0];
        dst6[1] = src8[1];
        if ((channel_mask & kMaskLfe) != 0u && (channel_mask & 0x04u) == 0u) {
            // 4.1 (FL, FR, LFE, BL, BR)
            dst6[2] = 0.0f;
            dst6[3] = src8[2];
            dst6[4] = src8[3];
            dst6[5] = src8[4];
        } else {
            // 5.0 (FL, FR, FC, BL/SL, BR/SR)
            dst6[2] = src8[2];
            dst6[3] = 0.0f;
            dst6[4] = src8[3];
            dst6[5] = src8[4];
        }
    } else if (in_channels == 4u) {
        constexpr uint32_t kMaskFcLfe = 0x0Cu;       // SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY
        constexpr uint32_t kMaskSurround = 0x630u;   // BL | BR | SL | SR
        dst6[0] = src8[0];
        dst6[1] = src8[1];
        if ((channel_mask & kMaskFcLfe) == kMaskFcLfe && (channel_mask & kMaskSurround) == 0u) {
            // 3.1 (FL, FR, FC, LFE)
            dst6[2] = src8[2];
            dst6[3] = src8[3];
            dst6[4] = 0.0f;
            dst6[5] = 0.0f;
        } else {
            // 4.0 Quad (FL, FR, BL, BR or FL, FR, SL, SR) -> map rear/side to SL, SR
            dst6[2] = 0.0f;
            dst6[3] = 0.0f;
            dst6[4] = src8[2];
            dst6[5] = src8[3];
        }
    } else if (in_channels == 3u) {
        dst6[0] = src8[0];
        dst6[1] = src8[1];
        if ((channel_mask & 0x08u) != 0u && (channel_mask & 0x04u) == 0u) {
            // 2.1 (FL, FR, LFE)
            dst6[2] = 0.0f;
            dst6[3] = src8[2];
        } else {
            // 3.0 (FL, FR, FC)
            dst6[2] = src8[2];
            dst6[3] = 0.0f;
        }
        dst6[4] = 0.0f;
        dst6[5] = 0.0f;
    } else if (in_channels == 2u) {
        dst6[0] = src8[0];
        dst6[1] = src8[1];
    } else if (in_channels == 1u) {
        dst6[0] = src8[0];
        dst6[1] = src8[0];
    }

    if (!bit_perfect && in_channels >= 2u) {
        if (routing_mode == ChannelRoutingMode::STEREO_UPMIX) {
            float L = dst6[0];
            float R = dst6[1];
            dst6[2] = (L + R) * 0.5f;
            dst6[3] = (L + R) * 0.35f;
            dst6[4] = L * 0.707f;
            dst6[5] = R * 0.707f;
        } else if (routing_mode == ChannelRoutingMode::DUAL_MONO) {
            float L = dst6[0];
            float R = dst6[1];
            dst6[0] = dst6[2] = dst6[4] = L;
            dst6[1] = dst6[3] = dst6[5] = R;
        }
    }
}

inline void NormalizeChannelsTo6ChEx(
    const float src8[8],
    float dst6[6],
    uint16_t in_channels,
    uint32_t channel_mask,
    float downmix_attenuation_db,
    ChannelRoutingMode routing_mode,
    bool bit_perfect
) {
    NormalizeChannelsTo6ChLinear(
        src8,
        dst6,
        in_channels,
        channel_mask,
        ComputeDownmixLinear(downmix_attenuation_db),
        routing_mode,
        bit_perfect
    );
}

/**
 * Stateful 6-channel Windowed-Sinc (Blackman) Polyphase LUT sample-rate converter.
 * Precomputes a 64-phase filter bank in Configure() so Process() executes zero
 * sin()/cos() calls. Bypasses processing with zero bit modification when in_rate == out_rate.
 */
class RateResampler6Ch {
public:
    static constexpr size_t kChannels = 6u;
    static constexpr size_t kMaxTaps = 64u;
    static constexpr size_t kNumPhases = 64u;
    static constexpr double kPi = 3.14159265358979323846;

    void Configure(uint32_t in_rate, uint32_t out_rate) {
        if (in_rate == in_rate_ && out_rate == out_rate_ && configured_) {
            return;
        }
        in_rate_ = in_rate;
        out_rate_ = out_rate;
        configured_ = (in_rate >= 8000u && in_rate <= 384000u &&
                       out_rate >= 8000u && out_rate <= 384000u);

        if (!configured_ || in_rate_ == out_rate_) {
            taps_ = 16u;
            half_taps_ = taps_ / 2u;
            cutoff_ = 0.45;
            polyphase_lut_.clear();
        } else {
            // Scale tap count with decimation ratio so 2:1 uses 32 taps and 4:1 uses 64 taps.
            double down_ratio = (in_rate_ > out_rate_)
                ? (static_cast<double>(in_rate_) / static_cast<double>(out_rate_))
                : 1.0;
            size_t desired = static_cast<size_t>(std::ceil(16.0 * down_ratio));
            if (desired & 1u) {
                desired++;
            }
            taps_ = (std::clamp)(desired, size_t{16}, kMaxTaps);
            half_taps_ = taps_ / 2u;
            cutoff_ = (in_rate_ > out_rate_)
                ? (0.45 * static_cast<double>(out_rate_) / static_cast<double>(in_rate_))
                : 0.45;
            BuildPolyphaseLut();
        }
        Reset();
    }

    void Reset() {
        buffer_.assign((half_taps_ - 1u) * kChannels, 0.0f);
        phase_ = static_cast<double>(half_taps_ - 1u);
    }

    bool IsBypass() const {
        return configured_ && (in_rate_ == out_rate_);
    }

    /**
     * Appends input frames (nullptr = in_frames of zeros) and writes resampled
     * 6-channel interleaved output into out_interleaved_6ch (cleared first).
     * Returns number of output frames written.
     */
    size_t Process(
        const float* in_interleaved_6ch,
        size_t in_frames,
        std::vector<float>& out_interleaved_6ch
    ) {
        out_interleaved_6ch.clear();
        if (!configured_ || in_frames == 0u) {
            return 0u;
        }

        if (in_rate_ == out_rate_) {
            out_interleaved_6ch.resize(in_frames * kChannels, 0.0f);
            if (in_interleaved_6ch != nullptr) {
                std::memcpy(
                    out_interleaved_6ch.data(),
                    in_interleaved_6ch,
                    in_frames * kChannels * sizeof(float)
                );
            }
            return in_frames;
        }

        const size_t prev_samples = buffer_.size();
        buffer_.resize(prev_samples + in_frames * kChannels, 0.0f);
        if (in_interleaved_6ch != nullptr) {
            std::memcpy(
                buffer_.data() + prev_samples,
                in_interleaved_6ch,
                in_frames * kChannels * sizeof(float)
            );
        }

        const size_t total_frames = buffer_.size() / kChannels;
        const double step = static_cast<double>(in_rate_) / static_cast<double>(out_rate_);
        const size_t est_out = static_cast<size_t>(std::ceil(static_cast<double>(in_frames) / step)) + 8u;
        out_interleaved_6ch.reserve(est_out * kChannels);

        const float* lut_data = polyphase_lut_.data();

        while (true) {
            const size_t idx = static_cast<size_t>(std::floor(phase_));
            if (idx + half_taps_ >= total_frames) {
                break;
            }
            const double frac = phase_ - static_cast<double>(idx);
            const size_t start_idx = idx - (half_taps_ - 1u);

            const double scaled_p = frac * static_cast<double>(kNumPhases);
            size_t p0 = static_cast<size_t>(scaled_p);
            if (p0 >= kNumPhases) {
                p0 = kNumPhases - 1u;
            }
            const size_t p1 = p0 + 1u;
            const float alpha = static_cast<float>(scaled_p - static_cast<double>(p0));
            const float one_minus_alpha = 1.0f - alpha;

            const float* row0 = lut_data + p0 * taps_;
            const float* row1 = lut_data + p1 * taps_;
            const float* src_ptr = &buffer_[start_idx * kChannels];

            double a0 = 0.0;
            double a1 = 0.0;
            double a2 = 0.0;
            double a3 = 0.0;
            double a4 = 0.0;
            double a5 = 0.0;

            for (size_t k = 0; k < taps_; ++k) {
                const double w = static_cast<double>(row0[k] * one_minus_alpha + row1[k] * alpha);
                a0 += static_cast<double>(src_ptr[0]) * w;
                a1 += static_cast<double>(src_ptr[1]) * w;
                a2 += static_cast<double>(src_ptr[2]) * w;
                a3 += static_cast<double>(src_ptr[3]) * w;
                a4 += static_cast<double>(src_ptr[4]) * w;
                a5 += static_cast<double>(src_ptr[5]) * w;
                src_ptr += kChannels;
            }

            out_interleaved_6ch.push_back(static_cast<float>(a0));
            out_interleaved_6ch.push_back(static_cast<float>(a1));
            out_interleaved_6ch.push_back(static_cast<float>(a2));
            out_interleaved_6ch.push_back(static_cast<float>(a3));
            out_interleaved_6ch.push_back(static_cast<float>(a4));
            out_interleaved_6ch.push_back(static_cast<float>(a5));
            phase_ += step;
        }

        // Discard already consumed prefix while preserving (half_taps_ - 1) lookback samples.
        const size_t next_idx = static_cast<size_t>(std::floor(phase_));
        if (next_idx > (half_taps_ - 1u)) {
            const size_t drop_frames = next_idx - (half_taps_ - 1u);
            if (drop_frames < total_frames) {
                const size_t drop_elems = drop_frames * kChannels;
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop_elems));
                phase_ -= static_cast<double>(drop_frames);
            } else {
                Reset();
            }
        }

        return out_interleaved_6ch.size() / kChannels;
    }

private:
    void BuildPolyphaseLut() {
        polyphase_lut_.assign((kNumPhases + 1u) * taps_, 0.0f);
        const double half_span = static_cast<double>(half_taps_);
        const double inv_span = 1.0 / static_cast<double>(taps_);
        std::array<double, kMaxTaps> row_weights{};

        for (size_t p = 0; p <= kNumPhases; ++p) {
            const double frac = static_cast<double>(p) / static_cast<double>(kNumPhases);
            double weight_sum = 0.0;
            for (size_t k = 0; k < taps_; ++k) {
                const double x = (static_cast<double>(k) - static_cast<double>(half_taps_ - 1u)) - frac;
                const double win_arg = (x + half_span) * inv_span;
                double w = 0.0;
                if (win_arg > 0.0 && win_arg < 1.0) {
                    w = 0.42 - 0.5 * std::cos(2.0 * kPi * win_arg) + 0.08 * std::cos(4.0 * kPi * win_arg);
                }
                const double sinc_arg = 2.0 * cutoff_ * x;
                const double s = (std::fabs(sinc_arg) < 1e-9)
                    ? 1.0
                    : std::sin(kPi * sinc_arg) / (kPi * sinc_arg);
                const double coeff = w * s;
                row_weights[k] = coeff;
                weight_sum += coeff;
            }
            const double norm = (std::fabs(weight_sum) > 1e-12) ? (1.0 / weight_sum) : 1.0;
            float* dst = &polyphase_lut_[p * taps_];
            for (size_t k = 0; k < taps_; ++k) {
                dst[k] = static_cast<float>(row_weights[k] * norm);
            }
        }
    }

    uint32_t in_rate_ = 0u;
    uint32_t out_rate_ = 0u;
    bool configured_ = false;
    size_t taps_ = 16u;
    size_t half_taps_ = 8u;
    double cutoff_ = 0.45;
    double phase_ = 7.0;
    std::vector<float> polyphase_lut_;
    std::vector<float> buffer_;
};

} // namespace ht306::audio
