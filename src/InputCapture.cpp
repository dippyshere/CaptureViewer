#include "InputCapture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <vector>
#include <limits>
#include <cmath>

namespace
{
    constexpr std::uint8_t kHidModifierLeftCtrl = 0x01;
    constexpr std::uint8_t kHidModifierLeftShift = 0x02;
    constexpr std::uint8_t kHidModifierLeftAlt = 0x04;
    constexpr std::uint8_t kHidModifierLeftGui = 0x08;
    constexpr std::uint8_t kHidModifierRightCtrl = 0x10;
    constexpr std::uint8_t kHidModifierRightShift = 0x20;
    constexpr std::uint8_t kHidModifierRightAlt = 0x40;
    constexpr std::uint8_t kHidModifierRightGui = 0x80;

    constexpr int kMouseDeltaMax = 127;
    constexpr int kMouseDeltaMin = -127;

    void logInput(const std::string& message)
    {
        std::ofstream("pckvm.log", std::ios::app) << message << '\n';
    }

    std::int8_t clampInt8(int value)
    {
        return static_cast<std::int8_t>(std::clamp(value, kMouseDeltaMin, kMouseDeltaMax));
    }

    constexpr UINT kMenuHotkeyVirtualKey = 'M';

    bool isMenuModifierKey(UINT vk)
    {
        switch (vk)
        {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
            return true;
        default:
            return false;
        }
    }

    bool isMenuChordKey(UINT vk)
    {
        return (vk == kMenuHotkeyVirtualKey) || isMenuModifierKey(vk);
    }
}

InputCaptureManager* InputCaptureManager::instance_ = nullptr;
std::mutex InputCaptureManager::instanceMutex_;

InputCaptureManager::InputCaptureManager()
{
    stopRelativeCapture(false);
    removeHooks();
}

void InputCaptureManager::setEnabled(bool enabled)
{
    const bool current = enabled_.load(std::memory_order_acquire);
    if (enabled && !current)
    {
        relativeCaptureSuspended_.store(false, std::memory_order_release);
        activeKeys_.clear();
        keyboardOverflow_ = false;
        hasLastMousePoint_ = false;
        menuChordLatched_ = false;
        skipNextRelativeEvent_ = false;
        menuChordEnabled_.store(false, std::memory_order_release);
        installHooks();
    }
    else if (!enabled && current)
    {
        enabled_.store(false, std::memory_order_release);
        stopRelativeCapture(false);
        removeHooks();
        menuChordLatched_ = false;
        skipNextRelativeEvent_ = false;
        menuChordEnabled_.store(false, std::memory_order_release);
        requestCursorClip(false);
    }
}

void InputCaptureManager::setAbsoluteMode(bool absolute)
{
    const bool previous = absoluteMode_.exchange(absolute, std::memory_order_acq_rel);
    if (previous == absolute)
    {
        return;
    }

    hasLastMousePoint_ = false;
    lastMousePoint_ = POINT{};
    if (absolute)
    {
        stopRelativeCapture(false);
        relativeCaptureSuspended_.store(false, std::memory_order_release);
        requestCursorClip(false);
    }
    else
    {
        relativeCaptureSuspended_.store(false, std::memory_order_release);
        if (relativeCaptureActive_.load(std::memory_order_acquire))
        {
            requestCursorClip(true);
        }
    }
    logInput(std::string("[Input] Mouse mode -> ") + (absolute ? "absolute" : "relative"));
}

void InputCaptureManager::setMenuChordEnabled(bool enabled)
{
    menuChordEnabled_.store(enabled, std::memory_order_release);
}

void InputCaptureManager::setCaptureRegion(const RECT& screenRect, bool valid)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(boundsMutex_);
        const bool previousValid = captureBoundsValid_.load(std::memory_order_relaxed);
        if (previousValid != valid)
        {
            changed = true;
        }
        else if (valid)
        {
            changed = captureBounds_.left != screenRect.left ||
                      captureBounds_.top != screenRect.top ||
                      captureBounds_.right != screenRect.right ||
                      captureBounds_.bottom != screenRect.bottom;
        }

        captureBounds_ = screenRect;
        captureBoundsValid_.store(valid, std::memory_order_release);
        if (!valid)
        {
            videoBoundsValid_.store(false, std::memory_order_release);
        }
    }
    if (!valid)
    {
        hasLastMousePoint_ = false;
        stopRelativeCapture(false);
    }
    if (changed)
    {
        logInput(std::string("[Input] Capture region -> ") + (valid ? "active" : "inactive"));
    }
}

