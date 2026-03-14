#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <array>

constexpr UINT WM_INPUT_CAPTURE_SHOW_MENU = WM_APP + 0x201;
constexpr UINT WM_INPUT_CAPTURE_UPDATE_CLIP = WM_APP + 0x202;

class SerialStreamer;

class InputCaptureManager {
public:
    explicit InputCaptureManager(SerialStreamer& streamer);
    ~InputCaptureManager();

    void setEnabled(bool enabled);
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    void setAbsoluteMode(bool absolute);
    [[nodiscard]] bool isAbsoluteMode() const noexcept { return absoluteMode_.load(std::memory_order_acquire); }
    void setCaptureRegion(const RECT& screenRect, bool valid);
    void setTargetWindow(HWND hwnd);
    void setTargetResolution(int width, int height);
    void setVideoViewport(const RECT& viewport, bool valid);
    void setMenuChordEnabled(bool enabled);
    [[nodiscard]] bool relativeCaptureActive() const noexcept { return relativeCaptureActive_.load(std::memory_order_acquire); }
    void requestCursorUncapture();
    void applyCursorClip(bool enable);
    void clearModifierState();
    void setOverlayPassthrough(bool enabled);

private:
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);
    static bool shouldConsumeKeyboard(const KBDLLHOOKSTRUCT& data);
    static bool shouldBlockMouse(const MSLLHOOKSTRUCT& data, WPARAM wParam);

    void installHooks();
    void removeHooks();

    void handleKeyboardEvent(WPARAM wParam, const KBDLLHOOKSTRUCT& data);
    void handleMouseEvent(WPARAM wParam, const MSLLHOOKSTRUCT& data);
    void updateModifierState(UINT vk, UINT scanCode, bool extended, bool keyDown);

    void sendKeyboardReport();
    void resetKeyboardState();
    static bool isModifierVirtualKey(UINT vk);
    static std::uint8_t translateVirtualKeyToUsage(UINT vk, UINT scanCode, bool extended);
    std::uint8_t currentModifierBits() const;
    void updateMouseButtonState(WPARAM wParam, const MSLLHOOKSTRUCT& data);
    std::uint8_t currentMouseButtonBits() const;
    bool sendAbsoluteMouseState(POINT point, std::uint8_t buttons, std::int8_t wheel, std::int8_t pan);
    [[nodiscard]] bool isWithinCaptureBounds(POINT pt) const;
    [[nodiscard]] bool getCaptureBounds(RECT& rect) const;
    [[nodiscard]] bool getVideoBounds(RECT& rect) const;
    [[nodiscard]] bool isPointOnTargetWindow(POINT pt) const;
    void startRelativeCapture(const MSLLHOOKSTRUCT& data);
    void stopRelativeCapture(bool suspend);
    void ensureCursorHidden(bool hidden);
    void requestCursorClip(bool enable);
    bool computeClipRect(RECT& rect) const;
    static bool isMouseButtonDownMessage(WPARAM wParam);

    SerialStreamer& streamer_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> absoluteMode_{false};
    std::atomic<bool> captureBoundsValid_{false};
    std::atomic<HWND> targetWindow_{nullptr};
    std::atomic<bool> menuChordEnabled_{false};
    std::atomic<bool> overlayPassthrough_{false};
    std::atomic<int> targetWidth_{1920};
    std::atomic<int> targetHeight_{1080};
    std::atomic<bool> relativeCaptureActive_{false};
    HHOOK keyboardHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    POINT lastMousePoint_{};
    bool hasLastMousePoint_ = false;
    std::vector<std::uint8_t> activeKeys_;
    bool keyboardOverflow_ = false;
    RECT captureBounds_{};
    RECT videoBounds_{};
    mutable std::mutex boundsMutex_;
    std::atomic<bool> videoBoundsValid_{false};
    bool leftCtrl_ = false;
    bool rightCtrl_ = false;
    bool leftShift_ = false;
    bool rightShift_ = false;
    bool leftAlt_ = false;
    bool rightAlt_ = false;
    bool leftWin_ = false;
    bool rightWin_ = false;
    bool menuChordLatched_ = false;
    std::atomic<bool> relativeCaptureSuspended_{false};
    POINT relativeAnchorPoint_{};
    bool cursorHidden_ = false;
    bool cursorClipped_ = false;
    bool skipNextRelativeEvent_ = false;
    bool leftButtonDown_ = false;
    bool rightButtonDown_ = false;
    bool middleButtonDown_ = false;
    bool xButton1Down_ = false;
    bool xButton2Down_ = false;
    mutable std::mutex relativeMutex_;

    static InputCaptureManager* instance_;
    static std::mutex instanceMutex_;
};
