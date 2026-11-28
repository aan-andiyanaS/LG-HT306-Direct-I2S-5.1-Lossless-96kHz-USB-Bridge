#pragma once

#include <cstdint>
#include <atomic>
#include <deque>
#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <type_traits>

namespace ht306 {

enum class ConnectionState {
    DISCONNECTED = 0,
    CONNECTING,
    READY,
    STREAMING,
    ERR_STATE
};

enum class RoutingPreset {
    DISCRETE_1TO1 = 0,
    STEREO_UPMIX,
    DOWNMIX_71,
    DUAL_MONO
};

enum class CapturePathMode : uint8_t {
    DISCONNECTED = 0,
    LOOPBACK = 1,
    VBCABLE_CAPTURE = 2,
    SWEEP_ONLY = 3
};

enum class SampleRatePreference : uint8_t {
    AUTO = 0,
    LOCK_96K = 1,
    LOCK_48K = 2
};

struct AudioEndpointInfo {
    std::wstring id;
    std::string name;
    bool is_default = false;
};

struct ChannelMeters {
    float peak_db = -60.0f;     // -60 dBFS to 0 dBFS
    float peak_hold_db = -60.0f;
    bool clipped = false;
};

struct StreamTelemetry {
    std::atomic<uint32_t> sample_rate{96000};
    std::atomic<uint32_t> input_sample_rate{96000};
    std::atomic<uint32_t> bit_depth{24};
    std::atomic<uint32_t> channels{6};
    std::atomic<bool> rate_clamped{false};
    std::atomic<CapturePathMode> capture_path{CapturePathMode::DISCONNECTED};
    std::atomic<float> throughput_kbs{0.0f};
    float latency_ms = 3.8f;
    float buffer_fill_pct = 62.0f;
    std::atomic<uint64_t> total_frames{0};
    std::atomic<uint64_t> total_packets{0};
    std::atomic<uint32_t> underrun_count{0};
    int link_usage_pct = 68;
};

struct HardwareInfo {
    bool detected = false;
    std::string com_port = "";
    uint16_t vid = 0x2E8A;
    uint16_t pid = 0x4306;
    std::string serial_number = "HT306-RP2350Z-96K";
    std::string firmware_version = "v1.3.0";
    uint32_t uptime_sec = 0;
    std::string last_heartbeat_text = "Waiting for RP2350...";
    bool pio_ready = false;
    bool mux_high = false;
    bool dsp_locked = false;
};

// Zero-allocation POD DSP snapshot safe for high-frequency AudioCapture MMCSS thread reads
struct AudioDspSettings {
    bool bit_perfect_mode = false;
    bool mute_master = false;
    float master_volume_db = 0.0f;      // -60.0 dB to +6.0 dB
    bool limiter_enabled = true;

    // Channel trims (-12dB to +12dB)
    float trims[6] = { 0.0f, 0.0f, +1.5f, -2.0f, 0.0f, 0.0f };
    bool mutes[6] = { false, false, false, false, false, false };
    bool solos[6] = { false, false, false, false, false, false };
    bool phase_inverts[6] = { false, false, false, false, false, false };

    // 6x6 routing matrix (output_ch -> input_ch, default identity 0->0, 1->1, ...)
    int routing_map[6] = { 0, 1, 2, 3, 4, 5 };
    RoutingPreset routing_preset = RoutingPreset::DISCRETE_1TO1;
    float downmix_attenuation_db = -3.0f;
};

struct AppSettings : AudioDspSettings {
    int format_mode = 0;                // 0: Auto Windows, 1: Best, 2: Adaptive, 3: Manual
    int lip_sync_delay_ms = 24;         // 0 to 200 ms
    std::string active_profile = "Movie";
};

enum class LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
};

struct LogEntry {
    std::string timestamp;
    LogLevel level;
    std::string subsystem;
    std::string message;
};

