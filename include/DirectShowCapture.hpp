#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct DirectShowCaptureImpl;

class DirectShowCapture {
public:
    enum class PixelFormat {
        BGRA8,
        NV12,
    };

    enum class VideoFormatPreference {
        Auto,
        XRGB,
        NV12,
    };

    struct Frame {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t stride{};
        std::uint64_t timestamp100ns{};
        const std::uint8_t* data{};
        std::size_t dataSize{};
        PixelFormat pixelFormat = PixelFormat::BGRA8;
        bool bottomUp{};
        std::uint32_t sampleWidth{};
        std::uint32_t sampleHeight{};
        std::uint32_t contentLeft{};
        std::uint32_t contentTop{};
        std::uint32_t contentRight{};
        std::uint32_t contentBottom{};
        std::uint32_t nominalFrameRate100{};
    };

    using FrameHandler = std::function<void(const Frame&)>;

    struct Options {
        std::string deviceMoniker;
        bool enableAudio = false;
        std::uint32_t desiredWidth = 0;
        std::uint32_t desiredHeight = 0;
        std::uint32_t desiredFrameRate100 = 0;
        VideoFormatPreference videoFormatPreference = VideoFormatPreference::XRGB;
    };

    DirectShowCapture();
    ~DirectShowCapture();

    void start(FrameHandler handler, const Options& options = {});
    void stop();

    [[nodiscard]] std::string consumeLastError();
    [[nodiscard]] std::string currentDeviceFriendlyName() const;

    DirectShowCapture(const DirectShowCapture&) = delete;
    DirectShowCapture& operator=(const DirectShowCapture&) = delete;

private:
    std::unique_ptr<DirectShowCaptureImpl> impl_;
};
