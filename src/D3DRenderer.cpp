#include "D3DRenderer.hpp"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef PCKVM_RENDERER_LOGGING
#define PCKVM_RENDERER_LOGGING 0
#endif

using Microsoft::WRL::ComPtr;

namespace
{
    struct Vertex
    {
        float position[3];
        float tex[2];
    };

    constexpr std::array<Vertex, 4> kVertices = {
        Vertex{{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
        Vertex{{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        Vertex{{1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        Vertex{{1.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
    };

    constexpr std::array<std::uint16_t, 6> kIndices = {0, 1, 2, 2, 1, 3};

    constexpr const char* kVertexShaderSource = R"(struct VSInput
{
    float3 position : POSITION;
    float2 tex : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 tex : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0f);
    float2 tex;
    tex.x = (input.position.x + 1.0f) * 0.5f;
    tex.y = (1.0f - input.position.y) * 0.5f;
    output.tex = tex;
    return output;
}
)";

    constexpr const char* kPixelShaderSource = R"(Texture2D frameTex : register(t0);
SamplerState frameSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 tex : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    return frameTex.Sample(frameSampler, input.tex);
}
)";

    constexpr const char* kPixelShaderNv12Source = R"(Texture2D yTex : register(t0);
Texture2D uvTex : register(t1);
SamplerState frameSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 tex : TEXCOORD0;
};

float4 SampleNv12(float2 uv)
{
    const float y = yTex.Sample(frameSampler, uv).r;
    const float2 chroma = uvTex.Sample(frameSampler, uv).rg;

    const float c = y - (16.0f / 255.0f);
    const float u = chroma.x - 0.5f;
    const float v = chroma.y - 0.5f;

    float3 rgb;
    rgb.r = 1.164383f * c + 1.596027f * v;
    rgb.g = 1.164383f * c - 0.391762f * u - 0.812968f * v;
    rgb.b = 1.164383f * c + 2.017232f * u;
    return float4(saturate(rgb), 1.0f);
}

float4 main(PSInput input) : SV_Target
{
    return SampleNv12(input.tex);
}
)";

    constexpr const char* kPixelShaderGradientSource = R"(struct PSInput
{
    float4 position : SV_Position;
    float2 tex : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    return float4(input.tex, 0.0f, 1.0f);
}
)";

    constexpr const char* kPixelShaderBlurSource = R"(Texture2D frameTex : register(t0);
SamplerState frameSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 tex : TEXCOORD0;
};

float4 SampleDualKawaseLevel(float2 uv, float2 offset)
{
    float4 d = 0.0f;
    d += frameTex.Sample(frameSampler, uv + float2( offset.x,  offset.y));
    d += frameTex.Sample(frameSampler, uv + float2(-offset.x,  offset.y));
    d += frameTex.Sample(frameSampler, uv + float2( offset.x, -offset.y));
    d += frameTex.Sample(frameSampler, uv + float2(-offset.x, -offset.y));

    float4 a = 0.0f;
    a += frameTex.Sample(frameSampler, uv + float2( offset.x, 0.0f));
    a += frameTex.Sample(frameSampler, uv + float2(-offset.x, 0.0f));
    a += frameTex.Sample(frameSampler, uv + float2(0.0f,  offset.y));
    a += frameTex.Sample(frameSampler, uv + float2(0.0f, -offset.y));

    return (d + a) * (1.0f / 8.0f);
}

float4 main(PSInput input) : SV_Target
{
    uint w = 0, h = 0;
    frameTex.GetDimensions(w, h);
    float2 texel = float2(1.0f / max(1u, w), 1.0f / max(1u, h));

    const float samplePosModifier = 1.0f;

    float2 level2Offset = texel * (2.0f * samplePosModifier);
    float2 level3Offset = texel * (3.0f * samplePosModifier);
    float2 level4Offset = texel * (4.0f * samplePosModifier);
    float2 level5Offset = texel * (5.0f * samplePosModifier);
    float2 level6Offset = texel * (6.0f * samplePosModifier);

    float4 l1 = SampleDualKawaseLevel(input.tex, level2Offset);
    float4 l2 = SampleDualKawaseLevel(input.tex, level3Offset);
    float4 l3 = SampleDualKawaseLevel(input.tex, level4Offset);
    float4 l4 = SampleDualKawaseLevel(input.tex, level5Offset);
    float4 l5 = SampleDualKawaseLevel(input.tex, level6Offset);

    float4 c = l1 * 0.10f + l2 * 0.15f + l3 * 0.25f + l4 * 0.20f + l5 * 0.12f;

    c.rgb *= 0.40f;
    return c;
}
)";

    constexpr const char* kPixelShaderNv12BlurSource = R"(Texture2D yTex : register(t0);
Texture2D uvTex : register(t1);
SamplerState frameSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 tex : TEXCOORD0;
};

float4 SampleNv12(float2 uv)
{
    const float y = yTex.Sample(frameSampler, uv).r;
    const float2 chroma = uvTex.Sample(frameSampler, uv).rg;

    const float c = y - (16.0f / 255.0f);
    const float u = chroma.x - 0.5f;
    const float v = chroma.y - 0.5f;

    float3 rgb;
    rgb.r = 1.164383f * c + 1.596027f * v;
    rgb.g = 1.164383f * c - 0.391762f * u - 0.812968f * v;
    rgb.b = 1.164383f * c + 2.017232f * u;
    return float4(saturate(rgb), 1.0f);
}

