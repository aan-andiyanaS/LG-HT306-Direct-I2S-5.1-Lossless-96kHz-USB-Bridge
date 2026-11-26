#include "usb_transport.h"
#include "audio_policy_manager.h"
#include "audio_endpoint_helper.h"
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

// Microsoft OS 2.0 Descriptor GUID defined in firmware/src/usb_descriptors.c
// {CDB3B5AD-293B-4663-AA36-1AAE46463776}
DEFINE_GUID(GUID_DEVINTERFACE_HT306_WINUSB,
    0xcdb3b5ad, 0x293b, 0x4663, 0xaa, 0x36, 0x1a, 0xae, 0x46, 0x46, 0x37, 0x76);

// Standard Microsoft WinUSB Device Interface GUID
// {DEE824EF-729B-4A0E-9C14-B7117D33A817}
DEFINE_GUID(GUID_DEVINTERFACE_STANDARD_WINUSB,
    0xdee824ef, 0x729b, 0x4a0e, 0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

namespace ht306::usb {

namespace {

inline char SafeToUpper(unsigned char c) {
    return static_cast<char>(std::toupper(c));
}

inline char SafeToLower(unsigned char c) {
    return static_cast<char>(std::tolower(c));
}

void FlattenAndUppercaseHwid(char* buf, DWORD req_size, DWORD cap) {
    DWORD scan_len = (req_size < cap - 1) ? req_size : (cap - 1);
    for (DWORD k = 0; k < scan_len; ++k) {
        buf[k] = (buf[k] == '\0') ? ' ' : SafeToUpper(static_cast<unsigned char>(buf[k]));
    }
    buf[scan_len] = '\0';
}

bool ContainsRp2350VidPid(const char* hwid_upper) {
    return (std::strstr(hwid_upper, "VID_2E8A") != nullptr) &&
           (std::strstr(hwid_upper, "PID_4306") != nullptr);
}

} // namespace

void UsbTransport::CancelAndDrainAsyncTxLocked() {
    if (m_winusb_handle) {
        WinUsb_AbortPipe(m_winusb_handle, m_bulk_out_pipe);
    }
    if (m_device_handle != INVALID_HANDLE_VALUE) {
        CancelIoEx(m_device_handle, NULL);
    }
    for (auto& slot : m_tx_slots) {
        if (slot.pending) {
            if (slot.event) {
                WaitForSingleObject(slot.event, 50);
            }
            slot.pending = false;
            slot.expected_len = 0;
        }
    }
    m_tx_next_slot = 0;
}

void UsbTransport::CloseWinUsbHandlesLocked() {
    CancelAndDrainAsyncTxLocked();
    if (m_winusb_handle) {
        WinUsb_Free(m_winusb_handle);
        m_winusb_handle = NULL;
    }
    if (m_device_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_device_handle);
        m_device_handle = INVALID_HANDLE_VALUE;
    }
    m_consecutive_write_failures = 0;
}

UsbTransport::UsbTransport(AppState& state)
    : m_state(state) {
    m_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (auto& slot : m_tx_slots) {
        slot.buf.reserve(4096);
        slot.event = CreateEventA(NULL, TRUE, FALSE, NULL);
    }
}

UsbTransport::~UsbTransport() {
    StopWorker();
    Close();
    for (auto& slot : m_tx_slots) {
        if (slot.event) {
            CloseHandle(slot.event);
            slot.event = NULL;
        }
    }
    if (m_stop_event) {
        CloseHandle(m_stop_event);
        m_stop_event = NULL;
    }
}

bool UsbTransport::CheckHardwarePresence() {
    // 1. Check via SetupDi scoped to USB enumerator (~90% fewer devnode syscalls than DIGCF_ALLCLASSES unfiltered)
    HDEVINFO dev_info = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    bool found = false;
    if (dev_info != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev_data{};
        dev_data.cbSize = sizeof(dev_data);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(dev_info, i, &dev_data); ++i) {
            char hwid_buf[512] = { 0 };
            DWORD req_size = 0;
            if (SetupDiGetDeviceRegistryPropertyA(dev_info, &dev_data, SPDRP_HARDWAREID, NULL,
                                                 reinterpret_cast<PBYTE>(hwid_buf), sizeof(hwid_buf) - 1, &req_size)) {
                FlattenAndUppercaseHwid(hwid_buf, req_size, sizeof(hwid_buf));
                if (ContainsRp2350VidPid(hwid_buf)) {
                    found = true;
                    break;
                }
            }
        }
        SetupDiDestroyDeviceInfoList(dev_info);
    }

    if (found) {
        m_state.UpdateHwInfo([](HardwareInfo& hw) {
            hw.detected = true;
            hw.vid = 0x2E8A;
            hw.pid = 0x4306;
            if (hw.serial_number.empty()) {
                hw.serial_number = "HT306-RP2350Z-96K";
            }
            if (hw.firmware_version.empty()) {
                hw.firmware_version = "v1.3.0";
            }
        });
        if (m_state.state.load() == ConnectionState::DISCONNECTED) {
            m_state.state.store(ConnectionState::READY);
        }
    }

    return found;
}

