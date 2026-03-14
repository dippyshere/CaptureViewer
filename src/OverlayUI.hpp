#pragma once

#include "AppDefaults.hpp"
#include "DeviceEnumeration.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <string>
#include <vector>

struct ImDrawData;
class Application;
class D3DRenderer;

class OverlayUI {
public:
    OverlayUI() = default;
    ~OverlayUI();

    bool initialize(HWND hwnd, D3DRenderer& renderer);
    void shutdown();

    void newFrame();
    void buildUI(Application& app);
    void endFrame();
    void render(ID3D12GraphicsCommandList* commandList);
    bool hasDrawData() const { return drawDataValid_; }

    bool processEvent(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void toggleMenu(Application& app);
    void hideMenu(Application& app);
    bool isMenuVisible() const { return menuVisible_; }

private:
    struct BridgeOption {
        SerialPortInfo port;
        unsigned int suggestedBaud = AppDefaults::kDefaultSerialBaudRate;
    };

    void showMenu(Application& app);
    void refreshDeviceLists(Application& app);
    void refreshVideoModes(Application& app);
    void drawMenuWindow(Application& app);

    HWND hwnd_ = nullptr;
    bool initialized_ = false;
    bool menuVisible_ = false;
    bool drawDataValid_ = false;

    D3DRenderer* renderer_ = nullptr;
    ID3D12DescriptorHeap* srvHeap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE fontCpuHandle_{};
    D3D12_GPU_DESCRIPTOR_HANDLE fontGpuHandle_{};

    ImDrawData* drawData_ = nullptr;

    std::vector<VideoDeviceInfo> videoDevices_;
    std::vector<AudioCaptureDeviceInfo> audioDevices_;
    std::vector<MicrophoneDeviceInfo> microphoneDevices_;
    std::vector<BridgeOption> bridgeDevices_;
    std::vector<VideoModeInfo> videoModes_;
    unsigned int bridgeBaudRateDraft_ = AppDefaults::kDefaultSerialBaudRate;
};