float4 SampleDualKawaseLevel(float2 uv, float2 offset)
{
    float4 d = 0.0f;
    d += SampleNv12(uv + float2( offset.x,  offset.y));
    d += SampleNv12(uv + float2(-offset.x,  offset.y));
    d += SampleNv12(uv + float2( offset.x, -offset.y));
    d += SampleNv12(uv + float2(-offset.x, -offset.y));

    float4 a = 0.0f;
    a += SampleNv12(uv + float2( offset.x, 0.0f));
    a += SampleNv12(uv + float2(-offset.x, 0.0f));
    a += SampleNv12(uv + float2(0.0f,  offset.y));
    a += SampleNv12(uv + float2(0.0f, -offset.y));

    return (d + a) * (1.0f / 8.0f);
}

float4 main(PSInput input) : SV_Target
{
    uint w = 0, h = 0;
    yTex.GetDimensions(w, h);
    float2 texel = float2(1.0f / max(1u, w), 1.0f / max(1u, h));

    const float samplePosModifier = 1.0f;

    float2 level2Offset = texel * (2.0f * samplePosModifier);
    float2 level3Offset = texel * (3.0f * samplePosModifier);
    float2 level4Offset = texel * (4.0f * samplePosModifier);
    float2 level5Offset = texel * (5.0f * samplePosModifier);
    float2 level6Offset = texel * (6.0f * samplePosModifier);

    float4 l1 = SampleDualKawaseLevel(input.tex, level2Offset);
    float4 l2 = SampleDualKawaseLevel(input.tex, level3Offset);
    float4 l3 = SampleDualKawaseLevel(input.tex, level4Offset);
    float4 l4 = SampleDualKawaseLevel(input.tex, level5Offset);
    float4 l5 = SampleDualKawaseLevel(input.tex, level6Offset);

    float4 c = l1 * 0.10f + l2 * 0.15f + l3 * 0.25f + l4 * 0.20f + l5 * 0.12f;

    c.rgb *= 0.40f;
    return c;
}
)";
#if PCKVM_RENDERER_LOGGING
    using OutputStream = std::ofstream;

    void logMessage(std::string_view message)
    {
        OutputStream("viewer.log", std::ios::app) << message << '\n';
    }

    std::string hrToString(HRESULT hr)
    {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
            << static_cast<unsigned long>(hr);
        return oss.str();
    }

    void logFailure(const char* label, HRESULT hr)
    {
        std::ostringstream oss;
        oss << "[Renderer] " << label << " failed hr=" << hrToString(hr);
        logMessage(oss.str());
    }

    std::string wideToUtf8(const std::wstring& input)
    {
        if (input.empty())
        {
            return {};
        }

        const int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            return {};
        }

        std::string result(static_cast<std::size_t>(size - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, result.data(), size, nullptr, nullptr);
        return result;
    }
#else
    inline void logMessage(std::string_view) {}
    inline void logFailure(const char*, HRESULT) {}
    inline std::string wideToUtf8(const std::wstring&) { return {}; }
#endif
}

#if PCKVM_RENDERER_LOGGING
namespace
{
    void logInfoQueueMessages(ID3D12Device* device, const char* scope)
    {
        if (!device)
        {
            return;
        }

        ComPtr<ID3D12InfoQueue> infoQueue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(infoQueue.GetAddressOf()))))
        {
            return;
        }

        const UINT64 messageCount = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < messageCount; ++i)
        {
            SIZE_T messageLength = 0;
            if (FAILED(infoQueue->GetMessage(i, nullptr, &messageLength)) || messageLength == 0)
            {
                continue;
            }

            std::vector<char> storage(messageLength);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue->GetMessage(i, message, &messageLength)) && message)
            {
                std::ostringstream oss;
                oss << "[Renderer][InfoQueue] " << scope << " severity=" << static_cast<int>(message->Severity)
                    << " id=" << message->ID << " description=" << message->pDescription;
                logMessage(oss.str());
            }
        }

        infoQueue->ClearStoredMessages();
    }
}
#else
namespace
{
    inline void logInfoQueueMessages(ID3D12Device*, const char*) {}
}
#endif

D3DRenderer::~D3DRenderer()
{
    shutdown();
}

bool D3DRenderer::initialize(HWND hwnd)
{
    if (hwnd == nullptr)
    {
        return false;
    }

    if (!createDevice(hwnd))
    {
        return false;
    }

    if (!createSwapChain(hwnd)
    )
    {
        return false;
    }

    if (!createPipelineResources())
    {
        return false;
    }

    if (!createRenderTargets())
    {
        return false;
    }

    updateViewport(backBufferWidth_, backBufferHeight_);

    return true;
}

void D3DRenderer::shutdown()
{
    waitForGpu();

    destroyFrameResources();
    destroyRenderTarget();

    if (frameLatencyWaitableObject_)
    {
        CloseHandle(frameLatencyWaitableObject_);
        frameLatencyWaitableObject_ = nullptr;
    }

    if (fenceEvent_)
    {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }

    commandList_.Reset();
    for (auto& ctx : frameContexts_)
    {
        ctx.commandAllocator.Reset();
        ctx.fenceValue = 0;
    }

    fence_.Reset();
    for (auto& rt : renderTargets_)
    {
        rt.Reset();
    }

    samplerHeap_.Reset();
    srvHeap_.Reset();
    rtvHeap_.Reset();

    pipelineStateBlur_.Reset();
    pipelineStateNv12Blur_.Reset();
    pipelineStateGradient_.Reset();
    pipelineStateNv12_.Reset();
    pipelineState_.Reset();
    rootSignature_.Reset();
    indexBuffer_.Reset();
    vertexBuffer_.Reset();
    frameTexture_.Reset();
    swapChain_.Reset();
    commandQueue_.Reset();
    device_.Reset();

    frameWidth_ = frameHeight_ = frameStride_ = 0;
    frameFormat_ = FrameFormat::BGRA8;
    backBufferWidth_ = backBufferHeight_ = 0;
    fenceValue_ = 1;
    allowTearing_ = false;
    loggedGpuPixels_ = false;
}

