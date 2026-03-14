#pragma once

#include "AppDefaults.hpp"

#include <Windows.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SerialStreamer {
public:
    SerialStreamer();
    ~SerialStreamer();

    void start();
    void stop();

    void requestReconnect();
    void setBaudRate(unsigned int baudRate);
    void setPreferredPort(const std::wstring& portName);

    void publishKeyboardReport(const std::array<std::uint8_t, 8>& report);
    void publishMouseReport(const std::array<std::uint8_t, 5>& report);
    void publishMouseAbsoluteReport(const std::array<std::uint8_t, 7>& report);
    void publishMicrophoneSamples(const std::uint8_t* data, std::size_t byteCount);

    [[nodiscard]] bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    enum class PacketType : std::uint8_t {
        Keyboard = 0x01,
        Mouse = 0x02,
        Microphone = 0x03,
        MouseAbsolute = 0x04,
    };

    void enqueuePacket(PacketType type, const std::uint8_t* payload, std::size_t payloadSize);
    void workerLoop();
    bool openDeviceLocked();
    void closeDeviceLocked();
    void flushQueueLocked();
    void trimQueuesLocked();
    void tracePacketDebug(PacketType type, const std::uint8_t* payload, std::size_t payloadSize) const;
    [[nodiscard]] std::wstring findPortName() const;

    std::vector<std::uint8_t> buildPacket(PacketType type, const std::uint8_t* payload, std::size_t payloadSize) const;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<std::uint8_t>> mouseQueue_;
    std::deque<std::vector<std::uint8_t>> keyboardQueue_;
    std::deque<std::vector<std::uint8_t>> microphoneQueue_;
    std::size_t totalQueued_ = 0;
    std::thread worker_;
    std::atomic<bool> running_{false};
    bool exitRequested_ = false;
    bool portDirty_ = false;
    HANDLE portHandle_ = INVALID_HANDLE_VALUE;
    std::wstring currentPortName_;
    std::wstring preferredPortName_;
    unsigned int baudRate_ = AppDefaults::kDefaultSerialBaudRate;
};
