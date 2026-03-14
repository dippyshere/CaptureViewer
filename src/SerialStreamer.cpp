#include "SerialStreamer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cwctype>

#include <setupapi.h>
#include <devguid.h>

namespace
{
#ifndef SERIAL_STREAMER_DEBUG
#define SERIAL_STREAMER_DEBUG 0
#endif

    constexpr std::size_t kMaxQueuedPackets = 1024;
    constexpr bool kSerialDebug = SERIAL_STREAMER_DEBUG != 0;
    constexpr unsigned int kTargetVid = 0x303A;
    constexpr unsigned int kTargetPid = 0x1001;
    constexpr wchar_t kTargetDescription[] = L"USB JTAG/serial debug unit";
    constexpr unsigned int kTargetVidAlt = 0x1A86;
    constexpr unsigned int kTargetPidAlt = 0x55D3;
    constexpr wchar_t kTargetDescriptionAlt[] = L"USB Single Serial";
    constexpr std::uint8_t kFrameSync0 = 0xD5;
    constexpr std::uint8_t kFrameSync1 = 0xAA;
    constexpr std::uint8_t kTypeKeyboard = 0x01;
    constexpr std::uint8_t kTypeMouse = 0x02;
    constexpr std::uint8_t kTypeMicrophone = 0x03;
    constexpr std::uint8_t kTypeMouseAbsolute = 0x04;
    constexpr DWORD kSerialBacklogThresholdBytes = 16 * 1024; // roughly 0.17 s of audio

    void logSerial(const std::string& message)
    {
        std::ofstream("pckvm.log", std::ios::app) << message << '\n';
    }

    std::string describePacket(const std::vector<std::uint8_t>& packet)
    {
#if SERIAL_STREAMER_DEBUG
        if (packet.size() < 5)
        {
            return "[Serial][Debug] Truncated packet";
        }

        const std::uint8_t type = packet[2];
        const std::size_t length = (static_cast<std::size_t>(packet[3]) << 8) | packet[4];
        const std::uint8_t* payload = packet.data() + 5;
        const std::size_t payloadSize = std::min(length, packet.size() - 5);

        std::ostringstream oss;
        oss << "[Serial][Debug] Sync="
            << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(packet[0])
            << ' ' << std::setw(2) << static_cast<int>(packet[1])
            << " Type=0x" << std::setw(2) << static_cast<int>(type)
            << std::dec << " Length=" << length;

        switch (type)
        {
        case kTypeMouse:
            if (payloadSize >= 5)
            {
                oss << " Mouse(buttons=" << static_cast<int>(payload[0])
                    << ", dx=" << static_cast<int>(static_cast<std::int8_t>(payload[1]))
                    << ", dy=" << static_cast<int>(static_cast<std::int8_t>(payload[2]))
                    << ", wheel=" << static_cast<int>(static_cast<std::int8_t>(payload[3]))
                    << ", pan=" << static_cast<int>(static_cast<std::int8_t>(payload[4])) << ")";
            }
            break;
        case kTypeKeyboard:
            if (payloadSize >= 8)
            {
                oss << " Keyboard(mod=" << static_cast<int>(payload[0]) << ", keys=[";
                for (std::size_t i = 2; i < 8; ++i)
                {
                    if (i > 2)
                    {
                        oss << ',';
                    }
                    oss << static_cast<int>(payload[i]);
                }
                oss << "])";
            }
            break;
        case kTypeMicrophone:
            {
                const std::size_t preview = std::min<std::size_t>(payloadSize, 16);
                oss << " Microphone(samples=" << (payloadSize / 2) << ", data=";
                for (std::size_t i = 0; i < preview; ++i)
                {
                    if (i > 0)
                    {
                        oss << ' ';
                    }
                    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(payload[i]) << std::dec;
                }
                if (payloadSize > preview)
                {
                    oss << " …";
                }
                oss << ')';
            }
            break;
        case kTypeMouseAbsolute:
            if (payloadSize >= 7)
            {
                const std::uint16_t x = static_cast<std::uint16_t>((payload[1] << 8) | payload[2]);
                const std::uint16_t y = static_cast<std::uint16_t>((payload[3] << 8) | payload[4]);
                oss << " MouseAbs(buttons=" << static_cast<int>(payload[0])
                    << ", x=" << x
                    << ", y=" << y
                    << ", wheel=" << static_cast<int>(static_cast<std::int8_t>(payload[5]))
                    << ", pan=" << static_cast<int>(static_cast<std::int8_t>(payload[6])) << ")";
            }
            break;
        default:
            {
                const std::size_t preview = std::min<std::size_t>(payloadSize, 16);
                oss << " Payload=";
                for (std::size_t i = 0; i < preview; ++i)
                {
                    if (i > 0)
                    {
                        oss << ' ';
                    }
                    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(payload[i]) << std::dec;
                }
                if (payloadSize > preview)
                {
                    oss << " …";
                }
            }
            break;
        }

        return oss.str();
#else
        (void)packet;
        return {};
#endif
    }

