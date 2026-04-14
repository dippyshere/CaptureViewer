#include "AudioPlayback.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <utility>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    void logAudio(const std::string& message)
    {
        std::ofstream("viewer.log", std::ios::app) << message << '\n';
    }

    std::wstring toLowerCopy(std::wstring text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return text;
    }

    std::wstring extractVidPidToken(const std::wstring& moniker)
    {
        const std::wstring lower = toLowerCopy(moniker);
        const std::size_t vidPos = lower.find(L"vid_");
        if (vidPos == std::wstring::npos || vidPos + 8 > lower.size())
        {
            return {};
        }

        const std::size_t pidPos = lower.find(L"&pid_", vidPos + 4);
        if (pidPos == std::wstring::npos || pidPos + 9 > lower.size())
        {
            return {};
        }

        std::size_t end = pidPos + 9;
        while (end < lower.size())
        {
            const wchar_t ch = lower[end];
            if ((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'))
            {
                ++end;
                continue;
            }
            break;
        }

        return lower.substr(vidPos, end - vidPos);
    }
}

AudioPlayback::AudioPlayback() = default;
AudioPlayback::~AudioPlayback()
{
    stop();
}

std::wstring AudioPlayback::widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), required);
    return result;
}

std::string AudioPlayback::narrow(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
    return result;
}

void AudioPlayback::start(const std::string& deviceMoniker, const std::vector<std::string>& outputDeviceMonikers, bool useDefaultOutputOnly)
{
    stop();

    std::lock_guard<std::mutex> lock(mutex_);

    requestedMoniker_ = widen(deviceMoniker);
    requestedOutputMonikers_.clear();
    requestedOutputMonikers_.reserve(outputDeviceMonikers.size());
    for (const std::string& moniker : outputDeviceMonikers)
    {
        const std::wstring wideMoniker = widen(moniker);
        if (!wideMoniker.empty())
        {
            requestedOutputMonikers_.push_back(wideMoniker);
        }
    }
    useDefaultOutputOnly_ = useDefaultOutputOnly || requestedOutputMonikers_.empty();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE)
    {
        coInitialized_ = true;
    }
    else if (hr == RPC_E_CHANGED_MODE)
    {
        coInitialized_ = false;
        logAudio("[Audio] CoInitializeEx: COM already initialised with different model; continuing");
    }
    else
    {
        coInitialized_ = false;
        logAudio("[Audio] CoInitializeEx failed");
    }

    if (!selectDevice(requestedMoniker_))
    {
        releaseGraph();
        if (coInitialized_)
        {
            CoUninitialize();
            coInitialized_ = false;
        }
        return;
    }

    if (!buildGraph())
    {
        releaseGraph();
        if (coInitialized_)
        {
            CoUninitialize();
            coInitialized_ = false;
        }
        return;
    }

    if (control_ && SUCCEEDED(control_->Run()))
    {
        running_ = true;
        logAudio("[Audio] Audio playback started for '" + narrow(selectedFriendlyName_.empty() ? selectedDisplayName_ : selectedFriendlyName_) + "'");
    }
    else
    {
        logAudio("[Audio] Failed to start audio graph");
        releaseGraph();
        if (coInitialized_)
        {
            CoUninitialize();
            coInitialized_ = false;
        }
    }
}

void AudioPlayback::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_ && control_)
    {
        control_->Stop();
    }
    running_ = false;

    releaseGraph();

    if (coInitialized_)
    {
        CoUninitialize();
        coInitialized_ = false;
    }
}

std::string AudioPlayback::currentDeviceFriendlyName() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::wstring& name = !selectedFriendlyName_.empty() ? selectedFriendlyName_ : selectedDisplayName_;
    return narrow(name);
}