void D3DRenderer::onResize(UINT width, UINT height)
{
    if (!swapChain_)
    {
        return;
    }

    waitForGpu();
    destroyRenderTarget();

    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (allowTearing_)
    {
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    HRESULT hr = swapChain_->ResizeBuffers(kFrameCount,
                                           width,
                                           height,
                                           DXGI_FORMAT_B8G8R8A8_UNORM,
                                           flags);
    if (FAILED(hr))
    {
        throw std::runtime_error("Failed to resize swap chain buffers");
    }

    frameLatencyWaitableObject_ = swapChain_->GetFrameLatencyWaitableObject();

    if (!createRenderTargets())
    {
        throw std::runtime_error("Failed to recreate render targets after resize");
    }

    updateViewport(backBufferWidth_, backBufferHeight_);
}

bool D3DRenderer::ensureFrameResources(std::uint32_t width,
                                       std::uint32_t height,
                                       std::uint32_t stride,
                                       FrameFormat format)
{
    if (!device_ || width == 0 || height == 0)
    {
        return false;
    }

    if (format == FrameFormat::NV12 && ((width & 1u) != 0 || (height & 1u) != 0))
    {
        return false;
    }

    const std::uint32_t defaultStride = (format == FrameFormat::NV12) ? width : width * 4u;
    const std::uint32_t effectiveStride = stride != 0 ? stride : defaultStride;

    const bool needsRecreate = !frameTexture_
        || frameWidth_ != width
        || frameHeight_ != height
        || frameFormat_ != format;

    if (needsRecreate)
    {
        waitForGpu();
        destroyFrameResources();

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = (format == FrameFormat::NV12) ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        HRESULT hr = device_->CreateCommittedResource(&defaultHeap,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &desc,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                      nullptr,
                                                      IID_PPV_ARGS(frameTexture_.GetAddressOf()));
        if (FAILED(hr))
        {
            logFailure("CreateCommittedResource frame texture", hr);
            frameTexture_.Reset();
            return false;
        }

        if (format == FrameFormat::NV12)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC yView{};
            yView.Format = DXGI_FORMAT_R8_UNORM;
            yView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            yView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            yView.Texture2D.MostDetailedMip = 0;
            yView.Texture2D.MipLevels = 1;
            yView.Texture2D.PlaneSlice = 0;
            yView.Texture2D.ResourceMinLODClamp = 0.0f;
            device_->CreateShaderResourceView(frameTexture_.Get(), &yView, srvHandleFrameCpu_);

            D3D12_SHADER_RESOURCE_VIEW_DESC uvView = yView;
            uvView.Format = DXGI_FORMAT_R8G8_UNORM;
            uvView.Texture2D.PlaneSlice = 1;
            device_->CreateShaderResourceView(frameTexture_.Get(), &uvView, srvHandleFramePlane1Cpu_);
        }
        else
        {
            device_->CreateShaderResourceView(frameTexture_.Get(), nullptr, srvHandleFrameCpu_);
            device_->CreateShaderResourceView(frameTexture_.Get(), nullptr, srvHandleFramePlane1Cpu_);
        }

        std::uint64_t totalBytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
        UINT rowCounts[2]{};
        std::uint64_t rowSizes[2]{};
        const UINT subresourceCount = (format == FrameFormat::NV12) ? 2u : 1u;

        device_->GetCopyableFootprints(&desc,
                                       0,
                                       subresourceCount,
                                       0,
                                       footprints,
                                       rowCounts,
                                       rowSizes,
                                       &totalBytes);

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Alignment = 0;
        uploadDesc.Width = totalBytes;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.SampleDesc.Quality = 0;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        for (auto& upload : frameUploads_)
        {
            HRESULT uploadHr = device_->CreateCommittedResource(&uploadHeap,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &uploadDesc,
                                                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                                                nullptr,
                                                                IID_PPV_ARGS(upload.resource.GetAddressOf()));
            if (FAILED(uploadHr))
            {
                logFailure("CreateCommittedResource frame upload", uploadHr);
                destroyFrameResources();
                frameTexture_.Reset();
                return false;
            }

            upload.sizeBytes = totalBytes;
            upload.subresourceCount = subresourceCount;
            for (UINT i = 0; i < subresourceCount; ++i)
            {
                upload.layouts[i] = footprints[i];
                upload.rowCounts[i] = rowCounts[i];
                upload.rowSizes[i] = rowSizes[i];
            }

            uploadHr = upload.resource->Map(0, nullptr, reinterpret_cast<void**>(&upload.cpuAddress));
            if (FAILED(uploadHr))
            {
                logFailure("Map frame upload buffer", uploadHr);
                destroyFrameResources();
                frameTexture_.Reset();
                return false;
            }
        }

        std::fill(std::begin(pendingUpload_), std::end(pendingUpload_), false);
    }

    frameWidth_ = width;
    frameHeight_ = height;
    frameStride_ = effectiveStride;
    frameFormat_ = format;

    return true;
}