    std::wstring toLower(std::wstring text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return text;
    }

    std::string narrow(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required <= 1)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(required - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), required, nullptr, nullptr);
        return result;
    }
}

SerialStreamer::SerialStreamer() = default;
SerialStreamer::~SerialStreamer()
{
    stop();
}

void SerialStreamer::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        exitRequested_ = false;
        portDirty_ = true;
    }

    logSerial(std::string("[Serial] TLV debug logging ") + (kSerialDebug ? "enabled" : "disabled"));

    worker_ = std::thread(&SerialStreamer::workerLoop, this);
}

void SerialStreamer::stop()
{
    bool wasRunning = running_.exchange(false);
    if (!wasRunning)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        exitRequested_ = true;
    }
    cv_.notify_all();

    if (worker_.joinable())
    {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    flushQueueLocked();
    closeDeviceLocked();
}

void SerialStreamer::requestReconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    portDirty_ = true;
    cv_.notify_one();
}

void SerialStreamer::setBaudRate(unsigned int baudRate)
{
    if (baudRate == 0)
    {
        baudRate = AppDefaults::kDefaultSerialBaudRate;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (baudRate_ == baudRate)
    {
        return;
    }
    baudRate_ = baudRate;
    portDirty_ = true;
    cv_.notify_one();
}

void SerialStreamer::setPreferredPort(const std::wstring& portName)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (preferredPortName_ == portName)
    {
        return;
    }
    preferredPortName_ = portName;
    portDirty_ = true;
    cv_.notify_one();
}

void SerialStreamer::publishKeyboardReport(const std::array<std::uint8_t, 8>& report)
{
    tracePacketDebug(PacketType::Keyboard, report.data(), report.size());
    if (!isRunning())
    {
        return;
    }
    enqueuePacket(PacketType::Keyboard, report.data(), report.size());
}

void SerialStreamer::publishMouseReport(const std::array<std::uint8_t, 5>& report)
{
    tracePacketDebug(PacketType::Mouse, report.data(), report.size());
    if (!isRunning())
    {
        return;
    }
    enqueuePacket(PacketType::Mouse, report.data(), report.size());
}

void SerialStreamer::publishMouseAbsoluteReport(const std::array<std::uint8_t, 7>& report)
{
    tracePacketDebug(PacketType::MouseAbsolute, report.data(), report.size());
    if (!isRunning())
    {
        return;
    }
    enqueuePacket(PacketType::MouseAbsolute, report.data(), report.size());
}

void SerialStreamer::publishMicrophoneSamples(const std::uint8_t* data, std::size_t byteCount)
{
    if (!data || byteCount == 0 || !isRunning())
    {
        return;
    }

    bool serialReady = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        serialReady = (portHandle_ != INVALID_HANDLE_VALUE) && !portDirty_;
    }
    if (!serialReady)
    {
        return;
    }

    const bool running = isRunning();
    const std::size_t maxChunk = 0xFFFFu;
    std::size_t remaining = byteCount;
    const std::uint8_t* cursor = data;
    while (remaining > 0)
    {
        const std::size_t chunk = std::min(remaining, maxChunk);
        tracePacketDebug(PacketType::Microphone, cursor, chunk);
        if (running)
        {
            enqueuePacket(PacketType::Microphone, cursor, chunk);
        }
        cursor += chunk;
        remaining -= chunk;
    }
}

