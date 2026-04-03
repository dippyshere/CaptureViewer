#include "DeviceEnumeration.hpp"

#include <Windows.h>
#include <SetupAPI.h>
#include <devguid.h>
#include <dshow.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <winreg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "Setupapi.lib")

namespace
{
    using Microsoft::WRL::ComPtr;

    void logFormatEnum(const std::string& message)
    {
        std::ofstream("pckvm.log", std::ios::app) << "[FormatEnum] " << message << '\n';
    }

    std::string guidToString(const GUID& guid)
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0')
            << std::setw(8) << static_cast<unsigned long>(guid.Data1) << "-"
            << std::setw(4) << static_cast<unsigned int>(guid.Data2) << "-"
            << std::setw(4) << static_cast<unsigned int>(guid.Data3) << "-"
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[0])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[1]) << "-"
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[2])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[3])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[4])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[5])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[6])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[7]);
        return oss.str();
    }

    const char* mediaSubtypeName(const GUID& subtype)
    {
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB24)) return "RGB24";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB32)) return "RGB32";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_ARGB32)) return "ARGB32";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_NV12)) return "NV12";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_YUY2)) return "YUY2";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_P010)) return "P010";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_MJPG)) return "MJPG";
        return "UNKNOWN";
    }

    bool isXrgbCompatibleSubtype(const GUID& subtype)
    {
        return InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB24) ||
               InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB32) ||
               InlineIsEqualGUID(subtype, MEDIASUBTYPE_ARGB32);
    }

    class ScopedCoInit
    {
    public:
        explicit ScopedCoInit(DWORD flags)
        {
            const HRESULT hr = CoInitializeEx(nullptr, flags);
            if (SUCCEEDED(hr))
            {
                shouldUninit_ = true;
            }
            else if (hr == RPC_E_CHANGED_MODE)
            {
                shouldUninit_ = false;
            }
        }

        ~ScopedCoInit()
        {
            if (shouldUninit_)
            {
                CoUninitialize();
            }
        }

    private:
        bool shouldUninit_ = false;
    };

    std::string wideToUtf8(const std::wstring& input)
    {
        if (input.empty())
        {
            return {};
        }
        const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        if (sizeNeeded <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(sizeNeeded), '\0');
        WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), result.data(), sizeNeeded, nullptr, nullptr);
        return result;
    }

    std::string bstrToUtf8(BSTR value)
    {
        if (!value)
        {
            return {};
        }
        return wideToUtf8(std::wstring(value, SysStringLen(value)));
    }

    std::wstring utf8ToWide(const std::string& input)
    {
        if (input.empty())
        {
            return {};
        }
        const int required = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0);
        if (required <= 0)
        {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), result.data(), required);
        return result;
    }

    void freeMediaType(AM_MEDIA_TYPE& mt)
    {
        if (mt.cbFormat != 0 && mt.pbFormat)
        {
            CoTaskMemFree(mt.pbFormat);
            mt.cbFormat = 0;
            mt.pbFormat = nullptr;
        }
        if (mt.pUnk)
        {
            mt.pUnk->Release();
            mt.pUnk = nullptr;
        }
    }

    template <typename DeviceInfo>
    std::vector<DeviceInfo> enumerateCategory(REFCLSID category)
    {
        ScopedCoInit coInit(COINIT_APARTMENTTHREADED);

        std::vector<DeviceInfo> devices;

        ComPtr<ICreateDevEnum> devEnum;
        if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum))))
        {
            return devices;
        }

        ComPtr<IEnumMoniker> enumMoniker;
        if (devEnum->CreateClassEnumerator(category, enumMoniker.GetAddressOf(), 0) != S_OK || !enumMoniker)
        {
            return devices;
        }

        ComPtr<IMoniker> moniker;
        ULONG fetched = 0;
        while (enumMoniker->Next(1, moniker.GetAddressOf(), &fetched) == S_OK)
        {
            DeviceInfo info;

            LPOLESTR displayName = nullptr;
            if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &displayName)) && displayName)
            {
                info.monikerDisplayName = wideToUtf8(displayName);
                CoTaskMemFree(displayName);
            }

            ComPtr<IPropertyBag> props;
            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&props))) && props)
            {
                VARIANT friendly;
                VariantInit(&friendly);
                if (SUCCEEDED(props->Read(L"FriendlyName", &friendly, nullptr)) && friendly.vt == VT_BSTR)
                {
                    info.friendlyName = bstrToUtf8(friendly.bstrVal);
                }
                VariantClear(&friendly);
            }

            devices.push_back(std::move(info));
            moniker.Reset();
        }

        return devices;
    }
}