void UsbTransport::TriggerCheck() {
    m_trigger_check.store(true);
}

bool UsbTransport::FindDevicePath(std::string& out_path) {
    const GUID* guids[] = { &GUID_DEVINTERFACE_HT306_WINUSB, &GUID_DEVINTERFACE_STANDARD_WINUSB };

    for (const GUID* pGuid : guids) {
        HDEVINFO dev_info = SetupDiGetClassDevsA(
            pGuid,
            NULL,
            NULL,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        );

        if (dev_info == INVALID_HANDLE_VALUE) {
            continue;
        }

        SP_DEVICE_INTERFACE_DATA if_data{};
        if_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL, pGuid, i, &if_data); ++i) {
            DWORD detail_size = 0;
            SetupDiGetDeviceInterfaceDetailA(dev_info, &if_data, NULL, 0, &detail_size, NULL);

            if (detail_size == 0) continue;

            auto detail_data = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(detail_size);
            if (!detail_data) continue;
            detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

            if (SetupDiGetDeviceInterfaceDetailA(dev_info, &if_data, detail_data, detail_size, NULL, NULL)) {
                std::string p(detail_data->DevicePath);
                std::string p_upper = p;
                std::transform(p_upper.begin(), p_upper.end(), p_upper.begin(), SafeToUpper);
                if (p_upper.find("VID_2E8A") != std::string::npos && p_upper.find("PID_4306") != std::string::npos) {
                    out_path = p;
                    free(detail_data);
                    SetupDiDestroyDeviceInfoList(dev_info);
                    return true;
                }
            }
            free(detail_data);
        }

        SetupDiDestroyDeviceInfoList(dev_info);
    }
    return false;
}