bool AudioPlayback::selectDevice(const std::wstring& requestedMoniker)
{
    ComPtr<ICreateDevEnum> devEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
    if (FAILED(hr))
    {
        logAudio("[Audio] Failed to create device enumerator");
        return false;
    }

    struct SelectedDevice
    {
        ComPtr<IMoniker> moniker;
        std::wstring friendly;
        std::wstring display;
        bool exactMatch = false;
        bool hasAny = false;
    };

    auto enumerateCategory = [&](REFCLSID category) -> SelectedDevice
    {
        SelectedDevice result;

        ComPtr<IEnumMoniker> enumMoniker;
        hr = devEnum->CreateClassEnumerator(category, enumMoniker.GetAddressOf(), 0);
        if (hr != S_OK || !enumMoniker)
        {
            return result;
        }

        ComPtr<IMoniker> fallback;
        std::wstring fallbackFriendly;
        std::wstring fallbackDisplay;

        ComPtr<IMoniker> current;
        ULONG fetched = 0;
        while (enumMoniker->Next(1, current.GetAddressOf(), &fetched) == S_OK)
        {
            std::wstring displayName;
            {
                LPOLESTR name = nullptr;
                if (SUCCEEDED(current->GetDisplayName(nullptr, nullptr, &name)) && name)
                {
                    displayName.assign(name);
                    CoTaskMemFree(name);
                }
            }

            std::wstring friendly;
            ComPtr<IPropertyBag> bag;
            if (SUCCEEDED(current->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))) && bag)
            {
                VARIANT value;
                VariantInit(&value);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) && value.vt == VT_BSTR)
                {
                    friendly.assign(value.bstrVal, SysStringLen(value.bstrVal));
                }
                VariantClear(&value);
            }

            if (!requestedMoniker.empty() &&
                ((!displayName.empty() && displayName == requestedMoniker) || (!friendly.empty() && friendly == requestedMoniker)))
            {
                result.moniker = current;
                result.friendly = friendly;
                result.display = displayName;
                result.exactMatch = true;
                result.hasAny = true;
                return result;
            }

            if (!fallback)
            {
                fallback = current;
                fallbackFriendly = friendly;
                fallbackDisplay = displayName;
            }

            current.Reset();
        }

        if (fallback)
        {
            result.moniker = fallback;
            result.friendly = fallbackFriendly;
            result.display = fallbackDisplay;
            result.hasAny = true;
        }

        return result;
    };

    auto findAudioByVidPid = [&](const std::wstring& vidPidToken) -> SelectedDevice
    {
        SelectedDevice result;
        if (vidPidToken.empty())
        {
            return result;
        }

        ComPtr<IEnumMoniker> enumMoniker;
        hr = devEnum->CreateClassEnumerator(CLSID_AudioInputDeviceCategory, enumMoniker.GetAddressOf(), 0);
        if (hr != S_OK || !enumMoniker)
        {
            return result;
        }

        ComPtr<IMoniker> current;
        ULONG fetched = 0;
        while (enumMoniker->Next(1, current.GetAddressOf(), &fetched) == S_OK)
        {
            std::wstring displayName;
            {
                LPOLESTR name = nullptr;
                if (SUCCEEDED(current->GetDisplayName(nullptr, nullptr, &name)) && name)
                {
                    displayName.assign(name);
                    CoTaskMemFree(name);
                }
            }

            std::wstring friendly;
            ComPtr<IPropertyBag> bag;
            if (SUCCEEDED(current->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))) && bag)
            {
                VARIANT value;
                VariantInit(&value);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) && value.vt == VT_BSTR)
                {
                    friendly.assign(value.bstrVal, SysStringLen(value.bstrVal));
                }
                VariantClear(&value);
            }

            const std::wstring displayLower = toLowerCopy(displayName);
            const std::wstring friendlyLower = toLowerCopy(friendly);
            if ((!displayLower.empty() && displayLower.find(vidPidToken) != std::wstring::npos) ||
                (!friendlyLower.empty() && friendlyLower.find(vidPidToken) != std::wstring::npos))
            {
                result.moniker = current;
                result.friendly = friendly;
                result.display = displayName;
                result.exactMatch = true;
                result.hasAny = true;
                return result;
            }

            current.Reset();
        }

        return result;
    };

    SelectedDevice audioCandidate = enumerateCategory(CLSID_AudioInputDeviceCategory);
    if (audioCandidate.exactMatch)
    {
        selectedMoniker_ = audioCandidate.moniker;
        selectedFriendlyName_ = audioCandidate.friendly;
        selectedDisplayName_ = audioCandidate.display;
        if (selectedFriendlyName_.empty())
        {
            selectedFriendlyName_ = selectedDisplayName_;
        }
        return true;
    }

    if (!requestedMoniker.empty())
    {
        SelectedDevice videoCandidate = enumerateCategory(CLSID_VideoInputDeviceCategory);
        if (videoCandidate.exactMatch)
        {
            const std::wstring vidPid = extractVidPidToken(videoCandidate.display);
            SelectedDevice pairedAudio = findAudioByVidPid(vidPid);
            if (pairedAudio.moniker)
            {
                audioCandidate = std::move(pairedAudio);
                logAudio("[Audio] Selected paired audio-input device for requested video source");
            }
            else
            {
                audioCandidate = std::move(videoCandidate);
            }
        }
        else
        {
            logAudio("[Audio] Requested audio device not found by moniker/friendly name");
            return false;
        }
    }

    if (requestedMoniker.empty() && !audioCandidate.hasAny)
    {
        SelectedDevice videoFallback = enumerateCategory(CLSID_VideoInputDeviceCategory);
        if (videoFallback.hasAny)
        {
            audioCandidate = std::move(videoFallback);
        }
    }

    if (!audioCandidate.moniker)
    {
        logAudio("[Audio] Unable to select an audio capture device");
        return false;
    }

    selectedMoniker_ = audioCandidate.moniker;
    selectedFriendlyName_ = audioCandidate.friendly;
    selectedDisplayName_ = audioCandidate.display;
    if (selectedFriendlyName_.empty())
    {
        selectedFriendlyName_ = selectedDisplayName_;
    }

    return true;
}

