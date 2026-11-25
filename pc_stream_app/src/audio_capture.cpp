#include "audio_capture.h"
#include "audio_policy_manager.h"
#include "lossless_codec.h"
#include <avrt.h>
#include <endpointvolume.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmsystem.h>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>
#include <cstdint>

#ifndef AUDCLNT_E_DEVICE_INVALIDATED
#define AUDCLNT_E_DEVICE_INVALIDATED static_cast<HRESULT>(0x88890004L)
#endif
#ifndef AUDCLNT_E_DEVICE_IN_USE
#define AUDCLNT_E_DEVICE_IN_USE static_cast<HRESULT>(0x8889000AL)
#endif
#ifndef AUDCLNT_E_ENDPOINT_CREATE_FAILED
#define AUDCLNT_E_ENDPOINT_CREATE_FAILED static_cast<HRESULT>(0x8889000FL)
#endif
#ifndef KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_CAP_GUID = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
#define KSDATAFORMAT_SUBTYPE_IEEE_FLOAT KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_CAP_GUID
#endif
#ifndef KSDATAFORMAT_SUBTYPE_PCM
static const GUID KSDATAFORMAT_SUBTYPE_PCM_CAP_GUID = {
    0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
#define KSDATAFORMAT_SUBTYPE_PCM KSDATAFORMAT_SUBTYPE_PCM_CAP_GUID
#endif

namespace ht306::audio {

namespace {

SampleRateMode ToDspMode(SampleRatePreference pref) {
    switch (pref) {
        case SampleRatePreference::LOCK_96K: return SampleRateMode::LOCK_96K;
        case SampleRatePreference::LOCK_48K: return SampleRateMode::LOCK_48K;
        case SampleRatePreference::AUTO:
        default:
            return SampleRateMode::AUTO;
    }
}

std::string HexHr(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return oss.str();
}

std::wstring QueryCurrentDefaultRenderId(IMMDeviceEnumerator* enumerator) {
    if (!enumerator) return {};
    IMMDevice* dev = NULL;
    std::wstring out_id;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) && dev) {
        LPWSTR raw_id = NULL;
        if (SUCCEEDED(dev->GetId(&raw_id)) && raw_id) {
            out_id = raw_id;
            CoTaskMemFree(raw_id);
        }
        dev->Release();
    }
    return out_id;
}

void CheckCaptureEndpointGain(IMMDevice* cap_dev, AppState& state) {
    if (!cap_dev) return;
    IAudioEndpointVolume* ep_vol = NULL;
    if (SUCCEEDED(cap_dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL,
                                    reinterpret_cast<void**>(&ep_vol))) && ep_vol) {
        BOOL muted = FALSE;
        float scalar = 1.0f;
        ep_vol->GetMute(&muted);
        ep_vol->GetMasterVolumeLevelScalar(&scalar);
        if (muted) {
            if (SUCCEEDED(ep_vol->SetMute(FALSE, NULL))) {
                state.AddLog(LogLevel::LOG_INFO, "AudioCapture",
                    "LG HT306 Loopback Stream sempat MUTE — otomatis dibuka (unmute) agar sinyal audio masuk.");
            } else {
                state.AddLog(LogLevel::LOG_WARN, "AudioCapture",
                    "LG HT306 Loopback Stream (Recording) dalam kondisi MUTE di Windows Sound — audio tidak akan keluar!");
            }
        }
        if (scalar < 0.99f) {
            state.AddLog(LogLevel::LOG_WARN, "AudioCapture",
                "Level Recording LG HT306 Loopback Stream < 100% — atur ke 100 di Windows Sound agar bit-accurate.");
        }
        ep_vol->Release();
    }
}

struct OpenedStream {
    IAudioClient* client = NULL;
    IAudioCaptureClient* capture = NULL;
    WAVEFORMATEX* pwfx = NULL;
    std::wstring resolved_render_id;
    bool is_vbcable_capture = false;
    bool is_ht306_target = false;
    HRESULT last_hr = S_OK;

    void Release() {
        if (capture) {
            capture->Release();
            capture = NULL;
        }
        if (client) {
            client->Release();
            client = NULL;
        }
        if (pwfx) {
            CoTaskMemFree(pwfx);
            pwfx = NULL;
        }
    }
};

bool InitializeWasapiClient(
    IMMDevice* device,
    DWORD stream_flags,
    REFERENCE_TIME buffer_duration,
    IAudioClient** out_client,
    IAudioCaptureClient** out_capture,
    WAVEFORMATEX** out_pwfx,
    HRESULT* out_hr
) {
    *out_client = NULL;
    *out_capture = NULL;
    *out_pwfx = NULL;

    IAudioClient* client = NULL;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                                  reinterpret_cast<void**>(&client));
    if (FAILED(hr) || !client) {
        if (out_hr) *out_hr = hr;
        return false;
    }

    WAVEFORMATEX* pwfx = NULL;
    hr = client->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) {
        client->Release();
        if (out_hr) *out_hr = hr;
        return false;
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, buffer_duration, 0, pwfx, NULL);
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        client->Release();
        if (out_hr) *out_hr = hr;
        return false;
    }

    IAudioCaptureClient* capture = NULL;
    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        CoTaskMemFree(pwfx);
        client->Release();
        if (out_hr) *out_hr = hr;
        return false;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        capture->Release();
        CoTaskMemFree(pwfx);
        client->Release();
        if (out_hr) *out_hr = hr;
        return false;
    }

    *out_client = client;
    *out_capture = capture;
    *out_pwfx = pwfx;
    if (out_hr) *out_hr = S_OK;
    return true;
}

} // namespace