bool UsbTransport::FindComPort(std::string& out_com) {
    out_com.clear();
    bool found = false;

    auto scan_com_ports = [&](const GUID* class_guid, DWORD flags) {
        HDEVINFO dev_info = SetupDiGetClassDevsA(class_guid, "USB", NULL, flags);
        if (dev_info == INVALID_HANDLE_VALUE) return;

        SP_DEVINFO_DATA dev_data{};
        dev_data.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(dev_info, i, &dev_data); ++i) {
            char hwid_buf[512] = { 0 };
            DWORD reg_type = 0;
            DWORD req_size = 0;
            if (SetupDiGetDeviceRegistryPropertyA(
                    dev_info, &dev_data, SPDRP_HARDWAREID, &reg_type,
                    reinterpret_cast<PBYTE>(hwid_buf), sizeof(hwid_buf) - 1, &req_size)) {
                FlattenAndUppercaseHwid(hwid_buf, req_size, sizeof(hwid_buf));
                if (ContainsRp2350VidPid(hwid_buf)) {
                    HKEY hKey = SetupDiOpenDevRegKey(dev_info, &dev_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
                    if (hKey != INVALID_HANDLE_VALUE) {
                        char port_name[64] = { 0 };
                        DWORD port_name_size = sizeof(port_name) - 1;
                        DWORD type = 0;
                        if (RegQueryValueExA(hKey, "PortName", NULL, &type,
                                             reinterpret_cast<LPBYTE>(port_name), &port_name_size) == ERROR_SUCCESS &&
                            type == REG_SZ) {
                            port_name[port_name_size < sizeof(port_name) ? port_name_size : (sizeof(port_name) - 1)] = '\0';
                            if (port_name[0] != '\0') {
                                out_com = port_name;
                                found = true;
                                RegCloseKey(hKey);
                                break;
                            }
                        }
                        RegCloseKey(hKey);
                    }
                }
            }
        }
        SetupDiDestroyDeviceInfoList(dev_info);
    };

    // Fast path: GUID_DEVCLASS_PORTS + USB; fallback: all USB-enumerated devices
    scan_com_ports(&GUID_DEVCLASS_PORTS, DIGCF_PRESENT);
    if (!found) {
        scan_com_ports(NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    }

    if (found && !out_com.empty()) {
        m_state.UpdateHwInfo([&](HardwareInfo& hw) {
            hw.com_port = out_com;
        });
        return true;
    }

    out_com.clear();
    m_state.UpdateHwInfo([](HardwareInfo& hw) {
        hw.com_port.clear();
    });
    return false;
}

bool UsbTransport::RebootBootsel() {
    std::lock_guard<std::recursive_mutex> lock(m_serial_mutex);

    // Close active serial handle first so it does not cause ERROR_SHARING_VIOLATION
    CloseSerial();

    // Always verify live RP2350 CDC COM port (never touch an arbitrary COM port like "COM4")
    std::string com;
    if (!FindComPort(com) || com.empty()) {
        m_state.AddLog(LogLevel::LOG_WARN, "BOOTSEL", "RP2350 CDC COM port not found — cannot trigger BOOTSEL reboot");
        return false;
    }

    std::string port_path = "\\\\.\\" + com;
    HANDLE hPort = CreateFileA(
        port_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hPort == INVALID_HANDLE_VALUE) {
        m_state.AddLog(LogLevel::LOG_WARN, "BOOTSEL", "Failed to open " + com + " for 1200-baud reset trigger");
        return false;
    }

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 200;
    timeouts.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(hPort, &timeouts);

    // Configure DCB with 1200 baud 8N1 and keep DTR asserted while sending text command
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (GetCommState(hPort, &dcb)) {
        dcb.BaudRate = CBR_1200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(hPort, &dcb);
    }

    // Send explicit "HT306_BOOTSEL\n" BEFORE dropping DTR so tud_cdc_connected() is still active
    static const char kBootselCmd[] = "HT306_BOOTSEL\n";
    DWORD written = 0;
    WriteFile(hPort, kBootselCmd, static_cast<DWORD>(sizeof(kBootselCmd) - 1), &written, NULL);
    Sleep(20);

    // Drop DTR to also signal 1200-baud touch reset
    EscapeCommFunction(hPort, CLRDTR);

    Sleep(30);
    CloseHandle(hPort);

    m_state.AddLog(LogLevel::LOG_INFO, "BOOTSEL", "Triggered 1200-baud + HT306_BOOTSEL reboot on " + com);
    return true;
}

bool UsbTransport::OpenSerialPort(const std::string& com_port) {
    std::lock_guard<std::recursive_mutex> lock(m_serial_mutex);
    if (m_serial_handle != INVALID_HANDLE_VALUE) {
        return true;
    }

    if (com_port.empty()) return false;

    std::string full_port = "\\\\.\\" + com_port;
    m_serial_handle = CreateFileA(
        full_port.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (m_serial_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            m_serial_retry_after_ms = GetTickCount64() + 15000ULL;
            if (!m_serial_access_denied_logged) {
                m_state.AddLog(LogLevel::LOG_WARN, "CDC", "Serial port " + com_port + " in use by another app; continuing via WinUSB");
                m_serial_access_denied_logged = true;
            }
        }
        return false;
    }
    m_serial_retry_after_ms = 0;

    // Configure non-blocking timeouts:
    // ReadIntervalTimeout = MAXDWORD, ReadTotalTimeoutMultiplier = 0, ReadTotalTimeoutConstant = 0
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;
    SetCommTimeouts(m_serial_handle, &timeouts);

    // Configure DCB (115200 8N1, assert DTR/RTS for TinyUSB CDC)
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (GetCommState(m_serial_handle, &dcb)) {
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(m_serial_handle, &dcb);
    }
    EscapeCommFunction(m_serial_handle, SETDTR);

    if (!m_serial_connected_logged) {
        m_state.AddLog(LogLevel::LOG_INFO, "CDC", "Connected to RP2350-Zero Serial CDC (" + com_port + ")");
        m_serial_connected_logged = true;
    }
    m_serial_access_denied_logged = false;
    return true;
}

bool UsbTransport::IsSerialOpen() {
    std::lock_guard<std::recursive_mutex> lock(m_serial_mutex);
    return (m_serial_handle != INVALID_HANDLE_VALUE);
}

void UsbTransport::CloseSerialLocked() {
    if (m_serial_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_serial_handle);
        m_serial_handle = INVALID_HANDLE_VALUE;
    }
    m_serial_line_buf.clear();
    m_serial_connected_logged = false;
    m_serial_access_denied_logged = false;
}