struct AppState {
    std::atomic<ConnectionState> state{ConnectionState::DISCONNECTED};
    std::atomic<bool> is_streaming{false};

    // Leaf mutex protecting hw_info, meters[6], and settings across Audio/USB/UI threads.
    mutable std::mutex state_mutex;
    std::atomic<bool> hw_detected{false};
    HardwareInfo hw_info;
    StreamTelemetry telemetry;
    ChannelMeters meters[6]; // FL, FR, C, LFE, SL, SR
    AppSettings settings;

    mutable std::mutex devices_mutex;
    std::vector<AudioEndpointInfo> available_devices;
    std::atomic<int> active_device_index{0};
    std::wstring active_device_id;
    std::atomic<SampleRatePreference> sample_rate_mode{SampleRatePreference::LOCK_96K};
    std::atomic<uint32_t> capture_reinit_generation{0};

    // 5.1 Acoustic Sweep test state
    std::atomic<int> sweep_active_channel{-1}; // -1 = idle, 0..5 = FL, FR, C, LFE, SL, SR
    std::atomic<bool> sweep_running{false};
    std::atomic<int> test_signal_type{0};      // 0: Chirp Sweep, 1: Pink Noise, 2: Sine 1kHz, 3: Dual-Tone, 4: Polarity Pulse
    std::atomic<float> test_level_dbfs{-20.0f};

    // Diagnostics / Log
    mutable std::mutex log_mutex;
    std::deque<LogEntry> logs;
    std::atomic<uint64_t> log_revision{0};
    std::atomic<uint64_t> settings_revision{1};
    std::atomic<uint64_t> devices_revision{1};

    AppState();
    void AddLog(LogLevel level, const std::string& subsystem, const std::string& msg);
    void ClearLogs();
    uint64_t GetLogRevision() const { return log_revision.load(std::memory_order_acquire); }
    uint64_t GetSettingsRevision() const { return settings_revision.load(std::memory_order_acquire); }
    uint64_t GetDevicesRevision() const { return devices_revision.load(std::memory_order_acquire); }
    std::vector<LogEntry> GetRecentLogsSnapshot(size_t max_count) const;

    // Thread-safe snapshots and updates for settings, hw_info, and meters
    AppSettings GetSettingsSnapshot() const;
    AudioDspSettings GetAudioDspSettingsSnapshot() const;
    template <typename Fn>
    void UpdateSettings(Fn&& fn) {
        std::lock_guard<std::mutex> lock(state_mutex);
        fn(settings);
        settings_revision.fetch_add(1, std::memory_order_release);
    }

    bool IsHwDetected() const { return hw_detected.load(std::memory_order_acquire); }
    HardwareInfo GetHwInfoSnapshot() const;
    template <typename Fn, typename = std::enable_if_t<std::is_invocable_v<Fn, HardwareInfo&>>>
    void UpdateHwInfo(Fn&& fn) {
        std::lock_guard<std::mutex> lock(state_mutex);
        fn(hw_info);
        hw_detected.store(hw_info.detected, std::memory_order_release);
    }

    void GetMetersSnapshot(ChannelMeters out[6]) const;
    void UpdateMetersFromPeaks(const float ch_max[6], const bool ch_clipped[6]);
    void DecayMeters();
    void ResetMeterPeaks();

    std::vector<AudioEndpointInfo> GetAvailableDevicesSnapshot() const;
    void SetAvailableDevices(
        std::vector<AudioEndpointInfo> next_devices,
        const std::wstring& preferred_id_or_empty,
        bool auto_select_ht306
    );
    bool SelectDeviceByIndex(int idx, std::string* out_name = nullptr);
    bool SelectDeviceById(const std::wstring& id);
    AudioEndpointInfo GetActiveDeviceSnapshot(int* out_index = nullptr) const;
    void RequestCaptureReinit() { capture_reinit_generation.fetch_add(1, std::memory_order_release); }
};

} // namespace ht306
