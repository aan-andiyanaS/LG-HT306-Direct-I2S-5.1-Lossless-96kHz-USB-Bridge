// ============================================================================
// LG HT306 Audio Bridge - Windows Audio Policy & Endpoint Manager
// ============================================================================

#include "audio_policy_manager.h"
#include "policy_config.h"
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <cstdio>

#ifndef KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_GUID = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
#define KSDATAFORMAT_SUBTYPE_IEEE_FLOAT KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_GUID
#endif

#ifndef KSDATAFORMAT_SUBTYPE_PCM
static const GUID KSDATAFORMAT_SUBTYPE_PCM_GUID = {
    0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
#define KSDATAFORMAT_SUBTYPE_PCM KSDATAFORMAT_SUBTYPE_PCM_GUID
#endif

namespace ht306::audio {

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
    return str;
}

AudioPolicyManager& AudioPolicyManager::Instance() {
    static AudioPolicyManager instance;
    return instance;
}

std::wstring AudioPolicyManager::GetCurrentDefaultEndpointId() {
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    std::wstring default_id;

    IMMDeviceEnumerator* enumerator = NULL;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator
    );

    if (SUCCEEDED(hr) && enumerator) {
        IMMDevice* default_device = NULL;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &default_device)) && default_device) {
            LPWSTR dev_id = NULL;
            if (SUCCEEDED(default_device->GetId(&dev_id)) && dev_id) {
                default_id = dev_id;
                CoTaskMemFree(dev_id);
            }
            default_device->Release();
        }
        enumerator->Release();
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    return default_id;
}

static std::wstring QueryRegStringSafe(HKEY hKey, const wchar_t* valueName) {
    wchar_t buf[256] = { 0 };
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, valueName, NULL, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        size_t chars = cb / sizeof(wchar_t);
        if (chars >= 256) chars = 255;
        buf[chars] = L'\0';
        return std::wstring(buf);
    }
    return {};
}

AudioDriverStatus AudioPolicyManager::CheckDriverStatus() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Only run full registry discovery/sync once on first probe; never repeat every 4s in GetCachedDriverStatus()
    if (!m_initial_sync_done) {
        SyncEndpointNames();
    }

    AudioDriverStatus status;
    status.installed = false;
    status.endpoint_visible = false;
    status.is_default = false;

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    std::wstring current_default;
    IMMDeviceEnumerator* enumerator = NULL;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator
    );

    if (SUCCEEDED(hr) && enumerator) {
        IMMDevice* def_dev = NULL;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &def_dev)) && def_dev) {
            LPWSTR def_id = NULL;
            if (SUCCEEDED(def_dev->GetId(&def_id)) && def_id) {
                current_default = def_id;
                CoTaskMemFree(def_id);
            }
            def_dev->Release();
        }
        // 1. Direct O(1) lookup by resolved VB-Cable GUID if available from SyncEndpointNames()
        if (!m_ht306_guid.empty()) {
            std::wstring target_id = L"{0.0.0.00000000}." + m_ht306_guid;
            IMMDevice* device = NULL;
            if (SUCCEEDED(enumerator->GetDevice(target_id.c_str(), &device)) && device) {
                DWORD dwState = 0;
                device->GetState(&dwState);
                status.installed = true;
                status.endpoint_visible = (dwState == DEVICE_STATE_ACTIVE);
                status.endpoint_id = target_id;
                status.endpoint_name = "LG HT306 Home Theater (5.1 Surround)";

                IPropertyStore* props = NULL;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                    PROPVARIANT var_name;
                    PropVariantInit(&var_name);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var_name)) &&
                        var_name.vt == VT_LPWSTR && var_name.pwszVal) {
                        std::string name = WideToUtf8(var_name.pwszVal);
                        if (!name.empty()) {
                            status.endpoint_name = name;
                        }
                    }
                    PropVariantClear(&var_name);
                    props->Release();
                }

                if (status.endpoint_visible && !current_default.empty() && current_default == target_id) {
                    status.is_default = true;
                }
                device->Release();
            }
        }

        // 2. Fallback enumeration if direct GUID lookup did not resolve
        if (!status.installed) {
            IMMDeviceCollection* collection = NULL;
            hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &collection);
            if (SUCCEEDED(hr) && collection) {
                UINT count = 0;
                collection->GetCount(&count);

                for (UINT i = 0; i < count; ++i) {
                    IMMDevice* device = NULL;
                    if (SUCCEEDED(collection->Item(i, &device)) && device) {
                        LPWSTR dev_id = NULL;
                        device->GetId(&dev_id);

                        DWORD dwState = 0;
                        device->GetState(&dwState);

                        IPropertyStore* props = NULL;
                        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                            PROPVARIANT var_name;
                            PropVariantInit(&var_name);
                            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var_name)) &&
                        var_name.vt == VT_LPWSTR && var_name.pwszVal) {
                                std::string name = WideToUtf8(var_name.pwszVal);
                                
                                // Check specifically for LG HT306 or VB-CABLE Input (exclude 16ch sub-endpoint)
                                if ((name.find("HT306") != std::string::npos ||
                                     name.find("CABLE Input") != std::string::npos) &&
                                    name.find("16ch") == std::string::npos &&
                                    (dwState == DEVICE_STATE_ACTIVE || dwState == DEVICE_STATE_DISABLED)) {
                                    
                                    status.installed = true;
                                    status.endpoint_visible = (dwState == DEVICE_STATE_ACTIVE);
                                    status.endpoint_name = name;
                                    if (dev_id) status.endpoint_id = dev_id;
                                    if (status.endpoint_visible && !current_default.empty() && dev_id && current_default == dev_id) {
                                        status.is_default = true;
                                    }
                                    PropVariantClear(&var_name);
                                    props->Release();
                                    if (dev_id) CoTaskMemFree(dev_id);
                                    device->Release();
                                    break;
                                }
                            }
                            PropVariantClear(&var_name);
                            props->Release();
                        }
                        if (dev_id) CoTaskMemFree(dev_id);
                        device->Release();
                    }
                }
                collection->Release();
            }
        }
        enumerator->Release();
    }

    // Fallback to Registry scan result from SyncEndpointNames() when endpoint is hidden
    if (!status.installed && !m_ht306_guid.empty()) {
        status.installed = true;
        status.endpoint_visible = (m_ht306_reg_state == 0x00000001);
        status.endpoint_id = L"{0.0.0.00000000}." + m_ht306_guid;
        status.endpoint_name = "LG HT306 Home Theater (5.1 Surround)";
        if (status.endpoint_visible && !current_default.empty() && current_default == status.endpoint_id) {
            status.is_default = true;
        }
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }

    m_cached_status = status;
    m_last_check_tick = GetTickCount64();
    return status;
}