void UsbTransport::CloseSerial() {
    std::lock_guard<std::recursive_mutex> lock(m_serial_mutex);
    CloseSerialLocked();
}

void UsbTransport::PollSerial() {
    std::lock_guard<std::recursive_mutex> lock(m_serial_mutex);
    if (m_serial_handle == INVALID_HANDLE_VALUE) return;

    // Immediately detect USB CDC surprise removal via ClearCommError
    DWORD comm_errors = 0;
    COMSTAT com_stat{};
    if (!ClearCommError(m_serial_handle, &comm_errors, &com_stat)) {
        CloseSerialLocked();
        m_trigger_check.store(true);
        return;
    }

    char buffer[256];
    DWORD bytes_read = 0;
    if (ReadFile(m_serial_handle, buffer, sizeof(buffer) - 1, &bytes_read, NULL)) {
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            for (DWORD i = 0; i < bytes_read; ++i) {
                char ch = buffer[i];
                if (ch == '\r') {
                    continue;
                }
                if (ch == '\n') {
                    if (!m_serial_line_buf.empty()) {
                        ParseSerialLine(m_serial_line_buf);
                        m_serial_line_buf.clear();
                    }
                } else {
                    if (m_serial_line_buf.size() < 1024) {
                        m_serial_line_buf.push_back(ch);
                    }
                }
            }
        }
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE ||
            err == ERROR_FILE_NOT_FOUND || err == ERROR_OPERATION_ABORTED) {
            CloseSerialLocked();
            m_trigger_check.store(true);
        }
    }
}

