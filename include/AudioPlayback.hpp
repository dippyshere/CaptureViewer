#pragma once

#include <Windows.h>
#include <dshow.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <vector>

class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    void start(const std::string& deviceMoniker, const std::vector<std::string>& outputDeviceMonikers, bool useDefaultOutputOnly);
    void stop();

    [[nodiscard]] bool isRunning() const noexcept { return running_; }
    [[nodiscard]] std::string currentDeviceFriendlyName() const;

private:
    bool selectDevice(const std::wstring& requestedMoniker);
    bool buildGraph();
    void releaseGraph();

    static std::wstring widen(const std::string& text);
    static std::string narrow(const std::wstring& text);

    bool running_ = false;
    bool coInitialized_ = false;
    std::wstring requestedMoniker_;
    std::vector<std::wstring> requestedOutputMonikers_;
    bool useDefaultOutputOnly_ = true;
    std::wstring selectedFriendlyName_;
    std::wstring selectedDisplayName_;

    Microsoft::WRL::ComPtr<IGraphBuilder> graph_;
    Microsoft::WRL::ComPtr<ICaptureGraphBuilder2> builder_;
    Microsoft::WRL::ComPtr<IMediaControl> control_;
    Microsoft::WRL::ComPtr<IBaseFilter> sourceFilter_;
    Microsoft::WRL::ComPtr<IMoniker> selectedMoniker_;
    mutable std::mutex mutex_;
};