AudioDriverStatus AudioPolicyManager::GetCachedDriverStatus() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    uint64_t now = GetTickCount64();
    if (m_last_check_tick == 0 || (now - m_last_check_tick > 4000)) {
        return CheckDriverStatus();
    }
    return m_cached_status;
}

std::wstring AudioPolicyManager::FindFallbackNonHT306EndpointId() {
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    std::wstring fallback_id;

    IMMDeviceEnumerator* enumerator = NULL;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator
    );

    if (SUCCEEDED(hr) && enumerator) {
        IMMDeviceCollection* collection = NULL;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) && collection) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* device = NULL;
                if (SUCCEEDED(collection->Item(i, &device)) && device) {
                    LPWSTR dev_id = NULL;
                    device->GetId(&dev_id);

                    IPropertyStore* props = NULL;
                    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                        PROPVARIANT var_name;
                        PropVariantInit(&var_name);
                        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var_name)) &&
                        var_name.vt == VT_LPWSTR && var_name.pwszVal) {
                            std::string name = WideToUtf8(var_name.pwszVal);
                            if (name.find("HT306") == std::string::npos &&
                                name.find("CABLE") == std::string::npos &&
                                name.find("VB-Audio") == std::string::npos) {
                                if (dev_id) fallback_id = dev_id;
                            }
                        }
                        PropVariantClear(&var_name);
                        props->Release();
                    }
                    if (dev_id) CoTaskMemFree(dev_id);
                    device->Release();
                    if (!fallback_id.empty()) break;
                }
            }
            collection->Release();
        }
        enumerator->Release();
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    return fallback_id;
}

bool AudioPolicyManager::SetEndpointVisibility(bool visible) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_ht306_guid.empty()) {
        SyncEndpointNames();
    }
    if (m_ht306_guid.empty()) {
        return false;
    }

    std::wstring endpoint_id = L"{0.0.0.00000000}." + m_ht306_guid;
    std::wstring subPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + m_ht306_guid;

    // 1. If surfacing, ensure low nibble of DeviceState in Registry is 0x01 (DEVICE_STATE_ACTIVE)
    // in case it was previously set to 0x10000002 (DEVICE_STATE_DISABLED).
    if (visible) {
        HKEY hSubKeyWrite = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subPath.c_str(), 0, KEY_SET_VALUE, &hSubKeyWrite) == ERROR_SUCCESS) {
            DWORD activeState = 0x00000001;
            RegSetValueExW(hSubKeyWrite, L"DeviceState", 0, REG_DWORD, (const BYTE*)&activeState, sizeof(activeState));
            RegCloseKey(hSubKeyWrite);
        }
    }

    // 2. Notify Audiosrv in real-time via IPolicyConfig::SetEndpointVisibility so Windows 11 Taskbar
    // immediately shows or hides the endpoint without needing an Audiosrv restart.
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool ok = false;

    IPolicyConfig* pPolicyConfig = NULL;
    HRESULT hr = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        NULL,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        (void**)&pPolicyConfig
    );

    if (SUCCEEDED(hr) && pPolicyConfig) {
        if (visible) {
            // Toggle 0 -> 1 to force Audiosrv to reload Registry DeviceState and broadcast OnDeviceStateChanged
            pPolicyConfig->SetEndpointVisibility(endpoint_id.c_str(), 0);
            HRESULT hrVis = pPolicyConfig->SetEndpointVisibility(endpoint_id.c_str(), 1);
            ok = SUCCEEDED(hrVis);
        } else {
            HRESULT hrVis = pPolicyConfig->SetEndpointVisibility(endpoint_id.c_str(), 0);
            ok = SUCCEEDED(hrVis);
            if (!m_ht306_capture_guid.empty()) {
                std::wstring cap_id = L"{0.0.1.00000000}." + m_ht306_capture_guid;
                pPolicyConfig->SetEndpointVisibility(cap_id.c_str(), 0);
            }
        }
        pPolicyConfig->Release();
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }

    if (visible) {
        EnsureCaptureEndpointActive();
    }

    if (ok) {
        m_ht306_reg_state = visible ? 0x00000001 : 0x10000001;
    }
    m_last_check_tick = 0; // Invalidate cache
    return ok;
}

