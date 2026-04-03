#include "DirectShowCapture.hpp"

#include <Windows.h>
#include <OleAuto.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <uuids.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr wchar_t kPreferredDeviceName[] = L"AVerMedia HD Capture GC573 1";

    const GUID kCLSID_SampleGrabber = {0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
    const GUID kIID_ISampleGrabber = {0x6B652FFF, 0x11FE, 0x4FCE, {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
    const GUID kIID_ISampleGrabberCB = {0x0579154A, 0x2B53, 0x4994, {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};
    const GUID kCLSID_NullRenderer = {0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
    const GUID kPreferredVideoSubtypeXrgb = MEDIASUBTYPE_RGB32;

    using Microsoft::WRL::ComPtr;

    void logMessage(const std::string& text)
    {
        std::ofstream("pckvm.log", std::ios::app) << text << '\n';
    }

    std::string formatHr(HRESULT hr)
    {
        char buffer[512] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr,
                       hr,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       buffer,
                       static_cast<DWORD>(std::size(buffer)),
                       nullptr);
        return buffer;
    }

    void throwIfFailed(HRESULT hr, const char* context)
    {
        if (FAILED(hr))
        {
            std::ostringstream oss;
            oss << context << " (HRESULT 0x" << std::hex << std::uppercase
                << static_cast<unsigned long>(hr) << "): " << formatHr(hr);
            logMessage(std::string("[Capture] ") + oss.str());
            throw std::runtime_error(oss.str());
        }
    }

    std::string narrow(const std::wstring& wstr)
    {
        if (wstr.empty())
        {
            return {};
        }
        const int length = ::WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(length - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, result.data(), length, nullptr, nullptr);
        return result;
    }

    std::wstring widen(const std::string& str)
    {
        if (str.empty())
        {
            return {};
        }
        const int required = ::MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
        if (required <= 0)
        {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), required);
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

    bool isSubtype(const GUID& value, const GUID& expected)
    {
        return InlineIsEqualGUID(value, expected) != FALSE;
    }
}

struct ISampleGrabberCB : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long bufferLen) = 0;
};

struct ISampleGrabber : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};

struct DirectShowCaptureImpl;

class SampleGrabberCallback : public ISampleGrabberCB
{
public:
    explicit SampleGrabberCallback(DirectShowCaptureImpl* owner) noexcept
        : owner_(owner)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == kIID_ISampleGrabberCB)
        {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG value = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (value == 0)
        {
            delete this;
        }
        return value;
    }

    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long bufferLen) override;

    void resetOwner()
    {
        owner_.store(nullptr, std::memory_order_release);
    }

private:
    std::atomic<ULONG> refCount_{1};
    std::atomic<DirectShowCaptureImpl*> owner_;
};

struct DirectShowCaptureImpl
{
    DirectShowCapture::FrameHandler handler;
    std::thread worker;
    std::atomic<bool> running{false};

    std::mutex initMutex;
    std::condition_variable initCv;
    bool initCompleted = false;
    std::exception_ptr initError;

    std::mutex errorMutex;
    std::string lastError;

    std::atomic<bool> frameReceived{false};

    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> captureBuilder;
    ComPtr<IMediaControl> control;
    ComPtr<IBaseFilter> captureFilter;
    ComPtr<IBaseFilter> sampleGrabberFilter;
    ComPtr<ISampleGrabber> sampleGrabber;
    ComPtr<IBaseFilter> nullRenderer;
    ComPtr<IMoniker> selectedMoniker;
    SampleGrabberCallback* callback = nullptr;

    std::uint32_t frameWidth = 0;
    std::uint32_t frameHeight = 0;
    std::uint32_t frameStride = 0;
    std::uint32_t contentLeft = 0;
    std::uint32_t contentTop = 0;
    std::uint32_t contentRight = 0;
    std::uint32_t contentBottom = 0;
    bool bottomUp = false;
    std::atomic<bool> loggedSampleSize{false};

    std::wstring requestedMoniker;
    std::wstring selectedFriendlyName;
    std::wstring selectedMonikerDisplayName;
    bool audioEnabled = false;
    std::uint32_t requestedWidth = 0;
    std::uint32_t requestedHeight = 0;