bool AudioPlayback::buildGraph()
{
    const ComPtr<IMoniker> selectedMoniker = selectedMoniker_;
    const std::wstring selectedFriendlyName = selectedFriendlyName_;
    const std::wstring selectedDisplayName = selectedDisplayName_;
    const std::vector<std::wstring> requestedOutputMonikers = requestedOutputMonikers_;
    const bool useDefaultOutputOnly = useDefaultOutputOnly_;

    releaseGraph();

    selectedMoniker_ = selectedMoniker;
    selectedFriendlyName_ = selectedFriendlyName;
    selectedDisplayName_ = selectedDisplayName;

    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph_));
    if (FAILED(hr))
    {
        logAudio("[Audio] Failed to create FilterGraph");
        return false;
    }

    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder_));
    if (FAILED(hr))
    {
        logAudio("[Audio] Failed to create CaptureGraphBuilder2");
        return false;
    }

    hr = builder_->SetFiltergraph(graph_.Get());
    if (FAILED(hr))
    {
        logAudio("[Audio] Failed to set filter graph on builder");
        return false;
    }

    if (!selectedMoniker_)
    {
        logAudio("[Audio] No selected moniker to build graph");
        return false;
    }

    ComPtr<IBaseFilter> filter;
    hr = selectedMoniker_->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter));
    if (FAILED(hr))
    {
        logAudio("[Audio] Failed to bind capture filter");
        return false;
    }

    const std::wstring filterName = !selectedFriendlyName_.empty() ? selectedFriendlyName_ : std::wstring(L"Audio Capture");
    hr = graph_->AddFilter(filter.Get(), filterName.c_str());
    if (FAILED(hr))
    {
        logAudio("[Audio] Failed to add capture filter to graph");
        return false;
    }

    sourceFilter_ = filter;

    std::vector<ComPtr<IBaseFilter>> outputRenderers;
    outputRenderers.reserve(std::max<std::size_t>(1, requestedOutputMonikers.size()));

    auto addDefaultRenderer = [&]() -> bool {
        ComPtr<IBaseFilter> dsoundRenderer;
        HRESULT localHr = CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dsoundRenderer));
        if (FAILED(localHr) || !dsoundRenderer)
        {
            return false;
        }

        localHr = graph_->AddFilter(dsoundRenderer.Get(), L"DirectSound Audio Renderer");
        if (FAILED(localHr))
        {
            return false;
        }

        outputRenderers.push_back(std::move(dsoundRenderer));
        logAudio("[Audio] Using default DirectSound renderer for low-latency monitoring");
        return true;
    };

    auto addRendererFromMoniker = [&](const std::wstring& monikerText) -> bool {
        if (monikerText.empty())
        {
            return false;
        }

        ComPtr<ICreateDevEnum> devEnum;
        if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum))) || !devEnum)
        {
            return false;
        }

        ComPtr<IEnumMoniker> enumMoniker;
        if (devEnum->CreateClassEnumerator(CLSID_AudioRendererCategory, enumMoniker.GetAddressOf(), 0) != S_OK || !enumMoniker)
        {
            return false;
        }

        ComPtr<IMoniker> matchedMoniker;
        std::wstring rendererName = L"Audio Renderer";

        ComPtr<IMoniker> current;
        ULONG fetched = 0;
        while (enumMoniker->Next(1, current.GetAddressOf(), &fetched) == S_OK)
        {
            std::wstring displayName;
            LPOLESTR display = nullptr;
            if (SUCCEEDED(current->GetDisplayName(nullptr, nullptr, &display)) && display)
            {
                displayName.assign(display);
                CoTaskMemFree(display);
            }

            if (displayName != monikerText)
            {
                current.Reset();
                continue;
            }

            matchedMoniker = current;

            ComPtr<IPropertyBag> bag;
            if (SUCCEEDED(current->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))) && bag)
            {
                VARIANT value;
                VariantInit(&value);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) && value.vt == VT_BSTR)
                {
                    rendererName.assign(value.bstrVal, SysStringLen(value.bstrVal));
                }
                VariantClear(&value);
            }

            break;
        }

        if (!matchedMoniker)
        {
            return false;
        }

        ComPtr<IBaseFilter> renderer;
        if (FAILED(matchedMoniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&renderer))) || !renderer)
        {
            return false;
        }

        if (FAILED(graph_->AddFilter(renderer.Get(), rendererName.c_str())))
        {
            return false;
        }

        outputRenderers.push_back(std::move(renderer));
        logAudio("[Audio] Added output renderer '" + narrow(rendererName) + "'");
        return true;
    };

    if (useDefaultOutputOnly)
    {
        addDefaultRenderer();
    }
    else
    {
        for (const std::wstring& moniker : requestedOutputMonikers)
        {
            if (!addRendererFromMoniker(moniker))
            {
                logAudio("[Audio] Failed to resolve selected output renderer moniker");
            }
        }
    }

    if (outputRenderers.empty())
    {
        if (!addDefaultRenderer())
        {
            logAudio("[Audio] Failed to add any audio renderer");
            return false;
        }
    }

    hr = E_FAIL;
    if (outputRenderers.size() == 1)
    {
        hr = builder_->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Audio, sourceFilter_.Get(), nullptr, outputRenderers.front().Get());
        if (FAILED(hr))
        {
            hr = builder_->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, sourceFilter_.Get(), nullptr, outputRenderers.front().Get());
        }
    }
    else
    {
        ComPtr<IBaseFilter> tee;
        hr = CoCreateInstance(CLSID_InfTee, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tee));
        if (SUCCEEDED(hr) && tee)
        {
            hr = graph_->AddFilter(tee.Get(), L"Audio Output Tee");
        }

        if (SUCCEEDED(hr))
        {
            hr = builder_->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Audio, sourceFilter_.Get(), nullptr, tee.Get());
            if (FAILED(hr))
            {
                hr = builder_->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, sourceFilter_.Get(), nullptr, tee.Get());
            }
        }

        if (SUCCEEDED(hr))
        {
            for (const ComPtr<IBaseFilter>& renderer : outputRenderers)
            {
                HRESULT renderHr = builder_->RenderStream(nullptr, &MEDIATYPE_Audio, tee.Get(), nullptr, renderer.Get());
                if (FAILED(renderHr))
                {
                    hr = renderHr;
                    break;
                }
            }
        }
    }

    if (FAILED(hr))
    {
        logAudio("[Audio] Failed to render audio stream");
        return false;
    }

    ComPtr<IMediaFilter> mediaFilter;
    if (SUCCEEDED(graph_->QueryInterface(IID_PPV_ARGS(&mediaFilter))) && mediaFilter)
    {
        mediaFilter->SetSyncSource(nullptr);
    }

    hr = graph_->QueryInterface(IID_PPV_ARGS(&control_));
    if (FAILED(hr))
    {
        logAudio("[Audio] Failed to acquire IMediaControl");
        return false;
    }

    return true;
}

void AudioPlayback::releaseGraph()
{
    if (control_)
    {
        control_->Stop();
        control_.Reset();
    }
    if (sourceFilter_)
    {
        sourceFilter_.Reset();
    }
    if (builder_)
    {
        builder_.Reset();
    }
    if (graph_)
    {
        graph_.Reset();
    }
    selectedMoniker_.Reset();
    selectedFriendlyName_.clear();
    selectedDisplayName_.clear();
}