void SerialStreamer::enqueuePacket(PacketType type, const std::uint8_t* payload, std::size_t payloadSize)
{
    if (!isRunning())
    {
        return;
    }

    auto packet = buildPacket(type, payload, payloadSize);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& targetQueue = [&]() -> std::deque<std::vector<std::uint8_t>>& {
        switch (type)
        {
        case PacketType::Mouse:
        case PacketType::MouseAbsolute:
            return mouseQueue_;
        case PacketType::Keyboard:
            return keyboardQueue_;
        case PacketType::Microphone:
        default:
            return microphoneQueue_;
        }
    }();

    targetQueue.push_back(std::move(packet));
    ++totalQueued_;
    trimQueuesLocked();
    cv_.notify_one();
}

std::vector<std::uint8_t> SerialStreamer::buildPacket(PacketType type, const std::uint8_t* payload, std::size_t payloadSize) const
{
    const std::size_t cappedSize = std::min<std::size_t>(payloadSize, 0xFFFF);
    std::vector<std::uint8_t> packet;
    packet.reserve(5 + cappedSize);
    packet.push_back(kFrameSync0);
    packet.push_back(kFrameSync1);
    packet.push_back(static_cast<std::uint8_t>(type));
    packet.push_back(static_cast<std::uint8_t>((cappedSize >> 8) & 0xFF));
    packet.push_back(static_cast<std::uint8_t>(cappedSize & 0xFF));
    if (payload && cappedSize > 0)
    {
        packet.insert(packet.end(), payload, payload + static_cast<std::ptrdiff_t>(cappedSize));
    }
    return packet;
}

void SerialStreamer::tracePacketDebug(PacketType type, const std::uint8_t* payload, std::size_t payloadSize) const
{
#if SERIAL_STREAMER_DEBUG
    auto packet = buildPacket(type, payload, payloadSize);
    if (!packet.empty())
    {
        logSerial(describePacket(packet));
    }
#else
    (void)type;
    (void)payload;
    (void)payloadSize;
#endif
}

void SerialStreamer::workerLoop()
{
    logSerial("[Serial] Worker thread started");
    while (true)
    {
        std::vector<std::uint8_t> packet;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return exitRequested_ || portDirty_ || totalQueued_ > 0;
            });

            if (exitRequested_)
            {
                break;
            }

            if (portDirty_)
            {
                closeDeviceLocked();
                flushQueueLocked();
                portDirty_ = false;
                if (!openDeviceLocked())
                {
                    portDirty_ = true;
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }
            }

            if (mouseQueue_.empty() && keyboardQueue_.empty() && microphoneQueue_.empty())
            {
                continue;
            }

            bool packetDequeued = false;
            if (!mouseQueue_.empty())
            {
                packet = std::move(mouseQueue_.front());
                mouseQueue_.pop_front();
                packetDequeued = true;
            }
            else if (!keyboardQueue_.empty())
            {
                packet = std::move(keyboardQueue_.front());
                keyboardQueue_.pop_front();
                packetDequeued = true;
            }
            else if (!microphoneQueue_.empty())
            {
                packet = std::move(microphoneQueue_.front());
                microphoneQueue_.pop_front();
                packetDequeued = true;
            }
            if (packetDequeued && totalQueued_ > 0)
            {
                --totalQueued_;
            }
        }

        HANDLE handle = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handle = portHandle_;
        }

        if (handle == INVALID_HANDLE_VALUE)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::size_t offset = 0;
        while (offset < packet.size())
        {
            DWORD written = 0;
            if (!WriteFile(handle,
                           packet.data() + offset,
                           static_cast<DWORD>(packet.size() - offset),
                           &written,
                           nullptr))
            {
                const DWORD error = GetLastError();
                logSerial("[Serial] WriteFile failed with error " + std::to_string(error));
                std::lock_guard<std::mutex> lock(mutex_);
                closeDeviceLocked();
                portDirty_ = true;
                cv_.notify_one();
                break;
            }

            if (written == 0)
            {
                break;
            }

            offset += written;

            DWORD errors = 0;
            COMSTAT status{};
            if (!ClearCommError(handle, &errors, &status))
            {
                logSerial("[Serial] ClearCommError failed after write");
                std::lock_guard<std::mutex> lock(mutex_);
                closeDeviceLocked();
                portDirty_ = true;
                cv_.notify_one();
                break;
            }

            if (status.cbOutQue > kSerialBacklogThresholdBytes)
            {
                logSerial("[Serial] Detected " + std::to_string(status.cbOutQue) + " bytes pending on COM port, reconnecting");
                std::lock_guard<std::mutex> lock(mutex_);
                PurgeComm(handle, PURGE_TXCLEAR | PURGE_RXCLEAR);
                closeDeviceLocked();
                portDirty_ = true;
                cv_.notify_one();
                break;
            }

            if (errors != 0)
            {
                std::ostringstream oss;
                oss << "[Serial] Comm error mask 0x" << std::hex << errors;
                logSerial(oss.str());
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        flushQueueLocked();
        closeDeviceLocked();
    }

    logSerial("[Serial] Worker thread exiting");
}