bool AudioPolicyManager::EnsureCaptureEndpointActive() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_ht306_capture_guid.empty()) {
        SyncEndpointNames();
    }
    if (m_ht306_capture_guid.empty()) {
        return false;
    }

    std::wstring cap_id = L"{0.0.1.00000000}." + m_ht306_capture_guid;
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    DWORD dwState = 0;
    IMMDeviceEnumerator* enumerator = NULL;
    IMMDevice* cap_dev = NULL;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&enumerator)) && enumerator) {
        if (SUCCEEDED(enumerator->GetDevice(cap_id.c_str(), &cap_dev)) && cap_dev) {
            cap_dev->GetState(&dwState);
        }
    }

    if (dwState != DEVICE_STATE_ACTIVE) {
        // Best-effort Registry write in case DeviceState has 0x10000001 / disabled bit
        std::wstring capSubPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\" + m_ht306_capture_guid;
        HKEY hSubKeyWrite = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, capSubPath.c_str(), 0, KEY_SET_VALUE, &hSubKeyWrite) == ERROR_SUCCESS) {
            DWORD activeState = 0x00000001;
            RegSetValueExW(hSubKeyWrite, L"DeviceState", 0, REG_DWORD, (const BYTE*)&activeState, sizeof(activeState));
            RegCloseKey(hSubKeyWrite);
        }

        IPolicyConfig* pPolicyConfig = NULL;
        if (SUCCEEDED(CoCreateInstance(CLSID_CPolicyConfigClient, NULL, CLSCTX_ALL,
                                       IID_IPolicyConfig, (void**)&pPolicyConfig)) && pPolicyConfig) {
            pPolicyConfig->SetEndpointVisibility(cap_id.c_str(), 1);
            Sleep(40);
            if (cap_dev) {
                cap_dev->GetState(&dwState);
            }
            if (dwState != DEVICE_STATE_ACTIVE) {
                // Toggle 0 -> 1 to force AudioSrv reload of capture endpoint state
                pPolicyConfig->SetEndpointVisibility(cap_id.c_str(), 0);
                pPolicyConfig->SetEndpointVisibility(cap_id.c_str(), 1);
            }
            for (int attempt = 0; attempt < 10 && dwState != DEVICE_STATE_ACTIVE; ++attempt) {
                Sleep(40);
                if (cap_dev) {
                    cap_dev->GetState(&dwState);
                }
            }
            pPolicyConfig->Release();
        }
    }

    if (cap_dev) {
        cap_dev->Release();
    }
    if (enumerator) {
        enumerator->Release();
    }
    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    return (dwState == DEVICE_STATE_ACTIVE);
}

bool AudioPolicyManager::SetDefaultEndpoint(const std::wstring& device_id) {
    if (device_id.empty()) return false;

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool success = false;

    IPolicyConfig* pPolicyConfig = NULL;
    HRESULT hr = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        NULL,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        (void**)&pPolicyConfig
    );

    if (SUCCEEDED(hr) && pPolicyConfig) {
        // Set default for Multimedia (music, videos, browser, youtube)
        HRESULT hr1 = pPolicyConfig->SetDefaultEndpoint(device_id.c_str(), eMultimedia);
        // Set default for Console (games, system notifications)
        HRESULT hr2 = pPolicyConfig->SetDefaultEndpoint(device_id.c_str(), eConsole);
        
        success = SUCCEEDED(hr1) || SUCCEEDED(hr2);
        pPolicyConfig->Release();
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    return success;
}