    DirectShowCaptureImpl() = default;

    ~DirectShowCaptureImpl()
    {
        stop();
    }

    void start(DirectShowCapture::FrameHandler cb, const DirectShowCapture::Options& options)
    {
        if (!cb)
        {
            throw std::invalid_argument("Frame handler must not be empty");
        }

        handler = std::move(cb);
        requestedMoniker = widen(options.deviceMoniker);
        selectedFriendlyName.clear();
        selectedMonikerDisplayName.clear();
        audioEnabled = options.enableAudio;
        requestedWidth = options.desiredWidth;
        requestedHeight = options.desiredHeight;
        if (running.exchange(true))
        {
            throw std::runtime_error("Capture already running");
        }

        {
            std::lock_guard<std::mutex> lock(initMutex);
            initCompleted = false;
            initError = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastError.clear();
        }
        frameReceived.store(false, std::memory_order_release);

        worker = std::thread([this]() {
            runCaptureThread();
        });

        std::unique_lock<std::mutex> lock(initMutex);
        initCv.wait(lock, [this]() { return initCompleted; });
        if (initError)
        {
            auto err = initError;
            initError = nullptr;
            lock.unlock();
            running.store(false, std::memory_order_release);
            if (worker.joinable())
            {
                worker.join();
            }
            std::rethrow_exception(err);
        }

        logMessage("[Capture] Initialization completed successfully");
    }

    void stop()
    {
        if (!running.exchange(false))
        {
            return;
        }

        if (worker.joinable())
        {
            worker.join();
        }

        releaseGraph();
    }

    std::string currentFriendlyName() const
    {
        const std::wstring& source = !selectedFriendlyName.empty() ? selectedFriendlyName : selectedMonikerDisplayName;
        return narrow(source);
    }

    void runCaptureThread()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        auto finalizeInit = [this](std::exception_ptr err = nullptr) {
            std::lock_guard<std::mutex> lock(initMutex);
            initError = err;
            initCompleted = true;
            initCv.notify_all();
        };

        try
        {
            selectCaptureDevice();
            buildGraph();
            finalizeInit();
            logMessage("[Capture] Graph constructed");

            if (control)
            {
                throwIfFailed(control->Run(), "Failed to start graph");
                logMessage("[Capture] Graph running");
            }

            while (running.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            if (control)
            {
                control->StopWhenReady();
                logMessage("[Capture] Graph stop requested");
            }

            if (!frameReceived.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (lastError.empty())
                {
                    const std::string deviceLabel = selectedFriendlyName.empty()
                        ? "the selected capture device"
                        : narrow(selectedFriendlyName);
                    lastError = "No video frames received from '" + deviceLabel + "'. Verify the signal and that no other application is using the device.";
                }
                logMessage("[Capture] No frames were received from the device");
            }
        }
        catch (...)
        {
            auto err = std::current_exception();
            finalizeInit(err);
            storeRuntimeError(err);
            running.store(false, std::memory_order_release);
            logMessage("[Capture] Exception thrown inside capture thread");
        }

        releaseGraph();
        CoUninitialize();
        logMessage("[Capture] Capture thread exited");
    }

    void selectCaptureDevice()
    {
        ComPtr<ICreateDevEnum> devEnum;
        throwIfFailed(CoCreateInstance(CLSID_SystemDeviceEnum,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&devEnum)),
                      "Failed to create device enumerator");

