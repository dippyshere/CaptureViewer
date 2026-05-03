#pragma once

#define UNICODE

#include "DirectShowCapture.hpp"
#include "D3DRenderer.hpp"
#include "Settings.hpp"
#include "AudioPlayback.hpp"
#include "OverlayUI.hpp"
#include "DeviceEnumeration.hpp"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

class Application {
public:
    Application();
    ~Application();

    int run();

private:
    friend class OverlayUI;
    struct CpuFrame {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        std::uint64_t timestamp100ns = 0;
        DirectShowCapture::PixelFormat pixelFormat = DirectShowCapture::PixelFormat::BGRA8;
        std::vector<std::uint8_t> data;
    };

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool createWindow(int width, int height);
    void destroyWindow();
    void handleFrame(const DirectShowCapture::Frame& frame);
    void renderLoop();
    void parseCommandLine();
    void loadPersistentSettings();
    void savePersistentSettings();
    void showSettingsMenu();
    void applyAudioPlaybackSetting();
    void restartVideoCapture();
    bool shouldUseVideoAudio() const;
    bool shouldEnableCaptureAudio() const;
    void applySourceDimensions(std::uint32_t width, std::uint32_t height);
    bool resizeWindowToClient(int width, int height, bool preserveClientPosition = false, const POINT* clientOrigin = nullptr);
    void updateWindowResizeMode(bool preserveClientPosition = false);
    void captureWindowPlacementForPersistence();
    bool applyLockedWindowSize(MINMAXINFO* info) const;
    RECT computeVideoViewport(const RECT& clientRect, bool& valid) const;
    bool uploadLatestFrame();
    bool renderFrame(bool forcePresent);
    void setAudioPlaybackEnabled(bool enabled);
    void selectVideoDevice(const std::string& moniker);
    void selectAudioDevice(const std::string& moniker);
    void setAudioOutputUseDefaultOnly(bool enabled);
    void setAudioOutputDeviceSelected(const std::string& moniker, bool selected);
    void setVideoResolution(std::uint32_t width, std::uint32_t height);
    void setVideoFrameRate100(std::uint32_t frameRate100);
    void setVideoAllowResizing(bool enabled);
    void setVideoAspectMode(VideoAspectMode mode);
    void setVideoFormatPreference(VideoFormatPreference preference);
    void setVSyncEnabled(bool enabled);
    void setBorderlessWindowed(bool enabled);
    void setFullscreen(bool enabled);
    void recenterWindow();
    void requestImmediateRender();
    void processPendingSourceDimensions();
    static std::string toLowerCopy(const std::string& text);
    HWND hwnd() const { return hwnd_; }
    AppSettings& settings() { return settings_; }
    const AppSettings& settings() const { return settings_; }
    std::uint32_t currentCaptureWidth() const { return currentSourceWidth_.load(std::memory_order_acquire); }
    std::uint32_t currentCaptureHeight() const { return currentSourceHeight_.load(std::memory_order_acquire); }
    std::uint32_t currentCaptureFrameRate100() const { return currentSourceFrameRate100_.load(std::memory_order_acquire); }

    HWND hwnd_ = nullptr;
    D3DRenderer renderer_;
    DirectShowCapture directShowCapture_;

    std::mutex frameMutex_;
    CpuFrame frames_[2];
    std::atomic<std::uint64_t> frameCounter_{0};
    std::uint64_t lastPresentedFrame_ = 0;
    int frontBufferIndex_ = 0;
    bool running_ = false;
    bool classRegistered_ = false;
    bool audioEnabled_ = false;

    AudioPlayback audioPlayback_;
    OverlayUI overlay_;

    SettingsManager settingsManager_;
    AppSettings settings_{};
    std::atomic<std::uint32_t> pendingSourceWidth_{0};
    std::atomic<std::uint32_t> pendingSourceHeight_{0};
    std::atomic<std::uint32_t> pendingSourceFrameRate100_{0};
    std::atomic<bool> sourceChangePending_{false};
    std::atomic<std::uint32_t> currentSourceWidth_{0};
    std::atomic<std::uint32_t> currentSourceHeight_{0};
    std::atomic<std::uint32_t> currentSourceFrameRate100_{0};
    int lockedClientWidth_ = 0;
    int lockedClientHeight_ = 0;
    bool suppressCaptureDrivenResize_ = false;
    bool initialMaximizePending_ = false;
    bool initialMaximizeRestoreSeen_ = false;
    bool restoreWindowPlacementAfterFullscreen_ = false;
    std::atomic<bool> forceRender_{false};
    std::chrono::steady_clock::time_point overlayNextFrameDeadline_;
    HANDLE frameReadyEvent_ = nullptr;
};