bool AudioPolicyManager::SwitchToHT306(AppState& state) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_ht306_guid.empty()) {
        SyncEndpointNames();
    }
    AudioDriverStatus status = CheckDriverStatus();
    if (!status.installed || status.endpoint_id.empty()) {
        state.AddLog(LogLevel::LOG_WARN, "AudioPolicy", 
            "Virtual Audio Driver (HT306/VB-Cable) not installed. Please click 'Install Driver' in Settings.");
        return false;
    }

    // Save current non-HT306 default playback endpoint before surfacing HT306
    std::wstring current_def = GetCurrentDefaultEndpointId();
    if (!current_def.empty() && current_def != status.endpoint_id) {
        m_saved_previous_device_id = current_def;
    }

    // 1. Ensure the LG HT306 Home Theater endpoint is visible in Windows Taskbar (DeviceState = 1)
    bool surfaced_now = false;
    if (!status.endpoint_visible) {
        SetEndpointVisibility(true);
        surfaced_now = true;
        // Poll up to 400ms (10 x 40ms) for Audiosrv to register the newly activated endpoint
        for (int attempt = 0; attempt < 10; ++attempt) {
            Sleep(40);
            status = CheckDriverStatus();
            if (status.endpoint_visible) break;
        }
    }

    // 2. Switch default audio output to LG HT306 Home Theater
    current_def = GetCurrentDefaultEndpointId();
    if (current_def != status.endpoint_id) {
        bool ok = SetDefaultEndpoint(status.endpoint_id);
        if (ok) {
            state.AddLog(LogLevel::LOG_INFO, "AudioPolicy", 
                "RP2350 connected: Surfaced & switched Windows Taskbar audio to " + status.endpoint_name);
        } else {
            state.AddLog(LogLevel::LOG_ERROR, "AudioPolicy", "Failed to switch default audio device via IPolicyConfig");
            return false;
        }
    }

    // 3. Configure 5.1 surround format respecting user SampleRatePreference
    uint32_t desired_rate = 96000;
    auto sr_pref = state.sample_rate_mode.load();
    if (sr_pref == SampleRatePreference::LOCK_48K) {
        desired_rate = 48000;
    } else if (sr_pref == SampleRatePreference::LOCK_96K) {
        desired_rate = 96000;
    } else {
        uint32_t existing = QueryEndpoint6ChRateLocked(status.endpoint_id);
        desired_rate = (existing == 48000 || existing == 96000) ? existing : 96000;
    }

    if (!ConfigureEndpointFormat(status.endpoint_id, desired_rate)) {
        state.AddLog(LogLevel::LOG_WARN, "AudioPolicy",
            "Gagal menyetel format 5.1 " + std::to_string(desired_rate) + "Hz pada endpoint Render HT306");
    }
    if (!surfaced_now) {
        EnsureCaptureEndpointActive();
    }
    std::wstring cap_id = GetCaptureEndpointId();
    if (!cap_id.empty()) {
        if (!ConfigureEndpointFormat(cap_id, desired_rate)) {
            state.AddLog(LogLevel::LOG_WARN, "AudioPolicy",
                "Gagal menyetel format 5.1 " + std::to_string(desired_rate) + "Hz pada LG HT306 Loopback Stream");
        }
    }
    CheckDriverStatus(); // Refresh cached status

    // 4. Preserve target selection and notify capture thread
    state.SelectDeviceById(status.endpoint_id);
    state.RequestCaptureReinit();

    return true;
}

bool AudioPolicyManager::RestorePreviousDefault(AppState& state) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    AudioDriverStatus status = CheckDriverStatus();
    if (!status.installed) {
        return false;
    }

    // 1. Determine fallback speaker if saved previous ID is empty or points to HT306
    if (m_saved_previous_device_id.empty() || m_saved_previous_device_id == status.endpoint_id) {
        m_saved_previous_device_id = FindFallbackNonHT306EndpointId();
    }

    // 2. Switch default playback back to PC speaker before hiding HT306
    if (!m_saved_previous_device_id.empty()) {
        SetDefaultEndpoint(m_saved_previous_device_id);
    }

    // 3. Hide LG HT306 Home Theater from Windows Taskbar unless manual override is active
    if (!m_manual_visibility_override.load(std::memory_order_acquire)) {
        if (SetEndpointVisibility(false) && status.endpoint_visible) {
            state.AddLog(LogLevel::LOG_INFO, "AudioPolicy", 
                "RP2350 disconnected: Restored PC speaker and hid LG HT306 Home Theater from Windows Taskbar");
        }
    }

    CheckDriverStatus(); // Refresh cached status
    state.SelectDeviceByIndex(0);
    state.RequestCaptureReinit();
    return true;
}

uint32_t AudioPolicyManager::QueryEndpoint6ChRateLocked(const std::wstring& device_id) {
    if (device_id.empty()) return 0;
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    uint32_t found_rate = 0;

    IPolicyConfig* pPolicyConfig = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_CPolicyConfigClient, NULL, CLSCTX_ALL, IID_IPolicyConfig, (void**)&pPolicyConfig)) && pPolicyConfig) {
        WAVEFORMATEX* pwfx = NULL;
        if (SUCCEEDED(pPolicyConfig->GetMixFormat(device_id.c_str(), &pwfx)) && pwfx) {
            if (pwfx->nChannels >= 6 && (pwfx->nSamplesPerSec == 48000 || pwfx->nSamplesPerSec == 96000)) {
                found_rate = pwfx->nSamplesPerSec;
            }
            CoTaskMemFree(pwfx);
        }
        pPolicyConfig->Release();
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    return found_rate;
}