        ComPtr<IEnumMoniker> enumMoniker;
        HRESULT hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, enumMoniker.GetAddressOf(), 0);
        if (hr != S_OK || !enumMoniker)
        {
            throw std::runtime_error("No video capture devices were found");
        }

        const std::wstring requested = requestedMoniker;

        ComPtr<IMoniker> matched;
        std::wstring matchedFriendly;
        std::wstring matchedDisplay;

        ComPtr<IMoniker> preferred;
        std::wstring preferredFriendly;
        std::wstring preferredDisplay;

        ComPtr<IMoniker> fallback;
        std::wstring fallbackFriendly;
        std::wstring fallbackDisplay;

        ComPtr<IMoniker> current;
        ULONG fetched = 0;
        while (enumMoniker->Next(1, current.GetAddressOf(), &fetched) == S_OK)
        {
            std::wstring displayName;
            {
                LPOLESTR temp = nullptr;
                if (SUCCEEDED(current->GetDisplayName(nullptr, nullptr, &temp)) && temp)
                {
                    displayName.assign(temp);
                    CoTaskMemFree(temp);
                }
            }

            std::wstring friendly;
            ComPtr<IPropertyBag> bag;
            if (SUCCEEDED(current->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))) && bag)
            {
                VARIANT name;
                VariantInit(&name);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &name, nullptr)) && name.vt == VT_BSTR)
                {
                    friendly.assign(name.bstrVal, SysStringLen(name.bstrVal));
                }
                VariantClear(&name);
            }

            const std::string friendlyLog = friendly.empty() ? narrow(displayName) : narrow(friendly);
            logMessage("[Capture] Found device: " + (friendlyLog.empty() ? std::string("<unnamed>") : friendlyLog));

            const bool matchesRequested = !requested.empty() &&
                ((!displayName.empty() && displayName == requested) || (!friendly.empty() && friendly == requested));

            if (matchesRequested)
            {
                matched = current;
                matchedFriendly = friendly;
                matchedDisplay = displayName;
                logMessage("[Capture] Selected requested device");
                break;
            }

            if (!preferred && !friendly.empty() && friendly == kPreferredDeviceName)
            {
                preferred = current;
                preferredFriendly = friendly;
                preferredDisplay = displayName;
                logMessage("[Capture] Remembering preferred device by friendly name");
            }

            if (!fallback)
            {
                fallback = current;
                fallbackFriendly = friendly;
                fallbackDisplay = displayName;
            }

            current.Reset();
        }

        if (!matched)
        {
            if (preferred)
            {
                matched = preferred;
                matchedFriendly = preferredFriendly;
                matchedDisplay = preferredDisplay;
                logMessage("[Capture] Using preferred device fallback");
            }
            else if (fallback)
            {
                matched = fallback;
                matchedFriendly = fallbackFriendly;
                matchedDisplay = fallbackDisplay;
                logMessage("[Capture] Falling back to first enumerated device");
            }
        }

        if (!matched)
        {
            throw std::runtime_error("Failed to select a video capture device");
        }

        selectedMoniker = matched;
        selectedFriendlyName = matchedFriendly;
        selectedMonikerDisplayName = matchedDisplay;
        if (selectedFriendlyName.empty() && !selectedMonikerDisplayName.empty())
        {
            selectedFriendlyName = selectedMonikerDisplayName;
        }

        logMessage("[Capture] Using device: " + narrow(selectedFriendlyName.empty() ? selectedMonikerDisplayName : selectedFriendlyName));
    }

    void buildGraph()
    {
        throwIfFailed(CoCreateInstance(CLSID_FilterGraph,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&graph)),
                      "Failed to create FilterGraph");

        throwIfFailed(CoCreateInstance(CLSID_CaptureGraphBuilder2,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&captureBuilder)),
                      "Failed to create CaptureGraphBuilder2");

        throwIfFailed(captureBuilder->SetFiltergraph(graph.Get()), "Failed to associate filter graph");

        const std::wstring filterName = !selectedFriendlyName.empty() ? selectedFriendlyName :
            (!selectedMonikerDisplayName.empty() ? selectedMonikerDisplayName : std::wstring(L"Video Capture Source"));

        throwIfFailed(addSourceFilterForMoniker(selectedMoniker.Get(), filterName.c_str(), captureFilter.GetAddressOf()),
                      "Failed to add capture filter");

        throwIfFailed(CoCreateInstance(kCLSID_SampleGrabber,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&sampleGrabberFilter)),
                      "Failed to create Sample Grabber filter");

        throwIfFailed(sampleGrabberFilter->QueryInterface(kIID_ISampleGrabber, reinterpret_cast<void**>(sampleGrabber.GetAddressOf())),
                      "Failed to query ISampleGrabber");

        AM_MEDIA_TYPE mediaType{};
        mediaType.majortype = MEDIATYPE_Video;
        mediaType.formattype = FORMAT_VideoInfo;
        mediaType.subtype = kPreferredVideoSubtypeXrgb;
        throwIfFailed(sampleGrabber->SetMediaType(&mediaType), "Failed to set Sample Grabber media type");
        logMessage("[Capture] Sample Grabber requested XRGB-compatible RGB32 media subtype");

        throwIfFailed(graph->AddFilter(sampleGrabberFilter.Get(), L"Sample Grabber"),
                      "Failed to add Sample Grabber to graph");

        throwIfFailed(CoCreateInstance(kCLSID_NullRenderer,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&nullRenderer)),
                      "Failed to create Null Renderer");
        throwIfFailed(graph->AddFilter(nullRenderer.Get(), L"Null Renderer"),
                      "Failed to add Null Renderer to graph");

        callback = new SampleGrabberCallback(this);
        throwIfFailed(sampleGrabber->SetOneShot(FALSE), "Failed to configure Sample Grabber");
        throwIfFailed(sampleGrabber->SetBufferSamples(TRUE), "Failed to configure Sample Grabber buffering");
        throwIfFailed(sampleGrabber->SetCallback(callback, 1), "Failed to set Sample Grabber callback");

        ComPtr<IAMStreamConfig> streamConfig;
        HRESULT hrConfig = captureBuilder->FindInterface(&PIN_CATEGORY_CAPTURE,
                                                         &MEDIATYPE_Video,
                                                         captureFilter.Get(),
                                                         IID_PPV_ARGS(streamConfig.GetAddressOf()));
        if (FAILED(hrConfig) || !streamConfig)
        {
            hrConfig = captureBuilder->FindInterface(&PIN_CATEGORY_PREVIEW,
                                                     &MEDIATYPE_Video,
                                                     captureFilter.Get(),
                                                     IID_PPV_ARGS(streamConfig.GetAddressOf()));
        }

        if (streamConfig && requestedWidth != 0 && requestedHeight != 0)
        {
            applyRequestedFormat(streamConfig.Get());
        }

        HRESULT hr = captureBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, captureFilter.Get(), sampleGrabberFilter.Get(), nullRenderer.Get());
        if (FAILED(hr))
        {
            hr = captureBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, captureFilter.Get(), sampleGrabberFilter.Get(), nullRenderer.Get());
        }
        throwIfFailed(hr, "Failed to build capture graph");

        logCurrentFormat("Negotiated capture format (post RenderStream)");

        if (audioEnabled)
        {
            HRESULT audioHr = captureBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, captureFilter.Get(), nullptr, nullptr);
            if (FAILED(audioHr))
            {
                audioHr = captureBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Audio, captureFilter.Get(), nullptr, nullptr);
            }
            if (SUCCEEDED(audioHr))
            {
                logMessage("[Capture] Audio playback path connected");
            }
            else
            {
                logMessage("[Capture] Failed to connect audio playback path; continuing without audio");
                audioEnabled = false;
            }
        }

        logSampleGrabberFormat();

        throwIfFailed(graph->QueryInterface(IID_PPV_ARGS(&control)), "Failed to query IMediaControl");
    }

    void applyRequestedFormat(IAMStreamConfig* streamConfig)
    {
        if (!streamConfig || requestedWidth == 0 || requestedHeight == 0)
        {
            return;
        }

        int capabilityCount = 0;
        int capabilitySize = 0;
        if (FAILED(streamConfig->GetNumberOfCapabilities(&capabilityCount, &capabilitySize)) || capabilityCount <= 0 || capabilitySize <= 0)
        {
            logMessage("[Capture] Requested format " + std::to_string(requestedWidth) + "x" + std::to_string(requestedHeight) + " not supported (no capabilities)");
            return;
        }

        std::vector<std::uint8_t> capabilityBuffer(static_cast<std::size_t>(capabilitySize));
        bool applied = false;
        AM_MEDIA_TYPE* genericCandidate = nullptr;

        for (int i = 0; i < capabilityCount; ++i)
        {
            AM_MEDIA_TYPE* mediaType = nullptr;
            if (FAILED(streamConfig->GetStreamCaps(i, &mediaType, capabilityBuffer.data())) || !mediaType)
            {
                continue;
            }

            const bool hasVideoInfo = mediaType->formattype == FORMAT_VideoInfo && mediaType->cbFormat >= sizeof(VIDEOINFOHEADER) && mediaType->pbFormat;
            if (!hasVideoInfo)
            {
                freeMediaType(*mediaType);
                CoTaskMemFree(mediaType);
                continue;
            }

            const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mediaType->pbFormat);
            const std::uint32_t width = static_cast<std::uint32_t>(std::abs(vih->bmiHeader.biWidth));
            const std::uint32_t height = static_cast<std::uint32_t>(std::abs(vih->bmiHeader.biHeight));
            if (width != requestedWidth || height != requestedHeight)
            {
                freeMediaType(*mediaType);
                CoTaskMemFree(mediaType);
                continue;
            }

            if (isSubtype(mediaType->subtype, kPreferredVideoSubtypeXrgb))
            {
                if (SUCCEEDED(streamConfig->SetFormat(mediaType)))
                {
                    logMessage("[Capture] Requested capture format " + std::to_string(requestedWidth) + "x" + std::to_string(requestedHeight) + " applied as XRGB-compatible RGB32");
                    applied = true;
                }
                else
                {
                    logMessage("[Capture] Failed to apply requested capture format " + std::to_string(requestedWidth) + "x" + std::to_string(requestedHeight) + " as XRGB-compatible RGB32");
                }

                freeMediaType(*mediaType);
                CoTaskMemFree(mediaType);
                break;
            }

            if (!genericCandidate)
            {
                genericCandidate = mediaType;
                continue;
            }

            freeMediaType(*mediaType);
            CoTaskMemFree(mediaType);
        }

        if (!applied && genericCandidate)
        {
            if (SUCCEEDED(streamConfig->SetFormat(genericCandidate)))
            {
                logMessage("[Capture] Requested capture format " + std::to_string(requestedWidth) + "x" + std::to_string(requestedHeight) + " applied using non-RGB fallback subtype");
                applied = true;
            }
            else
            {
                logMessage("[Capture] Failed to apply requested capture format " + std::to_string(requestedWidth) + "x" + std::to_string(requestedHeight) + " using non-RGB fallback subtype");
            }
        }

        if (genericCandidate)
        {
            freeMediaType(*genericCandidate);
            CoTaskMemFree(genericCandidate);
        }

        if (!applied)
        {
            logMessage("[Capture] Requested capture format " + std::to_string(requestedWidth) + "x" + std::to_string(requestedHeight) + " not found in device capabilities");
        }
    }

    void logCurrentFormat(const std::string& context)
    {
        if (!captureBuilder || !captureFilter)
        {
            logMessage("[Capture] " + context + ": capture builder unavailable");
            return;
        }

        ComPtr<IAMStreamConfig> streamConfig;
        HRESULT hr = captureBuilder->FindInterface(&PIN_CATEGORY_CAPTURE,
                                                    &MEDIATYPE_Video,
                                                    captureFilter.Get(),
                                                    IID_PPV_ARGS(streamConfig.GetAddressOf()));
        if (FAILED(hr) || !streamConfig)
        {
            hr = captureBuilder->FindInterface(&PIN_CATEGORY_PREVIEW,
                                               &MEDIATYPE_Video,
                                               captureFilter.Get(),
                                               IID_PPV_ARGS(streamConfig.GetAddressOf()));
        }

        if (FAILED(hr) || !streamConfig)
        {
            logMessage("[Capture] " + context + ": IAMStreamConfig not available");
            return;
        }

        AM_MEDIA_TYPE* currentType = nullptr;
        hr = streamConfig->GetFormat(&currentType);
        if (FAILED(hr) || !currentType)
        {
            logMessage("[Capture] " + context + ": IAMStreamConfig::GetFormat failed");
            return;
        }

        const bool hasVideoInfo = currentType->formattype == FORMAT_VideoInfo && currentType->cbFormat >= sizeof(VIDEOINFOHEADER);
        if (!hasVideoInfo)
        {
            logMessage("[Capture] " + context + ": unexpected media type");
        }
        else
        {
            const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(currentType->pbFormat);
            describeVideoInfo(*vih, context, false);
        }

        if (currentType)
        {
            if (currentType->cbFormat != 0 && currentType->pbFormat)
            {
                CoTaskMemFree(currentType->pbFormat);
                currentType->pbFormat = nullptr;
                currentType->cbFormat = 0;
            }
            if (currentType->pUnk)
            {
                currentType->pUnk->Release();
                currentType->pUnk = nullptr;
            }
            CoTaskMemFree(currentType);
        }
    }

    void logSampleGrabberFormat()
    {
        if (!sampleGrabber)
        {
            return;
        }

        AM_MEDIA_TYPE connected{};
        HRESULT hr = sampleGrabber->GetConnectedMediaType(&connected);
        if (FAILED(hr))
        {
            logMessage("[Capture] SampleGrabber::GetConnectedMediaType failed");
            return;
        }

        const bool hasVideoInfo = connected.formattype == FORMAT_VideoInfo && connected.cbFormat >= sizeof(VIDEOINFOHEADER);
        if (!hasVideoInfo)
        {
            freeMediaType(connected);
            throw std::runtime_error("Sample Grabber did not provide a VIDEOINFOHEADER");
        }

        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(connected.pbFormat);
        describeVideoInfo(*vih, "SampleGrabber format", true);
        freeMediaType(connected);
    }

    void describeVideoInfo(const VIDEOINFOHEADER& vih, const std::string& context, bool updateState)
    {
        const LONG biWidth = vih.bmiHeader.biWidth;
        const LONG biHeight = vih.bmiHeader.biHeight;
        const std::uint32_t width = static_cast<std::uint32_t>(std::abs(biWidth));
        const std::uint32_t height = static_cast<std::uint32_t>(std::abs(biHeight));
        const DWORD bits = vih.bmiHeader.biBitCount ? vih.bmiHeader.biBitCount : 32;

        RECT active = vih.rcSource;
        if (active.right <= active.left || active.bottom <= active.top)
        {
            active.left = 0;
            active.top = 0;
            active.right = static_cast<LONG>(width);
            active.bottom = static_cast<LONG>(height);
        }

        const auto clampRect = [](LONG value, LONG minValue, LONG maxValue) {
            return std::clamp(value, minValue, maxValue);
        };

        active.left = clampRect(active.left, 0, static_cast<LONG>(width));
        active.top = clampRect(active.top, 0, static_cast<LONG>(height));
        active.right = clampRect(active.right, active.left + 1, static_cast<LONG>(width));
        active.bottom = clampRect(active.bottom, active.top + 1, static_cast<LONG>(height));

        std::uint32_t bytesPerPixel = bits != 0 ? static_cast<std::uint32_t>((bits + 7u) / 8u) : 4u;
        if (bytesPerPixel == 0)
        {
            bytesPerPixel = 4;
        }
        const std::uint32_t stride = width * bytesPerPixel;
        const bool isBottomUp = biHeight > 0;

        std::ostringstream oss;
        oss << "[Capture] " << context
            << ": frame=" << width << "x" << height
            << " stride=" << stride
            << " bottomUp=" << (isBottomUp ? "true" : "false")
            << " rcSource={" << active.left << ", " << active.top << ", " << active.right << ", " << active.bottom << "}";
        logMessage(oss.str());

        if (updateState)
        {
            frameWidth = width;
            frameHeight = height;
            frameStride = stride;
            bottomUp = isBottomUp;
            contentLeft = static_cast<std::uint32_t>(active.left);
            contentTop = static_cast<std::uint32_t>(active.top);
            contentRight = static_cast<std::uint32_t>(active.right);
            contentBottom = static_cast<std::uint32_t>(active.bottom);
        }
    }

    HRESULT processBuffer(double sampleTime, const BYTE* buffer, long bufferLen)
    {
        if (!running.load(std::memory_order_acquire) || !handler)
        {
            return S_OK;
        }

        if (bufferLen <= 0 || frameWidth == 0 || frameHeight == 0)
        {
            return S_OK;
        }

        DirectShowCapture::Frame frame{};
        frame.data = buffer;
        frame.dataSize = static_cast<std::size_t>(bufferLen);
        frame.sampleWidth = frameWidth;
        frame.sampleHeight = frameHeight;
        frame.contentLeft = contentLeft;
        frame.contentTop = contentTop;
        frame.contentRight = contentRight != 0 ? contentRight : frameWidth;
        frame.contentBottom = contentBottom != 0 ? contentBottom : frameHeight;

        const std::uint32_t activeWidth = frame.contentRight > frame.contentLeft ? (frame.contentRight - frame.contentLeft) : frameWidth;
        const std::uint32_t activeHeight = frame.contentBottom > frame.contentTop ? (frame.contentBottom - frame.contentTop) : frameHeight;

        frame.width = activeWidth != 0 ? activeWidth : frameWidth;
        frame.height = activeHeight != 0 ? activeHeight : frameHeight;
        frame.stride = frameStride != 0 ? frameStride : frameWidth * 4;
        frame.timestamp100ns = sampleTime >= 0.0 ? static_cast<std::uint64_t>(sampleTime * 10'000'000.0) : 0;
        frame.bottomUp = bottomUp;

        try
        {
            if (!loggedSampleSize.exchange(true, std::memory_order_acq_rel))
            {
                logMessage("[Capture] First sample size=" + std::to_string(frame.dataSize));
            }
            handler(frame);
            frameReceived.store(true, std::memory_order_release);
        }
        catch (...)
        {
            storeRuntimeError(std::current_exception());
            return E_FAIL;
        }

        return S_OK;
    }

    void releaseGraph()
    {
        if (sampleGrabber)
        {
            sampleGrabber->SetCallback(nullptr, 0);
        }
        if (callback)
        {
            callback->resetOwner();
            callback->Release();
            callback = nullptr;
        }

        if (control)
        {
            control->Stop();
        }

        nullRenderer.Reset();
        sampleGrabber.Reset();
        sampleGrabberFilter.Reset();
        captureFilter.Reset();
        control.Reset();
        captureBuilder.Reset();
        graph.Reset();
        selectedMoniker.Reset();
        frameWidth = frameHeight = frameStride = 0;
        contentLeft = contentTop = 0;
        contentRight = contentBottom = 0;
        bottomUp = false;
        audioEnabled = false;
    }

    HRESULT addSourceFilterForMoniker(IMoniker* moniker, const wchar_t* name, IBaseFilter** outFilter)
    {
        if (!moniker || !graph)
        {
            return E_POINTER;
        }

        ComPtr<IBaseFilter> filter;
        HRESULT hr = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter));
        if (FAILED(hr))
        {
            return hr;
        }

        hr = graph->AddFilter(filter.Get(), name);
        if (FAILED(hr))
        {
            return hr;
        }

        if (outFilter)
        {
            *outFilter = filter.Detach();
        }

        return S_OK;
    }

    void storeRuntimeError(const std::exception_ptr& err)
    {
        if (!err)
        {
            return;
        }
        try
        {
            std::rethrow_exception(err);
        }
        catch (const std::exception& ex)
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastError = ex.what();
            logMessage(std::string("[Capture] Runtime exception: ") + lastError);
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastError = "Unknown capture error";
            logMessage("[Capture] Runtime exception: unknown");
        }
    }
};

HRESULT SampleGrabberCallback::BufferCB(double sampleTime, BYTE* buffer, long bufferLen)
{
    auto* owner = owner_.load(std::memory_order_acquire);
    if (!owner)
    {
        return S_OK;
    }
    return owner->processBuffer(sampleTime, buffer, bufferLen);
}

DirectShowCapture::DirectShowCapture()
    : impl_(std::make_unique<DirectShowCaptureImpl>())
{
}

DirectShowCapture::~DirectShowCapture() = default;

void DirectShowCapture::start(FrameHandler handler, const Options& options)
{
    impl_->start(std::move(handler), options);
}

void DirectShowCapture::stop()
{
    impl_->stop();
}

std::string DirectShowCapture::consumeLastError()
{
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    std::string result = impl_->lastError;
    impl_->lastError.clear();
    return result;
}

std::string DirectShowCapture::currentDeviceFriendlyName() const
{
    return impl_->currentFriendlyName();
}