void InputCaptureManager::setTargetWindow(HWND hwnd)
{
    targetWindow_.store(hwnd, std::memory_order_release);
    if (!hwnd)
    {
        ReleaseCapture();
        ClipCursor(nullptr);
        cursorClipped_ = false;
    }
}

void InputCaptureManager::setTargetResolution(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    targetWidth_.store(width, std::memory_order_release);
    targetHeight_.store(height, std::memory_order_release);
}

void InputCaptureManager::setVideoViewport(const RECT& viewport, bool valid)
{
    std::lock_guard<std::mutex> lock(boundsMutex_);
    videoBounds_ = viewport;
    videoBoundsValid_.store(valid, std::memory_order_release);
}

bool InputCaptureManager::getVideoBounds(RECT& rect) const
{
    if (!videoBoundsValid_.load(std::memory_order_acquire))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(boundsMutex_);
    if (!videoBoundsValid_.load(std::memory_order_relaxed))
    {
        return false;
    }
    rect = videoBounds_;
    return true;
}

void InputCaptureManager::requestCursorUncapture()
{
    stopRelativeCapture(true);
}

bool InputCaptureManager::isWithinCaptureBounds(POINT pt) const
{
    RECT bounds{};
    if (!getCaptureBounds(bounds))
    {
        return false;
    }

    if (!(pt.x >= bounds.left && pt.x < bounds.right && pt.y >= bounds.top && pt.y < bounds.bottom))
    {
        return false;
    }

    HWND target = targetWindow_.load(std::memory_order_acquire);
    if (!target)
    {
        return true;
    }

    HWND hitWindow = WindowFromPoint(pt);
    if (!hitWindow)
    {
        return false;
    }

    HWND root = GetAncestor(hitWindow, GA_ROOT);
    if (!root)
    {
        root = hitWindow;
    }

    return root == target;
}

bool InputCaptureManager::isPointOnTargetWindow(POINT pt) const
{
    HWND target = targetWindow_.load(std::memory_order_acquire);
    if (!target)
    {
        return false;
    }

    HWND hitWindow = WindowFromPoint(pt);
    if (!hitWindow)
    {
        return false;
    }

    HWND root = GetAncestor(hitWindow, GA_ROOT);
    if (!root)
    {
        root = hitWindow;
    }

    return root == target;
}

bool InputCaptureManager::getCaptureBounds(RECT& rect) const
{
    if (!captureBoundsValid_.load(std::memory_order_acquire))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(boundsMutex_);
    if (!captureBoundsValid_.load(std::memory_order_relaxed))
    {
        return false;
    }

    rect = captureBounds_;
    return true;
}

void InputCaptureManager::installHooks()
{
    {
        std::lock_guard<std::mutex> guard(instanceMutex_);
        if (instance_ && instance_ != this)
        {
            logInput("[Input] Another instance already installed hooks; skipping");
            enabled_.store(false, std::memory_order_release);
            return;
        }
        instance_ = this;
    }

    HINSTANCE module = GetModuleHandleW(nullptr);
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &InputCaptureManager::keyboardProc, module, 0);
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, &InputCaptureManager::mouseProc, module, 0);

    if (!keyboardHook_ || !mouseHook_)
    {
        logInput("[Input] Failed to install low-level hooks");
        removeHooks();
        enabled_.store(false, std::memory_order_release);
        return;
    }

    enabled_.store(true, std::memory_order_release);
    resetKeyboardState();
    logInput("[Input] Keyboard and mouse hooks installed");
}