AudioCapture::AudioCapture(AppState& state, usb::UsbTransport& transport)
    : m_state(state), m_transport(transport) {
    m_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    for (int c = 0; c < 6; ++c) {
        m_smoothed_gain[c] = 1.0f;
    }
}

AudioCapture::~AudioCapture() {
    Stop();
    if (m_stop_event) {
        CloseHandle(m_stop_event);
        m_stop_event = NULL;
    }
}

bool AudioCapture::Start() {
    if (m_running) return true;
    ResetEvent(m_stop_event);
    m_running = true;
    m_thread = CreateThread(NULL, 0, CaptureThreadProc, this, 0, NULL);
    if (m_thread == NULL) {
        m_running = false;
        return false;
    }
    return true;
}

void AudioCapture::Stop() {
    if (!m_running) return;
    m_running = false;
    if (m_stop_event) SetEvent(m_stop_event);
    if (m_thread) {
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = NULL;
    }
}

bool AudioCapture::WaitStopMs(DWORD ms) const {
    if (m_stop_event) {
        return (WaitForSingleObject(m_stop_event, ms) == WAIT_OBJECT_0);
    }
    Sleep(ms);
    return !m_running.load();
}

bool AudioCapture::IsRunning() const {
    return m_running.load();
}

DWORD WINAPI AudioCapture::CaptureThreadProc(LPVOID param) {
    auto self = reinterpret_cast<AudioCapture*>(param);
    self->CaptureLoop();
    return 0;
}

void AudioCapture::LogThrottled(LogLevel level, const std::string& key, const std::string& msg) {
    if (m_last_status_key == key) {
        return;
    }
    m_last_status_key = key;
    m_state.AddLog(level, "AudioCapture", msg);
}

AudioStreamFormat AudioCapture::ParseWaveFormat(const WAVEFORMATEX* pwfx) {
    AudioStreamFormat fmt{};
    if (!pwfx || pwfx->nChannels == 0 || pwfx->nSamplesPerSec == 0) {
        return fmt;
    }
    fmt.channels = pwfx->nChannels;
    fmt.sample_rate = pwfx->nSamplesPerSec;
    fmt.valid_bits = pwfx->wBitsPerSample;

    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        fmt.format = (pwfx->wBitsPerSample == 32) ? SampleFormat::IEEE_FLOAT32 : SampleFormat::UNSUPPORTED;
    } else if (pwfx->wFormatTag == WAVE_FORMAT_PCM) {
        if (pwfx->wBitsPerSample == 16) fmt.format = SampleFormat::PCM16;
        else if (pwfx->wBitsPerSample == 24) fmt.format = SampleFormat::PCM24_PACKED;
        else if (pwfx->wBitsPerSample == 32) fmt.format = SampleFormat::PCM32;
    } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               pwfx->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        fmt.valid_bits = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample : pwfx->wBitsPerSample;
        fmt.channel_mask = ext->dwChannelMask;
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            fmt.format = (pwfx->wBitsPerSample == 32) ? SampleFormat::IEEE_FLOAT32 : SampleFormat::UNSUPPORTED;
        } else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (pwfx->wBitsPerSample == 16) fmt.format = SampleFormat::PCM16;
            else if (pwfx->wBitsPerSample == 24) fmt.format = SampleFormat::PCM24_PACKED;
            else if (pwfx->wBitsPerSample == 32) fmt.format = SampleFormat::PCM32;
        }
    }
    return fmt;
}

float AudioCapture::ApplySoftLimiter(float x) {
    const float threshold = 0.85f;
    const float ceiling = 0.9885f;
    const float headroom = ceiling - threshold;
    float abs_x = std::fabs(x);
    if (abs_x <= threshold) {
        return x;
    }
    float compressed = threshold + headroom * std::tanh((abs_x - threshold) / headroom);
    return (x < 0.0f) ? -compressed : compressed;
}