bool AudioPolicyManager::ConfigureEndpointFormat(const std::wstring& device_id, uint32_t sample_rate, HRESULT* out_last_hr) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (device_id.empty() || (sample_rate != 48000 && sample_rate != 96000)) {
        if (out_last_hr) *out_last_hr = E_INVALIDARG;
        return false;
    }

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool success = false;
    HRESULT last_hr = E_FAIL;

    IPolicyConfig* pPolicyConfig = NULL;
    HRESULT hr = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        NULL,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        (void**)&pPolicyConfig
    );
    if (FAILED(hr)) {
        last_hr = hr;
    }

    if (SUCCEEDED(hr) && pPolicyConfig) {
        auto verifyRateApplied = [&]() -> bool {
            WAVEFORMATEX* verify_df = NULL;
            HRESULT hrVer = pPolicyConfig->GetDeviceFormat(device_id.c_str(), 0, &verify_df);
            bool matched = false;
            if (SUCCEEDED(hrVer) && verify_df) {
                matched = (verify_df->nSamplesPerSec == sample_rate);
                CoTaskMemFree(verify_df);
            }
            return matched;
        };

        auto tryApplyPair = [&](WAVEFORMATEX* ep_fmt, WAVEFORMATEX* mix_fmt) -> bool {
            if (!ep_fmt) return false;
            last_hr = pPolicyConfig->SetDeviceFormat(device_id.c_str(), ep_fmt, mix_fmt);
            if (FAILED(last_hr) && mix_fmt != NULL) {
                last_hr = pPolicyConfig->SetDeviceFormat(device_id.c_str(), ep_fmt, NULL);
            }
            return SUCCEEDED(last_hr) && verifyRateApplied();
        };

        // Step 1: Clone currently active 6-channel endpoint/mix formats and retarget sample rate
        WAVEFORMATEX* cur_df = NULL;
        WAVEFORMATEX* cur_mf = NULL;
        HRESULT hrGetDf = pPolicyConfig->GetDeviceFormat(device_id.c_str(), 0, &cur_df);
        HRESULT hrGetMf = pPolicyConfig->GetMixFormat(device_id.c_str(), &cur_mf);
        if (SUCCEEDED(hrGetDf) && cur_df && SUCCEEDED(hrGetMf) && cur_mf &&
            cur_df->nChannels >= 6 && cur_mf->nChannels >= 6) {
            cur_df->nSamplesPerSec = sample_rate;
            cur_df->nAvgBytesPerSec = sample_rate * cur_df->nBlockAlign;
            cur_mf->nSamplesPerSec = sample_rate;
            cur_mf->nAvgBytesPerSec = sample_rate * cur_mf->nBlockAlign;
            if (tryApplyPair(cur_df, cur_mf)) {
                success = true;
            }
        }
        if (cur_df) CoTaskMemFree(cur_df);
        if (cur_mf) CoTaskMemFree(cur_mf);

        // Step 2: Fallback 6-channel format candidate matrix
        if (!success) {
            const DWORD masks[] = {
                SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                    SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT,
                SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                    SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT
            };

            struct EpCandidate {
                uint16_t container_bits;
                uint16_t valid_bits;
                bool is_float;
            };
            const EpCandidate candidates[] = {
                { 24, 24, false }, // WDM 24-bit packed PCM (standard VB-Cable pin format)
                { 32, 24, false }, // WDM 24-in-32 PCM
                { 16, 16, false }, // WDM 16-bit PCM fallback
                { 32, 32, true  }  // IEEE Float fallback
            };

            for (DWORD mask : masks) {
                WAVEFORMATEXTENSIBLE mixFmt{};
                mixFmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                mixFmt.Format.nChannels = 6;
                mixFmt.Format.nSamplesPerSec = sample_rate;
                mixFmt.Format.wBitsPerSample = 32;
                mixFmt.Format.nBlockAlign = 6 * sizeof(float);
                mixFmt.Format.nAvgBytesPerSec = sample_rate * mixFmt.Format.nBlockAlign;
                mixFmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                mixFmt.Samples.wValidBitsPerSample = 32;
                mixFmt.dwChannelMask = mask;
                mixFmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

                for (const auto& c : candidates) {
                    WAVEFORMATEXTENSIBLE epFmt{};
                    epFmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                    epFmt.Format.nChannels = 6;
                    epFmt.Format.nSamplesPerSec = sample_rate;
                    epFmt.Format.wBitsPerSample = c.container_bits;
                    epFmt.Format.nBlockAlign = static_cast<WORD>(6 * (c.container_bits / 8));
                    epFmt.Format.nAvgBytesPerSec = sample_rate * epFmt.Format.nBlockAlign;
                    epFmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                    epFmt.Samples.wValidBitsPerSample = c.valid_bits;
                    epFmt.dwChannelMask = mask;
                    epFmt.SubFormat = c.is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;

                    if (tryApplyPair(reinterpret_cast<WAVEFORMATEX*>(&epFmt),
                                     reinterpret_cast<WAVEFORMATEX*>(&mixFmt))) {
                        success = true;
                        break;
                    }
                }
                if (success) break;
            }
        }
        pPolicyConfig->Release();
    }

    if (out_last_hr) {
        *out_last_hr = last_hr;
    }
    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    return success;
}