void InputCaptureManager::removeHooks()
{
    if (keyboardHook_)
    {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (mouseHook_)
    {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> guard(instanceMutex_);
        if (instance_ == this)
        {
            instance_ = nullptr;
        }
    }

    logInput("[Input] Hooks removed");
    resetKeyboardState();
    hasLastMousePoint_ = false;
}

LRESULT CALLBACK InputCaptureManager::keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HC_ACTION)
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    KBDLLHOOKSTRUCT* data = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    InputCaptureManager* self = nullptr;
    {
        std::lock_guard<std::mutex> guard(instanceMutex_);
        self = instance_;
    }

    if (self && self->enabled_.load(std::memory_order_acquire))
    {
        const bool within = self->shouldConsumeKeyboard(*data);
        const UINT vkCode = static_cast<UINT>(data->vkCode);
        const bool chordEnabled = self->menuChordEnabled_.load(std::memory_order_acquire);
        const bool isChordKey = chordEnabled && isMenuChordKey(vkCode);

        if (within || isChordKey)
        {
            self->handleKeyboardEvent(wParam, *data);
        }

        const bool menuChordActive = self->menuChordLatched_;
        const bool allowChordKey = isChordKey && menuChordActive;
        const bool allowThrough = !within || allowChordKey;

        if (allowThrough)
        {
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }

        return 1;
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool InputCaptureManager::shouldConsumeKeyboard(const KBDLLHOOKSTRUCT& data)
{
    (void)data;

    POINT cursor{};
    if (!GetCursorPos(&cursor))
    {
        return false;
    }

    InputCaptureManager* self = nullptr;
    {
        std::lock_guard<std::mutex> guard(instanceMutex_);
        self = instance_;
    }

    if (!self)
    {
        return false;
    }

    return self->isWithinCaptureBounds(cursor);
}

LRESULT CALLBACK InputCaptureManager::mouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HC_ACTION)
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    MSLLHOOKSTRUCT* data = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    InputCaptureManager* self = nullptr;
    {
        std::lock_guard<std::mutex> guard(instanceMutex_);
        self = instance_;
    }

    if (self && self->enabled_.load(std::memory_order_acquire))
    {
        self->handleMouseEvent(wParam, *data);

        if (shouldBlockMouse(*data, wParam))
        {
            return 1;
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void InputCaptureManager::handleKeyboardEvent(WPARAM wParam, const KBDLLHOOKSTRUCT& data)
{
    const bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!keyDown && !keyUp)
    {
        return;
    }

    const UINT vk = static_cast<UINT>(data.vkCode);
    const bool extended = (data.flags & LLKHF_EXTENDED) != 0;
    const bool injected = (data.flags & LLKHF_INJECTED) != 0;
    const bool chordEnabled = menuChordEnabled_.load(std::memory_order_acquire);
    const bool chordCandidate = chordEnabled && isMenuChordKey(vk);

    if (!chordCandidate && !injected)
    {
        POINT cursor{};
        if (!GetCursorPos(&cursor) || !isWithinCaptureBounds(cursor))
        {
            return;
        }
    }

    updateModifierState(vk, data.scanCode, extended, keyDown);

    const bool ctrlActive = leftCtrl_ || rightCtrl_;
    const bool altActive = leftAlt_ || rightAlt_;
    const bool menuChord = chordEnabled && ctrlActive && altActive;
    const bool isMenuKey = chordEnabled && (vk == kMenuHotkeyVirtualKey);

    if (isMenuKey)
    {
        if (menuChord && keyDown)
        {
            if (!menuChordLatched_)
            {
                menuChordLatched_ = true;
                HWND target = targetWindow_.load(std::memory_order_acquire);
                if (target)
                {
                    PostMessage(target, WM_INPUT_CAPTURE_SHOW_MENU, 0, 0);
                    PostMessage(target, WM_INPUT_CAPTURE_UPDATE_CLIP, 0, 0);
                }
            }
            return;
        }

        if (menuChordLatched_)
        {
            if (keyUp || !menuChord)
            {
                menuChordLatched_ = false;
                if (relativeCaptureActive_.load(std::memory_order_acquire))
                {
                    requestCursorClip(true);
                }
            }
            return;
        }
    }
    else if (menuChordLatched_ && chordEnabled && !menuChord && keyUp && isMenuModifierKey(vk))
    {
        menuChordLatched_ = false;
        if (relativeCaptureActive_.load(std::memory_order_acquire))
        {
            requestCursorClip(true);
        }
        // fall through so releases reach the remote endpoint
    }

    if (!isModifierVirtualKey(vk))
    {
        const std::uint8_t usage = translateVirtualKeyToUsage(vk, data.scanCode, extended);
        if (usage != 0)
        {
            if (keyDown)
            {
                if (std::find(activeKeys_.begin(), activeKeys_.end(), usage) == activeKeys_.end())
                {
                    if (activeKeys_.size() < 6)
                    {
                        activeKeys_.push_back(usage);
                        keyboardOverflow_ = false;
                    }
                    else
                    {
                        keyboardOverflow_ = true;
                    }
                }
            }
            else if (keyUp)
            {
                auto it = std::find(activeKeys_.begin(), activeKeys_.end(), usage);
                if (it != activeKeys_.end())
                {
                    activeKeys_.erase(it);
                }
                keyboardOverflow_ = false;
            }
        }
    }

    sendKeyboardReport();
}

void InputCaptureManager::updateModifierState(UINT vk, UINT scanCode, bool extended, bool keyDown)
{
    UINT effectiveVk = vk;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
    {
        UINT mapped = MapVirtualKey(scanCode, MAPVK_VSC_TO_VK_EX);
        if (mapped != 0)
        {
            effectiveVk = mapped;
        }
        else if (vk == VK_CONTROL)
        {
            effectiveVk = extended ? VK_RCONTROL : VK_LCONTROL;
        }
        else if (vk == VK_MENU)
        {
            effectiveVk = extended ? VK_RMENU : VK_LMENU;
        }
        else if (vk == VK_SHIFT)
        {
            effectiveVk = (scanCode == 0x36) ? VK_RSHIFT : VK_LSHIFT;
        }
    }

    switch (effectiveVk)
    {
    case VK_LCONTROL: leftCtrl_ = keyDown; break;
    case VK_RCONTROL: rightCtrl_ = keyDown; break;
    case VK_LSHIFT: leftShift_ = keyDown; break;
    case VK_RSHIFT: rightShift_ = keyDown; break;
    case VK_LMENU: leftAlt_ = keyDown; break;
    case VK_RMENU: rightAlt_ = keyDown; break;
    case VK_LWIN: leftWin_ = keyDown; break;
    case VK_RWIN: rightWin_ = keyDown; break;
    default:
        break;
    }
}

void InputCaptureManager::handleMouseEvent(WPARAM wParam, const MSLLHOOKSTRUCT& data)
{
    const bool absoluteMode = absoluteMode_.load(std::memory_order_acquire);
    const bool injected = (data.flags & LLMHF_INJECTED) != 0;

    const bool insideBounds = isWithinCaptureBounds(data.pt);
    if (!insideBounds && !injected)
    {
        hasLastMousePoint_ = false;
        if (!absoluteMode)
        {
            stopRelativeCapture(false);
        }
        return;
    }

    if (absoluteMode)
    {
        stopRelativeCapture(false);
    }

    if (!absoluteMode)
    {
        if (!relativeCaptureActive_.load(std::memory_order_acquire))
        {
            startRelativeCapture(data);
            if (!relativeCaptureActive_.load(std::memory_order_acquire))
            {
                hasLastMousePoint_ = false;
                return;
            }
        }

        if (skipNextRelativeEvent_ && wParam == WM_MOUSEMOVE)
        {
            skipNextRelativeEvent_ = false;
            return;
        }
    }

    std::int8_t wheel = 0;
    std::int8_t pan = 0;
    if (wParam == WM_MOUSEWHEEL)
    {
        const int steps = static_cast<int>(static_cast<SHORT>(HIWORD(data.mouseData))) / WHEEL_DELTA;
        wheel = clampInt8(steps);
    }
    else if (wParam == WM_MOUSEHWHEEL)
    {
        const int steps = static_cast<int>(static_cast<SHORT>(HIWORD(data.mouseData))) / WHEEL_DELTA;
        pan = clampInt8(steps);
    }

    updateMouseButtonState(wParam, data);
    std::uint8_t buttons = currentMouseButtonBits();

    if (absoluteMode)
    {
        if (sendAbsoluteMouseState(data.pt, buttons, wheel, pan))
        {
            lastMousePoint_ = data.pt;
            hasLastMousePoint_ = true;
        }
        else
        {
            hasLastMousePoint_ = false;
        }
        return;
    }
    else
    {
        POINT anchor{};
        {
            std::lock_guard<std::mutex> lock(relativeMutex_);
            anchor = relativeAnchorPoint_;
        }

        if (!relativeCaptureActive_.load(std::memory_order_acquire))
        {
            return;
        }

        int dx = data.pt.x - anchor.x;
        int dy = data.pt.y - anchor.y;

        std::int8_t dx8 = clampInt8(dx);
        std::int8_t dy8 = clampInt8(dy);

        std::array<std::uint8_t, 5> report{};
        report[0] = static_cast<std::uint8_t>(buttons & 0x1F);
        report[1] = static_cast<std::uint8_t>(dx8);
        report[2] = static_cast<std::uint8_t>(dy8);
        report[3] = static_cast<std::uint8_t>(wheel);
        report[4] = static_cast<std::uint8_t>(pan);

        SetCursorPos(anchor.x, anchor.y);
    }
}

bool InputCaptureManager::shouldBlockMouse(const MSLLHOOKSTRUCT& data, WPARAM wParam)
{
    (void)data;

    InputCaptureManager* self = nullptr;
    {
        std::lock_guard<std::mutex> guard(instanceMutex_);
        self = instance_;
    }

    if (!self)
    {
        return false;
    }

    if (self->absoluteMode_.load(std::memory_order_acquire))
    {
        return false;
    }

    if (!self->relativeCaptureActive_.load(std::memory_order_acquire))
    {
        return false;
    }

    switch (wParam)
    {
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return true;
    default:
        break;
    }

    return false;
}

std::uint8_t InputCaptureManager::currentModifierBits() const
{
    std::uint8_t bits = 0;
    if (leftCtrl_) bits |= kHidModifierLeftCtrl;
    if (leftShift_) bits |= kHidModifierLeftShift;
    if (leftAlt_) bits |= kHidModifierLeftAlt;
    if (leftWin_) bits |= kHidModifierLeftGui;
    if (rightCtrl_) bits |= kHidModifierRightCtrl;
    if (rightShift_) bits |= kHidModifierRightShift;
    if (rightAlt_) bits |= kHidModifierRightAlt;
    if (rightWin_) bits |= kHidModifierRightGui;
    return bits;
}


void InputCaptureManager::sendKeyboardReport()
{
    std::array<std::uint8_t, 8> report{};
    report[0] = currentModifierBits();

    if (keyboardOverflow_)
    {
        std::fill(report.begin() + 2, report.end(), 0x01);
    }
    else
    {
        std::size_t index = 2;
        for (std::uint8_t usage : activeKeys_)
        {
            if (index >= report.size())
            {
                break;
            }
            report[index++] = usage;
        }
    }
}

void InputCaptureManager::resetKeyboardState()
{
    activeKeys_.clear();
    keyboardOverflow_ = false;
    leftCtrl_ = rightCtrl_ = leftShift_ = rightShift_ = false;
    leftAlt_ = rightAlt_ = leftWin_ = rightWin_ = false;
    menuChordLatched_ = false;
    skipNextRelativeEvent_ = false;
    leftButtonDown_ = rightButtonDown_ = middleButtonDown_ = false;
    xButton1Down_ = xButton2Down_ = false;
    std::array<std::uint8_t, 8> report{};
}

void InputCaptureManager::clearModifierState()
{
    const bool hadButtons = leftButtonDown_ || rightButtonDown_ || middleButtonDown_ || xButton1Down_ || xButton2Down_;

    leftCtrl_ = rightCtrl_ = leftShift_ = rightShift_ = false;
    leftAlt_ = rightAlt_ = leftWin_ = rightWin_ = false;
    menuChordLatched_ = false;
    leftButtonDown_ = rightButtonDown_ = middleButtonDown_ = false;
    xButton1Down_ = xButton2Down_ = false;
    sendKeyboardReport();

    if (hadButtons)
    {
        if (absoluteMode_.load(std::memory_order_acquire) && hasLastMousePoint_)
        {
            (void)sendAbsoluteMouseState(lastMousePoint_, 0, 0, 0);
        }
        else if (!absoluteMode_.load(std::memory_order_acquire) && relativeCaptureActive_.load(std::memory_order_acquire))
        {
            std::array<std::uint8_t, 5> report{};
        }
    }
}

void InputCaptureManager::updateMouseButtonState(WPARAM wParam, const MSLLHOOKSTRUCT& data)
{
    switch (wParam)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        leftButtonDown_ = true;
        break;
    case WM_LBUTTONUP:
        leftButtonDown_ = false;
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        rightButtonDown_ = true;
        break;
    case WM_RBUTTONUP:
        rightButtonDown_ = false;
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        middleButtonDown_ = true;
        break;
    case WM_MBUTTONUP:
        middleButtonDown_ = false;
        break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    {
        const WORD buttonFlags = HIWORD(data.mouseData);
        const bool pressed = (wParam == WM_XBUTTONDOWN) || (wParam == WM_XBUTTONDBLCLK);
        if (buttonFlags & XBUTTON1)
        {
            xButton1Down_ = pressed;
        }
        if (buttonFlags & XBUTTON2)
        {
            xButton2Down_ = pressed;
        }
        break;
    }
    default:
        break;
    }
}