float AudioCapture::GenerateTestSample(uint32_t mix_rate, float amp) {
    if (mix_rate == 0) mix_rate = 96000;
    const float two_pi = 6.28318530717958647692f;
    const int sig_type = m_state.test_signal_type.load();

    if (sig_type == 1) {
        m_lcg_seed = m_lcg_seed * 1664525u + 1013904223u;
        float white = ((float)(m_lcg_seed & 0x7FFFFFFF) / 1073741824.0f) - 1.0f;

        m_b0 = 0.99886f * m_b0 + white * 0.0555179f;
        m_b1 = 0.99332f * m_b1 + white * 0.0750759f;
        m_b2 = 0.96900f * m_b2 + white * 0.1538520f;
        m_b3 = 0.86650f * m_b3 + white * 0.3104856f;
        m_b4 = 0.55000f * m_b4 + white * 0.5329522f;
        m_b5 = -0.7616f * m_b5 - white * 0.0168980f;
        float pink = m_b0 + m_b1 + m_b2 + m_b3 + m_b4 + m_b5 + m_b6 + white * 0.5362f;
        m_b6 = white * 0.115926f;
        return pink * 0.11f * amp;
    } else if (sig_type == 0) {
        const float f_start = 40.0f;
        const float f_end = 16000.0f;
        const float duration = 1.5f;

        float progress = m_chirp_t / duration;
        if (progress > 1.0f) progress = 1.0f;
        float f_inst = f_start * std::pow(f_end / f_start, progress);

        m_chirp_phase += (two_pi * f_inst) / (float)mix_rate;
        if (m_chirp_phase >= two_pi) {
            m_chirp_phase = std::fmod(m_chirp_phase, two_pi);
        }

        m_chirp_t += 1.0f / (float)mix_rate;
        if (m_chirp_t >= duration) {
            m_chirp_t = 0.0f;
        }
        return std::sin(m_chirp_phase) * amp;
    } else {
        m_sine_phase += (two_pi * 1000.0f) / (float)mix_rate;
        if (m_sine_phase >= two_pi) {
            m_sine_phase = std::fmod(m_sine_phase, two_pi);
        }
        return std::sin(m_sine_phase) * amp;
    }
}

void AudioCapture::NormalizeChannelsTo6Ch(
    const float src8[8],
    float dst6[6],
    uint16_t in_channels,
    uint32_t channel_mask,
    float downmix_lin,
    const AudioDspSettings& settings
) const {
    ChannelRoutingMode mode = ChannelRoutingMode::DISCRETE_1TO1;
    switch (settings.routing_preset) {
        case RoutingPreset::STEREO_UPMIX: mode = ChannelRoutingMode::STEREO_UPMIX; break;
        case RoutingPreset::DOWNMIX_71:   mode = ChannelRoutingMode::DOWNMIX_71;   break;
        case RoutingPreset::DUAL_MONO:    mode = ChannelRoutingMode::DUAL_MONO;    break;
        case RoutingPreset::DISCRETE_1TO1:
        default:                          mode = ChannelRoutingMode::DISCRETE_1TO1; break;
    }
    NormalizeChannelsTo6ChLinear(
        src8,
        dst6,
        in_channels,
        channel_mask,
        downmix_lin,
        mode,
        settings.bit_perfect_mode
    );
}

void AudioCapture::FlushUsbBatch(
    std::vector<uint8_t>& usb_batch,
    uint32_t& usb_batch_packets,
    uint32_t output_rate
) {
    if (usb_batch.empty()) {
        usb_batch_packets = 0;
        return;
    }

    const size_t batch_bytes = usb_batch.size();
    const uint32_t batch_pkts = usb_batch_packets;
    const bool was_open = m_transport.IsOpen();
    bool ok = false;
    if (was_open) {
        ok = m_transport.WritePacket(usb_batch.data(), batch_bytes);
    }

    if (ok) {
        m_state.telemetry.total_frames.fetch_add(static_cast<uint64_t>(batch_pkts) * 48u);
        m_state.telemetry.total_packets.fetch_add(batch_pkts);
        if (batch_pkts > 0u && output_rate > 0u) {
            float avg_pkt_bytes = static_cast<float>(batch_bytes) / static_cast<float>(batch_pkts);
            m_state.telemetry.throughput_kbs.store(
                (avg_pkt_bytes * (static_cast<float>(output_rate) / 48.0f)) / 1024.0f
            );
        }
    } else {
        m_state.telemetry.throughput_kbs.store(0.0f);
        if (was_open) {
            m_state.telemetry.underrun_count.fetch_add(1);
        }
    }

    usb_batch.clear();
    usb_batch_packets = 0;
}