bool AudioPolicyManager::ApplySampleRateToHT306Endpoints(AppState& state, uint32_t sample_rate) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (sample_rate != 48000 && sample_rate != 96000) {
        return false;
    }

    auto formatHrHex = [](HRESULT hr) -> std::string {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
        return std::string(buf);
    };

    bool render_ok = false;
    bool cap_ok = false;
    HRESULT hr_render = E_FAIL;
    HRESULT hr_cap = E_FAIL;

    std::wstring render_id = GetRenderEndpointId();
    if (!render_id.empty()) {
        if (ConfigureEndpointFormat(render_id, sample_rate, &hr_render)) {
            render_ok = true;
        } else {
            state.AddLog(LogLevel::LOG_WARN, "Format",
                "Endpoint Render HT306 menolak ubah format ke " + std::to_string(sample_rate) +
                " Hz (hr=" + formatHrHex(hr_render) + ")");
        }
    }

    EnsureCaptureEndpointActive();
    std::wstring cap_id = GetCaptureEndpointId();
    if (!cap_id.empty()) {
        if (ConfigureEndpointFormat(cap_id, sample_rate, &hr_cap)) {
            cap_ok = true;
        } else {
            state.AddLog(LogLevel::LOG_WARN, "Format",
                "Endpoint Capture (LG HT306 Loopback Stream) menolak ubah format ke " + std::to_string(sample_rate) +
                " Hz (hr=" + formatHrHex(hr_cap) + ")");
        }
    }

    if (render_ok || cap_ok) {
        state.AddLog(LogLevel::LOG_INFO, "Format",
            "Format Windows VB-Cable diubah ke " + std::to_string(sample_rate) +
            " Hz (Render=" + (render_ok ? "OK" : "FAIL") +
            ", Capture=" + (cap_ok ? "OK" : "FAIL") + ")");
    }

    state.RequestCaptureReinit();
    return (render_ok || cap_ok);
}

std::wstring AudioPolicyManager::GetRenderEndpointId() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_ht306_guid.empty()) {
        SyncEndpointNames();
    }
    if (m_ht306_guid.empty()) return {};
    return L"{0.0.0.00000000}." + m_ht306_guid;
}

std::wstring AudioPolicyManager::GetCaptureEndpointId() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_ht306_capture_guid.empty()) {
        SyncEndpointNames();
    }
    if (m_ht306_capture_guid.empty()) return {};
    return L"{0.0.1.00000000}." + m_ht306_capture_guid;
}