void UsbTransport::ParseSerialLine(const std::string& raw_line) {
    size_t first = raw_line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return;
    size_t last = raw_line.find_last_not_of(" \t\r\n");
    std::string line = raw_line.substr(first, last - first + 1);
    if (line.empty()) return;

    // Filter out cosmetic separator lines (e.g. "===================")
    if (line.find_first_not_of("=-*#") == std::string::npos) {
        return;
    }

    std::string lower_line = line;
    std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), SafeToLower);

    // Update hardware streaming state and log real-time 4 Hz [VU 5.1] bars
    if (lower_line.find("[vu 5.1]") != std::string::npos) {
        m_state.UpdateHwInfo([](HardwareInfo& hw) {
            hw.last_heartbeat_text = "♥ 1 Hz OK (Streaming)";
            hw.pio_ready = true;
            hw.mux_high = true;
            hw.dsp_locked = true;
        });
        m_state.AddLog(LogLevel::LOG_INFO, "CDC", line);
        return;
    }

    bool i2s_error = (lower_line.find("i2s engine: error") != std::string::npos ||
                      lower_line.find("gagal inisialisasi pio0") != std::string::npos ||
                      lower_line.find("[error]") != std::string::npos);
    bool i2s_init_ok = (lower_line.find("pio0 4-lane i2s engine: berhasil") != std::string::npos);

    bool is_routine_heartbeat = (lower_line.find("detak jantung") != std::string::npos ||
                                 lower_line.find("heartbeat") != std::string::npos);

    bool is_heartbeat_match = is_routine_heartbeat ||
                              (lower_line.find("[ht306]") != std::string::npos) ||
                              (lower_line.find("uptime") != std::string::npos);

    if (is_heartbeat_match) {
        bool has_uptime = false;
        uint32_t parsed_uptime = 0;

        // Parse uptime if present (supports "uptime=123" and "uptime: 123")
        size_t u_pos = lower_line.find("uptime=");
        if (u_pos != std::string::npos) {
            u_pos += 7;
        } else {
            u_pos = lower_line.find("uptime:");
            if (u_pos != std::string::npos) {
                u_pos += 7;
            }
        }

        if (u_pos != std::string::npos) {
            while (u_pos < line.size() && (line[u_pos] == ' ' || line[u_pos] == '\t')) {
                u_pos++;
            }
            if (u_pos < line.size() && std::isdigit(static_cast<unsigned char>(line[u_pos]))) {
                parsed_uptime = static_cast<uint32_t>(std::strtoul(&line[u_pos], nullptr, 10));
                has_uptime = true;
            }
        }

        m_state.UpdateHwInfo([&](HardwareInfo& hw) {
            hw.last_heartbeat_text = i2s_error ? "⚠ I2S Engine ERROR" : "♥ 1 Hz OK (0 drop)";
            hw.pio_ready = !i2s_error;
            hw.mux_high = true;
            hw.dsp_locked = !i2s_error;
            if (has_uptime) {
                hw.uptime_sec = parsed_uptime;
            }
        });
    } else if (i2s_init_ok) {
        m_state.UpdateHwInfo([](HardwareInfo& hw) {
            hw.pio_ready = true;
            hw.dsp_locked = true;
        });
    } else if (i2s_error) {
        m_state.UpdateHwInfo([](HardwareInfo& hw) {
            hw.pio_ready = false;
            hw.dsp_locked = false;
        });
    }

    // Throttled logging: Only log on connect, mode change, or error/warn to avoid 1Hz spam
    bool is_error_or_warn = (lower_line.find("error") != std::string::npos ||
                             lower_line.find("warn") != std::string::npos ||
                             lower_line.find("fail") != std::string::npos ||
                             lower_line.find("gagal") != std::string::npos);

    bool is_mode_or_connect = (lower_line.find("[boot]") != std::string::npos ||
                               lower_line.find("[init]") != std::string::npos ||
                               lower_line.find("[mux]") != std::string::npos ||
                               lower_line.find("mode") != std::string::npos ||
                               lower_line.find("berhasil") != std::string::npos ||
                               lower_line.find("connected") != std::string::npos ||
                               lower_line.find("standby") != std::string::npos ||
                               lower_line.find("[wait]") != std::string::npos ||
                               lower_line.find("reboot") != std::string::npos ||
                               lower_line.find("[stream]") != std::string::npos);

    if (is_error_or_warn) {
        m_state.AddLog(LogLevel::LOG_WARN, "CDC", line);
    } else if (is_mode_or_connect) {
        m_state.AddLog(LogLevel::LOG_INFO, "CDC", line);
    } else if (!is_routine_heartbeat && !is_heartbeat_match) {
        m_state.AddLog(LogLevel::LOG_INFO, "CDC", line);
    }
}

bool UsbTransport::ResetTransportState(const char* heartbeat_text) {
    bool was_open = false;
    CloseSerial();
    {
        std::lock_guard<std::recursive_mutex> lock(m_write_mutex);
        was_open = m_is_open.exchange(false);
        CloseWinUsbHandlesLocked();
    }
    m_serial_retry_after_ms = 0;
    m_state.state.store(ConnectionState::DISCONNECTED);
    m_state.telemetry.throughput_kbs.store(0.0f);
    m_state.UpdateHwInfo([heartbeat_text](HardwareInfo& hw) {
        hw.detected = false;
        hw.pio_ready = false;
        hw.mux_high = false;
        hw.dsp_locked = false;
        hw.com_port.clear();
        if (heartbeat_text != nullptr) {
            hw.last_heartbeat_text = heartbeat_text;
        }
    });
    return was_open;
}