std::vector<VideoDeviceInfo> enumerateVideoCaptureDevices()
{
    return enumerateCategory<VideoDeviceInfo>(CLSID_VideoInputDeviceCategory);
}

std::vector<AudioCaptureDeviceInfo> enumerateAudioCaptureDevices()
{
    return enumerateCategory<AudioCaptureDeviceInfo>(CLSID_AudioInputDeviceCategory);
}

std::vector<VideoModeInfo> enumerateVideoModes(const std::string& monikerDisplayName)
{
    std::vector<VideoModeInfo> modes;
    if (monikerDisplayName.empty())
    {
        return modes;
    }

    ScopedCoInit coInit(COINIT_MULTITHREADED);

    ComPtr<IGraphBuilder> graph;
    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))))
    {
        return modes;
    }

    ComPtr<ICaptureGraphBuilder2> builder;
    if (FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder))))
    {
        return modes;
    }

    if (FAILED(builder->SetFiltergraph(graph.Get())))
    {
        return modes;
    }

    const std::wstring monikerWide = utf8ToWide(monikerDisplayName);
    if (monikerWide.empty())
    {
        return modes;
    }

    ComPtr<IBindCtx> bindCtx;
    if (FAILED(CreateBindCtx(0, &bindCtx)))
    {
        return modes;
    }

    ULONG eaten = 0;
    ComPtr<IMoniker> moniker;
    if (FAILED(MkParseDisplayName(bindCtx.Get(), monikerWide.c_str(), &eaten, moniker.GetAddressOf())) || !moniker)
    {
        return modes;
    }

    ComPtr<IBaseFilter> captureFilter;
    if (FAILED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&captureFilter))) || !captureFilter)
    {
        return modes;
    }

    if (FAILED(graph->AddFilter(captureFilter.Get(), L"Source")))
    {
        return modes;
    }

    ComPtr<IAMStreamConfig> streamConfig;
    HRESULT hr = builder->FindInterface(&PIN_CATEGORY_CAPTURE,
                                        &MEDIATYPE_Video,
                                        captureFilter.Get(),
                                        IID_PPV_ARGS(streamConfig.GetAddressOf()));
    if (FAILED(hr) || !streamConfig)
    {
        hr = builder->FindInterface(&PIN_CATEGORY_PREVIEW,
                                     &MEDIATYPE_Video,
                                     captureFilter.Get(),
                                     IID_PPV_ARGS(streamConfig.GetAddressOf()));
    }

    if (FAILED(hr) || !streamConfig)
    {
        return modes;
    }

    int capabilityCount = 0;
    int capabilitySize = 0;
    if (FAILED(streamConfig->GetNumberOfCapabilities(&capabilityCount, &capabilitySize)) || capabilityCount <= 0 || capabilitySize <= 0)
    {
        return modes;
    }

    std::vector<std::uint8_t> capabilityBuffer(static_cast<std::size_t>(capabilitySize));
    std::map<std::pair<std::uint32_t, std::uint32_t>, double> uniqueModes;

    for (int i = 0; i < capabilityCount; ++i)
    {
        AM_MEDIA_TYPE* mediaType = nullptr;
        if (FAILED(streamConfig->GetStreamCaps(i, &mediaType, capabilityBuffer.data())) || !mediaType)
        {
            continue;
        }

        if (mediaType->formattype == FORMAT_VideoInfo && mediaType->cbFormat >= sizeof(VIDEOINFOHEADER) && mediaType->pbFormat)
        {
            const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mediaType->pbFormat);
            const std::uint32_t width = static_cast<std::uint32_t>(std::abs(vih->bmiHeader.biWidth));
            const std::uint32_t height = static_cast<std::uint32_t>(std::abs(vih->bmiHeader.biHeight));
            double frameRate = 0.0;
            if (vih->AvgTimePerFrame > 0)
            {
                frameRate = 10'000'000.0 / static_cast<double>(vih->AvgTimePerFrame);
            }

            auto key = std::make_pair(width, height);
            auto existing = uniqueModes.find(key);
            if (existing == uniqueModes.end() || frameRate > existing->second)
            {
                uniqueModes[key] = frameRate;
            }
        }

        freeMediaType(*mediaType);
        CoTaskMemFree(mediaType);
    }

    modes.reserve(uniqueModes.size());
    for (const auto& entry : uniqueModes)
    {
        VideoModeInfo mode;
        mode.width = entry.first.first;
        mode.height = entry.first.second;
        mode.frameRate = entry.second;
        modes.push_back(mode);
    }

    std::sort(modes.begin(), modes.end(), [](const VideoModeInfo& a, const VideoModeInfo& b) {
        if (a.width != b.width)
        {
            return a.width > b.width;
        }
        if (a.height != b.height)
        {
            return a.height > b.height;
        }
        return a.frameRate > b.frameRate;
    });

    return modes;
}

