#pragma once

#include "app_state.h"
#include <windows.h>
#include <winusb.h>
#include <string>
#include <mutex>
#include <atomic>

namespace ht306::usb {

class UsbTransport {
public:
    UsbTransport(AppState& state);
    ~UsbTransport();

    bool Open();
    void Close();
    bool IsOpen() const;

    // COM Port auto-discovery for RP2350-Zero CDC ACM
    bool FindComPort(std::string& out_com);

    // Trigger 1200-baud reset into RP2350 USB BOOTSEL mode
    bool RebootBootsel();

    bool WritePacket(const uint8_t* data, size_t length);

    // Background monitoring thread (heartbeat / connection watchdog)
    void StartWorker();
    void StopWorker();

    // Flexible hardware presence check & instant hotplug trigger
    bool CheckHardwarePresence();
    void TriggerCheck();

    // Force-close stale USB & Serial handles and trigger instant reconnect after Windows Sleep/Wake
    void HandleSystemResume();

private:
    void CloseWinUsbHandlesLocked();
    void CloseSerialLocked();
    bool ResetTransportState(const char* heartbeat_text);
    bool FindDevicePath(std::string& out_path);
    bool OpenSerialPort(const std::string& com_port);
    bool IsSerialOpen();
    void CloseSerial();
    void PollSerial();
    void ParseSerialLine(const std::string& raw_line);
    void OnDeviceDisconnected(bool force_restore = false);

    AppState& m_state;
    HANDLE m_device_handle = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE m_winusb_handle = NULL;
    UCHAR m_bulk_out_pipe = 0x03;

    // CDC ACM Serial telemetry handle and buffers
    HANDLE m_serial_handle = INVALID_HANDLE_VALUE;
    std::recursive_mutex m_serial_mutex;
    std::string m_serial_line_buf;
    bool m_serial_connected_logged = false;
    bool m_serial_access_denied_logged = false;
    ULONGLONG m_serial_retry_after_ms = 0;

    // Concurrency and thread management
    std::recursive_mutex m_write_mutex;
    std::atomic<bool> m_is_open{false};
    std::atomic<bool> m_worker_running{false};
    std::atomic<bool> m_trigger_check{true};
    HANDLE m_worker_thread = NULL;
    HANDLE m_stop_event = NULL;
    int m_consecutive_disconnect_count = 0;
    int m_consecutive_write_failures = 0;
    std::atomic<bool> m_attempted_switch_for_present{false};
    std::atomic<bool> m_attempted_restore_for_absent{false};
    bool m_last_debug_override_seen = false;

    static DWORD WINAPI WorkerThreadProc(LPVOID param);
    void WorkerLoop();
};

} // namespace ht306::usb