bool AudioPolicyManager::SyncEndpointNames() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_initial_sync_done = true;

    HKEY hRenderKey = NULL;
    const wchar_t* renderPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, renderPath, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hRenderKey) != ERROR_SUCCESS) {
        return false;
    }

    m_ht306_guid.clear();
    bool render_active_found = false;
    wchar_t subKeyName[256];
    DWORD index = 0;
    DWORD nameLen = 256;

    while (RegEnumKeyExW(hRenderKey, index, subKeyName, &nameLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        index++;
        nameLen = 256;

        std::wstring subPath = std::wstring(renderPath) + L"\\" + subKeyName;
        std::wstring propPath = subPath + L"\\Properties";

        HKEY hSubKeyRead = NULL;
        DWORD currentState = 0;
        DWORD cbState = sizeof(currentState);
        DWORD stateType = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subPath.c_str(), 0, KEY_QUERY_VALUE, &hSubKeyRead) == ERROR_SUCCESS) {
            if (RegQueryValueExW(hSubKeyRead, L"DeviceState", NULL, &stateType, (LPBYTE)&currentState, &cbState) != ERROR_SUCCESS ||
                stateType != REG_DWORD) {
                currentState = 0;
            }
            RegCloseKey(hSubKeyRead);
        }

        // Skip uninstalled / non-present ghost endpoints (low nibble 0x04 = DEVICE_STATE_NOTPRESENT)
        DWORD stateLow = currentState & 0x0F;
        if (stateLow != DEVICE_STATE_ACTIVE && stateLow != DEVICE_STATE_DISABLED) {
            continue;
        }

        HKEY hPropKey = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, propPath.c_str(), 0, KEY_QUERY_VALUE, &hPropKey) == ERROR_SUCCESS) {
            std::wstring wDesc = QueryRegStringSafe(hPropKey, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
            std::wstring wIfName = QueryRegStringSafe(hPropKey, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6");
            std::wstring wHwId = QueryRegStringSafe(hPropKey, L"{a8b865dd-2e3d-4094-ad97-e593a70c75d6},8");
            std::wstring wSubType = QueryRegStringSafe(hPropKey, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},8");
            RegCloseKey(hPropKey);

            bool isVbCable = (wHwId.find(L"VBAudioVACWDM") != std::wstring::npos) ||
                             (wDesc.find(L"CABLE") != std::wstring::npos) ||
                             (wDesc == L"LG HT306 Home Theater") ||
                             (wIfName.find(L"VB-Audio") != std::wstring::npos);

            if (isVbCable) {
                bool is16ch = (wDesc.find(L"16ch") != std::wstring::npos) ||
                              (wIfName.find(L"16ch") != std::wstring::npos) ||
                              (wSubType.find(L"DFF21FE3") != std::wstring::npos) ||
                              (wSubType.find(L"dff21fe3") != std::wstring::npos);

                if (is16ch) {
                    if (currentState != 0x10000002) {
                        // Hide 16ch endpoint live via IPolicyConfig::SetEndpointVisibility(0) and lock Registry to 0x10000002
                        HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
                        IPolicyConfig* pPC = NULL;
                        if (SUCCEEDED(CoCreateInstance(CLSID_CPolicyConfigClient, NULL, CLSCTX_ALL, IID_IPolicyConfig, (void**)&pPC)) && pPC) {
                            std::wstring id16ch = std::wstring(L"{0.0.0.00000000}.") + subKeyName;
                            pPC->SetEndpointVisibility(id16ch.c_str(), 0);
                            pPC->Release();
                        }
                        if (SUCCEEDED(hrCom)) CoUninitialize();

                        HKEY hSubKeyWrite = NULL;
                        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subPath.c_str(), 0, KEY_SET_VALUE, &hSubKeyWrite) == ERROR_SUCCESS) {
                            DWORD disabledState = 0x10000002;
                            RegSetValueExW(hSubKeyWrite, L"DeviceState", 0, REG_DWORD, (const BYTE*)&disabledState, sizeof(disabledState));
                            RegCloseKey(hSubKeyWrite);
                        }
                    }
                } else {
                    if (!render_active_found || currentState == DEVICE_STATE_ACTIVE) {
                        m_ht306_guid = subKeyName;
                        m_ht306_reg_state = currentState;
                        if (currentState == DEVICE_STATE_ACTIVE) {
                            render_active_found = true;
                        }
                    }

                    if (wDesc != L"LG HT306 Home Theater" || wIfName != L"5.1 Surround") {
                        HKEY hPropWrite = NULL;
                        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, propPath.c_str(), 0, KEY_SET_VALUE, &hPropWrite) == ERROR_SUCCESS) {
                            const wchar_t* newDesc = L"LG HT306 Home Theater";
                            const wchar_t* newIfName = L"5.1 Surround";
                            const wchar_t* newFriendly = L"LG HT306 Home Theater (5.1 Surround)";
                            const wchar_t* speakerIcon = L"%windir%\\system32\\mmres.dll,-3010";
                            RegSetValueExW(hPropWrite, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", 0, REG_SZ,
                                           (const BYTE*)newDesc, (DWORD)((wcslen(newDesc) + 1) * sizeof(wchar_t)));
                            RegSetValueExW(hPropWrite, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", 0, REG_SZ,
                                           (const BYTE*)newIfName, (DWORD)((wcslen(newIfName) + 1) * sizeof(wchar_t)));
                            RegSetValueExW(hPropWrite, L"{a45c254e-df1c-4efd-8020-67d146a850e0},14", 0, REG_SZ,
                                           (const BYTE*)newFriendly, (DWORD)((wcslen(newFriendly) + 1) * sizeof(wchar_t)));
                            RegSetValueExW(hPropWrite, L"{259abffc-50a7-47ce-af08-68c9a7d73366},12", 0, REG_SZ,
                                           (const BYTE*)speakerIcon, (DWORD)((wcslen(speakerIcon) + 1) * sizeof(wchar_t)));
                            RegCloseKey(hPropWrite);
                        }
                    }
                }
            }
        }
    }

    RegCloseKey(hRenderKey);

    // Scan Capture side for VB-Cable Output ("LG HT306 Loopback Stream" / "CABLE Output")
    m_ht306_capture_guid.clear();
    HKEY hCaptureKey = NULL;
    const wchar_t* capturePath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, capturePath, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hCaptureKey) == ERROR_SUCCESS) {
        bool cap_active_found = false;
        index = 0;
        nameLen = 256;
        while (RegEnumKeyExW(hCaptureKey, index, subKeyName, &nameLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            index++;
            nameLen = 256;

            std::wstring subPath = std::wstring(capturePath) + L"\\" + subKeyName;
            std::wstring propPath = subPath + L"\\Properties";

            HKEY hSubKeyRead = NULL;
            DWORD currentState = 0;
            DWORD cbState = sizeof(currentState);
            DWORD stateType = 0;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subPath.c_str(), 0, KEY_QUERY_VALUE, &hSubKeyRead) == ERROR_SUCCESS) {
                if (RegQueryValueExW(hSubKeyRead, L"DeviceState", NULL, &stateType, (LPBYTE)&currentState, &cbState) != ERROR_SUCCESS ||
                    stateType != REG_DWORD) {
                    currentState = 0;
                }
                RegCloseKey(hSubKeyRead);
            }

            DWORD stateLow = currentState & 0x0F;
            if (stateLow != DEVICE_STATE_ACTIVE && stateLow != DEVICE_STATE_DISABLED) {
                continue;
            }

            HKEY hPropKey = NULL;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, propPath.c_str(), 0, KEY_QUERY_VALUE, &hPropKey) == ERROR_SUCCESS) {
                std::wstring wDesc = QueryRegStringSafe(hPropKey, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
                std::wstring wIfName = QueryRegStringSafe(hPropKey, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6");
                std::wstring wHwId = QueryRegStringSafe(hPropKey, L"{a8b865dd-2e3d-4094-ad97-e593a70c75d6},8");
                RegCloseKey(hPropKey);

                bool isVbCapture = (wHwId.find(L"VBAudioVACWDM") != std::wstring::npos) ||
                                   (wDesc.find(L"CABLE Output") != std::wstring::npos) ||
                                   (wDesc.find(L"HT306 Loopback") != std::wstring::npos) ||
                                   (wIfName.find(L"VB-Audio") != std::wstring::npos);
                bool is16ch = (wDesc.find(L"16ch") != std::wstring::npos) ||
                              (wIfName.find(L"16ch") != std::wstring::npos);

                if (isVbCapture && !is16ch) {
                    if (!cap_active_found || currentState == DEVICE_STATE_ACTIVE) {
                        m_ht306_capture_guid = subKeyName;
                        if (currentState == DEVICE_STATE_ACTIVE) {
                            cap_active_found = true;
                        }
                    }
                }
            }
        }
        RegCloseKey(hCaptureKey);
    }

    m_last_check_tick = 0; // Invalidate cache so next check reads updated names
    return !m_ht306_guid.empty();
}

