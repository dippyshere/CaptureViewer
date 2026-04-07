#include "OverlayUI.hpp"

#include "Application.hpp"
#include "D3DRenderer.hpp"
#include "Settings.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
#include "backends/imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

OverlayUI::~OverlayUI()
{
    shutdown();
}

bool OverlayUI::initialize(HWND hwnd, D3DRenderer& renderer)
{
    if (initialized_)
    {
        return true;
    }

    hwnd_ = hwnd;
    renderer_ = &renderer;
    srvHeap_ = renderer.srvHeap();
    fontCpuHandle_ = renderer.imguiSrvCpuHandle();
    fontGpuHandle_ = renderer.imguiSrvGpuHandle();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->Build();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX12_Init(renderer.device(), static_cast<int>(renderer.frameCount()), renderer.renderTargetFormat(),
                        srvHeap_, fontCpuHandle_, fontGpuHandle_);

    initialized_ = true;
    return true;
}

void OverlayUI::shutdown()
{
    if (!initialized_)
    {
        return;
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    initialized_ = false;
    menuVisible_ = false;
    srvHeap_ = nullptr;
    renderer_ = nullptr;
    drawData_ = nullptr;
    drawDataValid_ = false;
}

void OverlayUI::newFrame()
{
    if (!initialized_)
    {
        return;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = menuVisible_;

}

void OverlayUI::buildUI(Application& app)
{
    if (!initialized_ || !menuVisible_)
    {
        return;
    }

    drawMenuWindow(app);
}

void OverlayUI::endFrame()
{
    if (!initialized_)
    {
        return;
    }

    ImGui::Render();
    drawData_ = ImGui::GetDrawData();
    drawDataValid_ = (drawData_ != nullptr) && (drawData_->CmdListsCount > 0);
}

void OverlayUI::render(ID3D12GraphicsCommandList* commandList)
{
    if (!initialized_ || !drawData_)
    {
        drawDataValid_ = false;
        return;
    }

    if (drawData_->CmdListsCount > 0)
    {
        ID3D12DescriptorHeap* heaps[] = {srvHeap_};
        commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(drawData_, commandList);
    }

    drawData_ = nullptr;
    drawDataValid_ = false;
}

bool OverlayUI::processEvent(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!initialized_)
    {
        return false;
    }
    bool handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
    return menuVisible_ && handled;
}

void OverlayUI::toggleMenu(Application& app)
{
    if (!initialized_)
    {
        return;
    }
    if (menuVisible_)
    {
        hideMenu(app);
    }
    else
    {
        showMenu(app);
    }
}

void OverlayUI::hideMenu(Application& app)
{
    if (!initialized_)
    {
        return;
    }
    if (!menuVisible_)
    {
        return;
    }
    menuVisible_ = false;
    drawDataValid_ = false;
    ImGui::GetIO().MouseDrawCursor = false;
    app.requestImmediateRender();
}

void OverlayUI::showMenu(Application& app)
{
    if (!initialized_)
    {
        return;
    }
    menuVisible_ = true;
    refreshDeviceLists(app);
    ImGui::GetIO().MouseDrawCursor = true;
    app.requestImmediateRender();
}

void OverlayUI::refreshDeviceLists(Application& app)
{
    videoDevices_ = enumerateVideoCaptureDevices();
    audioDevices_ = enumerateAudioCaptureDevices();

    refreshVideoModes(app);
    refreshVideoFormats(app);
}

void OverlayUI::refreshVideoModes(Application& app)
{
    videoModes_.clear();
    const std::string& moniker = app.settings().videoDeviceMoniker;
    if (moniker.empty())
    {
        return;
    }

    videoModes_ = enumerateVideoModes(moniker);
}

void OverlayUI::refreshVideoFormats(Application& app)
{
    videoFormats_.clear();
    const std::string& moniker = app.settings().videoDeviceMoniker;
    if (moniker.empty())
    {
        return;
    }

    videoFormats_ = enumerateVideoFormats(moniker);
}

void OverlayUI::drawMenuWindow(Application& app)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##overlay_bg", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    const float panelWidth = 460.0f;
    const float panelHeight = 720.0f;
    ImVec2 panelPos((io.DisplaySize.x - panelWidth) * 0.5f, (io.DisplaySize.y - panelHeight) * 0.5f);
    ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CaptureKVM Settings", &menuVisible_, windowFlags))
    {
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (!menuVisible_)
        {
            hideMenu(app);
        }
        return;
    }

    ImGui::TextUnformatted("General");
    ImGui::Separator();

    bool audioPlayback = app.settings().audioPlaybackEnabled;
    if (ImGui::Checkbox("Enable Audio Playback", &audioPlayback))
    {
        app.setAudioPlaybackEnabled(audioPlayback);
    }

    ImGui::Spacing();

    ImGui::TextUnformatted("Video Settings");
    ImGui::Separator();
    bool allowResizing = app.settings().videoAllowResizing;
    if (ImGui::Checkbox("Allow Resizing", &allowResizing))
    {
        app.setVideoAllowResizing(allowResizing);
    }

    bool borderlessWindowed = app.settings().videoBorderlessWindowed;
    if (ImGui::Checkbox("Borderless Windowed", &borderlessWindowed))
    {
        app.setBorderlessWindowed(borderlessWindowed);
    }

    bool fullscreen = app.settings().videoFullscreen;
    if (ImGui::Checkbox("Fullscreen", &fullscreen))
    {
        app.setFullscreen(fullscreen);
    }

    bool vsyncEnabled = app.settings().vsyncEnabled;
    if (ImGui::Checkbox("VSync", &vsyncEnabled))
    {
        app.setVSyncEnabled(vsyncEnabled);
    }

    static const char* aspectOptions[] = {"Stretch", "Force Aspect Ratio", "Force Capture Resolution"};
    int currentAspect = static_cast<int>(app.settings().videoAspectMode);
    if (ImGui::Combo("Aspect Mode", &currentAspect, aspectOptions, IM_ARRAYSIZE(aspectOptions)))
    {
        currentAspect = std::clamp(currentAspect, 0, 2);
        app.setVideoAspectMode(static_cast<VideoAspectMode>(currentAspect));
    }

    ImGui::Spacing();

    if (ImGui::Button("Refresh Devices"))
    {
        refreshDeviceLists(app);
    }

    ImGui::Spacing();

    const float listHeight = 130.0f;

    ImGui::TextUnformatted("Video Capture Devices");
    ImGui::BeginChild("VideoDevices", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders);
    const std::string& currentVideo = app.settings().videoDeviceMoniker;
    if (videoDevices_.empty())
    {
        ImGui::TextDisabled("No video capture devices detected");
    }
    else
    {
        for (const auto& device : videoDevices_)
        {
            std::string label = !device.friendlyName.empty() ? device.friendlyName : device.monikerDisplayName;
            bool selected = (!currentVideo.empty() && currentVideo == device.monikerDisplayName);
            if (ImGui::Selectable(label.c_str(), selected))
            {
                app.selectVideoDevice(device.monikerDisplayName);
                refreshVideoModes(app);
                refreshVideoFormats(app);
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    std::string resolutionLabel;
    if (app.settings().videoPreferredWidth == 0 || app.settings().videoPreferredHeight == 0)
    {
        resolutionLabel = "Auto (driver default)";
    }
    else
    {
        resolutionLabel = std::to_string(app.settings().videoPreferredWidth) + "x" + std::to_string(app.settings().videoPreferredHeight);
    }

    if (ImGui::BeginCombo("Capture Resolution", resolutionLabel.c_str()))
    {
        const bool autoSelected = app.settings().videoPreferredWidth == 0 || app.settings().videoPreferredHeight == 0;
        if (ImGui::Selectable("Auto (driver default)", autoSelected))
        {
            app.setVideoResolution(0, 0);
        }

        for (const auto& mode : videoModes_)
        {
            std::ostringstream oss;
            oss << mode.width << "x" << mode.height;
            if (mode.frameRate > 0.0)
            {
                oss << " @" << std::fixed << std::setprecision(2) << mode.frameRate << " Hz";
            }
            const std::string modeLabel = oss.str();
            const bool selected = app.settings().videoPreferredWidth == mode.width &&
                                  app.settings().videoPreferredHeight == mode.height;
            if (ImGui::Selectable(modeLabel.c_str(), selected))
            {
                app.setVideoResolution(mode.width, mode.height);
            }
        }

        ImGui::EndCombo();
    }

    const VideoFormatPreference currentFormat = app.settings().videoFormatPreference;
    const auto formatLabel = [](VideoFormatPreference value) {
        switch (value)
        {
        case VideoFormatPreference::XRGB:
            return "XRGB";
        case VideoFormatPreference::NV12:
            return "NV12";
        default:
            return "Auto";
        }
    };

    if (ImGui::BeginCombo("Capture Format", formatLabel(currentFormat)))
    {
        const bool autoSelected = currentFormat == VideoFormatPreference::Auto;
        if (ImGui::Selectable("Auto", autoSelected))
        {
            app.setVideoFormatPreference(VideoFormatPreference::Auto);
        }

        bool hasXrgb = false;
        bool hasNv12 = false;
        for (VideoFormatPreference format : videoFormats_)
        {
            hasXrgb = hasXrgb || (format == VideoFormatPreference::XRGB);
            hasNv12 = hasNv12 || (format == VideoFormatPreference::NV12);
        }

        if (hasXrgb)
        {
            const bool selected = currentFormat == VideoFormatPreference::XRGB;
            if (ImGui::Selectable("XRGB", selected))
            {
                app.setVideoFormatPreference(VideoFormatPreference::XRGB);
            }
        }

        if (hasNv12)
        {
            const bool selected = currentFormat == VideoFormatPreference::NV12;
            if (ImGui::Selectable("NV12", selected))
            {
                app.setVideoFormatPreference(VideoFormatPreference::NV12);
            }
        }

        ImGui::EndCombo();
    }

    ImGui::Spacing();

    const std::uint32_t signalWidth = app.currentCaptureWidth();
    const std::uint32_t signalHeight = app.currentCaptureHeight();
    if (signalWidth != 0 && signalHeight != 0)
    {
        ImGui::Text("Current Signal: %ux%u", signalWidth, signalHeight);
    }
    else
    {
        ImGui::TextDisabled("Current Signal: awaiting frames");
    }

    ImGui::Spacing();

    ImGui::TextUnformatted("Audio Capture Devices");
    ImGui::BeginChild("AudioDevices", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders);
    const std::string& currentAudio = app.settings().audioDeviceMoniker;
    bool useVideoAudio = currentAudio == "@video" || currentAudio.empty();
    if (ImGui::Selectable("Use Video Source Audio", useVideoAudio))
    {
        app.selectAudioDevice("@video");
    }
    if (audioDevices_.empty())
    {
        ImGui::TextDisabled("No dedicated audio capture devices detected");
    }
    else
    {
        for (const auto& device : audioDevices_)
        {
            std::string label = !device.friendlyName.empty() ? device.friendlyName : device.monikerDisplayName;
            bool selected = (!currentAudio.empty() && currentAudio == device.monikerDisplayName);
            if (ImGui::Selectable(label.c_str(), selected))
            {
                app.selectAudioDevice(device.monikerDisplayName);
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    if (ImGui::IsKeyReleased(ImGuiKey_Escape))
    {
        hideMenu(app);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    if (!menuVisible_)
    {
        hideMenu(app);
    }
}