void D3DRenderer::destroyFrameResources()
{
    for (auto& upload : frameUploads_)
    {
        if (upload.resource && upload.cpuAddress)
        {
            upload.resource->Unmap(0, nullptr);
        }

        upload.cpuAddress = nullptr;
        upload.resource.Reset();
        std::fill(std::begin(upload.layouts), std::end(upload.layouts), D3D12_PLACED_SUBRESOURCE_FOOTPRINT{});
        std::fill(std::begin(upload.rowCounts), std::end(upload.rowCounts), 0u);
        std::fill(std::begin(upload.rowSizes), std::end(upload.rowSizes), 0ull);
        upload.subresourceCount = 0;
        upload.sizeBytes = 0;
    }

    frameTexture_.Reset();
    std::fill(std::begin(pendingUpload_), std::end(pendingUpload_), false);
    frameWidth_ = 0;
    frameHeight_ = 0;
    frameStride_ = 0;
    frameFormat_ = FrameFormat::BGRA8;
}

void D3DRenderer::uploadFrame(const void* data,
                              std::size_t dataSize,
                              std::uint32_t stride,
                              std::uint32_t width,
                              std::uint32_t height,
                              FrameFormat format)
{
    if (!device_ || !data || width == 0 || height == 0)
    {
        return;
    }

    const std::uint32_t defaultStride = (format == FrameFormat::NV12) ? width : width * 4u;
    const std::uint32_t effectiveStride = stride != 0 ? stride : defaultStride;
    if (!ensureFrameResources(width, height, effectiveStride, format))
    {
        return;
    }

    UINT uploadIndex = 0;
    if (swapChain_)
    {
        uploadIndex = swapChain_->GetCurrentBackBufferIndex();
        uploadIndex %= kFrameCount;
    }

    waitForFrame(frameContexts_[uploadIndex]);

    UploadResource& upload = frameUploads_[uploadIndex];
    if (!upload.cpuAddress || upload.subresourceCount == 0)
    {
        return;
    }

    const auto* sourceBytes = static_cast<const std::uint8_t*>(data);

    auto copyPlane = [&](UINT subresourceIndex, std::size_t sourcePlaneOffset, std::uint32_t sourceStride) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = upload.layouts[subresourceIndex];
        const UINT rowCount = upload.rowCounts[subresourceIndex];
        const std::size_t rowSize = static_cast<std::size_t>(upload.rowSizes[subresourceIndex]);
        if (layout.Footprint.RowPitch == 0 || rowCount == 0 || rowSize == 0)
        {
            return false;
        }
        if (sourceStride < rowSize)
        {
            return false;
        }

        std::uint8_t* dstBase = upload.cpuAddress + layout.Offset;
        const std::size_t dstPitch = layout.Footprint.RowPitch;

        for (UINT row = 0; row < rowCount; ++row)
        {
            const std::size_t srcOffset = sourcePlaneOffset + static_cast<std::size_t>(row) * sourceStride;
            std::uint8_t* dstRow = dstBase + static_cast<std::size_t>(row) * dstPitch;

            if (srcOffset >= dataSize)
            {
                std::memset(dstRow, 0, rowSize);
                continue;
            }

            const std::size_t available = dataSize - srcOffset;
            const std::size_t copyBytes = std::min(rowSize, available);
            std::memcpy(dstRow, sourceBytes + srcOffset, copyBytes);
            if (copyBytes < rowSize)
            {
                std::memset(dstRow + copyBytes, 0, rowSize - copyBytes);
            }
        }

        return true;
    };

    bool copied = false;
    if (format == FrameFormat::NV12)
    {
        const std::size_t uvOffset = static_cast<std::size_t>(effectiveStride) * height;
        copied = copyPlane(0, 0, effectiveStride)
            && upload.subresourceCount >= 2
            && copyPlane(1, uvOffset, effectiveStride);
    }
    else
    {
        copied = copyPlane(0, 0, effectiveStride);
    }

    if (!copied)
    {
        return;
    }

    pendingUpload_[uploadIndex] = true;
    loggedGpuPixels_ = false;
}