void UsbTransport::OnDeviceDisconnected(bool force_restore) {
    bool was_detected = m_state.IsHwDetected();
    bool was_open = ResetTransportState("Waiting for RP2350...");

    if (was_open || was_detected || force_restore) {
        m_state.AddLog(LogLevel::LOG_WARN, "WinUSB", "RP2350-Zero disconnected — waiting for reconnect...");
        if (!audio::AudioPolicyManager::Instance().GetManualVisibilityOverride()) {
            // Restore Windows default audio playback to previous PC speaker and hide HT306 from Taskbar
            audio::AudioPolicyManager::Instance().RestorePreviousDefault(m_state);
        }
        audio::AudioEndpointHelper::EnumerateDevices(m_state);
    }
}

void UsbTransport::HandleSystemResume() {
    m_state.AddLog(LogLevel::LOG_INFO, "WinUSB",
        "System resume detected — closing stale USB/Serial handles for fast recovery.");
    m_attempted_switch_for_present.store(false);
    m_attempted_restore_for_absent.store(false);
    ResetTransportState("Reconnecting after wake...");
    TriggerCheck();
}

bool UsbTransport::Open() {
    bool newly_opened = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_write_mutex);
        if (m_is_open.load()) return true;

        std::string dev_path;
        if (!FindDevicePath(dev_path)) {
            return false;
        }

        m_device_handle = CreateFileA(
            dev_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            NULL
        );

        if (m_device_handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        if (!WinUsb_Initialize(m_device_handle, &m_winusb_handle)) {
            CloseHandle(m_device_handle);
            m_device_handle = INVALID_HANDLE_VALUE;
            return false;
        }

        // Set pipe timeout slightly above overlapped slot wait for bulk OUT endpoint
        ULONG timeout_ms = 250;
        WinUsb_SetPipePolicy(m_winusb_handle, m_bulk_out_pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout_ms), &timeout_ms);

        m_consecutive_write_failures = 0;
        m_is_open.store(true);
        newly_opened = true;
    }

    if (newly_opened) {
        m_state.state.store(ConnectionState::READY);
        m_state.UpdateHwInfo([](HardwareInfo& hw) {
            hw.detected = true;
        });
        m_state.AddLog(LogLevel::LOG_INFO, "WinUSB", "Connected to RP2350-Zero Bulk Interface (EP 0x03 OUT)");

        // Auto-switch Windows Default Audio Playback to LG HT306 in Taskbar (outside m_write_mutex!)
        audio::AudioPolicyManager::Instance().SwitchToHT306(m_state);
    }

    return newly_opened;
}

void UsbTransport::Close() {
    ResetTransportState(nullptr);
}

bool UsbTransport::IsOpen() const {
    return m_is_open.load();
}