std::uint8_t InputCaptureManager::currentMouseButtonBits() const
{
    std::uint8_t bits = 0;
    if (leftButtonDown_) bits |= 0x01;
    if (rightButtonDown_) bits |= 0x02;
    if (middleButtonDown_) bits |= 0x04;
    if (xButton1Down_) bits |= 0x08;
    if (xButton2Down_) bits |= 0x10;
    return bits;
}

bool InputCaptureManager::sendAbsoluteMouseState(POINT point, std::uint8_t buttons, std::int8_t wheel, std::int8_t pan)
{
    RECT bounds{};
    if (!getCaptureBounds(bounds))
    {
        return false;
    }

    RECT videoBounds{};
    bool haveVideoBounds = getVideoBounds(videoBounds);
    if (!haveVideoBounds)
    {
        videoBounds = bounds;
    }

    if (haveVideoBounds)
    {
        if (point.x < videoBounds.left || point.x >= videoBounds.right ||
            point.y < videoBounds.top || point.y >= videoBounds.bottom)
        {
            return false;
        }
    }

    const long width = std::max<long>(videoBounds.right - videoBounds.left, 1);
    const long height = std::max<long>(videoBounds.bottom - videoBounds.top, 1);

    const long clampedX = std::clamp<long>(point.x - videoBounds.left, 0, width - 1);
    const long clampedY = std::clamp<long>(point.y - videoBounds.top, 0, height - 1);

    int targetW = targetWidth_.load(std::memory_order_acquire);
    int targetH = targetHeight_.load(std::memory_order_acquire);
    if (targetW <= 0)
    {
        targetW = width;
    }
    if (targetH <= 0)
    {
        targetH = height;
    }

    const long scaledX = (width > 1) ? (clampedX * static_cast<long long>(targetW - 1) / std::max<long>(width - 1, 1)) : 0;
    const long scaledY = (height > 1) ? (clampedY * static_cast<long long>(targetH - 1) / std::max<long>(height - 1, 1)) : 0;

    constexpr std::uint16_t kAbsoluteMax = static_cast<std::uint16_t>(std::numeric_limits<short>::max()); // 32767
    const double normX = (targetW > 1) ? static_cast<double>(scaledX) / static_cast<double>(targetW - 1) : 0.0;
    const double normY = (targetH > 1) ? static_cast<double>(scaledY) / static_cast<double>(targetH - 1) : 0.0;

    const std::uint16_t absX = static_cast<std::uint16_t>(std::clamp<long>(static_cast<long>(std::lround(normX * kAbsoluteMax)), 0, kAbsoluteMax));
    const std::uint16_t absY = static_cast<std::uint16_t>(std::clamp<long>(static_cast<long>(std::lround(normY * kAbsoluteMax)), 0, kAbsoluteMax));

    std::array<std::uint8_t, 7> report{};
    report[0] = static_cast<std::uint8_t>(buttons & 0x1F);
    report[1] = static_cast<std::uint8_t>((absX >> 8) & 0xFF);
    report[2] = static_cast<std::uint8_t>(absX & 0xFF);
    report[3] = static_cast<std::uint8_t>((absY >> 8) & 0xFF);
    report[4] = static_cast<std::uint8_t>(absY & 0xFF);
    report[5] = static_cast<std::uint8_t>(wheel);
    report[6] = static_cast<std::uint8_t>(pan);

    return true;
}