void D3DRenderer::render(const std::function<void(ID3D12GraphicsCommandList*)>& overlayCallback)
{
    if (!swapChain_ || !commandQueue_ || !commandList_)
    {
        return;
    }

    const UINT backBufferIndex = swapChain_->GetCurrentBackBufferIndex();
    FrameContext& frameContext = frameContexts_[backBufferIndex];
    waitForFrame(frameContext);

    HRESULT hr = frameContext.commandAllocator->Reset();
    if (FAILED(hr))
    {
        logFailure("CommandAllocator::Reset", hr);
        return;
    }

    hr = commandList_->Reset(frameContext.commandAllocator.Get(), nullptr);
    if (FAILED(hr))
    {
        logFailure("CommandList::Reset", hr);
        return;
    }

    UploadResource& upload = frameUploads_[backBufferIndex];
    if (pendingUpload_[backBufferIndex] && frameTexture_ && upload.resource)
    {
        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        toCopy.Transition.pResource = frameTexture_.Get();
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commandList_->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = frameTexture_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

        for (UINT subresource = 0; subresource < upload.subresourceCount; ++subresource)
        {
            dst.SubresourceIndex = subresource;
            src.PlacedFootprint = upload.layouts[subresource];
            commandList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        D3D12_RESOURCE_BARRIER toShader{};
        toShader.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toShader.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        toShader.Transition.pResource = frameTexture_.Get();
        toShader.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toShader.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList_->ResourceBarrier(1, &toShader);

        pendingUpload_[backBufferIndex] = false;

#if PCKVM_RENDERER_LOGGING
        if (!loggedGpuPixels_ && upload.cpuAddress)
        {
            const std::uint32_t bytesPerPixel = 4;
            const std::size_t dstPitch = upload.layouts[0].Footprint.RowPitch;
            const std::uint8_t* pixels = upload.cpuAddress + upload.layouts[0].Offset;

            auto logPixel = [&](const char* label, std::uint32_t row, std::uint32_t col) {
                if (row < frameHeight_ && col < frameWidth_)
                {
                    const std::size_t offset = static_cast<std::size_t>(row) * dstPitch + static_cast<std::size_t>(col) * bytesPerPixel;
                    const std::uint8_t* px = pixels + offset;
                    std::ostringstream pixelStream;
                    pixelStream << "[CPU] Sample pixel " << label
                                << " (row=" << row << ", col=" << col << ") = "
                                << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                                << static_cast<int>(px[0])
                                << std::setw(2) << static_cast<int>(px[1])
                                << std::setw(2) << static_cast<int>(px[2])
                                << std::setw(2) << static_cast<int>(px[3]);
                    logMessage(pixelStream.str());
                }
            };

            std::ostringstream oss;
            oss << "[CPU] RowStride=" << dstPitch;
            logMessage(oss.str());
            logPixel("top-left", 0, 0);
            logPixel("center", frameHeight_ / 2, frameWidth_ / 2);
            logPixel("bottom-right", frameHeight_ - 1, frameWidth_ - 1);
            loggedGpuPixels_ = true;
        }
#endif
    }

    ID3D12Resource* backBuffer = renderTargets_[backBufferIndex].Get();
    if (!backBuffer)
    {
        commandList_->Close();
        return;
    }

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toRenderTarget.Transition.pResource = backBuffer;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList_->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHandleStart_;
    rtvHandle.ptr += static_cast<SIZE_T>(backBufferIndex) * rtvDescriptorSize_;

    const float clearColor[] = {0.f, 0.f, 0.f, 1.f};
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    ID3D12PipelineState* activePso = (frameFormat_ == FrameFormat::NV12 && pipelineStateNv12_)
        ? pipelineStateNv12_.Get()
        : pipelineState_.Get();
    if (blurEnabled_)
    {
        if (frameFormat_ == FrameFormat::NV12 && pipelineStateNv12Blur_)
        {
            activePso = pipelineStateNv12Blur_.Get();
        }
        else if (pipelineStateBlur_)
        {
            activePso = pipelineStateBlur_.Get();
        }
    }
    commandList_->SetPipelineState(activePso);

    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get(), samplerHeap_.Get()};
    commandList_->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
    commandList_->SetGraphicsRootDescriptorTable(0, srvHandleFrameGpu_);
    commandList_->SetGraphicsRootDescriptorTable(1, samplerHandleGpu_);

    commandList_->RSSetViewports(1, &viewport_);
    commandList_->RSSetScissorRects(1, &scissorRect_);

    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList_->IASetIndexBuffer(&indexBufferView_);
    commandList_->DrawIndexedInstanced(static_cast<UINT>(kIndices.size()), 1, 0, 0, 0);

    if (overlayCallback)
    {
        ID3D12DescriptorHeap* overlayHeaps[] = {srvHeap_.Get()};
        commandList_->SetDescriptorHeaps(1, overlayHeaps);
        overlayCallback(commandList_.Get());
        commandList_->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
    }

    D3D12_RESOURCE_BARRIER toPresent{};
    toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toPresent.Transition.pResource = backBuffer;
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &toPresent);

    hr = commandList_->Close();
    if (FAILED(hr))
    {
        logFailure("CommandList::Close", hr);
        return;
    }

    ID3D12CommandList* const commandLists[] = {commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);

    const UINT syncInterval = vsyncEnabled_ ? 1u : (allowTearing_ ? 0u : 1u);
    const UINT presentFlags = (!vsyncEnabled_ && allowTearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    swapChain_->Present(syncInterval, presentFlags);

    const std::uint64_t fenceValue = fenceValue_++;
    commandQueue_->Signal(fence_.Get(), fenceValue);
    frameContext.fenceValue = fenceValue;
}

bool D3DRenderer::createDevice(HWND)
{
    ComPtr<IDXGIFactory6> factory;
    UINT factoryFlags = 0;

    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateDXGIFactory2 (device)", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    std::wstring adapterNameW;
    UINT vendorId = 0;
    UINT deviceId = 0;
    for (UINT adapterIndex = 0;
         SUCCEEDED(factory->EnumAdapterByGpuPreference(adapterIndex,
                                                       DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                       IID_PPV_ARGS(adapter.GetAddressOf())));
         ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter.Reset();
            continue;
        }

        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device_.GetAddressOf()));
        if (SUCCEEDED(hr))
        {
            adapterNameW.assign(desc.Description);
            vendorId = desc.VendorId;
            deviceId = desc.DeviceId;
            break;
        }
        logFailure("D3D12CreateDevice (hardware adapter)", hr);
        device_.Reset();
        adapter.Reset();
    }

    if (!device_)
    {
        ComPtr<IDXGIAdapter> warpAdapter;
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(warpAdapter.GetAddressOf()))))
        {
            logMessage("[Renderer] EnumWarpAdapter failed");
            return false;
        }

        hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device_.GetAddressOf()));
        if (FAILED(hr))
        {
            logFailure("D3D12CreateDevice (WARP)", hr);
            return false;
        }

        DXGI_ADAPTER_DESC warpDesc{};
        warpAdapter->GetDesc(&warpDesc);
        adapterNameW.assign(warpDesc.Description);
        vendorId = warpDesc.VendorId;
        deviceId = warpDesc.DeviceId;
    }