static std::string FindVBCableSetupExe() {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecA(exePath);

    char candidate[MAX_PATH] = { 0 };
    char canonical[MAX_PATH] = { 0 };

    // 1. In <exe_dir>\tools\vbcable\VBCABLE_Setup_x64.exe
    PathCombineA(candidate, exePath, "tools\\vbcable\\VBCABLE_Setup_x64.exe");
    if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
        if (GetFullPathNameA(candidate, MAX_PATH, canonical, NULL)) return canonical;
        return candidate;
    }

    // 2. In <exe_dir>\..\tools\vbcable\VBCABLE_Setup_x64.exe
    PathCombineA(candidate, exePath, "..\\tools\\vbcable\\VBCABLE_Setup_x64.exe");
    if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
        if (GetFullPathNameA(candidate, MAX_PATH, canonical, NULL)) return canonical;
        return candidate;
    }

    // 3. In <exe_dir>\..\..\tools\vbcable\VBCABLE_Setup_x64.exe
    PathCombineA(candidate, exePath, "..\\..\\tools\\vbcable\\VBCABLE_Setup_x64.exe");
    if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
        if (GetFullPathNameA(candidate, MAX_PATH, canonical, NULL)) return canonical;
        return candidate;
    }

    // 4. Relative to current working directory
    if (GetFileAttributesA("tools\\vbcable\\VBCABLE_Setup_x64.exe") != INVALID_FILE_ATTRIBUTES) {
        if (GetFullPathNameA("tools\\vbcable\\VBCABLE_Setup_x64.exe", MAX_PATH, canonical, NULL)) return canonical;
        return "tools\\vbcable\\VBCABLE_Setup_x64.exe";
    }
    if (GetFileAttributesA("pc_stream_app\\tools\\vbcable\\VBCABLE_Setup_x64.exe") != INVALID_FILE_ATTRIBUTES) {
        if (GetFullPathNameA("pc_stream_app\\tools\\vbcable\\VBCABLE_Setup_x64.exe", MAX_PATH, canonical, NULL)) return canonical;
        return "pc_stream_app\\tools\\vbcable\\VBCABLE_Setup_x64.exe";
    }

    return "";
}

bool AudioPolicyManager::InstallDriver(HWND parent_hwnd, const std::atomic<bool>* running_flag) {
    std::string setup_exe = FindVBCableSetupExe();
    if (setup_exe.empty()) {
        MessageBoxA(parent_hwnd, 
            "File installer driver 'VBCABLE_Setup_x64.exe' tidak ditemukan dalam direktori tools/vbcable.",
            "LG HT306 Driver Setup", MB_ICONERROR | MB_OK);
        return false;
    }

    std::string setup_dir = setup_exe;
    size_t last_sep = setup_dir.find_last_of("\\/");
    if (last_sep != std::string::npos) {
        setup_dir.erase(last_sep);
    } else {
        setup_dir.clear();
    }

    // Initialize COM for ShellExecuteEx (STA is recommended for shell operations)
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Launch VBCABLE_Setup_x64.exe with administrator elevation
    SHELLEXECUTEINFOA sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = NULL; // Safe for worker thread elevation
    sei.lpVerb = "runas"; // Elevate with Windows UAC prompt
    sei.lpFile = setup_exe.c_str();
    sei.lpParameters = NULL; // Interactive GUI installer
    sei.lpDirectory = setup_dir.empty() ? NULL : setup_dir.c_str();
    sei.nShow = SW_SHOWNORMAL;

    BOOL shellRet = ShellExecuteExA(&sei);

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }

    if (!shellRet) {
        // If UAC was cancelled or rejected by user, cleanly exit
        return false;
    }

    if (sei.hProcess) {
        // Wait for setup process (up to 120s) in 250ms slices so app shutdown never hangs
        for (int wait_slice = 0; wait_slice < 480; ++wait_slice) {
            if (running_flag && !running_flag->load()) {
                CloseHandle(sei.hProcess);
                return false;
            }
            if (WaitForSingleObject(sei.hProcess, 250) == WAIT_OBJECT_0) {
                break;
            }
        }
        CloseHandle(sei.hProcess);
    }

    // Windows PnP and AudioEndpointBuilder automatically discover the new WHQL endpoint (interruptible 1.5s wait)
    for (int i = 0; i < 15; ++i) {
        if (running_flag && !running_flag->load()) {
            return false;
        }
        Sleep(100);
    }

    // Run automated script to set friendly name to 'LG HT306 Home Theater' and disable 16ch
    SyncEndpointNames();

    // Check if installation succeeded
    AudioDriverStatus st = CheckDriverStatus();
    if (st.installed) {
        ConfigureEndpointFormat(st.endpoint_id, 96000);
    }

    return st.installed;
}

} // namespace ht306::audio