bool SerialStreamer::openDeviceLocked()
{
    if (portHandle_ != INVALID_HANDLE_VALUE)
    {
        return true;
    }

    std::wstring portName;
    if (!preferredPortName_.empty())
    {
        portName = preferredPortName_;
    }
    else
    {
        portName = findPortName();
    }
    if (portName.empty())
    {
        logSerial("[Serial] Target serial bridge not found");
        return false;
    }

    std::wstring devicePath = portName;
    if (devicePath.rfind(L"\\\\.\\", 0) != 0)
    {
        devicePath = L"\\\\.\\" + devicePath;
    }

    HANDLE handle = CreateFileW(devicePath.c_str(),
                               GENERIC_WRITE,
                               0,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                               nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        logSerial("[Serial] Failed to open port '" + narrow(portName) + "' (error " + std::to_string(GetLastError()) + ")");
        if (!preferredPortName_.empty())
        {
            const std::wstring fallback = findPortName();
            if (!fallback.empty() && fallback != portName)
            {
                logSerial("[Serial] Falling back to auto-detected port '" + narrow(fallback) + "'");
                portName = fallback;
                std::wstring devicePathFallback = fallback;
                if (devicePathFallback.rfind(L"\\.\\", 0) != 0)
                {
                    devicePathFallback = L"\\.\\" + devicePathFallback;
                }
                handle = CreateFileW(devicePathFallback.c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                    nullptr);
            }
        }
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
    }

    SetupComm(handle, 4096, 4096);

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(handle, &dcb))
    {
        logSerial("[Serial] GetCommState failed for port");
        CloseHandle(handle);
        return false;
    }

    dcb.BaudRate = baudRate_;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;

    if (!SetCommState(handle, &dcb))
    {
        logSerial("[Serial] SetCommState failed for port");
        CloseHandle(handle);
        return false;
    }

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;
    SetCommTimeouts(handle, &timeouts);

    PurgeComm(handle, PURGE_TXCLEAR | PURGE_RXCLEAR);

    portHandle_ = handle;
    currentPortName_ = portName;

    logSerial("[Serial] Connected to " + narrow(portName) + " with " + std::to_string(baudRate_) + " baud");
    return true;
}

void SerialStreamer::closeDeviceLocked()
{
    if (portHandle_ != INVALID_HANDLE_VALUE)
    {
        PurgeComm(portHandle_, PURGE_TXCLEAR | PURGE_RXCLEAR);
        CloseHandle(portHandle_);
        portHandle_ = INVALID_HANDLE_VALUE;
        logSerial("[Serial] Disconnected from serial bridge");
    }
    currentPortName_.clear();
}