#if PCKVM_RENDERER_LOGGING
    {
        std::string name = wideToUtf8(adapterNameW);
        std::ostringstream oss;
        oss << "[Renderer] Using adapter '" << name << "' VendorId=" << vendorId
            << " DeviceId=" << deviceId;
        logMessage(oss.str());
    }
#endif

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue_.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateCommandQueue", hr);
        return false;
    }

    for (auto& context : frameContexts_)
    {
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(context.commandAllocator.GetAddressOf()));
        if (FAILED(hr))
        {
            logFailure("CreateCommandAllocator", hr);
            return false;
        }
    }

    hr = device_->CreateCommandList(0,
                                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    frameContexts_[0].commandAllocator.Get(),
                                    nullptr,
                                    IID_PPV_ARGS(commandList_.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateCommandList", hr);
        return false;
    }

    commandList_->Close();

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateFence", hr);
        return false;
    }

    fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_)
    {
        logMessage("[Renderer] CreateEvent failed");
        return false;
    }

    return true;
}

bool D3DRenderer::createSwapChain(HWND hwnd)
{
    ComPtr<IDXGIFactory6> factory;
    UINT factoryFlags = 0;

    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateDXGIFactory2 (swap chain)", hr);
        return false;
    }

    ComPtr<IDXGIFactory5> factory5;
    allowTearing_ = false;
    if (SUCCEEDED(factory.As(&factory5)))
    {
        BOOL tearingSupported = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &tearingSupported,
                                                    sizeof(tearingSupported))) &&
            tearingSupported)
        {
            allowTearing_ = true;
        }
    }

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = 0;
    swapDesc.Height = 0;
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.Stereo = FALSE;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SampleDesc.Quality = 0;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = kFrameCount;
    swapDesc.Scaling = DXGI_SCALING_STRETCH;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (allowTearing_)
    {
        swapDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory->CreateSwapChainForHwnd(commandQueue_.Get(),
                                         hwnd,
                                         &swapDesc,
                                         nullptr,
                                         nullptr,
                                         swapChain1.GetAddressOf());
    if (FAILED(hr))
    {
        logFailure("CreateSwapChainForHwnd", hr);
        return false;
    }

    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = swapChain1.As(&swapChain_);
    if (FAILED(hr))
    {
        logFailure("QueryInterface IDXGISwapChain4", hr);
        return false;
    }

    swapChain_->SetMaximumFrameLatency(1);
    frameLatencyWaitableObject_ = swapChain_->GetFrameLatencyWaitableObject();

    return true;
}