bool InputCaptureManager::isModifierVirtualKey(UINT vk)
{
    switch (vk)
    {
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_SHIFT:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_CONTROL:
    case VK_LMENU:
    case VK_RMENU:
    case VK_MENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

std::uint8_t InputCaptureManager::translateVirtualKeyToUsage(UINT vk, UINT scanCode, bool extended)
{
    // Alphabetic keys
    if (vk >= 'A' && vk <= 'Z')
    {
        return static_cast<std::uint8_t>(0x04 + (vk - 'A'));
    }

    // Number row (shiftless values)
    if (vk >= '1' && vk <= '9')
    {
        return static_cast<std::uint8_t>(0x1E + (vk - '1'));
    }
    if (vk == '0')
    {
        return 0x27;
    }

    switch (vk)
    {
    case VK_RETURN:
        return extended ? 0x58 : 0x28;
    case VK_ESCAPE:
        return 0x29;
    case VK_BACK:
        return 0x2A;
    case VK_TAB:
        return 0x2B;
    case VK_SPACE:
        return 0x2C;
    case VK_OEM_MINUS:
        return 0x2D;
    case VK_OEM_PLUS:
        return 0x2E;
    case VK_OEM_4: // [
        return 0x2F;
    case VK_OEM_6: // ]
        return 0x30;
    case VK_OEM_5: // backslash
        return 0x31;
    case VK_OEM_1: // ;
        return 0x33;
    case VK_OEM_7: // '
        return 0x34;
    case VK_OEM_3: // `
        return 0x35;
    case VK_OEM_COMMA:
        return 0x36;
    case VK_OEM_PERIOD:
        return 0x37;
    case VK_OEM_2: // /
        return 0x38;
    case VK_CAPITAL:
        return 0x39;
    case VK_F1:
    case VK_F2:
    case VK_F3:
    case VK_F4:
    case VK_F5:
    case VK_F6:
    case VK_F7:
    case VK_F8:
    case VK_F9:
    case VK_F10:
    case VK_F11:
    case VK_F12:
        return static_cast<std::uint8_t>(0x3A + (vk - VK_F1));
    case VK_PRINT:
    case VK_SNAPSHOT:
        return 0x46;
    case VK_SCROLL:
        return 0x47;
    case VK_PAUSE:
        return 0x48;
    case VK_INSERT:
        return 0x49;
    case VK_HOME:
        return 0x4A;
    case VK_PRIOR: // Page Up
        return 0x4B;
    case VK_DELETE:
        return 0x4C;
    case VK_END:
        return 0x4D;
    case VK_NEXT: // Page Down
        return 0x4E;
    case VK_RIGHT:
        return 0x4F;
    case VK_LEFT:
        return 0x50;
    case VK_DOWN:
        return 0x51;
    case VK_UP:
        return 0x52;
    case VK_NUMLOCK:
        return 0x53;
    case VK_DIVIDE:
        return 0x54;
    case VK_MULTIPLY:
        return 0x55;
    case VK_SUBTRACT:
        return 0x56;
    case VK_ADD:
        return 0x57;
    case VK_SEPARATOR:
        return 0x58;
    case VK_DECIMAL:
        return 0x63;
    case VK_CLEAR:
        return 0x5D;
    case VK_APPS:
        return 0x65;
    case VK_OEM_102: // Non-US \\|
        return 0x64;
    }

    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9)
    {
        return static_cast<std::uint8_t>(0x59 + (vk - VK_NUMPAD1));
    }
    if (vk == VK_NUMPAD0)
    {
        return 0x62;
    }

    if (vk >= VK_F13 && vk <= VK_F24)
    {
        return static_cast<std::uint8_t>(0x68 + (vk - VK_F13));
    }

    // Attempt to map additional OEM keys via scan code for specific layouts
    if (!extended)
    {
        switch (scanCode & 0xFF)
        {
        case 0x56: // OEM 102 on some layouts
            return 0x64;
        default:
            break;
        }
    }

    return 0;
}


bool InputCaptureManager::isMouseButtonDownMessage(WPARAM wParam)
{
    switch (wParam)
    {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        return true;
    default:
        return false;
    }
}

void InputCaptureManager::startRelativeCapture(const MSLLHOOKSTRUCT& data)
{
    if (absoluteMode_.load(std::memory_order_acquire))
    {
        return;
    }

    bool activated = false;
    POINT anchor = data.pt;
    RECT bounds{};
    if (getCaptureBounds(bounds))
    {
        anchor.x = std::clamp(anchor.x, bounds.left, bounds.right - 1);
        anchor.y = std::clamp(anchor.y, bounds.top, bounds.bottom - 1);
    }
    {
        std::lock_guard<std::mutex> lock(relativeMutex_);
        if (relativeCaptureActive_.load(std::memory_order_acquire))
        {
            return;
        }

        relativeAnchorPoint_ = anchor;
        relativeCaptureActive_.store(true, std::memory_order_release);
        relativeCaptureSuspended_.store(false, std::memory_order_release);
        ensureCursorHidden(true);
        skipNextRelativeEvent_ = true;
        activated = true;
    }

    if (activated)
    {
        SetCursorPos(anchor.x, anchor.y);
        requestCursorClip(true);
    }

    lastMousePoint_ = anchor;
    hasLastMousePoint_ = true;
}

void InputCaptureManager::stopRelativeCapture(bool suspend)
{
    const bool wasActive = relativeCaptureActive_.exchange(false, std::memory_order_acq_rel);

    {
        std::lock_guard<std::mutex> lock(relativeMutex_);
        if (wasActive)
        {
            ensureCursorHidden(false);
        }
    }

    if (wasActive)
    {
        requestCursorClip(false);
    }

    relativeCaptureSuspended_.store(suspend, std::memory_order_release);

    if (wasActive)
    {
        hasLastMousePoint_ = false;
    }

    skipNextRelativeEvent_ = false;
}

void InputCaptureManager::ensureCursorHidden(bool hidden)
{
    if (cursorHidden_ == hidden)
    {
        return;
    }

    if (hidden)
    {
        while (ShowCursor(FALSE) >= 0) {}
    }
    else
    {
        while (ShowCursor(TRUE) < 0) {}
    }

    cursorHidden_ = hidden;
}

void InputCaptureManager::applyCursorClip(bool enable)
{
    HWND target = targetWindow_.load(std::memory_order_acquire);
    if (!target)
    {
        return;
    }

    if (enable)
    {
        RECT clipRect{};
        if (!computeClipRect(clipRect))
        {
            ReleaseCapture();
            ClipCursor(nullptr);
            cursorClipped_ = false;
            return;
        }

        SetCapture(target);
        cursorClipped_ = ClipCursor(&clipRect) != FALSE;
    }
    else
    {
        ReleaseCapture();
        ClipCursor(nullptr);
        cursorClipped_ = false;
    }
}

void InputCaptureManager::requestCursorClip(bool enable)
{
    HWND target = targetWindow_.load(std::memory_order_acquire);
    if (!target)
    {
        return;
    }

    DWORD windowThread = GetWindowThreadProcessId(target, nullptr);
    if (windowThread == GetCurrentThreadId())
    {
        applyCursorClip(enable);
        return;
    }

    PostMessage(target, WM_INPUT_CAPTURE_UPDATE_CLIP, enable ? 1 : 0, 0);
}

bool InputCaptureManager::computeClipRect(RECT& rect) const
{
    HWND target = targetWindow_.load(std::memory_order_acquire);
    if (!target)
    {
        return false;
    }

    RECT client{};
    if (!GetClientRect(target, &client))
    {
        return false;
    }

    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    if (!ClientToScreen(target, &topLeft) || !ClientToScreen(target, &bottomRight))
    {
        return false;
    }

    rect.left = topLeft.x;
    rect.top = topLeft.y;
    rect.right = bottomRight.x;
    rect.bottom = bottomRight.y;

    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return false;
    }

    rect.right -= 1;
    rect.bottom -= 1;

    if (rect.right < rect.left)
    {
        rect.right = rect.left;
    }
    if (rect.bottom < rect.top)
    {
        rect.bottom = rect.top;
    }

    return true;
}
