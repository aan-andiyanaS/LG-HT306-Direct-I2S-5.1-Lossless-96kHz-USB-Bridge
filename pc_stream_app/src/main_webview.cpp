// ============================================================================
// LG HT306 Audio Bridge - Direct I2S 96kHz Lossless
// WebView2 Desktop Application Host & Native IPC Bridge
// ============================================================================

#ifndef WEBVIEW_BUILD
#define WEBVIEW_BUILD 1
#endif
#ifndef WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL
#define WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL 1
#endif

#include <windows.h>
#include <dbt.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <shellapi.h>
#include "WebView2.h"
#include "webview/webview.h"
#include "app_state.h"
#include "audio_capture.h"
#include "usb_transport.h"
#include "audio_endpoint_helper.h"
#include "audio_policy_manager.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <tlhelp32.h>

#ifndef PBT_APMSUSPEND
#define PBT_APMSUSPEND 0x0004
#endif
#ifndef PBT_APMRESUMESUSPEND
#define PBT_APMRESUMESUSPEND 0x0007
#endif
#ifndef PBT_APMRESUMEAUTOMATIC
#define PBT_APMRESUMEAUTOMATIC 0x0012
#endif

namespace ht306 {
    using AudioCapture = audio::AudioCapture;
    using UsbTransport = usb::UsbTransport;
}

// ----------------------------------------------------------------------------
// Global Instances
// ----------------------------------------------------------------------------
ht306::AppState g_State;
ht306::UsbTransport g_UsbTransport(g_State);
ht306::AudioCapture g_AudioCapture(g_State, g_UsbTransport);
std::atomic<bool> g_Running{true};
std::atomic<bool> g_TelemetryDirty{true};
std::atomic<bool> g_FullSnapshotDirty{true};
std::atomic<HWND> g_MainHwnd{NULL};
webview::webview* g_pWebview = nullptr;
ICoreWebView2* g_pCoreWebView2 = nullptr;

enum class PolicyCmd : uint8_t {
    NONE = 0,
    SWITCH_HT306,
    RESTORE_DEFAULT,
    DEBUG_ON,
    DEBUG_OFF,
    SYNC_NAMES
};

struct PolicyAction {
    PolicyCmd cmd = PolicyCmd::NONE;
    int rate_pref = -1; // -1 = none, 0 = AUTO, 1 = LOCK_96K, 2 = LOCK_48K
    bool refresh_devices = false;
};

std::mutex g_PolicyMutex;
std::condition_variable g_PolicyCv;
std::deque<PolicyAction> g_PolicyQueue;

// Tracked auxiliary background threads (joined on exit — zero detached threads)
std::mutex g_AuxThreadMutex;
std::thread g_InstallThread;
std::atomic<bool> g_InstallInProgress{false};
std::thread g_PostBootTrimThread;
std::mutex g_PostBootTrimMutex;
std::condition_variable g_PostBootTrimCv;
std::atomic<uint32_t> g_PostBootTrimGen{0};

void EnqueuePolicyAction(PolicyCmd cmd, int rate_pref = -1, bool refresh_devices = false) {
    {
        std::lock_guard<std::mutex> lock(g_PolicyMutex);
        // Coalesce consecutive pure device-refresh requests during USB hotplug storms
        if (cmd == PolicyCmd::NONE && rate_pref < 0 && refresh_devices && !g_PolicyQueue.empty()) {
            const auto& back = g_PolicyQueue.back();
            if (back.cmd == PolicyCmd::NONE && back.rate_pref < 0 && back.refresh_devices) {
                return;
            }
        }
        g_PolicyQueue.push_back(PolicyAction{ cmd, rate_pref, refresh_devices });
    }
    g_PolicyCv.notify_one();
}

void SignalShutdown() {
    {
        std::lock_guard<std::mutex> lock(g_PolicyMutex);
        g_Running.store(false);
    }
    g_PolicyCv.notify_all();
    {
        std::lock_guard<std::mutex> lock(g_PostBootTrimMutex);
    }
    g_PostBootTrimCv.notify_all();
}

void JoinAuxThreads() {
    {
        std::lock_guard<std::mutex> lock(g_PostBootTrimMutex);
    }
    g_PostBootTrimCv.notify_all();
    std::thread trim_to_join;
    std::thread install_to_join;
    {
        std::lock_guard<std::mutex> lock(g_AuxThreadMutex);
        if (g_PostBootTrimThread.joinable()) {
            trim_to_join = std::move(g_PostBootTrimThread);
        }
        if (g_InstallThread.joinable()) {
            install_to_join = std::move(g_InstallThread);
        }
    }
    if (trim_to_join.joinable()) {
        trim_to_join.join();
    }
    if (install_to_join.joinable()) {
        install_to_join.join();
    }
}