bool D3DRenderer::createPipelineResources()
{
    if (!device_)
    {
        return false;
    }

    HRESULT hr = S_OK;

    if (!rtvHeap_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = kFrameCount;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(rtvHeap_.GetAddressOf()))))
        {
            logMessage("[Renderer] CreateDescriptorHeap RTV failed");
            return false;
        }
        rtvHandleStart_ = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.NumDescriptors = 3;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(srvHeap_.GetAddressOf()))))
    {
        logMessage("[Renderer] CreateDescriptorHeap SRV failed");
        return false;
    }
    srvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srvHandleFrameCpu_ = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvHandleFrameGpu_ = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    srvHandleFramePlane1Cpu_ = srvHandleFrameCpu_;
    srvHandleFramePlane1Cpu_.ptr += srvDescriptorSize_;
    srvHandleFramePlane1Gpu_ = srvHandleFrameGpu_;
    srvHandleFramePlane1Gpu_.ptr += srvDescriptorSize_;
    srvHandleImGuiCpu_ = srvHandleFrameCpu_;
    srvHandleImGuiCpu_.ptr += static_cast<SIZE_T>(srvDescriptorSize_) * 2u;
    srvHandleImGuiGpu_ = srvHandleFrameGpu_;
    srvHandleImGuiGpu_.ptr += static_cast<UINT64>(srvDescriptorSize_) * 2u;

    D3D12_DESCRIPTOR_HEAP_DESC samplerDesc{};
    samplerDesc.NumDescriptors = 1;
    samplerDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&samplerDesc, IID_PPV_ARGS(samplerHeap_.GetAddressOf()))))
    {
        logMessage("[Renderer] CreateDescriptorHeap Sampler failed");
        return false;
    }
    samplerHandleGpu_ = samplerHeap_->GetGPUDescriptorHandleForHeapStart();

    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] = sampler.BorderColor[3] = 0.0f;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    device_->CreateSampler(&sampler, samplerHeap_->GetCPUDescriptorHandleForHeapStart());

    const UINT vbSize = static_cast<UINT>(kVertices.size() * sizeof(Vertex));
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vbDesc{};
    vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Alignment = 0;
    vbDesc.Width = vbSize;
    vbDesc.Height = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels = 1;
    vbDesc.Format = DXGI_FORMAT_UNKNOWN;
    vbDesc.SampleDesc.Count = 1;
    vbDesc.SampleDesc.Quality = 0;
    vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    vbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(device_->CreateCommittedResource(&uploadHeap,
                                                D3D12_HEAP_FLAG_NONE,
                                                &vbDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr,
                                                IID_PPV_ARGS(vertexBuffer_.GetAddressOf()))))
    {
        logMessage("[Renderer] CreateCommittedResource vertex buffer failed");
        return false;
    }

    void* mappedData = nullptr;
    if (FAILED(vertexBuffer_->Map(0, nullptr, &mappedData)))
    {
        logMessage("[Renderer] Vertex buffer map failed");
        return false;
    }
    std::memcpy(mappedData, kVertices.data(), vbSize);
    vertexBuffer_->Unmap(0, nullptr);

    vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vertexBufferView_.StrideInBytes = sizeof(Vertex);
    vertexBufferView_.SizeInBytes = vbSize;

    const UINT ibSize = static_cast<UINT>(kIndices.size() * sizeof(std::uint16_t));
    D3D12_RESOURCE_DESC ibDesc = vbDesc;
    ibDesc.Width = ibSize;

    if (FAILED(device_->CreateCommittedResource(&uploadHeap,
                                                D3D12_HEAP_FLAG_NONE,
                                                &ibDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr,
                                                IID_PPV_ARGS(indexBuffer_.GetAddressOf()))))
    {
        logMessage("[Renderer] CreateCommittedResource index buffer failed");
        return false;
    }

    mappedData = nullptr;
    if (FAILED(indexBuffer_->Map(0, nullptr, &mappedData)))
    {
        logMessage("[Renderer] Index buffer map failed");
        return false;
    }
    std::memcpy(mappedData, kIndices.data(), ibSize);
    indexBuffer_->Unmap(0, nullptr);

    indexBufferView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    indexBufferView_.Format = DXGI_FORMAT_R16_UINT;
    indexBufferView_.SizeInBytes = ibSize;

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> psNv12Blob;
    ComPtr<ID3DBlob> psGradientBlob;
    ComPtr<ID3DBlob> psBlurBlob;
    ComPtr<ID3DBlob> psNv12BlurBlob;
    ComPtr<ID3DBlob> errorBlob;

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    if (FAILED(D3DCompile(kVertexShaderSource,
                          std::strlen(kVertexShaderSource),
                          nullptr,
                          nullptr,
                          nullptr,
                          "main",
                          "vs_5_0",
                          compileFlags,
                          0,
                          vsBlob.GetAddressOf(),
                          errorBlob.GetAddressOf())))
    {
        logMessage("[Renderer] Vertex shader compilation failed");
        return false;
    }

    errorBlob.Reset();
    if (FAILED(D3DCompile(kPixelShaderSource,
                          std::strlen(kPixelShaderSource),
                          nullptr,
                          nullptr,
                          nullptr,
                          "main",
                          "ps_5_0",
                          compileFlags,
                          0,
                          psBlob.GetAddressOf(),
                          errorBlob.GetAddressOf())))
    {
        logMessage("[Renderer] Pixel shader compilation failed");
        return false;
    }

    errorBlob.Reset();
    if (FAILED(D3DCompile(kPixelShaderNv12Source,
                          std::strlen(kPixelShaderNv12Source),
                          nullptr,
                          nullptr,
                          nullptr,
                          "main",
                          "ps_5_0",
                          compileFlags,
                          0,
                          psNv12Blob.GetAddressOf(),
                          errorBlob.GetAddressOf())))
    {
        logMessage("[Renderer] NV12 pixel shader compilation failed");
        return false;
    }

    errorBlob.Reset();
    if (FAILED(D3DCompile(kPixelShaderGradientSource,
                          std::strlen(kPixelShaderGradientSource),
                          nullptr,
                          nullptr,
                          nullptr,
                          "main",
                          "ps_5_0",
                          compileFlags,
                          0,
                          psGradientBlob.GetAddressOf(),
                          errorBlob.GetAddressOf())))
    {
        logMessage("[Renderer] Gradient shader compilation failed");
        return false;
    }

    errorBlob.Reset();
    if (FAILED(D3DCompile(kPixelShaderNv12BlurSource,
                          std::strlen(kPixelShaderNv12BlurSource),
                          nullptr,
                          nullptr,
                          nullptr,
                          "main",
                          "ps_5_0",
                          compileFlags,
                          0,
                          psNv12BlurBlob.GetAddressOf(),
                          errorBlob.GetAddressOf())))
    {
        logMessage("[Renderer] NV12 blur shader compilation failed");
        return false;
    }

    errorBlob.Reset();
    if (FAILED(D3DCompile(kPixelShaderBlurSource,
                          std::strlen(kPixelShaderBlurSource),
                          nullptr,
                          nullptr,
                          nullptr,
                          "main",
                          "ps_5_0",
                          compileFlags,
                          0,
                          psBlurBlob.GetAddressOf(),
                          errorBlob.GetAddressOf())))
    {
        logMessage("[Renderer] Blur shader compilation failed");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE samplerRange{};
    samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors = 1;
    samplerRange.BaseShaderRegister = 0;
    samplerRange.RegisterSpace = 0;
    samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[2] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = &srvRange;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &samplerRange;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = rootParameters;
    rootDesc.NumStaticSamplers = 0;
    rootDesc.pStaticSamplers = nullptr;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSig;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc,
                                           D3D_ROOT_SIGNATURE_VERSION_1,
                                           serializedRootSig.GetAddressOf(),
                                           errorBlob.ReleaseAndGetAddressOf())))
    {
        logMessage("[Renderer] Root signature serialization failed");
        return false;
    }

    if (FAILED(device_->CreateRootSignature(0,
                                            serializedRootSig->GetBufferPointer(),
                                            serializedRootSig->GetBufferSize(),
                                            IID_PPV_ARGS(rootSignature_.GetAddressOf()))))
    {
        logMessage("[Renderer] CreateRootSignature failed");
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, tex), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;
    rasterDesc.ForcedSampleCount = 0;
    rasterDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    auto& rtBlend = blendDesc.RenderTarget[0];
    rtBlend.BlendEnable = FALSE;
    rtBlend.LogicOpEnable = FALSE;
    rtBlend.SrcBlend = D3D12_BLEND_ONE;
    rtBlend.DestBlend = D3D12_BLEND_ZERO;
    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depthDesc.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    psoDesc.pRootSignature = rootSignature_.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineState_.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateGraphicsPipelineState (frame)", hr);
        logInfoQueueMessages(device_.Get(), "CreateGraphicsPipelineState (frame)");
        return false;
    }

    psoDesc.PS = {psNv12Blob->GetBufferPointer(), psNv12Blob->GetBufferSize()};
    hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineStateNv12_.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateGraphicsPipelineState (nv12)", hr);
        logInfoQueueMessages(device_.Get(), "CreateGraphicsPipelineState (nv12)");
        return false;
    }

    psoDesc.PS = {psGradientBlob->GetBufferPointer(), psGradientBlob->GetBufferSize()};
    hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineStateGradient_.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateGraphicsPipelineState (gradient)", hr);
        logInfoQueueMessages(device_.Get(), "CreateGraphicsPipelineState (gradient)");
        return false;
    }

    psoDesc.PS = {psBlurBlob->GetBufferPointer(), psBlurBlob->GetBufferSize()};
    hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineStateBlur_.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateGraphicsPipelineState (blur)", hr);
        logInfoQueueMessages(device_.Get(), "CreateGraphicsPipelineState (blur)");
        return false;
    }

    psoDesc.PS = {psNv12BlurBlob->GetBufferPointer(), psNv12BlurBlob->GetBufferSize()};
    hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineStateNv12Blur_.GetAddressOf()));
    if (FAILED(hr))
    {
        logFailure("CreateGraphicsPipelineState (nv12 blur)", hr);
        logInfoQueueMessages(device_.Get(), "CreateGraphicsPipelineState (nv12 blur)");
        return false;
    }

    return true;
}