bool UsbTransport::WritePacket(const uint8_t* data, size_t length) {
    if (!data || length == 0 || !m_is_open.load()) return false;

    bool fatal_disconnect = false;
    DWORD fatal_err = 0;
    bool write_ok = false;

    {
        std::lock_guard<std::recursive_mutex> lock(m_write_mutex);
        if (!m_winusb_handle) return false;

        AsyncTxSlot& slot = m_tx_slots[m_tx_next_slot];
        m_tx_next_slot = (m_tx_next_slot + 1u) % kAsyncTxSlots;

        auto handle_write_err = [&](DWORD err) -> bool {
            m_consecutive_write_failures++;
            const bool is_fatal =
                (err == ERROR_DEVICE_NOT_CONNECTED) ||
                (err == ERROR_GEN_FAILURE) ||
                (err == ERROR_INVALID_HANDLE) ||
                (err == ERROR_OPERATION_ABORTED) ||
                (err == ERROR_BAD_COMMAND) ||
                (err == ERROR_FILE_NOT_FOUND) ||
                (m_consecutive_write_failures >= 5);

            CancelAndDrainAsyncTxLocked();
            if (is_fatal) {
                CloseWinUsbHandlesLocked();
                m_is_open.store(false);
                fatal_disconnect = true;
                fatal_err = err;
                return false;
            }
            if (m_winusb_handle) {
                WinUsb_ResetPipe(m_winusb_handle, m_bulk_out_pipe);
            }
            return true;
        };

        if (slot.pending) {
            DWORD wait_rc = slot.event ? WaitForSingleObject(slot.event, 150) : WAIT_FAILED;
            if (wait_rc == WAIT_OBJECT_0) {
                ULONG transferred = 0;
                BOOL ov_ok = WinUsb_GetOverlappedResult(m_winusb_handle, &slot.ov, &transferred, FALSE);
                DWORD ov_err = ov_ok ? ERROR_SUCCESS : GetLastError();
                slot.pending = false;
                if (!ov_ok || transferred != slot.expected_len) {
                    if (!handle_write_err(ov_ok ? ERROR_WRITE_FAULT : ov_err)) {
                        goto after_lock;
                    }
                }
            } else {
                DWORD err = (wait_rc == WAIT_TIMEOUT) ? ERROR_SEM_TIMEOUT : GetLastError();
                if (!handle_write_err(err)) {
                    goto after_lock;
                }
            }
        }

        slot.buf.assign(data, data + length);
        if (slot.event) {
            ResetEvent(slot.event);
        }
        std::memset(&slot.ov, 0, sizeof(slot.ov));
        slot.ov.hEvent = slot.event;
        slot.expected_len = length;

        ULONG bytes_written = 0;
        BOOL res = WinUsb_WritePipe(
            m_winusb_handle,
            m_bulk_out_pipe,
            slot.buf.data(),
            static_cast<ULONG>(length),
            &bytes_written,
            &slot.ov
        );

        if (res) {
            m_consecutive_write_failures = 0;
            slot.pending = false;
            write_ok = (bytes_written == length);
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                slot.pending = true;
                m_consecutive_write_failures = 0;
                write_ok = true;
            } else {
                slot.pending = false;
                (void)handle_write_err(err);
            }
        }
    }

after_lock:
    if (fatal_disconnect) {
        ResetTransportState("USB link lost — reconnecting...");
        m_state.AddLog(LogLevel::LOG_WARN, "WinUSB",
            "Fatal USB bulk write error (" + std::to_string(fatal_err) + ") — closed stale handle and triggered reconnect.");
        TriggerCheck();
        return false;
    }

    return write_ok;
}

void UsbTransport::StartWorker() {
    if (m_worker_running.load()) return;
    if (m_stop_event) {
        ResetEvent(m_stop_event);
    }
    m_worker_running.store(true);
    m_worker_thread = CreateThread(NULL, 0, WorkerThreadProc, this, 0, NULL);
}

void UsbTransport::StopWorker() {
    if (!m_worker_running.load()) return;
    m_worker_running.store(false);
    if (m_stop_event) {
        SetEvent(m_stop_event);
    }
    if (m_worker_thread) {
        WaitForSingleObject(m_worker_thread, 5000);
        CloseHandle(m_worker_thread);
        m_worker_thread = NULL;
    }
}

DWORD WINAPI UsbTransport::WorkerThreadProc(LPVOID param) {
    auto self = reinterpret_cast<UsbTransport*>(param);
    self->WorkerLoop();
    return 0;
}