void PolicyWorkerLoop() {
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    while (g_Running.load()) {
        PolicyAction action{};

        {
            std::unique_lock<std::mutex> lock(g_PolicyMutex);
            g_PolicyCv.wait(lock, []() {
                return !g_Running.load() || !g_PolicyQueue.empty();
            });
            if (!g_Running.load()) break;

            action = g_PolicyQueue.front();
            g_PolicyQueue.pop_front();
        }

        const PolicyCmd cmd = action.cmd;
        const int rate_pref = action.rate_pref;
        const bool refresh_devices = action.refresh_devices;

        if (cmd == PolicyCmd::SWITCH_HT306) {
            if (!g_State.IsHwDetected()) {
                ht306::audio::AudioPolicyManager::Instance().SetManualVisibilityOverride(true);
            }
            bool ok = ht306::audio::AudioPolicyManager::Instance().SwitchToHT306(g_State);
            ht306::audio::AudioEndpointHelper::EnumerateDevices(g_State);
            if (ok) {
                g_State.AddLog(ht306::LogLevel::LOG_INFO, "AudioPolicy",
                    "Switched Windows taskbar default audio device to LG HT306");
            }
        } else if (cmd == PolicyCmd::RESTORE_DEFAULT) {
            ht306::audio::AudioPolicyManager::Instance().SetManualVisibilityOverride(false);
            ht306::audio::AudioPolicyManager::Instance().RestorePreviousDefault(g_State);
            ht306::audio::AudioEndpointHelper::EnumerateDevices(g_State);
        } else if (cmd == PolicyCmd::DEBUG_ON) {
            ht306::audio::AudioPolicyManager::Instance().SetManualVisibilityOverride(true);
            if (ht306::audio::AudioPolicyManager::Instance().SwitchToHT306(g_State)) {
                g_State.AddLog(ht306::LogLevel::LOG_INFO, "DebugAudio",
                    "Mode Debug Aktif: LG HT306 Home Theater diaktifkan tanpa RP2350");
            }
            ht306::audio::AudioEndpointHelper::EnumerateDevices(g_State);
        } else if (cmd == PolicyCmd::DEBUG_OFF) {
            ht306::audio::AudioPolicyManager::Instance().SetManualVisibilityOverride(false);
            if (!g_UsbTransport.CheckHardwarePresence()) {
                ht306::audio::AudioPolicyManager::Instance().RestorePreviousDefault(g_State);
                g_State.AddLog(ht306::LogLevel::LOG_INFO, "DebugAudio",
                    "Mode Debug Nonaktif: LG HT306 Home Theater disembunyikan dari Taskbar");
            } else {
                g_State.AddLog(ht306::LogLevel::LOG_INFO, "DebugAudio",
                    "Mode Debug Nonaktif (RP2350 masih terhubung)");
            }
            ht306::audio::AudioEndpointHelper::EnumerateDevices(g_State);
        } else if (cmd == PolicyCmd::SYNC_NAMES) {
            bool ok = ht306::audio::AudioPolicyManager::Instance().SyncEndpointNames();
            if (ok) {
                ht306::audio::AudioEndpointHelper::EnumerateDevices(g_State);
                ht306::audio::AudioPolicyManager::Instance().SwitchToHT306(g_State);
                g_State.AddLog(ht306::LogLevel::LOG_INFO, "Driver",
                    "Nama audio taskbar berhasil diubah menjadi: LG HT306 Home Theater");
            } else {
                g_State.AddLog(ht306::LogLevel::LOG_WARN, "Driver",
                    "Gagal atau dibatalkan menyinkronkan nama taskbar.");
            }
        }

        if (rate_pref == static_cast<int>(ht306::SampleRatePreference::LOCK_48K) ||
            rate_pref == static_cast<int>(ht306::SampleRatePreference::LOCK_96K)) {
            const uint32_t target_sr = (rate_pref == static_cast<int>(ht306::SampleRatePreference::LOCK_48K))
                ? 48000u : 96000u;
            const bool was_capturing = g_AudioCapture.IsRunning();
            if (was_capturing) {
                g_AudioCapture.Stop();
            }
            ht306::audio::AudioPolicyManager::Instance().ApplySampleRateToHT306Endpoints(g_State, target_sr);
            if (was_capturing && g_Running.load()) {
                g_AudioCapture.Start();
            }
        } else if (rate_pref == static_cast<int>(ht306::SampleRatePreference::AUTO)) {
            g_State.RequestCaptureReinit();
        }

        if (refresh_devices) {
            ht306::audio::AudioPolicyManager::Instance().CheckDriverStatus();
            ht306::audio::AudioEndpointHelper::EnumerateDevices(g_State);
        }

        g_TelemetryDirty.store(true);
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
}


// ----------------------------------------------------------------------------
// Child WebView2 Working Set Trimmer (never evict our own real-time audio process)
// ----------------------------------------------------------------------------
void TrimWorkingSetTree() {
    DWORD myPid = GetCurrentProcessId();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        std::vector<DWORD> pids;
        if (Process32First(hSnap, &pe)) {
            do {
                if (pe.th32ParentProcessID == myPid) {
                    pids.push_back(pe.th32ProcessID);
                }
            } while (Process32Next(hSnap, &pe));
        }
        const size_t direct_count = pids.size();
        if (direct_count > 0 && Process32First(hSnap, &pe)) {
            do {
                for (size_t i = 0; i < direct_count; ++i) {
                    if (pe.th32ParentProcessID == pids[i]) {
                        pids.push_back(pe.th32ProcessID);
                        break;
                    }
                }
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);

        for (DWORD pid : pids) {
            HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (hProc) {
                SetProcessWorkingSetSize(hProc, (SIZE_T)-1, (SIZE_T)-1);
                CloseHandle(hProc);
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Win32 Theming & Helper Functions
// ----------------------------------------------------------------------------
void EnableImmersiveDarkMode(HWND hwnd) {
    if (!hwnd) return;
    BOOL darkMode = TRUE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE (20 on Win10 2004+, 19 on earlier builds)
    if (FAILED(DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode)))) {
        DwmSetWindowAttribute(hwnd, 19 /* DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 */, &darkMode, sizeof(darkMode));
    }
}

static std::string ResolveUiHtmlPath() {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecA(exePath);

    char candidate[MAX_PATH] = { 0 };

    // 1. In <exe_dir>\ui\index.html
    PathCombineA(candidate, exePath, "ui\\index.html");
    if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
        return candidate;
    }

    // 2. In <exe_dir>\..\ui\index.html (when running from build/)
    PathCombineA(candidate, exePath, "..\\ui\\index.html");
    if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
        char full[MAX_PATH] = { 0 };
        GetFullPathNameA(candidate, MAX_PATH, full, NULL);
        return full;
    }

    // 3. Current working directory: ui\index.html
    if (GetFileAttributesA("ui\\index.html") != INVALID_FILE_ATTRIBUTES) {
        char full[MAX_PATH] = { 0 };
        GetFullPathNameA("ui\\index.html", MAX_PATH, full, NULL);
        return full;
    }

    // 4. pc_stream_app\ui\index.html
    if (GetFileAttributesA("pc_stream_app\\ui\\index.html") != INVALID_FILE_ATTRIBUTES) {
        char full[MAX_PATH] = { 0 };
        GetFullPathNameA("pc_stream_app\\ui\\index.html", MAX_PATH, full, NULL);
        return full;
    }

    // 5. Absolute fallback path
    const char* fallback = "E:\\Project\\HomeTheather\\pc_stream_app\\ui\\index.html";
    if (GetFileAttributesA(fallback) != INVALID_FILE_ATTRIBUTES) {
        return fallback;
    }

    return "ui\\index.html";
}

static std::string PathToUrl(const std::string& path) {
    char full[MAX_PATH] = { 0 };
    GetFullPathNameA(path.c_str(), MAX_PATH, full, NULL);
    std::string s = full;
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return "file:///" + s;
}

// ----------------------------------------------------------------------------
// JSON Unwrapping & Parsing Utilities
// ----------------------------------------------------------------------------
static std::string UnwrapJsonRpc(const std::string& input) {
    std::string s = input;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();

    // If it's a JSON array [ ... ]
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        s = s.substr(1, s.size() - 2);
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.erase(0, 1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    }
    // If it's a quoted string "..."
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        int n = webview::detail::json_unescape(s.c_str(), s.length(), nullptr);
        if (n > 0) {
            std::string unesc(n, '\0');
            webview::detail::json_unescape(s.c_str(), s.length(), &unesc[0]);
            s = unesc;
        }
    }
    return s;
}

static std::string EscapeJsonString(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char uc : input) {
        char c = static_cast<char>(uc);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (uc < 0x20u) {
            char hex[7];
            std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned int>(uc));
            out += hex;
        } else {
            out += c;
        }
    }
    return out;
}

static inline float SafeJsonFloat(float v, float fallback = 0.0f) {
    return std::isfinite(v) ? v : fallback;
}

static bool JsonGetInt(const std::string& s, const std::string& key, int& out_val) {
    std::string v = webview::detail::json_parse(s, key, 0);
    if (v.empty()) return false;
    try {
        out_val = std::stoi(v);
        return true;
    } catch (...) {
        return false;
    }
}