std::wstring SerialStreamer::findPortName() const
{
    HDEVINFO deviceInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (deviceInfo == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    std::wstring result;
    const std::wstring descEsp = toLower(std::wstring(kTargetDescription));
    const std::wstring descAlt = toLower(std::wstring(kTargetDescriptionAlt));

    auto makeVidToken = [](unsigned int vid) {
        std::wstringstream stream;
        stream << L"vid_" << std::hex << std::nouppercase << std::setw(4) << std::setfill(L'0') << vid;
        return toLower(stream.str());
    };

    auto makePidToken = [](unsigned int pid) {
        std::wstringstream stream;
        stream << L"pid_" << std::hex << std::nouppercase << std::setw(4) << std::setfill(L'0') << pid;
        return toLower(stream.str());
    };

    const std::wstring vidEsp = makeVidToken(kTargetVid);
    const std::wstring pidEsp = makePidToken(kTargetPid);
    const std::wstring vidAlt = makeVidToken(kTargetVidAlt);
    const std::wstring pidAlt = makePidToken(kTargetPidAlt);

    SP_DEVINFO_DATA deviceData{};
    deviceData.cbSize = sizeof(deviceData);

    for (DWORD index = 0; SetupDiEnumDeviceInfo(deviceInfo, index, &deviceData); ++index)
    {
        bool matches = false;

        std::array<wchar_t, 256> buffer{};
        if (SetupDiGetDeviceRegistryPropertyW(deviceInfo,
                                              &deviceData,
                                              SPDRP_DEVICEDESC,
                                              nullptr,
                                              reinterpret_cast<PBYTE>(buffer.data()),
                                              static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                                              nullptr))
        {
            const std::wstring descLower = toLower(std::wstring(buffer.data()));
            if (descLower.find(descEsp) != std::wstring::npos || descLower.find(descAlt) != std::wstring::npos)
            {
                matches = true;
            }
        }

        if (!matches)
        {
            if (SetupDiGetDeviceRegistryPropertyW(deviceInfo,
                                                  &deviceData,
                                                  SPDRP_FRIENDLYNAME,
                                                  nullptr,
                                                  reinterpret_cast<PBYTE>(buffer.data()),
                                                  static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                                                  nullptr))
            {
                const std::wstring friendlyLower = toLower(std::wstring(buffer.data()));
                if (friendlyLower.find(descEsp) != std::wstring::npos || friendlyLower.find(descAlt) != std::wstring::npos)
                {
                    matches = true;
                }
            }
        }

        if (!matches)
        {
            DWORD requiredSize = 0;
            SetupDiGetDeviceRegistryPropertyW(deviceInfo,
                                              &deviceData,
                                              SPDRP_HARDWAREID,
                                              nullptr,
                                              nullptr,
                                              0,
                                              &requiredSize);
            if (requiredSize > 0)
            {
                std::vector<wchar_t> hardwareIds((requiredSize / sizeof(wchar_t)) + 1);
                if (SetupDiGetDeviceRegistryPropertyW(deviceInfo,
                                                       &deviceData,
                                                       SPDRP_HARDWAREID,
                                                       nullptr,
                                                       reinterpret_cast<PBYTE>(hardwareIds.data()),
                                                       requiredSize,
                                                       nullptr))
                {
                    for (const wchar_t* id = hardwareIds.data(); id && *id; id += wcslen(id) + 1)
                    {
                        std::wstring idLower = toLower(id);
                        const bool espMatch = idLower.find(vidEsp) != std::wstring::npos && idLower.find(pidEsp) != std::wstring::npos;
                        const bool altMatch = idLower.find(vidAlt) != std::wstring::npos && idLower.find(pidAlt) != std::wstring::npos;
                        if (espMatch || altMatch)
                        {
                            matches = true;
                            break;
                        }
                    }
                }
            }
        }

        if (!matches)
        {
            continue;
        }

        HKEY deviceKey = SetupDiOpenDevRegKey(deviceInfo, &deviceData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (deviceKey == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        std::array<wchar_t, 256> portName{};
        DWORD bufferSize = static_cast<DWORD>(portName.size() * sizeof(wchar_t));
        DWORD valueType = 0;
        if (RegQueryValueExW(deviceKey, L"PortName", nullptr, &valueType, reinterpret_cast<LPBYTE>(portName.data()), &bufferSize) == ERROR_SUCCESS && valueType == REG_SZ)
        {
            result = portName.data();
        }
        RegCloseKey(deviceKey);

        if (!result.empty())
        {
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfo);
    return result;
}

void SerialStreamer::flushQueueLocked()
{
    mouseQueue_.clear();
    keyboardQueue_.clear();
    microphoneQueue_.clear();
    totalQueued_ = 0;
}

void SerialStreamer::trimQueuesLocked()
{
    auto dropFrom = [&](std::deque<std::vector<std::uint8_t>>& queue)
    {
        if (!queue.empty())
        {
            queue.pop_front();
            if (totalQueued_ > 0)
            {
                --totalQueued_;
            }
            return true;
        }
        return false;
    };

    while (totalQueued_ > kMaxQueuedPackets)
    {
        if (dropFrom(microphoneQueue_))
        {
            continue;
        }
        if (dropFrom(keyboardQueue_))
        {
            continue;
        }
        if (!dropFrom(mouseQueue_))
        {
            break;
        }
    }
}