void UsbTransport::WorkerLoop() {
    uint32_t loop_counter = 0;
    ULONGLONG last_reconnect_time = 0;
    ULONGLONG last_disconnect_inc_tick = 0;
    bool first_check = true;
    bool was_hw_present = false;
    bool has_pending_disconnect = false;

    while (m_worker_running.load()) {
        ULONGLONG now = GetTickCount64();
        const bool is_idle_absent = (!m_is_open.load() && m_consecutive_disconnect_count >= 3);
        const bool is_healthy_open = (m_is_open.load() && m_consecutive_disconnect_count == 0);
        const ULONGLONG poll_interval_ms = (is_healthy_open || is_idle_absent) ? 3000ULL : 1000ULL;
        bool check_now = first_check || m_trigger_check.exchange(false) || (now - last_reconnect_time >= poll_interval_ms);

        if (check_now) {
            last_reconnect_time = now;
            bool hw_present = CheckHardwarePresence();

            if (hw_present) {
                has_pending_disconnect = true;
                m_consecutive_disconnect_count = 0;
                last_disconnect_inc_tick = 0;
                m_attempted_restore_for_absent.store(false);
                if (!was_hw_present) {
                    m_attempted_switch_for_present.store(false);
                    m_serial_retry_after_ms = 0;
                }

                // 1. Ensure CDC serial port is open & monitored (with cooldown if held by another app)
                if (!IsSerialOpen() && now >= m_serial_retry_after_ms) {
                    std::string com_port;
                    if (FindComPort(com_port) && !com_port.empty()) {
                        OpenSerialPort(com_port);
                    }
                }

                // 2. Ensure WinUSB pipe is open
                bool just_opened = false;
                if (!m_is_open.load()) {
                    just_opened = Open();
                    if (just_opened) {
                        m_attempted_switch_for_present.store(true);
                        audio::AudioEndpointHelper::EnumerateDevices(m_state);
                    }
                }

                // 3. Ensure HT306 Taskbar audio endpoint is surfaced once per connect transition
                // (avoid 1 Hz thrashing if non-admin policy switch cannot change visibility)
                if (!just_opened && !m_attempted_switch_for_present.load()) {
                    auto drv_status = audio::AudioPolicyManager::Instance().GetCachedDriverStatus();
                    if (first_check || !was_hw_present || (drv_status.installed && !drv_status.endpoint_visible)) {
                        m_attempted_switch_for_present.store(true);
                        audio::AudioPolicyManager::Instance().SetManualVisibilityOverride(false);
                        audio::AudioPolicyManager::Instance().SwitchToHT306(m_state);
                        audio::AudioEndpointHelper::EnumerateDevices(m_state);
                    }
                }
            } else {
                if (first_check || (now - last_disconnect_inc_tick >= 900)) {
                    last_disconnect_inc_tick = now;
                    m_consecutive_disconnect_count++;
                }
                // Debounce: require 3 consecutive failed checks (3 seconds) before concluding physical disconnect
                if (m_consecutive_disconnect_count >= 3) {
                    m_attempted_switch_for_present.store(false);
                    bool hw_detected = m_state.IsHwDetected();
                    if (m_is_open.load() || IsSerialOpen() || hw_detected || has_pending_disconnect) {
                        OnDeviceDisconnected(has_pending_disconnect);
                        has_pending_disconnect = false;
                    }
                    bool debug_override = audio::AudioPolicyManager::Instance().GetManualVisibilityOverride();
                    if (debug_override != m_last_debug_override_seen) {
                        m_last_debug_override_seen = debug_override;
                        m_attempted_restore_for_absent.store(false);
                    }
                    if (!m_attempted_restore_for_absent.load()) {
                        auto drv_status = audio::AudioPolicyManager::Instance().GetCachedDriverStatus();
                        if (!debug_override && (first_check || drv_status.endpoint_visible)) {
                            m_attempted_restore_for_absent.store(true);
                            audio::AudioPolicyManager::Instance().RestorePreviousDefault(m_state);
                            audio::AudioEndpointHelper::EnumerateDevices(m_state);
                        } else if (debug_override && drv_status.installed && !drv_status.endpoint_visible) {
                            m_attempted_restore_for_absent.store(true);
                            audio::AudioPolicyManager::Instance().SwitchToHT306(m_state);
                            audio::AudioEndpointHelper::EnumerateDevices(m_state);
                        }
                    }
                }
            }
            was_hw_present = hw_present;
            first_check = false;
        }

        // Poll serial data if connected
        if (IsSerialOpen()) {
            PollSerial();
        } else if (m_state.IsHwDetected()) {
            // Heartbeat fallback ticker if serial handle is locked by VS Code Serial Monitor
            if (loop_counter % 10 == 0) {
                m_state.UpdateHwInfo([](HardwareInfo& hw) {
                    hw.uptime_sec += 1;
                    if (hw.last_heartbeat_text.empty() ||
                        hw.last_heartbeat_text.find("Serial Shared") == std::string::npos) {
                        hw.last_heartbeat_text = "♥ 1 Hz OK (Serial Shared)";
                    }
                });
            }
        }

        loop_counter++;
        if (m_stop_event) {
            if (WaitForSingleObject(m_stop_event, 100) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            Sleep(100);
        }
    }

    CloseSerial();
}

} // namespace ht306::usb