static bool JsonGetFloat(const std::string& s, const std::string& key, float& out_val) {
    std::string v = webview::detail::json_parse(s, key, 0);
    if (v.empty()) return false;
    try {
        float parsed = std::stof(v);
        if (!std::isfinite(parsed)) return false;
        out_val = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

static bool JsonGetBool(const std::string& s, const std::string& key, bool& out_val) {
    std::string v = webview::detail::json_parse(s, key, 0);
    if (v.empty()) return false;
    if (v == "true" || v == "1") {
        out_val = true;
        return true;
    }
    if (v == "false" || v == "0") {
        out_val = false;
        return true;
    }
    return false;
}

static int ParseChannelIndex(const std::string& ch_str) {
    if (ch_str.empty()) return -1;
    if (ch_str == "FL"  || ch_str == "fl"  || ch_str == "0") return 0;
    if (ch_str == "FR"  || ch_str == "fr"  || ch_str == "1") return 1;
    if (ch_str == "C"   || ch_str == "c"   || ch_str == "2") return 2;
    if (ch_str == "LFE" || ch_str == "lfe" || ch_str == "SW" || ch_str == "sw" || ch_str == "3") return 3;
    if (ch_str == "SL"  || ch_str == "sl"  || ch_str == "4") return 4;
    if (ch_str == "SR"  || ch_str == "sr"  || ch_str == "5") return 5;
    try {
        int idx = std::stoi(ch_str);
        if (idx >= 0 && idx < 6) return idx;
    } catch (...) {}
    return -1;
}

static bool ParseRoutingMap(const std::string& json, int map_out[6]) {
    size_t pos = json.find("\"routing_map\"");
    if (pos == std::string::npos) pos = json.find("routing_map");
    if (pos == std::string::npos) return false;

    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return false;

    size_t obj_start = json.find('{', colon);
    size_t arr_start = json.find('[', colon);

    if (arr_start != std::string::npos && (obj_start == std::string::npos || arr_start < obj_start)) {
        size_t arr_end = json.find(']', arr_start);
        if (arr_end != std::string::npos) {
            std::string sub = json.substr(arr_start + 1, arr_end - arr_start - 1);
            std::stringstream ss(sub);
            std::string item;
            int idx = 0;
            while (std::getline(ss, item, ',') && idx < 6) {
                try {
                    int val = std::stoi(item);
                    if (val >= 0 && val < 6) {
                        map_out[idx] = val;
                    }
                    idx++;
                } catch (...) {}
            }
            return (idx == 6);
        }
    } else if (obj_start != std::string::npos) {
        size_t obj_end = json.find('}', obj_start);
        if (obj_end != std::string::npos) {
            std::string sub = json.substr(obj_start, obj_end - obj_start + 1);
            for (int i = 0; i < 6; ++i) {
                std::string k1 = "\"" + std::to_string(i) + "\"";
                size_t kp = sub.find(k1);
                if (kp == std::string::npos) {
                    kp = sub.find(std::to_string(i) + ":");
                }
                if (kp != std::string::npos) {
                    size_t c = sub.find(':', kp);
                    if (c != std::string::npos) {
                        size_t val_start = sub.find_first_of("012345", c);
                        if (val_start != std::string::npos && val_start < sub.size()) {
                            map_out[i] = sub[val_start] - '0';
                        }
                    }
                }
            }
            return true;
        }
    }
    return false;
}


// ----------------------------------------------------------------------------
// IPC Command Handler
// ----------------------------------------------------------------------------
void HandleNativeCommand(const std::string& raw_json_str) {
    std::string json = UnwrapJsonRpc(raw_json_str);
    std::string action = webview::detail::json_parse(json, "action", 0);
    if (action.empty()) {
        action = webview::detail::json_parse(json, "cmd", 0);
    }

    if (action == "toggle_stream") {
        bool next_stream = !g_State.is_streaming.load();
        std::string str_val = webview::detail::json_parse(json, "streaming", 0);
        if (str_val == "true" || str_val == "1") {
            next_stream = true;
        } else if (str_val == "false" || str_val == "0") {
            next_stream = false;
        }

        if (next_stream) {
            g_AudioCapture.Start();
            g_State.is_streaming.store(true);
            g_State.state.store(ht306::ConnectionState::STREAMING);
            uint32_t cur_sr = g_State.telemetry.sample_rate.load();
            if (cur_sr == 0) {
                cur_sr = (g_State.sample_rate_mode.load() == ht306::SampleRatePreference::LOCK_48K) ? 48000u : 96000u;
            }
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Stream",
                "Audio stream started (" + std::to_string(cur_sr / 1000) + "kHz 5.1)");
        } else {
            g_AudioCapture.Stop();
            g_State.is_streaming.store(false);
            g_State.telemetry.throughput_kbs.store(0.0f);
            g_State.state.store(g_UsbTransport.IsOpen() ? ht306::ConnectionState::READY : ht306::ConnectionState::DISCONNECTED);
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Stream", "Audio stream stopped");
        }
    }
    else if (action == "set_volume" || action == "set_master_volume") {
        float vol = 0.0f;
        bool has_vol = (JsonGetFloat(json, "volume_db", vol) || JsonGetFloat(json, "volume", vol) || JsonGetFloat(json, "val", vol));
        if (has_vol) {
            if (vol < -60.0f) vol = -60.0f;
            if (vol > 6.0f) vol = 6.0f;
        }
        bool muted = false;
        bool has_muted = JsonGetBool(json, "muted", muted);
        if (has_vol || has_muted) {
            g_State.UpdateSettings([&](ht306::AppSettings& s) {
                if (has_vol) s.master_volume_db = vol;
                if (has_muted) s.mute_master = muted;
            });
        }
    }
    else if (action == "set_trim" || action == "set_gain") {
        std::string ch_str = webview::detail::json_parse(json, "channel", 0);
        int ch_idx = ParseChannelIndex(ch_str);
        float gain = 0.0f;
        bool has_gain = false;
        if (ch_idx >= 0 && ch_idx < 6) {
            if (JsonGetFloat(json, "gain_db", gain) || JsonGetFloat(json, "trim_db", gain) || JsonGetFloat(json, "val", gain)) {
                if (gain < -12.0f) gain = -12.0f;
                if (gain > 12.0f) gain = 12.0f;
                has_gain = true;
            }
        }
        bool reset_val = false;
        bool do_reset = (JsonGetBool(json, "reset", reset_val) && reset_val);
        if (has_gain || do_reset) {
            g_State.UpdateSettings([&](ht306::AppSettings& s) {
                if (has_gain) s.trims[ch_idx] = gain;
                if (do_reset) {
                    for (int i = 0; i < 6; ++i) s.trims[i] = 0.0f;
                }
            });
        }
    }
    else if (action == "set_mute" || action == "set_mutes") {
        std::string ch_str = webview::detail::json_parse(json, "channel", 0);
        int ch_idx = ParseChannelIndex(ch_str);
        bool muted_val = false;
        if (ch_idx >= 0 && ch_idx < 6 && JsonGetBool(json, "muted", muted_val)) {
            g_State.UpdateSettings([&](ht306::AppSettings& s) {
                s.mutes[ch_idx] = muted_val;
            });
        }
    }
    else if (action == "set_solo") {
        std::string ch_str = webview::detail::json_parse(json, "channel", 0);
        int ch_idx = ParseChannelIndex(ch_str);
        bool solo_val = false;
        if (ch_idx >= 0 && ch_idx < 6 && JsonGetBool(json, "solo", solo_val)) {
            g_State.UpdateSettings([&](ht306::AppSettings& s) {
                s.solos[ch_idx] = solo_val;
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Mixer", "Channel " + std::to_string(ch_idx) + (solo_val ? " solo enabled" : " solo disabled"));
        }
    }
    else if (action == "set_phase") {
        std::string ch_str = webview::detail::json_parse(json, "channel", 0);
        int ch_idx = ParseChannelIndex(ch_str);
        bool invert_val = false;
        if (ch_idx >= 0 && ch_idx < 6 && JsonGetBool(json, "invert", invert_val)) {
            g_State.UpdateSettings([&](ht306::AppSettings& s) {
                s.phase_inverts[ch_idx] = invert_val;
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Mixer", "Channel " + std::to_string(ch_idx) + (invert_val ? " phase inverted (180 deg)" : " phase normal (0 deg)"));
        }
    }
    else if (action == "set_test_signal") {
        int sig_type = 0;
        if (JsonGetInt(json, "signal_type", sig_type) || JsonGetInt(json, "type", sig_type)) {
            g_State.test_signal_type.store(sig_type);
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "SweepTest", "Test signal type set to " + std::to_string(sig_type));
        }
    }
    else if (action == "set_mode") {
        std::string mode = webview::detail::json_parse(json, "mode", 0);
        bool bitperfect = (mode == "bitperfect");
        bool bp_field = false;
        if (JsonGetBool(json, "bit_perfect_mode", bp_field)) {
            bitperfect = bp_field;
        }
        g_State.UpdateSettings([&](ht306::AppSettings& s) {
            s.bit_perfect_mode = bitperfect;
        });
        g_State.AddLog(ht306::LogLevel::LOG_INFO, "DSP", bitperfect ? "Switched to Bit-Perfect Pass-thru mode" : "Switched to Desktop OS-Linked mode");
    }
    else if (action == "test_sweep" || action == "sweep_test") {
        std::string sw_action = webview::detail::json_parse(json, "sweep_action", 0);
        if (sw_action.empty()) sw_action = webview::detail::json_parse(json, "sub_action", 0);
        if (sw_action.empty()) sw_action = webview::detail::json_parse(json, "cmd", 0);
        if (sw_action.empty()) sw_action = webview::detail::json_parse(json, "action", 0);
        if (sw_action == "test_sweep" || sw_action == "sweep_test") sw_action = "start";

        std::string ch_str = webview::detail::json_parse(json, "channel", 0);
        int ch_idx = ParseChannelIndex(ch_str);
        float lvl = g_State.test_level_dbfs.load();
        if (JsonGetFloat(json, "level_db", lvl)) {
            lvl = (std::clamp)(lvl, -60.0f, 0.0f);
        }

        int sig_type = 1;
        if (JsonGetInt(json, "signal_type", sig_type)) {
            g_State.test_signal_type.store(sig_type);
        }

        if (sw_action == "stop" || sw_action == "pause") {
            g_State.sweep_running.store(false);
            g_State.sweep_active_channel.store(-1);
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "SweepTest", "5.1 acoustic sweep paused/idle");
        } else {
            if (ch_idx < 0) ch_idx = 0;
            // Set test_level_dbfs BEFORE activating sweep channel so audio thread never reads stale level
            g_State.test_level_dbfs.store(lvl);
            g_State.sweep_active_channel.store(ch_idx);
            g_State.sweep_running.store(true);
            if (!g_AudioCapture.IsRunning()) {
                g_AudioCapture.Start();
                g_State.is_streaming.store(true);
                g_State.state.store(ht306::ConnectionState::STREAMING);
            }
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "SweepTest", "Acoustic test signal (" + std::to_string(g_State.test_signal_type.load()) + ") active on channel " + std::to_string(ch_idx));
        }
    }
    else if (action == "install_driver" || action == "install_audio_driver") {
        if (g_InstallInProgress.exchange(true)) {
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Driver", "Driver installation is already in progress.");
        } else {
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Driver", "Starting background installation for LG HT306 Virtual Audio Driver (WHQL)...");
            std::thread prev_thread;
            {
                std::lock_guard<std::mutex> lock(g_AuxThreadMutex);
                if (g_InstallThread.joinable()) {
                    prev_thread = std::move(g_InstallThread);
                }
                g_InstallThread = std::thread([]() {
                    // Never pass UI HWND across threads (avoids invalid/destroyed HWND in MessageBoxA)
                    bool ok = ht306::audio::AudioPolicyManager::Instance().InstallDriver(NULL, &g_Running);
                    g_InstallInProgress.store(false);
                    if (!g_Running.load()) return;
                    if (ok) {
                        g_State.AddLog(ht306::LogLevel::LOG_INFO, "Driver", "LG HT306 Audio Driver successfully installed & set as default playback device!");
                        EnqueuePolicyAction(PolicyCmd::SWITCH_HT306, -1, true);
                    } else {
                        g_State.AddLog(ht306::LogLevel::LOG_WARN, "Driver", "Driver installation cancelled or not completed.");
                        g_TelemetryDirty.store(true);
                    }
                });
            }
            if (prev_thread.joinable()) {
                prev_thread.join();
            }
        }
    }
    else if (action == "sync_audio_names" || action == "rename_audio_device") {
        g_State.AddLog(ht306::LogLevel::LOG_INFO, "Driver", "Menyinkronkan penamaan Windows Taskbar ke 'LG HT306 Home Theater'...");
        EnqueuePolicyAction(PolicyCmd::SYNC_NAMES);
    }
    else if (action == "switch_to_ht306" || action == "set_taskbar_default" || action == "force_show_ht306") {
        EnqueuePolicyAction(PolicyCmd::SWITCH_HT306);
    }
    else if (action == "restore_default_playback" || action == "hide_ht306_endpoint") {
        EnqueuePolicyAction(PolicyCmd::RESTORE_DEFAULT);
    }
    else if (action == "toggle_debug_audio" || action == "set_debug_audio") {
        bool cur_override = ht306::audio::AudioPolicyManager::Instance().GetManualVisibilityOverride();
        bool next_override = !cur_override;
        JsonGetBool(json, "enabled", next_override);
        JsonGetBool(json, "enable", next_override);
        EnqueuePolicyAction(next_override ? PolicyCmd::DEBUG_ON : PolicyCmd::DEBUG_OFF);
    }
    else if (action == "reboot_bootsel") {
        bool ok = g_UsbTransport.RebootBootsel();
        g_State.AddLog(ok ? ht306::LogLevel::LOG_WARN : ht306::LogLevel::LOG_ERROR, "RP2350", ok ? "Rebooted RP2350 to BOOTSEL mode via CDC-ACM 1200 baud touch" : "Failed to reboot RP2350 to BOOTSEL");
    }
    else if (action == "set_profile") {
        std::string prof = webview::detail::json_parse(json, "profile", 0);
        if (prof.empty()) prof = webview::detail::json_parse(json, "name", 0);
        if (!prof.empty()) {
            g_State.UpdateSettings([&](ht306::AppSettings& s) {
                s.active_profile = prof;
                for (int i = 0; i < 6; ++i) s.routing_map[i] = i;
                if (prof.find("Movie") != std::string::npos) {
                    float t[6] = {0.0f, 0.0f, 1.5f, 3.0f, 0.0f, 0.0f};
                    for (int i = 0; i < 6; ++i) s.trims[i] = t[i];
                    s.bit_perfect_mode = false;
                    s.limiter_enabled = true;
                    s.routing_preset = ht306::RoutingPreset::DISCRETE_1TO1;
                } else if (prof.find("Music") != std::string::npos) {
                    float t[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                    for (int i = 0; i < 6; ++i) s.trims[i] = t[i];
                    s.bit_perfect_mode = false;
                    s.limiter_enabled = true;
                    s.routing_preset = ht306::RoutingPreset::STEREO_UPMIX;
                } else if (prof.find("Game") != std::string::npos || prof.find("Gaming") != std::string::npos) {
                    float t[6] = {0.0f, 0.0f, 1.0f, 2.0f, 2.5f, 2.5f};
                    for (int i = 0; i < 6; ++i) s.trims[i] = t[i];
                    s.bit_perfect_mode = false;
                    s.limiter_enabled = true;
                    s.routing_preset = ht306::RoutingPreset::DISCRETE_1TO1;
                } else if (prof.find("Bit-Perfect") != std::string::npos || prof.find("Pass-Through") != std::string::npos || prof.find("bitperfect") != std::string::npos || prof.find("Direct") != std::string::npos) {
                    float t[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                    for (int i = 0; i < 6; ++i) s.trims[i] = t[i];
                    s.bit_perfect_mode = true;
                    s.limiter_enabled = false;
                    s.routing_preset = ht306::RoutingPreset::DISCRETE_1TO1;
                }
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Profiles", "Activated profile: " + prof);
        }
    }
    else if (action == "set_routing_preset") {
        std::string preset = webview::detail::json_parse(json, "preset", 0);
        if (preset == "standard" || preset == "discrete") {
            g_State.UpdateSettings([](ht306::AppSettings& s) {
                s.routing_preset = ht306::RoutingPreset::DISCRETE_1TO1;
                int m[6] = { 0, 1, 2, 3, 4, 5 };
                for (int i = 0; i < 6; ++i) s.routing_map[i] = m[i];
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Routing", "Applied routing preset: standard (1-to-1 Discrete)");
        } else if (preset == "swapped") {
            g_State.UpdateSettings([](ht306::AppSettings& s) {
                s.routing_preset = ht306::RoutingPreset::DISCRETE_1TO1;
                int m[6] = { 0, 1, 2, 3, 5, 4 };
                for (int i = 0; i < 6; ++i) s.routing_map[i] = m[i];
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Routing", "Applied routing preset: swapped (SL <-> SR)");
        } else if (preset == "all-stereo") {
            g_State.UpdateSettings([](ht306::AppSettings& s) {
                s.routing_preset = ht306::RoutingPreset::DUAL_MONO;
                int m[6] = { 0, 1, 0, 1, 0, 1 };
                for (int i = 0; i < 6; ++i) s.routing_map[i] = m[i];
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Routing", "Applied routing preset: all-stereo (Dual Mono)");
        } else if (preset == "stereo-upmix" || preset == "stereo" || preset == "upmix") {
            g_State.UpdateSettings([](ht306::AppSettings& s) {
                s.routing_preset = ht306::RoutingPreset::STEREO_UPMIX;
                int m[6] = { 0, 1, 2, 3, 4, 5 };
                for (int i = 0; i < 6; ++i) s.routing_map[i] = m[i];
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Routing", "Applied routing preset: stereo-upmix (Stereo Upmix)");
        }
    }
    else if (action == "set_routing") {
        auto cur_settings = g_State.GetSettingsSnapshot();
        int parsed_map[6];
        for (int i = 0; i < 6; ++i) {
            parsed_map[i] = cur_settings.routing_map[i];
        }
        if (ParseRoutingMap(json, parsed_map)) {
            g_State.UpdateSettings([&](ht306::AppSettings& s) {
                for (int i = 0; i < 6; ++i) {
                    s.routing_map[i] = (std::clamp)(parsed_map[i], 0, 5);
                }
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Routing", "Updated 6x6 channel routing matrix");
        }
    }
    else if (action == "swap_surround_channels") {
        g_State.UpdateSettings([](ht306::AppSettings& s) {
            std::swap(s.routing_map[4], s.routing_map[5]);
            std::swap(s.trims[4], s.trims[5]);
        });
        g_State.AddLog(ht306::LogLevel::LOG_INFO, "Routing", "Swapped surround channels (SL <-> SR)");
    }
    else if (action == "set_device" || action == "set_audio_device") {
        int dev_idx = 0;
        if (JsonGetInt(json, "device_index", dev_idx) || JsonGetInt(json, "index", dev_idx) || JsonGetInt(json, "dev", dev_idx)) {
            std::string dev_name;
            if (g_State.SelectDeviceByIndex(dev_idx, &dev_name)) {
                g_State.RequestCaptureReinit();
                g_State.AddLog(ht306::LogLevel::LOG_INFO, "AudioDevice", "Active device changed to [" + std::to_string(dev_idx) + "]: " + dev_name);
            } else {
                g_State.AddLog(ht306::LogLevel::LOG_WARN, "AudioDevice", "Invalid audio device index: " + std::to_string(dev_idx));
            }
        }
    }
    else if (action == "set_format_mode") {
        int fmt = 0;
        std::string str_mode = webview::detail::json_parse(json, "format_mode", 0);
        if (!str_mode.empty()) {
            if (str_mode == "0" || str_mode == "auto" || str_mode == "Auto: Windows" || str_mode == "Auto") {
                fmt = 0;
            } else if (str_mode == "1" || str_mode == "best" || str_mode == "Best") {
                fmt = 1;
            } else if (str_mode == "2" || str_mode == "adaptive" || str_mode == "Adaptive") {
                fmt = 2;
            } else if (str_mode == "3" || str_mode == "manual" || str_mode == "Manual") {
                fmt = 3;
            } else {
                try { fmt = std::stoi(str_mode); } catch (...) {}
            }
        } else {
            JsonGetInt(json, "mode", fmt);
        }
        if (fmt < 0) fmt = 0;
        if (fmt > 3) fmt = 3;
        g_State.UpdateSettings([&](ht306::AppSettings& s) {
            s.format_mode = fmt;
        });
        g_State.AddLog(ht306::LogLevel::LOG_INFO, "Format", "Format mode set to " + std::to_string(fmt));
    }
    else if (action == "select_sample_rate" || action == "set_sample_rate") {
        std::string mode = webview::detail::json_parse(json, "mode", 0);
        int rate = 0;
        JsonGetInt(json, "rate", rate);
        std::string mode_lower = mode;
        std::transform(mode_lower.begin(), mode_lower.end(), mode_lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        ht306::SampleRatePreference pref = ht306::SampleRatePreference::LOCK_96K;
        std::string label = "96 kHz (Lock)";
        if (mode_lower == "48k" || mode_lower == "48000" || mode_lower == "lock_48k" || (mode_lower.empty() && rate == 48000)) {
            pref = ht306::SampleRatePreference::LOCK_48K;
            label = "48 kHz (Lock)";
        }

        g_State.sample_rate_mode.store(pref);
        EnqueuePolicyAction(PolicyCmd::NONE, static_cast<int>(pref), false);
        g_State.AddLog(ht306::LogLevel::LOG_INFO, "Format", "Sample rate mode: " + label);
    }
    else if (action == "set_limiter") {
        bool lim = true;
        if (JsonGetBool(json, "limiter_enabled", lim) || JsonGetBool(json, "enabled", lim) || JsonGetBool(json, "limiter", lim)) {
            g_State.UpdateSettings([&](ht306::AppSettings& s) {
                s.limiter_enabled = lim;
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "DSP", lim ? "Master peak limiter enabled" : "Master peak limiter disabled");
        }
    }
    else if (action == "set_lipsync" || action == "set_delay") {
        int delay = 0;
        if (JsonGetInt(json, "lip_sync_delay_ms", delay) || JsonGetInt(json, "delay_ms", delay) || JsonGetInt(json, "lipsync_ms", delay) || JsonGetInt(json, "delay", delay)) {
            if (delay < 0) delay = 0;
            if (delay > 200) delay = 200;
            g_State.UpdateSettings([&](ht306::AppSettings& s) {
                s.lip_sync_delay_ms = delay;
            });
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "DSP", "Lip-sync delay set to " + std::to_string(delay) + " ms");
        }
    }
    else if (action == "factory_reset") {
        g_State.UpdateSettings([](ht306::AppSettings& s) {
            s = ht306::AppSettings();
        });
        g_State.sample_rate_mode.store(ht306::SampleRatePreference::LOCK_96K);
        g_State.SelectDeviceByIndex(0);
        EnqueuePolicyAction(PolicyCmd::NONE, static_cast<int>(ht306::SampleRatePreference::LOCK_96K), true);
        g_State.AddLog(ht306::LogLevel::LOG_WARN, "Config", "Settings reset to factory defaults");
    }
    else if (action == "save_config") {
        g_State.AddLog(ht306::LogLevel::LOG_INFO, "Config", "Configuration saved & synced to hardware bridge");
    }
    else if (action == "reset_buffer") {
        g_State.telemetry.underrun_count.store(0);
        g_State.AddLog(ht306::LogLevel::LOG_INFO, "Buffer", "Audio FIFO buffer and telemetry counters reset");
    }
    else if (action == "reset_peaks") {
        g_State.ResetMeterPeaks();
    }
    else if (action == "clear_logs") {
        g_State.ClearLogs();
    }
    else if (action == "window_minimize") {
        if (g_pWebview) {
            HWND hwnd = (HWND)g_pWebview->get_native_handle(WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
            if (hwnd) { ShowWindow(hwnd, SW_MINIMIZE); TrimWorkingSetTree(); }
        }
    }
    else if (action == "window_maximize") {
        if (g_pWebview) {
            HWND hwnd = (HWND)g_pWebview->get_native_handle(WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
            if (hwnd) {
                if (IsZoomed(hwnd)) {
                    ShowWindow(hwnd, SW_RESTORE);
                } else {
                    ShowWindow(hwnd, SW_MAXIMIZE);
                }
            }
        }
    }
    else if (action == "window_close") {
        if (g_pWebview) {
            HWND hwnd = (HWND)g_pWebview->get_native_handle(WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
            if (hwnd) PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
    }
    else if (action == "window_drag") {
        if (g_pWebview) {
            HWND hwnd = (HWND)g_pWebview->get_native_handle(WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
            if (hwnd) {
                ReleaseCapture();
                SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
        }
    }
    else if (action == "app_ready") {
        if (!g_AudioCapture.IsRunning()) {
            g_AudioCapture.Start();
            g_State.is_streaming.store(true);
        }
        g_UsbTransport.TriggerCheck();
        // Preempt any prior post-boot trim thread cleanly so rapid/duplicate app_ready never blocks UI thread
        uint32_t my_gen = 0;
        {
            std::lock_guard<std::mutex> trim_lk(g_PostBootTrimMutex);
            my_gen = g_PostBootTrimGen.fetch_add(1) + 1;
        }
        g_PostBootTrimCv.notify_all();
        std::thread prev_trim;
        {
            std::lock_guard<std::mutex> lock(g_AuxThreadMutex);
            if (g_PostBootTrimThread.joinable()) {
                prev_trim = std::move(g_PostBootTrimThread);
            }
            g_PostBootTrimThread = std::thread([my_gen]() {
                const int delays_ms[3] = { 600, 1200, 2000 };
                for (int delay : delays_ms) {
                    std::unique_lock<std::mutex> lk(g_PostBootTrimMutex);
                    g_PostBootTrimCv.wait_for(lk, std::chrono::milliseconds(delay), [my_gen]() {
                        return !g_Running.load() || g_PostBootTrimGen.load() != my_gen;
                    });
                    if (!g_Running.load() || g_PostBootTrimGen.load() != my_gen) return;
                    lk.unlock();
                    TrimWorkingSetTree();
                }
            });
        }
        if (prev_trim.joinable()) {
            prev_trim.join();
        }
    }
    g_TelemetryDirty.store(true);
    g_FullSnapshotDirty.store(true);
}

// ----------------------------------------------------------------------------
// Windows System Tray, Power Resume, & Unicode Subclassing
// ----------------------------------------------------------------------------
#define WM_TRAYICON (WM_USER + 100)
#define ID_TRAY_RESTORE 1001
#define ID_TRAY_TOGGLE_STREAM 1002
#define ID_TRAY_BITPERFECT 1003
#define ID_TRAY_EXIT 1004
#define ID_TRAY_DEBUG_AUDIO 1005

static WNDPROC g_OrigWndProc = NULL;
static NOTIFYICONDATAA g_Nid = { 0 };

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_TRAYICON) {
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            g_TelemetryDirty.store(true);
            return 0;
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            if (hMenu) {
                InsertMenuA(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_RESTORE, "Restore Window");
                InsertMenuA(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

                bool streaming = g_State.is_streaming.load();
                InsertMenuA(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_TOGGLE_STREAM,
                            streaming ? "Stop Stream" : "Start Stream");

                bool bitperfect = g_State.GetSettingsSnapshot().bit_perfect_mode;
                UINT bpFlags = MF_BYPOSITION | MF_STRING | (bitperfect ? MF_CHECKED : MF_UNCHECKED);
                InsertMenuA(hMenu, -1, bpFlags, ID_TRAY_BITPERFECT, "Bit-Perfect Mode");

                bool debugAudio = ht306::audio::AudioPolicyManager::Instance().GetManualVisibilityOverride();
                UINT dbgFlags = MF_BYPOSITION | MF_STRING | (debugAudio ? MF_CHECKED : MF_UNCHECKED);
                InsertMenuA(hMenu, -1, dbgFlags, ID_TRAY_DEBUG_AUDIO, "Debug: Aktifkan Audio Tanpa RP2350");

                InsertMenuA(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
                InsertMenuA(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, "Exit");

                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);

                if (cmd == ID_TRAY_RESTORE) {
                    ShowWindow(hwnd, SW_SHOW);
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                    g_TelemetryDirty.store(true);
                } else if (cmd == ID_TRAY_TOGGLE_STREAM) {
                    HandleNativeCommand("{\"action\":\"toggle_stream\"}");
                } else if (cmd == ID_TRAY_BITPERFECT) {
                    bool new_bp = !g_State.GetSettingsSnapshot().bit_perfect_mode;
                    std::string cmd_str = "{\"action\":\"set_mode\",\"bit_perfect_mode\":" + std::string(new_bp ? "true" : "false") + "}";
                    HandleNativeCommand(cmd_str);
                } else if (cmd == ID_TRAY_DEBUG_AUDIO) {
                    HandleNativeCommand("{\"action\":\"toggle_debug_audio\"}");
                } else if (cmd == ID_TRAY_EXIT) {
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                }
            }
            return 0;
        }
    } else if (uMsg == WM_POWERBROADCAST) {
        if (wParam == PBT_APMSUSPEND) {
            g_State.AddLog(ht306::LogLevel::LOG_INFO, "Power", "Windows entering sleep/suspend.");
        } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            static ULONGLONG s_last_resume_ms = 0;
            ULONGLONG now_ms = GetTickCount64();
            if (now_ms - s_last_resume_ms >= 1500) {
                s_last_resume_ms = now_ms;
                g_UsbTransport.HandleSystemResume();
                g_State.RequestCaptureReinit();
                EnqueuePolicyAction(PolicyCmd::NONE, -1, true);
                g_TelemetryDirty.store(true);
            }
        }
        return TRUE;
    } else if (uMsg == WM_DEVICECHANGE) {
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            // USB hardware plugged in or removed - trigger instant connection check
            g_UsbTransport.TriggerCheck();
        }
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
            static ULONGLONG s_last_dev_refresh_ms = 0;
            ULONGLONG now_ms = GetTickCount64();
            if (now_ms - s_last_dev_refresh_ms >= 250) {
                s_last_dev_refresh_ms = now_ms;
                EnqueuePolicyAction(PolicyCmd::NONE, -1, true);
            }
        }
        return TRUE;
    } else if (uMsg == WM_DESTROY) {
        Shell_NotifyIconA(NIM_DELETE, &g_Nid);
    }
    return CallWindowProcW(g_OrigWndProc, hwnd, uMsg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// Background Telemetry Thread
// ----------------------------------------------------------------------------
void TelemetryLoop() {
    auto rateModeToString = [](ht306::SampleRatePreference pref) -> const char* {
        switch (pref) {
            case ht306::SampleRatePreference::LOCK_96K: return "96k";
            case ht306::SampleRatePreference::LOCK_48K: return "48k";
            default: return "auto";
        }
    };
    auto capturePathToString = [](ht306::CapturePathMode mode) -> const char* {
        switch (mode) {
            case ht306::CapturePathMode::LOOPBACK: return "loopback";
            case ht306::CapturePathMode::VBCABLE_CAPTURE: return "vbcable_capture";
            case ht306::CapturePathMode::SWEEP_ONLY: return "sweep_only";
            default: return "disconnected";
        }
    };

    uint32_t counter = 0;
    ULONGLONG last_hidden_trim_ms = 0;
    bool was_hidden = false;
    bool last_detected = false;
    bool last_usb_open = false;
    std::string last_com_port;
    std::string last_hb;
    uint64_t last_log_rev = UINT64_MAX;
    uint64_t last_settings_rev = UINT64_MAX;
    uint64_t last_devices_rev = UINT64_MAX;
    ht306::audio::AudioDriverStatus cached_drv_status{};
    bool cached_drv_debug = false;
    bool last_drv_installed = false;
    bool last_drv_visible = false;
    bool last_drv_default = false;
    bool last_drv_debug = false;
    std::string last_drv_name;
    uint32_t last_out_rate = 0;
    uint32_t last_in_rate = 0;
    bool last_rate_clamped = false;
    uint8_t last_sr_mode = 255;
    uint8_t last_cap_path = 255;
    int last_dev_idx = -1;

    while (g_Running.load()) {
        HWND main_hwnd = g_MainHwnd.load(std::memory_order_acquire);
        bool is_hidden_now = (main_hwnd != NULL) && (!IsWindowVisible(main_hwnd) || IsIconic(main_hwnd));
        bool active_stream = g_State.is_streaming.load() || g_State.sweep_running.load();
        // Sleep dynamically: 250ms when hidden or idle (0% CPU/GPU), 35ms when visible & streaming
        int sleep_ms = is_hidden_now ? 250 : (active_stream ? 35 : 250);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        if (!g_Running.load() || !g_pWebview) continue;

        counter++;

        main_hwnd = g_MainHwnd.load(std::memory_order_acquire);
        bool is_hidden = (main_hwnd != NULL) && (!IsWindowVisible(main_hwnd) || IsIconic(main_hwnd));
        if (is_hidden) {
            was_hidden = true;
            ULONGLONG now_trim = GetTickCount64();
            if (last_hidden_trim_ms == 0 || (now_trim - last_hidden_trim_ms >= 30000ULL)) {
                last_hidden_trim_ms = now_trim;
                TrimWorkingSetTree();
            }
            continue;
        }
        last_hidden_trim_ms = 0;
        bool resumed_visible = was_hidden;
        was_hidden = false;

        const bool force_full = g_FullSnapshotDirty.exchange(false) || resumed_visible || (counter <= 2);

        bool drv_changed = false;
        if (force_full || counter % 16 == 0) {
            cached_drv_status = ht306::audio::AudioPolicyManager::Instance().GetCachedDriverStatus();
            cached_drv_debug = ht306::audio::AudioPolicyManager::Instance().GetManualVisibilityOverride();
            if (cached_drv_status.installed != last_drv_installed ||
                cached_drv_status.endpoint_visible != last_drv_visible ||
                cached_drv_status.is_default != last_drv_default ||
                cached_drv_debug != last_drv_debug ||
                cached_drv_status.endpoint_name != last_drv_name) {
                last_drv_installed = cached_drv_status.installed;
                last_drv_visible = cached_drv_status.endpoint_visible;
                last_drv_default = cached_drv_status.is_default;
                last_drv_debug = cached_drv_debug;
                last_drv_name = cached_drv_status.endpoint_name;
                EnqueuePolicyAction(PolicyCmd::NONE, -1, true);
                drv_changed = true;
            }
        }

        const uint64_t cur_log_rev = g_State.GetLogRevision();
        const uint64_t cur_settings_rev = g_State.GetSettingsRevision();
        const uint64_t cur_devices_rev = g_State.GetDevicesRevision();
        const bool logs_changed = (cur_log_rev != last_log_rev) || force_full;
        const bool settings_changed = (cur_settings_rev != last_settings_rev) || force_full;
        const bool devices_changed = (cur_devices_rev != last_devices_rev) || force_full;
        const bool send_driver = drv_changed || force_full;
        const bool fast_dirty = active_stream || force_full || drv_changed || logs_changed ||
                                settings_changed || devices_changed ||
                                g_TelemetryDirty.exchange(false);

        const ht306::HardwareInfo hw = g_State.GetHwInfoSnapshot();
        bool cur_detected = hw.detected;
        bool cur_usb_open = g_UsbTransport.IsOpen();
        const std::string& cur_com = hw.com_port;
        const std::string& cur_hb = hw.last_heartbeat_text;
        uint32_t cur_out_rate = g_State.telemetry.sample_rate.load();
        uint32_t cur_in_rate = g_State.telemetry.input_sample_rate.load();
        uint32_t cur_bit_depth = g_State.telemetry.bit_depth.load();
        uint32_t cur_channels = g_State.telemetry.channels.load();
        bool cur_rate_clamped = g_State.telemetry.rate_clamped.load();
        auto cur_sr_enum = g_State.sample_rate_mode.load();
        auto cur_cap_enum = g_State.telemetry.capture_path.load();
        uint8_t cur_sr_mode = static_cast<uint8_t>(cur_sr_enum);
        uint8_t cur_cap_path = static_cast<uint8_t>(cur_cap_enum);
        int cur_dev_idx = g_State.active_device_index.load();

        bool hw_changed = (cur_detected != last_detected) ||
                          (cur_usb_open != last_usb_open) ||
                          (cur_com != last_com_port) ||
                          (cur_hb != last_hb) ||
                          (cur_out_rate != last_out_rate) ||
                          (cur_in_rate != last_in_rate) ||
                          (cur_rate_clamped != last_rate_clamped) ||
                          (cur_sr_mode != last_sr_mode) ||
                          (cur_cap_path != last_cap_path) ||
                          (cur_dev_idx != last_dev_idx);
        bool should_push = fast_dirty || hw_changed;

        if (!should_push) {
            if (counter == 5 || counter == 12 || counter == 20 || counter % 40 == 0) {
                TrimWorkingSetTree();
            }
            continue;
        }

        last_detected = cur_detected;
        last_usb_open = cur_usb_open;
        last_com_port = cur_com;
        last_hb = cur_hb;
        last_log_rev = cur_log_rev;
        last_settings_rev = cur_settings_rev;
        last_devices_rev = cur_devices_rev;
        last_out_rate = cur_out_rate;
        last_in_rate = cur_in_rate;
        last_rate_clamped = cur_rate_clamped;
        last_sr_mode = cur_sr_mode;
        last_cap_path = cur_cap_path;
        last_dev_idx = cur_dev_idx;

        // Always report DISCONNECTED when hardware is not detected
        std::string state_str = "DISCONNECTED";
        if (hw.detected) {
            state_str = g_State.is_streaming.load() ? "STREAMING" : "READY";
        }

        ht306::ChannelMeters meters[6];
        g_State.GetMetersSnapshot(meters);

        const char* sr_mode_str = rateModeToString(cur_sr_enum);
        const char* cap_path_str = capturePathToString(cur_cap_enum);
        float cur_throughput = SafeJsonFloat(g_State.telemetry.throughput_kbs.load(), 0.0f);
        uint32_t cur_underruns = g_State.telemetry.underrun_count.load();

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1);

        ss << "{"
           << "\"state\":\"" << state_str << "\","
           << "\"is_streaming\":" << (g_State.is_streaming.load() ? "true" : "false") << ","
           << "\"format\":{\"rate\":" << cur_out_rate << ",\"bits\":" << cur_bit_depth << ",\"channels\":" << cur_channels << "},"
           << "\"dBFS\":[";
        for (int i = 0; i < 6; ++i) {
            ss << SafeJsonFloat(meters[i].peak_db, -60.0f) << (i < 5 ? "," : "");
        }
        ss << "],\"peak_hold_db\":[";
        for (int i = 0; i < 6; ++i) {
            ss << SafeJsonFloat(meters[i].peak_hold_db, -60.0f) << (i < 5 ? "," : "");
        }
        ss << "],\"clipped\":[";
        for (int i = 0; i < 6; ++i) {
            ss << (meters[i].clipped ? "true" : "false") << (i < 5 ? "," : "");
        }
        ss << "],"
           << "\"hw_info\":{"
           << "\"uptime_sec\":" << hw.uptime_sec << ","
           << "\"com_port\":\"" << EscapeJsonString(hw.com_port) << "\","
           << "\"vid\":" << hw.vid << ","
           << "\"pid\":" << hw.pid << ","
           << "\"pio_ready\":" << (hw.pio_ready ? "true" : "false") << ","
           << "\"mux_high\":" << (hw.mux_high ? "true" : "false") << ","
           << "\"dsp_locked\":" << (hw.dsp_locked ? "true" : "false") << ","
           << "\"heartbeat_text\":\"" << EscapeJsonString(hw.last_heartbeat_text) << "\""
           << "},"
           << "\"telemetry\":{"
           << "\"throughput_kbs\":" << cur_throughput << ","
           << "\"underrun_count\":" << cur_underruns << ","
           << "\"sample_rate\":" << cur_out_rate << ","
           << "\"input_sample_rate\":" << cur_in_rate << ","
           << "\"bit_depth\":" << cur_bit_depth << ","
           << "\"channels\":" << cur_channels << ","
           << "\"rate_clamped\":" << (cur_rate_clamped ? "true" : "false") << ","
           << "\"sample_rate_mode\":\"" << sr_mode_str << "\","
           << "\"capture_path\":\"" << cap_path_str << "\""
           << "}";

        if (send_driver) {
            ss << ",\"audio_driver\":{"
               << "\"installed\":" << (cached_drv_status.installed ? "true" : "false") << ","
               << "\"visible\":" << (cached_drv_status.endpoint_visible ? "true" : "false") << ","
               << "\"is_default\":" << (cached_drv_status.is_default ? "true" : "false") << ","
               << "\"debug_override\":" << (cached_drv_debug ? "true" : "false") << ","
               << "\"name\":\"" << EscapeJsonString(cached_drv_status.endpoint_name) << "\""
               << "}";
        }

        if (devices_changed) {
            auto devices_snapshot = g_State.GetAvailableDevicesSnapshot();
            ss << ",\"active_device_index\":" << cur_dev_idx << ",\"devices\":[";
            for (size_t i = 0; i < devices_snapshot.size(); ++i) {
                if (i > 0) ss << ",";
                ss << "{\"index\":" << i << ",\"name\":\"" << EscapeJsonString(devices_snapshot[i].name) << "\""
                   << ",\"is_default\":" << (devices_snapshot[i].is_default ? "true" : "false") << "}";
            }
            ss << "]";
        }

        if (logs_changed) {
            auto recent_logs = g_State.GetRecentLogsSnapshot(50);
            ss << ",\"logs\":[";
            for (size_t i = 0; i < recent_logs.size(); ++i) {
                if (i > 0) ss << ",";
                const auto& entry = recent_logs[i];
                std::string formatted = entry.subsystem.empty() ? entry.message : ("[" + entry.subsystem + "] " + entry.message);
                ss << "\"" << EscapeJsonString(formatted) << "\"";
            }
            ss << "]";
        }

        if (settings_changed) {
            const ht306::AppSettings settings = g_State.GetSettingsSnapshot();
            ss << ",\"config\":{"
               << "\"mode\":\"" << (settings.bit_perfect_mode ? "bitperfect" : "desktop") << "\","
               << "\"profile\":\"" << EscapeJsonString(settings.active_profile) << "\","
               << "\"master_volume_db\":" << SafeJsonFloat(settings.master_volume_db, 0.0f) << ","
               << "\"mute_master\":" << (settings.mute_master ? "true" : "false") << ","
               << "\"format_mode\":" << settings.format_mode << ","
               << "\"sample_rate_mode\":\"" << sr_mode_str << "\","
               << "\"limiter_enabled\":" << (settings.limiter_enabled ? "true" : "false") << ","
               << "\"lip_sync_delay_ms\":" << settings.lip_sync_delay_ms << ","
               << "\"active_device_index\":" << cur_dev_idx << ","
               << "\"trims_db\":[";
            for (int i = 0; i < 6; ++i) {
                ss << SafeJsonFloat(settings.trims[i], 0.0f) << (i < 5 ? "," : "");
            }
            ss << "],\"mutes\":[";
            for (int i = 0; i < 6; ++i) {
                ss << (settings.mutes[i] ? "true" : "false") << (i < 5 ? "," : "");
            }
            ss << "],\"solos\":[";
            for (int i = 0; i < 6; ++i) {
                ss << (settings.solos[i] ? "true" : "false") << (i < 5 ? "," : "");
            }
            ss << "],\"phase_inverts\":[";
            for (int i = 0; i < 6; ++i) {
                ss << (settings.phase_inverts[i] ? "true" : "false") << (i < 5 ? "," : "");
            }
            ss << "],\"routing_map\":[";
            for (int i = 0; i < 6; ++i) {
                ss << settings.routing_map[i] << (i < 5 ? "," : "");
            }
            ss << "]}";
        }
        ss << "}";

        std::string json_str = ss.str();
        std::wstring wjson;
        if (g_pCoreWebView2) {
            int len = MultiByteToWideChar(CP_UTF8, 0, json_str.c_str(), -1, NULL, 0);
            if (len > 0) {
                wjson.resize(static_cast<size_t>(len), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, json_str.c_str(), -1, &wjson[0], len);
            }
        }
        if (g_pWebview) {
            g_pWebview->dispatch([json_str = std::move(json_str), wjson = std::move(wjson)]() {
                if (!g_Running.load() || !g_pWebview) return;
                if (g_pCoreWebView2 && !wjson.empty()) {
                    g_pCoreWebView2->PostWebMessageAsJson(wjson.c_str());
                } else {
                    std::string js = "if(window.handleNativeTelemetry) window.handleNativeTelemetry(" + json_str + ");";
                    g_pWebview->eval(js);
                }
            });
        }
        if (!active_stream) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            TrimWorkingSetTree();
        }
    }
}

// ----------------------------------------------------------------------------
// Windows 11 EcoQoS Opt-Out (prevent 15.6 ms timer coalescing when minimized)
// ----------------------------------------------------------------------------
static void DisableWin11TimerThrottling() {
    struct HT306_PROCESS_POWER_THROTTLING_STATE {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };
    constexpr int kProcessPowerThrottling = 4;
    constexpr ULONG kPowerThrottlingCurrentVersion = 1;
    constexpr ULONG kExecutionSpeed = 0x1;        // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
    constexpr ULONG kIgnoreTimerResolution = 0x4; // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION

    using SetProcessInformationFn = BOOL (WINAPI*)(HANDLE, int, LPVOID, DWORD);
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) return;
    auto pSetProcessInformation = reinterpret_cast<SetProcessInformationFn>(
        GetProcAddress(hKernel32, "SetProcessInformation")
    );
    if (!pSetProcessInformation) return;

    HT306_PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = kPowerThrottlingCurrentVersion;
    throttling.ControlMask = kExecutionSpeed | kIgnoreTimerResolution;
    throttling.StateMask = 0; // Opt out of EcoQoS & timer resolution throttling while minimized
    pSetProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &throttling, sizeof(throttling));
}

// ----------------------------------------------------------------------------
// WinMain & Application Entry Point
// ----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // 0. Ensure single instance in user session so second launch activates existing window
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\HT306AudioBridgeSingleInstanceMutex");
    if (hMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingHwnd = FindWindowW(NULL, L"LG HT306 Audio Bridge \u2014 Direct I2S 96kHz Lossless");
        if (existingHwnd) {
            ShowWindow(existingHwnd, SW_SHOW);
            ShowWindow(existingHwnd, SW_RESTORE);
            SetForegroundWindow(existingHwnd);
        }
        CloseHandle(hMutex);
        return 0;
    }

    // Bind our process tree to a Job Object so Windows cleanly terminates only our own
    // child WebView2 processes when the OS closes our handle table at final process teardown.
    // IMPORTANT: Never call CloseHandle(hJob) manually while our process is still running,
    // because KILL_ON_JOB_CLOSE would synchronously terminate our own process before destructors run.
    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)) ||
            !AssignProcessToJobObject(hJob, GetCurrentProcess())) {
            CloseHandle(hJob);
            hJob = NULL;
        }
    }

    // Opt out of Windows 11 EcoQoS timer coalescing so minimized tray streaming never stutters
    DisableWin11TimerThrottling();

    // 1. Initialize audio endpoint helper and sync visibility when RP2350 is absent
    // (When RP2350 is present, UsbTransport::WorkerLoop performs Open + SwitchToHT306 + EnumerateDevices once)
    ht306::audio::AudioPolicyManager::Instance().SyncEndpointNames();
    if (!g_UsbTransport.CheckHardwarePresence()) {
        ht306::audio::AudioPolicyManager::Instance().RestorePreviousDefault(g_State);
        ht306::audio::AudioEndpointHelper::EnumerateDevices(g_State);
    }
    g_UsbTransport.StartWorker();
    g_AudioCapture.Start();
    g_State.is_streaming.store(true);

    // 2. Start background policy worker thread (telemetry starts only after WebView2 is ready!)
    std::thread policy_thread(PolicyWorkerLoop);
    std::thread telemetry_thread;
    HDEVNOTIFY hDevNotify = NULL;

    // 3. Initialize WebView2 with low-RAM parameters
    try {
        // Enforce lowest possible memory constraints on Chromium WebView2
        SetEnvironmentVariableA("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
            "--disable-gpu "
            "--disable-gpu-compositing "
            "--disk-cache-size=1 --media-cache-size=1 --js-flags=\"--max-old-space-size=16 --optimize_for_size --lite-mode\" "
            "--renderer-process-limit=1 "
            "--disable-background-networking "
            "--disable-component-update "
            "--disable-sync "
            "--disable-features=TranslateUI,AutofillServerCommunication,CalculateNativeWinOcclusion,SpareRendererForSitePerProcess "
            "--disable-default-apps "
            "--disable-extensions --allow-file-access-from-files"
        );

        // debug = false to disable full Chromium DevTools backend (~60MB RAM savings)
        webview::webview w(false, nullptr);
        g_pWebview = &w;

        // RAII guard ensures telemetry_thread joins & g_pCoreWebView2 releases BEFORE ~webview() calls CoUninitialize()
        struct WebViewComGuard {
            std::thread& telemetry;
            HDEVNOTIFY& dev_notify;
            ~WebViewComGuard() {
                SignalShutdown();
                if (telemetry.joinable()) {
                    telemetry.join();
                }
                g_MainHwnd.store(NULL, std::memory_order_release);
                g_pWebview = nullptr;
                if (g_pCoreWebView2) {
                    g_pCoreWebView2->Release();
                    g_pCoreWebView2 = nullptr;
                }
                if (dev_notify) {
                    UnregisterDeviceNotification(dev_notify);
                    dev_notify = NULL;
                }
                Shell_NotifyIconA(NIM_DELETE, &g_Nid);
            }
        } webview_guard{ telemetry_thread, hDevNotify };

        w.set_title("LG HT306 Audio Bridge \xE2\x80\x94 Direct I2S 96kHz Lossless");
        w.set_size(1280, 840, WEBVIEW_HINT_NONE);

        HWND hwnd = (HWND)w.get_native_handle(WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
        g_MainHwnd.store(hwnd, std::memory_order_release);
        if (hwnd) {
            EnableImmersiveDarkMode(hwnd);

            // Frameless window: remove standard OS caption bar so only the inner UI header displays
            LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            style &= ~WS_CAPTION;
            style &= ~WS_THICKFRAME;
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

            // Subclass main window (Unicode) to handle system tray, hotplug, and Sleep/Wake
            g_OrigWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)TrayWndProc);

            // Register Shell_NotifyIconA
            ZeroMemory(&g_Nid, sizeof(g_Nid));
            g_Nid.cbSize = sizeof(NOTIFYICONDATAA);
            g_Nid.hWnd = hwnd;
            g_Nid.uID = 1;
            g_Nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            g_Nid.uCallbackMessage = WM_TRAYICON;
            g_Nid.hIcon = LoadIconA(NULL, IDI_APPLICATION);
            strcpy_s(g_Nid.szTip, sizeof(g_Nid.szTip), "LG HT306 Audio Bridge - 96kHz Lossless");
            Shell_NotifyIconA(NIM_ADD, &g_Nid);

            // Register for USB device hotplug notifications
            DEV_BROADCAST_DEVICEINTERFACE_A notificationFilter{};
            notificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE_A);
            notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            hDevNotify = RegisterDeviceNotificationA(hwnd, &notificationFilter, DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
        }

        ICoreWebView2Controller* ctrl = (ICoreWebView2Controller*)w.get_native_handle(WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER);
        if (ctrl) {
            ctrl->get_CoreWebView2(&g_pCoreWebView2);
            if (g_pCoreWebView2) {
                ICoreWebView2_19* core19 = nullptr;
                if (SUCCEEDED(g_pCoreWebView2->QueryInterface(IID_ICoreWebView2_19, (void**)&core19)) && core19) {
                    core19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
                    core19->Release();
                }
            }
        }

        w.bind("nativePostCommand", [](std::string req) -> std::string {
            HandleNativeCommand(req);
            return "{\"status\":\"ok\"}";
        });

        std::string ui_path = ResolveUiHtmlPath();
        std::string ui_url = PathToUrl(ui_path);
        w.navigate(ui_url);

        // Start telemetry thread AFTER g_pWebview and g_pCoreWebView2 are fully initialized
        telemetry_thread = std::thread(TelemetryLoop);

        w.run();
        // WebViewComGuard stops telemetry, releases COM interface, unregisters notifications & tray BEFORE ~webview() calls CoUninitialize
    } catch (const webview::exception& e) {
        std::cerr << "WebView error: " << e.what() << std::endl;
        std::string err = e.what();
        if (err.empty()) {
            err = "Tidak dapat menginisialisasi WebView2 runtime.\nPastikan Microsoft Edge WebView2 Runtime terpasang dan tidak ada proses aplikasi yang menggantung.";
        }
        MessageBoxA(NULL, err.c_str(), "LG HT306 Audio Bridge", MB_ICONWARNING | MB_OK);
    } catch (const std::exception& e) {
        std::cerr << "Standard error: " << e.what() << std::endl;
        MessageBoxA(NULL, e.what(), "Application Error", MB_ICONERROR | MB_OK);
    }

    // 4. Final cleanup of non-WebView worker threads upon exit
    SignalShutdown();
    if (policy_thread.joinable()) {
        policy_thread.join();
    }
    JoinAuxThreads();

    g_AudioCapture.Stop();
    g_UsbTransport.StopWorker();
    g_UsbTransport.Close();

    // Restore Windows default playback ONCE during final cleanup (after workers have stopped)
    ht306::audio::AudioPolicyManager::Instance().SetManualVisibilityOverride(false);
    ht306::audio::AudioPolicyManager::Instance().RestorePreviousDefault(g_State);

    // Intentionally keep hJob open until final kernel process teardown so return 0 & C++ destructors run cleanly.
    (void)hJob;
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return 0;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