bool D3DRenderer::createRenderTargets()
{
    if (!swapChain_ || !device_)
    {
        return false;
    }

    for (UINT i = 0; i < kFrameCount; ++i)
    {
        renderTargets_[i].Reset();
        HRESULT hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(renderTargets_[i].GetAddressOf()));
        if (FAILED(hr))
        {
            logFailure("GetBuffer (swap chain)", hr);
            return false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHandleStart_;
        handle.ptr += static_cast<SIZE_T>(i) * rtvDescriptorSize_;
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, handle);

        D3D12_RESOURCE_DESC desc = renderTargets_[i]->GetDesc();
        backBufferWidth_ = static_cast<UINT>(desc.Width);
        backBufferHeight_ = desc.Height;
    }

    return true;
}

void D3DRenderer::destroyRenderTarget()
{
    for (auto& target : renderTargets_)
    {
        target.Reset();
    }

    backBufferWidth_ = 0;
    backBufferHeight_ = 0;
}

void D3DRenderer::waitForFrame(FrameContext& frameContext)
{
    if (!fence_ || frameContext.fenceValue == 0)
    {
        return;
    }

    if (fence_->GetCompletedValue() >= frameContext.fenceValue)
    {
        frameContext.fenceValue = 0;
        return;
    }

    fence_->SetEventOnCompletion(frameContext.fenceValue, fenceEvent_);
    WaitForSingleObject(fenceEvent_, INFINITE);
    frameContext.fenceValue = 0;
}

void D3DRenderer::waitForGpu()
{
    if (!commandQueue_ || !fence_)
    {
        return;
    }

    const std::uint64_t fenceValue = fenceValue_++;
    if (SUCCEEDED(commandQueue_->Signal(fence_.Get(), fenceValue)))
    {
        if (fence_->GetCompletedValue() < fenceValue)
        {
            fence_->SetEventOnCompletion(fenceValue, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }
}

void D3DRenderer::updateViewport(UINT width, UINT height)
{
    const float fallback = 1.0f;
    viewport_.TopLeftX = 0.0f;
    viewport_.TopLeftY = 0.0f;
    viewport_.Width = width != 0 ? static_cast<float>(width) : fallback;
    viewport_.Height = height != 0 ? static_cast<float>(height) : fallback;
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;

    scissorRect_.left = 0;
    scissorRect_.top = 0;
    scissorRect_.right = width != 0 ? static_cast<LONG>(width) : 1;
    scissorRect_.bottom = height != 0 ? static_cast<LONG>(height) : 1;
}

void D3DRenderer::setViewportRect(float x, float y, float width, float height)
{
    if (!swapChain_ || backBufferWidth_ == 0 || backBufferHeight_ == 0)
    {
        updateViewport(backBufferWidth_, backBufferHeight_);
        return;
    }

    if (width <= 0.0f || height <= 0.0f)
    {
        updateViewport(backBufferWidth_, backBufferHeight_);
        return;
    }

    viewport_.TopLeftX = x;
    viewport_.TopLeftY = y;
    viewport_.Width = width;
    viewport_.Height = height;
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;

    const auto clampLong = [](long value, long minVal, long maxVal) {
        return std::min(std::max(value, minVal), maxVal);
    };

    const long right = static_cast<long>(std::ceil(x + width));
    const long bottom = static_cast<long>(std::ceil(y + height));

    scissorRect_.left = clampLong(static_cast<long>(std::floor(x)), 0, static_cast<long>(backBufferWidth_));
    scissorRect_.top = clampLong(static_cast<long>(std::floor(y)), 0, static_cast<long>(backBufferHeight_));
    scissorRect_.right = clampLong(right, 0, static_cast<long>(backBufferWidth_));
    scissorRect_.bottom = clampLong(bottom, 0, static_cast<long>(backBufferHeight_));
}