std::vector<VideoFormatPreference> enumerateVideoFormats(const std::string& monikerDisplayName)
{
    std::vector<VideoFormatPreference> formats;
    if (monikerDisplayName.empty())
    {
        return formats;
    }

    logFormatEnum("Enumerating formats for device moniker: " + monikerDisplayName);

    ScopedCoInit coInit(COINIT_MULTITHREADED);

    ComPtr<IGraphBuilder> graph;
    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))))
    {
        return formats;
    }

    ComPtr<ICaptureGraphBuilder2> builder;
    if (FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder))))
    {
        return formats;
    }

    if (FAILED(builder->SetFiltergraph(graph.Get())))
    {
        return formats;
    }

    const std::wstring monikerWide = utf8ToWide(monikerDisplayName);
    if (monikerWide.empty())
    {
        return formats;
    }

    ComPtr<IBindCtx> bindCtx;
    if (FAILED(CreateBindCtx(0, &bindCtx)))
    {
        return formats;
    }

    ULONG eaten = 0;
    ComPtr<IMoniker> moniker;
    if (FAILED(MkParseDisplayName(bindCtx.Get(), monikerWide.c_str(), &eaten, moniker.GetAddressOf())) || !moniker)
    {
        return formats;
    }

    ComPtr<IBaseFilter> captureFilter;
    if (FAILED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&captureFilter))) || !captureFilter)
    {
        return formats;
    }

    if (FAILED(graph->AddFilter(captureFilter.Get(), L"Source")))
    {
        return formats;
    }

    ComPtr<IAMStreamConfig> streamConfig;
    HRESULT hr = builder->FindInterface(&PIN_CATEGORY_CAPTURE,
                                        &MEDIATYPE_Video,
                                        captureFilter.Get(),
                                        IID_PPV_ARGS(streamConfig.GetAddressOf()));
    if (FAILED(hr) || !streamConfig)
    {
        hr = builder->FindInterface(&PIN_CATEGORY_PREVIEW,
                                    &MEDIATYPE_Video,
                                    captureFilter.Get(),
                                    IID_PPV_ARGS(streamConfig.GetAddressOf()));
    }

    if (FAILED(hr) || !streamConfig)
    {
        return formats;
    }

    int capabilityCount = 0;
    int capabilitySize = 0;
    if (FAILED(streamConfig->GetNumberOfCapabilities(&capabilityCount, &capabilitySize)) || capabilityCount <= 0 || capabilitySize <= 0)
    {
        return formats;
    }

    bool hasXrgb = false;
    bool hasNv12 = false;
    std::vector<std::uint8_t> capabilityBuffer(static_cast<std::size_t>(capabilitySize));

    for (int i = 0; i < capabilityCount; ++i)
    {
        AM_MEDIA_TYPE* mediaType = nullptr;
        if (FAILED(streamConfig->GetStreamCaps(i, &mediaType, capabilityBuffer.data())) || !mediaType)
        {
            logFormatEnum("cap[" + std::to_string(i) + "]: GetStreamCaps failed");
            continue;
        }

        if (isXrgbCompatibleSubtype(mediaType->subtype))
        {
            hasXrgb = true;
        }
        else if (InlineIsEqualGUID(mediaType->subtype, MEDIASUBTYPE_NV12))
        {
            hasNv12 = true;
        }

        freeMediaType(*mediaType);
        CoTaskMemFree(mediaType);
    }

    if (hasXrgb)
    {
        formats.push_back(VideoFormatPreference::XRGB);
    }
    if (hasNv12)
    {
        formats.push_back(VideoFormatPreference::NV12);
    }

    if (formats.empty())
    {
        formats.push_back(VideoFormatPreference::Auto);
    }

    return formats;
}
