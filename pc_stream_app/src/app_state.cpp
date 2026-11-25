#include "app_state.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ht306 {

AppState::AppState() {
    AddLog(LogLevel::LOG_INFO, "SYS_CORE", "HT306 Audio Bridge Engine initialized (24-bit Lossless Direct-I2S).");
}

void AppState::AddLog(LogLevel level, const std::string& subsystem, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms.count());

    std::lock_guard<std::mutex> lock(log_mutex);
    logs.push_back({ buf, level, subsystem, msg });
    if (logs.size() > 500) {
        logs.pop_front();
    }
    log_revision.fetch_add(1, std::memory_order_release);
}

void AppState::ClearLogs() {
    std::lock_guard<std::mutex> lock(log_mutex);
    logs.clear();
    log_revision.fetch_add(1, std::memory_order_release);
}

std::vector<LogEntry> AppState::GetRecentLogsSnapshot(size_t max_count) const {
    std::lock_guard<std::mutex> lock(log_mutex);
    size_t n = logs.size();
    size_t start = (n > max_count) ? (n - max_count) : 0;
    std::vector<LogEntry> out;
    out.reserve(n - start);
    for (size_t i = start; i < n; ++i) {
        out.push_back(logs[i]);
    }
    return out;
}

AppSettings AppState::GetSettingsSnapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex);
    return settings;
}

AudioDspSettings AppState::GetAudioDspSettingsSnapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex);
    return static_cast<const AudioDspSettings&>(settings);
}

HardwareInfo AppState::GetHwInfoSnapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex);
    return hw_info;
}

void AppState::GetMetersSnapshot(ChannelMeters out[6]) const {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (int i = 0; i < 6; ++i) {
        out[i] = meters[i];
    }
}

void AppState::UpdateMetersFromPeaks(const float ch_max[6], const bool ch_clipped[6]) {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (int c = 0; c < 6; ++c) {
        float peak = ch_max[c];
        float db = -60.0f;
        if (std::isfinite(peak) && peak > 1e-5f) {
            db = 20.0f * std::log10(peak);
        }
        if (!std::isfinite(db) || db < -60.0f) db = -60.0f;
        if (db > 0.0f) db = 0.0f;

        meters[c].peak_db = (meters[c].peak_db * 0.7f) + (db * 0.3f);
        if (meters[c].peak_db < -59.95f) meters[c].peak_db = -60.0f;
        if (db > meters[c].peak_hold_db) {
            meters[c].peak_hold_db = db;
        } else {
            meters[c].peak_hold_db = (std::max)(-60.0f, meters[c].peak_hold_db - 0.5f);
        }
        meters[c].clipped = ch_clipped[c];
    }
}

void AppState::DecayMeters() {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (int c = 0; c < 6; ++c) {
        meters[c].peak_db = (meters[c].peak_db * 0.7f) + (-60.0f * 0.3f);
        if (meters[c].peak_db < -59.9f) {
            meters[c].peak_db = -60.0f;
        }
        meters[c].peak_hold_db = (std::max)(-60.0f, meters[c].peak_hold_db - 0.5f);
        meters[c].clipped = false;
    }
}

void AppState::ResetMeterPeaks() {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (int c = 0; c < 6; ++c) {
        meters[c].peak_hold_db = -60.0f;
        meters[c].clipped = false;
    }
}

namespace {

int FindDeviceIndexById(const std::vector<AudioEndpointInfo>& list, const std::wstring& id) {
    if (id.empty()) return -1;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int FindVirtualHT306Index(const std::vector<AudioEndpointInfo>& list) {
    for (size_t i = 0; i < list.size(); ++i) {
        const std::string& name = list[i].name;
        if (name.find("16ch") != std::string::npos) {
            continue;
        }
        if (name.find("HT306") != std::string::npos ||
            name.find("CABLE") != std::string::npos ||
            name.find("Virtual") != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

std::vector<AudioEndpointInfo> AppState::GetAvailableDevicesSnapshot() const {
    std::lock_guard<std::mutex> lock(devices_mutex);
    return available_devices;
}

void AppState::SetAvailableDevices(
    std::vector<AudioEndpointInfo> next_devices,
    const std::wstring& preferred_id_or_empty,
    bool auto_select_ht306
) {
    std::lock_guard<std::mutex> lock(devices_mutex);
    available_devices = std::move(next_devices);

    if (available_devices.empty()) {
        active_device_id.clear();
        active_device_index.store(0);
        devices_revision.fetch_add(1, std::memory_order_release);
        return;
    }

    int target_idx = -1;
    if (!preferred_id_or_empty.empty()) {
        target_idx = FindDeviceIndexById(available_devices, preferred_id_or_empty);
    }
    if (target_idx < 0 && !active_device_id.empty()) {
        target_idx = FindDeviceIndexById(available_devices, active_device_id);
    }
    if (target_idx <= 0 && auto_select_ht306) {
        int ht_idx = FindVirtualHT306Index(available_devices);
        if (ht_idx > 0) {
            target_idx = ht_idx;
        }
    }
    if (target_idx < 0) {
        int cur = active_device_index.load();
        target_idx = (cur >= 0 && cur < static_cast<int>(available_devices.size())) ? cur : 0;
    }

    active_device_index.store(target_idx);
    active_device_id = available_devices[static_cast<size_t>(target_idx)].id;
    devices_revision.fetch_add(1, std::memory_order_release);
}

bool AppState::SelectDeviceByIndex(int idx, std::string* out_name) {
    bool found = false;
    std::lock_guard<std::mutex> lock(devices_mutex);
    if (idx >= 0 && idx < static_cast<int>(available_devices.size())) {
        active_device_index.store(idx);
        active_device_id = available_devices[static_cast<size_t>(idx)].id;
        if (out_name) {
            *out_name = available_devices[static_cast<size_t>(idx)].name;
        }
        found = true;
    } else {
        if (idx == 0 && available_devices.empty()) {
            active_device_index.store(0);
            active_device_id.clear();
        }
        if (out_name) {
            out_name->clear();
        }
    }
    devices_revision.fetch_add(1, std::memory_order_release);
    return found;
}

bool AppState::SelectDeviceById(const std::wstring& id) {
    std::lock_guard<std::mutex> lock(devices_mutex);
    active_device_id = id;
    int idx = FindDeviceIndexById(available_devices, id);
    if (idx >= 0) {
        active_device_index.store(idx);
        devices_revision.fetch_add(1, std::memory_order_release);
        return true;
    }
    devices_revision.fetch_add(1, std::memory_order_release);
    return false;
}

AudioEndpointInfo AppState::GetActiveDeviceSnapshot(int* out_index) const {
    std::lock_guard<std::mutex> lock(devices_mutex);
    if (available_devices.empty()) {
        if (out_index) *out_index = 0;
        AudioEndpointInfo empty_def{};
        empty_def.id = active_device_id;
        empty_def.name = "Default Windows Playback Device";
        empty_def.is_default = active_device_id.empty();
        return empty_def;
    }
    int idx = active_device_index.load();
    if (idx < 0 || idx >= static_cast<int>(available_devices.size())) {
        idx = 0;
    }
    if (out_index) {
        *out_index = idx;
    }
    return available_devices[static_cast<size_t>(idx)];
}

} // namespace ht306