void AudioCapture::EmitProcessedFrames6Ch(
    const float* frames_6ch,
    size_t num_frames,
    uint32_t output_rate,
    bool is_sweep,
    bool allow_silence_gate,
    const AudioDspSettings& settings,
    std::vector<int32_t>& pcm6ch_block,
    int& frame_in_block,
    std::vector<uint8_t>& usb_packet,
    std::vector<uint8_t>& usb_batch,
    uint32_t& usb_batch_packets,
    ULONGLONG& last_meter_update_tick
) {
    if (!frames_6ch || num_frames == 0 || output_rate == 0) {
        return;
    }

    static constexpr uint32_t kUsbBatchMaxSubframes = 4u;
    static constexpr size_t kUsbBatchMaxBytes = 3584u;

    float ch_max[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    bool ch_clipped[6] = { false, false, false, false, false, false };

    const bool bit_perfect = settings.bit_perfect_mode;
    bool bit_perfect_settled = bit_perfect;
    float target_gain[6];
    if (bit_perfect) {
        for (int c = 0; c < 6; ++c) {
            target_gain[c] = 1.0f;
            if (std::fabs(m_smoothed_gain[c] - 1.0f) > 1e-4f) {
                bit_perfect_settled = false;
            }
        }
    } else {
        float master_db = (std::clamp)(settings.master_volume_db, -60.0f, 6.0f);
        float master_lin = std::pow(10.0f, master_db / 20.0f);
        for (int c = 0; c < 6; ++c) {
            target_gain[c] = master_lin * std::pow(10.0f, settings.trims[c] / 20.0f);
        }
    }

    bool any_solo = false;
    for (int c = 0; c < 6; ++c) {
        if (settings.solos[c]) {
            any_solo = true;
            break;
        }
    }

    const uint32_t silence_limit = ((output_rate / 48u) * 6u) / 10u; // ~600 ms

    for (size_t f = 0; f < num_frames; ++f) {
        const float* in6 = &frames_6ch[f * 6u];
        float routed[6];

        if (is_sweep) {
            for (int c = 0; c < 6; ++c) {
                routed[c] = in6[c];
            }
        } else if (bit_perfect) {
            if (bit_perfect_settled) {
                for (int c = 0; c < 6; ++c) {
                    routed[c] = in6[c];
                }
            } else {
                bool all_settled = true;
                for (int c = 0; c < 6; ++c) {
                    m_smoothed_gain[c] += 0.005f * (1.0f - m_smoothed_gain[c]);
                    if (std::fabs(m_smoothed_gain[c] - 1.0f) <= 1e-4f) {
                        m_smoothed_gain[c] = 1.0f;
                    } else {
                        all_settled = false;
                    }
                    routed[c] = in6[c] * m_smoothed_gain[c];
                }
                if (all_settled) {
                    bit_perfect_settled = true;
                }
            }
        } else {
            for (int c = 0; c < 6; ++c) {
                int src_idx = (std::clamp)(settings.routing_map[c], 0, 5);
                float val = in6[src_idx];
                if (settings.phase_inverts[c]) {
                    val = -val;
                }
                m_smoothed_gain[c] += 0.005f * (target_gain[c] - m_smoothed_gain[c]);
                routed[c] = val * m_smoothed_gain[c];
            }
        }

        if (!is_sweep) {
            for (int c = 0; c < 6; ++c) {
                if (settings.mute_master || settings.mutes[c] ||
                    (any_solo && !settings.solos[c])) {
                    routed[c] = 0.0f;
                }
            }
            if (settings.limiter_enabled && !bit_perfect) {
                for (int c = 0; c < 6; ++c) {
                    routed[c] = ApplySoftLimiter(routed[c]);
                }
            }
        }

        for (int c = 0; c < 6; ++c) {
            float s = routed[c];
            float abs_s = std::fabs(s);
            if (abs_s >= 0.988f) {
                ch_clipped[c] = true;
            }
            if (abs_s > ch_max[c]) {
                ch_max[c] = abs_s;
            }
            if (abs_s > m_current_block_peak) {
                m_current_block_peak = abs_s;
            }
            pcm6ch_block[static_cast<size_t>(frame_in_block) * 6u + c] = FloatToInt24Safe(s);
        }

        frame_in_block++;
        if (frame_in_block == 48) {
            frame_in_block = 0;
            bool is_block_silent = (m_current_block_peak < 1e-6f);
            m_current_block_peak = 0.0f;

            if (allow_silence_gate && !is_sweep && is_block_silent) {
                m_silent_blocks_streak++;
            } else {
                m_silent_blocks_streak = 0;
            }

            if (allow_silence_gate && !is_sweep && m_silent_blocks_streak > silence_limit) {
                // Flush any already-batched subframes before entering silence standby
                FlushUsbBatch(usb_batch, usb_batch_packets, output_rate);
                m_state.telemetry.throughput_kbs.store(0.0f);
                continue;
            }

            size_t enc_len = 0;
            uint8_t enc_flags = HT306_FLAG_BITDEPTH_24;
            if (output_rate >= 88200u) {
                enc_flags |= HT306_FLAG_RATE_96KHZ;
            }

            HT306_CodecStatus_t st = ht306_lossless_encode_frame_flags(
                pcm6ch_block.data(),
                48,
                m_seq++,
                enc_flags,
                usb_packet.data(),
                HT306_MAX_PACKET_BYTES,
                &enc_len
            );

            if (st == HT306_CODEC_OK && enc_len > 0 && enc_len <= HT306_MAX_PACKET_BYTES) {
                if (!usb_batch.empty() &&
                    (usb_batch_packets >= kUsbBatchMaxSubframes ||
                     (usb_batch.size() + enc_len) > kUsbBatchMaxBytes)) {
                    FlushUsbBatch(usb_batch, usb_batch_packets, output_rate);
                }
                usb_batch.insert(
                    usb_batch.end(),
                    usb_packet.begin(),
                    usb_packet.begin() + static_cast<std::ptrdiff_t>(enc_len)
                );
                usb_batch_packets++;

                if (usb_batch_packets >= kUsbBatchMaxSubframes) {
                    FlushUsbBatch(usb_batch, usb_batch_packets, output_rate);
                }
            }
        }
    }

    // Flush remaining encoded subframes at the end of this chunk
    FlushUsbBatch(usb_batch, usb_batch_packets, output_rate);

    m_state.UpdateMetersFromPeaks(ch_max, ch_clipped);
    last_meter_update_tick = GetTickCount64();
}

void AudioCapture::PumpSweepIfActive(
    LARGE_INTEGER qpc_freq,
    LARGE_INTEGER& qpc_last,
    uint32_t sweep_rate,
    std::vector<int32_t>& pcm6ch_block,
    int& frame_in_block,
    std::vector<uint8_t>& usb_packet,
    std::vector<uint8_t>& usb_batch,
    uint32_t& usb_batch_packets,
    ULONGLONG& last_meter_update_tick
) {
    int active_sweep_ch = m_state.sweep_active_channel.load();
    bool is_sweep = m_state.sweep_running.load() || (active_sweep_ch >= 0 && active_sweep_ch < 6);
    if (!is_sweep) {
        m_last_sweep_ch = -1;
        QueryPerformanceCounter(&qpc_last);
        return;
    }

    if (active_sweep_ch < 0 || active_sweep_ch >= 6) {
        active_sweep_ch = 0;
    }
    if (active_sweep_ch != m_last_sweep_ch) {
        m_last_sweep_ch = active_sweep_ch;
        m_chirp_t = 0.0f;
        m_chirp_phase = 0.0f;
        frame_in_block = 0;
        m_current_block_peak = 0.0f;
    }

    LARGE_INTEGER qpc_now;
    QueryPerformanceCounter(&qpc_now);
    double dt = static_cast<double>(qpc_now.QuadPart - qpc_last.QuadPart) / static_cast<double>(qpc_freq.QuadPart);
    int blocks = static_cast<int>(dt * static_cast<double>(sweep_rate) / 48.0);
    if (blocks <= 0) {
        return;
    }
    if (blocks > 12) {
        blocks = 12;
        qpc_last = qpc_now;
    } else {
        qpc_last.QuadPart += static_cast<LONGLONG>(
            (static_cast<double>(blocks * 48) / static_cast<double>(sweep_rate)) *
            static_cast<double>(qpc_freq.QuadPart)
        );
    }

    float raw_dbfs = m_state.test_level_dbfs.load();
    float test_dbfs = std::isfinite(raw_dbfs) ? (std::clamp)(raw_dbfs, -60.0f, 0.0f) : -20.0f;
    float test_amp = std::pow(10.0f, test_dbfs / 20.0f);

    const size_t total_frames = static_cast<size_t>(blocks) * 48u;
    m_in_frames_6ch.assign(total_frames * 6u, 0.0f);
    for (size_t f = 0; f < total_frames; ++f) {
        m_in_frames_6ch[f * 6u + static_cast<size_t>(active_sweep_ch)] =
            GenerateTestSample(sweep_rate, test_amp);
    }

    AudioDspSettings settings_snap = m_state.GetAudioDspSettingsSnapshot();
    EmitProcessedFrames6Ch(
        m_in_frames_6ch.data(),
        total_frames,
        sweep_rate,
        true,
        false,
        settings_snap,
        pcm6ch_block,
        frame_in_block,
        usb_packet,
        usb_batch,
        usb_batch_packets,
        last_meter_update_tick
    );
}

void AudioCapture::CaptureLoop() {
    timeBeginPeriod(1);
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    HRESULT hr_co = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    std::vector<int32_t> pcm6ch_block(48 * 6, 0);
    std::vector<uint8_t> usb_packet(HT306_MAX_PACKET_BYTES, 0);
    std::vector<uint8_t> usb_batch;
    usb_batch.reserve(3584);
    uint32_t usb_batch_packets = 0;
    int frame_in_block = 0;
    ULONGLONG last_meter_update_tick = GetTickCount64();

    LARGE_INTEGER qpc_freq;
    QueryPerformanceFrequency(&qpc_freq);
    LARGE_INTEGER qpc_last;
    QueryPerformanceCounter(&qpc_last);

    IMMDeviceEnumerator* enumerator = NULL;
    CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)
    );

    try {
    while (m_running) {
        int current_dev_idx = 0;
        AudioEndpointInfo active_dev = m_state.GetActiveDeviceSnapshot(&current_dev_idx);
        uint32_t current_gen = m_state.capture_reinit_generation.load(std::memory_order_acquire);
        SampleRatePreference current_rate_mode = m_state.sample_rate_mode.load();

        OpenedStream stream{};
        bool stream_opened = false;

        if (!enumerator) {
            CoCreateInstance(
                __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)
            );
        }

        if (enumerator) {
            IMMDevice* render_dev = NULL;
            HRESULT dev_hr = S_OK;
            if (!active_dev.id.empty()) {
                dev_hr = enumerator->GetDevice(active_dev.id.c_str(), &render_dev);
            }
            if (!render_dev) {
                dev_hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &render_dev);
            }
            if (!render_dev && (dev_hr == RPC_E_DISCONNECTED || dev_hr == AUDCLNT_E_SERVICE_NOT_RUNNING || FAILED(dev_hr))) {
                enumerator->Release();
                enumerator = NULL;
            }

            if (render_dev) {
                LPWSTR raw_id = NULL;
                if (SUCCEEDED(render_dev->GetId(&raw_id)) && raw_id) {
                    stream.resolved_render_id = raw_id;
                    CoTaskMemFree(raw_id);
                }
                std::wstring ht306_render_id = AudioPolicyManager::Instance().GetRenderEndpointId();
                stream.is_ht306_target = (!ht306_render_id.empty() &&
                                          stream.resolved_render_id == ht306_render_id);

                if (stream.is_ht306_target) {
                    // Step A (PRIMARY for HT306): Capture directly from VB-Cable Output (eCapture).
                    // Because this endpoint stays separate from eRender, JRiver WASAPI Exclusive never clashes with us!
                    std::wstring cap_id = AudioPolicyManager::Instance().GetCaptureEndpointId();
                    if (!cap_id.empty()) {
                        IMMDevice* cap_dev = NULL;
                        HRESULT get_hr = enumerator->GetDevice(cap_id.c_str(), &cap_dev);
                        DWORD cap_state = 0;
                        if (SUCCEEDED(get_hr) && cap_dev) {
                            cap_dev->GetState(&cap_state);
                        }
                        if (!cap_dev || cap_state != DEVICE_STATE_ACTIVE) {
                            if (cap_dev) {
                                cap_dev->Release();
                                cap_dev = NULL;
                            }
                            AudioPolicyManager::Instance().EnsureCaptureEndpointActive();
                            enumerator->GetDevice(cap_id.c_str(), &cap_dev);
                        }

                        if (cap_dev) {
                            HRESULT cap_hr = S_OK;
                            stream_opened = InitializeWasapiClient(
                                cap_dev,
                                0, // Recording endpoint MUST NOT pass AUDCLNT_STREAMFLAGS_LOOPBACK
                                300000, // 30 ms
                                &stream.client,
                                &stream.capture,
                                &stream.pwfx,
                                &cap_hr
                            );
                            if (stream_opened) {
                                stream.is_vbcable_capture = true;
                                CheckCaptureEndpointGain(cap_dev, m_state);
                            } else {
                                stream.last_hr = cap_hr;
                                if (cap_hr == E_ACCESSDENIED) {
                                    LogThrottled(LogLevel::LOG_WARN, "mic_denied",
                                        "Akses LG HT306 Loopback Stream ditolak (E_ACCESSDENIED). Aktifkan izin Mikrofon di Windows Privacy Settings!");
                                }
                            }
                            cap_dev->Release();
                        }
                    }

                    // Step B (FALLBACK for HT306): Only try eRender Shared Loopback if eCapture could not open
                    if (!stream_opened) {
                        HRESULT render_hr = S_OK;
                        stream_opened = InitializeWasapiClient(
                            render_dev,
                            AUDCLNT_STREAMFLAGS_LOOPBACK,
                            400000, // 40 ms
                            &stream.client,
                            &stream.capture,
                            &stream.pwfx,
                            &render_hr
                        );
                        if (!stream_opened && stream.last_hr == S_OK) {
                            stream.last_hr = render_hr;
                        }
                    }
                } else {
                    // Non-HT306 device (e.g. Realtek Speakers): use eRender Shared Loopback directly
                    stream_opened = InitializeWasapiClient(
                        render_dev,
                        AUDCLNT_STREAMFLAGS_LOOPBACK,
                        400000, // 40 ms
                        &stream.client,
                        &stream.capture,
                        &stream.pwfx,
                        &stream.last_hr
                    );
                }

                render_dev->Release();
                render_dev = NULL;
            }
        }

        if (!stream_opened) {
            bool is_sweep = m_state.sweep_running.load() ||
                            (m_state.sweep_active_channel.load() >= 0 && m_state.sweep_active_channel.load() < 6);
            m_state.telemetry.capture_path.store(is_sweep ? CapturePathMode::SWEEP_ONLY : CapturePathMode::DISCONNECTED);

            uint32_t sweep_rate = (current_rate_mode == SampleRatePreference::LOCK_48K) ? 48000u : 96000u;
            m_state.telemetry.sample_rate.store(is_sweep ? sweep_rate : 0u);
            m_state.telemetry.input_sample_rate.store(0u);
            m_state.telemetry.rate_clamped.store(false);
            if (!is_sweep) {
                m_state.telemetry.throughput_kbs.store(0.0f);
            }

            if (stream.last_hr != S_OK && stream.last_hr != E_ACCESSDENIED) {
                LogThrottled(LogLevel::LOG_WARN, "cap_open_fail_" + HexHr(stream.last_hr),
                    "Menunggu endpoint audio siap (" + HexHr(stream.last_hr) + "). Matikan Exclusive Mode di player atau cek izin Mikrofon.");
            }

            // Non-blocking 500ms retry backoff while still driving Sweep Test smoothly
            ULONGLONG retry_until = GetTickCount64() + 500;
            while (m_running && GetTickCount64() < retry_until) {
                if (m_state.capture_reinit_generation.load(std::memory_order_acquire) != current_gen ||
                    m_state.active_device_index.load() != current_dev_idx ||
                    m_state.sample_rate_mode.load() != current_rate_mode) {
                    break;
                }
                bool sweep_now = m_state.sweep_running.load() ||
                                 (m_state.sweep_active_channel.load() >= 0 && m_state.sweep_active_channel.load() < 6);
                m_state.telemetry.capture_path.store(sweep_now ? CapturePathMode::SWEEP_ONLY : CapturePathMode::DISCONNECTED);
                m_state.telemetry.sample_rate.store(sweep_now ? sweep_rate : 0u);
                if (sweep_now) {
                    PumpSweepIfActive(
                        qpc_freq, qpc_last, sweep_rate, pcm6ch_block, frame_in_block,
                        usb_packet, usb_batch, usb_batch_packets, last_meter_update_tick
                    );
                } else {
                    ULONGLONG now_idle = GetTickCount64();
                    if (now_idle - last_meter_update_tick >= 30) {
                        last_meter_update_tick = now_idle;
                        m_state.DecayMeters();
                    }
                }
                if (WaitStopMs(sweep_now ? 1 : 20)) {
                    break;
                }
            }
            continue;
        }

        // Validate stream format cleanly (reject raw bitstream / non-PCM formats)
        AudioStreamFormat fmt = ParseWaveFormat(stream.pwfx);
        ResolvedRate rr = ResolveTargetSampleRate(ToDspMode(current_rate_mode), fmt.sample_rate);
        if (fmt.format == SampleFormat::UNSUPPORTED || !rr.valid_input) {
            LogThrottled(LogLevel::LOG_ERROR, "fmt_unsup",
                "Format audio endpoint tidak didukung (" + std::to_string(fmt.sample_rate) +
                " Hz, " + std::to_string(fmt.channels) + " ch). Pastikan PCM/Float dan non-encoded AC3/DTS.");
            stream.client->Stop();
            stream.Release();
            WaitStopMs(250);
            continue;
        }

        // Clean slate for every re-initialization (explicitly reset resampler even when rate unchanged!)
        frame_in_block = 0;
        m_current_block_peak = 0.0f;
        m_silent_blocks_streak = 0;
        usb_batch.clear();
        usb_batch_packets = 0;
        m_resampler.Configure(fmt.sample_rate, rr.target_rate);
        m_resampler.Reset();

        m_state.telemetry.capture_path.store(
            stream.is_vbcable_capture ? CapturePathMode::VBCABLE_CAPTURE : CapturePathMode::LOOPBACK
        );
        m_state.telemetry.input_sample_rate.store(fmt.sample_rate);
        m_state.telemetry.sample_rate.store(rr.target_rate);
        m_state.telemetry.rate_clamped.store(rr.clamped_from_high);
        m_state.telemetry.channels.store(6);
        m_state.telemetry.bit_depth.store(24);

        {
            std::string path_name = stream.is_vbcable_capture
                ? "VB-Cable Capture (LG HT306 Loopback Stream — Exclusive Safe)"
                : "WASAPI Shared Loopback";
            std::string key = path_name + "_" + std::to_string(fmt.sample_rate) + "_" + std::to_string(rr.target_rate);
            std::string msg = "Jalur aktif: " + path_name + " | Input " + std::to_string(fmt.sample_rate) +
                              " Hz (" + std::to_string(fmt.channels) + "ch) -> RP2350 " +
                              std::to_string(rr.target_rate) + " Hz (24-bit Lossless)";
            if (rr.clamped_from_high) {
                msg += " [Mode Auto: Dinetralkan <=96 kHz]";
            } else if (rr.resampled) {
                msg += " [Resampled]";
            }
            LogThrottled(LogLevel::LOG_INFO, key, msg);
        }

        QueryPerformanceCounter(&qpc_last);
        ULONGLONG last_def_check_tick = GetTickCount64();
        ULONGLONG last_capture_reprobe_tick = last_def_check_tick;
        bool was_sweep = false;

        while (m_running) {
            if (m_state.capture_reinit_generation.load(std::memory_order_acquire) != current_gen ||
                m_state.active_device_index.load() != current_dev_idx ||
                m_state.sample_rate_mode.load() != current_rate_mode) {
                break;
            }

            const ULONGLONG now_tick = GetTickCount64();

            // If capturing while set to Default Windows Device (index 0),
            // verify every 1s that Windows default playback endpoint has not changed.
            if (current_dev_idx == 0 && enumerator) {
                if (now_tick - last_def_check_tick >= 1000) {
                    last_def_check_tick = now_tick;
                    std::wstring cur_def = QueryCurrentDefaultRenderId(enumerator);
                    if (!cur_def.empty() && cur_def != stream.resolved_render_id) {
                        break;
                    }
                }
            }

            // If on LOOPBACK fallback for HT306 target, re-probe eCapture every 5s so we recover Exclusive-safe path
            if (stream.is_ht306_target && !stream.is_vbcable_capture && enumerator &&
                (now_tick - last_capture_reprobe_tick >= 5000)) {
                last_capture_reprobe_tick = now_tick;
                std::wstring cap_id = AudioPolicyManager::Instance().GetCaptureEndpointId();
                if (!cap_id.empty()) {
                    IMMDevice* cap_dev = NULL;
                    DWORD cap_state = 0;
                    if (SUCCEEDED(enumerator->GetDevice(cap_id.c_str(), &cap_dev)) && cap_dev) {
                        cap_dev->GetState(&cap_state);
                        if (cap_state == DEVICE_STATE_ACTIVE) {
                            IAudioClient* probe_client = NULL;
                            IAudioCaptureClient* probe_cap = NULL;
                            WAVEFORMATEX* probe_wfx = NULL;
                            if (InitializeWasapiClient(cap_dev, 0, 300000, &probe_client, &probe_cap, &probe_wfx, NULL)) {
                                probe_client->Stop();
                                probe_cap->Release();
                                probe_client->Release();
                                CoTaskMemFree(probe_wfx);
                                cap_dev->Release();
                                break; // Switch cleanly to primary eCapture path on outer re-open
                            }
                        }
                        cap_dev->Release();
                    }
                }
            }

            int active_sweep_ch = m_state.sweep_active_channel.load();
            bool is_sweep = m_state.sweep_running.load() || (active_sweep_ch >= 0 && active_sweep_ch < 6);

            if (is_sweep && !was_sweep) {
                frame_in_block = 0;
                m_current_block_peak = 0.0f;
                usb_batch.clear();
                usb_batch_packets = 0;
            } else if (!is_sweep && was_sweep) {
                m_resampler.Reset();
                frame_in_block = 0;
                m_current_block_peak = 0.0f;
                usb_batch.clear();
                usb_batch_packets = 0;
            }
            was_sweep = is_sweep;

            UINT32 packet_length = 0;
            HRESULT hr = stream.capture->GetNextPacketSize(&packet_length);
            if (FAILED(hr)) {
                break;
            }

            if (is_sweep) {
                // Drain pending capture packets so driver ring doesn't stall while sweep runs at target_rate
                while (packet_length > 0) {
                    BYTE* pDummy = NULL;
                    UINT32 nFrames = 0;
                    DWORD flags = 0;
                    if (FAILED(stream.capture->GetBuffer(&pDummy, &nFrames, &flags, NULL, NULL))) {
                        break;
                    }
                    stream.capture->ReleaseBuffer(nFrames);
                    if (FAILED(stream.capture->GetNextPacketSize(&packet_length))) {
                        break;
                    }
                }
                PumpSweepIfActive(
                    qpc_freq, qpc_last, rr.target_rate, pcm6ch_block, frame_in_block,
                    usb_packet, usb_batch, usb_batch_packets, last_meter_update_tick
                );
                if (WaitStopMs(1)) {
                    break;
                }
                continue;
            }

            if (packet_length == 0) {
                QueryPerformanceCounter(&qpc_last);
                if (now_tick - last_meter_update_tick >= 30) {
                    last_meter_update_tick = now_tick;
                    m_state.DecayMeters();
                }
                if (WaitStopMs(2)) {
                    break;
                }
                continue;
            }

            while (packet_length > 0 && m_running) {
                BYTE* pData = NULL;
                UINT32 numFramesAvailable = 0;
                DWORD flags = 0;

                hr = stream.capture->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
                if (FAILED(hr) || hr == AUDCLNT_S_BUFFER_EMPTY) {
                    packet_length = 0;
                    break;
                }
                if (numFramesAvailable > 65536u) {
                    stream.capture->ReleaseBuffer(numFramesAvailable);
                    hr = E_FAIL;
                    break;
                }

                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                    m_resampler.Reset();
                    frame_in_block = 0;
                    m_current_block_peak = 0.0f;
                    usb_batch.clear();
                    usb_batch_packets = 0;
                }

                bool is_silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || (pData == nullptr);
                if (numFramesAvailable > 0) {
                    const AudioDspSettings settings_snap = m_state.GetAudioDspSettingsSnapshot();
                    const float downmix_lin = ComputeDownmixLinear(settings_snap.downmix_attenuation_db);
                    m_in_frames_6ch.resize(static_cast<size_t>(numFramesAvailable) * 6u);
                    if (is_silent) {
                        std::fill(m_in_frames_6ch.begin(), m_in_frames_6ch.end(), 0.0f);
                    } else {
                        float decoded[8];
                        for (UINT32 f = 0; f < numFramesAvailable; ++f) {
                            DecodeInterleavedFrameToFloat8(pData, f, fmt, decoded);
                            NormalizeChannelsTo6Ch(
                                decoded,
                                &m_in_frames_6ch[static_cast<size_t>(f) * 6u],
                                fmt.channels,
                                fmt.channel_mask,
                                downmix_lin,
                                settings_snap
                            );
                        }
                    }

                    const float* out_ptr = m_in_frames_6ch.data();
                    size_t out_frames = static_cast<size_t>(numFramesAvailable);
                    if (!m_resampler.IsBypass()) {
                        out_frames = m_resampler.Process(
                            m_in_frames_6ch.data(),
                            numFramesAvailable,
                            m_resampled_6ch
                        );
                        out_ptr = m_resampled_6ch.data();
                    }

                    EmitProcessedFrames6Ch(
                        out_ptr,
                        out_frames,
                        rr.target_rate,
                        false,
                        true, // Allow silence gate in both VBCABLE_CAPTURE and LOOPBACK
                        settings_snap,
                        pcm6ch_block,
                        frame_in_block,
                        usb_packet,
                        usb_batch,
                        usb_batch_packets,
                        last_meter_update_tick
                    );
                }

                stream.capture->ReleaseBuffer(numFramesAvailable);
                QueryPerformanceCounter(&qpc_last);

                if (FAILED(stream.capture->GetNextPacketSize(&packet_length))) {
                    hr = E_FAIL;
                    break;
                }
            }
            if (FAILED(hr)) {
                break;
            }
        }

        stream.client->Stop();
        stream.Release();
        if (m_running) {
            WaitStopMs(20);
        }
    }
    } catch (const std::exception& e) {
        m_state.AddLog(LogLevel::LOG_ERROR, "WASAPI", std::string("Capture loop exception: ") + e.what());
    }

    if (enumerator) {
        enumerator->Release();
        enumerator = NULL;
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(hr_co)) CoUninitialize();
    timeEndPeriod(1);
}

} // namespace ht306::audio
